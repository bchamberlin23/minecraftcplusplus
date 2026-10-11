#include "render/packs/uniforms.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace packs {

// ---- store ---------------------------------------------------------------------------------------------------------

UValue& UniformStore::at(const std::string& name) {
    auto& p = values_[name];
    if (!p) p = std::make_unique<UValue>();
    return *p;
}
const UValue* UniformStore::find(const std::string& name) const {
    auto it = values_.find(name);
    return it == values_.end() ? nullptr : it->second.get();
}
void UniformStore::setFloat(const std::string& n, float v) {
    UValue& u = at(n);
    u.set = true;
    u.isInt = false, u.count = 1, u.f[0] = v, u.i[0] = i32(v);
}
void UniformStore::setInt(const std::string& n, i32 v) {
    UValue& u = at(n);
    u.set = true;
    u.isInt = true, u.count = 1, u.i[0] = v, u.f[0] = float(v);
}
void UniformStore::setVec(const std::string& n, float x, float y, float z, float w, u8 count) {
    UValue& u = at(n);
    u.set = true;
    u.isInt = false, u.count = count;
    u.f[0] = x, u.f[1] = y, u.f[2] = z, u.f[3] = w;
    for (int k = 0; k < 4; ++k) u.i[k] = i32(u.f[k]);
}
void UniformStore::setIvec(const std::string& n, i32 x, i32 y, i32 z, i32 w, u8 count) {
    UValue& u = at(n);
    u.set = true;
    u.isInt = true, u.count = count;
    u.i[0] = x, u.i[1] = y, u.i[2] = z, u.i[3] = w;
    for (int k = 0; k < 4; ++k) u.f[k] = float(u.i[k]);
}
void UniformStore::setMat4(const std::string& n, const float* m) {
    UValue& u = at(n);
    u.set = true;
    u.isInt = false, u.count = 16;
    std::memcpy(u.f, m, 64);
}
void UniformStore::setMat3(const std::string& n, const float* m) {
    UValue& u = at(n);
    u.set = true;
    u.isInt = false, u.count = 9;
    std::memcpy(u.f, m, 36);
}

void resolveUniforms(const StageReflection& refl, UniformStore& store, std::vector<UniformSlot>& slots,
                     std::vector<std::string>* unknown) {
    slots.clear();
    for (const UniformMember& m : refl.uniforms) {
        (void)unknown;
        slots.push_back({&store.at(m.name), m});
    }
}

void writeUniforms(const std::vector<UniformSlot>& slots, u8* dst) {
    for (const UniformSlot& s : slots) {
        const UniformMember& m = s.member;
        const UValue& v = *s.value;
        u8* p = dst + m.offset;
        if (m.arraySize) {
            // Arrays of scalars / vectors: element k from component k (rare: mcpp_TextureMatrix handled as matrices).
            if (m.columns > 1) {  // array of matrices: consecutive mat4s are not stored; identity-like data only
                for (u32 e = 0; e < m.arraySize; ++e)
                    for (u32 c = 0; c < m.columns; ++c)
                        std::memcpy(p + e * m.arrayStride + c * m.matrixStride, v.f + (c * m.vecsize) % 16, m.vecsize * 4);
                continue;
            }
            for (u32 e = 0; e < m.arraySize && e < 16; ++e) {
                if (m.type == BaseType::Float) std::memcpy(p + e * m.arrayStride, &v.f[e], 4);
                else std::memcpy(p + e * m.arrayStride, &v.i[e], 4);
            }
            continue;
        }
        if (m.columns > 1) {  // matrix: column c at offset c * matrixStride
            const u32 cols = m.columns, rows = m.vecsize;
            const u32 srcRows = v.count == 9 ? 3 : 4;
            for (u32 c = 0; c < cols; ++c) {
                float col[4] = {};
                for (u32 r = 0; r < rows; ++r) col[r] = (c < srcRows && r < srcRows) ? v.f[c * srcRows + r] : 0.0f;
                std::memcpy(p + c * m.matrixStride, col, rows * 4);
            }
            continue;
        }
        switch (m.type) {
            case BaseType::Float: {
                float f[4];
                for (u32 k = 0; k < m.vecsize; ++k) f[k] = v.isInt ? float(v.i[k]) : v.f[k];
                std::memcpy(p, f, m.vecsize * 4);
                break;
            }
            case BaseType::Int:
            case BaseType::UInt:
            case BaseType::Bool: {
                i32 n[4];
                for (u32 k = 0; k < m.vecsize; ++k) {
                    n[k] = v.isInt ? v.i[k] : i32(v.f[k]);
                    if (m.type == BaseType::Bool) n[k] = n[k] != 0;
                }
                std::memcpy(p, n, m.vecsize * 4);
                break;
            }
            default: break;
        }
    }
}

// ---- biome constants ------------------------------------------------------------------------------------------------

namespace {
const std::vector<std::string>& biomeNames() {
    static const std::vector<std::string> names = {
        "badlands", "bamboo_jungle", "basalt_deltas", "beach", "birch_forest", "cherry_grove", "cold_ocean",
        "crimson_forest", "dark_forest", "deep_cold_ocean", "deep_dark", "deep_frozen_ocean", "deep_lukewarm_ocean",
        "deep_ocean", "desert", "dripstone_caves", "end_barrens", "end_highlands", "end_midlands", "eroded_badlands",
        "flower_forest", "forest", "frozen_ocean", "frozen_peaks", "frozen_river", "grove", "ice_spikes",
        "jagged_peaks", "jungle", "lukewarm_ocean", "lush_caves", "mangrove_swamp", "meadow", "mushroom_fields",
        "nether_wastes", "ocean", "old_growth_birch_forest", "old_growth_pine_taiga", "old_growth_spruce_taiga",
        "pale_garden", "plains", "river", "savanna", "savanna_plateau", "small_end_islands", "snowy_beach",
        "snowy_plains", "snowy_slopes", "snowy_taiga", "soul_sand_valley", "sparse_jungle", "stony_peaks",
        "stony_shore", "sulfur_caves", "sunflower_plains", "swamp", "taiga", "the_end", "the_void", "warm_ocean",
        "warped_forest", "windswept_forest", "windswept_gravelly_hills", "windswept_hills", "windswept_savanna",
        "wooded_badlands"};
    return names;
}
}  // namespace

int biomeConstant(const std::string& name, bool& found) {
    found = true;
    static const std::pair<const char*, int> kCats[] = {
        {"CAT_NONE", 0}, {"CAT_TAIGA", 1}, {"CAT_EXTREME_HILLS", 2}, {"CAT_JUNGLE", 3}, {"CAT_MESA", 4},
        {"CAT_PLAINS", 5}, {"CAT_SAVANNA", 6}, {"CAT_ICY", 7}, {"CAT_THE_END", 8}, {"CAT_BEACH", 9},
        {"CAT_FOREST", 10}, {"CAT_OCEAN", 11}, {"CAT_DESERT", 12}, {"CAT_RIVER", 13}, {"CAT_SWAMP", 14},
        {"CAT_MUSHROOM", 15}, {"CAT_NETHER", 16}, {"CAT_MOUNTAIN", 17}, {"CAT_UNDERGROUND", 18},
        {"PPT_NONE", 0}, {"PPT_RAIN", 1}, {"PPT_SNOW", 2}};
    for (const auto& [n, v] : kCats)
        if (name == n) return v;
    if (name.compare(0, 6, "BIOME_") == 0) {
        std::string b = name.substr(6);
        for (char& c : b) c = char(std::tolower(u8(c)));
        const auto& names = biomeNames();
        auto it = std::find(names.begin(), names.end(), b);
        if (it != names.end()) return int(it - names.begin());
        return -1;  // unknown biome: never matches
    }
    found = false;
    return 0;
}

int biomeIndex(std::string_view name) {
    if (name.substr(0, 10) == "minecraft:") name.remove_prefix(10);
    const auto& names = biomeNames();
    auto it = std::find(names.begin(), names.end(), name);
    return it == names.end() ? -1 : int(it - names.begin());
}

// ---- expressions -----------------------------------------------------------------------------------------------------

namespace {
struct Val {
    float v[4] = {};
    u8 n = 1;
    float x() const { return v[0]; }
};
Val num(float f) {
    Val r;
    r.v[0] = f;
    return r;
}
}  // namespace

struct CustomUniforms::Node {
    enum Kind { Const, Var, Member, Call, Unary, Binary } kind = Const;
    Val value;
    std::string name;  // Var / Call / operator
    int component = 0;
    std::vector<std::unique_ptr<Node>> args;
    // smooth() state
    float smoothValue = 0;
    bool smoothInit = false;
};

struct CustomUniforms::Entry {
    std::string type, name;
    std::unique_ptr<Node> root;
};

namespace {
struct ExprParser {
    std::string_view s;
    size_t p = 0;
    std::string err;
    using NodeP = std::unique_ptr<CustomUniforms::Node>;

    void ws() {
        while (p < s.size() && std::isspace(u8(s[p]))) ++p;
    }
    bool eat(std::string_view t) {
        ws();
        if (s.substr(p, t.size()) == t) {
            // don't split "<=" when looking for "<"
            if (t.size() == 1 && (t == "<" || t == ">" || t == "=" || t == "!") && p + 1 < s.size() && s[p + 1] == '=')
                return false;
            if (t.size() == 1 && (t == "&" || t == "|") && p + 1 < s.size() && s[p + 1] == t[0]) return false;
            p += t.size();
            return true;
        }
        return false;
    }
    NodeP make(CustomUniforms::Node::Kind k) {
        auto n = std::make_unique<CustomUniforms::Node>();
        n->kind = k;
        return n;
    }
    NodeP bin(const char* op, NodeP a, NodeP b) {
        auto n = make(CustomUniforms::Node::Binary);
        n->name = op;
        n->args.push_back(std::move(a));
        n->args.push_back(std::move(b));
        return n;
    }
    NodeP primary() {
        ws();
        if (p >= s.size()) {
            err = "unexpected end";
            return make(CustomUniforms::Node::Const);
        }
        if (eat("(")) {
            NodeP e = orExpr();
            if (!eat(")")) err = "missing )";
            return postfix(std::move(e));
        }
        if (std::isdigit(u8(s[p])) || s[p] == '.') {
            size_t q = p;
            while (q < s.size() && (std::isalnum(u8(s[q])) || s[q] == '.')) ++q;
            auto n = make(CustomUniforms::Node::Const);
            n->value = num(float(std::atof(std::string(s.substr(p, q - p)).c_str())));
            p = q;
            return n;
        }
        if (std::isalpha(u8(s[p])) || s[p] == '_') {
            size_t q = p;
            while (q < s.size() && (std::isalnum(u8(s[q])) || s[q] == '_')) ++q;
            std::string id(s.substr(p, q - p));
            p = q;
            if (eat("(")) {
                auto n = make(CustomUniforms::Node::Call);
                n->name = id;
                ws();
                if (!eat(")")) {
                    for (;;) {
                        n->args.push_back(orExpr());
                        if (eat(",")) continue;
                        if (!eat(")")) err = "missing ) after arguments of " + id;
                        break;
                    }
                }
                return postfix(std::move(n));
            }
            auto n = make(CustomUniforms::Node::Var);
            n->name = id;
            if (id == "true" || id == "false") {
                n->kind = CustomUniforms::Node::Const;
                n->value = num(id == "true" ? 1.0f : 0.0f);
            } else if (id == "pi" || id == "PI") {
                n->kind = CustomUniforms::Node::Const;
                n->value = num(3.14159265358979f);
            } else {
                bool found;
                int c = biomeConstant(id, found);
                if (found) {
                    n->kind = CustomUniforms::Node::Const;
                    n->value = num(float(c));
                }
            }
            return postfix(std::move(n));
        }
        err = std::string("unexpected '") + s[p] + "'";
        ++p;
        return make(CustomUniforms::Node::Const);
    }
    NodeP postfix(NodeP n) {
        while (eat(".")) {
            ws();
            char c = p < s.size() ? s[p++] : 'x';
            int comp = c == 'x' || c == 'r' ? 0 : c == 'y' || c == 'g' ? 1 : c == 'z' || c == 'b' ? 2 : 3;
            auto m = make(CustomUniforms::Node::Member);
            m->component = comp;
            m->args.push_back(std::move(n));
            n = std::move(m);
        }
        return n;
    }
    NodeP unary() {
        if (eat("-")) {
            auto n = make(CustomUniforms::Node::Unary);
            n->name = "-";
            n->args.push_back(unary());
            return n;
        }
        if (eat("!")) {
            auto n = make(CustomUniforms::Node::Unary);
            n->name = "!";
            n->args.push_back(unary());
            return n;
        }
        if (eat("+")) return unary();
        return primary();
    }
    NodeP mul() {
        NodeP a = unary();
        for (;;) {
            if (eat("*")) a = bin("*", std::move(a), unary());
            else if (eat("/")) a = bin("/", std::move(a), unary());
            else if (eat("%")) a = bin("%", std::move(a), unary());
            else return a;
        }
    }
    NodeP add() {
        NodeP a = mul();
        for (;;) {
            if (eat("+")) a = bin("+", std::move(a), mul());
            else if (eat("-")) a = bin("-", std::move(a), mul());
            else return a;
        }
    }
    NodeP rel() {
        NodeP a = add();
        for (;;) {
            if (eat("<=")) a = bin("<=", std::move(a), add());
            else if (eat(">=")) a = bin(">=", std::move(a), add());
            else if (eat("<")) a = bin("<", std::move(a), add());
            else if (eat(">")) a = bin(">", std::move(a), add());
            else return a;
        }
    }
    NodeP eq() {
        NodeP a = rel();
        for (;;) {
            if (eat("==")) a = bin("==", std::move(a), rel());
            else if (eat("!=")) a = bin("!=", std::move(a), rel());
            else return a;
        }
    }
    NodeP andExpr() {
        NodeP a = eq();
        while (eat("&&")) a = bin("&&", std::move(a), eq());
        return a;
    }
    NodeP orExpr() {
        NodeP a = andExpr();
        while (eat("||")) a = bin("||", std::move(a), andExpr());
        return a;
    }
};

float frac(float x) { return x - std::floor(x); }
}  // namespace

CustomUniforms::CustomUniforms() = default;
CustomUniforms::~CustomUniforms() = default;

void CustomUniforms::add(const std::string& type, const std::string& name, const std::string& expr, bool isVariable) {
    (void)isVariable;
    ExprParser parser{expr};
    auto e = std::make_unique<Entry>();
    e->type = type;
    e->name = name;
    e->root = parser.orExpr();
    parser.ws();
    if (parser.err.empty() && parser.p != expr.size()) parser.err = "trailing text";
    if (!parser.err.empty()) {
        errors_.push_back(name + " = " + expr + ": " + parser.err);
        return;
    }
    entries_.push_back(std::move(e));
}

namespace {
struct Evaluator {
    UniformStore& store;
    float frameTime;
    int smoothCounter = 0;

    Val eval(CustomUniforms::Node& n) {
        using N = CustomUniforms::Node;
        switch (n.kind) {
            case N::Const: return n.value;
            case N::Var: {
                const UValue* u = store.find(n.name);
                Val r;
                if (!u) return r;
                r.n = u->count > 4 ? 4 : u->count;
                for (int k = 0; k < 4; ++k) r.v[k] = u->isInt ? float(u->i[k]) : u->f[k];
                return r;
            }
            case N::Member: {
                Val a = eval(*n.args[0]);
                return num(a.v[n.component]);
            }
            case N::Unary: {
                Val a = eval(*n.args[0]);
                if (n.name == "-")
                    for (float& f : a.v) f = -f;
                else a = num(a.x() == 0 ? 1.0f : 0.0f);
                return a;
            }
            case N::Binary: {
                const std::string& op = n.name;
                if (op == "&&") return num(eval(*n.args[0]).x() != 0 && eval(*n.args[1]).x() != 0 ? 1.0f : 0.0f);
                if (op == "||") return num(eval(*n.args[0]).x() != 0 || eval(*n.args[1]).x() != 0 ? 1.0f : 0.0f);
                Val a = eval(*n.args[0]), b = eval(*n.args[1]);
                Val r;
                r.n = std::max(a.n, b.n);
                for (int k = 0; k < 4; ++k) {
                    float x = a.v[a.n == 1 ? 0 : k], y = b.v[b.n == 1 ? 0 : k];
                    float& o = r.v[k];
                    if (op == "+") o = x + y;
                    else if (op == "-") o = x - y;
                    else if (op == "*") o = x * y;
                    else if (op == "/") o = y != 0 ? x / y : 0;
                    else if (op == "%") o = y != 0 ? x - y * std::floor(x / y) : 0;
                    else if (op == "<") o = x < y;
                    else if (op == "<=") o = x <= y;
                    else if (op == ">") o = x > y;
                    else if (op == ">=") o = x >= y;
                    else if (op == "==") o = x == y;
                    else if (op == "!=") o = x != y;
                }
                if (op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=") r.n = 1;
                return r;
            }
            case N::Call: return call(n);
        }
        return {};
    }

    Val call(CustomUniforms::Node& n) {
        const std::string& f = n.name;
        auto arg = [&](size_t i) { return i < n.args.size() ? eval(*n.args[i]) : Val{}; };
        auto map1 = [&](float (*fn)(float)) {
            Val a = arg(0);
            for (float& x : a.v) x = fn(x);
            return a;
        };
        if (f == "if") {
            // if(c1, v1, c2, v2, ..., else)
            size_t i = 0;
            for (; i + 1 < n.args.size(); i += 2)
                if (eval(*n.args[i]).x() != 0) return eval(*n.args[i + 1]);
            return i < n.args.size() ? eval(*n.args[i]) : Val{};
        }
        if (f == "smooth") {
            // smooth([id,] value, fadeUp [, fadeDown]) - exponential, per call site
            size_t base = n.args.size() >= 3 ? 1 : 0;
            if (n.args.size() == 3) {
                // ambiguous: (id, value, fade) or (value, up, down); Iris: 3 args = id, value, fade if the first is a constant
                base = n.args[0]->kind == CustomUniforms::Node::Const ? 1 : 0;
            }
            float target = arg(base).x();
            float up = arg(base + 1).x(), down = n.args.size() > base + 2 ? arg(base + 2).x() : up;
            if (!n.smoothInit) {
                n.smoothInit = true;
                n.smoothValue = target;
            } else {
                float t = target > n.smoothValue ? up : down;
                float k = t > 0 ? 1.0f - std::exp(-frameTime * 0.6931472f / t) : 1.0f;  // half-life semantics
                n.smoothValue += (target - n.smoothValue) * k;
            }
            return num(n.smoothValue);
        }
        if (f == "in") {
            float x = arg(0).x();
            for (size_t i = 1; i < n.args.size(); ++i)
                if (eval(*n.args[i]).x() == x) return num(1);
            return num(0);
        }
        if (f == "between") {
            float x = arg(0).x();
            return num(x >= arg(1).x() && x <= arg(2).x() ? 1.0f : 0.0f);
        }
        if (f == "equals") return num(std::abs(arg(0).x() - arg(1).x()) <= arg(2).x() ? 1.0f : 0.0f);
        if (f == "sin") return map1([](float x) { return std::sin(x); });
        if (f == "cos") return map1([](float x) { return std::cos(x); });
        if (f == "tan") return map1([](float x) { return std::tan(x); });
        if (f == "asin") return map1([](float x) { return std::asin(std::clamp(x, -1.0f, 1.0f)); });
        if (f == "acos") return map1([](float x) { return std::acos(std::clamp(x, -1.0f, 1.0f)); });
        if (f == "atan") return map1([](float x) { return std::atan(x); });
        if (f == "atan2") return num(std::atan2(arg(0).x(), arg(1).x()));
        if (f == "torad") return map1([](float x) { return x * 0.01745329252f; });
        if (f == "todeg") return map1([](float x) { return x * 57.2957795f; });
        if (f == "abs") return map1([](float x) { return std::abs(x); });
        if (f == "floor") return map1([](float x) { return std::floor(x); });
        if (f == "ceil") return map1([](float x) { return std::ceil(x); });
        if (f == "frac") return map1([](float x) { return frac(x); });
        if (f == "exp") return map1([](float x) { return std::exp(x); });
        if (f == "exp2") return map1([](float x) { return std::exp2(x); });
        if (f == "log") return map1([](float x) { return x > 0 ? std::log(x) : 0.0f; });
        if (f == "log2") return map1([](float x) { return x > 0 ? std::log2(x) : 0.0f; });
        if (f == "sqrt") return map1([](float x) { return std::sqrt(std::max(x, 0.0f)); });
        if (f == "cbrt") return map1([](float x) { return std::cbrt(x); });
        if (f == "signum" || f == "sign") return map1([](float x) { return float((x > 0) - (x < 0)); });
        if (f == "round") return map1([](float x) { return std::round(x); });
        if (f == "pow") return num(std::pow(arg(0).x(), arg(1).x()));
        if (f == "fmod") {
            float x = arg(0).x(), y = arg(1).x();
            return num(y != 0 ? std::fmod(x, y) : 0);
        }
        if (f == "min" || f == "max") {
            Val r = arg(0);
            for (size_t i = 1; i < n.args.size(); ++i) {
                Val b = eval(*n.args[i]);
                for (int k = 0; k < 4; ++k) r.v[k] = f == "min" ? std::min(r.v[k], b.v[k]) : std::max(r.v[k], b.v[k]);
            }
            return r;
        }
        if (f == "clamp") {
            Val a = arg(0);
            float lo = arg(1).x(), hi = arg(2).x();
            for (float& x : a.v) x = std::clamp(x, lo, hi);
            return a;
        }
        if (f == "random") {
            static std::mt19937 rng(1);
            return num(std::uniform_real_distribution<float>(0, 1)(rng));
        }
        if (f == "vec2" || f == "vec3" || f == "vec4") {
            Val r;
            r.n = u8(f[3] - '0');
            size_t k = 0;
            for (size_t i = 0; i < n.args.size() && k < 4; ++i) {
                Val a = eval(*n.args[i]);
                for (u8 c = 0; c < a.n && k < 4; ++c) r.v[k++] = a.v[c];
            }
            if (n.args.size() == 1)
                for (; k < r.n; ++k) r.v[k] = r.v[0];
            return r;
        }
        return {};
    }
};
}  // namespace

void CustomUniforms::evaluate(UniformStore& store, float frameTime) {
    Evaluator ev{store, frameTime};
    for (auto& e : entries_) {
        Val v = ev.eval(*e->root);
        if (e->type == "float") store.setFloat(e->name, v.x());
        else if (e->type == "int") store.setInt(e->name, i32(v.x()));
        else if (e->type == "bool") store.setBool(e->name, v.x() != 0);
        else if (e->type == "vec2") store.setVec(e->name, v.v[0], v.v[1], 0, 0, 2);
        else if (e->type == "vec3") store.setVec(e->name, v.v[0], v.v[1], v.v[2], 0, 3);
        else if (e->type == "vec4") store.setVec(e->name, v.v[0], v.v[1], v.v[2], v.v[3], 4);
        else store.setFloat(e->name, v.x());
    }
}

}  // namespace packs
