#pragma once
// The F3 debug screen (debug_screen.cpp): DebugScreenOverlay with its configurable entries and the Debug Options
// screen, the F3 key combinations, the debug renderers.
#include "render/renderer.h"
#include "ui/modules.h"

#include <string_view>
#include <vector>

namespace ui {

void drawDebugScreen(UiState& s, Canvas& c);  // the overlay (always-on entries without F3 too)
bool debugEntryEnabled(UiState& s, std::string_view id);  // "chunk_borders", "3d_crosshair", ...
bool debugKeys(UiState& s, const InputEvent& e);  // bed_gamemode.cpp: the debug overlay key and F3 combinations
bool debugScreenKey(UiState& s, const InputEvent& e);     // an F3 + key combination (F3 held); true: handled
bool debugChart(int which);  // 0 fps (+ tps), 1 network, 2 profiler, 3 lightmap: shown
// The debug renderers' lines (camera-relative; the screen size for line widths).
void debugLines(UiState& s, std::vector<LineVertex>& out, const CameraState& cam, float screenW, float screenH);

}  // namespace ui
