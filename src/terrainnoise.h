#ifndef TERRAINNOISE_H_
#define TERRAINNOISE_H_

/*
 * Viewer-only include shim for MinGW.
 *
 * Upstream finders.h includes terrainnoise.h (huge TerrainNoise). Many viewer
 * TUs pull finders.h via config.h, which OOMs cc1plus under -O2/-O3.
 *
 * At build time we place a copy of finders.h next to this shim so that
 * #include "terrainnoise.h" from finders.h resolves here instead of the
 * real cubiomes/terrainnoise.h. libcubiomes is still built from the
 * unmodified submodule (no shim on its include path).
 *
 * BlendedNoise / SurfaceNoise / Generator come from generator.h.
 */
#include "generator.h"

#endif /* TERRAINNOISE_H_ */
