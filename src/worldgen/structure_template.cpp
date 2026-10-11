// Structure templates and processors: StructureTemplate (load, transform, getJigsaws, filterBlocks,
// placeInWorld with waterlogging), StructureProcessor types (rule, block_rot, gravity, protected_blocks, capped,
// block_ignore, block_age, blackstone_replace, lava_submerged_block, jigsaw_replacement) and the shape updates
// templates trigger.
#include "worldgen/structure_template.h"

#include "assets/assets.h"

#include <libdeflate.h>

#include <algorithm>

namespace wg::st {

BlockPos transformPos(const BlockPos& p, Mir m, Rot r, const BlockPos& pivot) {
    int x = p.x, y = p.y, z = p.z;
    bool mirrored = true;
    switch (m) {
    case Mir::LeftRight: z = -z; break;
    case Mir::FrontBack: x = -x; break;
    default: mirrored = false;
    }
    const int px = pivot.x, pz = pivot.z;
    switch (r) {
    case Rot::CCW90: return {px - pz + z, y, px + pz - x};
    case Rot::CW90: return {px + pz - z, y, pz - px + x};
    case Rot::CW180: return {px + px - x, y, pz + pz - z};
    default: return mirrored ? BlockPos{x, y, z} : p;
    }
}

BlockPos zeroPositionWithTransform(const BlockPos& zero, Mir m, Rot r, int sizeX, int sizeZ) {
    --sizeX, --sizeZ;
    const int mx = m == Mir::FrontBack ? sizeX : 0, mz = m == Mir::LeftRight ? sizeZ : 0;
    switch (r) {
    case Rot::CCW90: return zero.offset(mz, 0, sizeX - mx);
    case Rot::CW90: return zero.offset(sizeZ - mz, 0, mx);
    case Rot::CW180: return zero.offset(sizeX - mx, 0, sizeZ - mz);
    default: return zero.offset(mx, 0, mz);
    }
}

BBox templateBox(const BlockPos& position, Rot r, const BlockPos& pivot, Mir m, const BlockPos& size) {
    const BlockPos a = transformPos({0, 0, 0}, m, r, pivot);
    const BlockPos b = transformPos({size.x - 1, size.y - 1, size.z - 1}, m, r, pivot);
    return BBox::fromCorners(a, b).move(position.x, position.y, position.z);
}

// ---- loading -----------------------------------------------------------------------------------------------

namespace {

bool gunzip(const std::vector<u8>& in, std::vector<u8>& out) {
    if (in.size() < 18) return false;
    const size_t n = size_t(in[in.size() - 4]) | size_t(in[in.size() - 3]) << 8 | size_t(in[in.size() - 2]) << 16 |
                     size_t(in[in.size() - 1]) << 24;
    out.resize(std::max<size_t>(n, 1));
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    size_t actual = 0;
    const libdeflate_result res = libdeflate_gzip_decompress(d, in.data(), in.size(), out.data(), out.size(), &actual);
    libdeflate_free_decompressor(d);
    out.resize(actual);
    return res == LIBDEFLATE_SUCCESS;
}

NbtValue* child(NbtValue* v, std::string_view k) { return v ? v->find(k) : nullptr; }
const NbtValue* child(const NbtValue* v, std::string_view k) { return v ? const_cast<NbtValue*>(v)->find(k) : nullptr; }
std::string str(const NbtValue* v, std::string_view k, std::string def = "") {
    const NbtValue* e = child(v, k);
    return e && e->type == TagString ? e->s : def;
}
int integer(const NbtValue* v, std::string_view k, int def = 0) {
    const NbtValue* e = child(v, k);
    return e ? int(e->i) : def;
}
std::string withNamespace(std::string s) { return s.find(':') == std::string::npos ? "minecraft:" + s : s; }

int listInt(const NbtValue* list, size_t i) {
    if (!list) return 0;
    if (!list->ints.empty()) return i < list->ints.size() ? list->ints[i] : 0;
    return i < list->items.size() ? int(list->items[i].i) : 0;
}

}  // namespace

const Template* TemplateManager::get(std::string_view rawId) const {
    const std::string id = withNamespace(std::string(rawId));
    std::lock_guard lock(mutex_);
    if (auto it = cache_.find(id); it != cache_.end()) return it->second.get();
    auto t = std::make_unique<Template>();
    t->id = id;
    const size_t colon = id.find(':');
    const std::string path = "data/" + id.substr(0, colon) + "/structure/" + id.substr(colon + 1) + ".nbt";
    std::vector<u8> raw, bytes;
    NbtValue root;
    if (!env_.data.readFile(path, raw) || !gunzip(raw, bytes) || !nbtRead(bytes.data(), bytes.size(), root)) {
        LOG("structures: template %s not found or unreadable", id.c_str());
        return (cache_[id] = std::move(t)).get();
    }
    const SB& b = env_.b;
    if (NbtValue* s = root.find("size")) t->size = {listInt(s, 0), listInt(s, 1), listInt(s, 2)};
    if (NbtValue* e = root.find("entities")) t->hasEntities = !e->items.empty();
    std::vector<NbtValue*> palettes;
    if (NbtValue* ps = root.find("palettes"))
        for (NbtValue& p : ps->items) palettes.push_back(&p);
    else if (NbtValue* p = root.find("palette"))
        palettes.push_back(p);
    NbtValue* blocks = root.find("blocks");
    // Block entity data, shared by every palette.
    std::vector<const NbtValue*> nbtOf;
    if (blocks)
        for (NbtValue& e : blocks->items) {
            NbtValue* n = e.find("nbt");
            if (n) {
                t->nbtStore.push_back(std::make_unique<NbtValue>(std::move(*n)));
                nbtOf.push_back(t->nbtStore.back().get());
            } else {
                nbtOf.push_back(nullptr);
            }
        }
    for (NbtValue* pal : palettes) {
        std::vector<u16> states;
        for (NbtValue& e : pal->items) {
            std::string name = str(&e, "Name", str(&e, "id", "minecraft:air"));  // 26.x templates: "id" / "properties"
            std::vector<std::pair<std::string, std::string>> kv;
            NbtValue* props = e.find("Properties");
            if (!props) props = e.find("properties");
            if (props)
                for (size_t k = 0; k < props->names.size(); ++k) kv.push_back({props->names[k], props->items[k].s});
            std::sort(kv.begin(), kv.end());
            std::string key = withNamespace(name);
            if (!kv.empty()) {
                key += '[';
                for (size_t k = 0; k < kv.size(); ++k) key += (k ? "," : "") + kv[k].first + "=" + kv[k].second;
                key += ']';
            }
            states.push_back(b.reg.idFor(key));
        }
        std::vector<TemplateBlock> full, other, entities;
        if (blocks)
            for (size_t i = 0; i < blocks->items.size(); ++i) {
                NbtValue& e = blocks->items[i];
                NbtValue* pos = e.find("pos");
                const int si = integer(&e, "state");
                TemplateBlock tb{{listInt(pos, 0), listInt(pos, 1), listInt(pos, 2)},
                                 si >= 0 && size_t(si) < states.size() ? states[size_t(si)] : b.air, nbtOf[i]};
                if (tb.nbt)
                    entities.push_back(tb);
                else if (b.fullCollision(tb.state))
                    full.push_back(tb);
                else
                    other.push_back(tb);
            }
        auto byYXZ = [](const TemplateBlock& a, const TemplateBlock& c) {
            if (a.pos.y != c.pos.y) return a.pos.y < c.pos.y;
            if (a.pos.x != c.pos.x) return a.pos.x < c.pos.x;
            return a.pos.z < c.pos.z;
        };
        std::stable_sort(full.begin(), full.end(), byYXZ);
        std::stable_sort(other.begin(), other.end(), byYXZ);
        std::stable_sort(entities.begin(), entities.end(), byYXZ);
        Template::Palette p;
        p.blocks = std::move(full);
        p.blocks.insert(p.blocks.end(), other.begin(), other.end());
        p.blocks.insert(p.blocks.end(), entities.begin(), entities.end());
        for (size_t i = 0; i < p.blocks.size(); ++i) {
            const TemplateBlock& tb = p.blocks[i];
            if (b.isIdx(tb.state, b.blockIndex("minecraft:structure_block"))) p.structureBlocks.push_back(i);
            if (!b.isIdx(tb.state, b.blockIndex("minecraft:jigsaw"))) continue;
            p.jigsawBlocks.push_back(i);
            JigsawInfo j;
            j.pos = tb.pos;
            j.state = tb.state;
            const std::string orientation(b.prop(tb.state, "orientation"));
            const size_t us = orientation.find('_');
            j.front = dirFromName(orientation.substr(0, us)).value_or(North);
            j.top = dirFromName(us == std::string::npos ? "up" : orientation.substr(us + 1)).value_or(Up);
            const std::string joint = str(tb.nbt, "joint");
            j.rollable = joint.empty() ? !horizontal(j.front) : joint == "rollable";
            j.name = withNamespace(str(tb.nbt, "name", "minecraft:empty"));
            j.pool = withNamespace(str(tb.nbt, "pool", "minecraft:empty"));
            j.target = withNamespace(str(tb.nbt, "target", "minecraft:empty"));
            j.placementPriority = integer(tb.nbt, "placement_priority");
            j.selectionPriority = integer(tb.nbt, "selection_priority");
            p.jigsaws.push_back(std::move(j));
        }
        t->palettes.push_back(std::move(p));
    }
    static const bool debug = std::getenv("MCPP_STRUCT_DEBUG") != nullptr;
    if (debug && !t->palettes.empty()) {
        LOG("template %s: size %d %d %d, %zu palettes, %zu blocks, %zu jigsaws", id.c_str(), t->size.x, t->size.y, t->size.z,
            t->palettes.size(), t->palettes[0].blocks.size(), t->palettes[0].jigsaws.size());
        for (const JigsawInfo& j : t->palettes[0].jigsaws)
            LOG("  jigsaw at %d %d %d front %d top %d name %s pool %s target %s", j.pos.x, j.pos.y, j.pos.z, int(j.front), int(j.top),
                j.name.c_str(), j.pool.c_str(), j.target.c_str());
    }
    return (cache_[id] = std::move(t)).get();
}

// ---- settings ----------------------------------------------------------------------------------------------

const Template::Palette& PlaceSettings::palette(const Template& t, const BlockPos& pos) const {
    const int n = int(t.palettes.size());
    if (random) return t.palettes[size_t(random->nextInt(n))];
    LegacyRandom r(positionSeed(pos.x, pos.y, pos.z));
    return t.palettes[size_t(r.nextInt(n))];
}

float PlaceSettings::randomFloatAt(const BlockPos& p) const {
    if (random) return random->nextFloat();
    LegacyRandom r(positionSeed(p.x, p.y, p.z));
    return r.nextFloat();
}

std::vector<JigsawInfo> templateJigsaws(const Template& t, const BlockPos& position, Rot r, const SB& b) {
    std::vector<JigsawInfo> out;
    if (t.palettes.empty()) return out;
    PlaceSettings s;
    s.rotation = r;
    const Template::Palette& p = s.palette(t, position);
    out.reserve(p.jigsaws.size());
    for (const JigsawInfo& j : p.jigsaws) {
        JigsawInfo o = j;
        o.pos = transformPos(j.pos, Mir::None, r, {0, 0, 0}).offset(position.x, position.y, position.z);
        o.state = b.rotate(j.state, r);
        o.front = rotate(r, j.front);
        o.top = rotate(r, j.top);
        out.push_back(std::move(o));
    }
    return out;
}

std::vector<ProcInfo> filterBlocks(const Template& t, const BlockPos& position, const PlaceSettings& s, u16 blockState,
                                   const SB& b) {
    std::vector<ProcInfo> out;
    if (t.palettes.empty()) return out;
    const Template::Palette& p = s.palette(t, position);
    const u16 kind = b.f.blockOf(blockState);
    for (const TemplateBlock& tb : p.blocks) {
        if (!b.isIdx(tb.state, kind)) continue;
        const BlockPos wp = transformPos(tb.pos, s.mirror, s.rotation, s.pivot).offset(position.x, position.y, position.z);
        if (s.box && !s.box->isInside(wp)) continue;
        out.push_back({wp, b.rotate(tb.state, s.rotation), tb.nbt != nullptr, tb.nbt});
    }
    return out;
}

// ---- processors -------------------------------------------------------------------------------------------

namespace {

class PosRuleTest {
public:
    virtual ~PosRuleTest() = default;
    virtual bool test(const BlockPos& inTemplate, const BlockPos& world, const BlockPos& ref, RandomSource& r) const = 0;
};
class PosAlwaysTrue final : public PosRuleTest {
public:
    bool test(const BlockPos&, const BlockPos&, const BlockPos&, RandomSource&) const override { return true; }
};
class LinearPos final : public PosRuleTest {
public:
    LinearPos(float mn, float mx, int d0, int d1, int axis) : minChance(mn), maxChance(mx), minDist(d0), maxDist(d1), axis(axis) {}
    bool test(const BlockPos&, const BlockPos& w, const BlockPos& ref, RandomSource& r) const override {
        int dist;
        if (axis < 0) {
            dist = std::abs(w.x - ref.x) + std::abs(w.y - ref.y) + std::abs(w.z - ref.z);
        } else {
            const float xd = std::fabs(float((w.x - ref.x) * (axis == 0)));
            const float yd = std::fabs(float((w.y - ref.y) * (axis == 1)));
            const float zd = std::fabs(float((w.z - ref.z) * (axis == 2)));
            dist = int(xd + yd + zd);
        }
        const float rnd = r.nextFloat();
        return rnd <= clampedLerp(inverseLerp(float(dist), float(minDist), float(maxDist)), minChance, maxChance);
    }
    float minChance, maxChance;
    int minDist, maxDist, axis;  // axis: -1 linear_pos, 0..2 axis_aligned x/y/z
};

std::unique_ptr<PosRuleTest> parsePosRule(const json& j) {
    const std::string t = DataPack::normalize(j.value("predicate_type", std::string("always_true")));
    if (t == "linear_pos" || t == "axis_aligned_linear_pos") {
        int axis = -1;
        if (t == "axis_aligned_linear_pos") {
            const std::string a = j.value("axis", std::string("y"));
            axis = a == "x" ? 0 : a == "z" ? 2 : 1;
        }
        return std::make_unique<LinearPos>(j.value("min_chance", 0.0f), j.value("max_chance", 0.0f), j.value("min_dist", 0),
                                           j.value("max_dist", 0), axis);
    }
    return std::make_unique<PosAlwaysTrue>();
}

struct Rule {
    std::shared_ptr<const RuleTest> input, location;
    std::unique_ptr<PosRuleTest> position;
    u16 output;
    enum class Modifier { Passthrough, Clear, AppendStatic, AppendLoot } modifier = Modifier::Passthrough;
};

class RuleProcessor final : public Processor {
public:
    std::vector<Rule> rules;
    bool process(PlaceCtx& c, const BlockPos&, const BlockPos& ref, const BlockPos& rel, ProcInfo& info,
                 const PlaceSettings&) const override {
        LegacyRandom r(positionSeed(info.pos.x, info.pos.y, info.pos.z));
        for (const Rule& rule : rules) {
            if (!rule.input->test(info.state, info.pos, r)) continue;
            if (!rule.location->test(c.get(info.pos), info.pos, r)) continue;
            if (!rule.position->test(rel, info.pos, ref, r)) continue;
            info.state = rule.output;
            switch (rule.modifier) {
            case Rule::Modifier::Passthrough: break;
            case Rule::Modifier::Clear: info.hasNbt = true, info.nbt = nullptr; break;
            case Rule::Modifier::AppendStatic: info.hasNbt = true; break;
            case Rule::Modifier::AppendLoot:
                r.nextLong();
                info.hasNbt = true;
                info.nbt = nullptr;
                break;
            }
            return true;
        }
        return true;
    }
};

class BlockRotProcessor final : public Processor {
public:
    std::optional<BlockBits> rottable;
    float integrity = 1.0f;
    bool process(PlaceCtx& c, const BlockPos&, const BlockPos&, const BlockPos&, ProcInfo& info, const PlaceSettings& s) const override {
        // getRandom(pos) is created (or the shared random consumed) whatever the block.
        if (s.random) {
            const bool inSet = !rottable || c.b.f.in(info.state, *rottable);
            return !(inSet && !(s.random->nextFloat() <= integrity));
        }
        LegacyRandom r(positionSeed(info.pos.x, info.pos.y, info.pos.z));
        if (rottable && !c.b.f.in(info.state, *rottable)) return true;
        return r.nextFloat() <= integrity;
    }
};

class ProtectedProcessor final : public Processor {
public:
    BlockBits set;
    bool process(PlaceCtx& c, const BlockPos&, const BlockPos&, const BlockPos&, ProcInfo& info, const PlaceSettings&) const override {
        return !c.b.f.in(c.get(info.pos), set);
    }
};

class IgnoreProcessor final : public Processor {
public:
    std::vector<u16> blocks;  // block indices
    bool process(PlaceCtx& c, const BlockPos&, const BlockPos&, const BlockPos&, ProcInfo& info, const PlaceSettings&) const override {
        const u16 k = c.b.f.blockOf(info.state);
        return std::find(blocks.begin(), blocks.end(), k) == blocks.end();
    }
};

class GravityProcessor final : public Processor {
public:
    HType heightmap = HType::WorldSurfaceWG;
    int offset = 0;
    bool process(PlaceCtx& c, const BlockPos&, const BlockPos&, const BlockPos& rel, ProcInfo& info, const PlaceSettings&) const override {
        const int h = c.height(Heightmap(int(heightmap)), info.pos.x, info.pos.z) + offset;
        info.pos.y = h + rel.y;
        return true;
    }
};

class JigsawReplacement final : public Processor {
public:
    bool process(PlaceCtx& c, const BlockPos&, const BlockPos&, const BlockPos&, ProcInfo& info, const PlaceSettings&) const override {
        if (info.state != c.b.jigsaw && !c.b.is(info.state, "minecraft:jigsaw")) return true;
        if (!info.hasNbt || !info.nbt) return true;
        const NbtValue* fs = const_cast<NbtValue*>(info.nbt)->find("final_state");
        const std::string s = fs ? fs->s : std::string("minecraft:air");
        const u16 state = c.b.key(s);
        if (c.b.is(state, "minecraft:structure_void")) return false;
        info.state = state;
        info.hasNbt = false;
        info.nbt = nullptr;
        return true;
    }
};

class CappedProcessor final : public Processor {
public:
    std::shared_ptr<const Processor> delegate;
    std::unique_ptr<IntProvider> limit;
    bool entirePiece() const override { return true; }
    void finalize(PlaceCtx& c, const BlockPos& position, const BlockPos& ref, const std::vector<ProcInfo>& originals,
                  std::vector<ProcInfo>& processed, const PlaceSettings& s) const override {
        if (limit->maxValue() == 0 || processed.empty() || originals.size() != processed.size()) return;
        LegacyRandom base(c.env.seed);
        const auto factory = base.forkPositional();
        const auto r = factory->at(position.x, position.y, position.z);
        const int maxToReplace = std::min(limit->sample(*r), int(processed.size()));
        if (maxToReplace < 1) return;
        std::vector<int> indices(processed.size());
        for (size_t i = 0; i < indices.size(); ++i) indices[i] = int(i);
        for (size_t i = indices.size(); i > 1; --i) std::swap(indices[i - 1], indices[size_t(r->nextInt(int(i)))]);
        int replaced = 0;
        static const bool debug = std::getenv("MCPP_CAPPED_DEBUG") != nullptr;
        if (const char* d = std::getenv("MCPP_CAPPED_DUMP")) {
            int dx, dy, dz;
            if (std::sscanf(d, "%d,%d,%d", &dx, &dy, &dz) == 3 && dx == position.x && dy == position.y && dz == position.z) {
                static std::mutex m;
                std::lock_guard lock(m);
                static int n = 0;
                if (n++ == 0)
                    for (size_t i = 0; i < processed.size(); ++i)
                        LOG("capdump %zu %d %d %d %s %d %d %d", i, processed[i].pos.x, processed[i].pos.y, processed[i].pos.z,
                            c.b.name(processed[i].state).c_str(), originals[i].pos.x, originals[i].pos.y, originals[i].pos.z);
            }
        }
        for (size_t k = 0; k < indices.size() && replaced < maxToReplace; ++k) {
            const int index = indices[k];
            ProcInfo altered = processed[size_t(index)];
            const bool kept = delegate->process(c, position, ref, originals[size_t(index)].pos, altered, s);
            if (kept && !(altered == processed[size_t(index)])) {
                ++replaced;
                processed[size_t(index)] = altered;
                if (debug)
                    LOG("capped at %d %d %d (list %zu, max %d): index %d -> %d %d %d", position.x, position.y, position.z, processed.size(),
                        maxToReplace, index, altered.pos.x, altered.pos.y, altered.pos.z);
            }
        }
    }
};

class BlockAgeProcessor final : public Processor {
public:
    float mossiness = 0;
    bool process(PlaceCtx& c, const BlockPos&, const BlockPos&, const BlockPos&, ProcInfo& info, const PlaceSettings& s) const override {
        std::unique_ptr<LegacyRandom> own;
        RandomSource* r = s.random;
        if (!r) own = std::make_unique<LegacyRandom>(positionSeed(info.pos.x, info.pos.y, info.pos.z)), r = own.get();
        const SB& b = c.b;
        const u16 st = info.state;
        std::optional<u16> ns;
        auto randomFacingStairs = [&](const char* block) {
            u16 v = b.def(block);
            v = b.with(v, "facing", kPlaneHorizontal[r->nextInt(4)]);
            return b.with(v, "half", r->nextInt(2) == 0 ? "top" : "bottom");
        };
        auto pick = [&](const std::array<u16, 2>& nonMossy, const std::array<u16, 2>& mossy) {
            return r->nextFloat() < mossiness ? mossy[size_t(r->nextInt(2))] : nonMossy[size_t(r->nextInt(2))];
        };
        if (b.is(st, "minecraft:stone_bricks") || b.is(st, "minecraft:stone") || b.is(st, "minecraft:chiseled_stone_bricks")) {
            if (!(r->nextFloat() >= 0.5f)) {
                const u16 n0 = b.def("minecraft:cracked_stone_bricks");
                const u16 n1 = randomFacingStairs("minecraft:stone_brick_stairs");
                const u16 m0 = b.def("minecraft:mossy_stone_bricks");
                const u16 m1 = randomFacingStairs("minecraft:mossy_stone_brick_stairs");
                ns = pick({n0, n1}, {m0, m1});
            }
        } else if (b.tag(st, "minecraft:stairs")) {
            if (!(r->nextFloat() >= 0.5f)) {
                const std::array<u16, 2> nonMossy{b.def("minecraft:stone_slab"), b.def("minecraft:stone_brick_slab")};
                const std::array<u16, 2> mossy{b.withPropertiesOf(b.def("minecraft:mossy_stone_brick_stairs"), st),
                                               b.def("minecraft:mossy_stone_brick_slab")};
                ns = pick(nonMossy, mossy);
            }
        } else if (b.tag(st, "minecraft:slabs")) {
            if (r->nextFloat() < mossiness) ns = b.withPropertiesOf(b.def("minecraft:mossy_stone_brick_slab"), st);
        } else if (b.tag(st, "minecraft:walls")) {
            if (r->nextFloat() < mossiness) ns = b.withPropertiesOf(b.def("minecraft:mossy_stone_brick_wall"), st);
        } else if (b.is(st, "minecraft:obsidian")) {
            if (r->nextFloat() < 0.15f) ns = b.def("minecraft:crying_obsidian");
        }
        if (ns) info.state = *ns;
        return true;
    }
};

class BlackstoneProcessor final : public Processor {
public:
    bool process(PlaceCtx& c, const BlockPos&, const BlockPos&, const BlockPos&, ProcInfo& info, const PlaceSettings&) const override {
        static const std::pair<const char*, const char*> kMap[] = {
            {"cobblestone", "blackstone"}, {"mossy_cobblestone", "blackstone"}, {"stone", "polished_blackstone"},
            {"stone_bricks", "polished_blackstone_bricks"}, {"mossy_stone_bricks", "polished_blackstone_bricks"},
            {"cobblestone_stairs", "blackstone_stairs"}, {"mossy_cobblestone_stairs", "blackstone_stairs"},
            {"stone_stairs", "polished_blackstone_stairs"}, {"stone_brick_stairs", "polished_blackstone_brick_stairs"},
            {"mossy_stone_brick_stairs", "polished_blackstone_brick_stairs"}, {"cobblestone_slab", "blackstone_slab"},
            {"mossy_cobblestone_slab", "blackstone_slab"}, {"smooth_stone_slab", "polished_blackstone_slab"},
            {"stone_slab", "polished_blackstone_slab"}, {"stone_brick_slab", "polished_blackstone_brick_slab"},
            {"mossy_stone_brick_slab", "polished_blackstone_brick_slab"}, {"stone_brick_wall", "polished_blackstone_brick_wall"},
            {"mossy_stone_brick_wall", "polished_blackstone_brick_wall"}, {"cobblestone_wall", "blackstone_wall"},
            {"mossy_cobblestone_wall", "blackstone_wall"}, {"chiseled_stone_bricks", "chiseled_polished_blackstone"},
            {"cracked_stone_bricks", "cracked_polished_blackstone_bricks"}, {"iron_bars", "iron_chain"}};
        const SB& b = c.b;
        const std::string_view n = b.blockName(info.state);
        for (const auto& [from, to] : kMap) {
            if (n.substr(n.find(':') + 1) != from) continue;
            const u16 old = info.state;
            u16 ns = b.def(std::string("minecraft:") + to);
            if (b.has(old, "facing") && b.has(ns, "facing")) ns = b.with(ns, "facing", b.prop(old, "facing"));
            if (b.has(old, "half") && b.has(ns, "half")) ns = b.with(ns, "half", b.prop(old, "half"));
            if (b.has(old, "type") && b.has(ns, "type")) ns = b.with(ns, "type", b.prop(old, "type"));
            info.state = ns;
            break;
        }
        return true;
    }
};

class LavaSubmergedProcessor final : public Processor {
public:
    bool process(PlaceCtx& c, const BlockPos&, const BlockPos&, const BlockPos&, ProcInfo& info, const PlaceSettings&) const override {
        if (c.b.is(c.get(info.pos), "minecraft:lava") && !c.b.fullOutline(info.state)) {
            info.state = c.b.lava;
        }
        return true;
    }
};

class NopProcessor final : public Processor {};

std::shared_ptr<const Processor> parseProcessor(const StructureEnv& env, const json& pj) {
    const std::string type = DataPack::normalize(pj.value("processor_type", std::string()));
    const SB& b = env.b;
    const FeatureEnv* fe = env.featureEnv();
    if (type == "rule") {
        auto p = std::make_shared<RuleProcessor>();
        for (const json& rj : pj.at("rules")) {
            Rule r;
            r.input = fe->ruleTest(rj.at("input_predicate"));
            r.location = fe->ruleTest(rj.at("location_predicate"));
            r.position = parsePosRule(rj.value("position_predicate", json::object()));
            r.output = b.state(rj.at("output_state"));
            if (auto it = rj.find("block_entity_modifier"); it != rj.end()) {
                const std::string m = DataPack::normalize(it->value("type", std::string()));
                r.modifier = m == "clear" ? Rule::Modifier::Clear : m == "append_static" ? Rule::Modifier::AppendStatic
                             : m == "append_loot" ? Rule::Modifier::AppendLoot : Rule::Modifier::Passthrough;
            }
            p->rules.push_back(std::move(r));
        }
        return p;
    }
    if (type == "block_rot") {
        auto p = std::make_shared<BlockRotProcessor>();
        p->integrity = pj.value("integrity", 1.0f);
        if (pj.contains("rottable_blocks")) p->rottable = b.f.holderSet(pj["rottable_blocks"]);
        return p;
    }
    if (type == "protected_blocks") {
        auto p = std::make_shared<ProtectedProcessor>();
        p->set = b.f.holderSet(pj.at("value"));
        return p;
    }
    if (type == "block_ignore") {
        auto p = std::make_shared<IgnoreProcessor>();
        for (const json& s : pj.at("blocks")) p->blocks.push_back(b.f.blockOf(b.state(s)));
        return p;
    }
    if (type == "gravity") {
        auto p = std::make_shared<GravityProcessor>();
        p->heightmap = htypeFromName(pj.value("heightmap", std::string("WORLD_SURFACE_WG")));
        p->offset = pj.value("offset", 0);
        return p;
    }
    if (type == "capped") {
        auto p = std::make_shared<CappedProcessor>();
        p->delegate = parseProcessor(env, pj.at("delegate"));
        p->limit = fe->intProvider(pj.at("limit"));
        return p;
    }
    if (type == "block_age") return blockAge(pj.value("mossiness", 0.0f));
    if (type == "blackstone_replace") return blackstoneReplace();
    if (type == "lava_submerged_block") return lavaSubmerged();
    if (type == "jigsaw_replacement") return jigsawReplacement();
    if (type != "nop") LOG("structures: unknown structure processor %s", type.c_str());
    return std::make_shared<NopProcessor>();
}

}  // namespace

ProcessorList parseProcessors(const StructureEnv& env, const json& j) {
    const json* list = &j;
    if (j.is_string()) {
        list = env.data.worldgen("processor_list", j.get<std::string>());
        if (!list) {
            LOG("structures: unknown processor list %s", j.get<std::string>().c_str());
            return {};
        }
    }
    if (list->is_object()) {
        if (!list->contains("processors")) return {};
        list = &(*list)["processors"];
    }
    ProcessorList out;
    if (list->is_array())
        for (const json& pj : *list) out.push_back(parseProcessor(env, pj));
    return out;
}

std::shared_ptr<const Processor> blockIgnore(const StructureEnv& env, std::vector<std::string> blocks) {
    auto p = std::make_shared<IgnoreProcessor>();
    for (const std::string& n : blocks) p->blocks.push_back(env.b.blockIndex(n));
    return p;
}
std::shared_ptr<const Processor> jigsawReplacement() {
    static const auto p = std::make_shared<JigsawReplacement>();
    return p;
}
std::shared_ptr<const Processor> gravity(HType h, int offset) {
    auto p = std::make_shared<GravityProcessor>();
    p->heightmap = h, p->offset = offset;
    return p;
}
std::shared_ptr<const Processor> blockRot(float integrity) {
    auto p = std::make_shared<BlockRotProcessor>();
    p->integrity = integrity;
    return p;
}
std::shared_ptr<const Processor> blockAge(float mossiness) {
    auto p = std::make_shared<BlockAgeProcessor>();
    p->mossiness = mossiness;
    return p;
}
std::shared_ptr<const Processor> blackstoneReplace() {
    static const auto p = std::make_shared<BlackstoneProcessor>();
    return p;
}
std::shared_ptr<const Processor> lavaSubmerged() {
    static const auto p = std::make_shared<LavaSubmergedProcessor>();
    return p;
}

// ---- placeInWorld -----------------------------------------------------------------------------------------

namespace {

bool liquidContainer(const SB& b, u16 s) {  // LiquidBlockContainer
    if (b.has(s, "waterlogged")) return true;
    const std::string_view n = b.blockName(s);
    return n == "minecraft:kelp" || n == "minecraft:kelp_plant" || n == "minecraft:seagrass" || n == "minecraft:tall_seagrass";
}

// LiquidBlockContainer.placeLiquid: SimpleWaterloggedBlock takes source water.
void placeLiquid(PlaceCtx& c, const BlockPos& p, u16 state, u16 fluidFrom) {
    if (c.b.prop(state, "waterlogged") == "false" && c.b.f.fluidType(fluidFrom) == FluidWater)
        c.set(p, c.b.with(state, "waterlogged", true));
}

}  // namespace

bool placeTemplate(PlaceCtx& c, const Template& t, const BlockPos& position, const BlockPos& ref, const PlaceSettings& s,
                   RandomSource& random) {
    if (t.palettes.empty()) return false;
    const Template::Palette& palette = s.palette(t, position);
    if ((palette.blocks.empty() && (s.ignoreEntities || !t.hasEntities)) || t.size.x < 1 || t.size.y < 1 || t.size.z < 1)
        return false;
    const SB& b = c.b;
    // processBlockInfos
    bool onlyInChunk = true;
    for (const auto& p : s.processors)
        if (p->entirePiece()) onlyInChunk = false;
    std::vector<ProcInfo> originals, processed;
    for (const TemplateBlock& tb : palette.blocks) {
        const BlockPos wp = transformPos(tb.pos, s.mirror, s.rotation, s.pivot).offset(position.x, position.y, position.z);
        if (onlyInChunk && s.box && !s.box->isInside(wp)) continue;
        ProcInfo info{wp, tb.state, tb.nbt != nullptr, tb.nbt};
        bool keep = true;
        for (const auto& p : s.processors)
            if (!p->process(c, position, ref, tb.pos, info, s)) {
                keep = false;
                break;
            }
        if (!keep) continue;
        processed.push_back(info);
        originals.push_back({tb.pos, tb.state, tb.nbt != nullptr, tb.nbt});
    }
    for (const auto& p : s.processors) p->finalize(c, position, ref, originals, processed, s);

    std::vector<BlockPos> toFill, locked, placed;
    for (const ProcInfo& info : processed) {
        const BlockPos& p = info.pos;
        if (s.box && !s.box->isInside(p)) continue;
        const u16 previous = s.applyWaterlogging ? c.get(p) : b.air;
        const u16 state = b.rotate(b.mirror(info.state, s.mirror), s.rotation);
        if (!c.level.setBlock(p, state)) continue;
        placed.push_back(p);
        if (info.hasNbt && isRandomizableContainer(b.blockName(state))) random.nextLong();  // LootTableSeed
        if (s.applyWaterlogging) {
            if (b.fluidSource(state)) {
                locked.push_back(p);
            } else if (liquidContainer(b, state)) {
                placeLiquid(c, p, state, previous);
                if (!b.fluidSource(previous)) toFill.push_back(p);
            }
        }
    }
    static constexpr Direction kFill[5] = {Up, North, East, South, West};
    bool filled = true;
    while (filled && !toFill.empty()) {
        filled = false;
        for (size_t i = 0; i < toFill.size();) {
            const BlockPos p = toFill[i];
            u16 toPlace = c.get(p);  // its fluid state
            for (int k = 0; k < 5 && !b.fluidSource(toPlace); ++k) {
                const BlockPos n = relative(p, kFill[k]);
                const u16 ns = c.get(n);
                if (b.fluidSource(ns) && std::find(locked.begin(), locked.end(), n) == locked.end()) toPlace = ns;
            }
            if (b.fluidSource(toPlace)) {
                const u16 st = c.get(p);
                if (liquidContainer(b, st)) {
                    placeLiquid(c, p, st, toPlace);
                    filled = true;
                    toFill.erase(toFill.begin() + std::ptrdiff_t(i));
                    continue;
                }
            }
            ++i;
        }
    }
    if (!placed.empty() && !s.knownShape) {
        // updateShapeAtEdge over the placed blocks' outer faces, then updateFromNeighbourShapes for each.
        int minX = INT32_MAX, minY = INT32_MAX, minZ = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN, maxZ = INT32_MIN;
        for (const BlockPos& p : placed)
            minX = std::min(minX, p.x), minY = std::min(minY, p.y), minZ = std::min(minZ, p.z), maxX = std::max(maxX, p.x),
            maxY = std::max(maxY, p.y), maxZ = std::max(maxZ, p.z);
        const int sx = maxX - minX + 1, sy = maxY - minY + 1, sz = maxZ - minZ + 1;
        std::vector<u8> shape(size_t(sx) * sy * sz, 0);
        auto idx = [&](int x, int y, int z) { return (size_t(x - minX) * sy + size_t(y - minY)) * sz + size_t(z - minZ); };
        for (const BlockPos& p : placed) shape[idx(p.x, p.y, p.z)] = 1;
        auto filledAt = [&](int x, int y, int z) {
            return x >= minX && x <= maxX && y >= minY && y <= maxY && z >= minZ && z <= maxZ && shape[idx(x, y, z)];
        };
        // DiscreteVoxelShape.forAllFaces order: per axis (X, Y, Z), the negative face then the positive one, cells in
        // x, y, z order.
        auto edge = [&](const BlockPos& p, Direction d) {
            const BlockPos n = relative(p, d);
            const u16 st = c.get(p), nst = c.get(n);
            const u16 ns = updateShape(c, st, p, d, n, nst);
            if (ns != st) c.set(p, ns);
            const u16 nn = updateShape(c, nst, n, opposite(d), p, ns);
            if (nn != nst) c.set(n, nn);
        };
        constexpr Direction kAxes[3][2] = {{West, East}, {Down, Up}, {North, South}};
        for (int axis = 0; axis < 3; ++axis)
            for (int x = minX; x <= maxX; ++x)
                for (int y = minY; y <= maxY; ++y)
                    for (int z = minZ; z <= maxZ; ++z) {
                        if (!filledAt(x, y, z)) continue;
                        for (Direction d : kAxes[axis])
                            if (!filledAt(x + stepX(d), y + stepY(d), z + stepZ(d))) edge({x, y, z}, d);
                    }
        for (const BlockPos& p : placed) {
            const u16 st = c.get(p);
            const u16 ns = updateFromNeighbourShapes(c, st, p);
            if (ns != st) c.set(p, ns);
        }
    }
    return true;
}

// ---- TemplateStructurePiece ----------------------------------------------------------------------------------

void TemplatePiece::postProcess(PlaceCtx& c, RandomSource& random, const BBox& chunkBB, const BlockPos& ref) {
    settings.box = chunkBB;
    box = templateBox(templatePosition, settings.rotation, settings.pivot, settings.mirror, tmpl->size);
    if (!placeTemplate(c, *tmpl, templatePosition, ref, settings, random)) return;
    for (const ProcInfo& marker : filterBlocks(*tmpl, templatePosition, settings, c.b.structureBlock, c.b)) {
        if (!marker.nbt) continue;
        NbtValue* n = const_cast<NbtValue*>(marker.nbt);
        const NbtValue* mode = n->find("mode");
        if (!mode || mode->s != "DATA") continue;
        const NbtValue* meta = n->find("metadata");
        handleDataMarker(c, meta ? meta->s : std::string(), marker.pos, random, chunkBB);
    }
    for (const ProcInfo& j : filterBlocks(*tmpl, templatePosition, settings, c.b.jigsaw, c.b)) {
        if (!j.nbt) continue;
        const NbtValue* fs = const_cast<NbtValue*>(j.nbt)->find("final_state");
        c.set(j.pos, c.b.key(fs ? fs->s : std::string("minecraft:air")));
    }
}

// ---- shape updates -----------------------------------------------------------------------------------------

namespace {

bool connectionException(const SB& b, u16 s) {  // Block.isExceptionForConnection
    return b.tag(s, "minecraft:leaves") || b.is(s, "minecraft:barrier") || b.is(s, "minecraft:carved_pumpkin") ||
           b.is(s, "minecraft:jack_o_lantern") || b.is(s, "minecraft:melon") || b.is(s, "minecraft:pumpkin") ||
           b.tag(s, "minecraft:shulker_boxes");
}

bool isStairs(const SB& b, u16 s) { return b.tag(s, "minecraft:stairs"); }

const char* stairsShape(PlaceCtx& c, u16 state, const BlockPos& pos) {
    const SB& b = c.b;
    const Direction facing = b.dir(state).value_or(North);
    const std::string_view half = b.prop(state, "half");
    auto canTakeShape = [&](Direction n) {
        const u16 ns = c.get(relative(pos, n));
        return !isStairs(b, ns) || b.dir(ns) != facing || b.prop(ns, "half") != half;
    };
    const u16 behind = c.get(relative(pos, facing));
    if (isStairs(b, behind) && b.prop(behind, "half") == half) {
        const Direction bf = b.dir(behind).value_or(North);
        if (axisX(bf) != axisX(facing) && canTakeShape(opposite(bf)))
            return bf == counterClockWise(facing) ? "outer_left" : "outer_right";
    }
    const u16 front = c.get(relative(pos, opposite(facing)));
    if (isStairs(b, front) && b.prop(front, "half") == half) {
        const Direction ff = b.dir(front).value_or(North);
        if (axisX(ff) != axisX(facing) && canTakeShape(ff)) return ff == counterClockWise(facing) ? "inner_left" : "inner_right";
    }
    return "straight";
}

bool fenceGateConnects(const SB& b, u16 s, Direction d) {  // FenceGateBlock.connectsToDirection
    if (!b.tag(s, "minecraft:fence_gates")) return false;
    const auto f = b.dir(s);
    return f && axisX(*f) == axisX(clockWise(d));
}

}  // namespace

namespace {

// WallBlock.connectsTo
bool wallConnects(const SB& b, u16 s, bool faceSolid, Direction d) {
    const std::string_view n = b.blockName(s);
    const bool bars = n.ends_with("_pane") || n == "minecraft:iron_bars" || n.ends_with("_bars");
    return b.tag(s, "minecraft:walls") || (!connectionException(b, s) && faceSolid) || bars || fenceGateConnects(b, s, d);
}

// WallBlock.isCovered: the block above's collision shape, where it touches its bottom face, covers the test
// rectangle (x0..x1, z0..z1 in 1/16 block).
bool coveredFromAbove(const SB& b, u16 top, int x0, int z0, int x1, int z1) {
    const BlockInfo& bi = b.reg.infos()[top];
    if (!bi.collisionCount) return false;
    for (int x = x0; x < x1; ++x)
        for (int z = z0; z < z1; ++z) {
            const float cx = (float(x) + 0.5f) / 16.0f, cz = (float(z) + 0.5f) / 16.0f;
            bool hit = false;
            for (u32 i = 0; i < bi.collisionCount && !hit; ++i) {
                const Box& bx = b.reg.box(bi.collisionFirst + i);
                hit = bx.min[1] <= 0.0001f && bx.max[1] > 0.0001f && bx.min[0] <= cx && cx <= bx.max[0] && bx.min[2] <= cz && cz <= bx.max[2];
            }
            if (!hit) return false;
        }
    return true;
}

// WallBlock.updateShape(level, state, topPos, topNeighbour, n, e, s, w): the sides' heights and the post.
u16 wallShape(PlaceCtx& c, u16 state, u16 top, const bool conn[4]) {
    const SB& b = c.b;
    static constexpr const char* kSide[4] = {"north", "east", "south", "west"};
    static constexpr int kTest[4][4] = {{7, 0, 9, 9}, {7, 7, 16, 9}, {7, 7, 9, 16}, {0, 7, 9, 9}};  // NORTH/EAST/SOUTH/WEST_TEST
    u16 s = state;
    for (int i = 0; i < 4; ++i) {
        const bool covered = coveredFromAbove(b, top, kTest[i][0], kTest[i][1], kTest[i][2], kTest[i][3]);
        s = b.with(s, kSide[i], conn[i] ? (covered ? "tall" : "low") : "none");
    }
    const bool topFull = coveredFromAbove(b, top, 7, 7, 9, 9);  // POST_TEST
    bool post;
    if (b.tag(top, "minecraft:walls") && b.prop(top, "up") == "true") {
        post = true;
    } else {
        const bool n = b.prop(s, "north") == "none", e = b.prop(s, "east") == "none", so = b.prop(s, "south") == "none",
                   w = b.prop(s, "west") == "none";
        const bool corner = (n && so && w && e) || n != so || w != e;
        if (corner) post = true;
        else if ((b.prop(s, "north") == "tall" && b.prop(s, "south") == "tall") || (b.prop(s, "east") == "tall" && b.prop(s, "west") == "tall"))
            post = false;
        else
            post = b.tag(top, "minecraft:wall_post_override") || topFull;
    }
    return b.with(s, "up", post);
}

}  // namespace

u16 updateShape(PlaceCtx& c, u16 state, const BlockPos& pos, Direction dir, const BlockPos& npos, u16 neighbour) {
    const SB& b = c.b;
    if (b.isAir(state)) return state;
    // Walls.
    if (b.tag(state, "minecraft:walls") && dir != Down) {
        static constexpr Direction kSides[4] = {North, East, South, West};
        bool conn[4];
        for (int i = 0; i < 4; ++i) {
            const Direction d = kSides[i];
            if (dir == d)
                conn[i] = wallConnects(b, neighbour, b.f.sturdy(neighbour, opposite(d)), opposite(d));
            else
                conn[i] = b.prop(state, kDirNames[d]) != "none";
        }
        const u16 top = dir == Up ? neighbour : u16(c.get(pos.offset(0, 1, 0)));
        return wallShape(c, state, top, conn);
    }
    // Blocks that break without support.
    {
        const std::string_view n = b.blockName(state);
        if ((n == "minecraft:torch" || n == "minecraft:soul_torch" || n == "minecraft:redstone_torch") && dir == Down)
            return b.f.has(neighbour, fact::CenterUp) ? state : b.air;
        if (n == "minecraft:wall_torch" || n == "minecraft:soul_wall_torch" || n == "minecraft:redstone_wall_torch" ||
            n == "minecraft:ladder") {
            const auto f = b.dir(state);
            if (f && dir == opposite(*f)) return b.f.sturdy(neighbour, *f) ? state : b.air;
            return state;
        }
    }
    // Snowy dirt.
    if (dir == Up && b.has(state, "snowy") && (b.is(state, "minecraft:grass_block") || b.is(state, "minecraft:podzol") ||
                                               b.is(state, "minecraft:mycelium")))
        return b.with(state, "snowy", b.tag(neighbour, "minecraft:snow"));
    if (horizontal(dir)) {
        const char* side = kDirNames[dir];
        if (b.tag(state, "minecraft:fences")) {
            const bool wooden = b.tag(state, "minecraft:wooden_fences");
            const bool sameFence = b.tag(neighbour, "minecraft:fences") && b.tag(neighbour, "minecraft:wooden_fences") == wooden;
            const bool sturdy = b.f.sturdy(neighbour, opposite(dir));
            const bool connect = (!connectionException(b, neighbour) && sturdy) || sameFence || fenceGateConnects(b, neighbour, opposite(dir));
            return b.with(state, side, connect);
        }
        const std::string_view n = b.blockName(state);
        if (n.ends_with("_pane") || n == "minecraft:iron_bars" || n.ends_with("_bars")) {
            const std::string_view nn = b.blockName(neighbour);
            const bool bars = nn.ends_with("_pane") || nn == "minecraft:iron_bars" || nn.ends_with("_bars");
            const bool sturdy = b.f.sturdy(neighbour, opposite(dir));
            const bool connect = (!connectionException(b, neighbour) && sturdy) || bars || b.tag(neighbour, "minecraft:walls");
            return b.with(state, side, connect);
        }
        if (isStairs(b, state)) return b.with(state, "shape", stairsShape(c, state, pos));
    }
    // Two-block plants and doors: the other half must match.
    const std::string_view half = b.prop(state, "half");
    if ((half == "lower" || half == "upper") && (dir == Up || dir == Down)) {
        const bool lower = half == "lower";
        if (lower == (dir == Up)) {
            if (b.f.blockOf(neighbour) != b.f.blockOf(state) || b.prop(neighbour, "half") == half) return b.air;
        }
    }
    (void)npos;
    return state;
}

u16 updateFromNeighbourShapes(PlaceCtx& c, u16 state, const BlockPos& pos) {
    static constexpr Direction kOrder[6] = {West, East, North, South, Down, Up};  // Block.UPDATE_SHAPE_ORDER
    for (Direction d : kOrder) {
        const BlockPos n = relative(pos, d);
        state = updateShape(c, state, pos, d, n, c.get(n));
    }
    return state;
}

}  // namespace wg::st
