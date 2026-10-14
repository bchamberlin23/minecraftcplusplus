// Client-side movement of the entities both sides simulate (MoveSimulationType.SERVER_AND_CLIENT): item entities
// (ItemEntity.tick), experience orbs (ExperienceOrb.tick) and falling blocks (FallingBlockEntity.tick). Entity.move
// against the block collision shapes (no stepping), Entity.updateFluidInteraction (fluid heights and currents),
// gravity, drag, block friction and the bounces; an orb's own interpolation shifts with what it moves itself
// (AbstractInterpolationHandler.applyPredictedMovement).
#include "entity/entities.h"
#include "entity/species.h"

#include "game/player.h"
#include "world/world.h"

#include <cmath>
#include <mutex>
#include <random>
#include <shared_mutex>

namespace ent {

namespace {

constexpr int kItem = 1, kOrb = 2, kFallingBlock = 3, kTnt = 4, kFirework = 5, kHook = 6;

AABB boxOf(const Entity& e) {
    double w, h;
    entitySize(e, w, h);
    return {{e.pos[0] - w / 2, e.pos[1], e.pos[2] - w / 2}, {e.pos[0] + w / 2, e.pos[1] + h, e.pos[2] + w / 2}};
}

// Entity.updateFluidInteraction: the water and lava heights over the box, and the currents pushing it.
void updateFluids(const Physics& ph, Entity& e) {
    const AABB full = boxOf(e);
    AABB b = full;
    for (int a = 0; a < 3; ++a) b.min[a] += 0.001, b.max[a] -= 0.001;
    double current[2][3] = {}, currentHeight[2] = {};
    int count[2] = {};
    e.waterHeight = e.lavaHeight = 0;
    for (int x = int(std::floor(b.min[0])); x <= int(std::ceil(b.max[0])) - 1; ++x)
        for (int y = int(std::floor(b.min[1])); y <= int(std::ceil(b.max[1])) - 1; ++y)
            for (int z = int(std::floor(b.min[2])); z <= int(std::ceil(b.max[2])) - 1; ++z) {
                const Physics::Fluid f = ph.fluid(x, y, z);
                if (!f.type) continue;
                const double top = y + ph.fluidHeight(x, y, z, f);
                if (top < b.min[1]) continue;
                const int t = f.type - 1;
                double& height = t == 0 ? e.waterHeight : e.lavaHeight;
                height = std::max(top - full.min[1], height);
                double flow[3];
                ph.fluidFlow(x, y, z, f, flow);
                currentHeight[t] = std::max(height, currentHeight[t]);
                if (currentHeight[t] < 0.4)
                    for (double& v : flow) v *= currentHeight[t];
                for (int a = 0; a < 3; ++a) current[t][a] += flow[a];
                ++count[t];
            }
    for (int t = 0; t < 2; ++t) {  // CurrentAccumulator.applyTo (water 0.014, lava 0.0023333)
        const double* c = current[t];
        if (!count[t] || c[0] * c[0] + c[1] * c[1] + c[2] * c[2] < double(1.0E-5F)) continue;
        const double scale = t == 0 ? 0.014 : 0.0023333333333333335;
        double impulse[3] = {c[0] / count[t] * scale, c[1] / count[t] * scale, c[2] / count[t] * scale};
        const double len = std::sqrt(impulse[0] * impulse[0] + impulse[1] * impulse[1] + impulse[2] * impulse[2]);
        if (std::abs(e.vel[0]) < 0.003 && std::abs(e.vel[2]) < 0.003 && len < 0.0045000000000000005 && len > 0)
            for (double& v : impulse) v = v / len * 0.0045000000000000005;
        for (int a = 0; a < 3; ++a) e.vel[a] += impulse[a];
    }
}

// Entity.getBlockPosBelowThatAffectsMyMovement: the block 0.5 below the feet.
int belowY(const Entity& e) { return int(std::floor(e.pos[1] - 0.500001)); }

// Entity.move(SELF): the collisions, the collision flags, restituteMovementAfterCollisions and the block speed factor.
void move(const Physics& ph, Entity& e, const double in[3], double gravity) {
    const double d[3] = {in[0], in[1], in[2]};  // `in` may be the entity's own velocity
    AABB b = boxOf(e);
    AABB area = b;
    for (int a = 0; a < 3; ++a) (d[a] < 0 ? area.min[a] : area.max[a]) += d[a];
    std::vector<AABB> boxes;
    ph.collisionBoxes(area, boxes);
    double out[3];
    Physics::collideBoxes(boxes, b, out, d);
    const double moved = out[0] * out[0] + out[1] * out[1] + out[2] * out[2];
    if (moved > 1e-7 || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - moved < 1e-7) {
        for (int a = 0; a < 3; ++a) e.pos[a] += out[a];
        if (e.linearRemaining > 0)  // recordMovement -> applyPredictedMovement: the target moves along
            for (int a = 0; a < 3; ++a) e.linearTarget[a] += out[a];
    }
    const bool xCollision = std::abs(d[0] - out[0]) >= 1e-5, zCollision = std::abs(d[2] - out[2]) >= 1e-5;  // !Mth.equal
    e.horizontalCollision = xCollision || zCollision;
    bool vertical = false;
    if (std::abs(d[1]) > 0) {
        vertical = d[1] != out[1];
        e.verticalCollisionBelow = vertical && d[1] < 0;
        e.onGround = e.verticalCollisionBelow;
    }
    const int ex = int(std::floor(e.pos[0])), ey = int(std::floor(e.pos[1] - 0.2)), ez = int(std::floor(e.pos[2]));  // getOnPosLegacy
    if ((std::abs(d[1]) > 0 && vertical) || e.horizontalCollision) {
        double next[3] = {e.vel[0], e.vel[1], e.vel[2]};
        if (xCollision) next[0] = -e.vel[0] * 0.0;  // getEntityBounciness: 0
        if (zCollision) next[2] = -e.vel[2] * 0.0;
        if (vertical) {
            const double restitution = e.verticalCollisionBelow && !(-e.vel[1] <= gravity) && !ph.suppressesBounce(ex, ey, ez)
                                           ? std::max(0.0, double(ph.bounceRestitution(ex, ey, ez)))
                                           : 0.0;
            double compensation = 0, drag = 1;
            if (restitution > 0) {
                const double portion = out[1] / e.vel[1];
                compensation = portion * gravity;
                drag = 1.0 + portion * (double(0.98f) - 1.0);  // Mth.lerp(portion, 1, getAirDrag)
            }
            next[1] = (compensation - e.vel[1]) * drag * restitution;
        }
        std::copy(next, next + 3, e.vel);
    }
    // Entity.getBlockSpeedFactor: the block here, else (when it is 1 and not water) the one below
    const int bx = int(std::floor(e.pos[0])), by = int(std::floor(e.pos[1])), bz = int(std::floor(e.pos[2]));
    float f = ph.speedFactor(bx, by, bz);
    const Physics::Fluid here = ph.fluid(bx, by, bz);
    if (f == 1.0f && !(here.type == 1 && here.source)) f = ph.speedFactor(bx, belowY(e), bz);
    e.vel[0] *= f, e.vel[2] *= f;
}

}  // namespace

bool EntityWorld::simulateClient(Entity& e) {
    if (!e.info) return false;
    const std::string& n = e.info->name;
    const int kind = n == "minecraft:item" ? kItem : n == "minecraft:experience_orb" ? kOrb : n == "minecraft:falling_block" ? kFallingBlock
                   : n == "minecraft:tnt" ? kTnt : n == "minecraft:firework_rocket" ? kFirework : n == "minecraft:fishing_bobber" ? kHook : 0;
    if (!kind) return false;
    if (!physics_) physics_ = std::make_unique<Physics>(world_, registry_);
    const Physics& ph = *physics_;
    std::shared_lock lock(world_.mutex());
    if (!world_.hasChunk(int(std::floor(e.pos[0])) >> 4, int(std::floor(e.pos[2])) >> 4)) return true;  // not ticked
    // Entity.commonTick: the interpolation first (experience orbs: LinearInterpolationHandler)
    if (e.linearRemaining > 0) {
        const double alpha = 1.0 / e.linearRemaining;
        for (int k = 0; k < 3; ++k) e.pos[k] += (e.linearTarget[k] - e.pos[k]) * alpha;
        --e.linearRemaining;
    }
    const double gravity = e.noGravity ? 0.0 : kind == kOrb ? 0.03 : 0.04;
    const int bx = int(std::floor(e.pos[0])), bz = int(std::floor(e.pos[2]));
    if (kind == kTnt) {  // PrimedTnt.tick: gravity, the move, air drag, the bounce on the ground, the fluids
        e.vel[1] -= gravity;
        move(ph, e, e.vel, gravity);
        for (double& v : e.vel) v *= 0.98;
        if (e.onGround) e.vel[0] *= 0.7, e.vel[1] *= -0.5, e.vel[2] *= 0.7;
        updateFluids(ph, e);
        return true;
    }
    if (kind == kHook) {  // FishingHook.tick on the client: flying, hooked in an entity, bobbing on the water
        static std::mt19937 rng(0xF15E);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const bool biting = dataInt(e, "DATA_BITING") != 0;
        if (biting && !e.hookBiting) e.vel[1] = -0.4 * (0.6 + 0.4 * unit(rng));  // onSyncedDataUpdated: the dip
        e.hookBiting = biting;
        const int hooked = int(dataInt(e, "DATA_HOOKED_ENTITY")) - 1;
        const Entity* hookedIn = hooked >= 0 && hooked != selfId ? entity(hooked) : nullptr;
        const bool hookedSelf = hooked >= 0 && hooked == selfId;
        const int by = int(std::floor(e.pos[1]));
        const Physics::Fluid f = ph.fluid(bx, by, bz);
        const double liquidHeight = f.type == 1 ? ph.fluidHeight(bx, by, bz, f) : 0.0;
        const bool inWater = liquidHeight > 0;
        if (e.hookState == 0) {
            if (hookedIn || hookedSelf) {
                e.vel[0] = e.vel[1] = e.vel[2] = 0;
                e.hookState = 1;
                return true;
            }
            if (inWater) {
                e.vel[0] *= 0.3, e.vel[1] *= 0.2, e.vel[2] *= 0.3;
                e.hookState = 2;
                return true;
            }
        } else if (e.hookState == 1) {  // at the hooked entity, 0.8 of its height up
            if (hookedSelf) {
                std::copy(localPlayer.pos, localPlayer.pos + 3, e.pos);
                e.pos[1] += 1.8 * 0.8;
            } else if (hookedIn) {
                double w, h;
                entitySize(*hookedIn, w, h);
                std::copy(hookedIn->pos, hookedIn->pos + 3, e.pos);
                e.pos[1] += h * 0.8;
            } else if (hooked < 0) {
                e.hookState = 0;
            }
            return true;
        } else {  // bobbing: pulled to the surface, the bite pulling it under
            double force = e.pos[1] + e.vel[1] - by - liquidHeight;
            if (std::abs(force) < 0.01) force += (force > 0 ? 1 : force < 0 ? -1 : 0) * 0.1;
            e.vel[0] *= 0.9, e.vel[1] -= force * unit(rng) * 0.2, e.vel[2] *= 0.9;
            if (inWater && biting) e.vel[1] += -0.1 * unit(rng) * unit(rng);
        }
        if (f.type != 1 && !e.onGround && !hookedIn && !hookedSelf) e.vel[1] -= 0.03;
        move(ph, e, e.vel, 0.03);
        const double h = std::sqrt(e.vel[0] * e.vel[0] + e.vel[2] * e.vel[2]);  // updateRotation
        if (h > 1e-7 || std::abs(e.vel[1]) > 1e-7) {
            const float yaw = float(std::atan2(e.vel[0], e.vel[2]) * 180.0 / 3.14159265358979323846);
            const float pitch = float(std::atan2(e.vel[1], h) * 180.0 / 3.14159265358979323846);
            while (pitch - e.pitch < -180.0f) e.pitch -= 360.0f;
            while (pitch - e.pitch >= 180.0f) e.pitch += 360.0f;
            while (yaw - e.yaw < -180.0f) e.yaw -= 360.0f;
            while (yaw - e.yaw >= 180.0f) e.yaw += 360.0f;
            e.pitch += (pitch - e.pitch) * 0.2f, e.yaw += (yaw - e.yaw) * 0.2f;
        }
        if (e.hookState == 0 && (e.onGround || e.horizontalCollision)) e.vel[0] = e.vel[1] = e.vel[2] = 0;
        for (double& v : e.vel) v *= 0.92;
        return true;
    }
    if (kind == kFirework) {  // FireworkRocketEntity.tick
        if (e.attachedTo >= 0) {  // held by a (gliding) entity: at it (the hand offset left out)
            const Entity* holder = e.attachedTo == selfId ? nullptr : entity(e.attachedTo);
            const double* at = e.attachedTo == selfId ? localPlayer.pos : holder ? holder->pos : nullptr;
            if (at) std::copy(at, at + 3, e.pos);
            if (holder) std::copy(holder->vel, holder->vel + 3, e.vel);
            return true;
        }
        if (!dataInt(e, "DATA_SHOT_AT_ANGLE")) {  // rising: speeds up sideways (not against a wall), 0.04 up
            const double a = e.horizontalCollision ? 1.0 : 1.15;
            e.vel[0] *= a, e.vel[2] *= a, e.vel[1] += 0.04;
        }
        const double movement[3] = {e.vel[0], e.vel[1], e.vel[2]};
        move(ph, e, movement, 0.0);
        std::copy(movement, movement + 3, e.vel);  // no gravity, no drag
        const double h = std::sqrt(e.vel[0] * e.vel[0] + e.vel[2] * e.vel[2]);  // updateRotation: lerped 0.2
        float yaw = float(std::atan2(e.vel[0], e.vel[2]) * 180.0 / 3.14159265358979323846), pitch = float(std::atan2(e.vel[1], h) * 180.0 / 3.14159265358979323846);
        while (pitch - e.pitch < -180.0f) e.pitch -= 360.0f;
        while (pitch - e.pitch >= 180.0f) e.pitch += 360.0f;
        while (yaw - e.yaw < -180.0f) e.yaw -= 360.0f;
        while (yaw - e.yaw >= 180.0f) e.yaw += 360.0f;
        e.pitch += (pitch - e.pitch) * 0.2f, e.yaw += (yaw - e.yaw) * 0.2f;
        return true;
    }
    if (kind == kFallingBlock) {  // FallingBlockEntity.tick (no baseTick: no fluid interaction)
        e.vel[1] -= gravity;
        move(ph, e, e.vel, gravity);
        for (double& v : e.vel) v *= 0.98;
        return true;
    }
    if (kind == kItem) {  // ItemEntity.tick
        updateFluids(ph, e);  // Entity.baseTick
        auto fluidMovement = [&](double m) {  // setFluidMovement
            e.vel[0] *= m, e.vel[2] *= m;
            if (e.vel[1] < double(0.06f)) e.vel[1] += double(5.0E-4F);
        };
        if (e.waterHeight > 0 && e.waterHeight > double(0.1f)) fluidMovement(double(0.99f));
        else if (e.lavaHeight > 0 && e.lavaHeight > double(0.1f)) fluidMovement(double(0.95f));
        else e.vel[1] -= gravity;
        if (e.onGround && !(e.vel[0] * e.vel[0] + e.vel[2] * e.vel[2] > double(1.0E-5F)) && (e.age + e.id) % 4 != 0) {
            // resting: applyEffectsFromBlocksForLastMovements only
        } else {
            move(ph, e, e.vel, gravity);
            const double airDrag = double(0.98f);
            const double ground = e.onGround ? airDrag * ph.slipperiness(bx, belowY(e), bz) : airDrag;
            e.vel[0] *= ground, e.vel[1] *= airDrag, e.vel[2] *= ground;
            if (e.onGround && e.vel[1] < 0) e.vel[1] *= -0.5;
        }
        updateFluids(ph, e);  // updateFluidInteraction at the end of the tick
        return true;
    }
    // ExperienceOrb.tick: the client skips its first tick
    if (e.simFirstTick) {
        e.simFirstTick = false;
        return true;
    }
    updateFluids(ph, e);  // Entity.baseTick
    const bool colliding = !ph.noCollision(boxOf(e));
    // isEyeInFluid(WATER): the fluid surface above the eyes (Entity.updateFluidOnEyes: eye y - 1/9)
    const double eyeY = e.pos[1] + double(e.info->eye) - double(0.11111111F);
    const int ey = int(std::floor(eyeY));
    const Physics::Fluid atEyes = ph.fluid(bx, ey, bz);
    if (atEyes.type == 1 && ey + ph.fluidHeight(bx, ey, bz, atEyes) > eyeY) {
        e.vel[0] *= double(0.99f), e.vel[2] *= double(0.99f);
        e.vel[1] = std::min(e.vel[1] + double(5.0E-4F), double(0.06f));
    } else if (!colliding) {
        e.vel[1] -= gravity;
    }
    if (ph.fluid(bx, int(std::floor(e.pos[1])), bz).type == 2) {  // in lava: hops about
        static std::mt19937 rng(0x0B5);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        e.vel[0] = (u(rng) - u(rng)) * 0.2f, e.vel[1] = 0.2f, e.vel[2] = (u(rng) - u(rng)) * 0.2f;
    }
    // followNearbyPlayer: the nearest player within 8 blocks (not spectating, alive), kept while within 8
    auto playerAt = [&](int id, double out[3], double& eye) {
        if (id == selfId) {
            if (!localPlayer.targetable) return false;
            std::copy(localPlayer.pos, localPlayer.pos + 3, out), eye = localPlayer.eyeHeight;
            return true;
        }
        auto it = entities_.find(id);
        if (it == entities_.end() || it->second.category != Category::Player || it->second.health <= 0) return false;
        if (isSpectator && isSpectator(it->second)) return false;
        std::copy(it->second.pos, it->second.pos + 3, out);
        double w, h;
        entitySize(it->second, w, h);
        eye = it->second.info && it->second.info->height > 0 ? it->second.info->eye * h / it->second.info->height : 1.62;
        return true;
    };
    auto dist2 = [&](const double p[3]) {
        const double dx = p[0] - e.pos[0], dy = p[1] - e.pos[1], dz = p[2] - e.pos[2];
        return dx * dx + dy * dy + dz * dz;
    };
    double target[3], eye = 0;
    if (!playerAt(e.orbTarget, target, eye) || dist2(target) > 64.0) {
        e.orbTarget = INT_MIN;
        double best = -1;
        auto consider = [&](int id) {
            double p[3], ee;
            if (!playerAt(id, p, ee)) return;
            const double d = dist2(p);
            if (d < 64.0 && (best < 0 || d < best)) best = d, e.orbTarget = id;
        };
        consider(selfId);
        for (const auto& [id, other] : entities_)
            if (other.category == Category::Player) consider(id);
    }
    if (e.orbTarget != INT_MIN && playerAt(e.orbTarget, target, eye)) {
        const double delta[3] = {target[0] - e.pos[0], target[1] + eye / 2.0 - e.pos[1], target[2] - e.pos[2]};
        const double length = delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2];
        const double power = 1.0 - std::sqrt(length) / 8.0;
        const double norm = std::sqrt(length);
        if (norm >= 1.0E-5)
            for (int a = 0; a < 3; ++a) e.vel[a] += delta[a] / norm * (power * power * 0.1);
    }
    const double fallSpeed = e.vel[1];
    move(ph, e, e.vel, gravity);
    double friction = double(0.98f);
    if (e.onGround) friction *= ph.slipperiness(int(std::floor(e.pos[0])), belowY(e), int(std::floor(e.pos[2])));
    for (double& v : e.vel) v *= friction;
    if (e.verticalCollisionBelow && fallSpeed < -gravity) e.vel[1] = -fallSpeed * 0.4;
    return true;
}

}  // namespace ent
