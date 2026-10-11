#pragma once
// Minecraft Java Edition wire format: big-endian scalars, VarInt/VarLong, length-prefixed strings,
// nameless-root network NBT, chat components and paletted chunk sections.
#include "core/common.h"
#include "world/world.h"

#include <unordered_map>

class BlockRegistry;
class GameReports;

namespace net {

// Reads one packet body. Any overrun sets ok = false and parks at the end (like NbtReader).
struct Reader {
    const u8* p;
    const u8* end;
    bool ok = true;

    Reader(const u8* data, size_t size) : p(data), end(data + size) {}
    size_t left() const { return size_t(end - p); }
    bool need(size_t n) {
        if (!ok || left() < n) {
            ok = false;
            p = end;
            return false;
        }
        return true;
    }
    void skip(size_t n) {
        if (need(n)) p += n;
    }
    u8 u8v() { return need(1) ? *p++ : 0; }
    bool boolean() { return u8v() != 0; }
    u16 u16v();
    u32 u32v();
    u64 u64v();
    i16 i16v() { return i16(u16v()); }
    i32 i32v() { return i32(u32v()); }
    i64 i64v() { return i64(u64v()); }
    float f32();
    double f64();
    i32 varInt();
    i64 varLong();
    std::string_view bytes(size_t n);
    std::string_view byteArray() { return bytes(size_t(u32(varInt()))); }  // VarInt length + bytes
    std::string_view str() { return byteArray(); }                          // UTF-8
    void blockPos(int& x, int& y, int& z);
    // Network NBT (type byte + nameless payload). Returns the whole tag, empty for TagEnd.
    std::string_view nbt();
};

struct Writer {
    std::vector<u8> buf;

    void u8v(u8 v) { buf.push_back(v); }
    void boolean(bool v) { buf.push_back(v ? 1 : 0); }
    void u16v(u16 v);
    void u32v(u32 v);
    void u64v(u64 v);
    void f32(float v);
    void f64(double v);
    void varInt(i32 v);
    void varLong(i64 v);
    void bytes(const void* data, size_t n);
    void str(std::string_view s);
    void blockPos(int x, int y, int z) { u64v((u64(x & 0x3FFFFFF) << 38) | (u64(z & 0x3FFFFFF) << 12) | u64(y & 0xFFF)); }
};

// Flattens a network NBT chat component to plain text, resolving translation keys with `lang`.
std::string componentText(std::string_view nbt, const std::unordered_map<std::string, std::string>* lang);
// Same for the JSON form used by login disconnects.
std::string jsonComponentText(std::string_view json, const std::unordered_map<std::string, std::string>* lang);

// Network block state ids (the global palette) -> BlockRegistry ids, resolved on first use.
// Not thread-safe: owned by the network thread.
class BlockIds {
public:
    void init(const GameReports& reports, BlockRegistry& registry);
    // A server's own state table (modded Fabric servers, net/fabric_sync.h): id -> state key.
    void initTable(std::vector<std::string> names, BlockRegistry& registry);
    u16 local(u32 globalId) {
        if (globalId >= map_.size()) return 0;
        const u16 v = map_[globalId];
        return v != kUnresolved ? v : resolve(globalId);
    }
    int globalBits() const { return globalBits_; }  // bits per entry of a direct-palette section

private:
    static constexpr u16 kUnresolved = 0xFFFF;
    u16 resolve(u32 globalId);
    std::vector<u16> map_;
    std::vector<std::string> names_;
    BlockRegistry* registry_ = nullptr;
    int globalBits_ = 16;
};

int ceilLog2(u32 n);

// Decodes the section data of a level_chunk_with_light packet ("buffer": `count` sections from
// section y minSy) into out[1 .. count] (out[0] and out[count + 1] are the light-only sections
// below and above the build range). Returns false on malformed data.
// biomeIds: the server's biome registry ids -> World biome ids (sections keep their lowest quart layer).
bool decodeSections(Reader& r, int count, BlockIds& ids, int biomeBits, std::vector<World::PackedSection>& out,
                    const std::vector<u8>* biomeIds = nullptr);

// One light layer section from a light mask: data (2048 bytes) or all zero (`empty` mask).
struct LightSection {
    int sy = 0;
    bool sky = false;
    std::vector<u8> data;  // empty = all zero
};
// Reads the light part of chunk/light packets: section index i is section y minSy - 1 + i.
bool readLight(Reader& r, int minSy, int lightSections, std::vector<LightSection>& out);

}  // namespace net
