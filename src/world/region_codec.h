#pragma once
// Region file chunk streams (RegionFile / RegionFileVersion): the compression a chunk is stored with and where its
// files are. Every version the game reads: 1 gzip, 2 zlib (the default), 3 none, 4 LZ4 (lz4-java's LZ4Block stream,
// server.properties region-file-compression=lz4); 127 "custom" the game itself cannot read. A chunk of 256 sectors or
// more lives in an external c.<x>.<z>.mcc next to the region (flag 128 on its version byte).
#include "core/common.h"

#include <vector>

namespace region {

constexpr int kGzip = 1, kDeflate = 2, kNone = 3, kLz4 = 4, kCustom = 127;
constexpr int kExternalFlag = 128;

// Decompresses one chunk stream into `out`; false for an unknown / unreadable version (custom included).
bool decode(int version, const u8* in, size_t size, std::vector<u8>& out);
// Compresses NBT into a chunk stream of `version` (1-4); false if it cannot.
bool encode(int version, const u8* in, size_t size, std::vector<u8>& out);

// The region folder of a dimension ("minecraft:the_nether"): dimensions/<ns>/<path>/region (26.x), or the old
// layout's region, DIM-1/region, DIM1/region when the save still uses it.
fs::path regionDir(const fs::path& saveDir, const std::string& dimension);
// c.<cx>.<cz>.mcc (an oversized chunk's stream, beside its region file).
fs::path externalChunk(const fs::path& regionDir, int cx, int cz);

}  // namespace region
