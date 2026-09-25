#ifndef SWAMP_HUT_SEARCH_H
#define SWAMP_HUT_SEARCH_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct WitchHutProgress {
    std::atomic_int current{0};
    std::atomic_int total{0};
    std::atomic_bool try_pause{false};
    std::atomic_bool try_stop{false};
};

struct WitchHutHit {
    int x = 0;
    int y = 0;
    int z = 0;
    int dir = 0; // 0=N 1=E 2=S 3=W
};

struct WitchHutSearchConfig {
    uint64_t seed = 0;
    int mc = 0;
    int minX = 0;
    int maxX = 0;
    int minZ = 0;
    int maxZ = 0;
    int maxY = -40; // phase-2 avg_y threshold; phase-1 uses max(maxY, -50)
    int threads = 0;
};

/** Returns false if session init failed (OOM / invalid). On failure, errOut is set. */
bool runWitchHutSearch(const WitchHutSearchConfig &cfg, WitchHutProgress *progress,
                       std::vector<WitchHutHit> &out, std::string *errOut = nullptr);

#endif
