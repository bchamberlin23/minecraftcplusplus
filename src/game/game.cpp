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
constexpr double kTick = 0.05;   // 20 ticks per second
constexpr double kReach = 5.0;   // creative block reach
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
    const double t = alpha_;
    c.x = player_.prev[0] + (player_.pos[0] - player_.prev[0]) * t;
    c.y = player_.prev[1] + (player_.pos[1] - player_.prev[1]) * t + player_.eyeHeight();
    c.z = player_.prev[2] + (player_.pos[2] - player_.prev[2]) * t;
    c.yaw = player_.yaw;
    c.pitch = player_.pitch;
    return c;
}

CameraState Game::viewCamera() const {
    CameraState c = camera();
    if (cameraMode == 0) return c;
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
    return c;
}

PlayerInput Game::readInput(Window& w) {
    PlayerInput in;
    if (!w.mouseCaptured()) return in;
    in.forward = float(w.keyDown('W')) - float(w.keyDown('S'));
    in.strafe = float(w.keyDown('A')) - float(w.keyDown('D'));
    in.jump = w.keyDown(VK_SPACE);
    in.sneak = w.keyDown(VK_SHIFT);
    in.sprint = w.keyDown(VK_CONTROL);
    return in;
}

void Game::frame(Window& w, double dt) {
    clock_ += dt;
    float mdx, mdy;
    w.takeMouseDelta(mdx, mdy);
    if (inputSuspended) mdx = mdy = 0;
    player_.yaw += mdx * sensitivity;
    player_.pitch = std::clamp(player_.pitch + mdy * sensitivity, -1.55f, 1.55f);

    const bool f5 = !inputSuspended && w.keyDown(VK_F5);
    if (f5 && !f5WasDown_) cameraMode = (cameraMode + 1) % 3;
    f5WasDown_ = f5;
    const bool esc = w.keyDown(VK_ESCAPE);
    if (handleEscape && esc && !escWasDown_) w.setMouseCaptured(false);
    escWasDown_ = esc;
    for (int i = 0; i < 9; ++i) {
        const bool down = !inputSuspended && w.mouseCaptured() && w.keyDown('1' + i);
        if (down && !keyWasDown_[i]) selected_ = i;
        keyWasDown_[i] = down;
    }
    // Double-tapping jump toggles flight, like creative mode.
    const bool jump = !inputSuspended && w.mouseCaptured() && w.keyDown(VK_SPACE);
    if (jump && !jumpWasDown_) {
        if (clock_ - lastJumpPress_ < 0.3 && player_.mayFly && !player_.noClip) {
            player_.flying = !player_.flying;
            player_.vel[1] = 0;
            lastJumpPress_ = -1;
        } else {
            lastJumpPress_ = clock_;
        }
    }
    jumpWasDown_ = jump;

    const PlayerInput input = inputSuspended ? PlayerInput{} : readInput(w);
    tickAccumulator_ += std::min(dt, 0.25);
    while (tickAccumulator_ >= kTick) {
        player_.tick(physics_, input);
        if (onPlayerTick) onPlayerTick();
        tickAccumulator_ -= kTick;
    }
    alpha_ = tickAccumulator_ / kTick;

    retarget();
    handleClicks(w, dt);
    buildOverlay();
}

void Game::retarget() {
    const CameraState cam = camera();
    const double origin[3] = {cam.x, cam.y, cam.z};
    const double cp = std::cos(cam.pitch);
    const double dir[3] = {-std::sin(cam.yaw) * cp, -std::sin(cam.pitch), std::cos(cam.yaw) * cp};
    target_ = physics_.raycast(origin, dir, kReach);
}

void Game::simulate(const PlayerInput& input, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        player_.tick(physics_, input);
        if (onPlayerTick) onPlayerTick();
    }
    alpha_ = 1.0;
    retarget();
}

void Game::handleClicks(Window& w, double dt) {
    const Window::MouseEvents ev = w.takeMouseEvents();
    if (inputSuspended) return;
    if (ev.left > 0 && onSwing) onSwing();  // Minecraft.startAttack swings on every press
    selected_ = ((selected_ - ev.wheel) % 9 + 9) % 9;
    const bool leftHeld = w.mouseCaptured() && w.keyDown(VK_LBUTTON);
    const bool rightHeld = w.mouseCaptured() && w.keyDown(VK_RBUTTON);
    repeatTimer_ -= dt;
    const bool doBreak = ev.left > 0 || (leftHeld && repeatTimer_ <= 0);
    const bool doPlace = ev.right > 0 || (rightHeld && repeatTimer_ <= 0);
    if (ev.middle && target_.hit) {
        if (onPickBlock) onPickBlock(target_.x, target_.y, target_.z);
        else hotbar_[selected_] = world_.block(target_.x, target_.y, target_.z);
    }
    // An entity in front of the targeted block takes the click (attack / use only on a press, like the game).
    if (usingItem_ && !rightHeld) {  // Minecraft.handleKeybinds: releaseUsingItem
        usingItem_ = false;
        if (onReleaseUseItem) onReleaseUseItem();
    }
    if ((doBreak || doPlace) && onEntityClick &&
        onEntityClick(ev.left > 0, ev.left == 0 && ev.right > 0, target_.hit ? target_.distance : -1.0)) {
        repeatTimer_ = 0.3;
        if (ev.left == 0 && ev.right > 0 && !usingItem_ && onUseItem) {  // e.g. a shield raised facing a mob
            onUseItem(nullptr, true);
            usingItem_ = true;
        }
        return;
    }
    useConsumed_ = false;
    if (target_.hit && (doBreak || doPlace)) {
        repeatTimer_ = (ev.left || ev.right) ? 0.3 : 0.2;  // the game repeats every 4 ticks while held
        useTarget(!doBreak);
    }
    if (ev.left > 0 && onPunch) onPunch();
    // Minecraft.startUseItem: a press that neither placed a block nor used the targeted one uses the held item.
    if (ev.right > 0 && !doBreak && !useConsumed_ && !usingItem_ && onUseItem) {
        onUseItem(target_.hit ? &target_ : nullptr, false);
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
    // Using a block that opens a screen (or acting with an item that is not a block) goes to the server as is.
    const bool interacts = onInteract && !player_.sneaking && onInteract(target_.x, target_.y, target_.z);
    if (interacts || !hotbar_[selected_]) {
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

    // Outline of the targeted block's selection shape.
    if (target_.hit) {
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
