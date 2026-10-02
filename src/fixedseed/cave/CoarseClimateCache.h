#ifndef FIXEDSEED_COARSE_CLIMATE_CACHE_H
#define FIXEDSEED_COARSE_CLIMATE_CACHE_H
#include "BiomeSampler.h"
#include <array>
#include <unordered_map>

// One phase-1 worker owns this cache for one coarse chunk. Overlapping 512-block
// refine windows reuse exact gates; no seed/config survives between chunks.
class CoarseClimateCache {
    static constexpr int Side = 64;
    struct Tile { std::array<uint8_t, Side * Side> cells; Tile() { cells.fill(2); } };
    std::unordered_map<uint64_t, Tile> tiles_;
    uint64_t lastKey_ = 0;
    Tile *last_ = nullptr;
    static int floorTile(int x) { int q = x / Side; return q - (x % Side < 0); }
public:
    bool sample(const BiomeNoise *bn, int x, int z) {
        const int tx = floorTile(x), tz = floorTile(z);
        const uint64_t key = (uint64_t(uint32_t(tx)) << 32) | uint32_t(tz);
        if (!last_ || lastKey_ != key) {
            auto found = tiles_.find(key);
            if (found == tiles_.end()) {
                if (tiles_.size() >= 512) tiles_.clear();
                found = tiles_.try_emplace(key).first;
            }
            last_ = &found->second;
            lastKey_ = key;
        }
        auto &value = last_->cells[(z - tz * Side) * Side + x - tx * Side];
        if (value == 2) value = passCoarseCaveCell(bn, x, z, COARSE_SAMPLE_FLAGS);
        return value != 0;
    }
};
#endif
