// The tutorial (client/tutorial: Tutorial, TutorialSteps): hints for a new player as toasts, one step at a time,
// the step kept in options.txt (tutorialStep): move and look around, then in survival find a tree, punch it, open the
// inventory and craft planks.
#include "assets/assets.h"
#include "game/game.h"
#include "ui/items.h"
#include "ui/modules.h"
#include "ui/toasts.h"
#include "ui/ui_state.h"
#include "world/world.h"

#include <cmath>

namespace ui {

namespace {

enum class Step { Movement, FindTree, PunchTree, OpenInventory, CraftPlanks, None };
const char* kNames[] = {"movement", "find_tree", "punch_tree", "open_inventory", "craft_planks", "none"};

struct State {
    bool started = false;
    Step step = Step::None;
    int timeWaiting = 0;
    // Movement
    int moved = 0, looked = 0, moveCompleted = -1, lookCompleted = -1;
    float lastYaw = 0, lastPitch = 0;
    bool haveLook = false;
    // Punch tree
    int resets = 0;
    bool wasBreaking = false;
    int breakX = 0, breakY = 0, breakZ = 0;  // the log being broken
    int toast = 0, toast2 = 0;  // TutorialToast handles (0: none)
} g;

Step stepOf(const std::string& name) {  // TutorialSteps.getByName: unknown names are NONE
    for (int i = 0; i < 6; ++i)
        if (name == kNames[i]) return Step(i);
    return Step::None;
}

void hideToasts() {
    if (g.toast) hideTutorialToast(g.toast), g.toast = 0;
    if (g.toast2) hideTutorialToast(g.toast2), g.toast2 = 0;
}

void setStep(UiState& s, Step step) {  // Tutorial.setStep: saved at once, a fresh instance
    hideToasts();
    g.step = step, g.timeWaiting = 0, g.moved = g.looked = 0, g.moveCompleted = g.lookCompleted = -1, g.resets = 0;
    s.settings.tutorialStep = kNames[int(step)];
    s.settings.save();
}

Text key(UiState& s, Key k) {  // Tutorial.key: the bound key's name, bold
    Style bold;
    bold.bold = 1;
    return s.keyLabel(s.settings.key(k)).withStyle(bold);
}

// completes_find_tree_tutorial (#logs, #leaves, #wart_blocks), as block and item names.
bool treeName(std::string_view n) {
    if (n.starts_with("minecraft:")) n.remove_prefix(10);
    return n.ends_with("_log") || n.ends_with("_wood") || n.ends_with("_stem") || n.ends_with("_hyphae") ||
           n.ends_with("_leaves") || n == "nether_wart_block" || n == "warped_wart_block";
}
bool logName(std::string_view n) {  // #logs
    if (n.starts_with("minecraft:")) n.remove_prefix(10);
    return n.ends_with("_log") || n.ends_with("_wood") || n.ends_with("_stem") || n.ends_with("_hyphae");
}

bool hasItem(UiState& s, bool (*match)(std::string_view)) {
    for (const ItemStack& st : s.inventory.slots)
        if (st.item >= 0 && st.count > 0 && match(s.items.name(st.item))) return true;
    return false;
}
bool planks(std::string_view n) { return n.ends_with("_planks"); }

std::string targetBlock(UiState& s) {
    const RayHit& t = s.game->target();
    if (!t.hit || !s.world || !s.registry) return {};
    const std::string& name = s.registry->name(s.world->block(t.x, t.y, t.z));
    return name.substr(0, name.find('['));
}

void tick(UiState& s) {
    if (!s.game || !s.online()) {  // Tutorial.tick: stopped without a level
        if (g.started) hideToasts(), g.started = false;
        return;
    }
    if (!g.started) {
        g.started = true;
        g.step = stepOf(s.settings.tutorialStep);
        g.timeWaiting = 0, g.moved = g.looked = 0, g.moveCompleted = g.lookCompleted = -1, g.haveLook = false;
    }
    const bool survival = s.game->gameMode == 0;  // Tutorial.isSurvival
    ++g.timeWaiting;
    switch (g.step) {
        case Step::None: return;
        case Step::Movement: {  // MovementTutorialStepInstance
            const Player& p = s.game->player();
            const PlayerInput& in = p.lastInput;
            if (in.forward != 0 || in.strafe != 0 || in.jump) ++g.moved;
            if (g.haveLook && (std::abs(p.yaw - g.lastYaw) > 1e-4f || std::abs(p.pitch - g.lastPitch) > 1e-4f)) ++g.looked;
            g.lastYaw = p.yaw, g.lastPitch = p.pitch, g.haveLook = true;
            if (g.moveCompleted < 0 && g.moved > 40) {
                g.moveCompleted = g.timeWaiting;
                if (g.toast) hideTutorialToast(g.toast), g.toast = 0;
            }
            if (g.lookCompleted < 0 && g.looked > 40) {
                g.lookCompleted = g.timeWaiting;
                if (g.toast2) hideTutorialToast(g.toast2), g.toast2 = 0;
            }
            if (g.moveCompleted >= 0 && g.lookCompleted >= 0) return setStep(s, survival ? Step::FindTree : Step::None);
            if (g.timeWaiting >= 100 && g.moveCompleted < 0 && !g.toast)
                g.toast = showTutorialToast("movement_keys",
                                            translatable("tutorial.move.title", {key(s, Key::Forward), key(s, Key::Left), key(s, Key::Back), key(s, Key::Right)}, s.lang()),
                                            translatable("tutorial.move.description", {key(s, Key::Jump)}, s.lang()), true);
            if (g.moveCompleted >= 0 && g.timeWaiting - g.moveCompleted >= 20 && g.lookCompleted < 0 && !g.toast2)
                g.toast2 = showTutorialToast("mouse", Text(s.res.translate("tutorial.look.title")),
                                             Text(s.res.translate("tutorial.look.description")), true);
            if (g.toast) setTutorialToastProgress(g.toast, std::min(1.0f, float(g.moved) / 40.0f));
            if (g.toast2) setTutorialToastProgress(g.toast2, std::min(1.0f, float(g.looked) / 40.0f));
            return;
        }
        case Step::FindTree: {  // FindTreeTutorialStepInstance
            if (!survival) return setStep(s, Step::None);
            if (hasItem(s, treeName)) return setStep(s, Step::CraftPlanks);
            if (treeName(targetBlock(s))) return setStep(s, Step::PunchTree);
            if (g.timeWaiting >= 6000 && !g.toast)
                g.toast = showTutorialToast("tree", Text(s.res.translate("tutorial.find_tree.title")),
                                            Text(s.res.translate("tutorial.find_tree.description")), false);
            return;
        }
        case Step::PunchTree: {  // PunchTreeTutorialStepInstance
            if (!survival) return setStep(s, Step::None);
            if (hasItem(s, logName)) return setStep(s, Step::CraftPlanks);
            const float progress = s.game->destroyProgressValue();
            const bool breaking = progress > 0 && logName(targetBlock(s));
            if (g.wasBreaking && !breaking) {
                // The log broke (onDestroyBlock at 1): open the inventory next; else the digging stopped (a reset).
                const std::string& was = s.registry->name(s.world->block(g.breakX, g.breakY, g.breakZ));
                if (!logName(was.substr(0, was.find('[')))) return setStep(s, Step::OpenInventory);
                ++g.resets;
            }
            if (breaking) g.breakX = s.game->target().x, g.breakY = s.game->target().y, g.breakZ = s.game->target().z;
            g.wasBreaking = breaking;
            if ((g.timeWaiting >= 600 || g.resets > 3) && !g.toast)
                g.toast = showTutorialToast("tree", Text(s.res.translate("tutorial.punch_tree.title")),
                                            translatable("tutorial.punch_tree.description", {key(s, Key::Attack)}, s.lang()), true);
            if (g.toast) setTutorialToastProgress(g.toast, breaking ? progress : 0.0f);
            return;
        }
        case Step::OpenInventory: {  // OpenInventoryTutorialStep
            if (!survival) return setStep(s, Step::None);
            if (s.screen == Screen::Inventory) return setStep(s, Step::CraftPlanks);
            if (g.timeWaiting >= 600 && !g.toast)
                g.toast = showTutorialToast("recipe_book", Text(s.res.translate("tutorial.open_inventory.title")),
                                            translatable("tutorial.open_inventory.description", {key(s, Key::Inventory)}, s.lang()), false);
            return;
        }
        case Step::CraftPlanks: {  // CraftPlanksTutorialStep
            if (!survival) return setStep(s, Step::None);
            if (hasItem(s, planks)) return setStep(s, Step::None);
            if (g.timeWaiting >= 1200 && !g.toast)
                g.toast = showTutorialToast("wooden_planks", Text(s.res.translate("tutorial.craft_planks.title")),
                                            Text(s.res.translate("tutorial.craft_planks.description")), false);
            return;
        }
    }
}

const UiModuleRegistrar kModule({.name = "tutorial", .tick = tick});

}  // namespace

}  // namespace ui
