#include "game/player.h"

#include "assets/assets.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>

u8 Physics::specialKind(u16 id) const {
    if (special_.size() <= id) special_.resize(size_t(id) + 1, 0);
    if (!special_[id]) {
        const std::string& n = registry_.name(id);
        special_[id] = n.starts_with("minecraft:scaffolding") ? 2 : n.starts_with("minecraft:powder_snow") ? 3 : 1;
    }
    return special_[id];
}

void Physics::collisionBoxes(const AABB& area, std::vector<AABB>& out) const {
    out.clear();
    const BlockInfo* infos = registry_.infos();
    const int x0 = int(std::floor(area.min[0])), x1 = int(std::floor(area.max[0]));
    const int y0 = int(std::floor(area.min[1])) - 1, y1 = int(std::floor(area.max[1]));  // -1: tall shapes (fences)
    const int z0 = int(std::floor(area.min[2])), z1 = int(std::floor(area.max[2]));
    auto add = [&](int x, int y, int z, double x0_, double y0_, double z0_, double x1_, double y1_, double z1_) {
        const AABB a{{x + x0_, y + y0_, z + z0_}, {x + x1_, y + y1_, z + z1_}};
        if (a.intersects(area)) out.push_back(a);
    };
    for (int y = y0; y <= y1; ++y)
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                const u16 id = world_.block(x, y, z);
                if (context.set && id) {
                    const u8 kind = specialKind(id);
                    if (kind == 2) {  // ScaffoldingBlock.getCollisionShape for the player
                        if (context.feetY > y + 1.0 - 1e-5 && !context.descending) {  // SHAPE_STABLE: top and legs
                            add(x, y, z, 0, 0.875, 0, 1, 1, 1);
                            add(x, y, z, 0, 0, 0, 0.125, 1, 0.125), add(x, y, z, 0.875, 0, 0, 1, 1, 0.125);
                            add(x, y, z, 0, 0, 0.875, 0.125, 1, 1), add(x, y, z, 0.875, 0, 0.875, 1, 1, 1);
                        } else if (registry_.property(id, "distance") != "0" && registry_.property(id, "bottom") == "true" &&
                                   context.feetY > y - 1e-5) {
                            add(x, y, z, 0, 0, 0, 1, 0.125, 1);  // SHAPE_UNSTABLE_BOTTOM
                        }
                        continue;
                    }
                    if (kind == 3) {  // PowderSnowBlock.getCollisionShape
                        if (context.fallDistance > 2.5) add(x, y, z, 0, 0, 0, 1, 0.9, 1);
                        else if (context.powderSnowWalker && context.feetY > y + 1.0 - 1e-5 && !context.descending)
                            add(x, y, z, 0, 0, 0, 1, 1, 1);
                        continue;
                    }
                }
                const BlockInfo& b = infos[id];
                for (u32 i = 0; i < b.collisionCount; ++i) {
                    const Box& s = registry_.box(b.collisionFirst + i);
                    AABB a{{x + s.min[0], y + s.min[1], z + s.min[2]}, {x + s.max[0], y + s.max[1], z + s.max[2]}};
                    if (a.intersects(area)) out.push_back(a);
                }
            }
}

bool Physics::inFluid(const AABB& box) const {
    const BlockInfo* infos = registry_.infos();
    for (int y = int(std::floor(box.min[1])); y <= int(std::floor(box.max[1] - 0.4)); ++y)
        for (int z = int(std::floor(box.min[2])); z <= int(std::floor(box.max[2])); ++z)
            for (int x = int(std::floor(box.min[0])); x <= int(std::floor(box.max[0])); ++x) {
                const BlockInfo& b = infos[world_.block(x, y, z)];
                if (b.fluid || b.waterlogged) return true;
            }
    return false;
}

bool Physics::inLava(const AABB& box) const {
    for (int y = int(std::floor(box.min[1])); y <= int(std::floor(box.max[1] - 0.4)); ++y)
        for (int z = int(std::floor(box.min[2])); z <= int(std::floor(box.max[2])); ++z)
            for (int x = int(std::floor(box.min[0])); x <= int(std::floor(box.max[0])); ++x)
                if (registry_.infos()[world_.block(x, y, z)].fluid && registry_.name(world_.block(x, y, z)).starts_with("minecraft:lava"))
                    return true;
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

namespace {
std::string_view shortBlock(const std::string& state) {
    std::string_view n = state;
    n = n.substr(0, n.find('['));
    if (n.starts_with("minecraft:")) n.remove_prefix(10);
    return n;
}
}  // namespace

bool Physics::climbable(int x, int y, int z) const {
    const std::string_view n = shortBlock(registry_.name(world_.block(x, y, z)));
    return n == "ladder" || n == "vine" || n == "scaffolding" || n == "weeping_vines" || n == "weeping_vines_plant" ||
           n == "twisting_vines" || n == "twisting_vines_plant" || n == "cave_vines" || n == "cave_vines_plant";
}

float Physics::speedFactor(int x, int y, int z) const {
    const std::string_view n = shortBlock(registry_.name(world_.block(x, y, z)));
    return n == "soul_sand" || n == "honey_block" ? 0.4f : 1.0f;
}

float Physics::jumpFactor(int x, int y, int z) const {
    return shortBlock(registry_.name(world_.block(x, y, z))) == "honey_block" ? 0.5f : 1.0f;
}

bool Physics::stuckMultiplier(const AABB& b, double out[3]) const {
    bool any = false;
    for (int y = int(std::floor(b.min[1] + 1e-5)); y <= int(std::floor(b.max[1] - 1e-5)); ++y)
        for (int z = int(std::floor(b.min[2] + 1e-5)); z <= int(std::floor(b.max[2] - 1e-5)); ++z)
            for (int x = int(std::floor(b.min[0] + 1e-5)); x <= int(std::floor(b.max[0] - 1e-5)); ++x) {
                const std::string_view n = shortBlock(registry_.name(world_.block(x, y, z)));
                if (n == "cobweb") out[0] = 0.25, out[1] = 0.05, out[2] = 0.25, any = true;
                else if (n == "sweet_berry_bush" && !any) out[0] = 0.8, out[1] = 0.75, out[2] = 0.8, any = true;
                else if (n == "powder_snow" && !any && y == int(std::floor(b.min[1] + 1e-5)))  // the block at the feet
                    out[0] = 0.9, out[1] = 1.5, out[2] = 0.9, any = true;
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
RayHit Physics::raycast(const double o[3], const double d[3], double maxDistance) const {
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
    for (int i = 0; i < 64; ++i) {
        const BlockInfo& b = infos[world_.block(cell[0], cell[1], cell[2])];
        for (u32 k = 0; k < b.selectionCount; ++k) {
            const Box& s = registry_.box(b.selectionFirst + k);
            double tNear = 0, tFar = maxDistance;
            int face = -1;
            bool miss = false;
            for (int a = 0; a < 3 && !miss; ++a) {
                const double lo = cell[a] + s.min[a], hi = cell[a] + s.max[a];
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

// Player.canFallAtLeast: nothing below the box (moved by dx, dz) within minHeight.
bool Player::canFallAtLeast(const Physics& physics, double dx, double dz, double minHeight) const {
    const AABB b = box();
    const AABB area{{b.min[0] + 1e-7 + dx, b.min[1] - minHeight - 1e-7, b.min[2] + 1e-7 + dz},
                    {b.max[0] - 1e-7 + dx, b.min[1], b.max[2] - 1e-7 + dz}};
    std::vector<AABB> boxes;
    physics.collisionBoxes(area, boxes);
    return std::none_of(boxes.begin(), boxes.end(), [&](const AABB& o) { return o.intersects(area); });
}

void Player::move(const Physics& physics, double dx, double dy, double dz) {
    // Player.maybeBackOffFromEdge: sneaking on the ground never walks off an edge higher than a step.
    if (!flying && dy <= 0 && sneaking && onGround) {
        constexpr double kStep = 0.05, kMaxDown = 0.6;
        const double sx = dx > 0 ? kStep : dx < 0 ? -kStep : 0, sz = dz > 0 ? kStep : dz < 0 ? -kStep : 0;
        while (dx != 0 && canFallAtLeast(physics, dx, 0, kMaxDown)) {
            if (std::abs(dx) <= kStep) {
                dx = 0;
                break;
            }
            dx -= sx;
        }
        while (dz != 0 && canFallAtLeast(physics, 0, dz, kMaxDown)) {
            if (std::abs(dz) <= kStep) {
                dz = 0;
                break;
            }
            dz -= sz;
        }
        while (dx != 0 && dz != 0 && canFallAtLeast(physics, dx, dz, kMaxDown)) {
            dx = std::abs(dx) <= kStep ? 0 : dx - sx;
            dz = std::abs(dz) <= kStep ? 0 : dz - sz;
        }
    }
    if (stuck_[0] != 0 || stuck_[1] != 0 || stuck_[2] != 0) {  // Entity.move: stuck in a cobweb / bush
        dx *= stuck_[0], dy *= stuck_[1], dz *= stuck_[2];
        std::fill(stuck_, stuck_ + 3, 0.0);
        std::fill(vel, vel + 3, 0.0);
    }
    const double in[3] = {dx, dy, dz};
    AABB start = box();
    AABB area = start;
    for (int a = 0; a < 3; ++a) (in[a] < 0 ? area.min[a] : area.max[a]) += in[a];
    area.max[1] += attr.stepHeight;  // room for stepping up
    std::vector<AABB> boxes;
    physics.collisionBoxes(area, boxes);

    double out[3];
    AABB b = start;
    collide(boxes, b, out, in);

    // Step up (0.6 blocks) when blocked horizontally while on the ground.
    const bool blockedH = out[0] != dx || out[2] != dz;
    if (blockedH && (onGround || (out[1] != dy && dy < 0)) && !flying) {
        double stepOut[3];
        AABB s = start;
        const double up[3] = {0, attr.stepHeight, 0};
        double upOut[3];
        collide(boxes, s, upOut, up);
        const double h[3] = {dx, 0, dz};
        collide(boxes, s, stepOut, h);
        const double down[3] = {0, -(upOut[1]) + dy, 0};
        double downOut[3];
        collide(boxes, s, downOut, down);
        if (stepOut[0] * stepOut[0] + stepOut[2] * stepOut[2] > out[0] * out[0] + out[2] * out[2] + 1e-9) {
            b = s;
            out[0] = stepOut[0], out[2] = stepOut[2];
            out[1] = upOut[1] + downOut[1];
        }
    }

    pos[0] = (b.min[0] + b.max[0]) / 2;
    pos[1] = b.min[1];
    pos[2] = (b.min[2] + b.max[2]) / 2;
    onGround = out[1] != dy && dy < 0;
    horizontalCollision = out[0] != dx || out[2] != dz;
    moved_[0] = out[0], moved_[2] = out[2];
    if (out[0] != dx) vel[0] = 0;
    if (out[2] != dz) vel[2] = 0;
    if (out[1] != dy) vel[1] = 0;
}

// Player.updatePlayerPose: the desired pose (swimming, gliding, crouching with the sneak key) if the body fits,
// else crouching, else crawling (the swimming pose under a one-block gap).
void Player::updatePose(const Physics& physics) {
    auto fits = [&](Pose p) {
        AABB b = boxAt(pos, p);
        for (int a = 0; a < 3; ++a) b.min[a] += 1e-7, b.max[a] -= 1e-7;
        return physics.noCollision(b);
    };
    if (noClip) {
        pose = Pose::Standing;
        return;
    }
    if (!fits(Pose::Swimming)) return;
    const Pose desired = swimming ? Pose::Swimming : fallFlying ? Pose::FallFlying
                         : (lastInput.sneak && !flying) ? Pose::Crouching : Pose::Standing;
    pose = fits(desired) ? desired : fits(Pose::Crouching) ? Pose::Crouching : Pose::Swimming;
}

void Player::tick(const Physics& physics, const PlayerInput& rawInput) {
    std::copy(pos, pos + 3, prev);
    PlayerInput input = rawInput;
    lastInput = rawInput;
    const bool jumpPressed = rawInput.jump && !wasJumping_;
    wasJumping_ = rawInput.jump;
    // Collision shapes that depend on the player (scaffolding, powder snow).
    physics.context = {true, pos[1], fallDistance, rawInput.sneak, attr.powderSnowWalker};
    updatePose(physics);
    // Player.isMovingSlowly: crouching or crawling walks at the sneaking speed.
    sneaking = pose == Pose::Crouching && !flying;
    const bool crawling = pose == Pose::Swimming && !swimming;
    const bool slow = sneaking || crawling;
    // LocalPlayer.aiStep sprinting: the sprint key starts it while moving forward; it lasts until the forward
    // impulse ends, a wall stops the player, food runs low or the player sneaks.
    const bool forwardImpulse = rawInput.forward > 0;
    // Entity.isUnderWater: water at the eyes (1/9 below them, like updateFluidOnEyes).
    auto waterAt = [&](double x, double y, double z) {
        const u16 id = physics.world().block(int(std::floor(x)), int(std::floor(y)), int(std::floor(z)));
        const BlockInfo& b = physics.registry().infos()[id];
        return b.waterlogged || (b.fluid && physics.registry().name(id).starts_with("minecraft:water")) ||
               physics.registry().name(id).starts_with("minecraft:bubble_column");
    };
    const bool underWater = waterAt(pos[0], pos[1] + eyeHeight() - 0.11111111, pos[2]);
    if (!sprinting && rawInput.sprint && forwardImpulse && (!slow || underWater) && canSprint && !useBlocksSprint &&
        (!fallFlying || underWater))
        sprinting = true;
    bool minorCollision = false;  // LocalPlayer.isHorizontalCollisionMinor: moved within 8 degrees of the input
    {
        const double sy = std::sin(yaw), cy = std::cos(yaw);
        const double gx = rawInput.strafe * cy - rawInput.forward * sy, gz = rawInput.forward * cy + rawInput.strafe * sy;
        const double in2 = gx * gx + gz * gz, mv2 = moved_[0] * moved_[0] + moved_[2] * moved_[2];
        if (in2 >= 1e-5 && mv2 >= 1e-5)
            minorCollision = std::acos(std::clamp((gx * moved_[0] + gz * moved_[2]) / std::sqrt(in2 * mv2), -1.0, 1.0)) < 0.13962634;
    }
    if (sprinting) {
        // shouldStopRunSprinting / shouldStopSwimSprinting
        const bool stop = swimming ? (!canSprint || !inWater || (!forwardImpulse && !onGround && !rawInput.sneak))
                                   : (!forwardImpulse || (horizontalCollision && !minorCollision) || !canSprint ||
                                      (inWater && !underWater) || (sneaking && !underWater));
        if (stop) sprinting = false;
    }
    input.sprint = sprinting;
    {  // KeyboardInput (normalized) + LocalPlayer.modifyInput: 0.98, the use slowdown, the sneaking speed, stretched
       // to the unit square
        float f = input.forward, s = input.strafe;
        const float len = std::sqrt(f * f + s * s);
        if (len > 0) {
            f /= len, s /= len;
            float scale = 0.98f * useSpeed * (slow ? float(attr.sneakSpeed) : 1.0f);
            const float ax = std::abs(s), ay = std::abs(f);
            const float tan = ay > ax ? ax / ay : ay / ax;
            scale = std::min(scale * std::sqrt(1.0f + tan * tan), 1.0f);
            input.forward = f * scale, input.strafe = s * scale;
        }
    }
    inLava = physics.inLava(box());
    inWater = physics.inFluid(box()) && !inLava;
    // Entity.updateSwimming: sprinting under water starts it, leaving the water or the sprint ends it.
    if (flying || noClip) swimming = false;
    else if (swimming) swimming = sprinting && inWater;
    else swimming = sprinting && underWater && waterAt(pos[0], pos[1], pos[2]);
    // Gliding ends on the ground, in liquids, on a ladder or without a glider (the server's LivingEntity.updateFallFlying).
    if (fallFlying && (onGround || inWater || inLava || flying || !attr.canGlide || attr.levitation >= 0)) fallFlying = false;
    const int bx = int(std::floor(pos[0])), by = int(std::floor(pos[1])), bz = int(std::floor(pos[2]));
    const bool onClimbable = !flying && physics.climbable(bx, by, bz);
    // LocalPlayer.aiStep: jump pressed in mid-air with a glider starts gliding (tryToStartFallFlying).
    if (jumpPressed && !flying && !onGround && !onClimbable && !fallFlying && attr.canGlide && !inWater && !inLava &&
        attr.levitation < 0) {
        fallFlying = true;
        if (onStartFallFlying) onStartFallFlying();
    }
    // Entity.getBlockSpeedFactor: the block at the feet, else the one below (soul sand, honey).
    auto blockFactor = [&](float (Physics::*f)(int, int, int) const) {
        const float at = (physics.*f)(bx, by, bz);
        return at != 1.0f || inWater ? at : (physics.*f)(bx, int(std::floor(pos[1] - 0.5000001)), bz);
    };
    // Powder snow at the body (Entity.isInPowderSnow) for climbing out with leather boots.
    bool inPowderSnow = false;
    {
        const AABB b = box();
        for (int y = int(std::floor(b.min[1])); y <= int(std::floor(b.max[1] - 1e-7)) && !inPowderSnow; ++y)
            for (int z = int(std::floor(b.min[2])); z <= int(std::floor(b.max[2] - 1e-7)) && !inPowderSnow; ++z)
                for (int x = int(std::floor(b.min[0])); x <= int(std::floor(b.max[0] - 1e-7)) && !inPowderSnow; ++x)
                    inPowderSnow = physics.registry().name(physics.world().block(x, y, z)).starts_with("minecraft:powder_snow");
    }
    // Entity.move tail: block speed factor, then blocks the player is inside (cobwebs, powder snow, bubble columns).
    auto afterMove = [&] {
        const float f = blockFactor(&Physics::speedFactor);
        vel[0] *= f, vel[2] *= f;
        double stuck[3];
        if (physics.stuckMultiplier(box(), stuck)) std::copy(stuck, stuck + 3, stuck_);
        // BubbleColumnBlock.entityInside: inside the column, or riding its surface (air above).
        const AABB b = box();
        for (int y = int(std::floor(b.min[1])); y <= int(std::floor(b.max[1] - 1e-7)); ++y)
            for (int z = int(std::floor(b.min[2])); z <= int(std::floor(b.max[2] - 1e-7)); ++z)
                for (int x = int(std::floor(b.min[0])); x <= int(std::floor(b.max[0] - 1e-7)); ++x) {
                    const u16 id = physics.world().block(x, y, z);
                    if (!physics.registry().name(id).starts_with("minecraft:bubble_column")) continue;
                    const bool down = physics.registry().property(id, "drag") == "true";
                    const u16 above = physics.world().block(x, y + 1, z);
                    const BlockInfo& a = physics.registry().infos()[above];
                    if (!a.collisionCount && !a.fluid && !a.waterlogged)  // Entity.onAboveBubbleColumn
                        vel[1] = down ? std::max(-0.9, vel[1] - 0.03) : std::min(1.8, vel[1] + 0.1);
                    else  // onInsideBubbleColumn
                        vel[1] = down ? std::max(-0.3, vel[1] - 0.03) : std::min(0.7, vel[1] + 0.06);
                    fallDistance = 0;
                    goto bubbleDone;
                }
    bubbleDone:;
    };
    // LivingEntity.handleOnClimbable.
    auto climbClamp = [&] {
        if (!onClimbable) return;
        fallDistance = 0;
        vel[0] = std::clamp(vel[0], -0.15, 0.15), vel[2] = std::clamp(vel[2], -0.15, 0.15);
        vel[1] = std::max(vel[1], -0.15);
        if (vel[1] < 0 && sneaking && physics.registry().name(physics.world().block(bx, by, bz)).find("scaffolding") == std::string::npos)
            vel[1] = 0;
    };
    // LivingEntity.jumpOutOfFluid: a wall at the surface lifts the player out.
    auto jumpOut = [&](double startY) {
        if (!horizontalCollision) return;
        AABB b = box();
        const double d[3] = {vel[0], vel[1] + 0.6 - pos[1] + startY, vel[2]};
        for (int a = 0; a < 3; ++a) b.min[a] += d[a], b.max[a] += d[a];
        if (physics.noCollision(b) && !physics.containsLiquid(b)) vel[1] = 0.3;
    };
    auto trackFall = [&](double startY) {  // Entity.checkFallDamage: the fall distance (powder snow catches big falls)
        if (onGround || inWater || inLava || flying || attr.slowFalling >= 0 && vel[1] > -0.5) fallDistance = 0;
        else if (pos[1] < startY) fallDistance += startY - pos[1];
    };

    if (noClip) {  // spectators: always flying, no collisions (Entity.noPhysics)
        flying = true;
        const double flySpeed = input.sprint ? 0.1 : 0.05;
        if (input.jump) vel[1] += flySpeed * 3;
        if (input.sneak) vel[1] -= flySpeed * 3;
        moveRelative(input, flySpeed);
        for (int a = 0; a < 3; ++a) pos[a] += vel[a];
        onGround = horizontalCollision = false;
        vel[0] *= 0.91, vel[2] *= 0.91, vel[1] *= 0.6;
        return;
    }
    if (flying) {
        const double flySpeed = input.sprint ? 0.1 : 0.05;
        if (input.jump) vel[1] += flySpeed * 3;
        if (input.sneak) vel[1] -= flySpeed * 3;
        moveRelative(input, flySpeed);
        move(physics, vel[0], vel[1], vel[2]);
        vel[0] *= 0.91, vel[2] *= 0.91, vel[1] *= 0.6;
        fallDistance = 0;
        if (onGround) flying = false;  // landing ends flight, like the game
        return;
    }
    const double startY = pos[1];
    // Gravity: Slow Falling caps it while falling, Levitation replaces it (LivingEntity.getEffectiveGravity /
    // travelInAir).
    const double gravity = vel[1] <= 0 && attr.slowFalling >= 0 ? std::min(attr.gravity, 0.01) : attr.gravity;
    // Player.travel: swimming steers up and down with the look.
    if (swimming) {
        const double lookY = -std::sin(pitch);
        const double m = lookY < -0.2 ? 0.085 : 0.06;
        const u16 above = physics.world().block(bx, int(std::floor(pos[1] + 1.0 - 0.1)), bz);
        const BlockInfo& a = physics.registry().infos()[above];
        if (lookY <= 0 || input.jump || a.fluid || a.waterlogged) vel[1] += (lookY - vel[1]) * m;
    }
    if (inWater || inLava) {  // LivingEntity.travelInWater / travelInLava
        if (input.jump) vel[1] += 0.04;  // LivingEntity.jumpInLiquid
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
            afterMove();
            if (horizontalCollision && onClimbable) vel[1] = 0.2;
            vel[0] *= slowDown, vel[1] *= 0.8, vel[2] *= slowDown;
            // getFluidFallingAdjustedMovement (not while sprint-swimming)
            if (!sprinting) {
                const bool falling = vel[1] <= 0;
                if (falling && std::abs(vel[1] - 0.005) >= 0.003 && std::abs(vel[1] - gravity / 16.0) < 0.003) vel[1] = -0.003;
                else vel[1] -= gravity / 16.0;
            }
        } else {
            moveRelative(input, 0.02);
            move(physics, vel[0], vel[1], vel[2]);
            afterMove();
            vel[0] *= 0.5, vel[1] *= 0.5, vel[2] *= 0.5;
            vel[1] -= gravity / 4.0;
        }
        jumpOut(startY);
        trackFall(startY);
        return;
    }
    if (fallFlying) {  // LivingEntity.travelFallFlying / updateFallFlyingMovement
        if (attr.rocketBoost) {  // FireworkRocketEntity.tick: pushes its glider along the look
            const double lx = -std::sin(yaw) * std::cos(pitch), ly = -std::sin(pitch), lz = std::cos(yaw) * std::cos(pitch);
            vel[0] += lx * 0.1 + (lx * 1.5 - vel[0]) * 0.5;
            vel[1] += ly * 0.1 + (ly * 1.5 - vel[1]) * 0.5;
            vel[2] += lz * 0.1 + (lz * 1.5 - vel[2]) * 0.5;
        }
        const double lx = -std::sin(yaw) * std::cos(pitch), ly = -std::sin(pitch), lz = std::cos(yaw) * std::cos(pitch);
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
        vel[0] *= 0.99f, vel[1] *= 0.98f, vel[2] *= 0.99f;
        move(physics, vel[0], vel[1], vel[2]);
        afterMove();
        fallDistance = 0;
        if (onGround) fallFlying = false;
        return;
    }
    // LivingEntity.travelInAir: block friction (1 in the air) scales the acceleration only above 0.6 (ice,
    // slime); the velocity keeps friction * 0.91 per tick.
    const double friction = onGround ? physics.slipperiness(int(std::floor(pos[0])), int(std::floor(pos[1] - 0.5)),
                                                             int(std::floor(pos[2])))
                                     : 1.0;
    const double slip = friction * 0.91;
    if (input.jump && onGround) {  // LivingEntity.jumpFromGround: jump strength, Jump Boost
        const double power = attr.jumpStrength * blockFactor(&Physics::jumpFactor) +
                             (attr.jumpBoost >= 0 ? 0.1 * (attr.jumpBoost + 1) : 0.0);
        if (power > 1e-5) {
            vel[1] = std::max(power, vel[1]);
            if (input.sprint) {
                vel[0] -= std::sin(yaw) * 0.2;
                vel[2] += std::cos(yaw) * 0.2;
            }
        }
    }
    // getSpeed: the movement_speed attribute (with the sprint modifier); in the air Player.getFlyingSpeed.
    const double walk = attr.moveSpeed * (input.sprint ? 1.3 : 1.0);
    const double speed = onGround ? (friction > 0.6 ? walk * (0.21600002 / (friction * friction * friction)) : walk)
                                  : (input.sprint ? 0.026 : 0.02);
    moveRelative(input, speed);
    climbClamp();
    move(physics, vel[0], vel[1], vel[2]);
    afterMove();
    // handleRelativeFrictionAndCalculateMovement: climbing ladders, and powder snow with leather boots.
    if ((horizontalCollision || input.jump) && (onClimbable || (wasInPowderSnow_ && attr.powderSnowWalker))) vel[1] = 0.2;
    wasInPowderSnow_ = inPowderSnow;
    if (attr.levitation >= 0) vel[1] += (0.05 * (attr.levitation + 1) - vel[1]) * 0.2;
    else vel[1] -= gravity;
    vel[1] *= 0.98;
    vel[0] *= slip, vel[2] *= slip;
    trackFall(startY);
}
