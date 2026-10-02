#include "slime_pipeline.h"
#include "slime_biome_conv.h"
#include "slimerander.h"
#include "radar/gpu_loader.h"
#include "optimizer.hpp"
#include "geometry.hpp"
#include "types.hpp"

#include "cubiomes/generator.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

struct RadarCollect {
    std::mutex mu;
    std::vector<SrResult> results;
    SlimePipelineProgress *progress = nullptr;
    bool serialResults = false;
};

static void onRadarResult(void *ctx, SrResult res)
{
    auto *c = (RadarCollect *)ctx;
    if (c->serialResults) { c->results.push_back(res); return; }
    std::lock_guard<std::mutex> lock(c->mu);
    c->results.push_back(res);
}

static void onRadarProgress(void *ctx, uint64_t done, uint64_t total)
{
    auto *c = (RadarCollect *)ctx;
    if (!c->progress) return;
    if (c->progress->try_stop.load()) sr_control_stop();
    if (c->progress->try_pause.load()) sr_control_pause();
    else sr_control_resume();
    c->progress->phase = 1;
    c->progress->current = total>uint64_t(INT_MAX)
        ? int(std::min<long double>(1.0L,static_cast<long double>(done)/total)*INT_MAX)
        : int(std::min(done,total));
    c->progress->total = int(std::min<uint64_t>(total,INT_MAX));
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
    }
    sr_control_resume();
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
    const char *backend = std::getenv("SR_BACKEND");
    bool forceGPU = backend && std::strcmp(backend,"gpu")==0;
    bool forceCPU = backend && std::strcmp(backend,"cpu")==0;
    uint64_t cells=uint64_t(int64_t(cx1)-cx0)*uint64_t(int64_t(cz1)-cz0);
    int rc=SR_GPU_UNAVAILABLE;
    if(!forceCPU && (forceGPU || cells>=4294967296ULL)){
        if(progress)progress->backend=2;
        rc=sr_try_gpu(&sp,&collect,onRadarResult,onRadarProgress,[](void *ctx){
            return waitCtrl(static_cast<RadarCollect*>(ctx)->progress)?1:0;
        });
    }
    if(rc==SR_GPU_UNAVAILABLE && !forceGPU){
        if(progress)progress->backend=1;
        collect.results.clear(); // Initialization failure occurs before any GPU result.
        collect.serialResults = true; // CPU callbacks occur serially after join.
        rc=sr_search_cpu(&sp,threads,&collect,onRadarResult,onRadarProgress);
    }
    // Never present a partial scan as a completed search after a runtime error.
    if(rc!=SR_OK){if(progress)progress->phase=-2;return;}
    if (rc != SR_OK && rc != 0) return;
    if (!waitCtrl(progress)) {
        if (progress) progress->phase = -1;
        return;
    }

    std::sort(collect.results.begin(), collect.results.end(), [](const SrResult &a, const SrResult &b) {
        // Process nearby candidate windows together so the exact chunk cache
        // is reused. This affects work order only; final hit ranking is below.
        const int64_t ax = floorDiv(a.x, 64), az = floorDiv(a.z, 64);
        const int64_t bx = floorDiv(b.x, 64), bz = floorDiv(b.z, 64);
        if (ax != bx) return ax < bx;
        if (az != bz) return az < bz;
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

    // Preserve every radar candidate. Exact AFK coordinates are deduplicated only after optimization.
    const auto &uniqueCands = collect.results;
    SlimeOptimizer optimizer(params, ringSpans, threads, [&] { return waitCtrl(progress); });

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
        if (a.chunks != b.chunks) return a.chunks > b.chunks;
        if (a.afkX != b.afkX) return a.afkX < b.afkX;
        return a.afkZ < b.afkZ;
    };

    for (size_t i = 0; i < uniqueCands.size(); ++i) {
        if (!waitCtrl(progress)) break;
        Candidate cand;
        cand.chunkX = uniqueCands[i].x;
        cand.chunkZ = uniqueCands[i].z;
        cand.count = (int)uniqueCands[i].count;
        Result r = optimizer.optimize(cand);
        if (!waitCtrl(progress)) break;
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
