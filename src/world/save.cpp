#include "world/save.h"

#include "assets/assets.h"
#include "world/nbt_dom.h"
#include "world/world.h"

#include <libdeflate.h>

#include <algorithm>
#include <ctime>
#include <map>
#include <span>
#include <unordered_map>

namespace {

u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }
void putBe32(u8* p, u32 v) {
    p[0] = u8(v >> 24), p[1] = u8(v >> 16), p[2] = u8(v >> 8), p[3] = u8(v);
}

NbtValue scalar(u8 type, i64 v) {
    NbtValue t = NbtValue::make(type);
    t.i = v;
    return t;
}
NbtValue string(std::string_view s) {
    NbtValue t = NbtValue::make(TagString);
    t.s = std::string(s);
    return t;
}

// "minecraft:oak_stairs[facing=north,half=bottom]" -> {Name, Properties} (up to 1.21), or the
// 26.x form: {id, properties}, with property-less states as a string wrapped in {"": ...}.
NbtValue paletteEntry(std::string_view key, bool modern) {
    NbtValue e = NbtValue::make(TagCompound);
    const size_t b = key.find('[');
    if (modern && b == std::string_view::npos) {
        e.set("", string(key));
        return e;
    }
    e.set(modern ? "id" : "Name", string(key.substr(0, b)));
    if (b != std::string_view::npos) {
        NbtValue props = NbtValue::make(TagCompound);
        std::string_view rest = key.substr(b + 1, key.size() - b - 2);
        while (!rest.empty()) {
            const size_t comma = rest.find(',');
            const std::string_view kv = rest.substr(0, comma);
            const size_t eq = kv.find('=');
            if (eq != std::string_view::npos) props.set(kv.substr(0, eq), string(kv.substr(eq + 1)));
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
        e.set(modern ? "properties" : "Properties", std::move(props));
    }
    return e;
}

// Re-encodes a section compactly: only ids still in use, minimal index width (>= 4 bits).
NbtValue blockStates(const World::Region& r, const World::Section& s, const BlockRegistry& registry, bool modern) {
    u16 ids[4096] = {};
    if (s.paletteSize) World::decode(r, s, ids);
    std::vector<u16> used;
    std::unordered_map<u16, u16> index;
    u16 local[4096];
    for (int i = 0; i < 4096; ++i) {
        auto [it, inserted] = index.try_emplace(ids[i], u16(used.size()));
        if (inserted) used.push_back(ids[i]);
        local[i] = it->second;
    }
    NbtValue states = NbtValue::make(TagCompound);
    NbtValue palette = NbtValue::make(TagList);
    palette.listType = TagCompound;
    for (u16 id : used) palette.items.push_back(paletteEntry(registry.name(id), modern));
    states.set("palette", std::move(palette));
    if (used.size() > 1) {
        u32 bits = 4;
        while ((size_t(1) << bits) < used.size()) ++bits;
        const u32 perWord = 64 / bits;
        NbtValue data = NbtValue::make(TagLongArray);
        data.longs.assign((4096 + perWord - 1) / perWord, 0);
        for (u32 i = 0; i < 4096; ++i)
            data.longs[i / perWord] = i64(u64(data.longs[i / perWord]) | (u64(local[i]) << ((i % perWord) * bits)));
        states.set("data", std::move(data));
    }
    return states;
}

NbtValue lightTag(const World::Region& r, u32 ref) {
    NbtValue t = NbtValue::make(TagByteArray);
    if (ref & World::kLightUniform) {
        const u8 v = u8(ref & 15);
        t.bytes.assign(2048, u8(v | (v << 4)));
    } else {
        t.bytes.assign(r.lightArrays[ref].begin(), r.lightArrays[ref].end());
    }
    return t;
}

bool writeFileAtomically(const fs::path& path, const std::vector<u8>& data) {
    fs::path tmp = path;
    tmp += L".mcpp-tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, data.data(), DWORD(data.size()), &written, nullptr) && written == data.size() &&
                    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) return false;
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::vector<u8> inflate(const u8* in, size_t size, int type) {
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    std::vector<u8> out(size * 4 + 4096);
    for (;;) {
        size_t actual = 0;
        libdeflate_result r = type == 1 ? libdeflate_gzip_decompress(d, in, size, out.data(), out.size(), &actual)
                                        : libdeflate_zlib_decompress(d, in, size, out.data(), out.size(), &actual);
        if (r == LIBDEFLATE_SUCCESS) {
            out.resize(actual);
            break;
        }
        if (r != LIBDEFLATE_INSUFFICIENT_SPACE || out.size() > (size_t(512) << 20)) {
            out.clear();
            break;
        }
        out.resize(out.size() * 2);
    }
    libdeflate_free_decompressor(d);
    return out;
}

}  // namespace

WorldSaver::WorldSaver(World& world, const BlockRegistry& registry, const fs::path& saveDir)
    : world_(world), registry_(registry), dir_(saveDir) {
    regionDir_ = dir_ / "region";
    if (!fs::exists(regionDir_)) regionDir_ = dir_ / "dimensions" / "minecraft" / "overworld" / "region";
    backupDir_ = dir_ / "minecraftCplusplus-backup";
}

WorldSaver::~WorldSaver() {
    if (lock_ != INVALID_HANDLE_VALUE) CloseHandle(lock_);
}

bool WorldSaver::lock() {
    const fs::path path = dir_ / "session.lock";
    lock_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock_ == INVALID_HANDLE_VALUE) return false;
    OVERLAPPED ov{};
    if (!LockFileEx(lock_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &ov)) {
        CloseHandle(lock_);
        lock_ = INVALID_HANDLE_VALUE;
        return false;
    }
    return true;
}

bool WorldSaver::saveRegion(int rx, int rz, const std::vector<int>& chunks) {
    const fs::path file = regionDir_ / ("r." + std::to_string(rx) + "." + std::to_string(rz) + ".mca");
    std::vector<u8> old;
    std::error_code ec;
    const bool existed = fs::exists(file, ec);
    if (!existed) {
        if (!biomeName_) return false;
        fs::create_directories(regionDir_, ec);
        old.assign(8192, 0);  // a brand new region holding only generated chunks
    } else if (!readFile(file, old) || old.size() < 8192) {
        return false;
    }
    const fs::path backup = backupDir_ / file.filename();
    if (existed && !fs::exists(backup, ec)) {
        fs::create_directories(backupDir_, ec);
        if (!fs::copy_file(file, backup, ec)) {
            LOG("error: could not back up %s; not saving it", file.string().c_str());
            return false;
        }
    }

    // Raw stored payloads ([length][compression][data]) for every chunk present.
    // Unchanged chunks point into the file buffer. Only edited chunks need owned payloads;
    // copying every stored chunk first used a second region-sized buffer on every save.
    std::vector<std::span<const u8>> payload(1024);
    std::vector<std::vector<u8>> changed(1024);
    for (int idx = 0; idx < 1024; ++idx) {
        const u32 sector = be32(&old[size_t(idx) * 4]) >> 8;
        if (!sector) continue;
        const size_t pos = size_t(sector) * 4096;
        if (pos + 5 > old.size()) continue;
        const u32 len = be32(&old[pos]);
        if (len < 1 || pos + 4 + len > old.size()) continue;
        payload[idx] = std::span<const u8>(old.data() + pos, size_t(4) + len);
    }

    libdeflate_compressor* comp = libdeflate_alloc_compressor(6);
    bool ok = true;
    for (int idx : chunks) {
        const std::span<const u8> p = payload[idx];
        const int cx = rx * 32 + (idx & 31), cz = rz * 32 + (idx >> 5);
        NbtValue root;
        std::string rootName;
        if (p.size() < 5) {
            // Generated this session: write a chunk at the "terrain" status, which the game
            // finishes (features, light) when it loads it, keeping our edits.
            if (!biomeName_) {
                ok = false;
                continue;
            }
            root = newChunk(cx, cz);
        } else {
            if (p[4] & 0x80) {  // external storage: left alone, keep the edit dirty for retry
                ok = false;
                continue;
            }
            const int type = p[4];
            std::vector<u8> raw =
                type == 3 ? std::vector<u8>(p.begin() + 5, p.end()) : inflate(&p[5], p.size() - 5, type);
            if (raw.empty() || !nbtRead(raw.data(), raw.size(), root, &rootName)) {
                ok = false;
                continue;
            }
        }

        const World::Region* r = nullptr;
        const World::Column* col = world_.column(cx, cz, &r);
        if (!col) continue;
        NbtValue* sections = root.find("sections");
        if (!sections) {
            NbtValue list = NbtValue::make(TagList);
            list.listType = TagCompound;
            sections = &root.set("sections", std::move(list));
        }
        // Keep the palette format the chunk was written in (new chunks use the current one).
        bool modern = p.size() < 5;
        for (auto& sec : sections->items)
            if (NbtValue* bs = sec.find("block_states"))
                if (NbtValue* pal = bs->find("palette"); pal && !pal->items.empty())
                    modern = pal->items[0].type == TagString || !pal->items[0].find("Name");
        auto findY = [&](int y) -> NbtValue* {
            for (auto& sec : sections->items)
                if (NbtValue* Y = sec.find("Y"); Y && Y->i == y) return &sec;
            return nullptr;
        };
        for (int k = 0; k < col->count; ++k) {
            const int sy = col->minY + k;
            const World::Section& s = r->sections[col->first + k];
            NbtValue* sec = findY(sy);
            if (!sec) {
                if (!s.paletteSize && s.sky == World::kLightAbsent && s.block == World::kLightAbsent) continue;
                NbtValue fresh = NbtValue::make(TagCompound);
                fresh.set("Y", scalar(TagByte, sy));
                // Biomes are per section: borrow the nearest neighbour's.
                const NbtValue* nearest = nullptr;
                int best = 1 << 30;
                for (auto& other : sections->items)
                    if (NbtValue* Y = other.find("Y"); Y && other.find("biomes") && std::abs(int(Y->i) - sy) < best)
                        best = std::abs(int(Y->i) - sy), nearest = &other;
                if (nearest) fresh.set("biomes", *const_cast<NbtValue*>(nearest)->find("biomes"));
                sections->items.push_back(std::move(fresh));
                sec = &sections->items.back();
            }
            sec->set("block_states", blockStates(*r, s, registry_, modern));
            if (s.sky != World::kLightAbsent)
                sec->set("SkyLight", lightTag(*r, s.sky));
            else
                sec->erase("SkyLight");
            if (s.block != World::kLightAbsent)
                sec->set("BlockLight", lightTag(*r, s.block));
            else
                sec->erase("BlockLight");
        }
        root.erase("Heightmaps");  // the game rebuilds missing heightmaps on load

        std::vector<u8> encoded;
        nbtWrite(root, encoded, rootName);
        std::vector<u8> packed(libdeflate_zlib_compress_bound(comp, encoded.size()));
        const size_t n = libdeflate_zlib_compress(comp, encoded.data(), encoded.size(), packed.data(), packed.size());
        if (!n) {
            ok = false;
            continue;
        }
        auto& replacement = changed[idx];
        replacement.resize(n + 5);
        putBe32(replacement.data(), u32(n + 1));
        replacement[4] = 2;  // zlib
        std::memcpy(replacement.data() + 5, packed.data(), n);
        payload[idx] = replacement;
    }
    libdeflate_free_compressor(comp);

    // Re-pack every chunk into fresh sectors behind the two header sectors.
    std::vector<u8> out(8192, 0);
    size_t outputSize = 8192;
    for (const auto p : payload) outputSize += ((p.size() + 4095) / 4096) * 4096;
    out.reserve(outputSize);
    std::memcpy(&out[4096], &old[4096], 4096);  // timestamps
    const u32 now = u32(std::time(nullptr));
    for (int idx = 0; idx < 1024; ++idx) {
        const std::span<const u8> p = payload[idx];
        if (p.empty()) continue;
        const size_t sectors = (p.size() + 4095) / 4096;
        if (sectors > 255) {  // would need the game's external .mcc storage
            LOG("error: chunk too large to store in region file");
            return false;
        }
        const u32 start = u32(out.size() / 4096);
        out.insert(out.end(), p.begin(), p.end());
        out.resize(out.size() + (sectors * 4096 - p.size()), 0);
        putBe32(&out[size_t(idx) * 4], (start << 8) | u32(sectors));
    }
    for (int idx : chunks) putBe32(&out[4096 + size_t(idx) * 4], now);
    return writeFileAtomically(file, out) && ok;
}

bool WorldSaver::savePlayer(const SavedPlayer& pl) {
    const fs::path file = dir_ / "level.dat";
    std::vector<u8> gz;
    if (!readFile(file, gz) || gz.size() < 18) return false;
    std::vector<u8> raw = inflate(gz.data(), gz.size(), 1);
    NbtValue root;
    std::string rootName;
    if (raw.empty() || !nbtRead(raw.data(), raw.size(), root, &rootName)) return false;
    NbtValue* data = root.find("Data");
    NbtValue* player = data ? data->find("Player") : nullptr;
    if (!player) return true;  // no single-player entry (e.g. a server world): nothing to update
    NbtValue pos = NbtValue::make(TagList);
    pos.listType = TagDouble;
    for (double v : {pl.x, pl.y, pl.z}) {
        NbtValue d = NbtValue::make(TagDouble);
        d.d = v;
        pos.items.push_back(d);
    }
    player->set("Pos", std::move(pos));
    NbtValue rot = NbtValue::make(TagList);
    rot.listType = TagFloat;
    for (float v : {pl.yawDeg, pl.pitchDeg}) {
        NbtValue f = NbtValue::make(TagFloat);
        f.d = v;
        rot.items.push_back(f);
    }
    player->set("Rotation", std::move(rot));

    std::vector<u8> encoded;
    nbtWrite(root, encoded, rootName);
    libdeflate_compressor* comp = libdeflate_alloc_compressor(6);
    std::vector<u8> packed(libdeflate_gzip_compress_bound(comp, encoded.size()));
    const size_t n = libdeflate_gzip_compress(comp, encoded.data(), encoded.size(), packed.data(), packed.size());
    libdeflate_free_compressor(comp);
    if (!n) return false;
    packed.resize(n);
    std::error_code ec;
    fs::copy_file(file, dir_ / "level.dat_old", fs::copy_options::overwrite_existing, ec);
    return writeFileAtomically(file, packed);
}

// A chunk generated by this engine, as a proto chunk past its features (its blocks include them): biomes per section
// and what generation left (World::genExtras); the blocks are filled in by the normal section update. The game lights
// it, spawns its mobs and makes it a full chunk when it loads it. Structure starts are not written (the structures'
// blocks are), so the game knows of no structure there.
NbtValue WorldSaver::newChunk(int cx, int cz) {
    NbtValue root = NbtValue::make(TagCompound);
    root.set("DataVersion", scalar(TagInt, dataVersion_));
    root.set("xPos", scalar(TagInt, cx));
    root.set("zPos", scalar(TagInt, cz));
    root.set("yPos", scalar(TagInt, world_.minSectionY()));
    root.set("Status", string("minecraft:features"));
    root.set("LastUpdate", scalar(TagLong, 0));
    root.set("InhabitedTime", scalar(TagLong, 0));
    root.set("isLightOn", scalar(TagByte, 0));  // lit by the game's light engine (initialize_light, light)
    NbtValue structures = NbtValue::make(TagCompound);
    structures.set("starts", NbtValue::make(TagCompound));
    structures.set("References", NbtValue::make(TagCompound));
    root.set("structures", std::move(structures));
    for (const char* list : {"block_entities", "entities", "block_ticks", "fluid_ticks"}) {
        NbtValue l = NbtValue::make(TagList);
        l.listType = TagCompound;
        root.set(list, std::move(l));
    }
    if (const NbtValue* extras = world_.genExtras(cx, cz))
        for (size_t k = 0; k < extras->names.size(); ++k) {
            // PostProcessing has one list per section of the chunk.
            if (extras->names[k] == "PostProcessing" &&
                extras->items[k].items.size() != size_t(world_.maxSectionY() - world_.minSectionY() + 1))
                continue;
            root.set(extras->names[k], extras->items[k]);
        }
    NbtValue sections = NbtValue::make(TagList);
    sections.listType = TagCompound;
    for (int sy = world_.minSectionY(); sy <= world_.maxSectionY(); ++sy) {
        NbtValue sec = NbtValue::make(TagCompound);
        sec.set("Y", scalar(TagByte, sy));
        std::vector<std::string> palette;
        int index[64];
        for (int i = 0; i < 64; ++i) {
            const std::string b = biomeName_(cx * 4 + (i & 3), sy * 4 + (i >> 4), cz * 4 + ((i >> 2) & 3));
            auto it = std::find(palette.begin(), palette.end(), b);
            if (it == palette.end()) it = palette.insert(palette.end(), b);
            index[i] = int(it - palette.begin());
        }
        NbtValue biomes = NbtValue::make(TagCompound);
        NbtValue pal = NbtValue::make(TagList);
        pal.listType = TagString;
        for (const std::string& b : palette) pal.items.push_back(string(b));
        biomes.set("palette", std::move(pal));
        if (palette.size() > 1) {
            int bits = 0;
            while ((size_t(1) << bits) < palette.size()) ++bits;
            const int perWord = 64 / bits;
            NbtValue data = NbtValue::make(TagLongArray);
            data.longs.assign(size_t((64 + perWord - 1) / perWord), 0);
            for (int i = 0; i < 64; ++i)
                data.longs[size_t(i / perWord)] =
                    i64(u64(data.longs[size_t(i / perWord)]) | (u64(index[i]) << ((i % perWord) * bits)));
            biomes.set("data", std::move(data));
        }
        sec.set("biomes", std::move(biomes));
        sections.items.push_back(std::move(sec));
    }
    root.set("sections", std::move(sections));
    return root;
}

SaveStats WorldSaver::save(const SavedPlayer* player) {
    SaveStats st;
    Timer t;
    if (!locked()) {
        st.ok = false;
        return st;
    }
    std::map<std::pair<int, int>, std::vector<int>> byRegion;
    for (u64 k : world_.dirtyChunks()) {
        const int cx = int(u32(k >> 32)), cz = int(u32(k));
        byRegion[{cx >> 5, cz >> 5}].push_back(((cz & 31) << 5) | (cx & 31));
    }
    for (const auto& [rc, chunks] : byRegion) {
        if (saveRegion(rc.first, rc.second, chunks)) {
            ++st.regions;
            st.chunks += chunks.size();
            for (int idx : chunks) world_.clearDirtyChunk(rc.first * 32 + (idx & 31), rc.second * 32 + (idx >> 5));
        } else {
            st.ok = false;
        }
    }
    if (player && !savePlayer(*player)) st.ok = false;
    st.ms = t.ms();
    return st;
}
