// Server dialogs (net.minecraft.server.dialog, client/gui/screens/dialog): ClientboundShowDialogPacket /
// ClientboundClearDialogPacket in the configuration and play phases, the dialog registry (minecraft:dialog with its
// tags from update_tags) and server links; DialogScreen with its subclasses (SimpleDialogScreen for notice and
// confirmation, MultiButtonDialogScreen, DialogListDialogScreen, ServerLinksDialogScreen), the body elements (plain
// message, item) and input controls (text, boolean, single option, number range), actions (static click events,
// dynamic/run_command templates, dynamic/custom sent back with the inputs), the after-action (close, none, wait for
// response: WaitingForResponseScreen), the warning screen behind the dialog's warning button, the command
// confirmation of ClientPacketListener.sendUnattendedCommand and ConfirmLinkScreen. Also the pause menu's custom
// options / server links button and the quick actions key.
#include "ui/dialogs.h"

#include "audio/audio.h"
#include "ui/frontend.h"
#include "ui/mp_hud.h"
#include "ui/ui_state.h"
#include "world/nbt_dom.h"

#include <shellapi.h>

#include <cmath>
#include <map>

namespace ui {

namespace {

// --- model (net.minecraft.server.dialog) ----------------------------------------------------------------------------

struct Action {  // dialog Action: a static click event, dynamic/run_command or dynamic/custom
    enum Kind { Static, RunCommand, Custom } kind = Static;
    ClickAction click;    // Static
    std::string text;     // RunCommand: the template; Custom: the id
    bool hasAdditions = false;
    NbtValue additions;   // Custom
};
struct ActionButton {
    Text label;
    bool hasTooltip = false;
    Text tooltip;
    int width = 150;
    std::optional<Action> action;
};
struct DialogRef {  // Holder<Dialog>: a registry id or an inline dialog (network NBT)
    std::string id, nbt;
};
struct Body {
    bool item = false;
    Text contents;  // plain message (or the item's description)
    int width = 200;
    bool hasDescription = false;
    ItemStack stack;
    bool decorations = true, tooltip = true;
    int itemW = 16, itemH = 16;
};
struct Input {
    std::string key;
    enum Kind { TextBox, Boolean, SingleOption, NumberRange } kind = TextBox;
    Text label;
    int width = 200;
    bool labelVisible = true;
    std::string initial;
    int maxLength = 32;
    bool multiline = false;
    int maxLines = -1, height = -1;
    bool initialBool = false;
    std::string onTrue = "true", onFalse = "false";
    struct Option {
        std::string id;
        bool hasDisplay = false;
        Text display;
        bool initial = false;
    };
    std::vector<Option> options;
    std::string labelFormat = "options.generic_value";
    float start = 0, end = 1;
    std::optional<float> initialValue, step;
};
struct Dialog {
    std::string type;  // notice, confirmation, multi_action, dialog_list, server_links
    Text title;
    bool hasExternalTitle = false;
    Text externalTitle;
    bool canCloseWithEscape = true, pause = true;
    int afterAction = 0;  // DialogAction: 0 close, 1 none, 2 wait for response
    std::vector<Body> body;
    std::vector<Input> inputs;
    std::vector<ActionButton> buttons;  // notice: action; confirmation: yes, no; multi_action: actions
    std::optional<ActionButton> exit;   // button list dialogs
    int columns = 2, buttonWidth = 150;
    std::string dialogs;                // dialog_list: the HolderSet as network NBT
    Text externalTitleOrTitle() const { return hasExternalTitle ? externalTitle : title; }
    std::optional<Action> onCancel() const {  // NoticeDialog / ConfirmationDialog (no) / ButtonListDialog (exit)
        if (type == "notice" && !buttons.empty()) return buttons[0].action;
        if (type == "confirmation" && buttons.size() > 1) return buttons[1].action;
        if (exit) return exit->action;
        return std::nullopt;
    }
};

std::string netNbt(const NbtValue& v) {
    std::vector<u8> out;
    nbtWriteNetwork(v, out);
    return std::string(out.begin(), out.end());
}
bool readNet(const std::string& nbt, NbtValue& out) {
    return !nbt.empty() && nbtReadNetwork(reinterpret_cast<const u8*>(nbt.data()), nbt.size(), out);
}
// An element of a heterogeneous NBT list: non-compound values are wrapped as {"": value}.
const NbtValue& unwrap(const NbtValue& e) { return e.type == TagCompound && e.names.size() == 1 && e.names[0].empty() ? e.items[0] : e; }
std::string strip(std::string id) { return id.starts_with("minecraft:") ? id.substr(10) : id; }
std::string withNs(const std::string& id) { return id.find(':') == std::string::npos ? "minecraft:" + id : id; }

struct Parser {
    const Lang* lang;
    const Items* items;

    const NbtValue* get(const NbtValue& v, const char* key) const {
        return const_cast<NbtValue&>(v).find(key);
    }
    Text text(const NbtValue* v) const { return v ? parseComponent(netNbt(*v), lang) : Text(); }
    std::string str(const NbtValue& v, const char* key, std::string def = {}) const {
        const NbtValue* e = get(v, key);
        return e && e->type == TagString ? e->s : def;
    }
    bool flag(const NbtValue& v, const char* key, bool def) const {
        const NbtValue* e = get(v, key);
        return e && e->type >= TagByte && e->type <= TagLong ? e->i != 0 : def;
    }
    double num(const NbtValue& v, const char* key, double def) const {
        const NbtValue* e = get(v, key);
        if (!e) return def;
        if (e->type >= TagByte && e->type <= TagLong) return double(e->i);
        if (e->type == TagFloat || e->type == TagDouble) return e->d;
        return def;
    }
    std::optional<double> optNum(const NbtValue& v, const char* key) const {
        if (!get(v, key)) return std::nullopt;
        return num(v, key, 0);
    }

    ClickAction click(const NbtValue& v, const std::string& action) const {  // a ClickEvent's map codec fields
        ClickAction c;
        c.action = strip(action);
        if (c.action == "open_url") c.value = str(v, "url");
        else if (c.action == "run_command" || c.action == "suggest_command") c.value = str(v, "command");
        else if (c.action == "copy_to_clipboard") c.value = str(v, "value");
        else if (c.action == "change_page") c.value = std::to_string(i64(num(v, "page", 1)));
        else if (c.action == "custom") {
            c.value = str(v, "id");
            if (const NbtValue* p = get(v, "payload")) c.nbt = netNbt(*p);
        } else if (c.action == "show_dialog") {
            if (const NbtValue* d = get(v, "dialog")) c.nbt = netNbt(*d);
        }
        return c;
    }

    std::optional<Action> action(const NbtValue* v) const {
        if (!v || v->type != TagCompound) return std::nullopt;
        Action a;
        const std::string type = strip(str(*v, "type"));
        if (type == "dynamic/run_command") {
            a.kind = Action::RunCommand, a.text = str(*v, "template");
        } else if (type == "dynamic/custom") {
            a.kind = Action::Custom, a.text = withNs(str(*v, "id"));
            if (const NbtValue* add = get(*v, "additions"); add && add->type == TagCompound) a.hasAdditions = true, a.additions = *add;
        } else {
            a.click = click(*v, type);
        }
        return a;
    }

    ActionButton button(const NbtValue& v, int defWidth = 150) const {
        ActionButton b;
        b.label = text(get(v, "label"));
        if (const NbtValue* t = get(v, "tooltip")) b.hasTooltip = true, b.tooltip = text(t);
        b.width = int(num(v, "width", defWidth));
        b.action = action(get(v, "action"));
        return b;
    }

    Body body(const NbtValue& v) const {
        Body b;
        if (v.type != TagCompound) {  // PlainMessage.CODEC alternative: a bare component
            b.contents = text(&v);
            return b;
        }
        const std::string type = strip(str(v, "type", "plain_message"));
        if (type == "item") {
            b.item = true;
            if (const NbtValue* it = get(v, "item")) {  // ItemStackTemplate: id, count, components
                std::string id = it->type == TagString ? it->s : str(*it, "id");
                b.stack.item = items->id(withNs(id));
                b.stack.count = it->type == TagCompound ? int(num(*it, "count", 1)) : 1;
                if (it->type == TagCompound)
                    if (const NbtValue* comps = get(*it, "components"); comps && comps->type == TagCompound) {
                        if (const NbtValue* n = get(*comps, "minecraft:custom_name")) b.stack.hasCustomName = true, b.stack.customName = text(n);
                        if (const NbtValue* n = get(*comps, "minecraft:item_name")) b.stack.hasItemName = true, b.stack.itemName = text(n);
                        if (const NbtValue* l = get(*comps, "minecraft:lore"); l && l->type == TagList)
                            for (const NbtValue& line : l->items) b.stack.lore.push_back(text(&line));
                        if (const NbtValue* g = get(*comps, "minecraft:enchantment_glint_override")) b.stack.glint = g->i != 0;
                        if (const NbtValue* d = get(*comps, "minecraft:damage")) b.stack.damage = int(d->i);
                    }
            }
            if (const NbtValue* d = get(v, "description")) {
                b.hasDescription = true;
                if (d->type == TagCompound && get(*d, "contents")) {
                    b.contents = text(get(*d, "contents"));
                    b.width = int(num(*d, "width", 200));
                } else {
                    b.contents = text(d);
                }
            }
            b.decorations = flag(v, "show_decorations", true);
            b.tooltip = flag(v, "show_tooltip", true);
            b.itemW = int(num(v, "width", 16)), b.itemH = int(num(v, "height", 16));
            return b;
        }
        b.contents = text(get(v, "contents"));
        b.width = int(num(v, "width", 200));
        return b;
    }

    Input input(const NbtValue& v) const {
        Input in;
        in.key = str(v, "key");
        const std::string type = strip(str(v, "type"));
        in.label = text(get(v, "label"));
        in.width = int(num(v, "width", 200));
        if (type == "boolean") {
            in.kind = Input::Boolean;
            in.initialBool = flag(v, "initial", false);
            in.onTrue = str(v, "on_true", "true"), in.onFalse = str(v, "on_false", "false");
        } else if (type == "single_option") {
            in.kind = Input::SingleOption;
            in.labelVisible = flag(v, "label_visible", true);
            if (const NbtValue* opts = get(v, "options"); opts && opts->type == TagList)
                for (const NbtValue& item : opts->items) {
                    const NbtValue& o = unwrap(item);
                    Input::Option op;
                    if (o.type == TagString) {
                        op.id = o.s;
                    } else {
                        op.id = str(o, "id");
                        if (const NbtValue* d = get(o, "display")) op.hasDisplay = true, op.display = text(d);
                        op.initial = flag(o, "initial", false);
                    }
                    in.options.push_back(std::move(op));
                }
        } else if (type == "number_range") {
            in.kind = Input::NumberRange;
            in.labelFormat = str(v, "label_format", "options.generic_value");
            in.start = float(num(v, "start", 0)), in.end = float(num(v, "end", 1));
            if (auto i = optNum(v, "initial")) in.initialValue = float(*i);
            if (auto s = optNum(v, "step")) in.step = float(*s);
        } else {
            in.kind = Input::TextBox;
            in.labelVisible = flag(v, "label_visible", true);
            in.initial = str(v, "initial");
            in.maxLength = int(num(v, "max_length", 32));
            if (const NbtValue* m = get(v, "multiline"); m && m->type == TagCompound) {
                in.multiline = true;
                in.maxLines = int(num(*m, "max_lines", -1));
                in.height = int(num(*m, "height", -1));
            }
        }
        return in;
    }

    std::shared_ptr<Dialog> dialog(const NbtValue& v) const {
        if (v.type != TagCompound) return nullptr;
        auto d = std::make_shared<Dialog>();
        d->type = strip(str(v, "type"));
        d->title = text(get(v, "title"));
        if (const NbtValue* t = get(v, "external_title")) d->hasExternalTitle = true, d->externalTitle = text(t);
        d->canCloseWithEscape = flag(v, "can_close_with_escape", true);
        d->pause = flag(v, "pause", true);
        const std::string after = str(v, "after_action", "close");
        d->afterAction = after == "none" ? 1 : after == "wait_for_response" ? 2 : 0;
        if (const NbtValue* b = get(v, "body")) {  // DialogBody.COMPACT_LIST_CODEC: one or a list
            if (b->type == TagList)
                for (const NbtValue& e : b->items) d->body.push_back(body(unwrap(e)));
            else
                d->body.push_back(body(*b));
        }
        if (const NbtValue* in = get(v, "inputs"); in && in->type == TagList)
            for (const NbtValue& e : in->items) d->inputs.push_back(input(e));
        if (d->type == "notice") {
            if (const NbtValue* a = get(v, "action")) d->buttons.push_back(button(*a));
            else d->buttons.push_back({translatable("gui.ok", {}, lang)});  // NoticeDialog.DEFAULT_ACTION
        } else if (d->type == "confirmation") {
            if (const NbtValue* y = get(v, "yes")) d->buttons.push_back(button(*y));
            if (const NbtValue* n = get(v, "no")) d->buttons.push_back(button(*n));
        } else if (d->type == "multi_action") {
            if (const NbtValue* a = get(v, "actions"); a && a->type == TagList)
                for (const NbtValue& e : a->items) d->buttons.push_back(button(e));
        }
        if (const NbtValue* e = get(v, "exit_action")) d->exit = button(*e);
        d->columns = std::max(1, int(num(v, "columns", 2)));
        d->buttonWidth = int(num(v, "button_width", 150));
        if (const NbtValue* l = get(v, "dialogs")) d->dialogs = netNbt(*l);
        return d;
    }
};

// --- connection data ------------------------------------------------------------------------------------------------

struct Link {
    Text name;
    std::string url;
};

// --- screens --------------------------------------------------------------------------------------------------------

struct InputState {
    Ui::State::TextField field;  // text (single line)
    std::string multi;          // text (multiline)
    int multiCursor = 0;
    float multiScroll = 0;
    bool checked = false;
    int option = 0;
    float slider = 0.5f;
};

struct Widget {
    enum Kind { Message, Item, Button, Checkbox, Cycle, Slider, Edit, MultiEdit, Label } kind = Label;
    int x = 0, y = 0, w = 0, h = 0;  // body widgets: relative to the body's top-left
    Text text;
    std::vector<Text> lines;
    int input = -1;
    int button = -1;  // Button: index into Instance::buttons
    ItemStack stack;
    bool decorations = true, tooltip = true;
};

struct Instance {  // an open DialogScreen
    std::shared_ptr<const Dialog> d;
    Screen previous = Screen::None;
    bool config = false;  // shown in the configuration phase: commands are not supported
    std::vector<InputState> inputs;
    std::vector<ActionButton> buttons;  // every button of the screen (body grid and footer)
    std::vector<Widget> body, fixed;    // fixed: header and footer (absolute)
    int bodyW = 0, bodyH = 0, footerH = 33;
    float scroll = 0;
    int focus = -1;  // the focused input (text boxes), -1 none
    int draggingSlider = -1;
    bool hovered = false;
};

// A screen to show (Minecraft.setScreen's argument).
struct Target {
    enum Kind { Plain, DialogScreen, Waiting } kind = Plain;
    Screen screen = Screen::None;          // Plain: the screen (None: the game); Waiting: its previous screen
    std::shared_ptr<Instance> dialog;      // DialogScreen
};

struct ConfirmButton {
    Text label;
    int width = 150;
    std::function<void()> press;
};

struct DState {
    enum class Mode { Dialog, Warning, Waiting, Confirm } mode = Mode::Dialog;
    std::shared_ptr<Instance> dialog;  // Dialog: shown
    Target warningReturn;              // Warning: where Back goes
    Screen waitingPrevious = Screen::None;
    int waitingTicks = 0;
    // ConfirmScreen (warning, command confirmation, links)
    Text confirmTitle, confirmMessage, confirmWarning;
    std::vector<ConfirmButton> confirmButtons;
    std::function<void()> confirmEscape;
    // Connection data (main thread)
    std::vector<Link> links;                                  // ClientCommonPacketListenerImpl.serverLinks
    std::map<std::string, std::vector<int>> dialogTags;      // minecraft:dialog tags -> registry ids
    std::vector<Widget> confirmWidgets;
};
DState g;

Parser parser(UiState& s) { return Parser{s.lang(), &s.items}; }

// Holder<Dialog> resolution: a registry id or an inline dialog.
std::shared_ptr<Dialog> resolve(UiState& s, const DialogRef& ref) {
    NbtValue v;
    if (!ref.nbt.empty()) {
        if (!readNet(ref.nbt, v)) return nullptr;
        if (v.type == TagString) return resolve(s, {v.s, {}});
        return parser(s).dialog(v);
    }
    if (!s.client) return nullptr;
    const std::string id = withNs(ref.id);
    for (const auto& e : s.client->registryEntries("minecraft:dialog"))
        if (e.id == id) {
            if (!readNet(e.nbt, v)) return nullptr;
            return parser(s).dialog(v);
        }
    LOG("ui: unknown dialog %s", id.c_str());
    return nullptr;
}

std::vector<DialogRef> tagMembers(UiState& s, const std::string& tag) {
    std::vector<DialogRef> out;
    if (!s.client) return out;
    const auto& entries = s.client->registryEntries("minecraft:dialog");
    if (auto it = g.dialogTags.find(withNs(tag)); it != g.dialogTags.end())
        for (int id : it->second)
            if (id >= 0 && size_t(id) < entries.size()) out.push_back({entries[size_t(id)].id, {}});
    return out;
}

// HolderSet<Dialog> (RegistryCodecs.holderSet): "#tag", an id, an inline dialog or a list of those.
std::vector<DialogRef> holderSet(UiState& s, const std::string& nbt) {
    std::vector<DialogRef> out;
    NbtValue v;
    if (!readNet(nbt, v)) return out;
    auto one = [&](const NbtValue& e) {
        if (e.type == TagString && e.s.starts_with("#")) {
            const auto members = tagMembers(s, e.s.substr(1));
            out.insert(out.end(), members.begin(), members.end());
        } else if (e.type == TagString) {
            out.push_back({e.s, {}});
        } else if (e.type == TagCompound) {
            out.push_back({{}, netNbt(e)});
        }
    };
    if (v.type == TagList)
        for (const NbtValue& e : v.items) one(unwrap(e));
    else
        one(v);
    return out;
}

// --- number range (NumberRangeInput.RangeInfo) ---
float scaledValue(const Input& in, float slider) {
    const float value = in.start + (in.end - in.start) * slider;
    if (!in.step) return value;
    const float initial = in.initialValue ? *in.initialValue : (in.start + in.end) / 2.0f;
    auto toSlider = [&](float v) { return in.start == in.end ? 0.5f : (v - in.start) / (in.end - in.start); };
    const int steps = int(std::lround((value - initial) / *in.step));
    float result = initial + float(steps) * *in.step;
    const float pos = toSlider(result);
    if (pos < 0.0f || pos > 1.0f) result = initial + float(steps - (steps > 0) + (steps < 0)) * *in.step;
    return result;
}
float initialSlider(const Input& in) {
    const float v = in.initialValue ? *in.initialValue : (in.start + in.end) / 2.0f;
    return in.start == in.end ? 0.5f : (v - in.start) / (in.end - in.start);
}
std::string floatString(float v) {  // NumberRangeHandler.valueToString: an int when whole, else Float.toString
    if (float(int(v)) == v) return std::to_string(int(v));
    char buf[32];
    for (int p = 1; p <= 9; ++p) {  // the shortest decimal that reads back as the same float
        std::snprintf(buf, sizeof(buf), "%.*f", p, double(v));
        if (std::strtof(buf, nullptr) == v) break;
    }
    return buf;
}

std::string escapeWithoutQuotes(const std::string& in) {  // StringTag.escapeWithoutQuotes
    std::string out;
    for (char ch : in) {
        if (ch == '"' || ch == '\'' || ch == '\\') {
            out += '\\', out += ch;
        } else if (u8(ch) < 0x20) {
            static const char hex[] = "0123456789ABCDEF";
            switch (ch) {
                case '\b': out += "\\b"; break;
                case '\t': out += "\\t"; break;
                case '\n': out += "\\n"; break;
                case '\f': out += "\\f"; break;
                case '\r': out += "\\r"; break;
                default: out += "\\x", out += hex[u8(ch) >> 4], out += hex[u8(ch) & 15];
            }
        } else {
            out += ch;
        }
    }
    return out;
}

// Action.ValueGetter of each input: template text and NBT.
std::string templateValue(const Input& in, const InputState& st) {
    switch (in.kind) {
        case Input::Boolean: return st.checked ? in.onTrue : in.onFalse;
        case Input::SingleOption: return in.options.empty() ? std::string() : in.options[size_t(st.option)].id;
        case Input::NumberRange: return floatString(scaledValue(in, st.slider));
        default: return escapeWithoutQuotes(in.multiline ? st.multi : st.field.value);
    }
}
NbtValue tagValue(const Input& in, const InputState& st) {
    NbtValue v;
    switch (in.kind) {
        case Input::Boolean: v.type = TagByte, v.i = st.checked; break;
        case Input::NumberRange: v.type = TagFloat, v.d = scaledValue(in, st.slider); break;
        case Input::SingleOption:
            v.type = TagString, v.s = in.options.empty() ? std::string() : in.options[size_t(st.option)].id;
            break;
        default: v.type = TagString, v.s = in.multiline ? st.multi : st.field.value;
    }
    return v;
}

// Action.createAction with the screen's inputs.
std::optional<ClickAction> createAction(const Instance& inst, const std::optional<Action>& a) {
    if (!a) return std::nullopt;
    if (a->kind == Action::Static) return a->click;
    if (a->kind == Action::RunCommand) {  // ParsedTemplate.instantiate: $(key) -> the value, "" when unknown
        std::string out;
        const std::string& t = a->text;
        for (size_t i = 0; i < t.size();) {
            const size_t close = t[i] == '$' && i + 1 < t.size() && t[i + 1] == '(' ? t.find(')', i + 2) : std::string::npos;
            if (close == std::string::npos) {
                out += t[i++];
                continue;
            }
            const std::string key = t.substr(i + 2, close - i - 2);
            for (size_t k = 0; k < inst.d->inputs.size(); ++k)
                if (inst.d->inputs[k].key == key) out += templateValue(inst.d->inputs[k], inst.inputs[k]);
            i = close + 1;
        }
        return ClickAction{"run_command", out, {}};
    }
    NbtValue tag = a->hasAdditions ? a->additions : NbtValue::make(TagCompound);  // CustomAll: additions + every input
    for (size_t k = 0; k < inst.d->inputs.size(); ++k) tag.set(inst.d->inputs[k].key, tagValue(inst.d->inputs[k], inst.inputs[k]));
    return ClickAction{"custom", a->text, netNbt(tag)};
}

// --- screen switching -----------------------------------------------------------------------------------------------

void buildInstance(UiState& s, Instance& inst);

void setScreen(UiState& s, const Target& t) {  // Minecraft.setScreen
    if (t.kind == Target::DialogScreen && t.dialog) {
        g.mode = DState::Mode::Dialog;
        g.dialog = t.dialog;
        s.openScreen(Screen::Dialog);
    } else if (t.kind == Target::Waiting) {
        g.mode = DState::Mode::Waiting;
        g.waitingPrevious = t.screen;
        g.waitingTicks = 0;
        s.openScreen(Screen::Dialog);
    } else if (t.screen == Screen::None) {
        if (s.screen != Screen::None) s.closeScreen();
    } else {
        s.openScreen(t.screen);
    }
}

Target plain(Screen sc) { return Target{Target::Plain, sc, nullptr}; }

// The screen shown now, as a target (Minecraft.gui.screen()).
Target current(UiState& s) {
    if (s.screen != Screen::Dialog) return plain(s.screen);
    if (g.mode == DState::Mode::Dialog) return Target{Target::DialogScreen, Screen::None, g.dialog};
    if (g.mode == DState::Mode::Waiting) return Target{Target::Waiting, g.waitingPrevious, nullptr};
    return plain(Screen::Dialog);  // a confirmation: not a dialog
}

void showConfirmScreen(UiState& s, Text title, Text message, std::vector<ConfirmButton> buttons, std::function<void()> escape,
                       Text warning = Text()) {
    g.mode = DState::Mode::Confirm;
    g.confirmTitle = std::move(title), g.confirmMessage = std::move(message), g.confirmWarning = std::move(warning);
    g.confirmButtons = std::move(buttons);
    g.confirmEscape = std::move(escape);
    s.openScreen(Screen::Dialog);
}

void openUrl(const std::string& url) {
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, url.c_str(), int(url.size()), nullptr, 0)), L'\0');
    if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, url.c_str(), int(url.size()), w.data(), int(w.size()));
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Screen.clickUrlAction: ConfirmLinkScreen (untrusted): Yes / Copy to Clipboard / No, then `after`.
void clickUrl(UiState& s, const std::string& url, const Target& after) {
    if (!url.starts_with("http://") && !url.starts_with("https://")) return;  // ExtraCodecs.UNTRUSTED_URI
    UiState* sp = &s;
    std::vector<ConfirmButton> b;
    b.push_back({tr(s, "gui.yes"), 100, [sp, url, after] {
                     openUrl(url);
                     setScreen(*sp, after);
                 }});
    b.push_back({tr(s, "chat.copy"), 100, [sp, url, after] {
                     sp->window->setClipboard(url);
                     setScreen(*sp, after);
                 }});
    b.push_back({tr(s, "gui.no"), 100, [sp, after] { setScreen(*sp, after); }});
    Style warn = colorStyle(0xFFCCCC);
    showConfirmScreen(s, tr(s, "chat.link.confirm"), Text(url), std::move(b), [sp, after] { setScreen(*sp, after); },
                      Text(s.res.translate("chat.link.warning"), warn));
}

void sendCommand(UiState& s, const std::string& command) {
    if (!s.online()) return;
    net::Writer w;
    w.str(command);
    s.client->sendPlay("chat_command", w);
    LOG("[chat] sent command /%s", command.c_str());
}

// ClientPacketListener.sendUnattendedCommand: run it, or confirm first (parse errors, permissions, signing).
void sendUnattendedCommand(UiState& s, const std::string& command, const Target& after) {
    const CommandCheck check = verifyCommand(command);
    if (check == CommandCheck::NoIssues) {
        sendCommand(s, command);
        return setScreen(s, after);
    }
    UiState* sp = &s;
    const Target here = current(s);
    Style yellow = colorStyle(0xFFFF55);
    const char* key = check == CommandCheck::ParseErrors ? "multiplayer.confirm_command.parse_errors"
                      : check == CommandCheck::SignatureRequired ? "multiplayer.confirm_command.signature_required"
                                                                : "multiplayer.confirm_command.permissions_required";
    const Text message = translatable(key, {Text(command, yellow)}, s.lang());
    std::vector<ConfirmButton> b;
    if (check == CommandCheck::SignatureRequired) {  // openSignedCommandSendConfirmationWindow
        const bool chat = after.kind == Target::Plain && after.screen == Screen::None;
        if (chat) {
            b.push_back({tr(s, "multiplayer.confirm_command.suggest_command"), 150, [sp, command] {
                             sp->openScreen(Screen::Chat);  // openChatAndAddText(COMMAND, command)
                             sp->chatInput.value = "/" + command;
                             sp->chatInput.cursor = sp->chatInput.selection = int(sp->chatInput.value.size());
                         }});
        } else {
            b.push_back({tr(s, "chat.copy"), 150, [sp, command, after] {
                             sp->window->setClipboard("/" + command);
                             setScreen(*sp, after);
                         }});
        }
    } else {
        b.push_back({tr(s, "multiplayer.confirm_command.run_command"), 150, [sp, command, after] {
                         sendCommand(*sp, command);
                         setScreen(*sp, after);
                     }});
    }
    const bool hadScreen = !(here.kind == Target::Plain && here.screen == Screen::None);
    b.push_back({tr(s, hadScreen ? "gui.back" : "gui.cancel"), 150, [sp, here] { setScreen(*sp, here); }});
    showConfirmScreen(s, tr(s, "multiplayer.confirm_command.title"), message, std::move(b), [sp, here] { setScreen(*sp, here); });
}

void sendCustomAction(UiState& s, const std::string& id, const std::string& payload) {  // ServerboundCustomClickActionPacket
    if (!s.client) return;
    net::Writer w;
    w.str(withNs(id));
    const std::string tag = payload.empty() ? std::string(1, '\0') : payload;  // optional tag: TagEnd when absent
    w.varInt(i32(tag.size()));
    w.bytes(tag.data(), tag.size());
    if (s.client->inConfiguration()) s.client->sendConfig("custom_click_action", w);
    else s.client->sendPlay("custom_click_action", w);
    LOG("ui: custom click action %s (%zu bytes)", id.c_str(), payload.size());
}

void showDialog(UiState& s, const std::shared_ptr<Dialog>& d, const Target& active, bool config = false);

// DialogScreen.handleDialogClickEvent / Screen.defaultHandleGameClickEvent.
void handleClick(UiState& s, const ClickAction& c, const Target& after, bool config) {
    LOG("ui: click %s \"%s\"", c.action.c_str(), c.value.c_str());
    if (c.action == "run_command") {
        std::string cmd = c.value;
        if (!cmd.empty() && cmd[0] == '/') cmd.erase(0, 1);  // Commands.trimOptionalPrefix
        if (config) return LOG("ui: commands are not supported in configuration phase, trying to run '%s'", cmd.c_str());
        return sendUnattendedCommand(s, cmd, after);
    }
    if (c.action == "show_dialog") {
        NbtValue v;
        std::shared_ptr<Dialog> d;
        if (readNet(c.nbt, v)) d = v.type == TagString ? resolve(s, {v.s, {}}) : parser(s).dialog(v);
        if (d) showDialog(s, d, after, config);
        else LOG("ui: failed to show dialog");
        return;
    }
    if (c.action == "custom") {
        sendCustomAction(s, c.value, c.nbt);
        return setScreen(s, after);
    }
    if (c.action == "open_url") return clickUrl(s, c.value, after);  // the confirm screen returns to `after`
    if (c.action == "copy_to_clipboard") s.window->setClipboard(c.value);
    else if (c.action != "suggest_command") LOG("ui: don't know how to handle click event %s", c.action.c_str());
    if (s.screen != Screen::Chat || after.kind != Target::Plain || after.screen != Screen::Chat) setScreen(s, after);
    if (c.action == "suggest_command" && after.kind == Target::Plain && after.screen == Screen::Chat) {
        s.chatInput.value = c.value.substr(0, size_t(s.chatInput.maxLength));  // ChatScreen.insertText(command, true)
        s.chatInput.cursor = s.chatInput.selection = int(s.chatInput.value.size());
    }
}

void showDialog(UiState& s, const std::shared_ptr<Dialog>& d, const Target& active, bool config) {
    // ClientCommonPacketListenerImpl.showDialog: a dialog replaces a dialog (keeping its previous screen) or the
    // waiting screen; under the warning screen it becomes what Back returns to.
    auto inst = std::make_shared<Instance>();
    inst->d = d;
    inst->config = config;
    if (s.screen == Screen::Dialog && g.mode == DState::Mode::Warning && active.kind == Target::Plain && active.screen == Screen::Dialog) {
        inst->previous = g.warningReturn.kind == Target::DialogScreen && g.warningReturn.dialog ? g.warningReturn.dialog->previous
                                                                                                  : g.warningReturn.screen;
        g.warningReturn = Target{Target::DialogScreen, Screen::None, inst};
        return;
    }
    inst->previous = active.kind == Target::DialogScreen && active.dialog ? active.dialog->previous : active.screen;
    if (inst->previous == Screen::Dialog) inst->previous = Screen::None;  // over a confirmation: back to the game
    setScreen(s, Target{Target::DialogScreen, Screen::None, inst});
}

// DialogScreen.runAction(closeAction, afterAction).
void runAction(UiState& s, const std::shared_ptr<Instance>& inst, const std::optional<ClickAction>& click, int afterAction) {
    Target next;
    if (afterAction == 1) next = Target{Target::DialogScreen, Screen::None, inst};
    else if (afterAction == 2) next = Target{Target::Waiting, inst->previous, nullptr};
    else next = plain(inst->previous);
    if (click) handleClick(s, *click, next, inst->config);
    else setScreen(s, next);
}

void openWarning(UiState& s) {  // DialogScreen.WarningScreen
    g.warningReturn = Target{Target::DialogScreen, Screen::None, g.dialog};
    UiState* sp = &s;
    std::vector<ConfirmButton> b;
    const bool local = s.fe->cfg.singleplayer;  // CommonComponents.disconnectButtonLabel(isLocalServer)
    b.push_back({tr(s, local ? "menu.returnToMenu" : "menu.disconnect"), 150, [sp] {
                     if (sp->client) sp->client->disconnect(sp->res.translate("menu.custom_screen_info.disconnect"));
                 }});
    b.push_back({tr(s, "gui.back"), 150, [sp] { setScreen(*sp, g.warningReturn); }});
    showConfirmScreen(s, tr(s, "menu.custom_screen_info.title"), tr(s, "menu.custom_screen_info.contents"), std::move(b),
                      [sp] { setScreen(*sp, g.warningReturn); });
    g.mode = DState::Mode::Warning;
}

// --- layout (DialogScreen.init) -------------------------------------------------------------------------------------

constexpr int kHeader = 33;

struct Element {  // a body element: its widgets relative to its top-left
    int w = 0, h = 0;
    std::vector<Widget> widgets;
};

Text optionLabel(UiState& s, const Input& in, int option) {  // CycleButton: "Label: Value" or the value
    const Input::Option& o = in.options[size_t(option)];
    const Text value = o.hasDisplay ? o.display : Text(o.id);
    return in.labelVisible ? translatable("options.generic_value", {in.label, value}, s.lang()) : value;
}
Text sliderLabel(UiState& s, const Input& in, float slider) {
    return translatable(in.labelFormat, {in.label, Text(floatString(scaledValue(in, slider)))}, s.lang());
}

Element messageElement(UiState& s, const Text& contents, int width) {  // FocusableTextWidget (padding 4, centred)
    Element e;
    Widget w;
    w.kind = Widget::Message;
    w.text = contents;
    w.lines = s.font.split(contents, std::max(1, width - 8));
    w.w = width, w.h = int(w.lines.size()) * 9 + 8;
    e.w = w.w, e.h = w.h;
    e.widgets.push_back(std::move(w));
    return e;
}

Element buttonGrid(Instance& inst, const std::vector<int>& buttons, int columns) {  // DialogScreen.packControlsIntoColumns
    Element e;
    const int n = int(buttons.size());
    const int fullRows = n / columns, full = fullRows * columns;
    std::vector<int> colW(size_t(columns), 0);
    for (int i = 0; i < full; ++i) colW[size_t(i % columns)] = std::max(colW[size_t(i % columns)], inst.buttons[size_t(buttons[size_t(i)])].width);
    int gridW = 0;
    for (int c = 0; c < columns; ++c) gridW += colW[size_t(c)] + (c ? 2 : 0);
    int lastW = 0;
    for (int i = full; i < n; ++i) lastW += inst.buttons[size_t(buttons[size_t(i)])].width + (i > full ? 2 : 0);
    if (full == 0) gridW = 0;
    gridW = std::max(gridW, lastW);
    // Columns are centred when the last row is the wider one (GridLayout cells keep their widths).
    int x0 = 0;
    for (int i = 0; i < full; ++i) {
        const int c = i % columns, r = i / columns;
        int cx = x0;
        for (int k = 0; k < c; ++k) cx += colW[size_t(k)] + 2;
        Widget w;
        w.kind = Widget::Button;
        w.button = buttons[size_t(i)];
        w.w = inst.buttons[size_t(w.button)].width, w.h = 20;
        w.x = cx + (colW[size_t(c)] - w.w) / 2, w.y = r * 22;
        e.widgets.push_back(w);
    }
    if (n > full) {
        int x = (gridW - lastW) / 2;
        for (int i = full; i < n; ++i) {
            Widget w;
            w.kind = Widget::Button;
            w.button = buttons[size_t(i)];
            w.w = inst.buttons[size_t(w.button)].width, w.h = 20;
            w.x = x, w.y = fullRows * 22;
            x += w.w + 2;
            e.widgets.push_back(w);
        }
    }
    e.w = gridW, e.h = (fullRows + (n > full ? 1 : 0)) * 22 - 2;
    if (n == 0) e.h = 0;
    return e;
}

void buildInstance(UiState& s, Instance& inst) {
    const Dialog& d = *inst.d;
    const int W = s.guiW(), H = s.guiH();
    inst.body.clear(), inst.fixed.clear(), inst.buttons.clear();
    std::vector<Element> elements;
    for (const Body& b : d.body) {
        if (!b.item) {
            elements.push_back(messageElement(s, b.contents, b.width));
            continue;
        }
        Element e;
        Widget item;
        item.kind = Widget::Item;
        item.stack = b.stack, item.decorations = b.decorations, item.tooltip = b.tooltip;
        item.w = b.itemW, item.h = b.itemH;
        if (b.hasDescription) {  // horizontal, spacing 2, vertically centred
            Element text = messageElement(s, b.contents, b.width);
            e.h = std::max(item.h, text.h);
            item.y = (e.h - item.h) / 2;
            e.widgets.push_back(item);
            Widget t = text.widgets[0];
            t.x = item.w + 2, t.y = (e.h - t.h) / 2;
            e.widgets.push_back(t);
            e.w = item.w + 2 + t.w;
        } else {
            e.w = item.w, e.h = item.h;
            item.text = s.items.hoverName(b.stack);
            e.widgets.push_back(item);
        }
        elements.push_back(std::move(e));
    }
    for (size_t k = 0; k < d.inputs.size(); ++k) {
        const Input& in = d.inputs[k];
        Element e;
        Widget w;
        w.input = int(k);
        if (in.kind == Input::TextBox) {
            w.kind = in.multiline ? Widget::MultiEdit : Widget::Edit;
            w.w = in.width;
            w.h = !in.multiline ? 20 : in.height > 0 ? in.height : std::min(9 * (in.maxLines > 0 ? in.maxLines : 4) + 8, 512);
            if (in.labelVisible) {  // CommonLayouts.labeledElement: the label, 4, the box
                Widget l;
                l.kind = Widget::Label;
                l.text = in.label;
                l.w = s.font.width(in.label), l.h = 9;
                e.widgets.push_back(l);
                w.y = 13;
                e.w = std::max(l.w, w.w);
            } else {
                e.w = w.w;
            }
            e.h = w.y + w.h;
        } else if (in.kind == Input::Boolean) {
            w.kind = Widget::Checkbox;
            w.text = in.label;
            w.w = 17 + 4 + s.font.width(in.label), w.h = 17;
            e.w = w.w, e.h = w.h;
        } else if (in.kind == Input::SingleOption) {
            w.kind = Widget::Cycle;
            w.w = in.width, w.h = 20;
            e.w = w.w, e.h = w.h;
        } else {
            w.kind = Widget::Slider;
            w.w = in.width, w.h = 20;
            e.w = w.w, e.h = w.h;
        }
        e.widgets.push_back(std::move(w));
        elements.push_back(std::move(e));
    }
    // Buttons: the body grid of button list dialogs, the footer of simple dialogs.
    const bool list = d.type == "multi_action" || d.type == "dialog_list" || d.type == "server_links";
    if (list) {
        std::vector<int> grid;
        if (d.type == "multi_action") {
            for (const ActionButton& b : d.buttons) grid.push_back(int(inst.buttons.size())), inst.buttons.push_back(b);
        } else if (d.type == "dialog_list") {  // DialogListDialogScreen: each dialog's external title opens it
            for (const DialogRef& ref : holderSet(s, d.dialogs)) {
                const std::shared_ptr<Dialog> sub = resolve(s, ref);
                if (!sub) continue;
                ActionButton b;
                b.label = sub->externalTitleOrTitle();
                b.width = d.buttonWidth;
                Action a;
                a.click = ClickAction{"show_dialog", {}, ref.nbt};
                if (ref.nbt.empty()) {
                    NbtValue id = NbtValue::make(TagString);
                    id.s = ref.id;
                    a.click.nbt = netNbt(id);
                }
                b.action = a;
                grid.push_back(int(inst.buttons.size())), inst.buttons.push_back(b);
            }
        } else {  // ServerLinksDialogScreen: a button per link
            for (const Link& l : g.links) {
                ActionButton b;
                b.label = l.name, b.width = d.buttonWidth;
                Action a;
                a.click = ClickAction{"open_url", l.url, {}};
                b.action = a;
                grid.push_back(int(inst.buttons.size())), inst.buttons.push_back(b);
            }
        }
        elements.push_back(buttonGrid(inst, grid, d.columns));
    }
    // Body: vertical, spacing 10, centred.
    int bodyW = 0, bodyH = 0;
    for (const Element& e : elements) bodyW = std::max(bodyW, e.w);
    for (size_t i = 0; i < elements.size(); ++i) {
        const Element& e = elements[i];
        const int ex = (bodyW - e.w) / 2;
        for (Widget w : e.widgets) {
            w.x += ex, w.y += bodyH;
            inst.body.push_back(std::move(w));
        }
        bodyH += e.h + (i + 1 < elements.size() ? 10 : 0);
    }
    inst.bodyW = bodyW, inst.bodyH = bodyH;
    // Header: the title and the warning button (horizontal, spacing 10, centred).
    const int titleW = s.font.width(d.title);
    const int hx = (W - (titleW + 10 + 20)) / 2, hy = (kHeader - 20) / 2;
    Widget title;
    title.kind = Widget::Label;
    title.text = d.title;
    title.x = hx, title.y = hy + (20 - 9) / 2, title.w = titleW, title.h = 9;
    inst.fixed.push_back(title);
    Widget warn;
    warn.kind = Widget::Button;
    warn.button = -2;  // the warning button
    warn.x = hx + titleW + 10, warn.y = hy, warn.w = 20, warn.h = 20;
    if (warn.x < 0 || warn.y < 0 || warn.x > W - 20 || warn.y > H - 20) warn.x = std::max(0, W - 40), warn.y = std::min(5, H);
    inst.fixed.push_back(warn);
    // Footer.
    std::vector<int> footer;
    if (!list) {
        for (const ActionButton& b : d.buttons) footer.push_back(int(inst.buttons.size())), inst.buttons.push_back(b);
    } else if (d.exit) {
        footer.push_back(int(inst.buttons.size())), inst.buttons.push_back(*d.exit);
    }
    inst.footerH = list && !d.exit ? 5 : kHeader;
    int fw = 0;
    for (size_t i = 0; i < footer.size(); ++i) fw += inst.buttons[size_t(footer[i])].width + (i ? 8 : 0);
    int fx = (W - fw) / 2;
    for (int b : footer) {
        Widget w;
        w.kind = Widget::Button;
        w.button = b;
        w.w = inst.buttons[size_t(b)].width, w.h = 20;
        w.x = fx, w.y = H - inst.footerH + (inst.footerH - 20) / 2;
        fx += w.w + 8;
        inst.fixed.push_back(w);
    }
}

// The scroll area (ScrollableLayout in the contents frame).
struct Area {
    int x, y, w, h;  // the container
    int contentX, contentY;
    bool scrollable;
};
Area area(UiState& s, const Instance& inst) {
    const int W = s.guiW(), H = s.guiH();
    const int avail = std::max(0, H - kHeader - inst.footerH);
    Area a;
    a.w = inst.bodyW + 20;
    a.h = std::min(inst.bodyH, avail);
    a.x = (W - a.w) / 2;
    a.y = std::min(kHeader + 30, H - inst.footerH - a.h);
    a.scrollable = inst.bodyH > a.h;
    a.contentX = a.x + 10, a.contentY = a.y - int(inst.scroll);
    return a;
}

float maxScroll(UiState& s, const Instance& inst) { return float(std::max(0, inst.bodyH - area(s, inst).h)); }

void initInputs(Instance& inst) {
    inst.inputs.assign(inst.d->inputs.size(), InputState{});
    for (size_t k = 0; k < inst.d->inputs.size(); ++k) {
        const Input& in = inst.d->inputs[k];
        InputState& st = inst.inputs[k];
        st.field.value = st.multi = in.initial;
        st.field.maxLength = in.maxLength * 4;  // TextField counts bytes; max_length is characters (checked on typing)
        st.field.cursor = st.field.selection = int(in.initial.size());
        st.multiCursor = int(in.initial.size());
        st.checked = in.initialBool;
        for (size_t o = 0; o < in.options.size(); ++o)
            if (in.options[o].initial) st.option = int(o);
        st.slider = initialSlider(in);
    }
}

// --- drawing -------------------------------------------------------------------------------------------------------

int codepoints(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size();) nextCodepoint(s, i), n += 1;
    return n;
}

void drawButtonSprite(UiState& s, Canvas& c, float x, float y, float w, float h, const Text& label, bool active, bool hover) {
    const char* sp = !active ? "widget/button_disabled" : hover ? "widget/button_highlighted" : "widget/button";
    blitSprite(c, s.sprites.sprite(sp), x, y, w, h);
    // AbstractButton.extractDefaultLabel: centred, clipped to the button's inner area (2 px).
    const int tw = s.font.width(label);
    const float inner = w - 4;
    c.pushScissor(x + 2, y, x + w - 2, y + h);
    const float tx = float(tw) <= inner ? x + float(int(w - float(tw)) / 2) : x + 2;
    s.font.draw(c, label, tx, y + float(int(h - 8) / 2), active ? 0xFFFFFFFF : 0xFFA0A0A0, true);
    c.popScissor();
}

bool inside(float mx, float my, float x, float y, float w, float h) { return mx >= x && my >= y && mx < x + w && my < y + h; }

// The style under the mouse in a message widget (for click / hover events).
const Style* messageStyleAt(UiState& s, const Widget& w, float ox, float oy, float mx, float my, Style& out) {
    const float top = oy + float(w.y) + 4;
    const int line = int(std::floor((my - top) / 9.0f));
    if (line < 0 || line >= int(w.lines.size())) return nullptr;
    const Text& t = w.lines[size_t(line)];
    const float x0 = ox + float(w.x) + float(w.w / 2 - s.font.width(t) / 2);
    float pen = x0;
    bool found = false;
    s.font.iterate(t, [&](int, const Style& st, u32 cp) {
        const float adv = s.font.glyph(cp).advanceFor(st.isBold());
        if (mx >= pen && mx < pen + adv) {
            out = st, found = true;
            return false;
        }
        pen += adv;
        return true;
    });
    return found ? &out : nullptr;
}

void drawMulti(UiState& s, Canvas& c, InputState& st, float x, float y, float w, float h, bool focused) {
    blitSprite(c, s.sprites.sprite(focused ? "widget/text_field_highlighted" : "widget/text_field"), x, y, w, h);
    const std::vector<Text> lines = s.font.split(Text(st.multi), int(w) - 8);
    c.pushScissor(x + 1, y + 1, x + w - 1, y + h - 1);
    float ty = y + 4 - st.multiScroll;
    for (const Text& l : lines) {
        s.font.draw(c, l, x + 4, ty, 0xFFE0E0E0, true);
        ty += 9;
    }
    if (focused && int((s.clock) / 0.3) % 2 == 0) {  // the cursor at the end of the text
        const float cx = x + 4 + float(lines.empty() ? 0 : s.font.width(lines.back()));
        const float cy = y + 4 - st.multiScroll + float(std::max<int>(0, int(lines.size()) - 1) * 9);
        if (!st.multi.empty() && st.multi.back() == '\n') s.font.draw(c, "_", x + 4, cy + 9, 0xFFD0D0D0, true);
        else s.font.draw(c, "_", cx, cy, 0xFFD0D0D0, true);
    }
    c.popScissor();
}

void drawDialog(UiState& s, Canvas& c, Instance& inst) {
    menuBackground(s, c);
    const float mx = s.mouseX, my = s.mouseY;
    const Area a = area(s, inst);
    const Dialog& d = *inst.d;
    std::vector<Text> tooltip;
    const ItemStack* tooltipItem = nullptr;
    // Slider dragging.
    if (inst.draggingSlider >= 0) {
        const Widget& w = inst.body[size_t(inst.draggingSlider)];
        inst.inputs[size_t(w.input)].slider = std::clamp((mx - (float(a.contentX + w.x) + 4)) / float(w.w - 8), 0.0f, 1.0f);
    }
    c.pushScissor(float(a.x), float(a.y), float(a.x + a.w), float(a.y + a.h));
    const bool inArea = inside(mx, my, float(a.x), float(a.y), float(a.w), float(a.h));
    for (size_t i = 0; i < inst.body.size(); ++i) {
        const Widget& w = inst.body[i];
        const float x = float(a.contentX + w.x), y = float(a.contentY + w.y);
        if (y > float(a.y + a.h) || y + float(w.h) < float(a.y)) continue;
        const bool hover = inArea && inside(mx, my, x, y, float(w.w), float(w.h));
        switch (w.kind) {
            case Widget::Label: s.drawText(c, w.text, x, y, 0xFFFFFFFF, true); break;
            case Widget::Message: {
                float ly = y + 4;
                for (const Text& l : w.lines) {
                    s.drawText(c, l, x + float(w.w / 2 - s.font.width(l) / 2), ly, 0xFFFFFFFF, true);
                    ly += 9;
                }
                Style st;
                if (hover && messageStyleAt(s, w, float(a.contentX), float(a.contentY), mx, my, st) && st.events &&
                    st.events->hover == "show_text")
                    tooltip = s.font.split(st.events->hoverText, std::max(s.guiW() / 2, 200));
                break;
            }
            case Widget::Item:
                s.drawItem(c, w.stack, x, y, w.decorations);
                if (hover && w.tooltip && !w.stack.empty()) tooltipItem = &w.stack;
                break;
            case Widget::Button: {
                const ActionButton& b = inst.buttons[size_t(w.button)];
                drawButtonSprite(s, c, x, y, float(w.w), float(w.h), b.label, true, hover);
                if (hover && b.hasTooltip) tooltip = s.font.split(b.tooltip, 170);
                break;
            }
            case Widget::Checkbox: {
                const bool on = inst.inputs[size_t(w.input)].checked, focused = inst.focus == w.input;
                const char* sp = on ? (focused ? "widget/checkbox_selected_highlighted" : "widget/checkbox_selected")
                                    : (focused ? "widget/checkbox_highlighted" : "widget/checkbox");
                blitSprite(c, s.sprites.sprite(sp), x, y, 17, 17);
                s.drawText(c, w.text, x + 17 + 4, y + float(17 / 2 - 9 / 2), 0xFFFFFFFF, true);
                break;
            }
            case Widget::Cycle:
                drawButtonSprite(s, c, x, y, float(w.w), float(w.h), optionLabel(s, d.inputs[size_t(w.input)], inst.inputs[size_t(w.input)].option),
                                 true, hover);
                break;
            case Widget::Slider: {
                const float v = inst.inputs[size_t(w.input)].slider;
                blitSprite(c, s.sprites.sprite("widget/slider"), x, y, float(w.w), float(w.h));
                const bool lit = hover || inst.draggingSlider == int(i);
                blitSprite(c, s.sprites.sprite(lit ? "widget/slider_handle_highlighted" : "widget/slider_handle"),
                           x + float(int(v * float(w.w - 8))), y, 8, 20);
                const Text label = sliderLabel(s, d.inputs[size_t(w.input)], v);
                s.drawCentered(c, label, x + float(w.w) / 2, y + float((w.h - 8) / 2), 0xFFFFFFFF);
                break;
            }
            case Widget::Edit: {
                InputState& st = inst.inputs[size_t(w.input)];
                const bool focused = inst.focus == w.input;
                blitSprite(c, s.sprites.sprite(focused ? "widget/text_field_highlighted" : "widget/text_field"), x, y, float(w.w), float(w.h));
                c.pushScissor(x + 2, std::max(y, float(a.y)), x + float(w.w) - 2, std::min(y + float(w.h), float(a.y + a.h)));
                s.drawTextField(c, st.field, x + 4, y + float((w.h - 8) / 2), float(w.w - 8), 0xFFE0E0E0, focused, true);
                c.popScissor();
                break;
            }
            case Widget::MultiEdit:
                drawMulti(s, c, inst.inputs[size_t(w.input)], x, y, float(w.w), float(w.h), inst.focus == w.input);
                break;
        }
    }
    c.popScissor();
    if (a.scrollable) {  // AbstractScrollArea.extractScrollbar
        const int barH = std::clamp(a.h * a.h / std::max(1, inst.bodyH), 32, std::max(32, a.h - 8));
        const float ms = maxScroll(s, inst);
        const int barY = ms <= 0 ? a.y : std::max(a.y, int(inst.scroll) * (a.h - barH) / int(ms) + a.y);
        blitSprite(c, s.sprites.sprite("widget/scroller_background"), float(a.x + a.w - 6), float(a.y), 6, float(a.h));
        blitSprite(c, s.sprites.sprite("widget/scroller"), float(a.x + a.w - 6), float(barY), 6, float(barH));
    }
    for (const Widget& w : inst.fixed) {
        const bool hover = inside(mx, my, float(w.x), float(w.y), float(w.w), float(w.h));
        if (w.kind == Widget::Label) {
            s.drawText(c, w.text, float(w.x), float(w.y), 0xFFFFFFFF, true);
        } else if (w.button == -2) {  // ImageButton with the warning sprites
            blitSprite(c, s.sprites.sprite(hover ? "dialog/warning_button_highlighted" : "dialog/warning_button"), float(w.x), float(w.y), 20, 20);
            if (hover) tooltip = s.font.split(tr(s, "menu.custom_screen_info.tooltip"), 170);
        } else {
            const ActionButton& b = inst.buttons[size_t(w.button)];
            drawButtonSprite(s, c, float(w.x), float(w.y), float(w.w), float(w.h), b.label, true, hover);
            if (hover && b.hasTooltip) tooltip = s.font.split(b.tooltip, 170);
        }
    }
    if (tooltipItem) s.drawTooltip(c, s.items.tooltip(*tooltipItem, s.settings.advancedItemTooltips), mx, my);
    else if (!tooltip.empty()) s.drawTooltip(c, tooltip, mx, my);
}

// ConfirmScreen: title, message (max width W - 50, centred), optional warning, buttons (spacing 4, 16 above),
// spacing 8, centred on the screen.
void layoutConfirm(UiState& s, std::vector<Widget>& out) {
    out.clear();
    const int W = s.guiW(), H = s.guiH();
    const std::vector<Text> lines = s.font.split(g.confirmMessage, W - 50);
    const bool warning = !g.confirmWarning.empty();
    int bw = 0;
    for (size_t i = 0; i < g.confirmButtons.size(); ++i) bw += g.confirmButtons[i].width + (i ? 4 : 0);
    const int total = 9 + 8 + int(std::min<size_t>(lines.size(), 15)) * 9 + (warning ? 8 + 9 : 0) + 8 + 16 + 20;
    int y = (H - total) / 2;
    Widget t;
    t.kind = Widget::Label, t.text = g.confirmTitle, t.w = s.font.width(g.confirmTitle), t.x = (W - t.w) / 2, t.y = y;
    out.push_back(t);
    y += 9 + 8;
    Widget m;
    m.kind = Widget::Message, m.lines = lines, m.y = y - 4;
    m.w = 0;
    for (const Text& l : lines) m.w = std::max(m.w, s.font.width(l));
    m.x = (W - m.w) / 2;
    out.push_back(m);
    y += int(std::min<size_t>(lines.size(), 15)) * 9;
    if (warning) {
        y += 8;
        Widget wl;
        wl.kind = Widget::Label, wl.text = g.confirmWarning, wl.w = s.font.width(g.confirmWarning), wl.x = (W - wl.w) / 2, wl.y = y;
        out.push_back(wl);
        y += 9;
    }
    y += 8 + 16;
    int x = (W - bw) / 2;
    for (size_t i = 0; i < g.confirmButtons.size(); ++i) {
        Widget b;
        b.kind = Widget::Button, b.button = int(i), b.x = x, b.y = y, b.w = g.confirmButtons[i].width, b.h = 20;
        x += b.w + 4;
        out.push_back(b);
    }
}

void drawConfirm(UiState& s, Canvas& c) {
    menuBackground(s, c);
    layoutConfirm(s, g.confirmWidgets);
    for (const Widget& w : g.confirmWidgets) {
        if (w.kind == Widget::Label) {
            s.drawText(c, w.text, float(w.x), float(w.y), 0xFFFFFFFF, true);
        } else if (w.kind == Widget::Message) {
            float ly = float(w.y) + 4;
            for (size_t i = 0; i < w.lines.size() && i < 15; ++i) {
                s.drawCentered(c, w.lines[i], float(s.guiW() / 2), ly, 0xFFFFFFFF);
                ly += 9;
            }
        } else {
            const bool hover = inside(s.mouseX, s.mouseY, float(w.x), float(w.y), float(w.w), float(w.h));
            drawButtonSprite(s, c, float(w.x), float(w.y), float(w.w), float(w.h), g.confirmButtons[size_t(w.button)].label, true, hover);
        }
    }
}

// WaitingForResponseScreen: the title in a 33 high header, after a second a Back button counting down, active at 5 s.
void drawWaiting(UiState& s, Canvas& c) {
    menuBackground(s, c);
    const int W = s.guiW();
    s.drawCentered(c, tr(s, "gui.waitingForResponse.title"), float(W / 2), float((33 - 9) / 2), 0xFFFFFFFF);
    const int seconds = std::min(g.waitingTicks / 20, 5);
    if (seconds < 1) return;
    const Text label = seconds >= 5 ? tr(s, "gui.back")
                                    : translatable("gui.waitingForResponse.button.inactive", {Text(std::to_string(5 - seconds))}, s.lang());
    const float x = float((W - 200) / 2), y = 33 + 30;
    drawButtonSprite(s, c, x, y, 200, 20, label, seconds >= 5, inside(s.mouseX, s.mouseY, x, y, 200, 20));
}

void draw(UiState& s, Canvas& c) {
    switch (g.mode) {
        case DState::Mode::Dialog:
            if (g.dialog) drawDialog(s, c, *g.dialog);
            break;
        case DState::Mode::Waiting: drawWaiting(s, c); break;
        default: drawConfirm(s, c); break;
    }
}

// --- input -----------------------------------------------------------------------------------------------------------

void pressButton(UiState& s, int index) {  // DialogControlSet.createActionButton: run the action (with the inputs)
    audio::playUiClick();
    std::shared_ptr<Instance> inst = g.dialog;
    if (index == -2) return openWarning(s);
    const ActionButton b = inst->buttons[size_t(index)];
    runAction(s, inst, createAction(*inst, b.action), inst->d->afterAction);
}

void closeDialog(UiState& s) {  // DialogScreen.onClose: the cancel action, then back
    std::shared_ptr<Instance> inst = g.dialog;
    runAction(s, inst, createAction(*inst, inst->d->onCancel()), 0);
}

bool multiKey(UiState& s, InputState& st, const Input& in, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    auto lineCount = [&](const std::string& v) { return int(s.font.split(Text(v), std::max(1, in.width - 8)).size()); };
    if (e.type == E::Char) {
        std::string add;
        appendUtf8(add, u32(e.code));
        if (u32(e.code) < 32 || u32(e.code) == 127 || u32(e.code) == 0xA7) return true;
        if (codepoints(st.multi) + 1 > in.maxLength) return true;
        const std::string next = st.multi + add;
        if (in.maxLines > 0 && lineCount(next) > in.maxLines) return true;
        st.multi = next;
        return true;
    }
    if (e.type != E::Key || !e.down) return false;
    if (e.code == VK_BACK && !st.multi.empty()) {
        size_t i = st.multi.size() - 1;
        while (i > 0 && (u8(st.multi[i]) & 0xC0) == 0x80) --i;
        st.multi.erase(i);
    } else if (e.code == VK_RETURN) {
        if (codepoints(st.multi) + 1 <= in.maxLength && (in.maxLines <= 0 || lineCount(st.multi + "\n ") <= in.maxLines)) st.multi += '\n';
    } else if (e.code == 'V' && s.window->keyDown(VK_CONTROL)) {
        return false;
    }
    return true;
}

void dialogInput(UiState& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    std::shared_ptr<Instance> inst = g.dialog;
    const Dialog& d = *inst->d;
    const Area a = area(s, *inst);
    const float mx = s.mouseX, my = s.mouseY;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) {
        if (d.canCloseWithEscape) closeDialog(s);
        return;
    }
    if (e.type == E::Wheel) {
        if (a.scrollable) inst->scroll = std::clamp(inst->scroll - float(e.code > 0 ? 1 : -1) * 10.0f, 0.0f, maxScroll(s, *inst));
        return;
    }
    if (e.type == E::Button && !e.down && e.code == 0) {
        inst->draggingSlider = -1;
        return;
    }
    if (e.type == E::Button && e.down && e.code == 0) {
        for (const Widget& w : inst->fixed)
            if (w.kind == Widget::Button && inside(mx, my, float(w.x), float(w.y), float(w.w), float(w.h))) return pressButton(s, w.button);
        inst->focus = -1;
        if (!inside(mx, my, float(a.x), float(a.y), float(a.w), float(a.h))) return;
        for (size_t i = 0; i < inst->body.size(); ++i) {
            const Widget& w = inst->body[i];
            const float x = float(a.contentX + w.x), y = float(a.contentY + w.y);
            if (!inside(mx, my, x, y, float(w.w), float(w.h))) continue;
            switch (w.kind) {
                case Widget::Button: return pressButton(s, w.button);
                case Widget::Checkbox:
                    audio::playUiClick();
                    inst->inputs[size_t(w.input)].checked = !inst->inputs[size_t(w.input)].checked;
                    inst->focus = w.input;
                    return;
                case Widget::Cycle: {
                    audio::playUiClick();
                    const int n = int(d.inputs[size_t(w.input)].options.size());
                    int& o = inst->inputs[size_t(w.input)].option;
                    if (n > 0) o = (o + (s.window->keyDown(VK_SHIFT) ? n - 1 : 1)) % n;
                    return;
                }
                case Widget::Slider:
                    inst->draggingSlider = int(i);
                    inst->inputs[size_t(w.input)].slider = std::clamp((mx - (x + 4)) / float(w.w - 8), 0.0f, 1.0f);
                    return;
                case Widget::Edit: {
                    inst->focus = w.input;
                    auto& f = inst->inputs[size_t(w.input)].field;
                    const std::string_view visible = std::string_view(f.value).substr(size_t(std::min<int>(f.scroll, int(f.value.size()))));
                    const std::string head = s.font.headByWidth(visible, int(std::max(0.0f, mx - (x + 4))));
                    f.cursor = f.selection = std::min(int(f.value.size()), f.scroll + int(head.size()));
                    f.blinkStart = s.clock;
                    return;
                }
                case Widget::MultiEdit: inst->focus = w.input; return;
                case Widget::Message: {  // DialogBodyHandlers.runActionOnParent: a clicked component's click event
                    Style st;
                    if (messageStyleAt(s, w, float(a.contentX), float(a.contentY), mx, my, st) && st.events && !st.events->click.empty())
                        runAction(s, inst, clickOf(*st.events), d.afterAction);
                    return;
                }
                default: return;
            }
        }
        return;
    }
    // Typing into the focused text box.
    if (inst->focus >= 0 && size_t(inst->focus) < d.inputs.size() && d.inputs[size_t(inst->focus)].kind == Input::TextBox) {
        const Input& in = d.inputs[size_t(inst->focus)];
        InputState& st = inst->inputs[size_t(inst->focus)];
        if (in.multiline) {
            multiKey(s, st, in, e);
            return;
        }
        if (e.type == E::Char && codepoints(st.field.value) >= in.maxLength && st.field.cursor == st.field.selection) return;
        s.textFieldKey(st.field, e);
    }
}

void confirmInput(UiState& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) {
        if (auto fn = g.confirmEscape) fn();
        return;
    }
    if (e.type != E::Button || !e.down || e.code != 0) return;
    layoutConfirm(s, g.confirmWidgets);
    for (const Widget& w : g.confirmWidgets)
        if (w.kind == Widget::Button && inside(s.mouseX, s.mouseY, float(w.x), float(w.y), float(w.w), float(w.h))) {
            audio::playUiClick();
            auto fn = g.confirmButtons[size_t(w.button)].press;
            if (fn) fn();
            return;
        }
}

void input(UiState& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    switch (g.mode) {
        case DState::Mode::Dialog:
            if (g.dialog) dialogInput(s, e);
            break;
        case DState::Mode::Waiting: {
            const bool active = g.waitingTicks / 20 >= 5;
            const float x = float((s.guiW() - 200) / 2), y = 33 + 30;
            if (active && ((e.type == E::Key && e.down && e.code == VK_ESCAPE) ||
                           (e.type == E::Button && e.down && e.code == 0 && inside(s.mouseX, s.mouseY, x, y, 200, 20)))) {
                if (e.type == E::Button) audio::playUiClick();
                setScreen(s, plain(g.waitingPrevious));
            }
            break;
        }
        default: confirmInput(s, e); break;
    }
}

void build(UiState& s) {
    if (g.mode == DState::Mode::Dialog && g.dialog) {
        Instance& inst = *g.dialog;
        if (inst.inputs.size() != inst.d->inputs.size()) initInputs(inst);
        buildInstance(s, inst);
        inst.scroll = std::clamp(inst.scroll, 0.0f, maxScroll(s, inst));
    }
}

const ScreenRegistrar kScreen({Screen::Dialog, build, draw, input, nullptr});

// --- packets -------------------------------------------------------------------------------------------------------

void clearDialog(UiState& s) {  // ClientCommonPacketListenerImpl.clearDialog
    if (s.screen != Screen::Dialog) return;
    if (g.mode == DState::Mode::Warning) {
        if (g.warningReturn.kind == Target::DialogScreen && g.warningReturn.dialog)
            g.warningReturn = plain(g.warningReturn.dialog->previous);
    } else if (g.mode == DState::Mode::Dialog && g.dialog) {
        setScreen(s, plain(g.dialog->previous));
    }
}

void registerHandlers(UiState& s, net::Client& client) {
    UiState* sp = &s;
    const Lang* lang = &s.res.lang();
    g = DState{};  // a new connection: links and tags arrive in its configuration phase
    // show_dialog: play phase Holder<Dialog> (0: inline, else registry id + 1), configuration phase inline NBT.
    auto show = [sp](std::string nbt, int registryId, bool config) {
        sp->post([sp, nbt, registryId, config] {
            std::shared_ptr<Dialog> d;
            if (registryId >= 0) {
                if (sp->client) {
                    const auto& entries = sp->client->registryEntries("minecraft:dialog");
                    if (size_t(registryId) < entries.size()) d = resolve(*sp, {entries[size_t(registryId)].id, {}});
                }
            } else {
                d = resolve(*sp, {{}, nbt});
            }
            if (!d) return LOG("ui: failed to show dialog");
            LOG("ui: dialog \"%s\" (%s)", d->title.plain().c_str(), d->type.c_str());
            showDialog(*sp, d, current(*sp), config);
        });
    };
    client.onPlay("show_dialog", [show](net::Reader& r) {
        const int holder = r.varInt();
        if (holder == 0) show(std::string(r.nbt()), -1, false);
        else if (r.ok) show({}, holder - 1, false);
    });
    client.onConfig("show_dialog", [show](net::Reader& r) {
        const std::string nbt(r.nbt());
        if (r.ok) show(nbt, -1, true);
    });
    auto clear = [sp](net::Reader&) { sp->post([sp] { clearDialog(*sp); }); };
    client.onPlay("clear_dialog", clear);
    client.onConfig("clear_dialog", clear);
    // ClientboundServerLinksPacket: known types (translated "known_server_link.<name>") or a component, and the URL.
    auto links = [sp, lang](net::Reader& r) {
        static const char* kKnown[] = {"report_bug", "community_guidelines", "support", "status", "feedback",
                                       "community", "website", "forums", "news", "announcements"};
        std::vector<Link> list;
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {
            Link l;
            if (r.boolean()) {
                const int type = r.varInt();
                l.name = translatable(std::string("known_server_link.") + kKnown[std::clamp(type, 0, 9)], {}, lang);
            } else {
                l.name = parseComponent(r.nbt(), lang);
            }
            l.url = std::string(r.str());
            if (l.url.starts_with("http://") || l.url.starts_with("https://")) list.push_back(std::move(l));
            else LOG("ui: received invalid link %s", l.url.c_str());
        }
        if (r.ok) sp->post([list] { g.links = list; });
    };
    client.onPlay("server_links", links);
    client.onConfig("server_links", links);
    // update_tags: the minecraft:dialog tags (pause_screen_additions, quick_actions).
    auto tags = [sp](net::Reader& r) {
        const i32 registries = r.varInt();
        std::map<std::string, std::vector<int>> dialogTags;
        bool seen = false;
        for (i32 i = 0; i < registries && r.ok; ++i) {
            const std::string registry(r.str());
            const i32 count = r.varInt();
            for (i32 t = 0; t < count && r.ok; ++t) {
                const std::string tag(r.str());
                std::vector<int> ids;
                const i32 n = r.varInt();
                for (i32 k = 0; k < n && r.ok; ++k) ids.push_back(r.varInt());
                if (registry == "minecraft:dialog") dialogTags[tag] = std::move(ids), seen = true;
            }
        }
        if (r.ok && seen) sp->post([dialogTags] { g.dialogTags = dialogTags; });
    };
    client.onPlay("update_tags", tags);
    client.onConfig("update_tags", tags);
}

void tick(UiState& s) {
    if (s.screen == Screen::Dialog && g.mode == DState::Mode::Waiting) ++g.waitingTicks;
}

bool worldKey(UiState& s, const Window::InputEvent& e) {  // Minecraft.handleKeybinds: keyQuickActions
    if (!s.online() || !s.isKey(Key::QuickActions, e) || e.repeat) return false;
    const std::vector<DialogRef> members = tagMembers(s, "minecraft:quick_actions");
    if (members.empty()) return true;
    const std::shared_ptr<Dialog> d = resolve(s, members.size() == 1 ? members[0] : DialogRef{"minecraft:quick_actions", {}});
    if (d) showDialog(s, d, current(s));
    return true;
}

const UiModuleRegistrar kModule({.name = "dialogs", .registerHandlers = registerHandlers, .tick = tick,
                                 .worldKey = worldKey});

}  // namespace

void handleGameClick(UiState& s, const ClickAction& click, Screen back) { handleClick(s, click, plain(back), false); }

bool dialogPausesGame() {
    if (g.mode == DState::Mode::Dialog) return g.dialog && g.dialog->d->pause;
    return g.mode != DState::Mode::Waiting;  // WaitingForResponseScreen does not pause, confirmations do
}

bool pauseMenuDialogButton(UiState& s, Text& label, std::function<void()>& open) {
    if (!s.online()) return false;
    std::shared_ptr<Dialog> d;
    const std::vector<DialogRef> additions = tagMembers(s, "minecraft:pause_screen_additions");
    if (!additions.empty()) d = resolve(s, additions.size() == 1 ? additions[0] : DialogRef{"minecraft:custom_options", {}});
    else if (!g.links.empty()) d = resolve(s, {"minecraft:server_links", {}});
    if (!d) return false;
    label = d->externalTitleOrTitle();
    UiState* sp = &s;
    open = [sp, d] { showDialog(*sp, d, plain(Screen::Pause)); };
    return true;
}

}  // namespace ui
