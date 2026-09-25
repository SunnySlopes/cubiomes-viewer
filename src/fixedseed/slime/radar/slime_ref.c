/*
 * slime_ref.c — 阶段 0 朴素黄金参照实现（只求正确，不求快）
 *
 * 用法:  slime_ref SEED RANGE THRESHOLD
 *   区域为区块坐标半开区间 [-RANGE, RANGE)²，阈值默认 45。
 * 输出:  每个命中中心 `x,z,count`（CSV，未排序），到 stdout。
 *
 * 正确性策略:
 *   数甜甜圈仍是最朴素的“逐格 dx,dz ∈ [-8,8]，取 1<d²<=64 的格累加”。
 *   唯一的非算法处理: 先把 [-R-8, R+8) 的 is_slime 结果缓存进字节数组，
 *   避免同一格被重复判定 ~289 次（否则 R=10000 单线程跑不完）。
 *   这不改变计数逻辑，结果与纯朴素逐点重算逐位一致。
 *
 * 参见 渐进式开发流程.md 阶段 0。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "slime.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s SEED RANGE [THRESHOLD]\n", argv[0]);
        return 1;
    }
    int64_t  seed      = (int64_t)strtoll(argv[1], NULL, 10);
    int64_t  range     = (int64_t)strtoll(argv[2], NULL, 10);
    unsigned threshold = (argc >= 4) ? (unsigned)strtoul(argv[3], NULL, 10) : 45u;

    if (range <= 0) { fprintf(stderr, "RANGE must be > 0\n"); return 1; }

    /* 搜索中心区域: [-R, R) × [-R, R) （区块坐标，半开区间）。 */
    const int32_t lo = (int32_t)(-range);
    const int32_t hi = (int32_t)( range);          /* 不含 */
    const int64_t side = 2 * range;                /* 中心区域边长 */

    /* slime 图需覆盖 [lo-8, hi+8) 以供边缘中心的 17×17 窗口读取。 */
    const int32_t map_lo   = lo - SR_OFFSET;
    const int64_t map_side = side + 2 * SR_OFFSET;  /* = 2R + 16 */
    const size_t  map_cells = (size_t)map_side * (size_t)map_side;

    uint8_t *map = (uint8_t *)malloc(map_cells);
    if (!map) {
        fprintf(stderr, "malloc failed (%zu bytes)\n", map_cells);
        return 2;
    }

    /* 填 slime 图: map[(z-map_lo)*map_side + (x-map_lo)] = is_slime(x,z) */
    fprintf(stderr, "[ref] building slime map %lldx%lld (%.0f MiB)...\n",
            (long long)map_side, (long long)map_side,
            (double)map_cells / (1024.0 * 1024.0));
    for (int64_t rz = 0; rz < map_side; ++rz) {
        int32_t z = map_lo + (int32_t)rz;
        uint8_t *row = map + (size_t)rz * (size_t)map_side;
        for (int64_t rx = 0; rx < map_side; ++rx) {
            int32_t x = map_lo + (int32_t)rx;
            row[rx] = (uint8_t)sr_is_slime(seed, x, z);
        }
    }

    /* 预计算甜甜圈偏移列表（相对 dx,dz），朴素逐格数用。 */
    int off_dx[SR_WINDOW * SR_WINDOW];
    int off_dz[SR_WINDOW * SR_WINDOW];
    int n_off = 0;
    for (int dz = -SR_OFFSET; dz <= SR_OFFSET; ++dz)
        for (int dx = -SR_OFFSET; dx <= SR_OFFSET; ++dx)
            if (sr_in_donut(dx, dz)) { off_dx[n_off] = dx; off_dz[n_off] = dz; ++n_off; }

    fprintf(stderr, "[ref] donut cells = %d; scanning centers...\n", n_off);

    /* 遍历每个中心，朴素数甜甜圈。map 索引偏移: 中心 (x,z) 在图中的行列。 */
    uint64_t hits = 0;
    for (int32_t cz = lo; cz < hi; ++cz) {
        int64_t base_rz = (int64_t)cz - map_lo;   /* 中心行（图坐标） */
        for (int32_t cx = lo; cx < hi; ++cx) {
            int64_t base_rx = (int64_t)cx - map_lo;
            unsigned count = 0;
            for (int i = 0; i < n_off; ++i) {
                size_t idx = (size_t)(base_rz + off_dz[i]) * (size_t)map_side
                           + (size_t)(base_rx + off_dx[i]);
                count += map[idx];
            }
            if (count >= threshold) {
                printf("%d,%d,%u\n", cx, cz, count);
                ++hits;
            }
        }
    }

    fflush(stdout);
    fprintf(stderr, "[ref] done. hits = %llu\n", (unsigned long long)hits);
    free(map);
    return 0;
}
