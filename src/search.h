#ifndef SEARCH_H
#define SEARCH_H

#include "config.h"

#include "lua/src/lua.hpp"

#include <QVector>
#include <QString>
#include <QMap>
#include <atomic>

#define PRECOMPUTE48_BUFSIZ ((int64_t)1 << 30)

enum
{
    CAT_NONE,
    CAT_HELPER,
    CAT_QUAD,
    CAT_STRUCT,
    CAT_BIOMES,
    CAT_NETHER,
    CAT_END,
    CAT_OTHER,
    CATEGORY_MAX,
};

enum
{
    F_SELECT,
    F_QH_IDEAL,
    F_QH_CLASSIC,
    F_QH_NORMAL,
    F_QH_BARELY,
    F_QM_95,
    F_QM_90,
    F_BIOME,
    F_BIOME_4_RIVER,
    F_BIOME_16,
    F_BIOME_64,
    F_BIOME_256,
    F_BIOME_256_OTEMP,
    F_TEMPS,
    F_SLIME,
    F_SPAWN,
    F_STRONGHOLD,
    F_DESERT,
    F_JUNGLE,
    F_HUT,
    F_IGLOO,
    F_MONUMENT,
    F_VILLAGE,
    F_OUTPOST,
    F_MANSION,
    F_TREASURE,
    F_RUINS,
    F_SHIPWRECK,
    F_PORTAL,
    F_FORTRESS,
    F_BASTION,
    F_ENDCITY,
    F_BIOME_NETHER_1,
    F_BIOME_NETHER_4,
    F_BIOME_NETHER_16,
    F_BIOME_NETHER_64,
    F_BIOME_NETHER_256,
    F_BIOME_END_1,
    F_BIOME_END_4,
    F_BIOME_END_16,
    F_BIOME_END_64,
    F_PORTALN,
    F_GATEWAY,
    F_MINESHAFT,
    F_SPIRAL_1,
    F_SPIRAL_16,
    F_SPIRAL_64,
    F_SPIRAL_256,
    F_SPIRAL_512,
    F_SPIRAL_1024,
    F_BIOME_4, // differs from F_BIOME_4_RIVER, since this may include oceans
    F_SCALE_TO_NETHER,
    F_SCALE_TO_OVERWORLD,
    F_LOGIC_OR,
    F_SPIRAL_4,
    F_FIRST_STRONGHOLD,
    F_CLIMATE_NOISE,
    F_ANCIENT_CITY,
    F_LOGIC_NOT,
    F_BIOME_CENTER,
    F_BIOME_CENTER_256,
    F_CLIMATE_MINMAX,
    F_HEIGHT,
    F_LUA,
    F_WELL,
    F_TRAILS,
    // new filters should be added here at the end to keep some downwards compatibility
    FILTER_MAX,
};

struct FilterInfo
{
    int cat;    // seed source category
    bool dep64; // depends on 64-bit seed
    bool coord; // requires coordinate entry
    bool area;  // requires area entry
    bool rmax;  // supports radial range
    int layer;  // associated generator layer
    int stype;  // structure type
    int step;   // coordinate multiplier
    int pow2;   // bit position of step
    int count;  // can have instances
    int mcmin;  // minimum version
    int mcmax;  // maximum version
    int dim;    // dimension
    int hasy;   // has vertical height
    int disp;   // display order
    const char *icon;
    const char *name;
    const char *description;
};

// global table of filter data (as constants with enum indexing)
static const struct FilterList
{
    FilterInfo list[FILTER_MAX];

    FilterList() : list{}
    {
        int disp = 0; // display order

        list[F_SELECT] = FilterInfo{
            CAT_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, MC_UNDEF, MC_NEWEST, 0, 0, disp++,
            NULL,
            "",
            ""
        };
        list[F_LOGIC_OR] = FilterInfo{
            CAT_HELPER, 0, 0, 0, 0, 0, 0, 1, 0, 0, MC_UNDEF, MC_NEWEST, DIM_UNDEF, 0, disp++,
            ":icons/helper.png",
            QT_TRANSLATE_NOOP("Filter", "OR logic gate"),
            "符合其中任一条件即返回true, 没有条件时默认为true"
        };
        list[F_LOGIC_NOT] = FilterInfo{
            CAT_HELPER, 0, 0, 0, 0, 0, 0, 1, 0, 0, MC_UNDEF, MC_NEWEST, DIM_UNDEF, 0, disp++,
            ":icons/helper.png",
            QT_TRANSLATE_NOOP("Filter", "NOT logic gate"),
            "没有任何符合的条件才返回true, 没有条件时默认为true"
        };
        list[F_LUA] = FilterInfo{
            CAT_HELPER, 0, 0, 0, 0, 0, 0, 1, 0, 0, MC_UNDEF, MC_NEWEST, DIM_UNDEF, 0, disp++,
            ":icons/helper.png",
            QT_TRANSLATE_NOOP("Filter", "Lua"),
            "使用Lua脚本自定义条件"
        };
        list[F_SCALE_TO_NETHER] = FilterInfo{
            CAT_HELPER, 0, 0, 0, 0, 0, 0, 1, 0, 0, MC_UNDEF, MC_NEWEST, DIM_UNDEF, 0, disp++,
            ":icons/portal_lit.png",
            QT_TRANSLATE_NOOP("Filter", "Coordinate factor x/8"),
            "将坐标除以8, 用作主世界到地狱的坐标转换"
        };
        list[F_SCALE_TO_OVERWORLD] = FilterInfo{
            CAT_HELPER, 0, 0, 0, 0, 0, 0, 1, 0, 0, MC_UNDEF, MC_NEWEST, DIM_UNDEF, 0, disp++,
            ":icons/portal_lit.png",
            QT_TRANSLATE_NOOP("Filter", "Coordinate factor x*8"),
            "将坐标乘以8, 用作地狱到主世界的坐标转换"
        };
        const char *spiral_desc = "螺旋迭代器可以让其连带的搜索条件以固定步长遍历整片区域, 每一步所有条件都会重新检查一遍。\n"
                                  "迭代器会以螺旋状路径状遍历整片区域, 所以靠近区域中心的地方会被优先遍历到。"
        ;
        list[F_SPIRAL_1] = FilterInfo{
            CAT_HELPER, 0, 1, 1, 0, 0, 0, 1, 0, 0, MC_UNDEF, MC_NEWEST, DIM_UNDEF, 0, disp++,
            ":icons/reference.png",
            QT_TRANSLATE_NOOP("Filter", "Spiral iterator 1:1"),
            spiral_desc
        };
        list[F_SPIRAL_4] = FilterInfo{
            CAT_HELPER, 0, 1, 1, 0, 0, 0, 4, 2, 0, MC_UNDEF, MC_NEWEST, DIM_UNDEF, 0, disp++,
            ":icons/reference.png",
            QT_TRANSLATE_NOOP("Filter", "Spiral iterator 1:4"),
            spiral_desc
        };
        list[F_SPIRAL_16] = FilterInfo{
            CAT_HELPER, 0, 1, 1, 0, 0, 0, 16, 4, 0, MC_UNDEF, MC_NEWEST, DIM_UNDEF, 0, disp++,
            ":icons/reference.png",
            QT_TRANSLATE_NOOP("Filter", "Spiral iterator 1:16"),
            spiral_desc
        };
        list[F_SPIRAL_64] = FilterInfo{
            CAT_HELPER, 0, 1, 1, 0, 0, 0, 64, 6, 0, MC_UNDEF, MC_NEWEST, DIM_UNDEF, 0, disp++,
            ":icons/reference.png",
            QT_TRANSLATE_NOOP("Filter", "Spiral iterator 1:64"),
            spiral_desc
        };
        list[F_SPIRAL_256] = FilterInfo{
            CAT_HELPER, 0, 1, 1, 0, 0, 0, 256, 8, 0, MC_UNDEF, MC_NEWEST, DIM_UNDEF, 0, disp++,
            ":icons/reference.png",
            QT_TRANSLATE_NOOP("Filter", "Spiral iterator 1:256"),
            spiral_desc
        };
        list[F_SPIRAL_512] = FilterInfo{
            CAT_HELPER, 0, 1, 1, 0, 0, 0, 512, 9, 0, MC_UNDEF, MC_NEWEST, DIM_UNDEF, 0, disp++,
            ":icons/reference.png",
            QT_TRANSLATE_NOOP("Filter", "Spiral iterator 1:512"),
            spiral_desc
        };
        list[F_SPIRAL_1024] = FilterInfo{
            CAT_HELPER, 0, 1, 1, 0, 0, 0, 1024, 10, 0, MC_UNDEF, MC_NEWEST, DIM_UNDEF, 0, disp++,
            ":icons/reference.png",
            QT_TRANSLATE_NOOP("Filter", "Spiral iterator 1:1024"),
            spiral_desc
        };

        list[F_QH_IDEAL] = FilterInfo{
            CAT_QUAD, 0, 1, 1, 0, 0, Swamp_Hut, 512, 9, 0, MC_1_4, MC_NEWEST, 0, 0, disp++,
            ":icons/quad.png",
            QT_TRANSLATE_NOOP("Filter", "Quad-hut (ideal)"),
            "种子的低48位(二进制)决定了该种子具有成为最佳配置的四联女巫小屋的可能性"
        };

        list[F_QH_CLASSIC] = FilterInfo{
            CAT_QUAD, 0, 1, 1, 0, 0, Swamp_Hut, 512, 9, 0, MC_1_4, MC_NEWEST, 0, 0, disp++,
            ":icons/quad.png",
            QT_TRANSLATE_NOOP("Filter", "Quad-hut (classic)"),
            "种子的低48位(二进制)决定了该种子具有成为经典配置的四联女巫小屋的可能性"
        };

        list[F_QH_NORMAL] = FilterInfo{
            CAT_QUAD, 0, 1, 1, 0, 0, Swamp_Hut, 512, 9, 0, MC_1_4, MC_NEWEST, 0, 0, disp++,
            ":icons/quad.png",
            QT_TRANSLATE_NOOP("Filter", "Quad-hut (normal)"),
            "种子的低48位(二进制)决定了该种子具有成为普通配置的四联女巫小屋的可能性\n"
            "(保证四个小屋都在单人挂机距离内并且都有足够的垂直空间来摔死女巫)"
        };

        list[F_QH_BARELY] = FilterInfo{
            CAT_QUAD, 0, 1, 1, 0, 0, Swamp_Hut, 512, 9, 0, MC_1_4, MC_NEWEST, 0, 0, disp++,
            ":icons/quad.png",
            QT_TRANSLATE_NOOP("Filter", "Quad-hut (barely)"),
            "种子的低48位(二进制)决定了该种子具有成为最差配置的四联女巫小屋的可能性\n"
            "(只能保证四个小屋都在单人挂机距离内)"
        };

        list[F_QM_95] = FilterInfo{
            CAT_QUAD, 0, 1, 1, 0, 0, Monument, 512, 9, 0, MC_1_8, MC_NEWEST, 0, 0, disp++,
            ":icons/quad.png",
            QT_TRANSLATE_NOOP("Filter", "Quad-ocean-monument (>95%)"),
            "种子的低48位(二进制)决定了该种子具有成为四联海底神殿的可能性, 并且有超过95%的面积落在单人挂机距离内"
        };

        list[F_QM_90] = FilterInfo{
            CAT_QUAD, 0, 1, 1, 0, 0, Monument, 512, 9, 0, MC_1_8, MC_NEWEST, 0, 0, disp++,
            ":icons/quad.png",
            QT_TRANSLATE_NOOP("Filter", "Quad-ocean-monument (>90%)"),
            "种子的低48位(二进制)决定了该种子具有成为四联海底神殿的可能性, 并且有超过90%的面积落在单人挂机距离内"
        };

        list[F_BIOME] = FilterInfo{
            CAT_BIOMES, 1, 1, 1, 0, L_VORONOI_1, 0, 1, 0, 0, MC_B1_7, MC_1_17, 0, 1, disp++, // disable for 1.18
            ":icons/map.png",
            QT_TRANSLATE_NOOP("Filter", "Biomes 1:1"),
            "在指定范围内包括所有你想要的(+)群系并排除所有你不要的(-)"
        };

        list[F_BIOME_4] = FilterInfo{
            CAT_BIOMES, 1, 1, 1, 0, 0, 0, 4, 2, 0, MC_B1_7, MC_NEWEST, 0, 1, disp++,
            ":icons/map.png",
            QT_TRANSLATE_NOOP("Filter", "Biomes 1:4"),
            "在指定范围内包括所有你想要的(+)群系并排除所有你不要的(-)"
        };
        list[F_BIOME_16] = FilterInfo{
            CAT_BIOMES, 1, 1, 1, 0, 0, 0, 16, 4, 0, MC_B1_7, MC_NEWEST, 0, 1, disp++,
            ":icons/map.png",
            QT_TRANSLATE_NOOP("Filter", "Biomes 1:16"),
            "在指定范围内包括所有你想要的(+)群系并排除所有你不要的(-)"
        };
        list[F_BIOME_64] = FilterInfo{
            CAT_BIOMES, 1, 1, 1, 0, 0, 0, 64, 6, 0, MC_B1_7, MC_NEWEST, 0, 1, disp++,
            ":icons/map.png",
            QT_TRANSLATE_NOOP("Filter", "Biomes 1:64"),
            "在指定范围内包括所有你想要的(+)群系并排除所有你不要的(-)"
        };
        list[F_BIOME_256] = FilterInfo{
            CAT_BIOMES, 1, 1, 1, 0, 0, 0, 256, 8, 0, MC_B1_7, MC_NEWEST, 0, 1, disp++,
            ":icons/map.png",
            QT_TRANSLATE_NOOP("Filter", "Biomes 1:256"),
            "在指定范围内包括所有你想要的(+)群系并排除所有你不要的(-)"
        };

        list[F_BIOME_4_RIVER] = FilterInfo{
            CAT_BIOMES, 1, 1, 1, 0, L_RIVER_MIX_4, 0, 4, 2, 0, MC_1_13, MC_1_17, 0, 0, disp++,
            ":icons/map.png",
            QT_TRANSLATE_NOOP("Filter", "Biomes 1:4 RIVER"),
            "在指定范围内包括所有你想要的(+)群系并排除所有你不要的(-)\n"
            "但是只生成到1:4层的河流为止, 不生成海洋变种"
        };
        list[F_BIOME_256_OTEMP] = FilterInfo{
            CAT_BIOMES, 0, 1, 1, 0, L_OCEAN_TEMP_256, 0, 256, 8, 0, MC_1_13, MC_1_17, 0, 0, disp++,
            ":icons/map.png",
            QT_TRANSLATE_NOOP("Filter", "Biomes 1:256 O.TEMP"),
            "在指定范围内包括所有你想要的(+)群系并排除所有你不要的(-)\n"
            "仅生成到决定海洋温度的1:256\n"
            "这部分群系生成仅由种子低48位决定"
        };
        list[F_CLIMATE_NOISE] = FilterInfo{
            CAT_BIOMES, 0, 1, 1, 0, 0, 0, 4, 2, 0, MC_1_18, MC_NEWEST, 0, 0, disp++,
            ":icons/map.png",
            QT_TRANSLATE_NOOP("Filter", "Climate parameters 1:4"),
            "自定义指定区域内的群系的气候参数限制"
        };
        list[F_CLIMATE_MINMAX] = FilterInfo{
            CAT_BIOMES, 0, 1, 1, 0, 0, 0, 4, 2, 0, MC_1_18, MC_NEWEST, 0, 0, disp++,
            ":icons/map.png",
            QT_TRANSLATE_NOOP("Filter", "Locate climate extreme 1:4"),
            "找到该区域中气候参数的极值"
        };
        list[F_BIOME_CENTER] = FilterInfo{
            CAT_BIOMES, 1, 1, 1, 0, 0, 0, 4, 2, 1, MC_B1_7, MC_NEWEST, 0, 1, disp++,
            ":icons/map.png",
            QT_TRANSLATE_NOOP("Filter", "Locate biome center 1:4"),
            "找到给定群系的中心点"
        };
        list[F_BIOME_CENTER_256] = FilterInfo{
            CAT_BIOMES, 1, 1, 1, 0, 0, 0, 256, 8, 1, MC_B1_7, MC_1_17, 0, 1, disp++,
            ":icons/map.png",
            QT_TRANSLATE_NOOP("Filter", "Locate biome center 1:256"),
            "基于1:256群系层找到给定群系的中心点"
        };
        list[F_TEMPS] = FilterInfo{
            CAT_BIOMES, 1, 1, 1, 0, 0, 0, 1024, 10, 0, MC_1_7, MC_1_17, 0, 0, disp++,
            ":icons/tempcat.png",
            QT_TRANSLATE_NOOP("Filter", "Temperature categories"),
            "检查这块区域是否包含大于等于你所指定的数目的温度群系"
        };

        list[F_BIOME_NETHER_1] = FilterInfo{
            CAT_NETHER, 1, 1, 1, 0, 0, 0, 1, 0, 0, MC_1_16_1, 0, -1, 1, disp++, // disabled
            ":icons/nether.png",
            QT_TRANSLATE_NOOP("Filter", "Nether biomes 1:1 (disabled)"),
            "通过泰森多边形法算到 1:1 的下界群系"
        };
        list[F_BIOME_NETHER_4] = FilterInfo{
            CAT_NETHER, 0, 1, 1, 0, 0, 0, 4, 2, 0, MC_1_16_1, MC_NEWEST, -1, 0, disp++,
            ":icons/nether.png",
            QT_TRANSLATE_NOOP("Filter", "Nether biomes 1:4"),
            "使用正常噪声生成后在 1:4 的尺度下取样的下界群系"
        };
        list[F_BIOME_NETHER_16] = FilterInfo{
            CAT_NETHER, 0, 1, 1, 0, 0, 0, 16, 4, 0, MC_1_16_1, MC_NEWEST, -1, 0, disp++,
            ":icons/nether.png",
            QT_TRANSLATE_NOOP("Filter", "Nether biomes 1:16"),
            "使用正常噪声生成后在 1:16 的尺度下取样的下界群系"
        };
        list[F_BIOME_NETHER_64] = FilterInfo{
            CAT_NETHER, 0, 1, 1, 0, 0, 0, 64, 6, 0, MC_1_16_1, MC_NEWEST, -1, 0, disp++,
            ":icons/nether.png",
            QT_TRANSLATE_NOOP("Filter", "Nether biomes 1:64"),
            "使用正常噪声生成后在 1:64 的尺度下取样的下界群系"
        };
        list[F_BIOME_NETHER_256] = FilterInfo{
            CAT_NETHER, 0, 1, 1, 0, 0, 0, 256, 8, 0, MC_1_16_1, MC_NEWEST, -1, 0, disp++,
            ":icons/nether.png",
            QT_TRANSLATE_NOOP("Filter", "Nether biomes 1:256"),
            "使用正常噪声生成后在 1:256 的尺度下取样的下界群系"
        };

        list[F_BIOME_END_1] = FilterInfo{
            CAT_END, 1, 1, 1, 0, 0, 0, 1, 0, 0, MC_1_9, 0, +1, 1, disp++, // disabled
            ":icons/the_end.png",
            QT_TRANSLATE_NOOP("Filter", "End biomes 1:1 (disabled)"),
            "通过泰森多边形法算到 1:1 的末地群系"
        };
        list[F_BIOME_END_4] = FilterInfo{
            CAT_END, 0, 1, 1, 0, 0, 0, 4, 2, 0, MC_1_9, MC_NEWEST, +1, 0, disp++,
            ":icons/the_end.png",
            QT_TRANSLATE_NOOP("Filter", "End biomes 1:4"),
            "在 1:4 尺度下取样的末地群系生成\n"
            "注意其只是 1:16 的放大"
        };
        list[F_BIOME_END_16] = FilterInfo{
            CAT_END, 0, 1, 1, 0, 0, 0, 16, 4, 0, MC_1_9, MC_NEWEST, +1, 0, disp++,
            ":icons/the_end.png",
            QT_TRANSLATE_NOOP("Filter", "End biomes 1:16"),
            "在 1:16 尺度下正常取样的末地群系生成"
        };
        list[F_BIOME_END_64] = FilterInfo{
            CAT_END, 0, 1, 1, 0, 0, 0, 64, 6, 0, MC_1_9, MC_NEWEST, +1, 0, disp++,
            ":icons/the_end.png",
            QT_TRANSLATE_NOOP("Filter", "End biomes 1:64"),
            "在 1:64 尺度下松散取样的末地群系生成"
        };

        list[F_SPAWN] = FilterInfo{
            CAT_OTHER, 1, 1, 1, 1, 0, 0, 1, 0, 0, MC_B1_8, MC_NEWEST, 0, 0, disp++,
            ":icons/spawn.png",
            QT_TRANSLATE_NOOP("Filter", "Spawn"),
            ""
        };

        list[F_SLIME] = FilterInfo{
            CAT_OTHER, 0, 1, 1, 0, 0, 0, 16, 4, 1, MC_UNDEF, MC_NEWEST, 0, 0, disp++,
            ":icons/slime.png",
            QT_TRANSLATE_NOOP("Filter", "Slime chunk"),
            ""
        };
        list[F_HEIGHT] = FilterInfo{
            CAT_OTHER, 0, 1, 0, 0, 0, 0, 4, 2, 0, MC_1_1, MC_NEWEST, 0, 0, disp++,
            ":icons/height.png",
            QT_TRANSLATE_NOOP("Filter", "Surface height"),
            "以1:4的比例在某个坐标检查大致地表高度"
        };

        list[F_FIRST_STRONGHOLD] = FilterInfo{
            CAT_OTHER, 0, 1, 1, 1, 0, 0, 1, 0, 0, MC_B1_8, MC_NEWEST, 0, 0, disp++,
            ":icons/stronghold.png",
            QT_TRANSLATE_NOOP("Filter", "First stronghold"),
            "仅依靠低48位(二进制)找到第一个要塞的大致位置(+/-112格)"
        };

        list[F_STRONGHOLD] = FilterInfo{
            CAT_STRUCT, 1, 1, 1, 1, 0, 0, 1, 0, 1, MC_B1_8, MC_NEWEST, 0, 0, disp++,
            ":icons/stronghold.png",
            QT_TRANSLATE_NOOP("Filter", "Stronghold"),
            ""
        };

        list[F_VILLAGE] = FilterInfo{
            CAT_STRUCT, 1, 1, 1, 1, 0, Village, 1, 0, 1, MC_B1_8, MC_NEWEST, 0, 0, disp++,
            ":icons/village.png",
            QT_TRANSLATE_NOOP("Filter", "Village"),
            ""
        };

        list[F_MINESHAFT] = FilterInfo{
            CAT_STRUCT, 1, 1, 1, 0, 0, Mineshaft, 1, 0, 1, MC_B1_8, MC_NEWEST, 0, 0, disp++,
            ":icons/mineshaft.png",
            QT_TRANSLATE_NOOP("Filter", "Abandoned mineshaft"),
            ""
        };

        list[F_DESERT] = FilterInfo{
            CAT_STRUCT, 1, 1, 1, 1, 0, Desert_Pyramid, 1, 0, 1, MC_1_3, MC_NEWEST, 0, 0, disp++,
            ":icons/desert.png",
            QT_TRANSLATE_NOOP("Filter", "Desert pyramid"),
            "注意, 在1.18中, 林地府邸、沙漠神殿和丛林神殿的生成还会考虑其表面的高度, \n"
            "所以其在（含水）洞穴、河流或者海洋群系（甚至是较高的沙丘）周围可能会生成失败"
        };

        list[F_JUNGLE] = FilterInfo{
            CAT_STRUCT, 1, 1, 1, 1, 0, Jungle_Temple, 1, 0, 1, MC_1_3, MC_NEWEST, 0, 0, disp++,
            ":icons/jungle.png",
            QT_TRANSLATE_NOOP("Filter", "Jungle temple"),
            "注意, 在1.18中, 林地府邸、沙漠神殿和丛林神殿的生成还会考虑其表面的高度, \n"
            "所以其在（含水）洞穴、河流或者海洋群系（甚至是较高的沙丘）周围可能会生成失败"
        };

        list[F_HUT] = FilterInfo{
            CAT_STRUCT, 1, 1, 1, 1, 0, Swamp_Hut, 1, 0, 1, MC_1_4, MC_NEWEST, 0, 0, disp++,
            ":icons/hut.png",
            QT_TRANSLATE_NOOP("Filter", "Swamp hut"),
            ""
        };

        list[F_MONUMENT] = FilterInfo{
            CAT_STRUCT, 1, 1, 1, 1, 0, Monument, 1, 0, 1, MC_1_8, MC_NEWEST, 0, 0, disp++,
            ":icons/monument.png",
            QT_TRANSLATE_NOOP("Filter", "Ocean monument"),
            ""
        };

        list[F_IGLOO] = FilterInfo{
            CAT_STRUCT, 1, 1, 1, 1, 0, Igloo, 1, 0, 1, MC_1_9, MC_NEWEST, 0, 0, disp++,
            ":icons/igloo.png",
            QT_TRANSLATE_NOOP("Filter", "Igloo"),
            ""
        };

        list[F_MANSION] = FilterInfo{
            CAT_STRUCT, 1, 1, 1, 1, 0, Mansion, 1, 0, 1, MC_1_11, MC_NEWEST, 0, 0, disp++,
            ":icons/mansion.png",
            QT_TRANSLATE_NOOP("Filter", "Woodland mansion"),
            "注意, 在1.18中, 林地府邸、沙漠神殿和丛林神殿的生成还会考虑其表面的高度, \n"
            "所以其在（含水）洞穴、河流或者海洋群系（甚至是较高的沙丘）周围可能会生成失败"
        };

        list[F_RUINS] = FilterInfo{
            CAT_STRUCT, 1, 1, 1, 1, 0, Ocean_Ruin, 1, 0, 1, MC_1_13, MC_NEWEST, 0, 0, disp++,
            ":icons/ruins.png",
            QT_TRANSLATE_NOOP("Filter", "Ocean ruins"),
            ""
        };

        list[F_SHIPWRECK] = FilterInfo{
            CAT_STRUCT, 1, 1, 1, 1, 0, Shipwreck, 1, 0, 1, MC_1_13, MC_NEWEST, 0, 0, disp++,
            ":icons/shipwreck.png",
            QT_TRANSLATE_NOOP("Filter", "Shipwreck"),
            ""
        };

        list[F_TREASURE] = FilterInfo{
            CAT_STRUCT, 1, 1, 1, 1, 0, Treasure, 1, 0, 1, MC_1_13, MC_NEWEST, 0, 0, disp++,
            ":icons/treasure.png",
            QT_TRANSLATE_NOOP("Filter", "Buried treasure"),
            "宝藏总是出现在区块中心附近而不是区块边界附近, 请合理划定查找范围"
        };

        list[F_WELL] = FilterInfo{
            CAT_STRUCT, 1, 1, 1, 1, 0, Desert_Well, 1, 0, 1, MC_1_13, MC_NEWEST, 0, 0, disp++,
            ":icons/well.png",
            QT_TRANSLATE_NOOP("Filter", "Desert well"),
            ""
        };

        list[F_OUTPOST] = FilterInfo{
            CAT_STRUCT, 1, 1, 1, 1, 0, Outpost, 1, 0, 1, MC_1_14, MC_NEWEST, 0, 0, disp++,
            ":icons/outpost.png",
            QT_TRANSLATE_NOOP("Filter", "Pillager outpost"),
            ""
        };

        list[F_ANCIENT_CITY] = FilterInfo{
            CAT_STRUCT, 1, 1, 1, 1, 0, Ancient_City, 1, 0, 1, MC_1_19, MC_NEWEST, 0, 0, disp++,
            ":icons/ancient_city.png",
            QT_TRANSLATE_NOOP("Filter", "Ancient city"),
            ""
        };

        list[F_TRAILS] = FilterInfo{
            CAT_STRUCT, 1, 1, 1, 1, 0, Trail_Ruin, 1, 0, 1, MC_1_20, MC_NEWEST, 0, 0, disp++,
            ":icons/trail.png",
            QT_TRANSLATE_NOOP("Filter", "Trail ruins"),
            ""
        };

        list[F_PORTAL] = FilterInfo{
            CAT_STRUCT, 0, 1, 1, 1, 0, Ruined_Portal, 1, 0, 1, MC_1_16_1, MC_NEWEST, 0, 0, disp++,
            ":icons/portal.png",
            QT_TRANSLATE_NOOP("Filter", "Ruined portal (overworld)"),
            ""
        };

        list[F_PORTALN] = FilterInfo{
            CAT_STRUCT, 0, 1, 1, 1, 0, Ruined_Portal_N, 1, 0, 1, MC_1_16_1, MC_NEWEST, -1, 0, disp++,
            ":icons/portal.png",
            QT_TRANSLATE_NOOP("Filter", "Ruined portal (nether)"),
            ""
        };

        list[F_FORTRESS] = FilterInfo{
            CAT_STRUCT, 0, 1, 1, 1, 0, Fortress, 1, 0, 1, MC_1_0, MC_NEWEST, -1, 0, disp++,
            ":icons/fortress.png",
            QT_TRANSLATE_NOOP("Filter", "Nether fortress"),
            ""
        };

        list[F_BASTION] = FilterInfo{
            CAT_STRUCT, 0, 1, 1, 1, 0, Bastion, 1, 0, 1, MC_1_16_1, MC_NEWEST, -1, 0, disp++,
            ":icons/bastion.png",
            QT_TRANSLATE_NOOP("Filter", "Bastion remnant"),
            ""
        };

        list[F_ENDCITY] = FilterInfo{
            CAT_STRUCT, 0, 1, 1, 1, 0, End_City, 1, 0, 1, MC_1_9, MC_NEWEST, +1, 0, disp++,
            ":icons/endcity.png",
            QT_TRANSLATE_NOOP("Filter", "End city"),
            ""
        };

        list[F_GATEWAY] = FilterInfo{
            CAT_STRUCT, 0, 1, 1, 1, 0, End_Gateway, 1, 0, 1, MC_1_13, MC_NEWEST, +1, 0, disp++,
            ":icons/gateway.png",
            QT_TRANSLATE_NOOP("Filter", "End gateway"),
            "特指返程折跃门, 而非那些你打龙开的折跃门"
        };
    }
}
g_filterinfo;


struct /*__attribute__((packed))*/ Condition
{
    // should be POD or at least Standard Layout
    // layout needs to remain consistent across versions

    enum { // condition version upgrades
        VER_LEGACY      = 0,
        VER_2_3_0       = 1,
        VER_2_4_0       = 2,
        VER_CURRENT     = VER_2_4_0,
    };
    enum { // meta flags
        DISABLED        = 0x0001,
    };
    enum { // condition flags
        FLG_APPROX      = 0x0001,
        FLG_MATCH_ANY   = 0x0010,
        FLG_IN_RANGE    = 0x0020,
    };
    enum { // variant flags
        VAR_WITH_START  = 0x0001, // restrict start piece index and biome
        VAR_ABANODONED  = 0x0002, // zombie village
        VAR_ENDSHIP     = 0x0004, // end city ship
        VAR_DENSE_BB    = 0x0008, // fortress with a 2x2 arrangement of start/crossings
        VAR_NOT         = 0x0010, // invert flag (e.g. not abandoned)
        VAR_BASEMENT    = 0x0020, // igloo with basement
    };
    int16_t     type;
    uint16_t    meta;
    int32_t     x1, z1, x2, z2;
    int32_t     save;
    int32_t     relative;
    uint8_t     skipref;
    uint8_t     pad0[3]; // legacy
    char        text[28];
    uint8_t     pad1[12]; // legacy
    uint64_t    hash;
    uint8_t     deps[16]; // currently unused
    uint64_t    biomeToFind, biomeToFindM; // inclusion biomes
    int32_t     biomeId; // legacy oceanToFind(8)
    uint32_t    biomeSize;
    uint8_t     tol; // legacy specialCnt(4)
    uint8_t     minmax;
    uint8_t     para;
    uint8_t     octave;
    uint8_t     pad2[2]; // legacy zero initialized
    uint16_t    version; // condition data version
    uint64_t    biomeToExcl, biomeToExclM; // exclusion biomes
    int32_t     temps[9];
    int32_t     count;
    int32_t     y;
    uint32_t    flags;
    int32_t     rmax; // (<=0):disabled; (>0):strict upper radius
    uint16_t    varflags;
    int16_t     varbiome; // unused
    uint64_t    varstart;
    int32_t     limok[NP_MAX][2];
    int32_t     limex[NP_MAX][2];
    float       value;

    // generated members - initialized when the search is started
    uint8_t     generated_start[0]; // address dummy
    BiomeFilter bf;

    // perform version upgrades
    bool versionUpgrade();

    // initialize the generated members
    QString apply(int mc);

    QString toHex() const;
    bool readHex(const QString& hex);

    QString summary() const;
};

static_assert(
    offsetof(Condition, generated_start) == 308,
    "Layout of Condition has changed!"
);

struct ConditionTree
{
    QVector<Condition> condvec;
    std::vector<std::vector<char>> references;

    ~ConditionTree();
    QString set(const QVector<Condition>& cv, int mc);
};

struct SearchThreadEnv
{
    ConditionTree *condtree;

    Generator g;
    SurfaceNoise sn;

    int mc, large;
    uint64_t seed;
    int surfdim;
    int octaves;

    std::map<uint64_t, lua_State*> l_states;

    SearchThreadEnv();
    ~SearchThreadEnv();

    QString init(int mc, bool large, ConditionTree *condtree);

    void setSeed(uint64_t seed);
    void init4Dim(int dim);
    void init4Noise(int nptype, int octaves);
    void prepareSurfaceNoise(int dim);
};


#define MAX_INSTANCES 4096 // should be at least 128

enum
{
    COND_FAILED = 0,            // seed does not meet the condition
    COND_MAYBE_POS_INVAL = 1,   // search pass insufficient for result
    COND_MAYBE_POS_VALID = 2,   // search pass insufficient, but known center
    COND_OK = 3,                // seed satisfies the condition
};

enum
{
    PASS_FAST_48,       // only do fast checks that do not require biome gen
    PASS_FULL_48,       // include possible biome checks for 48-bit seeds
    PASS_FULL_64,       // run full test on a 64-bit seed
};

/* Checks if a seed satisfies the conditions tree.
 * Returns the lowest condition fulfillment status.
 */
int testTreeAt(
    Pos                         at,             // relative origin
    SearchThreadEnv           * env,            // thread-local environment
    int                         pass,           // search pass
    std::atomic_bool          * abort,          // abort signal
    Pos                       * path = 0        // ok trigger positions
);

int testCondAt(
    Pos                         at,             // relative origin
    SearchThreadEnv           * env,            // thread-local environment
    int                         pass,           // search pass
    std::atomic_bool          * abort,          // abort signal
    Pos                       * cent,           // output center position(s)
    int                       * imax,           // max instances (NULL for avg)
    Condition                 * cond            // condition to check
);

struct QuadInfo
{
    uint64_t c; // constellation seed
    Pos p[4];   // individual positions
    Pos afk;    // optimal afk position
    int flt;    // filter id (quality)
    int typ;    // type of structure
    int spcnt;  // number of planar spawning spaces
    float rad;  // enclosing radius
};

void findQuadStructs(int styp, Generator *g, QVector<QuadInfo> *out);


#endif // SEARCH_H
