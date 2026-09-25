#include "slime_pipeline.h"
#include "slime_biome_conv.h"
#include "slimerander.h"
#include "optimizer.hpp"
#include "geometry.hpp"
#include "types.hpp"

#include "cubiomes/generator.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

struct RadarCollect {
    std::mutex mu;
    std::vector<SrResult> results;
    SlimePipelineProgress *progress = nullptr;
};

static void onRadarResult(void *ctx, SrResult res)
{
    auto *c = (RadarCollect *)ctx;
    std::lock_guard<std::mutex> lock(c->mu);
    c->results.push_back(res);
}

static void onRadarProgress(void *ctx, uint64_t done, uint64_t total)
{
    auto *c = (RadarCollect *)ctx;
    if (!c->progress) return;
    c->progress->phase = 1;
    c->progress->current = (int)std::min<uint64_t>(done, (uint64_t)INT_MAX);
    c->progress->total = (int)std::min<uint64_t>(total, (uint64_t)INT_MAX);
}

static bool waitCtrl(SlimePipelineProgress *p)
{
    if (!p) return sr_control_should_run();
    while (p->try_pause.load()) {
        sr_control_pause();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (p->try_stop.load()) {
            sr_control_stop();
            return false;
        }
        sr_control_resume();
    }
    if (p->try_stop.load()) {
        sr_control_stop();
        return false;
    }
    return sr_control_should_run();
}

static int64_t convertArea(Generator *g, int mc, int64_t afkX, int64_t afkZ,
                           int inner, int outer, bool biomeConv, int64_t rawArea)
{
    if (!biomeConv) return rawArea;
    // Approximate conversion: sample biome at AFK and scale whole area
    int id = getBiomeAt(g, 1, (int)afkX, -64, (int)afkZ);
    int num = 1, den = 1;
    SlimeBiomeConv::factorsFor(mc, id, num, den);
    (void)inner; (void)outer;
    return SlimeBiomeConv::apply((int)rawArea, num, den);
}

} // namespace

void runSlimePipeline(const SlimePipelineConfig &cfg, SlimePipelineProgress *progress,
                      std::vector<SlimePipelineHit> &out)
{
    out.clear();
    sr_control_reset();

    // Block inclusive -> chunk half-open
    int32_t cx0 = cfg.blockX0 >> 4;
    int32_t cz0 = cfg.blockZ0 >> 4;
    int32_t cx1 = (cfg.blockX1 >> 4) + 1;
    int32_t cz1 = (cfg.blockZ1 >> 4) + 1;

    SrParams sp;
    sp.world_seed = cfg.seed;
    sp.threshold = (uint8_t)std::min(255, std::max(1, cfg.threshold));
    sp.x0 = cx0; sp.z0 = cz0; sp.x1 = cx1; sp.z1 = cz1;

    RadarCollect collect;
    collect.progress = progress;
    if (progress) {
        progress->phase = 1;
        progress->current = 0;
        progress->total = 1;
    }

    unsigned threads = cfg.threads > 0 ? (unsigned)cfg.threads : 0;
    int rc = sr_search_cpu(&sp, threads, &collect, onRadarResult, onRadarProgress);
    if (rc != SR_OK && rc != 0) return;
    if (!waitCtrl(progress)) {
        if (progress) progress->phase = -1;
        return;
    }

    std::sort(collect.results.begin(), collect.results.end(), [](const SrResult &a, const SrResult &b) {
        if (a.count != b.count) return a.count > b.count;
        int64_t da = (int64_t)a.x * a.x + (int64_t)a.z * a.z;
        int64_t db = (int64_t)b.x * b.x + (int64_t)b.z * b.z;
        if (da != db) return da < db;
        if (a.x != b.x) return a.x < b.x;
        return a.z < b.z;
    });
    if (!waitCtrl(progress)) {
        if (progress) progress->phase = -1;
        return;
    }

    Params params;
    params.worldSeed = cfg.seed;
    params.chunkSearchRange = 20;
    params.playerSearchRange = 10;
    params.innerRadius = 24;
    params.outerRadius = 128;
    auto ringSpans = computeRingSpans(params.innerRadius, params.outerRadius);

    // Light candidate merge: only collapse near-identical radar peaks.
    // (Previously used playerSearchRange=10, which dropped most candidates.)
    std::vector<SrResult> uniqueCands;
    uniqueCands.reserve(collect.results.size());
    const int mergeR = 2;
    for (const SrResult &r : collect.results) {
        bool near = false;
        for (const SrResult &u : uniqueCands) {
            const int dx = std::abs((int)r.x - (int)u.x);
            const int dz = std::abs((int)r.z - (int)u.z);
            if (dx <= mergeR && dz <= mergeR) {
                near = true;
                break;
            }
        }
        if (!near)
            uniqueCands.push_back(r);
    }

    Generator g;
    setupGenerator(&g, cfg.mc, 0);
    applySeed(&g, DIM_OVERWORLD, (uint64_t)cfg.seed);

    if (progress) {
        progress->phase = 2;
        progress->current = 0;
        progress->total = std::max(1, (int)uniqueCands.size());
    }

    // Keep best hit per exact AFK only (no coarse cell cull).
    auto packKey = [](int32_t a, int32_t b) -> uint64_t {
        return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b;
    };
    std::unordered_map<uint64_t, SlimePipelineHit> byAfk;
    byAfk.reserve(uniqueCands.size());

    auto betterThan = [](const SlimePipelineHit &a, const SlimePipelineHit &b) {
        if (a.area != b.area) return a.area > b.area;
        if (a.rawArea != b.rawArea) return a.rawArea > b.rawArea;
        return a.chunks > b.chunks;
    };

    for (size_t i = 0; i < uniqueCands.size(); ++i) {
        if (!waitCtrl(progress)) break;
        Candidate cand;
        cand.chunkX = uniqueCands[i].x;
        cand.chunkZ = uniqueCands[i].z;
        cand.count = (int)uniqueCands[i].count;
        Result r = optimizeCandidate(params, cand, ringSpans);
        const int64_t rawArea = r.effectiveSpawnArea;
        const int64_t area = convertArea(&g, cfg.mc, r.playerBlockX, r.playerBlockZ,
                                         24, 128, cfg.biomeConv, rawArea);
        if (area >= cfg.minArea) {
            SlimePipelineHit h;
            h.afkX = r.playerBlockX;
            h.afkZ = r.playerBlockZ;
            h.rawArea = rawArea;
            h.area = area;
            h.chunks = r.effectiveSlimeChunks;

            const uint64_t afkKey = packKey((int32_t)h.afkX, (int32_t)h.afkZ);
            auto afkIt = byAfk.find(afkKey);
            if (afkIt == byAfk.end() || betterThan(h, afkIt->second))
                byAfk[afkKey] = h;
        }
        if (progress) progress->current = (int)(i + 1);
    }

    std::vector<SlimePipelineHit> hits;
    hits.reserve(byAfk.size());
    for (auto &kv : byAfk)
        hits.push_back(std::move(kv.second));

    std::sort(hits.begin(), hits.end(), [&](const SlimePipelineHit &a, const SlimePipelineHit &b) {
        return betterThan(a, b);
    });
    out.swap(hits);
}
