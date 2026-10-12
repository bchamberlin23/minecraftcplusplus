#pragma once
// Riding (part of the gameplay layer): the local player as a passenger.
//  - the vehicle comes from ClientboundSetPassengersPacket (Gameplay peeks it);
//  - the player sits on the vehicle's seat (Entity.positionRider: the vehicle's passenger attachment point minus the
//    player's vehicle attachment point) and the camera follows the seat as the vehicle is drawn;
//  - vehicles the player controls (Entity.isLocalInstanceAuthoritative) move on this client and are reported with
//    ServerboundMoveVehiclePacket: boats (AbstractBoat physics, paddle_boat), saddled horses / donkeys / mules /
//    camels and pigs / striders steered with their item (LivingEntity.travel with the ridden input and speed), saddled
//    nautiluses / zombie nautiluses swimming where the rider looks, harnessed happy ghasts flying (unless holding still);
//    others (minecarts, llamas, a second boat seat) follow the server;
//  - jumping vehicles: the jump bar charge (LocalPlayer.aiStep) and START_RIDING_JUMP; the client's horse jump, the
//    camel's and the nautilus's dash and its cooldown (Camel / AbstractNautilus.getJumpCooldown).
// The player dismounts by sneaking (the server reads the shift key from player_input).
#include "core/common.h"
#include "game/player.h"

#include <string>
#include <vector>

class Game;
class Gameplay;
class ItemRules;
namespace net {
class Client;
}
namespace ent {
class EntityWorld;
struct Entity;
}  // namespace ent

class Riding {
public:
    Riding(Gameplay& gameplay, Game& game, ent::EntityWorld* entities, const ItemRules& rules);
    void setClient(net::Client* client) { client_ = client; }

    // ClientboundSetPassengersPacket (main thread): the vehicle's passengers in order.
    void passengers(int vehicle, const std::vector<int>& ids, int self);
    void dismounted();             // respawn / new level: not riding any more
    void moveVehicle(const double pos[3], float yRot, float xRot);  // ClientboundMoveVehiclePacket (server correction)
    void frame();                  // main thread, before Game::frame: local control, Game hooks
    int vehicle() const { return vehicle_; }
    bool controlled() const { return controlled_; }

    // HUD (Hud.extractVehicleHealth, JumpableVehicleBar): 0 hearts when the vehicle shows none.
    int vehicleHearts() const;
    float vehicleHealth() const;
    bool jumpable() const;             // LocalPlayer.jumpableVehicle
    float jumpScale() const { return jumpScale_; }
    int jumpCooldown() const;

private:
    enum class Kind { None, Boat, Living, Other };
    ent::Entity* entity() const;
    bool computeControlled(const ent::Entity& e) const;
    void seat(const ent::Entity& e, const double vehiclePos[3], float vehicleYaw, double out[3]) const;  // rider's feet
    void rideTick(const PlayerInput& in);  // Game::rideTick
    void vehicleTick(ent::Entity& e);      // EntityWorld local control tick
    void boatTick(ent::Entity& e);
    void livingTick(ent::Entity& e);
    void swimFlyTick(ent::Entity& e);  // happy ghast, nautilus
    void sendVehicleMove(const ent::Entity& e);
    // Moves an axis-aligned box (vehicle) by delta with block collisions and an optional step up; returns the
    // movement done and the collision flags. Landing on a bouncy block bounces like Entity.restituteMovementAfter-
    // Collisions (gravity: the vehicle's; living: full block restitution, others 0.8 of it). extraBoxes: more
    // colliders (a strider's lava surface).
    void collideMove(ent::Entity& e, double width, double height, double step, double delta[3], bool& onGround,
                     bool& hCollision, double gravity = 0, bool living = false);
    std::vector<AABB> extraBoxes_;

    Gameplay& gameplay_;
    Game& game_;
    ent::EntityWorld* entities_;
    const ItemRules& rules_;
    net::Client* client_ = nullptr;

    int vehicle_ = -1, seatIndex_ = 0, passengerCount_ = 0;
    Kind kind_ = Kind::None;
    bool controlled_ = false;
    std::string typeName_;
    PlayerInput input_;
    bool wasJumping_ = false;
    int jumpRidingTicks_ = 0;
    float jumpScale_ = 0, pendingJump_ = 0;
    int dashCooldown_ = 0;  // camel (55 ticks) / nautilus (40 ticks) after a dash
    // AbstractBoat state.
    enum class Status { InWater, UnderWater, UnderFlowingWater, OnLand, InAir };
    Status status_ = Status::InAir, oldStatus_ = Status::InAir;
    double waterLevel_ = 0, lastYd_ = 0;
    float landFriction_ = 0, deltaRotation_ = 0;
    bool paddleLeft_ = false, paddleRight_ = false;
    bool vehicleOnGround_ = false, vehicleHCollision_ = false;
};
