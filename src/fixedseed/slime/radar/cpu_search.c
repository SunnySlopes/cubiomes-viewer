/*
 * cpu_search.c — CPU 模式搜索
 *   阶段 1: 标量 SAT + 包围盒预筛 + 20 段精确甜甜圈
 *   阶段 3: Win32 多线程 + 无锁结果收集（block 静态均分，每线程独立 SAT 缓冲）
 *
 * 接口即最终 sr_search_cpu；阶段 2 只把 search_one_block 里的判定向量化，签名不变。
 *
 * 坐标语义（钉死，避免 off-by-8）:
 *   - 块左上中心绝对坐标 (bx0,bz0)；slime 图格 (i沿x, j沿z) 左上比中心早 8 格。
 *   - 块内第 k 个中心 (kx,kz∈[0,496)) 窗口 = slime 图 [kx,kx+17)×[kz,kz+17)，外扩 8 已含。
 *   - 命中中心绝对坐标 = (bx0+kx, bz0+kz)。
 *   - SAT padding: 行0/列0=0；rect 差分全程 u16 wrapping（查询矩形 ≤289<65535，精确）。
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
  #define _POSIX_C_SOURCE 199309L   /* nanosleep / CLOCK_MONOTONIC (Linux) */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "slimerander.h"
#include "slime.h"
#include "cpu_simd.h"

/* ---- 平台线程抽象：Windows 走 Win32，其余(Linux/macOS) 走 pthreads。 ---- */
#if defined(_WIN32)
  #include <windows.h>
  typedef HANDLE            sr_thread_t;
  #define SR_THREAD_RET     DWORD WINAPI
  #define SR_THREAD_PARAM   LPVOID
  static unsigned sr_cpu_count(void) {
      SYSTEM_INFO si; GetSystemInfo(&si);
      return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
  }
  static void sr_sleep_ms(unsigned ms) { Sleep(ms); }
#else
  #include <pthread.h>
  #include <unistd.h>
  #include <time.h>
  typedef pthread_t         sr_thread_t;
  #define SR_THREAD_RET     void *
  #define SR_THREAD_PARAM   void *
  /* Win32 整型别名：让结构体字段与局部变量的 LONG64/LONG 在 Linux 下也可用（原子操作走 __atomic_*）。 */
  typedef int64_t LONG64;
  typedef int32_t LONG;
  static unsigned sr_cpu_count(void) {
      long n = sysconf(_SC_NPROCESSORS_ONLN);
      return (n > 0) ? (unsigned)n : 1;
  }
  static void sr_sleep_ms(unsigned ms) {
      struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
      nanosleep(&ts, NULL);
  }
#endif

/* 批量判定函数指针类型：out[8] = is_slime(seed, x0+k, z)。 */
typedef void (*sr_batch_fn)(int64_t seed, int32_t x0, int32_t z, uint8_t out[SR_SIMD_LANES]);

/* ---- 原子封装：GCC 走 __atomic_*，MSVC 走 Interlocked*（同一套 relaxed 语义）。 ---- */
#if defined(__GNUC__) || defined(__clang__)
  #define SR_ATOMIC_FAA64(p)    __atomic_fetch_add((p), 1, __ATOMIC_RELAXED)
  #define SR_ATOMIC_STORE32(p,v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)
  #define SR_ATOMIC_LOAD64(p)   __atomic_load_n((p), __ATOMIC_RELAXED)
  #define SR_ATOMIC_LOAD32(p)   __atomic_load_n((p), __ATOMIC_RELAXED)
#else
  /* MSVC: Interlocked 返回旧值语义与 __atomic_fetch_add 一致；无 relaxed 变体，用全序（正确性无损）。 */
  #define SR_ATOMIC_FAA64(p)    InterlockedExchangeAdd64((volatile LONG64 *)(p), 1)
  #define SR_ATOMIC_STORE32(p,v) InterlockedExchange((volatile LONG *)(p), (v))
  #define SR_ATOMIC_LOAD64(p)   InterlockedCompareExchange64((volatile LONG64 *)(p), 0, 0)
  #define SR_ATOMIC_LOAD32(p)   InterlockedCompareExchange((volatile LONG *)(p), 0, 0)
#endif

#define SR_SIZE    512
#define SR_TESTED  (SR_SIZE - SR_WINDOW + 1)   /* 496 */
#define SR_SATDIM  (SR_SIZE + 1)               /* 513 */
#define SR_MAX_RESULTS (1u << 20)              /* 结果缓冲上限 ~1M 条 */

#define SAT_AT(sat, r, c) ((sat)[(size_t)(r) * SR_SATDIM + (size_t)(c)])

static volatile LONG g_sr_pause = 0;
static volatile LONG g_sr_stop = 0;

void sr_control_pause(void)  { SR_ATOMIC_STORE32(&g_sr_pause, 1); }
void sr_control_resume(void) { SR_ATOMIC_STORE32(&g_sr_pause, 0); }
void sr_control_stop(void)   { SR_ATOMIC_STORE32(&g_sr_stop, 1); }
void sr_control_reset(void)  {
    SR_ATOMIC_STORE32(&g_sr_pause, 0);
    SR_ATOMIC_STORE32(&g_sr_stop, 0);
}
int sr_control_should_run(void) {
    return SR_ATOMIC_LOAD32(&g_sr_stop) == 0;
}

static int sr_wait_if_paused(void) {
    while (SR_ATOMIC_LOAD32(&g_sr_pause) && SR_ATOMIC_LOAD32(&g_sr_stop) == 0)
        sr_sleep_ms(50);
    return SR_ATOMIC_LOAD32(&g_sr_stop) == 0;
}

/* ---- 共享上下文（多线程） ---- */
typedef struct {
    /* 只读参数 */
    int64_t  seed;
    uint32_t thr;
    int32_t  x0, z0, x1, z1;
    int64_t  blocks_x, blocks_z;
    uint64_t total_blocks;
    SrDonutRun runs[SR_DONUT_RUNS];
    sr_batch_fn batch;   /* 批量判定：CPUID 分派 AVX2 或标量 */

    /* 用户回调 */
    void *ctx;
    sr_result_fn  on_result;
    sr_progress_fn on_progress;

    /* 频繁写的原子字段：各占一条缓存行，避免彼此及与只读字段伪共享。 */
    char _pad0[64];
    volatile LONG64 result_count;  char _pad1[64 - sizeof(LONG64)];  /* 已写入结果数 */
    volatile LONG64 blocks_done;   char _pad2[64 - sizeof(LONG64)];  /* 已完成 block 数 */
    volatile LONG   overflow;      char _pad3[64 - sizeof(LONG)];    /* 结果溢出标志 */

    /* 预分配结果数组（无锁写入） */
    SrResult *results;
    uint32_t  capacity;
} SharedCtx;

typedef struct {
    SharedCtx *sh;
    int64_t start_block, end_block;
} WorkerArg;

/* 处理单个 block：建 SAT + 预筛 + 精确甜甜圈，命中原子写入共享缓冲。
 * xterm: 每线程一份的 x 项表缓冲（SR_SIZE 个 u64），标量快路径用；AVX2 路径不用可传 NULL。 */
static void search_one_block(SharedCtx *sh, uint16_t *sat, uint64_t *xterm,
                             int64_t bxi, int64_t bzi) {
    const int32_t bx0 = sh->x0 + (int32_t)(bxi * SR_TESTED);
    const int32_t bz0 = sh->z0 + (int32_t)(bzi * SR_TESTED);
    const int32_t cx_count = (int32_t)((sh->x1 - bx0) < SR_TESTED ? (sh->x1 - bx0) : SR_TESTED);
    const int32_t cz_count = (int32_t)((sh->z1 - bz0) < SR_TESTED ? (sh->z1 - bz0) : SR_TESTED);
    const int32_t sx0 = bx0 - SR_OFFSET;
    const int32_t sz0 = bz0 - SR_OFFSET;
    const int64_t seed = sh->seed;
    const uint32_t thr = sh->thr;

    /* 融合构建 SAT：逐行判定 → 行内前缀和(carry) → 加上一行前缀。 */
    for (int c = 0; c < SR_SATDIM; ++c) SAT_AT(sat, 0, c) = 0;
    for (int r = 1; r < SR_SATDIM; ++r) SAT_AT(sat, r, 0) = 0;

    sr_batch_fn batch = sh->batch;
    if (!batch) {
        /* 标量快路径：chunk_seed 可加性拆分——x 项整块预表一次，每行只算 z 项，
         * 逐格一次加+异或后判定，消除跨 512 行重复的 x 相关乘法（对拍逐位一致）。 */
        for (int i = 0; i < SR_SIZE; ++i) xterm[i] = sr_xterm(sx0 + i);
        for (int j = 0; j < SR_SIZE; ++j) {
            const uint64_t zbase = sr_zbase(seed, sz0 + j);
            uint16_t run = 0;
            uint16_t *row_prev = &SAT_AT(sat, j,     1);
            uint16_t *row_cur  = &SAT_AT(sat, j + 1, 1);
            for (int i = 0; i < SR_SIZE; ++i) {
                int64_t cs = (int64_t)((zbase + xterm[i]) ^ (uint64_t)987234911LL);
                run = (uint16_t)(run + (uint16_t)sr_is_slime_from_cs(cs));
                row_cur[i] = (uint16_t)(row_prev[i] + run);
            }
        }
    } else {
        /* SIMD 路径（SR_USE_AVX2=1 时）：整行按 8 lane 批量判定，无尾巴。 */
        for (int j = 0; j < SR_SIZE; ++j) {
            int32_t z = sz0 + j;
            uint16_t run = 0;
            uint8_t s8[SR_SIMD_LANES];
            for (int i = 0; i < SR_SIZE; i += SR_SIMD_LANES) {
                batch(seed, sx0 + i, z, s8);
                for (int k = 0; k < SR_SIMD_LANES; ++k) {
                    run = (uint16_t)(run + s8[k]);
                    SAT_AT(sat, j + 1, i + k + 1) = (uint16_t)(SAT_AT(sat, j, i + k + 1) + run);
                }
            }
        }
    }

    for (int32_t kz = 0; kz < cz_count; ++kz) {
        for (int32_t kx = 0; kx < cx_count; ++kx) {
            uint16_t box = (uint16_t)(
                  SAT_AT(sat, kz + SR_WINDOW, kx + SR_WINDOW)
                - SAT_AT(sat, kz,             kx + SR_WINDOW)
                - SAT_AT(sat, kz + SR_WINDOW, kx)
                + SAT_AT(sat, kz,             kx));
            if (box < thr) continue;

            uint16_t exact = 0;
            for (int s = 0; s < SR_DONUT_RUNS; ++s) {
                int rr = kz + sh->runs[s].dx;
                int c1 = kx + sh->runs[s].c1;
                int c2 = kx + sh->runs[s].c2 + 1;
                exact = (uint16_t)(exact
                    + SAT_AT(sat, rr + 1, c2)
                    - SAT_AT(sat, rr,     c2)
                    - SAT_AT(sat, rr + 1, c1)
                    + SAT_AT(sat, rr,     c1));
            }
            if (exact >= thr) {
                /* 无锁占槽：relaxed 足够，槽位互不重叠，join 有 full barrier。 */
                LONG64 idx = SR_ATOMIC_FAA64(&sh->result_count);
                if (idx < (LONG64)sh->capacity) {
                    SrResult res = { bx0 + kx, bz0 + kz, exact };
                    sh->results[idx] = res;
                } else {
                    SR_ATOMIC_STORE32(&sh->overflow, 1);
                }
            }
        }
    }
}

static SR_THREAD_RET worker(SR_THREAD_PARAM param) {
    WorkerArg *wa = (WorkerArg *)param;
    SharedCtx *sh = wa->sh;

    /* 每线程独立 SAT 缓冲（~526KB，堆分配，绝不共享） */
    uint16_t *sat = (uint16_t *)malloc((size_t)SR_SATDIM * SR_SATDIM * sizeof(uint16_t));
    /* 每线程 x 项表（标量快路径用，SR_SIZE 个 u64 = 4KB）。 */
    uint64_t *xterm = (uint64_t *)malloc((size_t)SR_SIZE * sizeof(uint64_t));
    if (!sat || !xterm) { free(sat); free(xterm); SR_ATOMIC_STORE32(&sh->overflow, 2); return 0; }

    for (int64_t b = wa->start_block; b < wa->end_block; ++b) {
        if (!sr_wait_if_paused()) break;
        int64_t bzi = b / sh->blocks_x;
        int64_t bxi = b % sh->blocks_x;
        search_one_block(sh, sat, xterm, bxi, bzi);
        SR_ATOMIC_FAA64(&sh->blocks_done);
    }

    free(sat);
    free(xterm);
    return 0;
}

int sr_search_cpu(const SrParams *p, unsigned thread_count,
                  void *ctx, sr_result_fn on_result, sr_progress_fn on_progress) {
    if (!p || !on_result) return SR_ERR_PARAM;
    if (p->x1 <= p->x0 || p->z1 <= p->z0) return SR_OK;

    if (sr_verify_donut_runs() != 0) {
        fprintf(stderr, "[cpu] donut_runs self-check FAILED\n");
        return SR_ERR_PARAM;
    }

    /* 线程数：0=自动（逻辑处理器数） */
    unsigned nthreads = thread_count;
    if (nthreads == 0) nthreads = sr_cpu_count();

    SharedCtx sh;
    memset(&sh, 0, sizeof(sh));
    sh.seed = p->world_seed;
    sh.thr  = p->threshold;
    sh.x0 = p->x0; sh.z0 = p->z0; sh.x1 = p->x1; sh.z1 = p->z1;
    sh.ctx = ctx; sh.on_result = on_result; sh.on_progress = on_progress;
    sr_build_donut_runs(sh.runs);

    /* 批量判定分派。
     * 实测结论（GCC16 -O3 + 本机 AVX2）：手写 AVX2 判定不快于标量——判定瓶颈是 LCG 的
     * 64 位乘，AVX2 无原生 _mm256_mullo_epi64，需 3 次 _mm256_mul_epu32 拼装，其开销抵消
     * 了 8 路并行；而 GCC16 对标量 sr_is_slime 已优化到位。印证 代码需求.md §7.0
     *（LCG 是单核天花板，向量化收益有限）。故默认走标量；AVX2 保留但需 SR_USE_AVX2=1 显式启用
     * （已单独对拍 = golden，供不同 CPU/编译器重新评估）。 */
    SrIsa isa = SR_ISA_SCALAR;
    if (getenv("SR_USE_AVX2") && sr_detect_isa() == SR_ISA_AVX2) isa = SR_ISA_AVX2;
    /* batch==NULL 标记标量快路径（chunk_seed 预表拆分）；AVX2 显式启用时才走 batch 函数。 */
    sh.batch = (isa == SR_ISA_AVX2) ? sr_are_slime_avx2 : NULL;

    const int64_t width  = (int64_t)p->x1 - p->x0;
    const int64_t height = (int64_t)p->z1 - p->z0;
    sh.blocks_x = (width  + SR_TESTED - 1) / SR_TESTED;
    sh.blocks_z = (height + SR_TESTED - 1) / SR_TESTED;
    sh.total_blocks = (uint64_t)sh.blocks_x * (uint64_t)sh.blocks_z;
    if (sh.total_blocks == 0) return SR_OK;
    if ((int64_t)nthreads > (int64_t)sh.total_blocks) nthreads = (unsigned)sh.total_blocks;

    /* 预分配结果缓冲 */
    sh.capacity = SR_MAX_RESULTS;
    sh.results = (SrResult *)malloc((size_t)sh.capacity * sizeof(SrResult));
    if (!sh.results) return SR_ERR_ALLOC;

    /* 启动线程：block 静态均分 start=total*tid/n, end=total*(tid+1)/n */
    sr_thread_t *handles = (sr_thread_t *)malloc(nthreads * sizeof(sr_thread_t));
    WorkerArg   *args    = (WorkerArg *)malloc(nthreads * sizeof(WorkerArg));
    unsigned char *spawned = (unsigned char *)calloc(nthreads, 1);  /* 标记成功创建的线程 */
    if (!handles || !args || !spawned) {
        free(sh.results); free(handles); free(args); free(spawned); return SR_ERR_ALLOC;
    }

    for (unsigned t = 0; t < nthreads; ++t) {
        args[t].sh = &sh;
        args[t].start_block = (int64_t)sh.total_blocks * t / nthreads;
        args[t].end_block   = (int64_t)sh.total_blocks * (t + 1) / nthreads;
#if defined(_WIN32)
        handles[t] = CreateThread(NULL, 0, worker, &args[t], 0, NULL);
        spawned[t] = (handles[t] != NULL);
        if (!spawned[t]) worker(&args[t]);  /* 回退：当前线程直接跑该区间 */
#else
        spawned[t] = (pthread_create(&handles[t], NULL, worker, &args[t]) == 0);
        if (!spawned[t]) worker(&args[t]);  /* 回退：当前线程直接跑该区间 */
#endif
    }

    /* 进度轮询（两平台通用，靠原子计数）。停止请求时尽快退出，避免卡死。 */
    if (on_progress) {
        for (;;) {
            LONG64 d = SR_ATOMIC_LOAD64(&sh.blocks_done);
            on_progress(ctx, (uint64_t)d, sh.total_blocks);
            if ((uint64_t)d >= sh.total_blocks) break;
            if (!sr_control_should_run()) break;
            sr_sleep_ms(100);
        }
    }

    /* join 所有成功创建的线程。 */
    for (unsigned t = 0; t < nthreads; ++t) {
        if (!spawned[t]) continue;
#if defined(_WIN32)
        WaitForSingleObject(handles[t], INFINITE);
        CloseHandle(handles[t]);
#else
        pthread_join(handles[t], NULL);
#endif
    }
    free(spawned);

    /* join 后有 full barrier：安全读取结果，统一回调（顺序留给输出层排序）。 */
    LONG64 total = SR_ATOMIC_LOAD64(&sh.result_count);
    uint64_t emit = (total < (LONG64)sh.capacity) ? (uint64_t)total : sh.capacity;
    for (uint64_t i = 0; i < emit; ++i)
        on_result(ctx, sh.results[i]);

    if (on_progress) on_progress(ctx, sh.total_blocks, sh.total_blocks);

    int rc = SR_OK;
    if (SR_ATOMIC_LOAD32(&sh.overflow) != 0) {
        fprintf(stderr, "[cpu] WARNING: result buffer overflow (>%u hits); "
                        "output truncated. Lower RANGE or raise THRESHOLD.\n", sh.capacity);
        rc = SR_OK; /* 非致命：结果被截断但已警告 */
    }

    free(handles); free(args); free(sh.results);
    return rc;
}
