#ifndef MONUMENT_SEARCH_CORE_H
#define MONUMENT_SEARCH_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque cubiomes Generator wrapper (defined only in .c). */
typedef struct MonumentGenerator MonumentGenerator;

MonumentGenerator *monument_generator_alloc(void);
void monument_generator_free(MonumentGenerator *g);

void monument_init_generator(MonumentGenerator *g, int mc, uint64_t seed);

/** @return 1 if monument exists at region and passes deep-ocean viability */
int monument_try_region(
    uint64_t seed,
    int mc,
    int regX,
    int regZ,
    int *outBlockX,
    int *outBlockZ);

/** Uses an already-initialized generator (no setupGenerator per call). */
int monument_try_region_g(
    MonumentGenerator *g,
    int mc,
    uint64_t seed,
    int regX,
    int regZ,
    int *outBlockX,
    int *outBlockZ);

/** @return 1 if a monument exists near block (targetX,targetZ) within tolerance */
int monument_exists_at_block(
    uint64_t seed,
    int mc,
    int targetX,
    int targetZ,
    int *outBlockX,
    int *outBlockZ);

int monument_exists_at_block_g(
    MonumentGenerator *g,
    int mc,
    uint64_t seed,
    int targetX,
    int targetZ,
    int *outBlockX,
    int *outBlockZ);

/** Ocean type at pair center: 0=others, 1=cold, 2=frozen */
int monument_classify_ocean_type(
    int mc,
    uint64_t seed,
    int centerX,
    int centerZ);

int monument_classify_ocean_type_g(
    MonumentGenerator *g,
    int mc,
    int centerX,
    int centerZ);

#ifdef __cplusplus
}
#endif

#endif
