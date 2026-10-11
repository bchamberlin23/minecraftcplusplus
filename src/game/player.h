#pragma once
#include "core/common.h"

class World;
class BlockRegistry;

struct AABB {
    double min[3], max[3];
    bool intersects(const AABB& o) const {
        for (int a = 0; a < 3; ++a)
            if (max[a] <= o.min[a] || min[a] >= o.max[a]) return false;
        return true;
    }
};

struct RayHit {
    bool hit = false;
    int x = 0, y = 0, z = 0;  // block
    int face = 0;              // 0 down 1 up 2 north 3 south 4 west 5 east
    double distance = 0;
    double point[3] = {};
};

// Collision and picking queries against block shapes.
class Physics {
public:
    Physics(const World& world, const BlockRegistry& registry) : world_(world), registry_(registry) {}
    void collisionBoxes(const AABB& area, std::vector<AABB>& out) const;
    RayHit raycast(const double origin[3], const double dir[3], double maxDistance) const;
    bool inFluid(const AABB& box) const;
    float slipperiness(int x, int y, int z) const;

private:
    const World& world_;
    const BlockRegistry& registry_;
};

struct PlayerInput {
    float forward = 0, strafe = 0;  // -1..1 (strafe: +1 = left, like the game)
    bool jump = false, sneak = false, sprint = false;
};

// Vanilla player movement at 20 ticks per second.
class Player {
public:
    static constexpr double kWidth = 0.6, kHeight = 1.8, kEyeHeight = 1.62, kSneakEyeHeight = 1.27;

    double pos[3] = {};   // feet, centre
    double prev[3] = {};  // position at the previous tick (for interpolation)
    double vel[3] = {};
    float yaw = 0, pitch = 0;  // radians
    bool onGround = false, flying = true, sneaking = false, inWater = false;

    void tick(const Physics& physics, const PlayerInput& input);
    AABB box() const { return boxAt(pos); }
    static AABB boxAt(const double p[3]) {
        return {{p[0] - kWidth / 2, p[1], p[2] - kWidth / 2}, {p[0] + kWidth / 2, p[1] + kHeight, p[2] + kWidth / 2}};
    }
    double eyeHeight() const { return sneaking ? kSneakEyeHeight : kEyeHeight; }

private:
    void moveRelative(const PlayerInput& input, double speed);
    void move(const Physics& physics, double dx, double dy, double dz);
};
