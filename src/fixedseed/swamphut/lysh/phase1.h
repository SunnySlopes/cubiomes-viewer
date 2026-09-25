/* lysh phase1.h —— 阶段 1 完整判定（SearchCoords.check 的 C 移植）。
 *
 * 判定顺序（必须与 Java 一致，否则 funnel 计数不同）：
 *   1. 群系近似门：erosion >= 0.55 → temperature → ridge   （单群系世界整段跳过）
 *   2. Entrance(x+3, 50, z+3) >= 0        → 淘汰
 *   3. Entrance(x+3, 60, z+3) >= 0        → 淘汰
 *   4. Entrance2(x+3, maxHeight, z+3) >= 0 && Cheese(...) >= 0  → 淘汰
 *   5. y = 0, -10, ... >= ladderFloor 且 maxHeight < y：
 *        Entrance2(y) >= 0 && Cheese(y) >= 0 → 淘汰
 *      ladderFloor = max(-50, min(-40, maxHeight))
 *   6. y = 10, 20, 30, 40：Entrance(y) >= 0 && Cheese(y) >= 0 → 淘汰
 *   7. 群系近似门：continentalness < -0.11 → 淘汰
 *   8. y = maxHeight, +10, ... <= 60：aquiferFloodedness(x+3, y*0.67, z+3) > 0.41 → 淘汰
 *
 * ⚠️ 第 7 步在洞穴梯子【之后】—— 别为了"看起来更合理"把它提前，
 *    否则 funnel 数字会变（这一步只在 <1% 的格子上被采到）。
 *
 * 关于近似：第 1、7 步是用户明确要求保持的近似（精确群系判定会慢约十倍）。
 * C 端的目标是【逐位复现这个近似】，不是"修正"它。
 */
#ifndef LYSH_PHASE1_H
#define LYSH_PHASE1_H

#include <stdint.h>

#include "caves.h"
#include "climate.h"
#include "noise.h"

typedef struct {
    lysh_climate climate;
    lysh_caves caves;
    lysh_dblnoise aquifer_floodedness;   /* minecraft:aquifer_fluid_level_floodedness */
} lysh_phase1;

typedef struct {
    int mc_1_18_2;      /* 温度只判下界 -0.45（其余版本还判上界 0.2） */
    int single_biome;   /* 单群系世界：跳过第 1、7 步 */
    /* ⚠️ 这是**反向**开关：0（= 全零初始化的"没传"）表示 **26.2 语义**，即
     *    RidgeFold 之后的 ridge <= -0.91 也要淘汰。置 1 才是 26.2 之前
     *    （1.19 ~ 26.1）那条"没有 ridge 下界"的分支。
     *    之所以反向，是因为用户明确要求 **26.2 是默认版本**：任何
     *    `memset(&opts, 0, sizeof(opts))` 的调用方都必须拿到 26.2 语义，
     *    而不是某个更老的分支。显式调用方（CLI / JNI）按自己的版本来设这个位。 */
    int pre_26_2;       /* 1 = 用 26.2 之前的 ridge 规则（无 ridge <= -0.91 淘汰） */
    /* 0（默认）= 26.3 群系参数树；1 = 仍用 26.2 参数树（btree262）。 */
    int pre_26_3;
    int large_biomes;   /* erosion/temperature/continentalness 换 _large 变体 */
} lysh_phase1_opts;

void lysh_phase1_init(lysh_phase1 *p, uint64_t world_seed, const lysh_phase1_opts *opts);

/* 淘汰原因：与 Java 的每个 return 点一一对应，便于把 funnel 差异定位到具体那一步 */
enum {
    LYSH_P1_REJECT_EROSION = 0,
    LYSH_P1_REJECT_TEMPERATURE = 1,
    LYSH_P1_REJECT_RIDGE = 2,
    LYSH_P1_REJECT_ENTRANCE_50 = 3,
    LYSH_P1_REJECT_ENTRANCE_60 = 4,
    LYSH_P1_REJECT_CAVE_AT_MAXH = 5,
    LYSH_P1_REJECT_CAVE_LADDER_NEG = 6,
    LYSH_P1_REJECT_CAVE_LADDER_POS = 7,
    LYSH_P1_REJECT_CONTINENTALNESS = 8,
    LYSH_P1_REJECT_FLOODEDNESS = 9,
    LYSH_P1_ACCEPT = 10,
    LYSH_P1_REJECT_KIND_N = 11
};

typedef struct {
    int reject;      /* 上面的枚举 */
} lysh_phase1_result;

/* 「erosion 提前退出」的档位统计。
 * tier_hist（长度 LYSH_TIER_MAX，可为 NULL）：每在某一档提前淘汰就 +1。 */
int lysh_phase1_check_ex(const lysh_phase1 *p, const lysh_phase1_opts *o,
                         int hut_x, int hut_z, int max_height,
                         lysh_phase1_result *out,
                         long long *tier_hist);

/* ---- 探针：逐格采样原始 double --------------------------------------------
 * 按【固定顺序】无条件采样 LYSH_P1_PROBE_N 个原始 double（不做任何短路），
 * 这样一次 diff 就能同时验证「值对不对」和「判定逻辑对不对」。
 * 调用时 max_height 恒取 -50（= 阶段 1 的 phase1CheckHeight 规范值）。
 * 顺序定义在 phase1.c 的 lysh_phase1_probe 上方（唯一一份）。
 * 产品路径在 `lysh hut` 里按固定下标读前四项（气候门），见 main.c。 */
#define LYSH_P1_PROBE_N 38
void lysh_phase1_probe(const lysh_phase1 *p, int hut_x, int hut_z, int max_height,
                       double *out /* [LYSH_P1_PROBE_N] */);

#endif /* LYSH_PHASE1_H */
