#include "java_random.hpp"

int32_t JavaRandom::next(int bits) {
    state_ = (state_ * kMultiplier + kAddend) & kMask;
    // next(31) yields a value in [0, 2^31-1], which fits a positive int32_t.
    return static_cast<int32_t>(state_ >> (48 - bits));
}

int32_t JavaRandom::nextInt(int32_t bound) {
    // Power-of-two fast path (as in java.util.Random).
    if ((bound & -bound) == bound) {
        return static_cast<int32_t>(
            (static_cast<int64_t>(bound) * static_cast<int64_t>(next(31))) >> 31);
    }

    // Rejection sampling to avoid modulo bias. The overflow check
    // `bits - val + (bound - 1) < 0` is performed with 32-bit wrapping
    // arithmetic, matching Java's signed int overflow (and Slimy's Zig +%).
    int32_t bits, val;
    do {
        bits = next(31);
        val  = bits % bound;
    } while (static_cast<int32_t>(
                 static_cast<uint32_t>(bits) - static_cast<uint32_t>(val) +
                 static_cast<uint32_t>(bound - 1)) < 0);
    return val;
}
