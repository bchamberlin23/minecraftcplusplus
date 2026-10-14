// NoticeWithLinkScreen: a bold title, a message and Open in Browser / Copy Link to Clipboard / Back, centred as one
// column (rows 10 apart). The symlink warnings: a world (WorldOpenFlows.createWorldAccess, a symlink world in the
// list) or dropped packs (PackSelectionScreen.onFilesDrop).
#include "ui/frontend.h"
#include "ui/modules.h"
#include "ui/ui_state.h"

#include <shellapi.h>

namespace ui {

namespace {

struct Notice {
    Text title, message;
    std::string url;
    std::function<void()> onClose;
} g_notice;

std::vector<Text> messageLines(UiState& s) { return s.font.split(g_notice.message, s.guiW() - 50); }
float columnTop(UiState& s) {  // FrameLayout.centerInRectangle: title, message, buttons
    const float height = 9 + 10 + float(messageLines(s).size() * 9) + 10 + 20;
    return float(int((float(s.guiH()) - height) / 2));
}

void build(UiState& s) {
    s.buttons.clear();
    const float y = columnTop(s) + 9 + 10 + float(messageLines(s).size() * 9) + 10;
    const float x = float(s.guiW() / 2 - (120 * 3 + 5 * 2) / 2);
    UiState* p = &s;
    addButton(s, x, y, 120, Text(s.res.translate("chat.link.open")), [] {
        ShellExecuteA(nullptr, "open", g_notice.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
    addButton(s, x + 125, y, 120, Text(s.res.translate("gui.copy_link_to_clipboard")), [p] { p->window->setClipboard(g_notice.url); });
    addButton(s, x + 250, y, 120, Text(s.res.translate("gui.back")), [] {
        if (auto fn = g_notice.onClose) fn();
    });
}

void draw(UiState& s, Canvas& c) {
    menuBackground(s, c);
    const float W = float(c.guiWidth());
    float y = columnTop(s);
    s.drawCentered(c, g_notice.title, float(int(W / 2)), y, 0xFFFFFFFF);
    y += 9 + 10;
    for (const Text& line : messageLines(s)) {
        s.drawCentered(c, line, float(int(W / 2)), y, 0xFFFFFFFF);
        y += 9;
    }
    s.drawButtons(c);
}

void input(UiState& s, const InputEvent& e) {
    if (e.type == InputEvent::Key && e.down && e.code == VK_ESCAPE) {  // onClose
        if (auto fn = g_notice.onClose) fn();
        return;
    }
    s.buttonsInput(e);
}

const ScreenRegistrar kScreen({Screen::NoticeWithLink, build, draw, input, nullptr});

}  // namespace

// createWorldSymlinkWarningScreen / createPackSymlinkWarningScreen
void openSymlinkWarning(UiState& s, bool pack, std::function<void()> onClose) {
    static const std::string kHelp = "https://aka.ms/MinecraftSymLinks";  // CommonLinks.SYMLINK_HELP
    Style bold;
    bold.bold = 1;
    const std::string kind = pack ? "pack" : "world";
    g_notice = {Text(s.res.translate("symlink_warning.title." + kind), bold),
                translatable("symlink_warning.message." + kind, {Text(kHelp)}, s.lang()), kHelp, std::move(onClose)};
    s.openScreen(Screen::NoticeWithLink);
}

}  // namespace ui
