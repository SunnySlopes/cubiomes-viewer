#include "slime_check.hpp"
#include "java_random.hpp"

#include <cstdint>

// 完全按照 Minecraft(Java 版)原版精度复刻区块专属种子的计算。
//
// 关键点:原版各项的运算精度是混合的 ——
//   x*x*4987142   在 32 位 int 下计算(会发生 32 位溢出回绕),再转 int64
//   x*5947611     在 32 位 int 下计算,再转 int64
//   z*z           先在 32 位 int 下计算,转成 int64 后再乘 4392871L(64 位乘法)
//   z*389711      在 32 位 int 下计算,再转 int64
// 最后所有项在 64 位下相加,再与 987234911 做异或(异或优先级最低)。
//
// 对于坐标较大的区块(如 x*x 超过 int32 范围),32 位与 64 位结果不同,
// 因此必须严格保留这种混合精度,否则大坐标下的判定会出错。
//
// C++ 的有符号整数溢出是未定义行为,这里统一用 uint32_t 模拟 32 位回绕,
// 再重新解释为有符号数并做符号扩展,既避免 UB 又保证与 Java 结果一致。
bool isSlimeChunk(int64_t worldSeed, int32_t chunkX, int32_t chunkZ) {
    const uint32_t ux = static_cast<uint32_t>(chunkX);
    const uint32_t uz = static_cast<uint32_t>(chunkZ);

    // x*x*4987142  (32 位)
    int32_t term1 = static_cast<int32_t>(ux * ux * 4987142u);
    // x*5947611    (32 位)
    int32_t term2 = static_cast<int32_t>(ux * 5947611u);
    // z*z          (32 位) -> 转 int64 -> *4392871 (64 位)
    int32_t zz    = static_cast<int32_t>(uz * uz);
    int64_t term3 = static_cast<int64_t>(zz) * 4392871LL;
    // z*389711     (32 位)
    int32_t term4 = static_cast<int32_t>(uz * 389711u);

    // 64 位累加(用 uint64_t 保证回绕定义良好);int32 项先符号扩展再重新解释。
    uint64_t seed = static_cast<uint64_t>(worldSeed);
    seed += static_cast<uint64_t>(static_cast<int64_t>(term1));
    seed += static_cast<uint64_t>(static_cast<int64_t>(term2));
    seed += static_cast<uint64_t>(term3);
    seed += static_cast<uint64_t>(static_cast<int64_t>(term4));
    seed ^= 987234911ULL;   // 异或优先级最低,最后执行

    JavaRandom rnd(seed);
    return rnd.nextInt(10) == 0;
}
