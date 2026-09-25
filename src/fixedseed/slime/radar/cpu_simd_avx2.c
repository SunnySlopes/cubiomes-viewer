/*
 * cpu_simd_avx2.c — AVX2 批量史莱姆判定（8 lane 全向量化），用 -mavx2 编译。
 *
 * 教训（实测）：半向量化（只向量 chunk_seed、LCG 标量）因 store/load 往返 + even/odd
 *   重排开销，反而比标量慢。必须全程向量化、消除搬运，才可能超过标量。
 *
 * 全向量策略（与 slime.h 逐位一致）:
 *   1. chunk_seed 的 32 位乘法链用 _mm256_mullo_epi32（8×i32）；zz*4392871 的 64 位部分
 *      分 even/odd 用 _mm256_mul_epi32 算，得 8 lane 的 chunk_seed（分 lo/hi 两个 4×i64）。
 *   2. LCG 只迭代一次（判定只取首个 nextInt(10)）：state=(cs^MUL)&MASK; state=(state*MUL+ADD)&MASK。
 *      64 位乘 state*MUL 用 _mm256_mul_epu32 手工拼低 48 位（AVX2 无 _mm256_mullo_epi64）。
 *   3. bits=state>>17; 首抽 val=bits%10 向量算；去偏判定 (bits-val+9)<2^31 向量算。
 *      未通过去偏的 lane（概率~1e-8）标量重抽修正——冷路径。
 *   4. is_slime = (val==0 且去偏通过) 的 lane。
 *
 * 全程 4×i64 两组（lo=lane0-3, hi=lane4-7）处理 64 位量。
 */
#include <immintrin.h>
#include <stdint.h>
#include "cpu_simd.h"

SrIsa sr_detect_isa(void) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) return SR_ISA_AVX2;
    return SR_ISA_SCALAR;
#elif defined(_MSC_VER)
    int regs[4];
    __cpuidex(regs, 7, 0);
    if (regs[1] & (1 << 5)) return SR_ISA_AVX2;  /* leaf7 EBX bit5 = AVX2 */
    return SR_ISA_SCALAR;
#else
    return SR_ISA_SCALAR;
#endif
}

/* 64 位向量乘低位：返回 (a*b) mod 2^64 的低位（我们只需低 48 位）。4×i64。
 * a*b = a_lo*b_lo + ((a_hi*b_lo + a_lo*b_hi) << 32)，高位溢出无所谓（后面 &MASK）。 */
static inline __m256i mul64_lo(__m256i a, __m256i b) {
    __m256i mask32 = _mm256_set1_epi64x(0xFFFFFFFFLL);
    __m256i a_lo = _mm256_and_si256(a, mask32);
    __m256i a_hi = _mm256_srli_epi64(a, 32);
    __m256i b_lo = _mm256_and_si256(b, mask32);
    __m256i b_hi = _mm256_srli_epi64(b, 32);
    __m256i ll = _mm256_mul_epu32(a_lo, b_lo);           /* a_lo*b_lo */
    __m256i lh = _mm256_mul_epu32(a_lo, b_hi);           /* a_lo*b_hi */
    __m256i hl = _mm256_mul_epu32(a_hi, b_lo);           /* a_hi*b_lo */
    __m256i cross = _mm256_add_epi64(lh, hl);
    cross = _mm256_slli_epi64(cross, 32);
    return _mm256_add_epi64(ll, cross);
}

/* 把 8×i32 的低 4 lane 符号扩展成 4×i64。 */
static inline __m256i sext_lo4(__m256i v32) {
    __m128i lo = _mm256_castsi256_si128(v32);
    return _mm256_cvtepi32_epi64(lo);
}
static inline __m256i sext_hi4(__m256i v32) {
    __m128i hi = _mm256_extracti128_si256(v32, 1);
    return _mm256_cvtepi32_epi64(hi);
}

/* 对一组 4 lane 的 chunk_seed（4×i64）跑一次 LCG + 首抽，输出 state（4×i64）。 */
static inline __m256i lcg_once(__m256i cs) {
    const __m256i MUL  = _mm256_set1_epi64x((long long)0x5DEECE66DULL);
    const __m256i ADD  = _mm256_set1_epi64x(0xB);
    const __m256i MASK = _mm256_set1_epi64x((long long)((1ULL<<48)-1));
    __m256i s = _mm256_and_si256(_mm256_xor_si256(cs, MUL), MASK);
    s = _mm256_and_si256(_mm256_add_epi64(mul64_lo(s, MUL), ADD), MASK);
    return s;
}

void sr_are_slime_avx2(int64_t seed, int32_t x0, int32_t z, uint8_t out[SR_SIMD_LANES]) {
    /* --- chunk_seed：32 位乘法链（8×i32） --- */
    __m256i x  = _mm256_add_epi32(_mm256_set1_epi32(x0), _mm256_setr_epi32(0,1,2,3,4,5,6,7));
    __m256i zv = _mm256_set1_epi32(z);
    __m256i xx = _mm256_mullo_epi32(x, x);
    __m256i t1 = _mm256_mullo_epi32(xx, _mm256_set1_epi32(4987142));
    __m256i t2 = _mm256_mullo_epi32(x,  _mm256_set1_epi32(5947611));
    __m256i zz = _mm256_mullo_epi32(zv, zv);
    __m256i t4 = _mm256_mullo_epi32(zv, _mm256_set1_epi32(389711));

    /* 符号扩展 32 位项到 64 位（lo/hi 各 4 lane） */
    __m256i t1_lo = sext_lo4(t1), t1_hi = sext_hi4(t1);
    __m256i t2_lo = sext_lo4(t2), t2_hi = sext_hi4(t2);
    __m256i t4_lo = sext_lo4(t4), t4_hi = sext_hi4(t4);
    __m256i zz_lo = sext_lo4(zz), zz_hi = sext_hi4(zz);

    /* t3 = (int64)zz * 4392871（64 位） */
    __m256i c4392871 = _mm256_set1_epi64x(4392871);
    __m256i t3_lo = mul64_lo(zz_lo, c4392871);
    __m256i t3_hi = mul64_lo(zz_hi, c4392871);

    /* u = seed + t1 + t2 + t3 + t4; u ^= 987234911 */
    __m256i sd = _mm256_set1_epi64x(seed);
    __m256i xr = _mm256_set1_epi64x(987234911LL);
    __m256i cs_lo = _mm256_add_epi64(_mm256_add_epi64(_mm256_add_epi64(_mm256_add_epi64(sd, t1_lo), t2_lo), t3_lo), t4_lo);
    __m256i cs_hi = _mm256_add_epi64(_mm256_add_epi64(_mm256_add_epi64(_mm256_add_epi64(sd, t1_hi), t2_hi), t3_hi), t4_hi);
    cs_lo = _mm256_xor_si256(cs_lo, xr);
    cs_hi = _mm256_xor_si256(cs_hi, xr);

    /* --- LCG 一次 + 首抽 bits --- */
    __m256i st_lo = lcg_once(cs_lo);
    __m256i st_hi = lcg_once(cs_hi);
    __m256i bits_lo = _mm256_srli_epi64(st_lo, 48 - 31);   /* 0..2^31-1，非负 */
    __m256i bits_hi = _mm256_srli_epi64(st_hi, 48 - 31);

    /* 向量化 val = bits % 10 与去偏，避免标量 store/load 收尾。
     * bits ∈ [0,2^31)，装在 4×i64 的低 32 位。div10 = (bits * 0xCCCCCCCD) >> 35（32位 magic）。 */
    __m256i magic = _mm256_set1_epi64x(0xCCCCCCCDULL);
    /* bits * magic：bits<2^31, magic<2^32 → 积<2^63，_mm256_mul_epu32 (32×32→64) 够用。 */
    __m256i q_lo = _mm256_srli_epi64(_mm256_mul_epu32(bits_lo, magic), 35);
    __m256i q_hi = _mm256_srli_epi64(_mm256_mul_epu32(bits_hi, magic), 35);
    __m256i ten  = _mm256_set1_epi64x(10);
    /* val = bits - q*10 */
    __m256i val_lo = _mm256_sub_epi64(bits_lo, _mm256_mul_epu32(q_lo, ten));
    __m256i val_hi = _mm256_sub_epi64(bits_hi, _mm256_mul_epu32(q_hi, ten));
    /* 去偏: (bits - val + 9) < 2^31 → 通过。不通过极罕见(~1e-8)。 */
    __m256i nine = _mm256_set1_epi64x(9);
    __m256i chk_lo = _mm256_add_epi64(_mm256_sub_epi64(bits_lo, val_lo), nine);
    __m256i chk_hi = _mm256_add_epi64(_mm256_sub_epi64(bits_hi, val_hi), nine);
    __m256i lim = _mm256_set1_epi64x(0x80000000LL);
    /* is_slime = (val==0) && (chk < 2^31)。val==0 掩码 & chk<lim 掩码。 */
    __m256i valz_lo = _mm256_cmpeq_epi64(val_lo, _mm256_setzero_si256());
    __m256i valz_hi = _mm256_cmpeq_epi64(val_hi, _mm256_setzero_si256());
    /* chk < lim  等价 lim > chk（有符号比较安全，chk,lim 均 < 2^32 正数） */
    __m256i pass_lo = _mm256_cmpgt_epi64(lim, chk_lo);
    __m256i pass_hi = _mm256_cmpgt_epi64(lim, chk_hi);
    __m256i slime_lo = _mm256_and_si256(valz_lo, pass_lo);
    __m256i slime_hi = _mm256_and_si256(valz_hi, pass_hi);

    /* 存回：全掩码 lane → 1，否则 0。仅极罕见"去偏未通过"的 lane 需标量重抽修正。 */
    int64_t sl[SR_SIMD_LANES], pass[SR_SIMD_LANES], cs[SR_SIMD_LANES];
    _mm256_storeu_si256((__m256i*)&sl[0], slime_lo);
    _mm256_storeu_si256((__m256i*)&sl[4], slime_hi);
    _mm256_storeu_si256((__m256i*)&pass[0], pass_lo);
    _mm256_storeu_si256((__m256i*)&pass[4], pass_hi);

    int need_fix = (~pass[0] | ~pass[1] | ~pass[2] | ~pass[3]
                  | ~pass[4] | ~pass[5] | ~pass[6] | ~pass[7]) != 0;
    if (!need_fix) {
        for (int k = 0; k < SR_SIMD_LANES; ++k) out[k] = (uint8_t)(sl[k] & 1);
    } else {
        const uint64_t MUL = 0x5DEECE66DULL, ADD = 0xBULL, MASK = (1ULL<<48)-1;
        _mm256_storeu_si256((__m256i*)&cs[0], cs_lo);
        _mm256_storeu_si256((__m256i*)&cs[4], cs_hi);
        for (int k = 0; k < SR_SIMD_LANES; ++k) {
            if (pass[k]) { out[k] = (uint8_t)(sl[k] & 1); continue; }
            uint64_t s = ((uint64_t)cs[k] ^ MUL) & MASK;   /* 冷路径重抽 */
            for (;;) {
                s = (s * MUL + ADD) & MASK;
                int32_t bb = (int32_t)(s >> (48 - 31));
                int32_t vv = bb % 10;
                if (((uint32_t)bb - (uint32_t)vv + 9u) < 0x80000000u) { out[k] = (uint8_t)(vv == 0); break; }
            }
        }
    }
}
