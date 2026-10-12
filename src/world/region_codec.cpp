#include "world/region_codec.h"

#include <libdeflate.h>

#include <algorithm>
#include <cstring>

namespace region {

namespace {

u32 le32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }
void putLe32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(u8(v >> (8 * i)));
}
u32 rotl(u32 x, int r) { return (x << r) | (x >> (32 - r)); }

// XXHash32 (lz4-java's StreamingXXHash32, seeded like LZ4BlockOutputStream).
u32 xxh32(const u8* p, size_t n, u32 seed) {
    constexpr u32 P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u, P4 = 668265263u, P5 = 374761393u;
    const u8* end = p + n;
    u32 h;
    if (n >= 16) {
        u32 v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        for (; p + 16 <= end; p += 16) {
            v1 = rotl(v1 + le32(p) * P2, 13) * P1;
            v2 = rotl(v2 + le32(p + 4) * P2, 13) * P1;
            v3 = rotl(v3 + le32(p + 8) * P2, 13) * P1;
            v4 = rotl(v4 + le32(p + 12) * P2, 13) * P1;
        }
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
    } else {
        h = seed + P5;
    }
    h += u32(n);
    for (; p + 4 <= end; p += 4) h = rotl(h + le32(p) * P3, 17) * P4;
    for (; p < end; ++p) h = rotl(h + *p * P5, 11) * P1;
    h ^= h >> 15, h *= P2, h ^= h >> 13, h *= P3, h ^= h >> 16;
    return h;
}

// LZ4BlockOutputStream / LZ4BlockInputStream framing: "LZ4Block", token (method | level), compressed length,
// original length, checksum (XXHash32 & 0xFFFFFFF), data; a block of zero lengths ends the stream.
constexpr char kMagic[] = "LZ4Block";
constexpr u32 kSeed = 0x9747b28c;
constexpr u8 kRaw = 0x10, kLz4Method = 0x20;
constexpr size_t kBlockSize = 1 << 16;  // the default block size
constexpr u8 kLevel = 6;                // 32 - numberOfLeadingZeros(blockSize - 1) - 10

bool lz4BlockDecompress(const u8* in, size_t n, u8* out, size_t outSize) {
    const u8 *ip = in, *iend = in + n;
    size_t op = 0;
    while (ip < iend) {
        const u8 token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15)
            for (u8 b = 255; b == 255 && ip < iend;) lit += (b = *ip++);
        if (size_t(iend - ip) < lit || outSize - op < lit) return false;
        std::memcpy(out + op, ip, lit);
        ip += lit, op += lit;
        if (ip >= iend) break;  // the last sequence has literals only
        if (iend - ip < 2) return false;
        const size_t offset = size_t(ip[0]) | (size_t(ip[1]) << 8);
        ip += 2;
        size_t len = token & 15;
        if (len == 15)
            for (u8 b = 255; b == 255 && ip < iend;) len += (b = *ip++);
        len += 4;
        if (!offset || offset > op || outSize - op < len) return false;
        for (size_t i = 0; i < len; ++i, ++op) out[op] = out[op - offset];  // may overlap
    }
    return op == outSize;
}

void lz4Length(std::vector<u8>& out, size_t v) {
    for (; v >= 255; v -= 255) out.push_back(255);
    out.push_back(u8(v));
}

// Greedy LZ4 block (format rules: the last 5 bytes are literals, no match starts in the last 12).
void lz4BlockCompress(const u8* in, size_t n, std::vector<u8>& out) {
    constexpr int kHashBits = 14;
    std::vector<i32> table(size_t(1) << kHashBits, -1);
    auto read32 = [&](size_t i) {
        u32 v;
        std::memcpy(&v, in + i, 4);
        return v;
    };
    auto emit = [&](size_t anchor, size_t litEnd, size_t offset, size_t matchLen) {
        const size_t lit = litEnd - anchor;
        const size_t ml = matchLen ? matchLen - 4 : 0;
        out.push_back(u8((std::min<size_t>(lit, 15) << 4) | (matchLen ? std::min<size_t>(ml, 15) : 0)));
        if (lit >= 15) lz4Length(out, lit - 15);
        out.insert(out.end(), in + anchor, in + litEnd);
        if (!matchLen) return;
        out.push_back(u8(offset)), out.push_back(u8(offset >> 8));
        if (ml >= 15) lz4Length(out, ml - 15);
    };
    size_t anchor = 0, i = 0;
    if (n >= 13) {
        const size_t mflimit = n - 12, matchLimit = n - 5;
        while (i < mflimit) {
            const u32 v = read32(i);
            const u32 h = (v * 2654435761u) >> (32 - kHashBits);
            const i32 cand = table[h];
            table[h] = i32(i);
            if (cand >= 0 && i - size_t(cand) <= 65535 && read32(size_t(cand)) == v) {
                size_t len = 4;
                while (i + len < matchLimit && in[size_t(cand) + len] == in[i + len]) ++len;
                emit(anchor, i, i - size_t(cand), len);
                i += len, anchor = i;
            } else {
                ++i;
            }
        }
    }
    emit(anchor, n, 0, 0);
}

bool lz4StreamDecode(const u8* in, size_t n, std::vector<u8>& out) {
    out.clear();
    size_t pos = 0;
    while (pos + 21 <= n) {
        if (std::memcmp(in + pos, kMagic, 8) != 0) return false;
        const u8 method = in[pos + 8] & 0xF0;
        const u32 clen = le32(in + pos + 9), olen = le32(in + pos + 13), check = le32(in + pos + 17);
        pos += 21;
        if (olen == 0 && clen == 0) return check == 0;  // end mark
        if (clen > n - pos || olen > (u32(64) << 20)) return false;
        const size_t at = out.size();
        out.resize(at + olen);
        if (method == kRaw) {
            if (clen != olen) return false;
            std::memcpy(out.data() + at, in + pos, olen);
        } else if (method != kLz4Method || !lz4BlockDecompress(in + pos, clen, out.data() + at, olen)) {
            return false;
        }
        if ((xxh32(out.data() + at, olen, kSeed) & 0xFFFFFFF) != check) return false;
        pos += clen;
    }
    return pos == n && !out.empty();  // a stream cut short of its end mark still yields what it holds
}

void lz4StreamEncode(const u8* in, size_t n, std::vector<u8>& out) {
    out.clear();
    std::vector<u8> block;
    auto header = [&](u8 method, u32 clen, u32 olen, u32 check) {
        out.insert(out.end(), kMagic, kMagic + 8);
        out.push_back(u8(method | kLevel));
        putLe32(out, clen), putLe32(out, olen), putLe32(out, check);
    };
    for (size_t at = 0; at < n; at += kBlockSize) {
        const size_t len = std::min(kBlockSize, n - at);
        block.clear();
        lz4BlockCompress(in + at, len, block);
        const u32 check = xxh32(in + at, len, kSeed) & 0xFFFFFFF;
        if (block.size() >= len) {
            header(kRaw, u32(len), u32(len), check);
            out.insert(out.end(), in + at, in + at + len);
        } else {
            header(kLz4Method, u32(block.size()), u32(len), check);
            out.insert(out.end(), block.begin(), block.end());
        }
    }
    header(kRaw, 0, 0, 0);
}

}  // namespace

bool decode(int version, const u8* in, size_t size, std::vector<u8>& out) {
    switch (version) {
    case kNone: out.assign(in, in + size); return true;
    case kLz4: return lz4StreamDecode(in, size, out);
    case kGzip:
    case kDeflate: {
        libdeflate_decompressor* d = libdeflate_alloc_decompressor();
        out.resize(size * 4 + 4096);
        libdeflate_result r;
        for (;;) {
            size_t actual = 0;
            r = version == kGzip ? libdeflate_gzip_decompress(d, in, size, out.data(), out.size(), &actual)
                                 : libdeflate_zlib_decompress(d, in, size, out.data(), out.size(), &actual);
            if (r == LIBDEFLATE_SUCCESS) out.resize(actual);
            if (r != LIBDEFLATE_INSUFFICIENT_SPACE || out.size() > (size_t(512) << 20)) break;
            out.resize(out.size() * 2);
        }
        libdeflate_free_decompressor(d);
        return r == LIBDEFLATE_SUCCESS;
    }
    default: return false;  // custom (127) or unknown: the game cannot read these either
    }
}

bool encode(int version, const u8* in, size_t size, std::vector<u8>& out) {
    switch (version) {
    case kNone: out.assign(in, in + size); return true;
    case kLz4: lz4StreamEncode(in, size, out); return true;
    case kGzip:
    case kDeflate: {
        libdeflate_compressor* c = libdeflate_alloc_compressor(6);
        out.resize(version == kGzip ? libdeflate_gzip_compress_bound(c, size) : libdeflate_zlib_compress_bound(c, size));
        const size_t n = version == kGzip ? libdeflate_gzip_compress(c, in, size, out.data(), out.size())
                                          : libdeflate_zlib_compress(c, in, size, out.data(), out.size());
        libdeflate_free_compressor(c);
        out.resize(n);
        return n != 0;
    }
    default: return false;
    }
}

fs::path regionDir(const fs::path& saveDir, const std::string& dimension) {
    std::string ns = "minecraft", path = dimension.empty() ? "overworld" : dimension;
    if (const size_t colon = path.find(':'); colon != std::string::npos) ns = path.substr(0, colon), path = path.substr(colon + 1);
    const fs::path modern = saveDir / "dimensions" / ns / path / "region";
    std::error_code ec;
    if (fs::exists(modern, ec)) return modern;
    fs::path legacy;
    if (ns == "minecraft" && path == "overworld") legacy = saveDir / "region";
    else if (ns == "minecraft" && path == "the_nether") legacy = saveDir / "DIM-1" / "region";
    else if (ns == "minecraft" && path == "the_end") legacy = saveDir / "DIM1" / "region";
    return !legacy.empty() && fs::exists(legacy, ec) ? legacy : modern;
}

fs::path externalChunk(const fs::path& regionDir, int cx, int cz) {
    return regionDir / ("c." + std::to_string(cx) + "." + std::to_string(cz) + ".mcc");
}

}  // namespace region
