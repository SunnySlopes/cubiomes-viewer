#pragma once

#include <cstdint>
#include <utility>
#include <vector>

// Floor division that is correct for negative operands (unlike C++ truncation).
inline int64_t floorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

// Precomputes all integer offsets (dx, dz) with  N^2 < dx^2 + dz^2 <= M^2.
//
// The block-center +0.5 offsets cancel because both the player and the target
// block use them, so the relative offset is an integer and the comparison uses
// only squared integer distances (no sqrt).
std::vector<std::pair<int, int>> computeRingOffsets(int innerRadius, int outerRadius);

// 环形区按行(dz)拆成连续的 dx 区间 [loDx, hiDx](含端点);被内圆挖空的
// 行拆成 2 段。用它可把「逐块」优化成「逐行区间」,每个玩家坐标只需按区块
// 跳跃扫描区间,而非遍历全部环内方块 —— 大幅提速,结果完全一致。
struct RingSpan {
    int dz;
    int loDx;
    int hiDx;
};
std::vector<RingSpan> computeRingSpans(int innerRadius, int outerRadius);
