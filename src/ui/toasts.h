#pragma once
// Toasts like the game's (ToastManager): panels that slide in at the top right, five 32-pixel slots, drawn over every
// screen. Main thread only.
//  - SystemToast: the "toast/system" panel with a yellow title and white message lines, five seconds;
//  - AdvancementToast: "Advancement Made!" / "Goal Reached!" / "Challenge Complete!" with the advancement's icon;
//  - TutorialToast: an icon, a title and a message (and a progress bar), shown until the tutorial hides it.
#include "ui/text.h"
#include "ui/ui.h"

#include <functional>
#include <string>

namespace ui {

class Canvas;
struct ItemStack;

// id: SystemToast.addOrUpdate (a toast with the same id shows the new text and starts its time again); none:
// SystemToast.add (always a new one).
void showToast(const Text& title, const Text& message = Text(), const std::string& id = {}, double displaySeconds = 5.0);
// type: AdvancementType (0 task, 1 challenge, 2 goal).
void showAdvancementToast(const Text& title, int type, const ItemStack& icon);
// icon: a "toast/<icon>" sprite ("movement_keys", "mouse", "tree", "recipe_book", "wooden_planks"). Returns a handle.
// seconds > 0: it hides itself after that long, the bar filling with the time (TutorialToast.timeToDisplayMs).
int showTutorialToast(const std::string& icon, const Text& title, const Text& message, bool progressable, double seconds = 0);
void setTutorialToastProgress(int handle, float progress);
void hideTutorialToast(int handle);
// A toast drawing itself (RecipeToast): one per kind; showing it again while it is up updates it and restarts its
// five seconds. shownFor: seconds fully visible (ToastInstance.fullyVisibleFor).
void showCustomToast(const char* kind, int width, std::function<void(Canvas& c, float x, float y, double shownFor)> draw);
bool customToastActive(const char* kind);  // ToastManager.getToast: one of the kind queued or up
void forceHideToast(const std::string& id);  // SystemToast.forceHide: the toast with that id slides out
void clearToasts();                          // ToastManager.clear: all gone (Hud.onDisconnected)
// NowPlayingToast: the song playing (MusicManager) at x, y as the toast draws it (the pause menu draws it at 0, 0),
// and its music notes' colour cycle (tickMusicNotes).
void drawNowPlaying(Ui::State& s, Canvas& c, float x, float y);
void tickNowPlayingNotes(Ui::State& s);

}  // namespace ui
