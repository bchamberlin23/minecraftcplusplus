// Options screens laid out like the game's: OptionsScreen (FOV, Online..., the 2 x 5 grid of sub-screens, Done),
// Online Options, Skin Customization, Music & Sound Options, Controls, Mouse Settings, Key Binds, Accessibility
// Settings, Chat Settings and Font Settings, all on the OptionsSubScreen list layout (options_list.h). Video Settings
// are video_screen.cpp, Language language_screen.cpp. Every change is saved to options.txt (Settings) at once, like
// Options.save.
#include "ui/options_list.h"

#include "audio/audio.h"
#include "ui/frontend.h"
#include "ui/narrator.h"

#include <cmath>

namespace ui {

namespace {

constexpr int kHeaderH = 33, kFooterH = 33;

// The open list screen (OptionsList builds it; one at a time).
struct ListState {
    Screen screen = Screen::None, back = Screen::None;
    Text title, subtitle;
    u32 subtitleColor = 0xFFFFFFFF;
    struct Label {
        Text text;
        float x, y;  // y in list coordinates
        bool centred;
    };
    std::vector<Label> labels;
    std::vector<float> rowY;  // list buttons (the first rowY.size() buttons): y in list coordinates
    int rowWidth = 310, itemHeight = 25;
    float contentH = 4;  // AbstractSelectionList.contentHeight: rows + 4
    float scroll = 0;
    bool draggingBar = false;
    float dragStartY = 0, dragStartScroll = 0;
    struct Dropdown {  // an open choiceButton list
        size_t button = SIZE_MAX;  // the list widget it belongs to; SIZE_MAX: none open
        std::vector<Choice> choices;
        int selected = -1;
        std::function<void(int)> pick;
        int first = 0;   // the first visible choice (a long list scrolls)
        int cursor = -1;  // the choice the arrow keys are on
    } drop;
};
ListState g;

float listTop() { return float(kHeaderH); }
float listBottom(const UiState& s) { return float(s.guiH() - kFooterH); }
float maxScroll(const UiState& s) { return std::max(0.0f, g.contentH - (listBottom(s) - listTop())); }
float origin() { return listTop() + 2 - g.scroll; }  // AbstractSelectionList.getFirstEntryY - scroll
float scrollBarX(const UiState& s) { return float(s.guiW() / 2 + g.rowWidth / 2 + 6 + 2); }
float scrollerHeight(const UiState& s) {
    const float h = listBottom(s) - listTop();
    return std::clamp(float(int(h * h / g.contentH)), 32.0f, h - 8);
}

// Positions the list buttons for this frame's scroll.
void layout(UiState& s) {
    g.scroll = std::clamp(g.scroll, 0.0f, maxScroll(s));
    for (size_t i = 0; i < g.rowY.size() && i < s.buttons.size(); ++i) s.buttons[i].y = origin() + g.rowY[i];
}

// The open drop-down: under its widget (above it when there is more room there), at least as wide, 12 per row.
constexpr float kDropRowH = 12;
struct DropRect {
    float x, y, w, h;
    int rows;
};
bool dropOpen(const UiState& s) { return g.drop.button < s.buttons.size(); }
DropRect dropRect(UiState& s) {
    const Button& b = s.buttons[g.drop.button];
    float w = b.w;
    for (const Choice& ch : g.drop.choices) w = std::max(w, float(s.font.width(ch.label) + 14));
    w = std::min(w, float(s.guiW() - 4));
    const int n = int(g.drop.choices.size());
    const float below = float(s.guiH()) - 2 - (b.y + b.h), above = b.y - 2;
    const bool down = below >= float(n) * kDropRowH + 2 || below >= above;
    const int rows = std::clamp(int(((down ? below : above) - 2) / kDropRowH), 1, std::max(n, 1));
    const float h = float(rows) * kDropRowH + 2;
    return {std::clamp(b.x, 2.0f, float(s.guiW()) - 2 - w), down ? b.y + b.h : b.y - h, w, h, rows};
}

void drawDropdown(UiState& s, Canvas& c) {
    const DropRect r = dropRect(s);
    const int n = int(g.drop.choices.size());
    c.fill(r.x - 1, r.y - 1, r.x + r.w + 1, r.y + r.h + 1, 0xFFA0A0A0);
    c.fill(r.x, r.y, r.x + r.w, r.y + r.h, 0xF0000000);
    int hovered = -1;
    for (int k = 0; k < r.rows && g.drop.first + k < n; ++k) {
        const int i = g.drop.first + k;
        const Choice& ch = g.drop.choices[size_t(i)];
        const float y = r.y + 1 + float(k) * kDropRowH;
        if ((s.mouseX >= r.x && s.mouseX < r.x + r.w && s.mouseY >= y && s.mouseY < y + kDropRowH) || i == g.drop.cursor) {
            hovered = i;
            if (ch.active) c.fill(r.x, y, r.x + r.w, y + kDropRowH, 0xFF505050);
            s.requestCursor(ch.active ? UiState::CursorPointingHand : UiState::CursorNotAllowed);
        }
        const u32 color = !ch.active ? 0xFF707070 : i == g.drop.selected ? 0xFFFFFF55 : 0xFFFFFFFF;
        s.drawText(c, ch.label, r.x + 6, y + 2, color, true);
    }
    if (n > r.rows) {  // where the visible rows are in a longer list
        const float th = std::max(6.0f, r.h * float(r.rows) / float(n));
        const float ty = r.y + (r.h - th) * float(g.drop.first) / float(n - r.rows);
        c.fill(r.x + r.w - 2, ty, r.x + r.w, ty + th, 0xFFC0C0C0);
    }
    if (hovered >= 0 && g.drop.choices[size_t(hovered)].tooltip) {
        const std::vector<Text> lines = tooltipLines(s, g.drop.choices[size_t(hovered)].tooltip());
        if (!lines.empty()) s.drawTooltip(c, lines, s.mouseX, s.mouseY);
    }
}

void dropdownInput(UiState& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    const DropRect r = dropRect(s);
    const int n = int(g.drop.choices.size());
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) {
        g.drop = {};
    } else if (e.type == E::Key && e.down && (e.code == VK_UP || e.code == VK_DOWN)) {
        int i = g.drop.cursor < 0 ? g.drop.selected : g.drop.cursor;
        for (int k = 0; k < n; ++k) {  // the next available choice
            i = std::clamp(i + (e.code == VK_UP ? -1 : 1), 0, n - 1);
            if (g.drop.choices[size_t(i)].active) break;
        }
        g.drop.cursor = i;
        g.drop.first = std::clamp(g.drop.first, i - r.rows + 1, i);
    } else if (e.type == E::Key && e.down && (e.code == VK_RETURN || e.code == VK_SPACE) && g.drop.cursor >= 0) {
        const int i = g.drop.cursor;
        if (!g.drop.choices[size_t(i)].active) return;
        const auto pick = std::move(g.drop.pick);
        g.drop = {};
        audio::playUiClick();
        pick(i);
    } else if (e.type == E::Wheel) {
        g.drop.first = std::clamp(g.drop.first - (e.code > 0 ? 1 : e.code < 0 ? -1 : 0), 0, std::max(0, n - r.rows));
    } else if (e.type == E::Button && e.down) {
        if (s.mouseX >= r.x && s.mouseX < r.x + r.w && s.mouseY >= r.y && s.mouseY < r.y + r.h) {
            const int i = g.drop.first + int((s.mouseY - r.y - 1) / kDropRowH);
            if (i < 0 || i >= n || !g.drop.choices[size_t(i)].active) return;  // stays open
            const auto pick = std::move(g.drop.pick);
            g.drop = {};
            audio::playUiClick();
            pick(i);  // may rebuild the screen
        } else {
            g.drop = {};  // a click outside only closes it
        }
    }
}

Text boldUnderlined(Text t) {
    Style st;
    st.bold = 1, st.underlined = 1;
    return t.withStyle(st);
}

}  // namespace

// --- OptionsList -----------------------------------------------------------------------------------------------

OptionsList::OptionsList(UiState& s, Text title, Screen back) : s_(s) {
    if (back == Screen::Options) back = s.optionsParent;
    if (g.screen != s.screen) g.scroll = 0;  // a new screen starts at the top; a rebuild (resize, change) keeps it
    const float scroll = g.scroll;
    g = ListState{};
    g.screen = s.screen, g.back = back, g.title = std::move(title), g.scroll = scroll;
    s.buttons.clear();
    s.narrationTitle = g.title;
}

void OptionsList::subtitle(Text text, u32 color) { g.subtitle = std::move(text), g.subtitleColor = color; }

void OptionsList::header(Text text) {  // HeaderEntry: 18 high, 18 more above all but the first entry
    const float padding = g.contentH > 4 ? 18.0f : 0.0f;
    const float top = g.contentH - 4;
    g.labels.push_back({boldUnderlined(std::move(text)), float(s_.guiW() / 2), top + 2 + padding + 4, true});
    g.contentH += padding + 18;
}

void OptionsList::big(Button b) {
    b.x = float(s_.guiW() / 2 - 155), b.w = 310, b.h = 20;
    g.rowY.push_back(g.contentH - 4 + 2);
    s_.buttons.push_back(std::move(b));
    g.contentH += float(g.itemHeight);
}

void OptionsList::half(Button a) {
    a.x = float(s_.guiW() / 2 - 155), a.w = 150, a.h = 20;
    g.rowY.push_back(g.contentH - 4 + 2);
    s_.buttons.push_back(std::move(a));
    g.contentH += float(g.itemHeight);
}

void OptionsList::half(Button a, Button b) {  // OptionsList.Entry: x = width / 2 - 155, the next 160 further
    const float y = g.contentH - 4 + 2;
    a.x = float(s_.guiW() / 2 - 155), a.w = 150, a.h = 20;
    b.x = a.x + 160, b.w = 150, b.h = 20;
    g.rowY.push_back(y);
    s_.buttons.push_back(std::move(a));
    g.rowY.push_back(y);
    s_.buttons.push_back(std::move(b));
    g.contentH += float(g.itemHeight);
}

void OptionsList::halves(std::vector<Button> v) {
    for (size_t i = 0; i < v.size(); i += 2)
        if (i + 1 < v.size()) half(std::move(v[i]), std::move(v[i + 1]));
        else half(std::move(v[i]));
}

void OptionsList::footer(std::vector<Button> v) {
    const float y = float(s_.guiH() - kFooterH + (kFooterH - 20) / 2);
    if (v.empty()) {
        Button done;
        done.label = Text(s_.res.translate("gui.done"));
        done.onPress = [p = &s_] { optionsListBack(*p); };
        done.x = float((s_.guiW() - 200) / 2), done.y = y, done.w = 200;
        s_.buttons.push_back(std::move(done));
        return;
    }
    // LinearLayout.horizontal().spacing(8) centred in the footer.
    const int total = int(v.size()) * 150 + (int(v.size()) - 1) * 8;
    float x = float((s_.guiW() - total) / 2);
    for (Button& b : v) {
        b.x = x, b.y = y, b.w = 150, b.h = 20;
        x += 158;
        s_.buttons.push_back(std::move(b));
    }
}

void OptionsList::setRows(int width, int height) { g.rowWidth = width, g.itemHeight = height; }

float OptionsList::addRow(int height) {
    const float top = g.contentH - 4;
    g.contentH += float(height >= 0 ? height : g.itemHeight);
    return top;
}

void OptionsList::rowButton(Button b, float rowTop, float yOffset) {
    g.rowY.push_back(rowTop + yOffset);
    s_.buttons.push_back(std::move(b));
}

void OptionsList::rowLabel(Text text, float x, float rowTop, float yOffset, bool centred) {
    g.labels.push_back({std::move(text), x, rowTop + yOffset, centred});
}

// --- drawing and input ---------------------------------------------------------------------------------------------

std::vector<Text> tooltipLines(UiState& s, const Text& text) {
    std::vector<Text> out;
    const std::string plain = text.plain();
    if (plain.empty()) return out;
    size_t start = 0;
    while (start <= plain.size()) {  // Tooltip.splitTooltip: lines, each wrapped at 170
        const size_t nl = plain.find('\n', start);
        const std::string line = plain.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        for (Text& t : s.font.split(Text(line), 170)) out.push_back(std::move(t));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

namespace {
void dragScrollBar(UiState& s) {  // AbstractScrollArea.mouseDragged on the scroll bar
    if (!g.draggingBar) return;
    if (!s.leftDown) {
        g.draggingBar = false;
        return;
    }
    const float h = listBottom(s) - listTop();
    const float scale = std::max(1.0f, std::max(1.0f, maxScroll(s)) / (h - scrollerHeight(s)));
    g.scroll = std::clamp(g.dragStartScroll + (s.mouseY - g.dragStartY) * scale, 0.0f, maxScroll(s));
}

}  // namespace

void drawOptionsList(UiState& s, Canvas& c, const std::function<void(Canvas&, float originY)>& extra) {
    const int W = c.guiWidth();
    dragScrollBar(s);
    layout(s);
    menuBackground(s, c);
    const float top = listTop(), bottom = listBottom(s);
    listBackground(s, c, top, bottom);
    const bool inList = s.mouseY >= top && s.mouseY < bottom;
    c.pushScissor(0, top, float(W), bottom);
    for (const ListState::Label& l : g.labels) {
        const float y = origin() + l.y;
        if (y > bottom || y + 9 < top) continue;
        const float x = l.centred ? l.x - float(s.font.width(l.text) / 2) : l.x;
        s.drawText(c, l.text, x, y, 0xFFFFFFFF, true);
    }
    if (extra) extra(c, origin());
    for (size_t i = 0; i < g.rowY.size() && i < s.buttons.size(); ++i) {
        const Button& b = s.buttons[i];
        if (b.y > bottom || b.y + b.h < top) continue;
        s.drawButton(c, i, inList && !dropOpen(s));
    }
    c.popScissor();
    if (maxScroll(s) > 0) {  // AbstractScrollArea.extractScrollbar
        const float h = bottom - top, bar = scrollerHeight(s);
        const float y = std::max(top, float(int(g.scroll) * int(h - bar) / int(maxScroll(s))) + top);
        blitSprite(c, s.sprites.sprite("widget/scroller_background"), scrollBarX(s), top, 6, h);
        blitSprite(c, s.sprites.sprite("widget/scroller"), scrollBarX(s), y, 6, bar);
        s.scrollbarCursor(scrollBarX(s), top, h);
    }
    // Header: the title centred (a second line 2 below it when there is one), footer buttons.
    const float titleY = g.subtitle.empty() ? float((kHeaderH - 9) / 2) : float((kHeaderH - 20) / 2);
    s.drawCentered(c, g.title, float(W / 2), titleY, 0xFFFFFFFF);
    if (!g.subtitle.empty()) s.drawCentered(c, g.subtitle, float(W / 2), titleY + 11, g.subtitleColor);
    for (size_t i = g.rowY.size(); i < s.buttons.size(); ++i) s.drawButton(c, i, !dropOpen(s));
    if (dropOpen(s)) return drawDropdown(s, c);
    static float lastMouse[2] = {-1, -1};  // a mouse move makes the mouse the last input again
    if (s.mouseX != lastMouse[0] || s.mouseY != lastMouse[1]) s.keyboardFocus = false, lastMouse[0] = s.mouseX, lastMouse[1] = s.mouseY;
    // Tooltip of the hovered widget, else of the keyboard-focused one (WidgetTooltipHolder).
    bool tooltipShown = false;
    for (size_t i = 0; i < s.buttons.size(); ++i) {
        const Button& b = s.buttons[i];
        if (!b.tooltip || (i < g.rowY.size() && !inList)) continue;
        if (s.mouseX < b.x || s.mouseY < b.y || s.mouseX >= b.x + b.w || s.mouseY >= b.y + b.h) continue;
        const std::vector<Text> lines = tooltipLines(s, b.tooltip());
        const float rect[4] = {b.x, b.y, b.w, b.h};  // the widget's own tooltip (MenuTooltipPositioner)
        if (!lines.empty()) s.drawTooltip(c, lines, s.mouseX, s.mouseY, nullptr, false, {}, rect);
        tooltipShown = true;
        break;
    }
    if (!tooltipShown && s.keyboardFocus && s.focusedButton >= 0 && size_t(s.focusedButton) < s.buttons.size()) {
        const Button& b = s.buttons[size_t(s.focusedButton)];
        const bool visible = size_t(s.focusedButton) >= g.rowY.size() || (b.y >= listTop() && b.y + b.h <= listBottom(s));
        if (b.tooltip && visible) {
            const std::vector<Text> lines = tooltipLines(s, b.tooltip());
            const float rect[4] = {b.x, b.y, b.w, b.h};
            if (!lines.empty()) s.drawTooltip(c, lines, s.mouseX, s.mouseY, nullptr, false, {}, rect, true);
        }
    }
}

void optionsListBack(UiState& s) {
    s.settings.save();  // OptionsSubScreen.removed
    s.openScreen(g.back);
}

void optionsListInput(UiState& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    layout(s);
    if (dropOpen(s)) return dropdownInput(s, e);
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) return optionsListBack(s);
    if (s.buttonsNavigate(e)) {  // the focused list row scrolled into view (AbstractSelectionList.ensureVisible)
        if (s.focusedButton >= 0 && size_t(s.focusedButton) < g.rowY.size()) {
            const float y = origin() + g.rowY[size_t(s.focusedButton)], top = listTop(), bottom = listBottom(s);
            if (y < top) g.scroll -= top - y + 2;
            else if (y + 20 > bottom) g.scroll += y + 20 - bottom + 2;
            layout(s);
        }
        return;
    }
    const float top = listTop(), bottom = listBottom(s);
    if (e.type == E::Wheel && s.screen == Screen::Video && s.window->keyDown(VK_CONTROL)) {
        // VideoSettingsScreen.mouseScrolled with Control: the GUI scale steps (Auto counting as the largest + 1), not
        // past the largest or into Auto
        int maxScale = 1;
        const u32 w = s.window->width(), h = s.window->height();
        while (w / u32(maxScale + 1) >= 320 && h / u32(maxScale + 1) >= 240) ++maxScale;
        const int old = s.settings.guiScale == 0 ? maxScale + 1 : s.settings.guiScale;
        const int nv = old + (e.code > 0 ? 1 : e.code < 0 ? -1 : 0);
        if (nv != 0 && nv <= maxScale && nv >= 0 && nv != s.settings.guiScale) {
            s.settings.guiScale = nv, g.scroll = 0;
            s.settings.save();
            s.buildButtons();
        }
        return;
    }
    if (e.type == E::Wheel) {  // AbstractScrollArea.mouseScrolled: half a row per notch (with the mouse options)
        if (s.mouseY < top || s.mouseY >= bottom) return;
        const double notches = (s.settings.discreteMouseScroll ? (e.code > 0 ? 1.0 : -1.0) : double(e.code)) *
                               s.settings.mouseWheelSensitivity;
        g.scroll = std::clamp(g.scroll - float(notches * double(g.itemHeight / 2)), 0.0f, maxScroll(s));
        layout(s);
        return;
    }
    if (e.type == E::Button && e.code == 0 && !e.down) g.draggingBar = false;
    if (e.type == E::Button && e.code == 0 && e.down && maxScroll(s) > 0 && s.mouseX >= scrollBarX(s) &&
        s.mouseX <= scrollBarX(s) + 6 && s.mouseY >= top && s.mouseY < bottom) {
        g.draggingBar = true, g.dragStartY = s.mouseY, g.dragStartScroll = g.scroll;
        return;
    }
    if (e.type == E::Button && e.down && s.mouseY >= top && s.mouseY < bottom) s.buttonsInput(e, 0, g.rowY.size());
    else if (e.type == E::Button && e.down) s.buttonsInput(e, g.rowY.size());
    else s.buttonsInput(e);
}

// --- widgets -------------------------------------------------------------------------------------------------------

Text optionLabel(UiState& s, const char* captionKey, const Text& value) {
    return translatable("options.generic_value", {Text(s.res.translate(captionKey)), value}, s.lang());
}

Button cycleButton(std::function<Text()> label, std::function<void()> next, std::function<Text()> tooltip) {
    Button b;
    b.labelFn = std::move(label);
    b.onPress = std::move(next);
    b.tooltip = std::move(tooltip);
    return b;
}

Button choiceButton(UiState& s, std::function<Text()> label, std::function<std::vector<Choice>()> choices,
                    std::function<int()> selected, std::function<void(int)> pick, std::function<Text()> tooltip) {
    Button b = cycleButton(std::move(label), {}, std::move(tooltip));
    b.onPress = [p = &s, choices = std::move(choices), selected = std::move(selected), pick = std::move(pick)] {
        for (size_t i = 0; i < g.rowY.size() && i < p->buttons.size(); ++i) {  // the list widget clicked (or focused)
            const Button& w = p->buttons[i];
            const bool under = p->mouseX >= w.x && p->mouseY >= w.y && p->mouseX < w.x + w.w && p->mouseY < w.y + w.h;
            if (!under && int(i) != p->focusedButton) continue;
            g.drop = {};
            g.drop.button = i, g.drop.choices = choices(), g.drop.selected = selected(), g.drop.pick = pick;
            const int rows = dropRect(*p).rows;  // the current choice in view
            g.drop.first = std::clamp(g.drop.selected - rows / 2, 0, std::max(0, int(g.drop.choices.size()) - rows));
            return;
        }
    };
    return b;
}

Button onOffButton(UiState& s, const char* captionKey, bool* value, std::function<void()> changed, const char* tooltipKey) {
    UiState* p = &s;
    return cycleButton(
        [p, captionKey, value] { return optionLabel(*p, captionKey, Text(p->res.translate(*value ? "options.on" : "options.off"))); },
        [p, value, changed] {
            *value = !*value;
            if (changed) changed();
            p->settings.save();
        },
        tooltipKey ? std::function<Text()>([p, tooltipKey] { return Text(p->res.translate(tooltipKey)); }) : std::function<Text()>());
}

Button sliderButton(float value, std::function<Text()> label, std::function<void(float)> slide) {
    Button b;
    b.slider = true, b.value = value, b.labelFn = std::move(label), b.onSlide = std::move(slide);
    return b;
}

Button screenButton(UiState& s, const char* labelKey, Screen target, bool active) {
    Button b;
    b.label = Text(s.res.translate(labelKey));
    b.onPress = [p = &s, target] { p->openScreen(target); };
    b.active = active;
    return b;
}

std::function<Text()> tooltipKey(UiState& s, const char* key) {
    return [p = &s, key] { return Text(p->res.translate(key)); };
}

Text percentLabel(UiState& s, const char* captionKey, double v) {
    return translatable("options.percent_value", {Text(s.res.translate(captionKey)), Text(std::to_string(int(v * 100.0)))}, s.lang());
}

Text percentOrOffLabel(UiState& s, const char* captionKey, double v) {
    if (v == 0.0) return optionLabel(s, captionKey, Text(s.res.translate("options.off")));
    return percentLabel(s, captionKey, v);
}

Button enumButton(UiState& s, const char* captionKey, int* value, std::vector<const char*> valueKeys, std::function<void()> changed,
                  std::function<Text()> tooltip) {
    UiState* p = &s;
    const int n = int(valueKeys.size());
    if (n > 2)
        return choiceButton(
            s,
            [p, captionKey, value, valueKeys] {
                return optionLabel(*p, captionKey, Text(p->res.translate(valueKeys[size_t(std::clamp(*value, 0, int(valueKeys.size()) - 1))])));
            },
            [p, valueKeys] {
                std::vector<Choice> v;
                for (const char* k : valueKeys) v.push_back({Text(p->res.translate(k))});
                return v;
            },
            [value] { return *value; },
            [p, value, changed](int i) {
                *value = i;
                p->settings.save();
                if (changed) changed();
            },
            std::move(tooltip));
    return cycleButton(
        [p, captionKey, value, valueKeys] {
            return optionLabel(*p, captionKey, Text(p->res.translate(valueKeys[size_t(std::clamp(*value, 0, int(valueKeys.size()) - 1))])));
        },
        [p, value, n, changed] {
            *value = (std::clamp(*value, 0, n - 1) + (p->window->keyDown(VK_SHIFT) ? n - 1 : 1)) % n;  // Shift: backwards
            p->settings.save();
            if (changed) changed();
        },
        std::move(tooltip));
}

Button rangeSlider(UiState& s, int lo, int hi, std::function<int()> get, std::function<void(int)> set, std::function<Text()> label) {
    // IntRangeBase.toSliderValue / fromSliderValue: each value owns an equal stretch of the slider.
    const int v = get();
    const float pos = v <= lo ? 0.0f : v >= hi ? 1.0f : float((double(v) + 0.5 - lo) / (double(hi) + 1.0 - lo));
    return sliderButton(pos, std::move(label), [lo, hi, get, set](float t) {
        const double slider = t >= 1.0f ? 0.99999 : double(t);
        const int nv = std::clamp(int(std::floor(lo + slider * (double(hi) + 1.0 - lo))), lo, hi);
        if (nv != get()) set(nv);
    });
}

Button unitSlider(UiState& s, double* value, std::function<Text()> label, std::function<void()> changed, bool squared) {
    return sliderButton(float(squared ? std::sqrt(*value) : *value), std::move(label), [value, changed, squared](float t) {
        const double nv = squared ? double(t) * double(t) : double(t);
        if (nv == *value) return;
        *value = nv;
        if (changed) changed();
    });
}

namespace {

Text percentOrOff(UiState& s, const char* captionKey, double v) { return percentOrOffLabel(s, captionKey, v); }

// Client information changed (main hand, skin parts, server listing): the server hears it now
// (Options.broadcastOptions).
void broadcastOptions(UiState& s) {
    s.settings.save();
    if (s.client) s.client->setInformation(clientInformation(s.settings));
}

// --- OptionsScreen ---------------------------------------------------------------------------------------------------

void optionsBuild(UiState& s) {
    s.buttons.clear();
    const int W = s.guiW(), H = s.guiH();
    UiState* p = &s;
    // Header (61 high): the title, then FOV and Online... side by side (308 wide), the column centred.
    const float rowX = float((W - 308) / 2), rowY = float((61 - 37) / 2 + 9 + 8);
    Button fov = sliderButton((s.settings.fov - 30.f) / 80.f,
                              [p] {
                                  const int f = int(std::lround(p->settings.fov));
                                  if (f == 70) return optionLabel(*p, "options.fov", Text(p->res.translate("options.fov.min")));
                                  if (f == 110) return optionLabel(*p, "options.fov", Text(p->res.translate("options.fov.max")));
                                  return optionLabel(*p, "options.fov", Text(std::to_string(f)));
                              },
                              [p](float v) { p->settings.fov = std::round(30.f + v * 80.f); });
    fov.x = rowX, fov.y = rowY, fov.w = 150;
    s.buttons.push_back(std::move(fov));
    Button online = screenButton(s, "options.online", Screen::Online);
    online.x = rowX + 158, online.y = rowY, online.w = 150;
    s.buttons.push_back(std::move(online));
    // Contents: 2 columns of 150 wide buttons, 4 apart horizontally, 4 below each.
    auto notAvailable = [] { return Text("Not available in minecraftCplusplus yet"); };
    std::vector<Button> grid;
    grid.push_back(screenButton(s, "options.skinCustomisation", Screen::SkinCustomization));
    grid.push_back(screenButton(s, "options.sounds", Screen::Sounds));
    grid.push_back(screenButton(s, "options.video", Screen::Video));
    grid.push_back(screenButton(s, "options.controls", Screen::Controls));
    grid.push_back(screenButton(s, "options.language", Screen::Language));
    grid.push_back(screenButton(s, "options.chat", Screen::ChatOptions));
    {  // Resource packs (and shader / data packs, mods) are managed on the Mods & Packs screen.
        Button b = screenButton(s, "options.resourcepack", Screen::Mods);
        b.onPress = [p] {
            p->modsReturn = Screen::Options;
            p->openScreen(Screen::Mods);
        };
        grid.push_back(std::move(b));
    }
    grid.push_back(screenButton(s, "options.accessibility", Screen::Accessibility));
    grid.push_back(screenButton(s, "options.telemetry", Screen::None, false));
    grid.back().tooltip = [p] { return Text(p->res.translate("options.telemetry.disabled")); };
    grid.push_back(screenButton(s, "options.credits_and_attribution", Screen::CreditsAttribution));
    const float gridX = float((W - 316) / 2), gridY = float(std::min(61 + 30, H - kFooterH - 120));
    for (size_t i = 0; i < grid.size(); ++i) {
        grid[i].x = gridX + 4 + float(i % 2) * 158, grid[i].y = gridY + float(i / 2) * 24, grid[i].w = 150;
        s.buttons.push_back(std::move(grid[i]));
    }
    Button done;
    done.label = Text(s.res.translate("gui.done"));
    done.x = float((W - 200) / 2), done.y = float(H - kFooterH + (kFooterH - 20) / 2), done.w = 200;
    done.onPress = [p] {
        p->settings.save();
        p->openScreen(p->fe->cfg.menu ? Screen::Title : Screen::Pause);
    };
    s.buttons.push_back(std::move(done));
}

void optionsDraw(UiState& s, Canvas& c) {
    menuBackground(s, c);
    const Text title(s.res.translate("options.title"));
    s.drawCentered(c, title, float(c.guiWidth() / 2), float((61 - 37) / 2), 0xFFFFFFFF);
    s.drawButtons(c);
    for (const Button& b : s.buttons)
        if (b.tooltip && s.mouseX >= b.x && s.mouseY >= b.y && s.mouseX < b.x + b.w && s.mouseY < b.y + b.h) {
            const float rect[4] = {b.x, b.y, b.w, b.h};
            s.drawTooltip(c, tooltipLines(s, b.tooltip()), s.mouseX, s.mouseY, nullptr, false, {}, rect);
            break;
        }
}

void optionsInput(UiState& s, const Window::InputEvent& e) {
    if (e.type == Window::InputEvent::Key && e.down && e.code == VK_ESCAPE) {
        s.settings.save();
        return s.openScreen(s.fe->cfg.menu ? Screen::Title : Screen::Pause);
    }
    s.buttonsInput(e);
}

// --- Online Options ------------------------------------------------------------------------------------------------

void onlineBuild(UiState& s) {
    OptionsList l(s, Text(s.res.translate("options.online.title")), Screen::Options);
    UiState* p = &s;
    // Friends: only the in-game notifications (the friends list, requests and visibility are Microsoft account settings)
    l.header(Text(s.res.translate("options.online.friends.header")));
    l.half(onOffButton(s, "options.inGameNotification", &s.settings.inGameNotification, {}, "options.inGameNotification.tooltip"));
    l.header(Text(s.res.translate("options.online.servers.header")));
    l.big(onOffButton(s, "options.allowServerListing", &s.settings.allowServerListing, [p] { broadcastOptions(*p); },
                      "options.allowServerListing.tooltip"));
    l.header(Text(s.res.translate("options.online.realms.header")));  // kept in options.txt; Realms itself is not here
    l.big(onOffButton(s, "options.realmsNotifications.button", &s.settings.realmsNotifications, {}, "options.realmsNotifications.tooltip"));
    l.footer();
}

// --- Skin Customization ----------------------------------------------------------------------------------------------

void skinBuild(UiState& s) {
    OptionsList l(s, Text(s.res.translate("options.skinCustomisation.title")), Screen::Options);
    UiState* p = &s;
    std::vector<Button> widgets;
    for (int i = 0; i < 7; ++i) {  // PlayerModelPart.values(): CycleButton.onOffBuilder named after the part
        const std::string key = std::string("options.modelPart.") + kModelParts[i];
        widgets.push_back(cycleButton(
            [p, key, i] {
                return translatable("options.generic_value",
                                    {Text(p->res.translate(key)),
                                     Text(p->res.translate(p->settings.modelParts & (1 << i) ? "options.on" : "options.off"))},
                                    p->lang());
            },
            [p, i] {
                p->settings.modelParts ^= 1 << i;
                broadcastOptions(*p);
            }));
    }
    widgets.push_back(cycleButton(
        [p] {
            return optionLabel(*p, "options.mainHand",
                               Text(p->res.translate(p->settings.mainHand == 0 ? "options.mainHand.left" : "options.mainHand.right")));
        },
        [p] {
            p->settings.mainHand = 1 - p->settings.mainHand;
            broadcastOptions(*p);
        }));
    l.halves(std::move(widgets));
    l.footer();
}

// --- Music & Sound Options -------------------------------------------------------------------------------------------

Button volumeSlider(UiState& s, audio::Category c) {
    UiState* p = &s;
    audio::AudioSystem* a = audio::instance();
    const std::string key = std::string("soundCategory.") + audio::categoryName(c);
    Button b = sliderButton(a ? a->volume(c) : 1.0f,
                            [p, key, c] {
                                audio::AudioSystem* au = audio::instance();
                                return percentOrOff(*p, key.c_str(), au ? double(au->volume(c)) : 1.0);
                            },
                            [c](float v) {
                                if (audio::AudioSystem* au = audio::instance(); au && au->volume(c) != v) au->setVolume(c, v);
                            });
    b.active = a != nullptr;
    return b;
}

Button subtitlesButton(UiState& s) {
    return onOffButton(s, "options.showSubtitles", &s.settings.showSubtitles, {}, "options.showSubtitles.tooltip");
}

void soundsBuild(UiState& s) {
    OptionsList l(s, Text(s.res.translate("options.sounds.title")), Screen::Options);
    UiState* p = &s;
    l.big(volumeSlider(s, audio::Category::Master));
    std::vector<Button> others;
    for (int c = 1; c < audio::kCategoryCount; ++c) others.push_back(volumeSlider(s, audio::Category(c)));
    l.halves(std::move(others));
    {  // Device: "" (System Default), then the output devices XAudio2 can open (LazyEnum over the device list)
        Button device = cycleButton(
            [p] {
                const std::string& d = p->settings.soundDevice;
                return optionLabel(*p, "options.audioDevice",
                                   d.empty() ? Text(p->res.translate("options.audioDevice.default")) : Text(d));
            },
            [p] {
                const std::vector<std::string> ids = audio::outputDevices();
                size_t i = 0;  // 0: the default, i: ids[i - 1]
                for (size_t k = 0; k < ids.size(); ++k)
                    if (ids[k] == p->settings.soundDevice) i = k + 1;
                i = (i + 1) % (ids.size() + 1);
                p->settings.soundDevice = i ? ids[i - 1] : std::string();
                p->settings.save();
                if (audio::AudioSystem* a = audio::instance()) a->setOutputDevice(p->settings.soundDevice, p->settings.directionalAudio);
                audio::playUiClick();  // SoundManager.reload, then the click on the new device
            });
        l.big(std::move(device));
    }
    {
        Button directional = cycleButton(
            [p] { return optionLabel(*p, "options.directionalAudio", Text(p->res.translate(p->settings.directionalAudio ? "options.on" : "options.off"))); },
            [p] {
                p->settings.directionalAudio = !p->settings.directionalAudio;
                p->settings.save();
                if (audio::AudioSystem* a = audio::instance()) a->setOutputDevice(p->settings.soundDevice, p->settings.directionalAudio);
                audio::playUiClick();
            },
            [p] { return Text(p->res.translate(p->settings.directionalAudio ? "options.directionalAudio.on.tooltip" : "options.directionalAudio.off.tooltip")); });
        l.half(subtitlesButton(s), std::move(directional));
    }
    {
        Button frequency = enumButton(s, "options.music_frequency", &s.settings.musicFrequency,
                                      {"options.music_frequency.default", "options.music_frequency.frequent", "options.music_frequency.constant"},
                                      {}, tooltipKey(s, "options.music_frequency.tooltip"));
        static const char* const kToast[3] = {"options.musicToast.never", "options.musicToast.pauseMenu", "options.musicToast.pauseMenuAndToast"};
        Button toast = cycleButton(  // MusicToastDisplayState: the value alone, its own tooltip
            [p] { return optionLabel(*p, "options.musicToast", Text(p->res.translate(kToast[std::clamp(p->settings.musicToast, 0, 2)]))); },
            [p] {
                p->settings.musicToast = (p->settings.musicToast + 1) % 3;
                p->settings.save();
            },
            [p] { return Text(p->res.translate(std::string(kToast[std::clamp(p->settings.musicToast, 0, 2)]) + ".tooltip")); });
        l.half(std::move(frequency), std::move(toast));
    }
    l.footer();
}

// --- Controls / Mouse Settings ------------------------------------------------------------------------------------

Button holdToggleButton(UiState& s, const char* keyName, bool* toggle) {  // Options.toggleCrouch / Sprint / Attack / Use
    UiState* p = &s;
    return cycleButton(
        [p, keyName, toggle] { return optionLabel(*p, keyName, Text(p->res.translate(*toggle ? "options.key.toggle" : "options.key.hold"))); },
        [p, toggle] {
            *toggle = !*toggle;
            p->settings.save();
        });
}

Button sprintWindowSlider(UiState& s) {  // Options.sprintWindow: IntRange(0, 10), 0 shows Off
    UiState* p = &s;
    Button b = rangeSlider(s, 0, 10, [p] { return p->settings.sprintWindow; }, [p](int v) { p->settings.sprintWindow = v; },
                           [p] {
                               const int v = p->settings.sprintWindow;
                               return optionLabel(*p, "options.sprintWindow",
                                                  v == 0 ? Text(p->res.translate("options.off"))
                                                         : translatable("options.value", {Text(std::to_string(v))}, p->lang()));
                           });
    b.tooltip = tooltipKey(s, "options.sprintWindow.tooltip");
    return b;
}

void controlsBuild(UiState& s) {
    OptionsList l(s, Text(s.res.translate("controls.title")), Screen::Options);
    l.half(screenButton(s, "options.mouse_settings", Screen::MouseSettings), screenButton(s, "controls.keybinds", Screen::KeyBinds));
    l.halves({holdToggleButton(s, "key.sneak", &s.settings.toggleCrouch), holdToggleButton(s, "key.sprint", &s.settings.toggleSprint),
              holdToggleButton(s, "key.attack", &s.settings.toggleAttack), holdToggleButton(s, "key.use", &s.settings.toggleUse),
              onOffButton(s, "options.autoJump", &s.settings.autoJump), sprintWindowSlider(s),
              onOffButton(s, "options.quitShortcuts", &s.settings.quitShortcuts, {}, "options.quitShortcuts.tooltip"),
              onOffButton(s, "options.operatorItemsTab", &s.settings.operatorItemsTab)});
    l.footer();
}

void mouseBuild(UiState& s) {
    OptionsList l(s, Text(s.res.translate("options.mouse_settings.title")), Screen::Controls);
    UiState* p = &s;
    Button sensitivity = sliderButton(s.settings.sensitivity,
                                      [p] {
                                          const float v = p->settings.sensitivity;
                                          if (v == 0.0f) return optionLabel(*p, "options.sensitivity", Text(p->res.translate("options.sensitivity.min")));
                                          if (v == 1.0f) return optionLabel(*p, "options.sensitivity", Text(p->res.translate("options.sensitivity.max")));
                                          return translatable("options.percent_value",
                                                              {Text(p->res.translate("options.sensitivity")), Text(std::to_string(int(2.0 * v * 100.0)))},
                                                              p->lang());
                                      },
                                      [p](float v) { p->settings.sensitivity = v; });
    // mouseWheelSensitivity: IntRange(-200, 100) through logMouse (10^(v / 100)), shown with two decimals.
    Button wheel = rangeSlider(
        s, -200, 100, [p] { return int(std::floor(std::log10(p->settings.mouseWheelSensitivity) * 100.0)); },  // unlogMouse
        [p](int v) { p->settings.mouseWheelSensitivity = std::pow(10.0, double(v) / 100.0); },             // logMouse
        [p] {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.2f", p->settings.mouseWheelSensitivity);
            return optionLabel(*p, "options.mouseWheelSensitivity", Text(buf));
        });
    l.halves({std::move(sensitivity), std::move(wheel), onOffButton(s, "options.discrete_mouse_scroll", &s.settings.discreteMouseScroll),
              onOffButton(s, "options.invertMouseX", &s.settings.invertMouseX),
              onOffButton(s, "options.invertMouseY", &s.settings.invertMouseY),
              onOffButton(s, "options.allowCursorChanges", &s.settings.allowCursorChanges, {}, "options.allowCursorChanges.tooltip")});
    l.footer();
}

// --- Key Binds -------------------------------------------------------------------------------------------------------

struct KeyBinds {
    int selected = -1;  // the mapping waiting for a key (KeyBindsScreen.selectedKey)
    std::vector<std::pair<float, Key>> rows;  // key rows: top in list coordinates (the collision stripe)
};
KeyBinds kb;

bool isDefault(const UiState& s, size_t i) { return s.settings.keys[i] == keyDef(Key(i)).defaultKey; }

// KeyEntry.refreshEntry: the other mappings on the same key (when either is not at its default).
std::vector<size_t> collisions(const UiState& s, size_t i) {
    std::vector<size_t> out;
    const int k = s.settings.keys[i];
    if (!k) return out;
    for (size_t j = 0; j < kKeyCount; ++j)
        if (j != i && s.settings.keys[j] == k && (!isDefault(s, j) || !isDefault(s, i))) out.push_back(j);
    return out;
}

void keyBindsBuild(UiState& s) {
    OptionsList l(s, Text(s.res.translate("controls.keybinds.title")), Screen::Controls);
    l.setRows(340, 20);
    UiState* p = &s;
    static const char* const kCategories[] = {"movement", "misc", "multiplayer", "gameplay", "inventory", "creative", "spectator", "debug"};
    std::vector<size_t> order(kKeyCount);
    for (size_t i = 0; i < kKeyCount; ++i) order[i] = i;
    auto categoryIndex = [](const char* c) {
        for (size_t i = 0; i < std::size(kCategories); ++i)
            if (std::string_view(c) == kCategories[i]) return int(i);
        return 99;
    };
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {  // KeyMapping.compareTo
        const KeyDef &ka = keyDef(Key(a)), &kb2 = keyDef(Key(b));
        const int ca = categoryIndex(ka.category), cb = categoryIndex(kb2.category);
        if (ca != cb) return ca < cb;
        if (ka.order != kb2.order) return ka.order < kb2.order;
        return s.res.translate(ka.name) < s.res.translate(kb2.name);
    });
    kb.rows.clear();
    const float rowLeft = float(s.guiW() / 2 - 170);
    const float scrollBar = float(s.guiW() / 2 + 170 + 6 + 2);
    const char* category = nullptr;
    for (size_t i : order) {
        const KeyDef& d = keyDef(Key(i));
        if (!category || std::string_view(category) != d.category) {  // CategoryEntry: the label at the row's bottom
            category = d.category;
            const float top = l.addRow();
            l.rowLabel(boldUnderlined(Text(s.res.translate(std::string("key.category.minecraft.") + category))),
                       float(s.guiW() / 2), top, 2 + 16 - 17 + 4, true);
        }
        const float top = l.addRow();
        kb.rows.push_back({top, Key(i)});
        l.rowLabel(Text(s.res.translate(d.name)), rowLeft + 2, top, 2 + 8 - 4, false);
        Button change;  // 75 wide, 5 left of Reset; Reset 50 wide, 10 left of the scroll bar
        change.x = scrollBar - 50 - 10 - 5 - 75, change.w = 75;
        change.labelFn = [p, i] {  // updateChangeButtonMessage
            Text key = p->keyLabel(p->settings.keys[i]);
            const bool collides = !collisions(*p, i).empty();
            if (kb.selected == int(i)) {  // "> " message (the collision's brackets too) in white, underlined, " <"
                Style under = colorStyle(0xFFFFFF);
                under.underlined = 1;
                Text t("> ", colorStyle(0xFFFF55));
                if (collides) t.append("[ ", under);
                t.append(key.withStyle(under));
                if (collides) t.append(" ]", under);
                t.append(" <", colorStyle(0xFFFF55));
                return t;
            }
            if (!collides) return key;
            Text t("[ ", colorStyle(0xFFFF55));
            t.append(key.withStyle(colorStyle(0xFFFFFF)));
            t.append(" ]", colorStyle(0xFFFF55));
            return t;
        };
        change.tooltip = [p, i] {  // controls.keybinds.duplicateKeybinds with the other mappings' names
            const std::vector<size_t> others = collisions(*p, i);
            if (others.empty()) return Text();
            std::string names;
            for (size_t j : others) names += (names.empty() ? "" : ", ") + p->res.translate(keyDef(Key(j)).name);
            return Text(formatJava(p->res.translate("controls.keybinds.duplicateKeybinds"), {names}));
        };
        change.onPress = [p, i] {
            kb.selected = int(i);
            p->keyCaptureActive = true;
        };
        l.rowButton(std::move(change), top, 0);
        Button reset;
        reset.x = scrollBar - 50 - 10, reset.w = 50;
        reset.label = Text(s.res.translate("controls.reset"));
        reset.active = !isDefault(s, i);
        reset.onPress = [p, i] {
            p->settings.keys[i] = keyDef(Key(i)).defaultKey;
            p->settings.save();
            p->buildButtons();
        };
        l.rowButton(std::move(reset), top, 0);
    }
    Button resetAll;
    resetAll.label = Text(s.res.translate("controls.resetAll"));
    resetAll.active = s.settings.keys != Settings::defaultKeys();
    resetAll.onPress = [p] {
        p->settings.keys = Settings::defaultKeys();
        p->settings.save();
        p->buildButtons();
    };
    Button done;
    done.label = Text(s.res.translate("gui.done"));
    done.onPress = [p] { optionsListBack(*p); };
    l.footer({std::move(resetAll), std::move(done)});
}

void keyBindsDraw(UiState& s, Canvas& c) {
    drawOptionsList(s, c, [&s](Canvas& cv, float originY) {
        const float scrollBar = float(s.guiW() / 2 + 170 + 6 + 2), changeX = scrollBar - 140;
        for (const auto& [top, key] : kb.rows)  // the collision stripe left of the button (yellow, 3 wide)
            if (!collisions(s, size_t(key)).empty())
                cv.fill(changeX - 6, originY + top + 2 - 1, changeX - 3, originY + top + 2 + 16, 0xFFFFFF00);
    });
}

void keyBindsInput(UiState& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    if (kb.selected >= 0) {  // KeyBindsScreen.keyPressed / mouseClicked: the next key or button binds (Esc unbinds)
        int bound = -1;
        if (e.type == E::Key && e.down) bound = e.code == VK_ESCAPE ? 0 : e.code;
        if (e.type == E::Button && e.down && e.code >= 0 && e.code < 5) {
            static const int kButtons[5] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
            bound = kButtons[e.code];
        }
        if (bound < 0) return;
        s.settings.keys[size_t(kb.selected)] = bound;
        kb.selected = -1;
        s.keyCaptureActive = false;
        s.settings.save();
        s.buildButtons();
        return;
    }
    optionsListInput(s, e);
}

void keyBindsClosed(UiState& s) {
    kb.selected = -1;
    s.keyCaptureActive = false;
}

// --- Accessibility Settings ----------------------------------------------------------------------------------------

}  // namespace

// Options.narrator: the system speech engine's status; "Not Available" without one.
Button narratorButton(UiState& s) {
    UiState* p = &s;
    static const char* const kStatus[4] = {"options.narrator.off", "options.narrator.all", "options.narrator.chat", "options.narrator.system"};
    Button b = cycleButton(
        [p] {
            return optionLabel(*p, "options.narrator",
                               Text(p->res.translate(narrator::available() ? kStatus[std::clamp(p->settings.narrator, 0, 3)]
                                                                           : "options.narrator.notavailable")));
        },
        [p] {
            p->settings.narrator = (p->settings.narrator + 1) % 4;
            p->settings.save();
            narrator::statusChanged(*p);
        });
    return b;  // clickable without a narrator too (the value cycles; the label reads Not Available)
}

namespace {

// The options shared by Accessibility and Chat Settings (same OptionInstance in the game).
Button textBackgroundOpacitySlider(UiState& s) {
    UiState* p = &s;
    return unitSlider(s, &s.settings.textBackgroundOpacity,
                      [p] { return percentLabel(*p, "options.accessibility.text_background_opacity", p->settings.textBackgroundOpacity); },
                      [p] { chatRescale(*p); });
}
Button chatOpacitySlider(UiState& s) {
    UiState* p = &s;
    return unitSlider(s, &s.settings.chatOpacity,
                      [p] { return percentLabel(*p, "options.chat.opacity", p->settings.chatOpacity * 0.9 + 0.1); },
                      [p] { chatRescale(*p); });
}
Button chatLineSpacingSlider(UiState& s) {
    UiState* p = &s;
    return unitSlider(s, &s.settings.chatLineSpacing, [p] { return percentLabel(*p, "options.chat.line_spacing", p->settings.chatLineSpacing); });
}
Button chatDelaySlider(UiState& s) {  // IntRange(0, 60) / 10 seconds
    UiState* p = &s;
    return rangeSlider(s, 0, 60, [p] { return int(p->settings.chatDelay * 10.0); },
                       [p](int v) { p->settings.chatDelay = v / 10.0; },
                       [p] {
                           if (p->settings.chatDelay <= 0.0) return Text(p->res.translate("options.chat.delay_none"));
                           char buf[16];
                           std::snprintf(buf, sizeof(buf), "%.1f", p->settings.chatDelay);
                           return Text(formatJava(p->res.translate("options.chat.delay"), {buf}));
                       });
}

void accessibilityBuild(UiState& s) {
    OptionsList l(s, Text(s.res.translate("options.accessibility.title")), Screen::Options);
    UiState* p = &s;
    Settings& o = s.settings;
    l.half(narratorButton(s), screenButton(s, "options.controls", Screen::Controls));
    std::vector<Button> v;
    v.push_back(subtitlesButton(s));
    {  // High Contrast: the game's high_contrast resource pack on / off (greyed when the pack is missing)
        Button hc = onOffButton(s, "options.accessibility.high_contrast", &o.highContrast, [p] { applyHighContrast(*p); },
                                "options.accessibility.high_contrast.tooltip");
        if (!highContrastAvailable(s)) {
            hc.active = false;
            hc.tooltip = tooltipKey(s, "options.accessibility.high_contrast.error.tooltip");
        }
        v.push_back(std::move(hc));
    }
    {
        Button blur = rangeSlider(s, 0, 10, [p] { return p->settings.menuBackgroundBlurriness; },
                                  [p](int b) {
                                      p->settings.menuBackgroundBlurriness = b;
                                      p->settings.graphicsPreset = 3;  // setGraphicsPresetToCustom
                                  },
                                  [p] {
                                      const int b = p->settings.menuBackgroundBlurriness;
                                      return optionLabel(*p, "options.accessibility.menu_background_blurriness",
                                                         Text(b == 0 ? p->res.translate("options.off") : std::to_string(b)));
                                  });
        blur.tooltip = tooltipKey(s, "options.accessibility.menu_background_blurriness.tooltip");
        v.push_back(std::move(blur));
    }
    v.push_back(textBackgroundOpacitySlider(s));
    v.push_back(cycleButton(  // Text Background: Chat / Everywhere
        [p] {
            return optionLabel(*p, "options.accessibility.text_background",
                               Text(p->res.translate(p->settings.backgroundForChatOnly ? "options.accessibility.text_background.chat"
                                                                                       : "options.accessibility.text_background.everywhere")));
        },
        [p] {
            p->settings.backgroundForChatOnly = !p->settings.backgroundForChatOnly;
            p->settings.save();
        }));
    v.push_back(chatOpacitySlider(s));
    v.push_back(chatLineSpacingSlider(s));
    v.push_back(chatDelaySlider(s));
    {
        Button t = rangeSlider(s, 5, 100, [p] { return int(p->settings.notificationDisplayTime * 10.0); },
                               [p](int x) { p->settings.notificationDisplayTime = x / 10.0; },
                               [p] {
                                   char buf[16];
                                   std::snprintf(buf, sizeof(buf), "%.1f", p->settings.notificationDisplayTime);  // Double.toString
                                   return optionLabel(*p, "options.notifications.display_time",
                                                      Text(formatJava(p->res.translate("options.multiplier"), {buf})));
                               });
        t.tooltip = tooltipKey(s, "options.notifications.display_time.tooltip");
        v.push_back(std::move(t));
    }
    v.push_back(onOffButton(s, "options.viewBobbing", &o.bobView));
    auto effect = [&](const char* key, const char* tip, double* value, bool squared) {
        Button b = unitSlider(s, value, [p, key, value] { return percentOrOffLabel(*p, key, *value); }, {}, squared);
        b.tooltip = tooltipKey(s, tip);
        v.push_back(std::move(b));
    };
    effect("options.screenEffectScale", "options.screenEffectScale.tooltip", &o.screenEffectScale, false);
    effect("options.fovEffectScale", "options.fovEffectScale.tooltip", &o.fovEffectScale, true);
    effect("options.darknessEffectScale", "options.darknessEffectScale.tooltip", &o.darknessEffectScale, true);
    effect("options.damageTiltStrength", "options.damageTiltStrength.tooltip", &o.damageTiltStrength, false);
    effect("options.glintSpeed", "options.glintSpeed.tooltip", &o.glintSpeed, false);
    effect("options.glintStrength", "options.glintStrength.tooltip", &o.glintStrength, false);
    v.push_back(onOffButton(s, "options.hideLightningFlashes", &o.hideLightningFlash, {}, "options.hideLightningFlashes.tooltip"));
    v.push_back(onOffButton(s, "options.darkMojangStudiosBackgroundColor", &o.darkMojangStudiosBackground, {},
                            "options.darkMojangStudiosBackgroundColor.tooltip"));
    v.push_back(unitSlider(s, &o.panoramaSpeed, [p] { return percentLabel(*p, "options.accessibility.panorama_speed", p->settings.panoramaSpeed); }));
    v.push_back(onOffButton(s, "options.hideSplashTexts", &o.hideSplashTexts, {}, "options.hideSplashTexts.tooltip"));
    v.push_back(onOffButton(s, "options.accessibility.narrator_hotkey", &o.narratorHotkey, {}, "options.accessibility.narrator_hotkey.tooltip"));
    {  // only in worlds with the Minecart Improvements experiment
        Button minecart = onOffButton(s, "options.rotateWithMinecart", &o.rotateWithMinecart, {}, "options.rotateWithMinecart.tooltip");
        minecart.active = s.client && s.client->hasFeature("minecraft:minecart_improvements");
        v.push_back(std::move(minecart));
    }
    v.push_back(onOffButton(s, "options.accessibility.high_contrast_block_outline", &o.highContrastBlockOutline, {},
                            "options.accessibility.high_contrast_block_outline.tooltip"));
    l.halves(std::move(v));
    Button guide;
    guide.label = Text(s.res.translate("options.accessibility.link"));
    guide.onPress = [p] { openConfirmLink(*p, "https://aka.ms/MinecraftJavaAccessibility", Screen::Accessibility); };
    Button done;
    done.label = Text(s.res.translate("gui.done"));
    done.onPress = [p] { optionsListBack(*p); };
    l.footer({std::move(guide), std::move(done)});
}

// --- Chat Settings -------------------------------------------------------------------------------------------------

void chatOptionsBuild(UiState& s) {
    OptionsList l(s, Text(s.res.translate("options.chat.title")), Screen::Options);
    UiState* p = &s;
    Settings& o = s.settings;
    std::vector<Button> v;
    v.push_back(enumButton(s, "options.chat.visibility", &o.chatVisibility,
                           {"options.chat.visibility.full", "options.chat.visibility.system", "options.chat.visibility.hidden"},
                           [p] { broadcastOptions(*p); }));
    v.push_back(onOffButton(s, "options.chat.color", &o.chatColors, [p] { broadcastOptions(*p); }));
    v.push_back(onOffButton(s, "options.chat.links", &o.chatLinks));
    v.push_back(onOffButton(s, "options.chat.links.prompt", &o.chatLinksPrompt));
    v.push_back(chatOpacitySlider(s));
    v.push_back(textBackgroundOpacitySlider(s));
    v.push_back(unitSlider(s, &o.chatScale,
                           [p] {
                               if (p->settings.chatScale == 0.0)
                                   return optionLabel(*p, "options.chat.scale", Text(p->res.translate("options.off")));
                               return percentLabel(*p, "options.chat.scale", p->settings.chatScale);
                           },
                           [p] { chatRescale(*p); }));
    v.push_back(chatLineSpacingSlider(s));
    v.push_back(chatDelaySlider(s));
    auto pixels = [p](const char* key, int px) {
        return translatable("options.pixel_value", {Text(p->res.translate(key)), Text(std::to_string(px))}, p->lang());
    };
    v.push_back(unitSlider(s, &o.chatWidth, [p, pixels] { return pixels("options.chat.width", chatWidthPixels(p->settings.chatWidth)); },
                           [p] { chatRescale(*p); }));
    v.push_back(unitSlider(s, &o.chatHeightFocused,
                           [p, pixels] { return pixels("options.chat.height.focused", chatHeightPixels(p->settings.chatHeightFocused)); },
                           [p] { chatRescale(*p); }));
    v.push_back(unitSlider(s, &o.chatHeightUnfocused,
                           [p, pixels] { return pixels("options.chat.height.unfocused", chatHeightPixels(p->settings.chatHeightUnfocused)); },
                           [p] { chatRescale(*p); }));
    v.push_back(narratorButton(s));
    v.push_back(onOffButton(s, "options.autoSuggestCommands", &o.autoSuggestions));
    v.push_back(onOffButton(s, "options.hideMatchedNames", &o.hideMatchedNames, {}, "options.hideMatchedNames.tooltip"));
    v.push_back(onOffButton(s, "options.reducedDebugInfo", &o.reducedDebugInfo));
    v.push_back(onOffButton(s, "options.onlyShowSecureChat", &o.onlyShowSecureChat, {}, "options.onlyShowSecureChat.tooltip"));
    v.push_back(onOffButton(s, "options.chat.drafts", &o.saveChatDrafts, {}, "options.chat.drafts.tooltip"));
    l.halves(std::move(v));
    l.footer();
}

// --- Font Settings -------------------------------------------------------------------------------------------------

void fontOptionsBuild(UiState& s) {
    OptionsList l(s, Text(s.res.translate("options.font.title")), Screen::Language);
    UiState* p = &s;
    l.half(onOffButton(s, "options.forceUnicodeFont", &s.settings.forceUnicodeFont, [p] { reloadFonts(*p); }),
           onOffButton(s, "options.japaneseGlyphVariants", &s.settings.japaneseGlyphVariants, [p] { reloadFonts(*p); },
                       "options.japaneseGlyphVariants.tooltip"));
    l.footer();
}

void listDraw(UiState& s, Canvas& c) { drawOptionsList(s, c); }

const ScreenRegistrar kOptions({Screen::Options, optionsBuild, optionsDraw, optionsInput, nullptr});
const ScreenRegistrar kOnline({Screen::Online, onlineBuild, listDraw, optionsListInput, nullptr});
const ScreenRegistrar kSkin({Screen::SkinCustomization, skinBuild, listDraw, optionsListInput, nullptr});
const ScreenRegistrar kSounds({Screen::Sounds, soundsBuild, listDraw, optionsListInput, nullptr});
const ScreenRegistrar kControls({Screen::Controls, controlsBuild, listDraw, optionsListInput, nullptr});
const ScreenRegistrar kMouse({Screen::MouseSettings, mouseBuild, listDraw, optionsListInput, nullptr});
const ScreenRegistrar kKeyBinds({Screen::KeyBinds, keyBindsBuild, keyBindsDraw, keyBindsInput, keyBindsClosed});
const ScreenRegistrar kAccessibility({Screen::Accessibility, accessibilityBuild, listDraw, optionsListInput, nullptr});
const ScreenRegistrar kChatOptions({Screen::ChatOptions, chatOptionsBuild, listDraw, optionsListInput, nullptr});
const ScreenRegistrar kFontOptions({Screen::FontOptions, fontOptionsBuild, listDraw, optionsListInput, nullptr});

}  // namespace

}  // namespace ui
