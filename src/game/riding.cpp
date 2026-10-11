#include "game/riding.h"

#include "assets/assets.h"
#include "entity/entities.h"
#include "game/game.h"
#include "game/gameplay.h"
#include "game/item_rules.h"
#include "net/client.h"
#include "ui/items.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <shared_mutex>

namespace {

constexpr double kPi = 3.14159265358979;
constexpr float kDegf = 3.14159265f / 180.0f;

float wrapDegrees(float d) {
    d = std::fmod(d, 360.0f);
    if (d >= 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return d;
}

std::string_view shortName(std::string_view n) { return n.starts_with("minecraft:") ? n.substr(10) : n; }

bool isHorseLike(std::string_view n) {
    return n == "horse" || n == "donkey" || n == "mule" || n == "skeleton_horse" || n == "zombie_horse" || n == "camel" ||
           n == "camel_husk";
}
bool isBoat(std::string_view n) { return n.ends_with("_boat") || n.ends_with("_raft"); }

}  // namespace

Riding::Riding(Gameplay& gameplay, Game& game, ent::EntityWorld* entities, const ItemRules& rules)
    : gameplay_(gameplay), game_(game), entities_(entities), rules_(rules) {
    game.rideTick = [this](const PlayerInput& in) { rideTick(in); };
    game.rideEye = [this](double eye[3]) {
        const ent::Entity* e = entity();
        if (!e) return false;
        // The seat as the vehicle is drawn this frame (its interpolated position), plus the eye height.
        const float pt = std::clamp(entities_->partialTick(), 0.0f, 1.0f);
        double pos[3];
        for (int a = 0; a < 3; ++a) pos[a] = e->prevPos[a] + (e->pos[a] - e->prevPos[a]) * pt;
        float yaw = e->prevYaw + wrapDegrees(e->yaw - e->prevYaw) * pt;
        seat(*e, pos, yaw, eye);
        eye[1] += game_.player().eyeHeight();
        return true;
    };
}

ent::Entity* Riding::entity() const { return entities_ && vehicle_ >= 0 ? entities_->entity(vehicle_) : nullptr; }

void Riding::passengers(int vehicle, const std::vector<int>& ids, int self) {
    const auto it = std::find(ids.begin(), ids.end(), self);
    if (it == ids.end()) {
        if (vehicle == vehicle_) dismounted();
        return;
    }
    const bool mounting = vehicle_ != vehicle;
    vehicle_ = vehicle;
    seatIndex_ = int(it - ids.begin());
    passengerCount_ = int(ids.size());
    if (mounting) {
        status_ = oldStatus_ = Status::InAir;
        deltaRotation_ = 0, landFriction_ = 0, lastYd_ = 0;
        jumpRidingTicks_ = 0, jumpScale_ = 0, pendingJump_ = 0, wasJumping_ = false;
        typeName_.clear();
        LOG("riding: mounted entity %d (seat %d of %d)", vehicle, seatIndex_, passengerCount_);
    }
}

void Riding::dismounted() {
    if (vehicle_ >= 0) LOG("riding: dismounted entity %d", vehicle_);
    vehicle_ = -1;
    kind_ = Kind::None;
    controlled_ = false;
    if (entities_ && entities_->localControl() >= 0) entities_->setLocalControl(-1, nullptr);
    game_.vehicleId = -1;
    game_.handsBusy = false;
    jumpScale_ = 0;
}

void Riding::moveVehicle(const double pos[3], float yRot, float) {
    ent::Entity* e = entity();
    if (!e || !controlled_) return;
    std::copy(pos, pos + 3, e->pos);
    std::copy(pos, pos + 3, e->prevPos);
    e->yaw = e->prevYaw = yRot;
}

bool Riding::computeControlled(const ent::Entity& e) const {
    if (seatIndex_ != 0) return false;  // only the first passenger drives
    const std::string_view n = shortName(typeName_);
    if (isBoat(n)) return true;  // AbstractBoat.getControllingPassenger: a player in the first seat
    const bool saddled = e.equipment[7] >= 0;  // EquipmentSlot.SADDLE
    if (isHorseLike(n)) return saddled;
    auto holding = [&](const char* item) {
        for (const ui::ItemStack* s : {gameplay_.mainHand(), gameplay_.offHand()})
            if (s && gameplay_.itemName(s->item) == item) return true;
        return false;
    };
    if (n == "pig") return saddled && holding("minecraft:carrot_on_a_stick");
    if (n == "strider") return saddled && holding("minecraft:warped_fungus_on_a_stick");
    return false;
}

void Riding::frame() {
    game_.vehicleHud = {};
    if (vehicle_ < 0) {
        game_.vehicleId = -1;
        return;
    }
    ent::Entity* e = entity();
    game_.vehicleId = vehicle_;
    if (!e) return;  // not spawned on this client (yet)
    game_.vehicleHud = {vehicleHearts(), vehicleHealth(), jumpable(), jumpScale_, jumpCooldown()};
    if (typeName_.empty() && e->info) {
        typeName_ = e->info->name;
        const std::string_view n = shortName(typeName_);
        kind_ = isBoat(n) ? Kind::Boat : e->category == ent::Category::Living ? Kind::Living : Kind::Other;
    }
    const bool control = computeControlled(*e);
    if (control != controlled_ || (control && entities_->localControl() != vehicle_)) {
        controlled_ = control;
        if (control) {
            std::fill(e->vel, e->vel + 3, 0.0);
            e->stepCount = 0, e->linearRemaining = 0;
            entities_->setLocalControl(vehicle_, [this](ent::Entity& v) { vehicleTick(v); });
        } else {
            entities_->setLocalControl(-1, nullptr);
        }
        LOG("riding: %s %s", typeName_.c_str(), control ? "controlled by this client" : "moved by the server");
    }
}

// Entity.positionRider: the vehicle's passenger attachment point minus the player's vehicle attachment (0.6 up).
void Riding::seat(const ent::Entity& e, const double vehiclePos[3], float vehicleYaw, double out[3]) const {
    double p[3] = {0, 0, 0};
    const std::string_view n = shortName(typeName_);
    const ItemRules::EntityDims* dims = rules_.entity(typeName_);
    if (kind_ == Kind::Boat) {
        // AbstractBoat.getPassengerAttachmentPoint: rideHeight, front / back seats.
        const double height = dims ? dims->height : 0.5625;
        p[1] = n.ends_with("_raft") ? height * 0.8888889 : height / 3.0;
        double z = n.find("chest") != std::string_view::npos ? 0.15 : 0.0;
        if (passengerCount_ > 1) z = seatIndex_ == 0 ? 0.2 : -0.6;
        p[2] = z;
    } else if (dims && !dims->seats.empty()) {
        const auto& s = dims->seats[size_t(std::clamp(seatIndex_, 0, int(dims->seats.size()) - 1))];
        p[0] = s[0], p[1] = s[1], p[2] = s[2];
    } else {
        p[1] = dims ? dims->height : e.info ? e.info->height : 1.0;
    }
    // Vec3.yRot(-yRot): rotate around Y by the vehicle's yaw.
    const double a = -double(vehicleYaw) * kPi / 180.0, c = std::cos(a), s = std::sin(a);
    const double x = p[0] * c + p[2] * s, z = p[2] * c - p[0] * s;
    out[0] = vehiclePos[0] + x;
    out[1] = vehiclePos[1] + p[1] - 0.6;  // Avatar.DEFAULT_VEHICLE_ATTACHMENT
    out[2] = vehiclePos[2] + z;
}

bool Riding::jumpable() const {
    if (!controlled_ || kind_ != Kind::Living) return false;
    return isHorseLike(shortName(typeName_));  // PlayerRideableJumping.canJump: saddled (checked by controlled_)
}

int Riding::jumpCooldown() const { return 0; }

int Riding::vehicleHearts() const {
    const ent::Entity* e = entity();
    if (!e || e->category != ent::Category::Living) return 0;
    const double maxHealth = gameplay_.entityAttribute(vehicle_, "max_health", e->info ? 20.0 : 0.0);
    return std::min(int(float(maxHealth) + 0.5f) / 2, 30);
}

float Riding::vehicleHealth() const {
    const ent::Entity* e = entity();
    return e ? e->health : 0.0f;
}

void Riding::rideTick(const PlayerInput& in) {
    input_ = in;
    Player& p = game_.player();
    std::copy(p.pos, p.pos + 3, p.prev);
    p.sneaking = false, p.sprinting = false, p.onGround = false;
    std::fill(p.vel, p.vel + 3, 0.0);
    // LocalPlayer.aiStep: the jump bar charges while jump is held and fires on release.
    if (jumpable() && jumpCooldown() == 0) {
        if (jumpRidingTicks_ < 0 && ++jumpRidingTicks_ == 0) jumpScale_ = 0;
        if (wasJumping_ && !in.jump) {
            jumpRidingTicks_ = -10;
            const int amount = int(std::floor(jumpScale_ * 100.0f));
            // AbstractHorse.onPlayerJump -> getPlayerJumpPendingScale
            pendingJump_ = amount >= 90 ? 1.0f : 0.4f + 0.4f * float(std::max(amount, 0)) / 90.0f;
            if (client_) {
                net::Writer w;
                w.varInt(gameplay_.entityId());
                w.varInt(3);  // ServerboundPlayerCommandPacket.Action.START_RIDING_JUMP
                w.varInt(amount);
                client_->sendPlay("player_command", w);
            }
        } else if (!wasJumping_ && in.jump) {
            jumpRidingTicks_ = 0, jumpScale_ = 0;
        } else if (wasJumping_) {
            ++jumpRidingTicks_;
            jumpScale_ = jumpRidingTicks_ < 10 ? float(jumpRidingTicks_) * 0.1f : 0.8f + 2.0f / float(jumpRidingTicks_ - 9) * 0.1f;
        }
    } else {
        jumpScale_ = 0;
    }
    wasJumping_ = in.jump;
    // LocalPlayer.rideTick: paddling a boat keeps the hands busy.
    game_.handsBusy = controlled_ && kind_ == Kind::Boat && (in.forward != 0 || in.strafe != 0);
    // The player sits on the seat (the vehicle's current position).
    if (const ent::Entity* e = entity()) seat(*e, e->pos, e->yaw, p.pos);
}

void Riding::vehicleTick(ent::Entity& e) {
    std::shared_lock lock(gameplay_.world().mutex());
    if (kind_ == Kind::Boat) boatTick(e);
    else livingTick(e);
    lock.unlock();
    sendVehicleMove(e);
}

void Riding::sendVehicleMove(const ent::Entity& e) {
    if (!client_) return;
    net::Writer w;  // ServerboundMoveVehiclePacket: position, yRot, xRot, onGround
    w.f64(e.pos[0]), w.f64(e.pos[1]), w.f64(e.pos[2]);
    w.f32(e.yaw), w.f32(e.pitch);
    w.boolean(vehicleOnGround_);
    client_->sendPlay("move_vehicle", w);
}

// Entity.move for a vehicle's bounding box: Y, then the larger horizontal axis first; step up like Entity.collide.
void Riding::collideMove(ent::Entity& e, double width, double height, double step, double d[3], bool& onGround,
                         bool& hCollision) {
    const Physics physics(gameplay_.world(), gameplay_.registry());
    auto boxAt = [&](const double p[3]) {
        return AABB{{p[0] - width / 2, p[1], p[2] - width / 2}, {p[0] + width / 2, p[1] + height, p[2] + width / 2}};
    };
    auto clip = [](const std::vector<AABB>& boxes, const AABB& b, int axis, double delta) {
        const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
        for (const AABB& o : boxes) {
            if (o.max[a1] <= b.min[a1] || o.min[a1] >= b.max[a1] || o.max[a2] <= b.min[a2] || o.min[a2] >= b.max[a2]) continue;
            if (delta > 0 && o.min[axis] >= b.max[axis] - 1e-7) delta = std::min(delta, o.min[axis] - b.max[axis]);
            else if (delta < 0 && o.max[axis] <= b.min[axis] + 1e-7) delta = std::max(delta, o.max[axis] - b.min[axis]);
        }
        return delta;
    };
    auto collide = [&](const std::vector<AABB>& boxes, AABB& b, const double in[3], double out[3]) {
        out[1] = clip(boxes, b, 1, in[1]);
        b.min[1] += out[1], b.max[1] += out[1];
        const int first = std::abs(in[0]) < std::abs(in[2]) ? 2 : 0, second = first == 0 ? 2 : 0;
        out[first] = clip(boxes, b, first, in[first]);
        b.min[first] += out[first], b.max[first] += out[first];
        out[second] = clip(boxes, b, second, in[second]);
        b.min[second] += out[second], b.max[second] += out[second];
    };
    const AABB start = boxAt(e.pos);
    AABB area = start;
    for (int a = 0; a < 3; ++a) (d[a] < 0 ? area.min[a] : area.max[a]) += d[a];
    area.max[1] += step;
    std::vector<AABB> boxes;
    physics.collisionBoxes(area, boxes);
    double out[3];
    AABB b = start;
    collide(boxes, b, d, out);
    const bool blockedH = out[0] != d[0] || out[2] != d[2];
    if (step > 0 && blockedH && (onGround || (out[1] != d[1] && d[1] < 0))) {
        AABB s = start;
        const double up[3] = {0, step, 0};
        double upOut[3], stepOut[3], downOut[3];
        collide(boxes, s, up, upOut);
        const double h[3] = {d[0], 0, d[2]};
        collide(boxes, s, h, stepOut);
        const double down[3] = {0, -upOut[1] + d[1], 0};
        collide(boxes, s, down, downOut);
        if (stepOut[0] * stepOut[0] + stepOut[2] * stepOut[2] > out[0] * out[0] + out[2] * out[2] + 1e-9) {
            b = s;
            out[0] = stepOut[0], out[2] = stepOut[2], out[1] = upOut[1] + downOut[1];
        }
    }
    e.pos[0] = (b.min[0] + b.max[0]) / 2, e.pos[1] = b.min[1], e.pos[2] = (b.min[2] + b.max[2]) / 2;
    onGround = out[1] != d[1] && d[1] < 0;
    hCollision = out[0] != d[0] || out[2] != d[2];
    if (out[0] != d[0]) e.vel[0] = 0;
    if (out[2] != d[2]) e.vel[2] = 0;
    if (out[1] != d[1]) e.vel[1] = 0;  // Block.updateEntityMovementAfterFallOn
    std::copy(out, out + 3, d);
}

// ---------------------------------------------------------------------------------------------------------
// AbstractBoat (client side, controlled by the local player)

void Riding::boatTick(ent::Entity& e) {
    World& world = gameplay_.world();
    BlockRegistry& reg = gameplay_.registry();
    const ItemRules::EntityDims* dims = rules_.entity(typeName_);
    const double width = dims ? dims->width : 1.375, height = dims ? dims->height : 0.5625;
    auto box = [&] {
        return AABB{{e.pos[0] - width / 2, e.pos[1], e.pos[2] - width / 2}, {e.pos[0] + width / 2, e.pos[1] + height, e.pos[2] + width / 2}};
    };
    // FluidState of water at a block: its height (FluidState.getHeight: 1 under the same fluid), -1 for none.
    auto water = [&](int x, int y, int z, bool* source) -> double {
        const u16 id = world.block(x, y, z);
        const BlockInfo& b = reg.infos()[id];
        const std::string& name = reg.name(id);
        const bool isWater = b.waterlogged || (b.fluid && name.starts_with("minecraft:water")) ||
                             name.starts_with("minecraft:bubble_column");
        if (!isWater) return -1.0;
        int level = 0;
        if (!b.waterlogged && b.fluid) {
            const std::string_view l = reg.property(id, "level");
            if (!l.empty()) level = std::atoi(std::string(l).c_str());
        }
        if (source) *source = level == 0;
        const u16 above = world.block(x, y + 1, z);
        const BlockInfo& ba = reg.infos()[above];
        const std::string& an = reg.name(above);
        if (ba.waterlogged || (ba.fluid && an.starts_with("minecraft:water")) || an.starts_with("minecraft:bubble_column")) return 1.0;
        return level == 0 || level >= 8 ? 8.0 / 9.0 : double(8 - level) / 9.0;
    };
    // getStatus: under water, in water, on land (block friction below), in the air.
    auto underwater = [&](Status& st) {
        const AABB bb = box();
        const double maxY = bb.max[1] + 0.001;
        bool under = false;
        for (int x = int(std::floor(bb.min[0])); x < int(std::ceil(bb.max[0])); ++x)
            for (int y = int(std::floor(bb.max[1])); y < int(std::ceil(maxY)); ++y)
                for (int z = int(std::floor(bb.min[2])); z < int(std::ceil(bb.max[2])); ++z) {
                    bool source = false;
                    const double h = water(x, y, z, &source);
                    if (h >= 0 && maxY < y + h) {
                        if (!source) {
                            st = Status::UnderFlowingWater;
                            return true;
                        }
                        under = true;
                    }
                }
        if (under) st = Status::UnderWater;
        return under;
    };
    auto checkInWater = [&] {
        const AABB bb = box();
        bool in = false;
        waterLevel_ = -1e300;
        for (int x = int(std::floor(bb.min[0])); x < int(std::ceil(bb.max[0])); ++x)
            for (int y = int(std::floor(bb.min[1])); y < int(std::ceil(bb.min[1] + 0.001)); ++y)
                for (int z = int(std::floor(bb.min[2])); z < int(std::ceil(bb.max[2])); ++z) {
                    const double h = water(x, y, z, nullptr);
                    if (h < 0) continue;
                    waterLevel_ = std::max(waterLevel_, double(y) + h);
                    in |= bb.min[1] < double(y) + h;
                }
        return in;
    };
    auto groundFriction = [&] {
        const AABB bb = box();
        const AABB slab{{bb.min[0], bb.min[1] - 0.001, bb.min[2]}, {bb.max[0], bb.min[1], bb.max[2]}};
        const int x0 = int(std::floor(slab.min[0])) - 1, x1 = int(std::ceil(slab.max[0])) + 1;
        const int y0 = int(std::floor(slab.min[1])) - 1, y1 = int(std::ceil(slab.max[1])) + 1;
        const int z0 = int(std::floor(slab.min[2])) - 1, z1 = int(std::ceil(slab.max[2])) + 1;
        const Physics physics(world, reg);
        float friction = 0;
        int count = 0;
        for (int x = x0; x < x1; ++x)
            for (int z = z0; z < z1; ++z) {
                const int edges = (x == x0 || x == x1 - 1 ? 1 : 0) + (z == z0 || z == z1 - 1 ? 1 : 0);
                if (edges == 2) continue;
                for (int y = y0; y < y1; ++y) {
                    if (edges > 0 && (y == y0 || y == y1 - 1)) continue;
                    const u16 id = world.block(x, y, z);
                    if (reg.name(id).starts_with("minecraft:lily_pad")) continue;
                    const BlockInfo& b = reg.infos()[id];
                    bool hit = false;
                    for (u32 i = 0; i < b.collisionCount && !hit; ++i) {
                        const Box& s = reg.box(b.collisionFirst + i);
                        const AABB a{{x + s.min[0], y + s.min[1], z + s.min[2]}, {x + s.max[0], y + s.max[1], z + s.max[2]}};
                        hit = a.max[0] > slab.min[0] && a.min[0] < slab.max[0] && a.max[1] > slab.min[1] &&
                              a.min[1] < slab.max[1] && a.max[2] > slab.min[2] && a.min[2] < slab.max[2];
                    }
                    if (hit) friction += physics.slipperiness(x, y, z), ++count;
                }
            }
        return count ? friction / float(count) : 0.0f;
    };
    auto waterLevelAbove = [&]() -> double {
        const AABB bb = box();
        const int minX = int(std::floor(bb.min[0])), maxX = int(std::ceil(bb.max[0]));
        const int minY = int(std::floor(bb.max[1])), maxY = int(std::ceil(bb.max[1] - lastYd_));
        const int minZ = int(std::floor(bb.min[2])), maxZ = int(std::ceil(bb.max[2]));
        for (int y = minY; y < maxY; ++y) {
            double blockHeight = 0;
            for (int x = minX; x < maxX && blockHeight < 1.0; ++x)
                for (int z = minZ; z < maxZ && blockHeight < 1.0; ++z) blockHeight = std::max(blockHeight, water(x, y, z, nullptr));
            if (blockHeight < 1.0) return double(y) + blockHeight;
        }
        return double(maxY) + 1;
    };

    oldStatus_ = status_;
    {
        Status st = Status::InAir;
        if (underwater(st)) {
            waterLevel_ = box().max[1];
            status_ = st;
        } else if (checkInWater()) {
            status_ = Status::InWater;
        } else if (const float f = groundFriction(); f > 0) {
            landFriction_ = f;
            status_ = Status::OnLand;
        } else {
            status_ = Status::InAir;
        }
    }
    // floatBoat
    const double gravity = 0.04;
    double vspeed = -gravity, buoyancy = 0;
    float invFriction = 0.05f;
    if (oldStatus_ == Status::InAir && status_ != Status::InAir && status_ != Status::OnLand) {
        waterLevel_ = e.pos[1] + height;
        const double targetY = waterLevelAbove() - height + 0.101;
        AABB moved = box();
        moved.min[1] += targetY - e.pos[1], moved.max[1] += targetY - e.pos[1];
        if (Physics(world, reg).noCollision(moved)) {
            e.pos[1] = targetY;
            e.vel[1] = 0;
            lastYd_ = 0;
        }
        status_ = Status::InWater;
    } else {
        if (status_ == Status::InWater) {
            buoyancy = (waterLevel_ - e.pos[1]) / height;
            invFriction = 0.9f;
        } else if (status_ == Status::UnderFlowingWater) {
            vspeed = -7.0E-4;
            invFriction = 0.9f;
        } else if (status_ == Status::UnderWater) {
            buoyancy = 0.01f;
            invFriction = 0.45f;
        } else if (status_ == Status::InAir) {
            invFriction = 0.9f;
        } else if (status_ == Status::OnLand) {
            invFriction = landFriction_;
            landFriction_ /= 2.0f;  // a player controls it
        }
        e.vel[0] *= invFriction, e.vel[1] += vspeed, e.vel[2] *= invFriction;
        deltaRotation_ *= invFriction;
        if (buoyancy > 0) e.vel[1] = (e.vel[1] + buoyancy * (gravity / 0.65)) * 0.75;
    }
    // controlBoat: input from the rider (strafe +1 = left).
    const bool left = input_.strafe > 0, right = input_.strafe < 0, up = input_.forward > 0, down = input_.forward < 0;
    float acceleration = 0;
    if (left) deltaRotation_ -= 1.0f;
    if (right) deltaRotation_ += 1.0f;
    if (right != left && !up && !down) acceleration += 0.005f;
    e.yaw += deltaRotation_;
    if (up) acceleration += 0.04f;
    if (down) acceleration -= 0.005f;
    e.vel[0] += std::sin(-e.yaw * kDegf) * acceleration;
    e.vel[2] += std::cos(e.yaw * kDegf) * acceleration;
    paddleLeft_ = (right && !left) || up;
    paddleRight_ = (left && !right) || up;
    e.paddle[0] = paddleLeft_, e.paddle[1] = paddleRight_;  // the paddle animation follows our own input
    if (client_) {
        net::Writer w;  // ServerboundPaddleBoatPacket
        w.boolean(paddleLeft_), w.boolean(paddleRight_);
        client_->sendPlay("paddle_boat", w);
    }
    // move (no step: boats cannot climb)
    double d[3] = {e.vel[0], e.vel[1], e.vel[2]};
    collideMove(e, width, height, 0.0, d, vehicleOnGround_, vehicleHCollision_);
    lastYd_ = e.vel[1];
    e.bodyYaw = e.headYaw = e.yaw;
    // positionRider: the rider turns with the boat, its look kept within 105 degrees of the boat's heading.
    Player& p = game_.player();
    float riderYaw = p.yaw / kDegf + deltaRotation_;
    const float delta = wrapDegrees(riderYaw - e.yaw);
    riderYaw += std::clamp(delta, -105.0f, 105.0f) - delta;
    p.yaw = riderYaw * kDegf;
}

// ---------------------------------------------------------------------------------------------------------
// LivingEntity.travelRidden for horses, camels, pigs and striders

void Riding::livingTick(ent::Entity& e) {
    const std::string_view n = shortName(typeName_);
    const bool horse = isHorseLike(n);
    const Player& p = game_.player();
    // tickRidden: the vehicle faces where the rider looks.
    e.yaw = p.yaw / kDegf;
    e.pitch = p.pitch / kDegf * 0.5f;
    e.bodyYaw = e.headYaw = e.yaw;
    // getRiddenInput: the rider's input (LocalPlayer.modifyInput: x 0.98); horses strafe at half, back off at a quarter;
    // pigs and striders always walk forward.
    double strafe = input_.strafe * 0.98, forward = input_.forward * 0.98;
    if (horse) {
        strafe *= 0.5;
        if (forward <= 0) forward *= 0.25;
    } else {
        strafe = 0, forward = 1;
    }
    // getRiddenSpeed
    double speed = gameplay_.entityAttribute(vehicle_, "movement_speed", horse ? 0.225 : n == "pig" ? 0.25 : 0.175);
    if (n == "pig") speed *= 0.225;
    else if (n == "strider") speed *= 0.55;
    const double step = gameplay_.entityAttribute(vehicle_, "step_height", horse ? 1.0 : 0.6);
    const double gravity = gameplay_.entityAttribute(vehicle_, "gravity", 0.08);
    const ItemRules::EntityDims* dims = rules_.entity(typeName_);
    const double width = dims ? dims->width : 1.4, height = dims ? dims->height : 1.6;
    // AbstractHorse.tickRidden: the charged jump on the ground (executeRidersJump).
    if (horse && vehicleOnGround_) {
        if (pendingJump_ > 0) {
            const double jumpStrength = gameplay_.entityAttribute(vehicle_, "jump_strength", 0.7);
            e.vel[1] = jumpStrength * pendingJump_;
            if (forward > 0) {
                e.vel[0] += -0.4 * std::sin(e.yaw * kDegf) * pendingJump_;
                e.vel[2] += 0.4 * std::cos(e.yaw * kDegf) * pendingJump_;
            }
        }
        pendingJump_ = 0;
    }
    auto moveRelative = [&](double sp) {
        double f = forward, s = strafe;
        const double len = f * f + s * s;
        if (len < 1e-7) return;
        if (len > 1.0) {
            const double inv = 1.0 / std::sqrt(len);
            f *= inv, s *= inv;
        }
        f *= sp, s *= sp;
        const double sy = std::sin(e.yaw * kDegf), cy = std::cos(e.yaw * kDegf);
        e.vel[0] += s * cy - f * sy;
        e.vel[2] += f * cy + s * sy;
    };
    const Physics physics(gameplay_.world(), gameplay_.registry());
    const AABB body{{e.pos[0] - width / 2, e.pos[1], e.pos[2] - width / 2}, {e.pos[0] + width / 2, e.pos[1] + height, e.pos[2] + width / 2}};
    if (physics.inFluid(body)) {  // travelInFluid, simplified: swim up with jump
        if (input_.jump) e.vel[1] += 0.04;
        moveRelative(0.02);
        double d[3] = {e.vel[0], e.vel[1], e.vel[2]};
        collideMove(e, width, height, step, d, vehicleOnGround_, vehicleHCollision_);
        e.vel[0] *= 0.8, e.vel[1] *= 0.8, e.vel[2] *= 0.8;
        e.vel[1] -= gravity / 16.0;
        return;
    }
    // travelInAir
    const float friction = vehicleOnGround_ ? physics.slipperiness(int(std::floor(e.pos[0])), int(std::floor(e.pos[1] - 0.5)),
                                                                   int(std::floor(e.pos[2])))
                                            : 1.0f;
    const double sp = vehicleOnGround_ ? (friction > 0.6f ? speed * (0.21600002 / (double(friction) * friction * friction)) : speed)
                                       : speed * 0.1;  // getFlyingSpeed with a player in control
    moveRelative(sp);
    double d[3] = {e.vel[0], e.vel[1], e.vel[2]};
    collideMove(e, width, height, step, d, vehicleOnGround_, vehicleHCollision_);
    e.vel[1] = (e.vel[1] - gravity) * 0.98;
    e.vel[0] *= double(friction) * 0.91, e.vel[2] *= double(friction) * 0.91;
    e.onGround = vehicleOnGround_;
}
