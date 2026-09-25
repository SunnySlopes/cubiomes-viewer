#ifndef SLIME_PIPELINE_H
#define SLIME_PIPELINE_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

struct SlimePipelineProgress {
    std::atomic_int current{0};
    std::atomic_int total{0};
    std::atomic_int phase{0}; // 1=radar, 2=opt
    std::atomic_bool try_pause{false};
    std::atomic_bool try_stop{false};
};

struct SlimePipelineHit {
    int64_t afkX = 0;
    int64_t afkZ = 0;
    int64_t rawArea = 0;       // before biome conversion
    int64_t area = 0;          // after biome conversion (same as raw if disabled)
    int chunks = 0;
};

struct SlimePipelineConfig {
    int64_t seed = 0;
    int mc = 0;
    int blockX0 = 0, blockZ0 = 0, blockX1 = 0, blockZ1 = 0; // inclusive
    int threshold = 40;
    int minArea = 0;
    bool biomeConv = false;
    int threads = 0;
};

void runSlimePipeline(const SlimePipelineConfig &cfg, SlimePipelineProgress *progress,
                      std::vector<SlimePipelineHit> &out);

#endif
