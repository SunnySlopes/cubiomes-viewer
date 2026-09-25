#include "swamp_hut_search.h"

#include "eval.h"
#include "structure.h"

#include "cubiomes/biomes.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

/* lysh/search.h conflicts with viewer src/search.h on the include path. */
extern "C" int lysh_cpu_count(void);

namespace {

static bool checkProgress(WitchHutProgress *p)
{
    if (!p) return true;
    while (p->try_pause.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (p->try_stop.load()) return false;
    }
    return !p->try_stop.load();
}

static void fillPhase1Opts(lysh_phase1_opts *opts, int mc)
{
    std::memset(opts, 0, sizeof(*opts));
    opts->mc_1_18_2 = (mc <= MC_1_18_2) ? 1 : 0;
    opts->single_biome = 0;
    // pre_26_2=0 → 26.2+ ridge rules; pre_26_2=1 → older ridge rules
    opts->pre_26_2 = (mc < MC_26_2) ? 1 : 0;
    // pre_26_3=0 → btree263; pre_26_3=1 → btree262 (when not already on older trees)
    opts->pre_26_3 = (mc < MC_26_3) ? 1 : 0;
    opts->large_biomes = 0;
}

static void appendHits(const lysh_hut_grade &grade, std::vector<WitchHutHit> &out)
{
    if (!grade.hits || grade.hits_written <= 0)
        return;
    for (int i = 0; i < grade.hits_written; i++) {
        const int *h = grade.hits + (size_t)i * LYSH_HUT_GRADE_INTS;
        // layout: hutX, hutZ, rx, rz, dir, avg_y, flooded, ok, ...
        if (!h[7])
            continue;
        WitchHutHit hit;
        hit.x = h[0];
        hit.z = h[1];
        hit.dir = h[4];
        hit.y = h[5];
        out.push_back(hit);
    }
}

/** Open a scan session; back off thread count on OOM (LowYSwampHut scanOpen == 0). */
static lysh_scan_session *openSessionWithBackoff(uint64_t seed, const lysh_phase1_opts *opts,
                                                 int maxHeight, int salt, int wantThreads)
{
    int cpu = lysh_cpu_count();
    if (cpu < 1) cpu = 1;
    int t = wantThreads > 0 ? wantThreads : cpu;
    if (t > cpu) t = cpu;
    if (t < 1) t = 1;

    while (t >= 1) {
        lysh_scan_session *session = lysh_scan_session_open(seed, opts, maxHeight, salt, t);
        if (session)
            return session;
        if (t == 1)
            break;
        t = (t > 2) ? (t / 2) : 1;
    }
    return nullptr;
}

} // namespace

bool runWitchHutSearch(const WitchHutSearchConfig &cfg, WitchHutProgress *progress,
                       std::vector<WitchHutHit> &out, std::string *errOut)
{
    out.clear();
    if (errOut)
        errOut->clear();

    const int minX = std::min(cfg.minX, cfg.maxX);
    const int maxX = std::max(cfg.minX, cfg.maxX);
    const int minZ = std::min(cfg.minZ, cfg.maxZ);
    const int maxZ = std::max(cfg.minZ, cfg.maxZ);

    const int rx0 = minX >> 9;
    const int rx1 = (maxX >> 9) + 1;
    const int rz0 = minZ >> 9;
    const int rz1 = (maxZ >> 9) + 1;
    if (rx0 >= rx1 || rz0 >= rz1) {
        if (progress) {
            progress->current = 1;
            progress->total = 1;
        }
        return true;
    }

    lysh_phase1_opts opts;
    fillPhase1Opts(&opts, cfg.mc);

    const int salt = 0; // default LYSH_SWAMP_HUT_SALT inside open
    // LowYSwampHut: phase1CheckHeight = max(maxY, -50); -54/-50 both use -50 in phase 1
    const int maxHeight = std::max(cfg.maxY, -50);
    const int maxY = cfg.maxY;

    const int bandRows = rz1 - rz0;
    if (progress) {
        progress->current = 0;
        // +1 step for session init so the bar moves before the first slow band.
        progress->total = std::max(1, bandRows + 1);
    }

    // One seed → one session (noise stack init once). OOM → back off threads then fail loudly.
    lysh_scan_session *session =
        openSessionWithBackoff(cfg.seed, &opts, maxHeight, salt, cfg.threads);
    if (!session) {
        if (progress)
            progress->current = progress->total.load();
        if (errOut) {
            *errOut = "lysh_scan_session_open returned null "
                      "(out of memory or invalid arguments; each worker needs ~0.5 MiB)";
        }
        return false;
    }
    if (progress)
        progress->current = 1;

    // Growable hit buffer (12 ints per hut).
    int hitCap = 256;
    std::vector<int> hitBuf((size_t)hitCap * LYSH_HUT_GRADE_INTS);

    for (int rz = rz0; rz < rz1; rz++) {
        if (!checkProgress(progress))
            break;

        lysh_hut_grade grade;
        std::memset(&grade, 0, sizeof(grade));
        grade.hits = hitBuf.data();
        grade.hits_cap = hitCap;

        lysh_scan_session_band(session, rx0, rx1, rz, rz + 1, maxY, &grade);

        // Retry with larger buffer if capacity was insufficient.
        if (grade.accepted > hitCap || (grade.accepted > 0 && grade.hits_written < grade.accepted
                                        && grade.hits_written == hitCap)) {
            hitCap = (int)grade.accepted + 64;
            hitBuf.assign((size_t)hitCap * LYSH_HUT_GRADE_INTS, 0);
            std::memset(&grade, 0, sizeof(grade));
            grade.hits = hitBuf.data();
            grade.hits_cap = hitCap;
            lysh_scan_session_band(session, rx0, rx1, rz, rz + 1, maxY, &grade);
        }

        appendHits(grade, out);

        if (progress)
            progress->current = 1 + (rz - rz0) + 1;
    }

    lysh_scan_session_free(session);

    if (progress)
        progress->current = progress->total.load();
    return true;
}
