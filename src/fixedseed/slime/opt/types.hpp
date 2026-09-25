#pragma once

#include <cstdint>
#include <string>

// A single deduplicated candidate region coming from the Slimy CSV.
struct Candidate {
    int32_t chunkX = 0;
    int32_t chunkZ = 0;
    int     count  = 0;   // Slimy cluster count (max kept after dedup).
};

// The best AFK result computed for one candidate region.
struct Result {
    int32_t inputChunkX = 0;
    int32_t inputChunkZ = 0;
    int     inputCount  = 0;

    int64_t playerBlockX = 0;   // best AFK block coordinate
    int64_t playerBlockZ = 0;

    int     effectiveSlimeChunks = 0;   // distinct slime chunks touched by the ring
    int64_t effectiveSpawnArea   = 0;   // number of ring blocks inside slime chunks
};

// Runtime parameters (filled from the command line).
struct Params {
    int64_t     worldSeed        = 0;
    int         chunkSearchRange  = 100;   // slime-map radius in chunks around a candidate
    int         playerSearchRange = 5;     // AFK search radius in chunks around a candidate
    int         innerRadius       = 24;    // N  (blocks) : N < dist
    int         outerRadius       = 128;   // M  (blocks) : dist <= M
    std::string csvPath;
    std::string outDir = "output";
};
