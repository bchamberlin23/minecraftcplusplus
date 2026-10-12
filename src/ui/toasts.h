#pragma once
// Toasts like the game's (ToastManager): panels that slide in at the top right, five 32-pixel slots, drawn over every
// screen. Main thread only.
//  - SystemToast: the "toast/system" panel with a yellow title and white message lines, five seconds;
//  - AdvancementToast: "Advancement Made!" / "Goal Reached!" / "Challenge Complete!" with the advancement's icon;
//  - TutorialToast: an icon, a title and a message (and a progress bar), shown until the tutorial hides it.
#include "ui/text.h"

#include <string>

namespace ui {

struct ItemStack;

void showToast(const Text& title, const Text& message = Text());
// type: AdvancementType (0 task, 1 challenge, 2 goal).
void showAdvancementToast(const Text& title, int type, const ItemStack& icon);
// icon: a "toast/<icon>" sprite ("movement_keys", "mouse", "tree", "recipe_book", "wooden_planks"). Returns a handle.
int showTutorialToast(const std::string& icon, const Text& title, const Text& message, bool progressable);
void setTutorialToastProgress(int handle, float progress);
void hideTutorialToast(int handle);

}  // namespace ui
