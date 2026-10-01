#include "optimizer.hpp"
#include "slime_check.hpp"
#include <algorithm>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <utility>

namespace {
struct Best {
    int64_t area = -1, x = 0, z = 0;
    int chunks = -1;
};
bool better(const Best &a, const Best &b) {
    if (a.area != b.area) return a.area > b.area;
    if (a.chunks != b.chunks) return a.chunks > b.chunks;
    if (a.x != b.x) return a.x < b.x;
    return a.z < b.z;
}
uint64_t key(int32_t x, int32_t z) {
    return (uint64_t(uint32_t(x)) << 32) | uint32_t(z);
}
}

struct SlimeOptimizer::Impl {
    Params params;
    unsigned threads;
    std::function<bool()> control;
    int radius, dim;
    // For each relative chunk, 256 exact intersection areas, one per AFK
    // position inside its own chunk. Values are <= 256.
    std::vector<uint16_t> weights;
    std::unordered_map<uint64_t, Best> cache;

    Impl(const Params &p, const std::vector<RingSpan> &spans,
         unsigned n, std::function<bool()> c)
        : params(p), threads(n ? n : std::thread::hardware_concurrency()),
          control(std::move(c)), radius((p.outerRadius + 15) / 16),
          dim(2 * radius + 1), weights(size_t(dim) * dim * 256, 0) {
        threads = std::max(1u, threads);
        for (int ox = 0; ox < 16; ++ox)
            for (int oz = 0; oz < 16; ++oz)
                for (const RingSpan &s : spans) {
                    const int cz = int(floorDiv(oz + s.dz, 16));
                    const int end = ox + s.hiDx;
                    for (int x = ox + s.loDx; x <= end;) {
                        const int cx = int(floorDiv(x, 16));
                        const int last = std::min(end, cx * 16 + 15);
                        weights[(size_t(cx + radius) * dim + cz + radius) * 256
                                + ox * 16 + oz] += uint16_t(last - x + 1);
                        x = last + 1;
                    }
                }
    }

    Best evaluateChunk(int32_t cx, int32_t cz) const {
        int64_t areas[256] = {};
        int counts[256] = {};
        for (int dx = -radius; dx <= radius; ++dx)
            for (int dz = -radius; dz <= radius; ++dz) {
                if (!isSlimeChunk(params.worldSeed, cx + dx, cz + dz)) continue;
                const uint16_t *w = &weights[(size_t(dx + radius) * dim
                                            + dz + radius) * 256];
                for (int i = 0; i < 256; ++i) {
                    areas[i] += w[i];
                    counts[i] += w[i] != 0;
                }
            }
        Best best;
        for (int i = 0; i < 256; ++i) {
            Best b;
            b.area = areas[i]; b.chunks = counts[i];
            b.x = int64_t(cx) * 16 + i / 16;
            b.z = int64_t(cz) * 16 + i % 16;
            if (better(b, best)) best = b;
        }
        return best;
    }

    Result optimize(const Candidate &candidate) {
        struct Missing { int32_t x, z; Best best; };
        std::vector<Missing> missing;
        Best best;
        const int pr = params.playerSearchRange;
        // Bounded session cache. Clearing only changes performance, never the
        // search domain or answers; ~6 MiB payload at this limit.
        if (cache.size() > 131072) cache.clear();
        for (int dx = -pr; dx <= pr; ++dx)
            for (int dz = -pr; dz <= pr; ++dz) {
                int32_t x = candidate.chunkX + dx, z = candidate.chunkZ + dz;
                auto it = cache.find(key(x, z));
                if (it != cache.end()) {
                    if (better(it->second, best)) best = it->second;
                } else missing.push_back({x, z, Best{}});
            }
        std::atomic<size_t> next{0};
        std::atomic<bool> stopped{false};
        // Only the calling thread touches the user control callback or cache.
        // Each batch costs a bounded number of chunk evaluations.
        for (size_t start = 0; start < missing.size(); start += 256) {
            if (control && !control()) { stopped = true; break; }
            const size_t end = std::min(missing.size(), start + 256);
            next = start;
            auto work = [&] {
                for (;;) {
                    const size_t i = next.fetch_add(1, std::memory_order_relaxed);
                    if (i >= end) break;
                    missing[i].best = evaluateChunk(missing[i].x, missing[i].z);
                }
            };
            const unsigned n = std::min<unsigned>(threads, unsigned(end - start));
            std::vector<std::thread> workers;
            for (unsigned t = 1; t < n; ++t) workers.emplace_back(work);
            work();
            for (auto &w : workers) w.join();
            for (size_t i = start; i < end; ++i) {
                cache.emplace(key(missing[i].x, missing[i].z), missing[i].best);
                if (better(missing[i].best, best)) best = missing[i].best;
            }
        }
        Result r;
        r.inputChunkX = candidate.chunkX; r.inputChunkZ = candidate.chunkZ;
        r.inputCount = candidate.count;
        if (!stopped && best.area >= 0) {
            r.playerBlockX = best.x; r.playerBlockZ = best.z;
            r.effectiveSpawnArea = best.area; r.effectiveSlimeChunks = best.chunks;
        }
        return r;
    }
};

SlimeOptimizer::SlimeOptimizer(const Params &p, const std::vector<RingSpan> &s,
                               unsigned n, std::function<bool()> c)
    : impl(new Impl(p, s, n, std::move(c))) {}
SlimeOptimizer::~SlimeOptimizer() = default;
Result SlimeOptimizer::optimize(const Candidate &c) { return impl->optimize(c); }
Result optimizeCandidate(const Params &p, const Candidate &c,
                         const std::vector<RingSpan> &s) {
    SlimeOptimizer optimizer(p, s);
    return optimizer.optimize(c);
}
