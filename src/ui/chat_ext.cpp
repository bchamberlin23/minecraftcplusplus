// Chat screen extras: command suggestions (CommandSuggestions: the command tree the server sent parses the input
// locally (argument colours, usage hints, errors) and completes literals; arguments ask the server; the list above
// the input, Tab / arrows / mouse) and chat component events (hover tooltips, click actions: ChatScreen
// .handleComponentClicked / Screen.defaultHandleGameClickEvent).
#include "assets/game_reports.h"
#include "ui/command_tree.h"
#include "ui/dialogs.h"
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
// CommandSuggestions' configuration and its EditBox: the chat input, or the command block editors' command field
// (commandsOnly, onlyShowIfCursorPastError, lineStartOffset 0, 7 lines, below the field, fill Integer.MIN_VALUE).
struct Target {
    UiState::TextField* field = nullptr;
    int originX = 4, innerW = 0;  // EditBox.getScreenX(0), getInnerWidth
    int textY = 0;                // the EditBox's text line (the suggestion after the text)
    bool bordered = false, anchorBottom = true, commandsOnly = false, pastError = false;
    int lineStart = 1, limit = 10;
    u32 fill = 0xD0000000;  // ChatScreen: fillColor
    Screen screen = Screen::Chat;
};
Target tg;
void useTarget(const Target& t) {  // another EditBox: its own suggestions state
    if (t.field != tg.field || t.screen != tg.screen) {
        const int next = cx.nextId;
        cx = ChatExt{};
        cx.nextId = next;
    }
    tg = t;
}
Target chatTarget(UiState& s) {  // ChatScreen: CommandSuggestions(..., false, false, 1, 10, true, -805306368)
    Target t;
    t.field = &s.chatInput;
    t.originX = 4, t.innerW = s.guiW() - 4;
    t.textY = s.guiH() - 12 + 2;
    return t;
}
struct Commands {  // the server's command tree (ClientboundCommandsPacket); kept over ChatExt resets
    cmd::Tree tree;
    std::vector<std::string> customCompletions;  // ClientSuggestionProvider.customCompletionSuggestions (a set)
};
Commands commands;


void hide() {
    cx.visible = false;
    cx.ghost.clear();
}

int screenX(UiState& s, int pos) {  // EditBox.getScreenX with the field's scroll
    const auto& f = *tg.field;
    pos = std::clamp(pos, f.scroll, int(f.value.size()));
    return tg.originX + s.font.width(f.value.substr(size_t(f.scroll), size_t(pos - f.scroll)));
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
    const std::string& v = tg.field->value;
    cx.ghost = applied.starts_with(v) ? applied.substr(v.size()) : std::string();  // calculateSuggestionSuffix
}

void cycle(UiState& s, int dir) {
    select(s, cx.current + dir);
    const int n = int(cx.list.size());
    const int first = cx.offset, last = cx.offset + tg.limit - 1;
    if (cx.current < first) cx.offset = std::clamp(cx.current, 0, std::max(n - tg.limit, 0));
    else if (cx.current > last) cx.offset = std::clamp(cx.current + tg.lineStart - tg.limit, 0, std::max(n - tg.limit, 0));
}

void useSuggestion(UiState& s) {
    if (cx.list.empty()) return;
    const Suggestion& sg = cx.list[size_t(cx.current)];
    auto& f = *tg.field;
    f.value = applySuggestion(sg, cx.original);
    f.cursor = f.selection = std::min(int(f.value.size()), cx.start + int(sg.text.size()));
    cx.lastValue = f.value;  // keepSuggestions: no new request for this change
    select(s, cx.current);
    cx.tabCycles = true;
}

void showSuggestions(UiState& s) {  // CommandSuggestions.showSuggestions
    if (!cx.haveResult || cx.result.empty()) return;
    const auto& f = *tg.field;
    int maxW = 0;
    for (const Suggestion& sg : cx.result) maxW = std::max(maxW, s.font.width(sg.text));
    const int x = std::clamp(screenX(s, cx.start), 0, tg.originX + tg.innerW - maxW);
    const int y = tg.anchorBottom ? s.guiH() - 12 : 72;
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
    const int shown = std::min(int(cx.list.size()), tg.limit);
    cx.x = x - (tg.bordered ? 0 : 1);
    cx.y = tg.anchorBottom ? y - 3 - shown * 12 : y - (tg.bordered ? 1 : 0);
    cx.w = maxW + 1, cx.h = shown * 12;
    cx.original = f.value;
    cx.offset = 0;
    cx.tabCycles = false;
    cx.visible = true;
    select(s, 0);
}

// CommandDispatcher.parse of the input with the server's tree (redone when the text changes).
void updateParse(UiState& s) {
    const std::string& v = tg.field->value;
    if (cx.parsed && v == cx.parsedValue) return;
    cx.parsed = true;
    cx.parsedValue = v;
    const bool slash = !v.empty() && v[0] == '/';
    cx.haveParse = (slash || tg.commandsOnly) && commands.tree.valid();
    if (cx.haveParse) cx.parse = cmd::parse(commands.tree, v, slash ? 1 : 0);
}

// CommandSuggestions.updateUsageInfo: parse errors when the cursor is at the end and nothing completes, else the
// usage of the arguments at the cursor (fillNodeUsage), else the trailing-data error.
void updateUsage(UiState& s) {
    cx.usage.clear();
    if (!cx.haveParse) return;
    const auto& f = *tg.field;
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
    cx.usageX = cx.usage.empty() ? 0 : std::clamp(screenX(s, cx.ctxStart), 0, tg.originX + tg.innerW - cx.usageW);
}

std::string lowerAscii(std::string v) {
    for (char& ch : v) ch = char(std::tolower(u8(ch)));
    return v;
}

// SharedSuggestionProvider.suggest(ClientSuggestionProvider.getCustomTabSuggestions(), builder): online player names and
// the custom completions matching the word (matchesSubStr: at the start or after . _ /), sorted like Suggestions.create;
// the word itself is not suggested (SuggestionsBuilder.suggest).
std::vector<std::string> customTabSuggestions(const std::string& word) {
    std::vector<std::string> all;
    for (const PlayerInfoView& p : playerInfos()) all.push_back(p.name);
    for (const std::string& c : commands.customCompletions)
        if (std::find(all.begin(), all.end(), c) == all.end()) all.push_back(c);
    const std::string prefix = lowerAscii(word);
    std::vector<std::string> out;
    for (const std::string& name : all) {
        const std::string l = lowerAscii(name);
        size_t index = 0;
        bool match = false;
        while (true) {
            if (l.compare(index, prefix.size(), prefix) == 0) {
                match = true;
                break;
            }
            const size_t split = l.find_first_of("._/", index);
            if (split == std::string::npos) break;
            index = split + 1;
        }
        if (match && name != word) out.push_back(name);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return lowerAscii(a) < lowerAscii(b); });
    return out;
}

// Input changed (ChatScreen.onEdited -> CommandSuggestions.updateCommandInfo): complete locally from the command tree
// (literals, booleans) or ask the server (other arguments, or no tree).
void update(UiState& s) {
    const auto& f = *tg.field;
    updateParse(s);
    if (cx.tracking && f.value == cx.lastValue) return;
    cx.tracking = true;
    cx.lastValue = f.value;
    hide();
    cx.haveResult = false;
    cx.usage.clear();
    if (!tg.commandsOnly && !f.value.empty() && f.value[0] != '/' && s.online()) {
        // Not a command: player names and the server's custom completions for the last word (Tab shows them).
        cx.pendingId = -1;
        if (f.value.find_first_not_of(" \t") == std::string::npos) return;
        const std::string partial = f.value.substr(0, size_t(std::clamp(f.cursor, 0, int(f.value.size()))));
        const size_t ws = partial.find_last_of(" \t");  // getLastWordIndex: after the last whitespace run
        const int lastWord = ws == std::string::npos ? 0 : int(ws) + 1;
        cx.haveResult = true, cx.start = lastWord, cx.length = int(partial.size()) - lastWord;
        cx.result.clear();
        for (const std::string& t : customTabSuggestions(partial.substr(size_t(lastWord)))) cx.result.push_back({t});
        return;
    }
    const bool slash = !f.value.empty() && f.value[0] == '/';
    const int parseStart = tg.pastError ? (slash ? 1 : 0) : 1;  // onlyShowIfCursorPastError: from the reader's cursor
    if (!(tg.commandsOnly || slash) || f.cursor < parseStart || !s.online()) {
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
    client.onPlay("custom_chat_completions", [sp](net::Reader& r) {  // ClientboundCustomChatCompletionsPacket
        const int action = r.varInt();  // 0 add, 1 remove, 2 set
        std::vector<std::string> entries;
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) entries.emplace_back(r.str());
        if (!r.ok) return;
        sp->post([action, entries] {  // ClientSuggestionProvider.modifyCustomCompletions
            auto& v = commands.customCompletions;
            if (action == 2) v.clear();
            for (const std::string& e : entries) {
                auto it = std::find(v.begin(), v.end(), e);
                if (action == 1 && it != v.end()) v.erase(it);
                else if (action != 1 && it == v.end()) v.push_back(e);
            }
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
            if (id != cx.pendingId || sp->screen != tg.screen) return;
            cx.haveResult = true, cx.start = start, cx.length = length, cx.result = list;
            updateUsage(*sp);
            if (sp->settings.autoSuggestions) showSuggestions(*sp);  // CommandSuggestions.updateCommandInfo
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
            useTarget(chatTarget(*sp));
            updateParse(*sp);
            if (!cx.haveParse) return false;
            out = cmd::format(cx.parse, visible, offset);
            return true;
        };
    };
    m.tick = [](UiState& s) {
        if (s.screen != tg.screen) cx.open = false;
    };
    return m;
}());

}  // namespace

CommandCheck verifyCommand(const std::string& command) {  // ClientPacketListener.verifyCommand
    const std::string input = "/" + command;
    const cmd::Parse p = cmd::parse(commands.tree, input, 1);
    if (!cmd::validCommand(commands.tree, p)) return CommandCheck::ParseErrors;
    if (cmd::hasSignedArguments(commands.tree, p)) return CommandCheck::SignatureRequired;
    if (!cmd::validCommand(commands.tree, cmd::parse(commands.tree, input, 1, false))) return CommandCheck::PermissionsRequired;
    return CommandCheck::NoIssues;
}

bool commandHasSignedArguments(const std::string& command) {  // SignableCommand.of(...).arguments() not empty
    return cmd::hasSignedArguments(commands.tree, cmd::parse(commands.tree, "/" + command, 1));
}

namespace {

// --- chat component events ----------------------------------------------------------------------------------------

// The style under GUI point (mx, my) in the open chat (hud.cpp); null: none.
const Style* chatStyleAt(UiState& s, float mx, float my, Style& out) { return chatLineStyleAt(s, mx, my, out) ? &out : nullptr; }

}  // namespace

std::vector<Text> hoverLines(UiState& s, const TextEvents& ev) {
    std::vector<Text> lines;
    if (ev.hover == "show_text") {
        lines = s.font.split(ev.hoverText, std::max(s.guiW() / 2, 200));
    } else if (ev.hover == "show_item") {
        std::string id = ev.hoverId.find(':') == std::string::npos ? "minecraft:" + ev.hoverId : ev.hoverId;
        ItemStack st;
        st.item = s.items.id(id), st.count = std::max(1, ev.hoverCount);
        if (st.item >= 0) lines = s.items.tooltip(st, s.settings.advancedItemTooltips);
    } else if (ev.hover == "show_entity" && s.settings.advancedItemTooltips) {  // HoverEvent.EntityTooltipInfo.getTooltipLines
        if (!ev.hoverText.empty()) lines.push_back(ev.hoverText);
        std::string type = ev.hoverId.find(':') == std::string::npos ? "minecraft:" + ev.hoverId : ev.hoverId;
        std::string key = "entity." + type;
        std::replace(key.begin(), key.end(), ':', '.');
        lines.push_back(translatable("gui.entity_tooltip.type", {Text(s.res.translate(key))}, s.lang()));
        if (!ev.hoverUuid.empty()) lines.push_back(Text(ev.hoverUuid));
    }
    return lines;
}

namespace {

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
    handleGameClick(s, clickOf(ev), Screen::Chat);  // dialogs.h: Screen.defaultHandleGameClickEvent
    return true;
}

// CommandSuggestions.keyPressed / mouseScrolled / mouseClicked for the current target.
bool suggestionsInput(UiState& s, const InputEvent& e) {
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
            cx.offset = std::clamp(cx.offset - (e.code > 0 ? 1 : -1), 0, std::max(int(cx.list.size()) - tg.limit, 0));
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
    }
    return false;
}

// The suggestion after the text, then the list or the usage lines; true when the list is drawn.
bool suggestionsDraw(UiState& s, Canvas& c) {
    if (!cx.open) cx.open = true, cx.tracking = false, hide(), cx.pendingId = -1;  // the screen (re)opened
    update(s);
    const auto& f = *tg.field;
    // EditBox suggestion: gray after the text when the cursor is at the end.
    if (!cx.ghost.empty() && f.cursor >= int(f.value.size()))
        s.font.draw(c, cx.ghost, float(screenX(s, int(f.value.size())) - 1), float(tg.textY), 0xFF808080, true);
    const float mx = s.mouseX, my = s.mouseY;
    const bool moved = mx != cx.lastMx || my != cx.lastMy;
    cx.lastMx = mx, cx.lastMy = my;
    if (cx.visible && !cx.list.empty()) {  // SuggestionsList.extractRenderState
        const int n = int(cx.list.size()), limit = std::min(n, tg.limit);
        const bool prev = cx.offset > 0, next = n > cx.offset + limit;
        if (prev || next) {
            c.fill(float(cx.x), float(cx.y - 1), float(cx.x + cx.w), float(cx.y), tg.fill);
            c.fill(float(cx.x), float(cx.y + cx.h), float(cx.x + cx.w), float(cx.y + cx.h + 1), tg.fill);
            for (int x = 0; x < cx.w; x += 2) {
                if (prev) c.fill(float(cx.x + x), float(cx.y - 1), float(cx.x + x + 1), float(cx.y), 0xFFFFFFFF);
                if (next) c.fill(float(cx.x + x), float(cx.y + cx.h), float(cx.x + x + 1), float(cx.y + cx.h + 1), 0xFFFFFFFF);
            }
        }
        bool hovered = false;
        for (int i = 0; i < limit; ++i) {
            const Suggestion& sg = cx.list[size_t(i + cx.offset)];
            const int y = cx.y + 12 * i;
            c.fill(float(cx.x), float(y), float(cx.x + cx.w), float(y + 12), tg.fill);
            if (mx > cx.x && mx < cx.x + cx.w && my > y && my < y + 12) {
                if (moved) select(s, i + cx.offset);
                hovered = true;
            }
            s.font.draw(c, sg.text, float(cx.x + 1), float(y + 2), i + cx.offset == cx.current ? 0xFFFFFF00 : 0xFFAAAAAA, true);
        }
        if (hovered && cx.list[size_t(cx.current)].hasTooltip)
            s.drawTooltip(c, {cx.list[size_t(cx.current)].tooltip}, mx, my);
        return true;
    }
    // CommandSuggestions.extractUsage: one line per entry, up from the bottom or down from 72.
    for (size_t i = 0; i < cx.usage.size(); ++i) {
        const int y = tg.anchorBottom ? s.guiH() - 27 - 12 * int(i) : 72 + 12 * int(i);
        c.fill(float(cx.usageX - 1), float(y), float(cx.usageX + cx.usageW + 1), float(y + 12), tg.fill);
        s.font.draw(c, cx.usage[i], float(cx.usageX), float(y + 2), 0xFFFFFFFF, true);
    }
    return false;
}

}  // namespace

bool chatExtInput(UiState& s, const InputEvent& e) {
    using E = InputEvent;
    useTarget(chatTarget(s));
    if (suggestionsInput(s, e)) return true;
    if (e.type == E::Button && e.down && e.code == 0) {
        if (chatSpecialClick(s, s.mouseX, s.mouseY)) return true;
        Style st;
        if (chatStyleAt(s, s.mouseX, s.mouseY, st)) return clickStyle(s, st, s.window->keyDown(VK_SHIFT));
    }
    return false;
}

void chatExtDraw(UiState& s, Canvas& c) {
    useTarget(chatTarget(s));
    if (suggestionsDraw(s, c)) return;
    const float mx = s.mouseX, my = s.mouseY;
    // Hovered message tag indicator (DrawingFocusedGraphicsAccess.handleTag): the tag's text.
    const int chatBottom = s.guiH() - 40;
    const int line = int(std::floor((float(chatBottom) - my) / 9.0f));
    if (my < float(chatBottom) && mx >= 0 && mx < 2 && line >= 0 && line < std::min(int(s.chatLines.size()) - s.chatScroll, 20)) {
        const ChatTag& tag = s.chatLines[size_t(line + s.chatScroll)].tag;
        if (tag.color && !tag.key.empty()) {
            s.drawTooltip(c, s.font.split(Text(s.res.translate(tag.key)), 210), mx, my);
            return;
        }
    }
    // Hovered chat component: its hover event.
    Style st;
    if (chatStyleAt(s, mx, my, st) && st.events && !st.events->hover.empty()) {
        const std::vector<Text> lines = hoverLines(s, *st.events);
        if (!lines.empty()) s.drawTooltip(c, lines, mx, my);
    }
}

namespace {
Target editorTarget(UiState& s, UiState::TextField& field, float x, float y, float w, float h) {
    Target t;
    t.field = &field;
    t.originX = int(x + 4), t.innerW = int(w - 8);  // EditBox.getScreenX(0) / getInnerWidth (bordered)
    t.textY = int(y + (h - 8) / 2);
    t.bordered = true, t.anchorBottom = false, t.commandsOnly = true, t.pastError = true;
    t.lineStart = 0, t.limit = 7, t.fill = 0x80000000u;
    t.screen = s.screen;
    return t;
}
}  // namespace

bool commandSuggestionsInput(UiState& s, UiState::TextField& field, float x, float y, float w, float h, const InputEvent& e) {
    useTarget(editorTarget(s, field, x, y, w, h));
    return suggestionsInput(s, e);
}

void commandFieldFormatter(UiState& s, UiState::TextField* field) {  // CommandSuggestions.formatChat on the field
    s.formattedField = field;
    if (!field) {
        s.fieldFormatter = nullptr;
        return;
    }
    UiState* sp = &s;
    s.fieldFormatter = [sp](const std::string& visible, int offset, Text& out) {
        if (tg.field != sp->formattedField) return false;  // its suggestions are not set up yet
        updateParse(*sp);
        if (!cx.haveParse) return false;
        out = cmd::format(cx.parse, visible, offset);
        return true;
    };
}

void commandSuggestionsDraw(UiState& s, UiState::TextField& field, float x, float y, float w, float h, Canvas& c) {
    useTarget(editorTarget(s, field, x, y, w, h));
    suggestionsDraw(s, c);
}

}  // namespace ui
