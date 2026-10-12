#include "ui/text.h"

#include "world/nbt.h"

namespace ui {

const u32 kLegacyColors[16] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xFFAA00, 0xAAAAAA,
                               0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};
namespace {
const char* kColorNames[16] = {"black", "dark_blue", "dark_green", "dark_aqua", "dark_red", "dark_purple",
                               "gold",  "gray",      "dark_gray",  "blue",      "green",    "aqua",
                               "red",   "light_purple", "yellow", "white"};

struct Node {
    enum Kind { Literal, Translate, Keybind } kind = Literal;
    std::string text, key, fallback;
    std::vector<Node> with, extra;
    Style style;
    // click_event / hover_event / insertion (turned into Style::events when flattened)
    std::string click, clickValue, clickNbt, hover, hoverId, hoverUuid, insertion;
    int hoverCount = 1;
    bool hasEvents = false, hasInsertion = false;
    std::vector<Node> hoverValue;  // show_text value / show_entity name (0 or 1)
};

std::string numberText(double v, bool integral) {
    if (integral) return std::to_string(i64(v));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

i8 flag(NbtReader& r, u8 tag) {
    if (tag == TagByte) return i8(r.u8v() != 0);
    r.skip(tag);
    return -1;
}

Node parseNbt(NbtReader& r, u8 tag, int depth);

void parseList(NbtReader& r, std::vector<Node>& out, int depth) {
    const u8 et = r.u8v();
    const i32 n = r.i32v();
    for (i32 i = 0; i < n && r.ok; ++i) out.push_back(parseNbt(r, et, depth + 1));
}

Node parseNbt(NbtReader& r, u8 tag, int depth) {
    Node n;
    if (depth > 64) {
        r.skip(tag);
        return n;
    }
    switch (tag) {
        case TagString: n.text = std::string(r.str()); return n;
        case TagByte: n.text = numberText(i8(r.u8v()), true); return n;
        case TagShort: n.text = numberText(r.i16v(), true); return n;
        case TagInt: n.text = numberText(r.i32v(), true); return n;
        case TagLong: n.text = std::to_string(r.i64v()); return n;
        case TagFloat: n.text = numberText(r.f32v(), false); return n;
        case TagDouble: n.text = numberText(r.f64v(), false); return n;
        case TagList: {
            std::vector<Node> items;
            parseList(r, items, depth);
            if (items.empty()) return n;
            n = std::move(items[0]);
            for (size_t i = 1; i < items.size(); ++i) n.extra.push_back(std::move(items[i]));
            return n;
        }
        case TagCompound: break;
        default: r.skip(tag); return n;
    }
    u8 t;
    std::string_view name;
    while (r.field(t, name)) {
        if ((name == "text" || name.empty()) && t != TagCompound && t != TagList) {
            n.text = parseNbt(r, t, depth + 1).text;
        } else if (name == "translate" && t == TagString) {
            n.kind = Node::Translate;
            n.key = r.str();
        } else if (name == "fallback" && t == TagString) {
            n.fallback = r.str();
        } else if (name == "keybind" && t == TagString) {
            n.kind = Node::Keybind;
            n.key = r.str();
        } else if (name == "with" && t == TagList) {
            parseList(r, n.with, depth);
        } else if (name == "extra" && t == TagList) {
            parseList(r, n.extra, depth);
        } else if (name == "selector" && t == TagString) {
            n.text = r.str();
        } else if (name == "score" && t == TagCompound) {
            u8 st;
            std::string_view sn;
            while (r.field(st, sn)) {
                if (sn == "name" && st == TagString) n.text = r.str();
                else r.skip(st);
            }
        } else if (name == "color" && t == TagString) {
            n.style.color = colorByName(r.str());
        } else if (name == "shadow_color" && t == TagInt) {
            n.style.shadow = i64(u32(r.i32v()));
        } else if (name == "shadow_color" && t == TagList) {
            const u8 et = r.u8v();
            const i32 cnt = r.i32v();
            float f[4] = {0, 0, 0, 1};
            for (i32 i = 0; i < cnt && r.ok; ++i) {
                const float v = et == TagFloat ? r.f32v() : et == TagDouble ? float(r.f64v()) : (r.skip(et), 0.0f);
                if (i < 4) f[i] = v;
            }
            auto c = [](float v) { return u32(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
            n.style.shadow = i64((c(f[3]) << 24) | (c(f[0]) << 16) | (c(f[1]) << 8) | c(f[2]));
        } else if (name == "bold") {
            n.style.bold = flag(r, t);
        } else if (name == "italic") {
            n.style.italic = flag(r, t);
        } else if (name == "underlined") {
            n.style.underlined = flag(r, t);
        } else if (name == "strikethrough") {
            n.style.strikethrough = flag(r, t);
        } else if (name == "obfuscated") {
            n.style.obfuscated = flag(r, t);
        } else if (name == "insertion" && t == TagString) {
            n.insertion = r.str(), n.hasInsertion = n.hasEvents = true;
        } else if ((name == "click_event" || name == "hover_event") && t == TagCompound) {
            const bool click = name == "click_event";
            u8 et;
            std::string_view en;
            while (r.field(et, en)) {
                if (en == "action" && et == TagString) {
                    (click ? n.click : n.hover) = r.str();
                } else if (click && (en == "url" || en == "command" || en == "value" || en == "path") && et == TagString) {
                    n.clickValue = r.str();
                } else if (click && en == "page" && et != TagCompound && et != TagList) {
                    n.clickValue = parseNbt(r, et, depth + 1).text;
                } else if (click && (en == "dialog" || en == "payload")) {  // show_dialog / custom: kept as NBT
                    const u8* start = r.p;
                    r.skip(et);
                    n.clickNbt = std::string(1, char(et)) + std::string(reinterpret_cast<const char*>(start), size_t(r.p - start));
                } else if (click && en == "id" && et == TagString) {  // custom
                    n.clickValue = r.str();
                } else if (!click && (en == "value" || en == "name")) {
                    n.hoverValue.clear();
                    n.hoverValue.push_back(parseNbt(r, et, depth + 1));
                } else if (!click && en == "id" && et == TagString) {
                    n.hoverId = r.str();
                } else if (!click && en == "count" && et == TagInt) {
                    n.hoverCount = r.i32v();
                } else if (!click && en == "uuid" && et == TagIntArray) {
                    const i32 cnt = r.i32v();
                    std::string hex;
                    for (i32 i = 0; i < cnt && r.ok; ++i) {
                        char b[9];
                        std::snprintf(b, sizeof(b), "%08x", u32(r.i32v()));
                        hex += b;
                    }
                    if (hex.size() == 32)
                        n.hoverUuid = hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20);
                } else if (!click && en == "uuid" && et == TagString) {
                    n.hoverUuid = r.str();
                } else {
                    r.skip(et);
                }
            }
            n.hasEvents = true;
        } else {
            r.skip(t);
        }
    }
    return n;
}

i8 jflag(const nlohmann::json& j, const char* k) {
    if (!j.contains(k)) return -1;
    const auto& v = j[k];
    return v.is_boolean() ? i8(v.get<bool>()) : v.is_number() ? i8(v.get<int>() != 0) : -1;
}

Node parseJson(const nlohmann::json& j, int depth) {
    Node n;
    if (depth > 64) return n;
    if (j.is_string()) {
        n.text = j.get<std::string>();
        return n;
    }
    if (j.is_number_integer()) return n.text = std::to_string(j.get<i64>()), n;
    if (j.is_number()) return n.text = numberText(j.get<double>(), false), n;
    if (j.is_boolean()) return n.text = j.get<bool>() ? "true" : "false", n;
    if (j.is_array()) {
        if (j.empty()) return n;
        n = parseJson(j[0], depth + 1);
        for (size_t i = 1; i < j.size(); ++i) n.extra.push_back(parseJson(j[i], depth + 1));
        return n;
    }
    if (!j.is_object()) return n;
    if (j.contains("text")) n.text = parseJson(j["text"], depth + 1).text;
    if (j.contains("translate") && j["translate"].is_string()) {
        n.kind = Node::Translate;
        n.key = j["translate"].get<std::string>();
        n.fallback = j.value("fallback", "");
        if (j.contains("with") && j["with"].is_array())
            for (const auto& a : j["with"]) n.with.push_back(parseJson(a, depth + 1));
    }
    if (j.contains("keybind") && j["keybind"].is_string()) n.kind = Node::Keybind, n.key = j["keybind"].get<std::string>();
    if (j.contains("selector") && j["selector"].is_string()) n.text = j["selector"].get<std::string>();
    if (j.contains("extra") && j["extra"].is_array())
        for (const auto& e : j["extra"]) n.extra.push_back(parseJson(e, depth + 1));
    if (j.contains("color") && j["color"].is_string()) n.style.color = colorByName(j["color"].get<std::string>());
    if (j.contains("shadow_color") && j["shadow_color"].is_number_integer())
        n.style.shadow = i64(u32(j["shadow_color"].get<i64>()));
    n.style.bold = jflag(j, "bold");
    n.style.italic = jflag(j, "italic");
    n.style.underlined = jflag(j, "underlined");
    n.style.strikethrough = jflag(j, "strikethrough");
    n.style.obfuscated = jflag(j, "obfuscated");
    if (j.contains("insertion") && j["insertion"].is_string()) n.insertion = j["insertion"].get<std::string>(), n.hasInsertion = n.hasEvents = true;
    if (j.contains("click_event") && j["click_event"].is_object()) {
        const auto& e = j["click_event"];
        n.click = e.value("action", "");
        for (const char* k : {"url", "command", "value", "path"})
            if (e.contains(k) && e[k].is_string()) n.clickValue = e[k].get<std::string>();
        if (e.contains("page") && e["page"].is_number_integer()) n.clickValue = std::to_string(e["page"].get<int>());
        n.hasEvents = true;
    }
    if (j.contains("hover_event") && j["hover_event"].is_object()) {
        const auto& e = j["hover_event"];
        n.hover = e.value("action", "");
        if (e.contains("value")) n.hoverValue.push_back(parseJson(e["value"], depth + 1));
        else if (e.contains("name")) n.hoverValue.push_back(parseJson(e["name"], depth + 1));
        if (e.contains("id") && e["id"].is_string()) n.hoverId = e["id"].get<std::string>();
        if (e.contains("count") && e["count"].is_number_integer()) n.hoverCount = e["count"].get<int>();
        if (e.contains("uuid") && e["uuid"].is_string()) n.hoverUuid = e["uuid"].get<std::string>();
        n.hasEvents = true;
    }
    return n;
}

void flatten(const Node& n, const Style& parent, const Lang* lang, Text& out, int depth);

// TranslatableContents.decompose: literal parts of the pattern in the component's style, %s arguments
// as their own components (inheriting it).
void translate(const Node& n, const Style& style, const Lang* lang, Text& out, int depth) {
    std::string fmt;
    if (lang)
        if (auto it = lang->find(n.key); it != lang->end()) fmt = it->second;
    if (fmt.empty()) fmt = n.fallback.empty() ? n.key : n.fallback;
    size_t next = 0;
    std::string lit;
    auto flush = [&] {
        if (!lit.empty()) out.spans.push_back({std::move(lit), style});
        lit.clear();
    };
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%' || i + 1 >= fmt.size()) {
            lit += fmt[i];
            continue;
        }
        if (fmt[i + 1] == '%') {
            lit += '%';
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9') ++j;
        size_t arg = next;
        bool explicitIndex = false;
        if (j < fmt.size() && fmt[j] == '$' && j > i + 1) {
            arg = size_t(std::atoi(fmt.c_str() + i + 1) - 1);
            explicitIndex = true;
            ++j;
        }
        if (j < fmt.size() && (fmt[j] == 's' || fmt[j] == 'd') && (explicitIndex || j == i + 1)) {
            if (!explicitIndex) ++next;
            flush();
            if (arg < n.with.size()) flatten(n.with[arg], style, lang, out, depth + 1);
            i = j;
            continue;
        }
        lit += fmt[i];
    }
    flush();
}

void flatten(const Node& n, const Style& parent, const Lang* lang, Text& out, int depth) {
    if (depth > 64) return;
    Style own = n.style;
    if (n.hasEvents) {  // Style.applyTo: unset click / hover / insertion come from the parent
        auto ev = std::make_shared<TextEvents>(parent.events ? *parent.events : TextEvents{});
        if (!n.click.empty()) ev->click = n.click, ev->clickValue = n.clickValue, ev->clickNbt = n.clickNbt;
        if (!n.hover.empty()) {
            ev->hover = n.hover, ev->hoverId = n.hoverId, ev->hoverCount = n.hoverCount, ev->hoverUuid = n.hoverUuid;
            ev->hoverText = Text();
            if (!n.hoverValue.empty()) flatten(n.hoverValue[0], Style{}, lang, ev->hoverText, depth + 1);
        }
        if (n.hasInsertion) ev->insertion = n.insertion, ev->hasInsertion = true;
        own.events = std::move(ev);
    }
    const Style style = own.over(parent);
    if (n.kind == Node::Translate) {
        translate(n, style, lang, out, depth);
    } else if (n.kind == Node::Keybind) {
        std::string s = n.key;
        if (lang)
            if (auto it = lang->find(n.key); it != lang->end()) s = it->second;
        out.spans.push_back({s, style});
    } else if (!n.text.empty()) {
        out.spans.push_back({n.text, style});
    }
    for (const Node& e : n.extra) flatten(e, style, lang, out, depth + 1);
}
}  // namespace

Style Style::over(const Style& p) const {
    Style s = *this;
    if (s.color < 0) s.color = p.color;
    if (s.shadow < 0) s.shadow = p.shadow;
    if (s.bold < 0) s.bold = p.bold;
    if (s.italic < 0) s.italic = p.italic;
    if (s.underlined < 0) s.underlined = p.underlined;
    if (s.strikethrough < 0) s.strikethrough = p.strikethrough;
    if (s.obfuscated < 0) s.obfuscated = p.obfuscated;
    if (!s.events) s.events = p.events;
    return s;
}

bool Style::applyLegacy(char code, const Style& reset) {
    const char c = char(std::tolower(u8(code)));
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
        bold = italic = underlined = strikethrough = obfuscated = 0;
        color = i32(kLegacyColors[c <= '9' ? c - '0' : c - 'a' + 10]);
    } else if (c == 'k') obfuscated = 1;
    else if (c == 'l') bold = 1;
    else if (c == 'm') strikethrough = 1;
    else if (c == 'n') underlined = 1;
    else if (c == 'o') italic = 1;
    else if (c == 'r') *this = reset;
    else return false;
    return true;
}

bool Text::empty() const {
    for (const Span& s : spans)
        if (!s.text.empty()) return false;
    return true;
}

std::string Text::plain() const {
    std::string out;
    for (const Span& s : spans)
        for (size_t i = 0; i < s.text.size(); ++i) {
            if (u8(s.text[i]) == 0xC2 && i + 1 < s.text.size() && u8(s.text[i + 1]) == 0xA7) {
                i += 2;  // section sign + code
                continue;
            }
            out += s.text[i];
        }
    return out;
}

Text& Text::append(const Text& t, const Style* parent) {
    for (const Span& s : t.spans) spans.push_back({s.text, parent ? s.style.over(*parent) : s.style});
    return *this;
}

Text Text::withStyle(const Style& base) const {
    Text t;
    for (const Span& s : spans) t.spans.push_back({s.text, s.style.over(base)});
    return t;
}

bool Text::operator==(const Text& o) const {
    if (spans.size() != o.spans.size()) return false;
    for (size_t i = 0; i < spans.size(); ++i)
        if (spans[i].text != o.spans[i].text || !(spans[i].style == o.spans[i].style)) return false;
    return true;
}

i32 colorByName(std::string_view name) {
    if (name.size() == 7 && name[0] == '#') return i32(std::strtoul(std::string(name.substr(1)).c_str(), nullptr, 16));
    for (int i = 0; i < 16; ++i)
        if (name == kColorNames[i]) return i32(kLegacyColors[i]);
    return -1;
}

Style colorStyle(u32 rgb) {
    Style s;
    s.color = i32(rgb & 0xFFFFFF);
    return s;
}

Text parseComponent(std::string_view nbt, const Lang* lang) {
    Text out;
    if (nbt.empty()) return out;
    NbtReader r(reinterpret_cast<const u8*>(nbt.data()) + 1, nbt.size() - 1);
    const Node n = parseNbt(r, u8(nbt[0]), 0);
    flatten(n, Style{}, lang, out, 0);
    return out;
}

Style parseStyle(std::string_view nbt, const Lang* lang) {
    if (nbt.empty()) return {};
    NbtReader r(reinterpret_cast<const u8*>(nbt.data()) + 1, nbt.size() - 1);
    Node n = parseNbt(r, u8(nbt[0]), 0);
    n.kind = Node::Literal, n.text = "\x01", n.extra.clear();
    Text t;
    flatten(n, Style{}, lang, t, 0);
    return t.spans.empty() ? Style{} : t.spans[0].style;
}

Text parseJsonComponent(const nlohmann::json& j, const Lang* lang) {
    Text out;
    flatten(parseJson(j, 0), Style{}, lang, out, 0);
    return out;
}

Text translatable(const std::string& key, const std::vector<Text>& args, const Lang* lang, Style style) {
    Node n;
    n.kind = Node::Translate;
    n.key = key;
    n.style = style;
    // Arguments are already flattened: render them as placeholders, then splice.
    for (size_t i = 0; i < args.size(); ++i) {
        Node a;
        a.text = "\x01" + std::to_string(i) + "\x01";
        n.with.push_back(a);
    }
    Text tmp, out;
    flatten(n, Style{}, lang, tmp, 0);
    for (const Span& s : tmp.spans) {
        if (s.text.size() >= 3 && s.text.front() == '\x01' && s.text.back() == '\x01') {
            const size_t idx = size_t(std::atoi(s.text.c_str() + 1));
            if (idx < args.size()) out.append(args[idx], &s.style);
            continue;
        }
        out.spans.push_back(s);
    }
    return out;
}

std::string formatJava(const std::string& fmt, const std::vector<std::string>& args) {
    std::vector<Text> targs;
    for (const auto& a : args) targs.emplace_back(a);
    Lang lang{{"\x02", fmt}};
    return translatable("\x02", targs, &lang).plain();
}

u32 nextCodepoint(std::string_view s, size_t& i) {
    const u8 c = u8(s[i++]);
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (extra < 0) return 0xFFFD;
    u32 cp = c & (0x3F >> extra);
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size() || (u8(s[i]) & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (u8(s[i++]) & 0x3F);
    }
    return cp;
}

void appendUtf8(std::string& s, u32 cp) {
    if (cp < 0x80) {
        s += char(cp);
    } else if (cp < 0x800) {
        s += char(0xC0 | (cp >> 6)), s += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += char(0xE0 | (cp >> 12)), s += char(0x80 | ((cp >> 6) & 0x3F)), s += char(0x80 | (cp & 0x3F));
    } else {
        s += char(0xF0 | (cp >> 18)), s += char(0x80 | ((cp >> 12) & 0x3F)), s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    }
}

}  // namespace ui
