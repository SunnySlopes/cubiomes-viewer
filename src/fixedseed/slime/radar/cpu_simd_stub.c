#include "cpu_simd.h"

SrIsa sr_detect_isa(void)
{
    return SR_ISA_SCALAR;
}

void sr_are_slime_avx2(int64_t seed, int32_t x0, int32_t z, uint8_t out[SR_SIMD_LANES])
{
    /* AVX2 path disabled in Cubiomes Viewer build; fall back to scalar. */
    sr_are_slime_scalar(seed, x0, z, out);
}
