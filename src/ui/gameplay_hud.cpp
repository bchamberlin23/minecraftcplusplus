#include "ui/gameplay_hud.h"

#include "game/item_rules.h"

#include <algorithm>
#include <cmath>

namespace ui {

static void drawSpyglass(Ui::State& s, Canvas& c);

// Hud.extractTextureOverlay: a full-screen texture tinted white with `alpha`.
static void textureOverlay(Canvas& c, const Sprite* tex, float alpha) {
    if (!tex) return;
    const int gw = c.guiWidth(), gh = c.guiHeight();
    const u32 color = (u32(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f) << 24) | 0xFFFFFF;
    blitTexture(c, tex, 0, 0, 0, 0, float(gw), float(gh), gw, gh, color);
}

void drawCameraOverlays(Ui::State& s, Canvas& c) {
    drawSpyglass(s, c);
    const Game::ItemUse& use = s.game->use;
    drawEffectOverlays(s, c, use.active && ItemRules::Anim(use.anim) == ItemRules::Anim::Spyglass);  // view_effects.cpp
    // Hud.extractCameraOverlays, after the scope: the powder snow frost by Entity.getPercentFrozen.
    if (s.ticksFrozen > 0) textureOverlay(c, s.sprites.texture("misc/powder_snow_outline"), float(std::min(s.ticksFrozen, 140)) / 140.0f);
    drawPortalOverlay(s, c);
}

static void drawSpyglass(Ui::State& s, Canvas& c) {
    // Hud.extractCameraOverlays: the scope grows from half size towards 1.125 (lerp 0.5 per game tick).
    static float scopeScale = 0.5f;
    const float deltaTicks = float(s.frameMs / 50.0);
    scopeScale = scopeScale + std::min(0.5f * deltaTicks, 1.0f) * (1.125f - scopeScale);
    const Game::ItemUse& use = s.game->use;
    const bool scoping = use.active && ItemRules::Anim(use.anim) == ItemRules::Anim::Spyglass;
    if (s.game->cameraMode != 0 || !scoping) {
        scopeScale = 0.5f;
        return;
    }
    // extractSpyglassOverlay: the scope texture square in the middle, black around it.
    const Sprite* scope = s.sprites.texture("misc/spyglass_scope");
    const float gw = float(c.guiWidth()), gh = float(c.guiHeight());
    const float src = std::min(gw, gh);
    const float ratio = std::min(gw / src, gh / src) * scopeScale;
    const float w = std::floor(src * ratio), h = std::floor(src * ratio);
    const float left = std::floor((gw - w) / 2), top = std::floor((gh - h) / 2), right = left + w, bottom = top + h;
    if (scope) blitTexture(c, scope, left, top, 0, 0, w, h, int(w), int(h));
    c.fill(0, bottom, gw, gh, 0xFF000000);
    c.fill(0, 0, gw, top, 0xFF000000);
    c.fill(0, top, left, bottom, 0xFF000000);
    c.fill(right, top, gw, bottom, 0xFF000000);
}

int drawVehicleHealth(Ui::State& s, Canvas& c, int xRight, int yBase) {
    const Game::VehicleHud& v = s.game->vehicleHud;
    if (v.hearts <= 0) return 0;
    const int current = int(std::ceil(v.health));
    int hearts = v.hearts, y = yBase;
    for (int base = 0; hearts > 0; base += 20) {
        const int row = std::min(hearts, 10);
        hearts -= row;
        for (int i = 0; i < row; ++i) {
            const float x = float(xRight - i * 8 - 9);
            blitSprite(c, s.sprites.sprite("hud/heart/vehicle_container"), x, float(y), 9, 9);
            if (i * 2 + 1 + base < current) blitSprite(c, s.sprites.sprite("hud/heart/vehicle_full"), x, float(y), 9, 9);
            if (i * 2 + 1 + base == current) blitSprite(c, s.sprites.sprite("hud/heart/vehicle_half"), x, float(y), 9, 9);
        }
        y -= 10;
    }
    return v.hearts;
}

void drawAttackIndicator(Ui::State& s, Canvas& c) {
    if (s.settings.attackIndicator != 1) return;  // AttackIndicatorStatus.CROSSHAIR
    const Game& g = *s.game;
    const float scale = g.attackStrengthScale(0.0f);
    const bool full = g.crosshairOnLiving && scale >= 1.0f && g.attackStrengthDelay() > 5.0f;
    const int y = c.guiHeight() / 2 - 7 + 16, x = c.guiWidth() / 2 - 8;
    c.setInvert(true);  // RenderPipelines.CROSSHAIR
    if (full) {
        blitSprite(c, s.sprites.sprite("hud/crosshair_attack_indicator_full"), float(x), float(y), 16, 16);
    } else if (scale < 1.0f) {
        const int progress = int(scale * 17.0f);
        blitSprite(c, s.sprites.sprite("hud/crosshair_attack_indicator_background"), float(x), float(y), 16, 4);
        if (progress > 0)
            blitSpriteRegion(c, s.sprites.sprite("hud/crosshair_attack_indicator_progress"), 16, 4, 0, 0, float(x), float(y),
                             float(progress), 4);
    }
    c.setInvert(false);
}

void drawHotbarAttackIndicator(Ui::State& s, Canvas& c) {
    if (s.settings.attackIndicator != 2) return;  // AttackIndicatorStatus.HOTBAR (Hud.extractItemHotbar)
    const float scale = s.game->attackStrengthScale(0.0f);
    if (scale >= 1.0f) return;
    const int y = c.guiHeight() - 20, x = c.guiWidth() / 2 + 91 + 6;  // beside the hotbar, away from the off hand (left)
    const int progress = int(scale * 19.0f);
    blitSprite(c, s.sprites.sprite("hud/hotbar_attack_indicator_background"), float(x), float(y), 18, 18);
    if (progress > 0)
        blitSpriteRegion(c, s.sprites.sprite("hud/hotbar_attack_indicator_progress"), 18, 18, 0, 18 - progress, float(x),
                         float(y + 18 - progress), 18, float(progress));
}

bool drawJumpBar(Ui::State& s, Canvas& c, int left, int top) {
    const Game::VehicleHud& v = s.game->vehicleHud;
    if (!v.jumpable) return false;
    blitSprite(c, s.sprites.sprite("hud/jump_bar_background"), float(left), float(top), 182, 5);
    if (v.jumpCooldown > 0) {
        blitSprite(c, s.sprites.sprite("hud/jump_bar_cooldown"), float(left), float(top), 182, 5);
    } else {
        const int progress = int(std::clamp(v.jumpScale, 0.0f, 1.0f) * 182.0f);  // Mth.lerpDiscrete(scale, 0, 182)
        if (progress > 0)
            blitSpriteRegion(c, s.sprites.sprite("hud/jump_bar_progress"), 182, 5, 0, 0, float(left), float(top), float(progress), 5);
    }
    return true;
}

}  // namespace ui
