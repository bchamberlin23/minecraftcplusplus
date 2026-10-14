#include "game/player.h"

#include "assets/assets.h"
#include "game/item_rules.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>

namespace {
std::string_view shortBlock(const std::string& state) {
    std::string_view n = state;
    n = n.substr(0, n.find('['));
    if (n.starts_with("minecraft:")) n.remove_prefix(10);
    return n;
}
constexpr int kStep[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
}  // namespace

u8 Physics::specialKind(u16 id) const {
    if (special_.size() <= id) special_.resize(size_t(id) + 1, 0);
    if (!special_[id]) {
        const std::string& n = registry_.name(id);
        special_[id] = n.starts_with("minecraft:scaffolding") ? 2 : n.starts_with("minecraft:powder_snow") ? 3 : 1;
    }
    return special_[id];
}

u8 Physics::tagBits(u16 id) const {
    if (tags_.size() <= id) tags_.resize(size_t(id) + 1, 0);
    if (!tags_[id]) {
        const std::string& state = registry_.name(id);
        const std::string block = state.substr(0, state.find('['));
        const std::string_view n = shortBlock(state);
        u8 bits = 1;
        if (rules && rules->loaded()) {
            if (rules->inTag("minecraft:climbable", block)) bits |= 2;
            if (rules->inTag("minecraft:blocks_fluid_flow", block)) bits |= 4;
            if (rules->inTag("minecraft:suppresses_bounce", block)) bits |= 8;
            if (rules->inTag("minecraft:can_glide_through", block)) bits |= 16;
        } else {
            if (n == "ladder" || n == "vine" || n == "scaffolding" || n == "weeping_vines" || n == "weeping_vines_plant" ||
                n == "twisting_vines" || n == "twisting_vines_plant" || n == "cave_vines" || n == "cave_vines_plant")
                bits |= 2;
            if (registry_.infos()[id].collisionCount || n.ends_with("_sign")) bits |= 4;
            if (n == "honey_block") bits |= 8;
            if (n.find("vines") != std::string_view::npos || n == "vine") bits |= 16;
        }
        tags_[id] = bits;
    }
    return tags_[id];
}

void Physics::addBoxes(u16 state, double x, double y, double z, const AABB& area, std::vector<AABB>& out) const {
    const BlockInfo& b = registry_.infos()[state];
    float off[3];  // BlockState.getOffset (bamboo, pointed dripstone)
    BlockRegistry::offset(b, i32(std::floor(x)), i32(std::floor(z)), off);
    x += off[0], y += off[1], z += off[2];
    for (u32 i = 0; i < b.collisionCount; ++i) {
        const Box& s = registry_.box(b.collisionFirst + i);
        const AABB a{{x + s.min[0], y + s.min[1], z + s.min[2]}, {x + s.max[0], y + s.max[1], z + s.max[2]}};
        if (a.intersects(area)) out.push_back(a);
    }
}

// PistonMovingBlockEntity.getCollisionShape: the retracting source piston's extended base, and the moved block (or the
// source piston's head) where the move has got to; nothing of the moving block while it pushes the entity (NOCLIP).
void Physics::pistonShape(const Piston& p, std::vector<AABB>& out) const {
    const AABB all{{-1e9, -1e9, -1e9}, {1e9, 1e9, 1e9}};
    if (!p.extending && p.source && p.extendedBase) addBoxes(p.extendedBase, p.x, p.y, p.z, all, out);
    const int movement = p.extending ? p.dir : (p.dir ^ 1);
    if (p.progress < 1.0f && context.pistonNoClip == movement) return;
    const u16 state = p.source ? p.head[(p.extending != (1.0f - p.progress < 0.25f)) ? 1 : 0] : p.moved;
    const double e = p.extending ? p.progress - 1.0 : 1.0 - p.progress;  // getExtendedProgress
    addBoxes(state, p.x + kStep[p.dir][0] * e, p.y + kStep[p.dir][1] * e, p.z + kStep[p.dir][2] * e, all, out);
}

void Physics::cellBoxes(int x, int y, int z, const AABB& area, std::vector<AABB>& out) const {
    auto add = [&](double x0_, double y0_, double z0_, double x1_, double y1_, double z1_) {
        const AABB a{{x + x0_, y + y0_, z + z0_}, {x + x1_, y + y1_, z + z1_}};
        if (a.intersects(area)) out.push_back(a);
    };
    const u16 id = world_.block(x, y, z);
    if (context.set && id) {
        const u8 kind = specialKind(id);
        if (kind == 2) {  // ScaffoldingBlock.getCollisionShape for the player
            if (context.feetY > y + 1.0 - 1e-5 && !context.descending) {  // SHAPE_STABLE: top and legs
                add(0, 0.875, 0, 1, 1, 1);
                add(0, 0, 0, 0.125, 1, 0.125), add(0.875, 0, 0, 1, 1, 0.125);
                add(0, 0, 0.875, 0.125, 1, 1), add(0.875, 0, 0.875, 1, 1, 1);
            } else if (registry_.property(id, "distance") != "0" && registry_.property(id, "bottom") == "true" &&
                       context.feetY > y - 1e-5) {
                add(0, 0, 0, 1, 0.125, 1);  // SHAPE_UNSTABLE_BOTTOM
            }
            return;
        }
        if (kind == 3) {  // PowderSnowBlock.getCollisionShape
            if (context.fallDistance > 2.5) add(0, 0, 0, 1, 0.9, 1);
            else if (context.powderSnowWalker && context.feetY > y + 1.0 - 1e-5 && !context.descending) add(0, 0, 0, 1, 1, 1);
            return;
        }
    }
    addBoxes(id, x, y, z, area, out);
}

bool Physics::supportingBlock(const AABB& area, const double pos[3], int out[3]) const {
    double best = 1e300;
    bool found = false;
    std::vector<AABB> boxes;
    for (int y = int(std::floor(area.min[1])) - 1; y <= int(std::floor(area.max[1])); ++y)
        for (int z = int(std::floor(area.min[2])); z <= int(std::floor(area.max[2])); ++z)
            for (int x = int(std::floor(area.min[0])); x <= int(std::floor(area.max[0])); ++x) {
                boxes.clear();
                cellBoxes(x, y, z, area, boxes);
                if (boxes.empty()) continue;
                const double dx = x + 0.5 - pos[0], dy = y + 0.5 - pos[1], dz = z + 0.5 - pos[2];
                const double d = dx * dx + dy * dy + dz * dz;  // BlockPos.distToCenterSqr; ties: the larger position
                const bool larger = found && (y != out[1] ? y > out[1] : z != out[2] ? z > out[2] : x > out[0]);
                if (d < best || (d == best && (!found || larger))) best = d, found = true, out[0] = x, out[1] = y, out[2] = z;
            }
    return found;
}

void Physics::collisionBoxes(const AABB& area, std::vector<AABB>& out) const {
    out.clear();
    const int x0 = int(std::floor(area.min[0])), x1 = int(std::floor(area.max[0]));
    const int y0 = int(std::floor(area.min[1])) - 1, y1 = int(std::floor(area.max[1]));  // -1: tall shapes (fences)
    const int z0 = int(std::floor(area.min[2])), z1 = int(std::floor(area.max[2]));
    for (int y = y0; y <= y1; ++y)
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) cellBoxes(x, y, z, area, out);
    // Moving pistons (MovingPistonBlock.getCollisionShape: the block entity's shape, which reaches into the next cell).
    if (!pistons.empty()) {
        std::vector<AABB> shape;
        for (const Piston& p : pistons) {
            if (p.x < x0 - 1 || p.x > x1 + 1 || p.y < y0 - 1 || p.y > y1 + 1 || p.z < z0 - 1 || p.z > z1 + 1) continue;
            shape.clear();
            pistonShape(p, shape);
            for (const AABB& a : shape)
                if (a.intersects(area)) out.push_back(a);
        }
    }
    if (context.set && entityColliders) entityColliders(area, out);
    if (context.set && border &&
        border->insideCloseToBorder(context.x, context.z, area.max[0] - area.min[0], area.max[2] - area.min[2])) {
        // WorldBorder.getCollisionShape: INFINITY minus the box floor(min) .. ceil(max); as slabs cut to the area.
        const double lo[2] = {std::floor(border->minX()), std::floor(border->minZ())};
        const double hi[2] = {std::ceil(border->maxX()), std::ceil(border->maxZ())};
        for (int side = 0; side < 4; ++side) {
            AABB a{{area.min[0] - 1, area.min[1] - 1, area.min[2] - 1}, {area.max[0] + 1, area.max[1] + 1, area.max[2] + 1}};
            const int axis = side < 2 ? 0 : 2, k = side < 2 ? 0 : 1;
            if (side & 1) a.min[axis] = std::max(a.min[axis], hi[k]);
            else a.max[axis] = std::min(a.max[axis], lo[k]);
            if (a.min[axis] < a.max[axis] && a.intersects(area)) out.push_back(a);
        }
    }
}

// --- Fluids (FlowingFluid / FluidState) ---------------------------------------------------------------------------

Physics::Fluid Physics::fluid(int x, int y, int z) const {
    const u16 id = world_.block(x, y, z);
    if (fluids_.size() <= id) fluids_.resize(size_t(id) + 1, 0);
    u16& f = fluids_[id];
    if (!f) {
        const BlockInfo& b = registry_.infos()[id];
        u16 v = 0x8000;
        if (b.fluid) {  // water / lava blocks: level 0 source, 1..7 flowing (amount 8 - level), 8.. falling (amount 8)
            const bool lava = registry_.name(id).starts_with("minecraft:lava");
            const std::string_view l = registry_.property(id, "level");
            int level = 0;
            for (char c : l) level = level * 10 + (c - '0');
            const int amount = level == 0 || level >= 8 ? 8 : 8 - level;
            v |= u16((lava ? 2 : 1) << 8) | (level == 0 ? 0x40 : 0) | (level >= 8 ? 0x20 : 0) | u16(amount);
        } else if (b.waterlogged) {  // waterlogged blocks, kelp, seagrass, bubble columns: a water source
            v |= u16(1 << 8) | 0x40 | 8;
        }
        f = v;
    }
    return {u8((f >> 8) & 3), u8(f & 15), (f & 0x20) != 0, (f & 0x40) != 0};
}

double Physics::fluidHeight(int x, int y, int z, const Fluid& f) const {
    return fluid(x, y + 1, z).type == f.type ? 1.0 : double(float(f.amount) / 9.0f);
}

double Physics::fluidHeightForCamera(int x, int y, int z, const Fluid& f) const {
    if (f.source && solidFace(x, y + 1, z, 0, 0)) return 1.0;  // a source under a sturdy bottom face fills the cell
    return fluidHeight(x, y, z, f);
}

// FlowingFluid.isSolidFace (and BlockState.isFaceSturdy for direction `dir`, from the collision boxes): never the
// same fluid, never ice.
bool Physics::solidFace(int x, int y, int z, int dir, u8 fluidType) const {
    if (fluidType && fluid(x, y, z).type == fluidType) return false;
    if (dir == 1) return true;
    const u16 id = world_.block(x, y, z);
    const std::string_view n = shortBlock(registry_.name(id));
    if (n == "ice" || n == "frosted_ice") return false;
    const BlockInfo& b = registry_.infos()[id];
    const int axis = dir < 2 ? 1 : dir < 4 ? 2 : 0, a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
    const float side = (dir & 1) ? 1.0f : 0.0f;
    for (u32 i = 0; i < b.collisionCount; ++i) {
        const Box& s = registry_.box(b.collisionFirst + i);
        if ((side == 1.0f ? s.max[axis] >= 1.0f - 1e-6f : s.min[axis] <= 1e-6f) && s.min[a1] <= 1e-6f && s.max[a1] >= 1.0f - 1e-6f &&
            s.min[a2] <= 1e-6f && s.max[a2] >= 1.0f - 1e-6f)
            return true;
    }
    return false;
}

void Physics::fluidFlow(int x, int y, int z, const Fluid& f, double out[3]) const {
    double fx = 0, fz = 0;
    const float own = float(f.amount) / 9.0f;
    static constexpr int kHorizontal[4] = {2, 5, 3, 4};  // Direction.Plane.HORIZONTAL: north, east, south, west
    for (int d : kHorizontal) {
        const int nx = x + kStep[d][0], nz = z + kStep[d][2];
        const Fluid n = fluid(nx, y, nz);
        if (n.type && n.type != f.type) continue;  // affectsFlow: empty or the same fluid
        float neighbour = n.type ? float(n.amount) / 9.0f : 0.0f, distance = 0.0f;
        if (neighbour == 0.0f) {
            if (!(tagBits(world_.block(nx, y, nz)) & 4)) {  // not #blocks_fluid_flow: the fluid below the neighbour
                const Fluid below = fluid(nx, y - 1, nz);
                if (!below.type || below.type == f.type) {
                    neighbour = below.type ? float(below.amount) / 9.0f : 0.0f;
                    if (neighbour > 0.0f) distance = own - (neighbour - 0.8888889f);
                }
            }
        } else if (neighbour > 0.0f) {
            distance = own - neighbour;
        }
        if (distance != 0.0f) fx += kStep[d][0] * double(distance), fz += kStep[d][2] * double(distance);
    }
    double v[3] = {fx, 0, fz};
    auto normalize = [](double a[3]) {
        const double l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (l < 1e-5) a[0] = a[1] = a[2] = 0;
        else a[0] /= l, a[1] /= l, a[2] /= l;
    };
    if (f.falling)
        for (int d : kHorizontal) {
            const int nx = x + kStep[d][0], nz = z + kStep[d][2];
            if (solidFace(nx, y, nz, d, f.type) || solidFace(nx, y + 1, nz, d, f.type)) {
                normalize(v);
                v[1] -= 6.0;
                break;
            }
        }
    normalize(v);
    std::copy(v, v + 3, out);
}

bool Physics::inFluid(const AABB& box) const {
    for (int y = int(std::floor(box.min[1])); y <= int(std::ceil(box.max[1])) - 1; ++y)
        for (int z = int(std::floor(box.min[2])); z <= int(std::ceil(box.max[2])) - 1; ++z)
            for (int x = int(std::floor(box.min[0])); x <= int(std::ceil(box.max[0])) - 1; ++x) {
                const Fluid f = fluid(x, y, z);
                if (f.type && y + fluidHeight(x, y, z, f) >= box.min[1]) return true;
            }
    return false;
}

bool Physics::inLava(const AABB& box) const {
    for (int y = int(std::floor(box.min[1])); y <= int(std::ceil(box.max[1])) - 1; ++y)
        for (int z = int(std::floor(box.min[2])); z <= int(std::ceil(box.max[2])) - 1; ++z)
            for (int x = int(std::floor(box.min[0])); x <= int(std::ceil(box.max[0])) - 1; ++x) {
                const Fluid f = fluid(x, y, z);
                if (f.type == 2 && y + fluidHeight(x, y, z, f) >= box.min[1]) return true;
            }
    return false;
}

bool Physics::containsLiquid(const AABB& box) const {
    const BlockInfo* infos = registry_.infos();
    for (int y = int(std::floor(box.min[1])); y <= int(std::floor(box.max[1] - 1e-7)); ++y)
        for (int z = int(std::floor(box.min[2])); z <= int(std::floor(box.max[2] - 1e-7)); ++z)
            for (int x = int(std::floor(box.min[0])); x <= int(std::floor(box.max[0] - 1e-7)); ++x)
                if (infos[world_.block(x, y, z)].fluid || infos[world_.block(x, y, z)].waterlogged) return true;
    return false;
}

bool Physics::noCollision(const AABB& box) const {
    std::vector<AABB> boxes;
    collisionBoxes(box, boxes);
    return std::none_of(boxes.begin(), boxes.end(), [&](const AABB& o) { return o.intersects(box); });
}

bool Physics::climbable(int x, int y, int z) const { return (tagBits(world_.block(x, y, z)) & 2) != 0; }

float Physics::speedFactor(int x, int y, int z) const {
    const std::string_view n = shortBlock(registry_.name(world_.block(x, y, z)));
    return n == "soul_sand" || n == "honey_block" ? 0.4f : 1.0f;
}

float Physics::jumpFactor(int x, int y, int z) const {
    return shortBlock(registry_.name(world_.block(x, y, z))) == "honey_block" ? 0.5f : 1.0f;
}

// BlockBehaviour.Properties.bounceRestitution: slime blocks 1, beds and shelf mushrooms 0.75.
float Physics::bounceRestitution(int x, int y, int z) const {
    const std::string_view n = shortBlock(registry_.name(world_.block(x, y, z)));
    if (n == "slime_block") return 1.0f;
    return (n.ends_with("_bed") && n != "straw_bed") || n == "shelf_mushroom" ? 0.75f : 0.0f;  // the BED collection
}

bool Physics::canGlideThrough(int x, int y, int z) const { return (tagBits(world_.block(x, y, z)) & 16) != 0; }

bool Physics::suppressesBounce(int x, int y, int z) const { return (tagBits(world_.block(x, y, z)) & 8) != 0; }

bool Physics::isSuffocating(int x, int y, int z) const {
    const u16 id = world_.block(x, y, z);
    const BlockInfo& b = registry_.infos()[id];
    if (!b.occludes || b.collisionCount != 1) return false;
    const Box& s = registry_.box(b.collisionFirst);
    return s.min[0] <= 0 && s.min[1] <= 0 && s.min[2] <= 0 && s.max[0] >= 1 && s.max[1] >= 1 && s.max[2] >= 1;
}

bool Physics::stuckMultiplier(const AABB& b, double out[3], bool weaving) const {
    // Each block's entityInside calls makeStuckInBlock in BlockPos.betweenClosed order (x fastest, then y, then z):
    // the last one wins. Powder snow holds a living entity only while its own block (getInBlockState) is powder snow.
    bool any = false;
    const double cx = (b.min[0] + b.max[0]) / 2, cz = (b.min[2] + b.max[2]) / 2;
    const bool feetInSnow = shortBlock(registry_.name(world_.block(int(std::floor(cx)), int(std::floor(b.min[1])),
                                                                   int(std::floor(cz))))) == "powder_snow";
    for (int z = int(std::floor(b.min[2] + 1e-5)); z <= int(std::floor(b.max[2] - 1e-5)); ++z)
        for (int y = int(std::floor(b.min[1] + 1e-5)); y <= int(std::floor(b.max[1] - 1e-5)); ++y)
            for (int x = int(std::floor(b.min[0] + 1e-5)); x <= int(std::floor(b.max[0] - 1e-5)); ++x) {
                const std::string_view n = shortBlock(registry_.name(world_.block(x, y, z)));
                if (n == "cobweb")  // WebBlock.entityInside
                    out[0] = weaving ? 0.5 : 0.25, out[1] = weaving ? 0.25 : 0.05, out[2] = out[0], any = true;
                else if (n == "sweet_berry_bush") out[0] = 0.8, out[1] = 0.75, out[2] = 0.8, any = true;
                else if (n == "powder_snow" && feetInSnow) out[0] = 0.9, out[1] = 1.5, out[2] = 0.9, any = true;
            }
    return any;
}

float Physics::slipperiness(int x, int y, int z) const {
    const std::string& n = registry_.name(world_.block(x, y, z));
    if (n.find("blue_ice") != std::string::npos) return 0.989f;
    if (n.find("ice") != std::string::npos) return 0.98f;
    if (n.find("slime_block") != std::string::npos) return 0.8f;
    return 0.6f;
}

// Amanatides-Woo voxel walk, testing each cell's selection boxes with a slab test.
RayHit Physics::raycast(const double o[3], const double d[3], double maxDistance, bool visual, bool collider) const {
    RayHit best;
    const BlockInfo* infos = registry_.infos();
    int cell[3] = {int(std::floor(o[0])), int(std::floor(o[1])), int(std::floor(o[2]))};
    int step[3];
    double tMax[3], tDelta[3];
    for (int a = 0; a < 3; ++a) {
        step[a] = d[a] > 0 ? 1 : -1;
        tDelta[a] = d[a] != 0 ? std::abs(1.0 / d[a]) : 1e30;
        const double boundary = d[a] > 0 ? cell[a] + 1 - o[a] : o[a] - cell[a];
        tMax[a] = d[a] != 0 ? boundary * tDelta[a] : 1e30;
    }
    // VoxelShape.clip: a ray starting inside a shape (tested a thousandth of the way along) hits it from inside, on
    // the face opposite the nearest direction of the ray.
    const double test[3] = {o[0] + d[0] * maxDistance * 0.001, o[1] + d[1] * maxDistance * 0.001, o[2] + d[2] * maxDistance * 0.001};
    for (int i = 0; i < 64; ++i) {
        const BlockInfo& b = infos[world_.block(cell[0], cell[1], cell[2])];
        float off[3];  // the shapes move with BlockState.getOffset (flowers, grass...)
        BlockRegistry::offset(b, cell[0], cell[2], off);
        const double at[3] = {cell[0] + off[0], cell[1] + off[1], cell[2] + off[2]};
        u32 first = b.selectionFirst, count = b.selectionCount;
        bool fullBlock = false;
        if (holdingScaffolding && !visual && !collider && specialKind(world_.block(cell[0], cell[1], cell[2])) == 2) fullBlock = true, count = 1;
        if (collider) first = b.collisionFirst, count = b.collisionCount;
        if (visual) {  // BlockBehaviour.getVisualShape: the collision shape, with its block overrides
            const u16 id = world_.block(cell[0], cell[1], cell[2]);
            if (visual_.size() <= id) visual_.resize(size_t(id) + 1, 0);
            u8& kind = visual_[id];
            if (!kind) {
                const std::string& n = registry_.name(id);
                const std::string_view s = std::string_view(n).substr(0, n.find('['));
                kind = s == "minecraft:glass" || s.ends_with("_stained_glass") || s == "minecraft:tinted_glass" ||
                               s.ends_with("copper_grate") || s.ends_with("_bars") || s.ends_with("glass_pane") ||
                               s == "minecraft:powder_snow"
                           ? 3  // TransparentBlock, IronBarsBlock, PowderSnowBlock: none
                       : s.ends_with("_fence") || s == "minecraft:snow" ? 2  // FenceBlock, SnowLayerBlock: the outline
                       : s == "minecraft:mud" || s == "minecraft:soul_sand" ? 4  // MudBlock, SoulSandBlock: a full block
                                                                            : 1;
            }
            if (kind == 1) first = b.collisionFirst, count = b.collisionCount;
            else if (kind == 3) count = 0;
            else if (kind == 4) fullBlock = true, count = 1;
        }
        static const Box kFullBlock{{0, 0, 0}, {1, 1, 1}};
        for (u32 k = 0; k < count; ++k) {
            const Box& s = fullBlock ? kFullBlock : registry_.box(first + k);
            bool inside = true;
            for (int a = 0; a < 3; ++a) inside = inside && test[a] > at[a] + s.min[a] && test[a] < at[a] + s.max[a];
            if (inside) {
                const int axis = std::abs(d[0]) >= std::abs(d[1]) && std::abs(d[0]) >= std::abs(d[2]) ? 0 : std::abs(d[1]) >= std::abs(d[2]) ? 1 : 2;
                const int toward = d[axis] > 0 ? 1 : 0;  // Direction.getApproximateNearest, then its opposite
                const int face = axis == 1 ? (toward ? 0 : 1) : axis == 2 ? (toward ? 2 : 3) : (toward ? 4 : 5);
                best = {true, cell[0], cell[1], cell[2], face, maxDistance * 0.001, {test[0], test[1], test[2]}, true};
                return best;
            }
            double tNear = 0, tFar = maxDistance;
            int face = -1;
            bool miss = false;
            for (int a = 0; a < 3 && !miss; ++a) {
                const double lo = at[a] + s.min[a], hi = at[a] + s.max[a];
                if (d[a] == 0) {
                    miss = o[a] < lo || o[a] > hi;
                    continue;
                }
                double t0 = (lo - o[a]) / d[a], t1 = (hi - o[a]) / d[a];
                int f = d[a] > 0 ? 0 : 1;  // entered through the min (0) or max (1) side
                if (t0 > t1) std::swap(t0, t1);
                if (t0 > tNear) {
                    tNear = t0;
                    // axis 1 = y: faces 0/1, axis 2 = z: faces 2/3, axis 0 = x: faces 4/5
                    face = a == 1 ? f : a == 2 ? 2 + f : 4 + f;
                }
                tFar = std::min(tFar, t1);
                miss = tNear > tFar;
            }
            if (!miss && face >= 0 && (!best.hit || tNear < best.distance)) {
                best = {true, cell[0], cell[1], cell[2], face, tNear, {o[0] + d[0] * tNear, o[1] + d[1] * tNear, o[2] + d[2] * tNear}};
            }
        }
        if (best.hit) return best;
        const int a = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
        if (tMax[a] > maxDistance) break;
        cell[a] += step[a];
        tMax[a] += tDelta[a];
    }
    return best;
}

void Player::moveRelative(const PlayerInput& input, double speed) {
    double f = input.forward, s = input.strafe;
    const double len = f * f + s * s;
    if (len < 1e-7) return;
    if (len > 1.0) {
        const double inv = 1.0 / std::sqrt(len);
        f *= inv, s *= inv;
    }
    f *= speed, s *= speed;
    const double sy = std::sin(yaw), cy = std::cos(yaw);
    vel[0] += s * cy - f * sy;
    vel[2] += f * cy + s * sy;
}

namespace {
// Clips movement along `axis` against boxes overlapping `b` on the other two axes.
double clip(const std::vector<AABB>& boxes, const AABB& b, int axis, double delta) {
    const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
    for (const AABB& o : boxes) {
        if (o.max[a1] <= b.min[a1] || o.min[a1] >= b.max[a1] || o.max[a2] <= b.min[a2] || o.min[a2] >= b.max[a2])
            continue;
        if (delta > 0 && o.min[axis] >= b.max[axis] - 1e-7)
            delta = std::min(delta, o.min[axis] - b.max[axis]);
        else if (delta < 0 && o.max[axis] <= b.min[axis] + 1e-7)
            delta = std::max(delta, o.max[axis] - b.min[axis]);
    }
    return delta;
}

void offset(AABB& b, int axis, double d) {
    b.min[axis] += d;
    b.max[axis] += d;
}

// Moves a box through the boxes: Y first, then the larger horizontal axis, like the game.
void collide(const std::vector<AABB>& boxes, AABB& b, double out[3], const double in[3]) {
    out[1] = clip(boxes, b, 1, in[1]);
    offset(b, 1, out[1]);
    const int first = std::abs(in[0]) < std::abs(in[2]) ? 2 : 0, second = first == 0 ? 2 : 0;
    out[first] = clip(boxes, b, first, in[first]);
    offset(b, first, out[first]);
    out[second] = clip(boxes, b, second, in[second]);
    offset(b, second, out[second]);
}
}  // namespace

void Physics::collideBoxes(const std::vector<AABB>& boxes, AABB& b, double out[3], const double in[3]) { collide(boxes, b, out, in); }

// Player.canFallAtLeast: nothing below the box (moved by dx, dz) within minHeight.
bool Player::canFallAtLeast(const Physics& physics, double dx, double dz, double minHeight) const {
    const AABB b = box();
    const AABB area{{b.min[0] + 1e-7 + dx, b.min[1] - minHeight - 1e-7, b.min[2] + 1e-7 + dz},
                    {b.max[0] - 1e-7 + dx, b.min[1], b.max[2] - 1e-7 + dz}};
    std::vector<AABB> boxes;
    physics.collisionBoxes(area, boxes);
    return std::none_of(boxes.begin(), boxes.end(), [&](const AABB& o) { return o.intersects(area); });
}

// Entity.move: collisions (Entity.collide with the step up), the collision flags, the supporting block, the fall
// distance, restituteMovementAfterCollisions (slime blocks and beds bounce) and the block speed factor.
void Player::move(const Physics& physics, double dx, double dy, double dz, bool piston) {
    if (piston) {  // Entity.limitPistonMovement: one axis, at most 0.51 per tick and axis
        if (dx * dx + dy * dy + dz * dz <= 1e-7) return;
        auto restrict = [&](int a, double amount) {
            const double r = std::clamp(amount + pistonDeltas_[a], -0.51, 0.51);
            amount = r - pistonDeltas_[a];
            pistonDeltas_[a] = r;
            return std::abs(amount) <= double(1.0E-5F) ? 0.0 : amount;
        };
        if (dx != 0) dx = restrict(0, dx), dy = dz = 0;
        else if (dy != 0) dy = restrict(1, dy), dz = 0;
        else dz = restrict(2, dz);
        if (dx == 0 && dy == 0 && dz == 0) return;
    }
    if (stuck_[0] * stuck_[0] + stuck_[1] * stuck_[1] + stuck_[2] * stuck_[2] > 1e-7) {  // in a cobweb / bush
        if (!piston) dx *= stuck_[0], dy *= stuck_[1], dz *= stuck_[2];
        std::fill(stuck_, stuck_ + 3, 0.0);
        std::fill(vel, vel + 3, 0.0);
    }
    // Player.maybeBackOffFromEdge: holding sneak on the ground never walks off an edge higher than a step.
    const double maxDown = attr.stepHeight;
    if (!piston && !flying && dy <= 0 && lastInput.sneak &&
        (onGround || (fallDistance < maxDown && !canFallAtLeast(physics, 0, 0, maxDown - fallDistance)))) {
        constexpr double kStep = 0.05;
        const double sx = dx > 0 ? kStep : dx < 0 ? -kStep : 0, sz = dz > 0 ? kStep : dz < 0 ? -kStep : 0;
        while (dx != 0 && canFallAtLeast(physics, dx, 0, maxDown)) {
            if (std::abs(dx) <= kStep) {
                dx = 0;
                break;
            }
            dx -= sx;
        }
        while (dz != 0 && canFallAtLeast(physics, 0, dz, maxDown)) {
            if (std::abs(dz) <= kStep) {
                dz = 0;
                break;
            }
            dz -= sz;
        }
        while (dx != 0 && dz != 0 && canFallAtLeast(physics, dx, dz, maxDown)) {
            dx = std::abs(dx) <= kStep ? 0 : dx - sx;
            dz = std::abs(dz) <= kStep ? 0 : dz - sz;
        }
    }
    const double in[3] = {dx, dy, dz};
    physics.context.x = pos[0], physics.context.z = pos[2];
    AABB start = box();
    AABB area = start;
    for (int a = 0; a < 3; ++a) (in[a] < 0 ? area.min[a] : area.max[a]) += in[a];
    area.max[1] += attr.stepHeight;  // room for stepping up
    std::vector<AABB> boxes;
    physics.collisionBoxes(area, boxes);

    double out[3];
    AABB b = start;
    collide(boxes, b, out, in);

    // Entity.collide: blocked sideways on (or onto) the ground, try the step heights the colliders offer (their
    // bottoms and tops above the feet, up to the step height) from the lowest; the first that goes further wins.
    const bool blockedH = out[0] != dx || out[2] != dz;
    const bool groundAfter = out[1] != dy && dy < 0;
    if (attr.stepHeight > 0 && (groundAfter || onGround) && blockedH) {
        AABB grounded = start;
        if (groundAfter) grounded.min[1] += out[1], grounded.max[1] += out[1];
        const float maxStep = float(attr.stepHeight), skip = float(out[1]);
        std::vector<float> candidates;
        for (const AABB& c : boxes)
            for (const double coord : {c.min[1], c.max[1]}) {
                const float rel = float(coord - grounded.min[1]);
                if (rel >= 0.0f && rel != skip && rel <= maxStep) candidates.push_back(rel);
            }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        for (const float h : candidates) {
            AABB s = grounded;
            const double mv[3] = {dx, double(h), dz};
            double stepOut[3];
            collide(boxes, s, stepOut, mv);
            if (stepOut[0] * stepOut[0] + stepOut[2] * stepOut[2] > out[0] * out[0] + out[2] * out[2]) {
                const double toGround = start.min[1] - grounded.min[1];
                b = s;
                out[0] = stepOut[0], out[1] = stepOut[1] - toGround, out[2] = stepOut[2];
                break;
            }
        }
    }

    pos[0] = (b.min[0] + b.max[0]) / 2;
    pos[1] = b.min[1];
    pos[2] = (b.min[2] + b.max[2]) / 2;
    const bool xCollision = std::abs(dx - out[0]) >= 1e-5, zCollision = std::abs(dz - out[2]) >= 1e-5;  // !Mth.equal
    horizontalCollision = xCollision || zCollision;
    const bool verticalCollision = dy != out[1], below = verticalCollision && dy < 0;
    onGround = below;
    // Entity.checkSupportingBlock: the block under the feet closest to the centre (or where the move came from).
    if (onGround) {
        const AABB bb = box();
        AABB t{{bb.min[0], bb.min[1] - 1e-6, bb.min[2]}, {bb.max[0], bb.min[1], bb.max[2]}};
        bool found = physics.supportingBlock(t, pos, support_);
        if (!found && !onGroundNoBlocks_) {
            for (int a : {0, 2}) t.min[a] -= out[a], t.max[a] -= out[a];
            found = physics.supportingBlock(t, pos, support_);
        }
        hasSupport_ = found;
        onGroundNoBlocks_ = !found;
    } else {
        onGroundNoBlocks_ = false;
        hasSupport_ = false;
    }
    minorHorizontalCollision_ = false;
    if (horizontalCollision) {  // LocalPlayer.isHorizontalCollisionMinor: moved within 8 degrees of the input
        const double sy = std::sin(yaw), cy = std::cos(yaw);
        const double gx = xxa_ * cy - zza_ * sy, gz = zza_ * cy + xxa_ * sy;
        const double in2 = gx * gx + gz * gz, mv2 = out[0] * out[0] + out[2] * out[2];
        if (in2 >= 1e-5 && mv2 >= 1e-5)
            minorHorizontalCollision_ = std::acos((gx * out[0] + gz * out[2]) / std::sqrt(in2 * mv2)) < 0.13962634;
    }
    moved_[0] = out[0], moved_[1] = out[1], moved_[2] = out[2];
    int effect[3];
    onPos(physics, 0.2, effect);  // getOnPosLegacy
    // Entity.checkFallDamage: the fall distance (out of water), reset on landing.
    if (!inWater && out[1] < 0) fallDistance -= out[1];
    if (onGround && fallDistance > 0) landedFall = fallDistance, ++landings;  // fallOn(... fallDistance)
    if (onGround) fallDistance = 0;
    // Entity.restituteMovementAfterCollisions: blocked axes stop (players do not bounce off walls); landing on a
    // bouncy block (not while sneaking, not off a honey block) bounces back up minus the gravity of the part of the
    // tick that was left.
    if (verticalCollision || horizontalCollision) {
        const bool suppress = lastInput.sneak;  // Entity.isSuppressingBounce: the sneak key
        double restitution = suppress ? 0.0 : attr.bounciness;
        double next[3] = {vel[0], vel[1], vel[2]};
        if (xCollision) next[0] = -vel[0] * restitution;
        if (zCollision) next[2] = -vel[2] * restitution;
        if (verticalCollision) {
            if (below)
                restitution = !(-vel[1] <= gravityNow()) && !suppress && !physics.suppressesBounce(effect[0], effect[1], effect[2])
                                  ? std::max(restitution, double(physics.bounceRestitution(effect[0], effect[1], effect[2])))
                                  : 0.0;
            double compensation = 0, drag = 1;
            if (restitution > 0) {
                const double portion = out[1] / vel[1];
                compensation = portion * gravityNow();
                drag = 1.0 + portion * (frictionModified(0.98f, attr.airDragModifier) - 1.0);  // Mth.lerp
            }
            next[1] = (compensation - vel[1]) * drag * restitution;
        }
        std::copy(next, next + 3, vel);
    }
    // Entity.getBlockSpeedFactor (LivingEntity: towards 1 by movement_efficiency): the block at the feet, else the one
    // below (soul sand, honey); water and bubble columns keep their own.
    // (Player: 1 while flying or gliding)
    const float f = flying || fallFlying ? 1.0f : blockSpeedFactor(physics);
    vel[0] *= f, vel[2] *= f;
}

// Entity.getOnPos: the supporting block's column at y - offset (a fence below within half a block, walls and fence
// gates: that block itself), or the block under the position.
void Player::onPos(const Physics& physics, double offset, int out[3]) const {
    if (hasSupport_) {
        std::copy(support_, support_ + 3, out);
        if (offset <= 1e-5) return;
        const std::string_view n = shortBlock(physics.registry().name(physics.world().block(support_[0], support_[1], support_[2])));
        const bool fence = n.ends_with("_fence") && !n.ends_with("_fence_gate"), wall = n.ends_with("_wall"), gate = n.ends_with("_fence_gate");
        if (!((offset <= 0.5 && fence) || wall || gate)) out[1] = int(std::floor(pos[1] - offset));
        return;
    }
    out[0] = int(std::floor(pos[0])), out[1] = int(std::floor(pos[1] - offset)), out[2] = int(std::floor(pos[2]));
}

// Player.updatePlayerPose (the end of Player.tick): the desired pose (swimming, gliding, the riptide spin,
// crouching with the sneak key) if the body fits, else crouching, else crawling (swimming under a one-block gap).
bool Player::fits(const Physics& physics, Pose p) const {
    AABB b = boxAt(pos, p);
    for (int a = 0; a < 3; ++a) b.min[a] += 1e-7, b.max[a] -= 1e-7;
    return physics.noCollision(b);
}

void Player::updatePose(const Physics& physics) {
    if (noClip) {
        pose = Pose::Standing;
        return;
    }
    if (!fits(physics, Pose::Swimming)) return;
    const Pose desired = swimming ? Pose::Swimming : fallFlying ? Pose::FallFlying : spinFlag ? Pose::SpinAttack
                         : (lastInput.sneak && !flying) ? Pose::Crouching : Pose::Standing;
    pose = fits(physics, desired) ? desired : fits(physics, Pose::Crouching) ? Pose::Crouching : Pose::Swimming;
}
// LocalPlayer.updateAutoJump: walking on the ground into an obstacle a jump can clear (more than half a block, up to
// 1.2 plus Jump Boost) with room above jumps on the next tick.
void Player::updateAutoJump(const Physics& physics, double xa, double za, const PlayerInput& raw, double speed) {
    const bool moving = raw.forward != 0 || raw.strafe != 0;
    const int bx = int(std::floor(pos[0])), bz = int(std::floor(pos[2]));
    if (!autoJump || autoJumpTime_ > 0 || !onGround || raw.sneak || !moving || blockJumpFactor(physics) < 1.0f)
        return;
    const double sy = std::sin(yaw), cy = std::cos(yaw);
    double mx = xa, mz = za;
    float distSq = float(mx * mx + mz * mz);
    if (distSq <= 0.001f) {  // blocked already: the direction the input pushes
        const double ix = speed * raw.strafe, iz = speed * raw.forward;
        mx = ix * cy - iz * sy, mz = iz * cy + ix * sy;
        distSq = float(mx * mx + mz * mz);
        if (distSq <= 0.001f) return;
    }
    const float inv = 1.0f / std::sqrt(distSq);
    const double dx = mx * inv, dz = mz * inv;
    const double fx = -sy * std::cos(pitch), fz = cy * std::cos(pitch);  // Entity.getForward
    if (float(fx * dx + fz * dz) < -0.15f) return;
    // A cell blocks when a collision box of it (or reaching into it) overlaps it.
    auto solid = [&](int x, int y, int z, double* top) {
        std::vector<AABB> boxes;
        const AABB cell{{double(x), double(y), double(z)}, {x + 1.0, y + 1.0, z + 1.0}};
        physics.collisionBoxes(cell, boxes);
        bool any = false;
        for (const AABB& b : boxes)
            if (b.intersects(cell)) {
                if (top) *top = any ? std::max(*top, b.max[1]) : b.max[1];
                any = true;
            }
        return any;
    };
    int ceilY = int(std::floor(box().max[1]));
    if (solid(bx, ceilY, bz, nullptr) || solid(bx, ceilY + 1, bz, nullptr)) return;
    ++ceilY;
    float jumpHeight = 1.2f;
    if (attr.jumpBoost >= 0) jumpHeight += float(attr.jumpBoost + 1) * 0.75f;
    const double ahead = std::max(speed * 7.0, 1.0 / double(inv));
    const double b0[3] = {pos[0], pos[1], pos[2]};
    const double e0[3] = {pos[0] + xa + dx * ahead, pos[1], pos[2] + za + dz * ahead};
    AABB test{{std::min(b0[0], e0[0]) - width(), b0[1], std::min(b0[2], e0[2]) - width()},
              {std::max(b0[0], e0[0]) + width(), b0[1] + height(pose), std::max(b0[2], e0[2]) + width()}};
    // Two segments half a body width left and right of the path, 0.51 above the feet.
    const double rx = -dz * width() * 0.5, rz = dx * width() * 0.5;  // moveDir x up, scaled
    auto segBox = [&](double side) {
        const double ax = b0[0] + rx * side, az = b0[2] + rz * side, ex = e0[0] + rx * side, ez = e0[2] + rz * side;
        const double y = b0[1] + 0.51;
        return AABB{{std::min(ax, ex), y, std::min(az, ez)}, {std::max(ax, ex), y, std::max(az, ez)}};
    };
    auto touches = [](const AABB& a, const AABB& seg) {  // AABB.intersects(Vec3, Vec3)
        for (int i = 0; i < 3; ++i)
            if (!(a.min[i] < seg.max[i] && a.max[i] > seg.min[i])) return false;
        return true;
    };
    const AABB left = segBox(-1), right = segBox(1);
    std::vector<AABB> boxes;
    physics.collisionBoxes(test, boxes);
    bool found = false;
    double obstacleHeight = 0;
    for (const AABB& b : boxes) {
        if (!b.intersects(test) || !(touches(b, left) || touches(b, right))) continue;
        found = true;
        obstacleHeight = b.max[1];
        const int ox = int(std::floor((b.min[0] + b.max[0]) / 2)), oy = int(std::floor((b.min[1] + b.max[1]) / 2)),
                  oz = int(std::floor((b.min[2] + b.max[2]) / 2));
        for (int steps = 1; float(steps) < jumpHeight; ++steps) {
            double top;
            if (solid(ox, oy + steps, oz, &top)) {
                obstacleHeight = top;
                if (obstacleHeight - pos[1] > jumpHeight) return;
            }
            if (steps > 1 && solid(bx, ++ceilY, bz, nullptr)) return;
        }
        break;
    }
    if (!found) return;
    const float ydelta = float(obstacleHeight - pos[1]);
    if (ydelta > 0.5f && ydelta <= jumpHeight) autoJumpTime_ = 1;
}

double Player::gravityNow() const {  // LivingEntity.getEffectiveGravity: Slow Falling caps it while falling
    return vel[1] <= 0 && attr.slowFalling >= 0 ? std::min(attr.gravity, 0.01) : attr.gravity;
}

// Entity.getBlockJumpFactor / getBlockSpeedFactor (LivingEntity: lerped towards 1 by movement_efficiency): the block at
// the feet, else the one under them that affects the movement.
float Player::blockJumpFactor(const Physics& physics) const {
    const float here = physics.jumpFactor(int(std::floor(pos[0])), int(std::floor(pos[1])), int(std::floor(pos[2])));
    int below[3];
    onPos(physics, 0.500001, below);
    return here == 1.0f ? physics.jumpFactor(below[0], below[1], below[2]) : here;
}

float Player::blockSpeedFactor(const Physics& physics) const {
    const int bx = int(std::floor(pos[0])), by = int(std::floor(pos[1])), bz = int(std::floor(pos[2]));
    float f = physics.speedFactor(bx, by, bz);
    const std::string& n = physics.registry().name(physics.world().block(bx, by, bz));
    if (f == 1.0f && !n.starts_with("minecraft:water") && !n.starts_with("minecraft:bubble_column")) {
        int below[3];
        onPos(physics, 0.500001, below);
        f = physics.speedFactor(below[0], below[1], below[2]);
    }
    return f + (1.0f - f) * float(attr.movementEfficiency);
}

// EntityFluidInteraction.update / Entity.updateFluidInteraction: the water and lava heights over the feet in the
// box, the eyes in water (to the fluid's surface as the camera sees it), and the currents (FlowingFluid.getFlow,
// averaged for players) pushing the player unless it flies.
void Player::updateFluids(const Physics& physics) {
    waterHeight = lavaHeight = 0;
    eyeInWater = false;
    const AABB full = box();
    AABB b = full;
    for (int a = 0; a < 3; ++a) b.min[a] += 0.001, b.max[a] -= 0.001;
    const double eyeY = pos[1] + eyeHeight();
    const int ex = int(std::floor(pos[0])), ez = int(std::floor(pos[2]));
    const bool pushed = !flying;
    double current[2][3] = {}, currentHeight[2] = {};
    int count[2] = {};
    for (int x = int(std::floor(b.min[0])); x <= int(std::ceil(b.max[0])) - 1; ++x)
        for (int y = int(std::floor(b.min[1])); y <= int(std::ceil(b.max[1])) - 1; ++y)
            for (int z = int(std::floor(b.min[2])); z <= int(std::ceil(b.max[2])) - 1; ++z) {
                const Physics::Fluid f = physics.fluid(x, y, z);
                if (!f.type) continue;
                const double top = y + physics.fluidHeight(x, y, z, f);
                if (top < b.min[1]) continue;
                const int t = f.type - 1;
                if (t == 0 && x == ex && z == ez && eyeY >= y && eyeY <= y + physics.fluidHeightForCamera(x, y, z, f))
                    eyeInWater = true;
                double& height = t == 0 ? waterHeight : lavaHeight;
                height = std::max(top - full.min[1], height);
                if (!pushed) continue;
                double flow[3];
                physics.fluidFlow(x, y, z, f, flow);
                currentHeight[t] = std::max(height, currentHeight[t]);
                if (currentHeight[t] < 0.4)
                    for (double& v : flow) v *= currentHeight[t];
                for (int a = 0; a < 3; ++a) current[t][a] += flow[a];
                ++count[t];
            }
    inWater = waterHeight > 0;
    inLava = lavaHeight > 0;
    if (!pushed) return;
    for (int t = 0; t < 2; ++t) {  // CurrentAccumulator.applyTo
        const double* c = current[t];
        if (!count[t] || c[0] * c[0] + c[1] * c[1] + c[2] * c[2] < double(1.0E-5F)) continue;
        const double scale = t == 0 ? 0.014 : fastLava ? 0.007 : 0.0023333333333333335;
        double impulse[3] = {c[0] / count[t] * scale, c[1] / count[t] * scale, c[2] / count[t] * scale};
        const double len = std::sqrt(impulse[0] * impulse[0] + impulse[1] * impulse[1] + impulse[2] * impulse[2]);
        if (std::abs(vel[0]) < 0.003 && std::abs(vel[2]) < 0.003 && len < 0.0045000000000000005 && len > 0)
            for (double& v : impulse) v = v / len * 0.0045000000000000005;
        for (int a = 0; a < 3; ++a) vel[a] += impulse[a];
    }
}

// LocalPlayer.moveTowardsClosestSpace: a corner of the body inside a suffocating block nudges the player out sideways
// towards the nearest free neighbour.
void Player::moveTowardsClosestSpace(const Physics& physics, double x, double z) {
    const AABB b = box();
    auto suffocates = [&](int px, int pz) {
        for (int y = int(std::floor(b.min[1] + 1e-7)); y <= int(std::floor(b.max[1] - 1e-7)); ++y)
            if (physics.isSuffocating(px, y, pz)) return true;
        return false;
    };
    const int px = int(std::floor(x)), pz = int(std::floor(z));
    if (!suffocates(px, pz)) return;
    const double xd = x - px, zd = z - pz;
    static constexpr int kDirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};  // west, east, north, south
    int best = -1;
    double closest = 1e300;
    for (int i = 0; i < 4; ++i) {
        const double axis = kDirs[i][0] != 0 ? xd : zd;
        const double toEdge = (kDirs[i][0] + kDirs[i][1]) > 0 ? 1.0 - axis : axis;
        if (toEdge < closest && !suffocates(px + kDirs[i][0], pz + kDirs[i][1])) closest = toEdge, best = i;
    }
    if (best < 0) return;
    if (kDirs[best][0] != 0) vel[0] = 0.1 * kDirs[best][0];
    else vel[2] = 0.1 * kDirs[best][1];
}

// Entity.applyEffectsFromBlocks after the travel: Block.stepOn of the block under the feet (slime blocks slow a
// walk), then the blocks the (slightly shrunk) box is inside (cobwebs, berry bushes and powder snow hold the player,
// bubble columns lift or drag it, honey block walls slow a fall to a slide).
void Player::applyEffectsFromBlocks(const Physics& physics) {
    if (onGround) {
        int at[3];
        onPos(physics, 0.2, at);
        if (shortBlock(physics.registry().name(physics.world().block(at[0], at[1], at[2]))) == "slime_block") {
            const double absY = std::abs(vel[1]);  // SlimeBlock.stepOn
            if (absY < 0.1 && !lastInput.sneak) {
                const double scale = 0.4 + absY * 0.2;
                vel[0] *= scale, vel[2] *= scale;
            }
        }
    }
    AABB b = box();
    for (int a = 0; a < 3; ++a) b.min[a] += 1e-5, b.max[a] -= 1e-5;
    double stuck[3];
    if (physics.stuckMultiplier(b, stuck, attr.weaving)) {  // Entity.makeStuckInBlock
        std::copy(stuck, stuck + 3, stuck_);
        fallDistance = 0;
    }
    isInPowderSnow_ = false;
    bool honeyed = false;
    slidingOnHoney = false;
    for (int y = int(std::floor(b.min[1])); y <= int(std::floor(b.max[1])); ++y)
        for (int z = int(std::floor(b.min[2])); z <= int(std::floor(b.max[2])); ++z)
            for (int x = int(std::floor(b.min[0])); x <= int(std::floor(b.max[0])); ++x) {
                const u16 id = physics.world().block(x, y, z);
                const std::string_view n = shortBlock(physics.registry().name(id));
                if (n == "powder_snow") isInPowderSnow_ = true;
                if (n == "bubble_column" && !flying) {  // BubbleColumnBlock.entityInside, every cell (Player: not flying)
                    const bool down = physics.registry().property(id, "drag") == "true";
                    const BlockInfo& a = physics.registry().infos()[physics.world().block(x, y + 1, z)];
                    if (!a.collisionCount && !a.fluid && !a.waterlogged) {  // Entity.onAboveBubbleColumn
                        vel[1] = down ? std::max(-0.9, vel[1] - 0.03) : std::min(1.8, vel[1] + 0.1);
                    } else {  // onInsideBubbleColumn (resets the fall)
                        vel[1] = down ? std::max(-0.3, vel[1] - 0.03) : std::min(0.7, vel[1] + 0.06);
                        fallDistance = 0;
                    }
                }
                if (n == "honey_block" && !honeyed) {  // HoneyBlock.isSlidingDown / doSlideMovement
                    const double old = vel[1] / double(0.98f) + 0.08;  // getOldDeltaY
                    const double dx = std::abs(x + 0.5 - pos[0]), dz = std::abs(z + 0.5 - pos[2]);
                    const double overlap = 0.4375 + width() / 2.0;
                    if (!onGround && pos[1] <= y + 0.9375 - 1e-7 && old < -0.08 && (dx + 1e-7 > overlap || dz + 1e-7 > overlap)) {
                        honeyed = true, slidingOnHoney = true;
                        if (old < -0.13) {
                            const double reduce = -0.05 / old;
                            vel[0] *= reduce, vel[2] *= reduce;
                        }
                        vel[1] = (-0.05 - 0.08) * double(0.98f);  // getNewDeltaY(-0.05)
                        fallDistance = 0;
                    }
                }
            }
}

// LivingEntity.travel and its parts (Player.travel: swimming steers with the look; flying keeps 0.6 of the
// vertical speed).
void Player::travel(const Physics& physics, const PlayerInput& input, bool jumping) {
    const int bx = int(std::floor(pos[0])), by = int(std::floor(pos[1])), bz = int(std::floor(pos[2]));
    if (swimming) {
        const double lookY = -std::sin(pitch);
        const double m = lookY < -0.2 ? 0.085 : 0.06;
        if (lookY <= 0 || jumping || physics.fluid(bx, int(std::floor(pos[1] + 1.0 - 0.1)), bz).type)
            vel[1] += (lookY - vel[1]) * m;
    }
    const double originalY = vel[1];
    const double gravity = gravityNow();
    const bool climbing = onClimbable(physics);
    auto travelInAir = [&] {
        int below[3];
        onPos(physics, 0.500001, below);
        const double friction = onGround ? frictionModified(physics.slipperiness(below[0], below[1], below[2]), attr.frictionModifier) : 1.0;
        // getFrictionInfluencedSpeed: the movement_speed attribute (with the sprint boost) on the ground, scaled for
        // slippery blocks; Player.getFlyingSpeed in the air.
        const double walk = attr.moveSpeed * (sprinting ? 1.3 : 1.0);
        const double speed = onGround ? (friction > 0.6 ? walk * (0.21600002 / (friction * friction * friction)) : walk)
                             : flying ? double(flyingSpeed) * (sprinting ? 2.0 : 1.0)
                                      : (sprinting ? double(0.025999999f) : 0.02);
        moveRelative(input, speed);
        if (climbing) {  // LivingEntity.handleOnClimbable
            fallDistance = 0;
            vel[0] = std::clamp(vel[0], -0.15, 0.15), vel[2] = std::clamp(vel[2], -0.15, 0.15);
            vel[1] = std::max(vel[1], -0.15);
            if (vel[1] < 0 && lastInput.sneak && shortBlock(physics.registry().name(physics.world().block(bx, by, bz))) != "scaffolding")
                vel[1] = 0;
        }
        const double beforeMove[2] = {pos[0], pos[2]};
        move(physics, vel[0], vel[1], vel[2]);
        updateAutoJump(physics, pos[0] - beforeMove[0], pos[2] - beforeMove[1], lastInput, walk);
        // after the move: onClimbable where the move ended (LivingEntity.travelInAir)
        if ((horizontalCollision || jumping) && (onClimbable(physics) || (wasInPowderSnow_ && attr.powderSnowWalker))) vel[1] = 0.2;
        double y = vel[1];
        if (attr.levitation >= 0) y += (0.05 * (attr.levitation + 1) - vel[1]) * 0.2;
        else y -= gravity;
        const double drag = frictionModified(0.91f, attr.airDragModifier);
        vel[0] *= friction * drag, vel[2] *= friction * drag;
        vel[1] = y * frictionModified(0.98f, attr.airDragModifier);
    };
    if (flying) {  // Player.isAffectedByFluids: never in fluids while flying
        travelInAir();
        vel[1] = originalY * 0.6;
        return;
    }
    auto jumpOutOfFluid = [&](double oldY) {
        if (!horizontalCollision) return;
        AABB b = box();
        const double d[3] = {vel[0], vel[1] + 0.6 - pos[1] + oldY, vel[2]};
        for (int a = 0; a < 3; ++a) b.min[a] += d[a], b.max[a] += d[a];
        if (physics.noCollision(b) && !physics.containsLiquid(b)) vel[1] = 0.3;
    };
    auto fluidFalling = [&](bool falling) {  // getFluidFallingAdjustedMovement (not while sprinting)
        if (gravity == 0 || sprinting) return;
        if (falling && std::abs(vel[1] - 0.005) >= 0.003 && std::abs(vel[1] - gravity / 16.0) < 0.003) vel[1] = -0.003;
        else vel[1] -= gravity / 16.0;
    };
    if (inWater || inLava) {  // LivingEntity.travelInFluid
        const bool falling = vel[1] <= 0;
        const double oldY = pos[1];
        if (inWater) {
            float slowDown = sprinting ? 0.9f : 0.8f;
            float speed = 0.02f;
            float walker = float(attr.waterEfficiency);
            if (!onGround) walker *= 0.5f;
            if (walker > 0) {
                slowDown += (0.54600006f - slowDown) * walker;
                speed += (float(attr.moveSpeed * (sprinting ? 1.3 : 1.0)) - speed) * walker;
            }
            if (attr.dolphinsGrace >= 0) slowDown = 0.96f;
            moveRelative(input, speed);
            move(physics, vel[0], vel[1], vel[2]);
            if (horizontalCollision && onClimbable(physics)) vel[1] = 0.2;  // onClimbable after the move
            vel[0] *= slowDown, vel[1] *= double(0.8f), vel[2] *= slowDown;
            fluidFalling(falling);
        } else {
            moveRelative(input, 0.02);
            move(physics, vel[0], vel[1], vel[2]);
            if (lavaHeight <= fluidJumpThreshold()) {  // isInShallowFluid(LAVA)
                vel[0] *= 0.5, vel[1] *= double(0.8f), vel[2] *= 0.5;
                fluidFalling(falling);
            } else {
                for (double& v : vel) v *= 0.5;
            }
            if (gravity != 0) vel[1] -= gravity / 4.0;
        }
        jumpOutOfFluid(oldY);
        return;
    }
    if (fallFlying && !climbing) {  // LivingEntity.travelFallFlying / updateFallFlyingMovement
        const double lx = -std::sin(yaw) * std::cos(pitch), ly = -std::sin(pitch), lz = std::cos(yaw) * std::cos(pitch);
        if (attr.rocketBoost) {  // FireworkRocketEntity.tick: pushes its glider along the look
            vel[0] += lx * 0.1 + (lx * 1.5 - vel[0]) * 0.5;
            vel[1] += ly * 0.1 + (ly * 1.5 - vel[1]) * 0.5;
            vel[2] += lz * 0.1 + (lz * 1.5 - vel[2]) * 0.5;
        }
        const double lean = pitch;
        const double lookHor = std::sqrt(lx * lx + lz * lz), moveHor = std::sqrt(vel[0] * vel[0] + vel[2] * vel[2]);
        const double lift = std::cos(lean) * std::cos(lean);
        vel[1] += gravity * (-1.0 + lift * 0.75);
        if (vel[1] < 0 && lookHor > 0) {
            const double convert = vel[1] * -0.1 * lift;
            vel[0] += lx * convert / lookHor, vel[1] += convert, vel[2] += lz * convert / lookHor;
        }
        if (lean < 0 && lookHor > 0) {
            const double convert = moveHor * -std::sin(lean) * 0.04;
            vel[0] += -lx * convert / lookHor, vel[1] += convert * 3.2, vel[2] += -lz * convert / lookHor;
        }
        if (lookHor > 0) {
            vel[0] += (lx / lookHor * moveHor - vel[0]) * 0.1;
            vel[2] += (lz / lookHor * moveHor - vel[2]) * 0.1;
        }
        vel[0] *= double(0.99f), vel[1] *= double(0.98f), vel[2] *= double(0.99f);
        move(physics, vel[0], vel[1], vel[2]);
        return;
    }
    travelInAir();
    if (fallFlying) fallFlying = false;  // gliding onto a climbable block stops (stopFallFlying)
}

// LivingEntity.onClimbable (Player: never while flying): #climbable at the feet, or an open trapdoor over a ladder
// facing the same way; gliding passes through #can_glide_through.
bool Player::onClimbable(const Physics& physics) const {
    if (flying || noClip) return false;
    const int x = int(std::floor(pos[0])), y = int(std::floor(pos[1])), z = int(std::floor(pos[2]));
    const u16 id = physics.world().block(x, y, z);
    if (fallFlying && physics.canGlideThrough(x, y, z)) return false;
    if (physics.climbable(x, y, z)) return true;
    if (shortBlock(physics.registry().name(id)).ends_with("_trapdoor") && physics.registry().property(id, "open") == "true") {
        const u16 below = physics.world().block(x, y - 1, z);
        return shortBlock(physics.registry().name(below)) == "ladder" &&
               physics.registry().property(below, "facing") == physics.registry().property(id, "facing");
    }
    return false;
}

double Player::fluidJumpThreshold() const { return eyeHeight() < 0.4 ? 0.0 : 0.4; }

void Player::tick(const Physics& physics, const PlayerInput& rawInput) {
    ++tickCount;
    std::copy(pos, pos + 3, prev);
    // Collision shapes that depend on the player (scaffolding, powder snow).
    physics.context = {true, pos[1], fallDistance, rawInput.sneak, attr.powderSnowWalker, pos[0], pos[2]};
    // Entity.baseTick: powder snow of the last tick, the eyes and body in fluids, swimming.
    wasInPowderSnow_ = isInPowderSnow_;
    wasEyeInWater = eyeInWater;
    updateFluids(physics);
    if (inWater) fallDistance = 0;
    const int bx = int(std::floor(pos[0])), by = int(std::floor(pos[1])), bz = int(std::floor(pos[2]));
    if (flying || noClip) swimming = false;  // Entity.updateSwimming (Player: never while flying)
    else if (swimming) swimming = sprinting && inWater;
    else swimming = sprinting && underWater() && physics.fluid(bx, by, bz).type == 1;
    if (inLava) fallDistance *= 0.5;

    // --- LocalPlayer.aiStep ---
    if (sprintTriggerTime_ > 0) --sprintTriggerTime_;
    const bool wasJumping = wasJumping_, wasShift = wasShift_, hadForwardImpulse = hadForwardImpulse_;
    // crouching: the sneak key (or no room to stand) with room to crouch, not flying or swimming.
    // (isShiftKeyDown: the keys as the last tick read them; input.tick comes after this)
    sneaking = !flying && !swimming && fits(physics, Pose::Crouching) && (lastInput.sneak || (!asleep && !fits(physics, Pose::Standing)));
    PlayerInput input = rawInput;
    lastInput = rawInput;
    bool wasAutoJump = false;
    if (autoJumpTime_ > 0) {  // the auto jump presses jump for this tick
        --autoJumpTime_;
        wasAutoJump = true;
        input.jump = true;
    }
    if (!noClip) {
        static constexpr double kCorners[4][2] = {{-0.35, 0.35}, {-0.35, -0.35}, {0.35, -0.35}, {0.35, 0.35}};
        for (const auto& c : kCorners) moveTowardsClosestSpace(physics, pos[0] + width() * c[0], pos[2] + width() * c[1]);
    }
    const bool forwardImpulse = input.forward > 1e-5f;  // ClientInput.hasForwardImpulse
    const bool crawling = pose == Pose::Swimming && !swimming;  // isVisuallyCrawling
    const bool movingSlowly = sneaking || crawling;
    auto sprintPossible = [&](bool allowedInShallowWater) {  // isSprintingPossible
        return !attr.blind && canSprint && (allowedInShallowWater || !(inWater && !underWater()));
    };
    // Sprinting: the sprint key, or forward pressed twice within the sprint window (sprintTriggerTime).
    if (wasShift || useBlocksSprint || rawInput.backward()) sprintTriggerTime_ = 0;
    const bool canStartSprinting = !sprinting && forwardImpulse && sprintPossible(flying) && !useBlocksSprint &&
                                   (!fallFlying || underWater()) && (!movingSlowly || underWater());
    if (canStartSprinting) {
        if (!hadForwardImpulse) {
            if (sprintTriggerTime_ > 0) sprinting = true;
            else sprintTriggerTime_ = sprintWindow;
        }
        if (rawInput.sprint) sprinting = true;
    }
    if (sprinting) {  // shouldStopSwimSprinting / shouldStopRunSprinting
        const bool stop = swimming ? (!sprintPossible(true) || !inWater || (!forwardImpulse && !onGround && !rawInput.sneak))
                                   : (!sprintPossible(flying) || !forwardImpulse || (horizontalCollision && !minorHorizontalCollision_));
        if (stop) sprinting = false;
    }
    // Flight: double-tapping jump within 7 ticks toggles it (spectators always fly).
    bool justToggledFlight = false;
    if (mayFly) {
        if (noClip) {
            if (!flying) flying = justToggledFlight = true;
        } else if (!wasJumping && input.jump && !wasAutoJump) {
            if (jumpTriggerTime_ == 0) {
                jumpTriggerTime_ = 7;
            } else if (!swimming) {
                flying = !flying;
                if (flying && onGround) jumpFromGround(physics);
                justToggledFlight = true;
                jumpTriggerTime_ = 0;
            }
        }
    }
    // Jump pressed in mid-air with a glider starts gliding (Player.tryToStartFallFlying).
    if (input.jump && !justToggledFlight && !wasJumping && !onClimbable(physics) && !fallFlying && !flying && !onGround &&
        attr.canGlide && attr.levitation < 0 && !inWater && !inLava) {  // !isInLiquid
        fallFlying = true;
        if (onStartFallFlying) onStartFallFlying();
    }
    if (inWater && rawInput.sneak && !flying) vel[1] -= double(0.04f);  // LivingEntity.goDownInWater
    // LocalPlayer.aiStep: flying up / down at 3x the ability's flying speed.
    if (flying) vel[1] += (int(input.jump) - int(rawInput.sneak)) * double(flyingSpeed) * 3.0;
    // Player.aiStep
    if (jumpTriggerTime_ > 0) --jumpTriggerTime_;
    if (flying) fallDistance = 0;
    wasJumping_ = input.jump, wasShift_ = rawInput.sneak, hadForwardImpulse_ = forwardImpulse;

    if (noClip) {  // spectators: always flying, no collisions (Entity.noPhysics)
        moveRelative(input, double(flyingSpeed) * (sprinting ? 2.0 : 1.0));
        for (int a = 0; a < 3; ++a) pos[a] += vel[a];
        onGround = horizontalCollision = false;
        vel[0] *= 0.91, vel[2] *= 0.91, vel[1] *= 0.6;
        pose = Pose::Standing;
        return;
    }

    // --- LivingEntity.aiStep ---
    if (noJumpDelay_ > 0) --noJumpDelay_;
    if (portalCooldown > 0) --portalCooldown;  // Entity.processPortalCooldown
    if (vel[0] * vel[0] + vel[2] * vel[2] < 9.0E-6) vel[0] = vel[2] = 0;  // players: the horizontal speed as a whole
    if (std::abs(vel[1]) < 0.003) vel[1] = 0;
    {  // LocalPlayer.applyInput / modifyInput: 0.98, the use slowdown, the sneaking speed, stretched to the unit square
        float f = input.forward, s = input.strafe;
        const float len = std::sqrt(f * f + s * s);
        if (len > 0) {
            f /= len, s /= len;
            float scale = 0.98f * useSpeed * (movingSlowly ? float(attr.sneakSpeed) : 1.0f);
            const float ax = std::abs(s), ay = std::abs(f);
            const float tan = ay > ax ? ax / ay : ay / ax;
            // KeyboardInput normalizes the move vector first: its length is 1 here, whatever the keys
            scale = std::min(scale * std::sqrt(1.0f + tan * tan), 1.0f);
            input.forward = f * scale, input.strafe = s * scale;
        }
        xxa_ = input.strafe, zza_ = input.forward;
    }
    // Jumping: from the ground (or shallow water), else up through water or lava (Player.isAffectedByFluids: not flying).
    if (input.jump && !flying) {
        const double fluidHeight = inLava ? lavaHeight : waterHeight;
        const bool waterWithHeight = inWater && fluidHeight > 0;
        const double threshold = fluidJumpThreshold();
        if (!waterWithHeight || (onGround && !(fluidHeight > threshold))) {
            if (!inLava || (onGround && lavaHeight <= threshold)) {
                if ((onGround || (waterWithHeight && fluidHeight <= threshold)) && noJumpDelay_ == 0) {
                    jumpFromGround(physics);
                    noJumpDelay_ = 10;
                }
            } else {
                vel[1] += double(0.04f);  // jumpInLiquid
            }
        } else {
            vel[1] += double(0.04f);
        }
    } else {
        noJumpDelay_ = 0;
    }
    // updateFallFlying while gliding: checkFallDistanceAccumulation; the end of the glide (on the ground, without a
    // glider, flying, levitating: canGlide) is the server's, through the shared flags.
    if (fallFlying && vel[1] > -0.5 && fallDistance > 1.0) fallDistance = 1.0;
    const AABB beforeTravel = box();
    if (attr.slowFalling >= 0 || attr.levitation >= 0) fallDistance = 0;
    travel(physics, input, input.jump);
    applyEffectsFromBlocks(physics);
    if (autoSpinAttackTicks > 0) {  // LivingEntity.checkAutoSpinAttack: the first living entity touched ends it
        --autoSpinAttackTicks;
        AABB swept = beforeTravel;
        const AABB now = box();
        for (int a = 0; a < 3; ++a) swept.min[a] = std::min(swept.min[a], now.min[a]), swept.max[a] = std::max(swept.max[a], now.max[a]);
        if (spinAttackHits && spinAttackHits(swept)) {
            autoSpinAttackTicks = 0;
            for (double& v : vel) v *= -0.2;
        } else if (horizontalCollision) {
            autoSpinAttackTicks = 0;
        }
    }
    if (onGround && flying) flying = false;  // landing ends flight
    updatePose(physics);  // Player.tick
}

void Player::jumpFromGround(const Physics& physics) {  // LivingEntity.jumpFromGround: jump strength, Jump Boost
    const double power = double(float(attr.jumpStrength) * blockJumpFactor(physics) +
                                (attr.jumpBoost >= 0 ? 0.1f * float(attr.jumpBoost + 1) : 0.0f));
    if (power <= 1e-5) return;
    vel[1] = std::max(power, vel[1]);
    if (sprinting) {
        vel[0] -= std::sin(yaw) * 0.2;
        vel[2] += std::cos(yaw) * 0.2;
    }
}

// PistonMovingBlockEntity.tick for each moving block (0.5 progress per tick): moveCollidedEntities pushes the player
// out of the moving shape (a slime block also launches it along the move), moveStuckEntities drags a player standing
// on a honey block moved sideways.
void Player::tickPistons(Physics& physics) {
    std::fill(pistonDeltas_, pistonDeltas_ + 3, 0.0);
    if (noClip) return;
    physics.context = {true, pos[1], fallDistance, lastInput.sneak, attr.powderSnowWalker, pos[0], pos[2]};
    for (Physics::Piston& p : physics.pistons) {
        p.progressO = p.progress;
        if (p.progress >= 1.0f) continue;
        const float next = p.progress + 0.5f;
        const int movement = p.extending ? p.dir : (p.dir ^ 1);  // getMovementDirection
        const double delta = next - p.progress;
        const double e = p.extending ? p.progress - 1.0 : 1.0 - p.progress;
        const double off[3] = {p.x + kStep[p.dir][0] * e, p.y + kStep[p.dir][1] * e, p.z + kStep[p.dir][2] * e};
        // getCollisionRelatedBlockState: a retracting source piston base pulls its head along.
        const u16 related = !p.extending && p.source && p.extendedBase ? p.headMoved[p.progress > 0.25f ? 1 : 0] : p.moved;
        const BlockInfo& info = physics.registry().infos()[related];
        const int axis = movement < 2 ? 1 : movement < 4 ? 2 : 0;
        const int sign = kStep[movement][axis];
        auto moveArea = [&](AABB a) {  // PistonMath.getMovementArea
            const double d = delta * sign, lo = std::min(d, 0.0), hi = std::max(d, 0.0);
            const double faceAt = sign > 0 ? a.max[axis] : a.min[axis];
            a.min[axis] = faceAt + lo, a.max[axis] = faceAt + hi;
            return a;
        };
        auto pushOut = [&](const AABB& moving, const AABB& body) {  // getMovement: how far the body must go
            return sign > 0 ? moving.max[axis] - body.min[axis] : body.max[axis] - moving.min[axis];
        };
        if (info.collisionCount) {
            const bool slime = shortBlock(physics.registry().name(p.moved)) == "slime_block";
            AABB bounds{{1e300, 1e300, 1e300}, {-1e300, -1e300, -1e300}};
            for (u32 i = 0; i < info.collisionCount; ++i) {
                const Box& s = physics.registry().box(info.collisionFirst + i);
                for (int a = 0; a < 3; ++a)
                    bounds.min[a] = std::min(bounds.min[a], off[a] + s.min[a]), bounds.max[a] = std::max(bounds.max[a], off[a] + s.max[a]);
            }
            AABB query = moveArea(bounds);  // the entities in the moving bounds and the swept part
            for (int a = 0; a < 3; ++a) query.min[a] = std::min(query.min[a], bounds.min[a]), query.max[a] = std::max(query.max[a], bounds.max[a]);
            if (query.intersects(box())) {
                if (slime) vel[axis] = sign;  // a moving slime block bounces whatever it touches along the move
                double push = 0;
                for (u32 i = 0; i < info.collisionCount; ++i) {
                    const Box& s = physics.registry().box(info.collisionFirst + i);
                    const AABB shape{{off[0] + s.min[0], off[1] + s.min[1], off[2] + s.min[2]}, {off[0] + s.max[0], off[1] + s.max[1], off[2] + s.max[2]}};
                    const AABB area = moveArea(shape);
                    if (area.intersects(box())) {
                        push = std::max(push, pushOut(area, box()));
                        if (push >= delta) break;
                    }
                }
                if (push > 0) {
                    push = std::min(push, delta) + 0.01;
                    physics.context.pistonNoClip = movement;
                    move(physics, kStep[movement][0] * push, kStep[movement][1] * push, kStep[movement][2] * push, true);
                    if (!p.extending && p.source) {  // fixEntityWithinPistonBase
                        const AABB base{{double(p.x), double(p.y), double(p.z)}, {p.x + 1.0, p.y + 1.0, p.z + 1.0}};
                        const AABB body = box();
                        if (body.intersects(base)) {
                            const int opposite = movement ^ 1, osign = -sign;
                            auto out = [&](const AABB& b) { return osign > 0 ? base.max[axis] - b.min[axis] : b.max[axis] - base.min[axis]; };
                            AABB inter = body;
                            for (int a = 0; a < 3; ++a) inter.min[a] = std::max(inter.min[a], base.min[a]), inter.max[a] = std::min(inter.max[a], base.max[a]);
                            double d = out(body) + 0.01;
                            if (std::abs(d - (out(inter) + 0.01)) < 0.01) {
                                d = std::min(d, delta) + 0.01;
                                move(physics, kStep[opposite][0] * d, kStep[opposite][1] * d, kStep[opposite][2] * d, true);
                            }
                        }
                    }
                    physics.context.pistonNoClip = -1;
                }
            }
        }
        // moveStuckEntities: a honey block moved sideways carries a player standing on it.
        if (axis != 1 && shortBlock(physics.registry().name(p.moved)) == "honey_block" && onGround) {
            const BlockInfo& h = physics.registry().infos()[p.moved];
            double top = 0;
            for (u32 i = 0; i < h.collisionCount; ++i) top = std::max(top, double(physics.registry().box(h.collisionFirst + i).max[1]));
            const AABB sticky{{off[0], off[1] + top, off[2]}, {off[0] + 1, off[1] + 1.5000010000000001, off[2] + 1}};
            const bool supported = hasSupport_ && support_[0] == p.x && support_[1] == p.y && support_[2] == p.z;
            if (sticky.intersects(box()) &&
                (supported || (pos[0] >= sticky.min[0] && pos[0] <= sticky.max[0] && pos[2] >= sticky.min[2] && pos[2] <= sticky.max[2]))) {
                physics.context.pistonNoClip = movement;
                move(physics, kStep[movement][0] * delta, kStep[movement][1] * delta, kStep[movement][2] * delta, true);
                physics.context.pistonNoClip = -1;
            }
        }
        p.progress = std::min(next, 1.0f);
    }
}

// TridentItem.releaseUsing with Riptide, on the client: push along the look by the enchantment's strength, spin for
// 20 ticks, and hop 1.2 blocks off the ground.
void Player::riptide(const Physics& physics, float strength) {
    float xd = -std::sin(yaw) * std::cos(pitch), yd = -std::sin(pitch), zd = std::cos(yaw) * std::cos(pitch);
    const float dist = std::sqrt(xd * xd + yd * yd + zd * zd);
    xd *= strength / dist, yd *= strength / dist, zd *= strength / dist;
    vel[0] += xd, vel[1] += yd, vel[2] += zd;
    autoSpinAttackTicks = 20;
    if (onGround) {
        physics.context = {true, pos[1], fallDistance, lastInput.sneak, attr.powderSnowWalker, pos[0], pos[2]};
        move(physics, 0, double(1.1999999f), 0);
    }
}
