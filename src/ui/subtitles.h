#pragma once
// Subtitles (SubtitleOverlay): drawn by the HUD, and by the pause menu over its background.
#include "ui/ui.h"

namespace ui {

class Canvas;
void drawSubtitles(Ui::State& s, Canvas& c);

}  // namespace ui
