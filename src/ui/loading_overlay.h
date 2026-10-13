#pragma once
// LoadingOverlay (the Mojang Studios screen): start-up and resource reloads.
#include "ui/modules.h"

#include <functional>

namespace ui {

class Canvas;

// fadeIn: over the current screen first (reloads); reload runs once the overlay covers the screen (null: nothing to
// wait for, the overlay fades out right away).
void startLoadingOverlay(bool fadeIn, std::function<bool()> reload = {});
bool loadingOverlayActive();
void drawLoadingOverlay(UiState& s, Canvas& c);  // the end of the frame, under the toasts

}  // namespace ui
