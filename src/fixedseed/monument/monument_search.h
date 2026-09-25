#ifndef MONUMENT_SEARCH_H
#define MONUMENT_SEARCH_H

#include <atomic>
#include <cstdint>

struct Progress {
    std::atomic_int current{0};
    std::atomic_int total{0};
    std::atomic_int status{0};
    std::atomic_bool try_pause{false};
    std::atomic_bool try_stop{false};
};

struct PairHit {
    int hangX = 0;
    int hangZ = 0;
    int dx = 0;
    int dz = 0;
    int lowEfficiency = 0;

    bool operator<(const PairHit &o) const noexcept;
};

inline bool PairHit::operator<(const PairHit &o) const noexcept
{
    if (hangX != o.hangX)
        return hangX < o.hangX;
    return hangZ < o.hangZ;
}

struct MonumentSearchConfig {
    uint64_t seed = 0;
    int minX = 0;
    int maxX = 0;
    int minZ = 0;
    int maxZ = 0;
    int mc = 25;
    bool pauseOnPairFound = false;
};

template<typename T>
class ThreadSafeResults;

void runMonumentSearch(
    const MonumentSearchConfig &cfg,
    Progress *progress,
    ThreadSafeResults<PairHit> &out,
    int numThreads);

#endif
