#pragma once

#include <cstdint>

// A bit-exact port of java.util.Random as used by Minecraft (and by Slimy's
// scalar.Random). All state is kept in a 48-bit LCG.
class JavaRandom {
public:
    explicit JavaRandom(uint64_t seed) { setSeed(seed); }

    // seed = (seed ^ multiplier) & mask
    void setSeed(uint64_t seed) { state_ = (seed ^ kMultiplier) & kMask; }

    // Advances the generator and returns the top `bits` bits.
    int32_t next(int bits);

    // Java Random.nextInt(bound), including the rejection-sampling loop so the
    // distribution matches Java exactly (never a plain modulo).
    int32_t nextInt(int32_t bound);

private:
    static constexpr uint64_t kMultiplier = 0x5DEECE66DULL;
    static constexpr uint64_t kAddend     = 0xBULL;
    static constexpr uint64_t kMask       = (1ULL << 48) - 1ULL;

    uint64_t state_ = 0;
};
