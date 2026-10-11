#include "render/packs/glsl_transform.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace packs {

namespace {

enum class K : u8 { Ident, Number, Punct, Space, Comment, Preproc };
struct Tok {
    K k;
    u32 b, e;
};

bool idStart(char c) { return std::isalpha(u8(c)) || c == '_'; }
bool idChar(char c) { return std::isalnum(u8(c)) || c == '_'; }

void lex(const std::string& s, std::vector<Tok>& out) {
    const size_t n = s.size();
    size_t i = 0;
    bool lineStart = true;
    while (i < n) {
        const char c = s[i];
        if (c == '\n') {
            out.push_back({K::Space, u32(i), u32(i + 1)});
            ++i;
            lineStart = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            size_t j = i;
            while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\f' || s[j] == '\v')) ++j;
            out.push_back({K::Space, u32(i), u32(j)});
            i = j;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            size_t j = s.find('\n', i);
            if (j == std::string::npos) j = n;
            out.push_back({K::Comment, u32(i), u32(j)});
            i = j;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            size_t j = s.find("*/", i + 2);
            j = j == std::string::npos ? n : j + 2;
            out.push_back({K::Comment, u32(i), u32(j)});
            i = j;
            continue;
        }
        if (c == '#' && lineStart) {
            size_t j = i;
            for (;;) {
                size_t e = s.find('\n', j);
                if (e == std::string::npos) {
                    j = n;
                    break;
                }
                size_t k = e;
                while (k > j && (s[k - 1] == '\r')) --k;
                if (k > j && s[k - 1] == '\\') {
                    j = e + 1;
                    continue;
                }
                j = e;
                break;
            }
            out.push_back({K::Preproc, u32(i), u32(j)});
            i = j;
            continue;
        }
        lineStart = false;
        if (idStart(c)) {
            size_t j = i;
            while (j < n && idChar(s[j])) ++j;
            out.push_back({K::Ident, u32(i), u32(j)});
            i = j;
            continue;
        }
        if (std::isdigit(u8(c)) || (c == '.' && i + 1 < n && std::isdigit(u8(s[i + 1])))) {
            size_t j = i;
            if (c == '0' && j + 1 < n && (s[j + 1] == 'x' || s[j + 1] == 'X')) {
                j += 2;
                while (j < n && std::isxdigit(u8(s[j]))) ++j;
            } else {
                while (j < n && (std::isdigit(u8(s[j])) || s[j] == '.')) ++j;
                if (j < n && (s[j] == 'e' || s[j] == 'E')) {
                    ++j;
                    if (j < n && (s[j] == '+' || s[j] == '-')) ++j;
                    while (j < n && std::isdigit(u8(s[j]))) ++j;
                }
            }
            while (j < n && idChar(s[j])) ++j;  // suffixes (u, f, lf)
            out.push_back({K::Number, u32(i), u32(j)});
            i = j;
            continue;
        }
        static const char* kTwo[] = {"==", "!=", "<=", ">=", "&&", "||", "^^", "++", "--", "+=", "-=", "*=",
                                     "/=", "%=", "&=", "|=", "^=", "<<", ">>"};
        size_t len = 1;
        if (i + 1 < n)
            for (const char* op : kTwo)
                if (c == op[0] && s[i + 1] == op[1]) {
                    len = 2;
                    break;
                }
        if (len == 2 && i + 2 < n && s[i + 2] == '=' && (s.compare(i, 2, "<<") == 0 || s.compare(i, 2, ">>") == 0)) len = 3;
        out.push_back({K::Punct, u32(i), u32(i + len)});
        i += len;
    }
}

// GLSL version that made a name a keyword / reserved word (renamed when the source is older and uses it as a name).
const std::unordered_map<std::string, int>& keywordVersions() {
    static const std::unordered_map<std::string, int> m = {
        {"sample", 400},    {"patch", 400},     {"subroutine", 400}, {"precise", 400},  {"buffer", 430},
        {"shared", 430},    {"coherent", 420},  {"volatile", 420},   {"restrict", 420}, {"readonly", 420},
        {"writeonly", 420}, {"filter", 130},    {"common", 130},     {"partition", 130}, {"active", 130},
        {"resource", 400},  {"smooth", 130},    {"flat", 130},       {"noperspective", 130}, {"demote", 460},
        // (layout, uint, switch, case, default: real syntax in packs that declare old versions; never renamed)
    };
    return m;
}

// Built-in functions introduced after GLSL 1.20: a user function of that name in an older shader is renamed.
const std::unordered_map<std::string, int>& builtinVersions() {
    static const std::unordered_map<std::string, int> m = {
        {"round", 130}, {"roundEven", 130}, {"trunc", 130}, {"sinh", 130}, {"cosh", 130}, {"tanh", 130},
        {"asinh", 130}, {"acosh", 130}, {"atanh", 130}, {"isnan", 130}, {"isinf", 130}, {"modf", 130},
        {"textureSize", 130}, {"texelFetch", 130}, {"texelFetchOffset", 130}, {"textureOffset", 130},
        {"textureProjOffset", 130}, {"textureLodOffset", 130}, {"textureProjLodOffset", 130}, {"textureGrad", 130},
        {"textureGradOffset", 130}, {"textureProjGrad", 130}, {"textureProjGradOffset", 130}, {"texture", 130},
        {"textureProj", 130}, {"textureLod", 130}, {"textureProjLod", 130}, {"inverse", 140}, {"determinant", 150},
        {"floatBitsToInt", 330}, {"floatBitsToUint", 330}, {"intBitsToFloat", 330}, {"uintBitsToFloat", 330},
        {"fma", 400}, {"frexp", 400}, {"ldexp", 400}, {"packUnorm2x16", 400}, {"packSnorm2x16", 420},
        {"packUnorm4x8", 400}, {"packSnorm4x8", 400}, {"unpackUnorm2x16", 400}, {"unpackSnorm2x16", 420},
        {"unpackUnorm4x8", 400}, {"unpackSnorm4x8", 400}, {"packHalf2x16", 420}, {"unpackHalf2x16", 420},
        {"bitfieldExtract", 400}, {"bitfieldInsert", 400}, {"bitfieldReverse", 400}, {"bitCount", 400},
        {"findLSB", 400}, {"findMSB", 400}, {"uaddCarry", 400}, {"usubBorrow", 400}, {"umulExtended", 400},
        {"imulExtended", 400}, {"textureGather", 400}, {"textureGatherOffset", 400}, {"textureQueryLod", 400},
        {"textureQueryLevels", 430}, {"interpolateAtCentroid", 400}, {"interpolateAtSample", 400},
        {"interpolateAtOffset", 400}, {"dFdxFine", 450}, {"dFdyFine", 450}, {"dFdxCoarse", 450},
        {"dFdyCoarse", 450}, {"fwidthFine", 450}, {"fwidthCoarse", 450}, {"packDouble2x32", 400},
        {"unpackDouble2x32", 400},
    };
    return m;
}

// Identifiers DXC rejects as reserved words.
const std::unordered_set<std::string>& hlslReserved() {
    static const std::unordered_set<std::string> s = {
        "new", "delete", "this", "template", "typename", "operator", "private", "protected", "public", "friend",
        "virtual", "mutable", "explicit", "throw", "try", "catch", "goto", "sizeof", "signed", "unsigned", "char",
        "short", "long", "auto", "namespace", "using", "static_cast", "const_cast", "dynamic_cast", "reinterpret_cast",
        "union", "enum", "class", "export", "compile", "typedef", "extern", "register", "snorm", "unorm", "packoffset",
        "groupshared", "nointerpolation", "linear", "row_major", "column_major", "globallycoherent",
        "tbuffer", "cbuffer", "string", "vector", "matrix", "half", "dword", "min16float", "min10float", "min16int",
        "min12int", "min16uint", "sampler", "samplerShadow", "technique", "pass", "pixelshader", "vertexshader", "stateblock"};
    return s;
}

// Legacy texture functions -> GLSL 1.30+ names (only when called).
const std::unordered_map<std::string, std::string>& textureRenames() {
    static const std::unordered_map<std::string, std::string> m = {
        {"texture1D", "texture"}, {"texture2D", "texture"}, {"texture3D", "texture"}, {"textureCube", "texture"},
        {"texture1DLod", "textureLod"}, {"texture2DLod", "textureLod"}, {"texture3DLod", "textureLod"},
        {"textureCubeLod", "textureLod"}, {"texture2DProj", "textureProj"}, {"texture2DProjLod", "textureProjLod"},
        {"texture2DGrad", "textureGrad"}, {"texture2DGradARB", "textureGrad"}, {"texture2DGradEXT", "textureGrad"},
        {"texture3DGrad", "textureGrad"}, {"texture3DGradARB", "textureGrad"}, {"texture2DLodEXT", "textureLod"},
        {"texture2DLodARB", "textureLod"}, {"texelFetch2D", "texelFetch"}, {"texelFetch3D", "texelFetch"},
        {"textureSize2D", "textureSize"}, {"texture2DArray", "texture"}, {"texture2DArrayLod", "textureLod"},
        {"shadow2D", "mcpp_shadow2D"}, {"shadow2DLod", "mcpp_shadow2DLod"}, {"shadow2DProj", "mcpp_shadow2DProj"},
        {"shadow2DProjLod", "mcpp_shadow2DProjLod"},
    };
    return m;
}

struct Builtin {
    const char* replacement;
    const char* decl;  // header declaration (by stage, see header())
};

class Transformer {
public:
    Transformer(const std::string& src, const GlslTransformOptions& opt, GlslTransformResult& out)
        : s_(src), opt_(opt), out_(out) {}

    bool run() {
        lex(s_, toks_);
        findVersion();
        analyze();
        std::string body = rewrite(toks_, s_, true);
        if (!out_.error.empty()) return false;
        out_.glsl = header() + "#line 1\n" + body;
        return true;
    }

private:
    std::string_view text(const Tok& t) const { return std::string_view(s_).substr(t.b, t.e - t.b); }
    std::string_view text(const std::vector<Tok>& v, const std::string& s, size_t i) const {
        return std::string_view(s).substr(v[i].b, v[i].e - v[i].b);
    }
    static bool significant(const Tok& t) { return t.k != K::Space && t.k != K::Comment; }
    size_t nextSig(const std::vector<Tok>& v, size_t i) const {
        for (++i; i < v.size(); ++i)
            if (significant(v[i])) return i;
        return v.size();
    }

    void findVersion() {
        for (const Tok& t : toks_) {
            if (t.k != K::Preproc) continue;
            std::string_view l = text(t);
            size_t p = l.find("version");
            if (p == std::string_view::npos || l.substr(1, p - 1).find_first_not_of(" \t") != std::string_view::npos) continue;
            out_.version = std::atoi(std::string(l.substr(p + 7)).c_str());
            out_.compatibility = l.find("compatibility") != std::string_view::npos;
            break;
        }
        if (out_.version == 0) out_.version = 110;
    }

    // ---- pass 1: global declarations ----
    void analyze() {
        const auto& kws = keywordVersions();
        const auto& bis = builtinVersions();
        // Statements at global scope.
        std::vector<size_t> stmt;  // significant token indices of the current global statement
        int depth = 0;
        for (size_t i = 0; i < toks_.size(); ++i) {
            const Tok& t = toks_[i];
            if (!significant(t) || t.k == K::Preproc) continue;
            std::string_view x = text(t);
            if (depth > 0) {
                if (x == "{") ++depth;
                else if (x == "}") {
                    if (--depth == 0 && funcBody_) {  // end of a function body
                        funcBody_ = false;
                        stmt.clear();
                    }
                }
                continue;
            }
            if (x == "{") {
                // Function body or a struct / interface block inside the statement.
                bool function = false;
                for (size_t k : stmt)
                    if (text(toks_[k]) == "(") function = true;
                if (function && !stmt.empty()) {
                    // Function definition: the name is the identifier before the first '('.
                    for (size_t k = 1; k < stmt.size(); ++k)
                        if (text(toks_[stmt[k]]) == "(" && toks_[stmt[k - 1]].k == K::Ident) {
                            userFunctions_.insert(std::string(text(toks_[stmt[k - 1]])));
                            break;
                        }
                    funcBody_ = true;
                }
                depth = 1;
                if (!function) stmt.push_back(i);  // block: keep collecting until ';'
                continue;
            }
            if (x == ";") {
                globalStatement(stmt, i);
                stmt.clear();
                continue;
            }
            stmt.push_back(i);
        }
        // Prototypes count as user functions too (collected in globalStatement).
        const int v = out_.version;
        for (const Tok& t : toks_) {
            if (t.k != K::Ident) continue;
            std::string name(text(t));
            if (auto it = kws.find(name); it != kws.end() && v < it->second) keywordCandidates_.insert(name);
        }
        for (const std::string& f : userFunctions_)
            if (auto it = bis.find(f); it != bis.end() && v < it->second) renames_[f] = "mcpp_u_" + f;
        if (textureIsSampler_) renames_["texture"] = "gtexture";
    }

    void globalStatement(const std::vector<size_t>& stmt, size_t semicolon) {
        if (stmt.empty()) return;
        for (size_t idx = 0; idx < stmt.size(); ++idx) {
            if (toks_[stmt[idx]].k != K::Ident) continue;
            std::string_view nx = idx + 1 < stmt.size() ? text(toks_[stmt[idx + 1]]) : std::string_view(";");
            if (nx == ";" || nx == "=" || nx == "," || nx == "[") declared_.insert(std::string(text(toks_[stmt[idx]])));
        }
        bool isUniform = false, isOut = false, isIn = false, hasLayoutLocation = false, isFunction = false, sawAssign = false;
        size_t layoutPos = SIZE_MAX;
        for (size_t idx = 0; idx < stmt.size(); ++idx) {
            std::string_view x = text(toks_[stmt[idx]]);
            if (x == "uniform") isUniform = true;
            else if (x == "out") isOut = true;
            else if (x == "in" || x == "attribute") isIn = true;
            else if (x == "varying") (opt_.stage == ShaderStage::Fragment ? isIn : isOut) = true;
            else if (x == "layout" && layoutPos == SIZE_MAX) layoutPos = idx;
            else if (x == "location") hasLayoutLocation = true;
            else if (x == "=") sawAssign = true;
            else if (x == "(" && !sawAssign && idx > 0 && toks_[stmt[idx - 1]].k == K::Ident && layoutPos != idx - 1)
                isFunction = true;
        }
        if (isFunction) {  // prototype
            for (size_t k = 1; k < stmt.size(); ++k)
                if (text(toks_[stmt[k]]) == "(" && toks_[stmt[k - 1]].k == K::Ident) {
                    userFunctions_.insert(std::string(text(toks_[stmt[k - 1]])));
                    break;
                }
            return;
        }
        if (isUniform) {
            // uniform <type> <name> [= init];
            size_t eq = SIZE_MAX;
            for (size_t idx = 0; idx < stmt.size(); ++idx)
                if (text(toks_[stmt[idx]]) == "=") {
                    eq = idx;
                    break;
                }
            const size_t end = eq == SIZE_MAX ? stmt.size() : eq;
            if (end >= 2) {
                std::string name(text(toks_[stmt[end - 1]]));
                std::string type(text(toks_[stmt[end - 2]]));
                if (name == "texture" && type.find("sampler") != std::string::npos) textureIsSampler_ = true;
                if (eq != SIZE_MAX) {
                    std::string init;
                    for (size_t k = stmt[eq]; k < semicolon; ++k) {
                        if (k > stmt[eq]) init += text(toks_[k]);
                        blank_.insert(k);
                    }
                    out_.uniformDefaults[name] = init;
                }
            }
            return;
        }
        if (opt_.stage == ShaderStage::Fragment && isOut && !isIn) {
            // User fragment outputs: name(s) and location.
            int location = -1;
            if (hasLayoutLocation)
                for (size_t idx = 0; idx + 2 < stmt.size(); ++idx)
                    if (text(toks_[stmt[idx]]) == "location" && text(toks_[stmt[idx + 1]]) == "=")
                        location = std::atoi(std::string(text(toks_[stmt[idx + 2]])).c_str());
            // Name: last identifier before ';' / '[' / ','.
            std::vector<std::string> names;
            for (size_t idx = 1; idx < stmt.size(); ++idx) {
                std::string_view x = text(toks_[stmt[idx]]);
                bool atEnd = idx + 1 == stmt.size();
                std::string_view nx = atEnd ? ";" : text(toks_[stmt[idx + 1]]);
                if (toks_[stmt[idx]].k == K::Ident && (nx == ";" || nx == "[" || nx == ",")) names.emplace_back(x);
            }
            for (const std::string& n : names) {
                int loc = location;
                if (loc < 0) {
                    loc = 0;
                    while (usedLocations_.count(loc)) ++loc;
                    insertBefore_[stmt[0]] += "layout(location = " + std::to_string(loc) + ") ";
                }
                usedLocations_.insert(loc);
                out_.outputs[n] = loc;
                if (location >= 0) location = -1;  // only the first declarator has the explicit location
            }
        }
    }

    // ---- pass 2: identifier rewrites ----
    std::string rewrite(const std::vector<Tok>& v, const std::string& s, bool main) {
        std::string r;
        r.reserve(s.size() + s.size() / 8);
        const bool fs = opt_.stage == ShaderStage::Fragment, vs = opt_.stage == ShaderStage::Vertex;
        std::unordered_map<size_t, std::string> before;  // text inserted before a token of this list
        for (size_t i = 0; i < v.size(); ++i) {
            const Tok& t = v[i];
            if (auto it = before.find(i); it != before.end()) r += it->second;
            if (main) {
                if (auto it = insertBefore_.find(i); it != insertBefore_.end()) r += it->second;
                if (blank_.count(i)) {
                    for (char c : text(v, s, i))
                        if (c == '\n') r += '\n';
                    continue;
                }
            }
            if (skip_.count(i) && !main) continue;
            if (main && skipMain_.count(i)) continue;
            if (t.k == K::Preproc) {
                r += preproc(std::string(text(v, s, i)));
                continue;
            }
            if (t.k != K::Ident) {
                r += text(v, s, i);
                continue;
            }
            std::string name(text(v, s, i));
            const size_t n1 = nextSig(v, i);
            usedIdents_.insert(name);
            if (name == "attribute" && vs) {
                r += "in";
                continue;
            }
            if (name == "varying" && (vs || fs)) {
                r += fs ? "in" : "out";
                continue;
            }
            const bool call = n1 < v.size() && text(v, s, n1) == "(";
            // gl_FragData[N]
            if (name == "gl_FragData" && fs) {
                const size_t n2 = nextSig(v, n1), n3 = nextSig(v, n2);
                if (!out_.fragDataArray && n1 < v.size() && text(v, s, n1) == "[" && n2 < v.size() && v[n2].k == K::Number &&
                    n3 < v.size() && text(v, s, n3) == "]") {
                    int idx = std::atoi(std::string(text(v, s, n2)).c_str());
                    fragData_.insert(idx);
                    r += "mcpp_FragData_" + std::to_string(idx);
                    for (size_t k = i + 1; k <= n3; ++k)
                        if (!significant(v[k])) r += text(v, s, k);
                    i = n3;
                    continue;
                }
                fragDataDynamic_ = true;
                r += "mcpp_FragData";
                continue;
            }
            if (name == "gl_TexCoord") {
                const size_t n2 = nextSig(v, n1);
                int count = 8;
                if (n1 < v.size() && text(v, s, n1) == "[" && n2 < v.size() && v[n2].k == K::Number)
                    count = std::atoi(std::string(text(v, s, n2)).c_str()) + 1;
                out_.texCoordCount = std::max(out_.texCoordCount, std::min(count, 8));
                use("mcpp_TexCoord");
                r += "mcpp_TexCoord";
                continue;
            }
            if (name == "ftransform" && call) {
                const size_t n2 = nextSig(v, n1);
                if (n2 < v.size() && text(v, s, n2) == ")") {
                    use("mcpp_ModelViewProjectionMatrix");
                    use("mcpp_Vertex");
                    r += "(mcpp_ModelViewProjectionMatrix * mcpp_Vertex)";
                    i = n2;
                    continue;
                }
            }
            if (hlslReserved().count(name)) {
                name = "mcpp_h_" + name;  // C++ words HLSL reserves (SPIRV-Cross does not rename all of them)
            } else if (auto it = renames_.find(name); it != renames_.end() && !(name == "texture" && call)) {
                name = it->second;
            } else if (keywordCandidates_.count(name)) {
                // A keyword in GLSL 460 but a name here: rename when not used as a qualifier (next is not a name).
                if (!(n1 < v.size() && v[n1].k == K::Ident)) name = "mcpp_k_" + name;
            }
            if (call && !userFunctions_.count(name)) {
                if (auto it = textureRenames().find(name); it != textureRenames().end()) {
                    name = it->second;
                    if (name.rfind("mcpp_", 0) == 0) use(name);
                }
                // Implicit-LOD sampling -> level 0 (compute shaders have no derivatives; full-screen passes sample
                // single-level targets, and D3D's derivatives in divergent loops are undefined).
                if (opt_.forceLod0 && (name == "texture" || name == "textureOffset" || name == "textureProj")) {
                    int depth = 0, commas = 0;
                    size_t close = v.size();
                    for (size_t k = n1; k < v.size(); ++k) {
                        if (v[k].k != K::Punct) continue;
                        std::string_view x = text(v, s, k);
                        if (x == "(" || x == "[") ++depth;
                        else if (x == ")" || x == "]") {
                            if (--depth == 0) {
                                close = k;
                                break;
                            }
                        } else if (x == "," && depth == 1) ++commas;
                    }
                    if (close < v.size()) {
                        if (name == "texture") {
                            name = "textureLod";
                            if (commas == 1) before[close] += ", 0.0";
                        } else if (name == "textureProj") {
                            name = "textureProjLod";
                            if (commas == 1) before[close] += ", 0.0";
                        } else if (commas == 2) {  // textureOffset(s, P, offset) -> textureLodOffset(s, P, 0, offset)
                            name = "textureLodOffset";
                            size_t depth2 = 0, c = 0;
                            for (size_t k = n1; k < close; ++k) {
                                if (v[k].k != K::Punct) continue;
                                std::string_view x = text(v, s, k);
                                if (x == "(" || x == "[") ++depth2;
                                else if (x == ")" || x == "]") --depth2;
                                else if (x == "," && depth2 == 1 && ++c == 2) before[k] += ", 0.0";
                            }
                        }
                    }
                }
                // Atlas sampling in terrain fragment shaders: tile-wrapped helpers.
                if (fs && opt_.atlasWrap &&
                    (name == "texture" || name == "textureLod" || name == "textureGrad" || name == "texelFetch")) {
                    const size_t n2 = nextSig(v, n1), n3 = nextSig(v, n2);
                    if (n2 < v.size() && v[n2].k == K::Ident && n3 < v.size() && text(v, s, n3) == ",") {
                        std::string sampler(text(v, s, n2));
                        if (auto rn = renames_.find(sampler); rn != renames_.end()) sampler = rn->second;
                        if (opt_.atlasSamplers.count(sampler)) {
                            name = "mcpp_atlas_" + name;
                            use("mcpp_atlas");
                        }
                    }
                }
            }
            r += builtin(name);
        }
        return r;
    }

    void use(const std::string& n) { out_.builtinsUsed.insert(n); }

    std::string builtin(const std::string& name) {
        if (name.size() < 3 || name.compare(0, 3, "gl_") != 0) return name;
        const bool fs = opt_.stage == ShaderStage::Fragment, vs = opt_.stage == ShaderStage::Vertex;
        static const std::unordered_map<std::string, std::string> common = {
            {"gl_ModelViewMatrix", "mcpp_ModelViewMatrix"},
            {"gl_ProjectionMatrix", "mcpp_ProjectionMatrix"},
            {"gl_ModelViewProjectionMatrix", "mcpp_ModelViewProjectionMatrix"},
            {"gl_NormalMatrix", "mcpp_NormalMatrix"},
            {"gl_TextureMatrix", "mcpp_TextureMatrix"},
            {"gl_ModelViewMatrixInverse", "mcpp_ModelViewMatrixInverse"},
            {"gl_ProjectionMatrixInverse", "mcpp_ProjectionMatrixInverse"},
            {"gl_ModelViewProjectionMatrixInverse", "mcpp_ModelViewProjectionMatrixInverse"},
            {"gl_ModelViewMatrixTranspose", "mcpp_ModelViewMatrixTranspose"},
            {"gl_ModelViewMatrixInverseTranspose", "mcpp_ModelViewMatrixInverseTranspose"},
            {"gl_ProjectionMatrixTranspose", "mcpp_ProjectionMatrixTranspose"},
            {"gl_Fog", "mcpp_Fog"},
            {"gl_DepthRange", "mcpp_DepthRange"},
        };
        static const std::unordered_map<std::string, std::string> vert = {
            {"gl_Vertex", "mcpp_Vertex"}, {"gl_Normal", "mcpp_Normal"}, {"gl_Color", "mcpp_Color"},
            {"gl_MultiTexCoord0", "mcpp_MultiTexCoord0"}, {"gl_MultiTexCoord1", "mcpp_MultiTexCoord1"},
            {"gl_MultiTexCoord2", "mcpp_MultiTexCoord2"}, {"gl_MultiTexCoord3", "mcpp_MultiTexCoord3"},
            {"gl_SecondaryColor", "mcpp_SecondaryColor"}, {"gl_FogCoord", "mcpp_FogCoord"},
            {"gl_FrontColor", "mcpp_FrontColor"}, {"gl_BackColor", "mcpp_FrontColor"},
            {"gl_FrontSecondaryColor", "mcpp_FrontSecondaryColor"}, {"gl_BackSecondaryColor", "mcpp_FrontSecondaryColor"},
            {"gl_FogFragCoord", "mcpp_FogFragCoord"}, {"gl_ClipVertex", "mcpp_ClipVertex"},
            {"gl_VertexID", "gl_VertexIndex"}, {"gl_InstanceID", "gl_InstanceIndex"},
        };
        static const std::unordered_map<std::string, std::string> frag = {
            {"gl_Color", "mcpp_FrontColor"}, {"gl_SecondaryColor", "mcpp_FrontSecondaryColor"},
            {"gl_FogFragCoord", "mcpp_FogFragCoord"}, {"gl_FragColor", "mcpp_FragColor"},
        };
        if (auto it = common.find(name); it != common.end()) return use(it->second), it->second;
        if (vs)
            if (auto it = vert.find(name); it != vert.end()) return use(it->second), it->second;
        if (fs)
            if (auto it = frag.find(name); it != frag.end()) return use(it->second), it->second;
        return name;
    }

    std::string preproc(const std::string& line) {
        // #version / #extension: dropped (the header has its own); #define bodies get the identifier rewrites.
        size_t p = 1;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
        size_t q = p;
        while (q < line.size() && idChar(line[q])) ++q;
        const std::string d = line.substr(p, q - p);
        auto newlines = [&] {
            std::string r;
            for (char c : line)
                if (c == '\n') r += '\n';
            return r;
        };
        if (d == "version" || d == "extension") return newlines();
        if (d == "define") {
            // Compatibility macros for names this transform handles itself (#define varying in, #define texture2D
            // texture, ...) are dropped.
            size_t a = q;
            while (a < line.size() && (line[a] == ' ' || line[a] == '	')) ++a;
            size_t b = a;
            while (b < line.size() && idChar(line[b])) ++b;
            const std::string macro = line.substr(a, b - a);
            if (macro == "varying" || macro == "attribute" || textureRenames().count(macro)) return newlines();
            std::vector<Tok> v;
            lex(line.substr(q), v);
            for (auto& t : v)  // a '#' inside a macro body is not a directive
                if (t.k == K::Preproc) t.k = K::Punct;
            return line.substr(0, q) + rewrite(v, line.substr(q), false);
        }
        return line;
    }

    std::string header() {
        const bool fs = opt_.stage == ShaderStage::Fragment, vs = opt_.stage == ShaderStage::Vertex;
        std::string h = "#version 460\n" + opt_.predefines;
        auto used = [&](const char* n) { return out_.builtinsUsed.count(n) != 0; };
        if (used("mcpp_Fog"))
            h += "struct mcpp_FogParameters { vec4 color; float density; float start; float end; float scale; };\n"
                 "uniform mcpp_FogParameters mcpp_Fog;\n";
        if (used("mcpp_DepthRange"))
            h += "struct mcpp_DepthRangeParameters { float near; float far; float diff; };\n"
                 "uniform mcpp_DepthRangeParameters mcpp_DepthRange;\n";
        static const char* kMat4[] = {"mcpp_ModelViewMatrix", "mcpp_ProjectionMatrix", "mcpp_ModelViewProjectionMatrix",
                                      "mcpp_ModelViewMatrixInverse", "mcpp_ProjectionMatrixInverse",
                                      "mcpp_ModelViewProjectionMatrixInverse", "mcpp_ModelViewMatrixTranspose",
                                      "mcpp_ModelViewMatrixInverseTranspose", "mcpp_ProjectionMatrixTranspose"};
        for (const char* m : kMat4)
            if (used(m)) h += std::string("uniform mat4 ") + m + ";\n";
        if (used("mcpp_NormalMatrix")) h += "uniform mat3 mcpp_NormalMatrix;\n";
        // Core-profile (1.17+) names Iris declares when a pack uses them without declaring them.
        auto implicit = [&](const char* name, const char* decl) {
            if (usedIdents_.count(name) && !declared_.count(name)) h += std::string(decl) + " " + name + ";\n";
        };
        implicit("modelViewMatrix", "uniform mat4");
        implicit("projectionMatrix", "uniform mat4");
        implicit("modelViewMatrixInverse", "uniform mat4");
        implicit("projectionMatrixInverse", "uniform mat4");
        implicit("normalMatrix", "uniform mat3");
        implicit("textureMatrix", "uniform mat4");
        implicit("chunkOffset", "uniform vec3");
        if (vs) {
            implicit("vaPosition", "in vec3");
            implicit("vaColor", "in vec4");
            implicit("vaUV0", "in vec2");
            implicit("vaUV1", "in ivec2");
            implicit("vaUV2", "in ivec2");
            implicit("vaNormal", "in vec3");
        }
        if (used("mcpp_TextureMatrix")) h += "uniform mat4 mcpp_TextureMatrix[4];\n";
        if (vs) {
            static const std::pair<const char*, const char*> kIn[] = {
                {"mcpp_Vertex", "vec4"},          {"mcpp_Normal", "vec3"},          {"mcpp_Color", "vec4"},
                {"mcpp_MultiTexCoord0", "vec4"}, {"mcpp_MultiTexCoord1", "vec4"}, {"mcpp_MultiTexCoord2", "vec4"},
                {"mcpp_MultiTexCoord3", "vec4"}, {"mcpp_SecondaryColor", "vec4"}, {"mcpp_FogCoord", "float"}};
            for (const auto& [n, type] : kIn)
                if (used(n)) h += std::string("in ") + type + " " + n + ";\n";
            if (used("mcpp_FrontColor")) h += "out vec4 mcpp_FrontColor;\n";
            if (used("mcpp_FrontSecondaryColor")) h += "out vec4 mcpp_FrontSecondaryColor;\n";
            if (used("mcpp_FogFragCoord")) h += "out float mcpp_FogFragCoord;\n";
            if (used("mcpp_ClipVertex")) h += "vec4 mcpp_ClipVertex;\n";
            if (out_.texCoordCount) h += "out vec4 mcpp_TexCoord[" + std::to_string(out_.texCoordCount) + "];\n";
        }
        if (fs) {
            if (used("mcpp_FrontColor")) h += "in vec4 mcpp_FrontColor;\n";
            if (used("mcpp_FrontSecondaryColor")) h += "in vec4 mcpp_FrontSecondaryColor;\n";
            if (used("mcpp_FogFragCoord")) h += "in float mcpp_FogFragCoord;\n";
            if (out_.texCoordCount) h += "in vec4 mcpp_TexCoord[" + std::to_string(out_.texCoordCount) + "];\n";
            if (fragDataDynamic_) {
                h += "layout(location = 0) out vec4 mcpp_FragData[8];\n";
                out_.fragDataArray = true;
                out_.outputs["mcpp_FragData"] = 0;
            } else {
                for (int idx : fragData_) {
                    h += "layout(location = " + std::to_string(idx) + ") out vec4 mcpp_FragData_" + std::to_string(idx) + ";\n";
                    out_.outputs["mcpp_FragData_" + std::to_string(idx)] = idx;
                }
            }
            if (used("mcpp_FragColor")) {
                h += "layout(location = 0) out vec4 mcpp_FragColor;\n";
                out_.outputs["mcpp_FragColor"] = 0;
            }
            if (used("mcpp_atlas")) {
                h += "layout(location = 31) flat in vec4 mcpp_tile;\n"
                     "vec2 mcpp_wrapUv(vec2 uv) { return mcpp_tile.z > 0.0 ? mcpp_tile.xy + mod(uv - mcpp_tile.xy, mcpp_tile.zw) : uv; }\n"
                     "vec4 mcpp_atlas_texture(sampler2D s, vec2 uv) {\n"
                     "    if (mcpp_tile.z > 0.0) return textureGrad(s, mcpp_wrapUv(uv), dFdx(uv), dFdy(uv));\n"
                     "    return texture(s, uv);\n}\n"
                     "vec4 mcpp_atlas_texture(sampler2D s, vec2 uv, float bias) {\n"
                     "    if (mcpp_tile.z > 0.0) { float k = exp2(bias); return textureGrad(s, mcpp_wrapUv(uv), dFdx(uv) * k, dFdy(uv) * k); }\n"
                     "    return texture(s, uv, bias);\n}\n"
                     "vec4 mcpp_atlas_textureLod(sampler2D s, vec2 uv, float lod) { return textureLod(s, mcpp_wrapUv(uv), lod); }\n"
                     "vec4 mcpp_atlas_textureGrad(sampler2D s, vec2 uv, vec2 dx, vec2 dy) { return textureGrad(s, mcpp_wrapUv(uv), dx, dy); }\n"
                     "vec4 mcpp_atlas_texelFetch(sampler2D s, ivec2 p, int lod) {\n"
                     "    if (mcpp_tile.z > 0.0) { vec2 size = vec2(textureSize(s, lod)); p = ivec2(mcpp_wrapUv((vec2(p) + 0.5) / size) * size); }\n"
                     "    return texelFetch(s, p, lod);\n}\n";
            }
        }
        if (used("mcpp_shadow2D"))
            h += "vec4 mcpp_shadow2D(sampler2DShadow s, vec3 c) { return vec4(texture(s, c)); }\n"
                 "vec4 mcpp_shadow2D(sampler2D s, vec3 c) { return vec4(step(c.z, texture(s, c.xy).r)); }\n";
        if (used("mcpp_shadow2DLod"))
            h += "vec4 mcpp_shadow2DLod(sampler2DShadow s, vec3 c, float l) { return vec4(textureLod(s, c, l)); }\n"
                 "vec4 mcpp_shadow2DLod(sampler2D s, vec3 c, float l) { return vec4(step(c.z, textureLod(s, c.xy, l).r)); }\n";
        if (used("mcpp_shadow2DProj"))
            h += "vec4 mcpp_shadow2DProj(sampler2DShadow s, vec4 c) { return vec4(textureProj(s, c)); }\n";
        if (used("mcpp_shadow2DProjLod"))
            h += "vec4 mcpp_shadow2DProjLod(sampler2DShadow s, vec4 c, float l) { return vec4(textureProjLod(s, c, l)); }\n";
        return h;
    }

    const std::string& s_;
    const GlslTransformOptions& opt_;
    GlslTransformResult& out_;
    std::vector<Tok> toks_;
    bool funcBody_ = false;
    bool textureIsSampler_ = false;
    bool fragDataDynamic_ = false;
    std::set<int> fragData_;
    std::set<int> usedLocations_;
    std::unordered_set<std::string> userFunctions_, declared_, usedIdents_;
    std::unordered_set<std::string> keywordCandidates_;
    std::unordered_map<std::string, std::string> renames_;
    std::unordered_map<size_t, std::string> insertBefore_;
    std::unordered_set<size_t> blank_, skip_, skipMain_;
};

}  // namespace

bool transformGlsl(const std::string& source, const GlslTransformOptions& opt, GlslTransformResult& out) {
    out = {};
    Transformer t(source, opt, out);
    return t.run();
}

}  // namespace packs
