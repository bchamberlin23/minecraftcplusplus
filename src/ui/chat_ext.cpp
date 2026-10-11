// Chat screen extras: command suggestions (CommandSuggestions: the server completes the typed command; the list above
// the input, Tab / arrows / mouse) and chat component events (hover tooltips, click actions: ChatScreen
// .handleComponentClicked / Screen.defaultHandleGameClickEvent).
#include "ui/frontend.h"
#include "ui/mp_hud.h"
#include "ui/ui_state.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

struct Suggestion {
    std::string text;
    bool hasTooltip = false;
    Text tooltip;
};

struct ChatExt {
    int nextId = 0, pendingId = -1;
    std::string pendingText;  // the request's input (up to the cursor)
    bool haveResult = false;
    int start = 0, length = 0;
    std::vector<Suggestion> result;
    // SuggestionsList
    bool visible = false;
    std::vector<Suggestion> list;
    int x = 0, y = 0, w = 0, h = 0, offset = 0, current = 0;
    std::string original;
    bool tabCycles = false;
    std::string ghost;  // EditBox suggestion (gray, after the text)
    // Change tracking
    std::string lastValue;
    bool tracking = false;
    bool open = false;  // drawn since the chat screen opened
    float lastMx = -1, lastMy = -1;
};
ChatExt cx;

constexpr int kLimit = 10;          // ChatScreen: suggestionLineLimit
constexpr u32 kFill = 0xD0000000;   // ChatScreen: fillColor

void hide() {
    cx.visible = false;
    cx.ghost.clear();
}

int screenX(UiState& s, int pos) {  // EditBox.getScreenX with the field's scroll
    const auto& f = s.chatInput;
    pos = std::clamp(pos, f.scroll, int(f.value.size()));
    return 4 + s.font.width(f.value.substr(size_t(f.scroll), size_t(pos - f.scroll)));
}

std::string applySuggestion(const Suggestion& sg, const std::string& input) {  // Suggestion.apply
    const size_t a = std::min(size_t(cx.start), input.size()), b = std::min(size_t(cx.start + cx.length), input.size());
    return input.substr(0, a) + sg.text + input.substr(b);
}

void select(UiState& s, int index) {
    if (cx.list.empty()) return;
    const int n = int(cx.list.size());
    cx.current = ((index % n) + n) % n;
    const std::string applied = applySuggestion(cx.list[size_t(cx.current)], cx.original);
    const std::string& v = s.chatInput.value;
    cx.ghost = applied.starts_with(v) ? applied.substr(v.size()) : std::string();  // calculateSuggestionSuffix
}

void cycle(UiState& s, int dir) {
    select(s, cx.current + dir);
    const int n = int(cx.list.size());
    const int first = cx.offset, last = cx.offset + kLimit - 1;
    if (cx.current < first) cx.offset = std::clamp(cx.current, 0, std::max(n - kLimit, 0));
    else if (cx.current > last) cx.offset = std::clamp(cx.current + 1 - kLimit, 0, std::max(n - kLimit, 0));  // lineStartOffset 1
}

void useSuggestion(UiState& s) {
    if (cx.list.empty()) return;
    const Suggestion& sg = cx.list[size_t(cx.current)];
    auto& f = s.chatInput;
    f.value = applySuggestion(sg, cx.original);
    f.cursor = f.selection = std::min(int(f.value.size()), cx.start + int(sg.text.size()));
    cx.lastValue = f.value;  // keepSuggestions: no new request for this change
    select(s, cx.current);
    cx.tabCycles = true;
}

void showSuggestions(UiState& s) {  // CommandSuggestions.showSuggestions
    if (!cx.haveResult || cx.result.empty()) return;
    const auto& f = s.chatInput;
    int maxW = 0;
    for (const Suggestion& sg : cx.result) maxW = std::max(maxW, s.font.width(sg.text));
    const int W = s.guiW(), H = s.guiH();
    const int x = std::clamp(screenX(s, cx.start), 0, 4 + (W - 4) - maxW);
    const int y = H - 12;
    // sortSuggestions: those starting with the last word first.
    const std::string partial = f.value.substr(0, size_t(std::clamp(f.cursor, 0, int(f.value.size()))));
    size_t lw = partial.find_last_of(" \t");
    std::string lastWord = lw == std::string::npos ? partial : partial.substr(lw + 1);
    for (char& ch : lastWord) ch = char(std::tolower(u8(ch)));
    std::vector<Suggestion> front, back;
    for (const Suggestion& sg : cx.result)
        (sg.text.starts_with(lastWord) || sg.text.starts_with("minecraft:" + lastWord) ? front : back).push_back(sg);
    front.insert(front.end(), back.begin(), back.end());
    cx.list = std::move(front);
    const int shown = std::min(int(cx.list.size()), kLimit);
    cx.x = x - 1, cx.y = y - 3 - shown * 12, cx.w = maxW + 1, cx.h = shown * 12;
    cx.original = f.value;
    cx.offset = 0;
    cx.tabCycles = false;
    cx.visible = true;
    select(s, 0);
}

// Input changed (ChatScreen.onEdited -> CommandSuggestions.updateCommandInfo): ask the server to complete commands.
void update(UiState& s) {
    const auto& f = s.chatInput;
    if (cx.tracking && f.value == cx.lastValue) return;
    cx.tracking = true;
    cx.lastValue = f.value;
    hide();
    cx.haveResult = false;
    if (f.value.empty() || f.value[0] != '/' || f.cursor < 1 || !s.online()) {
        cx.pendingId = -1;
        return;
    }
    cx.pendingId = ++cx.nextId;
    cx.pendingText = f.value.substr(0, size_t(f.cursor));
    net::Writer w;  // ServerboundCommandSuggestionPacket: the command up to the cursor
    w.varInt(cx.pendingId);
    w.str(cx.pendingText);
    s.client->sendPlay("command_suggestion", w);
    if (std::getenv("MCPP_UI_VERBOSE")) LOG("ui: command_suggestion #%d \"%s\"", cx.pendingId, cx.pendingText.c_str());
}

void registerHandlers(UiState& s, net::Client& client) {
    UiState* sp = &s;
    const Lang* lang = &s.res.lang();
    client.onPlay("command_suggestions", [sp, lang](net::Reader& r) {
        const int id = r.varInt(), start = r.varInt(), length = r.varInt();
        const i32 n = r.varInt();
        std::vector<Suggestion> list;
        for (i32 i = 0; i < n && r.ok; ++i) {
            Suggestion sg;
            sg.text = std::string(r.str());
            sg.hasTooltip = r.boolean();
            if (sg.hasTooltip) sg.tooltip = parseComponent(r.nbt(), lang);
            list.push_back(std::move(sg));
        }
        if (!r.ok) return;
        if (std::getenv("MCPP_UI_VERBOSE")) {
            std::string names;
            for (size_t i = 0; i < list.size() && i < 12; ++i) names += list[i].text + " ";
            LOG("ui: command_suggestions #%d start %d length %d, %d: %s", id, start, length, n, names.c_str());
        }
        sp->post([sp, id, start, length, list = std::move(list)] {
            if (id != cx.pendingId || sp->screen != Screen::Chat) return;
            cx.haveResult = true, cx.start = start, cx.length = length, cx.result = list;
            showSuggestions(*sp);  // autoSuggestions (default on)
        });
    });
}

UiModuleRegistrar reg([] {
    UiModule m;
    m.name = "chat_ext";
    m.registerHandlers = registerHandlers;
    m.attached = [](UiState&) { cx = ChatExt{}; };
    m.tick = [](UiState& s) {
        if (s.screen != Screen::Chat) cx.open = false;
    };
    return m;
}());

// --- chat component events ----------------------------------------------------------------------------------------

// The style under GUI point (mx, my) in the open chat (ChatComponent geometry of hud.cpp drawChat); null: none.
const Style* chatStyleAt(UiState& s, float mx, float my, Style& out) {
    const int chatBottom = s.guiH() - 40;
    if (my >= float(chatBottom) || mx < 4) return nullptr;
    const int i = int(std::floor((float(chatBottom) - my) / 9.0f));
    const int total = int(s.chatLines.size());
    if (i < 0 || i >= std::min(total - s.chatScroll, 20)) return nullptr;
    const Text& line = s.chatLines[size_t(i + s.chatScroll)].text;
    const float x = mx - 4;
    float pen = 0;
    bool found = false;
    s.font.iterate(line, [&](int, const Style& st, u32 cp) {
        const float adv = s.font.glyph(cp).advanceFor(st.isBold());
        if (x >= pen && x < pen + adv) {
            out = st;
            found = true;
            return false;
        }
        pen += adv;
        return true;
    });
    return found ? &out : nullptr;
}

std::vector<Text> hoverLines(UiState& s, const TextEvents& ev) {
    std::vector<Text> lines;
    if (ev.hover == "show_text") {
        lines = s.font.split(ev.hoverText, std::max(s.guiW() / 2, 200));
    } else if (ev.hover == "show_item") {
        std::string id = ev.hoverId.find(':') == std::string::npos ? "minecraft:" + ev.hoverId : ev.hoverId;
        ItemStack st;
        st.item = s.items.id(id), st.count = std::max(1, ev.hoverCount);
        if (st.item >= 0) lines = s.items.tooltip(st);
    } else if (ev.hover == "show_entity") {  // HoverEvent.EntityTooltipInfo.getTooltipLines
        if (!ev.hoverText.empty()) lines.push_back(ev.hoverText);
        std::string type = ev.hoverId.find(':') == std::string::npos ? "minecraft:" + ev.hoverId : ev.hoverId;
        std::string key = "entity." + type;
        std::replace(key.begin(), key.end(), ':', '.');
        lines.push_back(translatable("gui.entity_tooltip.type", {Text(s.res.translate(key))}, s.lang()));
    }
    return lines;
}

void openUrl(const std::string& url) {
    std::wstring w(url.begin(), url.end());
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ChatScreen.handleComponentClicked + Screen.defaultHandleGameClickEvent.
bool clickStyle(UiState& s, const Style& st, bool shift) {
    if (!st.events) return false;
    const TextEvents& ev = *st.events;
    auto& f = s.chatInput;
    if (shift) {  // insertion click mode
        if (!ev.hasInsertion) return false;
        const size_t a = size_t(std::min(f.cursor, f.selection)), b = size_t(std::max(f.cursor, f.selection));
        f.value = (f.value.substr(0, a) + ev.insertion + f.value.substr(b)).substr(0, size_t(f.maxLength));
        f.cursor = f.selection = std::min(int(a + ev.insertion.size()), int(f.value.size()));
        return true;
    }
    if (ev.click.empty()) return false;
    LOG("ui: chat click %s \"%s\"", ev.click.c_str(), ev.clickValue.c_str());
    if (ev.click == "run_command") {
        std::string cmd = ev.clickValue;
        if (!cmd.empty() && cmd[0] == '/') cmd.erase(0, 1);  // Commands.trimOptionalPrefix
        if (s.online()) {
            net::Writer w;
            w.str(cmd);
            s.client->sendPlay("chat_command", w);
        }
    } else if (ev.click == "suggest_command") {  // insertText(value, replace)
        f.value = ev.clickValue.substr(0, size_t(f.maxLength));
        f.cursor = f.selection = int(f.value.size());
    } else if (ev.click == "copy_to_clipboard") {
        s.window->setClipboard(ev.clickValue);
    } else if (ev.click == "open_url") {  // ConfirmLinkScreen (chatLinksPrompt)
        const std::string url = ev.clickValue;
        if (!url.starts_with("http://") && !url.starts_with("https://")) return true;
        UiState* sp = &s;
        showConfirm(s, Text(s.res.translate("chat.link.confirm")), Text(url), s.res.translate("chat.link.open"), [sp, url] {
            openUrl(url);
            sp->openScreen(Screen::Chat);
        }, Screen::Chat);
    }
    return true;
}

}  // namespace

bool chatExtInput(UiState& s, const InputEvent& e) {
    using E = InputEvent;
    update(s);
    if (e.type == E::Key && e.down) {
        if (cx.visible) {  // SuggestionsList.keyPressed
            if (e.code == VK_UP || e.code == VK_DOWN) {
                cycle(s, e.code == VK_UP ? -1 : 1);
                cx.tabCycles = false;
                return true;
            }
            if (e.code == VK_TAB) {
                if (cx.tabCycles) cycle(s, s.window->keyDown(VK_SHIFT) ? -1 : 1);
                useSuggestion(s);
                return true;
            }
            if (e.code == VK_ESCAPE) {
                hide();
                return true;
            }
        } else if (e.code == VK_TAB) {  // CommandSuggestions.keyPressed: Tab shows the list
            showSuggestions(s);
            return true;
        }
        return false;
    }
    if (e.type == E::Wheel && cx.visible) {
        if (s.mouseX >= cx.x && s.mouseX < cx.x + cx.w && s.mouseY >= cx.y && s.mouseY < cx.y + cx.h) {
            cx.offset = std::clamp(cx.offset - (e.code > 0 ? 1 : -1), 0, std::max(int(cx.list.size()) - kLimit, 0));
            return true;
        }
        return false;
    }
    if (e.type == E::Button && e.down && e.code == 0) {
        if (cx.visible && s.mouseX >= cx.x && s.mouseX < cx.x + cx.w && s.mouseY >= cx.y && s.mouseY < cx.y + cx.h) {
            const int line = int(s.mouseY - float(cx.y)) / 12 + cx.offset;
            if (line >= 0 && line < int(cx.list.size())) {
                select(s, line);
                useSuggestion(s);
            }
            return true;
        }
        Style st;
        if (chatStyleAt(s, s.mouseX, s.mouseY, st)) return clickStyle(s, st, s.window->keyDown(VK_SHIFT));
    }
    return false;
}

void chatExtDraw(UiState& s, Canvas& c) {
    if (!cx.open) cx.open = true, cx.tracking = false, hide(), cx.pendingId = -1;  // the chat (re)opened
    update(s);
    const auto& f = s.chatInput;
    // EditBox suggestion: gray after the text when the cursor is at the end.
    if (!cx.ghost.empty() && f.cursor >= int(f.value.size()))
        s.font.draw(c, cx.ghost, float(screenX(s, int(f.value.size())) - 1), float(s.guiH() - 12 + 2), 0xFF808080, true);
    const float mx = s.mouseX, my = s.mouseY;
    const bool moved = mx != cx.lastMx || my != cx.lastMy;
    cx.lastMx = mx, cx.lastMy = my;
    if (cx.visible && !cx.list.empty()) {  // SuggestionsList.extractRenderState
        const int n = int(cx.list.size()), limit = std::min(n, kLimit);
        const bool prev = cx.offset > 0, next = n > cx.offset + limit;
        if (prev || next) {
            c.fill(float(cx.x), float(cx.y - 1), float(cx.x + cx.w), float(cx.y), kFill);
            c.fill(float(cx.x), float(cx.y + cx.h), float(cx.x + cx.w), float(cx.y + cx.h + 1), kFill);
            for (int x = 0; x < cx.w; x += 2) {
                if (prev) c.fill(float(cx.x + x), float(cx.y - 1), float(cx.x + x + 1), float(cx.y), 0xFFFFFFFF);
                if (next) c.fill(float(cx.x + x), float(cx.y + cx.h), float(cx.x + x + 1), float(cx.y + cx.h + 1), 0xFFFFFFFF);
            }
        }
        bool hovered = false;
        for (int i = 0; i < limit; ++i) {
            const Suggestion& sg = cx.list[size_t(i + cx.offset)];
            const int y = cx.y + 12 * i;
            c.fill(float(cx.x), float(y), float(cx.x + cx.w), float(y + 12), kFill);
            if (mx > cx.x && mx < cx.x + cx.w && my > y && my < y + 12) {
                if (moved) select(s, i + cx.offset);
                hovered = true;
            }
            s.font.draw(c, sg.text, float(cx.x + 1), float(y + 2), i + cx.offset == cx.current ? 0xFFFFFF00 : 0xFFAAAAAA, true);
        }
        if (hovered && cx.list[size_t(cx.current)].hasTooltip)
            s.drawTooltip(c, {cx.list[size_t(cx.current)].tooltip}, mx, my);
        return;
    }
    // Hovered chat component: its hover event.
    Style st;
    if (chatStyleAt(s, mx, my, st) && st.events && !st.events->hover.empty()) {
        const std::vector<Text> lines = hoverLines(s, *st.events);
        if (!lines.empty()) s.drawTooltip(c, lines, mx, my);
    }
}

}  // namespace ui
