#ifndef RIVER_SEARCH_H
#define RIVER_SEARCH_H

#include <atomic>
#include <cstdint>
#include <vector>

struct RiverProgress {
    std::atomic_int current{0};
    std::atomic_int total{0};
    std::atomic_int phase{1}; // 1 coarse tiles, 2 scale-1 refine
    std::atomic_bool try_pause{false};
    std::atomic_bool try_stop{false};
};

struct RiverHit {
    int x = 0;
    int z = 0;
    int area = 0;

    bool operator<(const RiverHit &o) const noexcept
    {
        if (area != o.area) return area < o.area;
        if (x != o.x) return x > o.x;
        return z > o.z;
    }
};

struct RiverSearchConfig {
    uint64_t seed = 0;
    int mc = 0;
    int startX = 0;
    int startZ = 0;
    int sx = 0;
    int sz = 0;
    int y = -62;
    int minArea = 40000;
    int threads = 0;
};

void runRiverSearch(const RiverSearchConfig &cfg, RiverProgress *progress, std::vector<RiverHit> &out);

#endif
