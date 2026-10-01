#ifndef FIXEDSEED_EXACT_BIOME_COLUMN_CACHE_H
#define FIXEDSEED_EXACT_BIOME_COLUMN_CACHE_H

#include "cubiomes/generator.h"
#include "cubiomes/noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

// Worker-owned, seed-specific cache. Mirrors sampleBiomeNoise's float rounding
// and expression order; only depth depends on quart Y. No cubiomes modification.
class ExactBiomeColumnCache {
    struct Column {
        int64_t np[6]{};
        double offset = 0;
        int ys[4]{};
        int ids[4]{};
        unsigned count = 0, next = 0;
    };
    const Generator *generator_;
    uint32_t flags_;
    size_t limit_;
    std::unordered_map<uint64_t, Column> columns_;

    Column &column(int x, int z) {
        const uint64_t key = (uint64_t(uint32_t(x)) << 32) | uint32_t(z);
        auto found = columns_.find(key);
        if (found != columns_.end()) return found->second;
        if (columns_.size() >= limit_) columns_.clear();
        const BiomeNoise *bn = &generator_->bn;
        double px = x, pz = z;
        if (!(flags_ & SAMPLE_NO_SHIFT)) {
            px += sampleDoublePerlin(&bn->climate[NP_SHIFT], x, 0, z) * 4.0;
            pz += sampleDoublePerlin(&bn->climate[NP_SHIFT], z, x, 0) * 4.0;
        }
        const float c = sampleDoublePerlin(&bn->climate[NP_CONTINENTALNESS], px, 0, pz);
        const float e = sampleDoublePerlin(&bn->climate[NP_EROSION], px, 0, pz);
        const float w = sampleDoublePerlin(&bn->climate[NP_WEIRDNESS], px, 0, pz);
        Column col;
        if (!(flags_ & SAMPLE_NO_DEPTH)) {
            const float pv = -(std::fabs(std::fabs(w) - 0.6666667F) - 0.33333334F) * 3.0F;
            float params[] = {c, e, pv, w};
            col.offset = getSpline(bn->sp, params) + 0.015F;
        }
        const float t = sampleDoublePerlin(&bn->climate[NP_TEMPERATURE], px, 0, pz);
        const float h = sampleDoublePerlin(&bn->climate[NP_HUMIDITY], px, 0, pz);
        col.np[0] = int64_t(10000.0F * t);
        col.np[1] = int64_t(10000.0F * h);
        col.np[2] = int64_t(10000.0F * c);
        col.np[3] = int64_t(10000.0F * e);
        col.np[5] = int64_t(10000.0F * w);
        return columns_.emplace(key, col).first->second;
    }

public:
    explicit ExactBiomeColumnCache(const Generator *g, uint32_t flags = 0,
                                   size_t maxColumns = 65536)
        : generator_(g), flags_(flags), limit_(std::max<size_t>(16, maxColumns)) {
        columns_.reserve(std::min<size_t>(limit_, 16384));
    }

    int sampleQuart(int x, int y, int z) {
        const BiomeNoise *bn = &generator_->bn;
        if (bn->nptype >= 0)
            return sampleBiomeNoise(bn, nullptr, x, y, z, nullptr, flags_);
        Column &col = column(x, z);
        for (unsigned i = 0; i < col.count; ++i)
            if (col.ys[i] == y) return col.ids[i];
        float depth = 0;
        if (!(flags_ & SAMPLE_NO_DEPTH))
            depth = 1.0 - (y * 4) / 128.0 - 83.0/160.0 + col.offset;
        col.np[4] = int64_t(10000.0F * depth);
        const int id = climateToBiome(bn->mc, reinterpret_cast<const uint64_t *>(col.np), nullptr);
        const unsigned slot = col.next;
        col.ys[slot] = y; col.ids[slot] = id;
        col.next = (slot + 1) % 4;
        col.count = std::min(4u, col.count + 1);
        return id;
    }

    int sampleBlock(int x, int y, int z) {
        if (generator_->mc < MC_1_18)
            return getBiomeAt(generator_, 1, x, y, z);
        int qx, qy, qz;
        voronoiAccess3D(generator_->sha, x, y, z, &qx, &qy, &qz);
        return sampleQuart(qx, qy, qz);
    }
};

#endif
