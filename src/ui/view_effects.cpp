// The local player's mob effects and surroundings as the view shows them:
//  - the fog / lightmap inputs (ViewEffects, applied by sky/sky.cpp): blindness, darkness and its pulse, night vision,
//    the water vision (LocalPlayer.getWaterVision) and fire resistance;
//  - the nausea / nether portal warp of the world projection (GameRenderer.renderLevel);
//  - ScreenEffectRenderer's 3D screen effects, drawn first (in-wall block, underwater texture, on fire) and the camera
//    overlays of Hud.extractCameraOverlays that are not elsewhere (carved pumpkin, nether portal).
#include "assets/assets.h"
#include "audio/audio.h"
#include "render/renderer.h"
#include "ui/gameplay_hud.h"
#include "ui/ui_state.h"
#include "world/world.h"

#include <cmath>
#include <shared_mutex>

namespace ui {

namespace {
constexpr float kPi = 3.14159265f;

struct BlendDurations {
    int in = 0, out = 0, advance = 0;
};
BlendDurations blendDurations(const std::string& effect) {  // MobEffects: setBlendDuration
    if (effect == "nausea") return {150, 20, 60};
    if (effect == "darkness") return {22, 22, 22};
    return {};
}

// A texture repeated over the screen: u runs uLeft..uRight across, v vTop..vBottom down, in texture repeats.
void wrapped(Canvas& c, const Sprite* s, float uLeft, float uRight, float vTop, float vBottom, u32 color) {
    const float W = float(c.guiWidth()), H = float(c.guiHeight());
    const float du = s->u1 - s->u0, dv = s->v1 - s->v0;
    const float ua = std::min(uLeft, uRight), ub = std::max(uLeft, uRight);
    const float va = std::min(vTop, vBottom), vb = std::max(vTop, vBottom);
    auto xAt = [&](float u) { return (u - uLeft) / (uRight - uLeft) * W; };
    auto yAt = [&](float v) { return (v - vTop) / (vBottom - vTop) * H; };
    for (float ku = std::floor(ua); ku < ub; ku += 1.0f)
        for (float kv = std::floor(va); kv < vb; kv += 1.0f) {
            const float u0 = std::max(ua, ku), u1 = std::min(ub, ku + 1), v0 = std::max(va, kv), v1 = std::min(vb, kv + 1);
            float x0 = xAt(u0), x1 = xAt(u1), y0 = yAt(v0), y1 = yAt(v1);
            float tu0 = s->u0 + du * (u0 - ku), tu1 = s->u0 + du * (u1 - ku);
            float tv0 = s->v0 + dv * (v0 - kv), tv1 = s->v0 + dv * (v1 - kv);
            if (x0 > x1) std::swap(x0, x1), std::swap(tu0, tu1);
            if (y0 > y1) std::swap(y0, y1), std::swap(tv0, tv1);
            c.quad(x0, y0, x1, y1, tu0, tv0, tu1, tv1, color, ModeGui);
        }
}
}  // namespace

float Ui::State::effectBlend(const char* name) const {
    const Effect* e = findEffect(name);
    return e ? std::max(0.0f, e->blendO + (e->blend - e->blendO) * partialTick) : 0.0f;
}

void Ui::State::tickViewEffects(bool eyeWater) {
    // MobEffectInstance.tickClient: the blend state follows whether the effect is on (durations count down in tickHud).
    for (auto& [id, e] : effects) {
        const BlendDurations d = blendDurations(e.name);
        const bool on = !(e.duration >= 0 && e.duration <= d.advance);  // !endsWithin(blendOutAdvanceTicks)
        const float target = on ? 1.0f : 0.0f;
        if (e.blend < 0) {  // the packet had no FLAG_BLEND: BlendState.setImmediate
            e.blend = e.blendO = target;
            continue;
        }
        e.blendO = e.blend;
        if (e.blend != target) {
            const int ticks = on ? d.in : d.out;
            e.blend = ticks == 0 ? target : e.blend + std::clamp(target - e.blend, -1.0f / float(ticks), 1.0f / float(ticks));
        }
    }
    // LocalPlayer.aiStep: the water vision time.
    eyeInWater = eyeWater;
    if (eyeWater) waterVisionTime = std::min(600, waterVisionTime + (spectator() ? 10 : 1));
    else if (waterVisionTime > 0) waterVisionTime = std::max(0, waterVisionTime - 10);
    // handlePortalTransitionEffect: rises while inside a nether portal block, falls outside.
    portalIntensityO = portalIntensity;
    bool inPortal = false;
    {
        const AABB b = game->player().box();
        for (int x = int(std::floor(b.min[0] + 1e-5)); x <= int(std::floor(b.max[0] - 1e-5)) && !inPortal; ++x)
            for (int y = int(std::floor(b.min[1] + 1e-5)); y <= int(std::floor(b.max[1] - 1e-5)) && !inPortal; ++y)
                for (int z = int(std::floor(b.min[2] + 1e-5)); z <= int(std::floor(b.max[2] - 1e-5)) && !inPortal; ++z)
                    inPortal = registry->name(world->block(x, y, z)).starts_with("minecraft:nether_portal");
    }
    // Screens not allowed in a portal (Screen.isAllowedInPortal: the non-pause ones but chat and death) close, a
    // container's with closeContainer.
    if (inPortal && (screen == Screen::Inventory || screen == Screen::Container || screen == Screen::Creative ||
                     screen == Screen::Sign || screen == Screen::GameModeSwitcher || screen == Screen::SocialInteractions))
        closeScreen(true);
    if (inPortal && portalIntensity == 0.0f)  // SimpleSoundInstance.forLocalAmbience(PORTAL_TRIGGER, 0.8 .. 1.2, 0.25)
        if (audio::AudioSystem* a = audio::instance()) {
            const CameraState c = game->camera();
            a->play("minecraft:block.portal.trigger", audio::Category::Ambient, c.x, c.y, c.z, 0.25f,
                    float(std::rand() % 1000) / 1000.0f * 0.4f + 0.8f);
        }
    const float step = inPortal ? 0.0125f : portalIntensity > 0.0f ? -0.05f : 0.0f;
    portalIntensity = std::clamp(portalIntensity + step, 0.0f, 1.0f);
    // tickSpinningEffect.
    const Effect* nauseaEffect = findEffect("nausea");
    const float nausea = nauseaEffect ? nauseaEffect->blend : 0.0f;
    if (portalIntensity <= 0.0f && nausea <= 0.0f) {
        spinSpeed = 0;
    } else {
        spinSpeed = (portalIntensity * 20.0f + nausea * 7.0f) / (portalIntensity + nausea);
        spinTime += spinSpeed;
    }
}

ViewEffects Ui::viewEffects() const {
    const State& s = *s_;
    const float pt = s.partialTick;
    ViewEffects v;
    if (const State::Effect* e = s.findEffect("blindness")) {
        v.blindFog = e->duration < 0 ? 1.0f : std::min(1.0f, float(e->duration) / 20.0f);
        v.blindDarkness = e->duration >= 0 && e->duration <= 19 ? float(e->duration) / 20.0f : 1.0f;
    }
    v.darkness = s.findEffect("darkness") != nullptr;
    v.darknessBlend = s.effectBlend("darkness");
    // LightmapRenderStateExtractor: the Darkness Pulsing option scales the brightness loss and the pulse.
    v.darknessEffectScale = float(s.settings.darknessEffectScale);
    v.darknessScale = std::max(0.0f, std::cos((float(s.guiTicks) - pt) * kPi * 0.025f) * 0.45f * v.darknessBlend * v.darknessEffectScale) *
                      v.darknessEffectScale;
    if (s.eyeInWater) {  // LocalPlayer.getWaterVision
        const float t = float(s.waterVisionTime);
        v.waterVision = t >= 600.0f ? 1.0f
                                    : std::clamp(t / 100.0f, 0.0f, 1.0f) * 0.6f +
                                          (t < 100.0f ? 0.0f : std::clamp((t - 100.0f) / 500.0f, 0.0f, 1.0f)) * 0.4f;
    }
    if (const State::Effect* e = s.findEffect("night_vision")) {  // GameRenderer.nightVisionScale
        const float scale = e->duration < 0 || e->duration > 200 ? 1.0f : 0.7f + std::sin((float(e->duration) - pt) * kPi * 0.2f) * 0.3f;
        v.fogBrighten = v.darkness ? 0.0f : scale;
        v.lightmapNightVision = scale;
    } else if (v.waterVision > 0.0f && s.findEffect("conduit_power")) {
        v.lightmapNightVision = v.waterVision;
    }
    v.fireResistance = s.findEffect("fire_resistance") != nullptr;
    v.spectator = s.spectator();
    return v;
}

void Ui::warpCamera(CameraState& cam) const {
    const State& s = *s_;
    const float pt = s.partialTick;
    const float portal = s.portalIntensityO + (s.portalIntensity - s.portalIntensityO) * pt;
    const float effects = float(s.settings.screenEffectScale);
    const float intensity = std::max(portal, s.effectBlend("nausea")) * effects * effects;  // x screenEffectScale^2
    if (intensity <= 0.0f) return;
    float skew = 5.0f / (intensity * intensity + 5.0f) - intensity * 0.04f;
    skew *= skew;
    // projection.rotate(angle, n).scale(1 / skew, 1, 1).rotate(-angle, n), n = (0, sqrt(1/2), sqrt(1/2)), in the game's
    // view space (-z forward); ours has +z forward: conjugated by diag(1, 1, -1).
    const float angle = (s.spinTime + pt * s.spinSpeed) * kPi / 180.0f;
    const float n[3] = {0.0f, std::sqrt(0.5f), std::sqrt(0.5f)};
    auto rotation = [&](float a, float r[3][3]) {  // Rodrigues
        const float c = std::cos(a), sn = std::sin(a);
        const float k[3][3] = {{0, -n[2], n[1]}, {n[2], 0, -n[0]}, {-n[1], n[0], 0}};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r[i][j] = (i == j ? c : 0.0f) + sn * k[i][j] + (1 - c) * n[i] * n[j];
    };
    float ra[3][3], rb[3][3];
    rotation(angle, ra), rotation(-angle, rb);
    const float flip[3] = {1, 1, -1};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float w = 0;
            for (int k = 0; k < 3; ++k) w += ra[i][k] * (k == 0 ? 1.0f / skew : 1.0f) * rb[k][j];
            cam.warp[i * 3 + j] = flip[i] * w * flip[j];
        }
    cam.warpSet = true;
}

void drawScreenEffects(Ui::State& s, Canvas& c) {
    if (!s.game || s.screen == Screen::InBed) return;  // not while sleeping
    const float W = float(c.guiWidth()), H = float(c.guiHeight());
    const Player& p = s.game->player();
    const CameraState cam = s.game->camera();
    // GameRenderer.render3dHud: the HUD projection (Camera.calculateHudFov: 70 degrees by death or fluid, with the FOV
    // Effects scale).
    const float fov = s.fovDeathOrFluid(70.0f) * kPi / 180.0f;
    const float f = 1.0f / std::tan(fov * 0.5f), aspect = W / std::max(H, 1.0f);
    auto project = [&](float x, float y, float z, float out[2]) {
        out[0] = (f / aspect * x / -z + 1.0f) * 0.5f * W;
        out[1] = (1.0f - f * y / -z) * 0.5f * H;
    };
    // The block the eyes are in (LevelExtractor.getViewBlockingState): its texture, darkened, mirrored.
    if (!s.spectator()) {
        const double eye = p.pos[1] + p.eyeHeight();
        for (int i = 0; i < 8; ++i) {
            const double x = p.pos[0] + ((i & 1) - 0.5) * p.width() * 0.8;
            const double y = eye + (((i >> 1) & 1) - 0.5) * 0.1;
            const double z = p.pos[2] + (((i >> 2) & 1) - 0.5) * p.width() * 0.8;
            const BlockInfo& info = s.registry->infos()[s.world->block(int(std::floor(x)), int(std::floor(y)), int(std::floor(z)))];
            if (!info.occludes) continue;
            static std::vector<std::string> textureNames;
            if (textureNames.empty()) textureNames = s.registry->textureNames();
            const Sprite* tex = info.icon < textureNames.size() ? s.sprites.texture(textureNames[info.icon]) : nullptr;
            if (tex) {
                float a[2], b[2];
                project(-1, 1, -0.5f, a), project(1, -1, -0.5f, b);
                c.quad(a[0], a[1], b[0], b[1], tex->u1, tex->v0, tex->u0, tex->v1, 0xFF191919, ModeGui);
            }
            break;
        }
    }
    if (s.game->cameraMode != 0 || s.spectator()) return;
    // Underwater: misc/underwater, 4 x 4 repeats over the quad, scrolled by the look direction, at 10% (the light).
    if (s.eyeInWater) {
        if (const Sprite* tex = s.sprites.texture("misc/underwater")) {
            const float uOff = -p.yaw * 180.0f / kPi / 64.0f, vOff = p.pitch * 180.0f / kPi / 64.0f;
            // The quad spans x, y in -1..1 at z = -0.5; u = uOff + 2 (1 - x), v = vOff + 2 (1 - y).
            const float ex = aspect / (2.0f * f), ey = 1.0f / (2.0f * f);  // screen edges at that depth
            const int ex0 = int(std::floor(cam.x)), ey0 = int(std::floor(cam.y)), ez0 = int(std::floor(cam.z));
            float level;
            {
                std::shared_lock lock(s.world->mutex());
                const World::LightView lv = s.world->light(ex0 >> 4, ey0 >> 4, ez0 >> 4);
                level = float(std::max(lv.sky4(ex0 & 15, ey0 & 15, ez0 & 15), lv.block4(ex0 & 15, ey0 & 15, ez0 & 15))) / 15.0f;
            }
            const float bright = level / (4.0f - 3.0f * level);  // Lightmap.getBrightness
            const int b = int(bright * 255.0f);
            wrapped(c, tex, uOff + 2 * (1 + ex), uOff + 2 * (1 - ex), vOff + 2 * (1 - ey), vOff + 2 * (1 + ey),
                    argb(int(0.1f * 255.0f), b, b, b));
        }
    }
    // On fire: two block/fire_1 quads low in the corners, turned 10 degrees towards the middle.
    if (s.onFire) {
        if (const Sprite* tex = s.sprites.texture("block/fire_1")) {
            for (const float side : {1.0f, -1.0f}) {
                const float ang = -side * kPi / 18.0f, cs = std::cos(ang), sn = std::sin(ang);
                const float corners[4][2] = {{-0.5f, 0.5f}, {-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}};  // TL BL BR TR
                float xy[4][2], uv[4][2];
                for (int k = 0; k < 4; ++k) {
                    const float lx = corners[k][0], ly = corners[k][1], lz = -0.5f;
                    project(lx * cs + lz * sn + 0.24f * side, ly - 0.3f, -lx * sn + lz * cs, xy[k]);
                    uv[k][0] = lx < 0 ? tex->u1 : tex->u0;  // mirrored, like buildSpriteQuad
                    uv[k][1] = ly < 0 ? tex->v1 : tex->v0;
                }
                c.quad4(xy, uv, 0xE5FFFFFF, ModeGui);
            }
        }
    }
}

void drawEffectOverlays(Ui::State& s, Canvas& c, bool scoping) {
    const int gw = c.guiWidth(), gh = c.guiHeight();
    // Equippable camera overlays in first person (not while scoping): every equipment slot (EquipmentSlot order) whose
    // item's equippable component belongs in that slot and has a camera overlay (the carved pumpkin's misc/pumpkinblur).
    if (s.game->cameraMode == 0 && !scoping && s.inventory.slots.size() > 45) {
        static const std::pair<int, int> kSlots[6] = {{0, -1}, {5, 45}, {1, 8}, {2, 7}, {3, 6}, {4, 5}};  // slot id, menu slot
        for (const auto& [slotId, menu] : kSlots) {
            const ItemStack& st = s.inventory.slots[size_t(menu < 0 ? 36 + s.game->selectedSlot() : menu)];
            if (st.empty()) continue;
            int slot = -1;
            std::string overlay;
            const Items::ComponentValue v = s.items.component(st, "equippable");
            if (v.patched) {
                slot = st.equipSlot, overlay = st.equipCameraOverlay;
            } else if (v.fallback && v.fallback->is_object()) {
                static const char* const kNames[8] = {"mainhand", "feet", "legs", "chest", "head", "offhand", "body", "saddle"};
                const std::string name = v.fallback->value("slot", "");
                for (int i = 0; i < 8; ++i)
                    if (name == kNames[i]) slot = i;
                overlay = v.fallback->value("camera_overlay", "");
            }
            if (slot != slotId || overlay.empty()) continue;
            const size_t colon = overlay.find(':');
            if (const Sprite* tex = s.sprites.texture(colon == std::string::npos ? overlay : overlay.substr(colon + 1)))
                blitTexture(c, tex, 0, 0, 0, 0, float(gw), float(gh), gw, gh);  // extractTextureOverlay, alpha 1
        }
    }
}

void drawPortalOverlay(Ui::State& s, Canvas& c) {
    // Hud.extractPortalOverlay: the nether portal texture over the screen, fading in with the portal intensity.
    float alpha = s.portalIntensityO + (s.portalIntensity - s.portalIntensityO) * s.partialTick;
    if (alpha <= 0.0f) {
        // Hud.extractConfusionOverlay: with Distortion Effects below 1, nausea tints the screen instead of warping it
        // as much (misc/nausea, additive, scaled 2 -> 1 about the centre).
        const float nausea = s.effectBlend("nausea"), scale = float(s.settings.screenEffectScale);
        if (nausea <= 0.0f || scale >= 1.0f) return;
        const float strength = nausea * (1.0f - scale);
        const Sprite* tex = s.sprites.texture("misc/nausea");
        if (!tex) return;
        const float gw = float(c.guiWidth()), gh = float(c.guiHeight()), size = 2.0f + (1.0f - 2.0f) * strength;
        const u32 rgb = (u32(0.2f * strength * 255.0f) << 16) | (u32(0.4f * strength * 255.0f) << 8) | u32(0.2f * strength * 255.0f);
        c.setBlend(4);
        blitTexture(c, tex, gw * 0.5f * (1.0f - size), gh * 0.5f * (1.0f - size), 0, 0, gw * size, gh * size, int(gw * size), int(gh * size),
                    0xFF000000u | rgb);
        c.setBlend(0);
        return;
    }
    if (alpha < 1.0f) alpha = alpha * alpha * alpha * alpha * 0.8f + 0.2f;
    if (const Sprite* tex = s.sprites.texture("block/nether_portal"))
        c.quad(0, 0, float(c.guiWidth()), float(c.guiHeight()), tex->u0, tex->v0, tex->u1, tex->v1, withAlpha(0xFFFFFF, alpha), ModeGui);
}

}  // namespace ui
