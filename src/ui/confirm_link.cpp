// ConfirmLinkScreen: "open this website?" before a link opens in the browser (chat links with Prompt on Links, the
// Accessibility Guide): the title, the address, the warning for untrusted links, then Yes / Open in Browser, Copy to
// Clipboard and No / Cancel (100 wide, 4 apart, 16 below), all centred like ConfirmScreen's layout.
#include "ui/frontend.h"
#include "ui/options_list.h"

#include <shellapi.h>

namespace ui {

namespace {

struct LinkState {
    std::string url;
    bool trusted = true;
    Screen back = Screen::None;
};
LinkState g;

std::vector<Text> messageLines(UiState& s) {  // MultiLineTextWidget: max width - 50, at most 15 rows
    std::vector<Text> lines = s.font.split(Text(g.url), std::max(1, s.guiW() - 50));
    if (lines.size() > 15) lines.resize(15);
    return lines;
}

float layoutTop(UiState& s, size_t messageRows) {
    const float h = 9 + 8 + float(messageRows * 9) + (g.trusted ? 0 : 8 + 9) + 8 + 16 + 20;
    return std::floor((float(s.guiH()) - h) / 2);
}

void done(UiState& s, bool open) {
    if (open) openUrlInBrowser(g.url);
    if (g.back == Screen::None) s.closeScreen();
    else s.openScreen(g.back);
}

void build(UiState& s) {
    s.buttons.clear();
    const std::vector<Text> lines = messageLines(s);
    const float y = layoutTop(s, lines.size()) + 9 + 8 + float(lines.size() * 9) + (g.trusted ? 0 : 8 + 9) + 8 + 16;
    float x = float((s.guiW() - 308) / 2);
    UiState* p = &s;
    auto add = [&](const char* key, std::function<void()> fn) {
        Button b;
        b.label = Text(s.res.translate(key));
        b.x = x, b.y = y, b.w = 100, b.h = 20;
        b.onPress = std::move(fn);
        s.buttons.push_back(std::move(b));
        x += 104;
    };
    add(g.trusted ? "chat.link.open" : "gui.yes", [p] { done(*p, true); });
    add("chat.copy", [p] {
        p->window->setClipboard(g.url);
        done(*p, false);
    });
    add(g.trusted ? "gui.cancel" : "gui.no", [p] { done(*p, false); });
}

void draw(UiState& s, Canvas& c) {
    menuBackground(s, c);
    const std::vector<Text> lines = messageLines(s);
    float y = layoutTop(s, lines.size());
    const float cx = float(c.guiWidth() / 2);
    s.drawCentered(c, Text(s.res.translate(g.trusted ? "chat.link.confirmTrusted" : "chat.link.confirm")), cx, y, 0xFFFFFFFF);
    y += 9 + 8;
    for (const Text& l : lines) {
        s.drawCentered(c, l, cx, y, 0xFFFFFFFF);
        y += 9;
    }
    if (!g.trusted) s.drawCentered(c, Text(s.res.translate("chat.link.warning")), cx, y + 8, 0xFFFFCCCC);
    s.drawButtons(c);
}

void input(UiState& s, const Window::InputEvent& e) {
    if (e.type == Window::InputEvent::Key && e.down && e.code == VK_ESCAPE) return done(s, false);
    s.buttonsInput(e);
}

const ScreenRegistrar kConfirmLink({Screen::ConfirmLink, build, draw, input, nullptr});

}  // namespace

void openUrlInBrowser(const std::string& url) {
    if (!url.starts_with("http://") && !url.starts_with("https://")) return;
    const int n = MultiByteToWideChar(CP_UTF8, 0, url.data(), int(url.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.data(), int(url.size()), w.data(), n);
    LOG("ui: opening %s in the browser", url.c_str());
    if (!std::getenv("MCPP_NO_BROWSER")) ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void openConfirmLink(UiState& s, const std::string& url, Screen back, bool trusted) {
    g = {url, trusted, back};
    s.openScreen(Screen::ConfirmLink);
}

}  // namespace ui
