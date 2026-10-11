// Port of the game's tree-family features: TreeFeature (trunk, foliage and root placers, feature sizes, tree
// decorators, the leaf distance pass and edge shape updates), huge mushrooms, huge fungi, bamboo and fallen
// trees. Every random call, set iteration order and block write follows the game's code; see java_pos_set.h
// for the HashSet ordering the decorators and the leaf pass depend on.
#include "assets/assets.h"
#include "worldgen/feature_api.h"
#include "worldgen/java_pos_set.h"
#include "worldgen/rng.h"

#include <json/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace wg {
namespace {

using json = nlohmann::json;

constexpr double kPi = 3.141592653589793;  // Math.PI
constexpr Direction kHorizontal[4] = {North, East, South, West};  // Direction.Plane.HORIZONTAL
constexpr Direction kAllDirections[6] = {Down, Up, North, South, West, East};
constexpr const char* kDirName[6] = {"down", "up", "north", "south", "west", "east"};

Direction opposite(Direction d) {
    constexpr Direction o[6] = {Up, Down, South, North, East, West};
    return o[d];
}
Direction clockWise(Direction d) {  // horizontal directions only
    return d == North ? East : d == East ? South : d == South ? West : North;
}
char axisOf(Direction d) { return d <= Up ? 'y' : d <= South ? 'z' : 'x'; }
bool isPositive(Direction d) { return d == Up || d == South || d == East; }
BlockPos rel(const BlockPos& p, Direction d, int n = 1) {
    return p.offset(kDirOffset[d][0] * n, kDirOffset[d][1] * n, kDirOffset[d][2] * n);
}
BlockPos above(const BlockPos& p, int n = 1) { return p.offset(0, n, 0); }
BlockPos below(const BlockPos& p, int n = 1) { return p.offset(0, -n, 0); }
int distManhattan(const BlockPos& a, const BlockPos& b) {
    return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z);
}
Direction parseDirection(std::string_view s) {
    for (int i = 0; i < 6; ++i)
        if (s == kDirName[i]) return Direction(i);
    throw std::runtime_error("unknown direction " + std::string(s));
}

// Mth.sin / Mth.cos table, Mth.floor(float).
struct SinTable {
    float v[65536];
    SinTable() {
        for (int i = 0; i < 65536; ++i) v[i] = float(std::sin(i / 10430.378350470453));
    }
};
const SinTable& sinTable() {
    static const SinTable t;
    return t;
}
float msin(double v) { return sinTable().v[i64(v * 10430.378350470453) & 65535]; }
float mcos(double v) { return sinTable().v[i64(v * 10430.378350470453 + 16384.0) & 65535]; }
int mfloor(float v) { return jfloor(double(v)); }

// ---- JSON helpers -------------------------------------------------------------------------
const json& configOf(const json& j) {
    auto it = j.find("config");
    return it != j.end() && it->is_object() ? *it : j;
}
std::string_view stripNs(std::string_view s) {
    if (s.starts_with("minecraft:")) s.remove_prefix(10);
    return s;
}
std::string typeOf(const json& j) { return std::string(stripNs(j.at("type").get_ref<const std::string&>())); }
// Codec.FLOAT parses the number's text as a float; going through double could round twice.
float jsonFloat(const json& j) { return std::strtof(j.dump().c_str(), nullptr); }
int jsonInt(const json& j, const char* key, int fallback) {
    auto it = j.find(key);
    return it == j.end() ? fallback : it->get<int>();
}

// ---- block facts shared by all tree-family features ----------------------------------------
// How a block's updateShape reacts to a neighbour change (the classes TreeFeature's edge update can hit).
enum ShapeKind : u8 {
    kOther = 1,
    kVegetation,  // VegetationBlock and the like: air when it can no longer survive
    kSporeBlossom,
    kDoublePlant,
    kSnowy,
    kVine,
    kHangingMoss,
    kFence,
    kFenceGate,
    kPane
};
// Per-state facts cached next to the kind (BlockFacts::inTag takes a lock per call).
enum : u32 {
    kKindMask = 15,
    kTreeReplaceable = 1u << 4,  // #replaceable_by_trees
    kLog = 1u << 5,
    kLeaves = 1u << 6,
    kMushroomReplaceable = 1u << 7,
    kSnowTag = 1u << 8,
    kFenceTag = 1u << 9,
    kWoodenFence = 1u << 10,
    kWallTag = 1u << 11,
    kConnectionException = 1u << 12,  // Block.isExceptionForConnection
    kDistanceShift = 16,              // LeavesBlock.getOptionalDistanceAt + 1 (0: empty)
};

class Blocks {
public:
    explicit Blocks(const FeatureEnv& env)
        : reg(env.blocks()), facts(env.facts()), info_(new std::atomic<u32>[65536]()) {
        air = env.defaultState("minecraft:air");
        const u16 vine = env.defaultState("minecraft:vine");
        for (int i = 0; i < 4; ++i) vineWith[i] = reg.withProperty(vine, kDirName[kHorizontal[i]], "true");
        beeNest = with(env.defaultState("minecraft:bee_nest"), "facing", "south");
        cocoa = env.defaultState("minecraft:cocoa");
        creakingHeart = with(with(env.defaultState("minecraft:creaking_heart"), "creaking_heart_state", "dormant"),
                             "natural", "true");
        const u16 moss = env.defaultState("minecraft:pale_hanging_moss");
        mossBody = with(moss, "tip", "false");
        mossTip = with(moss, "tip", "true");
        shelfMushroom = env.defaultState("minecraft:shelf_mushroom");
        podzol = env.defaultState("minecraft:podzol");
        const u16 bamboo =
            with(with(with(env.defaultState("minecraft:bamboo"), "age", "1"), "leaves", "none"), "stage", "0");
        bambooTrunk = bamboo;
        bambooFinalLarge = with(with(bamboo, "leaves", "large"), "stage", "1");
        bambooTopLarge = with(bamboo, "leaves", "large");
        bambooTopSmall = with(bamboo, "leaves", "small");
        weepingVines = env.defaultState("minecraft:weeping_vines");
        weepingVinesPlant = env.defaultState("minecraft:weeping_vines_plant");
    }

    BlockRegistry& reg;
    const BlockFacts& facts;
    u16 air = 0, vineWith[4] = {}, beeNest = 0, cocoa = 0, creakingHeart = 0, mossBody = 0, mossTip = 0;
    u16 shelfMushroom = 0, podzol = 0, bambooTrunk = 0, bambooFinalLarge = 0, bambooTopLarge = 0, bambooTopSmall = 0;
    u16 weepingVines = 0, weepingVinesPlant = 0;

    std::string_view name(u16 id) const {  // block id without properties
        std::string_view n = reg.name(id);
        return n.substr(0, n.find('['));
    }
    bool is(u16 id, std::string_view blockName) const { return name(id) == blockName; }
    bool sameBlock(u16 a, u16 b) const { return name(a) == name(b); }
    std::string_view prop(u16 id, std::string_view key) const { return reg.property(id, key); }
    bool has(u16 id, std::string_view key) const { return !prop(id, key).empty(); }
    // BlockState.trySetValue: only blocks that have the property change.
    u16 with(u16 id, std::string_view key, std::string_view value) const {
        return has(id, key) ? reg.withProperty(id, key, value) : id;
    }
    u16 withFlag(u16 id, std::string_view key, bool value) const { return with(id, key, value ? "true" : "false"); }
    u16 withInt(u16 id, std::string_view key, int value) const { return with(id, key, std::to_string(value)); }
    bool tag(u16 id, std::string_view t) const { return facts.inTag(id, t); }
    bool isAir(u16 id) const { return facts.isAir(id); }
    bool isLog(u16 id) const { return (info(id) & kLog) != 0; }
    bool isLeaves(u16 id) const { return (info(id) & kLeaves) != 0; }
    bool mushroomReplaceable(u16 id) const { return (info(id) & kMushroomReplaceable) != 0; }
    bool isVine(u16 id) const { return kind(id) == kVine; }
    bool isWaterBlock(u16 id) const { return is(id, "minecraft:water"); }
    bool isSourceWaterFluid(u16 id) const { return facts.isSourceWater(id) || prop(id, "waterlogged") == "true"; }
    bool isAnyWaterFluid(u16 id) const { return facts.isWater(id) || prop(id, "waterlogged") == "true"; }
    // TreeFeature.validTreePos
    bool validTreePos(u16 id) const { return isAir(id) || (info(id) & kTreeReplaceable) != 0; }

    bool canBeReplaced(u16 id) const { return facts.canBeReplaced(id); }
    bool sturdyFace(u16 id, Direction face) const { return facts.isFaceSturdy(id, face); }

    // LeavesBlock.getOptionalDistanceAt; -1 when empty.
    int optionalDistance(u16 id) const { return int(info(id) >> kDistanceShift) - 1; }
    ShapeKind kind(u16 id) const { return ShapeKind(info(id) & kKindMask); }
    u32 info(u16 id) const {
        u32 v = info_[id].load(std::memory_order_relaxed);
        if (!v) {
            v = computeInfo(id);
            info_[id].store(v, std::memory_order_relaxed);
        }
        return v;
    }

    // BlockState.updateShape for the blocks a tree's edge can change; everything else keeps its state.
    u16 updateShape(const GenRegion& level, u16 state, const BlockPos& pos, Direction dir, u16 neighbor) const;

private:
    u32 computeInfo(u16 id) const {
        u32 v = classify(id);
        if (tag(id, "minecraft:replaceable_by_trees")) v |= kTreeReplaceable;
        if (tag(id, "minecraft:logs")) v |= kLog;
        if (tag(id, "minecraft:leaves")) v |= kLeaves;
        if (tag(id, "minecraft:replaceable_by_mushrooms")) v |= kMushroomReplaceable;
        if (tag(id, "minecraft:snow")) v |= kSnowTag;
        if (tag(id, "minecraft:fences")) v |= kFenceTag;
        if (tag(id, "minecraft:wooden_fences")) v |= kWoodenFence;
        if (tag(id, "minecraft:walls")) v |= kWallTag;
        const std::string_view n = name(id);
        if ((v & kLeaves) || tag(id, "minecraft:shulker_boxes") || n == "minecraft:barrier" ||
            n == "minecraft:carved_pumpkin" || n == "minecraft:jack_o_lantern" || n == "minecraft:melon" ||
            n == "minecraft:pumpkin")
            v |= kConnectionException;
        int distance = -1;
        if (tag(id, "minecraft:prevents_nearby_leaf_decay")) {
            distance = 0;
        } else if (const std::string_view d = prop(id, "distance"); !d.empty() && n != "minecraft:scaffolding") {
            distance = std::atoi(std::string(d).c_str());  // scaffolding's "distance" is another property
        }
        return v | u32(distance + 1) << kDistanceShift;
    }
    u8 classify(u16 id) const {
        if (isAir(id)) return kOther;
        const std::string_view n = name(id);
        static const std::unordered_set<std::string_view> kDoublePlants = {
            "minecraft:tall_grass", "minecraft:large_fern", "minecraft:sunflower", "minecraft:lilac",
            "minecraft:rose_bush", "minecraft:peony", "minecraft:pitcher_plant", "minecraft:tall_seagrass",
            "minecraft:small_dripleaf"};
        if (kDoublePlants.count(n)) return kDoublePlant;
        // Blocks whose updateShape returns air once canSurvive fails, whatever the direction.
        static const std::unordered_set<std::string_view> kVegetationBlocks = {
            "minecraft:allium", "minecraft:azure_bluet", "minecraft:blue_orchid", "minecraft:cornflower",
            "minecraft:dandelion", "minecraft:golden_dandelion", "minecraft:lily_of_the_valley",
            "minecraft:orange_tulip", "minecraft:oxeye_daisy", "minecraft:pink_tulip", "minecraft:poppy",
            "minecraft:red_tulip", "minecraft:torchflower", "minecraft:white_tulip", "minecraft:wither_rose",
            "minecraft:closed_eyeblossom", "minecraft:open_eyeblossom", "minecraft:fern", "minecraft:short_grass",
            "minecraft:bush", "minecraft:red_shrub", "minecraft:firefly_bush", "minecraft:pink_petals",
            "minecraft:wildflowers", "minecraft:sweet_berry_bush", "minecraft:dead_bush", "minecraft:short_dry_grass",
            "minecraft:tall_dry_grass", "minecraft:azalea", "minecraft:flowering_azalea", "minecraft:acacia_sapling",
            "minecraft:birch_sapling", "minecraft:cherry_sapling", "minecraft:dark_oak_sapling",
            "minecraft:jungle_sapling", "minecraft:oak_sapling", "minecraft:pale_oak_sapling",
            "minecraft:poplar_sapling", "minecraft:spruce_sapling", "minecraft:mangrove_propagule",
            "minecraft:crimson_fungus", "minecraft:warped_fungus", "minecraft:crimson_roots", "minecraft:warped_roots",
            "minecraft:nether_sprouts", "minecraft:nether_wart", "minecraft:wheat", "minecraft:carrots",
            "minecraft:potatoes", "minecraft:beetroots", "minecraft:torchflower_crop", "minecraft:brown_mushroom",
            "minecraft:red_mushroom", "minecraft:lily_pad", "minecraft:seagrass", "minecraft:sea_pickle",
            "minecraft:snow", "minecraft:soul_fire"};
        if (kVegetationBlocks.count(n) || n.ends_with("_carpet")) return kVegetation;
        if (n == "minecraft:spore_blossom") return kSporeBlossom;
        if (n == "minecraft:vine") return kVine;
        if (n == "minecraft:pale_hanging_moss") return kHangingMoss;
        if (tag(id, "minecraft:fences")) return kFence;
        if (tag(id, "minecraft:fence_gates")) return kFenceGate;
        if (n.ends_with("glass_pane") || n.ends_with("_bars")) return kPane;
        if (has(id, "snowy")) return kSnowy;
        return kOther;
    }
    bool connectionException(u16 id) const { return (info(id) & kConnectionException) != 0; }
    bool vineCanAttach(u16 id, Direction face) const {  // MultifaceBlock.canAttachTo: support or collision face
        return facts.isFaceSturdy(id, face) || facts.isCollisionFaceFull(id, face);
    }

    std::unique_ptr<std::atomic<u32>[]> info_;
};

u16 Blocks::updateShape(const GenRegion& level, u16 state, const BlockPos& pos, Direction dir, u16 neighbor) const {
    switch (kind(state)) {
    case kDoublePlant: {  // DoublePlantBlock, then VegetationBlock's survival check
        const std::string_view half = prop(state, "half");
        const bool lower = half == "lower";
        const bool vertical = dir == Up || dir == Down;
        if (!vertical || lower != (dir == Up) || (sameBlock(neighbor, state) && prop(neighbor, "half") != half)) {
            return facts.canSurvive(level, state, pos) ? state : air;
        }
        return air;
    }
    case kVegetation:
        return facts.canSurvive(level, state, pos) ? state : air;
    case kSporeBlossom:
        return dir == Up && !facts.canSurvive(level, state, pos) ? air : state;
    case kSnowy:
        return dir == Up ? withFlag(state, "snowy", (info(neighbor) & kSnowTag) != 0) : state;
    case kHangingMoss:
        return withFlag(state, "tip", !is(level.block(below(pos)), "minecraft:pale_hanging_moss"));
    case kVine: {
        if (dir == Down) return state;
        u16 s = state;
        const BlockPos up = above(pos);
        const u16 aboveState = level.block(up);
        if (prop(s, "up") == "true") s = withFlag(s, "up", vineCanAttach(aboveState, Up));
        for (Direction d : kHorizontal) {
            const char* face = kDirName[d];
            if (prop(s, face) != "true") continue;
            bool support = vineCanAttach(level.block(rel(pos, d)), opposite(d));
            if (!support) support = isVine(aboveState) && prop(aboveState, face) == "true";
            s = withFlag(s, face, support);
        }
        for (const char* face : {"up", "north", "east", "south", "west"})
            if (prop(s, face) == "true") return s;
        return air;
    }
    case kFence: {
        if (dir == Up || dir == Down) return state;
        const Direction back = opposite(dir);
        bool connect = !connectionException(neighbor) && sturdyFace(neighbor, back);
        if (!connect && (info(neighbor) & kFenceTag))
            connect = (info(neighbor) & kWoodenFence) == (info(state) & kWoodenFence);
        if (!connect && kind(neighbor) == kFenceGate) {
            const std::string_view facing = prop(neighbor, "facing");
            const char gateAxis = facing == "north" || facing == "south" ? 'z' : 'x';
            connect = gateAxis == axisOf(clockWise(back));
        }
        return withFlag(state, kDirName[dir], connect);
    }
    case kPane: {
        if (dir == Up || dir == Down) return state;
        const bool attach = (!connectionException(neighbor) && sturdyFace(neighbor, opposite(dir))) ||
                            kind(neighbor) == kPane || (info(neighbor) & kWallTag) != 0;
        return withFlag(state, kDirName[dir], attach);
    }
    default:
        return state;
    }
}

// A block set from JSON ("#tag", a block id, or a list of ids): HolderSet<Block>.
struct BlockSet {
    std::string tag;
    std::vector<std::string> names;
    std::shared_ptr<std::atomic<u8>[]> cache = std::make_shared<std::atomic<u8>[]>(65536);  // 0 unknown, 1 out, 2 in

    static BlockSet parse(const json& j) {
        BlockSet s;
        if (j.is_string()) {
            const std::string& v = j.get_ref<const std::string&>();
            if (!v.empty() && v[0] == '#')
                s.tag = v.substr(1);
            else
                s.names.push_back(v.find(':') == std::string::npos ? "minecraft:" + v : v);
        } else {
            for (const json& e : j) {
                const std::string& v = e.get_ref<const std::string&>();
                s.names.push_back(v.find(':') == std::string::npos ? "minecraft:" + v : v);
            }
        }
        return s;
    }
    bool contains(const Blocks& b, u16 id) const {
        u8 c = cache[id].load(std::memory_order_relaxed);
        if (!c) {
            bool in = false;
            if (!tag.empty()) {
                in = b.tag(id, tag);
            } else {
                const std::string_view n = b.name(id);
                for (const std::string& s : names) in = in || n == s;
            }
            c = in ? 2 : 1;
            cache[id].store(c, std::memory_order_relaxed);
        }
        return c == 2;
    }
};

// ---- updateShapeAtEdge over the BitSetDiscreteVoxelShape TreeFeature builds ---------------
struct VoxelShape {
    int xs, ys, zs;
    std::vector<u8> bits;
    VoxelShape(int x, int y, int z) : xs(x), ys(y), zs(z), bits(size_t(x) * y * z, 0) {}
    bool full(int x, int y, int z) const { return bits[(size_t(x) * ys + y) * zs + z] != 0; }
    void fill(int x, int y, int z) { bits[(size_t(x) * ys + y) * zs + z] = 1; }
};

// StructureTemplate.updateShapeAtEdge: forAllFaces walks z, then y, then x runs (AxisCycle NONE, FORWARD,
// BACKWARD) and updates both sides of every face between a filled and an empty cell.
void updateShapeAtEdge(GenRegion& level, const Blocks& b, const VoxelShape& shape, const BlockPos& origin) {
    auto face = [&](Direction dir, int x, int y, int z) {
        const BlockPos pos = origin.offset(x, y, z), npos = rel(pos, dir);
        const u16 state = level.block(pos), neighbor = level.block(npos);
        const u16 updated = b.updateShape(level, state, pos, dir, neighbor);
        if (updated != state) level.setBlock(pos, updated);
        const u16 updatedNeighbor = b.updateShape(level, neighbor, npos, opposite(dir), updated);
        if (updatedNeighbor != neighbor) level.setBlock(npos, updatedNeighbor);
    };
    // c runs along z (north/south faces), y (down/up), x (west/east).
    for (int a = 0; a < shape.xs; ++a)
        for (int bb = 0; bb < shape.ys; ++bb) {
            bool last = false;
            for (int c = 0; c <= shape.zs; ++c) {
                const bool full = c != shape.zs && shape.full(a, bb, c);
                if (!last && full) face(North, a, bb, c);
                if (last && !full) face(South, a, bb, c - 1);
                last = full;
            }
        }
    for (int a = 0; a < shape.zs; ++a)
        for (int bb = 0; bb < shape.xs; ++bb) {
            bool last = false;
            for (int c = 0; c <= shape.ys; ++c) {
                const bool full = c != shape.ys && shape.full(bb, c, a);
                if (!last && full) face(Down, bb, c, a);
                if (last && !full) face(Up, bb, c - 1, a);
                last = full;
            }
        }
    for (int a = 0; a < shape.ys; ++a)
        for (int bb = 0; bb < shape.zs; ++bb) {
            bool last = false;
            for (int c = 0; c <= shape.xs; ++c) {
                const bool full = c != shape.xs && shape.full(c, a, bb);
                if (!last && full) face(West, c, a, bb);
                if (last && !full) face(East, c - 1, a, bb);
                last = full;
            }
        }
}

std::vector<BlockPos> sortedByY(const JavaPosSet& set) {  // ObjectArrayList(set).sort(comparingInt(getY)): stable
    std::vector<BlockPos> v = set.toList();
    std::stable_sort(v.begin(), v.end(), [](const BlockPos& a, const BlockPos& b) { return a.y < b.y; });
    return v;
}

template <class T>
void shuffle(std::vector<T>& list, RandomSource& r) {  // Util.shuffle
    for (int i = int(list.size()); i > 1; --i) {
        const int swapTo = r.nextInt(i);
        std::swap(list[size_t(i - 1)], list[size_t(swapTo)]);
    }
}

// ---- tree decorators ---------------------------------------------------------------------
struct DecoratorContext {  // TreeDecorator.Context
    GenRegion& level;
    JavaPosSet* decorations;  // TreeFeature's decoration set; fallen trees write directly
    RandomSource& random;
    const Blocks& b;
    std::vector<BlockPos> logs, leaves, roots;

    void setBlock(const BlockPos& p, u16 state) {
        if (decorations) decorations->add(p);
        level.setBlock(p, state);
    }
    u16 at(const BlockPos& p) const { return level.block(p); }
    bool isAir(const BlockPos& p) const { return b.isAir(level.block(p)); }
    bool isReplaceable(const BlockPos& p) const { return b.canBeReplaced(level.block(p)); }
    void placeVine(const BlockPos& p, Direction face) {
        for (int i = 0; i < 4; ++i)
            if (kHorizontal[i] == face) setBlock(p, b.vineWith[i]);
    }
    bool isWaterOrWaterNearby(const BlockPos& p) const {
        return b.isWaterBlock(at(p)) || b.isWaterBlock(at(rel(p, East))) || b.isWaterBlock(at(rel(p, West))) ||
               b.isWaterBlock(at(rel(p, North))) || b.isWaterBlock(at(rel(p, South)));
    }
};

// TreeFeature.getLowestTrunkOrRootOfTree
std::vector<BlockPos> lowestTrunkOrRoot(const DecoratorContext& c) {
    std::vector<BlockPos> out;
    if (c.roots.empty()) {
        out = c.logs;
    } else if (!c.logs.empty() && c.roots.front().y == c.logs.front().y) {
        out = c.logs;
        out.insert(out.end(), c.roots.begin(), c.roots.end());
    } else {
        out = c.roots;
    }
    return out;
}

class TreeDecorator {
public:
    virtual ~TreeDecorator() = default;
    virtual void place(DecoratorContext& c) const = 0;
};

class TrunkVineDecorator final : public TreeDecorator {
public:
    void place(DecoratorContext& c) const override {
        for (const BlockPos& pos : c.logs) {
            if (c.random.nextInt(3) > 0 && c.isAir(rel(pos, West))) c.placeVine(rel(pos, West), East);
            if (c.random.nextInt(3) > 0 && c.isAir(rel(pos, East))) c.placeVine(rel(pos, East), West);
            if (c.random.nextInt(3) > 0 && c.isAir(rel(pos, North))) c.placeVine(rel(pos, North), South);
            if (c.random.nextInt(3) > 0 && c.isAir(rel(pos, South))) c.placeVine(rel(pos, South), North);
        }
    }
};

class LeaveVineDecorator final : public TreeDecorator {
public:
    explicit LeaveVineDecorator(float probability) : probability_(probability) {}
    void place(DecoratorContext& c) const override {
        for (const BlockPos& pos : c.leaves) {
            if (c.random.nextFloat() < probability_ && c.isAir(rel(pos, West))) hangingVine(c, rel(pos, West), East);
            if (c.random.nextFloat() < probability_ && c.isAir(rel(pos, East))) hangingVine(c, rel(pos, East), West);
            if (c.random.nextFloat() < probability_ && c.isAir(rel(pos, North))) hangingVine(c, rel(pos, North), South);
            if (c.random.nextFloat() < probability_ && c.isAir(rel(pos, South))) hangingVine(c, rel(pos, South), North);
        }
    }

private:
    static void hangingVine(DecoratorContext& c, const BlockPos& pos, Direction face) {
        c.placeVine(pos, face);
        BlockPos p = below(pos);
        for (int maxDir = 4; c.isAir(p) && maxDir > 0; --maxDir) {
            c.placeVine(p, face);
            p = below(p);
        }
    }
    float probability_;
};

class CocoaDecorator final : public TreeDecorator {
public:
    explicit CocoaDecorator(float probability) : probability_(probability) {}
    void place(DecoratorContext& c) const override {
        if (c.random.nextFloat() >= probability_ || c.logs.empty()) return;
        const int treeY = c.logs.front().y;
        for (const BlockPos& pos : c.logs) {
            if (pos.y - treeY > 2) continue;
            for (Direction d : kHorizontal) {
                if (c.random.nextFloat() <= 0.25f) {
                    const BlockPos cocoaPos = rel(pos, opposite(d));
                    if (c.isAir(cocoaPos)) {
                        const int age = c.random.nextInt(3);
                        c.setBlock(cocoaPos, c.b.with(c.b.withInt(c.b.cocoa, "age", age), "facing", kDirName[d]));
                    }
                }
            }
        }
    }

private:
    float probability_;
};

class BeehiveDecorator final : public TreeDecorator {
public:
    explicit BeehiveDecorator(float probability) : probability_(probability) {}
    void place(DecoratorContext& c) const override {
        if (c.logs.empty()) return;
        if (c.random.nextFloat() >= probability_) return;
        int hiveY;
        if (!c.leaves.empty()) {
            hiveY = std::max(c.leaves.front().y - 1, c.logs.front().y + 1);
        } else {
            const int extra = c.random.nextInt(3);
            hiveY = std::min(c.logs.front().y + 1 + extra, c.logs.back().y);
        }
        std::vector<BlockPos> placements;
        for (const BlockPos& pos : c.logs)
            if (pos.y == hiveY)
                for (Direction d : {East, South, West}) placements.push_back(rel(pos, d));
        if (placements.empty()) return;
        shuffle(placements, c.random);
        for (const BlockPos& p : placements) {
            if (c.isAir(p) && c.isAir(rel(p, South))) {
                c.setBlock(p, c.b.beeNest);
                // The nest's block entity gets 2-3 bees with random ticks in the hive; only the draws matter here.
                if (c.at(p) == c.b.beeNest) {
                    const int bees = 2 + c.random.nextInt(2);
                    for (int i = 0; i < bees; ++i) c.random.nextInt(599);
                }
                return;
            }
        }
    }

private:
    float probability_;
};

class AlterGroundDecorator final : public TreeDecorator {
public:
    explicit AlterGroundDecorator(std::unique_ptr<StateProvider> provider) : provider_(std::move(provider)) {}
    void place(DecoratorContext& c) const override {
        const std::vector<BlockPos> positions = lowestTrunkOrRoot(c);
        if (positions.empty()) return;
        const int minY = positions.front().y;
        for (const BlockPos& pos : positions) {
            if (pos.y != minY) continue;
            placeCircle(c, pos.offset(-1, 0, -1));
            placeCircle(c, pos.offset(2, 0, -1));
            placeCircle(c, pos.offset(-1, 0, 2));
            placeCircle(c, pos.offset(2, 0, 2));
            for (int i = 0; i < 5; ++i) {
                const int placement = c.random.nextInt(64);
                const int xx = placement % 8, zz = placement / 8;
                if (xx == 0 || xx == 7 || zz == 0 || zz == 7) placeCircle(c, pos.offset(-3 + xx, 0, -3 + zz));
            }
        }
    }

private:
    void placeCircle(DecoratorContext& c, const BlockPos& pos) const {
        for (int xx = -2; xx <= 2; ++xx)
            for (int zz = -2; zz <= 2; ++zz)
                if (std::abs(xx) != 2 || std::abs(zz) != 2) placeBlockAt(c, pos.offset(xx, 0, zz));
    }
    void placeBlockAt(DecoratorContext& c, const BlockPos& pos) const {
        for (int dy = 2; dy >= -3; --dy) {
            const BlockPos cursor = above(pos, dy);
            const u16 replaceWith = provider_->optionalState(c.level, c.random, cursor);
            if (replaceWith != StateProvider::kNone) {
                c.setBlock(cursor, replaceWith);
                break;
            }
            if (!c.isAir(cursor) && dy < 0) break;
        }
    }
    std::unique_ptr<StateProvider> provider_;
};

class AttachedToLeavesDecorator final : public TreeDecorator {
public:
    AttachedToLeavesDecorator(float probability, int exclusionXZ, int exclusionY,
                              std::unique_ptr<StateProvider> provider, int requiredEmpty,
                              std::vector<Direction> directions)
        : probability_(probability),
          exclusionXZ_(exclusionXZ),
          exclusionY_(exclusionY),
          provider_(std::move(provider)),
          requiredEmpty_(requiredEmpty),
          directions_(std::move(directions)) {}
    void place(DecoratorContext& c) const override {
        JavaPosSet blacklist;
        std::vector<BlockPos> leaves = c.leaves;
        shuffle(leaves, c.random);
        for (const BlockPos& leafPos : leaves) {
            const Direction d = directions_[size_t(c.random.nextInt(int(directions_.size())))];
            const BlockPos placement = rel(leafPos, d);
            if (!blacklist.contains(placement) && c.random.nextFloat() < probability_ &&
                hasRequiredEmpty(c, leafPos, d)) {
                for (int x = -exclusionXZ_; x <= exclusionXZ_; ++x)
                    for (int y = -exclusionY_; y <= exclusionY_; ++y)
                        for (int z = -exclusionXZ_; z <= exclusionXZ_; ++z) blacklist.add(placement.offset(x, y, z));
                c.setBlock(placement, provider_->state(c.level, c.random, placement));
            }
        }
    }

private:
    bool hasRequiredEmpty(const DecoratorContext& c, const BlockPos& leafPos, Direction d) const {
        for (int i = 1; i <= requiredEmpty_; ++i)
            if (!c.isAir(rel(leafPos, d, i))) return false;
        return true;
    }
    float probability_;
    int exclusionXZ_, exclusionY_;
    std::unique_ptr<StateProvider> provider_;
    int requiredEmpty_;
    std::vector<Direction> directions_;
};

class AttachedToLogsDecorator final : public TreeDecorator {
public:
    AttachedToLogsDecorator(float probability, std::unique_ptr<StateProvider> provider,
                            std::vector<Direction> directions)
        : probability_(probability), provider_(std::move(provider)), directions_(std::move(directions)) {}
    void place(DecoratorContext& c) const override {
        std::vector<BlockPos> logs = c.logs;
        shuffle(logs, c.random);
        for (const BlockPos& logPos : logs) {
            const Direction d = directions_[size_t(c.random.nextInt(int(directions_.size())))];
            const BlockPos placement = rel(logPos, d);
            if (c.random.nextFloat() <= probability_ && c.isAir(placement))
                c.setBlock(placement, provider_->state(c.level, c.random, placement));
        }
    }

private:
    float probability_;
    std::unique_ptr<StateProvider> provider_;
    std::vector<Direction> directions_;
};

class PaleMossDecorator final : public TreeDecorator {
public:
    PaleMossDecorator(float leaves, float trunk, float ground, std::shared_ptr<const ConfiguredFeature> mossPatch)
        : leaves_(leaves), trunk_(trunk), ground_(ground), mossPatch_(std::move(mossPatch)) {}
    void place(DecoratorContext& c) const override {
        std::vector<BlockPos> logs = c.logs;
        shuffle(logs, c.random);
        if (logs.empty()) return;
        BlockPos origin = logs.front();  // Collections.min: the first lowest in shuffled order
        for (const BlockPos& p : logs)
            if (p.y < origin.y) origin = p;
        if (c.random.nextFloat() < ground_ && mossPatch_) mossPatch_->place(c.level, c.random, above(origin));
        for (const BlockPos& pos : c.logs)
            if (c.random.nextFloat() < trunk_ && c.isAir(below(pos))) addMossHanger(c, below(pos));
        for (const BlockPos& pos : c.leaves)
            if (c.random.nextFloat() < leaves_ && c.isAir(below(pos))) addMossHanger(c, below(pos));
    }

private:
    static void addMossHanger(DecoratorContext& c, BlockPos pos) {
        while (c.isAir(below(pos)) && !(c.random.nextFloat() < 0.5f)) {
            c.setBlock(pos, c.b.mossBody);
            pos = below(pos);
        }
        c.setBlock(pos, c.b.mossTip);
    }
    float leaves_, trunk_, ground_;
    std::shared_ptr<const ConfiguredFeature> mossPatch_;
};

class CreakingHeartDecorator final : public TreeDecorator {
public:
    explicit CreakingHeartDecorator(float probability) : probability_(probability) {}
    void place(DecoratorContext& c) const override {
        if (c.logs.empty()) return;
        if (c.random.nextFloat() >= probability_) return;
        std::vector<BlockPos> placements = c.logs;
        shuffle(placements, c.random);
        for (const BlockPos& pos : placements) {
            bool enclosed = true;
            for (Direction d : kAllDirections)
                if (!c.b.isLog(c.at(rel(pos, d)))) {
                    enclosed = false;
                    break;
                }
            if (enclosed) {
                c.setBlock(pos, c.b.creakingHeart);
                return;
            }
        }
    }

private:
    float probability_;
};

class PlaceOnGroundDecorator final : public TreeDecorator {
public:
    PlaceOnGroundDecorator(int tries, int radius, int height, std::unique_ptr<StateProvider> provider)
        : tries_(tries), radius_(radius), height_(height), provider_(std::move(provider)) {}
    void place(DecoratorContext& c) const override {
        const std::vector<BlockPos> positions = lowestTrunkOrRoot(c);
        if (positions.empty()) return;
        const BlockPos origin = positions.front();
        const int minY = origin.y;
        int minX = origin.x, maxX = origin.x, minZ = origin.z, maxZ = origin.z;
        for (const BlockPos& p : positions)
            if (p.y == minY) {
                minX = std::min(minX, p.x);
                maxX = std::max(maxX, p.x);
                minZ = std::min(minZ, p.z);
                maxZ = std::max(maxZ, p.z);
            }
        // BoundingBox.inflatedBy(radius, height, radius)
        const int x0 = minX - radius_, x1 = maxX + radius_, y0 = minY - height_, y1 = minY + height_;
        const int z0 = minZ - radius_, z1 = maxZ + radius_;
        for (int i = 0; i < tries_; ++i) {
            const int x = c.random.nextIntBetweenInclusive(x0, x1);
            const int y = c.random.nextIntBetweenInclusive(y0, y1);
            const int z = c.random.nextIntBetweenInclusive(z0, z1);
            const BlockPos pos{x, y, z}, abovePos = above(pos);
            const u16 aboveState = c.at(abovePos);
            if ((c.b.isAir(aboveState) || c.b.isVine(aboveState)) && c.b.facts.isSolidRender(c.at(pos)) &&
                c.level.height(Heightmap::MotionBlockingNoLeaves, pos.x, pos.z) <= abovePos.y)
                c.setBlock(abovePos, provider_->state(c.level, c.random, abovePos));
        }
    }

private:
    int tries_, radius_, height_;
    std::unique_ptr<StateProvider> provider_;
};

class ShelfMushroomDecorator final : public TreeDecorator {
public:
    explicit ShelfMushroomDecorator(float probability) : probability_(probability) {}
    void place(DecoratorContext& c) const override {
        if (c.random.nextFloat() >= probability_) return;
        if (c.logs.empty()) return;
        if (c.logs.front().y == c.logs.back().y)
            placeOnFallenLog(c);
        else
            placeOnStandingTree(c);
    }

private:
    static void placeOnStandingTree(DecoratorContext& c) {
        const Direction first = kHorizontal[c.random.nextInt(4)];
        const Direction directions[2] = {first, clockWise(first)};
        const int baseY = c.logs.front().y;
        for (const BlockPos& logPos : c.logs) {
            const int dy = logPos.y - baseY;
            if (dy < 1 || dy > 4) continue;
            for (Direction facing : directions) {
                if (!(c.random.nextFloat() > 0.25f) && tryOnStandingTree(c, logPos, facing)) break;
            }
        }
    }
    static void placeOnFallenLog(DecoratorContext& c) {
        const BlockPos& first = c.logs.front();
        const BlockPos& last = c.logs.back();
        const bool alongX = first.x != last.x;
        const Direction directions[2] = {alongX ? North : East, alongX ? South : West};
        for (const BlockPos& logPos : c.logs)
            for (Direction facing : directions)
                if (!(c.random.nextFloat() > 0.25f)) tryOnFallenTree(c, logPos, facing);
    }
    static bool tryOnStandingTree(DecoratorContext& c, const BlockPos& logPos, Direction facing) {
        const BlockPos pos = rel(logPos, facing);
        if (!replaceable(c, pos)) return false;
        if (c.b.is(c.at(below(pos)), "minecraft:shelf_mushroom")) return false;
        placeMushroom(c, pos, facing);
        return true;
    }
    static void tryOnFallenTree(DecoratorContext& c, const BlockPos& logPos, Direction facing) {
        const BlockPos pos = rel(logPos, facing);
        if (replaceable(c, pos) && !adjacentMushroom(c, pos) && !adjacentMushroom(c, logPos))
            placeMushroom(c, pos, facing);
    }
    static bool adjacentMushroom(const DecoratorContext& c, const BlockPos& pos) {
        for (Direction d : kHorizontal)
            if (c.b.is(c.at(rel(pos, d)), "minecraft:shelf_mushroom")) return true;
        return false;
    }
    static bool replaceable(const DecoratorContext& c, const BlockPos& pos) {
        return c.isReplaceable(pos) && !c.isWaterOrWaterNearby(pos);
    }
    static void placeMushroom(DecoratorContext& c, const BlockPos& pos, Direction facing) {
        const int age = c.random.nextInt(2);
        c.setBlock(pos, c.b.with(c.b.withInt(c.b.shelfMushroom, "age", age), "facing", kDirName[facing]));
    }
    float probability_;
};

std::vector<Direction> parseDirections(const json& j) {
    std::vector<Direction> out;
    for (const json& d : j) out.push_back(parseDirection(stripNs(d.get_ref<const std::string&>())));
    if (out.empty()) throw std::runtime_error("empty decorator directions");
    return out;
}

std::unique_ptr<TreeDecorator> parseDecorator(const json& j, const FeatureEnv& env) {
    const std::string type = typeOf(j);
    if (type == "trunk_vine") return std::make_unique<TrunkVineDecorator>();
    if (type == "leave_vine") return std::make_unique<LeaveVineDecorator>(jsonFloat(j.at("probability")));
    if (type == "cocoa") return std::make_unique<CocoaDecorator>(jsonFloat(j.at("probability")));
    if (type == "beehive") return std::make_unique<BeehiveDecorator>(jsonFloat(j.at("probability")));
    if (type == "creaking_heart") return std::make_unique<CreakingHeartDecorator>(jsonFloat(j.at("probability")));
    if (type == "shelf_mushroom") return std::make_unique<ShelfMushroomDecorator>(jsonFloat(j.at("probability")));
    if (type == "alter_ground") return std::make_unique<AlterGroundDecorator>(env.stateProvider(j.at("provider")));
    if (type == "attached_to_leaves")
        return std::make_unique<AttachedToLeavesDecorator>(
            jsonFloat(j.at("probability")), j.at("exclusion_radius_xz").get<int>(),
            j.at("exclusion_radius_y").get<int>(), env.stateProvider(j.at("block_provider")),
            j.at("required_empty_blocks").get<int>(), parseDirections(j.at("directions")));
    if (type == "attached_to_logs")
        return std::make_unique<AttachedToLogsDecorator>(jsonFloat(j.at("probability")),
                                                         env.stateProvider(j.at("block_provider")),
                                                         parseDirections(j.at("directions")));
    if (type == "place_on_ground")
        return std::make_unique<PlaceOnGroundDecorator>(jsonInt(j, "tries", 128), jsonInt(j, "radius", 2),
                                                        jsonInt(j, "height", 1),
                                                        env.stateProvider(j.at("block_state_provider")));
    if (type == "pale_moss") {
        std::shared_ptr<const ConfiguredFeature> patch;
        try {
            patch = env.configured(json("minecraft:pale_moss_patch"));
        } catch (const std::exception& e) {
            LOG("pale_moss decorator: no pale_moss_patch feature (%s)", e.what());
        }
        return std::make_unique<PaleMossDecorator>(jsonFloat(j.at("leaves_probability")),
                                                   jsonFloat(j.at("trunk_probability")),
                                                   jsonFloat(j.at("ground_probability")), std::move(patch));
    }
    throw std::runtime_error("unknown tree decorator " + type);
}

std::vector<std::unique_ptr<TreeDecorator>> parseDecorators(const json& j, const FeatureEnv& env) {
    std::vector<std::unique_ptr<TreeDecorator>> out;
    for (const json& d : j) out.push_back(parseDecorator(d, env));
    return out;
}

// ---- TreeFeature ----------------------------------------------------------------------------
class TreeFeature;

struct FoliageAttachment {
    BlockPos pos;
    int radiusOffset = 0, foliageHeightOffset = 0, sizeX = 1, sizeZ = 1;
    FoliageAttachment(const BlockPos& p, int radiusOffsetXZ, bool doubleTrunk)
        : pos(p), radiusOffset(radiusOffsetXZ), sizeX(doubleTrunk ? 2 : 1), sizeZ(doubleTrunk ? 2 : 1) {}
    bool doubleTrunk() const { return sizeX == 2 && sizeZ == 2; }
};

// One TreeFeature.place call: the level plus the root/trunk/foliage/decoration position sets.
struct TreeRun {
    GenRegion& level;
    const TreeFeature& tree;
    const Blocks& b;
    JavaPosSet roots, trunks, foliage, decorations;

    void setRoot(const BlockPos& p, u16 s) {
        roots.add(p);
        level.setBlock(p, s);
    }
    void setTrunk(const BlockPos& p, u16 s) {
        trunks.add(p);
        level.setBlock(p, s);
    }
    void setFoliage(const BlockPos& p, u16 s) {
        foliage.add(p);
        level.setBlock(p, s);
    }
    bool foliageSet(const BlockPos& p) const { return foliage.contains(p); }
    u16 at(const BlockPos& p) const { return level.block(p); }
};

class FeatureSize {
public:
    virtual ~FeatureSize() = default;
    virtual int sizeAtHeight(int treeHeight, int yo) const = 0;
    int minClippedHeight = -1;  // OptionalInt.empty()
};
class TwoLayersFeatureSize final : public FeatureSize {
public:
    explicit TwoLayersFeatureSize(const json& j)
        : limit_(jsonInt(j, "limit", 1)), lower_(jsonInt(j, "lower_size", 0)), upper_(jsonInt(j, "upper_size", 1)) {
        minClippedHeight = jsonInt(j, "min_clipped_height", -1);
    }
    int sizeAtHeight(int, int yo) const override { return yo < limit_ ? lower_ : upper_; }

private:
    int limit_, lower_, upper_;
};
class ThreeLayersFeatureSize final : public FeatureSize {
public:
    explicit ThreeLayersFeatureSize(const json& j)
        : limit_(jsonInt(j, "limit", 1)),
          upperLimit_(jsonInt(j, "upper_limit", 1)),
          lower_(jsonInt(j, "lower_size", 0)),
          middle_(jsonInt(j, "middle_size", 1)),
          upper_(jsonInt(j, "upper_size", 1)) {
        minClippedHeight = jsonInt(j, "min_clipped_height", -1);
    }
    int sizeAtHeight(int treeHeight, int yo) const override {
        if (yo < limit_) return lower_;
        return yo >= treeHeight - upperLimit_ ? upper_ : middle_;
    }

private:
    int limit_, upperLimit_, lower_, middle_, upper_;
};

class TrunkPlacer {
public:
    explicit TrunkPlacer(const json& j)
        : baseHeight_(j.at("base_height").get<int>()), randA_(j.at("height_rand_a").get<int>()),
          randB_(j.at("height_rand_b").get<int>()) {}
    virtual ~TrunkPlacer() = default;
    int treeHeight(RandomSource& r) const {
        const int a = r.nextInt(randA_ + 1);
        const int b = r.nextInt(randB_ + 1);
        return baseHeight_ + a + b;
    }
    virtual std::vector<FoliageAttachment> placeTrunk(TreeRun& run, RandomSource& r, int treeHeight,
                                                      const BlockPos& origin) const = 0;
    virtual bool validTreePos(const TreeRun& run, const BlockPos& p) const { return run.b.validTreePos(run.at(p)); }
    bool isFree(const TreeRun& run, const BlockPos& p) const { return validTreePos(run, p) || run.b.isLog(run.at(p)); }

protected:
    static void placeBelowTrunkBlock(TreeRun& run, RandomSource& r, const BlockPos& p);
    // placeLog with an optional sideways axis (the state modifier is trySetValue(AXIS, axis)).
    bool placeLog(TreeRun& run, RandomSource& r, const BlockPos& p, char axis = 0) const;
    void placeLogIfFree(TreeRun& run, RandomSource& r, const BlockPos& p) const {
        if (isFree(run, p)) placeLog(run, r, p);
    }

    int baseHeight_, randA_, randB_;
};

class FoliagePlacer {
public:
    FoliagePlacer(const json& j, const FeatureEnv& env)
        : radius_(env.intProvider(j.at("radius"))), offset_(env.intProvider(j.at("offset"))) {}
    virtual ~FoliagePlacer() = default;
    void createFoliage(TreeRun& run, RandomSource& r, int treeHeight, const FoliageAttachment& att, int foliageHeight,
                       int leafRadius) const {
        const int offset = offset_->sample(r);
        create(run, r, treeHeight, att, foliageHeight, leafRadius, offset);
    }
    virtual int foliageHeight(RandomSource& r, int treeHeight) const = 0;
    virtual int foliageRadius(RandomSource& r, int trunkHeight) const {
        (void)trunkHeight;
        return radius_->sample(r);
    }

protected:
    virtual void create(TreeRun& run, RandomSource& r, int treeHeight, const FoliageAttachment& att, int foliageHeight,
                        int leafRadius, int offset) const = 0;
    virtual bool shouldSkipLocation(RandomSource& r, int dx, int y, int dz, int currentRadius,
                                    bool doubleTrunk) const = 0;
    virtual bool shouldSkipLocationSigned(RandomSource& r, int dx, int y, int dz, int currentRadius,
                                          bool doubleTrunk) const {
        int minDx, minDz;
        if (doubleTrunk) {
            minDx = std::min(std::abs(dx), std::abs(dx - 1));
            minDz = std::min(std::abs(dz), std::abs(dz - 1));
        } else {
            minDx = std::abs(dx);
            minDz = std::abs(dz);
        }
        return shouldSkipLocation(r, minDx, y, minDz, currentRadius, doubleTrunk);
    }
    void placeLeavesRow(TreeRun& run, RandomSource& r, const BlockPos& origin, int currentRadius, int y,
                        bool doubleTrunk) const {
        const int offset = doubleTrunk ? 1 : 0;
        for (int dx = -currentRadius; dx <= currentRadius + offset; ++dx)
            for (int dz = -currentRadius; dz <= currentRadius + offset; ++dz)
                if (!shouldSkipLocationSigned(r, dx, y, dz, currentRadius, doubleTrunk))
                    tryPlaceLeaf(run, r, origin.offset(dx, y, dz));
    }
    void placeLeavesRowWithHangingLeavesBelow(TreeRun& run, RandomSource& r, const BlockPos& origin, int currentRadius,
                                              int y, bool doubleTrunk, float hangingChance,
                                              float extensionChance) const {
        placeLeavesRow(run, r, origin, currentRadius, y, doubleTrunk);
        const int offset = doubleTrunk ? 1 : 0;
        const BlockPos logPos = below(origin);
        for (Direction alongEdge : kHorizontal) {
            const Direction toEdge = clockWise(alongEdge);
            const int offsetToEdge = isPositive(toEdge) ? currentRadius + offset : currentRadius;
            BlockPos pos = rel(rel(origin.offset(0, y - 1, 0), toEdge, offsetToEdge), alongEdge, -currentRadius);
            for (int along = -currentRadius; along < currentRadius + offset; ++along) {
                const bool leavesAbove = run.foliageSet(above(pos));
                if (leavesAbove && tryPlaceExtension(run, r, hangingChance, logPos, pos))
                    tryPlaceExtension(run, r, extensionChance, logPos, below(pos));
                pos = rel(pos, alongEdge);
            }
        }
    }
    static bool tryPlaceExtension(TreeRun& run, RandomSource& r, float chance, const BlockPos& logPos,
                                  const BlockPos& pos) {
        if (distManhattan(pos, logPos) >= 7) return false;
        return r.nextFloat() > chance ? false : tryPlaceLeaf(run, r, pos);
    }
    static bool tryPlaceLeaf(TreeRun& run, RandomSource& r, const BlockPos& pos);

    std::unique_ptr<IntProvider> radius_, offset_;
};

class RootPlacer {
public:
    RootPlacer(const json& j, const FeatureEnv& env)
        : trunkOffsetY_(env.intProvider(j.at("trunk_offset_y"))),
          rootProvider_(env.stateProvider(j.at("root_provider"))) {
        if (auto it = j.find("above_root_placement"); it != j.end()) {
            aboveRootProvider_ = env.stateProvider(it->at("above_root_provider"));
            aboveRootChance_ = jsonFloat(it->at("above_root_placement_chance"));
        }
    }
    virtual ~RootPlacer() = default;
    BlockPos trunkOrigin(const BlockPos& origin, RandomSource& r) const {
        return above(origin, trunkOffsetY_->sample(r));
    }
    virtual bool placeRoots(TreeRun& run, RandomSource& r, const BlockPos& origin,
                            const BlockPos& trunkOrigin) const = 0;

protected:
    virtual bool canPlaceRoot(const TreeRun& run, const BlockPos& p) const { return run.b.validTreePos(run.at(p)); }
    virtual void placeRoot(TreeRun& run, RandomSource& r, const BlockPos& p) const {
        if (!canPlaceRoot(run, p)) return;
        run.setRoot(p, waterlogged(run, p, rootProvider_->state(run.level, r, p)));
        if (aboveRootProvider_) {
            const BlockPos up = above(p);
            if (r.nextFloat() < aboveRootChance_ && run.b.isAir(run.at(up)))
                run.setRoot(up, waterlogged(run, up, aboveRootProvider_->state(run.level, r, up)));
        }
    }
    static u16 waterlogged(const TreeRun& run, const BlockPos& p, u16 state) {
        if (!run.b.has(state, "waterlogged")) return state;
        return run.b.withFlag(state, "waterlogged", run.b.isAnyWaterFluid(run.at(p)));
    }

    std::unique_ptr<IntProvider> trunkOffsetY_;
    std::unique_ptr<StateProvider> rootProvider_, aboveRootProvider_;
    float aboveRootChance_ = 0;
};

class TreeFeature final : public ConfiguredFeature {
public:
    TreeFeature(const json& j, const FeatureEnv& env, std::shared_ptr<const Blocks> blocks);
    bool place(GenRegion& level, RandomSource& random, const BlockPos& origin) const override;

    const Blocks& b() const { return *blocks_; }
    std::unique_ptr<StateProvider> trunkProvider, foliageProvider, belowTrunkProvider;

private:
    bool doPlace(TreeRun& run, RandomSource& r, const BlockPos& origin) const;
    int maxFreeTreeHeight(const TreeRun& run, int maxTreeHeight, const BlockPos& treePos) const;
    VoxelShape updateLeaves(GenRegion& level, int x0, int y0, int z0, int x1, int y1, int z1, const TreeRun& run) const;

    std::shared_ptr<const Blocks> blocks_;
    std::unique_ptr<TrunkPlacer> trunkPlacer_;
    std::unique_ptr<FoliagePlacer> foliagePlacer_;
    std::unique_ptr<RootPlacer> rootPlacer_;
    std::unique_ptr<FeatureSize> minimumSize_;
    std::vector<std::unique_ptr<TreeDecorator>> decorators_;
    bool ignoreVines_ = false;
};

void TrunkPlacer::placeBelowTrunkBlock(TreeRun& run, RandomSource& r, const BlockPos& p) {
    const u16 s = run.tree.belowTrunkProvider->optionalState(run.level, r, p);
    if (s != StateProvider::kNone) run.setTrunk(p, s);
}

bool TrunkPlacer::placeLog(TreeRun& run, RandomSource& r, const BlockPos& p, char axis) const {
    if (!validTreePos(run, p)) return false;
    u16 s = run.tree.trunkProvider->state(run.level, r, p);
    if (axis) s = run.b.with(s, "axis", std::string_view(&axis, 1));
    run.setTrunk(p, s);
    return true;
}

bool FoliagePlacer::tryPlaceLeaf(TreeRun& run, RandomSource& r, const BlockPos& pos) {
    const u16 current = run.at(pos);
    if (run.b.prop(current, "persistent") == "true" || !run.b.validTreePos(current)) return false;
    u16 s = run.tree.foliageProvider->state(run.level, r, pos);
    if (run.b.has(s, "waterlogged")) s = run.b.withFlag(s, "waterlogged", run.b.isSourceWaterFluid(run.at(pos)));
    run.setFoliage(pos, s);
    return true;
}

// ---- trunk placers ----
class StraightTrunkPlacer final : public TrunkPlacer {
public:
    using TrunkPlacer::TrunkPlacer;
    std::vector<FoliageAttachment> placeTrunk(TreeRun& run, RandomSource& r, int treeHeight,
                                              const BlockPos& origin) const override {
        placeBelowTrunkBlock(run, r, below(origin));
        for (int y = 0; y < treeHeight; ++y) placeLog(run, r, above(origin, y));
        return {FoliageAttachment(above(origin, treeHeight), 0, false)};
    }
};

class ForkingTrunkPlacer final : public TrunkPlacer {
public:
    using TrunkPlacer::TrunkPlacer;
    std::vector<FoliageAttachment> placeTrunk(TreeRun& run, RandomSource& r, int treeHeight,
                                              const BlockPos& origin) const override {
        placeBelowTrunkBlock(run, r, below(origin));
        std::vector<FoliageAttachment> attachments;
        const Direction lean = kHorizontal[r.nextInt(4)];
        const int leanHeight = treeHeight - r.nextInt(4) - 1;
        int leanSteps = 3 - r.nextInt(3);
        int tx = origin.x, tz = origin.z;
        bool hasTop = false;
        int ey = 0;
        for (int yo = 0; yo < treeHeight; ++yo) {
            const int yy = origin.y + yo;
            if (yo >= leanHeight && leanSteps > 0) {
                tx += kDirOffset[lean][0];
                tz += kDirOffset[lean][2];
                --leanSteps;
            }
            if (placeLog(run, r, {tx, yy, tz})) hasTop = true, ey = yy + 1;
        }
        if (hasTop) attachments.emplace_back(BlockPos{tx, ey, tz}, 1, false);
        tx = origin.x;
        tz = origin.z;
        const Direction branch = kHorizontal[r.nextInt(4)];
        if (branch != lean) {
            const int branchPos = leanHeight - r.nextInt(2) - 1;
            int branchSteps = 1 + r.nextInt(3);
            hasTop = false;
            for (int yo = branchPos; yo < treeHeight && branchSteps > 0; --branchSteps) {
                if (yo >= 1) {
                    const int yy = origin.y + yo;
                    tx += kDirOffset[branch][0];
                    tz += kDirOffset[branch][2];
                    if (placeLog(run, r, {tx, yy, tz})) hasTop = true, ey = yy + 1;
                }
                ++yo;
            }
            if (hasTop) attachments.emplace_back(BlockPos{tx, ey, tz}, 0, false);
        }
        return attachments;
    }
};

class GiantTrunkPlacer : public TrunkPlacer {
public:
    using TrunkPlacer::TrunkPlacer;
    std::vector<FoliageAttachment> placeTrunk(TreeRun& run, RandomSource& r, int treeHeight,
                                              const BlockPos& origin) const override {
        const BlockPos down = below(origin);
        placeBelowTrunkBlock(run, r, down);
        placeBelowTrunkBlock(run, r, rel(down, East));
        placeBelowTrunkBlock(run, r, rel(down, South));
        placeBelowTrunkBlock(run, r, rel(rel(down, South), East));
        for (int hh = 0; hh < treeHeight; ++hh) {
            placeLogIfFree(run, r, origin.offset(0, hh, 0));
            if (hh < treeHeight - 1) {
                placeLogIfFree(run, r, origin.offset(1, hh, 0));
                placeLogIfFree(run, r, origin.offset(1, hh, 1));
                placeLogIfFree(run, r, origin.offset(0, hh, 1));
            }
        }
        return {FoliageAttachment(above(origin, treeHeight), 0, true)};
    }
};

class MegaJungleTrunkPlacer final : public GiantTrunkPlacer {
public:
    using GiantTrunkPlacer::GiantTrunkPlacer;
    std::vector<FoliageAttachment> placeTrunk(TreeRun& run, RandomSource& r, int treeHeight,
                                              const BlockPos& origin) const override {
        std::vector<FoliageAttachment> attachments = GiantTrunkPlacer::placeTrunk(run, r, treeHeight, origin);
        for (int branchHeight = treeHeight - 2 - r.nextInt(4); branchHeight > treeHeight / 2;
             branchHeight -= 2 + r.nextInt(4)) {
            const float angle = r.nextFloat() * float(kPi * 2.0);
            int bx = 0, bz = 0;
            for (int b = 0; b < 5; ++b) {
                bx = int(1.5f + mcos(angle) * float(b));
                bz = int(1.5f + msin(angle) * float(b));
                placeLog(run, r, origin.offset(bx, branchHeight - 3 + b / 2, bz));
            }
            attachments.emplace_back(origin.offset(bx, branchHeight, bz), -2, false);
        }
        return attachments;
    }
};

class DarkOakTrunkPlacer final : public TrunkPlacer {
public:
    using TrunkPlacer::TrunkPlacer;
    std::vector<FoliageAttachment> placeTrunk(TreeRun& run, RandomSource& r, int treeHeight,
                                              const BlockPos& origin) const override {
        std::vector<FoliageAttachment> attachments;
        const BlockPos down = below(origin);
        placeBelowTrunkBlock(run, r, down);
        placeBelowTrunkBlock(run, r, rel(down, East));
        placeBelowTrunkBlock(run, r, rel(down, South));
        placeBelowTrunkBlock(run, r, rel(rel(down, South), East));
        const Direction lean = kHorizontal[r.nextInt(4)];
        const int leanHeight = treeHeight - r.nextInt(4);
        int leanSteps = 2 - r.nextInt(3);
        const int x = origin.x, y = origin.y, z = origin.z;
        int tx = x, tz = z;
        const int ey = y + treeHeight - 1;
        for (int dy = 0; dy < treeHeight; ++dy) {
            if (dy >= leanHeight && leanSteps > 0) {
                tx += kDirOffset[lean][0];
                tz += kDirOffset[lean][2];
                --leanSteps;
            }
            const BlockPos p{tx, y + dy, tz};
            const u16 s = run.at(p);
            if (run.b.isAir(s) || run.b.isLeaves(s)) {
                placeLog(run, r, p);
                placeLog(run, r, rel(p, East));
                placeLog(run, r, rel(p, South));
                placeLog(run, r, rel(rel(p, East), South));
            }
        }
        attachments.emplace_back(BlockPos{tx, ey, tz}, 0, true);
        for (int ox = -1; ox <= 2; ++ox)
            for (int oz = -1; oz <= 2; ++oz)
                if ((ox < 0 || ox > 1 || oz < 0 || oz > 1) && r.nextInt(3) <= 0) {
                    const int length = r.nextInt(3) + 2;
                    for (int by = 0; by < length; ++by) placeLog(run, r, {x + ox, ey - by - 1, z + oz});
                    attachments.emplace_back(BlockPos{x + ox, ey, z + oz}, 0, false);
                }
        return attachments;
    }
};

class FancyTrunkPlacer final : public TrunkPlacer {
public:
    using TrunkPlacer::TrunkPlacer;
    std::vector<FoliageAttachment> placeTrunk(TreeRun& run, RandomSource& r, int treeHeight,
                                              const BlockPos& origin) const override {
        const int height = treeHeight + 2;
        const int trunkHeight = jfloor(double(height) * 0.618);
        placeBelowTrunkBlock(run, r, below(origin));
        const double ratio = 1.0 * height / 13.0;
        const int clustersPerY = std::min(1, jfloor(1.382 + ratio * ratio));
        const int trunkTop = origin.y + trunkHeight;
        int relativeY = height - 5;
        std::vector<std::pair<FoliageAttachment, int>> foliageCoords;  // attachment, branch base
        foliageCoords.emplace_back(FoliageAttachment(above(origin, relativeY), 0, false), trunkTop);
        for (; relativeY >= 0; --relativeY) {
            const float shape = treeShape(height, relativeY);
            if (shape < 0.0f) continue;
            for (int i = 0; i < clustersPerY; ++i) {
                const double radius = 1.0 * double(shape) * (double(r.nextFloat()) + 0.328);
                const double angle = double(r.nextFloat() * 2.0f) * kPi;
                const double x = radius * std::sin(angle) + 0.5;
                const double z = radius * std::cos(angle) + 0.5;
                const BlockPos checkStart = origin.offset(jfloor(x), relativeY - 1, jfloor(z));
                const BlockPos checkEnd = above(checkStart, 5);
                if (makeLimb(run, r, checkStart, checkEnd, false)) {
                    const int dx = origin.x - checkStart.x, dz = origin.z - checkStart.z;
                    const double branchHeight = double(checkStart.y) - std::sqrt(double(dx * dx + dz * dz)) * 0.381;
                    const int branchTop = branchHeight > double(trunkTop) ? trunkTop : int(branchHeight);
                    const BlockPos branchBase{origin.x, branchTop, origin.z};
                    if (makeLimb(run, r, branchBase, checkStart, false))
                        foliageCoords.emplace_back(FoliageAttachment(checkStart, 0, false), branchBase.y);
                }
            }
        }
        makeLimb(run, r, origin, above(origin, trunkHeight), true);
        for (const auto& [att, branchBase] : foliageCoords) {  // makeBranches
            const BlockPos baseCoord{origin.x, branchBase, origin.z};
            if (!(baseCoord == att.pos) && trimBranches(height, branchBase - origin.y))
                makeLimb(run, r, baseCoord, att.pos, true);
        }
        std::vector<FoliageAttachment> attachments;
        for (const auto& [att, branchBase] : foliageCoords)
            if (trimBranches(height, branchBase - origin.y)) attachments.push_back(att);
        return attachments;
    }

private:
    bool makeLimb(TreeRun& run, RandomSource& r, const BlockPos& start, const BlockPos& end, bool doPlace) const {
        if (!doPlace && start == end) return true;
        const int ddx = end.x - start.x, ddy = end.y - start.y, ddz = end.z - start.z;
        const int steps = std::max(std::abs(ddx), std::max(std::abs(ddy), std::abs(ddz)));
        const float dx = float(ddx) / float(steps), dy = float(ddy) / float(steps), dz = float(ddz) / float(steps);
        for (int i = 0; i <= steps; ++i) {
            const BlockPos p = start.offset(mfloor(0.5f + float(i) * dx), mfloor(0.5f + float(i) * dy),
                                            mfloor(0.5f + float(i) * dz));
            if (doPlace) {
                placeLog(run, r, p, logAxis(start, p));
            } else if (!isFree(run, p)) {
                return false;
            }
        }
        return true;
    }
    static char logAxis(const BlockPos& start, const BlockPos& p) {
        const int xd = std::abs(p.x - start.x), zd = std::abs(p.z - start.z);
        const int maxd = std::max(xd, zd);
        return maxd > 0 ? (xd == maxd ? 'x' : 'z') : 'y';
    }
    static bool trimBranches(int height, int localY) { return double(localY) >= double(height) * 0.2; }
    static float treeShape(int height, int y) {
        if (float(y) < float(height) * 0.3f) return -1.0f;
        const float radius = float(height) / 2.0f;
        const float adjacent = radius - float(y);
        float distance = float(std::sqrt(double(radius * radius - adjacent * adjacent)));
        if (adjacent == 0.0f)
            distance = radius;
        else if (std::abs(adjacent) >= radius)
            return 0.0f;
        return distance * 0.5f;
    }
};

class BendingTrunkPlacer final : public TrunkPlacer {
public:
    BendingTrunkPlacer(const json& j, const FeatureEnv& env)
        : TrunkPlacer(j), minHeightForLeaves_(jsonInt(j, "min_height_for_leaves", 1)),
          bendLength_(env.intProvider(j.at("bend_length"))) {}
    std::vector<FoliageAttachment> placeTrunk(TreeRun& run, RandomSource& r, int treeHeight,
                                              const BlockPos& origin) const override {
        const Direction direction = kHorizontal[r.nextInt(4)];
        const int logHeight = treeHeight - 1;
        BlockPos pos = origin;
        placeBelowTrunkBlock(run, r, below(pos));
        std::vector<FoliageAttachment> points;
        for (int i = 0; i <= logHeight; ++i) {
            if (i + 1 >= logHeight + r.nextInt(2)) pos = rel(pos, direction);
            if (run.b.validTreePos(run.at(pos))) placeLog(run, r, pos);
            if (i >= minHeightForLeaves_) points.emplace_back(pos, 0, false);
            pos = above(pos);
        }
        const int dirLength = bendLength_->sample(r);
        for (int i = 0; i <= dirLength; ++i) {
            if (run.b.validTreePos(run.at(pos))) placeLog(run, r, pos);
            points.emplace_back(pos, 0, false);
            pos = rel(pos, direction);
        }
        return points;
    }

private:
    int minHeightForLeaves_;
    std::unique_ptr<IntProvider> bendLength_;
};

class UpwardsBranchingTrunkPlacer final : public TrunkPlacer {
public:
    UpwardsBranchingTrunkPlacer(const json& j, const FeatureEnv& env)
        : TrunkPlacer(j), extraBranchSteps_(env.intProvider(j.at("extra_branch_steps"))),
          branchProbability_(jsonFloat(j.at("place_branch_per_log_probability"))),
          extraBranchLength_(env.intProvider(j.at("extra_branch_length"))),
          canGrowThrough_(BlockSet::parse(j.at("can_grow_through"))) {}
    std::vector<FoliageAttachment> placeTrunk(TreeRun& run, RandomSource& r, int treeHeight,
                                              const BlockPos& origin) const override {
        std::vector<FoliageAttachment> attachments;
        for (int heightPos = 0; heightPos < treeHeight; ++heightPos) {
            const int currentHeight = origin.y + heightPos;
            if (placeLog(run, r, {origin.x, currentHeight, origin.z}) && heightPos < treeHeight - 1 &&
                r.nextFloat() < branchProbability_) {
                const Direction branchDir = kHorizontal[r.nextInt(4)];
                const int branchLen = extraBranchLength_->sample(r);
                const int branchPos = std::max(0, branchLen - extraBranchLength_->sample(r) - 1);
                const int branchSteps = extraBranchSteps_->sample(r);
                placeBranch(run, r, treeHeight, attachments, origin.x, origin.z, currentHeight, branchDir, branchPos,
                            branchSteps);
            }
            if (heightPos == treeHeight - 1)
                attachments.emplace_back(BlockPos{origin.x, currentHeight + 1, origin.z}, 0, false);
        }
        return attachments;
    }
    bool validTreePos(const TreeRun& run, const BlockPos& p) const override {
        const u16 s = run.at(p);
        return run.b.validTreePos(s) || canGrowThrough_.contains(run.b, s);
    }

private:
    void placeBranch(TreeRun& run, RandomSource& r, int treeHeight, std::vector<FoliageAttachment>& attachments,
                     int logX, int logZ, int currentHeight, Direction dir, int branchPos, int branchSteps) const {
        int heightAlongBranch = currentHeight + branchPos;
        for (int index = branchPos; index < treeHeight && branchSteps > 0; ++index, --branchSteps) {
            if (index >= 1) {
                const int placementHeight = currentHeight + index;
                logX += kDirOffset[dir][0];
                logZ += kDirOffset[dir][2];
                heightAlongBranch = placementHeight;
                const BlockPos p{logX, placementHeight, logZ};
                if (placeLog(run, r, p)) heightAlongBranch = placementHeight + 1;
                attachments.emplace_back(p, 0, false);
            }
        }
        if (heightAlongBranch - currentHeight > 1) {
            const BlockPos foliagePos{logX, heightAlongBranch, logZ};
            attachments.emplace_back(foliagePos, 0, false);
            attachments.emplace_back(below(foliagePos, 2), 0, false);
        }
    }
    std::unique_ptr<IntProvider> extraBranchSteps_;
    float branchProbability_;
    std::unique_ptr<IntProvider> extraBranchLength_;
    BlockSet canGrowThrough_;
};

class CherryTrunkPlacer final : public TrunkPlacer {
public:
    CherryTrunkPlacer(const json& j, const FeatureEnv& env)
        : TrunkPlacer(j), branchCount_(env.intProvider(j.at("branch_count"))),
          branchHorizontalLength_(env.intProvider(j.at("branch_horizontal_length"))),
          branchEndOffsetFromTop_(env.intProvider(j.at("branch_end_offset_from_top"))) {
        const json& start = j.at("branch_start_offset_from_top");  // a UniformInt
        startMin_ = start.at("min_inclusive").get<int>();
        startMax_ = start.at("max_inclusive").get<int>();
    }
    std::vector<FoliageAttachment> placeTrunk(TreeRun& run, RandomSource& r, int treeHeight,
                                              const BlockPos& origin) const override {
        placeBelowTrunkBlock(run, r, below(origin));
        const int firstStart = r.nextInt(startMax_ - startMin_ + 1) + startMin_;
        const int first = std::max(0, treeHeight - 1 + firstStart);
        const int secondStart = r.nextInt(startMax_ - 1 - startMin_ + 1) + startMin_;  // UniformInt.of(min, max - 1)
        int second = std::max(0, treeHeight - 1 + secondStart);
        if (second >= first) ++second;
        const int branchCount = branchCount_->sample(r);
        const bool hasMiddle = branchCount == 3;
        const bool hasBoth = branchCount >= 2;
        const int trunkHeight = hasMiddle ? treeHeight : hasBoth ? std::max(first, second) + 1 : first + 1;
        for (int y = 0; y < trunkHeight; ++y) placeLog(run, r, above(origin, y));
        std::vector<FoliageAttachment> attachments;
        if (hasMiddle) attachments.emplace_back(above(origin, trunkHeight), 0, false);
        const Direction treeDirection = kHorizontal[r.nextInt(4)];
        const char sideways = axisOf(treeDirection);
        attachments.push_back(
            generateBranch(run, r, treeHeight, origin, sideways, treeDirection, first, first < trunkHeight - 1));
        if (hasBoth)
            attachments.push_back(generateBranch(run, r, treeHeight, origin, sideways, opposite(treeDirection), second,
                                                 second < trunkHeight - 1));
        return attachments;
    }

private:
    FoliageAttachment generateBranch(TreeRun& run, RandomSource& r, int treeHeight, const BlockPos& origin,
                                     char sideways, Direction branchDir, int offsetFromOrigin,
                                     bool middleContinuesUpwards) const {
        BlockPos logPos = above(origin, offsetFromOrigin);
        const int endOffset = treeHeight - 1 + branchEndOffsetFromTop_->sample(r);
        const bool extend = middleContinuesUpwards || endOffset < offsetFromOrigin;
        const int distanceToTrunk = branchHorizontalLength_->sample(r) + (extend ? 1 : 0);
        const BlockPos branchEnd = above(rel(origin, branchDir, distanceToTrunk), endOffset);
        const int stepsHorizontally = extend ? 2 : 1;
        for (int i = 0; i < stepsHorizontally; ++i) {
            logPos = rel(logPos, branchDir);
            placeLog(run, r, logPos, sideways);
        }
        const Direction vertical = branchEnd.y > logPos.y ? Up : Down;
        while (true) {
            const int distance = distManhattan(logPos, branchEnd);
            if (distance == 0) return FoliageAttachment(above(branchEnd), 0, false);
            const float chance = float(std::abs(branchEnd.y - logPos.y)) / float(distance);
            const bool growVertically = r.nextFloat() < chance;
            logPos = rel(logPos, growVertically ? vertical : branchDir);
            placeLog(run, r, logPos, growVertically ? 0 : sideways);
        }
    }
    std::unique_ptr<IntProvider> branchCount_, branchHorizontalLength_, branchEndOffsetFromTop_;
    int startMin_ = 0, startMax_ = 0;
};

class PoplarTrunkPlacer final : public TrunkPlacer {
public:
    PoplarTrunkPlacer(const json& j, const FeatureEnv& env)
        : TrunkPlacer(j), trunkHeightAboveBranches_(env.intProvider(j.at("trunk_height_above_branches"))),
          branchAmount_(env.intProvider(j.at("branch_amount"))) {}
    std::vector<FoliageAttachment> placeTrunk(TreeRun& run, RandomSource& r, int treeHeight,
                                              const BlockPos& origin) const override {
        placeBelowTrunkBlock(run, r, below(origin));
        const int upToBranches = treeHeight - trunkHeightAboveBranches_->sample(r);
        for (int y = 0; y < treeHeight; ++y) {
            placeLog(run, r, above(origin, y));
            // Direction.allShuffled, horizontal ones kept in shuffled order; drawn for every log.
            std::vector<Direction> all(std::begin(kAllDirections), std::end(kAllDirections));
            shuffle(all, r);
            std::vector<Direction> directions;
            for (Direction d : all)
                if (d != Up && d != Down) directions.push_back(d);
            if (upToBranches - 1 == y) {
                const int branches = branchAmount_->sample(r);
                for (int x = 0; x < branches; ++x)
                    placeLog(run, r, rel(above(origin, y), directions[size_t(x)]), axisOf(directions[size_t(x)]));
            }
        }
        return {FoliageAttachment(above(origin, upToBranches), 0, false)};
    }

private:
    std::unique_ptr<IntProvider> trunkHeightAboveBranches_, branchAmount_;
};

std::unique_ptr<TrunkPlacer> parseTrunkPlacer(const json& j, const FeatureEnv& env) {
    const std::string type = typeOf(j);
    if (type == "straight_trunk_placer") return std::make_unique<StraightTrunkPlacer>(j);
    if (type == "forking_trunk_placer") return std::make_unique<ForkingTrunkPlacer>(j);
    if (type == "giant_trunk_placer") return std::make_unique<GiantTrunkPlacer>(j);
    if (type == "mega_jungle_trunk_placer") return std::make_unique<MegaJungleTrunkPlacer>(j);
    if (type == "dark_oak_trunk_placer") return std::make_unique<DarkOakTrunkPlacer>(j);
    if (type == "fancy_trunk_placer") return std::make_unique<FancyTrunkPlacer>(j);
    if (type == "bending_trunk_placer") return std::make_unique<BendingTrunkPlacer>(j, env);
    if (type == "upwards_branching_trunk_placer") return std::make_unique<UpwardsBranchingTrunkPlacer>(j, env);
    if (type == "cherry_trunk_placer") return std::make_unique<CherryTrunkPlacer>(j, env);
    if (type == "poplar_trunk_placer") return std::make_unique<PoplarTrunkPlacer>(j, env);
    throw std::runtime_error("unknown trunk placer " + type);
}

// ---- foliage placers ----
class BlobFoliagePlacer : public FoliagePlacer {
public:
    BlobFoliagePlacer(const json& j, const FeatureEnv& env)
        : FoliagePlacer(j, env), height_(j.at("height").get<int>()) {}
    int foliageHeight(RandomSource&, int) const override { return height_; }

protected:
    void create(TreeRun& run, RandomSource& r, int, const FoliageAttachment& att, int foliageHeight, int leafRadius,
                int offset) const override {
        const int h = foliageHeight + att.foliageHeightOffset;
        for (int yo = offset; yo >= offset - h; --yo) {
            const int currentRadius = std::max(leafRadius + att.radiusOffset - 1 - yo / 2, 0);
            placeLeavesRow(run, r, att.pos, currentRadius, yo, att.doubleTrunk());
        }
    }
    bool shouldSkipLocation(RandomSource& r, int dx, int y, int dz, int currentRadius, bool) const override {
        return dx == currentRadius && dz == currentRadius && (r.nextInt(2) == 0 || y == 0);
    }
    int height_;
};

class BushFoliagePlacer final : public BlobFoliagePlacer {
public:
    using BlobFoliagePlacer::BlobFoliagePlacer;

protected:
    void create(TreeRun& run, RandomSource& r, int, const FoliageAttachment& att, int foliageHeight, int leafRadius,
                int offset) const override {
        const int h = foliageHeight + att.foliageHeightOffset;
        for (int yo = offset; yo >= offset - h; --yo)
            placeLeavesRow(run, r, att.pos, leafRadius + att.radiusOffset - 1 - yo, yo, att.doubleTrunk());
    }
    bool shouldSkipLocation(RandomSource& r, int dx, int, int dz, int currentRadius, bool) const override {
        return dx == currentRadius && dz == currentRadius && r.nextInt(2) == 0;
    }
};

class FancyFoliagePlacer final : public BlobFoliagePlacer {
public:
    using BlobFoliagePlacer::BlobFoliagePlacer;

protected:
    void create(TreeRun& run, RandomSource& r, int, const FoliageAttachment& att, int foliageHeight, int leafRadius,
                int offset) const override {
        for (int yo = offset; yo >= offset - foliageHeight; --yo) {
            const int currentRadius = leafRadius + (yo != offset && yo != offset - foliageHeight ? 1 : 0);
            placeLeavesRow(run, r, att.pos, currentRadius, yo, att.doubleTrunk());
        }
    }
    bool shouldSkipLocation(RandomSource&, int dx, int, int dz, int currentRadius, bool) const override {
        const float fx = float(dx) + 0.5f, fz = float(dz) + 0.5f;
        return fx * fx + fz * fz > float(currentRadius * currentRadius);
    }
};

class SpruceFoliagePlacer final : public FoliagePlacer {
public:
    SpruceFoliagePlacer(const json& j, const FeatureEnv& env)
        : FoliagePlacer(j, env), trunkHeight_(env.intProvider(j.at("trunk_height"))) {}
    int foliageHeight(RandomSource& r, int treeHeight) const override {
        return std::max(4, treeHeight - trunkHeight_->sample(r));
    }

protected:
    void create(TreeRun& run, RandomSource& r, int, const FoliageAttachment& att, int foliageHeight, int leafRadius,
                int offset) const override {
        int currentRadius = r.nextInt(2);
        int maxRadius = 1, minRadius = 0;
        const int h = foliageHeight + att.foliageHeightOffset;
        for (int yo = offset; yo >= -h; --yo) {
            placeLeavesRow(run, r, att.pos, currentRadius, yo, att.doubleTrunk());
            if (currentRadius >= maxRadius) {
                currentRadius = minRadius;
                minRadius = 1;
                maxRadius = std::min(maxRadius + 1, leafRadius + att.radiusOffset);
            } else {
                ++currentRadius;
            }
        }
    }
    bool shouldSkipLocation(RandomSource&, int dx, int, int dz, int currentRadius, bool) const override {
        return dx == currentRadius && dz == currentRadius && currentRadius > 0;
    }

private:
    std::unique_ptr<IntProvider> trunkHeight_;
};

class PineFoliagePlacer final : public FoliagePlacer {
public:
    PineFoliagePlacer(const json& j, const FeatureEnv& env)
        : FoliagePlacer(j, env), height_(env.intProvider(j.at("height"))) {}
    int foliageHeight(RandomSource& r, int) const override { return height_->sample(r); }
    int foliageRadius(RandomSource& r, int trunkHeight) const override {
        const int base = radius_->sample(r);
        return base + r.nextInt(std::max(trunkHeight + 1, 1));
    }

protected:
    void create(TreeRun& run, RandomSource& r, int, const FoliageAttachment& att, int foliageHeight, int leafRadius,
                int offset) const override {
        int currentRadius = 0;
        const int h = foliageHeight + att.foliageHeightOffset;
        for (int yo = offset; yo >= offset - h; --yo) {
            placeLeavesRow(run, r, att.pos, currentRadius, yo, att.doubleTrunk());
            if (currentRadius >= 1 && yo == offset - h + 1)
                --currentRadius;
            else if (currentRadius < leafRadius + att.radiusOffset)
                ++currentRadius;
        }
    }
    bool shouldSkipLocation(RandomSource&, int dx, int, int dz, int currentRadius, bool) const override {
        return dx == currentRadius && dz == currentRadius && currentRadius > 0;
    }

private:
    std::unique_ptr<IntProvider> height_;
};

class AcaciaFoliagePlacer final : public FoliagePlacer {
public:
    using FoliagePlacer::FoliagePlacer;
    int foliageHeight(RandomSource&, int) const override { return 0; }

protected:
    void create(TreeRun& run, RandomSource& r, int, const FoliageAttachment& att, int foliageHeight, int leafRadius,
                int offset) const override {
        const bool dbl = att.doubleTrunk();
        const BlockPos pos = above(att.pos, offset);
        const int h = foliageHeight + att.foliageHeightOffset;
        placeLeavesRow(run, r, pos, leafRadius + att.radiusOffset, -1 - h, dbl);
        placeLeavesRow(run, r, pos, leafRadius - 1, -h, dbl);
        placeLeavesRow(run, r, pos, leafRadius + att.radiusOffset - 1, 0, dbl);
    }
    bool shouldSkipLocation(RandomSource&, int dx, int y, int dz, int currentRadius, bool) const override {
        return y == 0 ? (dx > 1 || dz > 1) && dx != 0 && dz != 0
                      : dx == currentRadius && dz == currentRadius && currentRadius > 0;
    }
};

class DarkOakFoliagePlacer final : public FoliagePlacer {
public:
    using FoliagePlacer::FoliagePlacer;
    int foliageHeight(RandomSource&, int) const override { return 4; }

protected:
    void create(TreeRun& run, RandomSource& r, int, const FoliageAttachment& att, int, int leafRadius,
                int offset) const override {
        const BlockPos pos = above(att.pos, offset);
        const bool dbl = att.doubleTrunk();
        if (dbl) {
            placeLeavesRow(run, r, pos, leafRadius + 2, -1, dbl);
            placeLeavesRow(run, r, pos, leafRadius + 3, 0, dbl);
            placeLeavesRow(run, r, pos, leafRadius + 2, 1, dbl);
            if (r.nextBoolean()) placeLeavesRow(run, r, pos, leafRadius, 2, dbl);
        } else {
            placeLeavesRow(run, r, pos, leafRadius + 2, -1, dbl);
            placeLeavesRow(run, r, pos, leafRadius + 1, 0, dbl);
        }
    }
    bool shouldSkipLocationSigned(RandomSource& r, int dx, int y, int dz, int currentRadius, bool dbl) const override {
        if (y != 0 || !dbl || (dx != -currentRadius && dx < currentRadius) ||
            (dz != -currentRadius && dz < currentRadius))
            return FoliagePlacer::shouldSkipLocationSigned(r, dx, y, dz, currentRadius, dbl);
        return true;
    }
    bool shouldSkipLocation(RandomSource&, int dx, int y, int dz, int currentRadius, bool dbl) const override {
        if (y == -1 && !dbl) return dx == currentRadius && dz == currentRadius;
        return y == 1 ? dx + dz > currentRadius * 2 - 2 : false;
    }
};

class RandomSpreadFoliagePlacer final : public FoliagePlacer {
public:
    RandomSpreadFoliagePlacer(const json& j, const FeatureEnv& env)
        : FoliagePlacer(j, env), foliageHeight_(env.intProvider(j.at("foliage_height"))),
          attempts_(j.at("leaf_placement_attempts").get<int>()) {}
    int foliageHeight(RandomSource& r, int) const override { return foliageHeight_->sample(r); }

protected:
    void create(TreeRun& run, RandomSource& r, int, const FoliageAttachment& att, int foliageHeight, int leafRadius,
                int) const override {
        for (int i = 0; i < attempts_; ++i) {
            const int x1 = r.nextInt(leafRadius), x2 = r.nextInt(leafRadius);
            const int y1 = r.nextInt(foliageHeight), y2 = r.nextInt(foliageHeight);
            const int z1 = r.nextInt(leafRadius), z2 = r.nextInt(leafRadius);
            tryPlaceLeaf(run, r, att.pos.offset(x1 - x2, y1 - y2, z1 - z2));
        }
    }
    bool shouldSkipLocation(RandomSource&, int, int, int, int, bool) const override { return false; }

private:
    std::unique_ptr<IntProvider> foliageHeight_;
    int attempts_;
};

class MegaJungleFoliagePlacer final : public FoliagePlacer {
public:
    MegaJungleFoliagePlacer(const json& j, const FeatureEnv& env)
        : FoliagePlacer(j, env), height_(j.at("height").get<int>()) {}
    int foliageHeight(RandomSource&, int) const override { return height_; }

protected:
    void create(TreeRun& run, RandomSource& r, int, const FoliageAttachment& att, int foliageHeight, int leafRadius,
                int offset) const override {
        const int leafHeight = (att.doubleTrunk() ? foliageHeight : 1 + r.nextInt(2)) + att.foliageHeightOffset;
        for (int yo = offset; yo >= offset - leafHeight; --yo)
            placeLeavesRow(run, r, att.pos, leafRadius + att.radiusOffset + 1 - yo, yo, att.doubleTrunk());
    }
    bool shouldSkipLocation(RandomSource&, int dx, int, int dz, int currentRadius, bool) const override {
        return dx + dz >= 7 ? true : dx * dx + dz * dz > currentRadius * currentRadius;
    }

private:
    int height_;
};

class MegaPineFoliagePlacer final : public FoliagePlacer {
public:
    MegaPineFoliagePlacer(const json& j, const FeatureEnv& env)
        : FoliagePlacer(j, env), crownHeight_(env.intProvider(j.at("crown_height"))) {}
    int foliageHeight(RandomSource& r, int) const override { return crownHeight_->sample(r); }

protected:
    void create(TreeRun& run, RandomSource& r, int, const FoliageAttachment& att, int foliageHeight, int leafRadius,
                int offset) const override {
        const BlockPos& fp = att.pos;
        int prevRadius = 0;
        const int h = foliageHeight + att.foliageHeightOffset;
        for (int yy = fp.y - h + offset; yy <= fp.y + offset; ++yy) {
            const int yo = fp.y - yy;
            const int smooth = leafRadius + att.radiusOffset + mfloor(float(yo) / float(h) * 3.5f);
            const int jagged = yo > 0 && smooth == prevRadius && (yy & 1) == 0 ? smooth + 1 : smooth;
            placeLeavesRow(run, r, {fp.x, yy, fp.z}, jagged, 0, att.doubleTrunk());
            prevRadius = smooth;
        }
    }
    bool shouldSkipLocation(RandomSource&, int dx, int, int dz, int currentRadius, bool) const override {
        return dx + dz >= 7 ? true : dx * dx + dz * dz > currentRadius * currentRadius;
    }

private:
    std::unique_ptr<IntProvider> crownHeight_;
};

class CherryFoliagePlacer final : public FoliagePlacer {
public:
    CherryFoliagePlacer(const json& j, const FeatureEnv& env)
        : FoliagePlacer(j, env),
          height_(env.intProvider(j.at("height"))),
          wideHole_(jsonFloat(j.at("wide_bottom_layer_hole_chance"))),
          cornerHole_(jsonFloat(j.at("corner_hole_chance"))),
          hanging_(jsonFloat(j.at("hanging_leaves_chance"))),
          hangingExtension_(jsonFloat(j.at("hanging_leaves_extension_chance"))) {}
    int foliageHeight(RandomSource& r, int) const override { return height_->sample(r); }

protected:
    void create(TreeRun& run, RandomSource& r, int, const FoliageAttachment& att, int foliageHeight, int leafRadius,
                int offset) const override {
        const bool dbl = att.doubleTrunk();
        const BlockPos pos = above(att.pos, offset);
        const int currentRadius = leafRadius + att.radiusOffset - 1;
        const int h = foliageHeight + att.foliageHeightOffset;
        placeLeavesRow(run, r, pos, currentRadius - 2, h - 3, dbl);
        placeLeavesRow(run, r, pos, currentRadius - 1, h - 4, dbl);
        for (int y = h - 5; y >= 0; --y) placeLeavesRow(run, r, pos, currentRadius, y, dbl);
        placeLeavesRowWithHangingLeavesBelow(run, r, pos, currentRadius, -1, dbl, hanging_, hangingExtension_);
        placeLeavesRowWithHangingLeavesBelow(run, r, pos, currentRadius - 1, -2, dbl, hanging_, hangingExtension_);
    }
    bool shouldSkipLocation(RandomSource& r, int dx, int y, int dz, int currentRadius, bool) const override {
        if (y == -1 && (dx == currentRadius || dz == currentRadius) && r.nextFloat() < wideHole_) return true;
        const bool corner = dx == currentRadius && dz == currentRadius;
        if (currentRadius > 2) return corner || (dx + dz > currentRadius * 2 - 2 && r.nextFloat() < cornerHole_);
        return corner && r.nextFloat() < cornerHole_;
    }

private:
    std::unique_ptr<IntProvider> height_;
    float wideHole_, cornerHole_, hanging_, hangingExtension_;
};

class PoplarFoliagePlacer final : public FoliagePlacer {
public:
    PoplarFoliagePlacer(const json& j, const FeatureEnv& env)
        : FoliagePlacer(j, env),
          height_(env.intProvider(j.at("height"))),
          sideHole_(jsonFloat(j.at("side_hole_chance"))) {}
    int foliageHeight(RandomSource& r, int) const override { return height_->sample(r); }

protected:
    void create(TreeRun& run, RandomSource& r, int, const FoliageAttachment& att, int foliageHeight, int leafRadius,
                int offset) const override {
        const bool dbl = att.doubleTrunk();
        const BlockPos pos = above(att.pos, offset);
        const int rad = leafRadius + att.radiusOffset - 1;
        const bool flip = r.nextBoolean();
        const int h = foliageHeight + att.foliageHeightOffset;
        row(run, r, pos, rad - 2, h - 1, dbl, h, flip);
        row(run, r, pos, rad - 1, h - 2, dbl, h, flip);
        row(run, r, pos, rad - 1, h - 3, dbl, h, flip);
        for (int y = h - 4; y >= 1; --y) row(run, r, pos, rad, y, dbl, h, flip);
        replaceLeavesWithLog(run, r, pos, rad, h - 4, dbl, h, flip);
        row(run, r, pos, rad - 1, 0, dbl, h, flip);
        row(run, r, pos, std::clamp(rad - 2, 1, 2), -1, dbl, h, flip);
    }
    bool shouldSkipLocation(RandomSource&, int, int, int, int, bool) const override {
        throw std::logic_error("PoplarFoliagePlacer rows need more context");
    }

private:
    static bool partialRow(int foliageHeight, int y) { return foliageHeight - 1 == y || foliageHeight - 2 == y; }
    static int cornerCut(int dx, int dz, int radius, bool partial, bool flip) {
        const bool small = flip ? (dx > 0 && dz > 0) || (dz < 0 && dx < 0) : (dx > 0 && dz < 0) || (dz > 0 && dx < 0);
        return small ? radius - 1 : partial ? radius + 1 : radius;
    }
    static bool inRhombus(int radius, int adx, int adz, int cut, int extra) {
        return adx + adz <= radius * 2 - (cut + extra);
    }

    void row(TreeRun& run, RandomSource& r, const BlockPos& origin, int radius, int y, bool dbl, int h,
             bool flip) const {
        const int offset = dbl ? 1 : 0;
        for (int dx = -radius; dx <= radius + offset; ++dx)
            for (int dz = -radius; dz <= radius + offset; ++dz)
                if (!skip(r, dx, y, dz, radius, h, flip)) tryPlaceLeaf(run, r, origin.offset(dx, y, dz));
    }
    bool skip(RandomSource& r, int dx, int y, int dz, int radius, int h, bool flip) const {
        const bool partial = partialRow(h, y);
        const int cut = cornerCut(dx, dz, radius, partial, flip);
        const int adx = std::abs(dx), adz = std::abs(dz);
        if (partial && (adx == radius || adz == radius)) return true;
        const int extra = r.nextFloat() <= sideHole_ ? 1 : 0;
        return !inRhombus(radius, adx, adz, cut, extra);
    }
    void replaceLeavesWithLog(TreeRun& run, RandomSource& r, const BlockPos& origin, int radius, int y, bool dbl, int h,
                              bool flip) const {
        const int offset = dbl ? 1 : 0;
        for (int dx = -radius; dx <= radius + offset; ++dx)
            for (int dz = -radius; dz <= radius + offset; ++dz) {
                const int adz = std::abs(dz), adx = std::abs(dx);
                if (inRhombus(radius, adx, adz, cornerCut(dx, dz, radius, partialRow(h, y), flip), 2) &&
                    ((adz == 0 && radius - adx >= 4) || (adx == 0 && radius - adz >= 4))) {
                    const BlockPos p = origin.offset(dx, y, dz);
                    const u16 current = run.at(p);
                    if (current == run.tree.foliageProvider->state(run.level, r, p)) {
                        const u16 log = run.tree.trunkProvider->state(run.level, r, p);
                        run.setFoliage(p, run.b.with(log, "axis", adz == 0 ? "x" : "z"));
                    }
                }
            }
    }
    std::unique_ptr<IntProvider> height_;
    float sideHole_;
};

std::unique_ptr<FoliagePlacer> parseFoliagePlacer(const json& j, const FeatureEnv& env) {
    const std::string type = typeOf(j);
    if (type == "blob_foliage_placer") return std::make_unique<BlobFoliagePlacer>(j, env);
    if (type == "spruce_foliage_placer") return std::make_unique<SpruceFoliagePlacer>(j, env);
    if (type == "pine_foliage_placer") return std::make_unique<PineFoliagePlacer>(j, env);
    if (type == "acacia_foliage_placer") return std::make_unique<AcaciaFoliagePlacer>(j, env);
    if (type == "bush_foliage_placer") return std::make_unique<BushFoliagePlacer>(j, env);
    if (type == "fancy_foliage_placer") return std::make_unique<FancyFoliagePlacer>(j, env);
    if (type == "jungle_foliage_placer") return std::make_unique<MegaJungleFoliagePlacer>(j, env);
    if (type == "mega_pine_foliage_placer") return std::make_unique<MegaPineFoliagePlacer>(j, env);
    if (type == "dark_oak_foliage_placer") return std::make_unique<DarkOakFoliagePlacer>(j, env);
    if (type == "random_spread_foliage_placer") return std::make_unique<RandomSpreadFoliagePlacer>(j, env);
    if (type == "cherry_foliage_placer") return std::make_unique<CherryFoliagePlacer>(j, env);
    if (type == "poplar_foliage_placer") return std::make_unique<PoplarFoliagePlacer>(j, env);
    throw std::runtime_error("unknown foliage placer " + type);
}

// ---- root placers ----
class MangroveRootPlacer final : public RootPlacer {
public:
    MangroveRootPlacer(const json& j, const FeatureEnv& env) : RootPlacer(j, env) {
        const json& m = j.at("mangrove_root_placement");
        canGrowThrough_ = BlockSet::parse(m.at("can_grow_through"));
        muddyRootsIn_ = BlockSet::parse(m.at("muddy_roots_in"));
        muddyRootsProvider_ = env.stateProvider(m.at("muddy_roots_provider"));
        maxRootWidth_ = m.at("max_root_width").get<int>();
        maxRootLength_ = m.at("max_root_length").get<int>();
        randomSkewChance_ = jsonFloat(m.at("random_skew_chance"));
    }
    bool placeRoots(TreeRun& run, RandomSource& r, const BlockPos& origin, const BlockPos& trunkOrigin) const override {
        for (BlockPos column = origin; column.y < trunkOrigin.y; column = above(column))
            if (!canPlaceRoot(run, column)) return false;
        std::vector<BlockPos> rootPositions{below(trunkOrigin)};
        for (Direction dir : kHorizontal) {
            const BlockPos pos = rel(trunkOrigin, dir);
            std::vector<BlockPos> inDirection;
            if (!simulateRoots(run, r, pos, dir, trunkOrigin, inDirection, 0)) return false;
            rootPositions.insert(rootPositions.end(), inDirection.begin(), inDirection.end());
            rootPositions.push_back(rel(trunkOrigin, dir));
        }
        for (const BlockPos& p : rootPositions) placeRoot(run, r, p);
        return true;
    }

protected:
    bool canPlaceRoot(const TreeRun& run, const BlockPos& p) const override {
        const u16 s = run.at(p);
        return run.b.validTreePos(s) || canGrowThrough_.contains(run.b, s);
    }
    void placeRoot(TreeRun& run, RandomSource& r, const BlockPos& p) const override {
        if (muddyRootsIn_.contains(run.b, run.at(p))) {
            const u16 muddy = muddyRootsProvider_->state(run.level, r, p);
            run.setRoot(p, waterlogged(run, p, muddy));
        } else {
            RootPlacer::placeRoot(run, r, p);
        }
    }

private:
    bool simulateRoots(const TreeRun& run, RandomSource& r, const BlockPos& rootPos, Direction dir,
                       const BlockPos& rootOrigin, std::vector<BlockPos>& positions, int layer) const {
        if (layer == maxRootLength_ || int(positions.size()) > maxRootLength_) return false;
        for (const BlockPos& p : potentialRootPositions(rootPos, dir, r, rootOrigin)) {
            if (canPlaceRoot(run, p)) {
                positions.push_back(p);
                if (!simulateRoots(run, r, p, dir, rootOrigin, positions, layer + 1)) return false;
            }
        }
        return true;
    }
    std::vector<BlockPos> potentialRootPositions(const BlockPos& pos, Direction prevDir, RandomSource& r,
                                                 const BlockPos& rootOrigin) const {
        const BlockPos down = below(pos), nextTo = rel(pos, prevDir);
        const int width = distManhattan(pos, rootOrigin);
        if (width > maxRootWidth_ - 3 && width <= maxRootWidth_) {
            if (r.nextFloat() < randomSkewChance_) return {down, below(nextTo)};
            return {down};
        }
        if (width > maxRootWidth_) return {down};
        if (r.nextFloat() < randomSkewChance_) return {down};
        return r.nextBoolean() ? std::vector<BlockPos>{nextTo} : std::vector<BlockPos>{down};
    }

    BlockSet canGrowThrough_, muddyRootsIn_;
    std::unique_ptr<StateProvider> muddyRootsProvider_;
    int maxRootWidth_ = 0, maxRootLength_ = 0;
    float randomSkewChance_ = 0;
};

std::unique_ptr<RootPlacer> parseRootPlacer(const json& j, const FeatureEnv& env) {
    const std::string type = typeOf(j);
    if (type == "mangrove_root_placer") return std::make_unique<MangroveRootPlacer>(j, env);
    throw std::runtime_error("unknown root placer " + type);
}

std::unique_ptr<FeatureSize> parseFeatureSize(const json& j) {
    const std::string type = typeOf(j);
    if (type == "two_layers_feature_size") return std::make_unique<TwoLayersFeatureSize>(j);
    if (type == "three_layers_feature_size") return std::make_unique<ThreeLayersFeatureSize>(j);
    throw std::runtime_error("unknown feature size " + type);
}

TreeFeature::TreeFeature(const json& j, const FeatureEnv& env, std::shared_ptr<const Blocks> blocks)
    : blocks_(std::move(blocks)) {
    trunkProvider = env.stateProvider(j.at("trunk_provider"));
    trunkPlacer_ = parseTrunkPlacer(j.at("trunk_placer"), env);
    foliageProvider = env.stateProvider(j.at("foliage_provider"));
    foliagePlacer_ = parseFoliagePlacer(j.at("foliage_placer"), env);
    if (auto it = j.find("root_placer"); it != j.end()) rootPlacer_ = parseRootPlacer(*it, env);
    minimumSize_ = parseFeatureSize(j.at("minimum_size"));
    decorators_ = parseDecorators(j.at("decorators"), env);
    ignoreVines_ = j.value("ignore_vines", false);
    belowTrunkProvider = env.stateProvider(j.at("below_trunk_provider"));
}

int TreeFeature::maxFreeTreeHeight(const TreeRun& run, int maxTreeHeight, const BlockPos& treePos) const {
    for (int y = 0; y <= maxTreeHeight + 1; ++y) {
        const int r = minimumSize_->sizeAtHeight(maxTreeHeight, y);
        for (int x = -r; x <= r; ++x)
            for (int z = -r; z <= r; ++z) {
                const BlockPos p = treePos.offset(x, y, z);
                if (!trunkPlacer_->isFree(run, p) || (!ignoreVines_ && run.b.isVine(run.at(p)))) return y - 2;
            }
    }
    return maxTreeHeight;
}

bool TreeFeature::doPlace(TreeRun& run, RandomSource& r, const BlockPos& origin) const {
    const int treeHeight = trunkPlacer_->treeHeight(r);
    const int foliageHeight = foliagePlacer_->foliageHeight(r, treeHeight);
    const int trunkHeight = treeHeight - foliageHeight;
    const int leafRadius = foliagePlacer_->foliageRadius(r, trunkHeight);
    const BlockPos trunkOrigin = rootPlacer_ ? rootPlacer_->trunkOrigin(origin, r) : origin;
    const int minY = std::min(origin.y, trunkOrigin.y);
    const int maxY = std::max(origin.y, trunkOrigin.y) + treeHeight + 1;
    if (minY < run.level.minY() + 1 || maxY > run.level.maxY()) return false;  // getMaxY() is inclusive
    const int clipped = maxFreeTreeHeight(run, treeHeight, trunkOrigin);
    const int minClipped = minimumSize_->minClippedHeight;
    if (clipped < treeHeight && (minClipped < 0 || clipped < minClipped)) return false;
    if (rootPlacer_ && !rootPlacer_->placeRoots(run, r, origin, trunkOrigin)) return false;
    const std::vector<FoliageAttachment> attachments = trunkPlacer_->placeTrunk(run, r, clipped, trunkOrigin);
    for (const FoliageAttachment& att : attachments)
        foliagePlacer_->createFoliage(run, r, clipped, att, foliageHeight, leafRadius);
    return true;
}

// TreeFeature.updateLeaves: leaf distances by increasing distance from the logs, in HashSet order.
VoxelShape TreeFeature::updateLeaves(GenRegion& level, int x0, int y0, int z0, int x1, int y1, int z1,
                                     const TreeRun& run) const {
    VoxelShape shape(x1 - x0 + 1, y1 - y0 + 1, z1 - z0 + 1);
    auto inside = [&](const BlockPos& p) {
        return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1 && p.z >= z0 && p.z <= z1;
    };
    run.decorations.forEach([&](const BlockPos& p) { shape.fill(p.x - x0, p.y - y0, p.z - z0); });
    run.roots.forEach([&](const BlockPos& p) { shape.fill(p.x - x0, p.y - y0, p.z - z0); });
    JavaPosSet toCheck[7];
    run.trunks.forEach([&](const BlockPos& p) { toCheck[0].add(p); });
    int smallest = 0;
    while (true) {
        while (smallest >= 7 || !toCheck[smallest].empty()) {
            if (smallest >= 7) return shape;
            const BlockPos pos = toCheck[smallest].removeFirst();
            if (!inside(pos)) continue;
            if (smallest != 0) {
                const u16 s = level.block(pos);
                if (b().has(s, "distance"))
                    level.setBlock(pos, b().reg.withProperty(s, "distance", std::to_string(smallest)));
            }
            shape.fill(pos.x - x0, pos.y - y0, pos.z - z0);
            for (Direction d : kAllDirections) {
                const BlockPos np = rel(pos, d);
                if (!inside(np) || shape.full(np.x - x0, np.y - y0, np.z - z0)) continue;
                const int distance = b().optionalDistance(level.block(np));
                if (distance < 0) continue;
                const int nd = std::min(distance, smallest + 1);
                if (nd < 7) {
                    toCheck[nd].add(np);
                    smallest = std::min(smallest, nd);
                }
            }
        }
        ++smallest;
    }
}

bool TreeFeature::place(GenRegion& level, RandomSource& random, const BlockPos& origin) const {
    TreeRun run{level, *this, *blocks_};
    const bool result = doPlace(run, random, origin);
    if (!result || (run.trunks.empty() && run.foliage.empty())) return false;
    if (!decorators_.empty()) {
        DecoratorContext ctx{level, &run.decorations, random, *blocks_, sortedByY(run.trunks), sortedByY(run.foliage),
                             sortedByY(run.roots)};
        for (const auto& d : decorators_) d->place(ctx);
    }
    int x0 = INT32_MAX, y0 = INT32_MAX, z0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN, z1 = INT32_MIN;
    auto grow = [&](const BlockPos& p) {
        x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), z0 = std::min(z0, p.z);
        x1 = std::max(x1, p.x), y1 = std::max(y1, p.y), z1 = std::max(z1, p.z);
    };
    run.roots.forEach(grow);
    run.trunks.forEach(grow);
    run.foliage.forEach(grow);
    run.decorations.forEach(grow);
    const VoxelShape shape = updateLeaves(level, x0, y0, z0, x1, y1, z1, run);
    updateShapeAtEdge(level, *blocks_, shape, {x0, y0, z0});
    return true;
}

// ---- huge mushrooms ------------------------------------------------------------------------
class HugeMushroomFeature : public ConfiguredFeature {
public:
    HugeMushroomFeature(const json& j, const FeatureEnv& env, std::shared_ptr<const Blocks> blocks)
        : blocks_(std::move(blocks)), capProvider_(env.stateProvider(j.at("cap_provider"))),
          stemProvider_(env.stateProvider(j.at("stem_provider"))), foliageRadius_(jsonInt(j, "foliage_radius", 2)),
          canPlaceOn_(env.blockPredicate(j.at("can_place_on"))) {}

    bool place(GenRegion& level, RandomSource& random, const BlockPos& origin) const override {
        int treeHeight = random.nextInt(3) + 4;
        if (random.nextInt(12) == 0) treeHeight *= 2;
        if (!isValidPosition(level, origin, treeHeight)) return false;
        makeCap(level, random, origin, treeHeight);
        for (int dy = 0; dy < treeHeight; ++dy)
            placeMushroomBlock(level, above(origin, dy), stemProvider_->state(level, random, origin));
        return true;
    }

protected:
    virtual int treeRadiusForHeight(int trunkHeight, int treeHeight, int leafRadius, int yo) const = 0;
    virtual void makeCap(GenRegion& level, RandomSource& random, const BlockPos& origin, int treeHeight) const = 0;

    void placeMushroomBlock(GenRegion& level, const BlockPos& p, u16 state) const {
        const u16 current = level.block(p);
        if (blocks_->isAir(current) || blocks_->mushroomReplaceable(current)) level.setBlock(p, state);
    }
    bool isValidPosition(GenRegion& level, const BlockPos& origin, int treeHeight) const {
        const int y = origin.y;
        if (y < level.minY() + 1 || y + treeHeight + 1 > level.maxY() - 1) return false;
        if (!canPlaceOn_->test(level, below(origin))) return false;
        for (int dy = 0; dy <= treeHeight; ++dy) {
            const int radius = treeRadiusForHeight(-1, -1, foliageRadius_, dy);
            for (int dx = -radius; dx <= radius; ++dx)
                for (int dz = -radius; dz <= radius; ++dz) {
                    const u16 s = level.block(origin.offset(dx, dy, dz));
                    if (!blocks_->isAir(s) && !blocks_->isLeaves(s)) return false;
                }
        }
        return true;
    }

    std::shared_ptr<const Blocks> blocks_;
    std::unique_ptr<StateProvider> capProvider_, stemProvider_;
    int foliageRadius_;
    std::unique_ptr<BlockPredicate> canPlaceOn_;
};

class HugeRedMushroomFeature final : public HugeMushroomFeature {
public:
    using HugeMushroomFeature::HugeMushroomFeature;

protected:
    int treeRadiusForHeight(int, int treeHeight, int leafRadius, int yo) const override {
        if (yo < treeHeight && yo >= treeHeight - 3) return leafRadius;
        return yo == treeHeight ? leafRadius : 0;
    }
    void makeCap(GenRegion& level, RandomSource& random, const BlockPos& origin, int treeHeight) const override {
        const Blocks& b = *blocks_;
        for (int dy = treeHeight - 3; dy <= treeHeight; ++dy) {
            const int radius = dy < treeHeight ? foliageRadius_ : foliageRadius_ - 1;
            const int center = foliageRadius_ - 2;
            for (int dx = -radius; dx <= radius; ++dx)
                for (int dz = -radius; dz <= radius; ++dz) {
                    const bool xEdge = dx == -radius || dx == radius;
                    const bool zEdge = dz == -radius || dz == radius;
                    if (dy >= treeHeight || xEdge != zEdge) {
                        u16 s = capProvider_->state(level, random, origin);
                        if (b.has(s, "west") && b.has(s, "east") && b.has(s, "north") && b.has(s, "south") &&
                            b.has(s, "up")) {
                            s = b.withFlag(s, "up", dy >= treeHeight - 1);
                            s = b.withFlag(s, "west", dx < -center);
                            s = b.withFlag(s, "east", dx > center);
                            s = b.withFlag(s, "north", dz < -center);
                            s = b.withFlag(s, "south", dz > center);
                        }
                        placeMushroomBlock(level, origin.offset(dx, dy, dz), s);
                    }
                }
        }
    }
};

class HugeBrownMushroomFeature final : public HugeMushroomFeature {
public:
    using HugeMushroomFeature::HugeMushroomFeature;

protected:
    int treeRadiusForHeight(int, int, int leafRadius, int yo) const override { return yo <= 3 ? 0 : leafRadius; }
    void makeCap(GenRegion& level, RandomSource& random, const BlockPos& origin, int treeHeight) const override {
        const Blocks& b = *blocks_;
        const int fr = foliageRadius_;
        for (int dx = -fr; dx <= fr; ++dx)
            for (int dz = -fr; dz <= fr; ++dz) {
                const bool minX = dx == -fr, maxX = dx == fr, minZ = dz == -fr, maxZ = dz == fr;
                const bool xEdge = minX || maxX, zEdge = minZ || maxZ;
                if (xEdge && zEdge) continue;
                const bool west = minX || (zEdge && dx == 1 - fr);
                const bool east = maxX || (zEdge && dx == fr - 1);
                const bool north = minZ || (xEdge && dz == 1 - fr);
                const bool south = maxZ || (xEdge && dz == fr - 1);
                u16 s = capProvider_->state(level, random, origin);
                if (b.has(s, "west") && b.has(s, "east") && b.has(s, "north") && b.has(s, "south")) {
                    s = b.withFlag(s, "west", west);
                    s = b.withFlag(s, "east", east);
                    s = b.withFlag(s, "north", north);
                    s = b.withFlag(s, "south", south);
                }
                placeMushroomBlock(level, origin.offset(dx, treeHeight, dz), s);
            }
    }
};

// ---- huge fungus ---------------------------------------------------------------------------
class HugeFungusFeature final : public ConfiguredFeature {
public:
    HugeFungusFeature(const json& j, const FeatureEnv& env, std::shared_ptr<const Blocks> blocks)
        : blocks_(std::move(blocks)),
          validBase_(env.state(j.at("valid_base_block"))),
          stem_(env.state(j.at("stem_state"))),
          hat_(env.state(j.at("hat_state"))),
          decor_(env.state(j.at("decor_state"))),
          replaceable_(env.blockPredicate(j.at("replaceable_blocks"))),
          planted_(j.value("planted", false)) {}

    bool place(GenRegion& level, RandomSource& random, const BlockPos& origin) const override {
        const Blocks& b = *blocks_;
        if (!b.sameBlock(level.block(below(origin)), validBase_)) return false;
        int totalHeight = random.nextInt(10) + 4;  // Mth.nextInt(random, 4, 13)
        if (random.nextInt(12) == 0) totalHeight *= 2;
        if (!planted_ && origin.y + totalHeight + 1 >= level.maxY() - level.minY()) return false;  // getGenDepth()
        const bool isHuge = !planted_ && random.nextFloat() < 0.06f;
        level.setBlock(origin, b.air);
        placeStem(level, random, origin, totalHeight, isHuge);
        placeHat(level, random, origin, totalHeight, isHuge);
        return true;
    }

private:
    bool isReplaceable(const GenRegion& level, const BlockPos& p, bool checkPlants) const {
        if (blocks_->canBeReplaced(level.block(p))) return true;
        return checkPlants && replaceable_->test(level, p);
    }
    void destroyIfPlanted(GenRegion& level, const BlockPos& p) const {  // destroyBlock without drops
        if (planted_ && !blocks_->isAir(level.block(below(p))) && !blocks_->isAir(level.block(p)))
            level.setBlock(p, blocks_->air);
    }
    void placeStem(GenRegion& level, RandomSource& random, const BlockPos& origin, int totalHeight, bool isHuge) const {
        const int stemRadius = isHuge ? 1 : 0;
        for (int dx = -stemRadius; dx <= stemRadius; ++dx)
            for (int dz = -stemRadius; dz <= stemRadius; ++dz) {
                const bool corner = isHuge && std::abs(dx) == stemRadius && std::abs(dz) == stemRadius;
                for (int dy = 0; dy < totalHeight; ++dy) {
                    const BlockPos p = origin.offset(dx, dy, dz);
                    if (!isReplaceable(level, p, true)) continue;
                    if (planted_) {
                        destroyIfPlanted(level, p);
                        level.setBlock(p, stem_);
                    } else if (corner) {
                        if (random.nextFloat() < 0.1f) level.setBlock(p, stem_);
                    } else {
                        level.setBlock(p, stem_);
                    }
                }
            }
    }
    void placeHat(GenRegion& level, RandomSource& random, const BlockPos& origin, int totalHeight, bool isHuge) const {
        const bool placeVines = blocks_->is(hat_, "minecraft:nether_wart_block");
        const int hatHeight = std::min(random.nextInt(1 + totalHeight / 3) + 5, totalHeight);
        const int hatStartY = totalHeight - hatHeight;
        for (int dy = hatStartY; dy <= totalHeight; ++dy) {
            int radius = dy < totalHeight - random.nextInt(3) ? 2 : 1;
            if (hatHeight > 8 && dy < hatStartY + 4) radius = 3;
            if (isHuge) ++radius;
            for (int dx = -radius; dx <= radius; ++dx)
                for (int dz = -radius; dz <= radius; ++dz) {
                    const bool edgeX = dx == -radius || dx == radius;
                    const bool edgeZ = dz == -radius || dz == radius;
                    const bool inside = !edgeX && !edgeZ && dy != totalHeight;
                    const bool corner = edgeX && edgeZ;
                    const bool hatBottom = dy < hatStartY + 3;
                    const BlockPos p = origin.offset(dx, dy, dz);
                    if (!isReplaceable(level, p, false)) continue;
                    destroyIfPlanted(level, p);
                    if (hatBottom) {
                        if (!inside) placeHatDropBlock(level, random, p, placeVines);
                    } else if (inside) {
                        placeHatBlock(level, random, p, 0.1f, 0.2f, placeVines ? 0.1f : 0.0f);
                    } else if (corner) {
                        placeHatBlock(level, random, p, 0.01f, 0.7f, placeVines ? 0.083f : 0.0f);
                    } else {
                        placeHatBlock(level, random, p, 5.0E-4f, 0.98f, placeVines ? 0.07f : 0.0f);
                    }
                }
        }
    }
    void placeHatBlock(GenRegion& level, RandomSource& random, const BlockPos& p, float decorChance, float hatChance,
                       float vinesChance) const {
        if (random.nextFloat() < decorChance) {
            level.setBlock(p, decor_);
        } else if (random.nextFloat() < hatChance) {
            level.setBlock(p, hat_);
            if (random.nextFloat() < vinesChance) tryPlaceWeepingVines(level, random, p);
        }
    }
    void placeHatDropBlock(GenRegion& level, RandomSource& random, const BlockPos& p, bool placeVines) const {
        if (blocks_->sameBlock(level.block(below(p)), hat_)) {
            level.setBlock(p, hat_);
        } else if (double(random.nextFloat()) < 0.15) {
            level.setBlock(p, hat_);
            if (placeVines && random.nextInt(11) == 0) tryPlaceWeepingVines(level, random, p);
        }
    }
    void tryPlaceWeepingVines(GenRegion& level, RandomSource& random, const BlockPos& hatPos) const {
        BlockPos p = below(hatPos);
        if (!level.isEmpty(p)) return;
        int goal = random.nextInt(5) + 1;  // Mth.nextInt(random, 1, 5)
        if (random.nextInt(7) == 0) goal *= 2;
        for (int height = 0; height <= goal; ++height) {
            if (level.isEmpty(p)) {
                if (height == goal || !level.isEmpty(below(p))) {
                    const int age = random.nextInt(3) + 23;  // Mth.nextInt(random, 23, 25)
                    level.setBlock(p, blocks_->withInt(blocks_->weepingVines, "age", age));
                    break;
                }
                level.setBlock(p, blocks_->weepingVinesPlant);
            }
            p = below(p);
        }
    }

    std::shared_ptr<const Blocks> blocks_;
    u16 validBase_, stem_, hat_, decor_;
    std::unique_ptr<BlockPredicate> replaceable_;
    bool planted_;
};

// ---- bamboo ----------------------------------------------------------------------------------
class BambooFeature final : public ConfiguredFeature {
public:
    BambooFeature(const json& j, const FeatureEnv&, std::shared_ptr<const Blocks> blocks)
        : blocks_(std::move(blocks)), probability_(jsonFloat(j.at("probability"))) {}

    bool place(GenRegion& level, RandomSource& random, const BlockPos& origin) const override {
        const Blocks& b = *blocks_;
        if (!level.isEmpty(origin)) return false;
        if (b.tag(level.block(below(origin)), "minecraft:supports_bamboo")) {
            const int height = random.nextInt(12) + 5;
            if (random.nextFloat() < probability_) {
                const int r = random.nextInt(4) + 1;
                for (int xx = origin.x - r; xx <= origin.x + r; ++xx)
                    for (int zz = origin.z - r; zz <= origin.z + r; ++zz) {
                        const int xd = xx - origin.x, zd = zz - origin.z;
                        if (xd * xd + zd * zd > r * r) continue;
                        const BlockPos podzolPos{xx, level.height(Heightmap::WorldSurface, xx, zz) - 1, zz};
                        if (b.tag(level.block(podzolPos), "minecraft:beneath_bamboo_podzol_replaceable"))
                            level.setBlock(podzolPos, b.podzol);
                    }
            }
            BlockPos p = origin;
            for (int i = 0; i < height && level.isEmpty(p); ++i) {
                level.setBlock(p, b.bambooTrunk);
                p = above(p);
            }
            if (p.y - origin.y >= 3) {
                level.setBlock(p, b.bambooFinalLarge);
                level.setBlock(below(p), b.bambooTopLarge);
                level.setBlock(below(p, 2), b.bambooTopSmall);
            }
        }
        return true;
    }

private:
    std::shared_ptr<const Blocks> blocks_;
    float probability_;
};

// ---- fallen tree -----------------------------------------------------------------------------
class FallenTreeFeature final : public ConfiguredFeature {
public:
    FallenTreeFeature(const json& j, const FeatureEnv& env, std::shared_ptr<const Blocks> blocks)
        : blocks_(std::move(blocks)),
          trunkProvider_(env.stateProvider(j.at("trunk_provider"))),
          logLength_(env.intProvider(j.at("log_length"))),
          stumpDecorators_(parseDecorators(j.at("stump_decorators"), env)),
          logDecorators_(parseDecorators(j.at("log_decorators"), env)) {}

    bool place(GenRegion& level, RandomSource& random, const BlockPos& origin) const override {
        const BlockPos stump = placeLogBlock(level, random, origin, 0);
        JavaPosSet stumpSet;
        stumpSet.add(stump);
        decorateLogs(level, random, stumpSet, stumpDecorators_);
        const Direction direction = kHorizontal[random.nextInt(4)];
        const int logLength = logLength_->sample(random) - 2;
        const int gap = random.nextInt(2);
        BlockPos start = rel(origin, direction, 2 + gap);
        start = above(start);  // setGroundHeightForFallenLogStartPos
        for (int i = 0; i < 6; ++i) {
            if (mayPlaceOn(level, start)) break;
            start = below(start);
        }
        if (canPlaceEntireFallenLog(level, logLength, start, direction)) {
            JavaPosSet fallenLog;
            BlockPos p = start;
            for (int i = 0; i < logLength; ++i) {
                fallenLog.add(placeLogBlock(level, random, p, axisOf(direction)));
                p = rel(p, direction);
            }
            decorateLogs(level, random, fallenLog, logDecorators_);
        }
        return true;
    }

private:
    bool overSolidGround(const GenRegion& level, const BlockPos& p) const {
        return blocks_->sturdyFace(level.block(below(p)), Up);
    }
    bool mayPlaceOn(const GenRegion& level, const BlockPos& p) const {
        return blocks_->validTreePos(level.block(p)) && overSolidGround(level, p);
    }
    bool canPlaceEntireFallenLog(const GenRegion& level, int logLength, BlockPos p, Direction direction) const {
        int gapInGround = 0;
        for (int i = 0; i < logLength; ++i) {
            if (!blocks_->validTreePos(level.block(p))) return false;
            if (!overSolidGround(level, p)) {
                if (++gapInGround > 2) return false;
            } else {
                gapInGround = 0;
            }
            p = rel(p, direction);
        }
        return true;
    }
    BlockPos placeLogBlock(GenRegion& level, RandomSource& random, const BlockPos& p, char axis) const {
        u16 s = trunkProvider_->state(level, random, p);
        if (axis) s = blocks_->with(s, "axis", std::string_view(&axis, 1));
        level.setBlock(p, s);
        return p;
    }
    void decorateLogs(GenRegion& level, RandomSource& random, const JavaPosSet& logs,
                      const std::vector<std::unique_ptr<TreeDecorator>>& decorators) const {
        if (decorators.empty()) return;
        DecoratorContext ctx{level, nullptr, random, *blocks_, sortedByY(logs), {}, {}};
        for (const auto& d : decorators) d->place(ctx);
    }

    std::shared_ptr<const Blocks> blocks_;
    std::unique_ptr<StateProvider> trunkProvider_;
    std::unique_ptr<IntProvider> logLength_;
    std::vector<std::unique_ptr<TreeDecorator>> stumpDecorators_, logDecorators_;
};

// The block facts are shared by every feature built from one FeatureTypes for the same registry.
struct SharedBlocks {
    std::mutex mutex;
    const BlockRegistry* registry = nullptr;
    const BlockFacts* facts = nullptr;
    std::shared_ptr<const Blocks> blocks;

    std::shared_ptr<const Blocks> get(const FeatureEnv& env) {
        std::lock_guard lock(mutex);
        if (!blocks || registry != &env.blocks() || facts != &env.facts()) {
            blocks = std::make_shared<const Blocks>(env);
            registry = &env.blocks();
            facts = &env.facts();
        }
        return blocks;
    }
};

template <class T>
void addFeature(FeatureTypes& types, const char* type, const std::shared_ptr<SharedBlocks>& shared) {
    types.add(type, [shared](const json& j, const FeatureEnv& env) -> std::unique_ptr<ConfiguredFeature> {
        return std::make_unique<T>(configOf(j), env, shared->get(env));
    });
}

}  // namespace

void registerTreeFeatures(FeatureTypes& types) {
    auto shared = std::make_shared<SharedBlocks>();
    addFeature<TreeFeature>(types, "minecraft:tree", shared);
    addFeature<HugeRedMushroomFeature>(types, "minecraft:huge_red_mushroom", shared);
    addFeature<HugeBrownMushroomFeature>(types, "minecraft:huge_brown_mushroom", shared);
    addFeature<HugeFungusFeature>(types, "minecraft:huge_fungus", shared);
    addFeature<BambooFeature>(types, "minecraft:bamboo", shared);
    addFeature<FallenTreeFeature>(types, "minecraft:fallen_tree", shared);
}

}  // namespace wg
