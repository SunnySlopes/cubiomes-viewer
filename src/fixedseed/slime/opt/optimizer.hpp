#pragma once

#include "geometry.hpp"
#include "types.hpp"
#include <vector>

// 为单个候选区域搜索最佳挂机点。
//
// 遍历玩家搜索区(候选区块 ± playerSearchRange)内的每个方块坐标,统计刷怪环
// 内落在史莱姆区块的方块数(有效刷怪面积)和触及的不重复史莱姆区块数(有效史
// 莱姆区块)。以面积最大为主、区块数最大为辅选出最优点。
//
// 使用按行区间(RingSpan)+ 按区块跳跃扫描,结果与逐块遍历完全一致但快得多。
Result optimizeCandidate(const Params& params,
                         const Candidate& candidate,
                         const std::vector<RingSpan>& ringSpans);
