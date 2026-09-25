#ifndef FORTRESS_SEARCH_H
#define FORTRESS_SEARCH_H

#include "Thread.h"

#include <atomic>
#include <cstdint>

struct FortressProgress {
    std::atomic_int current{0};
    std::atomic_int total{0};
    std::atomic_int chunkInRunning{0};
    std::atomic_int phase1{0};
    std::atomic_bool try_pause{false};
    std::atomic_bool try_stop{false};
};

struct FortressHit {
    int chunkX = 0;
    int chunkZ = 0;
    int outX = 0;
    int outY = 0;
    int outZ = 0;
    int mode = 0;
    int shapeCode = 0;
    int longEdge = 0;   // span mode: spanX  (maxX - minX + 1)
    int shortEdge = 0;  // span mode: spanZ  (maxZ - minZ + 1)
    int heightEdge = 0; // span mode: spanY  (maxY - minY + 1)

    bool operator<(const FortressHit &other) const noexcept
    {
        if (outX != other.outX)
            return outX < other.outX;
        return outZ < other.outZ;
    }
};

struct FortressSearchConfig {
    int mode = 0;
    int mc = 0;
    uint64_t seed = 0;
    int centerX = 0;
    int centerZ = 0;
    int r = 0;
    int crossFilter = 2;
    int minLong = 0;
    int minShort = 0;
    int minHeight = 0;
};

void fortressControlPause();
void fortressControlResume();
void fortressControlStop();
void fortressControlReset();

void runFortressSearch(const FortressSearchConfig &cfg,
                       FortressProgress *progress,
                       ThreadSafeResults<FortressHit> &out,
                       int numThreads);

#endif
