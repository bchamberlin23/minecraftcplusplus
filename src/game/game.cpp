#include "game/game.h"

#include "assets/assets.h"
#include "platform/window.h"
#include "world/terrain.h"
#include "world/world.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <unordered_set>

namespace {
constexpr double kPi = 3.14159265358979;
constexpr int kDir[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
constexpr const char* kFaceName[6] = {"down", "up", "north", "south", "west", "east"};

u32 abgr(u32 rgb, u8 alpha) { return (u32(alpha) << 24) | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF); }
}  // namespace

Game::Game(World& world, BlockRegistry& registry, Terrain& terrain, Renderer& renderer, const PlayerStart& start)
    : world_(world),
      registry_(registry),
      terrain_(terrain),
      renderer_(renderer),
      physics_(world, registry),
      lighting_(world, registry) {
    physics_.border = &border;
    player_.pos[0] = start.x, player_.pos[1] = start.y, player_.pos[2] = start.z;
    std::copy(player_.pos, player_.pos + 3, player_.prev);
    player_.yaw = float(start.yaw * kPi / 180.0);
    player_.pitch = float(start.pitch * kPi / 180.0);
    const char* defaults[9] = {"minecraft:grass_block[snowy=false]", "minecraft:dirt", "minecraft:stone",
                               "minecraft:cobblestone", "minecraft:oak_planks", "minecraft:oak_log[axis=y]",
                               "minecraft:glass", "minecraft:torch", "minecraft:glowstone"};
    for (int i = 0; i < 9; ++i) hotbar_[i] = registry_.idFor(defaults[i]);
}

CameraState Game::camera() const {
    CameraState c;
    if (cameraEntity >= 0 && entityView && entityView(cameraEntity, c)) return c;
    double eye[3];
    if (riding() && rideEye && rideEye(eye)) {  // the seat of the vehicle as it is drawn
        c.x = eye[0], c.y = eye[1], c.z = eye[2];
        c.yaw = player_.yaw, c.pitch = player_.pitch;
        return c;
    }
    const double t = alpha_;
    c.x = player_.prev[0] + (player_.pos[0] - player_.prev[0]) * t;
    c.y = player_.prev[1] + (player_.pos[1] - player_.prev[1]) * t + (eyeHeightOld_ + (eyeHeight_ - eyeHeightOld_) * t);
    c.z = player_.prev[2] + (player_.pos[2] - player_.prev[2]) * t;
    c.yaw = player_.yaw;
    c.pitch = player_.pitch;
    return c;
}

// GameRenderer.renderLevel: bobHurt then bobView multiply the projection (P * Hurt * Bob * View). As a view basis:
// rows R * (right, up, back) and the camera moved by the view-space translation.
static CameraState applyBob(const Game& g, CameraState c) {
    // GameRenderer.bobHurt / bobView follow the camera entity: our own hurt, death and walk bob not while spectating
    // another entity through its eyes
    if (g.cameraEntity >= 0) return c;
    const float pt = g.partialTick();
    constexpr float kPi = 3.14159265f, kDeg = kPi / 180.0f;
    struct M3 {
        float m[3][3];
    };
    auto mul = [](const M3& a, const M3& b) {
        M3 r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        return r;
    };
    auto rotX = [](float a) { return M3{{{1, 0, 0}, {0, std::cos(a), -std::sin(a)}, {0, std::sin(a), std::cos(a)}}}; };
    auto rotY = [](float a) { return M3{{{std::cos(a), 0, std::sin(a)}, {0, 1, 0}, {-std::sin(a), 0, std::cos(a)}}}; };
    auto rotZ = [](float a) { return M3{{{std::cos(a), -std::sin(a), 0}, {std::sin(a), std::cos(a), 0}, {0, 0, 1}}}; };
    M3 rot{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    float t[3] = {0, 0, 0};
    bool any = false;
    if (g.dead) {  // bobHurt: the death roll
        const float duration = std::min(float(g.deathTime) + pt, 20.0f);
        rot = mul(rot, rotZ((40.0f - 8000.0f / (duration + 200.0f)) * kDeg));
        any = true;
    }
    if (g.hurtTime > 0) {
        float hurt = (float(g.hurtTime) - pt) / float(g.hurtDuration);
        hurt = std::sin(hurt * hurt * hurt * hurt * kPi);
        rot = mul(rot, rotY(-g.hurtDir * kDeg));
        rot = mul(rot, rotZ(-hurt * 14.0f * g.damageTilt * kDeg));
        rot = mul(rot, rotY(g.hurtDir * kDeg));
        any = true;
    }
    if (g.bobView) {
        const float wd = -(g.walkDist + (g.walkDist - g.walkDistO) * pt);
        const float b = g.bobO + (g.bob - g.bobO) * pt;
        if (b != 0) {
            const float tx = std::sin(wd * kPi) * b * 0.5f, ty = -std::abs(std::cos(wd * kPi) * b);
            const float tr[3] = {tx, ty, 0};
            for (int i = 0; i < 3; ++i) t[i] = rot.m[i][0] * tr[0] + rot.m[i][1] * tr[1] + rot.m[i][2] * tr[2];
            rot = mul(rot, rotZ(std::sin(wd * kPi) * b * 3.0f * kDeg));
            rot = mul(rot, rotX(std::abs(std::cos(wd * kPi - 0.2f) * b) * 5.0f * kDeg));
            any = true;
        }
    }
    if (!any) return c;
    const float cy = std::cos(c.yaw), sy = std::sin(c.yaw), cp = std::cos(c.pitch), sp = std::sin(c.pitch);
    const float fwd[3] = {-sy * cp, -sp, cy * cp};
    float right[3] = {fwd[1] * 0 - fwd[2] * 1, fwd[2] * 0 - fwd[0] * 0, fwd[0] * 1 - fwd[1] * 0};  // fwd x (0, 1, 0)
    const float rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    for (float& v : right) v /= std::max(rl, 1e-6f);
    const float up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2], right[0] * fwd[1] - right[1] * fwd[0]};
    const float back[3] = {-fwd[0], -fwd[1], -fwd[2]};
    const float* rows[3] = {right, up, back};
    float out[3][3];
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) out[i][k] = rot.m[i][0] * rows[0][k] + rot.m[i][1] * rows[1][k] + rot.m[i][2] * rows[2][k];
    c.basisSet = true;
    for (int k = 0; k < 3; ++k) c.right[k] = out[0][k], c.up[k] = out[1][k], c.fwd[k] = -out[2][k];
    // Camera offset: c' = c - (t.x * right' + t.y * up' + t.z * back')
    c.x -= t[0] * out[0][0] + t[1] * out[1][0] + t[2] * out[2][0];
    c.y -= t[0] * out[0][1] + t[1] * out[1][1] + t[2] * out[2][1];
    c.z -= t[0] * out[0][2] + t[1] * out[1][2] + t[2] * out[2][2];
    return c;
}

CameraState Game::handCamera() const {
    CameraState c = camera();
    if (cameraMode == 0) return c;
    if (cameraMode == 2) c.yaw += float(kPi), c.pitch = -c.pitch;
    return c;
}

CameraState Game::viewCamera() const {
    CameraState c = camera();
    if (viewCameraOverride) viewCameraOverride(c);
    if (cameraMode == 0) return applyBob(*this, c);
    if (cameraMode == 2) c.yaw += float(kPi), c.pitch = -c.pitch;
    const double cp = std::cos(c.pitch);
    const double back[3] = {std::sin(c.yaw) * cp, std::sin(c.pitch), -std::cos(c.yaw) * cp};
    double dist = cameraDistance;  // Camera.getMaxZoom: 8 rays from jittered eye positions
    for (int i = 0; i < 8; ++i) {
        const double o[3] = {c.x + ((i & 1) * 2 - 1) * 0.1, c.y + (((i >> 1) & 1) * 2 - 1) * 0.1,
                             c.z + (((i >> 2) & 1) * 2 - 1) * 0.1};
        const RayHit h = physics_.raycast(o, back, dist, true);  // ClipContext.Block.VISUAL
        if (!h.hit) continue;
        const double dx = h.point[0] - c.x, dy = h.point[1] - c.y, dz = h.point[2] - c.z;
        dist = std::min(dist, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    c.x += back[0] * dist, c.y += back[1] * dist, c.z += back[2] * dist;
    return applyBob(*this, c);
}

bool Game::held(Window& w, int vk) const { return vk > 0 && w.mouseCaptured() && w.keyDown(vk); }

PlayerInput Game::readInput(Window& w) {
    PlayerInput in;
    if (!w.mouseCaptured()) return in;
    in.forward = float(held(w, keys.forward)) - float(held(w, keys.back));
    in.strafe = float(held(w, keys.left)) - float(held(w, keys.right));
    in.jump = held(w, keys.jump);
    // ToggleKeyMapping: with the toggle option a press switches sneaking / sprinting on or off.
    if (toggleCrouch && presses(keys.sneak) % 2) crouchToggled_ = !crouchToggled_;
    if (toggleSprint && presses(keys.sprint) % 2) sprintToggled_ = !sprintToggled_;
    in.sneak = toggleCrouch ? crouchToggled_ : held(w, keys.sneak);
    in.sprint = toggleSprint ? sprintToggled_ : held(w, keys.sprint);
    return in;
}

void Game::frame(Window& w, double dt) {
    float mdx, mdy;
    w.takeMouseDelta(mdx, mdy);
    presses_ = w.takePresses();
    if (paused) {  // Minecraft.isPaused: nothing ticks; input is dropped
        w.takeMouseEvents();
        buildOverlay();
        return;
    }
    clock_ += dt;
    if (inputSuspended) mdx = mdy = 0, presses_ = {};
    if (invertMouseX) mdx = -mdx;
    if (invertMouseY) mdy = -mdy;
    if (!inputSuspended) {  // MouseHandler.turnPlayer
        double dx, dy;
        if (smoothCamera) {
            // getNewDeltaValue(accumulated * sens, mousea * sens): the time scaled by sens too (sensitivity / 0.15 degrees)
            const double sens = sensitivity / (0.15 * kPi / 180.0);
            dx = smoothTurnX_.next(mdx * sensitivity, dt * sens), dy = smoothTurnY_.next(mdy * sensitivity, dt * sens);
        } else {
            smoothTurnX_ = {}, smoothTurnY_ = {};
            const float k = cameraMode == 0 && scoping() ? sensitivity / 8 : sensitivity;  // the spyglass: no x8
            dx = mdx * k, dy = mdy * k;
        }
        player_.yaw += float(dx);
        player_.pitch = std::clamp(player_.pitch + float(dy), -1.5707964f, 1.5707964f);  // Entity.setXRot: +-90 degrees
    }

    for (int n = presses(keys.perspective); n > 0; --n) cameraMode = (cameraMode + 1) % 3;  // CameraType.cycle per click
    const bool esc = w.keyDown(VK_ESCAPE);
    if (handleEscape && esc && !escWasDown_) w.setMouseCaptured(false);
    escWasDown_ = esc;
    const bool debugHeld = keys.debugModifier && w.keyDown(keys.debugModifier);  // F3 + 1..4 are charts
    for (int i = 0; i < 9; ++i)
        if (presses(keys.hotbar[i]) && !debugHeld && !(spectatorInput && spectatorInput(0, i)) && !(hotbarKey && hotbarKey(i))) selected_ = i;
    const PlayerInput input = inputSuspended ? PlayerInput{} : readInput(w);
    const Window::MouseEvents ev = w.takeMouseEvents();
    if (inputSuspended) pendingAttacks_ = 0;
    else pendingAttacks_ = std::min(pendingAttacks_ + presses(keys.attack), 4);
    // ToggleKeyMapping (Toggle Attack / Toggle Use): a press flips the key; releasing to a screen resets it (mouse keys
    // are not restored).
    if (inputSuspended || !w.mouseCaptured()) attackToggled_ = useToggled_ = false;
    if (toggleAttack && presses(keys.attack) % 2) attackToggled_ = !attackToggled_;
    if (toggleUse && presses(keys.use) % 2) useToggled_ = !useToggled_;
    const bool attackDown = !inputSuspended && (toggleAttack ? attackToggled_ : held(w, keys.attack));
    tickAccumulator_ += std::min(dt, 0.25);
    const double tick = tickRate.tickSeconds();  // 20 ticks per second, or the server's slower rate
    while (tickAccumulator_ >= tick) {
        // Minecraft.tick: the keybinds (attack / digging) with a fresh pick, then the player.
        ++ticks_;
        tickRate.tick();                   // TickRateManager.tick
        if (tickRate.runs) border.tick();  // ClientLevel.tick
        alpha_ = 1.0;
        retarget();
        if (inputSuspended) missTime_ = 10000;
        else tickAttack(attackDown);
        // LivingEntity.updatingUsingItem, then LocalPlayer.modifyInput / sprinting with the use's slowdown.
        if (use.active) {
            if (onUseTick) onUseTick();
            if (use.active) --use.remaining;  // below 0 too (useItemRemaining is not clamped)
        }
        player_.useSpeed = use.active ? use.speed : 1.0f;
        player_.useBlocksSprint = use.active && !use.canSprint;
        eyeHeightOld_ = eyeHeight_;
        eyeHeight_ += ((sleeping ? 0.2 : player_.eyeHeight()) - eyeHeight_) * 0.5;  // Camera.tick (SLEEPING_DIMENSIONS: 0.2)
        player_.asleep = sleeping;
        if (riding()) {  // a passenger does not move itself (LocalPlayer.rideTick)
            player_.lastInput = input;
            player_.sneaking = false;  // LocalPlayer.aiStep: crouching needs !isPassenger
            rideTick(input);
        } else if (cameraEntity >= 0) {  // spectating another entity: the keys reach the server, the player stays
            player_.tick(physics_, PlayerInput{});
            player_.lastInput = input;
        } else {
            refreshPistons();
            player_.tickPistons(physics_);  // the moving blocks' ticks (ClientLevel block entities)
            player_.tick(physics_, input);
            if (player_.onGround && !player_.lastInput.sneak)  // Entity.move: stepOn the block below (not stepping carefully)
                redstoneOreInteract(int(std::floor(player_.pos[0])), int(std::floor(player_.pos[1] - 0.2)), int(std::floor(player_.pos[2])), true);
            if (entityPush) entityPush(player_);  // other entities' pushEntities / boats push the player
            if (blockEntityTick) blockEntityTick(player_);
        }
        if (traceTicks)
            LOG("tick %llu: pos %.4f %.4f %.4f vel %.4f %.4f %.4f ground %d hcol %d water %.3f lava %.3f eye %d swim %d sprint %d "
                "fly %d glide %d pose %d spin %d", (unsigned long long)ticks_, player_.pos[0], player_.pos[1], player_.pos[2],
                player_.vel[0], player_.vel[1], player_.vel[2], int(player_.onGround), int(player_.horizontalCollision),
                player_.waterHeight, player_.lavaHeight, int(player_.eyeInWater), int(player_.swimming), int(player_.sprinting),
                int(player_.flying), int(player_.fallFlying), int(player_.pose), player_.autoSpinAttackTicks);
        tickCameraFeedback();
        ++cooldownTick_;  // Player.tick -> ItemCooldowns.tick
        std::erase_if(cooldowns_, [&](const auto& c) { return c.second.end <= cooldownTick_; });
        if (onPlayerTick) onPlayerTick();
        tickAccumulator_ -= tick;
    }
    alpha_ = tickAccumulator_ / tick;
    if (ticks_ % 20 == 0)  // LevelRenderer.tick: cracks nobody updated for 400 ticks go away
        for (auto it = destructions_.begin(); it != destructions_.end();)
            it = ticks_ > it->second.tick + 400 ? destructions_.erase(it) : std::next(it);

    retarget();
    handleClicks(w, dt, presses(keys.use), presses(keys.pickItem), ev.wheel);
    buildOverlay();
}

// The moving blocks the block entity layer animates, keeping each one's tick progress (keyed by its identity).
void Game::refreshPistons() {
    if (!pistonSource) return;
    std::vector<Physics::Piston> fresh;
    pistonSource(fresh);
    for (Physics::Piston& p : fresh)
        for (const Physics::Piston& old : physics_.pistons)
            if (old.key == p.key) p.progress = old.progress, p.progressO = old.progressO;
    physics_.pistons = std::move(fresh);
}

void Game::retarget() {
    const CameraState cam = camera();
    const double origin[3] = {cam.x, cam.y, cam.z};
    const double cp = std::cos(cam.pitch);
    const double dir[3] = {-std::sin(cam.yaw) * cp, -std::sin(cam.pitch), std::cos(cam.yaw) * cp};
    physics_.holdingScaffolding = hotbar_[selected_] && registry_.name(hotbar_[selected_]).starts_with("minecraft:scaffolding");
    target_ = physics_.raycast(origin, dir, std::max(blockReach, entityReach));
    blockDistance_ = target_.hit ? target_.distance : -1.0;
    if (target_.hit && !(target_.distance < blockReach)) target_.hit = false;  // filterHitResult: a miss
    if (entityBlocksTarget) target_.hit = false;  // an entity in front, out of reach: the whole hit is a miss
}

void Game::simulate(const PlayerInput& input, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        player_.tick(physics_, input);
        if (onPlayerTick) onPlayerTick();
    }
    alpha_ = 1.0;
    retarget();
}

void Game::handleClicks(Window& w, double dt, int rightPresses, int middlePresses, int wheel) {
    struct {
        int right, middle, wheel;
    } ev{rightPresses, middlePresses, wheel};
    if (inputSuspended) return;
    if (ev.wheel) {  // MouseHandler.onScroll (discrete scrolling, scroll sensitivity) and ScrollWheelHandler
        const double scroll = (discreteScroll ? (ev.wheel > 0 ? 1.0 : -1.0) : double(ev.wheel)) * wheelSensitivity;
        if (scrollAccum_ != 0 && (scroll > 0) != (scrollAccum_ > 0)) scrollAccum_ = 0;
        scrollAccum_ += scroll;
        const int steps = int(scrollAccum_);
        scrollAccum_ -= steps;
        // ScrollWheelHandler.getNextScrollWheelSelection: one slot by the step's sign
        if (steps && !(spectatorInput && spectatorInput(1, steps))) selected_ = ((selected_ - (steps > 0 ? 1 : -1)) % 9 + 9) % 9;
    }
    const bool rightHeld = toggleUse ? useToggled_ : held(w, keys.use);
    repeatTimer_ -= dt;
    // Breaking runs in tickAttack; Minecraft.startUseItem does nothing while a block is being destroyed or the hands
    // are busy (paddling).
    // Held: Minecraft.handleKeybinds repeats startUseItem every 4 ticks (rightClickDelay), not while an item is in use.
    const bool doPlace = (ev.right > 0 || (rightHeld && repeatTimer_ <= 0 && !use.active)) && !dig_.active && !handsBusy;
    if (doPlace) repeatTimer_ = 0.2;  // rightClickDelay 4 (ticks)
    if (usingItem_) ev.middle = 0;  // Minecraft.handleKeybinds: pick clicks are dropped while an item is in use
    if (ev.middle && spectatorInput && spectatorInput(2, 0)) ev.middle = 0;
    if (ev.middle && onPickEntity && onPickEntity(target_.hit ? blockDistance_ : -1.0)) {
        // Minecraft.pickBlockOrEntity: the entity under the crosshair
    } else if (ev.middle && target_.hit) {
        if (onPickBlock) onPickBlock(target_.x, target_.y, target_.z);
        else hotbar_[selected_] = world_.block(target_.x, target_.y, target_.z);
    }
    // An entity in front of the targeted block takes the click (attack / use only on a press, like the game).
    if (usingItem_ && !rightHeld) {  // Minecraft.handleKeybinds: releaseUsingItem
        usingItem_ = false;
        if (predictRelease && use.active) predictRelease();  // Item.releaseUsing on the client (riptide)
        use.active = false;  // Player.releaseUsingItem: the client stops at once
        if (onReleaseUseItem) onReleaseUseItem();
    }
    if (doPlace && onEntityClick && onEntityClick(false, true, blockDistance_)) {
        repeatTimer_ = 0.2;  // rightClickDelay: held, it repeats every 4 ticks
        // Minecraft.startUseItem on an entity: each hand's interaction, then that hand's item use, until one goes through
        for (int hand = 0; hand < 2; ++hand) {
            if (entityInteract && entityInteract(hand) == 1) {  // Success: the hand swings
                if (onSwing) {
                    const int attackSwing = swingTicks;
                    swingTicks = useSwingTicksOf[hand];
                    onSwing(hand, useSwingTypeOf[hand]);
                    swingTicks = attackSwing;
                }
                return;
            }
            if (!usingItem_ && startUseItem(true, hand)) return;
        }
        return;
    }
    // Minecraft.startUseItem: a press, or every 4 ticks while held (eating the next piece of food), unless an item is
    // in use.
    if (doPlace && !use.active) startUseItem(false);
}

bool Game::startUseItem(bool afterEntity, int onlyHand) {
    useConsumed_ = false;
    for (int hand = onlyHand < 0 ? 0 : onlyHand; hand < (onlyHand < 0 ? 2 : onlyHand + 1); ++hand) {
        if (!afterEntity && target_.hit) {
            useOnTakesItem = placedLast_ = false;
            const int r = useItemOn(hand);
            if (r != kUsePass) {  // consumed (placed, used the block) or failed: done
                useConsumed_ = true;
                // A use that took one of the stack (a placed block, an egg...) or any use in creative
                // (hasInfiniteMaterials) moves the hand.
                const bool hasItem = handHasItem ? handHasItem(hand) : hand == 0;
                if (r == kUseConsume && hasItem && (placedLast_ || useOnTakesItem || instabuild) && onItemUsed) onItemUsed(hand);
                return true;
            }
        }
        const bool hasItem = handHasItem ? handHasItem(hand) : (hand == 0 && hotbar_[selected_] != 0);
        if (!hasItem) continue;
        if (onUseItem) onUseItem(hand);  // ServerboundUseItemPacket (sent even while the item cools down)
        usingItem_ = true;
        if (predictItemUse && predictItemUse(hand, afterEntity || !target_.hit ? nullptr : &target_) == kUseConsume) {
            if (onItemUsed) onItemUsed(hand);
            return true;
        }
    }
    return false;
}

bool Game::useTarget(bool place) {
    if (!target_.hit) return false;
    if (!place) {
        EditAction action{place, target_.x, target_.y, target_.z, target_.face,
                          {target_.point[0], target_.point[1], target_.point[2]}, {target_.x, target_.y, target_.z}, 0};
        if (onEdit) onEdit(action);
        const u16 old = world_.block(target_.x, target_.y, target_.z);
        edit(target_.x, target_.y, target_.z, 0);
        updateNeighbourShapes(target_.x, target_.y, target_.z, old);
        if (onBlockChanged && old) onBlockChanged(false, target_.x, target_.y, target_.z, old);
        if (onSwing) onSwing(0, swingType);
        return true;
    }
    useItemOn(0);
    return placedLast_;
}

// MultiPlayerGameMode.useItemOn / performUseItemOn for a hand: the targeted block's own use (unless sneaking with
// something in hand), else ItemStack.useOn (BlockItem.place for blocks: slabs double up, waterloggable blocks fill
// with the water they replace, nothing goes into the player). ServerboundUseItemOnPacket goes out either way (with
// the predicted block), except outside the world border.
int Game::useItemOn(int hand) {
    placedLast_ = false;
    if (!border.blockWithinBounds(target_.x, target_.z)) return kUseFail;
    EditAction action{true, target_.x, target_.y, target_.z, target_.face,
                      {target_.point[0], target_.point[1], target_.point[2]}, {target_.x, target_.y, target_.z}, 0};
    action.hand = hand;
    action.inside = target_.inside;
    auto send = [&](int result) {
        if (onEdit && !placeOnly_) onEdit(action);
        return result;
    };
    if (gameMode == 3) return send(kUseConsume);
    auto has = [&](int h) { return handHasItem ? handHasItem(h) : (h == 0 && hotbar_[selected_] != 0); };
    const bool suppressUsingBlock = player_.lastInput.sneak && (has(0) || has(1));  // isSecondaryUseActive
    // NoteBlock.useItemOn: a head (#noteblock_top_instruments) on the top face goes on to the item's own use
    const bool headOnNoteBlock = target_.face == 1 && handInTag && handInTag(hand, "minecraft:noteblock_top_instruments") &&
                                 registry_.name(world_.block(target_.x, target_.y, target_.z)).starts_with("minecraft:note_block[");
    if (!placeOnly_ && !suppressUsingBlock && !headOnNoteBlock && onInteract && onInteract(target_.x, target_.y, target_.z))
        return send(kUseConsume);
    // RedStoneOreBlock.useItemOn on the client: its particles; SUCCESS unless a block item goes on to be placed
    if (!placeOnly_ && !suppressUsingBlock && redstoneOreInteract(target_.x, target_.y, target_.z, false) &&
        !(has(hand) && (hand == 0 ? hotbar_[selected_] : offhandBlock_) != 0))
        return send(kUseConsume);
    const u16 held = hand == 0 ? hotbar_[selected_] : offhandBlock_;
    if (!has(hand) || (handOnCooldown && handOnCooldown(hand))) return send(kUsePass);
    if (!placeOnly_ && held && (registry_.name(held).starts_with("minecraft:lily_pad") || registry_.name(held).starts_with("minecraft:frogspawn")))
        return send(kUsePass);  // PlaceOnWaterBlockItem.useOn: PASS (its use places it)
    if (!held) return send(predictUseOn ? predictUseOn(hand, target_) : kUsePass);  // not a block: Item.useOn
    // BlockItem.place: BlockPlaceContext (the clicked block when it can be replaced, else the cell in front of it).
    const BlockInfo* infos = registry_.infos();
    const place::Context pc = placeContext();
    int t[3] = {target_.x, target_.y, target_.z};
    const bool replaceClicked = place::canBeReplaced(pc, held, world_.block(t[0], t[1], t[2]), true, t);
    if (!replaceClicked)
        for (int a = 0; a < 3; ++a) t[a] += kDir[target_.face][a];
    const u16 existing = world_.block(t[0], t[1], t[2]);
    if (!replaceClicked && !place::canBeReplaced(pc, held, existing, false, t)) return send(kUseFail);  // !context.canPlace()
    place::Context at = pc;
    if (registry_.name(held).starts_with("minecraft:scaffolding")) {  // updatePlacementContext
        if (!place::scaffoldingTarget(pc, t, at.face)) return send(kUseFail);
        if (t[0] != target_.x || t[1] != target_.y || t[2] != target_.z)  // BlockPlaceContext.at: the face's centre
            for (int a = 0; a < 3; ++a) at.hit[a] = t[a] + 0.5 + kDir[at.face][a] * 0.5;
    }
    const place::Result placed = place::placementState(at, held, t, replaceClicked);  // getPlacementState
    if (!placed.state) return send(kUseFail);
    const u16 id = placed.state;
    const BlockInfo& b = infos[id];
    const AABB body = player_.box();
    float off[3];
    BlockRegistry::offset(b, t[0], t[2], off);
    for (u32 i = 0; i < b.collisionCount; ++i) {  // Level.isUnobstructed: never into the player
        const Box& s = registry_.box(b.collisionFirst + i);
        const double x = t[0] + off[0], y = t[1] + off[1], z = t[2] + off[2];
        const AABB a{{x + s.min[0], y + s.min[1], z + s.min[2]}, {x + s.max[0], y + s.max[1], z + s.max[2]}};
        if (a.intersects(body)) return send(kUseFail);
    }
    std::copy(t, t + 3, action.at);
    action.block = id;
    send(kUseConsume);
    edit(t[0], t[1], t[2], id);
    for (const auto& [x, y, z, s] : placed.extra) edit(x, y, z, u16(s));  // setPlacedBy: door / plant tops, bed heads
    updateNeighbourShapes(t[0], t[1], t[2], existing);
    for (const auto& [x, y, z, s] : placed.extra) updateNeighbourShapes(x, y, z, 0);
    placedLast_ = true;
    if (onBlockChanged) onBlockChanged(true, t[0], t[1], t[2], id);
    if (onSwing) {  // a use swing: the used hand's interact_animation
        const int attackSwing = swingTicks;
        swingTicks = useSwingTicksOf[hand & 1];
        onSwing(hand & 1, useSwingTypeOf[hand & 1]);
        swingTicks = attackSwing;
    }
    return kUseConsume;
}

int Game::placeFromUse(int hand, const RayHit& hit) {
    const RayHit saved = target_;
    target_ = hit, placeOnly_ = true;
    const int r = useItemOn(hand);
    target_ = saved, placeOnly_ = false;
    return r;
}

float Game::useFovModifier() const {
    if (!use.active || use.anim != 4) return 1.0f;  // ItemRules::Anim::Bow
    const float scale = std::min(float(use.ticksUsing()) / 20.0f, 1.0f);
    return 1.0f - scale * scale * 0.15f;
}

bool Game::scoping() const { return use.active && use.anim == 7; }  // ItemRules::Anim::Spyglass

float Game::cooldownPercent(const std::string& group, float partial) const {
    auto it = cooldowns_.find(group);
    if (it == cooldowns_.end()) return 0.0f;
    const float duration = float(it->second.end - it->second.start);
    const float remaining = float(it->second.end) - (float(cooldownTick_) + partial);
    return std::clamp(remaining / duration, 0.0f, 1.0f);
}

void Game::tickCameraFeedback() {
    // LocalPlayer.move -> addWalkedDistance; AbstractClientPlayer.updateBob (resetBob while riding).
    walkDistO = walkDist;
    const double dx = player_.pos[0] - player_.prev[0], dz = player_.pos[2] - player_.prev[2];
    if (!riding()) walkDist += float(std::sqrt(dx * dx + dz * dz)) * 0.6f;
    bobO = bob;
    if (riding()) {
        bob = 0;
    } else {
        const float tBob = player_.onGround && !dead && !player_.swimming && !player_.flying
                               ? std::min(0.1f, float(std::sqrt(player_.vel[0] * player_.vel[0] + player_.vel[2] * player_.vel[2])))
                               : 0.0f;
        bob += (tBob - bob) * 0.4f;
    }
    if (hurtTime > 0) --hurtTime;
    if (dead && deathTime < 20) ++deathTime;
    if (!dead) deathTime = 0;
    ++attackStrengthTicker;  // Player.tick
    if (kineticHitTicks >= 0) ++kineticHitTicks;
}

// ---------------------------------------------------------------------------------------------------------
// Mining (Minecraft.startAttack / continueAttack, MultiPlayerGameMode)

void Game::destroyBlockProgress(int id, int x, int y, int z, int stage) {
    if (stage >= 0 && stage < 10) destructions_[id] = {x, y, z, stage, ticks_};
    else destructions_.erase(id);
}

float Game::progressAt(int x, int y, int z, u16 state) const {
    return destroyProgress ? destroyProgress(x, y, z, state) : 1.0f;
}

void Game::sendDig(int action, int x, int y, int z, int face, bool predicted) {
    if (!onEdit) return;
    EditAction a;
    a.place = false;
    a.x = a.at[0] = x, a.y = a.at[1] = y, a.z = a.at[2] = z;
    a.face = face;
    std::copy(target_.point, target_.point + 3, a.hit);
    a.action = action;
    a.predicted = predicted;
    onEdit(a);
}

bool Game::destroyBlock(int x, int y, int z) {
    const u16 old = world_.block(x, y, z);
    if (!old) return false;
    if (instabuild && canDestroyInCreative && !canDestroyInCreative()) return false;
    const BlockInfo& b = registry_.infos()[old];
    edit(x, y, z, b.waterlogged ? registry_.waterId() : 0);  // FluidState.createLegacyBlock
    updateNeighbourShapes(x, y, z, old);  // Level.setBlock (UPDATE_ALL): the neighbours' updateShape
    if (onBlockChanged) onBlockChanged(false, x, y, z, old);
    return true;
}

// MultiPlayerGameMode.startDestroyBlock (the packet goes out after the prediction, like startPrediction).
bool Game::startDestroyBlock(int x, int y, int z, int face) {
    // Player.blockActionRestricted: spectators never, adventure only with a matching can_break on the held item.
    if (gameMode == 3 || (gameMode == 2 && !(adventureMayBreak && adventureMayBreak(x, y, z)))) return false;
    if (!border.blockWithinBounds(x, z)) return false;
    if (instabuild) {
        const bool breaks = !(canDestroyInCreative && !canDestroyInCreative()) && world_.block(x, y, z) != 0;
        sendDig(kStartDestroy, x, y, z, face, breaks);
        if (breaks) destroyBlock(x, y, z);
        destroyDelay_ = 5;
        return true;
    }
    const u64 held = heldIdentity ? heldIdentity() : 0;
    if (!dig_.active || !(dig_.x == x && dig_.y == y && dig_.z == z && dig_.held == held)) {
        if (dig_.active) sendDig(kAbortDestroy, dig_.x, dig_.y, dig_.z, face, false);
        const u16 state = world_.block(x, y, z);
        const bool notAir = state != 0;
        if (notAir && (!dig_.active || dig_.progress == 0.0f)) redstoneOreInteract(x, y, z, true);  // BlockState.attack
        if (notAir && progressAt(x, y, z, state) >= 1.0f) {
            sendDig(kStartDestroy, x, y, z, face, true);
            destroyBlock(x, y, z);
        } else {
            sendDig(kStartDestroy, x, y, z, face, false);
            if (dig_.active) destroyBlockProgress(kLocalBreaker, dig_.x, dig_.y, dig_.z, -1);
            dig_ = {true, x, y, z, face, held, 0.0f, 0};
            if (onBreakingEffects) onBreakingEffects(x, y, z, face, true);
        }
    }
    return true;
}

void Game::stopDestroyBlock() {
    if (!dig_.active) return;
    sendDig(kAbortDestroy, dig_.x, dig_.y, dig_.z, 0, false);  // Direction.DOWN
    dig_.active = false;
    dig_.progress = 0;
    destroyBlockProgress(kLocalBreaker, dig_.x, dig_.y, dig_.z, -1);
    resetAttackStrengthTicker();
}

bool Game::continueDestroyBlock(int x, int y, int z, int face) {
    if (destroyDelay_ > 0) {
        --destroyDelay_;
        return true;
    }
    if (instabuild && border.blockWithinBounds(x, z)) {
        destroyDelay_ = 5;
        const bool breaks = !(canDestroyInCreative && !canDestroyInCreative()) && world_.block(x, y, z) != 0;
        sendDig(kStartDestroy, x, y, z, face, breaks);
        if (breaks) destroyBlock(x, y, z);
        return true;
    }
    const u64 held = heldIdentity ? heldIdentity() : 0;
    if (!(dig_.active && dig_.x == x && dig_.y == y && dig_.z == z && dig_.held == held)) return startDestroyBlock(x, y, z, face);
    const u16 state = world_.block(x, y, z);
    if (!state) {
        dig_.active = false;
        return false;
    }
    dig_.progress += progressAt(x, y, z, state);
    ++dig_.ticks;
    if (dig_.progress >= 1.0f) {
        dig_.active = false;
        sendDig(kStopDestroy, x, y, z, face, true);
        destroyBlock(x, y, z);
        dig_.face = -1;
        dig_.progress = 0;
        dig_.ticks = 0;
        destroyDelay_ = 5;
    } else if (dig_.face != face) {
        dig_.face = face;
        sendDig(kChangeDestroyDirection, x, y, z, face, false);
    }
    // getDestroyStage: (int) (progress * 10) once there is progress, else none.
    destroyBlockProgress(kLocalBreaker, dig_.x, dig_.y, dig_.z, dig_.progress > 0 ? int(dig_.progress * 10.0f) : -1);
    return true;
}

bool Game::startAttack() {
    if (missTime_ > 0 || handsBusy) return false;
    if (gameMode == 3) {  // spectators: no attack, no digging; an entity under the crosshair is spectated
        if (onSpectatorAttack) onSpectatorAttack(blockDistance_);
        return true;
    }
    // Player.cannotAttackWithItem: a weapon with a minimum_attack_charge waits for its charge (no swing either).
    if (heldMinimumCharge > 0 && float(attackStrengthTicker) / attackStrengthDelay() < heldMinimumCharge) return false;
    if (heldPiercing) {  // MultiPlayerGameMode.piercingAttack: a stab (the server finds what it hits)
        if (onStab) onStab();
        if (onStabEffects) onStabEffects();
        attackStrengthTicker = 0;  // Player.onAttack
        if (onSwing) onSwing(0, swingType);
        return true;
    }
    // An entity in front of the targeted block takes the press (the attack packet carries its own punch).
    if (onEntityClick && onEntityClick(true, false, blockDistance_)) {
        // MultiPlayerGameMode.attack -> resetAttackStrengthTicker (not for a weapon's out-of-range swing), and a
        // creative hit delays the next block breaking (destroyDelay 5)
        if (!entityAttackMissed || !entityAttackMissed()) {
            resetAttackStrengthTicker();
            if (gameMode == 1) destroyDelay_ = 5;
        }
        if (onSwing) onSwing(0, swingType);
        return false;
    }
    bool endAttack = false;
    if (target_.hit && world_.block(target_.x, target_.y, target_.z) != 0) {
        startDestroyBlock(target_.x, target_.y, target_.z, target_.face);
        endAttack = world_.block(target_.x, target_.y, target_.z) == 0;
    } else {
        if (!instabuild) missTime_ = 10;  // MultiPlayerGameMode.hasMissTime: survival-like modes
        resetAttackStrengthTicker();         // resetAttackStrengthTicker
    }
    if (onSwing) onSwing(0, swingType);
    if (onPunch) onPunch();
    return endAttack;
}

void Game::continueAttack(bool down) {
    if (!down) missTime_ = 0;
    if (missTime_ > 0 || use.active || heldPiercing) return;  // piercing weapons do not break blocks
    // Minecraft.continueAttack: only while the hit result is the block (an entity in front stops the breaking)
    if (down && target_.hit && crosshairEntity < 0) {  // a block hit result (an air block: nothing this tick)
        if (world_.block(target_.x, target_.y, target_.z) != 0 && continueDestroyBlock(target_.x, target_.y, target_.z, target_.face)) {
            if (onSwing) onSwing(0, swingType);
            if (onPunch) onPunch();
        }
    } else {
        stopDestroyBlock();
    }
}

void Game::tickAttack(bool down) {
    bool instantAttack = false;
    if (use.active) {
        pendingAttacks_ = 0;  // Minecraft.handleKeybinds: clicks are consumed while an item is in use
    } else {
        for (; pendingAttacks_ > 0; --pendingAttacks_) instantAttack |= startAttack();
    }
    continueAttack(down && !instantAttack);
    if (missTime_ > 0) --missTime_;
}

place::Context Game::placeContext() {
    place::Context c{registry_};
    c.block = [this](int x, int y, int z) { return world_.block(x, y, z); };
    c.brightness = [this](int x, int y, int z) {
        if (y < world_.minSectionY() * 16 || y >= (world_.maxSectionY() + 1) * 16) return 15;
        const u8 l = world_.light(x >> 4, y >> 4, z >> 4).at(x & 15, y & 15, z & 15);
        return std::max(l >> 4, l & 15);
    };
    c.waterSource = [this](int x, int y, int z) {
        const Physics::Fluid f = physics_.fluid(x, y, z);
        return f.type == 1 && f.source;
    };
    c.clicked[0] = target_.x, c.clicked[1] = target_.y, c.clicked[2] = target_.z;
    c.face = target_.face;
    std::copy(target_.point, target_.point + 3, c.hit);
    c.yaw = player_.yaw, c.pitch = player_.pitch;
    c.sneaking = player_.lastInput.sneak;
    c.inside = target_.inside;
    c.maxY = world_.maxSectionY() * 16 + 15;
    return c;
}

// Level.setBlock's neighbour shape updates for a changed cell (connections, stair shapes, double chests, the other
// halves of doors, tall plants and beds), as the client's prediction does them.
void Game::updateNeighbourShapes(int x, int y, int z, u16 old) {
    std::vector<std::array<int, 4>> changes;
    place::neighbourShapes(placeContext(), x, y, z, old, changes);
    std::vector<u16> before;
    for (const auto& [cx, cy, cz, s] : changes) before.push_back(world_.block(cx, cy, cz)), edit(cx, cy, cz, u16(s));
    for (size_t i = 0; i < changes.size(); ++i)  // a removed half's own neighbours (its pair's other cells)
        if (changes[i][3] == 0) {
            std::vector<std::array<int, 4>> more;
            place::neighbourShapes(placeContext(), changes[i][0], changes[i][1], changes[i][2], before[i], more);
            for (const auto& [mx, my, mz, ms] : more) edit(mx, my, mz, u16(ms));
        }
}

bool Game::redstoneOreInteract(int x, int y, int z, bool light) {
    const u16 state = world_.block(x, y, z);
    const std::string& n = registry_.name(state);
    if (!n.starts_with("minecraft:redstone_ore[") && !n.starts_with("minecraft:deepslate_redstone_ore[")) return false;
    if (onRedstoneOreParticles) onRedstoneOreParticles(x, y, z);
    if (light && registry_.property(state, "lit") != "true") edit(x, y, z, registry_.withProperty(state, "lit", "true"));
    return true;
}

void Game::edit(int x, int y, int z, u16 id) {
    std::unordered_set<u64> dirty;
    {
        std::unique_lock lock(world_.mutex());
        const u16 old = world_.block(x, y, z);
        if (old == id || !world_.setBlock(x, y, z, id)) return;
        const int cx = x >> 4, sy = y >> 4, cz = z >> 4;
        dirty.insert(Lighting::sectionKey(cx, sy, cz));
        if ((x & 15) == 0) dirty.insert(Lighting::sectionKey(cx - 1, sy, cz));
        if ((x & 15) == 15) dirty.insert(Lighting::sectionKey(cx + 1, sy, cz));
        if ((y & 15) == 0) dirty.insert(Lighting::sectionKey(cx, sy - 1, cz));
        if ((y & 15) == 15) dirty.insert(Lighting::sectionKey(cx, sy + 1, cz));
        if ((z & 15) == 0) dirty.insert(Lighting::sectionKey(cx, sy, cz - 1));
        if ((z & 15) == 15) dirty.insert(Lighting::sectionKey(cx, sy, cz + 1));
        const BlockInfo* infos = registry_.infos();
        if (infos[old].opacity != infos[id].opacity || infos[old].emission != infos[id].emission)
            lighting_.blockChanged(x, y, z, dirty);
    }
    terrain_.remesh(std::vector<u64>(dirty.begin(), dirty.end()));
}

void Game::buildOverlay() {
    const CameraState cam = viewCamera();  // world lines are relative to the rendered camera
    std::vector<LineVertex> world, screen;
    std::vector<SpriteVertex> sprites;

    // LevelRenderer.submitBlockOutline: the targeted block's selection shape (none outside the world border). High
    // contrast draws a 7 px black pass, then the aqua outline (0xFF57FFE1); otherwise ARGB.black(102). The width is
    // Window.appropriateLineWidth: max(2.5, width / 1920 * 2.5).
    // GameRenderer.renderLevel: the outline needs the HUD shown; a spectator's only on blocks with a menu or portals
    // (showAsInteractableInSpectatorMode); in adventure, only where the held item may break or place on
    auto outlineShown = [&] {
        if (hudHidden) return false;
        const int tx = target_.x, ty = target_.y, tz = target_.z;
        if (gameMode == 3) {
            const std::string& full = registry_.name(world_.block(tx, ty, tz));
            const std::string n = full.substr(0, full.find('['));
            static const char* const kMenus[] = {"minecraft:barrel", "minecraft:furnace", "minecraft:blast_furnace", "minecraft:smoker",
                                                 "minecraft:dispenser", "minecraft:dropper", "minecraft:hopper", "minecraft:brewing_stand",
                                                 "minecraft:beacon", "minecraft:crafter", "minecraft:crafting_table",
                                                 "minecraft:enchanting_table", "minecraft:cartography_table", "minecraft:grindstone",
                                                 "minecraft:loom", "minecraft:smithing_table", "minecraft:stonecutter",
                                                 "minecraft:nether_portal", "minecraft:end_portal", "minecraft:end_gateway"};
            for (const char* m : kMenus)
                if (n == m) return true;
            return (n.ends_with("chest") && n != "minecraft:ender_chest") || n.ends_with("shulker_box") || n.ends_with("anvil") ||
                   (n == "minecraft:lectern" && full.find("has_book=true") != std::string::npos);
        }
        if (gameMode == 2)  // !mayBuild: the held stack's can_break / can_place_on
            return (adventureMayBreak && adventureMayBreak(tx, ty, tz)) || (adventureMayPlace && adventureMayPlace(tx, ty, tz));
        return true;
    };
    if (target_.hit && crosshairEntity < 0 && border.blockWithinBounds(target_.x, target_.z) && outlineShown()) {
        const u16 targetId = world_.block(target_.x, target_.y, target_.z);
        const BlockInfo& b = registry_.infos()[targetId];
        std::vector<Box> shape;  // getShape: scaffolding is a full block while holding scaffolding
        if (physics_.holdingScaffolding && registry_.name(targetId).starts_with("minecraft:scaffolding")) shape.push_back({{0, 0, 0}, {1, 1, 1}});
        else
            for (u32 i = 0; i < b.selectionCount; ++i) shape.push_back(registry_.box(b.selectionFirst + i));
        // VoxelShape.forAllEdges: the edges of the joined shape (DiscreteVoxelShape over the boxes' coordinates), each
        // run along an axis merged into one line.
        std::vector<float> coords[3];
        for (const Box& s : shape)
            for (int a = 0; a < 3; ++a) coords[a].push_back(s.min[a]), coords[a].push_back(s.max[a]);
        for (auto& c : coords) {
            std::sort(c.begin(), c.end());
            c.erase(std::unique(c.begin(), c.end()), c.end());
        }
        const int size[3] = {std::max(0, int(coords[0].size()) - 1), std::max(0, int(coords[1].size()) - 1),
                             std::max(0, int(coords[2].size()) - 1)};
        auto full = [&](const int c[3]) {
            for (int a = 0; a < 3; ++a)
                if (c[a] < 0 || c[a] >= size[a]) return false;
            const float mid[3] = {(coords[0][c[0]] + coords[0][c[0] + 1]) * 0.5f, (coords[1][c[1]] + coords[1][c[1] + 1]) * 0.5f,
                                  (coords[2][c[2]] + coords[2][c[2] + 1]) * 0.5f};
            for (const Box& s : shape)
                if (mid[0] > s.min[0] && mid[0] < s.max[0] && mid[1] > s.min[1] && mid[1] < s.max[1] && mid[2] > s.min[2] && mid[2] < s.max[2])
                    return true;
            return false;
        };
        std::vector<std::array<float, 6>> edges;
        float off[3];  // BlockState.getOffset moves the shape
        BlockRegistry::offset(b, target_.x, target_.z, off);
        const double base[3] = {target_.x - cam.x + off[0], target_.y - cam.y + off[1], target_.z - cam.z + off[2]};
        auto emit = [&](int axis, int a, int bb, int c0, int c1) {
            const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
            std::array<float, 6> e{};
            e[a1] = e[3 + a1] = float(base[a1] + coords[a1][a]);
            e[a2] = e[3 + a2] = float(base[a2] + coords[a2][bb]);
            e[axis] = float(base[axis] + coords[axis][c0]), e[3 + axis] = float(base[axis] + coords[axis][c1]);
            edges.push_back(e);
        };
        for (int axis = 0; axis < 3; ++axis) {  // forAllAxisEdges: lines along `axis`
            const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
            for (int a = 0; a <= size[a1]; ++a)
                for (int bb = 0; bb <= size[a2]; ++bb) {
                    int lastStart = -1;
                    for (int c = 0; c <= size[axis]; ++c) {
                        int fullSectors = 0, oddSectors = 0;
                        for (int da = 0; da <= 1; ++da)
                            for (int db = 0; db <= 1; ++db) {
                                int cell[3];
                                cell[a1] = a + da - 1, cell[a2] = bb + db - 1, cell[axis] = c;
                                if (full(cell)) ++fullSectors, oddSectors ^= da ^ db;
                            }
                        if (fullSectors == 1 || fullSectors == 3 || (fullSectors == 2 && (oddSectors & 1) == 0)) {
                            if (lastStart == -1) lastStart = c;
                        } else if (lastStart != -1) {
                            emit(axis, a, bb, lastStart, c);
                            lastStart = -1;
                        }
                    }
                }
        }
        const float W = float(std::max(1u, renderer_.width())), H = float(std::max(1u, renderer_.height()));
        auto pass = [&](u32 color, float width) {
            constexpr float kLayer = 1.0f - 1.0f / 4096.0f;  // VIEW_OFFSET_Z_LAYERING (ProjectionType.PERSPECTIVE)
            for (auto e : edges) {
                for (float& v : e) v *= kLayer;
                world.push_back({e[0], e[1], e[2], color, width / W, width / H});
                world.push_back({e[3], e[4], e[5], color, width / W, width / H});
            }
        };
        if (highContrastOutline) pass(0xFF000000, 7.0f);
        pass(highContrastOutline ? 0xFFE1FF57 : 0x66000000, std::max(2.5f, W / 1920.0f * 2.5f));
    }
    if (extraLines) extraLines(world, cam, float(std::max(1u, renderer_.width())), float(std::max(1u, renderer_.height())));

    if (!drawHud) {  // the UI draws the crosshair and hotbar
        renderer_.setOverlay(std::move(world), {}, {});
        return;
    }
    const float W = float(renderer_.width()), H = float(renderer_.height());
    auto nx = [&](float px) { return px / W * 2.0f - 1.0f; };
    auto ny = [&](float py) { return 1.0f - py / H * 2.0f; };
    const float cx = W / 2, cy = H / 2, arm = std::round(H / 108.0f);
    screen.push_back({nx(cx - arm), ny(cy), 0, 0xE0FFFFFF});
    screen.push_back({nx(cx + arm + 1), ny(cy), 0, 0xE0FFFFFF});
    screen.push_back({nx(cx), ny(cy - arm), 0, 0xE0FFFFFF});
    screen.push_back({nx(cx), ny(cy + arm + 1), 0, 0xE0FFFFFF});

    // Hotbar.
    auto quad = [&](float x0, float y0, float x1, float y1, u32 layer, u32 color) {
        const SpriteVertex v[4] = {{nx(x0), ny(y0), 0, 0, layer, color}, {nx(x1), ny(y0), 1, 0, layer, color},
                                   {nx(x0), ny(y1), 0, 1, layer, color}, {nx(x1), ny(y1), 1, 1, layer, color}};
        for (int k : {0, 1, 2, 2, 1, 3}) sprites.push_back(v[k]);
    };
    auto rect = [&](float x0, float y0, float x1, float y1, u32 color) {
        const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
        for (int k = 0; k < 4; ++k) {
            screen.push_back({nx(xs[k]), ny(ys[k]), 0, color});
            screen.push_back({nx(xs[(k + 1) & 3]), ny(ys[(k + 1) & 3]), 0, color});
        }
    };
    const float slot = std::round(H / 24.0f), pad = std::round(slot * 0.12f);
    const float x0 = std::round(W / 2 - slot * 4.5f), y0 = H - slot - pad * 2;
    quad(x0 - pad, y0 - pad, x0 + slot * 9 + pad, y0 + slot + pad, 0xFFFFFFFF, 0x90000000);
    for (int i = 0; i < 9; ++i) {
        const float sx = x0 + slot * i;
        const BlockInfo& b = registry_.infos()[hotbar_[i]];
        if (hotbar_[i]) {
            const u32 tint = b.kind == KindCube ? fixedTintRgb(b.faces[3].tint) : 0xFFFFFF;
            quad(sx + pad, y0 + pad, sx + slot - pad, y0 + slot - pad, b.icon, abgr(tint, 255));
        }
        if (i == selected_) rect(sx, y0 - 1, sx + slot, y0 + slot + 1, 0xFFFFFFFF);
    }
    renderer_.setOverlay(std::move(world), std::move(screen), std::move(sprites));
}

std::string Game::status() const {
    char buf[256];
    const std::string& held = registry_.name(hotbar_[selected_]);
    const std::string target = target_.hit ? registry_.name(world_.block(target_.x, target_.y, target_.z)) : "-";
    std::snprintf(buf, sizeof(buf), "%s | %.1f %.1f %.1f | looking at %s | holding %s",
                  player_.flying ? "flying" : (player_.inWater ? "swimming" : "walking"), player_.pos[0],
                  player_.pos[1], player_.pos[2], target.c_str(), held.c_str());
    return buf;
}
