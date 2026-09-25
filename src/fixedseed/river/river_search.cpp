#include "river_search.h"
#include "Thread.h"

#include "cubiomes/generator.h"
#include "cubiomes/biomes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

static bool checkProgress(RiverProgress *p)
{
    if (!p) return true;
    while (p->try_pause.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (p->try_stop.load()) return false;
    }
    return !p->try_stop.load();
}

static inline int riverValue(int id) { return id == river ? 1 : 0; }

/** One scale-1 plane via genBiomes instead of per-cell getBiomeAt. */
static void fillRiverRawScale1(Generator *g, int startX, int startZ, int W, int H, int y,
                               std::vector<int> &raw)
{
    raw.assign((size_t)W * (size_t)H, 0);
    if (W <= 0 || H <= 0)
        return;
    Range r = {1, startX, startZ, W, H, y, 1};
    int *cache = allocCache(g, r);
    if (!cache)
        return;
    if (genBiomes(g, cache, r) != 0) {
        free(cache);
        return;
    }
    const size_t n = (size_t)W * (size_t)H;
    for (size_t i = 0; i < n; i++)
        raw[i] = riverValue(cache[i]);
    free(cache);
}

/** Keep max-area hit per 64×64 block cell; sort by area descending. */
static void dedupe64MaxArea(std::vector<RiverHit> &hits)
{
    std::unordered_map<uint64_t, size_t> best;
    best.reserve(hits.size());
    for (size_t i = 0; i < hits.size(); i++) {
        const int cx = hits[i].x >> 6;
        const int cz = hits[i].z >> 6;
        const uint64_t key = ((uint64_t)(uint32_t)cx << 32) | (uint32_t)cz;
        auto it = best.find(key);
        if (it == best.end() || hits[i].area > hits[it->second].area)
            best[key] = i;
    }
    std::vector<RiverHit> out;
    out.reserve(best.size());
    for (const auto &kv : best)
        out.push_back(hits[kv.second]);
    std::sort(out.begin(), out.end(),
              [](const RiverHit &a, const RiverHit &b) { return a.area > b.area; });
    hits.swap(out);
}

template<int scale>
static std::vector<RiverHit> findBiggestRiver(
    Generator *g, int startX, int startZ, int sx, int sz, int y, int minArea, double f)
{
    std::vector<RiverHit> result;
    const int W = sx / scale;
    const int H = sz / scale;
    if (W <= 0 || H <= 0) return result;

    const int stride = W + 1;
    std::vector<int> raw((size_t)W * H, 0);
    std::vector<int> prefix((size_t)(W + 1) * (H + 1), 0);

    if constexpr (scale == 1) {
        fillRiverRawScale1(g, startX, startZ, W, H, y, raw);
    } else {
        for (int z = 0; z < H; z++) {
            for (int x = 0; x < W; x++) {
                int worldX = startX + x * scale + scale / 2;
                int worldZ = startZ + z * scale + scale / 2;
                int id = sampleBiomeNoise(&g->bn, nullptr, worldX / 4, y / 4 + 1, worldZ / 4,
                                          nullptr, SAMPLE_NO_SHIFT);
                raw[(size_t)x + (size_t)z * W] = riverValue(id);
            }
        }
    }

    for (int z = 1; z <= H; z++) {
        for (int x = 1; x <= W; x++) {
            prefix[(size_t)x + (size_t)z * stride] =
                raw[(size_t)(x - 1) + (size_t)(z - 1) * W]
                + prefix[(size_t)(x - 1) + (size_t)z * stride]
                + prefix[(size_t)x + (size_t)(z - 1) * stride]
                - prefix[(size_t)(x - 1) + (size_t)(z - 1) * stride];
        }
    }

    const int R_out = 128 / scale;
    const int R_in = 24 / scale;
    if (R_out * 2 >= W || R_out * 2 >= H) return result;

    std::vector<int> dxOut(2 * R_out + 1), dxIn(2 * R_in + 1);
    for (int dz = -R_out; dz <= R_out; dz++)
        dxOut[dz + R_out] = (int)std::floor(std::sqrt((double)R_out * R_out - dz * dz));
    for (int dz = -R_in; dz <= R_in; dz++)
        dxIn[dz + R_in] = (int)std::floor(std::sqrt((double)R_in * R_in - dz * dz));

    auto rect = [&](int L, int R, int row) {
        return prefix[(size_t)(R + 1) + (size_t)(row + 1) * stride]
             - prefix[(size_t)L + (size_t)(row + 1) * stride]
             - prefix[(size_t)(R + 1) + (size_t)row * stride]
             + prefix[(size_t)L + (size_t)row * stride];
    };

    int maxArea = 0;
    struct Hit { int area, x, z; };
    std::vector<Hit> cands;

    for (int cz = R_out; cz < H - R_out; cz++) {
        for (int cx = R_out; cx < W - R_out; cx++) {
            int area = 0;
            for (int dz = -R_out; dz <= R_out; dz++) {
                int row = cz + dz;
                int L = cx - dxOut[dz + R_out];
                int R = cx + dxOut[dz + R_out];
                if (std::abs(dz) > R_in) {
                    area += rect(L, R, row);
                } else {
                    int Lin = cx - dxIn[dz + R_in];
                    int Rin = cx + dxIn[dz + R_in];
                    area += rect(L, Lin - 1, row);
                    area += rect(Rin + 1, R, row);
                }
            }
            int worldArea = area * scale * scale;
            if (worldArea >= minArea && worldArea >= (int)(maxArea * f)) {
                cands.push_back({worldArea, startX + cx * scale, startZ + cz * scale});
                if (worldArea > maxArea) maxArea = worldArea;
            }
        }
    }

    std::sort(cands.begin(), cands.end(), [](const Hit &a, const Hit &b) { return a.area > b.area; });
    for (const Hit &h : cands) {
        if (h.area >= (int)(maxArea * f))
            result.push_back({h.x, h.z, h.area});
        else
            break;
    }
    return result;
}

} // namespace

void runRiverSearch(const RiverSearchConfig &cfg, RiverProgress *progress, std::vector<RiverHit> &out)
{
    out.clear();
    if (cfg.sx < 256 || cfg.sz < 256) {
        if (progress) {
            progress->phase = 2;
            progress->current = 1;
            progress->total = 1;
        }
        return;
    }

    const int chunkSize = 8192;
    const int overlap = 256;
    std::vector<std::pair<int,int>> tiles;
    for (int x = 0; x < cfg.sx; x += chunkSize - overlap)
        for (int z = 0; z < cfg.sz; z += chunkSize - overlap)
            tiles.emplace_back(x, z);

    const int tileCount = (int)tiles.size();
    if (progress) {
        progress->phase = 1;
        progress->current = 0;
        progress->total = std::max(1, tileCount);
    }

    int threads = cfg.threads > 0 ? cfg.threads : (int)std::max(1u, std::thread::hardware_concurrency());
    ThreadSafeResults<RiverHit> global;
    std::atomic_int next{0};
    std::atomic_int done{0};
    const uint64_t seedVal = cfg.seed;
    const int mc = cfg.mc;
    const int y = cfg.y;

    {
        ThreadPool pool((size_t)threads);
        for (int t = 0; t < threads; t++) {
            pool.enqueue([&]() {
                Generator local;
                setupGenerator(&local, mc, FORCE_OCEAN_VARIANTS);
                applySeed(&local, DIM_OVERWORLD, seedVal);

                while (true) {
                    if (!checkProgress(progress)) return;
                    int i = next.fetch_add(1);
                    if (i >= tileCount) return;
                    int ox = tiles[i].first, oz = tiles[i].second;
                    int csx = std::min(chunkSize, cfg.sx - ox);
                    int csz = std::min(chunkSize, cfg.sz - oz);
                    if (csx < 256 || csz < 256) {
                        int d = done.fetch_add(1) + 1;
                        if (progress) progress->current = d;
                        continue;
                    }
                    auto coarse = findBiggestRiver<16>(&local, cfg.startX + ox, cfg.startZ + oz,
                                                       csx, csz, y, cfg.minArea, 0.8);
                    int localMax = 0;
                    for (const auto &c : coarse) {
                        if (!checkProgress(progress)) return;
                        auto fine = findBiggestRiver<4>(&local, c.x - 256, c.z - 256, 512, 512,
                                                        y, cfg.minArea, 1.0);
                        if (fine.empty()) break;
                        if (fine[0].area < localMax * 0.9) break;
                        int relX = fine[0].x - (cfg.startX + ox);
                        int relZ = fine[0].z - (cfg.startZ + oz);
                        if (relX > overlap / 2 && relX < csx - overlap / 2 &&
                            relZ > overlap / 2 && relZ < csz - overlap / 2) {
                            global.addResult(fine[0]);
                            if (fine[0].area > localMax) localMax = fine[0].area;
                        }
                    }
                    int d = done.fetch_add(1) + 1;
                    if (progress) progress->current = d;
                }
            });
        }
    }

    if (progress && progress->try_stop.load()) {
        progress->phase = 2;
        progress->current = progress->total.load();
        return;
    }

    auto all = global.getAllResults();
    std::sort(all.begin(), all.end(), [](const RiverHit &a, const RiverHit &b) { return a.area > b.area; });
    int maxA = all.empty() ? 0 : all[0].area;

    if (all.empty()) {
        if (progress) {
            progress->phase = 2;
            progress->current = 1;
            progress->total = 1;
        }
        return;
    }

    if (progress) {
        progress->total = (int)all.size();
        progress->current = 0;
        progress->phase = 2;
    }

    ThreadSafeResults<RiverHit> refined;
    std::atomic_int nextRefine{0};
    std::atomic_int doneRefine{0};
    std::atomic_int maxAAtomic{maxA};

    {
        ThreadPool pool((size_t)threads);
        for (int t = 0; t < threads; t++) {
            pool.enqueue([&]() {
                Generator local;
                setupGenerator(&local, mc, FORCE_OCEAN_VARIANTS);
                applySeed(&local, DIM_OVERWORLD, seedVal);

                while (true) {
                    if (!checkProgress(progress)) return;
                    int i = nextRefine.fetch_add(1);
                    if (i >= (int)all.size()) return;

                    const RiverHit &it = all[i];
                    const int thr = (int)(maxAAtomic.load() * 0.90);
                    auto temp = findBiggestRiver<1>(&local, it.x - 160, it.z - 160, 320, 320, y, 1, 1.0);
                    if (!temp.empty() && temp[0].area >= thr) {
                        refined.addResult(temp[0]);
                        int cur = maxAAtomic.load();
                        while (temp[0].area > cur &&
                               !maxAAtomic.compare_exchange_weak(cur, temp[0].area)) {
                        }
                    }
                    int d = doneRefine.fetch_add(1) + 1;
                    if (progress) progress->current = d;
                }
            });
        }
    }

    out = refined.getAllResults();
    dedupe64MaxArea(out);

    if (progress)
        progress->current = progress->total.load();
}
