/*
 * slime.h — 史莱姆判定 + 甜甜圈掩码（所有模式共用的数学核心）
 *
 * 逐字复刻 Minecraft (Java 版) 判定：构造区块种子 → Java LCG 取 nextInt(10)==0。
 * 全程强制精确（带去偏 rejection），禁用偏置版。回绕位宽务必与注释一致。
 * 参见 代码需求.md §2.1 / 算法-main-CPU.md §1。
 */
#ifndef SLIMERANDER_SLIME_H
#define SLIMERANDER_SLIME_H

#include <stdint.h>

/* ---- PRNG / 种子常量 ---- */
#define SR_LCG_MUL   0x5DEECE66DULL
#define SR_LCG_ADD   0xBULL
#define SR_LCG_MASK  ((1ULL << 48) - 1ULL)

/* ---- 甜甜圈窗口常量 ---- */
#define SR_DONUT_INNER2  1    /* d² > 1     (排除中心与 4 个正邻格) */
#define SR_DONUT_OUTER2  64   /* d² <= 64   (outer = 8)             */
#define SR_WINDOW        17    /* 17×17 窗口 */
#define SR_OFFSET        8     /* 窗口半径，中心偏移 */

/* 32 位有符号回绕乘法：(int32_t)((uint32_t)a * (uint32_t)b) */
static inline int32_t sr_mul32(int32_t a, int32_t b) {
    return (int32_t)((uint32_t)a * (uint32_t)b);
}

/* 区块种子构造。注意 z*z 先 32 位回绕，再提升 64 位乘 4392871。 */
static inline int64_t sr_chunk_seed(int64_t world_seed, int32_t x, int32_t z) {
    int32_t t1 = sr_mul32(sr_mul32(x, x), 4987142);   /* 32 位 */
    int32_t t2 = sr_mul32(x, 5947611);                /* 32 位 */
    int64_t t3 = (int64_t)sr_mul32(z, z) * 4392871LL; /* z*z 先 32 位, 再 64 位 */
    int32_t t4 = sr_mul32(z, 389711);                 /* 32 位 */
    uint64_t u = (uint64_t)world_seed;
    u += (uint64_t)(int64_t)t1;
    u += (uint64_t)(int64_t)t2;
    u += (uint64_t)t3;
    u += (uint64_t)(int64_t)t4;
    u ^= (uint64_t)987234911LL;
    return (int64_t)u;
}

/* 精确 nextInt(10)==0 判定，输入为已构造好的区块种子（低位裸 seed）。
 * 拆出此层，供 SAT 构建预计算 chunk_seed 后直接判定（见下 sr_chunk_seed 的可加性拆分）。 */
static inline int sr_is_slime_from_cs(int64_t chunk_seed) {
    uint64_t s = ((uint64_t)chunk_seed ^ SR_LCG_MUL) & SR_LCG_MASK;
    for (;;) {
        s = (s * SR_LCG_MUL + SR_LCG_ADD) & SR_LCG_MASK;
        int32_t bits = (int32_t)(s >> (48 - 31));
        int32_t val  = bits % 10;
        if (((uint32_t)bits - (uint32_t)val + 9u) < 0x80000000u)
            return val == 0;
    }
}

/* 精确 nextInt(10)==0 判定。去偏用无符号运算避免有符号溢出 UB。 */
static inline int sr_is_slime(int64_t world_seed, int32_t x, int32_t z) {
    return sr_is_slime_from_cs(sr_chunk_seed(world_seed, x, z));
}

/*
 * chunk_seed 的可加性拆分（供 SAT 构建按行/列消除冗余乘法）。
 *   chunk_seed = ( (seed + t1(x)+t2(x)) + (t3(z)+t4(z)) ) ^ 987234911
 * 其中 t1/t2 只依赖 x、t3/t4 只依赖 z，且异或前全是 u64 加法（结合/交换）。
 * 故整块可预计算 x 项表（每列一次），每行预计算 z 项，逐格只需一次加 + 异或，
 * 结果与 sr_chunk_seed 逐位一致（已对拍验证）。
 */
static inline uint64_t sr_xterm(int32_t x) {   /* t1(x)+t2(x)，符号扩展后累加 */
    int32_t t1 = sr_mul32(sr_mul32(x, x), 4987142);
    int32_t t2 = sr_mul32(x, 5947611);
    return (uint64_t)(int64_t)t1 + (uint64_t)(int64_t)t2;
}
static inline uint64_t sr_zbase(int64_t world_seed, int32_t z) {   /* seed + t3(z)+t4(z) */
    int64_t t3 = (int64_t)sr_mul32(z, z) * 4392871LL;
    int32_t t4 = sr_mul32(z, 389711);
    return (uint64_t)world_seed + (uint64_t)t3 + (uint64_t)(int64_t)t4;
}

/* 甜甜圈掩码内某偏移是否有效：1 < dx²+dz² <= 64 */
static inline int sr_in_donut(int dx, int dz) {
    int d2 = dx * dx + dz * dz;
    return d2 > SR_DONUT_INNER2 && d2 <= SR_DONUT_OUTER2;
}

/*
 * 甜甜圈按行拆成水平连续段，供 SAT 矩形差分求精确计数。
 * 窗口坐标系 [0,17)：dx 为行(相对偏移 dx-8)，[c1,c2] 为该行连续史莱姆列的
 * 闭区间（窗口列坐标，含 c1、含 c2）。SAT 求和时用半开右端 c2+1。
 * 生成逻辑参照 slimy-cpubooest SearchBlock.zig，但直接由 sr_in_donut 扫出，
 * 保证段列表与判定掩码同源、不可能抄错。
 */
#define SR_DONUT_RUNS 20   /* 17×17 甜甜圈按行分解恒为 20 段 */

typedef struct { int dx, c1, c2; } SrDonutRun;

/* 运行时把甜甜圈扫成水平段，写入 runs[]，返回段数。窗口列坐标 [0,17)。 */
static inline int sr_build_donut_runs(SrDonutRun *runs) {
    int n = 0;
    for (int row = 0; row < SR_WINDOW; ++row) {
        int dx = row - SR_OFFSET;
        int in_run = 0, start = 0;
        for (int col = 0; col < SR_WINDOW; ++col) {
            int dz = col - SR_OFFSET;
            int inside = sr_in_donut(dx, dz);
            if (inside && !in_run) { start = col; in_run = 1; }
            else if (!inside && in_run) {
                runs[n].dx = row; runs[n].c1 = start; runs[n].c2 = col - 1; ++n;
                in_run = 0;
            }
        }
        if (in_run) {
            runs[n].dx = row; runs[n].c1 = start; runs[n].c2 = SR_WINDOW - 1; ++n;
        }
    }
    return n;
}

/*
 * 启动自检：段覆盖的格子集合必须与 sr_in_donut 逐格枚举完全一致，
 * 且段数为 SR_DONUT_RUNS。返回 0 成功，非 0 失败。
 */
static inline int sr_verify_donut_runs(void) {
    SrDonutRun runs[SR_WINDOW * SR_WINDOW];
    int n = sr_build_donut_runs(runs);
    if (n != SR_DONUT_RUNS) return 1;

    /* 段覆盖矩阵 vs 掩码矩阵逐格比对 */
    int total_mask = 0, total_runs = 0;
    for (int row = 0; row < SR_WINDOW; ++row)
        for (int col = 0; col < SR_WINDOW; ++col)
            if (sr_in_donut(row - SR_OFFSET, col - SR_OFFSET)) ++total_mask;

    for (int i = 0; i < n; ++i) {
        int row = runs[i].dx;
        for (int col = runs[i].c1; col <= runs[i].c2; ++col) {
            if (!sr_in_donut(row - SR_OFFSET, col - SR_OFFSET)) return 2; /* 段含非掩码格 */
            ++total_runs;
        }
    }
    if (total_runs != total_mask) return 3; /* 段格数与掩码格数不符 */
    return 0;
}

#endif /* SLIMERANDER_SLIME_H */
