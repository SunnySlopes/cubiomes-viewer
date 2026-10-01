#pragma once

#include "geometry.hpp"
#include "types.hpp"
#include <vector>
#include <functional>
#include <memory>

// Session-local exact chunk maxima. Overlapping candidate windows reuse the
// same evaluations; no candidate is discarded based on distance.
class SlimeOptimizer {
public:
    SlimeOptimizer(const Params&, const std::vector<RingSpan>&,
                   unsigned threads = 0, std::function<bool()> control = {});
    ~SlimeOptimizer();
    Result optimize(const Candidate&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// 为单个候选区域搜索最佳挂机点。
//
// 遍历玩家搜索区(候选区块 ± playerSearchRange)内的每个方块坐标,统计刷怪环
// 内落在史莱姆区块的方块数(有效刷怪面积)和触及的不重复史莱姆区块数(有效史
// 莱姆区块)。以面积最大为主、区块数最大为辅选出最优点。
//
// 使用精确的环/区块交集权重，结果与逐块遍历一致。同面积、同区块数时
// 按 x、z 升序选择，保证线程数和候选顺序不改变坐标。
Result optimizeCandidate(const Params& params,
                         const Candidate& candidate,
                         const std::vector<RingSpan>& ringSpans);
