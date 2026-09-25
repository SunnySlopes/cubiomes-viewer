#include "geometry.hpp"

#include <cmath>

std::vector<std::pair<int, int>> computeRingOffsets(int innerRadius, int outerRadius) {
    std::vector<std::pair<int, int>> offsets;
    const long long n2 = static_cast<long long>(innerRadius) * innerRadius;
    const long long m2 = static_cast<long long>(outerRadius) * outerRadius;

    for (int dx = -outerRadius; dx <= outerRadius; ++dx) {
        for (int dz = -outerRadius; dz <= outerRadius; ++dz) {
            long long d2 = static_cast<long long>(dx) * dx +
                           static_cast<long long>(dz) * dz;
            if (d2 > n2 && d2 <= m2) {
                offsets.emplace_back(dx, dz);
            }
        }
    }
    return offsets;
}

std::vector<RingSpan> computeRingSpans(int innerRadius, int outerRadius) {
    // 环形区 N < 距离 <= M。对每一行 dz,满足条件的 dx 构成 1~2 个连续区间:
    //   上界:dx^2 <= M^2 - dz^2  ->  |dx| <= outHi
    //   下界:dx^2 >  N^2 - dz^2  ->  |dx| >= inHi + 1 (当 N^2 - dz^2 >= 0 时)
    // 这样把「逐块」变成「逐行区间」,每个玩家坐标只需遍历区间而非所有块。
    std::vector<RingSpan> spans;
    const long long n2 = static_cast<long long>(innerRadius) * innerRadius;
    const long long m2 = static_cast<long long>(outerRadius) * outerRadius;

    for (int dz = -outerRadius; dz <= outerRadius; ++dz) {
        long long remOut = m2 - static_cast<long long>(dz) * dz;
        if (remOut < 0) continue;  // 该行完全在外圆之外
        int outHi = static_cast<int>(std::sqrt(static_cast<double>(remOut)));
        while (static_cast<long long>(outHi + 1) * (outHi + 1) <= remOut) ++outHi;
        while (static_cast<long long>(outHi) * outHi > remOut) --outHi;

        long long t = n2 - static_cast<long long>(dz) * dz;  // 内圆约束阈值
        if (t < 0) {
            // 整行都在内圆之外 -> 单区间 [-outHi, outHi]
            spans.push_back({dz, -outHi, outHi});
        } else {
            int inHi = static_cast<int>(std::sqrt(static_cast<double>(t)));
            while (static_cast<long long>(inHi + 1) * (inHi + 1) <= t) ++inHi;
            while (static_cast<long long>(inHi) * inHi > t) --inHi;
            // 排除 |dx| <= inHi,剩下两段
            if (inHi + 1 <= outHi) {
                spans.push_back({dz, -outHi, -(inHi + 1)});
                spans.push_back({dz, inHi + 1, outHi});
            }
        }
    }
    return spans;
}
