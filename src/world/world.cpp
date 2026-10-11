#include "world/world.h"

#include <mutex>

#include "assets/assets.h"
#include "core/jobs.h"
#include "world/nbt.h"
#include "world/nbt_dom.h"

#include <libdeflate.h>

#include <algorithm>
#include <atomic>
#include <climits>

namespace {

u64 regionKey(int rx, int rz) { return (u64(u32(rx)) << 32) | u32(rz); }
u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

struct Decompressor {
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    std::vector<u8> out = std::vector<u8>(size_t(1) << 20);
    ~Decompressor() { libdeflate_free_decompressor(d); }

    // type 1 = gzip, 2 = zlib. Returns decompressed size or SIZE_MAX.
    size_t run(int type, const u8* in, size_t inSize) {
        for (;;) {
            size_t actual = 0;
            libdeflate_result r = type == 1
                                      ? libdeflate_gzip_decompress(d, in, inSize, out.data(), out.size(), &actual)
                                      : libdeflate_zlib_decompress(d, in, inSize, out.data(), out.size(), &actual);
            if (r == LIBDEFLATE_SUCCESS) return actual;
            if (r != LIBDEFLATE_INSUFFICIENT_SPACE || out.size() >= (size_t(256) << 20)) return SIZE_MAX;
            out.resize(out.size() * 2);
        }
    }
};

struct ChunkParser {
    BlockRegistry& registry;
    World::Region& region;

    struct Tmp {
        int y;
        World::Section s;
        bool hasBiomes = false;
        u8 biomes[16];  // lowest quart layer of the section, [qz * 4 + qx]
    };
    std::vector<u8> biomePalette;
    std::vector<std::pair<std::string, u8>> biomeCache;  // names seen by this parser (the global table locks)
    std::vector<Tmp> sections;
    std::vector<u16> ids;
    std::vector<std::pair<std::string_view, std::string_view>> props;
    std::string key;
    std::vector<World::BlockEntityNbt> blockEntities;  // of the last parsed chunk

    // block_entities: compounds with x, y, z, id and the entity's data, kept whole as network NBT.
    void parseBlockEntities(NbtReader& r) {
        const u8 et = r.u8v();
        const i32 n = r.i32v();
        if (et != TagCompound) {
            for (i32 i = 0; i < n && r.ok; ++i) r.skip(et);
            return;
        }
        for (i32 i = 0; i < n && r.ok; ++i) {
            const u8* start = r.p;
            World::BlockEntityNbt be;
            u8 tag;
            std::string_view name;
            while (r.field(tag, name)) {
                if (tag == TagInt && name.size() == 1 && (name[0] == 'x' || name[0] == 'y' || name[0] == 'z'))
                    (name[0] == 'x' ? be.x : name[0] == 'y' ? be.y : be.z) = r.i32v();
                else
                    r.skip(tag);
            }
            if (!r.ok) break;
            be.nbt.reserve(size_t(r.p - start) + 1);
            be.nbt.push_back(char(TagCompound));
            be.nbt.append(reinterpret_cast<const char*>(start), size_t(r.p - start));
            blockEntities.push_back(std::move(be));
        }
    }

    void parsePalette(NbtReader& r) {
        // Entries are {Name, Properties} (up to 1.21), or since 26.x a plain string, a string
        // wrapped as {"": ...} in mixed lists, or {id, properties} with default properties omitted
        // (BlockRegistry::idFor completes them).
        u8 et = r.u8v();
        i32 n = r.i32v();
        if (et != TagCompound && et != TagString) {
            for (i32 i = 0; i < n && r.ok; ++i) r.skip(et);
            return;
        }
        for (i32 i = 0; i < n && r.ok; ++i) {
            std::string_view blockName;
            props.clear();
            if (et == TagString) {
                key.assign(r.str());
                ids.push_back(registry.idFor(key));
                continue;
            }
            u8 tag;
            std::string_view name;
            while (r.field(tag, name)) {
                if (tag == TagString && (name == "Name" || name == "id" || name.empty())) {
                    blockName = r.str();
                } else if (tag == TagCompound && (name == "Properties" || name == "properties")) {
                    u8 t;
                    std::string_view k;
                    while (r.field(t, k)) {
                        if (t == TagString)
                            props.push_back({k, r.str()});
                        else
                            r.skip(t);
                    }
                } else {
                    r.skip(tag);
                }
            }
            std::sort(props.begin(), props.end());
            key.assign(blockName);
            if (!props.empty()) {
                key += '[';
                for (size_t p = 0; p < props.size(); ++p) {
                    if (p) key += ',';
                    key += props[p].first;
                    key += '=';
                    key += props[p].second;
                }
                key += ']';
            }
            ids.push_back(registry.idFor(key));
        }
    }

    void parseSections(NbtReader& r) {
        u8 et = r.u8v();
        i32 n = r.i32v();
        if (et != TagCompound) {
            for (i32 i = 0; i < n && r.ok; ++i) r.skip(et);
            return;
        }
        for (i32 i = 0; i < n && r.ok; ++i) {
            int y = INT_MIN;
            const u8* data = nullptr;
            const u8* lightData[2] = {nullptr, nullptr};  // sky, block
            i32 dataLen = 0;
            ids.clear();
            biomePalette.clear();
            const u8* biomeData = nullptr;
            i32 biomeDataLen = 0;
            u8 tag;
            std::string_view name;
            while (r.field(tag, name)) {
                if (name == "Y" && tag == TagByte) {
                    y = i8(r.u8v());
                } else if (name == "Y" && tag == TagInt) {
                    y = r.i32v();
                } else if (tag == TagByteArray && (name == "SkyLight" || name == "BlockLight")) {
                    i32 len = r.i32v();
                    if (len == 2048 && r.need(2048)) lightData[name == "BlockLight"] = r.p;
                    r.advance(size_t(std::max(len, 0)));
                } else if (name == "biomes" && tag == TagCompound) {
                    u8 t2;
                    std::string_view n2;
                    while (r.field(t2, n2)) {
                        if (t2 == TagList && n2 == "palette") {
                            const u8 et = r.u8v();
                            const i32 n = r.i32v();
                            for (i32 k = 0; k < n && r.ok; ++k) {
                                if (et == TagString) {
                                    const std::string_view name = r.str();
                                    auto it = std::find_if(biomeCache.begin(), biomeCache.end(),
                                                           [&](const auto& e) { return e.first == name; });
                                    if (it == biomeCache.end())
                                        it = biomeCache.insert(biomeCache.end(), {std::string(name), World::biomeId(name)});
                                    biomePalette.push_back(it->second);
                                } else
                                    r.skip(et);
                            }
                        } else if (t2 == TagLongArray && n2 == "data") {
                            biomeDataLen = r.i32v();
                            if (biomeDataLen < 0 || !r.need(size_t(biomeDataLen) * 8)) break;
                            biomeData = r.p;
                            r.p += size_t(biomeDataLen) * 8;
                        } else {
                            r.skip(t2);
                        }
                    }
                } else if (name == "block_states" && tag == TagCompound) {
                    u8 t2;
                    std::string_view n2;
                    while (r.field(t2, n2)) {
                        if (t2 == TagList && n2 == "palette") {
                            parsePalette(r);
                        } else if (t2 == TagLongArray && n2 == "data") {
                            dataLen = r.i32v();
                            if (dataLen < 0 || !r.need(size_t(dataLen) * 8)) break;
                            data = r.p;
                            r.p += size_t(dataLen) * 8;
                        } else {
                            r.skip(t2);
                        }
                    }
                } else {
                    r.skip(tag);
                }
            }
            if (!r.ok || y == INT_MIN || y < -128 || y > 127) continue;
            Tmp t{y, {}};
            if (!biomePalette.empty()) {  // 4x4x4 quarts, index (qy * 4 + qz) * 4 + qx; keep the lowest layer
                t.hasBiomes = true;
                u32 bits = 0;
                while ((size_t(1) << bits) < biomePalette.size()) ++bits;
                for (int q = 0; q < 16; ++q) {
                    u32 p = 0;
                    if (bits && biomeData) {
                        const u32 perWord = 64 / bits;
                        if (i32(q / perWord) < biomeDataLen) {
                            u64 v;
                            std::memcpy(&v, biomeData + size_t(q / perWord) * 8, 8);
                            p = u32((_byteswap_uint64(v) >> ((q % perWord) * bits)) & ((u64(1) << bits) - 1));
                        }
                    }
                    t.biomes[q] = p < biomePalette.size() ? biomePalette[p] : World::kUnknownBiome;
                }
            }
            World::Section& s = t.s;
            for (int k = 0; k < 2; ++k) {
                const u8* l = lightData[k];
                if (!l) continue;
                u32& ref = k ? s.block : s.sky;
                if (std::all_of(l, l + 2048, [&](u8 b) { return b == l[0]; }) && (l[0] >> 4) == (l[0] & 15)) {
                    ref = World::kLightUniform | (l[0] & 15);
                } else {
                    ref = u32(region.lightArrays.size());
                    std::memcpy(region.lightArrays.emplace_back().data(), l, 2048);
                }
            }
            const bool hasBlocks = std::any_of(ids.begin(), ids.end(), [](u16 id) { return id != 0; });
            if (!hasBlocks) {
                if (s.sky != World::kLightAbsent || s.block != World::kLightAbsent) sections.push_back(t);
                continue;
            }

            s.paletteOffset = u32(region.palettes.size());
            s.paletteSize = u16(ids.size());
            if (ids.size() > 1 && data && dataLen > 0) {
                u32 perWord = (4096 + u32(dataLen) - 1) / u32(dataLen);
                u32 bits = perWord ? 64 / perWord : 0;
                if (bits == 0 || bits > 16 || (4096 + perWord - 1) / perWord != u32(dataLen)) continue;
                s.bits = u8(bits);
                s.dataOffset = u32(region.words.size());
                for (i32 k = 0; k < dataLen; ++k) {
                    u64 v;
                    std::memcpy(&v, data + size_t(k) * 8, 8);
                    region.words.push_back(_byteswap_uint64(v));
                }
            }
            region.palettes.insert(region.palettes.end(), ids.begin(), ids.end());
            sections.push_back(t);
        }
    }

    // Returns true and fills `column` if the chunk is fully generated and has blocks.
    bool parse(const u8* data, size_t size, World::Column& column, u8* biomes) {
        NbtReader r(data, size);
        sections.clear();
        blockEntities.clear();
        size_t palMark = region.palettes.size(), wordMark = region.words.size(), lightMark = region.lightArrays.size();
        std::string_view status;
        bool lightOn = true;
        if (r.u8v() != TagCompound) return false;
        r.str();
        u8 tag;
        std::string_view name;
        while (r.field(tag, name)) {
            if (tag == TagString && name == "Status")
                status = r.str();
            else if (tag == TagList && name == "sections")
                parseSections(r);
            else if (tag == TagByte && name == "isLightOn")
                lightOn = r.u8v() != 0;
            else if (tag == TagList && name == "block_entities")
                parseBlockEntities(r);
            else
                r.skip(tag);
        }
        bool full = status.empty() || status.ends_with("full") || status.ends_with("terrain");
        if (!r.ok || !full || sections.empty()) {
            region.palettes.resize(palMark);
            region.words.resize(wordMark);
            region.lightArrays.resize(lightMark);
            return false;
        }
        if (!lightOn)  // light was never computed for this chunk: treat as unlit data (full sky)
            for (Tmp& t : sections) t.s.sky = t.s.block = World::kLightAbsent;
        int minY = INT_MAX, maxY = INT_MIN;
        for (const Tmp& t : sections) minY = std::min(minY, t.y), maxY = std::max(maxY, t.y);
        column.first = u32(region.sections.size());
        column.minY = i8(minY);
        column.count = u8(maxY - minY + 1);
        region.sections.resize(region.sections.size() + column.count);
        for (const Tmp& t : sections) region.sections[column.first + (t.y - minY)] = t.s;
        // Surface biome: the highest section with blocks (biome colours only need the surface).
        const Tmp* top = nullptr;
        for (const Tmp& t : sections)
            if (t.hasBiomes && t.s.paletteSize && (!top || t.y > top->y)) top = &t;
        if (top) std::copy(top->biomes, top->biomes + 16, biomes);
        return true;
    }
};

}  // namespace

namespace {
std::mutex gBiomeMutex;
std::deque<std::string> gBiomeNames;  // deque: names never move (biomeNameView)
std::unordered_map<std::string, u8> gBiomeIds;
}  // namespace

u8 World::biomeId(std::string_view name) {
    std::lock_guard lock(gBiomeMutex);
    std::string key(name);
    if (key.find(':') == std::string::npos) key = "minecraft:" + key;
    if (auto it = gBiomeIds.find(key); it != gBiomeIds.end()) return it->second;
    if (gBiomeNames.size() >= kUnknownBiome) return kUnknownBiome;
    const u8 id = u8(gBiomeNames.size());
    gBiomeNames.push_back(key);
    gBiomeIds.emplace(std::move(key), id);
    return id;
}

std::string World::biomeName(u8 id) {
    std::lock_guard lock(gBiomeMutex);
    return id < gBiomeNames.size() ? gBiomeNames[id] : std::string();
}

std::string_view World::biomeNameView(u8 id) {
    std::lock_guard lock(gBiomeMutex);
    return id < gBiomeNames.size() ? std::string_view(gBiomeNames[id]) : std::string_view();
}

u8 World::biomeAt(int x, int z) const {
    const int cx = x >> 4, cz = z >> 4;
    auto it = lookup_.find(regionKey(cx >> 5, cz >> 5));
    if (it == lookup_.end()) return kUnknownBiome;
    return it->second->biomes[size_t(((cz & 31) << 5) | (cx & 31)) * 16 + ((z >> 2) & 3) * 4 + ((x >> 2) & 3)];
}

void World::setChunkBiomes(int cx, int cz, const u8* quarts) {
    if (Region* r = regionFor(cx, cz)) std::copy(quarts, quarts + 16, &r->biomes[size_t(((cz & 31) << 5) | (cx & 31)) * 16]);
}

void World::decode(const Region& r, const Section& s, u16* out) {
    const u16* pal = &r.palettes[s.paletteOffset];
    if (s.bits == 0) {
        std::fill(out, out + 4096, pal[0]);
        return;
    }
    const u32 bits = s.bits, perWord = 64 / bits;
    const u64 mask = (u64(1) << bits) - 1;
    const u64* w = &r.words[s.dataOffset];
    u32 idx = 0;
    for (u32 wi = 0; idx < 4096; ++wi) {
        u64 v = w[wi];
        for (u32 k = 0; k < perWord && idx < 4096; ++k, v >>= bits) {
            u32 p = u32(v & mask);
            out[idx++] = p < s.paletteSize ? pal[p] : 0;
        }
    }
}

const World::Column* World::column(int cx, int cz, const Region** outRegion) const {
    auto it = lookup_.find(regionKey(cx >> 5, cz >> 5));
    if (it == lookup_.end()) return nullptr;
    if (outRegion) *outRegion = it->second;
    return &it->second->columns[((cz & 31) << 5) | (cx & 31)];
}

World::Region* World::regionFor(int cx, int cz) {
    auto it = lookup_.find(regionKey(cx >> 5, cz >> 5));
    return it == lookup_.end() ? nullptr : it->second;
}

World::Section& World::ensureSection(Region& r, Column& c, int sy) {
    if (c.count && sy >= c.minY && sy < c.minY + c.count) return r.sections[c.first + (sy - c.minY)];
    const int lo = c.count ? std::min<int>(c.minY, sy) : sy;
    const int hi = c.count ? std::max<int>(c.minY + c.count - 1, sy) : sy;
    const u32 first = u32(r.sections.size());
    r.sections.resize(first + (hi - lo + 1));
    for (int k = 0; k < c.count; ++k) r.sections[first + (c.minY + k - lo)] = r.sections[c.first + k];
    c.first = first, c.minY = i8(lo), c.count = u8(hi - lo + 1);
    return r.sections[first + (sy - lo)];
}

u16 World::block(int x, int y, int z) const {
    const Region* r = nullptr;
    const Section* s = section(x >> 4, y >> 4, z >> 4, &r);
    return s ? blockAt(*r, *s, u32(((y & 15) << 8) | ((z & 15) << 4) | (x & 15))) : 0;
}

bool World::setBlock(int x, int y, int z, u16 id) {
    const int cx = x >> 4, sy = y >> 4, cz = z >> 4;
    if (sy < minSy_ || sy > maxSy_) return false;
    Region* r = regionFor(cx, cz);
    if (!r) return false;
    Section& s = ensureSection(*r, r->columns[((cz & 31) << 5) | (cx & 31)], sy);
    const u32 index = u32(((y & 15) << 8) | ((z & 15) << 4) | (x & 15));
    dirtyChunks_.insert(chunkKey(cx, cz));
    noteBlockChange(cx, cz);

    if (s.paletteSize == 0) {
        if (id == 0) return true;
        s.paletteOffset = u32(r->palettes.size());
        r->palettes.push_back(0);
        s.paletteSize = 1;
        s.bits = 0;
    }
    u32 pi = 0;
    while (pi < s.paletteSize && r->palettes[s.paletteOffset + pi] != id) ++pi;
    if (pi == s.paletteSize) {
        // Grow the palette (relocated to the end of the pool) and widen indices if needed.
        const u32 newSize = s.paletteSize + 1u;
        u32 bits = 4;
        while ((1u << bits) < newSize) ++bits;
        const u32 newPalette = u32(r->palettes.size());
        for (u32 i = 0; i < s.paletteSize; ++i) r->palettes.push_back(r->palettes[s.paletteOffset + i]);
        r->palettes.push_back(id);
        if (bits != s.bits) {
            u16 old[4096];
            if (s.bits == 0)
                std::fill(old, old + 4096, u16(0));
            else
                for (u32 i = 0; i < 4096; ++i) {
                    const u32 perWord = 64 / s.bits;
                    old[i] = u16((r->words[s.dataOffset + i / perWord] >> ((i % perWord) * s.bits)) & ((1u << s.bits) - 1));
                }
            const u32 perWord = 64 / bits;
            const u32 newData = u32(r->words.size());
            r->words.resize(newData + (4096 + perWord - 1) / perWord, 0);
            for (u32 i = 0; i < 4096; ++i) r->words[newData + i / perWord] |= u64(old[i]) << ((i % perWord) * bits);
            s.dataOffset = newData;
            s.bits = u8(bits);
        }
        s.paletteOffset = newPalette;
        s.paletteSize = u16(newSize);
    }
    if (s.bits == 0) return true;  // uniform section already holds this id
    const u32 perWord = 64 / s.bits, shift = (index % perWord) * s.bits;
    u64& w = r->words[s.dataOffset + index / perWord];
    w = (w & ~(((u64(1) << s.bits) - 1) << shift)) | (u64(pi) << shift);
    return true;
}

bool World::insertChunk(int cx, int cz, int minY, int height, const u16* blocks) {
    Region* r = regionFor(cx, cz);
    if (!r) {
        auto fresh = std::make_unique<Region>();
        fresh->rx = cx >> 5, fresh->rz = cz >> 5;
        r = fresh.get();
        lookup_[regionKey(r->rx, r->rz)] = r;
        regions_.push_back(std::move(fresh));
    }
    Column& c = r->columns[((cz & 31) << 5) | (cx & 31)];
    if (c.count) return false;  // saved or already generated

    const int lo = std::max(minSy_, minY >> 4), hi = std::min(maxSy_, (minY + height - 1) >> 4);
    c.first = u32(r->sections.size());
    c.minY = i8(lo);
    c.count = u8(hi - lo + 1);
    r->sections.resize(r->sections.size() + c.count);
    std::vector<u16> palette;
    u16 local[4096];
    for (int sy = lo; sy <= hi; ++sy) {
        palette.clear();
        bool any = false;
        for (int i = 0; i < 4096; ++i) {
            const int y = sy * 16 + (i >> 8);
            const u16 id = y >= minY && y < minY + height ? blocks[(size_t(y - minY) * 16 + ((i >> 4) & 15)) * 16 + (i & 15)] : 0;
            any |= id != 0;
            auto it = std::find(palette.begin(), palette.end(), id);
            if (it == palette.end()) {
                palette.push_back(id);
                it = palette.end() - 1;
            }
            local[i] = u16(it - palette.begin());
        }
        if (!any) continue;
        Section& s = r->sections[c.first + (sy - lo)];
        s.paletteOffset = u32(r->palettes.size());
        s.paletteSize = u16(palette.size());
        r->palettes.insert(r->palettes.end(), palette.begin(), palette.end());
        if (palette.size() > 1) {
            u32 bits = 4;
            while ((size_t(1) << bits) < palette.size()) ++bits;
            const u32 perWord = 64 / bits;
            s.bits = u8(bits);
            s.dataOffset = u32(r->words.size());
            r->words.resize(r->words.size() + (4096 + perWord - 1) / perWord, 0);
            for (u32 i = 0; i < 4096; ++i) r->words[s.dataOffset + i / perWord] |= u64(local[i]) << ((i % perWord) * bits);
        }
    }
    noteBlockChange(cx, cz);
    return true;
}

World::LightArray* World::lightArray(int cx, int sy, int cz, bool sky) {
    Region* r = regionFor(cx, cz);
    if (!r || sy < minSy_ - 1 || sy > maxSy_ + 1) return nullptr;
    const LightView view = light(cx, sy, cz);  // resolve before the column may move
    Section& s = ensureSection(*r, r->columns[((cz & 31) << 5) | (cx & 31)], sy);
    if (trackChanges_) dirtyChunks_.insert(chunkKey(cx, cz));  // callers write through the returned array
    u32& ref = sky ? s.sky : s.block;
    if (ref != kLightAbsent && !(ref & kLightUniform)) return &r->lightArrays[ref];
    LightArray& a = r->lightArrays.emplace_back();
    for (int i = 0; i < 4096; i += 2) {
        const int y = i >> 8, z = (i >> 4) & 15, x = i & 15;
        const u8 lo = sky ? view.sky4(x, y, z) : view.block4(x, y, z);
        const u8 hi = sky ? view.sky4(x + 1, y, z) : view.block4(x + 1, y, z);
        a[i >> 1] = u8(lo | (hi << 4));
    }
    ref = u32(r->lightArrays.size() - 1);
    return &a;
}

World::Region& World::regionOrCreate(int cx, int cz) {
    if (Region* r = regionFor(cx, cz)) return *r;
    auto fresh = std::make_unique<Region>();
    fresh->rx = cx >> 5, fresh->rz = cz >> 5;
    Region& r = *fresh;
    lookup_[regionKey(r.rx, r.rz)] = &r;
    regions_.push_back(std::move(fresh));
    return r;
}

// Light reference for 2048 bytes of nibbles: uniform when every nibble is equal, else a new array.
u32 World::storeLight(Region& r, u32 ref, const u8* data) {
    if (!data) return ref;
    if (std::all_of(data, data + 2048, [&](u8 b) { return b == data[0]; }) && (data[0] >> 4) == (data[0] & 15))
        return kLightUniform | (data[0] & 15);
    std::memcpy(r.lightArrays.emplace_back().data(), data, 2048);
    return u32(r.lightArrays.size() - 1);
}

void World::replaceChunk(int cx, int cz, int firstSy, const std::vector<PackedSection>& sections) {
    removeChunk(cx, cz);
    noteBlockChange(cx, cz);
    const int lo = std::max(firstSy, minSy_ - 1), hi = std::min(firstSy + int(sections.size()) - 1, maxSy_ + 1);
    if (lo > hi) return;
    Region& r = regionOrCreate(cx, cz);
    Column& c = r.columns[((cz & 31) << 5) | (cx & 31)];
    c.first = u32(r.sections.size());
    c.minY = i8(lo);
    c.count = u8(hi - lo + 1);
    r.sections.resize(r.sections.size() + c.count);
    for (int sy = lo; sy <= hi; ++sy) {
        const PackedSection& p = sections[size_t(sy - firstSy)];
        Section& s = r.sections[c.first + (sy - lo)];
        if (!p.palette.empty() && sy >= minSy_ && sy <= maxSy_) {
            s.paletteOffset = u32(r.palettes.size());
            s.paletteSize = u16(p.palette.size());
            r.palettes.insert(r.palettes.end(), p.palette.begin(), p.palette.end());
            if (p.bits) {
                s.bits = p.bits;
                s.dataOffset = u32(r.words.size());
                r.words.insert(r.words.end(), p.words.begin(), p.words.end());
            }
        }
        s.sky = storeLight(r, p.sky, p.skyData.size() == 2048 ? p.skyData.data() : nullptr);
        s.block = storeLight(r, p.block, p.blockData.size() == 2048 ? p.blockData.data() : nullptr);
    }
}

void World::setChunkBlockEntities(int cx, int cz, std::vector<BlockEntityNbt> list) {
    if (list.empty()) blockEntities_.erase(chunkKey(cx, cz));
    else blockEntities_[chunkKey(cx, cz)] = std::move(list);
    noteBlockChange(cx, cz);
}

void World::setBlockEntity(BlockEntityNbt be) {
    const int cx = be.x >> 4, cz = be.z >> 4;
    auto& list = blockEntities_[chunkKey(cx, cz)];
    auto it = std::find_if(list.begin(), list.end(), [&](const BlockEntityNbt& e) { return e.x == be.x && e.y == be.y && e.z == be.z; });
    if (it != list.end()) *it = std::move(be);
    else list.push_back(std::move(be));
    noteBlockChange(cx, cz);
}

void World::removeChunk(int cx, int cz) {
    Region* r = regionFor(cx, cz);
    if (!r) return;
    Column& c = r->columns[((cz & 31) << 5) | (cx & 31)];
    if (!c.count) return;
    c = Column{};
    noteBlockChange(cx, cz);
    // Dropped columns leave garbage in the region's pools; rebuild them once it dominates.
    size_t live = 0;
    for (const Column& col : r->columns) live += col.count;
    if (r->sections.size() > live * 2 + 256) compact(*r);
}

void World::compact(Region& r) {
    std::vector<Section> sections;
    std::vector<u16> palettes;
    std::vector<u64> words;
    std::deque<LightArray> lights;
    auto light = [&](u32 ref) {
        if (ref == kLightAbsent || (ref & kLightUniform)) return ref;
        lights.push_back(r.lightArrays[ref]);
        return u32(lights.size() - 1);
    };
    for (Column& c : r.columns) {
        const u32 first = u32(sections.size());
        for (int k = 0; k < c.count; ++k) {
            Section s = r.sections[c.first + k];
            if (s.paletteSize) {
                const u32 pal = u32(palettes.size());
                palettes.insert(palettes.end(), r.palettes.begin() + s.paletteOffset,
                                r.palettes.begin() + s.paletteOffset + s.paletteSize);
                s.paletteOffset = pal;
                if (s.bits) {
                    const u32 perWord = 64 / s.bits, n = (4096 + perWord - 1) / perWord, data = u32(words.size());
                    words.insert(words.end(), r.words.begin() + s.dataOffset, r.words.begin() + s.dataOffset + n);
                    s.dataOffset = data;
                }
            }
            s.sky = light(s.sky);
            s.block = light(s.block);
            sections.push_back(s);
        }
        c.first = first;
    }
    r.sections = std::move(sections);
    r.palettes = std::move(palettes);
    r.words = std::move(words);
    r.lightArrays = std::move(lights);
}

bool World::setLight(int cx, int sy, int cz, bool sky, u32 ref, const u8* data) {
    Region* r = regionFor(cx, cz);
    if (!r || sy < minSy_ - 1 || sy > maxSy_ + 1) return false;
    Column& c = r->columns[((cz & 31) << 5) | (cx & 31)];
    if (!c.count) return false;  // not loaded
    Section& s = ensureSection(*r, c, sy);
    u32& cur = sky ? s.sky : s.block;
    const bool curArray = cur != kLightAbsent && !(cur & kLightUniform);
    if (data && curArray) {  // overwrite in place
        LightArray& a = r->lightArrays[cur];
        if (std::memcmp(a.data(), data, 2048) == 0) return false;
        std::memcpy(a.data(), data, 2048);
        return true;
    }
    const u32 next = storeLight(*r, ref, data);
    if (next == cur) return false;
    cur = next;
    return true;
}

const World::Section* World::anySection(int cx, int sy, int cz, const Region** outRegion) const {
    const Region* r = nullptr;
    const Column* col = column(cx, cz, &r);
    if (!col) return nullptr;
    int i = sy - col->minY;
    if (i < 0 || i >= col->count) return nullptr;
    if (outRegion) *outRegion = r;
    return &r->sections[col->first + i];
}

const World::Section* World::section(int cx, int sy, int cz, const Region** outRegion) const {
    const Section* s = anySection(cx, sy, cz, outRegion);
    return s && s->paletteSize ? s : nullptr;
}

World::LightView World::light(int cx, int sy, int cz) const {
    LightView v;
    const Region* r = nullptr;
    const Column* col = column(cx, cz, &r);
    if (!col) return v;
    const int top = col->minY + col->count;
    if (sy >= col->minY && sy < top) {
        const Section& s = r->sections[col->first + (sy - col->minY)];
        v.block = s.block, v.blockRegion = r;
        if (s.sky != kLightAbsent) {
            v.sky = s.sky, v.skyRegion = r;
            return v;
        }
    }
    // No sky layer here: the game uses the bottom layer of the nearest section above that has one.
    for (int y = std::max(sy + 1, int(col->minY)); y < top; ++y) {
        const Section& s = r->sections[col->first + (y - col->minY)];
        if (s.sky != kLightAbsent) {
            v.sky = s.sky, v.skyRegion = r, v.skyBottomRow = true;
            return v;
        }
    }
    return v;  // open sky
}

bool World::load(const fs::path& saveDir, BlockRegistry& registry, int centerCx, int centerCz, int radius,
                 LoadStats& stats) {
    fs::path dir = saveDir / "region";
    if (!fs::exists(dir)) dir = saveDir / "dimensions" / "minecraft" / "overworld" / "region";

    struct File {
        fs::path path;
        int rx, rz;
    };
    std::vector<File> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".mca") continue;
        int rx, rz;
        if (std::sscanf(e.path().filename().string().c_str(), "r.%d.%d", &rx, &rz) != 2) continue;
        if (radius > 0) {
            i64 dx = std::clamp(centerCx, rx * 32, rx * 32 + 31) - centerCx;
            i64 dz = std::clamp(centerCz, rz * 32, rz * 32 + 31) - centerCz;
            if (dx * dx + dz * dz > i64(radius) * radius) continue;
        }
        files.push_back({e.path(), rx, rz});
    }
    if (files.empty()) {
        LOG("error: no region files in %s", dir.string().c_str());
        return false;
    }

    std::vector<std::unique_ptr<Region>> loaded(files.size());
    std::vector<std::vector<std::pair<u64, std::vector<BlockEntityNbt>>>> loadedBe(files.size());
    std::atomic<size_t> chunks{0}, sections{0}, unsupported{0};
    std::atomic<u64> bytes{0};
    Jobs::parallelFor(files.size(), [&](size_t i) {
        thread_local std::vector<u8> file;
        thread_local Decompressor dec;
        const File& f = files[i];
        if (!readFile(f.path, file) || file.size() < 8192) return;
        bytes += file.size();

        auto region = std::make_unique<Region>();
        region->rx = f.rx;
        region->rz = f.rz;
        ChunkParser parser{registry, *region};
        size_t localChunks = 0, localUnsupported = 0;
        for (int idx = 0; idx < 1024; ++idx) {
            int cx = f.rx * 32 + (idx & 31), cz = f.rz * 32 + (idx >> 5);
            if (radius > 0) {
                i64 dx = cx - centerCx, dz = cz - centerCz;
                if (dx * dx + dz * dz > i64(radius) * radius) continue;
            }
            u32 sector = be32(&file[size_t(idx) * 4]) >> 8;
            if (!sector) continue;
            size_t pos = size_t(sector) * 4096;
            if (pos + 5 > file.size()) continue;
            u32 len = be32(&file[pos]);
            u8 type = file[pos + 4];
            if (len < 1 || pos + 4 + len > file.size()) continue;
            const u8* payload = &file[pos + 5];
            size_t payloadLen = len - 1, size;
            const u8* data;
            if (type == 1 || type == 2) {
                size = dec.run(type, payload, payloadLen);
                if (size == SIZE_MAX) {
                    ++localUnsupported;
                    continue;
                }
                data = dec.out.data();
            } else if (type == 3) {
                data = payload;
                size = payloadLen;
            } else {
                ++localUnsupported;  // LZ4 / external chunk files: not yet supported
                continue;
            }
            if (parser.parse(data, size, region->columns[idx], &region->biomes[size_t(idx) * 16])) {
                ++localChunks;
                if (!parser.blockEntities.empty()) loadedBe[i].emplace_back(chunkKey(cx, cz), std::move(parser.blockEntities));
            }
        }
        size_t nonEmpty = 0;
        for (const Section& s : region->sections) nonEmpty += s.paletteSize != 0;
        chunks += localChunks;
        sections += nonEmpty;
        unsupported += localUnsupported;
        if (localChunks) {
            region->sections.shrink_to_fit();
            region->palettes.shrink_to_fit();
            region->words.shrink_to_fit();
            region->lightArrays.shrink_to_fit();
            loaded[i] = std::move(region);
        }
    });

    regions_.clear();
    lookup_.clear();
    blockEntities_.clear();
    for (auto& list : loadedBe)
        for (auto& [key, bes] : list) blockEntities_[key] = std::move(bes);
    minSy_ = INT_MAX, maxSy_ = INT_MIN;
    for (auto& r : loaded)
        if (r) {
            for (const Column& c : r->columns)
                for (int k = 0; k < c.count; ++k)
                    if (r->sections[c.first + k].paletteSize)
                        minSy_ = std::min(minSy_, c.minY + k), maxSy_ = std::max(maxSy_, c.minY + k);
            lookup_[regionKey(r->rx, r->rz)] = r.get();
            regions_.push_back(std::move(r));
        }
    if (minSy_ > maxSy_) minSy_ = maxSy_ = 0;
    minSy_ = std::min(minSy_, -4), maxSy_ = std::max(maxSy_, 19);  // at least the overworld build range
    stats.regions = regions_.size();
    stats.chunks = chunks;
    stats.sections = sections;
    stats.unsupportedChunks = unsupported;
    stats.compressedBytes = bytes;
    return !regions_.empty();
}

bool World::readSeed(const fs::path& saveDir, i64& seed) {
    auto load = [](const fs::path& file, NbtValue& root) {
        std::vector<u8> gz;
        if (!readFile(file, gz) || gz.size() < 18) return false;
        const u32 isize = u32(gz[gz.size() - 4]) | (u32(gz[gz.size() - 3]) << 8) | (u32(gz[gz.size() - 2]) << 16) |
                          (u32(gz[gz.size() - 1]) << 24);
        std::vector<u8> raw(std::max<u32>(isize, 1));
        libdeflate_decompressor* d = libdeflate_alloc_decompressor();
        size_t actual = 0;
        const bool ok = libdeflate_gzip_decompress(d, gz.data(), gz.size(), raw.data(), raw.size(), &actual) ==
                        LIBDEFLATE_SUCCESS;
        libdeflate_free_decompressor(d);
        return ok && nbtRead(raw.data(), actual, root);
    };
    NbtValue root;
    if (load(saveDir / "data" / "minecraft" / "world_gen_settings.dat", root))
        if (NbtValue* data = root.find("data"))
            if (NbtValue* s = data->find("seed")) {
                seed = s->i;
                return true;
            }
    if (load(saveDir / "level.dat", root))
        if (NbtValue* data = root.find("Data"))
            if (NbtValue* settings = data->find("WorldGenSettings"))
                if (NbtValue* s = settings->find("seed")) {
                    seed = s->i;
                    return true;
                }
    return false;
}

PlayerStart World::readPlayer(const fs::path& saveDir) {
    PlayerStart ps;
    auto gunzip = [](const fs::path& path, std::vector<u8>& raw) {
        std::vector<u8> gz;
        if (!readFile(path, gz) || gz.size() < 18) return false;
        u32 isize = u32(gz[gz.size() - 4]) | (u32(gz[gz.size() - 3]) << 8) | (u32(gz[gz.size() - 2]) << 16) |
                    (u32(gz[gz.size() - 1]) << 24);
        raw.resize(std::max<u32>(isize, 1));
        libdeflate_decompressor* d = libdeflate_alloc_decompressor();
        size_t actual = 0;
        libdeflate_result res = libdeflate_gzip_decompress(d, gz.data(), gz.size(), raw.data(), raw.size(), &actual);
        libdeflate_free_decompressor(d);
        raw.resize(actual);
        return res == LIBDEFLATE_SUCCESS;
    };
    std::vector<u8> raw;
    if (!gunzip(saveDir / "level.dat", raw)) return ps;

    NbtReader r(raw.data(), raw.size());
    if (r.u8v() != TagCompound) return ps;
    r.str();
    bool haveSpawn = false;
    int spawn[3] = {0, 100, 0};
    std::vector<i32> owner;  // singleplayer_uuid: 26.x keeps the owner in players/data/<uuid>.dat
    u8 t;
    std::string_view n;
    while (r.field(t, n)) {
        if (t != TagCompound || n != "Data") {
            r.skip(t);
            continue;
        }
        while (r.field(t, n)) {
            if (t == TagCompound && n == "Player") {
                while (r.field(t, n)) {
                    if (t == TagList && n == "Pos") {
                        u8 et = r.u8v();
                        i32 count = r.i32v();
                        if (et == TagDouble && count == 3) {
                            ps.x = r.f64v(), ps.y = r.f64v(), ps.z = r.f64v();
                            ps.found = true;
                        } else {
                            for (i32 i = 0; i < count && r.ok; ++i) r.skip(et);
                        }
                    } else if (t == TagList && n == "Rotation") {
                        u8 et = r.u8v();
                        i32 count = r.i32v();
                        if (et == TagFloat && count == 2) {
                            ps.yaw = r.f32v(), ps.pitch = r.f32v();
                        } else {
                            for (i32 i = 0; i < count && r.ok; ++i) r.skip(et);
                        }
                    } else {
                        r.skip(t);
                    }
                }
            } else if (t == TagIntArray && n == "singleplayer_uuid") {
                const i32 count = r.i32v();
                for (i32 i = 0; i < count && r.ok; ++i) owner.push_back(r.i32v());
            } else if (t == TagInt && (n == "SpawnX" || n == "SpawnY" || n == "SpawnZ")) {
                spawn[n.back() - 'X'] = r.i32v();
                haveSpawn = true;
            } else if (t == TagCompound && n == "spawn") {
                while (r.field(t, n)) {
                    if (t == TagIntArray && n == "pos") {
                        i32 count = r.i32v();
                        for (i32 i = 0; i < count && r.ok; ++i) {
                            i32 v = r.i32v();
                            if (i < 3) spawn[i] = v;
                        }
                        haveSpawn = count >= 3;
                    } else {
                        r.skip(t);
                    }
                }
            } else {
                r.skip(t);
            }
        }
    }
    // Where the owner left off, so the world shows there before the server places the player (only in the
    // overworld: the view starts there).
    if (!ps.found && owner.size() == 4) {
        char id[40];
        std::snprintf(id, sizeof(id), "%08x-%04x-%04x-%04x-%04x%08x", u32(owner[0]), u32(owner[1]) >> 16,
                      u32(owner[1]) & 0xFFFF, u32(owner[2]) >> 16, u32(owner[2]) & 0xFFFF, u32(owner[3]));
        NbtValue player;
        if (gunzip(saveDir / "players" / "data" / (std::string(id) + ".dat"), raw) &&
            nbtRead(raw.data(), raw.size(), player)) {
            const NbtValue* pos = player.find("Pos");
            const NbtValue* rot = player.find("Rotation");
            const NbtValue* dim = player.find("Dimension");
            if (pos && pos->items.size() == 3 && (!dim || dim->s == "minecraft:overworld")) {
                ps.x = pos->items[0].d, ps.y = pos->items[1].d, ps.z = pos->items[2].d;
                if (rot && rot->items.size() == 2) ps.yaw = float(rot->items[0].d), ps.pitch = float(rot->items[1].d);
                ps.found = true;
            }
        }
    }
    if (!ps.found && haveSpawn) {
        ps.x = spawn[0] + 0.5, ps.y = spawn[1] + 1.7, ps.z = spawn[2] + 0.5;
        ps.found = true;
    }
    return ps;
}
