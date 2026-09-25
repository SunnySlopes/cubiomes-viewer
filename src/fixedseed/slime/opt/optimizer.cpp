#include "optimizer.hpp"

#include "geometry.hpp"
#include "slime_check.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

// std::thread is unavailable on some toolchains (notably MinGW built with the
// win32 thread model, where libstdc++ omits <thread>). Detect real support and
// otherwise fall back to a correct single-threaded scan. (types.hpp above has
// already pulled in a libstdc++ header, so _GLIBCXX_HAS_GTHREADS is defined if
// the gthreads model is active.)
#if defined(_MSC_VER) || defined(_GLIBCXX_HAS_GTHREADS) || defined(_LIBCPP_VERSION)
#  include <thread>
#  define SLIME_HAVE_THREAD 1
#else
#  define SLIME_HAVE_THREAD 0
#endif

Result optimizeCandidate(const Params& params,
                         const Candidate& candidate,
                         const std::vector<RingSpan>& ringSpans) {
    const int pr = params.playerSearchRange;

    // Chunk reach that the ring can touch from any AFK block in the search area:
    //   player search (pr chunks) + ring radius (ceil(M/16) chunks) + slack.
    const int reach = pr + (params.outerRadius + 15) / 16 + 1;

    // Slime map covers max(chunkSearchRange, reach) so lookups never go OOB.
    const int range = std::max(params.chunkSearchRange, reach);
    const int dim   = 2 * range + 1;
    const int32_t minCX = candidate.chunkX - range;
    const int32_t minCZ = candidate.chunkZ - range;

    std::vector<uint8_t> slime(static_cast<size_t>(dim) * dim, 0);
    for (int i = 0; i < dim; ++i) {
        for (int j = 0; j < dim; ++j) {
            slime[static_cast<size_t>(i) * dim + j] =
                isSlimeChunk(params.worldSeed, minCX + i, minCZ + j) ? 1u : 0u;
        }
    }

    // Player search area in blocks: (2*pr + 1) chunks wide (176 blocks for pr=5).
    const int span = (2 * pr + 1) * 16;
    const int64_t baseX = static_cast<int64_t>(candidate.chunkX - pr) * 16;
    const int64_t baseZ = static_cast<int64_t>(candidate.chunkZ - pr) * 16;

    // Local "stamp" grid used to count distinct slime chunks per AFK position
    // without clearing it between iterations.
    const int localDim   = 2 * reach + 1;
    const int32_t lMinCX = candidate.chunkX - reach;
    const int32_t lMinCZ = candidate.chunkZ - reach;

    struct Best {
        int64_t area   = -1;
        int     chunks = -1;
        int64_t px     = 0;
        int64_t pz     = 0;
    };

#if SLIME_HAVE_THREAD
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    const int nthreads = std::max(1, std::min<int>(static_cast<int>(hw), span));
#else
    const int nthreads = 1;
#endif

    std::vector<Best> bests(nthreads);

    auto work = [&](int t) {
        Best best;
        std::vector<int32_t> stamp(static_cast<size_t>(localDim) * localDim, -1);
        int32_t visit = 0;

        for (int i = t; i < span; i += nthreads) {
            const int64_t px = baseX + i;
            for (int j = 0; j < span; ++j) {
                const int64_t pz = baseZ + j;
                ++visit;

                int64_t area   = 0;
                int     chunks = 0;

                // 按行区间扫描:每行 dz 上,方块 x 范围 [px+loDx, px+hiDx],
                // 都在区块行 cz 上。沿 x 按区块跳跃,一次处理落在同一区块里的
                // 一整段方块 —— 把每行的运算从 ~宽度 降到 ~宽度/16。
                for (const auto& sp : ringSpans) {
                    const int64_t bz = pz + sp.dz;
                    const int32_t cz = static_cast<int32_t>(floorDiv(bz, 16));
                    const int64_t xStart = px + sp.loDx;
                    const int64_t xEnd   = px + sp.hiDx;   // 含端点

                    int64_t bx = xStart;
                    while (bx <= xEnd) {
                        const int32_t cx = static_cast<int32_t>(floorDiv(bx, 16));
                        const int64_t chunkEndBx = static_cast<int64_t>(cx) * 16 + 15;
                        const int64_t segEnd = std::min(xEnd, chunkEndBx);
                        const int64_t segLen = segEnd - bx + 1;

                        if (slime[static_cast<size_t>(cx - minCX) * dim + (cz - minCZ)]) {
                            area += segLen;
                            const size_t li =
                                static_cast<size_t>(cx - lMinCX) * localDim + (cz - lMinCZ);
                            if (stamp[li] != visit) {
                                stamp[li] = visit;
                                ++chunks;
                            }
                        }
                        bx = segEnd + 1;
                    }
                }

                if (area > best.area ||
                    (area == best.area && chunks > best.chunks)) {
                    best.area   = area;
                    best.chunks = chunks;
                    best.px     = px;
                    best.pz     = pz;
                }
            }
        }
        bests[t] = best;
    };

#if SLIME_HAVE_THREAD
    if (nthreads > 1) {
        std::vector<std::thread> threads;
        threads.reserve(nthreads);
        for (int t = 0; t < nthreads; ++t) threads.emplace_back(work, t);
        for (auto& th : threads) th.join();
    } else {
        work(0);
    }
#else
    work(0);
#endif

    Best best;
    for (const auto& b : bests) {
        if (b.area > best.area ||
            (b.area == best.area && b.chunks > best.chunks)) {
            best = b;
        }
    }

    Result r;
    r.inputChunkX          = candidate.chunkX;
    r.inputChunkZ          = candidate.chunkZ;
    r.inputCount           = candidate.count;
    r.playerBlockX         = best.px;
    r.playerBlockZ         = best.pz;
    r.effectiveSlimeChunks = best.chunks < 0 ? 0 : best.chunks;
    r.effectiveSpawnArea   = best.area   < 0 ? 0 : best.area;
    return r;
}
