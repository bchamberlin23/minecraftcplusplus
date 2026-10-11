#include "game/game.h"

#include "assets/assets.h"
#include "platform/window.h"
#include "world/terrain.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_set>

namespace {
constexpr double kPi = 3.14159265358979;
constexpr int kDir[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
constexpr const char* kFaceName[6] = {"down", "up", "north", "south", "west", "east"};
constexpr u32 kTints[8] = {0xFFFFFF, 0x91BD59, 0x77AB2F, 0x80A755, 0x619961, 0x3F76E4, 0x208030, 0x8B1A00};

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
        rot = mul(rot, rotZ(-hurt * 14.0f * kDeg));
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
    double dist = 4.0;  // Camera.getMaxZoom: 8 rays from jittered eye positions
    for (int i = 0; i < 8; ++i) {
        const double o[3] = {c.x + ((i & 1) * 2 - 1) * 0.1, c.y + (((i >> 1) & 1) * 2 - 1) * 0.1,
                             c.z + (((i >> 2) & 1) * 2 - 1) * 0.1};
        const RayHit h = physics_.raycast(o, back, dist);
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
    player_.yaw += mdx * sensitivity;
    player_.pitch = std::clamp(player_.pitch + mdy * sensitivity, -1.55f, 1.55f);

    if (presses(keys.perspective) % 2) cameraMode = (cameraMode + 1) % 3;
    const bool esc = w.keyDown(VK_ESCAPE);
    if (handleEscape && esc && !escWasDown_) w.setMouseCaptured(false);
    escWasDown_ = esc;
    for (int i = 0; i < 9; ++i)
        if (presses(keys.hotbar[i])) selected_ = i;
    // Double-tapping jump toggles flight, like creative mode.
    for (int n = presses(keys.jump); n > 0; --n) {
        if (clock_ - lastJumpPress_ < 0.3 && player_.mayFly && !player_.noClip && !riding()) {
            player_.flying = !player_.flying;
            player_.vel[1] = 0;
            lastJumpPress_ = -1;
        } else {
            lastJumpPress_ = clock_;
        }
    }

    const PlayerInput input = inputSuspended ? PlayerInput{} : readInput(w);
    const Window::MouseEvents ev = w.takeMouseEvents();
    if (inputSuspended) pendingAttacks_ = 0;
    else pendingAttacks_ = std::min(pendingAttacks_ + presses(keys.attack), 4);
    const bool attackDown = !inputSuspended && held(w, keys.attack);
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
            if (use.active && use.remaining > 0) --use.remaining;
        }
        player_.useSpeed = use.active ? use.speed : 1.0f;
        player_.useBlocksSprint = use.active && !use.canSprint;
        eyeHeightOld_ = eyeHeight_;
        eyeHeight_ += (player_.eyeHeight() - eyeHeight_) * 0.5;
        if (riding()) {  // a passenger does not move itself (LocalPlayer.rideTick)
            player_.lastInput = input;
            rideTick(input);
        } else if (cameraEntity >= 0) {  // spectating another entity: the keys reach the server, the player stays
            player_.tick(physics_, PlayerInput{});
            player_.lastInput = input;
        } else {
            player_.tick(physics_, input);
        }
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

void Game::retarget() {
    const CameraState cam = camera();
    const double origin[3] = {cam.x, cam.y, cam.z};
    const double cp = std::cos(cam.pitch);
    const double dir[3] = {-std::sin(cam.yaw) * cp, -std::sin(cam.pitch), std::cos(cam.yaw) * cp};
    target_ = physics_.raycast(origin, dir, std::max(blockReach, entityReach));
    blockDistance_ = target_.hit ? target_.distance : -1.0;
    if (target_.hit && !(target_.distance < blockReach)) target_.hit = false;  // filterHitResult: a miss
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
        selected_ = ((selected_ - steps) % 9 + 9) % 9;
    }
    const bool rightHeld = held(w, keys.use);
    repeatTimer_ -= dt;
    // Breaking runs in tickAttack; Minecraft.startUseItem does nothing while a block is being destroyed or the hands
    // are busy (paddling).
    // Held: Minecraft.handleKeybinds repeats startUseItem every 4 ticks (rightClickDelay), not while an item is in use.
    const bool doPlace = (ev.right > 0 || (rightHeld && repeatTimer_ <= 0 && !use.active)) && !dig_.active && !handsBusy;
    if (doPlace) repeatTimer_ = ev.right > 0 ? 0.3 : 0.2;
    if (ev.middle && target_.hit) {
        if (onPickBlock) onPickBlock(target_.x, target_.y, target_.z);
        else hotbar_[selected_] = world_.block(target_.x, target_.y, target_.z);
    }
    // An entity in front of the targeted block takes the click (attack / use only on a press, like the game).
    if (usingItem_ && !rightHeld) {  // Minecraft.handleKeybinds: releaseUsingItem
        usingItem_ = false;
        use.active = false;  // Player.releaseUsingItem: the client stops at once
        if (onReleaseUseItem) onReleaseUseItem();
    }
    if (doPlace && onEntityClick && onEntityClick(false, ev.right > 0, blockDistance_)) {
        repeatTimer_ = 0.3;
        if (ev.right > 0 && !usingItem_ && onUseItem) {  // e.g. a shield raised facing a mob
            onUseItem(nullptr, true);
            if (predictUse) predictUse(nullptr, true);
            usingItem_ = true;
        }
        return;
    }
    useConsumed_ = false;
    if (target_.hit && doPlace) {
        repeatTimer_ = ev.right ? 0.3 : 0.2;  // the game repeats every 4 ticks while held
        useTarget(true);
    }
    // Minecraft.startUseItem: a press that neither placed a block nor used the targeted one uses the held item; held,
    // it repeats every 4 ticks once the last use has ended (eating the next piece of food).
    if ((ev.right > 0 ? !usingItem_ : (doPlace && rightHeld)) && !dig_.active && !useConsumed_ && !use.active && onUseItem) {
        onUseItem(target_.hit ? &target_ : nullptr, false);
        if (predictUse) predictUse(target_.hit ? &target_ : nullptr, false);
        usingItem_ = true;
    }
}

bool Game::useTarget(bool place) {
    if (!target_.hit) return false;
    EditAction action{place, target_.x, target_.y, target_.z, target_.face,
                      {target_.point[0], target_.point[1], target_.point[2]}, {target_.x, target_.y, target_.z}, 0};
    if (!place) {
        if (onEdit) onEdit(action);
        const u16 old = world_.block(target_.x, target_.y, target_.z);
        edit(target_.x, target_.y, target_.z, 0);
        if (onBlockChanged && old) onBlockChanged(false, target_.x, target_.y, target_.z, old);
        if (onSwing) onSwing();
        return true;
    }
    // MultiPlayerGameMode.useItemOn: a block outside the world border fails (nothing is sent, no item use follows).
    if (!border.blockWithinBounds(target_.x, target_.z)) {
        useConsumed_ = true;
        return false;
    }
    // Using a block that opens a screen (or acting with an item that is not a block) goes to the server as is.
    const bool interacts = onInteract && !player_.sneaking && onInteract(target_.x, target_.y, target_.z);
    // A held block cooling down is not placed (ItemStack.useOn is skipped); the use still goes to the server.
    if (interacts || !hotbar_[selected_] || (mainHandOnCooldown && mainHandOnCooldown())) {
        useConsumed_ = interacts;  // else the held item may still be used (food, buckets...)
        if (onEdit) onEdit(action);
        return false;
    }
    const BlockInfo* infos = registry_.infos();
    int t[3] = {target_.x, target_.y, target_.z};
    if (!infos[world_.block(t[0], t[1], t[2])].replaceable)
        for (int a = 0; a < 3; ++a) t[a] += kDir[target_.face][a];
    if (!infos[world_.block(t[0], t[1], t[2])].replaceable || !hotbar_[selected_]) return false;
    const u16 id = orientForPlacement(hotbar_[selected_], target_, t);
    const BlockInfo& b = infos[id];
    const AABB body = player_.box();
    for (u32 i = 0; i < b.collisionCount; ++i) {
        const Box& s = registry_.box(b.collisionFirst + i);
        const AABB a{{t[0] + s.min[0], t[1] + s.min[1], t[2] + s.min[2]}, {t[0] + s.max[0], t[1] + s.max[1], t[2] + s.max[2]}};
        if (a.intersects(body)) return false;  // never place a block inside the player
    }
    if (onEdit) {
        std::copy(t, t + 3, action.at);
        action.block = id;
        onEdit(action);
    }
    edit(t[0], t[1], t[2], id);
    useConsumed_ = true;
    if (onBlockChanged) onBlockChanged(true, t[0], t[1], t[2], id);
    if (onSwing) onSwing();
    return true;
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
    attackStrengthTicker = 0;
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
    if (gameMode == 3) return true;  // spectators: no attack, no digging
    // An entity in front of the targeted block takes the press (the attack packet carries its own punch).
    if (onEntityClick && onEntityClick(true, false, blockDistance_)) {
        attackStrengthTicker = 0;  // MultiPlayerGameMode.attack -> resetAttackStrengthTicker
        if (onSwing) onSwing();
        return false;
    }
    bool endAttack = false;
    if (target_.hit && world_.block(target_.x, target_.y, target_.z) != 0) {
        startDestroyBlock(target_.x, target_.y, target_.z, target_.face);
        endAttack = world_.block(target_.x, target_.y, target_.z) == 0;
    } else {
        if (!instabuild) missTime_ = 10;  // MultiPlayerGameMode.hasMissTime: survival-like modes
        attackStrengthTicker = 0;         // resetAttackStrengthTicker
    }
    if (onSwing) onSwing();
    if (onPunch) onPunch();
    return endAttack;
}

void Game::continueAttack(bool down) {
    if (!down) missTime_ = 0;
    if (missTime_ > 0 || use.active) return;
    if (down && target_.hit && world_.block(target_.x, target_.y, target_.z) != 0) {
        if (continueDestroyBlock(target_.x, target_.y, target_.z, target_.face)) {
            if (onSwing) onSwing();
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

// Adapts a held block state to how it is placed (axis, facing, top/bottom halves, wall torches),
// following the game's placement rules for the common property names.
u16 Game::orientForPlacement(u16 id, const RayHit& hit, const int target[3]) {
    const std::string name = registry_.name(id);
    const int face = hit.face;
    if (face >= 2) {
        const std::string facing = std::string("[facing=") + kFaceName[face];
        if (name == "minecraft:torch") return registry_.idFor("minecraft:wall_torch" + facing + "]");
        if (name == "minecraft:soul_torch") return registry_.idFor("minecraft:soul_wall_torch" + facing + "]");
        if (name.starts_with("minecraft:redstone_torch"))
            return registry_.idFor("minecraft:redstone_wall_torch" + facing + ",lit=true]");
    }
    if (!registry_.property(id, "axis").empty()) id = registry_.withProperty(id, "axis", face < 2 ? "y" : face < 4 ? "z" : "x");
    if (!registry_.property(id, "facing").empty()) {
        static const char* look[4] = {"south", "west", "north", "east"};
        const int q = int(std::floor(player_.yaw / (kPi / 2) + 0.5)) & 3;
        // Furnaces, chests... face the player; stairs, doors, fence gates and beds face where the player looks.
        const std::string base = name.substr(0, name.find('['));
        const bool towardsPlayer = name.find("stairs") == std::string::npos && !base.ends_with("_door") &&
                                   !base.ends_with("_fence_gate") && !base.ends_with("_bed");
        id = registry_.withProperty(id, "facing", look[towardsPlayer ? (q + 2) & 3 : q]);
    }
    const bool upper = face == 0 || (face >= 2 && hit.point[1] - target[1] > 0.5);
    const std::string_view half = registry_.property(id, "half");
    if (half == "top" || half == "bottom") id = registry_.withProperty(id, "half", upper ? "top" : "bottom");
    const std::string_view type = registry_.property(id, "type");
    if (type == "top" || type == "bottom") id = registry_.withProperty(id, "type", upper ? "top" : "bottom");
    return id;
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

    // Outline of the targeted block's selection shape (none outside the world border).
    if (target_.hit && border.blockWithinBounds(target_.x, target_.z)) {
        const BlockInfo& b = registry_.infos()[world_.block(target_.x, target_.y, target_.z)];
        for (u32 i = 0; i < b.selectionCount; ++i) {
            const Box& s = registry_.box(b.selectionFirst + i);
            float lo[3], hi[3];
            const double base[3] = {target_.x - cam.x, target_.y - cam.y, target_.z - cam.z};
            for (int a = 0; a < 3; ++a) {
                lo[a] = float(base[a] + s.min[a] - 0.002);
                hi[a] = float(base[a] + s.max[a] + 0.002);
            }
            for (int e = 0; e < 12; ++e) {  // 4 edges along each axis
                const int axis = e / 4, a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
                float p[3], q[3];
                p[a1] = q[a1] = (e & 1) ? hi[a1] : lo[a1];
                p[a2] = q[a2] = (e & 2) ? hi[a2] : lo[a2];
                p[axis] = lo[axis], q[axis] = hi[axis];
                world.push_back({p[0], p[1], p[2], 0x99000000});
                world.push_back({q[0], q[1], q[2], 0x99000000});
            }
        }
    }

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
            const u32 tint = b.kind == KindCube ? kTints[b.faces[3].tint & 7] : 0xFFFFFF;
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
