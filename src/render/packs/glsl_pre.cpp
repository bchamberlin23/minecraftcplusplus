#include "render/packs/glsl_pre.h"

#include "render/packs/pack_source.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace packs {

namespace {

bool isIdStart(char c) { return std::isalpha(u8(c)) || c == '_'; }
bool isIdChar(char c) { return std::isalnum(u8(c)) || c == '_'; }

std::string_view trimView(std::string_view s) {
    while (!s.empty() && std::isspace(u8(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(u8(s.back()))) s.remove_suffix(1);
    return s;
}

void splitLines(const std::string& text, std::vector<std::string>& out) {
    size_t i = 0;
    while (i <= text.size()) {
        size_t j = text.find('\n', i);
        if (j == std::string::npos) j = text.size();
        std::string line = text.substr(i, j - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
        if (j == text.size()) break;
        i = j + 1;
    }
}

// Removes // and /* */ comments from a single (directive) line; `inBlock` carries a block comment across lines.
std::string stripComments(std::string_view s, bool& inBlock) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (inBlock) {
            if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '/') inBlock = false, ++i;
            continue;
        }
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') break;
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            inBlock = true, ++i;
            r += ' ';
            continue;
        }
        r += s[i];
    }
    return r;
}

// Block comment state after scanning a code line (strings are rare in GLSL; not handled).
bool updateBlockState(std::string_view s, bool inBlock) {
    for (size_t i = 0; i + 1 < s.size() + 1; ++i) {
        if (i >= s.size()) break;
        if (inBlock) {
            if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '/') inBlock = false, ++i;
        } else {
            if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') break;
            if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') inBlock = true, ++i;
        }
    }
    return inBlock;
}

// ---- tokens ------------------------------------------------------------------------------------------------
enum class Tk { Id, Num, Op, End };
struct Token {
    Tk kind;
    std::string text;
};

void tokenize(std::string_view s, std::vector<Token>& out) {
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (std::isspace(u8(c))) {
            ++i;
            continue;
        }
        if (isIdStart(c)) {
            size_t j = i;
            while (j < s.size() && isIdChar(s[j])) ++j;
            out.push_back({Tk::Id, std::string(s.substr(i, j - i))});
            i = j;
            continue;
        }
        if (std::isdigit(u8(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(u8(s[i + 1])))) {
            size_t j = i;
            if (c == '0' && j + 1 < s.size() && (s[j + 1] == 'x' || s[j + 1] == 'X')) {
                j += 2;
                while (j < s.size() && std::isxdigit(u8(s[j]))) ++j;
            } else {
                while (j < s.size() && (std::isdigit(u8(s[j])) || s[j] == '.')) ++j;
                if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
                    ++j;
                    if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
                    while (j < s.size() && std::isdigit(u8(s[j]))) ++j;
                }
            }
            while (j < s.size() && (s[j] == 'u' || s[j] == 'U' || s[j] == 'f' || s[j] == 'F' || s[j] == 'l' || s[j] == 'L')) ++j;
            out.push_back({Tk::Num, std::string(s.substr(i, j - i))});
            i = j;
            continue;
        }
        static const char* kOps[] = {"<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "##"};
        bool two = false;
        if (i + 1 < s.size())
            for (const char* op : kOps)
                if (s[i] == op[0] && s[i + 1] == op[1]) {
                    out.push_back({Tk::Op, op});
                    i += 2, two = true;
                    break;
                }
        if (two) continue;
        out.push_back({Tk::Op, std::string(1, c)});
        ++i;
    }
}

std::string joinTokens(const std::vector<Token>& t) {
    std::string r;
    for (size_t i = 0; i < t.size(); ++i) {
        if (i) r += ' ';
        r += t[i].text;
    }
    return r;
}

// Macro expansion with hide sets (no # / ##). `keepDefined`: leave the operand of `defined` alone.
void expandTokens(const std::vector<Token>& in, const MacroTable& macros, std::vector<Token>& out,
                  std::unordered_set<std::string>& hide, bool keepDefined, int depth = 0) {
    for (size_t i = 0; i < in.size(); ++i) {
        const Token& t = in[i];
        if (t.kind != Tk::Id) {
            out.push_back(t);
            continue;
        }
        if (keepDefined && t.text == "defined") {
            out.push_back(t);
            size_t j = i + 1;
            if (j < in.size() && in[j].text == "(") {
                while (j < in.size() && in[j].text != ")") out.push_back(in[j++]);
                if (j < in.size()) out.push_back(in[j]);
                i = j;
            } else if (j < in.size()) {
                out.push_back(in[j]);
                i = j;
            }
            continue;
        }
        auto it = macros.find(t.text);
        if (it == macros.end() || hide.count(t.text) || depth > 64) {
            out.push_back(t);
            continue;
        }
        const Macro& m = it->second;
        std::vector<Token> body;
        tokenize(m.body, body);
        if (m.function) {
            if (i + 1 >= in.size() || in[i + 1].text != "(") {
                out.push_back(t);
                continue;
            }
            std::vector<std::vector<Token>> args(1);
            size_t j = i + 2;
            int level = 0;
            for (; j < in.size(); ++j) {
                const std::string& x = in[j].text;
                if (in[j].kind == Tk::Op && x == "(") ++level;
                if (in[j].kind == Tk::Op && x == ")") {
                    if (level == 0) break;
                    --level;
                }
                if (in[j].kind == Tk::Op && x == "," && level == 0) {
                    args.emplace_back();
                    continue;
                }
                args.back().push_back(in[j]);
            }
            i = j;  // at ')'
            std::vector<Token> substituted;
            for (const Token& b : body) {
                auto p = b.kind == Tk::Id ? std::find(m.params.begin(), m.params.end(), b.text) : m.params.end();
                if (p != m.params.end() && size_t(p - m.params.begin()) < args.size()) {
                    std::vector<Token> ex;
                    expandTokens(args[size_t(p - m.params.begin())], macros, ex, hide, keepDefined, depth + 1);
                    substituted.insert(substituted.end(), ex.begin(), ex.end());
                } else {
                    substituted.push_back(b);
                }
            }
            body.swap(substituted);
        }
        hide.insert(t.text);
        expandTokens(body, macros, out, hide, keepDefined, depth + 1);
        hide.erase(t.text);
    }
}

// ---- #if expression parser -------------------------------------------------------------------------------
struct Parser {
    const std::vector<Token>& t;
    const MacroTable& macros;
    size_t p = 0;
    bool ok = true;
    std::string err;

    const Token* peek() const { return p < t.size() ? &t[p] : nullptr; }
    bool accept(const char* op) {
        if (p < t.size() && t[p].kind == Tk::Op && t[p].text == op) return ++p, true;
        return false;
    }
    static PpValue mk(i64 v) { return {false, v, 0}; }
    static PpValue mkf(double v) { return {true, 0, v}; }
    static PpValue arith(const PpValue& a, const PpValue& b, char op) {
        if (a.isFloat || b.isFloat) {
            double x = a.num(), y = b.num();
            switch (op) {
                case '+': return mkf(x + y);
                case '-': return mkf(x - y);
                case '*': return mkf(x * y);
                case '/': return mkf(y != 0 ? x / y : 0);
                case '%': return mkf(y != 0 ? std::fmod(x, y) : 0);
            }
        }
        switch (op) {
            case '+': return mk(a.i + b.i);
            case '-': return mk(a.i - b.i);
            case '*': return mk(a.i * b.i);
            case '/': return mk(b.i != 0 ? a.i / b.i : 0);
            case '%': return mk(b.i != 0 ? a.i % b.i : 0);
        }
        return mk(0);
    }
    PpValue primary() {
        const Token* k = peek();
        if (!k) {
            ok = false, err = "unexpected end of expression";
            return mk(0);
        }
        if (accept("(")) {
            PpValue v = ternary();
            if (!accept(")")) ok = false, err = "missing )";
            return v;
        }
        if (k->kind == Tk::Num) {
            ++p;
            std::string s = k->text;
            while (!s.empty() && (s.back() == 'u' || s.back() == 'U' || s.back() == 'l' || s.back() == 'L' ||
                                  ((s.back() == 'f' || s.back() == 'F') && s.find('x') == std::string::npos)))
                s.pop_back();
            bool fl = s.find_first_of(".eE") != std::string::npos && s.find_first_of("xX") == std::string::npos;
            if (fl) return mkf(std::strtod(s.c_str(), nullptr));
            return mk(i64(std::strtoull(s.c_str(), nullptr, 0)));
        }
        if (k->kind == Tk::Id) {
            ++p;
            if (k->text == "defined") {
                bool paren = accept("(");
                const Token* n = peek();
                if (!n || n->kind != Tk::Id) {
                    ok = false, err = "defined without a name";
                    return mk(0);
                }
                ++p;
                if (paren && !accept(")")) ok = false, err = "missing ) after defined";
                return mk(macros.count(n->text) ? 1 : 0);
            }
            if (k->text == "true") return mk(1);
            return mk(0);  // undefined identifier (macros were expanded before parsing)
        }
        ok = false, err = "unexpected '" + k->text + "'";
        ++p;
        return mk(0);
    }
    PpValue unary() {
        if (accept("!")) return mk(unary().truthy() ? 0 : 1);
        if (accept("-")) {
            PpValue v = unary();
            return v.isFloat ? mkf(-v.f) : mk(-v.i);
        }
        if (accept("+")) return unary();
        if (accept("~")) return mk(~unary().i);
        return primary();
    }
    PpValue mul() {
        PpValue v = unary();
        for (;;) {
            if (accept("*")) v = arith(v, unary(), '*');
            else if (accept("/")) v = arith(v, unary(), '/');
            else if (accept("%")) v = arith(v, unary(), '%');
            else return v;
        }
    }
    PpValue add() {
        PpValue v = mul();
        for (;;) {
            if (accept("+")) v = arith(v, mul(), '+');
            else if (accept("-")) v = arith(v, mul(), '-');
            else return v;
        }
    }
    PpValue shift() {
        PpValue v = add();
        for (;;) {
            if (accept("<<")) v = mk(v.i << add().i);
            else if (accept(">>")) v = mk(v.i >> add().i);
            else return v;
        }
    }
    PpValue rel() {
        PpValue v = shift();
        for (;;) {
            if (accept("<=")) v = mk(v.num() <= shift().num());
            else if (accept(">=")) v = mk(v.num() >= shift().num());
            else if (accept("<")) v = mk(v.num() < shift().num());
            else if (accept(">")) v = mk(v.num() > shift().num());
            else return v;
        }
    }
    PpValue eq() {
        PpValue v = rel();
        for (;;) {
            if (accept("==")) v = mk(v.num() == rel().num());
            else if (accept("!=")) v = mk(v.num() != rel().num());
            else return v;
        }
    }
    PpValue band() {
        PpValue v = eq();
        while (p < t.size() && t[p].text == "&" ) {
            ++p;
            v = mk(v.i & eq().i);
        }
        return v;
    }
    PpValue bxor() {
        PpValue v = band();
        while (accept("^")) v = mk(v.i ^ band().i);
        return v;
    }
    PpValue bor() {
        PpValue v = bxor();
        while (p < t.size() && t[p].text == "|") {
            ++p;
            v = mk(v.i | bxor().i);
        }
        return v;
    }
    PpValue land() {
        PpValue v = bor();
        while (accept("&&")) {
            PpValue r = bor();
            v = mk(v.truthy() && r.truthy());
        }
        return v;
    }
    PpValue lor() {
        PpValue v = land();
        while (accept("||")) {
            PpValue r = land();
            v = mk(v.truthy() || r.truthy());
        }
        return v;
    }
    PpValue ternary() {
        PpValue c = lor();
        if (accept("?")) {
            PpValue a = ternary();
            if (!accept(":")) ok = false, err = "missing : in ?:";
            PpValue b = ternary();
            return c.truthy() ? a : b;
        }
        return c;
    }
};

std::string describe(const std::vector<SourceLine>& in, size_t i) {
    return "line " + std::to_string(in[i].line) + " (file #" + std::to_string(in[i].file) + ")";
}

struct Cond {
    bool parentActive;
    bool taken;   // some branch was taken
    bool active;  // this branch is active
    bool sawElse;
};

bool parseDirective(std::string_view line, std::string& name, std::string& rest) {
    std::string_view s = trimView(line);
    if (s.empty() || s[0] != '#') return false;
    s.remove_prefix(1);
    while (!s.empty() && (s[0] == ' ' || s[0] == '\t')) s.remove_prefix(1);
    size_t j = 0;
    while (j < s.size() && isIdChar(s[j])) ++j;
    name = std::string(s.substr(0, j));
    rest = std::string(trimView(s.substr(j)));
    return true;
}

}  // namespace

// ---- includes ----------------------------------------------------------------------------------------------

namespace {
bool includeRec(const PackSource& src, const std::string& file, const std::string& text, Included& out, int depth) {
    const u16 fileIndex = u16(out.files.size());
    out.files.push_back(file);
    std::vector<std::string> lines;
    splitLines(text, lines);
    const std::string dir = file.find('/') == std::string::npos ? "" : file.substr(0, file.rfind('/') + 1);
    bool inBlock = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        std::string name, rest;
        if (!inBlock && parseDirective(l, name, rest) && name == "include") {
            size_t a = rest.find_first_of("\"<"), b = a == std::string::npos ? a : rest.find_first_of("\">", a + 1);
            if (a == std::string::npos || b == std::string::npos) {
                out.lines.push_back({"#error malformed #include", fileIndex, u32(i + 1)});
                continue;
            }
            std::string target = rest.substr(a + 1, b - a - 1);
            std::string rel = target.size() && target[0] == '/' ? PackSource::normalize(target)
                                                                 : PackSource::normalize(dir + target);
            std::string inc;
            if (depth > 40 || !src.read(rel, inc)) {
                // Only an error if the include is in an active block (resolved by the preprocessor).
                out.lines.push_back({"#error missing include " + target, fileIndex, u32(i + 1)});
                continue;
            }
            includeRec(src, rel, inc, out, depth + 1);
            continue;
        }
        inBlock = updateBlockState(l, inBlock);
        out.lines.push_back({l, fileIndex, u32(i + 1)});
    }
    return true;
}
}  // namespace

bool expandIncludesText(const PackSource& src, const std::string& name, const std::string& text, Included& out) {
    out.lines.clear();
    out.files.clear();
    out.error.clear();
    return includeRec(src, name, text, out, 0);
}

bool expandIncludes(const PackSource& src, const std::string& file, Included& out) {
    std::string text;
    if (!src.read(file, text)) {
        out.error = "cannot read " + file;
        return false;
    }
    return expandIncludesText(src, file, text, out);
}

// ---- expressions -------------------------------------------------------------------------------------------

std::optional<PpValue> evalPp(std::string_view expr, const MacroTable& macros, std::string* error) {
    std::vector<Token> raw, ex;
    tokenize(expr, raw);
    std::unordered_set<std::string> hide;
    expandTokens(raw, macros, ex, hide, true);
    Parser p{ex, macros};
    PpValue v = p.ternary();
    if (p.ok && p.p != ex.size()) p.ok = false, p.err = "trailing tokens";
    if (!p.ok) {
        if (error) *error = p.err + " in '" + std::string(expr) + "'";
        return std::nullopt;
    }
    return v;
}

std::string expandMacros(std::string_view text, const MacroTable& macros) {
    std::vector<Token> raw, ex;
    tokenize(text, raw);
    std::unordered_set<std::string> hide;
    expandTokens(raw, macros, ex, hide, false);
    return joinTokens(ex);
}

// ---- preprocessing ------------------------------------------------------------------------------------------

bool preprocess(const std::vector<SourceLine>& in, MacroTable& macros, Preprocessed& out) {
    out.text.clear();
    out.lineOrigin.clear();
    out.error.clear();
    std::vector<Cond> stack;
    auto active = [&] { return stack.empty() || stack.back().active; };
    bool inBlock = false;
    for (size_t i = 0; i < in.size(); ++i) {
        const std::string& line = in[i].text;
        std::string name, rest;
        if (!inBlock && parseDirective(line, name, rest)) {
            // Continuation lines.
            size_t last = i;
            std::string full = rest;
            while (!full.empty() && full.back() == '\\' && last + 1 < in.size()) {
                full.pop_back();
                full += ' ';
                full += in[++last].text;
            }
            bool blk = false;
            std::string body = std::string(trimView(stripComments(full, blk)));
            const bool wasActive = active();
            if (name == "if" || name == "ifdef" || name == "ifndef") {
                bool v = false;
                if (wasActive) {
                    if (name == "if") {
                        std::string err;
                        auto r = evalPp(body, macros, &err);
                        if (!r) {
                            out.error = describe(in, i) + ": #if: " + err;
                            return false;
                        }
                        v = r->truthy();
                    } else {
                        std::string id;
                        for (char c : body) {
                            if (!isIdChar(c)) break;
                            id += c;
                        }
                        v = (macros.count(id) != 0) == (name == "ifdef");
                    }
                }
                stack.push_back({wasActive, v, wasActive && v, false});
            } else if (name == "elif") {
                if (stack.empty()) return out.error = describe(in, i) + ": #elif without #if", false;
                Cond& c = stack.back();
                if (c.parentActive && !c.taken) {
                    std::string err;
                    auto r = evalPp(body, macros, &err);
                    if (!r) return out.error = describe(in, i) + ": #elif: " + err, false;
                    c.active = r->truthy();
                    c.taken = c.active;
                } else {
                    c.active = false;
                }
            } else if (name == "else") {
                if (stack.empty()) return out.error = describe(in, i) + ": #else without #if", false;
                Cond& c = stack.back();
                c.active = c.parentActive && !c.taken;
                c.taken = true;
                c.sawElse = true;
            } else if (name == "endif") {
                if (stack.empty()) return out.error = describe(in, i) + ": #endif without #if", false;
                stack.pop_back();
            } else if (wasActive) {
                if (name == "define") {
                    size_t j = 0;
                    while (j < body.size() && isIdChar(body[j])) ++j;
                    std::string id = body.substr(0, j);
                    Macro m;
                    if (j < body.size() && body[j] == '(') {
                        m.function = true;
                        size_t close = body.find(')', j);
                        std::string params = body.substr(j + 1, close == std::string::npos ? std::string::npos : close - j - 1);
                        size_t a = 0;
                        while (a <= params.size()) {
                            size_t b = params.find(',', a);
                            if (b == std::string::npos) b = params.size();
                            std::string pn(trimView(std::string_view(params).substr(a, b - a)));
                            if (!pn.empty()) m.params.push_back(pn);
                            a = b + 1;
                        }
                        m.body = close == std::string::npos ? "" : std::string(trimView(std::string_view(body).substr(close + 1)));
                    } else {
                        m.body = std::string(trimView(std::string_view(body).substr(j)));
                    }
                    if (!id.empty()) macros[id] = std::move(m);
                } else if (name == "undef") {
                    std::string id;
                    for (char c : body) {
                        if (!isIdChar(c)) break;
                        id += c;
                    }
                    macros.erase(id);
                } else if (name == "error") {
                    out.error = describe(in, i) + ": #error " + body;
                    return false;
                }
                // #define / #undef / #version / #extension / #pragma / #line pass through to the GLSL compiler.
                if (name != "error" && name != "line") {
                    std::string text = in[i].text;
                    for (size_t k = i + 1; k <= last; ++k) {
                        text.pop_back();  // the backslash
                        text += ' ';
                        text += in[k].text;
                    }
                    out.text += text;
                    out.text += '\n';
                    out.lineOrigin.push_back(u32(i));
                }
            }
            // Keep line counts stable for the continuation lines.
            i = last;
            continue;
        }
        inBlock = updateBlockState(line, inBlock);
        if (active()) {
            out.text += line;
            out.text += '\n';
            out.lineOrigin.push_back(u32(i));
        }
    }
    if (!stack.empty()) return out.error = "unterminated #if", false;
    return true;
}

bool preprocessText(const std::string& text, MacroTable& macros, Preprocessed& out) {
    std::vector<std::string> lines;
    splitLines(text, lines);
    std::vector<SourceLine> in;
    in.reserve(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) in.push_back({lines[i], 0, u32(i + 1)});
    return preprocess(in, macros, out);
}

std::string describeLine(const Included& inc, const Preprocessed& pp, u32 outputLine) {
    if (outputLine >= pp.lineOrigin.size()) return "?";
    const SourceLine& l = inc.lines[pp.lineOrigin[outputLine]];
    return (l.file < inc.files.size() ? inc.files[l.file] : std::string("?")) + ":" + std::to_string(l.line);
}

// ---- options -------------------------------------------------------------------------------------------------

namespace {
struct DefineLine {
    bool commented = false;
    std::string name, value, comment;  // comment: after "//"
    size_t indentEnd = 0;
};
bool parseDefineLine(std::string_view l, DefineLine& d) {
    size_t i = 0;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
    d.indentEnd = i;
    if (l.substr(i, 2) == "//") {
        d.commented = true;
        i += 2;
        while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
    }
    if (l.substr(i, 7) != "#define") return false;
    i += 7;
    if (i >= l.size() || (l[i] != ' ' && l[i] != '\t')) return false;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
    size_t j = i;
    while (j < l.size() && isIdChar(l[j])) ++j;
    if (j == i || (j < l.size() && l[j] == '(')) return false;
    d.name = std::string(l.substr(i, j - i));
    size_t c = l.find("//", j);
    std::string_view value = l.substr(j, c == std::string_view::npos ? std::string_view::npos : c - j);
    d.value = std::string(trimView(value));
    d.comment = c == std::string_view::npos ? "" : std::string(trimView(l.substr(c + 2)));
    return true;
}
struct ConstLine {
    std::string type, name, value, comment;
};
bool parseConstLine(std::string_view l, ConstLine& c) {
    std::string_view s = trimView(l);
    if (s.substr(0, 6) != "const ") return false;
    s.remove_prefix(6);
    s = trimView(s);
    size_t j = 0;
    while (j < s.size() && isIdChar(s[j])) ++j;
    c.type = std::string(s.substr(0, j));
    if (c.type != "int" && c.type != "float" && c.type != "bool") return false;
    s = trimView(s.substr(j));
    j = 0;
    while (j < s.size() && isIdChar(s[j])) ++j;
    c.name = std::string(s.substr(0, j));
    s = trimView(s.substr(j));
    if (s.empty() || s[0] != '=') return false;
    size_t semi = s.find(';');
    if (semi == std::string_view::npos) return false;
    c.value = std::string(trimView(s.substr(1, semi - 1)));
    std::string_view after = trimView(s.substr(semi + 1));
    if (after.substr(0, 2) != "//") return false;
    c.comment = std::string(trimView(after.substr(2)));
    return !c.name.empty();
}
// "[a b c] rest" -> allowed values; returns false without a list.
bool parseAllowed(const std::string& comment, std::vector<std::string>& allowed, std::string& rest) {
    size_t a = comment.find('['), b = comment.find(']');
    if (a == std::string::npos || b == std::string::npos || b < a) return false;
    std::string_view list = std::string_view(comment).substr(a + 1, b - a - 1);
    size_t i = 0;
    while (i < list.size()) {
        while (i < list.size() && std::isspace(u8(list[i]))) ++i;
        size_t j = i;
        while (j < list.size() && !std::isspace(u8(list[j]))) ++j;
        if (j > i) allowed.emplace_back(list.substr(i, j - i));
        i = j;
    }
    rest = std::string(trimView(std::string_view(comment).substr(b + 1)));
    return !allowed.empty();
}
}  // namespace

void collectConditionNames(const std::vector<SourceLine>& lines, std::unordered_map<std::string, int>& out) {
    for (const SourceLine& l : lines) {
        std::string name, rest;
        if (!parseDirective(l.text, name, rest)) continue;
        if (name != "ifdef" && name != "ifndef" && name != "if" && name != "elif") continue;
        std::vector<Token> t;
        bool blk = false;
        tokenize(stripComments(rest, blk), t);
        for (const Token& k : t)
            if (k.kind == Tk::Id) ++out[k.text];
    }
}

void scanOptions(const std::vector<SourceLine>& lines, std::map<std::string, OptionDecl>& out) {
    std::unordered_map<std::string, int> conflicts;
    for (const SourceLine& l : lines) {
        DefineLine d;
        if (parseDefineLine(l.text, d)) {
            OptionDecl o;
            o.name = d.name;
            if (d.value.empty()) {
                o.boolean = true;
                o.value = d.commented ? "false" : "true";
                o.comment = d.comment;
            } else {
                if (d.commented) continue;
                if (!parseAllowed(d.comment, o.allowed, o.comment)) continue;
                o.value = d.value;
            }
            auto it = out.find(o.name);
            if (it == out.end()) {
                out.emplace(o.name, std::move(o));
            } else if (it->second.boolean && o.boolean && it->second.value != o.value) {
                ++conflicts[o.name];  // declared on in one file and off in another: not an option (Iris)
            }
            continue;
        }
        ConstLine c;
        if (parseConstLine(l.text, c)) {
            OptionDecl o;
            o.name = c.name;
            o.isConst = true;
            o.value = c.value;
            if (!parseAllowed(c.comment, o.allowed, o.comment)) continue;
            out.emplace(o.name, std::move(o));
        }
    }
    for (const auto& [n, k] : conflicts) out.erase(n);
}

void applyOptions(std::vector<SourceLine>& lines, const std::map<std::string, OptionDecl>& decls,
                  const std::unordered_map<std::string, std::string>& values) {
    if (values.empty()) return;
    for (SourceLine& l : lines) {
        DefineLine d;
        if (parseDefineLine(l.text, d)) {
            auto v = values.find(d.name);
            auto decl = decls.find(d.name);
            if (v == values.end() || decl == decls.end() || decl->second.isConst) continue;
            const std::string indent = l.text.substr(0, d.indentEnd);
            const std::string comment = d.comment.empty() ? "" : " // " + d.comment;
            if (decl->second.boolean) {
                if (!d.value.empty()) continue;
                l.text = indent + (v->second == "true" ? "" : "//") + "#define " + d.name + comment;
            } else {
                if (d.commented || d.value.empty()) continue;
                l.text = indent + "#define " + d.name + " " + v->second + comment;
            }
            continue;
        }
        ConstLine c;
        if (parseConstLine(l.text, c)) {
            auto v = values.find(c.name);
            auto decl = decls.find(c.name);
            if (v == values.end() || decl == decls.end() || !decl->second.isConst) continue;
            l.text = "const " + c.type + " " + c.name + " = " + v->second + "; // " + c.comment;
        }
    }
}

}  // namespace packs
