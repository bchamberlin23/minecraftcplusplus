// Chat screen extras: command suggestions (CommandSuggestions: the command tree the server sent parses the input
// locally (argument colours, usage hints, errors) and completes literals; arguments ask the server; the list above
// the input, Tab / arrows / mouse) and chat component events (hover tooltips, click actions: ChatScreen
// .handleComponentClicked / Screen.defaultHandleGameClickEvent).
#include "assets/game_reports.h"
#include "ui/command_tree.h"
#include "ui/frontend.h"
#include "ui/mp_hud.h"
#include "ui/options_list.h"
#include "ui/ui_state.h"

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
    // The parse of the input with the command tree (currentParse).
    std::string parsedValue;
    bool haveParse = false, parsed = false;
    cmd::Parse parse;
    int ctxParent = -1, ctxStart = 0;  // the suggestion context of the last completion
    // commandUsage: usage hints / parse errors above the input when no suggestion list shows.
    std::vector<Text> usage;
    int usageX = 0, usageW = 0;
};
ChatExt cx;
struct Commands {  // the server's command tree (ClientboundCommandsPacket); kept over ChatExt resets
    cmd::Tree tree;
};
Commands commands;

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

// CommandDispatcher.parse of the input with the server's tree (redone when the text changes).
void updateParse(UiState& s) {
    const std::string& v = s.chatInput.value;
    if (cx.parsed && v == cx.parsedValue) return;
    cx.parsed = true;
    cx.parsedValue = v;
    cx.haveParse = !v.empty() && v[0] == '/' && commands.tree.valid();
    if (cx.haveParse) cx.parse = cmd::parse(commands.tree, v, 1);
}

// CommandSuggestions.updateUsageInfo: parse errors when the cursor is at the end and nothing completes, else the
// usage of the arguments at the cursor (fillNodeUsage), else the trailing-data error.
void updateUsage(UiState& s) {
    cx.usage.clear();
    if (!cx.haveParse) return;
    const auto& f = s.chatInput;
    const Lang* lang = &s.res.lang();
    bool trailing = false;
    if (f.cursor == int(f.value.size())) {
        if (cx.result.empty() && !cx.parse.exceptions.empty()) {
            int literals = 0;
            for (const auto& [node, e] : cx.parse.exceptions) {
                if (e.literalIncorrect) ++literals;
                else cx.usage.push_back(cmd::errorText(e, lang));
            }
            if (literals > 0) cx.usage.push_back(cmd::errorText(cmd::unknownArgument(cx.parse), lang));
        } else if (cx.parse.canRead()) {
            trailing = true;
        }
    }
    if (cx.usage.empty()) {
        std::vector<Text> entries;
        for (const std::string& u : cmd::argumentUsage(commands.tree, cx.ctxParent)) entries.push_back(Text(u, colorStyle(0xAAAAAA)));
        cmd::Error e;
        if (entries.empty() && trailing && cmd::parseException(cx.parse, e)) cx.usage.push_back(cmd::errorText(e, lang));
        cx.usage.insert(cx.usage.end(), entries.begin(), entries.end());
    }
    cx.usageW = 0;
    for (const Text& t : cx.usage) cx.usageW = std::max(cx.usageW, s.font.width(t));
    cx.usageX = cx.usage.empty() ? 0 : std::clamp(screenX(s, cx.ctxStart), 0, 4 + (s.guiW() - 4) - cx.usageW);
}

// Input changed (ChatScreen.onEdited -> CommandSuggestions.updateCommandInfo): complete locally from the command tree
// (literals, booleans) or ask the server (other arguments, or no tree).
void update(UiState& s) {
    const auto& f = s.chatInput;
    updateParse(s);
    if (cx.tracking && f.value == cx.lastValue) return;
    cx.tracking = true;
    cx.lastValue = f.value;
    hide();
    cx.haveResult = false;
    cx.usage.clear();
    if (f.value.empty() || f.value[0] != '/' || f.cursor < 1 || !s.online()) {
        cx.pendingId = -1;
        return;
    }
    if (cx.haveParse) {
        const cmd::Completion comp = cmd::complete(commands.tree, cx.parse, f.cursor);
        cx.ctxParent = comp.parent, cx.ctxStart = comp.startPos;
        if (!comp.askServer) {  // the tree answers it: no request
            cx.pendingId = -1;
            cx.haveResult = true, cx.start = comp.start, cx.length = f.cursor - comp.start;
            cx.result.clear();
            for (const std::string& t : comp.local) cx.result.push_back({t});
            updateUsage(s);
            showSuggestions(s);
            return;
        }
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
    // ClientboundCommandsPacket: the commands this player may use, as a Brigadier tree.
    std::vector<std::string> argTypes;
    if (const nlohmann::json& reg = s.reports->registries(); reg.contains("minecraft:command_argument_type"))
        for (const auto& e : reg["minecraft:command_argument_type"]["entries"].items()) {
            const int id = e.value().value("protocol_id", -1);
            if (id < 0) continue;
            if (size_t(id) >= argTypes.size()) argTypes.resize(size_t(id) + 1);
            argTypes[size_t(id)] = e.key();
        }
    client.onPlay("commands", [sp, argTypes](net::Reader& r) {
        auto tree = std::make_shared<cmd::Tree>();
        if (!cmd::decode(r, argTypes, *tree)) {
            LOG("ui: command tree not decoded");
            return;
        }
        if (std::getenv("MCPP_UI_VERBOSE")) LOG("ui: command tree, %zu nodes", tree->nodes.size());
        sp->post([tree] {
            commands.tree = std::move(*tree);
            cx.parsed = false;
        });
    });
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
            updateUsage(*sp);
            showSuggestions(*sp);  // autoSuggestions (default on)
        });
    });
}

UiModuleRegistrar reg([] {
    UiModule m;
    m.name = "chat_ext";
    m.registerHandlers = registerHandlers;
    m.attached = [](UiState& s) {
        cx = ChatExt{};
        UiState* sp = &s;
        s.chatFormatter = [sp](const std::string& visible, int offset, Text& out) {  // CommandSuggestions.formatChat
            updateParse(*sp);
            if (!cx.haveParse) return false;
            out = cmd::format(cx.parse, visible, offset);
            return true;
        };
    };
    m.tick = [](UiState& s) {
        if (s.screen != Screen::Chat) cx.open = false;
    };
    return m;
}());

// --- chat component events ----------------------------------------------------------------------------------------

// The style under GUI point (mx, my) in the open chat (hud.cpp); null: none.
const Style* chatStyleAt(UiState& s, float mx, float my, Style& out) { return chatLineStyleAt(s, mx, my, out) ? &out : nullptr; }

std::vector<Text> hoverLines(UiState& s, const TextEvents& ev) {
    std::vector<Text> lines;
    if (ev.hover == "show_text") {
        lines = s.font.split(ev.hoverText, std::max(s.guiW() / 2, 200));
    } else if (ev.hover == "show_item") {
        std::string id = ev.hoverId.find(':') == std::string::npos ? "minecraft:" + ev.hoverId : ev.hoverId;
        ItemStack st;
        st.item = s.items.id(id), st.count = std::max(1, ev.hoverCount);
        if (st.item >= 0) lines = s.items.tooltip(st, s.settings.advancedItemTooltips);
    } else if (ev.hover == "show_entity") {  // HoverEvent.EntityTooltipInfo.getTooltipLines
        if (!ev.hoverText.empty()) lines.push_back(ev.hoverText);
        std::string type = ev.hoverId.find(':') == std::string::npos ? "minecraft:" + ev.hoverId : ev.hoverId;
        std::string key = "entity." + type;
        std::replace(key.begin(), key.end(), ':', '.');
        lines.push_back(translatable("gui.entity_tooltip.type", {Text(s.res.translate(key))}, s.lang()));
    }
    return lines;
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
    } else if (ev.click == "open_url") {  // Screen.clickUrlAction: Web Links, then Prompt on Links (ConfirmLinkScreen)
        const std::string url = ev.clickValue;
        if (!s.settings.chatLinks) return false;
        if (!url.starts_with("http://") && !url.starts_with("https://")) return true;
        if (s.settings.chatLinksPrompt) openConfirmLink(s, url, Screen::Chat, false);
        else openUrlInBrowser(url);
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
        if (chatSpecialClick(s, s.mouseX, s.mouseY)) return true;
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
    // CommandSuggestions.extractUsage: anchored to the bottom, one line per entry.
    for (size_t i = 0; i < cx.usage.size(); ++i) {
        const int y = s.guiH() - 27 - 12 * int(i);
        c.fill(float(cx.usageX - 1), float(y), float(cx.usageX + cx.usageW + 1), float(y + 12), kFill);
        s.font.draw(c, cx.usage[i], float(cx.usageX), float(y + 2), 0xFFFFFFFF, true);
    }
    // Hovered chat component: its hover event.
    Style st;
    if (chatStyleAt(s, mx, my, st) && st.events && !st.events->hover.empty()) {
        const std::vector<Text> lines = hoverLines(s, *st.events);
        if (!lines.empty()) s.drawTooltip(c, lines, mx, my);
    }
}

}  // namespace ui
