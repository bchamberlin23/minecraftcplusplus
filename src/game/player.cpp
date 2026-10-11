#include "game/player.h"

#include "assets/assets.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>

void Physics::collisionBoxes(const AABB& area, std::vector<AABB>& out) const {
    out.clear();
    const BlockInfo* infos = registry_.infos();
    const int x0 = int(std::floor(area.min[0])), x1 = int(std::floor(area.max[0]));
    const int y0 = int(std::floor(area.min[1])) - 1, y1 = int(std::floor(area.max[1]));  // -1: tall shapes (fences)
    const int z0 = int(std::floor(area.min[2])), z1 = int(std::floor(area.max[2]));
    for (int y = y0; y <= y1; ++y)
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                const BlockInfo& b = infos[world_.block(x, y, z)];
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
    area.max[1] += 0.6;  // room for stepping up
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
        const double up[3] = {0, 0.6, 0};
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

void Player::tick(const Physics& physics, const PlayerInput& rawInput) {
    std::copy(pos, pos + 3, prev);
    PlayerInput input = rawInput;
    lastInput = rawInput;
    sneaking = input.sneak && !flying;
    // LocalPlayer.aiStep sprinting: the sprint key starts it while moving forward; it lasts until the forward
    // impulse ends, a wall stops the player, food runs low or the player sneaks.
    const bool forwardImpulse = rawInput.forward > 0;
    if (!sprinting && rawInput.sprint && forwardImpulse && !sneaking && canSprint) sprinting = true;
    bool minorCollision = false;  // LocalPlayer.isHorizontalCollisionMinor: moved within 8 degrees of the input
    {
        const double sy = std::sin(yaw), cy = std::cos(yaw);
        const double gx = rawInput.strafe * cy - rawInput.forward * sy, gz = rawInput.forward * cy + rawInput.strafe * sy;
        const double in2 = gx * gx + gz * gz, mv2 = moved_[0] * moved_[0] + moved_[2] * moved_[2];
        if (in2 >= 1e-5 && mv2 >= 1e-5)
            minorCollision = std::acos(std::clamp((gx * moved_[0] + gz * moved_[2]) / std::sqrt(in2 * mv2), -1.0, 1.0)) < 0.13962634;
    }
    if (sprinting && (!forwardImpulse || (horizontalCollision && !minorCollision) || sneaking || !canSprint || inWater))
        sprinting = false;
    input.sprint = sprinting;
    {  // KeyboardInput (normalized) + LocalPlayer.modifyInput: 0.98, sneaking 0.3, stretched to the unit square
        float f = input.forward, s = input.strafe;
        const float len = std::sqrt(f * f + s * s);
        if (len > 0) {
            f /= len, s /= len;
            float scale = 0.98f * (sneaking ? 0.3f : 1.0f);
            const float ax = std::abs(s), ay = std::abs(f);
            const float tan = ay > ax ? ax / ay : ay / ax;
            scale = std::min(scale * std::sqrt(1.0f + tan * tan), 1.0f);
            input.forward = f * scale, input.strafe = s * scale;
        }
    }
    inLava = physics.inLava(box());
    inWater = physics.inFluid(box()) && !inLava;
    const int bx = int(std::floor(pos[0])), by = int(std::floor(pos[1])), bz = int(std::floor(pos[2]));
    const bool onClimbable = !flying && physics.climbable(bx, by, bz);
    // Entity.getBlockSpeedFactor: the block at the feet, else the one below (soul sand, honey).
    auto blockFactor = [&](float (Physics::*f)(int, int, int) const) {
        const float at = (physics.*f)(bx, by, bz);
        return at != 1.0f || inWater ? at : (physics.*f)(bx, int(std::floor(pos[1] - 0.5000001)), bz);
    };
    // Entity.move tail: block speed factor, then blocks the player is inside (cobwebs).
    auto afterMove = [&] {
        const float f = blockFactor(&Physics::speedFactor);
        vel[0] *= f, vel[2] *= f;
        double stuck[3];
        if (physics.stuckMultiplier(box(), stuck)) std::copy(stuck, stuck + 3, stuck_);
    };
    // LivingEntity.handleOnClimbable.
    auto climbClamp = [&] {
        if (!onClimbable) return;
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
        if (onGround) flying = false;  // landing ends flight, like the game
        return;
    }
    if (inWater || inLava) {  // LivingEntity.travelInWater / travelInLava
        const double startY = pos[1];
        if (input.jump) vel[1] += 0.04;
        moveRelative(input, 0.02);
        move(physics, vel[0], vel[1], vel[2]);
        afterMove();
        if (inWater) {
            if (horizontalCollision && onClimbable) vel[1] = 0.2;
            const double slow = sprinting ? 0.9 : 0.8;
            vel[0] *= slow, vel[1] *= 0.8, vel[2] *= slow;
            vel[1] -= 0.005;  // getFluidFallingAdjustedMovement: gravity / 16
        } else {
            vel[0] *= 0.5, vel[1] *= 0.5, vel[2] *= 0.5;
            vel[1] -= 0.02;
        }
        jumpOut(startY);
        return;
    }
    // LivingEntity.travelInAir: block friction (1 in the air) scales the acceleration only above 0.6 (ice,
    // slime); the velocity keeps friction * 0.91 per tick.
    const double friction = onGround ? physics.slipperiness(int(std::floor(pos[0])), int(std::floor(pos[1] - 0.5)),
                                                             int(std::floor(pos[2])))
                                     : 1.0;
    const double slip = friction * 0.91;
    if (input.jump && onGround) {
        vel[1] = 0.42 * blockFactor(&Physics::jumpFactor);
        if (input.sprint) {
            vel[0] -= std::sin(yaw) * 0.2;
            vel[2] += std::cos(yaw) * 0.2;
        }
    }
    const double walk = input.sprint && input.forward > 0 ? 0.13 : 0.1;
    const double speed = onGround ? (friction > 0.6 ? walk * (0.21600002 / (friction * friction * friction)) : walk)
                                  : (input.sprint ? 0.026 : 0.02);
    moveRelative(input, speed);
    climbClamp();
    move(physics, vel[0], vel[1], vel[2]);
    afterMove();
    if ((horizontalCollision || input.jump) && onClimbable) vel[1] = 0.2;
    vel[1] = (vel[1] - 0.08) * 0.98;
    vel[0] *= slip, vel[2] *= slip;
}
