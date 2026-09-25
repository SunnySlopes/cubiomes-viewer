#ifndef SEARCH_CONFIG_H
#define SEARCH_CONFIG_H

namespace SearchConfig {

/**
 * Cont 预筛步长；超过阈值的 tile 再膨胀，之后只在掩膜内做 Weirdness 环扫。
 */
constexpr int PHASE1_CONT_PREFILTER_SCALE = 128;
constexpr double PHASE1_CONT_PREFILTER_THRESHOLD = 0.525;
constexpr int PHASE1_CONT_PREFILTER_DILATE = 1;

/**
 * 掩膜内山脊（Weirdness）环扫步长。
 */
constexpr int PHASE1_WEIRDNESS_GRID_SCALE = 32;

/** 峰值邻域 ClimateCoarse 精筛步长（方块）。 */
constexpr int PHASE1_CLIMATE_GRID_SCALE = 4;

/** Phase2 精确群系采样网格步长（方块）。 */
constexpr int PRECISE_GRID_SCALE = 1;

/** Phase1 候选块聚合边长（方块）。 */
constexpr int CANDIDATE_TILE_BLOCKS = 256;

/** 气候精筛子区域半径（方块）。 */
constexpr int SUBSEARCH_RADIUS_BLOCKS = 256;
constexpr int SUBSEARCH_SIZE_BLOCKS = SUBSEARCH_RADIUS_BLOCKS * 2;

/** Phase2 精采样窗口（方块）。 */
constexpr int REFINE_WINDOW_BLOCKS = 320;
constexpr int REFINE_HALF_WINDOW = REFINE_WINDOW_BLOCKS / 2;

static_assert(PHASE1_CONT_PREFILTER_SCALE % PHASE1_WEIRDNESS_GRID_SCALE == 0,
              "PHASE1_CONT_PREFILTER_SCALE must be a multiple of PHASE1_WEIRDNESS_GRID_SCALE");
static_assert(PHASE1_WEIRDNESS_GRID_SCALE % PHASE1_CLIMATE_GRID_SCALE == 0,
              "PHASE1_WEIRDNESS_GRID_SCALE must be a multiple of PHASE1_CLIMATE_GRID_SCALE");

} // namespace SearchConfig

#endif
