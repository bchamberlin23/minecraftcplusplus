// DatapackLoadFailureScreen (WorldOpenFlows.openWorldLoadLevelStem): the local server could not load the world's
// datapacks. Safe Mode starts it again with only the vanilla datapack (--safeMode); Back leaves for the world list.
// A failure in safe mode is the AlertScreen "Failed to load world in Safe Mode".
#include "ui/frontend.h"
#include "ui/modules.h"
#include "ui/ui_state.h"

namespace ui {

namespace {

std::function<void()> g_safeMode;

void build(UiState& s) {
    s.buttons.clear();
    const int W = s.guiW(), H = s.guiH();
    addButton(s, float(W / 2 - 155), float(H / 6 + 96), 150, Text(s.res.translate("datapackFailure.safeMode")), [&s] {
        std::function<void()> fn = g_safeMode;
        s.closeScreen();
        if (fn) fn();
    });
    addButton(s, float(W / 2 - 155 + 160), float(H / 6 + 96), 150, Text(s.res.translate("gui.back")),
              [&s] { requestTitle(s, "worlds"); });
}

void draw(UiState& s, Canvas& c) {
    menuBackground(s, c);
    const int W = c.guiWidth();
    float y = 70;  // MultiLineLabel of the title, width - 50 wide, centred lines 9 apart
    for (const Text& line : s.font.split(Text(s.res.translate("datapackFailure.title")), W - 50)) {
        s.drawCentered(c, line, float(W / 2), y, 0xFFFFFFFF);
        y += 9;
    }
    s.drawButtons(c);
}

void input(UiState& s, const InputEvent& e) {
    if (e.type == InputEvent::Key && e.down && e.code == VK_ESCAPE) return;  // shouldCloseOnEsc: false
    s.buttonsInput(e);
}

const ScreenRegistrar kScreen({Screen::DatapackFailure, build, draw, input, nullptr});

}  // namespace

void serverFailure(UiState& s, bool datapacks, bool safeMode, std::function<void()> safeModeFn) {
    UiState* sp = &s;
    auto back = [sp] { requestTitle(*sp, "worlds"); };
    if (datapacks && !safeMode) {
        g_safeMode = std::move(safeModeFn);
        s.openScreen(Screen::DatapackFailure);
    } else if (datapacks) {
        showMessage(s, Text(s.res.translate("datapackFailure.safeMode.failed.title")),
                    Text(s.res.translate("datapackFailure.safeMode.failed.description")), s.res.translate("gui.back"), back, true);
    } else {
        showMessage(s, Text("Could not open the world"), Text("The local server stopped while starting; see the log for details."),
                    s.res.translate("gui.back"), back, true);
    }
}

}  // namespace ui
