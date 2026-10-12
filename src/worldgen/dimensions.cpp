#include "worldgen/dimensions.h"

#include "world/nbt_dom.h"

#include <libdeflate.h>

namespace wg {

namespace {

bool readGzipNbt(const fs::path& file, NbtValue& root) {
    std::vector<u8> gz;
    if (!readFile(file, gz) || gz.size() < 18) return false;
    const u32 isize = u32(gz[gz.size() - 4]) | (u32(gz[gz.size() - 3]) << 8) | (u32(gz[gz.size() - 2]) << 16) |
                      (u32(gz[gz.size() - 1]) << 24);
    std::vector<u8> raw(std::max<u32>(isize, 1));
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    size_t actual = 0;
    const bool ok = libdeflate_gzip_decompress(d, gz.data(), gz.size(), raw.data(), raw.size(), &actual) == LIBDEFLATE_SUCCESS;
    libdeflate_free_decompressor(d);
    return ok && nbtRead(raw.data(), actual, root);
}

// NBT as the JSON the codecs read (bytes are the game's booleans).
nlohmann::json toJson(const NbtValue& v) {
    switch (v.type) {
    case TagByte: return v.i == 0 || v.i == 1 ? nlohmann::json(v.i != 0) : nlohmann::json(v.i);
    case TagShort: case TagInt: case TagLong: return v.i;
    case TagFloat: case TagDouble: return v.d;
    case TagString: return v.s;
    case TagByteArray: {
        nlohmann::json a = nlohmann::json::array();
        for (u8 b : v.bytes) a.push_back(int(i8(b)));
        return a;
    }
    case TagIntArray: return v.ints;
    case TagLongArray: return v.longs;
    case TagList: {
        nlohmann::json a = nlohmann::json::array();
        for (const NbtValue& e : v.items) a.push_back(toJson(e));
        return a;
    }
    case TagCompound: {
        nlohmann::json o = nlohmann::json::object();
        for (size_t k = 0; k < v.names.size(); ++k) o[v.names[k]] = toJson(v.items[k]);
        return o;
    }
    default: return nullptr;
    }
}

std::string qualified(std::string id) { return id.find(':') == std::string::npos ? "minecraft:" + id : id; }

// LevelStem JSON (from a preset, a data pack or the save) -> spec; the dimension type by id or inline.
bool makeSpec(const DataPack& data, const std::string& name, const nlohmann::json& stem, DimensionSpec& out) {
    if (!stem.is_object() || !stem.contains("generator")) return false;
    out.name = qualified(name);
    out.generator = stem["generator"];
    const nlohmann::json* type = nullptr;
    if (const auto t = stem.find("type"); t != stem.end()) type = t->is_string() ? data.file("dimension_type", t->get<std::string>()) : &*t;
    if (type && type->is_object()) {
        out.minY = type->value("min_y", -64);
        out.height = type->value("height", 384);
        out.hasCeiling = type->value("has_ceiling", false);
    }
    return true;
}

void addDataPackDimensions(const DataPack& data, WorldGenSettings& s) {
    for (const std::string& id : data.fileIds("dimension")) {
        DimensionSpec spec;
        if (!makeSpec(data, id, *data.file("dimension", id), spec)) continue;
        bool replaced = false;
        for (DimensionSpec& d : s.dimensions)
            if (d.name == spec.name) d = spec, replaced = true;
        if (!replaced) s.dimensions.push_back(std::move(spec));
    }
}

}  // namespace

const DimensionSpec* WorldGenSettings::find(std::string_view name) const {
    for (const DimensionSpec& d : dimensions)
        if (d.name == name) return &d;
    return nullptr;
}

WorldGenSettings defaultWorldGenSettings(const DataPack& data, i64 seed, std::string_view preset) {
    WorldGenSettings s;
    s.seed = seed;
    if (const nlohmann::json* p = data.worldgen("world_preset", preset); p && p->contains("dimensions"))
        for (const auto& [name, stem] : (*p)["dimensions"].items()) {
            DimensionSpec spec;
            if (makeSpec(data, name, stem, spec)) s.dimensions.push_back(std::move(spec));
        }
    addDataPackDimensions(data, s);
    return s;
}

bool readWorldGenSettings(const fs::path& saveDir, const DataPack& data, WorldGenSettings& out) {
    NbtValue root;
    NbtValue* settings = nullptr;
    if (readGzipNbt(saveDir / "data" / "minecraft" / "world_gen_settings.dat", root)) settings = root.find("data");
    NbtValue level;
    if (!settings && readGzipNbt(saveDir / "level.dat", level))
        if (NbtValue* d = level.find("Data")) settings = d->find("WorldGenSettings");
    if (!settings || !settings->find("seed")) return false;
    out = defaultWorldGenSettings(data, settings->find("seed")->i);
    if (const NbtValue* g = settings->find("generate_features")) out.generateStructures = g->i != 0;  // older saves
    if (const NbtValue* g = settings->find("generate_structures")) out.generateStructures = g->i != 0;
    if (NbtValue* dims = settings->find("dimensions")) {
        for (size_t k = 0; k < dims->names.size(); ++k) {
            DimensionSpec spec;
            if (!makeSpec(data, dims->names[k], toJson(dims->items[k]), spec)) continue;
            bool replaced = false;
            for (DimensionSpec& d : out.dimensions)
                if (d.name == spec.name) d = spec, replaced = true;
            if (!replaced) out.dimensions.push_back(std::move(spec));
        }
        addDataPackDimensions(data, out);  // the data packs' registry wins over the saved copy, like the game
    }
    return true;
}

}  // namespace wg
