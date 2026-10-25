p = 'src/ui/gameplay_hud.h'
s = open(p, encoding='utf-8').read()
a = """// Before the crosshair: camera overlays (spyglass scope while scoping in first person).
void drawCameraOverlays(Ui::State& s, Canvas& c);"""
b = """// Before the crosshair: camera overlays (spyglass scope while scoping in first person, carved pumpkin, powder snow
// frost, nether portal).
void drawCameraOverlays(Ui::State& s, Canvas& c);
// view_effects.cpp: ScreenEffectRenderer's screen effects (in-wall block, underwater, on fire), before the whole HUD;
// the equippable camera overlay (carved pumpkin) unless scoping; the nether portal overlay.
void drawScreenEffects(Ui::State& s, Canvas& c);
void drawEffectOverlays(Ui::State& s, Canvas& c, bool scoping);
void drawPortalOverlay(Ui::State& s, Canvas& c);"""
assert a in s
s = s.replace(a, b)
open(p, 'w', encoding='utf-8').write(s)

p = 'src/ui/gameplay_hud.cpp'
s = open(p, encoding='utf-8').read()
a = """void drawCameraOverlays(Ui::State& s, Canvas& c) {
    drawSpyglass(s, c);
    // Hud.extractCameraOverlays, after the scope: the powder snow frost by Entity.getPercentFrozen.
    if (s.ticksFrozen > 0) textureOverlay(c, s.sprites.texture("misc/powder_snow_outline"), float(std::min(s.ticksFrozen, 140)) / 140.0f);
}"""
b = """void drawCameraOverlays(Ui::State& s, Canvas& c) {
    drawSpyglass(s, c);
    const Game::ItemUse& use = s.game->use;
    drawEffectOverlays(s, c, use.active && ItemRules::Anim(use.anim) == ItemRules::Anim::Spyglass);  // view_effects.cpp
    // Hud.extractCameraOverlays, after the scope: the powder snow frost by Entity.getPercentFrozen.
    if (s.ticksFrozen > 0) textureOverlay(c, s.sprites.texture("misc/powder_snow_outline"), float(std::min(s.ticksFrozen, 140)) / 140.0f);
    drawPortalOverlay(s, c);
}"""
assert a in s
s = s.replace(a, b)
open(p, 'w', encoding='utf-8').write(s)

p = 'src/ui/ui.cpp'
s = open(p, encoding='utf-8').read()
a = """    const bool hud = !s.hudHidden && !s.fe->cfg.menu;  // the title screen has no HUD
    if (hud) s.drawHud(c);"""
b = """    s.sprites.tick(u64(s.guiTicks));  // animated GUI sprites and textures
    if (!s.fe->cfg.menu) drawScreenEffects(s, c);  // view_effects.cpp: in the 3D view, with or without the HUD
    const bool hud = !s.hudHidden && !s.fe->cfg.menu;  // the title screen has no HUD
    if (hud) s.drawHud(c);"""
assert a in s
s = s.replace(a, b)
if '#include "ui/gameplay_hud.h"' not in s:
    s = s.replace('#include "ui/ui_state.h"', '#include "ui/gameplay_hud.h"\n#include "ui/ui_state.h"', 1)
a = """float Ui::fovRadians() const {  // Camera.getFov: the option scaled by the dynamic FOV modifier
    const float m = s_->oldFovModifier + (s_->fovModifier - s_->oldFovModifier) * s_->partialTick;
    return s_->settings.fov * m * kPi / 180.0f;
}"""
b = """float Ui::fovRadians() const {  // Camera.getFov: the option scaled by the dynamic FOV modifier
    const float m = s_->oldFovModifier + (s_->fovModifier - s_->oldFovModifier) * s_->partialTick;
    // modifyFovBasedOnDeathOrFluid: x 6/7 with the eyes in water or lava.
    const CameraState cam = s_->game->camera();
    const std::string& eye = s_->registry->name(s_->world->block(int(std::floor(cam.x)), int(std::floor(cam.y)), int(std::floor(cam.z))));
    const float fluid = s_->eyeInWater || eye.starts_with("minecraft:lava") ? 0.85714287f : 1.0f;
    return s_->settings.fov * m * fluid * kPi / 180.0f;
}"""
assert a in s
s = s.replace(a, b)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
