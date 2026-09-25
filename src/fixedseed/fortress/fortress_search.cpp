#ifdef __cplusplus
extern "C" {
#endif
#include "cubiomes/finders.h"
#include "cubiomes/generator.h"
#include "cubiomes/biomes.h"
#include "cubiomes/util.h"
#include "cubiomes/features/fortress.h"
#ifdef __cplusplus
}
#endif

#include "fortress_search.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// Global control so parallel multi-seed searches share one pause/stop switch.
static std::atomic_bool g_search_pause{false};
static std::atomic_bool g_search_stop{false};

void fortressControlPause() { g_search_pause.store(true); }
void fortressControlResume() { g_search_pause.store(false); }
void fortressControlStop() { g_search_stop.store(true); }
void fortressControlReset() {
    g_search_pause.store(false);
    g_search_stop.store(false);
}

namespace {
    constexpr int EXTEND = 128;
    constexpr int SPAN_TP_Y = 48;
    constexpr int REG_BLOCK = 27 * 16;
    constexpr int PIECES_MAX = 400;
    constexpr int BIOME_RADIUS = 64;
    constexpr int BIOME_SCALE = 16;

    int floorDiv(int a) {
        if (a >= 0)
            return a / REG_BLOCK;
        return (a - REG_BLOCK + 1) / REG_BLOCK;
    }

    int defaultHwThreads() {
        unsigned hw = std::thread::hardware_concurrency();
        return hw > 0 ? (int) hw : 1;
    }

    enum class Mode { Cross, Span };

    enum class CrossFilter {
        All = 2,
        TripleQuad = 3,
        QuadOnly = 4,
    };

    struct SearchArea {
        int centerX = 0;
        int centerZ = 0;
        int r = 0;
        int initX0 = 0, initX1 = 0, initZ0 = 0, initZ1 = 0;
        int extX0 = 0, extX1 = 0, extZ0 = 0, extZ1 = 0;
        int regX0 = 0, regX1 = 0, regZ0 = 0, regZ1 = 0;

        void init(int cx, int cz, int radius) {
            centerX = cx;
            centerZ = cz;
            r = radius;
            initX0 = centerX - r;
            initX1 = centerX + r;
            initZ0 = centerZ - r;
            initZ1 = centerZ + r;
            extX0 = initX0 - EXTEND;
            extX1 = initX1 + EXTEND;
            extZ0 = initZ0 - EXTEND;
            extZ1 = initZ1 + EXTEND;
            regX0 = floorDiv(extX0);
            regX1 = floorDiv(extX1);
            regZ0 = floorDiv(extZ0);
            regZ1 = floorDiv(extZ1);
        }

        [[nodiscard]] bool inInit(int x, int z) const {
            return x >= initX0 && x <= initX1 && z >= initZ0 && z <= initZ1;
        }

        [[nodiscard]] bool inExt(int x, int z) const {
            return x >= extX0 && x <= extX1 && z >= extZ0 && z <= extZ1;
        }
    };

    struct Crossing {
        int cx = 0, cy = 0, cz = 0;
        int minY = 0;
    };

    struct StoredPiece {
        int type = 0;
        int x0 = 0, y0 = 0, z0 = 0;
        int x1 = 0, y1 = 0, z1 = 0;
    };

    struct FortressData {
        int startX = 0, startZ = 0;
        int minX = 0, minZ = 0, maxX = 0, maxZ = 0, minY = 0, maxY = 0;
        int centerX = 0, centerZ = 0;
        std::vector<Crossing> crossings;
        std::vector<StoredPiece> pieces;
    };

    struct CrossHit {
        int outX = 0, outY = 0, outZ = 0;
        const char *shape = nullptr;
    };

    struct InternalConfig {
        Mode mode = Mode::Cross;
        int mc = MC_1_20;
        uint64_t seed = 0;
        SearchArea area;
        CrossFilter crossFilter = CrossFilter::All;
        int minLong = 0;
        int minShort = 0;
        int minHeight = 0;
    };

    InternalConfig toInternal(const FortressSearchConfig &cfg) {
        InternalConfig ic;
        ic.mode = cfg.mode == 1 ? Mode::Span : Mode::Cross;
        ic.mc = cfg.mc;
        ic.seed = cfg.seed;
        ic.area.init(cfg.centerX, cfg.centerZ, cfg.r);
        if (cfg.crossFilter == 3)
            ic.crossFilter = CrossFilter::TripleQuad;
        else if (cfg.crossFilter == 4)
            ic.crossFilter = CrossFilter::QuadOnly;
        else
            ic.crossFilter = CrossFilter::All;
        ic.minLong = cfg.minLong;
        ic.minShort = cfg.minShort > 0 ? cfg.minShort : cfg.minLong;
        ic.minHeight = cfg.minHeight;
        return ic;
    }

    int shapeNameToCode(const char *name) {
        if (!name)
            return 0;
        if (std::string(name) == "double")
            return 1;
        if (std::string(name) == "triple")
            return 2;
        if (std::string(name) == "quad")
            return 3;
        return 0;
    }

    bool checkProgress(FortressProgress *progress);

    bool parseFortress(int mc, uint64_t seed, int chunkX, int chunkZ, FortressData &out, FortressProgress *progress) {
        Piece pieces[PIECES_MAX];
        int n = getFortressPieces(pieces, PIECES_MAX, mc, seed, chunkX, chunkZ);
        if (n <= 0)
            return false;

        out = FortressData{};
        bool first = true;
        for (int i = 0; i < n; i++) {
            if (progress && (i & 15) == 0 && !checkProgress(progress))
                return false;
            const Piece &p = pieces[i];
            if (first) {
                out.minX = p.bb0.x;
                out.minZ = p.bb0.z;
                out.maxX = p.bb1.x;
                out.maxZ = p.bb1.z;
                out.minY = p.bb0.y;
                out.maxY = p.bb1.y;
                first = false;
            } else {
                out.minX = std::min(out.minX, p.bb0.x);
                out.minZ = std::min(out.minZ, p.bb0.z);
                out.maxX = std::max(out.maxX, p.bb1.x);
                out.maxZ = std::max(out.maxZ, p.bb1.z);
                out.minY = std::min(out.minY, p.bb0.y);
                out.maxY = std::max(out.maxY, p.bb1.y);
            }
            StoredPiece sp;
            sp.type = p.type;
            sp.x0 = p.bb0.x;
            sp.y0 = p.bb0.y;
            sp.z0 = p.bb0.z;
            sp.x1 = p.bb1.x;
            sp.y1 = p.bb1.y;
            sp.z1 = p.bb1.z;
            out.pieces.push_back(sp);

            if (p.type == BRIDGE_CROSSING) {
                Crossing c;
                c.cx = (p.bb0.x + p.bb1.x) / 2;
                c.cy = (p.bb0.y + p.bb1.y) / 2;
                c.cz = (p.bb0.z + p.bb1.z) / 2;
                c.minY = p.bb0.y;
                out.crossings.push_back(c);
            }
        }
        out.centerX = (out.minX + out.maxX) / 2;
        out.centerZ = (out.minZ + out.maxZ) / 2;
        return true;
    }

    bool onlySoulSandValley(Generator *g, int cx, int cy, int cz, FortressProgress *progress) {
        for (int dz = -BIOME_RADIUS; dz <= BIOME_RADIUS; dz += BIOME_SCALE) {
            if (progress && !checkProgress(progress))
                return false;
            for (int dx = -BIOME_RADIUS; dx <= BIOME_RADIUS; dx += BIOME_SCALE) {
                if (dx * dx + dz * dz > BIOME_RADIUS * BIOME_RADIUS)
                    continue;
                int id = getBiomeAt(g, BIOME_SCALE, (cx + dx) / BIOME_SCALE, cy, (cz + dz) / BIOME_SCALE);
                if (id != soul_sand_valley)
                    return false;
            }
        }
        return true;
    }

    bool fortressDenseBBAdjacent(const StoredPiece &a, const StoredPiece &b) {
        if (a.y0 != b.y0)
            return false;
        if (a.x1 != b.x1 && a.x1 + 1 != b.x0)
            return false;
        if (a.z1 != b.z1 && a.z1 + 1 != b.z0)
            return false;
        return true;
    }

    bool isFortressCrossOrStart(int type) {
        return type == FORTRESS_START || type == BRIDGE_CROSSING;
    }

    int pieceCenterX(const StoredPiece &p) {
        return (p.x0 + p.x1) / 2;
    }

    int pieceCenterZ(const StoredPiece &p) {
        return (p.z0 + p.z1) / 2;
    }

    void collectCrossNodes(const FortressData &fd, std::vector<const StoredPiece *> &nodes) {
        nodes.clear();
        nodes.reserve(fd.pieces.size());
        for (const StoredPiece &p: fd.pieces) {
            if (isFortressCrossOrStart(p.type))
                nodes.push_back(&p);
        }
    }

    int denseClusterCount(const std::vector<const StoredPiece *> &nodes,
                          const StoredPiece &seedPiece, std::vector<const StoredPiece *> &cluster) {
        cluster.clear();
        for (const StoredPiece *p: nodes) {
            if (fortressDenseBBAdjacent(seedPiece, *p))
                cluster.push_back(p);
        }
        return (int) cluster.size();
    }

    bool tripleJunctionFromPieces(const StoredPiece &corner,
                                  const StoredPiece &a, const StoredPiece &b, int &outX, int &outZ) {
        int cx = pieceCenterX(corner), cz = pieceCenterZ(corner);
        int ax = pieceCenterX(a), az = pieceCenterZ(a);
        int bx = pieceCenterX(b), bz = pieceCenterZ(b);
        int dxA = std::abs(ax - cx), dzA = std::abs(az - cz);
        int dxB = std::abs(bx - cx), dzB = std::abs(bz - cz);
        const StoredPiece *xArm = nullptr;
        const StoredPiece *zArm = nullptr;
        if (dxA >= dzA && dxB < dzB) {
            xArm = &a;
            zArm = &b;
        } else if (dxB >= dzB && dxA < dzA) {
            xArm = &b;
            zArm = &a;
        } else
            return false;

        outX = (pieceCenterX(corner) + pieceCenterX(*xArm)) / 2;
        outZ = (pieceCenterZ(corner) + pieceCenterZ(*zArm)) / 2;
        return true;
    }

    bool tryDouble(const FortressData &fd, CrossHit &hit) {
        std::vector<const StoredPiece *> nodes;
        collectCrossNodes(fd, nodes);
        const int b = (int) nodes.size();
        if (b < 2)
            return false;

        std::vector<const StoredPiece *> cluster;
        cluster.reserve(8);

        for (int i = 0; i < b; i++) {
            for (int j = i + 1; j < b; j++) {
                if (!fortressDenseBBAdjacent(*nodes[i], *nodes[j]))
                    continue;
                if (denseClusterCount(nodes, *nodes[i], cluster) != 2)
                    continue;
                if (denseClusterCount(nodes, *nodes[j], cluster) != 2)
                    continue;

                hit.outX = (pieceCenterX(*nodes[i]) + pieceCenterX(*nodes[j])) / 2;
                hit.outZ = (pieceCenterZ(*nodes[i]) + pieceCenterZ(*nodes[j])) / 2;
                hit.outY = std::min(nodes[i]->y0, nodes[j]->y0);
                hit.shape = "double";
                return true;
            }
        }
        return false;
    }

    bool tryTriple(const FortressData &fd, CrossHit &hit) {
        std::vector<const StoredPiece *> nodes;
        collectCrossNodes(fd, nodes);
        const int b = (int) nodes.size();
        if (b < 3)
            return false;

        std::vector<const StoredPiece *> cluster;
        cluster.reserve(8);

        for (int i = 0; i < b; i++) {
            if (denseClusterCount(nodes, *nodes[i], cluster) != 3)
                continue;

            for (int j = 0; j < 3; j++) {
                const StoredPiece &corner = *cluster[j];
                const StoredPiece *arms[2] = {nullptr, nullptr};
                int armN = 0;

                for (int k = 0; k < 3; k++) {
                    if (k == j)
                        continue;
                    if (!fortressDenseBBAdjacent(corner, *cluster[k]))
                        continue;
                    if (armN < 2)
                        arms[armN++] = cluster[k];
                }
                if (armN != 2)
                    continue;
                if (fortressDenseBBAdjacent(*arms[0], *arms[1]))
                    continue;

                int jx, jz;
                if (!tripleJunctionFromPieces(corner, *arms[0], *arms[1], jx, jz))
                    continue;

                hit.outX = jx;
                hit.outZ = jz;
                hit.outY = corner.y0;
                if (arms[0]->y0 < hit.outY)
                    hit.outY = arms[0]->y0;
                if (arms[1]->y0 < hit.outY)
                    hit.outY = arms[1]->y0;
                hit.shape = "triple";
                return true;
            }
        }
        return false;
    }

    bool tryQuad(const FortressData &fd, CrossHit &hit) {
        std::vector<const StoredPiece *> nodes;
        collectCrossNodes(fd, nodes);
        const int b = (int) nodes.size();
        if (b < 4)
            return false;

        for (int i = 0; i < b; i++) {
            const StoredPiece &pi = *nodes[i];
            int adj = 0;
            std::vector<const StoredPiece *> cluster;
            cluster.reserve(8);

            for (int j = 0; j < b; j++) {
                if (!fortressDenseBBAdjacent(pi, *nodes[j]))
                    continue;
                adj++;
                cluster.push_back(nodes[j]);
            }

            if (adj < 4)
                continue;
            if ((int) cluster.size() != 4)
                continue;

            int sumX = 0, sumZ = 0;
            int outY = cluster[0]->y0;
            for (const StoredPiece *sp: cluster) {
                outY = std::min(outY, sp->y0);
                sumX += (sp->x0 + sp->x1) / 2;
                sumZ += (sp->z0 + sp->z1) / 2;
            }

            hit.outX = sumX / 4;
            hit.outZ = sumZ / 4;
            hit.outY = outY;
            hit.shape = "quad";
            return true;
        }
        return false;
    }

    bool filterAllows(CrossFilter f, const char *shape) {
        if (f == CrossFilter::All)
            return true;
        if (f == CrossFilter::TripleQuad)
            return std::string(shape) == "triple" || std::string(shape) == "quad";
        if (f == CrossFilter::QuadOnly)
            return std::string(shape) == "quad";
        return true;
    }

    struct DedupState {
        std::mutex mu;
        std::unordered_set<uint64_t> seenCross;
        std::unordered_set<uint64_t> seenSpan;

        static uint64_t crossKey(int chunkX, int chunkZ, int shapeCode) {
            uint64_t h = (uint64_t) (uint32_t) chunkX << 32 | (uint32_t) chunkZ;
            h ^= (uint64_t) shapeCode * 0x9e3779b97f4a7c15ULL;
            return h;
        }

        static uint64_t spanKey(int chunkX, int chunkZ) {
            return (uint64_t) (uint32_t) chunkX << 32 | (uint32_t) chunkZ;
        }

        bool tryAddCross(int chunkX, int chunkZ, int shapeCode) {
            std::lock_guard<std::mutex> lock(mu);
            return seenCross.insert(crossKey(chunkX, chunkZ, shapeCode)).second;
        }

        bool tryAddSpan(int chunkX, int chunkZ) {
            std::lock_guard<std::mutex> lock(mu);
            return seenSpan.insert(spanKey(chunkX, chunkZ)).second;
        }
    };

    bool checkProgress(FortressProgress *progress) {
        while (g_search_pause.load() || (progress && progress->try_pause.load())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (g_search_stop.load() || (progress && progress->try_stop.load()))
                return false;
        }
        return !(g_search_stop.load() || (progress && progress->try_stop.load()));
    }

    void processRegionStrip(
        const InternalConfig &cfg,
        int regX,
        int regZ0,
        int regZ1,
        ThreadSafeResults<FortressHit> &out,
        DedupState &dedup,
        FortressProgress *progress) {
        Generator g;
        setupGenerator(&g, cfg.mc, 0);
        applySeed(&g, DIM_NETHER, cfg.seed);

        for (int regZ = regZ0; regZ <= regZ1; regZ++) {
            if (!checkProgress(progress))
                return;
            Pos pos;
            if (!getStructurePos(Fortress, cfg.mc, cfg.seed, regX, regZ, &pos))
                continue;
            if (!isViableStructurePos(Fortress, &g, pos.x, pos.z, 0))
                continue;
            if (!cfg.area.inExt(pos.x, pos.z))
                continue;

            const int chunkX = pos.x >> 4;
            const int chunkZ = pos.z >> 4;

            FortressData fd;
            if (!parseFortress(cfg.mc, cfg.seed, chunkX, chunkZ, fd, progress))
                continue;
            if (!checkProgress(progress))
                return;

            fd.startX = pos.x;
            fd.startZ = pos.z;

            if (cfg.mode == Mode::Span) {
                const int spanX = fd.maxX - fd.minX + 1;
                const int spanY = fd.maxY - fd.minY + 1;
                const int spanZ = fd.maxZ - fd.minZ + 1;
                const int longEdge = std::max(spanX, spanZ);
                const int shortEdge = std::min(spanX, spanZ);
                if (longEdge < cfg.minLong || shortEdge < cfg.minShort)
                    continue;
                if (spanY < cfg.minHeight)
                    continue;
                if (!cfg.area.inInit(fd.centerX, fd.centerZ))
                    continue;
                if (!dedup.tryAddSpan(chunkX, chunkZ))
                    continue;

                FortressHit hit;
                hit.chunkX = chunkX;
                hit.chunkZ = chunkZ;
                hit.outX = fd.centerX;
                hit.outY = SPAN_TP_Y;
                hit.outZ = fd.centerZ;
                hit.mode = 1;
                hit.shapeCode = 0;
                hit.longEdge = spanX;
                hit.heightEdge = spanY;
                hit.shortEdge = spanZ;
                out.addResult(hit);
                continue;
            }

            if (fd.crossings.size() < 2)
                continue;

            auto tryEmitCross = [&](const char *shapeName, bool (*fn)(const FortressData &, CrossHit &)) -> int {
                if (!checkProgress(progress))
                    return -1;
                if (!filterAllows(cfg.crossFilter, shapeName))
                    return 0;
                CrossHit h;
                if (!fn(fd, h))
                    return 0;
                if (!cfg.area.inInit(h.outX, h.outZ))
                    return 0;
                if (!onlySoulSandValley(&g, h.outX, h.outY, h.outZ, progress))
                    return 0;

                const int code = shapeNameToCode(shapeName);
                if (!dedup.tryAddCross(chunkX, chunkZ, code))
                    return 0;

                FortressHit hit;
                hit.chunkX = chunkX;
                hit.chunkZ = chunkZ;
                hit.outX = h.outX;
                hit.outY = h.outY;
                hit.outZ = h.outZ;
                hit.mode = 0;
                hit.shapeCode = code;
                out.addResult(hit);
                return 1;
            };

            // 若堡垒内存在四联路口，仅输出四联，忽略同堡垒的二�?三联
            int quadStatus = tryEmitCross("quad", tryQuad);
            if (quadStatus < 0)
                return;
            if (quadStatus > 0)
                continue;

            if (tryEmitCross("double", tryDouble) < 0)
                return;
            if (tryEmitCross("triple", tryTriple) < 0)
                return;
        }
    }
} // namespace

void runFortressSearch(const FortressSearchConfig &cfg,
                       FortressProgress *progress,
                       ThreadSafeResults<FortressHit> &out,
                       int numThreads) {
    const InternalConfig ic = toInternal(cfg);
    int threads = numThreads > 0 ? numThreads : defaultHwThreads();

    const int regXCount = ic.area.regX1 - ic.area.regX0 + 1;
    if (regXCount <= 0)
        return;

    if (progress) {
        progress->current = 0;
        progress->total = regXCount;
        progress->chunkInRunning = 0;
        progress->phase1 = 1;
        // try_pause / try_stop are controlled only via pause()/resume()/stop()/resetSearchState()
    }

    DedupState dedup;
    std::atomic<int> nextRegX{ic.area.regX0};
    std::atomic<int> completed{0};

    {
        ThreadPool pool((size_t) threads);
        if (progress)
            progress->chunkInRunning = 1;

        for (int t = 0; t < threads; t++) {
            pool.enqueue([&]() {
                while (true) {
                    if (!checkProgress(progress))
                        return;

                    int rx = nextRegX.fetch_add(1);
                    if (rx > ic.area.regX1)
                        return;

                    processRegionStrip(ic, rx, ic.area.regZ0, ic.area.regZ1, out, dedup, progress);

                    if (progress) {
                        progress->current = completed.fetch_add(1) + 1;
                    }
                }
            });
        }
    }

    if (progress) {
        progress->chunkInRunning = 0;
        progress->phase1 = -1;
    }
}
