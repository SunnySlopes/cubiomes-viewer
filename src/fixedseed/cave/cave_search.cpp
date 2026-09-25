#include "cave_search.h"

/* Include finder as a library unit (same pattern as upstream JNI bridge). */
#ifndef DRIPSTONECAVE_FINDER_JNI_LIB
#define DRIPSTONECAVE_FINDER_JNI_LIB
#endif
#include "dripstonecave_finder.cpp"

#include "cubiomes/generator.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

namespace {

static constexpr int kRingOuter = 128;
static constexpr float kPreserveRange = 0.9f;

static std::vector<Res> dedupHits(const std::vector<Res> &v)
{
    std::set<Point> seen;
    std::vector<Res> out;
    out.reserve(v.size());
    for (const auto &item : v) {
        if (seen.insert(item.point).second)
            out.push_back(item);
    }
    return out;
}

static void applyPreserveRange(std::vector<Res> &res, float opV)
{
    if (opV <= 0.0f || res.empty())
        return;
    const double thr = (double)res[0].area * (double)opV;
    size_t keep = 0;
    while (keep < res.size() && (double)res[keep].area >= thr)
        keep++;
    res.resize(keep);
}

/** Layout-compatible view of CaveProgress as finder's Progress. */
static Progress *asFinderProgress(CaveProgress *p)
{
    static_assert(sizeof(CaveProgress) == sizeof(Progress),
                  "CaveProgress must match finder Progress layout");
    return reinterpret_cast<Progress *>(p);
}

} // namespace

void runCaveSearch(const CaveSearchConfig &cfg, CaveProgress *progress, std::vector<CaveHit> &out)
{
    out.clear();

    float riverWeight = cfg.riverFactor;
    if (riverWeight < 0.0f) riverWeight = 0.0f;
    if (riverWeight > 1.0f) riverWeight = 1.0f;

    // Expand by ring outer radius so edge peaks can be scored (JNI bridge).
    const int startX = cfg.startX - kRingOuter;
    const int startZ = cfg.startZ - kRingOuter;
    const int width = cfg.sx + 2 * kRingOuter;
    const int height = cfg.sz + 2 * kRingOuter;

    if (width < SearchConfig::CANDIDATE_TILE_BLOCKS
        || height < SearchConfig::CANDIDATE_TILE_BLOCKS) {
        if (progress) {
            progress->phase1 = 2;
            progress->current = 1;
            progress->total = 1;
        }
        return;
    }

    Generator g;
    setupGenerator(&g, cfg.mc, FORCE_OCEAN_VARIANTS);
    applySeed(&g, DIM_OVERWORLD, cfg.seed);

    Progress *prog = progress ? asFinderProgress(progress) : nullptr;
    if (prog) {
        prog->chunkInRunning.store(0);
        prog->current.store(0);
        prog->total.store(0);
        prog->phase1.store(1);
        // try_pause / try_stop left to caller
    }

    int threads = cfg.threads > 0 ? cfg.threads
                                  : (int)std::max(1u, std::thread::hardware_concurrency());
    if (threads < 1) threads = 1;

    ThreadSafeResults<Res> globalResults;

    // fastSearch: Cont@128 + Weird@32 (SearchConfig defaults via 0,0)
    // !fastSearch: Cont@32 + Weird@16
    const int contScale = cfg.fastSearch ? 0 : 32;
    const int weirdScale = cfg.fastSearch ? 0 : 16;

    if (contScale <= 0 && weirdScale <= 0) {
        findBiggestRiverParallelPool(globalResults, &g, startX, startZ, width, height,
                                     cfg.minArea, prog, threads);
    } else {
        int cont = contScale > 0 ? contScale : SearchConfig::PHASE1_CONT_PREFILTER_SCALE;
        int weird = weirdScale > 0 ? weirdScale : SearchConfig::PHASE1_WEIRDNESS_GRID_SCALE;
        if (cont % weird != 0)
            cont = weird;
        findBiggestRiverParallelPool(globalResults, &g, startX, startZ, width, height,
                                     cfg.minArea, prog, threads, cont, 1, weird);
    }

    if (prog && prog->try_stop.load()) {
        prog->phase1.store(2);
        prog->current.store(prog->total.load());
        return;
    }

    auto res = globalResults.getAllResults();
    std::sort(res.begin(), res.end(), [](const Res &a, const Res &b) { return a.area > b.area; });
    applyPreserveRange(res, kPreserveRange);

    if (prog) {
        prog->phase1.store(2);
        prog->total.store((int)res.size());
        prog->current.store(0);
        prog->chunkInRunning.store(0);
    }

    globalResults.clear();

    if (res.empty()) {
        if (prog) {
            prog->current.store(1);
            prog->total.store(1);
            prog->phase1.store(-1);
        }
        return;
    }

    {
        ThreadPool pool((size_t)threads);
        for (const auto &it : res) {
            if (prog && prog->try_stop.load())
                break;
            pool.enqueue([&, cand = it, seedVal = cfg.seed, minArea = cfg.minArea,
                          riverWeight, mc = cfg.mc]() {
                if (prog && prog->try_stop.load())
                    return;
                while (prog && prog->try_pause.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    if (prog->try_stop.load())
                        return;
                }
                if (prog)
                    prog->chunkInRunning.fetch_add(1);

                thread_local Generator tlsG;
                thread_local bool tlsInited = false;
                thread_local int tlsMc = -1;
                thread_local uint64_t tlsSeed = ~0ull;
                if (!tlsInited || tlsMc != mc) {
                    setupGenerator(&tlsG, mc, FORCE_OCEAN_VARIANTS);
                    tlsInited = true;
                    tlsMc = mc;
                    tlsSeed = ~0ull;
                }
                if (tlsSeed != seedVal) {
                    applySeed(&tlsG, DIM_OVERWORLD, seedVal);
                    tlsSeed = seedVal;
                }

                auto temp = findBiggestRiver<1>(
                    &tlsG,
                    cand.point.x - SearchConfig::REFINE_HALF_WINDOW,
                    cand.point.y - SearchConfig::REFINE_HALF_WINDOW,
                    SearchConfig::REFINE_WINDOW_BLOCKS, SearchConfig::REFINE_WINDOW_BLOCKS,
                    minArea, 1.0, FilterMode::PreciseBiome, riverWeight);

                if (!temp.empty() && temp[0].area >= minArea)
                    globalResults.addResult(temp[0]);

                if (prog) {
                    prog->chunkInRunning.fetch_sub(1);
                    prog->current.fetch_add(1);
                }
            });
        }
    }

    if (prog && prog->try_stop.load()) {
        prog->phase1.store(2);
        return;
    }

    auto finalList = dedupHits(globalResults.getAllResults());
    out.reserve(finalList.size());
    for (const auto &item : finalList) {
        CaveHit h;
        h.x = item.point.x;
        h.z = item.point.y;
        h.area = item.area;
        h.caveArea = item.caveArea;
        h.riverArea = item.riverArea;
        out.push_back(h);
    }
    std::sort(out.begin(), out.end(),
              [](const CaveHit &a, const CaveHit &b) { return a.area > b.area; });

    if (prog) {
        prog->current.store(prog->total.load());
        prog->phase1.store(-1);
    }
}
