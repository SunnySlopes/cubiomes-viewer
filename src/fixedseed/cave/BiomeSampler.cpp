#include "BiomeSampler.h"
#include "cubiomes/biomes.h"
#include "cubiomes/noise.h"
#include <cstdlib>
#include <cstdint>
#include <unordered_map>

const int PRECISE_YS[PRECISE_Y_COUNT] = { -60, -56, -52 };

static constexpr double kDoublePerlinF = 337.0 / 331.0;

static double sampleContOctA(const DoublePerlinNoise *dpn, int idx, double x, double y, double z)
{
    if (idx < 0 || idx >= dpn->octA.octcnt)
        return 0.0;
    const PerlinNoise *p = dpn->octA.octaves + idx;
    const double lf = p->lacunarity;
    const double ax = maintainPrecision(x * lf);
    const double ay = maintainPrecision(y * lf);
    const double az = maintainPrecision(z * lf);
    return p->amplitude * samplePerlin(p, ax, ay, az, 0, 0);
}

static double sampleContOctB(const DoublePerlinNoise *dpn, int idx, double x, double y, double z)
{
    if (idx < 0 || idx >= dpn->octB.octcnt)
        return 0.0;
    const PerlinNoise *p = dpn->octB.octaves + idx;
    const double lf = p->lacunarity;
    const double ax = maintainPrecision(x * lf * kDoublePerlinF);
    const double ay = maintainPrecision(y * lf * kDoublePerlinF);
    const double az = maintainPrecision(z * lf * kDoublePerlinF);
    return p->amplitude * samplePerlin(p, ax, ay, az, 0, 0);
}

bool passContinentalnessPartialThr(const BiomeNoise *bn, int bx, int bz, double threshold)
{
    const DoublePerlinNoise *dpn = &bn->climate[NP_CONTINENTALNESS];
    // sampleDoublePerlin scales octA+octB by this; keep running sum in climate units
    const double amp = dpn->amplitude;
    const double x = bx;
    const double y = 0.0;
    const double z = bz;
    double sum = 0.0;

    sum += amp * (sampleContOctA(dpn, 0, x, y, z) + sampleContOctB(dpn, 0, x, y, z));
    if (sum < -0.2)
        return false;

    sum += amp * sampleContOctA(dpn, 1, x, y, z);
    if (sum < -0.1)
        return false;

    sum += amp * sampleContOctB(dpn, 1, x, y, z);
    if (sum < 0.0)
        return false;

    sum += amp * sampleContOctA(dpn, 2, x, y, z);
    if (sum < 0.13)
        return false;

    sum += amp * sampleContOctB(dpn, 2, x, y, z);
    if (sum < 0.3)
        return false;

    sum += amp * sampleContOctA(dpn, 3, x, y, z);
    if (sum < 0.37)
        return false;

    sum += amp * sampleContOctB(dpn, 3, x, y, z);
    if (sum < 0.44)
        return false;

    sum += amp * (sampleContOctA(dpn, 4, x, y, z) + sampleContOctB(dpn, 4, x, y, z));
    return sum > threshold;
}

bool passContinentalnessPartial(const BiomeNoise *bn, int bx, int bz)
{
    return passContinentalnessPartialThr(bn, bx, bz, CONT_PARTIAL_FINAL_THRESHOLD);
}

static int quantWeirdness(const BiomeNoise *bn, int bx, int bz, uint32_t sample_flags)
{
    double px = bx, pz = bz;
    if (!(sample_flags & SAMPLE_NO_SHIFT))
    {
        px += sampleDoublePerlin(&bn->climate[NP_SHIFT], bx, 0, bz) * 4.0;
        pz += sampleDoublePerlin(&bn->climate[NP_SHIFT], bz, bx, 0) * 4.0;
    }
    float w = sampleDoublePerlin(&bn->climate[NP_WEIRDNESS], px, 0, pz);
    int wi = (int) (10000.0 * w);
    return wi < 0 ? -wi : wi;
}

bool passCaveWeirdness(const BiomeNoise *bn, int bx, int bz, uint32_t sample_flags)
{
    int wi = quantWeirdness(bn, bx, bz, sample_flags);
    return wi < 500 || wi > 12800;
}

bool passCoarseCaveCell(const BiomeNoise *bn, int bx, int bz, uint32_t sample_flags)
{
    if (!passContinentalnessPartial(bn, bx, bz))
        return false;
    if (!passCaveWeirdness(bn, bx, bz, sample_flags))
        return false;

    double px = bx, pz = bz;
    if (!(sample_flags & SAMPLE_NO_SHIFT))
    {
        px += sampleDoublePerlin(&bn->climate[NP_SHIFT], bx, 0, bz) * 4.0;
        pz += sampleDoublePerlin(&bn->climate[NP_SHIFT], bz, bx, 0) * 4.0;
    }

    int ei = (int) (10000.0 * sampleDoublePerlin(&bn->climate[NP_EROSION], px, 0, pz));
    return ei > -3750;
}

bool passCaveClimate(const BiomeNoise *bn, int bx, int bz, uint32_t sample_flags)
{
    return passCoarseCaveCell(bn, bx, bz, sample_flags);
}

void samplePreciseCell(const Generator *g, int worldX, int worldZ, int *riverHits, int *caveHits)
{
    *riverHits = 0;
    *caveHits = 0;

    const int bx = worldX / 4;
    const int bz = worldZ / 4;
    if (!passCaveClimate(&g->bn, bx, bz, COARSE_SAMPLE_FLAGS))
        return;

    // Reuse a thread-local 1×1×sy cache instead of alloc/free per Y sample.
    const int y0 = PRECISE_YS[0];
    const int sy = PRECISE_YS[PRECISE_Y_COUNT - 1] - y0 + 1;
    Range r = {1, worldX, worldZ, 1, 1, y0, sy};

    thread_local int *tlsCache = nullptr;
    thread_local size_t tlsCap = 0;
    const size_t need = getMinCacheSize(g, r.scale, r.sx, r.sy, r.sz);
    if (need == 0)
        return;
    if (tlsCap < need)
    {
        free(tlsCache);
        tlsCache = (int *) calloc(need, sizeof(int));
        tlsCap = tlsCache ? need : 0;
    }
    if (!tlsCache)
        return;

    if (genBiomes(g, tlsCache, r) != 0)
        return;

    for (int i = 0; i < PRECISE_Y_COUNT; i++)
    {
        const int yi = PRECISE_YS[i] - y0;
        const int id = tlsCache[yi];
        *riverHits += (id == river);
        *caveHits += (id == dripstone_caves);
    }
}

void fillPreciseWindow(Generator *g, int startX, int startZ, int W, int H,
                       std::vector<int> &rawRiver, std::vector<int> &rawCave)
{
    const size_t n = (size_t) W * (size_t) H;
    rawRiver.assign(n, 0);
    rawCave.assign(n, 0);
    if (W <= 0 || H <= 0)
        return;

    std::vector<uint8_t> climatePass(n, 0);
    std::unordered_map<uint64_t, uint8_t> climCache;
    climCache.reserve((size_t) (W / 4 + 2) * (size_t) (H / 4 + 2));

    bool anyPass = false;
    for (int z = 0; z < H; z++)
    {
        const int worldZ = startZ + z;
        for (int x = 0; x < W; x++)
        {
            const int worldX = startX + x;
            const int bx = worldX / 4;
            const int bz = worldZ / 4;
            const uint64_t key = ((uint64_t) (uint32_t) bx << 32) | (uint32_t) bz;
            auto it = climCache.find(key);
            if (it == climCache.end())
            {
                const uint8_t ok = passCaveClimate(&g->bn, bx, bz, COARSE_SAMPLE_FLAGS) ? 1 : 0;
                it = climCache.emplace(key, ok).first;
            }
            climatePass[(size_t) x + (size_t) z * W] = it->second;
            anyPass = anyPass || it->second;
        }
    }
    if (!anyPass)
        return;

    const int y0 = PRECISE_YS[0];
    const int sy = PRECISE_YS[PRECISE_Y_COUNT - 1] - y0 + 1;
    Range r = {1, startX, startZ, W, H, y0, sy};
    int *cache = allocCache(g, r);
    if (!cache)
        return;
    if (genBiomes(g, cache, r) != 0)
    {
        free(cache);
        return;
    }

    const size_t plane = n;
    for (int z = 0; z < H; z++)
    {
        for (int x = 0; x < W; x++)
        {
            const size_t idx = (size_t) x + (size_t) z * W;
            if (!climatePass[idx])
                continue;
            int riverHits = 0;
            int caveHits = 0;
            for (int i = 0; i < PRECISE_Y_COUNT; i++)
            {
                const int yi = PRECISE_YS[i] - y0;
                const int id = cache[(size_t) yi * plane + idx];
                riverHits += (id == river);
                caveHits += (id == dripstone_caves);
            }
            rawRiver[idx] = riverHits;
            rawCave[idx] = caveHits;
        }
    }
    free(cache);
}
