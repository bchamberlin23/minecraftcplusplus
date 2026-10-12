#pragma once
// Toasts like the game's (ToastManager / SystemToast): the "toast/system" panel with a yellow title and white message
// lines slides in at the top right, stays five seconds and slides out; drawn over every screen. Main thread only.
#include "ui/text.h"

#include <functional>

namespace ui {

class Canvas;

void showToast(const Text& title, const Text& message = Text());
// A toast its owner draws (RecipeToast): while one of the same kind is up, another call only restarts its five
// seconds (ToastManager.getToast + addOrUpdate). draw gets the toast's top left and the seconds it has been shown.
void showCustomToast(const char* kind, int width, std::function<void(Canvas& c, float x, float y, double shownFor)> draw);

}  // namespace ui
