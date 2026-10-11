// Structure building blocks shared by every structure type: block state transforms (BlockState.rotate /
// mirror), StructurePiece's placement helpers, StructurePiecesBuilder, Structure.GenerationContext and the
// cached base terrain heights structures are positioned with.
#include "worldgen/structure_core.h"

#include "assets/assets.h"
#include "worldgen/jigsaw.h"
#include "worldgen/structure_template.h"

#include <algorithm>
#include <mutex>

namespace wg::st {

std::optional<Direction> dirFromName(std::string_view n) {
    for (int i = 0; i < 6; ++i)
        if (n == kDirNames[i]) return Direction(i);
    return std::nullopt;
}

std::vector<Rot> shuffledRotations(RandomSource& r) {
    std::vector<Rot> v{Rot::None, Rot::CW90, Rot::CW180, Rot::CCW90};
    for (size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[size_t(r.nextInt(int(i)))]);
    return v;
}

Rot rotFromName(std::string_view n) {
    if (n == "CLOCKWISE_90" || n == "clockwise_90") return Rot::CW90;
    if (n == "CLOCKWISE_180" || n == "180") return Rot::CW180;
    if (n == "COUNTERCLOCKWISE_90" || n == "counterclockwise_90") return Rot::CCW90;
    return Rot::None;
}

const char* rotName(Rot r) {
    constexpr const char* kNames[4] = {"NONE", "CLOCKWISE_90", "CLOCKWISE_180", "COUNTERCLOCKWISE_90"};
    return kNames[int(r)];
}

HType htypeFromName(std::string_view n) {
    if (n == "OCEAN_FLOOR_WG") return HType::OceanFloorWG;
    if (n == "WORLD_SURFACE") return HType::WorldSurface;
    if (n == "MOTION_BLOCKING") return HType::MotionBlocking;
    if (n == "MOTION_BLOCKING_NO_LEAVES") return HType::MotionBlockingNoLeaves;
    if (n == "OCEAN_FLOOR") return HType::OceanFloor;
    return HType::WorldSurfaceWG;
}

bool isRandomizableContainer(std::string_view n) {
    return n == "minecraft:chest" || n == "minecraft:trapped_chest" || n.ends_with("copper_chest") || n == "minecraft:barrel" ||
           n == "minecraft:dispenser" || n == "minecraft:dropper" || n == "minecraft:hopper" || n.ends_with("shulker_box") ||
           n == "minecraft:decorated_pot" || n == "minecraft:crafter";
}

// ---- SB -------------------------------------------------------------------------------------------------

SB::SB(BlockRegistry& registry, const BlockFacts& blockFacts)
    : reg(registry), facts(blockFacts), f(blockFacts.impl()), transforms_(std::make_unique<std::atomic<u32>[]>(size_t(0x10000) * 5)) {
    for (size_t i = 0; i < size_t(0x10000) * 5; ++i) transforms_[i].store(0, std::memory_order_relaxed);
    air = def("minecraft:air");
    caveAir = def("minecraft:cave_air");
    water = reg.idFor("minecraft:water[level=0]");
    lava = reg.idFor("minecraft:lava[level=0]");
    barrier = def("minecraft:barrier");
    jigsaw = def("minecraft:jigsaw");
    structureBlock = def("minecraft:structure_block");
    structureVoid = def("minecraft:structure_void");
}

u16 SB::def(std::string_view block) const {
    {
        std::shared_lock lock(mutex_);
        if (auto it = defs_.find(std::string(block)); it != defs_.end()) return it->second;
    }
    std::string k(block);
    if (k.find(':') == std::string::npos) k = "minecraft:" + k;
    if (k == "minecraft:water" || k == "minecraft:lava") k += "[level=0]";
    const u16 id = reg.idFor(k);
    std::unique_lock lock(mutex_);
    defs_.emplace(std::string(block), id);
    return id;
}

u16 SB::key(std::string_view stateKey) const {
    std::string k(stateKey);
    if (k.find(':') == std::string::npos || k.find(':') > k.find('[')) k = "minecraft:" + k;
    // Sort the properties (BlockRegistry::idFor wants them sorted by key).
    const size_t open = k.find('[');
    if (open != std::string::npos) {
        std::vector<std::string> parts;
        std::string body = k.substr(open + 1, k.size() - open - 2), cur;
        for (char ch : body) {
            if (ch == ',') parts.push_back(cur), cur.clear();
            else cur += ch;
        }
        if (!cur.empty()) parts.push_back(cur);
        std::sort(parts.begin(), parts.end());
        k = k.substr(0, open) + "[";
        for (size_t i = 0; i < parts.size(); ++i) k += (i ? "," : "") + parts[i];
        k += "]";
    } else if (k == "minecraft:water" || k == "minecraft:lava") {
        k += "[level=0]";
    }
    return reg.idFor(k);
}

u16 SB::state(const json& j) const { return reg.idFor(stateKeyOf(j)); }

u16 SB::with(u16 s, std::string_view k, std::string_view v) const { return reg.withProperty(s, k, v); }
std::string_view SB::prop(u16 s, std::string_view k) const { return reg.property(s, k); }
const std::string& SB::name(u16 s) const { return reg.name(s); }
std::string_view SB::blockName(u16 s) const { return baseName(reg.name(s)); }

namespace {
// The boxes (disjoint, like VoxelShape.toAabbs) fill the unit cube.
bool fillsCube(const BlockRegistry& reg, u32 first, u32 count) {
    double volume = 0;
    for (u32 i = 0; i < count; ++i) {
        const Box& b = reg.box(first + i);
        double v = 1;
        for (int a = 0; a < 3; ++a) v *= std::max(0.0, double(std::min(b.max[a], 1.0f)) - double(std::max(b.min[a], 0.0f)));
        volume += v;
    }
    return volume >= 0.9999;
}
}  // namespace

bool SB::fullCollision(u16 s) const {
    // Block.isShapeFullBlock(getCollisionShape) from the game's exact shapes (mud, soul sand... are not full).
    const BlockInfo& bi = reg.infos()[s];
    return bi.collisionCount > 0 && fillsCube(reg, bi.collisionFirst, bi.collisionCount);
}

bool SB::fullOutline(u16 s) const {
    const BlockInfo& bi = reg.infos()[s];
    if (bi.selectionCount) return fillsCube(reg, bi.selectionFirst, bi.selectionCount);
    return bi.collisionCount > 0 && fillsCube(reg, bi.collisionFirst, bi.collisionCount);
}

namespace {

using Props = std::vector<std::pair<std::string, std::string>>;

Props propsOf(std::string_view key) {
    Props out;
    for (const auto& [k, v] : stateProperties(key)) out.emplace_back(std::string(k), std::string(v));
    return out;
}

std::string compose(std::string_view base, Props props) {
    std::sort(props.begin(), props.end());
    std::string k(base);
    if (props.empty()) return k;
    k += '[';
    for (size_t i = 0; i < props.size(); ++i) k += (i ? "," : "") + props[i].first + "=" + props[i].second;
    return k + "]";
}

std::string* findProp(Props& p, std::string_view k) {
    for (auto& [key, v] : p)
        if (key == k) return &v;
    return nullptr;
}

bool isStairShape(std::string_view v) {
    return v == "straight" || v == "inner_left" || v == "inner_right" || v == "outer_left" || v == "outer_right";
}

// Rail shapes are pairs of direction names ("north_south", "ascending_east", "south_west"): transform each one and
// put them back in the game's naming order.
std::string transformRailShape(std::string_view shape, const std::function<Direction(Direction)>& fn) {
    if (shape.starts_with("ascending_")) {
        const auto d = dirFromName(shape.substr(10));
        return d ? "ascending_" + std::string(kDirNames[fn(*d)]) : std::string(shape);
    }
    const size_t us = shape.find('_');
    if (us == std::string_view::npos) return std::string(shape);
    const auto a = dirFromName(shape.substr(0, us)), b = dirFromName(shape.substr(us + 1));
    if (!a || !b) return std::string(shape);
    Direction x = fn(*a), y = fn(*b);
    // Names: north_south, east_west, south_east, south_west, north_west, north_east.
    auto rank = [](Direction d) { return d == North ? 0 : d == South ? 1 : d == East ? 2 : 3; };
    if ((x == North || x == South) && (y == North || y == South)) return "north_south";
    if ((x == East || x == West) && (y == East || y == West)) return "east_west";
    if (rank(x) > rank(y)) std::swap(x, y);
    return std::string(kDirNames[x]) + "_" + kDirNames[y];
}

std::string transformOrientation(std::string_view v, const std::function<Direction(Direction)>& fn) {
    const size_t us = v.find('_');
    if (us == std::string_view::npos) return std::string(v);
    const auto a = dirFromName(v.substr(0, us)), b = dirFromName(v.substr(us + 1));
    if (!a || !b) return std::string(v);
    return std::string(kDirNames[fn(*a)]) + "_" + kDirNames[fn(*b)];
}

constexpr const char* kSideNames[4] = {"north", "east", "south", "west"};

}  // namespace

u16 SB::transform(u16 s, int op) const {
    std::atomic<u32>& slot = transforms_[size_t(s) * 5 + size_t(op)];
    const u32 cached = slot.load(std::memory_order_relaxed);
    if (cached) return u16(cached - 1);
    const u16 r = op < 3 ? computeRotate(s, Rot(op + 1)) : computeMirror(s, Mir(op - 2));
    slot.store(u32(r) + 1, std::memory_order_relaxed);
    return r;
}

u16 SB::rotate(u16 s, Rot r) const { return r == Rot::None ? s : transform(s, int(r) - 1); }
u16 SB::mirror(u16 s, Mir m) const { return m == Mir::None ? s : transform(s, int(m) + 2); }

u16 SB::computeRotate(u16 s, Rot r) const {
    const std::string& full = name(s);
    Props p = propsOf(full);
    if (p.empty()) return s;
    const std::string_view base = baseName(full);
    const auto rot = [r](Direction d) { return st::rotate(r, d); };
    Props out;
    for (const auto& [k, v] : p) {
        if (k == "facing") {
            const auto d = dirFromName(v);
            out.emplace_back(k, d ? kDirNames[st::rotate(r, *d)] : v);
        } else if (k == "axis") {
            out.emplace_back(k, (r == Rot::CW90 || r == Rot::CCW90) ? (v == "x" ? "z" : v == "z" ? "x" : v) : v);
        } else if (k == "rotation") {
            out.emplace_back(k, std::to_string((std::stoi(v) + 4 * int(r)) % 16));
        } else if (k == "north" || k == "east" || k == "south" || k == "west") {
            out.emplace_back(kDirNames[st::rotate(r, *dirFromName(k))], v);
        } else if (k == "shape" && !isStairShape(v)) {
            out.emplace_back(k, transformRailShape(v, rot));
        } else if (k == "orientation") {
            out.emplace_back(k, transformOrientation(v, rot));
        } else {
            out.emplace_back(k, v);
        }
    }
    return reg.idFor(compose(base, std::move(out)));
}

u16 SB::computeMirror(u16 s, Mir m) const {
    const std::string& full = name(s);
    Props p = propsOf(full);
    if (p.empty()) return s;
    const std::string_view base = baseName(full);
    const std::string* facing = findProp(p, "facing");
    const auto facingDir = facing ? dirFromName(*facing) : std::nullopt;
    // Stairs (StairBlock.mirror).
    if (std::string* shape = findProp(p, "shape"); shape && isStairShape(*shape) && facingDir) {
        const bool matches = (m == Mir::LeftRight && axisZ(*facingDir)) || (m == Mir::FrontBack && axisX(*facingDir));
        if (!matches) return s;
        u16 r = rotate(s, Rot::CW180);
        if (*shape == "inner_left") return with(r, "shape", "inner_right");
        if (*shape == "inner_right") return with(r, "shape", "inner_left");
        if (*shape == "outer_left") return with(r, "shape", "outer_right");
        if (*shape == "outer_right") return with(r, "shape", "outer_left");
        return r;
    }
    // Doors (DoorBlock.mirror): rotate by the mirror's rotation of the facing, then flip the hinge.
    if (const std::string* hinge = findProp(p, "hinge"); hinge && facingDir) {
        const u16 r = rotate(s, st::mirrorRotation(m, *facingDir));
        return with(r, "hinge", *hinge == "left" ? "right" : "left");
    }
    // Chests (ChestBlock.mirror): rotate, then the double chest half swaps.
    if (const std::string* type = findProp(p, "type"); type && facingDir && (*type == "single" || *type == "left" || *type == "right")) {
        const u16 r = rotate(s, st::mirrorRotation(m, *facingDir));
        return *type == "single" ? r : with(r, "type", *type == "left" ? "right" : "left");
    }
    const auto mir = [m](Direction d) { return st::mirror(m, d); };
    Props out;
    for (const auto& [k, v] : p) {
        if (k == "facing") {
            const auto d = dirFromName(v);
            out.emplace_back(k, d ? kDirNames[st::mirror(m, *d)] : v);
        } else if (k == "rotation") {
            const int steps = 16, half = 8, rv = std::stoi(v);
            const int corrected = rv > half ? rv - steps : rv;
            const int res = m == Mir::LeftRight ? (half - corrected + steps) % steps : (steps - corrected) % steps;
            out.emplace_back(k, std::to_string(res));
        } else if (k == "north" || k == "east" || k == "south" || k == "west") {
            out.emplace_back(kDirNames[st::mirror(m, *dirFromName(k))], v);
        } else if (k == "shape" && !isStairShape(v)) {
            out.emplace_back(k, transformRailShape(v, mir));
        } else if (k == "orientation") {
            out.emplace_back(k, transformOrientation(v, mir));
        } else {
            out.emplace_back(k, v);
        }
    }
    (void)kSideNames;
    return reg.idFor(compose(base, std::move(out)));
}

u16 SB::withPropertiesOf(u16 target, u16 from) const {
    u16 s = target;
    for (const auto& [k, v] : stateProperties(name(from)))
        if (has(s, k)) s = with(s, k, v);
    return s;
}

// ---- Piece ------------------------------------------------------------------------------------------------

void Piece::setOrientation(std::optional<Direction> d) {
    orientation_ = d;
    rotation_ = Rot::None, mirror_ = Mir::None;
    if (!d) return;
    switch (*d) {
    case South: mirror_ = Mir::LeftRight; break;
    case West: mirror_ = Mir::LeftRight, rotation_ = Rot::CW90; break;
    case East: rotation_ = Rot::CW90; break;
    default: break;
    }
}

int Piece::worldX(int x, int z) const {
    if (!orientation_) return x;
    switch (*orientation_) {
    case North: case South: return box.minX + x;
    case West: return box.maxX - z;
    case East: return box.minX + z;
    default: return x;
    }
}

int Piece::worldZ(int x, int z) const {
    if (!orientation_) return z;
    switch (*orientation_) {
    case North: return box.maxZ - z;
    case South: return box.minZ + z;
    case West: case East: return box.minZ + x;
    default: return z;
    }
}

namespace {
bool shapeCheckBlock(const SB& b, u16 s) {
    static const char* const kNames[] = {"minecraft:nether_brick_fence", "minecraft:torch", "minecraft:wall_torch",
                                         "minecraft:oak_fence", "minecraft:spruce_fence", "minecraft:dark_oak_fence",
                                         "minecraft:pale_oak_fence", "minecraft:acacia_fence", "minecraft:birch_fence",
                                         "minecraft:jungle_fence", "minecraft:ladder", "minecraft:iron_bars"};
    const std::string_view n = b.blockName(s);
    for (const char* k : kNames)
        if (n == k) return true;
    return false;
}
}  // namespace

void Piece::placeBlock(PlaceCtx& c, u16 state, int x, int y, int z, const BBox& chunkBB) {
    const BlockPos pos = worldPos(x, y, z);
    if (!chunkBB.isInside(pos)) return;
    if (!canBeReplaced(c, x, y, z, chunkBB)) return;
    if (mirror_ != Mir::None) state = c.b.mirror(state, mirror_);
    if (rotation_ != Rot::None) state = c.b.rotate(state, rotation_);
    c.set(pos, state);
    if (shapeCheckBlock(c.b, state)) c.markPostProcess(pos);
}

u16 Piece::getBlock(PlaceCtx& c, int x, int y, int z, const BBox& chunkBB) const {
    const BlockPos p = worldPos(x, y, z);
    return chunkBB.isInside(p) ? c.get(p) : c.b.air;
}

bool Piece::isInterior(PlaceCtx& c, int x, int y, int z, const BBox& chunkBB) const {
    const BlockPos p = worldPos(x, y + 1, z);
    return chunkBB.isInside(p) && p.y < c.height(Heightmap::OceanFloorWG, p.x, p.z);
}

void Piece::generateAirBox(PlaceCtx& c, const BBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z) placeBlock(c, c.b.air, x, y, z, chunkBB);
}

void Piece::generateBox(PlaceCtx& c, const BBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1, u16 edge, u16 fill,
                        bool skipAir) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z) {
                if (skipAir && c.b.isAir(getBlock(c, x, y, z, chunkBB))) continue;
                if (y != y0 && y != y1 && x != x0 && x != x1 && z != z0 && z != z1)
                    placeBlock(c, fill, x, y, z, chunkBB);
                else
                    placeBlock(c, edge, x, y, z, chunkBB);
            }
}

void Piece::generateBox(PlaceCtx& c, const BBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1, bool skipAir,
                        RandomSource& r, BlockSelector& sel) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z) {
                if (skipAir && c.b.isAir(getBlock(c, x, y, z, chunkBB))) continue;
                sel.select(r, x, y, z, y == y0 || y == y1 || x == x0 || x == x1 || z == z0 || z == z1);
                placeBlock(c, sel.next, x, y, z, chunkBB);
            }
}

void Piece::generateMaybeBox(PlaceCtx& c, const BBox& chunkBB, RandomSource& r, float probability, int x0, int y0, int z0,
                             int x1, int y1, int z1, u16 edge, u16 fill, bool skipAir, bool hasToBeInside) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z) {
                if (r.nextFloat() > probability) continue;
                if (skipAir && c.b.isAir(getBlock(c, x, y, z, chunkBB))) continue;
                if (hasToBeInside && !isInterior(c, x, y, z, chunkBB)) continue;
                if (y != y0 && y != y1 && x != x0 && x != x1 && z != z0 && z != z1)
                    placeBlock(c, fill, x, y, z, chunkBB);
                else
                    placeBlock(c, edge, x, y, z, chunkBB);
            }
}

void Piece::maybeGenerateBlock(PlaceCtx& c, const BBox& chunkBB, RandomSource& r, float probability, int x, int y, int z, u16 s) {
    if (r.nextFloat() < probability) placeBlock(c, s, x, y, z, chunkBB);
}

void Piece::generateUpperHalfSphere(PlaceCtx& c, const BBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1, u16 fill,
                                    bool skipAir) {
    const float diagX = float(x1 - x0 + 1), diagY = float(y1 - y0 + 1), diagZ = float(z1 - z0 + 1);
    const float cx = float(x0) + diagX / 2.0f, cz = float(z0) + diagZ / 2.0f;
    for (int y = y0; y <= y1; ++y) {
        const float ny = float(y - y0) / diagY;
        for (int x = x0; x <= x1; ++x) {
            const float nx = (float(x) - cx) / (diagX * 0.5f);
            for (int z = z0; z <= z1; ++z) {
                const float nz = (float(z) - cz) / (diagZ * 0.5f);
                if (skipAir && c.b.isAir(getBlock(c, x, y, z, chunkBB))) continue;
                const float dist = nx * nx + ny * ny + nz * nz;
                if (dist <= 1.05f) placeBlock(c, fill, x, y, z, chunkBB);
            }
        }
    }
}

bool Piece::isReplaceableByStructures(PlaceCtx& c, u16 s) const {
    return c.b.isAir(s) || c.b.liquid(s) || c.b.is(s, "minecraft:glow_lichen") || c.b.is(s, "minecraft:seagrass") ||
           c.b.is(s, "minecraft:tall_seagrass");
}

void Piece::fillColumnDown(PlaceCtx& c, u16 s, int x, int startY, int z, const BBox& chunkBB) {
    BlockPos p = worldPos(x, startY, z);
    if (!chunkBB.isInside(p)) return;
    while (isReplaceableByStructures(c, c.get(p)) && p.y > c.minY() + 1) {
        c.set(p, s);
        --p.y;
    }
}

u16 Piece::reorient(PlaceCtx& c, const BlockPos& p, u16 state) {
    std::optional<Direction> solid;
    for (Direction d : kPlaneHorizontal) {
        const u16 n = c.get(relative(p, d));
        if (c.b.is(n, "minecraft:chest")) return state;
        if (c.b.solidRender(n)) {
            if (solid) {
                solid.reset();
                break;
            }
            solid = d;
        }
    }
    if (solid) return c.b.with(state, "facing", opposite(*solid));
    Direction lock = c.b.dir(state).value_or(North);
    BlockPos rp = relative(p, lock);
    if (c.b.solidRender(c.get(rp))) lock = opposite(lock), rp = relative(p, lock);
    if (c.b.solidRender(c.get(rp))) lock = clockWise(lock), rp = relative(p, lock);
    if (c.b.solidRender(c.get(rp))) lock = opposite(lock), rp = relative(p, lock);
    return c.b.with(state, "facing", lock);
}

bool Piece::createChest(PlaceCtx& c, const BBox& chunkBB, RandomSource& r, const BlockPos& p, std::optional<u16> state) {
    if (!chunkBB.isInside(p) || c.b.is(c.get(p), "minecraft:chest")) return false;
    const u16 s = state ? *state : reorient(c, p, c.b.def("minecraft:chest"));
    c.set(p, s);
    if (c.b.is(c.get(p), "minecraft:chest")) r.nextLong();  // ChestBlockEntity.setLootTable seed
    return true;
}

bool Piece::createDispenser(PlaceCtx& c, const BBox& chunkBB, RandomSource& r, int x, int y, int z, Direction facing) {
    const BlockPos p = worldPos(x, y, z);
    if (!chunkBB.isInside(p) || c.b.is(c.get(p), "minecraft:dispenser")) return false;
    placeBlock(c, c.b.with(c.b.def("minecraft:dispenser"), "facing", facing), x, y, z, chunkBB);
    if (c.b.is(c.get(p), "minecraft:dispenser")) r.nextLong();
    return true;
}

// ---- PiecesBuilder ----------------------------------------------------------------------------------------

BBox PiecesBuilder::boundingBox() const {
    BBox b = pieces.front()->box;
    for (const auto& p : pieces) b.encapsulate(p->box);
    return b;
}

int PiecesBuilder::moveBelowSeaLevel(int seaLevel, int minY, RandomSource& r, int offset) {
    const int maxY = seaLevel - offset;
    const BBox b = boundingBox();
    int y1 = b.ySpan() + minY + 1;
    if (y1 < maxY) y1 += r.nextInt(maxY - y1);
    const int dy = y1 - b.maxY;
    offsetPiecesVertically(dy);
    return dy;
}

void PiecesBuilder::moveInsideHeights(RandomSource& r, int lowestAllowed, int highestAllowed) {
    const BBox b = boundingBox();
    const int span = highestAllowed - lowestAllowed + 1 - b.ySpan();
    const int y0 = span > 1 ? lowestAllowed + r.nextInt(span) : lowestAllowed;
    offsetPiecesVertically(y0 - b.minY);
}

// ---- Structure / Start ------------------------------------------------------------------------------------

std::optional<Stub> Structure::onTopOfChunkCenter(GenCtx& c, HType h, std::function<void(PiecesBuilder&)> gen) const {
    if (!c.couldValidBiomeExistOnTopOfChunkCenter()) return std::nullopt;
    const int x = c.middleBlockX(), z = c.middleBlockZ();
    return Stub{{x, c.firstOccupiedHeight(x, z, h), z}, std::move(gen)};
}

void Start::placeInChunk(PlaceCtx& c, RandomSource& r, const BBox& chunkBB) const {
    if (pieces.empty()) return;
    const BBox& centre = pieces.front()->box;
    const BlockPos cp = centre.center();
    const BlockPos ref{cp.x, centre.minY, cp.z};
    for (const auto& p : pieces)
        if (p->box.intersects(chunkBB)) p->postProcess(c, r, chunkBB, ref);
    structure->afterPlace(c, r, chunkBB, pieces);
}

namespace {
std::mutex& typesMutex() {
    static std::mutex m;
    return m;
}
std::vector<std::pair<std::string, StructureFactory>>& types() {
    static std::vector<std::pair<std::string, StructureFactory>> t;
    return t;
}
}  // namespace

void registerStructureType(std::string type, StructureFactory f) {
    std::lock_guard lock(typesMutex());
    types().emplace_back(std::move(type), std::move(f));
}

const StructureFactory* findStructureType(std::string_view type) {
    std::lock_guard lock(typesMutex());
    const std::string want = DataPack::normalize(type);
    for (const auto& [n, f] : types())
        if (DataPack::normalize(n) == want) return &f;
    return nullptr;
}

// ---- GenCtx -------------------------------------------------------------------------------------------------

GenCtx::GenCtx(const StructureEnv& e, DensityContext& d, i64 s, int cx, int cz, const std::vector<bool>* biomes)
    : env(e), dctx(d), seed(s), chunkX(cx), chunkZ(cz), random(std::make_unique<LegacyRandom>(0)), validBiomes(biomes) {
    // WorldgenRandom.setLargeFeatureSeed
    random.setSeed(seed);
    const i64 xs = random.nextLong(), zs = random.nextLong();
    random.setSeed(jmul(i64(cx), xs) ^ jmul(i64(cz), zs) ^ seed);
}

int GenCtx::minY() const { return env.minY(); }
int GenCtx::maxY() const { return env.minY() + env.height(); }
int GenCtx::seaLevel() const { return env.seaLevel(); }

u16 GenCtx::noiseBiome(int qx, int qy, int qz) const { return env.gen.noiseBiome(dctx, qx, qy, qz); }

bool GenCtx::isValidBiomeAt(const BlockPos& p) const { return validBiome(noiseBiome(p.x >> 2, p.y >> 2, p.z >> 2)); }

bool GenCtx::couldStructureExistInColumn(int blockX, int blockZ, int minBlockY, int maxBlockY) const {
    const int qx = blockX >> 2, qz = blockZ >> 2, q0 = minBlockY >> 2, q1 = maxBlockY >> 2;
    for (int qy = q0; qy <= q1; ++qy)
        if (validBiome(noiseBiome(qx, qy, qz))) return true;
    return false;
}

bool GenCtx::couldValidBiomeExistOnTopOfChunkCenter() const {
    // couldValidBiomeExistInTerrainColumn: heightAccessor.getMinY() - 1 .. getMaxY() (inclusive max).
    return couldStructureExistInColumn(middleBlockX(), middleBlockZ(), minY() - 1, maxY() - 1);
}

int GenCtx::baseHeight(int x, int z, HType t) const { return env.baseHeight(dctx, x, z, t); }
const std::vector<u16>& GenCtx::baseColumn(int x, int z) const { return env.baseColumn(dctx, x, z); }

int GenCtx::lowestY(int sizeX, int sizeZ) const { return lowestY(minBlockX(), minBlockZ(), sizeX, sizeZ); }

int GenCtx::lowestY(int minX, int minZ, int sizeX, int sizeZ) const {
    const int a = firstOccupiedHeight(minX, minZ, HType::WorldSurfaceWG);
    const int b = firstOccupiedHeight(minX, minZ + sizeZ, HType::WorldSurfaceWG);
    const int c = firstOccupiedHeight(minX + sizeX, minZ, HType::WorldSurfaceWG);
    const int d = firstOccupiedHeight(minX + sizeX, minZ + sizeZ, HType::WorldSurfaceWG);
    return std::min(std::min(a, b), std::min(c, d));
}

int GenCtx::meanFirstOccupiedHeight(int minX, int sizeX, int minZ, int sizeZ) const {
    const int a = firstOccupiedHeight(minX, minZ, HType::WorldSurfaceWG);
    const int b = firstOccupiedHeight(minX, minZ + sizeZ, HType::WorldSurfaceWG);
    const int c = firstOccupiedHeight(minX + sizeX, minZ, HType::WorldSurfaceWG);
    const int d = firstOccupiedHeight(minX + sizeX, minZ + sizeZ, HType::WorldSurfaceWG);
    return (a + b + c + d) / 4;
}

// ---- StructureEnv --------------------------------------------------------------------------------------------

struct StructureEnv::HeightCache {
    struct Shard {
        std::mutex mutex;
        std::unordered_map<u64, std::array<i16, 4>> map;  // WORLD_SURFACE_WG, OCEAN_FLOOR_WG, MOTION_BLOCKING
    };
    static constexpr int kShards = 64;
    Shard shards[kShards];
};

StructureEnv::StructureEnv(const ChunkGenerator& generator, const FeatureSystem* featureSystem, BlockRegistry& blocks,
                           const DataPack& dataPack, i64 worldSeed)
    : gen(generator), features(featureSystem), data(dataPack), seed(worldSeed), facts(blocks, dataPack), b(blocks, facts),
      templates(std::make_unique<TemplateManager>(*this)), pools(std::make_unique<Pools>(*this)),
      heights_(std::make_unique<HeightCache>()) {}

const FeatureEnv* StructureEnv::featureEnv() const { return features ? &features->env() : nullptr; }

float StructureEnv::temperature(u16 biome, const BlockPos& p) const { return features ? features->temperature(biome, p) : 0.5f; }

void PlaceCtx::markPostProcess(const BlockPos& p) const { env.markPostProcess(p); }

void StructureEnv::markPostProcess(const BlockPos& p) const {
    const u64 k = (u64(u32(p.x >> 4)) << 32) | u32(p.z >> 4);
    std::lock_guard lock(markMutex_);
    std::vector<BlockPos>& v = marks_[k];
    if (std::find(v.begin(), v.end(), p) == v.end()) v.push_back(p);
}

void StructureEnv::trimCaches(int cx, int cz, int radius) const {
    auto far = [&](u64 key, int shift) {
        const int x = int(i32(u32(key >> 32))) >> shift, z = int(i32(u32(key))) >> shift;
        return std::max(std::abs(x - cx), std::abs(z - cz)) > radius;
    };
    for (HeightCache::Shard& shard : heights_->shards) {
        std::lock_guard lock(shard.mutex);
        std::erase_if(shard.map, [&](const auto& kv) { return far(kv.first, 4); });  // keys are block columns
    }
    std::lock_guard lock(markMutex_);
    std::erase_if(marks_, [&](const auto& kv) { return far(kv.first, 0); });  // keys are chunks
}

std::vector<BlockPos> StructureEnv::takeMarks(int cx, int cz) const {
    const u64 k = (u64(u32(cx)) << 32) | u32(cz);
    std::lock_guard lock(markMutex_);
    auto it = marks_.find(k);
    if (it == marks_.end()) return {};
    std::vector<BlockPos> v = std::move(it->second);
    marks_.erase(it);
    return v;
}

RandomSource& PlaceCtx::random() const {
    static thread_local LegacyRandom fallback(0);
    RandomSource* r = decorationContext().regionRandom;
    return r ? *r : fallback;
}

i64 PlaceCtx::seed() const { return env.seed; }

namespace {
thread_local const SB* threadSB = nullptr;
}
const SB& sb() { return *threadSB; }
void setThreadSB(const SB* b) { threadSB = b; }

bool ScatteredPiece::updateAverageGroundHeight(PlaceCtx& c, const BBox& chunkBB, int offset) {
    if (heightPosition >= 0) return true;
    int total = 0, count = 0;
    for (int z = box.minZ; z <= box.maxZ; ++z)
        for (int x = box.minX; x <= box.maxX; ++x)
            if (chunkBB.isInside(x, 64, z)) {
                total += c.height(Heightmap::MotionBlockingNoLeaves, x, z);
                ++count;
            }
    if (count == 0) return false;
    heightPosition = total / count;
    box.move(0, heightPosition - box.minY + offset, 0);
    return true;
}

bool ScatteredPiece::updateHeightPositionToLowestGroundHeight(PlaceCtx& c, int offset) {
    if (heightPosition >= 0) return true;
    int lowest = c.maxY() - 1 + 1;
    bool found = false;
    for (int z = box.minZ; z <= box.maxZ; ++z)
        for (int x = box.minX; x <= box.maxX; ++x) {
            lowest = std::min(lowest, c.height(Heightmap::MotionBlockingNoLeaves, x, z));
            found = true;
        }
    if (!found) return false;
    heightPosition = lowest;
    box.move(0, heightPosition - box.minY + offset, 0);
    return true;
}

StructureEnv::~StructureEnv() = default;

std::vector<bool> StructureEnv::biomeSet(const json& j) const {
    BiomeTable& table = biomes();
    std::vector<bool> out;
    auto add = [&](const std::string& n) {
        const u16 id = table.id(n.find(':') == std::string::npos ? "minecraft:" + n : n);
        if (id >= out.size()) out.resize(size_t(id) + 1, false);
        out[id] = true;
    };
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (!s.empty() && s[0] == '#')
            for (const std::string& n : data.tag("worldgen/biome", DataPack::normalize(s.substr(1)))) add(n);
        else
            add(s);
    } else if (j.is_array()) {
        for (const auto& e : j)
            if (e.is_string()) add(e.get<std::string>());
    }
    return out;
}

bool StructureEnv::biomeInTag(u16 biome, std::string_view tag) const {
    std::lock_guard lock(tagMutex_);
    auto it = biomeTags_.find(std::string(tag));
    if (it == biomeTags_.end()) it = biomeTags_.emplace(std::string(tag), biomeSet(json("#" + std::string(tag)))).first;
    return biome < it->second.size() && it->second[biome];
}

namespace {
// The base column's states: default block, fluids or air (NoiseBasedChunkGenerator.iterateNoiseColumn).
struct ColumnCache {
    int x = INT32_MIN, z = INT32_MIN;
    std::vector<u16> states;
};
}  // namespace

const std::vector<u16>& StructureEnv::baseColumn(DensityContext& ctx, int x, int z) const {
    (void)ctx;
    thread_local ColumnCache cache[8];
    thread_local int next = 0;
    thread_local const StructureEnv* owner = nullptr;
    if (owner != this) {
        for (auto& e : cache) e.x = INT32_MIN;
        owner = this;
    }
    for (auto& e : cache)
        if (e.x == x && e.z == z) return e.states;
    ColumnCache& e = cache[next];
    next = (next + 1) & 7;
    thread_local DensityContext columnCtx;
    gen.baseColumn(x, z, e.states, columnCtx);
    e.x = x, e.z = z;
    return e.states;
}

int StructureEnv::baseHeight(DensityContext& ctx, int x, int z, HType t) const {
    const u64 k = (u64(u32(x)) << 32) | u32(z);
    HeightCache::Shard& shard = heights_->shards[(u32(x) * 31u + u32(z) * 17u) & (HeightCache::kShards - 1)];
    std::array<i16, 4> h{};
    bool found = false;
    {
        std::lock_guard lock(shard.mutex);
        if (auto it = shard.map.find(k); it != shard.map.end()) h = it->second, found = true;
    }
    if (!found) {
        const std::vector<u16>& col = baseColumn(ctx, x, z);
        const int minY = gen.minY();
        int surface = minY, floor = minY, motion = minY;
        for (int i = int(col.size()) - 1; i >= 0; --i) {
            const u16 s = col[size_t(i)];
            if (surface == minY && !b.isAir(s)) surface = minY + i + 1;
            if (motion == minY && (b.blocksMotion(s) || !b.fluidEmpty(s))) motion = minY + i + 1;
            if (b.blocksMotion(s)) {
                floor = minY + i + 1;
                break;
            }
        }
        h = {i16(surface), i16(floor), i16(motion), 0};
        std::lock_guard lock(shard.mutex);
        if (shard.map.size() > 32768) shard.map.clear();  // bounded: ~2M columns over all shards
        shard.map.emplace(k, h);
    }
    switch (t) {
    case HType::WorldSurfaceWG: case HType::WorldSurface: return h[0];
    case HType::OceanFloorWG: case HType::OceanFloor: return h[1];
    default: return h[2];
    }
}

}  // namespace wg::st
