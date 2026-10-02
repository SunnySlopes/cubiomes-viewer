#ifndef SR_CPU_RNG_AVX2_H
#define SR_CPU_RNG_AVX2_H
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define SR_HAVE_FAST_AVX2 1
#define SR_AVX2_TARGET __attribute__((target("avx2")))
static SR_AVX2_TARGET inline __m256i sr_avx_lcg(__m256i a) {
    const __m256i low = _mm256_set1_epi64x(0xDEECE66DULL);
    const __m256i high = _mm256_set1_epi64x(5);
    __m256i ll = _mm256_mul_epu32(a, low);
    __m256i cross = _mm256_add_epi64(_mm256_mul_epu32(_mm256_srli_epi64(a,32),low),
                                    _mm256_mul_epu32(a,high));
    return _mm256_and_si256(_mm256_add_epi64(_mm256_add_epi64(ll,_mm256_slli_epi64(cross,32)),
                                            _mm256_set1_epi64x(11)),
                           _mm256_set1_epi64x((1ULL<<48)-1));
}
static SR_AVX2_TARGET inline unsigned sr_avx_divisible(__m256i bits) {
    // Unsigned constant-divisibility test: rotate(bits*inverse(5),1) <= floor(2^32/10).
    __m256i product = _mm256_mullo_epi32(bits,_mm256_set1_epi32((int)0xCCCCCCCDu));
    __m256i rotated = _mm256_or_si256(_mm256_srli_epi32(product,1),_mm256_slli_epi32(product,31));
    __m256i least = _mm256_min_epu32(rotated,_mm256_set1_epi32(0x19999999));
    return (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(least,rotated)));
}
static SR_AVX2_TARGET inline void sr_fast_slime8(const uint64_t *xterm,uint64_t zbase,uint8_t out[8]) {
    const __m256i base = _mm256_set1_epi64x((long long)zbase);
    const __m256i scramble = _mm256_set1_epi64x(987234911ULL ^ SR_LCG_MUL);
    __m256i cs0 = _mm256_xor_si256(_mm256_add_epi64(_mm256_loadu_si256((const __m256i*)xterm),base),scramble);
    __m256i cs1 = _mm256_xor_si256(_mm256_add_epi64(_mm256_loadu_si256((const __m256i*)(xterm+4)),base),scramble);
    __m256i b0 = _mm256_srli_epi64(sr_avx_lcg(cs0),17);
    __m256i b1 = _mm256_srli_epi64(sr_avx_lcg(cs1),17);
    __m256i limit = _mm256_set1_epi32(2147483640);
    unsigned accepted0 = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(limit,b0))) & 0x55;
    unsigned accepted1 = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(limit,b1))) & 0x55;
    unsigned zero0 = sr_avx_divisible(b0), zero1 = sr_avx_divisible(b1);
    for (int k=0;k<4;++k) {
        out[k]=(uint8_t)((zero0>>(k*2))&1);
        out[k+4]=(uint8_t)((zero1>>(k*2))&1);
    }
    if ((accepted0 & accepted1) != 0x55) {
        for (int k=0;k<8;++k) {
            unsigned accepted = k<4 ? accepted0 : accepted1;
            if (!(accepted & (1u << ((k%4)*2))))
                out[k]=(uint8_t)sr_is_slime_from_cs((int64_t)((xterm[k]+zbase)^987234911ULL));
        }
    }
}
static SR_AVX2_TARGET void sr_fast_sat_row(const uint64_t *xterm,uint64_t zbase,
                                         int width,const uint16_t *previous,uint16_t *current) {
    uint16_t run=0; int i=0;
    for(;i+8<=width;i+=8) {
        uint8_t values[8]; sr_fast_slime8(xterm+i,zbase,values);
        for(int k=0;k<8;++k) {
            run=(uint16_t)(run+values[k]);
            current[i+k]=(uint16_t)(previous[i+k]+run);
        }
    }
    for(;i<width;++i) {
        run=(uint16_t)(run+sr_is_slime_from_cs((int64_t)((xterm[i]+zbase)^987234911ULL)));
        current[i]=(uint16_t)(previous[i]+run);
    }
}
#endif
#endif
