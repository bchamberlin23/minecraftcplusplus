// Template based scattered structures: ocean ruins (OceanRuinStructure / OceanRuinPieces), shipwrecks
// (ShipwreckStructure / ShipwreckPieces) and ruined portals (RuinedPortalStructure / RuinedPortalPiece).
#include "worldgen/structure_template.h"

#include <array>

namespace wg::st {

namespace {

int nextIntMth(RandomSource& r, int a, int b) { return a >= b ? a : r.nextInt(b - a + 1) + a; }  // Mth.nextInt

// ---- ocean ruins ----------------------------------------------------------------------------------------

std::shared_ptr<const Processor> archyProcessor(const StructureEnv& env, const char* candidate, const char* replacement) {
    // CappedProcessor(RuleProcessor([block_match candidate -> replacement, append_loot]), constant 5)
    const json j = {{"processor_type", "minecraft:capped"},
                    {"limit", 5},
                    {"delegate",
                     {{"processor_type", "minecraft:rule"},
                      {"rules", json::array({{{"input_predicate", {{"predicate_type", "minecraft:block_match"}, {"block", candidate}}},
                                              {"location_predicate", {{"predicate_type", "minecraft:always_true"}}},
                                              {"output_state", {{"Name", replacement}}},
                                              {"block_entity_modifier", {{"type", "minecraft:append_loot"}, {"loot_table", "minecraft:x"}}}}})}}}};
    ProcessorList l = parseProcessors(env, json::array({j}));
    return l.front();
}

class OceanRuinPiece final : public TemplatePiece {
public:
    OceanRuinPiece(const StructureEnv& env, const std::string& location, const BlockPos& pos, Rot r, float integrity, bool cold, bool large)
        : TemplatePiece("minecraft:orp", 0, env.templates->get(location), makeSettings(env, r, integrity, cold), pos), cold_(cold),
          large_(large) {}

    static PlaceSettings makeSettings(const StructureEnv& env, Rot r, float integrity, bool cold) {
        static std::mutex m;
        static std::shared_ptr<const Processor> coldP, warmP;
        static const StructureEnv* owner = nullptr;
        {
            std::lock_guard lock(m);
            if (owner != &env) {
                coldP = archyProcessor(env, "minecraft:gravel", "minecraft:suspicious_gravel");
                warmP = archyProcessor(env, "minecraft:sand", "minecraft:suspicious_sand");
                owner = &env;
            }
        }
        PlaceSettings s;
        s.rotation = r;
        s.mirror = Mir::None;
        s.processors.push_back(blockRot(integrity));
        s.processors.push_back(blockIgnore(env, {"minecraft:air", "minecraft:structure_block"}));
        s.processors.push_back(cold ? coldP : warmP);
        return s;
    }

    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos& referencePos) override {
        const int h = level.height(Heightmap::OceanFloorWG, templatePosition.x, templatePosition.z);
        templatePosition = {templatePosition.x, h, templatePosition.z};
        const BlockPos c = transformPos({tmpl->size.x - 1, 0, tmpl->size.z - 1}, Mir::None, settings.rotation, {0, 0, 0});
        const BlockPos corner = c.offset(templatePosition.x, templatePosition.y, templatePosition.z);
        templatePosition = {templatePosition.x, getHeight(level, templatePosition, corner), templatePosition.z};
        TemplatePiece::postProcess(level, random, chunkBB, referencePos);
    }

protected:
    void handleDataMarker(PlaceCtx& level, const std::string& marker, const BlockPos& pos, RandomSource& random, const BBox&) override {
        if (marker == "chest") {
            level.set(pos, B("minecraft:chest").with("waterlogged", level.b.waterFluid(level.get(pos))));
            if (level.get(pos).is("minecraft:chest")) random.nextLong();
        } else if (marker == "drowned") {
            level.set(pos, pos.y > level.level.seaLevel() ? B("minecraft:air") : B("minecraft:water"));
        }
    }

private:
    int getHeight(PlaceCtx& level, const BlockPos& pos, const BlockPos& corner) {
        int newY = pos.y, minY = 512;
        const int topY = newY - 1;
        int area = 0;
        const int x0 = std::min(pos.x, corner.x), x1 = std::max(pos.x, corner.x), z0 = std::min(pos.z, corner.z),
                  z1 = std::max(pos.z, corner.z);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                int floorY = pos.y - 1;
                St s = level.get(x, floorY, z);
                while ((s.isAir() || level.b.waterFluid(s) || level.b.tag(s, "minecraft:ice")) && floorY > level.minY() + 1) {
                    --floorY;
                    s = level.get(x, floorY, z);
                }
                minY = std::min(minY, floorY);
                if (floorY < topY - 2) ++area;
            }
        const int width = std::abs(pos.x - corner.x);
        if (topY - minY > 2 && area > width - 2) newY = minY + 1;
        return newY;
    }
    bool cold_, large_;
};

class OceanRuinStructure final : public Structure {
public:
    OceanRuinStructure(const json& def) {
        cold_ = def.value("biome_temp", std::string("warm")) == "cold";
        large_ = def.value("large_probability", 0.0f);
        cluster_ = def.value("cluster_probability", 0.0f);
    }

    std::optional<Stub> findGenerationPoint(GenCtx& c) const override {
        GenCtx* ctx = &c;
        const OceanRuinStructure* self = this;
        return onTopOfChunkCenter(c, HType::OceanFloorWG, [ctx, self](PiecesBuilder& b) {
            const BlockPos offset{ctx->minBlockX(), 90, ctx->minBlockZ()};
            const Rot r = randomRotation(ctx->random);
            self->addPieces(*ctx, offset, r, b);
        });
    }

private:
    void addPieces(GenCtx& c, const BlockPos& pos, Rot r, PiecesBuilder& b) const {
        RandomSource& random = c.random;
        const bool isLarge = random.nextFloat() <= large_;
        const float integrity = isLarge ? 0.9f : 0.8f;
        addPiece(c, pos, r, b, isLarge, integrity);
        if (isLarge && random.nextFloat() <= cluster_) addClusterRuins(c, r, pos, b);
    }

    void addClusterRuins(GenCtx& c, Rot r, const BlockPos& p, PiecesBuilder& b) const {
        RandomSource& random = c.random;
        const BlockPos parentPos{p.x, 90, p.z};
        const BlockPos pc = transformPos({15, 0, 15}, Mir::None, r, {0, 0, 0}).offset(parentPos.x, parentPos.y, parentPos.z);
        const BBox parentBB = BBox::fromCorners(parentPos, pc);
        const BlockPos origin{std::min(parentPos.x, pc.x), parentPos.y, std::min(parentPos.z, pc.z)};
        std::vector<BlockPos> all;
        // allPositions: argument evaluation order (x before z) like Java.
        auto add = [&](int dx, int dz) { all.push_back(origin.offset(dx, 0, dz)); };
        { const int x = -16 + nextIntMth(random, 1, 8); const int z = 16 + nextIntMth(random, 1, 7); add(x, z); }
        { const int x = -16 + nextIntMth(random, 1, 8); const int z = nextIntMth(random, 1, 7); add(x, z); }
        { const int x = -16 + nextIntMth(random, 1, 8); const int z = -16 + nextIntMth(random, 4, 8); add(x, z); }
        { const int x = nextIntMth(random, 1, 7); const int z = 16 + nextIntMth(random, 1, 7); add(x, z); }
        { const int x = nextIntMth(random, 1, 7); const int z = -16 + nextIntMth(random, 4, 6); add(x, z); }
        { const int x = 16 + nextIntMth(random, 1, 7); const int z = 16 + nextIntMth(random, 3, 8); add(x, z); }
        { const int x = 16 + nextIntMth(random, 1, 7); const int z = nextIntMth(random, 1, 7); add(x, z); }
        { const int x = 16 + nextIntMth(random, 1, 7); const int z = -16 + nextIntMth(random, 4, 8); add(x, z); }
        const int ruins = nextIntMth(random, 4, 8);
        for (int i = 0; i < ruins; ++i) {
            if (all.empty()) continue;
            const int idx = random.nextInt(int(all.size()));
            const BlockPos pos = all[size_t(idx)];
            all.erase(all.begin() + idx);
            const Rot nr = randomRotation(random);
            const BlockPos nc = transformPos({5, 0, 6}, Mir::None, nr, {0, 0, 0}).offset(pos.x, pos.y, pos.z);
            if (!BBox::fromCorners(pos, nc).intersects(parentBB)) addPiece(c, pos, nr, b, false, 0.8f);
        }
    }

    void addPiece(GenCtx& c, const BlockPos& pos, Rot r, PiecesBuilder& b, bool isLarge, float integrity) const {
        RandomSource& random = c.random;
        auto name = [](const char* kind, int i) { return std::string("minecraft:underwater_ruin/") + kind + std::to_string(i); };
        if (!cold_) {
            static constexpr int kBigWarm[4] = {4, 5, 6, 7};
            const std::string loc = isLarge ? name("big_warm_", kBigWarm[random.nextInt(4)]) : name("warm_", random.nextInt(8) + 1);
            b.add(std::make_unique<OceanRuinPiece>(c.env, loc, pos, r, integrity, false, isLarge));
            return;
        }
        static constexpr int kBig[4] = {1, 2, 3, 8};
        const int idx = random.nextInt(isLarge ? 4 : 8);
        const int n = isLarge ? kBig[idx] : idx + 1;
        const char* pre = isLarge ? "big_" : "";
        b.add(std::make_unique<OceanRuinPiece>(c.env, name((std::string(pre) + "brick_").c_str(), n), pos, r, integrity, true, isLarge));
        b.add(std::make_unique<OceanRuinPiece>(c.env, name((std::string(pre) + "cracked_").c_str(), n), pos, r, 0.7f, true, isLarge));
        b.add(std::make_unique<OceanRuinPiece>(c.env, name((std::string(pre) + "mossy_").c_str(), n), pos, r, 0.5f, true, isLarge));
    }

    bool cold_;
    float large_, cluster_;
};

// ---- shipwrecks ---------------------------------------------------------------------------------------------

class ShipwreckPiece final : public TemplatePiece {
public:
    ShipwreckPiece(const StructureEnv& env, const std::string& location, const BlockPos& pos, Rot r, bool beached)
        : TemplatePiece("minecraft:shipwreck", 0, env.templates->get(location), makeSettings(env, r), pos), isBeached(beached) {}
    bool isBeached, heightAdjusted = false;

    static PlaceSettings makeSettings(const StructureEnv& env, Rot r) {
        PlaceSettings s;
        s.rotation = r;
        s.mirror = Mir::None;
        s.pivot = {4, 0, 15};
        s.processors.push_back(blockIgnore(env, {"minecraft:air", "minecraft:structure_block"}));
        return s;
    }
    bool isTooBigToFitInWorldGenRegion() const { return tmpl->size.x > 32 || tmpl->size.y > 32; }
    int calculateBeachedPosition(int minY, RandomSource& random) const { return minY - tmpl->size.y / 2 - random.nextInt(3); }
    void adjustPositionHeight(int h) {
        heightAdjusted = true;
        templatePosition = {templatePosition.x, h, templatePosition.z};
    }

    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos& referencePos) override {
        if (!heightAdjusted && !isTooBigToFitInWorldGenRegion()) {
            int minY = level.maxY() - 1 + 1, mean = 0;
            const BlockPos size = tmpl->size;
            const Heightmap h = isBeached ? Heightmap::WorldSurfaceWG : Heightmap::OceanFloorWG;
            const int baseSize = size.x * size.z;
            if (baseSize == 0) {
                mean = level.height(h, templatePosition.x, templatePosition.z);
            } else {
                const BlockPos corner = templatePosition.offset(size.x - 1, 0, size.z - 1);
                for (int z = std::min(templatePosition.z, corner.z); z <= std::max(templatePosition.z, corner.z); ++z)
                    for (int x = std::min(templatePosition.x, corner.x); x <= std::max(templatePosition.x, corner.x); ++x) {
                        const int hm = level.height(h, x, z);
                        mean += hm;
                        minY = std::min(minY, hm);
                    }
                mean /= baseSize;
            }
            adjustPositionHeight(isBeached ? calculateBeachedPosition(minY, random) : mean);
        }
        TemplatePiece::postProcess(level, random, chunkBB, referencePos);
    }

protected:
    void handleDataMarker(PlaceCtx& level, const std::string& marker, const BlockPos& pos, RandomSource& random, const BBox&) override {
        if (marker != "map_chest" && marker != "treasure_chest" && marker != "supply_chest") return;
        if (isRandomizableContainer(level.b.blockName(level.get(pos.offset(0, -1, 0))))) random.nextLong();
    }
};

class ShipwreckStructure final : public Structure {
public:
    explicit ShipwreckStructure(bool beached) : beached_(beached) {}
    std::optional<Stub> findGenerationPoint(GenCtx& c) const override {
        GenCtx* ctx = &c;
        const bool beached = beached_;
        return onTopOfChunkCenter(c, beached ? HType::WorldSurfaceWG : HType::OceanFloorWG, [ctx, beached](PiecesBuilder& b) {
            static const char* const kBeached[] = {"with_mast", "sideways_full", "sideways_fronthalf", "sideways_backhalf", "rightsideup_full",
                                                   "rightsideup_fronthalf", "rightsideup_backhalf", "with_mast_degraded",
                                                   "rightsideup_full_degraded", "rightsideup_fronthalf_degraded",
                                                   "rightsideup_backhalf_degraded"};
            static const char* const kOcean[] = {"with_mast", "upsidedown_full", "upsidedown_fronthalf", "upsidedown_backhalf", "sideways_full",
                                                 "sideways_fronthalf", "sideways_backhalf", "rightsideup_full", "rightsideup_fronthalf",
                                                 "rightsideup_backhalf", "with_mast_degraded", "upsidedown_full_degraded",
                                                 "upsidedown_fronthalf_degraded", "upsidedown_backhalf_degraded", "sideways_full_degraded",
                                                 "sideways_fronthalf_degraded", "sideways_backhalf_degraded", "rightsideup_full_degraded",
                                                 "rightsideup_fronthalf_degraded", "rightsideup_backhalf_degraded"};
            RandomSource& random = ctx->random;
            const Rot r = randomRotation(random);
            const BlockPos offset{ctx->minBlockX(), 90, ctx->minBlockZ()};
            const char* name = beached ? kBeached[random.nextInt(11)] : kOcean[random.nextInt(20)];
            auto piece = std::make_unique<ShipwreckPiece>(ctx->env, std::string("minecraft:shipwreck/") + name, offset, r, beached);
            if (piece->isTooBigToFitInWorldGenRegion()) {
                const BBox bb = piece->box;
                int h;
                if (beached) {
                    const int minY = ctx->lowestY(bb.minX, bb.minZ, bb.xSpan(), bb.zSpan());
                    h = piece->calculateBeachedPosition(minY, random);
                } else {
                    h = ctx->meanFirstOccupiedHeight(bb.minX, bb.xSpan(), bb.minZ, bb.zSpan());
                }
                piece->adjustPositionHeight(h);
            }
            b.add(std::move(piece));
        });
    }

private:
    bool beached_;
};

// ---- ruined portals --------------------------------------------------------------------------------------------

enum class Vertical { OnLandSurface, PartlyBuried, OnOceanFloor, InMountain, Underground, InNether };

Vertical verticalFromName(std::string_view n) {
    if (n == "partly_buried") return Vertical::PartlyBuried;
    if (n == "on_ocean_floor") return Vertical::OnOceanFloor;
    if (n == "in_mountain") return Vertical::InMountain;
    if (n == "underground") return Vertical::Underground;
    if (n == "in_nether") return Vertical::InNether;
    return Vertical::OnLandSurface;
}

struct PortalProperties {
    bool cold;
    float mossiness;
    bool airPocket, overgrown, vines, replaceWithBlackstone;
};

class RuinedPortalPiece final : public TemplatePiece {
public:
    RuinedPortalPiece(const StructureEnv& env, const BlockPos& pos, Vertical v, const PortalProperties& p, const Template* t, Rot r, Mir m,
                      const BlockPos& pivot)
        : TemplatePiece("minecraft:rupo", 0, t, makeSettings(env, m, r, v, pivot, p), pos), vertical(v), props(p) {}
    Vertical vertical;
    PortalProperties props;

    static PlaceSettings makeSettings(const StructureEnv& env, Mir m, Rot r, Vertical v, const BlockPos& pivot, const PortalProperties& p) {
        PlaceSettings s;
        s.rotation = r;
        s.mirror = m;
        s.pivot = pivot;
        s.processors.push_back(p.airPocket ? blockIgnore(env, {"minecraft:structure_block"})
                                           : blockIgnore(env, {"minecraft:air", "minecraft:structure_block"}));
        auto randomRule = [](const char* src, float prob, const char* dst) {
            return json{{"input_predicate", {{"predicate_type", "minecraft:random_block_match"}, {"block", src}, {"probability", prob}}},
                        {"location_predicate", {{"predicate_type", "minecraft:always_true"}}},
                        {"output_state", {{"Name", dst}}}};
        };
        auto rule = [](const char* src, const char* dst) {
            return json{{"input_predicate", {{"predicate_type", "minecraft:block_match"}, {"block", src}}},
                        {"location_predicate", {{"predicate_type", "minecraft:always_true"}}},
                        {"output_state", {{"Name", dst}}}};
        };
        json rules = json::array();
        rules.push_back(randomRule("minecraft:gold_block", 0.3f, "minecraft:air"));
        if (v == Vertical::OnOceanFloor)
            rules.push_back(rule("minecraft:lava", "minecraft:magma_block"));
        else
            rules.push_back(p.cold ? rule("minecraft:lava", "minecraft:netherrack") : randomRule("minecraft:lava", 0.2f, "minecraft:magma_block"));
        if (!p.cold) rules.push_back(randomRule("minecraft:netherrack", 0.07f, "minecraft:magma_block"));
        const json list = json::array({json{{"processor_type", "minecraft:rule"}, {"rules", rules}},
                                       json{{"processor_type", "minecraft:protected_blocks"}, {"value", "#minecraft:features_cannot_replace"}}});
        ProcessorList parsed = parseProcessors(env, list);
        s.processors.push_back(parsed[0]);
        s.processors.push_back(blockAge(p.mossiness));
        s.processors.push_back(parsed[1]);
        s.processors.push_back(lavaSubmerged());
        if (p.replaceWithBlackstone) s.processors.push_back(blackstoneReplace());
        return s;
    }

    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos& referencePos) override {
        const BBox bb = templateBox(templatePosition, settings.rotation, settings.pivot, settings.mirror, tmpl->size);
        if (!chunkBB.isInside(bb.center())) return;
        BBox expanded = chunkBB;
        expanded.encapsulate(bb);
        TemplatePiece::postProcess(level, random, expanded, referencePos);
        spreadNetherrack(random, level);
        addNetherrackDripColumnsBelowPortal(random, level);
        if (props.vines || props.overgrown)
            for (int z = box.minZ; z <= box.maxZ; ++z)
                for (int y = box.minY; y <= box.maxY; ++y)
                    for (int x = box.minX; x <= box.maxX; ++x) {
                        const BlockPos p{x, y, z};
                        if (props.vines) maybeAddVines(random, level, p);
                        if (props.overgrown) maybeAddLeavesAbove(random, level, p);
                    }
    }

    static Heightmap heightMapType(Vertical v) { return v == Vertical::OnOceanFloor ? Heightmap::OceanFloorWG : Heightmap::WorldSurfaceWG; }

protected:
    void handleDataMarker(PlaceCtx&, const std::string&, const BlockPos&, RandomSource&, const BBox&) override {}

private:
    void maybeAddVines(RandomSource& random, PlaceCtx& level, const BlockPos& pos) {
        const St s = level.get(pos);
        if (s.isAir() || s.is("minecraft:vine")) return;
        const Direction d = randomHorizontal(random);
        const BlockPos n = relative(pos, d);
        if (!level.get(n).isAir()) return;
        if (level.b.f.collisionFull(s, d)) level.set(n, B("minecraft:vine").with(kDirNames[opposite(d)], true));
    }
    void maybeAddLeavesAbove(RandomSource& random, PlaceCtx& level, const BlockPos& pos) {
        if (random.nextFloat() < 0.5f && level.get(pos).is("minecraft:netherrack") && level.get(pos.offset(0, 1, 0)).isAir())
            level.set(pos.offset(0, 1, 0), B("minecraft:jungle_leaves").with("persistent", true));
    }
    void addNetherrackDripColumnsBelowPortal(RandomSource& random, PlaceCtx& level) {
        for (int x = box.minX + 1; x < box.maxX; ++x)
            for (int z = box.minZ + 1; z < box.maxZ; ++z) {
                const BlockPos p{x, box.minY, z};
                if (level.get(p).is("minecraft:netherrack")) addNetherrackDripColumn(random, level, p.offset(0, -1, 0));
            }
    }
    void addNetherrackDripColumn(RandomSource& random, PlaceCtx& level, BlockPos pos) {
        placeNetherrackOrMagma(random, level, pos);
        int remaining = 8;
        while (remaining > 0 && random.nextFloat() < 0.5f) {
            --pos.y;
            --remaining;
            placeNetherrackOrMagma(random, level, pos);
        }
    }
    void spreadNetherrack(RandomSource& random, PlaceCtx& level) {
        const bool follow = vertical == Vertical::OnLandSurface || vertical == Vertical::OnOceanFloor;
        const BlockPos c = box.center();
        static constexpr float kProb[14] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.9f, 0.9f, 0.8f, 0.7f, 0.6f, 0.4f, 0.2f};
        const int maxDistance = 14;
        const int averageWidth = (box.xSpan() + box.zSpan()) / 2;
        const int adjustment = random.nextInt(std::max(1, 8 - averageWidth / 2));
        for (int x = c.x - maxDistance; x <= c.x + maxDistance; ++x)
            for (int z = c.z - maxDistance; z <= c.z + maxDistance; ++z) {
                const int distance = std::abs(x - c.x) + std::abs(z - c.z);
                const int adjusted = std::max(0, distance + adjustment);
                if (adjusted >= maxDistance) continue;
                if (!(random.nextDouble() < double(kProb[adjusted]))) continue;
                const int surfaceY = level.height(heightMapType(vertical), x, z) - 1;
                const int y = follow ? surfaceY : std::min(box.minY, surfaceY);
                const BlockPos p{x, y, z};
                if (std::abs(y - box.minY) <= 3 && canReplace(level, level.get(p))) {
                    placeNetherrackOrMagma(random, level, p);
                    if (props.overgrown) maybeAddLeavesAbove(random, level, p);
                    addNetherrackDripColumn(random, level, p.offset(0, -1, 0));
                }
            }
    }
    bool canReplace(PlaceCtx& level, St s) const {
        return !s.is("minecraft:air") && !s.is("minecraft:obsidian") && !level.b.tag(s, "minecraft:features_cannot_replace") &&
               (vertical == Vertical::InNether || !s.is("minecraft:lava"));
    }
    void placeNetherrackOrMagma(RandomSource& random, PlaceCtx& level, const BlockPos& p) {
        if (!canReplace(level, level.get(p))) return;
        if (!props.cold && random.nextFloat() < 0.07f)
            level.set(p, B("minecraft:magma_block"));
        else
            level.set(p, B("minecraft:netherrack"));
    }
};

class RuinedPortalStructure final : public Structure {
public:
    struct Setup {
        Vertical placement;
        float airPocketProbability, mossiness;
        bool overgrown, vines, canBeCold, replaceWithBlackstone;
        float weight;
    };

    explicit RuinedPortalStructure(const json& def) {
        for (const json& s : def.value("setups", json::array()))
            setups_.push_back({verticalFromName(s.value("placement", std::string())), s.value("air_pocket_probability", 0.0f),
                               s.value("mossiness", 0.0f), s.value("overgrown", false), s.value("vines", false), s.value("can_be_cold", false),
                               s.value("replace_with_blackstone", false), s.value("weight", 1.0f)});
    }

    std::optional<Stub> findGenerationPoint(GenCtx& c) const override {
        RandomSource& random = c.random;
        const Setup* chosen = nullptr;
        if (setups_.size() > 1) {
            float total = 0.0f;
            for (const Setup& s : setups_) total += s.weight;
            float pick = random.nextFloat();
            for (const Setup& s : setups_) {
                pick -= s.weight / total;
                if (pick < 0.0f) {
                    chosen = &s;
                    break;
                }
            }
        } else if (!setups_.empty()) {
            chosen = &setups_[0];
        }
        if (!chosen) return std::nullopt;
        const Setup setup = *chosen;
        const bool airPocket = setup.airPocketProbability == 0.0f ? false : setup.airPocketProbability == 1.0f ? true
                                                                                                                 : random.nextFloat() < setup.airPocketProbability;
        std::string location;
        if (random.nextFloat() < 0.05f)
            location = "minecraft:ruined_portal/giant_portal_" + std::to_string(random.nextInt(3) + 1);
        else
            location = "minecraft:ruined_portal/portal_" + std::to_string(random.nextInt(10) + 1);
        const Template* t = c.env.templates->get(location);
        const Rot r = Rot(random.nextInt(4));
        const Mir m = random.nextFloat() < 0.5f ? Mir::None : Mir::FrontBack;
        const BlockPos pivot{t->size.x / 2, 0, t->size.z / 2};
        const BlockPos base{c.minBlockX(), 0, c.minBlockZ()};
        const BBox bb = templateBox(base, r, pivot, m, t->size);
        const BlockPos centre = bb.center();
        const HType ht = setup.placement == Vertical::OnOceanFloor ? HType::OceanFloorWG : HType::WorldSurfaceWG;
        const int surfaceY = c.baseHeight(centre.x, centre.z, ht) - 1;
        const int projectedY = findSuitableY(c, random, setup.placement, airPocket, surfaceY, bb.ySpan(), bb);
        const BlockPos origin{base.x, projectedY, base.z};
        GenCtx* ctx = &c;
        return Stub{origin, [ctx, setup, airPocket, origin, t, r, m, pivot](PiecesBuilder& b) {
                        bool cold = false;
                        if (setup.canBeCold) {
                            const u16 biome = ctx->noiseBiome(origin.x >> 2, origin.y >> 2, origin.z >> 2);
                            cold = ctx->env.temperature(biome, origin) < 0.15f;  // Biome.coldEnoughToSnow
                        }
                        const PortalProperties p{cold, setup.mossiness, airPocket, setup.overgrown, setup.vines, setup.replaceWithBlackstone};
                        b.add(std::make_unique<RuinedPortalPiece>(ctx->env, origin, setup.placement, p, t, r, m, pivot));
                    }};
    }

private:
    static int randomWithin(RandomSource& r, int minPreferred, int max) {
        return minPreferred < max ? r.nextInt(max - minPreferred + 1) + minPreferred : max;
    }
    int findSuitableY(GenCtx& c, RandomSource& random, Vertical v, bool airPocket, int surfaceY, int ySpan, const BBox& bb) const {
        const int minY = c.minY() + 15;
        int newY;
        if (v == Vertical::InNether) {
            if (airPocket) newY = random.nextInt(100 - 32 + 1) + 32;
            else if (random.nextFloat() < 0.5f) newY = random.nextInt(3) + 27;
            else newY = random.nextInt(100 - 29 + 1) + 29;
        } else if (v == Vertical::InMountain) {
            newY = randomWithin(random, 70, surfaceY - ySpan);
        } else if (v == Vertical::Underground) {
            newY = randomWithin(random, minY, surfaceY - ySpan);
        } else if (v == Vertical::PartlyBuried) {
            newY = surfaceY - ySpan + random.nextInt(7) + 2;
        } else {
            newY = surfaceY;
        }
        const std::array<std::pair<int, int>, 4> corners{{{bb.minX, bb.minZ}, {bb.maxX, bb.minZ}, {bb.minX, bb.maxZ}, {bb.maxX, bb.maxZ}}};
        std::array<std::vector<u16>, 4> columns;
        for (size_t i = 0; i < 4; ++i) columns[i] = c.baseColumn(corners[i].first, corners[i].second);
        const bool oceanFloor = v == Vertical::OnOceanFloor;
        const SB& b = c.env.b;
        int y;
        for (y = newY; y > minY; --y) {
            int solid = 0;
            for (const auto& col : columns) {
                const int i = y - c.minY();
                const u16 s = i >= 0 && i < int(col.size()) ? col[size_t(i)] : b.air;
                const bool opaque = oceanFloor ? b.blocksMotion(s) : !b.isAir(s);
                if (opaque && ++solid == 3) return y;
            }
        }
        return y;
    }

    std::vector<Setup> setups_;
};

}  // namespace

void registerOceanStructures() {
    registerStructureType("minecraft:ocean_ruin", [](const json& def, StructureEnv&) -> std::unique_ptr<Structure> {
        return std::make_unique<OceanRuinStructure>(def);
    });
    registerStructureType("minecraft:shipwreck", [](const json& def, StructureEnv&) -> std::unique_ptr<Structure> {
        return std::make_unique<ShipwreckStructure>(def.value("is_beached", false));
    });
    registerStructureType("minecraft:ruined_portal", [](const json& def, StructureEnv&) -> std::unique_ptr<Structure> {
        return std::make_unique<RuinedPortalStructure>(def);
    });
}

}  // namespace wg::st
