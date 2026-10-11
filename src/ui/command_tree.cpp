#include "ui/command_tree.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <set>

namespace ui::cmd {

bool decode(net::Reader& r, const std::vector<std::string>& argTypes, Tree& out) {
    out = Tree{};
    const i32 count = r.varInt();
    if (!r.ok || count <= 0 || count > 1 << 20) return false;
    out.nodes.resize(size_t(count));
    for (Node& n : out.nodes) {
        const u8 flags = r.u8v();
        n.type = flags & 3;
        n.executable = (flags & 4) != 0;
        const i32 children = r.varInt();
        for (i32 i = 0; i < children && r.ok; ++i) n.children.push_back(r.varInt());
        if (flags & 8) n.redirect = r.varInt();
        if (n.type == 1) {
            n.name = std::string(r.str());
        } else if (n.type == 2) {
            n.name = std::string(r.str());
            const i32 id = r.varInt();
            if (id < 0 || size_t(id) >= argTypes.size()) return false;
            n.parser = argTypes[size_t(id)];
            const std::string& p = n.parser;
            if (p == "brigadier:float" || p == "brigadier:double" || p == "brigadier:integer" || p == "brigadier:long") {
                n.numFlags = r.u8v();
                auto num = [&]() -> double {
                    if (p == "brigadier:float") return r.f32();
                    if (p == "brigadier:double") return r.f64();
                    if (p == "brigadier:integer") return r.i32v();
                    return double(r.i64v());
                };
                if (n.numFlags & 1) n.min = num();
                if (n.numFlags & 2) n.max = num();
            } else if (p == "brigadier:string") {
                n.stringKind = r.varInt();
            } else if (p == "minecraft:entity" || p == "minecraft:score_holder") {
                r.u8v();
            } else if (p == "minecraft:time") {
                n.min = r.i32v();
            } else if (p == "minecraft:resource_or_tag" || p == "minecraft:resource_or_tag_key" || p == "minecraft:resource" ||
                       p == "minecraft:resource_key" || p == "minecraft:resource_selector") {
                r.str();
            }
            if (flags & 16) n.suggestions = std::string(r.str());
        }
        if (!r.ok) return false;
    }
    out.root = r.varInt();
    if (!r.ok || !out.valid()) return false;
    for (const Node& n : out.nodes) {
        for (int c : n.children)
            if (c < 0 || c >= count) return false;
        if (n.redirect >= count) return false;
    }
    return true;
}

namespace {

// com.mojang.brigadier.StringReader
struct Reader {
    const std::string& s;
    int cursor = 0;
    bool canRead(int n = 1) const { return cursor + n <= int(s.size()); }
    char peek(int o = 0) const { return s[size_t(cursor + o)]; }
    void skip() { ++cursor; }
    int remaining() const { return int(s.size()) - cursor; }
    static bool allowedNumber(char c) { return (c >= '0' && c <= '9') || c == '.' || c == '-'; }
    static bool quote(char c) { return c == '"' || c == '\''; }
    static bool unquoted(char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.' ||
               c == '+';
    }
};

Error ctxError(const Reader& r, std::string key, std::vector<std::string> args = {}) {
    Error e;
    e.key = std::move(key);
    e.args = std::move(args);
    e.hasContext = true;
    e.input = r.s;
    e.cursor = r.cursor;
    return e;
}

std::string javaDouble(double v) {  // close to Double.toString for the bounds in messages
    if (v == std::floor(v) && std::abs(v) < 1e7) {
        char b[32];
        std::snprintf(b, sizeof(b), "%.1f", v);
        return b;
    }
    char b[32];
    std::snprintf(b, sizeof(b), "%g", v);
    return b;
}

std::optional<Error> readNumber(Reader& r, bool integer, const char* kind, double& out) {
    const int start = r.cursor;
    while (r.canRead() && Reader::allowedNumber(r.peek())) r.skip();
    const std::string number = r.s.substr(size_t(start), size_t(r.cursor - start));
    if (number.empty()) return ctxError(r, std::string("parsing.") + kind + ".expected");
    char* end = nullptr;
    const double v = integer ? double(std::strtoll(number.c_str(), &end, 10)) : std::strtod(number.c_str(), &end);
    bool bad = end != number.c_str() + number.size();
    if (!integer && number.find_first_of("0123456789") == std::string::npos) bad = true;
    if (bad) {
        r.cursor = start;
        return ctxError(r, std::string("parsing.") + kind + ".invalid", {number});
    }
    out = v;
    return std::nullopt;
}

std::optional<Error> readStringUntil(Reader& r, char terminator, std::string& out) {
    bool escaped = false;
    while (r.canRead()) {
        const char c = r.peek();
        r.skip();
        if (escaped) {
            if (c == terminator || c == '\\') {
                out += c;
                escaped = false;
            } else {
                r.cursor -= 1;
                return ctxError(r, "parsing.quote.escape", {std::string(1, c)});
            }
        } else if (c == '\\') {
            escaped = true;
        } else if (c == terminator) {
            return std::nullopt;
        } else {
            out += c;
        }
    }
    return ctxError(r, "parsing.quote.expected.end");
}

std::optional<Error> readString(Reader& r, std::string& out) {
    out.clear();
    if (!r.canRead()) return std::nullopt;
    const char next = r.peek();
    if (Reader::quote(next)) {
        r.skip();
        return readStringUntil(r, next, out);
    }
    const int start = r.cursor;
    while (r.canRead() && Reader::unquoted(r.peek())) r.skip();
    out = r.s.substr(size_t(start), size_t(r.cursor - start));
    return std::nullopt;
}

// A game argument by its shape: up to the next space outside brackets / braces / quotes.
void scanToken(Reader& r) {
    int depth = 0;
    char q = 0;
    while (r.canRead()) {
        const char c = r.peek();
        if (q) {
            if (c == '\\' && r.canRead(2)) r.skip();
            else if (c == q) q = 0;
            r.skip();
            continue;
        }
        if (Reader::quote(c)) q = c;
        else if (c == '[' || c == '{' || c == '(') ++depth;
        else if (c == ']' || c == '}' || c == ')') depth = std::max(depth - 1, 0);
        else if (c == ' ' && depth == 0) break;
        r.skip();
    }
}

// WorldCoordinates / LocalCoordinates: `count` space-separated coordinates (~ / ^ prefixes).
std::optional<Error> parseCoordinates(Reader& r, int count, bool ints) {
    const int start = r.cursor;
    const char* incomplete = count == 3 ? "argument.pos3d.incomplete" : "argument.pos2d.incomplete";
    const bool local = r.canRead() && r.peek() == '^';
    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            if (!(r.canRead() && r.peek() == ' ')) {
                r.cursor = start;
                return ctxError(r, incomplete);
            }
            r.skip();
        }
        if (!r.canRead()) return ctxError(r, "argument.pos.missing.double");
        const char c = r.peek();
        if (local != (c == '^')) return ctxError(r, "argument.pos.mixed");
        const bool relative = c == '~' || c == '^';
        if (relative) r.skip();
        if (r.canRead() && r.peek() != ' ') {
            double v;
            if (auto e = readNumber(r, ints && !relative, ints && !relative ? "int" : "double", v)) return e;
        }
    }
    return std::nullopt;
}

std::optional<Error> parseArgument(const Node& n, Reader& r) {
    const std::string& p = n.parser;
    auto bounds = [&](double v, const char* kind) -> std::optional<Error> {
        const bool integer = std::string_view(kind) == "integer" || std::string_view(kind) == "long";
        auto str = [&](double x) { return integer ? std::to_string(i64(x)) : javaDouble(x); };
        if ((n.numFlags & 1) && v < n.min) return ctxError(r, std::string("argument.") + kind + ".low", {str(n.min), str(v)});
        if ((n.numFlags & 2) && v > n.max) return ctxError(r, std::string("argument.") + kind + ".big", {str(n.max), str(v)});
        return std::nullopt;
    };
    if (p == "brigadier:bool") {
        const int start = r.cursor;
        std::string v;
        if (auto e = readString(r, v)) return e;
        if (v.empty()) return ctxError(r, "parsing.bool.expected");
        if (v != "true" && v != "false") {
            r.cursor = start;
            return ctxError(r, "parsing.bool.invalid", {v});
        }
        return std::nullopt;
    }
    if (p == "brigadier:integer" || p == "brigadier:long" || p == "brigadier:float" || p == "brigadier:double") {
        const bool integer = p == "brigadier:integer" || p == "brigadier:long";
        const char* kind = p == "brigadier:integer" ? "integer" : p == "brigadier:long" ? "long" : p == "brigadier:float" ? "float" : "double";
        const char* readerKind = p == "brigadier:integer" ? "int" : p == "brigadier:long" ? "long" : p == "brigadier:float" ? "float" : "double";
        const int start = r.cursor;
        double v;
        if (auto e = readNumber(r, integer, readerKind, v)) return e;
        if (auto e = bounds(v, kind)) {
            r.cursor = start;
            e->cursor = start;
            return e;
        }
        return std::nullopt;
    }
    if (p == "brigadier:string") {
        if (n.stringKind == 2) {
            r.cursor = int(r.s.size());
            return std::nullopt;
        }
        if (n.stringKind == 1) {
            std::string v;
            return readString(r, v);
        }
        while (r.canRead() && Reader::unquoted(r.peek())) r.skip();
        return std::nullopt;
    }
    if (p == "minecraft:message") {
        r.cursor = int(r.s.size());
        return std::nullopt;
    }
    if (p == "minecraft:time") {  // TimeArgument: a float with an optional unit (d, s, t)
        double v;
        if (auto e = readNumber(r, false, "float", v)) return e;
        const int unitStart = r.cursor;
        while (r.canRead() && Reader::unquoted(r.peek())) r.skip();
        const std::string unit = r.s.substr(size_t(unitStart), size_t(r.cursor - unitStart));
        const int factor = unit.empty() || unit == "t" ? 1 : unit == "s" ? 20 : unit == "d" ? 24000 : 0;
        if (factor == 0) return ctxError(r, "argument.time.invalid_unit");
        const int ticks = int(std::lround(v * factor));
        if (ticks < int(n.min)) return ctxError(r, "argument.time.tick_count_too_low", {std::to_string(int(n.min)), std::to_string(ticks)});
        return std::nullopt;
    }
    if (p == "minecraft:angle") {  // AngleArgument: one (relative) float
        if (!r.canRead()) return ctxError(r, "argument.angle.incomplete");
        if (r.peek() == '~') r.skip();
        double v;
        if (r.canRead() && r.peek() != ' ')
            if (auto e = readNumber(r, false, "float", v)) return e;
        return std::nullopt;
    }
    if (p == "minecraft:block_pos" || p == "minecraft:vec3") return parseCoordinates(r, 3, p == "minecraft:block_pos");
    if (p == "minecraft:vec2" || p == "minecraft:rotation" || p == "minecraft:column_pos")
        return parseCoordinates(r, 2, p == "minecraft:column_pos");
    const int start = r.cursor;
    scanToken(r);
    if (r.cursor == start) {
        if (p == "minecraft:entity" || p == "minecraft:game_profile" || p == "minecraft:score_holder")
            return ctxError(r, "argument.entity.invalid");
        return ctxError(r, "command.unknown.argument");
    }
    return std::nullopt;
}

// LiteralCommandNode.parse
bool parseLiteral(const Node& n, Reader& r) {
    const int len = int(n.name.size());
    if (r.canRead(len) && r.s.compare(size_t(r.cursor), size_t(len), n.name) == 0) {
        const int end = r.cursor + len;
        if (end >= int(r.s.size()) || r.s[size_t(end)] == ' ') {
            r.cursor = end;
            return true;
        }
    }
    return false;
}

void withNode(Context& c, int node, Range range) {
    c.nodes.push_back({node, range});
    c.range = {std::min(c.range.start, range.start), std::max(c.range.end, range.end)};
}

// CommandDispatcher.parseNodes
Parse parseNodes(const Tree& t, int node, const Reader& original, const Context& soFar, int depth) {
    std::vector<std::pair<int, Error>> errors;
    std::vector<Parse> potentials;
    const int cursor = original.cursor;
    if (depth > 64) return {std::make_shared<Context>(soFar), original.s, cursor, {}};
    // CommandNode.getRelevantNodes: a literal matching the next word, else the arguments.
    std::vector<int> relevant;
    bool literalMatched = false;
    {
        Reader rr = original;
        while (rr.canRead() && rr.peek() != ' ') rr.skip();
        const std::string word = original.s.substr(size_t(cursor), size_t(rr.cursor - cursor));
        bool anyLiteral = false;
        for (int c : t.nodes[size_t(node)].children) {
            if (t.nodes[size_t(c)].type != 1) continue;
            anyLiteral = true;
            if (t.nodes[size_t(c)].name == word) relevant = {c}, literalMatched = true;
        }
        if (!literalMatched || !anyLiteral)
            for (int c : t.nodes[size_t(node)].children)
                if (t.nodes[size_t(c)].type == 2) relevant.push_back(c);
    }
    for (int child : relevant) {
        const Node& cn = t.nodes[size_t(child)];
        Context ctx = soFar;
        Reader reader = original;
        const int start = reader.cursor;
        std::optional<Error> err;
        if (cn.type == 1) {
            if (!parseLiteral(cn, reader)) {
                Error e = ctxError(reader, "argument.literal.incorrect", {cn.name});
                e.literalIncorrect = true;
                err = e;
            }
        } else {
            err = parseArgument(cn, reader);
        }
        if (!err && reader.canRead() && reader.peek() != ' ') err = ctxError(reader, "command.expected.separator");
        if (err) {
            errors.push_back({child, *err});
            continue;
        }
        const Range range{start, reader.cursor};
        withNode(ctx, child, range);
        if (cn.type == 2) ctx.args.push_back({cn.name, range});
        if (reader.canRead(cn.redirect < 0 ? 2 : 1)) {
            reader.skip();
            if (cn.redirect >= 0) {
                Context childCtx;
                childCtx.rootNode = cn.redirect;
                childCtx.range = {reader.cursor, reader.cursor};
                Parse parse = parseNodes(t, cn.redirect, reader, childCtx, depth + 1);
                ctx.child = parse.ctx;
                return {std::make_shared<Context>(ctx), parse.input, parse.cursor, parse.exceptions};
            }
            potentials.push_back(parseNodes(t, child, reader, ctx, depth + 1));
        } else {
            potentials.push_back({std::make_shared<Context>(ctx), reader.s, reader.cursor, {}});
        }
    }
    if (!potentials.empty()) {
        std::stable_sort(potentials.begin(), potentials.end(), [](const Parse& a, const Parse& b) {
            if (!a.canRead() && b.canRead()) return true;
            if (a.canRead() && !b.canRead()) return false;
            if (a.exceptions.empty() && !b.exceptions.empty()) return true;
            return false;
        });
        return potentials.front();
    }
    return {std::make_shared<Context>(soFar), original.s, cursor, errors};
}

struct SuggestionContext {
    int parent;
    int startPos;
};

// CommandContextBuilder.findSuggestionContext
SuggestionContext findSuggestionContext(const Context& c, int cursor) {
    if (c.range.start <= cursor) {
        if (c.range.end < cursor) {
            if (c.child) return findSuggestionContext(*c.child, cursor);
            if (!c.nodes.empty()) return {c.nodes.back().first, c.nodes.back().second.end + 1};
            return {c.rootNode, c.range.start};
        }
        int prev = c.rootNode;
        for (const auto& [node, range] : c.nodes) {
            if (range.start <= cursor && cursor <= range.end) return {prev, range.start};
            prev = node;
        }
        return {prev, c.range.start};
    }
    return {c.rootNode, c.range.start};
}

std::string lower(std::string s) {
    for (char& ch : s) ch = char(std::tolower(u8(ch)));
    return s;
}

std::string usageText(const Node& n) { return n.type == 1 ? n.name : "<" + n.name + ">"; }

// CommandDispatcher.getSmartUsage(node, source, optional, deep)
std::string smartUsage(const Tree& t, int node, bool optional, bool deep, int depth) {
    const Node& n = t.nodes[size_t(node)];
    const std::string self = optional ? "[" + usageText(n) + "]" : usageText(n);
    if (depth > 32) return self;
    const bool childOptional = n.executable;
    const char* open = childOptional ? "[" : "(";
    const char* close = childOptional ? "]" : ")";
    if (!deep) {
        if (n.redirect >= 0) return self + " " + (n.redirect == t.root ? "..." : "-> " + usageText(t.nodes[size_t(n.redirect)]));
        if (n.children.size() == 1) {
            const std::string u = smartUsage(t, n.children[0], childOptional, childOptional, depth + 1);
            if (!u.empty()) return self + " " + u;
        } else if (n.children.size() > 1) {
            std::vector<std::string> childUsage;  // LinkedHashSet
            for (int c : n.children) {
                const std::string u = smartUsage(t, c, childOptional, true, depth + 1);
                if (!u.empty() && std::find(childUsage.begin(), childUsage.end(), u) == childUsage.end()) childUsage.push_back(u);
            }
            if (childUsage.size() == 1) return self + " " + (childOptional ? "[" + childUsage[0] + "]" : childUsage[0]);
            if (childUsage.size() > 1) {
                std::string b = open;
                int count = 0;
                for (int c : n.children) {
                    if (count > 0) b += "|";
                    b += usageText(t.nodes[size_t(c)]);
                    ++count;
                }
                if (count > 0) return self + " " + b + close;
            }
        }
    }
    return self;
}

}  // namespace

Parse parse(const Tree& t, const std::string& input, int start) {
    Reader r{input, start};
    Context c;
    c.rootNode = t.root;
    c.range = {start, start};
    if (!t.valid()) return {std::make_shared<Context>(c), input, start, {}};
    return parseNodes(t, t.root, r, c, 0);
}

Completion complete(const Tree& t, const Parse& p, int cursor) {
    Completion out;
    const SuggestionContext sc = findSuggestionContext(*p.ctx, cursor);
    out.parent = sc.parent;
    out.startPos = sc.startPos;
    out.start = std::min(sc.startPos, cursor);
    if (sc.parent < 0 || sc.parent >= int(t.nodes.size())) return out;
    const std::string truncated = p.input.substr(0, size_t(std::clamp(cursor, 0, int(p.input.size()))));
    const std::string remaining = lower(truncated.substr(size_t(std::min(out.start, int(truncated.size())))));
    for (int c : t.nodes[size_t(sc.parent)].children) {
        const Node& n = t.nodes[size_t(c)];
        if (n.type == 1) {
            if (lower(n.name).starts_with(remaining)) out.local.push_back(n.name);
        } else if (n.parser == "brigadier:bool" && n.suggestions.empty()) {  // BoolArgumentType.listSuggestions
            for (const char* v : {"true", "false"})
                if (std::string(v).starts_with(remaining)) out.local.push_back(v);
        } else {
            out.askServer = true;
        }
    }
    std::sort(out.local.begin(), out.local.end());
    out.local.erase(std::unique(out.local.begin(), out.local.end()), out.local.end());
    std::stable_sort(out.local.begin(), out.local.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    return out;
}

std::vector<std::string> argumentUsage(const Tree& t, int parent) {
    std::vector<std::string> out;
    if (parent < 0 || parent >= int(t.nodes.size())) return out;
    const Node& n = t.nodes[size_t(parent)];
    for (int c : n.children)
        if (t.nodes[size_t(c)].type != 1) out.push_back(smartUsage(t, c, n.executable, false, 0));
    return out;
}

Text errorText(const Error& e, const Lang* lang) {
    std::vector<Text> args;
    for (const std::string& a : e.args) args.push_back(Text(a));
    const Text message = translatable(e.key, args, lang);
    if (!e.hasContext) return message;
    // CommandSyntaxException.getContext: up to 10 characters before the cursor, then <--[HERE].
    const int cursor = std::min(int(e.input.size()), e.cursor);
    std::string context = cursor > 10 ? "..." : "";
    context += e.input.substr(size_t(std::max(0, cursor - 10)), size_t(cursor - std::max(0, cursor - 10)));
    context += "<--[HERE]";
    return translatable("command.context.parse_error", {message, Text(std::to_string(e.cursor)), Text(context)}, lang);
}

Error unknownArgument(const Parse& p) {
    Error e;
    e.key = "command.unknown.argument";
    e.hasContext = true;
    e.input = p.input;
    e.cursor = p.cursor;
    return e;
}

bool parseException(const Parse& p, Error& out) {
    if (!p.canRead()) return false;
    if (p.exceptions.size() == 1) {
        out = p.exceptions.front().second;
        return true;
    }
    out = unknownArgument(p);
    if (p.ctx->range.start == p.ctx->range.end) out.key = "command.unknown.command";
    return true;
}

Text format(const Parse& p, const std::string& text, int offset) {
    static const u32 kArgColors[5] = {0x55FFFF, 0xFFFF55, 0x55FF55, 0xFF55FF, 0xFFAA00};  // aqua yellow green purple gold
    const Style literal = colorStyle(0xAAAAAA), unparsed = colorStyle(0xFF5555);
    Text out;
    const int len = int(text.size());
    int unformattedStart = 0, nextColor = -1;
    auto part = [&](int a, int b, const Style& st) {
        a = std::clamp(a, 0, len), b = std::clamp(b, a, len);
        if (b > a) out.append(text.substr(size_t(a), size_t(b - a)), st);
    };
    for (const auto& [name, range] : p.ctx->lastChild().args) {
        if (++nextColor >= 5) nextColor = 0;
        const int start = std::max(range.start - offset, 0);
        if (start >= len) break;
        const int end = std::min(range.end - offset, len);
        if (end > 0) {
            part(unformattedStart, start, literal);
            part(start, end, colorStyle(kArgColors[nextColor]));
            unformattedStart = std::max(unformattedStart, end);
        }
    }
    if (p.canRead()) {
        const int start = std::max(p.cursor - offset, 0);
        if (start < len) {
            const int end = std::min(start + int(p.input.size()) - p.cursor, len);
            part(unformattedStart, start, literal);
            part(start, end, unparsed);
            unformattedStart = std::max(unformattedStart, end);
        }
    }
    part(unformattedStart, len, literal);
    return out;
}

}  // namespace ui::cmd
