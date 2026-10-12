#pragma once
// Toasts like the game's (ToastManager / SystemToast): the "toast/system" panel with a yellow title and white message
// lines slides in at the top right, stays five seconds and slides out; drawn over every screen. Main thread only.
#include "ui/text.h"

namespace ui {

// id: SystemToast.addOrUpdate (a toast with the same id shows the new text and starts its time again).
void showToast(const Text& title, const Text& message = Text(), const std::string& id = {});

}  // namespace ui
