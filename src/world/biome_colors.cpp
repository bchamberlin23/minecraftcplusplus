#include "world/biome_colors.h"

#include "assets/asset_source.h"
#include "mesh/mesher.h"
#include "world/world.h"

#include <json/json.hpp>
#include <stb/stb_image.h>

#include <algorithm>
#include <unordered_map>

struct BiomeColors::Def {
    float temperature = 0.8f, downfall = 0.4f;
    i64 grass = -1, foliage = -1, water = 0x3F76E4;  // -1: from the colormap
    std::string grassModifier;
};

namespace {

i64 parseColor(const nlohmann::json& j) {
    if (j.is_number_integer()) return j.get<i64>() & 0xFFFFFF;
    if (j.is_string()) {
        std::string s = j.get<std::string>();
        if (!s.empty() && s[0] == '#') s.erase(0, 1);
        return i64(std::strtoul(s.c_str(), nullptr, 16) & 0xFFFFFF);
    }
    return -1;
}

// GrassColor.get / FoliageColor.get: the colormap at (temperature, downfall * temperature).
u32 colormap(const std::vector<u32>& map, double temperature, double downfall, u32 fallback) {
    temperature = std::clamp(temperature, 0.0, 1.0);
    downfall = std::clamp(downfall, 0.0, 1.0) * temperature;
    const int x = int((1.0 - temperature) * 255.0), y = int((1.0 - downfall) * 255.0);
    const size_t index = size_t(y) << 8 | size_t(x);
    return index < map.size() ? map[index] : fallback;
}

}  // namespace

BiomeColors::BiomeColors() : defs_(std::make_unique<std::unordered_map<std::string, Def>>()) {}
BiomeColors::~BiomeColors() = default;

bool BiomeColors::init(const fs::path& jar) {
    // Biomes from the game's data with the data packs (any namespace), colormaps from its assets with the
    // resource packs.
    const AssetSource& data = AssetSource::data(jar);
    const AssetSource& assets = AssetSource::client(jar);
    if (data.empty() || assets.empty()) return false;
    std::vector<u8> bytes;
    std::vector<std::string> files;
    for (const std::string& ns : data.namespaces("data"))
        for (std::string& f : data.list("data/" + ns + "/worldgen/biome/", ".json")) files.push_back(std::move(f));
    files.push_back("assets/minecraft/textures/colormap/grass.png");
    files.push_back("assets/minecraft/textures/colormap/foliage.png");
    for (const std::string& file : files) {
        std::string_view n = file;
        const size_t nsEnd = n.find('/', 5);
        const std::string_view biomePrefix = "/worldgen/biome/";
        if (n.starts_with("data/") && n.substr(nsEnd).starts_with(biomePrefix) &&
            n.find('/', nsEnd + biomePrefix.size()) == std::string_view::npos) {
            if (!data.read(file, bytes)) continue;
            const auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
            if (!j.is_object()) continue;
            Def d;
            d.temperature = j.value("temperature", 0.8f);
            d.downfall = j.value("downfall", 0.4f);
            if (j.contains("effects") && j["effects"].is_object()) {
                const auto& fx = j["effects"];
                if (fx.contains("grass_color")) d.grass = parseColor(fx["grass_color"]);
                if (fx.contains("foliage_color")) d.foliage = parseColor(fx["foliage_color"]);
                if (fx.contains("water_color")) d.water = parseColor(fx["water_color"]);
                d.grassModifier = fx.value("grass_color_modifier", "");
            }
            const std::string id = std::string(n.substr(5, nsEnd - 5)) + ":" +
                                   std::string(n.substr(nsEnd + biomePrefix.size(), n.size() - nsEnd - biomePrefix.size() - 5));
            World::biomeId(id);  // known biomes get ids up front
            (*defs_)[id] = d;
        } else if (n == "assets/minecraft/textures/colormap/grass.png" ||
                   n == "assets/minecraft/textures/colormap/foliage.png") {
            if (!assets.read(file, bytes)) continue;
            int w = 0, h = 0, c = 0;
            u8* img = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &c, 4);
            if (!img) continue;
            std::vector<u32>& map = n.ends_with("grass.png") ? grassMap_ : foliageMap_;
            map.resize(size_t(w) * h);
            for (size_t i = 0; i < map.size(); ++i)
                map[i] = (u32(img[i * 4]) << 16) | (u32(img[i * 4 + 1]) << 8) | img[i * 4 + 2];
            stbi_image_free(img);
        }
    }
    return !defs_->empty() && !grassMap_.empty() && !foliageMap_.empty();
}

u32 BiomeColors::compute(u8 biome, Kind kind) const {
    const std::string name = World::biomeName(biome);
    auto it = defs_->find(name);
    if (it == defs_->end()) it = defs_->find("minecraft:plains");
    const Def d = it != defs_->end() ? it->second : Def{};
    switch (kind) {
    case Grass: {
        u32 c = d.grass >= 0 ? u32(d.grass) : colormap(grassMap_, d.temperature, d.downfall, 0x91BD59);
        if (d.grassModifier == "dark_forest") c = ((c & 0xFEFEFE) + 0x28340A) >> 1;
        if (d.grassModifier == "swamp") c = 0x6A7039;  // the game alternates with 0x4C763C by a noise
        return c;
    }
    case Foliage: return d.foliage >= 0 ? u32(d.foliage) : colormap(foliageMap_, d.temperature, d.downfall, 0x77AB2F);
    default: return u32(d.water);
    }
}

u32 BiomeColors::color(u8 biome, Kind kind) const {
    std::atomic<u32>& slot = cache_[size_t(biome) * KindCount + kind];
    u32 v = slot.load(std::memory_order_relaxed);
    if (!(v & 0x80000000u)) {
        v = compute(biome, kind) | 0x80000000u;
        slot.store(v, std::memory_order_relaxed);
    }
    return v & 0xFFFFFF;
}

namespace {
std::atomic<int> gBlendRadius{2};
}  // namespace

void setBiomeBlendRadius(int radius) { gBlendRadius.store(std::clamp(radius, 0, 7)); }

void gatherColumnColors(const World& world, const BiomeColors& colors, int cx, int cz, ColumnColors& out) {
    // Biome colours of the block columns the (2r+1)^2 blend reaches (the Biome Blend option, BiomeColors with the
    // game's ColorResolver box), then a separable box filter.
    constexpr int kMaxR = 7, kMaxN = 18 + 2 * kMaxR;
    const int kR = gBlendRadius.load(std::memory_order_relaxed), kN = 18 + 2 * kR;
    const int x0 = cx * 16 - 1 - kR, z0 = cz * 16 - 1 - kR;
    static constexpr u32 kDefaults[BiomeColors::KindCount] = {0x91BD59, 0x77AB2F, 0x3F76E4};
    u8 biomes[kMaxN * kMaxN];
    bool any = false;
    for (int z = 0; z < kN; ++z)
        for (int x = 0; x < kN; ++x) {
            // Same quart as the left neighbour: reuse it (one world lookup per quart column and row).
            const bool sameQuart = x > 0 && ((x0 + x) >> 2) == ((x0 + x - 1) >> 2);
            const u8 b = sameQuart ? biomes[z * kN + x - 1] : world.biomeAt(x0 + x, z0 + z);
            biomes[z * kN + x] = b;
            any |= b != World::kUnknownBiome;
        }
    for (int k = 0; k < BiomeColors::KindCount; ++k) {
        if (!any) {
            std::fill(std::begin(out.index[k]), std::end(out.index[k]), u16(k == 0 ? 1 : k == 1 ? 2 : 5));
            continue;
        }
        u32 rgb[kMaxN * kMaxN][3];
        for (int i = 0; i < kN * kN; ++i) {
            const u32 c = biomes[i] == World::kUnknownBiome ? kDefaults[k]
                                                             : colors.color(biomes[i], BiomeColors::Kind(k));
            rgb[i][0] = c >> 16, rgb[i][1] = (c >> 8) & 255, rgb[i][2] = c & 255;
        }
        u32 rows[kMaxN * 18][3];  // horizontal pass: kN rows x 18 columns
        for (int z = 0; z < kN; ++z)
            for (int x = 0; x < 18; ++x)
                for (int ch = 0; ch < 3; ++ch) {
                    u32 s = 0;
                    for (int d = 0; d <= 2 * kR; ++d) s += rgb[z * kN + x + d][ch];
                    rows[z * 18 + x][ch] = s;
                }
        u32 lastRgb = 0xFFFFFFFF;
        u16 lastIndex = 0;
        for (int z = 0; z < 18; ++z)
            for (int x = 0; x < 18; ++x) {
                u32 c[3] = {};
                for (int ch = 0; ch < 3; ++ch) {
                    for (int d = 0; d <= 2 * kR; ++d) c[ch] += rows[(z + d) * 18 + x][ch];
                    c[ch] /= (2 * kR + 1) * (2 * kR + 1);
                }
                const u32 v = (c[0] << 16) | (c[1] << 8) | c[2];
                if (v != lastRgb) lastRgb = v, lastIndex = ColorTable::index(v);  // flat inside a biome
                out.index[k][z * 18 + x] = lastIndex;
            }
    }
}
