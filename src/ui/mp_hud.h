#pragma once
// Multiplayer HUD (mp_hud.cpp): player list (PlayerTabOverlay), boss bars (BossHealthOverlay), scoreboard sidebar;
// chat extras (chat_ext.cpp): command suggestions (CommandSuggestions) and chat click / hover events.
#include "ui/modules.h"
#include "ui/text.h"

namespace ui {

// Boss bar flags of the bars shown (BossHealthOverlay.shouldDarkenScreen / shouldCreateWorldFog / shouldPlayMusic):
// for the sky / fog / music code.
bool bossDarkenScreen();
bool bossWorldFog();
bool bossPlayMusic();

// Chat screen (screens.cpp, Screen::Chat): input first (true: consumed), drawing after the input box.
bool chatExtInput(UiState& s, const InputEvent& e);
void chatExtDraw(UiState& s, Canvas& c);
// hud.cpp: the chat's geometry (scale, width, line spacing options): the style under a GUI point, and clicks on the
// delayed-message line / the restricted prompt (true: handled).
bool chatLineStyleAt(UiState& s, float mx, float my, Style& out);
bool chatSpecialClick(UiState& s, float mx, float my);

}  // namespace ui
