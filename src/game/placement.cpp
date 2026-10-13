#include "game/placement.h"

#include "assets/assets.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace place {
namespace {

constexpr int kStep[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
constexpr const char* kName[6] = {"down", "up", "north", "south", "west", "east"};
constexpr int kDown = 0, kUp = 1, kNorth = 2, kSouth = 3, kWest = 4, kEast = 5;
constexpr float kPi = 3.14159265f;

int opposite(int d) { return d ^ 1; }
bool horizontal(int d) { return d >= 2; }
int axisOf(int d) { return d < 2 ? 1 : d < 4 ? 2 : 0; }
int clockWise(int d) { return d == kNorth ? kEast : d == kEast ? kSouth : d == kSouth ? kWest : d == kWest ? kNorth : d; }
int counterClockWise(int d) { return d == kNorth ? kWest : d == kWest ? kSouth : d == kSouth ? kEast : d == kEast ? kNorth : d; }
int faceByName(std::string_view n) {
    for (int d = 0; d < 6; ++d)
        if (n == kName[d]) return d;
    return -1;
}

std::string_view baseOf(const std::string& key) { return std::string_view(key).substr(0, key.find('[')); }

// BlockPlaceContext.getHorizontalDirection: Direction.fromYRot of the player's yaw.
int horizontalDirection(const Context& c) {
    static constexpr int kByQuarter[4] = {kSouth, kWest, kNorth, kEast};
    return kByQuarter[int(std::floor(c.yaw * 180.0f / kPi / 90.0f + 0.5f)) & 3];
}

// Direction.orderedByNearest: the six directions by how much the player looks along them.
std::array<int, 6> orderedByNearest(const Context& c) {
    const float pitch = c.pitch, yaw = -c.yaw;
    const float pitchSin = std::sin(pitch), pitchCos = std::cos(pitch), yawSin = std::sin(yaw), yawCos = std::cos(yaw);
    const bool xPos = yawSin > 0.0f, yPos = pitchSin < 0.0f, zPos = yawCos > 0.0f;
    const float xYaw = xPos ? yawSin : -yawSin, yMag = yPos ? -pitchSin : pitchSin, zYaw = zPos ? yawCos : -yawCos;
    const float xMag = xYaw * pitchCos, zMag = zYaw * pitchCos;
    const int ax = xPos ? kEast : kWest, ay = yPos ? kUp : kDown, az = zPos ? kSouth : kNorth;
    auto make = [](int a, int b, int d) { return std::array<int, 6>{a, b, d, opposite(d), opposite(b), opposite(a)}; };
    if (xYaw > zYaw) {
        if (yMag > xMag) return make(ay, ax, az);
        return zMag > yMag ? make(ax, az, ay) : make(ax, ay, az);
    }
    if (yMag > zMag) return make(ay, az, ax);
    return xMag > yMag ? make(az, ax, ay) : make(az, ay, ax);
}

// BlockPlaceContext.getNearestLookingDirections: placing next to the clicked block, its direction comes first.
std::array<int, 6> nearestLookingDirections(const Context& c, bool replaceClicked) {
    std::array<int, 6> d = orderedByNearest(c);
    if (replaceClicked) return d;
    const int first = opposite(c.face);
    int index = 0;
    while (index < 6 && d[size_t(index)] != first) ++index;
    if (index > 0 && index < 6) {
        for (int i = index; i > 0; --i) d[size_t(i)] = d[size_t(i - 1)];
        d[0] = first;
    }
    return d;
}

// Shape tests on the collision boxes (the support shape of most blocks).
// Whether the face `dir` of the block's collision shape covers the rectangle lo..hi of the face plane (coordinates
// per axis, 0..1; the face's own axis is ignored).
bool faceCovers(const BlockRegistry& reg, u16 id, int dir, const float lo[3], const float hi[3]) {
    const BlockInfo& b = reg.infos()[id];
    if (!b.collisionCount) return false;
    const int axis = axisOf(dir), a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
    const bool positive = dir & 1;
    constexpr int kSamples = 6;
    for (int i = 0; i < kSamples; ++i)
        for (int j = 0; j < kSamples; ++j) {
            const float u = lo[a1] + (hi[a1] - lo[a1]) * (float(i) + 0.5f) / kSamples;
            const float v = lo[a2] + (hi[a2] - lo[a2]) * (float(j) + 0.5f) / kSamples;
            bool covered = false;
            for (u32 k = 0; k < b.collisionCount && !covered; ++k) {
                const Box& s = reg.box(b.collisionFirst + k);
                const bool onPlane = positive ? s.max[axis] >= 1.0f - 1e-4f : s.min[axis] <= 1e-4f;
                covered = onPlane && u >= s.min[a1] && u <= s.max[a1] && v >= s.min[a2] && v <= s.max[a2];
            }
            if (!covered) return false;
        }
    return true;
}
// BlockState.isFaceSturdy(FULL) of the face `dir` of the block at the given state
bool faceCovered(const BlockRegistry& reg, u16 id, int dir, float lo, float hi) {
    const float l[3] = {lo, lo, lo}, h[3] = {hi, hi, hi};
    return faceCovers(reg, id, dir, l, h);
}
bool sturdy(const BlockRegistry& reg, u16 id, int dir) { return faceCovered(reg, id, dir, 0.0f, 1.0f); }
// Block.canSupportCenter: the face's 2 x 2 px centre column
bool supportsCenter(const BlockRegistry& reg, u16 id, int dir) { return faceCovered(reg, id, dir, 7.0f / 16.0f, 9.0f / 16.0f); }
// BlockBehaviour.BlockStateBase.isSolid (calculateSolid): a collision shape whose bounds average 0.7291 or a full height
bool solid(const BlockRegistry& reg, u16 id) {
    const BlockInfo& b = reg.infos()[id];
    if (!b.collisionCount || b.fluid) return false;
    float lo[3] = {1, 1, 1}, hi[3] = {0, 0, 0};
    for (u32 k = 0; k < b.collisionCount; ++k) {
        const Box& s = reg.box(b.collisionFirst + k);
        for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], s.min[a]), hi[a] = std::max(hi[a], s.max[a]);
    }
    const float size = ((hi[0] - lo[0]) + (hi[1] - lo[1]) + (hi[2] - lo[2])) / 3.0f;
    return size >= 0.7291666f || hi[1] - lo[1] >= 1.0f;
}
bool fullCollision(const BlockRegistry& reg, u16 id) {  // isCollisionShapeFullBlock
    const BlockInfo& b = reg.infos()[id];
    if (b.collisionCount != 1) return false;
    const Box& s = reg.box(b.collisionFirst);
    return s.min[0] <= 0 && s.min[1] <= 0 && s.min[2] <= 0 && s.max[0] >= 1 && s.max[1] >= 1 && s.max[2] >= 1;
}

u16 at(const Context& c, const int p[3], int d, int n = 1) {
    return c.block(p[0] + kStep[d][0] * n, p[1] + kStep[d][1] * n, p[2] + kStep[d][2] * n);
}
std::string nameOf(const Context& c, u16 id) { return std::string(baseOf(c.reg.name(id))); }
u16 with(const Context& c, u16 id, std::string_view key, std::string_view value) {
    return c.reg.property(id, key).empty() ? id : c.reg.withProperty(id, key, value);
}
int intProp(const Context& c, u16 id, std::string_view key) {
    const std::string_view v = c.reg.property(id, key);
    return v.empty() ? 0 : std::atoi(std::string(v).c_str());
}

bool isStairs(const std::string& n) { return n.ends_with("_stairs"); }
bool isDoor(const Context& c, u16 id) { return !c.reg.property(id, "hinge").empty(); }
bool isChest(const std::string& n) { return n == "minecraft:chest" || n == "minecraft:trapped_chest" || n.ends_with("copper_chest"); }
bool isFence(const std::string& n) { return n.ends_with("_fence"); }
bool isBars(const std::string& n) { return n.ends_with("_pane") || n == "minecraft:glass_pane" || n.ends_with("_bars"); }
bool isWall(const std::string& n) { return n.ends_with("_wall") && n.find("wall_") == std::string::npos; }
// Block.isExceptionForConnection
bool connectionException(const Context& c, u16 id, const std::string& n) {
    return c.reg.infos()[id].leaves || n == "minecraft:barrier" || n == "minecraft:carved_pumpkin" || n == "minecraft:jack_o_lantern" ||
           n == "minecraft:melon" || n == "minecraft:pumpkin" || n.ends_with("shulker_box");
}

// FenceBlock.connectsTo: towards the neighbour, from its face `face` (the side looking at the fence)
bool fenceConnects(const Context& c, const std::string& fence, u16 n, int face) {
    const std::string nn = nameOf(c, n);
    const bool wooden = fence != "minecraft:nether_brick_fence";
    const bool sameFence = isFence(nn) && (nn != "minecraft:nether_brick_fence") == wooden;
    bool gate = false;
    if (nn.ends_with("_fence_gate")) {  // FenceGateBlock.connectsToDirection: the gate's facing across the line
        const int facing = faceByName(c.reg.property(n, "facing"));
        gate = facing >= 0 && axisOf(facing) == axisOf(clockWise(face));
    }
    return (!connectionException(c, n, nn) && sturdy(c.reg, n, face)) || sameFence || gate;
}
// IronBarsBlock.attachsTo
bool barsAttach(const Context& c, u16 n, int face) {
    const std::string nn = nameOf(c, n);
    return (!connectionException(c, n, nn) && sturdy(c.reg, n, face)) || isBars(nn) || isWall(nn);
}

// WallBlock.connectsTo: the neighbour seen from its face `face`
bool wallConnects(const Context& c, u16 n, int face) {
    const std::string nn = nameOf(c, n);
    bool gate = false;
    if (nn.ends_with("_fence_gate")) {
        const int facing = faceByName(c.reg.property(n, "facing"));
        gate = facing >= 0 && axisOf(facing) == axisOf(clockWise(face));
    }
    return isWall(nn) || (!connectionException(c, n, nn) && sturdy(c.reg, n, face)) || isBars(nn) || gate;
}
// BlockTags.WALL_POST_OVERRIDE
bool postOverride(const std::string& n) {
    return n == "minecraft:torch" || n == "minecraft:soul_torch" || n == "minecraft:redstone_torch" || n == "minecraft:copper_torch" ||
           n == "minecraft:tripwire" || (n.ends_with("_sign") && !n.ends_with("hanging_sign")) || n.ends_with("_banner") ||
           n.ends_with("_pressure_plate") || n == "minecraft:cactus_flower";
}
// WallBlock.updateShape(level, state, topPos, topNeighbour, north, east, south, west): the sides' heights and the post
u16 wallShape(const Context& c, u16 state, const int p[3], const bool connected[6]) {
    const u16 top = at(c, p, kUp);
    auto covered = [&](float x0, float x1, float z0, float z1) {  // isCovered(aboveShape, test): the top's down face
        const float lo[3] = {x0 / 16.0f, 0.0f, z0 / 16.0f}, hi[3] = {x1 / 16.0f, 1.0f, z1 / 16.0f};
        return faceCovers(c.reg, top, kDown, lo, hi);
    };
    // TEST_SHAPES_WALL: a 2 px wide strip from the centre to each side
    const bool tall[6] = {false, false, covered(7, 9, 0, 9), covered(7, 9, 7, 16), covered(0, 9, 7, 9), covered(7, 16, 7, 9)};
    std::string_view side[6];
    for (int d = 2; d < 6; ++d) {
        side[d] = !connected[d] ? "none" : tall[d] ? "tall" : "low";
        state = with(c, state, kName[d], side[d]);
    }
    // shouldRaisePost
    bool post;
    const std::string topName = nameOf(c, top);
    if (isWall(topName) && c.reg.property(top, "up") == "true") post = true;
    else {
        const bool n = side[kNorth] == "none", s = side[kSouth] == "none", w = side[kWest] == "none", e = side[kEast] == "none";
        if ((n && s && w && e) || n != s || w != e) post = true;
        else if ((side[kNorth] == "tall" && side[kSouth] == "tall") || (side[kEast] == "tall" && side[kWest] == "tall")) post = false;
        else post = postOverride(topName) || covered(7, 9, 7, 9);
    }
    return with(c, state, "up", post ? "true" : "false");
}

// RedStoneWireBlock
bool isWire(const std::string& n) { return n == "minecraft:redstone_wire"; }
// BlockBehaviour.isRedstoneConductor: a full collision cube, but the blocks Blocks.java says never (or always)
bool conductor(const Context& c, u16 id) {
    const std::string n = nameOf(c, id);
    if (n == "minecraft:soul_sand" || n == "minecraft:mud") return true;
    if (c.reg.infos()[id].leaves || n.ends_with("glass") || n.find("copper_grate") != std::string::npos ||
        n.find("copper_bulb") != std::string::npos || n.ends_with("piston"))
        return false;
    for (std::string_view never : {"tnt", "ice", "frosted_ice", "glowstone", "beacon", "redstone_block", "sea_lantern", "chorus_flower",
                                   "observer", "bamboo", "scaffolding", "powder_snow", "pointed_dripstone", "sulfur_spike"})
        if (n.size() == never.size() + 10 && std::string_view(n).substr(10) == never) return false;
    return fullCollision(c.reg, id);
}
// BlockBehaviour.isSignalSource of the blocks that override it
bool signalSource(const std::string& n) {
    if (n.ends_with("_pressure_plate") || n.ends_with("_button") || n.ends_with("lightning_rod")) return true;
    for (std::string_view s : {"daylight_detector", "detector_rail", "comparator", "jukebox", "lectern", "lever", "redstone_block",
                               "redstone_torch", "redstone_wall_torch", "sculk_sensor", "calibrated_sculk_sensor", "target",
                               "trapped_chest", "tripwire_hook"})
        if (n.size() == s.size() + 10 && std::string_view(n).substr(10) == s) return true;
    return false;
}
// shouldRedstoneWireConnectTo (dir: from the wire to the block, -1 for none)
bool wireConnectsTo(const Context& c, u16 id, int dir) {
    const std::string n = nameOf(c, id);
    if (isWire(n)) return true;
    if (n == "minecraft:repeater") {
        const int f = faceByName(c.reg.property(id, "facing"));
        return dir >= 0 && (f == dir || f == opposite(dir));
    }
    if (n == "minecraft:observer") return dir >= 0 && faceByName(c.reg.property(id, "facing")) == dir;
    return signalSource(n) && dir >= 0;
}
bool wireSurvivesOn(const Context& c, u16 below) { return sturdy(c.reg, below, kUp) || nameOf(c, below) == "minecraft:hopper"; }
bool wireConnected(const Context& c, u16 s, int d) { return c.reg.property(s, kName[d]) != "none"; }
bool wireDot(const Context& c, u16 s) {
    for (int d = 2; d < 6; ++d)
        if (wireConnected(c, s, d)) return false;
    return true;
}
bool wireCross(const Context& c, u16 s) {
    for (int d = 2; d < 6; ++d)
        if (!wireConnected(c, s, d)) return false;
    return true;
}
// getConnectingSide
std::string_view wireSide(const Context& c, const int p[3], int dir) {
    const bool canConnectUp = !conductor(c, at(c, p, kUp));
    const int q[3] = {p[0] + kStep[dir][0], p[1], p[2] + kStep[dir][2]};
    const u16 rs = c.block(q[0], q[1], q[2]);
    if (canConnectUp) {
        const bool placeableAbove = nameOf(c, rs).ends_with("trapdoor") || wireSurvivesOn(c, rs);
        if (placeableAbove && isWire(nameOf(c, at(c, q, kUp)))) return sturdy(c.reg, rs, opposite(dir)) ? "up" : "side";
    }
    return !wireConnectsTo(c, rs, dir) && (conductor(c, rs) || !isWire(nameOf(c, at(c, q, kDown)))) ? "none" : "side";
}
// getConnectionState: the missing connections, a lone line extended to both sides
u16 wireConnectionState(const Context& c, u16 state, const int p[3]) {
    const bool wasDot = wireDot(c, state);
    u16 s = state;
    for (int d = 2; d < 6; ++d) s = with(c, s, kName[d], "none");  // defaultBlockState with the power
    for (int d = 2; d < 6; ++d) s = with(c, s, kName[d], wireSide(c, p, d));
    if (wasDot && wireDot(c, s)) return s;
    const bool north = wireConnected(c, s, kNorth), south = wireConnected(c, s, kSouth), east = wireConnected(c, s, kEast),
               west = wireConnected(c, s, kWest);
    const bool nsEmpty = !north && !south, ewEmpty = !east && !west;
    if (!west && nsEmpty) s = with(c, s, "west", "side");
    if (!east && nsEmpty) s = with(c, s, "east", "side");
    if (!north && ewEmpty) s = with(c, s, "north", "side");
    if (!south && ewEmpty) s = with(c, s, "south", "side");
    return s;
}
u16 wireCrossState(const Context& c, u16 state) {
    for (int d = 2; d < 6; ++d) state = with(c, state, kName[d], "side");
    return state;
}
// RedStoneWireBlock.updateShape of the wire `s` at q towards dirToNeighbour (the changed cell)
u16 wireUpdate(const Context& c, u16 s, const int q[3], int dirToNeighbour) {
    if (dirToNeighbour == kDown) return wireSurvivesOn(c, at(c, q, kDown)) ? s : 0;
    if (dirToNeighbour == kUp) return wireConnectionState(c, s, q);
    const std::string_view side = wireSide(c, q, dirToNeighbour);
    if ((side != "none") == wireConnected(c, s, dirToNeighbour) && !wireCross(c, s)) return with(c, s, kName[dirToNeighbour], side);
    return wireConnectionState(c, with(c, wireCrossState(c, s), kName[dirToNeighbour], side), q);
}

// TripWireBlock.shouldConnectTo: a hook facing back along `dir` (from the wire to it), or more wire
bool tripwireConnects(const Context& c, u16 id, int dir) {
    const std::string n = nameOf(c, id);
    if (n == "minecraft:tripwire_hook") return faceByName(c.reg.property(id, "facing")) == opposite(dir);
    return n == "minecraft:tripwire";
}

// StairBlock.getStairsShape of a stair state at p
std::string_view stairsShape(const Context& c, u16 state, const int p[3]) {
    const int facing = faceByName(c.reg.property(state, "facing"));
    if (facing < 2) return "straight";
    const std::string_view half = c.reg.property(state, "half");
    auto stairFacing = [&](u16 s, int& f) {
        if (!isStairs(nameOf(c, s)) || c.reg.property(s, "half") != half) return false;
        f = faceByName(c.reg.property(s, "facing"));
        return f >= 2;
    };
    auto canTakeShape = [&](int dir) {
        const u16 n = at(c, p, dir);
        return !isStairs(nameOf(c, n)) || faceByName(c.reg.property(n, "facing")) != facing || c.reg.property(n, "half") != half;
    };
    int behind;
    if (stairFacing(at(c, p, facing), behind) && axisOf(behind) != axisOf(facing) && canTakeShape(opposite(behind)))
        return behind == counterClockWise(facing) ? "outer_left" : "outer_right";
    int front;
    if (stairFacing(at(c, p, opposite(facing)), front) && axisOf(front) != axisOf(facing) && canTakeShape(front))
        return front == counterClockWise(facing) ? "inner_left" : "inner_right";
    return "straight";
}

// The wall-mounted form of a standing-or-wall item's block (StandingAndWallBlockItem), "" when there is none.
std::string wallVariant(const std::string& n, int& attachment) {
    attachment = kDown;
    auto replace = [&](std::string_view from, std::string_view to) {
        const size_t i = n.rfind(from);
        return i == std::string::npos ? std::string() : n.substr(0, i) + std::string(to) + n.substr(i + from.size());
    };
    if (n == "minecraft:torch") return "minecraft:wall_torch";
    if (n == "minecraft:soul_torch") return "minecraft:soul_wall_torch";
    if (n == "minecraft:redstone_torch") return "minecraft:redstone_wall_torch";
    if (n.ends_with("_torch") && n.find("wall") == std::string::npos) return replace("_torch", "_wall_torch");  // copper torches
    if (n.ends_with("_hanging_sign") && n.find("wall") == std::string::npos) {
        attachment = kUp;
        return replace("_hanging_sign", "_wall_hanging_sign");
    }
    if (n.ends_with("_sign") && n.find("wall") == std::string::npos) return replace("_sign", "_wall_sign");
    if (n.ends_with("_banner") && n.find("wall") == std::string::npos) return replace("_banner", "_wall_banner");
    if ((n.ends_with("_skull") || n.ends_with("_head")) && n.find("wall") == std::string::npos && n != "minecraft:piston_head")
        return n.ends_with("_skull") ? replace("_skull", "_wall_skull") : replace("_head", "_wall_head");
    if (n.ends_with("_coral_fan") && n.find("wall") == std::string::npos) return replace("_coral_fan", "_coral_wall_fan");
    return {};
}

// canSurvive of the standing and wall forms and the attached blocks predicted here (a sturdy face to hang on).
bool survives(const Context& c, u16 state, const int p[3]) {
    const std::string n = nameOf(c, state);
    const int facing = faceByName(c.reg.property(state, "facing"));
    const bool wallForm = n.find("wall_") != std::string::npos || n.find("_wall") != std::string::npos;
    if (wallForm && facing >= 2) {
        const u16 behind = at(c, p, opposite(facing));
        if (n.ends_with("hanging_sign")) {  // WallHangingSignBlock.canSurvive: a sturdy face (or such a sign) beside it
            auto attach = [&](int side, int face) {
                const u16 a = at(c, p, side);
                if (nameOf(c, a).ends_with("_wall_hanging_sign")) return axisOf(faceByName(c.reg.property(a, "facing"))) == axisOf(facing);
                return sturdy(c.reg, a, face);
            };
            return attach(clockWise(facing), counterClockWise(facing)) || attach(counterClockWise(facing), clockWise(facing));
        }
        if (n.ends_with("_sign") || n.ends_with("_banner")) return solid(c.reg, behind);   // WallSignBlock / WallBannerBlock
        return sturdy(c.reg, behind, facing);                                             // WallTorchBlock, coral fans
    }
    if (n.ends_with("torch")) return supportsCenter(c.reg, at(c, p, kDown), kUp);          // TorchBlock
    if (n.ends_with("_sign") && !n.ends_with("hanging_sign") && !c.reg.property(state, "rotation").empty())
        return solid(c.reg, at(c, p, kDown));                                             // StandingSignBlock
    if (n.ends_with("hanging_sign")) return supportsCenter(c.reg, at(c, p, kUp), kDown);  // CeilingHangingSignBlock
    if (n.ends_with("_banner")) return solid(c.reg, at(c, p, kDown));
    return true;
}

}  // namespace

bool canBeReplaced(const Context& c, u16 held, u16 state, bool replacingClicked, const int atPos[3]) {
    const std::string heldName = nameOf(c, held), name = nameOf(c, state);
    const bool same = heldName == name;
    if (same && heldName.ends_with("_slab")) {  // SlabBlock.canBeReplaced
        const std::string_view type = c.reg.property(state, "type");
        if (type == "double") return false;
        if (!replacingClicked) return true;
        const bool above = c.hit[1] - atPos[1] > 0.5;
        return type == "bottom" ? c.face == kUp || (above && c.face >= 2) : c.face == kDown || (!above && c.face >= 2);
    }
    if (same && name == "minecraft:snow") {  // SnowLayerBlock.canBeReplaced
        if (intProp(c, state, "layers") < 8) return replacingClicked ? c.face == kUp : true;
        return false;
    }
    if (name == "minecraft:snow" && !same) return intProp(c, state, "layers") == 1;
    if (same && !c.sneaking) {  // CandleBlock / SeaPickleBlock / TurtleEggBlock / FlowerBedBlock: one more, up to 4
        for (std::string_view key : {"candles", "pickles", "eggs", "flower_amount", "segment_amount"})
            if (!c.reg.property(state, key).empty()) return intProp(c, state, key) < 4;
    }
    return c.reg.infos()[state].replaceable && !same;
}

Result placementState(const Context& c, u16 held, const int pos[3], bool replaceClicked) {
    Result r;
    BlockRegistry& reg = c.reg;
    const std::string n = nameOf(c, held);
    const u16 existing = c.block(pos[0], pos[1], pos[2]);
    const bool water = c.waterSource && c.waterSource(pos[0], pos[1], pos[2]);
    const int horiz = horizontalDirection(c);
    const bool upperClick = c.face == kDown || (c.face >= 2 && c.hit[1] - pos[1] > 0.5);
    u16 s = held;

    // Stacking on the same block (a second slab, one more snow layer / candle / pickle / egg / flower).
    if (nameOf(c, existing) == n) {
        if (n.ends_with("_slab")) {
            r.state = with(c, with(c, existing, "type", "double"), "waterlogged", "false");
            return r;
        }
        if (n == "minecraft:snow") {
            r.state = with(c, existing, "layers", std::to_string(std::min(8, intProp(c, existing, "layers") + 1)));
            return r;
        }
        for (std::string_view key : {"candles", "pickles", "eggs", "flower_amount", "segment_amount"})
            if (!reg.property(existing, key).empty()) {
                r.state = with(c, existing, key, std::to_string(std::min(4, intProp(c, existing, key) + 1)));
                return r;
            }
    }

    // StandingAndWallBlockItem: the first of the nearest looking directions with a form that survives there.
    int attachment = kDown;
    if (const std::string wall = wallVariant(n, attachment); !wall.empty() && reg.defaultState(wall)) {
        const std::array<int, 6> dirs = nearestLookingDirections(c, replaceClicked);
        u16 wallState = 0;  // the wall block's own getStateForPlacement: the first horizontal direction that survives
        for (int d : dirs) {
            if (!horizontal(d)) continue;
            if (n.ends_with("_hanging_sign") && axisOf(d) == axisOf(c.face)) continue;  // WallHangingSignBlock: across the face
            const u16 w = with(c, reg.defaultState(wall), "facing", kName[opposite(d)]);
            if (n.ends_with("_skull") || n.ends_with("_head")) {  // WallSkullBlock: against anything not replaceable
                if (!reg.infos()[at(c, pos, d)].replaceable) {
                    wallState = w;
                    break;
                }
            } else if (survives(c, w, pos)) {
                wallState = w;
                break;
            }
        }
        u16 standing = held;
        if (!reg.property(held, "rotation").empty()) {  // signs / banners face away (+180), skulls the player's yaw
            const float deg = c.yaw * 180.0f / kPi + ((n.ends_with("_skull") || n.ends_with("_head")) ? 0.0f : 180.0f);
            standing = with(c, held, "rotation", std::to_string(int(std::floor(deg / 22.5f + 0.5f)) & 15));
        }
        if (n.ends_with("_hanging_sign")) {  // CeilingHangingSignBlock: under a full face it snaps to the 4 directions
            const bool attached = !sturdy(reg, at(c, pos, kUp), kDown) || c.sneaking;
            static constexpr int kSegment[6] = {0, 0, 8, 0, 4, 12};  // RotationSegment of a horizontal direction
            const int rotation = attached ? int(std::floor((c.yaw * 180.0f / kPi + 180.0f) / 22.5f + 0.5f)) & 15 : kSegment[opposite(horiz)];
            standing = with(c, with(c, held, "rotation", std::to_string(rotation)), "attached", attached ? "true" : "false");
        }
        u16 chosen = 0;
        for (int d : dirs) {
            if (d == opposite(attachment)) continue;
            const u16 candidate = d == attachment ? standing : wallState;
            if (candidate && survives(c, candidate, pos)) {
                chosen = candidate;
                break;
            }
        }
        if (!chosen) return r;
        s = chosen;
    } else if (isDoor(c, held)) {  // DoorBlock: the lower half facing the player's direction, the hinge by the sides
        if (pos[1] >= c.maxY || !reg.infos()[at(c, pos, kUp)].replaceable) return r;
        const int left = counterClockWise(horiz), right = clockWise(horiz);
        const int above[3] = {pos[0], pos[1] + 1, pos[2]};
        const u16 ls = at(c, pos, left), las = at(c, above, left), rs = at(c, pos, right), ras = at(c, above, right);
        const int balance = (fullCollision(reg, ls) ? -1 : 0) + (fullCollision(reg, las) ? -1 : 0) + (fullCollision(reg, rs) ? 1 : 0) +
                            (fullCollision(reg, ras) ? 1 : 0);
        const bool doorLeft = isDoor(c, ls) && reg.property(ls, "half") == "lower";
        const bool doorRight = isDoor(c, rs) && reg.property(rs, "half") == "lower";
        std::string_view hinge;
        if ((doorLeft && !doorRight) || balance > 0) hinge = "right";
        else if ((doorRight && !doorLeft) || balance < 0) hinge = "left";
        else {
            const double cx = c.hit[0] - pos[0], cz = c.hit[2] - pos[2];
            const int sx = kStep[horiz][0], sz = kStep[horiz][2];
            hinge = ((sx >= 0 || !(cz < 0.5)) && (sx <= 0 || !(cz > 0.5)) && (sz >= 0 || !(cx > 0.5)) && (sz <= 0 || !(cx < 0.5))) ? "left"
                                                                                                                              : "right";
        }
        s = with(c, with(c, with(c, held, "facing", kName[horiz]), "hinge", hinge), "half", "lower");
        r.extra.push_back({pos[0], pos[1] + 1, pos[2], int(with(c, s, "half", "upper"))});  // setPlacedBy
    } else if (n.ends_with("_bed") && !reg.property(held, "part").empty()) {  // BedBlock: the head one block ahead
        const int head[3] = {pos[0] + kStep[horiz][0], pos[1], pos[2] + kStep[horiz][2]};
        if (!reg.infos()[c.block(head[0], head[1], head[2])].replaceable) return r;
        s = with(c, with(c, held, "facing", kName[horiz]), "part", "foot");
        r.extra.push_back({head[0], head[1], head[2], int(with(c, s, "part", "head"))});
    } else if (reg.property(held, "half") == "lower" || reg.property(held, "half") == "upper") {  // DoublePlantBlock
        if (pos[1] >= c.maxY || !reg.infos()[at(c, pos, kUp)].replaceable) return r;
        if (!reg.property(held, "facing").empty()) s = with(c, s, "facing", kName[opposite(horiz)]);  // small dripleaf
        s = with(c, s, "half", "lower");
        u16 top = with(c, s, "half", "upper");
        top = with(c, top, "waterlogged", c.waterSource && c.waterSource(pos[0], pos[1] + 1, pos[2]) ? "true" : "false");
        r.extra.push_back({pos[0], pos[1] + 1, pos[2], int(top)});
    } else if (!reg.property(held, "face").empty()) {  // FaceAttachedHorizontalDirectionalBlock: buttons, levers, grindstone
        u16 chosen = 0;
        for (int d : nearestLookingDirections(c, replaceClicked)) {
            u16 cand;
            int support;
            if (!horizontal(d)) {
                cand = with(c, with(c, held, "face", d == kUp ? "ceiling" : "floor"), "facing", kName[horiz]);
                support = d;
            } else {
                cand = with(c, with(c, held, "face", "wall"), "facing", kName[opposite(d)]);
                support = d;
            }
            // canAttach: a sturdy face towards it (the grindstone stands anywhere)
            if (n == "minecraft:grindstone" || sturdy(reg, at(c, pos, support), opposite(support))) {
                chosen = cand;
                break;
            }
        }
        if (!chosen) return r;
        s = chosen;
    } else if (!reg.property(held, "hanging").empty() && n.ends_with("lantern")) {  // LanternBlock: hanging when looking up
        u16 chosen = 0;
        for (int d : nearestLookingDirections(c, replaceClicked)) {
            if (horizontal(d)) continue;
            const u16 cand = with(c, held, "hanging", d == kUp ? "true" : "false");
            if (supportsCenter(reg, at(c, pos, d), opposite(d))) {
                chosen = cand;
                break;
            }
        }
        if (!chosen) return r;
        s = chosen;
    } else if (n == "minecraft:ladder") {  // LadderBlock: against the first sturdy wall looked at
        if (!replaceClicked) {
            const u16 behind = at(c, pos, opposite(c.face));
            if (nameOf(c, behind) == n && faceByName(reg.property(behind, "facing")) == c.face) return r;
        }
        u16 chosen = 0;
        for (int d : nearestLookingDirections(c, replaceClicked)) {
            if (!horizontal(d)) continue;
            if (sturdy(reg, at(c, pos, d), opposite(d))) {
                chosen = with(c, held, "facing", kName[opposite(d)]);
                break;
            }
        }
        if (!chosen) return r;
        s = chosen;
    } else if (n.ends_with("trapdoor")) {  // TrapDoorBlock: against a side face, else facing the player
        if (!replaceClicked && horizontal(c.face))
            s = with(c, with(c, held, "facing", kName[c.face]), "half", c.hit[1] - pos[1] > 0.5 ? "top" : "bottom");
        else
            s = with(c, with(c, held, "facing", kName[opposite(horiz)]), "half", c.face == kUp ? "bottom" : "top");
    } else if (n == "minecraft:bell") {  // BellBlock: floor / ceiling by the clicked face, else the wall(s) beside
        if (!horizontal(c.face)) {
            s = with(c, with(c, held, "attachment", c.face == kDown ? "ceiling" : "floor"), "facing", kName[horiz]);
        } else {
            const int f = opposite(c.face);
            const bool both = sturdy(reg, at(c, pos, clockWise(f)), counterClockWise(f)) &&
                              sturdy(reg, at(c, pos, counterClockWise(f)), clockWise(f));
            s = with(c, with(c, held, "facing", kName[f]), "attachment", both ? "double_wall" : "single_wall");
        }
    } else if (n == "minecraft:crafter") {  // CrafterBlock: front against the look, top up (or by the yaw)
        const int front = opposite(orderedByNearest(c)[0]);
        const int top = front == kDown ? opposite(horiz) : front == kUp ? horiz : kUp;
        s = with(c, held, "orientation", std::string(kName[front]) + "_" + kName[top]);
    } else if (!reg.property(held, "axis").empty()) {  // RotatedPillarBlock and the like: the clicked face's axis
        s = with(c, held, "axis", c.face < 2 ? "y" : c.face < 4 ? "z" : "x");
    } else if (!reg.property(held, "facing").empty()) {
        const std::string_view current = reg.property(held, "facing");
        const bool sixWay = current == "up" || current == "down" ||
                            n == "minecraft:piston" || n == "minecraft:sticky_piston" || n == "minecraft:dispenser" ||
                            n == "minecraft:dropper" || n == "minecraft:observer" || n == "minecraft:barrel" || n.ends_with("command_block") ||
                            n.ends_with("shulker_box") || n == "minecraft:end_rod" || n.ends_with("lightning_rod") || n == "minecraft:hopper" ||
                            n.ends_with("amethyst_bud") || n == "minecraft:amethyst_cluster";
        int f;
        if (n == "minecraft:observer") f = orderedByNearest(c)[0];  // looking at it: the face towards the player... away
        else if (n == "minecraft:piston" || n == "minecraft:sticky_piston" || n == "minecraft:dispenser" || n == "minecraft:dropper" ||
                 n == "minecraft:barrel" || n.ends_with("command_block"))
            f = opposite(orderedByNearest(c)[0]);  // getNearestLookingDirection().getOpposite()
        else if (n.ends_with("shulker_box") || n.ends_with("lightning_rod") || n.ends_with("amethyst_bud") || n == "minecraft:amethyst_cluster")
            f = c.face;  // the clicked face
        else if (n == "minecraft:end_rod") {  // EndRodBlock: opposite an end rod it continues
            const u16 behind = at(c, pos, opposite(c.face));
            f = nameOf(c, behind) == n && faceByName(reg.property(behind, "facing")) == c.face ? opposite(c.face) : c.face;
        } else if (n == "minecraft:hopper") {  // HopperBlock: the clicked face's opposite, down from above or below
            f = horizontal(c.face) ? opposite(c.face) : kDown;
        } else if (sixWay) {
            f = opposite(orderedByNearest(c)[0]);
        } else if (n.ends_with("anvil")) {
            f = clockWise(horiz);  // AnvilBlock
        } else if (isStairs(n) || n.ends_with("_fence_gate") || n == "minecraft:campfire" || n == "minecraft:soul_campfire" ||
                   n == "minecraft:decorated_pot" || n == "minecraft:calibrated_sculk_sensor") {
            f = horiz;  // the direction the player looks
        } else {
            f = opposite(horiz);  // furnaces, chests, pumpkins, repeaters...: towards the player
        }
        s = with(c, held, "facing", kName[f]);
        if (isStairs(n)) s = with(c, s, "half", upperClick ? "top" : "bottom");
    }
    // Slabs: top or bottom by the click height.
    const std::string_view type = reg.property(s, "type");
    if (n.ends_with("_slab") && (type == "top" || type == "bottom")) s = with(c, s, "type", upperClick ? "top" : "bottom");
    // ChestBlock: paired with a chest beside facing the same way (sneaking: with the clicked chest)
    if (isChest(n)) {
        int facing = faceByName(reg.property(s, "facing"));
        std::string_view chestType = "single";
        auto partnerFacing = [&](int dir) -> int {
            const u16 nb = at(c, pos, dir);
            return nameOf(c, nb) == n && reg.property(nb, "type") == "single" ? faceByName(reg.property(nb, "facing")) : -1;
        };
        if (horizontal(c.face) && c.sneaking) {
            const int nf = partnerFacing(opposite(c.face));
            if (nf >= 2 && axisOf(nf) != axisOf(c.face)) {
                facing = nf;
                chestType = counterClockWise(facing) == opposite(c.face) ? "right" : "left";
            }
        }
        if (chestType == "single" && !c.sneaking)
            chestType = facing == partnerFacing(clockWise(facing)) ? "left" : facing == partnerFacing(counterClockWise(facing)) ? "right" : "single";
        s = with(c, with(c, s, "facing", kName[facing]), "type", chestType);
    }
    // Connections and shapes that getStateForPlacement computes from the neighbours.
    if (isStairs(n)) s = with(c, s, "shape", stairsShape(c, s, pos));
    if (isWire(n)) {  // RedStoneWireBlock.getStateForPlacement (canSurvive: on a sturdy top or a hopper)
        if (!wireSurvivesOn(c, at(c, pos, kDown))) return r;
        s = wireConnectionState(c, wireCrossState(c, s), pos);
    }
    if (isWall(n) && !reg.property(s, "up").empty()) {
        bool connected[6] = {};
        for (int d = 2; d < 6; ++d) connected[d] = wallConnects(c, at(c, pos, d), opposite(d));
        s = wallShape(c, s, pos, connected);
    }
    if (isFence(n) || isBars(n)) {
        for (int d = 2; d < 6; ++d) {
            const u16 nb = at(c, pos, d);
            const bool connect = isFence(n) ? fenceConnects(c, n, nb, opposite(d)) : barsAttach(c, nb, opposite(d));
            s = with(c, s, kName[d], connect ? "true" : "false");
        }
    }
    if (n == "minecraft:brown_mushroom_block" || n == "minecraft:red_mushroom_block" || n == "minecraft:mushroom_stem") {
        for (int d = 0; d < 6; ++d)  // HugeMushroomBlock: a face shows unless the same block is there
            s = with(c, s, kName[d], nameOf(c, at(c, pos, d)) == n ? "false" : "true");
    }
    if (n == "minecraft:chorus_plant") {  // ChorusPlantBlock.getStateWithConnections
        for (int d = 0; d < 6; ++d) {
            const std::string nb = nameOf(c, at(c, pos, d));
            s = with(c, s, kName[d], nb == n || nb == "minecraft:chorus_flower" || (d == kDown && nb == "minecraft:end_stone") ? "true" : "false");
        }
    }
    if (n == "minecraft:tripwire")  // TripWireBlock.shouldConnectTo
        for (int d = 2; d < 6; ++d) s = with(c, s, kName[d], tripwireConnects(c, at(c, pos, d), d) ? "true" : "false");
    if (!reg.property(s, "waterlogged").empty()) s = with(c, s, "waterlogged", water ? "true" : "false");
    r.state = s;
    return r;
}

void neighbourShapes(const Context& c, int x, int y, int z, u16 old, std::vector<std::array<int, 4>>& out) {
    BlockRegistry& reg = c.reg;
    const int p[3] = {x, y, z};
    const u16 self = c.block(x, y, z);
    const std::string selfName = nameOf(c, self);
    for (int d = 0; d < 6; ++d) {
        const int q[3] = {x + kStep[d][0], y + kStep[d][1], z + kStep[d][2]};
        const u16 nb = c.block(q[0], q[1], q[2]);
        const std::string nn = nameOf(c, nb);
        u16 updated = nb;
        if (horizontal(d) && (isFence(nn) || isBars(nn))) {  // FenceBlock / IronBarsBlock.updateShape: the side facing us
            const bool connect = isFence(nn) ? fenceConnects(c, nn, self, d) : barsAttach(c, self, d);
            updated = with(c, nb, kName[opposite(d)], connect ? "true" : "false");
        } else if (isWire(nn)) {
            updated = wireUpdate(c, nb, q, opposite(d));
        } else if (nn == "minecraft:brown_mushroom_block" || nn == "minecraft:red_mushroom_block" || nn == "minecraft:mushroom_stem") {
            if (selfName == nn) updated = with(c, nb, kName[opposite(d)], "false");  // HugeMushroomBlock.updateShape
        } else if (nn == "minecraft:chorus_plant") {  // ChorusPlantBlock.updateShape (while it survives)
            const int toUs = opposite(d);
            const bool connect = selfName == nn || selfName == "minecraft:chorus_flower" || (toUs == kDown && selfName == "minecraft:end_stone");
            updated = with(c, nb, kName[toUs], connect ? "true" : "false");
        } else if (nn == "minecraft:tripwire" && horizontal(d)) {  // TripWireBlock.updateShape
            updated = with(c, nb, kName[opposite(d)], tripwireConnects(c, self, opposite(d)) ? "true" : "false");
        } else if (isWall(nn) && d != kUp && !reg.property(nb, "up").empty()) {
            // WallBlock.updateShape: sideUpdate (the side facing us) or topUpdate (we are above it)
            bool connected[6] = {};
            for (int e = 2; e < 6; ++e) connected[e] = reg.property(nb, kName[e]) != "none";
            if (horizontal(d)) connected[opposite(d)] = wallConnects(c, self, d);
            updated = wallShape(c, nb, q, connected);
        } else if (horizontal(d) && isStairs(nn)) {  // StairBlock.updateShape
            updated = with(c, nb, "shape", stairsShape(c, nb, q));
        } else if (horizontal(d) && isChest(nn) && nn == selfName && reg.property(nb, "type") == "single") {
            // ChestBlock.updateShape: a single chest joins the new chest beside it that points at it
            const std::string_view t = reg.property(self, "type");
            const int sf = faceByName(reg.property(self, "facing"));
            if (t != "single" && sf == faceByName(reg.property(nb, "facing"))) {
                const int connected = t == "left" ? clockWise(sf) : counterClockWise(sf);
                if (connected == d) updated = with(c, nb, "type", t == "left" ? "right" : "left");
            }
        }
        // DoorBlock / DoublePlantBlock / BedBlock.updateShape: a half whose other half was here and is gone goes too
        const std::string_view half = reg.property(nb, "half");
        if (!horizontal(d) && (half == "lower" || half == "upper") && (half == "upper") == (d == kUp)) {
            const std::string_view selfHalf = reg.property(self, "half");
            if (selfName != nn || selfHalf == half || selfHalf.empty()) updated = 0;
        }
        if (horizontal(d) && nn.ends_with("_bed") && !reg.property(nb, "part").empty()) {
            const int f = faceByName(reg.property(nb, "facing"));
            const std::string_view part = reg.property(nb, "part");
            const int partner = part == "foot" ? f : opposite(f);
            if (partner == opposite(d) && (selfName != nn || reg.property(self, "part") == part)) updated = 0;
        }
        if (updated != nb) out.push_back({q[0], q[1], q[2], int(updated)});
    }
    // RedStoneWireBlock.updateIndirectNeighbourShapes of the old and the new state: the wires one step down or up
    // beyond each connected side
    for (const u16 wire : {old, self}) {
        if (!isWire(nameOf(c, wire))) continue;
        for (int d = 2; d < 6; ++d) {
            if (!wireConnected(c, wire, d) || isWire(nameOf(c, at(c, p, d)))) continue;
            for (const int dy : {-1, 1}) {
                const int q[3] = {x + kStep[d][0], y + dy, z + kStep[d][2]};
                const u16 nb = c.block(q[0], q[1], q[2]);
                if (!isWire(nameOf(c, nb))) continue;
                const u16 updated = wireUpdate(c, nb, q, opposite(d));
                if (updated != nb) out.push_back({q[0], q[1], q[2], int(updated)});
            }
        }
    }
}

}  // namespace place
