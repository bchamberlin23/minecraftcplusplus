#pragma once
// HUD parts driven by gameplay state (game/game.h, game/gameplay.h): the spyglass scope (Hud.extractCameraOverlays).
// Called from hud.cpp.
#include "ui/ui_state.h"

namespace ui {

// Before the crosshair: camera overlays (spyglass scope while scoping in first person, carved pumpkin, powder snow
// frost, nether portal).
void drawCameraOverlays(Ui::State& s, Canvas& c);
// view_effects.cpp: ScreenEffectRenderer's screen effects (in-wall block, underwater, on fire), before the whole HUD;
// the equippable camera overlay (carved pumpkin) unless scoping; the nether portal overlay.
void drawScreenEffects(Ui::State& s, Canvas& c);
void drawEffectOverlays(Ui::State& s, Canvas& c, bool scoping);
void drawPortalOverlay(Ui::State& s, Canvas& c);
// Hud.extractVehicleHealth: the mount's hearts in place of the food bar; returns the hearts drawn (0: none).
int drawVehicleHealth(Ui::State& s, Canvas& c, int xRight, int yBase);
// JumpableVehicleBar in place of the experience bar; false when not riding a jumping mount.
bool drawJumpBar(Ui::State& s, Canvas& c, int left, int top);
// Hud.extractCrosshair with the attack indicator option on CROSSHAIR (the default): the attack strength under it.
void drawAttackIndicator(Ui::State& s, Canvas& c);
// The attack indicator option on HOTBAR: the attack strength beside the hotbar (Hud.extractItemHotbar).
void drawHotbarAttackIndicator(Ui::State& s, Canvas& c);
// bed_gamemode.cpp: Hud.extractSleepOverlay, the dark fade while asleep (drawn with or without the HUD).
void drawSleepOverlay(Ui::State& s, Canvas& c);

}  // namespace ui
