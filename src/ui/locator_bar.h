#pragma once
// Waypoints (ClientWaypointManager) and the locator bar (LocatorBar), one of the HUD's contextual bars.
#include "ui/ui.h"

namespace ui {

class Canvas;
bool hasWaypoints();
void drawLocatorBarBackground(Ui::State& s, Canvas& c, int left, int top);
void drawLocatorBarDots(Ui::State& s, Canvas& c, int top);

}  // namespace ui
