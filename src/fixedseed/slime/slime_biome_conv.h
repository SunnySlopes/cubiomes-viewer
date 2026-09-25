#ifndef SLIME_BIOME_CONV_H
#define SLIME_BIOME_CONV_H

#include "cubiomes/biomes.h"

namespace SlimeBiomeConv {

inline void factorsFor(int mc, int biomeId, int &num, int &den)
{
    num = 1;
    den = 1;

    if (biomeId == river) { num = 515; den = 615; return; }
    if (biomeId == mushroom_fields) { num = 0; den = 1; return; }

    if (mc <= MC_1_17) {
        if (biomeId == giant_tree_taiga || biomeId == old_growth_pine_taiga) { num = 515; den = 540; return; }
        if (biomeId == giant_tree_taiga_hills) { num = 515; den = 540; return; }
        if (biomeId == mushroom_field_shore) { num = 0; den = 1; return; }
        return;
    }

    // 1.18+
    if (biomeId == dripstone_caves) { num = 515; den = 610; return; }
    if (biomeId == giant_tree_taiga || biomeId == old_growth_pine_taiga) { num = 515; den = 540; return; }

    if (mc >= MC_1_19 && biomeId == deep_dark) { num = 0; den = 1; return; }
    if (mc >= MC_26_2 && biomeId == sulfur_caves) { num = 0; den = 1; return; }
    // dappled_forest (26.3+) is surface; treat as neutral for slime-depth AFK.
}

inline int apply(int area, int num, int den)
{
    if (num == 0) return 0;
    if (den <= 0 || num == den) return area;
    return (area * num + den / 2) / den;
}

} // namespace SlimeBiomeConv

#endif
