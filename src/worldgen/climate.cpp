#include "worldgen/climate.h"

#include "worldgen/climate_tree.h"
#include "worldgen/rng.h"

#include <deque>
#include <stdexcept>

namespace wg {

// ---- BiomeTable ---------------------------------------------------------------------------

namespace {
std::string qualifiedBiome(std::string_view name) {
    return name.find(':') == std::string_view::npos ? "minecraft:" + std::string(name) : std::string(name);
}
}  // namespace

u16 BiomeTable::id(std::string_view name) {
    std::string key = qualifiedBiome(name);
    std::lock_guard lock(mutex_);
    if (const auto it = ids_.find(key); it != ids_.end()) return it->second;
    // name() hands out references without the lock: keep the strings from moving.
    if (names_.capacity() == 0) names_.reserve(4096);
    if (names_.size() == names_.capacity()) LOG("BiomeTable: more than %zu biomes", names_.size());
    const u16 id = u16(names_.size());
    names_.push_back(key);
    ids_.emplace(std::move(key), id);
    return id;
}

const std::string& BiomeTable::name(u16 id) const {
    std::lock_guard lock(mutex_);
    return names_.at(id);
}

size_t BiomeTable::size() const {
    std::lock_guard lock(mutex_);
    return names_.size();
}

// ---- BiomeSource ------------------------------------------------------------------------------

// ChunkAccess.fillBiomesFromNoise with the source's plain resolver.
void BiomeSource::fillBiomes(GenChunk& chunk, DensityContext& ctx) const {
    const int quartMinX = chunk.minBlockX() >> 2, quartMinZ = chunk.minBlockZ() >> 2, quartMinY = chunk.minY >> 2;
    const int quartHeight = chunk.height >> 2;
    for (int qy = 0; qy < quartHeight; ++qy)
        for (int x = 0; x < 4; ++x)
            for (int z = 0; z < 4; ++z)
                chunk.biomes[(size_t(qy) * 4 + z) * 4 + x] = noiseBiome(ctx, quartMinX + x, quartMinY + qy, quartMinZ + z);
}

// ---- MultiNoiseBiomeSource ------------------------------------------------------------------

namespace {
// RTree.lastResult is a ThreadLocal of each (shared) parameter list's tree; same here, per tree.
int& lastLeaf(const climate::Tree* tree) {
    thread_local std::vector<std::pair<const climate::Tree*, int>> hints;
    for (auto& h : hints)
        if (h.first == tree) return h.second;
    hints.push_back({tree, -1});
    return hints.back().second;
}

// MultiNoiseBiomeSourceParameterList.Preset.NETHER
const climate::Tree& netherTree() {
    static const climate::Tree tree([] {
        using namespace climate;
        auto e = [](float t, float h, float o, const char* biome) {
            Entry x{};
            const float v[kDims] = {t, h, 0, 0, 0, 0, o};
            for (int d = 0; d < kDims; ++d) x.space[d] = {quantize(v[d]), quantize(v[d])};
            x.biome = biome;
            return x;
        };
        return std::vector<Entry>{e(0.0f, 0.0f, 0.0f, "minecraft:nether_wastes"), e(0.0f, -0.5f, 0.0f, "minecraft:soul_sand_valley"),
                                  e(0.4f, 0.0f, 0.0f, "minecraft:crimson_forest"), e(0.0f, 0.5f, 0.375f, "minecraft:warped_forest"),
                                  e(-0.5f, 0.0f, 0.175f, "minecraft:basalt_deltas")};
    }());
    return tree;
}

// Climate.Parameter.CODEC: a number (a point), [min, max] or {"min", "max"}.
climate::Param parameter(const nlohmann::json& j) {
    if (j.is_number()) return {climate::quantize(j.get<float>()), climate::quantize(j.get<float>())};
    if (j.is_array() && j.size() == 2) return {climate::quantize(j[0].get<float>()), climate::quantize(j[1].get<float>())};
    if (j.is_object()) return {climate::quantize(j.at("min").get<float>()), climate::quantize(j.at("max").get<float>())};
    throw std::runtime_error("bad climate parameter " + j.dump());
}
}  // namespace

struct MultiNoiseBiomeSource::Impl {
    const NoiseState& noise;
    DensityRef fn[6];  // temperature, vegetation, continents, erosion, depth, ridges (Climate.Sampler order)
    const climate::Tree* tree = nullptr;
    std::unique_ptr<climate::Tree> owned;  // a data pack's own list
    std::deque<std::string> names;         // its biome names (the tree keeps pointers)
    std::vector<u16> ids;                  // tree biome index -> BiomeTable id

    u16 lookup(const float v[6]) const {
        const i64 target[climate::kDims] = {climate::quantize(v[0]), climate::quantize(v[1]), climate::quantize(v[2]),
                                            climate::quantize(v[3]), climate::quantize(v[4]), climate::quantize(v[5]),
                                            0};
        return ids[tree->search(target, lastLeaf(tree))];
    }
};

MultiNoiseBiomeSource::MultiNoiseBiomeSource(const NoiseState& noise, const nlohmann::json& router, BiomeTable& table,
                                             const nlohmann::json& source)
    : impl_(std::make_unique<Impl>(Impl{noise})) {
    static const char* const keys[6] = {"temperature", "vegetation", "continents", "erosion", "depth", "ridges"};
    for (int i = 0; i < 6; ++i) impl_->fn[i] = noise.parse(router.at(keys[i]));
    if (const auto p = source.find("preset"); p != source.end()) {
        // A multi_noise_biome_source_parameter_list entry (or its inline {"preset": ...} form) names a code preset.
        std::string preset = p->is_string() ? p->get<std::string>() : p->value("preset", std::string());
        if (p->is_string())
            if (const nlohmann::json* list = noise.data().worldgen("multi_noise_biome_source_parameter_list", preset))
                preset = list->value("preset", preset);
        preset = DataPack::normalize(preset);
        if (preset == "overworld") impl_->tree = &climate::overworldTree();
        else if (preset == "nether") impl_->tree = &netherTree();
        else throw std::runtime_error("unknown multi-noise preset " + preset);
    } else {
        std::vector<climate::Entry> entries;
        for (const nlohmann::json& e : source.at("biomes")) {
            climate::Entry x{};
            const nlohmann::json& p = e.at("parameters");
            static const char* const dims[6] = {"temperature", "humidity", "continentalness", "erosion", "depth", "weirdness"};
            for (int d = 0; d < 6; ++d) x.space[d] = parameter(p.at(dims[d]));
            const i64 offset = climate::quantize(p.value("offset", 0.0f));
            x.space[6] = {offset, offset};
            std::string biome = e.at("biome").get<std::string>();
            if (biome.find(':') == std::string::npos) biome = "minecraft:" + biome;
            x.biome = impl_->names.emplace_back(std::move(biome)).c_str();
            entries.push_back(x);
        }
        if (entries.empty()) throw std::runtime_error("empty multi-noise biome list");
        impl_->owned = std::make_unique<climate::Tree>(entries);
        impl_->tree = impl_->owned.get();
    }
    for (const char* name : impl_->tree->names()) impl_->ids.push_back(table.id(name));
}

MultiNoiseBiomeSource::~MultiNoiseBiomeSource() = default;

u16 MultiNoiseBiomeSource::noiseBiome(DensityContext& ctx, int quartX, int quartY, int quartZ) const {
    // QuartPos.toBlock (int shift, wrapping)
    const int x = i32(u32(quartX) << 2), y = i32(u32(quartY) << 2), z = i32(u32(quartZ) << 2);
    float v[6];
    for (int i = 0; i < 6; ++i) v[i] = impl_->noise.sample(impl_->fn[i], ctx, x, y, z);
    return impl_->lookup(v);
}

std::vector<u16> MultiNoiseBiomeSource::possibleBiomes() const { return impl_->ids; }

// ChunkGenerator.doCreateBiomes: MultiNoiseBiomeSource.createResolverForChunk samples the six
// climate functions over the column's quart grid, then ChunkAccess.fillBiomesFromNoise resolves
// section by section, x / y / z within each (the order matters for the RTree's last-result hint).
void MultiNoiseBiomeSource::fillBiomes(GenChunk& chunk, DensityContext& ctx) const {
    const int quartMinX = chunk.minBlockX() >> 2, quartMinZ = chunk.minBlockZ() >> 2;
    const int quartMinY = chunk.minY >> 2, quartHeight = chunk.height >> 2;
    Volume volume;
    volume.sizeX = 4, volume.sizeY = quartHeight, volume.sizeZ = 4;
    volume.minX = quartMinX << 2, volume.minY = quartMinY << 2, volume.minZ = quartMinZ << 2;
    volume.stepX = volume.stepY = volume.stepZ = 4;
    const size_t n = size_t(volume.size());
    std::vector<float> values(n * 6);
    for (int i = 0; i < 6; ++i) impl_->noise.sampleVolume(impl_->fn[i], ctx, volume, values.data() + n * i);

    const int minSection = chunk.minY >> 4, maxSection = (chunk.minY + chunk.height - 1) >> 4;
    for (int section = minSection; section <= maxSection; ++section) {
        for (int x = 0; x < 4; ++x) {
            for (int y = 0; y < 4; ++y) {
                const int qy = (section << 2) + y - quartMinY;
                if (qy < 0 || qy >= quartHeight) continue;
                for (int z = 0; z < 4; ++z) {
                    const size_t index = size_t(volume.index(x, qy, z));
                    float v[6];
                    for (int i = 0; i < 6; ++i) v[i] = values[n * i + index];
                    chunk.biomes[(size_t(qy) * 4 + z) * 4 + x] = impl_->lookup(v);
                }
            }
        }
    }
}

// ---- The End, fixed and checkerboard sources ----------------------------------------------------

namespace {

class TheEndBiomeSource final : public BiomeSource {
public:
    TheEndBiomeSource(const NoiseState& noise, const nlohmann::json& router, BiomeTable& table)
        : noise_(noise), erosion_(noise.parse(router.at("erosion"))) {
        for (const char* b : {"the_end", "end_highlands", "end_midlands", "small_end_islands", "end_barrens"})
            ids_.push_back(table.id(std::string("minecraft:") + b));
    }
    u16 noiseBiome(DensityContext& ctx, int quartX, int quartY, int quartZ) const override {
        const int blockX = i32(u32(quartX) << 2), blockY = i32(u32(quartY) << 2), blockZ = i32(u32(quartZ) << 2);
        const int chunkX = blockX >> 4, chunkZ = blockZ >> 4;
        if (i64(chunkX) * chunkX + i64(chunkZ) * chunkZ <= 4096) return ids_[0];
        const int weirdX = ((blockX >> 4) * 2 + 1) * 8, weirdZ = ((blockZ >> 4) * 2 + 1) * 8;
        const double height = noise_.sample(erosion_, ctx, weirdX, blockY, weirdZ);
        if (height > 0.25) return ids_[1];
        if (height >= -0.0625) return ids_[2];
        return height < -0.21875 ? ids_[3] : ids_[4];
    }
    std::vector<u16> possibleBiomes() const override { return ids_; }

private:
    const NoiseState& noise_;
    DensityRef erosion_;
    std::vector<u16> ids_;  // end, highlands, midlands, islands, barrens
};

class FixedBiomeSource final : public BiomeSource {
public:
    explicit FixedBiomeSource(u16 biome) : biome_(biome) {}
    u16 noiseBiome(DensityContext&, int, int, int) const override { return biome_; }
    void fillBiomes(GenChunk& chunk, DensityContext&) const override { std::fill(chunk.biomes.begin(), chunk.biomes.end(), biome_); }
    std::vector<u16> possibleBiomes() const override { return {biome_}; }
    int fixedBiome() const override { return biome_; }

private:
    u16 biome_;
};

class CheckerboardBiomeSource final : public BiomeSource {
public:
    CheckerboardBiomeSource(std::vector<u16> biomes, int scale) : biomes_(std::move(biomes)), shift_(scale + 2) {}
    u16 noiseBiome(DensityContext&, int quartX, int, int quartZ) const override {
        const int n = int(biomes_.size());
        return biomes_[size_t((((quartX >> shift_) + (quartZ >> shift_)) % n + n) % n)];  // Math.floorMod
    }
    std::vector<u16> possibleBiomes() const override {
        std::vector<u16> out;
        for (u16 b : biomes_)
            if (std::find(out.begin(), out.end(), b) == out.end()) out.push_back(b);
        return out;
    }

private:
    std::vector<u16> biomes_;
    int shift_;
};

std::string qualified(std::string id) { return id.find(':') == std::string::npos ? "minecraft:" + id : id; }

}  // namespace

std::unique_ptr<BiomeSource> makeBiomeSource(const nlohmann::json& source, const NoiseState& noise, const nlohmann::json& router,
                                             BiomeTable& table) {
    const std::string type = DataPack::normalize(source.value("type", std::string("multi_noise")));
    if (type == "multi_noise") return std::make_unique<MultiNoiseBiomeSource>(noise, router, table, source);
    if (type == "the_end") return std::make_unique<TheEndBiomeSource>(noise, router, table);
    if (type == "fixed") {
        const nlohmann::json& b = source.at("biome");
        return std::make_unique<FixedBiomeSource>(table.id(qualified(b.is_string() ? b.get<std::string>() : b.value("id", std::string()))));
    }
    if (type == "checkerboard") {
        // Biome.LIST_CODEC: a biome id, a list of them or a "#tag"
        std::vector<u16> ids;
        const nlohmann::json& b = source.at("biomes");
        auto add = [&](const std::string& id) {
            if (id.starts_with("#"))
                for (const std::string& t : noise.data().tag("worldgen/biome", id.substr(1))) ids.push_back(table.id(t));
            else
                ids.push_back(table.id(qualified(id)));
        };
        if (b.is_string()) add(b.get<std::string>());
        else
            for (const nlohmann::json& e : b) add(e.get<std::string>());
        if (ids.empty()) throw std::runtime_error("empty checkerboard biome list");
        return std::make_unique<CheckerboardBiomeSource>(std::move(ids), source.value("scale", 2));
    }
    throw std::runtime_error("unknown biome source type " + type);
}

// ---- BiomeZoom ----------------------------------------------------------------------------

namespace {
// SHA-256 (FIPS 180-4) of a short message.
void sha256(const u8* msg, size_t len, u8 out[32]) {
    static const u32 K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<u8> m(msg, msg + len);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    const u64 bits = u64(len) * 8;
    for (int i = 7; i >= 0; --i) m.push_back(u8(bits >> (8 * i)));
    auto rotr = [](u32 v, int n) { return (v >> n) | (v << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        u32 w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (u32(m[off + i * 4]) << 24) | (u32(m[off + i * 4 + 1]) << 16) | (u32(m[off + i * 4 + 2]) << 8) |
                   u32(m[off + i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const u32 t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const u32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    for (int i = 0; i < 32; ++i) out[i] = u8(h[i / 4] >> (24 - 8 * (i % 4)));
}

// LinearCongruentialGenerator.next
inline i64 lcgNext(i64 rval, i64 c) {
    return jadd(jmul(rval, jadd(jmul(rval, 6364136223846793005LL), 1442695040888963407LL)), c);
}

inline double fiddle(i64 rval) {
    const double uniform = double((rval >> 24) & 1023) / 1024.0;  // Math.floorMod(rval >> 24, 1024)
    return (uniform - 0.5) * 0.9;
}

double fiddledDistance(i64 seed, i32 x, i32 y, i32 z, double dx, double dy, double dz) {
    i64 r = lcgNext(seed, x);
    r = lcgNext(r, y);
    r = lcgNext(r, z);
    r = lcgNext(r, x);
    r = lcgNext(r, y);
    r = lcgNext(r, z);
    const double fx = fiddle(r);
    r = lcgNext(r, seed);
    const double fy = fiddle(r);
    r = lcgNext(r, seed);
    const double fz = fiddle(r);
    return (dz + fz) * (dz + fz) + (dy + fy) * (dy + fy) + (dx + fx) * (dx + fx);
}
}  // namespace

// BiomeManager.obfuscateSeed: Hashing.sha256().hashLong(seed).asLong() (little-endian in and out).
BiomeZoom::BiomeZoom(i64 worldSeed) {
    u8 in[8], digest[32];
    for (int i = 0; i < 8; ++i) in[i] = u8(u64(worldSeed) >> (8 * i));
    sha256(in, 8, digest);
    u64 v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | digest[i];
    zoomSeed_ = i64(v);
}

// BiomeManager.getBiome
u16 BiomeZoom::biome(int blockX, int blockY, int blockZ, const NoiseBiomeFn& noiseBiome) const {
    const i32 absX = i32(u32(blockX) - 2u), absY = i32(u32(blockY) - 2u), absZ = i32(u32(blockZ) - 2u);
    const i32 parentX = absX >> 2, parentY = absY >> 2, parentZ = absZ >> 2;
    const double fractX = (absX & 3) / 4.0, fractY = (absY & 3) / 4.0, fractZ = (absZ & 3) / 4.0;
    int minI = 0;
    double minDistance = HUGE_VAL;
    for (int i = 0; i < 8; ++i) {
        const bool xEven = (i & 4) == 0, yEven = (i & 2) == 0, zEven = (i & 1) == 0;
        const i32 cx = xEven ? parentX : i32(u32(parentX) + 1u);
        const i32 cy = yEven ? parentY : i32(u32(parentY) + 1u);
        const i32 cz = zEven ? parentZ : i32(u32(parentZ) + 1u);
        const double next = fiddledDistance(zoomSeed_, cx, cy, cz, xEven ? fractX : fractX - 1.0,
                                            yEven ? fractY : fractY - 1.0, zEven ? fractZ : fractZ - 1.0);
        if (minDistance > next) {
            minI = i;
            minDistance = next;
        }
    }
    return noiseBiome((minI & 4) == 0 ? parentX : i32(u32(parentX) + 1u),
                      (minI & 2) == 0 ? parentY : i32(u32(parentY) + 1u),
                      (minI & 1) == 0 ? parentZ : i32(u32(parentZ) + 1u));
}

}  // namespace wg
