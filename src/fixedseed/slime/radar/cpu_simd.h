/*
 * cpu_simd.h — 批量史莱姆判定（一次 8 个连续 x，固定 z），运行时 CPUID 分派。
 *
 * 阶段 2 策略（遵循 渐进式开发流程.md 阶段 2 铁律）:
 *   - 默认可达路径只有 AVX2 与 标量，且都必须对拍 = golden。
 *   - AVX-512 不写无法验证的路径：CPUID 命中 AVX-512 时也降级走 AVX2。
 *
 * 数学与 slime.h 逐字一致：z*z 先 32 位回绕，再 64 位乘 4392871；精确 nextInt(10) 去偏。
 * 批量判定：给定 world_seed、起始 x（连续 8 个 x=x..x+7）、固定 z，输出 8 个 0/1。
 *
 * 先提供标量批量版 sr_are_slime_scalar（逻辑与 SIMD 完全对应，用于对拍与回退）。
 * AVX2 版 sr_are_slime_avx2 在 cpu_simd_avx2.c 里，逐条替换成 intrinsics。
 */
#ifndef SLIMERANDER_CPU_SIMD_H
#define SLIMERANDER_CPU_SIMD_H

#include <stdint.h>
#include "slime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SR_SIMD_LANES 8

typedef enum { SR_ISA_SCALAR = 0, SR_ISA_AVX2 = 1 } SrIsa;

/* 运行时探测本机可用 ISA（AVX-512 也降级为 AVX2）。 */
SrIsa sr_detect_isa(void);

/*
 * 批量判定：out[k] = is_slime(seed, x0+k, z)，k ∈ [0,8)。
 * 标量版：逻辑与向量版逐 lane 对应，作为对拍基准与回退。
 */
static inline void sr_are_slime_scalar(int64_t seed, int32_t x0, int32_t z,
                                       uint8_t out[SR_SIMD_LANES]) {
    for (int k = 0; k < SR_SIMD_LANES; ++k)
        out[k] = (uint8_t)sr_is_slime(seed, x0 + k, z);
}

/* AVX2 批量判定（cpu_simd_avx2.c 实现，编译时带 -mavx2）。 */
void sr_are_slime_avx2(int64_t seed, int32_t x0, int32_t z, uint8_t out[SR_SIMD_LANES]);

#ifdef __cplusplus
}
#endif

#endif /* SLIMERANDER_CPU_SIMD_H */
