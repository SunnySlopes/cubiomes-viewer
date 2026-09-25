#include "cubiomes/generator.h"
#include "cubiomes/biomes.h"
#include <vector>
#include <algorithm>
#include <queue>
#include <iostream>
#include <chrono>
#include "../Thread.h"
#include "BiomeSampler.h"
#include "SearchConfig.h"
#include <functional>
#include <unordered_map>
#include <cmath>
#include <cstdint>
#include <thread>

enum class FilterMode {
    WeirdnessOnly,
    ContinentalnessOnly,
    ContThenWeird,
    WeirdThenCont,
    ClimateCoarse,
    PreciseBiome,
    /** Cell value taken from contPrefilter mask (no extra noise). */
    UsePrefilterMask
};

struct RingMask {
    struct Row { int dz; int out; int in; };
    std::vector<Row> rows;
    int R_out = 0;
};

template<int scale>
static const RingMask &getRingMask()
{
    static const RingMask mask = [] {
        RingMask m;
        m.R_out = 128 / scale;
        const int R_in = 24 / scale;
        std::vector<int> dxOut(2 * m.R_out + 1);
        std::vector<int> dxIn(2 * R_in + 1);
        for (int dz = -m.R_out; dz <= m.R_out; dz++)
            dxOut[dz + m.R_out] = (int) std::floor(std::sqrt((double) m.R_out * m.R_out - dz * dz));
        for (int dz = -R_in; dz <= R_in; dz++)
            dxIn[dz + R_in] = (int) std::floor(std::sqrt((double) R_in * R_in - dz * dz));
        m.rows.reserve(2 * m.R_out + 1);
        for (int dz = -m.R_out; dz <= m.R_out; dz++)
        {
            RingMask::Row row{};
            row.dz = dz;
            row.out = dxOut[dz + m.R_out];
            row.in = (std::abs(dz) <= R_in) ? dxIn[dz + R_in] : -1;
            m.rows.push_back(row);
        }
        return m;
    }();
    return mask;
}

static inline int coarseCellValue(Generator *g, int worldX, int worldZ, FilterMode mode)
{
    const int nx = worldX / 4;
    const int nz = worldZ / 4;
    if (mode == FilterMode::WeirdnessOnly)
        return passCaveWeirdness(&g->bn, nx, nz, COARSE_SAMPLE_FLAGS) ? 1 : 0;
    if (mode == FilterMode::ContinentalnessOnly)
        return passContinentalnessPartial(&g->bn, nx, nz) ? 1 : 0;
    if (mode == FilterMode::ContThenWeird)
    {
        if (!passContinentalnessPartial(&g->bn, nx, nz))
            return 0;
        return passCaveWeirdness(&g->bn, nx, nz, COARSE_SAMPLE_FLAGS) ? 1 : 0;
    }
    if (mode == FilterMode::WeirdThenCont)
    {
        if (!passCaveWeirdness(&g->bn, nx, nz, COARSE_SAMPLE_FLAGS))
            return 0;
        return passContinentalnessPartial(&g->bn, nx, nz) ? 1 : 0;
    }

    return passCoarseCaveCell(&g->bn, nx, nz, COARSE_SAMPLE_FLAGS) ? 1 : 0;
}

/** Build Cont@scale AND Weirdness@scale mask, then Chebyshev-dilate by `dilate` tiles. */
static std::vector<uint8_t> buildDilatedContWeirdPrefilterMask(
    Generator *g, int startX, int startZ, int sx, int sz,
    int scale, double contThr, int dilate)
{
    const int W = sx / scale;
    const int H = sz / scale;
    std::vector<uint8_t> keep((size_t) std::max(0, W) * std::max(0, H), 0);
    if (W <= 0 || H <= 0)
        return keep;

    for (int z = 0; z < H; z++)
    {
        const int worldZ = startZ + z * scale + scale / 2;
        for (int x = 0; x < W; x++)
        {
            const int worldX = startX + x * scale + scale / 2;
            const int nx = worldX / 4;
            const int nz = worldZ / 4;
            if (!passContinentalnessPartialThr(&g->bn, nx, nz, contThr))
                continue;
            if (!passCaveWeirdness(&g->bn, nx, nz, COARSE_SAMPLE_FLAGS))
                continue;
            keep[(size_t) z * W + x] = 1;
        }
    }

    if (dilate <= 0)
        return keep;

    std::vector<uint8_t> out = keep;
    for (int z = 0; z < H; z++)
    {
        for (int x = 0; x < W; x++)
        {
            if (!keep[(size_t) z * W + x])
                continue;
            for (int dz = -dilate; dz <= dilate; dz++)
            {
                for (int dx = -dilate; dx <= dilate; dx++)
                {
                    const int nx = x + dx;
                    const int nz = z + dz;
                    if (nx < 0 || nz < 0 || nx >= W || nz >= H)
                        continue;
                    out[(size_t) nz * W + nx] = 1;
                }
            }
        }
    }
    return out;
}

/** Build C>thr mask at `scale`, then Chebyshev-dilate by `dilate` tiles. */
static std::vector<uint8_t> buildDilatedContPrefilterMask(
    Generator *g, int startX, int startZ, int sx, int sz,
    int scale, double thr, int dilate)
{
    const int W = sx / scale;
    const int H = sz / scale;
    std::vector<uint8_t> keep((size_t) std::max(0, W) * std::max(0, H), 0);
    if (W <= 0 || H <= 0)
        return keep;

    for (int z = 0; z < H; z++)
    {
        const int worldZ = startZ + z * scale + scale / 2;
        for (int x = 0; x < W; x++)
        {
            const int worldX = startX + x * scale + scale / 2;
            if (passContinentalnessPartialThr(&g->bn, worldX / 4, worldZ / 4, thr))
                keep[(size_t) z * W + x] = 1;
        }
    }

    if (dilate <= 0)
        return keep;

    std::vector<uint8_t> out = keep;
    for (int z = 0; z < H; z++)
    {
        for (int x = 0; x < W; x++)
        {
            if (!keep[(size_t) z * W + x])
                continue;
            for (int dz = -dilate; dz <= dilate; dz++)
            {
                for (int dx = -dilate; dx <= dilate; dx++)
                {
                    const int nx = x + dx;
                    const int nz = z + dz;
                    if (nx < 0 || nz < 0 || nx >= W || nz >= H)
                        continue;
                    out[(size_t) nz * W + nx] = 1;
                }
            }
        }
    }
    return out;
}

static inline bool contPrefilterAllows(
    const std::vector<uint8_t> &mask, int maskW, int maskH,
    int startX, int startZ, int preScale, int worldX, int worldZ)
{
    if (mask.empty() || maskW <= 0 || maskH <= 0 || preScale <= 0)
        return true;
    const int tx = (worldX - startX) / preScale;
    const int tz = (worldZ - startZ) / preScale;
    if (tx < 0 || tz < 0 || tx >= maskW || tz >= maskH)
        return false;
    return mask[(size_t) tz * maskW + tx] != 0;
}

static void fillPreciseGrid(Generator *g, int startX, int startZ, int W, int H,
                            std::vector<int> &rawRiver, std::vector<int> &rawCave)
{
    fillPreciseWindow(g, startX, startZ, W, H, rawRiver, rawCave);
}

struct Point {
    int x = 0;
    int y = 0;

    bool operator<(const Point &o) const noexcept
    {
        if (x != o.x) return x < o.x;
        return y < o.y;
    }
};

struct Res {
    Point point;
    int area = 0;
    int caveArea = 0;
    int riverArea = 0;

    bool operator<(const Res &other) const noexcept
    {
        if (area != other.area) return area < other.area;
        return point < other.point;
    }

    Res() = default;
    Res(Point p, int total, int cave, int river = 0)
        : point(p), area(total), caveArea(cave), riverArea(river) {}
};

struct Progress {
    std::atomic_int current{0};
    std::atomic_int total{0};
    std::atomic_int chunkInRunning{0};
    std::atomic_int phase1{0};
    std::atomic_bool try_pause{false};
    std::atomic_bool try_stop{false};
};

template<int scale>
std::vector<Res> findBiggestRiver(
    Generator *g,
    int startX, int startZ,
    int sx, int sz,
    int min,
    double f,
    FilterMode mode,
    float riverWeight = 0.7f,
    const std::vector<uint8_t> *contPrefilter = nullptr,
    int contPrefilterScale = 0) noexcept
{
    std::vector<Res> result;
    const int W = sx / scale;
    const int H = sz / scale;
    if (W <= 0 || H <= 0) return result;

    const int preW = (contPrefilter && contPrefilterScale > 0) ? (sx / contPrefilterScale) : 0;
    const int preH = (contPrefilter && contPrefilterScale > 0) ? (sz / contPrefilterScale) : 0;

    const int stride = W + 1;
    std::vector<int> rawRiver((size_t) W * H, 0);
    std::vector<int> prefixRiver((size_t) (W + 1) * (H + 1), 0);

    // Cave grids only needed for precise (scale==1) path.
    std::vector<int> rawCave;
    std::vector<int> prefixCave;
    if constexpr (scale == 1)
    {
        rawCave.assign((size_t) W * H, 0);
        prefixCave.assign((size_t) (W + 1) * (H + 1), 0);
    }

#define RAWR(x,z) rawRiver[(size_t)(x) + (size_t)(z) * W]
#define RAWC(x,z) rawCave[(size_t)(x) + (size_t)(z) * W]
#define ARRR(x,z) prefixRiver[(size_t)(x) + (size_t)(z) * stride]
#define ARRC(x,z) prefixCave[(size_t)(x) + (size_t)(z) * stride]

    const int R_out = 128 / scale;
    const int occTile = std::max(8, R_out);
    const int oW = (W + occTile - 1) / occTile;
    const int oH = (H + occTile - 1) / occTile;
    std::vector<uint8_t> occ((size_t) std::max(0, oW) * std::max(0, oH), 0);

    auto markOcc = [&](int x, int z) {
        if (oW <= 0 || oH <= 0) return;
        occ[(size_t) (z / occTile) * oW + (size_t) (x / occTile)] = 1;
    };

    if constexpr (scale > 1)
    {
        for (int z = 0; z < H; z++)
        {
            const int worldZ = startZ + z * scale + scale / 2;
            for (int x = 0; x < W; x++)
            {
                const int worldX = startX + x * scale + scale / 2;
                if (mode == FilterMode::UsePrefilterMask)
                {
                    const int v = contPrefilter &&
                        contPrefilterAllows(*contPrefilter, preW, preH,
                                            startX, startZ, contPrefilterScale, worldX, worldZ)
                        ? 1 : 0;
                    RAWR(x, z) = v;
                    if (v)
                        markOcc(x, z);
                    continue;
                }
                if (contPrefilter &&
                    !contPrefilterAllows(*contPrefilter, preW, preH,
                                         startX, startZ, contPrefilterScale, worldX, worldZ))
                {
                    RAWR(x, z) = 0;
                    continue;
                }
                const int v = coarseCellValue(g, worldX, worldZ, mode);
                RAWR(x, z) = v;
                if (v)
                    markOcc(x, z);
            }
        }
    } else
    {
        fillPreciseGrid(g, startX, startZ, W, H, rawRiver, rawCave);
        for (int z = 0; z < H; z++)
        {
            for (int x = 0; x < W; x++)
            {
                if (RAWR(x, z) || RAWC(x, z))
                    markOcc(x, z);
            }
        }
    }

    for (int z = 1; z <= H; z++)
    {
        for (int x = 1; x <= W; x++)
        {
            ARRR(x, z) = RAWR(x - 1, z - 1) + ARRR(x - 1, z) + ARRR(x, z - 1) - ARRR(x - 1, z - 1);
            if constexpr (scale == 1)
            {
                ARRC(x, z) = RAWC(x - 1, z - 1) + ARRC(x - 1, z) + ARRC(x, z - 1) - ARRC(x - 1, z - 1);
            }
        }
    }

    auto occAnyInRect = [&](int x0, int x1, int z0, int z1) -> bool {
        if (oW <= 0 || oH <= 0) return true;
        const int tx0 = std::max(0, x0 / occTile);
        const int tx1 = std::min(oW - 1, x1 / occTile);
        const int tz0 = std::max(0, z0 / occTile);
        const int tz1 = std::min(oH - 1, z1 / occTile);
        if (tx0 > tx1 || tz0 > tz1) return false;
        for (int tz = tz0; tz <= tz1; tz++)
        {
            for (int tx = tx0; tx <= tx1; tx++)
            {
                if (occ[(size_t) tz * oW + (size_t) tx])
                    return true;
            }
        }
        return false;
    };

    struct CandidateArea {
        int area;
        int caveArea;
        int riverArea;
        int startX;
        int startZ;

        bool operator<(const CandidateArea &other) const noexcept
        {
            if (area != other.area) return area < other.area;
            if (startX != other.startX) return startX > other.startX;
            return startZ > other.startZ;
        }
    };

    std::priority_queue<CandidateArea> pq;
    const auto &ring = getRingMask<scale>();

    auto ringSum = [&](const std::vector<int> &pref, int cx, int cz) -> int {
        int area = 0;
        for (const auto &m: ring.rows)
        {
            const int row = cz + m.dz;
            const int L = cx - m.out;
            const int R = cx + m.out;
            if (m.in == -1)
            {
                area += pref[(size_t) (R + 1) + (size_t) (row + 1) * stride]
                      - pref[(size_t) L + (size_t) (row + 1) * stride]
                      - pref[(size_t) (R + 1) + (size_t) row * stride]
                      + pref[(size_t) L + (size_t) row * stride];
            } else
            {
                const int Lin = cx - m.in;
                const int Rin = cx + m.in;
                area += pref[(size_t) Lin + (size_t) (row + 1) * stride]
                      - pref[(size_t) L + (size_t) (row + 1) * stride]
                      - pref[(size_t) Lin + (size_t) row * stride]
                      + pref[(size_t) L + (size_t) row * stride];
                area += pref[(size_t) (R + 1) + (size_t) (row + 1) * stride]
                      - pref[(size_t) (Rin + 1) + (size_t) (row + 1) * stride]
                      - pref[(size_t) (R + 1) + (size_t) row * stride]
                      + pref[(size_t) (Rin + 1) + (size_t) row * stride];
            }
        }
        return area;
    };

    CandidateArea maxA{0, 0, 0, 0, 0};

    for (int cz = R_out; cz < H - R_out; cz++)
    {
        for (int cx = R_out; cx < W - R_out; cx++)
        {
            // Skip ring centers whose bounding box has no positive cells
            if (!occAnyInRect(cx - R_out, cx + R_out, cz - R_out, cz + R_out))
                continue;

            int worldTotal;
            int worldCave;
            int worldRiver;

            if constexpr (scale == 1)
            {
                const int sumR = ringSum(prefixRiver, cx, cz);
                const int sumC = ringSum(prefixCave, cx, cz);
                worldRiver = (sumR + 1) / 3;
                worldCave = (sumC + 1) / 3;
                worldTotal = worldCave + (int) (worldRiver * riverWeight);
            } else
            {
                const int sum = ringSum(prefixRiver, cx, cz);
                worldTotal = sum * scale * scale;
                worldCave = 0;
                worldRiver = 0;
            }

            if (worldTotal >= maxA.area * f && worldTotal >= min)
            {
                const int worldX = startX + cx * scale;
                const int worldZ = startZ + cz * scale;
                pq.push({worldTotal, worldCave, worldRiver, worldX, worldZ});
                if (worldTotal > maxA.area)
                {
                    maxA.area = worldTotal;
                    maxA.caveArea = worldCave;
                    maxA.riverArea = worldRiver;
                }
            }
        }
    }

    while (!pq.empty())
    {
        if (const auto &ra = pq.top(); ra.area >= maxA.area * f)
        {
            result.emplace_back(Point{ra.startX, ra.startZ}, ra.area, ra.caveArea, ra.riverArea);
            if (f == 1.0) break;
        }
        pq.pop();
    }

#undef RAWR
#undef RAWC
#undef ARRR
#undef ARRC
    return result;
}

/** phase1Pipeline: 0 = Cont+Weird joint mask (bench only); 1 = Cont mask + WeirdnessOnly@weirdScale (product). */
void findBiggestRiverParallelPool(
    ThreadSafeResults<Res> &globalResults,
    Generator *g,
    int startX, int startZ,
    int sx, int sz,
    int minArea,
    Progress *progress = nullptr,
    int numThreads = static_cast<int>(std::thread::hardware_concurrency()),
    int contPrefilterScaleOverride = -1,
    int phase1Pipeline = 1,
    int weirdnessGridScale = -1
)
{
    ThreadPool pool(numThreads);
    const int chunkSize = 4096 * 2;
    const int tile = SearchConfig::CANDIDATE_TILE_BLOCKS;
    const int overlap = tile;
    const int step = chunkSize - overlap;
    const int contScale = contPrefilterScaleOverride > 0
        ? contPrefilterScaleOverride
        : SearchConfig::PHASE1_CONT_PREFILTER_SCALE;
    const int subR = SearchConfig::SUBSEARCH_RADIUS_BLOCKS;
    const int subSz = SearchConfig::SUBSEARCH_SIZE_BLOCKS;
    const int weirdScale = weirdnessGridScale > 0
        ? weirdnessGridScale
        : SearchConfig::PHASE1_WEIRDNESS_GRID_SCALE;
    (void) phase1Pipeline; /* product always Cont+WeirdnessOnly; retained for bench CLI */
    std::atomic<int> completedChunks{0};
    int totalChunks = 0;

#ifndef DRIPSTONECAVE_FINDER_JNI_LIB
    auto startTime = std::chrono::high_resolution_clock::now();
#endif

    for (int x = 0; x < sx; x += step)
    {
        for (int z = 0; z < sz; z += step)
        {
            int currentSx = std::min(chunkSize, sx - x);
            int currentSz = std::min(chunkSize, sz - z);
            if (currentSx >= tile && currentSz >= tile)
                totalChunks++;
        }
    }

    if (progress)
        progress->total.store(totalChunks);

    const uint64_t seedVal = g->seed;
    const int mcVer = g->mc > 0 ? g->mc : MC_26_1;

    for (int x = 0; x < sx; x += step)
    {
        for (int z = 0; z < sz; z += step)
        {
            if (progress && progress->try_stop.load())
                return;

            int currentSx = std::min(chunkSize, sx - x);
            int currentSz = std::min(chunkSize, sz - z);

            if (currentSx >= tile && currentSz >= tile)
            {
                pool.enqueue([&, x, z, currentSx, currentSz, seedVal, contScale, weirdScale, mcVer]() {
                    if (progress)
                    {
                        if (progress->try_stop.load()) return;
                        while (progress->try_pause.load())
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(200));
                            if (progress->try_stop.load()) return;
                        }
                        progress->chunkInRunning.fetch_add(1);
                    }

                    thread_local Generator tlsG;
                    thread_local bool tlsInited = false;
                    thread_local int tlsMc = -1;
                    thread_local uint64_t tlsSeed = ~0ull;
                    if (!tlsInited || tlsMc != mcVer)
                    {
                        setupGenerator(&tlsG, mcVer, FORCE_OCEAN_VARIANTS);
                        tlsInited = true;
                        tlsMc = mcVer;
                        tlsSeed = ~0ull;
                    }
                    if (tlsSeed != seedVal)
                    {
                        applySeed(&tlsG, DIM_OVERWORLD, seedVal);
                        tlsSeed = seedVal;
                    }

                    const int csx = startX + x;
                    const int csz = startZ + z;

                    auto finishChunk = [&]() {
                        int completed = completedChunks.fetch_add(1) + 1;
                        if (progress)
                        {
                            progress->current.store(completed);
                            progress->chunkInRunning.fetch_sub(1);
                        }
#ifndef DRIPSTONECAVE_FINDER_JNI_LIB
                        if (completed % 500 == 0)
                        {
                            auto currentTime = std::chrono::high_resolution_clock::now();
                            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                currentTime - startTime).count();
                            double speed = static_cast<double>(completed) / elapsed * 1000;
                            std::cout << "Progress: " << completed << "/" << totalChunks
                                    << " (" << int(completed * 100.0 / totalChunks)
                                    << "%) - " << speed << " chunks/sec\n";
                        }
#endif
                    };

                    /* Cont prefilter + WeirdnessOnly ring. Keep only product scales 16/32
                     * so the fast (32) path stays as lean as the pre-UI Cont@128+Weird@32 build. */
                    const auto contMask = buildDilatedContPrefilterMask(
                        &tlsG, csx, csz, currentSx, currentSz,
                        contScale,
                        SearchConfig::PHASE1_CONT_PREFILTER_THRESHOLD,
                        SearchConfig::PHASE1_CONT_PREFILTER_DILATE);
                    if (std::none_of(contMask.begin(), contMask.end(),
                                     [](uint8_t v) { return v != 0; }))
                    {
                        finishChunk();
                        return;
                    }

                    std::vector<Res> blockResultsCoarse;
                    if (weirdScale <= 16)
                    {
                        blockResultsCoarse = findBiggestRiver<16>(
                            &tlsG, csx, csz, currentSx, currentSz,
                            minArea, 0.8, FilterMode::WeirdnessOnly,
                            0.7f, &contMask, contScale);
                    }
                    else
                    {
                        blockResultsCoarse = findBiggestRiver<SearchConfig::PHASE1_WEIRDNESS_GRID_SCALE>(
                            &tlsG, csx, csz, currentSx, currentSz,
                            minArea, 0.8, FilterMode::WeirdnessOnly,
                            0.7f, &contMask, contScale);
                    }

                    const int bx = currentSx / tile + 2;
                    const int bz = currentSz / tile + 2;
                    std::vector<Res> flags((size_t) bx * bz);
                    for (const auto &it: blockResultsCoarse)
                    {
                        int x2 = (it.point.x - csx) / tile;
                        int z2 = (it.point.y - csz) / tile;
                        if (x2 >= 0 && x2 < bx && z2 >= 0 && z2 < bz)
                        {
                            auto &itf = flags[(size_t) x2 + (size_t) bx * z2];
                            if (itf.area < it.area) itf = it;
                        }
                    }

                    std::vector<Res> pqX16;
                    for (auto &kv: flags)
                        if (kv.area > 0) pqX16.push_back(kv);
                    std::sort(pqX16.begin(), pqX16.end(), [](const Res &a, const Res &b) { return a.area > b.area; });

                    std::unordered_map<uint64_t, Res> blockResultsX4;
                    int max = 0;
                    for (auto &res: pqX16)
                    {
                        auto subResults = findBiggestRiver<SearchConfig::PHASE1_CLIMATE_GRID_SCALE>(
                            &tlsG,
                            res.point.x - subR, res.point.y - subR,
                            subSz, subSz,
                            minArea, 1.0, FilterMode::ClimateCoarse);

                        if (subResults.empty()) break;
                        if (subResults[0].area < max * 0.9) break;

                        auto &r = subResults[0];
                        uint64_t key = ((uint64_t) (uint32_t) r.point.x << 32) | (uint32_t) r.point.y;
                        auto it = blockResultsX4.find(key);
                        if (it == blockResultsX4.end())
                            blockResultsX4.emplace(key, r);
                        else if (it->second.area < r.area)
                            it->second = r;

                        if (subResults[0].area > max) max = subResults[0].area;
                    }

                    std::vector<Res> filteredResults;
                    for (const auto &kv: blockResultsX4)
                    {
                        const auto &result = kv.second;
                        int relX = result.point.x - csx;
                        int relZ = result.point.y - csz;
                        if (relX > overlap / 2 && relX < currentSx - overlap / 2 &&
                            relZ > overlap / 2 && relZ < currentSz - overlap / 2 &&
                            result.area > 0)
                        {
                            filteredResults.push_back(result);
                        }
                    }
                    std::sort(filteredResults.begin(), filteredResults.end(), [](const Res &a, const Res &b) { return a.area > b.area; });
                    if (!filteredResults.empty())
                        globalResults.addResults(filteredResults);

                    finishChunk();
                });
            }
        }
    }

#ifndef DRIPSTONECAVE_FINDER_JNI_LIB
    std::cout << "Submitted " << totalChunks << " chunks [Cont@" << contScale
              << "+Weird@" << weirdScale << "]\n";
#endif
}

#if defined(CONT_PREFILTER_BENCH) && !defined(DRIPSTONECAVE_FINDER_JNI_LIB)

#include "cubiomes/noise.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

static constexpr double kBenchDblF = 337.0 / 331.0;
static constexpr double kRingFullArea = 3.14159265358979323846 * (128.0 * 128.0 - 24.0 * 24.0);

static double benchOctA(const DoublePerlinNoise *dpn, int idx, double x, double z, int *perlins)
{
    if (idx < 0 || idx >= dpn->octA.octcnt)
        return 0.0;
    const PerlinNoise *p = dpn->octA.octaves + idx;
    const double lf = p->lacunarity;
    (*perlins)++;
    return p->amplitude * samplePerlin(p, maintainPrecision(x * lf), 0.0, maintainPrecision(z * lf), 0, 0);
}

static double benchOctB(const DoublePerlinNoise *dpn, int idx, double x, double z, int *perlins)
{
    if (idx < 0 || idx >= dpn->octB.octcnt)
        return 0.0;
    const PerlinNoise *p = dpn->octB.octaves + idx;
    const double lf = p->lacunarity;
    (*perlins)++;
    return p->amplitude * samplePerlin(p,
        maintainPrecision(x * lf * kBenchDblF), 0.0, maintainPrecision(z * lf * kBenchDblF), 0, 0);
}

/** Instrumented Cont matching legacy mid-thresholds; counts Perlin calls (max 10). */
struct ContFunnelStats {
    long long reached[9]{};   /* gate 1..8 */
    long long rejected[9]{};
    long long accepted = 0;
    long long samples = 0;
    long long perlin_sum = 0;
};

static bool contFunnelSample(const BiomeNoise *bn, int bx, int bz, double thr, ContFunnelStats &st)
{
    const DoublePerlinNoise *dpn = &bn->climate[NP_CONTINENTALNESS];
    const double amp = dpn->amplitude;
    double sum = 0.0;
    int perlins = 0;
    st.samples++;

    auto gate = [&](int g, double midThr, bool isFinal) -> int {
        /* return: -1 reject, 0 continue, 1 accept */
        st.reached[g]++;
        if (isFinal)
        {
            if (sum > thr)
            {
                st.accepted++;
                return 1;
            }
            st.rejected[g]++;
            return -1;
        }
        if (sum < midThr)
        {
            st.rejected[g]++;
            return -1;
        }
        return 0;
    };

    sum += amp * (benchOctA(dpn, 0, bx, bz, &perlins) + benchOctB(dpn, 0, bx, bz, &perlins));
    if (gate(1, -0.2, false) < 0) { st.perlin_sum += perlins; return false; }

    sum += amp * benchOctA(dpn, 1, bx, bz, &perlins);
    if (gate(2, -0.1, false) < 0) { st.perlin_sum += perlins; return false; }

    sum += amp * benchOctB(dpn, 1, bx, bz, &perlins);
    if (gate(3, 0.0, false) < 0) { st.perlin_sum += perlins; return false; }

    sum += amp * benchOctA(dpn, 2, bx, bz, &perlins);
    if (gate(4, 0.13, false) < 0) { st.perlin_sum += perlins; return false; }

    sum += amp * benchOctB(dpn, 2, bx, bz, &perlins);
    if (gate(5, 0.3, false) < 0) { st.perlin_sum += perlins; return false; }

    sum += amp * benchOctA(dpn, 3, bx, bz, &perlins);
    if (gate(6, 0.37, false) < 0) { st.perlin_sum += perlins; return false; }

    sum += amp * benchOctB(dpn, 3, bx, bz, &perlins);
    if (gate(7, 0.44, false) < 0) { st.perlin_sum += perlins; return false; }

    sum += amp * (benchOctA(dpn, 4, bx, bz, &perlins) + benchOctB(dpn, 4, bx, bz, &perlins));
    const int r = gate(8, 0.0, true);
    st.perlin_sum += perlins;
    return r > 0;
}

static void runContFunnel(Generator *g, int startX, int startZ, int sx, int sz,
                          int sampleScale, double thr, ContFunnelStats &st)
{
    for (int z = 0; z < sz; z += sampleScale)
    {
        const int worldZ = startZ + z + sampleScale / 2;
        for (int x = 0; x < sx; x += sampleScale)
        {
            const int worldX = startX + x + sampleScale / 2;
            contFunnelSample(&g->bn, worldX / 4, worldZ / 4, thr, st);
        }
    }
}

static void printContFunnel(const ContFunnelStats &st, int sampleScale, double thr,
                            double maskMs, double fillMs)
{
    std::printf("=== Cont octave funnel (scale=%d, thr=%.3f, N=%lld) ===\n",
                sampleScale, thr, (long long) st.samples);
    std::printf("gate  reached     reject     pass%%    cum_reject%%\n");
    long long cumRej = 0;
    for (int g = 1; g <= 8; g++)
    {
        const long long rch = st.reached[g];
        const long long rej = st.rejected[g];
        cumRej += rej;
        double passPct = 0.0;
        if (rch > 0)
        {
            if (g < 8)
                passPct = 100.0 * (double) (rch - rej) / (double) rch;
            else
                passPct = 100.0 * (double) st.accepted / (double) rch;
        }
        const double cumRejPct = st.samples > 0 ? 100.0 * (double) cumRej / (double) st.samples : 0.0;
        std::printf("%-4d  %-10lld  %-10lld  %6.2f    %6.2f\n",
                    g, (long long) rch, (long long) rej, passPct, cumRejPct);
    }
    const double avgOct = st.samples > 0 ? (double) st.perlin_sum / (double) st.samples : 0.0;
    const double contOnly = avgOct > 0.0 ? 10.0 / avgOct : 0.0;
    std::printf("avg_perlins = %.3f / 10  => Cont-only ~%.2fx vs full-10\n", avgOct, contOnly);
    const double total = maskMs + fillMs;
    const double estFull = total - maskMs + maskMs * (avgOct > 0 ? 10.0 / avgOct : 1.0);
    /* If early-exit Cont is what we use now, estimate time if Cont were full-10:
       T_full ≈ T_mask * (10/avg) + T_fill ; overall speedup of early-exit ≈ T_full / T_now */
    const double tIfFull10 = (avgOct > 0.0)
        ? (maskMs * (10.0 / avgOct) + fillMs)
        : total;
    const double overall = tIfFull10 > 0.0 ? tIfFull10 / total : 1.0;
    std::printf("phase1 wall: mask=%.1fms fill+ring=%.1fms total=%.1fms\n", maskMs, fillMs, total);
    std::printf("est. phase1 if Cont full-10: %.1fms  => overall ~%.2fx from Cont early-exit\n",
                tIfFull10, overall);
    std::printf("accept_rate=%.3f%% (%lld/%lld)\n\n",
                st.samples ? 100.0 * (double) st.accepted / (double) st.samples : 0.0,
                (long long) st.accepted, (long long) st.samples);
    (void) estFull;
}

static uint64_t tileKey(int x, int z, int tile)
{
    const int tx = x >= 0 ? x / tile : (x - tile + 1) / tile;
    const int tz = z >= 0 ? z / tile : (z - tile + 1) / tile;
    return ((uint64_t) (uint32_t) tx << 32) | (uint32_t) tz;
}

static std::set<uint64_t> peakTiles(const std::vector<Res> &peaks, int tile)
{
    std::set<uint64_t> s;
    for (const auto &p: peaks)
        s.insert(tileKey(p.point.x, p.point.y, tile));
    return s;
}

static std::vector<Res> runPhase1(Generator *g, int startX, int startZ, int sx, int sz,
                                  int minArea, int threads, int contScale,
                                  double *outMaskMs, double *outFillMs,
                                  int phase1Pipeline = 1,
                                  int weirdnessGridScale = -1)
{
    if (outMaskMs)
        *outMaskMs = 0.0;

    ThreadSafeResults<Res> results;
    auto t2 = std::chrono::high_resolution_clock::now();
    findBiggestRiverParallelPool(results, g, startX, startZ, sx, sz, minArea,
                                 nullptr, threads, contScale, phase1Pipeline, weirdnessGridScale);
    auto t3 = std::chrono::high_resolution_clock::now();
    if (outFillMs)
        *outFillMs = std::chrono::duration<double, std::milli>(t3 - t2).count();

    auto all = results.getAllResults();
    std::sort(all.begin(), all.end(), [](const Res &a, const Res &b) { return a.area > b.area; });
    return all;
}

int main(int argc, char **argv)
{
    int64_t seed = -8180004378910677489LL;
    int cx = 0, cz = 0, half = 8192;
    int threads = (int) std::thread::hardware_concurrency();
    if (threads < 1) threads = 1;
    bool cmp64Only = false; /* skip funnel + scale16; 64 = reference, report 128 miss */
    bool cmpWeird = false;  /* Weird@16 legacy vs Cont+Weird@128 joint */

    for (int i = 1; i < argc; i++)
    {
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
            seed = std::strtoll(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--cx") && i + 1 < argc)
            cx = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--cz") && i + 1 < argc)
            cz = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--half") && i + 1 < argc)
            half = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc)
            threads = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--cmp64"))
            cmp64Only = true;
        else if (!std::strcmp(argv[i], "--cmp-weird"))
            cmpWeird = true;
    }

    const int startX = cx - half;
    const int startZ = cz - half;
    const int sx = half * 2;
    const int sz = half * 2;
    const int minArea = (int) (kRingFullArea * 0.70);
    const int tile = SearchConfig::CANDIDATE_TILE_BLOCKS;
    const double thr = SearchConfig::PHASE1_CONT_PREFILTER_THRESHOLD;

    std::printf("Cont prefilter bench%s%s\n",
                cmp64Only ? " (--cmp64)" : "",
                cmpWeird ? " (--cmp-weird: Weird@16 vs joint@128)" : "");
    std::printf("seed=%lld window=[%d,%d)x[%d,%d) half=%d side=%d threads=%d minArea=%d (70%% of %.1f)\n\n",
                (long long) seed, startX, startX + sx, startZ, startZ + sz,
                half, half * 2, threads, minArea, kRingFullArea);

    Generator g;
    setupGenerator(&g, MC_1_21_3, FORCE_OCEAN_VARIANTS);
    applySeed(&g, DIM_OVERWORLD, (uint64_t) seed);

    if (cmpWeird)
    {
        /* Cont@128 + WeirdnessOnly@{16,32,64,128} + Climate@4; oracle = Weird@16 final ≥70% results. */
        const int weirdScales[] = {16, 32, 64, 128};
        struct Run {
            int weird;
            std::vector<Res> peaks;
            double ms;
        } runs[4]{};

        for (int i = 0; i < 4; i++)
        {
            runs[i].weird = weirdScales[i];
            std::printf("Running Cont@128 + WeirdnessOnly@%d + Climate@4 (final ≥70%%)...\n",
                        runs[i].weird);
            runs[i].peaks = runPhase1(&g, startX, startZ, sx, sz, minArea, threads, 128,
                                      nullptr, &runs[i].ms, /*pipeline=*/1, runs[i].weird);
            std::printf("  final_results=%zu time=%.1fms (%.2fs)\n",
                        runs[i].peaks.size(), runs[i].ms, runs[i].ms / 1000.0);
        }

        const auto ref = peakTiles(runs[0].peaks, tile);
        std::printf("\n=== Final ≥70%% results: Weird@16 oracle vs @32/@64/@128 (half=%d, tile=%d) ===\n",
                    half, tile);
        std::printf("oracle Weird@16: %zu final peak-tiles  time=%.1fms\n",
                    ref.size(), runs[0].ms);

        for (int i = 1; i < 4; i++)
        {
            const auto got = peakTiles(runs[i].peaks, tile);
            int miss = 0;
            for (uint64_t k: ref)
                if (!got.count(k))
                    miss++;
            int extra = 0;
            for (uint64_t k: got)
                if (!ref.count(k))
                    extra++;
            const double missPct = ref.empty() ? 0.0 : 100.0 * miss / (double) ref.size();
            const double speedup = runs[i].ms > 0.0 ? runs[0].ms / runs[i].ms : 0.0;
            std::printf("Weird@%d: final=%zu  miss=%d/%zu (%.2f%%) ~%.1f/100  extra=%d  "
                        "time=%.1fms  speedup=%.2fx\n",
                        runs[i].weird, got.size(), miss, ref.size(), missPct, missPct,
                        extra, runs[i].ms, speedup);
        }
        return 0;
    }

    if (!cmp64Only)
    {
        ContFunnelStats st{};
        double maskMs = 0, fillMs = 0;
        auto t0 = std::chrono::high_resolution_clock::now();
        runContFunnel(&g, startX, startZ, sx, sz, 64, thr, st);
        auto t1 = std::chrono::high_resolution_clock::now();
        maskMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        ThreadSafeResults<Res> tmp;
        auto t2 = std::chrono::high_resolution_clock::now();
        findBiggestRiverParallelPool(tmp, &g, startX, startZ, sx, sz, minArea, nullptr, threads, 64);
        auto t3 = std::chrono::high_resolution_clock::now();
        fillMs = std::chrono::duration<double, std::milli>(t3 - t2).count();
        printContFunnel(st, 64, thr, maskMs, fillMs);
    }

    struct ScaleRun {
        int scale;
        std::vector<Res> peaks;
        double maskMs;
        double fillMs;
    };

    if (cmp64Only)
    {
        ScaleRun run64{}, run128{};
        run64.scale = 64;
        run128.scale = 128;

        std::printf("Running Phase1 ClimateCoarse@16 with Cont prefilter scale=64 (reference)...\n");
        run64.peaks = runPhase1(&g, startX, startZ, sx, sz, minArea, threads, 64,
                                &run64.maskMs, &run64.fillMs);
        std::printf("  peaks=%zu mask=%.1fms fill+ring=%.1fms total=%.1fms\n",
                    run64.peaks.size(), run64.maskMs, run64.fillMs,
                    run64.maskMs + run64.fillMs);

        std::printf("Running Phase1 ClimateCoarse@16 with Cont prefilter scale=128...\n");
        run128.peaks = runPhase1(&g, startX, startZ, sx, sz, minArea, threads, 128,
                                 &run128.maskMs, &run128.fillMs);
        std::printf("  peaks=%zu mask=%.1fms fill+ring=%.1fms total=%.1fms\n",
                    run128.peaks.size(), run128.maskMs, run128.fillMs,
                    run128.maskMs + run128.fillMs);

        const auto ref = peakTiles(run64.peaks, tile);
        const auto got = peakTiles(run128.peaks, tile);
        int miss = 0;
        std::vector<uint64_t> missed;
        for (uint64_t k: ref)
        {
            if (!got.count(k))
            {
                miss++;
                missed.push_back(k);
            }
        }
        int extra = 0;
        for (uint64_t k: got)
            if (!ref.count(k))
                extra++;

        const double missPct = ref.empty() ? 0.0 : 100.0 * miss / (double) ref.size();
        std::printf("\n=== Cont prefilter: 128 miss vs 64 reference (minArea=70%%, tile=%d) ===\n", tile);
        std::printf("ref(scale=64): %zu peak-tiles\n", ref.size());
        std::printf("scale=128: %zu peak-tiles\n", got.size());
        std::printf("128 misses vs 64: %d/%zu (%.2f%%)  => ~%.1f per 100 candidates\n",
                    miss, ref.size(), missPct, missPct);
        std::printf("128 extras not in 64: %d\n", extra);
        std::printf("time 64=%.1fms  128=%.1fms\n",
                    run64.maskMs + run64.fillMs, run128.maskMs + run128.fillMs);
        const int show = (int) std::min<size_t>(missed.size(), 16);
        for (int j = 0; j < show; j++)
        {
            const int tx = (int) (missed[j] >> 32);
            const int tz = (int) (uint32_t) missed[j];
            std::printf("  missed tile (%d,%d) ~ block (%d,%d)\n",
                        tx, tz, tx * tile, tz * tile);
        }
        if ((int) missed.size() > show)
            std::printf("  ... +%d more\n", (int) missed.size() - show);
        return 0;
    }

    const int scales[] = {16, 64, 128};
    ScaleRun runs[3]{};
    for (int i = 0; i < 3; i++)
    {
        runs[i].scale = scales[i];
        std::printf("Running Phase1 ClimateCoarse@16 with Cont prefilter scale=%d...\n", scales[i]);
        runs[i].peaks = runPhase1(&g, startX, startZ, sx, sz, minArea, threads, scales[i],
                                  &runs[i].maskMs, &runs[i].fillMs);
        std::printf("  peaks=%zu mask=%.1fms fill+ring=%.1fms total=%.1fms\n",
                    runs[i].peaks.size(), runs[i].maskMs, runs[i].fillMs,
                    runs[i].maskMs + runs[i].fillMs);
    }

    const auto oracle = peakTiles(runs[0].peaks, tile);
    std::printf("\n=== Cont prefilter miss (minArea=70%%, tile=%d) ===\n", tile);
    std::printf("oracle(scale=16): %zu peak-tiles\n", oracle.size());

    for (int i = 1; i < 3; i++)
    {
        const auto got = peakTiles(runs[i].peaks, tile);
        int miss = 0;
        std::vector<uint64_t> missed;
        for (uint64_t k: oracle)
        {
            if (!got.count(k))
            {
                miss++;
                missed.push_back(k);
            }
        }
        const double missPct = oracle.empty() ? 0.0 : 100.0 * miss / (double) oracle.size();
        std::printf("scale=%d: miss %d/%zu (%.2f%%)  time=%.1fms\n",
                    runs[i].scale, miss, oracle.size(), missPct,
                    runs[i].maskMs + runs[i].fillMs);
        const int show = (int) std::min<size_t>(missed.size(), 8);
        for (int j = 0; j < show; j++)
        {
            const int tx = (int) (missed[j] >> 32);
            const int tz = (int) (uint32_t) missed[j];
            std::printf("  missed tile (%d,%d) ~ block (%d,%d)\n",
                        tx, tz, tx * tile, tz * tile);
        }
        if ((int) missed.size() > show)
            std::printf("  ... +%d more\n", (int) missed.size() - show);
    }

    return 0;
}

#elif !defined(DRIPSTONECAVE_FINDER_JNI_LIB)
int main(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    int64_t seed = -8180004378910677489;
    int px = 0, pz = 0, d = 4096;
    std::cout << "seed: ";
    std::cin >> seed;
    std::cout << "center_x: ";
    std::cin >> px;
    std::cout << "center_z: ";
    std::cin >> pz;
    std::cout << "r: ";
    std::cin >> d;

    int startX = px - d;
    int startZ = pz - d;
    int xRange = 2 * d;
    int zRange = 2 * d;
    int minArea = 40000;
    const char *outFile = "out1.txt";
    int outLimit = 1000;

    Generator g;
    setupGenerator(&g, MC_1_21_3, FORCE_OCEAN_VARIANTS);
    applySeed(&g, DIM_OVERWORLD, seed);

    ThreadSafeResults<Res> globalResults;
    findBiggestRiverParallelPool(globalResults, &g, startX, startZ, xRange, zRange, minArea, nullptr);

    auto res = globalResults.getAllResults();
    std::sort(res.begin(), res.end(), [](const Res &a, const Res &b) { return a.area > b.area; });

    int max = 0;
    std::vector<Res> finallyResults;
    for (const auto &it: res)
    {
        auto temp = findBiggestRiver<1>(
            &g,
            it.point.x - SearchConfig::REFINE_HALF_WINDOW,
            it.point.y - SearchConfig::REFINE_HALF_WINDOW,
            SearchConfig::REFINE_WINDOW_BLOCKS, SearchConfig::REFINE_WINDOW_BLOCKS,
            1, 1.0, FilterMode::PreciseBiome);
        if (!temp.empty() && temp[0].area > max * 0.90)
        {
            finallyResults.push_back(temp[0]);
            if (temp[0].area > max) max = temp[0].area;
        } else
        {
            break;
        }
    }

    FILE *fp = fopen(outFile, "w");
    if (fp)
    {
        fprintf(fp, "Cave Analysis Results (Seed: %lld)\n", (long long) seed);
        fprintf(fp, "Search Area: X=%d to %d, Z=%d to %d\n",
                startX, startX + xRange, startZ, startZ + zRange);
        fprintf(fp, "========================================\n");
    }

    int count = 0;
    for (const auto &it: finallyResults)
    {
        std::cout << "x:" << it.point.x << " y:" << it.point.y
                << "  Area:" << it.area << " cave:" << it.caveArea
                << " river:" << it.riverArea << std::endl;
        if (fp)
        {
            fprintf(fp, "x:%d y:%d  Area:%d cave:%d river:%d\n",
                    it.point.x, it.point.y, it.area, it.caveArea, it.riverArea);
        }
        count++;
        if (count > outLimit) break;
    }
    if (fp)
    {
        fprintf(fp, "\nTotal results: %d\n", count);
        fclose(fp);
        std::cout << "\nResults saved to: " << outFile << std::endl;
    }
    return 0;
}
#endif
