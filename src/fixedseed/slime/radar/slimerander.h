/*
 * slimerander.h — 公共类型与两种模式入口声明（纯 C）
 * 参见 代码需求.md §3.1 / §3.2。
 */
#ifndef SLIMERANDER_H
#define SLIMERANDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 搜索区域: [x0,x1) × [z0,z1)（区块坐标，半开区间）。 */
typedef struct {
    int64_t world_seed;
    uint8_t threshold;
    int32_t x0, z0, x1, z1;
} SrParams;

typedef struct {
    int32_t  x, z;
    uint32_t count;
} SrResult;

/* 回调：实现保证线程安全；count 为精确甜甜圈计数。 */
typedef void (*sr_result_fn)  (void *ctx, SrResult res);
typedef void (*sr_progress_fn)(void *ctx, uint64_t completed, uint64_t total);

/* 错误码 */
enum {
    SR_OK = 0,
    SR_ERR_ALLOC = 1,
    SR_ERR_PARAM = 2
};

/* 返回 0 成功，非 0 为错误码。ctx 透传给回调。thread_count=0 表示自动。 */
int sr_search_cpu(const SrParams *p, unsigned thread_count,
                  void *ctx, sr_result_fn on_result, sr_progress_fn on_progress);

/* 暂停 / 停止（合入 Viewer 后的协作式控制） */
void sr_control_pause(void);
void sr_control_resume(void);
void sr_control_stop(void);
void sr_control_reset(void);
int  sr_control_should_run(void); /* 0 = stop requested */

#ifdef __cplusplus
}
#endif

#endif /* SLIMERANDER_H */
