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

void Player::move(const Physics& physics, double dx, double dy, double dz) {
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
    if (out[0] != dx) vel[0] = 0;
    if (out[2] != dz) vel[2] = 0;
    if (out[1] != dy) vel[1] = 0;
}

void Player::tick(const Physics& physics, const PlayerInput& rawInput) {
    std::copy(pos, pos + 3, prev);
    PlayerInput input = rawInput;
    sneaking = input.sneak && !flying;
    if (sneaking) input.forward *= 0.3f, input.strafe *= 0.3f;
    inWater = physics.inFluid(box());

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
    if (inWater) {
        moveRelative(input, 0.02);
        if (input.jump) vel[1] += 0.04;
        move(physics, vel[0], vel[1], vel[2]);
        vel[0] *= 0.8, vel[1] *= 0.8, vel[2] *= 0.8;
        vel[1] -= 0.02;
        return;
    }
    const double slip = onGround ? physics.slipperiness(int(std::floor(pos[0])), int(std::floor(pos[1] - 0.5)),
                                                         int(std::floor(pos[2]))) * 0.91
                                 : 0.91;
    if (input.jump && onGround) {
        vel[1] = 0.42;
        if (input.sprint) {
            vel[0] -= std::sin(yaw) * 0.2;
            vel[2] += std::cos(yaw) * 0.2;
        }
    }
    const double walk = input.sprint && input.forward > 0 ? 0.13 : 0.1;
    const double speed = onGround ? walk * (0.21600002 / (slip * slip * slip)) : (input.sprint ? 0.026 : 0.02);
    moveRelative(input, speed);
    move(physics, vel[0], vel[1], vel[2]);
    vel[1] = (vel[1] - 0.08) * 0.98;
    vel[0] *= slip, vel[2] *= slip;
}
