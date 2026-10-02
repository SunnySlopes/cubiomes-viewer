// Adapted from LowYDripstoneCaveFinderGUI b4b499d (MIT).
// Changes: double tables, worker-local binding, resource-failure fallback.
#ifndef OCTAVE_FIELD_CACHE_H
#define OCTAVE_FIELD_CACHE_H

#include "cubiomes/biomenoise.h"
#include "cubiomes/noise.h"
#include <atomic>
#include <cstdint>
#include <vector>

/**
 * Period tables for Cont octA[0..4] (@contScale) and Ridge/Weirdness octA[0..2] (@weirdScale).
 * Sample lattice follows the requested origin; non-lattice points use live noise.
 */
struct OctaveACache {
    static constexpr int CONT_OCT = 5;
    static constexpr int RIDGE_OCT = 3;

    struct Field {
        int periodWorld = 0;
        int sampleScale = 0;
        int phaseX = 0, phaseZ = 0;
        int stepShift = -1;
        uint32_t stepMask = 0, periodMask = 0;
        bool onLattice(int bx, int bz) const {
            if (stepShift >= 0)
                return ((uint32_t(bx - phaseX) | uint32_t(bz - phaseZ)) & stepMask) == 0;
            const int step = sampleScale / 4;
            return step > 0 && (bx - phaseX) % step == 0 && (bz - phaseZ) % step == 0;
        }
        int n = 0; /* periodWorld / sampleScale */
        std::vector<double> data; /* n * n, row-major */

        double atClimate(int bx, int bz) const {
            if (stepShift >= 0) {
                const uint32_t ix = (uint32_t(bx - phaseX) >> stepShift) & periodMask;
                const uint32_t iz = (uint32_t(bz - phaseZ) >> stepShift) & periodMask;
                return data[(size_t)iz * n + ix];
            }
            if (n <= 0 || data.empty() || sampleScale <= 0) return 0.0;
            const int step = sampleScale / 4;
            auto latticeIndex = [&](int b, int phase) {
                const int diff = b - phase;
                int q = diff / step - (diff % step < 0);
                q %= n;
                return q < 0 ? q + n : q;
            };
            return data[(size_t)latticeIndex(bz, phaseZ) * n + latticeIndex(bx, phaseX)];
        }
    };

    Field contA[CONT_OCT];
    Field ridgeA[RIDGE_OCT];
    bool ready = false;
};

/** World-block periods for Cont A0..A4 and Ridge A0..A2. */
static constexpr int kContAPeriodWorld[OctaveACache::CONT_OCT] = {
    524288, 262144, 131072, 65536, 32768
};
static constexpr int kRidgeAPeriodWorld[OctaveACache::RIDGE_OCT] = {
    131072, 65536, 32768
};

/**
 * Build Cont+Ridge A period tables from seeded BiomeNoise.
 * Progress: current/total over all cells; phase should be 0. Honors pause/stop atomics.
 * Uses double values to avoid changing climate quantization near thresholds.
 */
bool buildOctaveACache(
    OctaveACache *out,
    const BiomeNoise *bn,
    int contScale,
    int weirdScale,
    int numThreads,
    std::atomic_int *progressCurrent,
    std::atomic_int *progressTotal,
    const std::atomic_bool *tryPause,
    const std::atomic_bool *tryStop, int startX = 0, int startZ = 0);

void setActiveOctaveACache(const OctaveACache *cache); /* nullptr disables */
const OctaveACache *getActiveOctaveACache();

struct ScopedOctaveACache { const OctaveACache *old; explicit ScopedOctaveACache(const OctaveACache *c): old(getActiveOctaveACache()) { setActiveOctaveACache(c); } ~ScopedOctaveACache() { setActiveOctaveACache(old); } };

#endif
