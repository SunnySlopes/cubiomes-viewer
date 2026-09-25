#ifndef CAVE_SEARCH_H
#define CAVE_SEARCH_H

#include <atomic>
#include <cstdint>
#include <vector>

/** Mirrors LowYDripstoneCaveFinder Progress (phase1 field = search phase). */
struct CaveProgress {
    std::atomic_int current{0};
    std::atomic_int total{0};
    std::atomic_int chunkInRunning{0};
    std::atomic_int phase1{0}; // 1 = coarse tiles, 2 = PreciseBiome refine, -1 = done
    std::atomic_bool try_pause{false};
    std::atomic_bool try_stop{false};
};

struct CaveHit {
    int x = 0;
    int z = 0;
    int area = 0;      // weighted total = cave + river * riverFactor
    int caveArea = 0;
    int riverArea = 0;

    bool operator<(const CaveHit &o) const noexcept
    {
        if (area != o.area) return area < o.area;
        if (x != o.x) return x > o.x;
        return z > o.z;
    }
};

struct CaveSearchConfig {
    uint64_t seed = 0;
    int mc = 0;
    int startX = 0;
    int startZ = 0;
    int sx = 0;
    int sz = 0;
    int y = -56; // unused; precise sample uses −60/−56/−52
    int minArea = 40000;
    int threads = 0;
    bool fastSearch = true;
    float riverFactor = 0.75f;
};

void runCaveSearch(const CaveSearchConfig &cfg, CaveProgress *progress, std::vector<CaveHit> &out);

#endif
