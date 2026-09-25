#pragma once

#include <cstdint>

// Returns true when the given chunk is a slime chunk for `worldSeed`.
//
// This is a direct port of Slimy's scalar.getRandomSeed() + scalar.isSlime().
// All arithmetic is done in 64-bit two's-complement (wrapping) form, exactly
// like Zig's +% / *% operators, so results match Slimy bit-for-bit.
bool isSlimeChunk(int64_t worldSeed, int32_t chunkX, int32_t chunkZ);
