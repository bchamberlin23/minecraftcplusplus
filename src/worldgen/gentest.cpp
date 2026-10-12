#include "worldgen/gentest.h"

#include "assets/assets.h"
#include "world/nbt_dom.h"
#include "worldgen/feature_api.h"
#include "worldgen/generator.h"

#include <libdeflate.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>

namespace {

u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

// Palette entry in any of the save formats (see World's parser) as a state key.
std::string stateName(const NbtValue& entry) {
    NbtValue& e = const_cast<NbtValue&>(entry);
    if (e.type == TagString) return e.s;
    std::string n;
    for (const char* k : {"Name", "id", ""})
        if (NbtValue* v = e.find(k)) n = v->s;
    std::vector<std::pair<std::string, std::string>> props;
    NbtValue* p = e.find("Properties");
    if (!p) p = e.find("properties");
    if (p)
        for (size_t k = 0; k < p->names.size(); ++k) props.push_back({p->names[k], p->items[k].s});
    std::sort(props.begin(), props.end());
    if (props.empty()) return n;
    n += '[';
    for (size_t i = 0; i < props.size(); ++i) n += (i ? "," : "") + props[i].first + "=" + props[i].second;
    return n + "]";
}

// Unpacks a paletted container: `count` indices into palette of `paletteSize`, minimum bit width `minBits`.
std::vector<int> unpack(const NbtValue* data, int paletteSize, int count, int minBits) {
    std::vector<int> out(size_t(count), 0);
    if (!data || paletteSize <= 1) return out;
    int bits = 0;
    while ((1 << bits) < paletteSize) ++bits;
    bits = std::max(bits, minBits);
    const int perWord = 64 / bits;
    for (int i = 0; i < count; ++i) {
        const u64 w = u64(data->longs[size_t(i / perWord)]);
        out[size_t(i)] = int((w >> ((i % perWord) * bits)) & ((u64(1) << bits) - 1));
    }
    return out;
}

// Every block of a saved chunk as registry ids, laid out like GenChunk::blocks.
std::vector<u16> readBlocks(NbtValue& root, BlockRegistry& registry, int minY, int height) {
    std::vector<u16> out(size_t(16) * 16 * size_t(height), 0);
    NbtValue* sections = root.find("sections");
    if (!sections) return out;
    for (NbtValue& sec : sections->items) {
        const int sy = int(i8(sec.find("Y")->i));
        if (sy * 16 < minY || sy * 16 >= minY + height) continue;
        NbtValue* states = sec.find("block_states");
        if (!states) continue;
        NbtValue* pal = states->find("palette");
        std::vector<u16> ids;
        for (const NbtValue& e : pal->items) ids.push_back(registry.idFor(stateName(e)));
        const auto idx4096 = unpack(states->find("data"), int(ids.size()), 4096, 4);
        for (int i = 0; i < 4096; ++i)
            out[size_t(sy * 16 - minY + (i >> 8)) * 256 + size_t(i & 255)] = ids[size_t(idx4096[size_t(i)])];
    }
    return out;
}

struct SavedChunk {
    int cx, cz;
    std::vector<u16> blocks;
};

// Decoration check: decorates every chunk around the saved "minecraft:full" chunks (the game decorates
// each chunk once, with its neighbours at terrain status or later) and compares the result.
int runDecorationTest(BlockRegistry& registry, wg::ChunkGenerator& gen, i64 seed, const std::vector<SavedChunk>& saved) {
    Timer t;
    wg::FeatureSystem features(gen, registry);
    LOG("gentest: feature system ready in %.1f ms", t.ms());
    {
        std::string order;
        for (u16 b : gen.possibleBiomes()) order += " " + gen.biomes().name(b);
        LOG("gentest: possible biomes:%s", order.c_str());
    }
    wg::DensityContext ctx;
    wg::BiomeZoom zoom(seed);
    const wg::NoiseBiomeFn noiseBiome = [&](int qx, int qy, int qz) { return gen.noiseBiome(ctx, qx, qy, qz); };
    std::map<std::pair<int, int>, std::unique_ptr<wg::GenChunk>> chunks;
    auto chunkAt = [&](int x, int z) -> wg::GenChunk& {
        auto& c = chunks[{x, z}];
        if (!c) {
            c = std::make_unique<wg::GenChunk>();
            gen.generate(x, z, *c, ctx);
        }
        return *c;
    };
    std::set<std::pair<int, int>> decorate;
    for (const SavedChunk& s : saved)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) decorate.insert({s.cx + dx, s.cz + dz});
    t = Timer();
    for (const auto& [x, z] : decorate)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) chunkAt(x + dx, z + dz);
    LOG("gentest: terrain for %zu chunks in %.1f ms", chunks.size(), t.ms());

    auto compare = [&](const SavedChunk& s, std::map<std::pair<std::string, std::string>, size_t>* confusion) {
        const wg::GenChunk& c = *chunks[{s.cx, s.cz}];
        size_t bad = 0;
        for (size_t i = 0; i < s.blocks.size(); ++i)
            if (registry.name(s.blocks[i]) != registry.name(c.blocks[i])) {
                ++bad;
                if (confusion) ++(*confusion)[{registry.name(s.blocks[i]), registry.name(c.blocks[i])}];
            }
        return bad;
    };
    size_t before = 0, total = 0;
    for (const SavedChunk& s : saved) before += compare(s, nullptr), total += s.blocks.size();

    // The game decorates neighbouring chunks in no fixed order. GENTEST_ORDER=reverse or a number (shuffle
    // seed) changes ours, to tell order-dependent overlaps from real differences.
    std::vector<std::pair<int, int>> order(decorate.begin(), decorate.end());
    if (const char* o = std::getenv("GENTEST_ORDER")) {
        if (std::string(o) == "reverse") {
            std::reverse(order.begin(), order.end());
        } else if (std::string(o) == "zmajor") {
            std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
                return a.second != b.second ? a.second < b.second : a.first < b.first;
            });
        } else {
            wg::LegacyRandom r(std::atoll(o));
            for (size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[size_t(r.nextInt(int(i)))]);
        }
    }
    t = Timer();
    for (const auto& [x, z] : order) {
        std::array<wg::GenChunk*, 9> region{};
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) region[size_t((dz + 1) * 3 + dx + 1)] = &chunkAt(x + dx, z + dz);
        features.decorate(region, zoom, noiseBiome);
    }
    const double decorateMs = t.ms();

    std::map<std::pair<std::string, std::string>, size_t> confusion;
    size_t after = 0;
    // GENTEST_DUMP=<file>: every remaining difference as "x y z expected generated".
    FILE* dump = nullptr;
    if (const char* path = std::getenv("GENTEST_DUMP")) dump = std::fopen(path, "w");
    for (const SavedChunk& s : saved) {
        const size_t bad = compare(s, &confusion);
        if (dump) {
            const wg::GenChunk& c = *chunks[{s.cx, s.cz}];
            for (size_t i = 0; i < s.blocks.size(); ++i)
                if (registry.name(s.blocks[i]) != registry.name(c.blocks[i]))
                    std::fprintf(dump, "%d %d %d %s %s\n", s.cx * 16 + int(i & 15), c.minY + int(i >> 8),
                                 s.cz * 16 + int((i >> 4) & 15), registry.name(s.blocks[i]).c_str(),
                                 registry.name(c.blocks[i]).c_str());
        }
        after += bad;
        LOG("gentest: decorated chunk %d,%d: %zu blocks differ", s.cx, s.cz, bad);
    }
    if (dump) std::fclose(dump);
    LOG("gentest: decoration: %zu full chunks | blocks differing %zu before decoration, %zu after (of %zu) | "
        "%.2f ms per decorated chunk",
        saved.size(), before, after, total, decorateMs / double(std::max<size_t>(decorate.size(), 1)));
    std::vector<std::pair<size_t, std::string>> top;
    for (const auto& [k, n] : confusion) top.push_back({n, k.first + "  ->  " + k.second});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < std::min<size_t>(top.size(), 80); ++i)
        LOG("gentest:   %8zu  expected %s", top[i].first, top[i].second.c_str());
    return after ? 2 : 0;
}

}  // namespace

GenTestTarget genTestTarget(const wg::DataPack& data, const fs::path& dir, i64 seed) {
    GenTestTarget t;
    t.seed = seed;
    t.regionDir = dir;
    wg::WorldGenSettings settings;
    const char* dim = std::getenv("GENTEST_DIM");
    const std::string name = dim ? dim : "minecraft:overworld";
    if (fs::exists(dir / "level.dat") && wg::readWorldGenSettings(dir, data, settings)) {
        t.seed = settings.seed;
        t.structures = settings.generateStructures;
        const size_t colon = name.find(':');
        t.regionDir = dir / "dimensions" / name.substr(0, colon) / name.substr(colon + 1) / "region";
    } else {
        settings = wg::defaultWorldGenSettings(data, seed);
    }
    if (const wg::DimensionSpec* d = settings.find(name)) t.dim = *d;
    LOG("gentest: %s of %s, seed %lld, generator %s", name.c_str(), dir.string().c_str(), (long long)t.seed, t.dim.generator.dump().c_str());
    return t;
}

int runGenTest(BlockRegistry& registry, const fs::path& jar, const fs::path& dir, i64 seedArg, int maxChunks) {
    if (const char* mode = std::getenv("GENTEST_STRUCTURES")) return runStructureTest(registry, jar, dir, seedArg, mode);
    wg::DataPack data;
    if (!data.open(jar)) {
        LOG("gentest: cannot read game data");
        return 1;
    }
    const GenTestTarget target = genTestTarget(data, dir, seedArg);
    const fs::path& regionDir = target.regionDir;
    const i64 seed = target.seed;
    Timer t;
    wg::ChunkGenerator gen(data, registry, seed, target.dim);
    if (!gen.ok()) return 1;
    LOG("gentest: generator ready in %.1f ms (seed %lld)", t.ms(), (long long)seed);
    if (const char* far = std::getenv("GENTEST_FAR")) {  // time far-terrain regions single-threaded
        const int level = std::atoi(far);
        wg::DensityContext fctx;
        FarColumns full, band, coarser;
        double phases[2] = {}, bandPhases[2] = {}, fullMs = 0, bandMs = 0;
        size_t cells = 0, differ = 0;
        const int n = level >= 4 ? 4 : 2;
        for (int rz = 0; rz < n; ++rz)
            for (int rx = 0; rx < n; ++rx) {
                Timer ft;
                gen.generateCoarse(rx + 3, rz - 2, level, full, fctx, nullptr, phases);
                fullMs += ft.ms();
                if (level >= 5) continue;
                gen.generateCoarse(rx + 3, rz - 2, level + 1, coarser, fctx);
                ft = Timer();
                gen.generateCoarse(rx + 3, rz - 2, level, band, fctx, &coarser, bandPhases);
                bandMs += ft.ms();
                for (int cz = 0; cz < full.n; ++cz)
                    for (int cx = 0; cx < full.n; ++cx)
                        for (int y = gen.minY(); y < gen.minY() + gen.height(); ++y) differ += full.at(cx, cz, y) != band.at(cx, cz, y);
                cells += size_t(full.n) * full.n * size_t(gen.height());
            }
        LOG("gentest: far level %d: %.1f ms per region (density %.1f, materials %.1f)", level, fullMs / (n * n),
            phases[0] / (n * n), phases[1] / (n * n));
        if (cells)
            LOG("gentest: refined from level %d: %.1f ms per region (density %.1f, materials %.1f), %.3f%% cells differ",
                level + 1, bandMs / (n * n), bandPhases[0] / (n * n), bandPhases[1] / (n * n),
                100.0 * double(differ) / double(cells));
        return 0;
    }

    wg::DensityContext ctx;
    wg::GenChunk chunk;
    wg::GenTimings timings;
    size_t chunks = 0, biomeCells = 0, biomeBad = 0, blocks = 0, blockBad = 0;
    std::map<std::pair<std::string, std::string>, size_t> confusion;
    std::map<std::string, size_t> badBiomes;
    std::vector<SavedChunk> fullChunks;
    libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
    std::vector<u8> raw(size_t(8) << 20);

    std::error_code ec;
    for (const auto& file : fs::directory_iterator(regionDir, ec)) {
        if (file.path().extension() != ".mca") continue;
        std::vector<u8> region;
        if (!readFile(file.path(), region) || region.size() < 8192) continue;
        int rx, rz;
        if (std::sscanf(file.path().filename().string().c_str(), "r.%d.%d", &rx, &rz) != 2) continue;
        for (int idx = 0; idx < 1024 && int(chunks) < maxChunks; ++idx) {
            const u32 sector = be32(&region[size_t(idx) * 4]) >> 8;
            if (!sector) continue;
            const size_t pos = size_t(sector) * 4096;
            const u32 len = be32(&region[pos]);
            size_t got = 0;
            if (libdeflate_zlib_decompress(dec, &region[pos + 5], len - 1, raw.data(), raw.size(), &got) != LIBDEFLATE_SUCCESS)
                continue;
            NbtValue root;
            if (!nbtRead(raw.data(), got, root)) continue;
            const NbtValue* status = root.find("Status");
            if (!status) continue;
            if (status->s == "minecraft:full") {
                fullChunks.push_back({rx * 32 + (idx & 31), rz * 32 + (idx >> 5), readBlocks(root, registry, gen.minY(), gen.height())});
                continue;
            }
            const bool terrain = status->s == "minecraft:terrain";
            if (!terrain && status->s != "minecraft:biomes") continue;

            const int cx = rx * 32 + (idx & 31), cz = rz * 32 + (idx >> 5);
            Timer tc;
            gen.generate(cx, cz, chunk, ctx, &timings);
            ++chunks;
            size_t chunkBad = 0;
            NbtValue* sections = root.find("sections");
            if (!sections) continue;
            for (NbtValue& sec : sections->items) {
                const int sy = int(i8(sec.find("Y")->i));
                if (sy * 16 < gen.minY() || sy * 16 >= gen.minY() + gen.height()) continue;
                if (NbtValue* biomes = sec.find("biomes")) {
                    NbtValue* pal = biomes->find("palette");
                    const auto idx64 = unpack(biomes->find("data"), int(pal->items.size()), 64, 0);
                    for (int i = 0; i < 64; ++i) {
                        const int qx = i & 3, qz = (i >> 2) & 3, qy = i >> 4;
                        const std::string& want = pal->items[size_t(idx64[size_t(i)])].s;
                        const std::string& have = gen.biomes().name(chunk.biome(qx, (sy * 16 - gen.minY()) / 4 + qy, qz));
                        ++biomeCells;
                        if (want != have) ++biomeBad, ++badBiomes[want + " -> " + have];
                    }
                }
                if (!terrain) continue;
                NbtValue* states = sec.find("block_states");
                if (!states) continue;
                NbtValue* pal = states->find("palette");
                std::vector<std::string> names;
                for (const NbtValue& e : pal->items)  // canonical full state keys, like ours
                    names.push_back(registry.name(registry.idFor(stateName(e))));
                const auto idx4096 = unpack(states->find("data"), int(names.size()), 4096, 4);
                for (int i = 0; i < 4096; ++i) {
                    const int x = i & 15, z = (i >> 4) & 15, y = sy * 16 + (i >> 8);
                    const std::string& want = names[size_t(idx4096[size_t(i)])];
                    const std::string& have = registry.name(chunk.get(x, y, z));
                    ++blocks;
                    if (want != have) {
                        if (chunkBad < 3)
                            LOG("gentest:   at %d %d %d expected %s, generated %s", cx * 16 + x, y, cz * 16 + z,
                                want.c_str(), have.c_str());
                        ++blockBad, ++chunkBad;
                        ++confusion[{want, have}];
                    }
                }
            }
            if (terrain)
                LOG("gentest: chunk %d,%d %s: %zu blocks differ (%.1f ms)", cx, cz, status->s.c_str(), chunkBad, tc.ms());
        }
    }
    libdeflate_free_decompressor(dec);

    LOG("gentest: %zu chunks | biomes %zu/%zu differ | blocks %zu/%zu differ", chunks, biomeBad, biomeCells, blockBad,
        blocks);
    LOG("gentest: per chunk biomes %.2f ms, fill %.2f ms, surface %.2f ms, carvers %.2f ms",
        timings.biomes / std::max<size_t>(chunks, 1), timings.fill / std::max<size_t>(chunks, 1),
        timings.surface / std::max<size_t>(chunks, 1), timings.carve / std::max<size_t>(chunks, 1));
    std::vector<std::pair<size_t, std::string>> top;
    for (const auto& [k, n] : confusion) top.push_back({n, k.first + "  ->  " + k.second});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < std::min<size_t>(top.size(), 60); ++i)
        LOG("gentest:   %8zu  expected %s", top[i].first, top[i].second.c_str());
    std::vector<std::pair<size_t, std::string>> topBiomes;
    for (const auto& [k, n] : badBiomes) topBiomes.push_back({n, k});
    std::sort(topBiomes.rbegin(), topBiomes.rend());
    for (size_t i = 0; i < std::min<size_t>(topBiomes.size(), 10); ++i)
        LOG("gentest:   %8zu  biome %s", topBiomes[i].first, topBiomes[i].second.c_str());
    int result = (biomeBad || blockBad) ? 2 : 0;
    if (!fullChunks.empty() && runDecorationTest(registry, gen, seed, fullChunks)) result = 2;
    return result;
}
