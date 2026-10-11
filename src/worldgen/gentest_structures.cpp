// Structure verification against a world the real server generated with structures (gentest mode
// GENTEST_STRUCTURES=starts|all): structure starts and their pieces' bounding boxes against every saved chunk's
// "structures.starts", references against "structures.References", and (all) the blocks of every saved full
// chunk after terrain (with the beardifier), decoration and structure placement, by category.
#include "worldgen/gentest.h"

#include "assets/assets.h"
#include "world/nbt_dom.h"
#include "worldgen/feature_api.h"
#include "worldgen/generator.h"
#include "worldgen/structures.h"

#include <libdeflate.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace {

u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

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
        const auto idx = unpack(states->find("data"), int(ids.size()), 4096, 4);
        for (int i = 0; i < 4096; ++i) out[size_t(sy * 16 - minY + (i >> 8)) * 256 + size_t(i & 255)] = ids[size_t(idx[size_t(i)])];
    }
    return out;
}

struct SavedPiece {
    std::string id;
    int bb[6];
};
struct SavedStart {
    std::string id;
    int cx, cz;
    std::vector<SavedPiece> pieces;
};
struct SavedChunk {
    int cx, cz;
    std::string status;
    std::vector<SavedStart> starts;
    std::set<std::string> references;  // "id@x,z"
    bool hasReferences = false;
    std::vector<u16> blocks;  // full chunks
};

int statusRank(const std::string& s) {
    static const char* kOrder[] = {"minecraft:empty", "minecraft:structure_starts", "minecraft:structure_references",
                                   "minecraft:biomes", "minecraft:noise", "minecraft:surface", "minecraft:carvers",
                                   "minecraft:features", "minecraft:initialize_light", "minecraft:light", "minecraft:spawn",
                                   "minecraft:full"};
    for (int i = 0; i < 12; ++i)
        if (s == kOrder[i]) return i;
    return -1;
}

}  // namespace

int runStructureTest(BlockRegistry& registry, const fs::path& jar, const fs::path& regionDir, i64 seed, const std::string& mode) {
    wg::DataPack data;
    if (!data.open(jar)) return 1;
    Timer t;
    wg::ChunkGenerator gen(data, registry, seed);
    if (!gen.ok()) return 1;
    wg::FeatureSystem features(gen.noise(), registry, gen.biomes(), gen.possibleBiomes());
    wg::StructureSystem structures(gen, &features, registry, seed);
    features.setStructures(&structures);
    LOG("structtest: generator, features and structures ready in %.1f ms", t.ms());

    // Optional area filter for the block comparison: GENTEST_AREA=x0,z0,x1,z1 (chunks).
    int area[4] = {INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX};
    if (const char* a = std::getenv("GENTEST_AREA")) std::sscanf(a, "%d,%d,%d,%d", &area[0], &area[1], &area[2], &area[3]);

    // ---- read the saved world ----
    std::vector<SavedChunk> saved;
    libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
    std::vector<u8> raw(size_t(8) << 20);
    std::error_code ec;
    for (const auto& file : fs::directory_iterator(regionDir, ec)) {
        if (file.path().extension() != ".mca") continue;
        std::vector<u8> region;
        if (!readFile(file.path(), region) || region.size() < 8192) continue;
        int rx, rz;
        if (std::sscanf(file.path().filename().string().c_str(), "r.%d.%d", &rx, &rz) != 2) continue;
        for (int idx = 0; idx < 1024; ++idx) {
            const u32 sector = be32(&region[size_t(idx) * 4]) >> 8;
            if (!sector) continue;
            const size_t pos = size_t(sector) * 4096;
            const u32 len = be32(&region[pos]);
            size_t got = 0;
            if (libdeflate_zlib_decompress(dec, &region[pos + 5], len - 1, raw.data(), raw.size(), &got) != LIBDEFLATE_SUCCESS) continue;
            NbtValue root;
            if (!nbtRead(raw.data(), got, root)) continue;
            SavedChunk c;
            c.cx = rx * 32 + (idx & 31), c.cz = rz * 32 + (idx >> 5);
            if (NbtValue* st = root.find("Status")) c.status = st->s;
            if (statusRank(c.status) < 1) continue;
            if (NbtValue* s = root.find("structures")) {
                if (NbtValue* starts = s->find("starts"))
                    for (size_t k = 0; k < starts->names.size(); ++k) {
                        NbtValue& v = starts->items[k];
                        SavedStart ss;
                        ss.id = starts->names[k];
                        ss.cx = v.find("ChunkX") ? int(v.find("ChunkX")->i) : c.cx;
                        ss.cz = v.find("ChunkZ") ? int(v.find("ChunkZ")->i) : c.cz;
                        if (NbtValue* children = v.find("Children"))
                            for (NbtValue& ch : children->items) {
                                SavedPiece p;
                                p.id = ch.find("id") ? ch.find("id")->s : "";
                                const NbtValue* bb = ch.find("BB");
                                for (int i = 0; i < 6; ++i) p.bb[i] = bb && size_t(i) < bb->ints.size() ? bb->ints[size_t(i)] : 0;
                                ss.pieces.push_back(std::move(p));
                            }
                        c.starts.push_back(std::move(ss));
                    }
                if (NbtValue* refs = s->find("References")) {
                    c.hasReferences = true;
                    for (size_t k = 0; k < refs->names.size(); ++k)
                        for (i64 key : refs->items[k].longs)
                            c.references.insert(refs->names[k] + "@" + std::to_string(int(i32(u32(u64(key))))) + "," +
                                                std::to_string(int(i32(u32(u64(key) >> 32)))));
                }
            }
            if ((c.status == "minecraft:full" || c.status == "minecraft:terrain") && c.cx >= area[0] && c.cz >= area[1] &&
                c.cx <= area[2] && c.cz <= area[3])
                c.blocks = readBlocks(root, registry, gen.minY(), gen.height());
            saved.push_back(std::move(c));
        }
    }
    libdeflate_free_decompressor(dec);
    LOG("structtest: %zu saved chunks read in %.1f ms", saved.size(), t.ms());

    // ---- starts (computed in parallel) ----
    t = Timer();
    std::vector<std::vector<wg::StructureSystem::StartReport>> ours(saved.size());
    {
        std::atomic<size_t> next{0};
        std::vector<std::thread> workers;
        const unsigned n = std::max(1u, std::thread::hardware_concurrency() - 2);
        for (unsigned i = 0; i < n; ++i)
            workers.emplace_back([&] {
                for (size_t k; (k = next++) < saved.size();) ours[k] = structures.startsIn(saved[k].cx, saved[k].cz);
            });
        for (auto& w : workers) w.join();
    }
    LOG("structtest: starts for %zu chunks in %.1f ms (%.1f ms of start generation)", saved.size(), t.ms(), structures.startMs());
    struct Stat {
        int theirs = 0, ours = 0, matched = 0, piecesExact = 0, pieceTheirs = 0, pieceMatched = 0;
    };
    std::map<std::string, Stat> stats;
    int reported = 0;
    for (size_t k = 0; k < saved.size(); ++k) {
        const SavedChunk& c = saved[k];
        for (const SavedStart& s : c.starts) {
            Stat& st = stats[s.id];
            ++st.theirs;
            st.pieceTheirs += int(s.pieces.size());
            auto it = std::find_if(ours[k].begin(), ours[k].end(), [&](const auto& o) { return o.id == s.id; });
            if (it == ours[k].end()) {
                if (reported++ < 40) LOG("structtest:   missing start %s at chunk %d,%d", s.id.c_str(), c.cx, c.cz);
                continue;
            }
            ++st.matched;
            // Pieces, in order.
            size_t same = 0;
            const size_t n = std::min(s.pieces.size(), it->pieces.size());
            size_t firstDiff = n;
            // Scattered and template pieces find their height when first placed: their saved box is moved since.
            auto placedHeight = [](const std::string& t) {
                return t == "minecraft:tedp" || t == "minecraft:tejp" || t == "minecraft:tesh" || t == "minecraft:iglu" ||
                       t == "minecraft:btp" || t == "minecraft:orp" || t == "minecraft:shipwreck";
            };
            for (size_t i = 0; i < n; ++i) {
                const int* a = s.pieces[i].bb;
                const int* b = it->pieces[i].box;
                const bool eq = placedHeight(s.pieces[i].id)
                                    ? a[0] == b[0] && a[2] == b[2] && a[3] == b[3] && a[5] == b[5] && a[4] - a[1] == b[4] - b[1]
                                    : std::equal(a, a + 6, b);
                if (eq) ++same;
                else if (firstDiff == n) firstDiff = i;
            }
            st.pieceMatched += int(same);
            if (same == s.pieces.size() && s.pieces.size() == it->pieces.size()) {
                ++st.piecesExact;
            } else if (reported++ < 60) {
                LOG("structtest:   %s at %d,%d: %zu/%zu pieces match (ours %zu); first difference at piece %zu", s.id.c_str(), c.cx,
                    c.cz, same, s.pieces.size(), it->pieces.size(), firstDiff);
                if (firstDiff < s.pieces.size()) {
                    const auto& p = s.pieces[firstDiff];
                    LOG("structtest:     theirs %s [%d %d %d %d %d %d]", p.id.c_str(), p.bb[0], p.bb[1], p.bb[2], p.bb[3], p.bb[4], p.bb[5]);
                }
                if (firstDiff < it->pieces.size()) {
                    const auto& p = it->pieces[firstDiff];
                    LOG("structtest:     ours   %s [%d %d %d %d %d %d] %s", p.type.c_str(), p.box[0], p.box[1], p.box[2], p.box[3],
                        p.box[4], p.box[5], p.detail.c_str());
                }
            }
        }
        for (const auto& o : ours[k]) {
            ++stats[o.id].ours;
            if (std::none_of(c.starts.begin(), c.starts.end(), [&](const SavedStart& s) { return s.id == o.id; }) && reported++ < 80)
                LOG("structtest:   extra start %s at chunk %d,%d", o.id.c_str(), c.cx, c.cz);
        }
    }
    int totalTheirs = 0, totalMatched = 0, totalOurs = 0, totalExact = 0;
    for (const auto& [id, s] : stats) {
        LOG("structtest: %-40s server %4d, ours %4d, matched %4d, pieces exact %4d (%d/%d pieces)", id.c_str(), s.theirs, s.ours,
            s.matched, s.piecesExact, s.pieceMatched, s.pieceTheirs);
        totalTheirs += s.theirs, totalMatched += s.matched, totalOurs += s.ours, totalExact += s.piecesExact;
    }
    LOG("structtest: starts: server %d, ours %d, matched %d, with identical pieces %d", totalTheirs, totalOurs, totalMatched, totalExact);

    // ---- references ----
    size_t refChunks = 0, refSame = 0, refMissing = 0, refExtra = 0;
    for (const SavedChunk& c : saved) {
        if (statusRank(c.status) < 2) continue;
        ++refChunks;
        std::set<std::string> mine;
        for (const std::string& r : structures.referencesOf(c.cx, c.cz)) mine.insert(r);
        bool same = true;
        for (const std::string& r : c.references)
            if (!mine.count(r)) ++refMissing, same = false;
        for (const std::string& r : mine)
            if (!c.references.count(r)) ++refExtra, same = false;
        refSame += same;
    }
    LOG("structtest: references: %zu chunks, %zu identical, %zu missing, %zu extra", refChunks, refSame, refMissing, refExtra);
    int result = (totalMatched == totalTheirs && totalOurs == totalTheirs) ? 0 : 2;
    if (mode != "all") return result;

    // ---- terrain (with the beardifier) of the chunks saved at "terrain" ----
    {
        t = Timer();
        std::vector<const SavedChunk*> terrainChunks;
        for (const SavedChunk& c : saved)
            if (!c.blocks.empty() && c.status == "minecraft:terrain") terrainChunks.push_back(&c);
        std::atomic<size_t> next{0}, bad{0}, badBeard{0}, total{0}, bearded{0};
        std::mutex m;
        std::map<std::pair<std::string, std::string>, size_t> conf;
        std::vector<std::thread> workers;
        const unsigned n = std::max(1u, std::thread::hardware_concurrency() - 2);
        for (unsigned i = 0; i < n; ++i)
            workers.emplace_back([&] {
                wg::DensityContext ctx;
                wg::GenChunk g;
                for (size_t k; (k = next++) < terrainChunks.size();) {
                    const SavedChunk& c = *terrainChunks[k];
                    std::unique_ptr<wg::DensityAddition> beard = structures.beardifier(c.cx, c.cz);
                    gen.generate(c.cx, c.cz, g, ctx, nullptr, beard.get());
                    size_t b = 0;
                    for (size_t j = 0; j < c.blocks.size(); ++j)
                        if (registry.name(c.blocks[j]) != registry.name(g.blocks[j])) {
                            ++b;
                            std::lock_guard lock(m);
                            ++conf[{registry.name(c.blocks[j]), registry.name(g.blocks[j])}];
                        }
                    bad += b, total += c.blocks.size();
                    if (beard) ++bearded, badBeard += b;
                }
            });
        for (auto& w : workers) w.join();
        LOG("structtest: terrain chunks: %zu, %zu of %zu blocks differ (%zu in the %zu chunks with terrain adaptation) in %.1f ms",
            terrainChunks.size(), bad.load(), total.load(), badBeard.load(), bearded.load(), t.ms());
        std::vector<std::pair<size_t, std::string>> v;
        for (const auto& [k, c] : conf) v.push_back({c, k.first + "  ->  " + k.second});
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < std::min<size_t>(v.size(), 12); ++i) LOG("structtest:   terrain %8zu  expected %s", v[i].first, v[i].second.c_str());
    }

    // ---- blocks of the full chunks ----
    std::vector<const SavedChunk*> full;
    for (const SavedChunk& c : saved)
        if (!c.blocks.empty() && c.status == "minecraft:full") full.push_back(&c);
    std::set<std::pair<int, int>> decorate, terrain;
    for (const SavedChunk* c : full)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) decorate.insert({c->cx + dx, c->cz + dz});
    for (const auto& [x, z] : decorate)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) terrain.insert({x + dx, z + dz});
    std::map<std::pair<int, int>, std::unique_ptr<wg::GenChunk>> chunks;
    for (const auto& k : terrain) chunks[k] = std::make_unique<wg::GenChunk>();
    t = Timer();
    {
        std::vector<std::pair<int, int>> list(terrain.begin(), terrain.end());
        std::atomic<size_t> next{0};
        std::atomic<size_t> bearded{0};
        std::vector<std::thread> workers;
        const unsigned n = std::max(1u, std::thread::hardware_concurrency() - 2);
        for (unsigned i = 0; i < n; ++i)
            workers.emplace_back([&] {
                wg::DensityContext ctx;
                for (size_t k; (k = next++) < list.size();) {
                    const auto [x, z] = list[k];
                    std::unique_ptr<wg::DensityAddition> beard = structures.beardifier(x, z);
                    if (beard) ++bearded;
                    gen.generate(x, z, *chunks[{x, z}], ctx, nullptr, beard.get());
                }
            });
        for (auto& w : workers) w.join();
        LOG("structtest: terrain for %zu chunks (%zu with terrain adaptation) in %.1f ms", list.size(), bearded.load(), t.ms());
    }
    wg::DensityContext ctx;
    wg::BiomeZoom zoom(seed);
    const wg::NoiseBiomeFn noiseBiome = [&](int qx, int qy, int qz) { return gen.noiseBiome(ctx, qx, qy, qz); };
    t = Timer();
    for (const auto& [x, z] : decorate) {
        std::array<wg::GenChunk*, 9> region{};
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) region[size_t((dz + 1) * 3 + dx + 1)] = chunks[{x + dx, z + dz}].get();
        features.decorate(region, zoom, noiseBiome);
    }
    LOG("structtest: decorated %zu chunks in %.1f ms (%.2f ms each)", decorate.size(), t.ms(), t.ms() / double(decorate.size()));

    // Categories: blocks inside a structure piece of a start referencing the chunk (by structure) or not.
    struct BlockStat {
        size_t total = 0, bad = 0;
    };
    std::map<std::string, BlockStat> byStructure;
    BlockStat outside, all;
    std::map<std::pair<std::string, std::string>, size_t> confusion, confusionInside;
    FILE* dump = nullptr;
    if (const char* path = std::getenv("GENTEST_DUMP")) dump = std::fopen(path, "w");
    for (const SavedChunk* c : full) {
        const wg::GenChunk& g = *chunks[{c->cx, c->cz}];
        // Our pieces near this chunk.
        struct Box {
            std::string id;
            int b[6];
        };
        std::vector<Box> boxes;
        for (const std::string& r : structures.referencesOf(c->cx, c->cz)) {
            const std::string id = r.substr(0, r.find('@'));
            int sx = 0, sz = 0;
            std::sscanf(r.c_str() + r.find('@') + 1, "%d,%d", &sx, &sz);
            for (const auto& s : structures.startsIn(sx, sz))
                if (s.id == id)
                    for (const auto& p : s.pieces) boxes.push_back({id, {p.box[0], p.box[1], p.box[2], p.box[3], p.box[4], p.box[5]}});
        }
        for (size_t i = 0; i < c->blocks.size(); ++i) {
            const int x = c->cx * 16 + int(i & 15), y = g.minY + int(i >> 8), z = c->cz * 16 + int((i >> 4) & 15);
            const std::string* in = nullptr;
            for (const Box& b : boxes)
                if (x >= b.b[0] && x <= b.b[3] && y >= b.b[1] && y <= b.b[4] && z >= b.b[2] && z <= b.b[5]) {
                    in = &b.id;
                    break;
                }
            BlockStat& cat = in ? byStructure[*in] : outside;
            ++cat.total, ++all.total;
            const std::string& want = registry.name(c->blocks[i]);
            const std::string& have = registry.name(g.blocks[i]);
            if (want == have) continue;
            ++cat.bad, ++all.bad;
            ++(in ? confusionInside : confusion)[{want, have}];
            if (dump) std::fprintf(dump, "%d %d %d %s %s %s\n", x, y, z, want.c_str(), have.c_str(), in ? in->c_str() : "-");
        }
    }
    if (dump) std::fclose(dump);
    LOG("structtest: blocks: %zu full chunks, %zu of %zu blocks differ (%.4f%%)", full.size(), all.bad, all.total,
        100.0 * double(all.bad) / double(std::max<size_t>(all.total, 1)));
    LOG("structtest:   outside structures: %zu of %zu differ", outside.bad, outside.total);
    for (const auto& [id, s] : byStructure)
        LOG("structtest:   inside %-36s %8zu of %9zu differ (%.3f%%)", id.c_str(), s.bad, s.total, 100.0 * double(s.bad) / double(std::max<size_t>(s.total, 1)));
    auto top = [](const std::map<std::pair<std::string, std::string>, size_t>& m, const char* label) {
        std::vector<std::pair<size_t, std::string>> v;
        for (const auto& [k, n] : m) v.push_back({n, k.first + "  ->  " + k.second});
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < std::min<size_t>(v.size(), 30); ++i) LOG("structtest:   %s %8zu  expected %s", label, v[i].first, v[i].second.c_str());
    };
    top(confusionInside, "in ");
    top(confusion, "out");
    return result;
}
