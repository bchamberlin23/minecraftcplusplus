#include "assets/assets.h"

#include "assets/block_shapes.h"

#include "assets/asset_source.h"
#include "assets/game_reports.h"
#include "assets/zip.h"
#include "core/jobs.h"
#include "worldgen/rng.h"

#include <json/json.hpp>
#include <stb/stb_image.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

using json = nlohmann::json;

namespace {

struct I3 {
    int x, y, z;
    bool operator==(const I3&) const = default;
    I3 operator-() const { return {-x, -y, -z}; }
};

struct F3 {
    float x = 0, y = 0, z = 0;
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};
F3 operator+(F3 a, F3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
F3 operator-(F3 a, F3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
F3 operator*(F3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(F3 a, F3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
F3 cross(F3 a, F3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
F3 toF(I3 v) { return {float(v.x), float(v.y), float(v.z)}; }

constexpr I3 kDir[6] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
// Canonical texture axes per face; identical to Minecraft's automatic UV mapping.
constexpr I3 kU[6] = {{1, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
constexpr I3 kV[6] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0}};
constexpr float kFaceShade[6] = {0.5f, 1.0f, 0.8f, 0.8f, 0.6f, 0.6f};

// Blockstate variant rotations: x first, then y, 90 degree steps (same sense as the game).
I3 rotX(I3 v) { return {v.x, v.z, -v.y}; }
I3 rotY(I3 v) { return {-v.z, v.y, v.x}; }
F3 rotX(F3 v) { return {v.x, v.z, -v.y}; }
F3 rotY(F3 v) { return {-v.z, v.y, v.x}; }
template <class T>
T rotVariant(T v, int rx, int ry) {
    for (int i = 0; i < rx; ++i) v = rotX(v);
    for (int i = 0; i < ry; ++i) v = rotY(v);
    return v;
}

int dirIndex(I3 v) {
    for (int d = 0; d < 6; ++d)
        if (kDir[d] == v) return d;
    return -1;
}

int faceIndex(std::string_view s) {
    if (s == "down" || s == "bottom") return 0;
    if (s == "up" || s == "top") return 1;
    if (s == "north") return 2;
    if (s == "south") return 3;
    if (s == "west") return 4;
    if (s == "east") return 5;
    return -1;
}

std::string stripNs(std::string_view s) {
    if (s.starts_with("minecraft:")) s.remove_prefix(10);
    return std::string(s);
}


float num(const json& v, float def) { return v.is_number() ? v.get<float>() : def; }
F3 vec3(const json& v, F3 def) {
    if (!v.is_array() || v.size() < 3) return def;
    return {num(v[0], def.x), num(v[1], def.y), num(v[2], def.z)};
}
const json& member(const json& obj, const char* key) {
    static const json kNull;
    auto it = obj.find(key);
    return it == obj.end() ? kNull : *it;
}

struct Face {
    int tex = 0;
    i8 cull = -1;
    u8 rot = 0;
    i8 tintIndex = -1;
    bool hasUv = false;
    float uv[4] = {};
};

struct Element {
    F3 from, to;
    bool rotated = false;
    F3 origin{8, 8, 8};
    F3 euler;  // degrees about x, then y, then z
    int rescaleAxis = -1;
    float rescale = 1.0f;
    bool shade = true;
    int shadeDir = -1;
    u8 lightEmission = 0;
    Face faces[6];
    u8 faceMask = 0;
};

struct Model {
    std::vector<Element> elements;
};

struct ModelRef {
    int model = -1;
    int x = 0, y = 0;
    bool uvlock = false;
};

struct Cond {
    std::vector<std::pair<std::string, std::vector<std::string>>> terms;  // AND of (key in values)
};

struct Case {
    std::vector<Cond> any;  // OR; empty = always
    ModelRef ref;           // refs[0]
    std::vector<ModelRef> refs;  // a weighted list's entries (one for a single model)
    std::vector<u32> weights;
};

struct BlockDef {
    bool multipart = false;
    std::vector<Case> cases;
};

using Props = std::vector<std::pair<std::string_view, std::string_view>>;

struct StrHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};
template <class V>
using StrMap = std::unordered_map<std::string, V, StrHash, std::equal_to<>>;

std::string jsonScalar(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    return v.dump();
}

std::vector<std::string> splitValues(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t bar = s.find('|', start);
        out.push_back(s.substr(start, bar - start));
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    return out;
}

bool condMatch(const Cond& c, const Props& props) {
    for (const auto& [k, vals] : c.terms) {
        const std::string_view* value = nullptr;
        for (const auto& [pk, pv] : props)
            if (pk == k) {
                value = &pv;
                break;
            }
        if (!value || std::find(vals.begin(), vals.end(), *value) == vals.end()) return false;
    }
    return true;
}

// BlockColors.createDefault: the tint of a face's tint layer (tintindex); blocks not listed there stay untinted.
u8 tintForBlock(std::string_view n, const Props& props, int layer) {
    if (layer < 0) return TintNone;
    auto value = [&](std::string_view key) {
        for (const auto& [k, v] : props)
            if (k == key) return std::clamp(std::atoi(std::string(v).c_str()), 0, 15);
        return 0;
    };
    if (n == "pink_petals" || n == "wildflowers") return layer == 1 ? TintGrass : TintNone;  // blank layer, then grass
    if (layer != 0) return TintNone;
    if (n == "grass_block" || n == "fern" || n == "short_grass" || n == "potted_fern" || n == "bush" || n == "large_fern" ||
        n == "tall_grass" || n == "sugar_cane")
        return TintGrass;
    if (n == "spruce_leaves") return TintSpruce;
    if (n == "birch_leaves") return TintBirch;
    if (n == "oak_leaves" || n == "jungle_leaves" || n == "acacia_leaves" || n == "dark_oak_leaves" || n == "vine" ||
        n == "mangrove_leaves")
        return TintFoliage;
    if (n == "leaf_litter") return TintDryFoliage;
    if (n == "water" || n == "bubble_column" || n == "water_cauldron") return TintWater;
    if (n == "redstone_wire") return u8(TintRedstone + value("power"));
    if (n == "attached_melon_stem" || n == "attached_pumpkin_stem") return TintAttachedStem;
    if (n == "melon_stem" || n == "pumpkin_stem") return u8(TintStem + std::min(value("age"), 7));
    if (n == "lily_pad") return TintLilyPad;
    return TintNone;
}

bool caseMatch(const Case& c, const Props& props) {
    if (c.any.empty()) return true;
    for (const Cond& cond : c.any)
        if (condMatch(cond, props)) return true;
    return false;
}

bool isFullCube(const Model& m) {
    if (m.elements.empty()) return false;
    const Element& e = m.elements[0];
    if (e.rotated || e.faceMask != 63) return false;
    for (int a = 0; a < 3; ++a)
        if (e.from[a] != 0 || e.to[a] != 16) return false;
    for (const Face& f : e.faces)
        if (f.hasUv && (f.uv[0] != 0 || f.uv[1] != 0 || f.uv[2] != 16 || f.uv[3] != 16)) return false;
    return true;
}

void autoUv(int d, F3 f, F3 t, float uv[4]) {
    const float r[6][4] = {
        {f.x, 16 - t.z, t.x, 16 - f.z},           // down
        {f.x, f.z, t.x, t.z},                     // up
        {16 - t.x, 16 - t.y, 16 - f.x, 16 - f.y}, // north
        {f.x, 16 - t.y, t.x, 16 - f.y},           // south
        {f.z, 16 - t.y, t.z, 16 - f.y},           // west
        {16 - t.z, 16 - t.y, 16 - f.z, 16 - f.y}, // east
    };
    std::memcpy(uv, r[d], sizeof(float) * 4);
}

F3 rotAxis(F3 v, int axis, float deg) {
    if (deg == 0) return v;
    float rad = deg * 3.14159265f / 180.0f, c = std::cos(rad), s = std::sin(rad);
    if (axis == 0) return {v.x, v.y * c - v.z * s, v.y * s + v.z * c};
    if (axis == 1) return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
    return {v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}

F3 applyElementRotation(const Element& e, F3 p, bool isNormal) {
    if (!e.rotated) return p;
    F3 o = isNormal ? F3{} : e.origin * (1.0f / 16.0f);
    F3 v = p - o;
    v = rotAxis(v, 0, e.euler.x);
    v = rotAxis(v, 1, e.euler.y);
    v = rotAxis(v, 2, e.euler.z);
    if (!isNormal && e.rescaleAxis >= 0)
        for (int a = 0; a < 3; ++a)
            if (a != e.rescaleAxis) v[a] *= e.rescale;
    return v + o;
}

// cutout: alpha-tested texture (leaves, plants): each mip's alpha is scaled so the share of texels passing the 0.5
// test stays that of the full-size texture (otherwise distant foliage thins out into see-through dark holes).
void buildMips(u8* layer, bool cutout = false) {
    int coverage = 0;  // texels with alpha >= 128 in the full-size texture
    for (int p = 0; p < kTexSize * kTexSize; ++p) coverage += layer[p * 4 + 3] >= 128;
    const float target = float(coverage) / float(kTexSize * kTexSize);
    for (int m = 1; m < kTexMips; ++m) {
        int s = kTexSize >> m;
        const u8* src = layer + kTexMipOffset[m - 1];
        u8* dst = layer + kTexMipOffset[m];
        for (int y = 0; y < s; ++y)
            for (int x = 0; x < s; ++x) {
                float rgb[3] = {}, plain[3] = {}, alpha = 0;
                for (int k = 0; k < 4; ++k) {
                    const u8* p = src + (((y * 2 + (k >> 1)) * s * 2) + x * 2 + (k & 1)) * 4;
                    float a = p[3] / 255.0f;
                    for (int c = 0; c < 3; ++c) {
                        rgb[c] += p[c] * a;
                        plain[c] += p[c];
                    }
                    alpha += a;
                }
                u8* o = dst + (y * s + x) * 4;
                for (int c = 0; c < 3; ++c) o[c] = u8(alpha > 0 ? rgb[c] / alpha + 0.5f : plain[c] / 4 + 0.5f);
                o[3] = u8(alpha / 4 * 255 + 0.5f);
            }
        if (!cutout || coverage == 0) continue;
        auto covered = [&](float scale) {
            int n = 0;
            for (int p = 0; p < s * s; ++p) n += float(dst[p * 4 + 3]) * scale >= 127.5f;
            return float(n) / float(s * s);
        };
        float lo = 0.0f, hi = 8.0f;
        for (int it = 0; it < 16; ++it) {
            const float mid = (lo + hi) * 0.5f;
            (covered(mid) < target ? lo : hi) = mid;
        }
        for (int p = 0; p < s * s; ++p) dst[p * 4 + 3] = u8(std::min(255.0f, float(dst[p * 4 + 3]) * hi + 0.5f));
    }
}

}  // namespace

struct BlockRegistry::Impl {
    StrMap<u16> texIndex;
    std::vector<u8> texClass;
    struct Animation {
        u32 layer, firstFrameLayer;                  // the texture's layer; its strip frames start here
        std::vector<std::pair<u16, u16>> sequence;   // (strip frame, ticks)
        u32 totalTicks;
    };
    std::vector<Animation> animations;
    std::vector<u8> pixels;
    u32 texCount = 0;

    StrMap<json> modelJson;       // "block/stone"
    StrMap<json> blockstateJson;  // "stone"
    StrMap<int> modelIndex;
    std::vector<Model> models;
    StrMap<BlockDef> defs;
    StrMap<u16> cullGroups;
    StrMap<u16> ids;
    std::mutex mutex;

    int texture(std::string ref, const StrMap<std::string>& vars) const {
        for (int guard = 0; !ref.empty() && ref[0] == '#' && guard < 16; ++guard) {
            auto it = vars.find(std::string_view(ref).substr(1));
            if (it == vars.end()) return 0;
            ref = it->second;
        }
        if (ref.empty() || ref[0] == '#') return 0;
        auto it = texIndex.find(stripNs(ref));
        return it == texIndex.end() ? 0 : it->second;
    }

    u16 cullGroup(const std::string& name) {
        auto it = cullGroups.find(name);
        if (it != cullGroups.end()) return it->second;
        u16 g = u16(cullGroups.size() + 1);
        cullGroups.emplace(name, g);
        return g;
    }

    int resolveModel(std::string_view rawName) {
        std::string name = stripNs(rawName);
        if (auto it = modelIndex.find(name); it != modelIndex.end()) return it->second;

        StrMap<std::string> vars;
        const json* elements = nullptr;
        std::string cur = name;
        for (int guard = 0; guard < 32; ++guard) {
            auto it = modelJson.find(cur);
            if (it == modelJson.end()) break;
            const json& j = it->second;
            const json& tex = member(j, "textures");
            if (tex.is_object())
                for (const auto& item : tex.items()) {
                    const json& v = item.value();
                    if (v.is_string())
                        vars.emplace(item.key(), v.get<std::string>());
                    else if (v.is_object() && member(v, "sprite").is_string())
                        vars.emplace(item.key(), member(v, "sprite").get<std::string>());
                }
            if (!elements && member(j, "elements").is_array()) elements = &member(j, "elements");
            const json& parent = member(j, "parent");
            if (!parent.is_string()) break;
            cur = stripNs(parent.get<std::string>());
        }

        Model m;
        if (elements)
            for (const json& ej : *elements) {
                if (!ej.is_object()) continue;
                Element e;
                e.from = vec3(member(ej, "from"), {0, 0, 0});
                e.to = vec3(member(ej, "to"), {16, 16, 16});
                if (const json& r = member(ej, "rotation"); r.is_object()) {
                    e.rotated = true;
                    e.origin = vec3(member(r, "origin"), {8, 8, 8});
                    if (member(r, "axis").is_string()) {
                        std::string ax = member(r, "axis").get<std::string>();
                        int a = ax == "x" ? 0 : ax == "y" ? 1 : 2;
                        float angle = num(member(r, "angle"), 0);
                        e.euler[a] = angle;
                        if (member(r, "rescale").is_boolean() && member(r, "rescale").get<bool>() &&
                            std::abs(angle) > 1e-3f) {
                            e.rescaleAxis = a;
                            e.rescale = 1.0f / std::cos(angle * 3.14159265f / 180.0f);
                        }
                    } else {
                        e.euler = {num(member(r, "x"), 0), num(member(r, "y"), 0), num(member(r, "z"), 0)};
                    }
                }
                if (member(ej, "shade").is_boolean()) e.shade = member(ej, "shade").get<bool>();
                e.lightEmission = u8(std::clamp(int(num(member(ej, "light_emission"), 0)), 0, 15));
                if (member(ej, "shade_direction_override").is_string())
                    e.shadeDir = faceIndex(member(ej, "shade_direction_override").get<std::string>());
                const json& faces = member(ej, "faces");
                if (faces.is_object())
                    for (const auto& item : faces.items()) {
                        int d = faceIndex(item.key());
                        const json& fj = item.value();
                        if (d < 0 || !fj.is_object()) continue;
                        Face f;
                        if (member(fj, "texture").is_string())
                            f.tex = texture(member(fj, "texture").get<std::string>(), vars);
                        if (const json& uv = member(fj, "uv"); uv.is_array() && uv.size() == 4) {
                            f.hasUv = true;
                            for (int k = 0; k < 4; ++k) f.uv[k] = num(uv[k], 0);
                        }
                        if (member(fj, "cullface").is_string())
                            f.cull = i8(faceIndex(member(fj, "cullface").get<std::string>()));
                        f.rot = u8(((int(num(member(fj, "rotation"), 0)) / 90) % 4 + 4) % 4);
                        f.tintIndex = i8(num(member(fj, "tintindex"), -1));
                        e.faces[d] = f;
                        e.faceMask |= u8(1 << d);
                    }
                m.elements.push_back(e);
            }

        int idx = int(models.size());
        models.push_back(std::move(m));
        modelIndex.emplace(name, idx);
        return idx;
    }

    ModelRef parseRef(const json& j0) {
        const json& j = j0.is_array() && !j0.empty() ? j0[0] : j0;
        ModelRef r;
        if (!j.is_object()) return r;
        if (member(j, "model").is_string()) r.model = resolveModel(member(j, "model").get<std::string>());
        r.x = ((int(num(member(j, "x"), 0)) / 90) % 4 + 4) % 4;
        r.y = ((int(num(member(j, "y"), 0)) / 90) % 4 + 4) % 4;
        r.uvlock = member(j, "uvlock").is_boolean() && member(j, "uvlock").get<bool>();
        return r;
    }

    void parseRefs(const json& j, Case& c) {
        for (const json& e : j.is_array() ? j : json::array({j})) {
            c.refs.push_back(parseRef(e));
            c.weights.push_back(u32(std::max(1, int(num(member(e, "weight"), 1)))));
        }
        c.ref = c.refs.empty() ? ModelRef{} : c.refs[0];
    }

    std::vector<Cond> parseWhen(const json& w) {
        std::vector<Cond> out;
        if (!w.is_object()) return out;
        if (const json& orList = member(w, "OR"); orList.is_array()) {
            for (const json& item : orList) {
                auto sub = parseWhen(item);
                out.insert(out.end(), sub.begin(), sub.end());
            }
            return out;
        }
        Cond c;
        if (const json& andList = member(w, "AND"); andList.is_array()) {
            for (const json& item : andList) {
                auto sub = parseWhen(item);
                if (!sub.empty()) c.terms.insert(c.terms.end(), sub[0].terms.begin(), sub[0].terms.end());
            }
        } else {
            for (const auto& item : w.items()) c.terms.push_back({item.key(), splitValues(jsonScalar(item.value()))});
        }
        out.push_back(std::move(c));
        return out;
    }

    const BlockDef* def(const std::string& name) {
        if (auto it = defs.find(name); it != defs.end()) return &it->second;
        auto js = blockstateJson.find(name);
        if (js == blockstateJson.end()) return nullptr;
        const json& j = js->second;
        BlockDef d;
        if (const json& variants = member(j, "variants"); variants.is_object()) {
            for (const auto& item : variants.items()) {
                Case c;
                std::string_view key = item.key();
                if (!key.empty()) {
                    Cond cond;
                    while (!key.empty()) {
                        size_t comma = key.find(',');
                        std::string_view kv = key.substr(0, comma);
                        size_t eq = kv.find('=');
                        if (eq != std::string_view::npos)
                            cond.terms.push_back({std::string(kv.substr(0, eq)), {std::string(kv.substr(eq + 1))}});
                        if (comma == std::string_view::npos) break;
                        key.remove_prefix(comma + 1);
                    }
                    c.any.push_back(std::move(cond));
                }
                parseRefs(item.value(), c);
                d.cases.push_back(std::move(c));
            }
        } else if (const json& parts = member(j, "multipart"); parts.is_array()) {
            d.multipart = true;
            for (const json& part : parts) {
                Case c;
                c.any = parseWhen(member(part, "when"));
                parseRefs(member(part, "apply"), c);
                d.cases.push_back(std::move(c));
            }
        }
        return &defs.emplace(name, std::move(d)).first->second;
    }

    bool bakeFace(const Element& e, int d, const ModelRef& r, std::string_view blockName, const Props& props, ModelQuad& q) const {
        const Face& f = e.faces[d];
        F3 lo = e.from * (1.0f / 16.0f), hi = e.to * (1.0f / 16.0f);
        int axis = d < 2 ? 1 : d < 4 ? 2 : 0;
        int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
        F3 corners[4];
        for (int i = 0; i < 4; ++i) {
            corners[i][axis] = (d & 1) ? hi[axis] : lo[axis];
            corners[i][a1] = (i & 1) ? hi[a1] : lo[a1];
            corners[i][a2] = (i & 2) ? hi[a2] : lo[a2];
        }
        F3 U = toF(kU[d]), V = toF(kV[d]);
        float cu[4], cv[4];
        for (int i = 0; i < 4; ++i) cu[i] = dot(corners[i], U), cv[i] = dot(corners[i], V);
        float uMin = *std::min_element(cu, cu + 4), uMax = *std::max_element(cu, cu + 4);
        float vMin = *std::min_element(cv, cv + 4), vMax = *std::max_element(cv, cv + 4);
        if (uMax - uMin < 1e-5f || vMax - vMin < 1e-5f) return false;

        float rect[4];
        if (f.hasUv)
            std::memcpy(rect, f.uv, sizeof(rect));
        else
            autoUv(d, e.from, e.to, rect);

        F3 normal = rotVariant(applyElementRotation(e, toF(kDir[d]), true), r.x, r.y);
        F3 pos[4];
        float uv[4][2];
        for (int i = 0; i < 4; ++i) {
            int au = cu[i] > (uMin + uMax) * 0.5f, av = cv[i] > (vMin + vMax) * 0.5f;
            int slot = au + 2 * av;
            float tu = float(au), tv = float(av);
            for (int k = 0; k < f.rot; ++k) {
                float nu = tv, nv = 1 - tu;
                tu = nu, tv = nv;
            }
            uv[slot][0] = (rect[0] + (rect[2] - rect[0]) * tu) / 16.0f;
            uv[slot][1] = (rect[1] + (rect[3] - rect[1]) * tv) / 16.0f;
            F3 p = applyElementRotation(e, corners[i], false);
            pos[slot] = rotVariant(p - F3{0.5f, 0.5f, 0.5f}, r.x, r.y) + F3{0.5f, 0.5f, 0.5f};
        }

        if (r.uvlock) {
            // Texture stays world-aligned: derive UVs from the final position on the face plane.
            int t = -1;
            for (int k = 0; k < 6; ++k)
                if (dot(normal, toF(kDir[k])) > 0.99f) t = k;
            if (t >= 0) {
                F3 tu = toF(kU[t]), tv = toF(kV[t]);
                float ou = float((kU[t].x < 0) + (kU[t].y < 0) + (kU[t].z < 0));
                float ov = float((kV[t].x < 0) + (kV[t].y < 0) + (kV[t].z < 0));
                for (int s = 0; s < 4; ++s) {
                    uv[s][0] = dot(pos[s], tu) + ou;
                    uv[s][1] = dot(pos[s], tv) + ov;
                }
            }
        }

        if (dot(cross(pos[1] - pos[0], pos[2] - pos[0]), normal) > 0) {
            std::swap(pos[1], pos[2]);
            std::swap(uv[1], uv[2]);
        }

        for (int s = 0; s < 4; ++s) {
            q.pos[s][0] = pos[s].x, q.pos[s][1] = pos[s].y, q.pos[s][2] = pos[s].z;
            q.uv[s][0] = uv[s][0], q.uv[s][1] = uv[s][1];
        }
        q.tex = u16(f.tex);
        q.tint = tintForBlock(blockName, props, f.tintIndex);
        q.emission = e.lightEmission;
        q.layer = texClass[f.tex];
        q.cull = f.cull >= 0 ? i8(dirIndex(rotVariant(kDir[f.cull], r.x, r.y))) : i8(-1);
        float shade;
        if (!e.shade)
            shade = 1.0f;
        else if (e.shadeDir >= 0)
            shade = kFaceShade[dirIndex(rotVariant(kDir[e.shadeDir], r.x, r.y))];
        else
            shade = normal.x * normal.x * 0.6f + normal.z * normal.z * 0.8f +
                    normal.y * normal.y * (normal.y > 0 ? 1.0f : 0.5f);
        q.shade = u8(std::clamp(shade, 0.0f, 1.0f) * 255.0f + 0.5f);
        return true;
    }
};

BlockRegistry::BlockRegistry() : impl_(std::make_unique<Impl>()) {}
BlockRegistry::~BlockRegistry() = default;

u32 BlockRegistry::textureCount() const { return impl_->texCount; }

const BlockVariant* BlockRegistry::variantAt(u16 id, i32 x, i32 y, i32 z) const {
    const BlockInfo& b = blocks_[id];
    if (b.variantCount < 2) return nullptr;
    wg::LegacyRandom random(wg::positionSeed(x, y, z));  // ModelBlockRenderer.tesselateBlock: state.getSeed(pos)
    u32 index = 0;
    if (b.variantMultipart) {  // MultiPartModel.collectParts: one seed for every part
        random.setSeed(random.nextLong());
        index = u32(random.nextInt(b.variantCount));
    } else {  // WeightedVariants: WeightedList.getRandom
        i32 total = 0;
        for (u32 v = 0; v < b.variantCount; ++v) total += i32(variants_[b.variantFirst + v].weight);
        i32 pick = random.nextInt(total);
        while (index + 1 < b.variantCount && pick >= i32(variants_[b.variantFirst + index].weight))
            pick -= i32(variants_[b.variantFirst + index++].weight);
    }
    return &variants_[b.variantFirst + index];
}

void BlockRegistry::offset(const BlockInfo& b, i32 x, i32 z, float out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!b.offsetType) return;
    const i64 seed = wg::positionSeed(x, 0, z);
    const double h = b.maxOffsetH;
    out[0] = float(std::clamp((double(float(seed & 15) / 15.0f) - 0.5) * 0.5, -h, h));
    out[2] = float(std::clamp((double(float((seed >> 8) & 15) / 15.0f) - 0.5) * 0.5, -h, h));
    if (b.offsetType == 2) out[1] = float((double(float((seed >> 4) & 15) / 15.0f) - 1.0) * b.maxOffsetV);
}

u32 fixedTintRgb(u8 tint) {
    static constexpr u32 kFixed[TintRedstone] = {0xFFFFFF, 0x91BD59, 0x77AB2F, 0x80A755, 0x619961,
                                                 0x3F76E4, 0x208030, 0x5C3B32, 0xE0C71C};
    if (tint < TintRedstone) return kFixed[tint];
    auto channel = [](float v) { return u32(std::floor(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    if (tint < TintStem) {  // RedStoneWireBlock.COLORS
        const float power = float(tint - TintRedstone) / 15.0f;
        const float red = power * 0.6f + (power > 0.0f ? 0.4f : 0.3f);
        const float green = std::clamp(power * power * 0.7f - 0.5f, 0.0f, 1.0f), blue = std::clamp(power * power * 0.6f - 0.7f, 0.0f, 1.0f);
        return channel(red) << 16 | channel(green) << 8 | channel(blue);
    }
    if (tint < TintCount) {  // BlockTintSources.stem
        const u32 age = tint - TintStem;
        return (age * 32) << 16 | (255 - age * 8) << 8 | (age * 4);
    }
    return 0xFFFFFF;
}
u16 BlockRegistry::textureLayer(std::string_view name) const {
    auto it = impl_->texIndex.find(std::string(name));
    return it == impl_->texIndex.end() ? 0 : it->second;
}

bool BlockRegistry::textureRemap(u64 tick, std::vector<u32>& remap) const {
    const Impl& im = *impl_;
    if (im.animations.empty()) return false;
    if (remap.size() != im.texCount) {
        remap.resize(im.texCount);
        for (u32 i = 0; i < im.texCount; ++i) remap[i] = i;
    }
    for (const Impl::Animation& a : im.animations) {
        u64 t = tick % std::max<u32>(1, a.totalTicks);
        for (const auto& [frame, ticks] : a.sequence) {
            if (t < ticks) {
                remap[a.layer] = a.firstFrameLayer + frame;
                break;
            }
            t -= ticks;
        }
    }
    return true;
}
std::vector<std::string> BlockRegistry::textureNames() const {
    std::vector<std::string> names(impl_->texCount);
    for (const auto& [name, layer] : impl_->texIndex)
        if (layer < names.size()) names[layer] = name;
    return names;
}
const std::vector<u8>& BlockRegistry::texturePixels() const { return impl_->pixels; }

fs::path findMinecraftJar(const fs::path& minecraftDir, std::string* versionOut) {
    fs::path versions = minecraftDir / "versions";
    std::vector<u8> bytes;
    if (readFile(versions / "version_manifest_v2.json", bytes)) {
        json j = json::parse(bytes.begin(), bytes.end(), nullptr, false);
        const json& latest = j.is_object() ? member(j, "latest") : j;
        if (latest.is_object() && member(latest, "release").is_string()) {
            std::string rel = member(latest, "release").get<std::string>();
            fs::path jar = versions / rel / (rel + ".jar");
            if (fs::exists(jar)) {
                if (versionOut) *versionOut = rel;
                return jar;
            }
        }
    }
    fs::path best;
    fs::file_time_type bestTime{};
    std::error_code ec;
    for (const auto& dir : fs::directory_iterator(versions, ec)) {
        fs::path jar = dir.path() / (dir.path().filename().string() + ".jar");
        if (!fs::exists(jar)) continue;
        auto t = fs::last_write_time(jar, ec);
        if (best.empty() || t > bestTime) best = jar, bestTime = t;
    }
    if (versionOut && !best.empty()) *versionOut = best.parent_path().filename().string();
    return best;
}

int gameDataVersion(const fs::path& jar) {
    ZipArchive zip;
    if (!zip.open(jar)) return 0;
    std::vector<u8> bytes;
    for (const auto& e : zip.entries())
        if (e.name == "version.json" && zip.read(e, bytes)) {
            json j = json::parse(bytes.begin(), bytes.end(), nullptr, false);
            return j.is_object() ? j.value("world_version", 0) : 0;
        }
    return 0;
}

namespace {
struct TexFile {
    std::string key, path;
};
// Block textures: the block atlas's sprite sources, merged over every pack (atlases/blocks.json); sorted by key.
std::vector<TexFile> blockTextureFiles(const AssetSource& src, const std::vector<std::string>& namespaces) {
    auto key = [](const std::string& ns, std::string_view path) {  // "block/stone", "mod:block/x"
        return ns == "minecraft" ? std::string(path) : ns + ":" + std::string(path);
    };
    std::vector<json> sources;
    src.readAll("assets/minecraft/atlases/blocks.json", [&](size_t, const std::vector<u8>& bytes) {
        const json j = json::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (j.is_object() && member(j, "sources").is_array())
            for (const json& source : member(j, "sources")) sources.push_back(source);
    });
    if (sources.empty()) sources.push_back(json{{"type", "directory"}, {"source", "block"}, {"prefix", "block/"}});
    StrMap<std::string> texPaths;  // sprite key -> file
    for (const json& source : sources) {
        if (!source.is_object()) continue;
        const std::string type = stripNs(source.value("type", std::string()));
        if (type == "directory") {
            const std::string dirName = source.value("source", std::string()), prefix = source.value("prefix", std::string());
            for (const std::string& ns : namespaces) {
                const std::string dir = "assets/" + ns + "/textures/" + dirName + "/";
                for (const std::string& f : src.list(dir, ".png"))
                    texPaths[key(ns, prefix + f.substr(dir.size(), f.size() - dir.size() - 4))] = f;
            }
        } else if (type == "single") {
            const std::string res = source.value("resource", std::string());
            const std::string sprite = source.value("sprite", res);
            const size_t colon = res.find(':');
            const std::string ns = colon == std::string::npos ? "minecraft" : res.substr(0, colon);
            const std::string file =
                "assets/" + ns + "/textures/" + res.substr(colon == std::string::npos ? 0 : colon + 1) + ".png";
            if (src.exists(file)) texPaths[stripNs(sprite)] = file;
        }
    }
    std::vector<TexFile> texFiles;
    texFiles.reserve(texPaths.size());
    for (auto& [k, f] : texPaths) texFiles.push_back({k, f});
    std::sort(texFiles.begin(), texFiles.end(), [](const TexFile& a, const TexFile& b) { return a.key < b.key; });
    return texFiles;
}

struct DecodedTexture {
    std::vector<u8> rgba;                       // 16x16 RGBA: the sequence's first frame
    std::vector<std::vector<u8>> frames;        // 16x16 RGBA per strip frame (animated textures)
    std::vector<std::pair<u16, u16>> sequence;  // (frame, ticks); empty: not animated
};
// One block texture resampled into the 16x16 layers. Animated textures are strips of frames (.mcmeta).
void decodeBlockTexture(const AssetSource& src, const std::string& path, DecodedTexture& out) {
    std::vector<u8> bytes;
    if (!src.read(path, bytes)) return;
    int w, h, n;
    u8* img = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
    if (!img) return;
    // Animated textures are strips of frames (.mcmeta): the sequence's first frame is the texture's layer.
    int fw = w, fh = std::min(w, h);
    json animJson;
    if (src.read(path + ".mcmeta", bytes)) {
        const json meta = json::parse(bytes.begin(), bytes.end(), nullptr, false);
        const json& anim = meta.is_object() ? member(meta, "animation") : meta;
        if (anim.is_object()) {
            const int sw = int(num(member(anim, "width"), -1)), sh = int(num(member(anim, "height"), -1));
            if (sw > 0 || sh > 0) fw = sw > 0 ? sw : sh, fh = sh > 0 ? sh : fw;
            animJson = anim;
        }
    }
    const int cols = std::max(1, w / std::max(1, fw)), rows = std::max(1, h / std::max(1, fh));
    const int count = cols * rows;
    std::vector<std::pair<u16, u16>> sequence;
    if (animJson.is_object()) {
        const int frametime = std::max(1, int(num(member(animJson, "frametime"), 1)));
        if (const json& frames = member(animJson, "frames"); frames.is_array() && !frames.empty()) {
            for (const json& f : frames) {
                const int index = f.is_object() ? int(num(member(f, "index"), 0)) : int(num(f, 0));
                const int time = f.is_object() ? std::max(1, int(num(member(f, "time"), float(frametime)))) : frametime;
                sequence.push_back({u16(std::clamp(index, 0, count - 1)), u16(time)});
            }
        } else {
            for (int k = 0; k < count; ++k) sequence.push_back({u16(k), u16(frametime)});
        }
    }
    const int first = sequence.empty() ? 0 : sequence[0].first;
    auto resample = [&](int frame, std::vector<u8>& out) {
        const int fx = (frame % cols) * fw, fy = (frame / cols) * fh;
        if (fw <= 0 || fh <= 0 || fx + fw > w || fy + fh > h) return;
        // Into the 16x16 layers: box filter (alpha weighted) for high-resolution packs, nearest below 16.
        out.resize(kTexSize * kTexSize * 4);
        for (int y = 0; y < kTexSize; ++y)
            for (int x = 0; x < kTexSize; ++x) {
                const int x0 = fx + x * fw / kTexSize, x1 = std::max(x0 + 1, fx + (x + 1) * fw / kTexSize);
                const int y0 = fy + y * fh / kTexSize, y1 = std::max(y0 + 1, fy + (y + 1) * fh / kTexSize);
                float c[3] = {}, plain[3] = {}, a = 0;
                int count = 0;
                for (int yy = y0; yy < y1; ++yy)
                    for (int xx = x0; xx < x1; ++xx, ++count) {
                        const u8* p = img + (size_t(yy) * w + xx) * 4;
                        for (int k = 0; k < 3; ++k) c[k] += p[k] * (p[3] / 255.0f), plain[k] += p[k];
                        a += p[3];
                    }
                u8* o = &out[(y * kTexSize + x) * 4];
                const float wa = a / 255.0f;
                for (int k = 0; k < 3; ++k)
                    o[k] = u8(std::clamp(wa > 0 ? c[k] / wa : plain[k] / float(count), 0.0f, 255.0f) + 0.5f);
                o[3] = u8(a / float(count) + 0.5f);
            }
    };
    resample(first, out.rgba);
    if (sequence.size() > 1 && count > 1) {
        out.frames.resize(size_t(count));
        for (int k = 0; k < count; ++k) resample(k, out.frames[size_t(k)]);
        out.sequence = std::move(sequence);
    }
    stbi_image_free(img);
}
}  // namespace

bool BlockRegistry::init(const fs::path& jarPath, const GameReports* reports) {
    reports_ = reports;
    // Game assets with the enabled resource packs (and mods' assets) layered over them.
    const AssetSource& src = AssetSource::client(jarPath);
    if (src.empty()) {
        LOG("error: cannot read %s", jarPath.string().c_str());
        return false;
    }
    const std::vector<std::string> namespaces = src.namespaces("assets");
    auto key = [](const std::string& ns, std::string_view path) {  // "block/stone", "mod:block/x"
        return ns == "minecraft" ? std::string(path) : ns + ":" + std::string(path);
    };
    const std::vector<TexFile> texFiles = blockTextureFiles(src, namespaces);
    // Block models and blockstates of every namespace (mods' blocks too).
    struct JsonFile {
        std::string path, key;
        bool blockstate;
    };
    std::vector<JsonFile> jsonFiles;
    for (const std::string& ns : namespaces) {
        const std::string models = "assets/" + ns + "/models/", states = "assets/" + ns + "/blockstates/";
        for (const std::string& f : src.list(ns == "minecraft" ? models + "block/" : models, ".json")) {
            const std::string_view rel = std::string_view(f).substr(models.size());
            if (ns != "minecraft" && rel.starts_with("item/")) continue;
            jsonFiles.push_back({f, key(ns, rel.substr(0, rel.size() - 5)), false});
        }
        for (const std::string& f : src.list(states, ".json"))
            jsonFiles.push_back(
                {f, key(ns, std::string_view(f).substr(states.size(), f.size() - states.size() - 5)), true});
    }

    // Decode textures and parse JSON in parallel. Animated textures keep every frame of their strip.
    std::vector<DecodedTexture> decoded(texFiles.size());
    std::vector<json> parsed(jsonFiles.size());
    Jobs::parallelFor(texFiles.size() + jsonFiles.size(), [&](size_t i) {
        if (i < texFiles.size()) return decodeBlockTexture(src, texFiles[i].path, decoded[i]);
        std::vector<u8> bytes;
        const size_t j = i - texFiles.size();
        if (src.read(jsonFiles[j].path, bytes)) parsed[j] = json::parse(bytes.begin(), bytes.end(), nullptr, false);
    });

    Impl& im = *impl_;
    // Layer 0 is the "missing texture" checkerboard. The frames of animated textures follow the textures, as far
    // as the texture array's limit allows.
    u32 layers = 1;
    for (const auto& t : decoded) layers += !t.rgba.empty();
    constexpr u32 kMaxLayers = 2048;  // D3D12 texture array limit
    u32 extra = 0;
    for (size_t i = 0; i < decoded.size(); ++i) {
        if (decoded[i].rgba.empty() || decoded[i].frames.empty()) continue;
        if (layers + extra + decoded[i].frames.size() > kMaxLayers) {
            decoded[i].frames.clear(), decoded[i].sequence.clear();
            continue;
        }
        extra += u32(decoded[i].frames.size());
    }
    const u32 mainLayers = layers;
    layers += extra;
    im.texCount = layers;
    im.pixels.assign(size_t(layers) * kTexLayerBytes, 0);
    im.texClass.assign(layers, LayerOpaque);
    for (int y = 0; y < kTexSize; ++y)
        for (int x = 0; x < kTexSize; ++x) {
            u8* p = &im.pixels[(y * kTexSize + x) * 4];
            bool magenta = ((x / 8) ^ (y / 8)) & 1;
            p[0] = magenta ? 248 : 0, p[1] = 0, p[2] = magenta ? 248 : 0, p[3] = 255;
        }
    buildMips(im.pixels.data());
    u32 layer = 1, extraLayer = mainLayers;
    im.animations.clear();
    for (size_t i = 0; i < decoded.size(); ++i) {
        if (decoded[i].rgba.empty()) continue;
        im.texIndex.emplace(texFiles[i].key, u16(layer));
        u8* base = &im.pixels[size_t(layer) * kTexLayerBytes];
        std::memcpy(base, decoded[i].rgba.data(), decoded[i].rgba.size());
        bool cut = false, partial = false;
        for (int p = 0; p < kTexSize * kTexSize; ++p) {
            u8 a = base[p * 4 + 3];
            cut |= a < 8;
            partial |= a >= 8 && a <= 247;
        }
        im.texClass[layer] = partial ? LayerTranslucent : cut ? LayerCutout : LayerOpaque;
        buildMips(base, im.texClass[layer] == LayerCutout);
        if (!decoded[i].frames.empty()) {
            Impl::Animation a{layer, extraLayer, {}, 0};
            for (const auto& frame : decoded[i].frames) {
                u8* fb = &im.pixels[size_t(extraLayer) * kTexLayerBytes];
                if (!frame.empty()) std::memcpy(fb, frame.data(), frame.size());
                buildMips(fb, im.texClass[layer] == LayerCutout);
                im.texClass[extraLayer] = im.texClass[layer];
                ++extraLayer;
            }
            a.sequence = std::move(decoded[i].sequence);
            for (const auto& [f, t] : a.sequence) a.totalTicks += t;
            im.animations.push_back(std::move(a));
        }
        ++layer;
    }

    for (size_t j = 0; j < jsonFiles.size(); ++j) {
        if (parsed[j].is_discarded() || !parsed[j].is_object()) continue;
        (jsonFiles[j].blockstate ? im.blockstateJson : im.modelJson).emplace(jsonFiles[j].key, std::move(parsed[j]));
    }

    blocks_.clear();
    blocks_.reserve(0x10000);
    quads_.clear();
    boxes_.clear();
    names_.clear();
    blocks_.push_back(BlockInfo{});  // id 0 = air
    blocks_[0].replaceable = true;
    names_.push_back("minecraft:air");
    blockCount_ = 1;
    im.ids.emplace("minecraft:air", u16(0));
    waterId_ = idFor("minecraft:water");
    return true;
}

bool BlockRegistry::reloadTextures() {
    // The same layers (models, meshes and render layers keep pointing at them) with the pixels the asset stack
    // has now: a texture a pack changes is redrawn, one it adds is not used until a restart.
    const AssetSource& src = AssetSource::client();
    if (src.empty()) return false;
    const std::vector<TexFile> texFiles = blockTextureFiles(src, src.namespaces("assets"));
    Impl& im = *impl_;
    std::lock_guard lock(im.mutex);
    std::vector<std::pair<const TexFile*, u16>> todo;
    for (const TexFile& f : texFiles)
        if (auto it = im.texIndex.find(f.key); it != im.texIndex.end()) todo.push_back({&f, it->second});
    std::vector<DecodedTexture> decoded(todo.size());
    Jobs::parallelFor(todo.size(), [&](size_t i) { decodeBlockTexture(src, todo[i].first->path, decoded[i]); });
    size_t changed = 0;
    for (size_t i = 0; i < todo.size(); ++i) {
        const DecodedTexture& d = decoded[i];
        if (d.rgba.empty()) continue;
        const u32 layer = todo[i].second;
        u8* base = &im.pixels[size_t(layer) * kTexLayerBytes];
        if (std::memcmp(base, d.rgba.data(), d.rgba.size()) != 0) ++changed;
        std::memcpy(base, d.rgba.data(), d.rgba.size());
        buildMips(base, im.texClass[layer] == LayerCutout);
        for (const Impl::Animation& a : im.animations) {  // the strip's frames, as far as the old strip had them
            if (a.layer != layer) continue;
            u32 frames = 0;
            for (const auto& [f, t] : a.sequence) frames = std::max<u32>(frames, f + 1u);
            for (u32 k = 0; k < frames && k < d.frames.size(); ++k) {
                if (d.frames[k].empty()) continue;
                u8* fb = &im.pixels[size_t(a.firstFrameLayer + k) * kTexLayerBytes];
                std::memcpy(fb, d.frames[k].data(), d.frames[k].size());
                buildMips(fb, im.texClass[layer] == LayerCutout);
            }
        }
    }
    LOG("assets: block textures reloaded (%zu of %zu changed)", changed, todo.size());
    return true;
}

u16 BlockRegistry::idFor(std::string_view key) {
    thread_local StrMap<u16> cache;
    thread_local const BlockRegistry* cacheOwner = nullptr;
    if (cacheOwner != this) {
        cache.clear();
        cacheOwner = this;
    }
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    u16 id;
    {
        std::lock_guard lock(impl_->mutex);
        auto it = impl_->ids.find(key);
        if (it != impl_->ids.end()) {
            id = it->second;
        } else {
            // Partial keys ("minecraft:oak_leaves") resolve to the full default state, so every
            // spelling of a state shares one id.
            const std::string canonical = reports_ ? reports_->complete(key) : std::string(key);
            auto c = impl_->ids.find(canonical);
            if (c != impl_->ids.end()) {
                id = c->second;
            } else {
                id = create(canonical);
                impl_->ids.emplace(canonical, id);
            }
            if (canonical != key) impl_->ids.emplace(std::string(key), id);
        }
    }
    cache.emplace(std::string(key), id);
    return id;
}

const std::string& BlockRegistry::name(u16 id) const { return names_[id < blockCount_ ? id : 0]; }

std::string_view BlockRegistry::property(u16 id, std::string_view key) const {
    std::string_view n = name(id);
    size_t b = n.find('[');
    if (b == std::string_view::npos) return {};
    n = n.substr(b + 1, n.size() - b - 2);
    while (!n.empty()) {
        size_t comma = n.find(',');
        std::string_view kv = n.substr(0, comma);
        size_t eq = kv.find('=');
        if (eq != std::string_view::npos && kv.substr(0, eq) == key) return kv.substr(eq + 1);
        if (comma == std::string_view::npos) break;
        n.remove_prefix(comma + 1);
    }
    return {};
}

u16 BlockRegistry::withProperty(u16 id, std::string_view key, std::string_view value) {
    // Hot in world generation (features change one property of a known state over and over): remember answers.
    thread_local std::unordered_map<u64, u16> memo;
    thread_local const BlockRegistry* memoOwner = nullptr;
    if (memoOwner != this) memo.clear(), memoOwner = this;
    u64 h = 1469598103934665603ull ^ id;  // FNV-1a over id, key and value
    for (std::string_view part : {key, std::string_view("="), value})
        for (char ch : part) h = (h ^ u8(ch)) * 1099511628211ull;
    if (auto it = memo.find(h); it != memo.end()) return it->second;
    const u16 result = withPropertyUncached(id, key, value);
    if (memo.size() < 65536) memo.emplace(h, result);
    return result;
}

u16 BlockRegistry::withPropertyUncached(u16 id, std::string_view key, std::string_view value) {
    const std::string full = name(id);
    std::string_view n = full;
    size_t b = n.find('[');
    std::string_view base = n.substr(0, b);
    std::vector<std::pair<std::string_view, std::string_view>> props;
    if (b != std::string_view::npos) {
        std::string_view rest = n.substr(b + 1, n.size() - b - 2);
        while (!rest.empty()) {
            size_t comma = rest.find(',');
            std::string_view kv = rest.substr(0, comma);
            size_t eq = kv.find('=');
            if (eq != std::string_view::npos && kv.substr(0, eq) != key)
                props.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
    }
    props.push_back({key, value});
    std::sort(props.begin(), props.end());
    std::string k(base);
    k += '[';
    for (size_t i = 0; i < props.size(); ++i) {
        if (i) k += ',';
        k += props[i].first;
        k += '=';
        k += props[i].second;
    }
    k += ']';
    u16 nid = idFor(k);
    // A combination the block does not define resolves to nothing: keep the original state.
    return infos()[nid].kind == KindAir && infos()[id].kind != KindAir ? id : nid;
}

namespace {

// Light emitted by a block state (the game hard-codes these in its block classes).
u8 emissionFor(std::string_view n, const Props& props) {
    auto prop = [&](std::string_view k) -> std::string_view {
        for (const auto& [pk, pv] : props)
            if (pk == k) return pv;
        return {};
    };
    auto num = [&](std::string_view k) { return std::atoi(std::string(prop(k)).c_str()); };
    const bool lit = prop("lit") == "true";
    if (n == "lava" || n == "fire" || n == "glowstone" || n == "sea_lantern" || n == "shroomlight" ||
        n == "jack_o_lantern" || n == "lantern" || n == "beacon" || n == "conduit" || n == "end_portal" ||
        n == "end_gateway" || n.ends_with("froglight"))
        return 15;
    if (n == "torch" || n == "wall_torch" || n == "end_rod") return 14;
    if (n == "nether_portal") return 11;
    if (n == "soul_torch" || n == "soul_wall_torch" || n == "soul_lantern" || n == "soul_fire" ||
        n == "crying_obsidian")
        return 10;
    if (n == "glow_lichen" || n == "enchanting_table" || n == "ender_chest") return 7;
    if (n == "magma_block") return 3;
    if (n == "amethyst_cluster") return 5;
    if (n == "large_amethyst_bud") return 4;
    if (n == "medium_amethyst_bud") return 2;
    if (n == "small_amethyst_bud" || n == "brewing_stand" || n == "dragon_egg" || n == "sculk_sensor") return 1;
    if (n == "light") return prop("level").empty() ? 15 : u8(num("level"));
    if ((n == "cave_vines" || n == "cave_vines_plant") && prop("berries") == "true") return 14;
    if (n == "sea_pickle" && prop("waterlogged") == "true") return u8(std::min(15, 3 + 3 * num("pickles")));
    if (n.ends_with("candle") && lit) return u8(std::min(15, 3 * num("candles")));
    if (!lit) return 0;
    if (n == "campfire" || n == "redstone_lamp" || n.ends_with("copper_bulb")) return 15;
    if (n == "soul_campfire") return 10;
    if (n == "furnace" || n == "smoker" || n == "blast_furnace") return 13;
    if (n == "redstone_torch" || n == "redstone_wall_torch" || n == "redstone_ore" || n == "deepslate_redstone_ore")
        return 7;
    return 0;
}

// Blocks that have a model but no collision (plants, torches, rails, signs...).
bool passable(std::string_view n) {
    static const char* kParts[] = {"grass", "fern", "flower", "tulip", "sapling", "torch", "rail", "button",
                                   "pressure_plate", "sign", "banner", "wire", "tripwire", "vine", "lichen",
                                   "kelp", "seagrass", "coral", "mushroom", "roots", "dead_bush", "sugar_cane",
                                   "wheat", "carrots", "potatoes", "beetroots", "nether_wart", "berry_bush",
                                   "cobweb", "fire", "lever", "dandelion", "poppy", "orchid", "allium", "bluet",
                                   "daisy", "cornflower", "lily_of_the_valley", "wither_rose", "sunflower",
                                   "lilac", "rose_bush", "peony", "bush", "spore_blossom", "pitcher",
                                   "torchflower", "petals", "leaf_litter", "sculk_vein", "stem", "frogspawn",
                                   "light", "structure_void", "portal", "string"};
    if (n == "grass_block" || n.ends_with("_mushroom_block") || n == "mushroom_stem" || n.ends_with("glass") ||
        n.ends_with("glass_pane") || n.ends_with("lantern") || n == "redstone_lamp" || n.ends_with("_wall") ||
        n == "lightning_rod" || n.ends_with("light_weighted_pressure_plate_never"))
        return false;
    for (const char* p : kParts)
        if (n.find(p) != std::string_view::npos) return true;
    return false;
}

bool replaceableName(std::string_view n, const Props& props) {
    if (n == "short_grass" || n == "grass" || n == "tall_grass" || n == "fern" || n == "large_fern" ||
        n == "dead_bush" || n == "vine" || n == "glow_lichen" || n == "seagrass" || n == "tall_seagrass" ||
        n == "fire" || n == "soul_fire" || n == "structure_void" || n == "light" || n == "bush" ||
        n == "short_dry_grass" || n == "tall_dry_grass" || n == "leaf_litter" || n == "hanging_roots")
        return true;
    if (n == "snow")
        for (const auto& [k, v] : props)
            if (k == "layers") return v == "1";
    return false;
}

Box elementBox(const Element& e, const ModelRef& r) {
    Box b{{1, 1, 1}, {0, 0, 0}};
    for (int i = 0; i < 8; ++i) {
        F3 p{(i & 1) ? e.to.x : e.from.x, (i & 2) ? e.to.y : e.from.y, (i & 4) ? e.to.z : e.from.z};
        p = applyElementRotation(e, p * (1.0f / 16.0f), false);
        p = rotVariant(p - F3{0.5f, 0.5f, 0.5f}, r.x, r.y) + F3{0.5f, 0.5f, 0.5f};
        for (int a = 0; a < 3; ++a) {
            b.min[a] = std::min(b.min[a], std::clamp(p[a], 0.0f, 1.0f));
            b.max[a] = std::max(b.max[a], std::clamp(p[a], 0.0f, 1.0f));
        }
    }
    return b;
}

}  // namespace

u16 BlockRegistry::create(std::string_view key) {
    std::string_view name = key;
    Props props;
    if (size_t b = key.find('['); b != std::string_view::npos) {
        name = key.substr(0, b);
        std::string_view rest = key.substr(b + 1);
        if (!rest.empty() && rest.back() == ']') rest.remove_suffix(1);
        while (!rest.empty()) {
            size_t comma = rest.find(',');
            std::string_view kv = rest.substr(0, comma);
            size_t eq = kv.find('=');
            if (eq != std::string_view::npos) props.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
    }
    std::string shortName = stripNs(name);
    if (shortName == "air" || shortName == "cave_air" || shortName == "void_air") return 0;
    if (blocks_.size() >= 0xFFFF) return 0;

    Impl& im = *impl_;
    BlockInfo bi;
    bi.cullGroup = im.cullGroup(shortName);
    auto propIs = [&](std::string_view k, std::string_view v) {
        for (const auto& [pk, pv] : props)
            if (pk == k) return pv == v;
        return false;
    };

    auto fluidCube = [&](const char* texName, u8 tint, std::string_view group) {
        auto it = im.texIndex.find(texName);
        u16 tex = it == im.texIndex.end() ? 0 : it->second;
        bi.kind = KindCube;
        bi.fluid = true;
        bi.layer = im.texClass[tex];
        bi.cullGroup = im.cullGroup(std::string(group));
        for (CubeFace& f : bi.faces) f = {tex, 0, tint};
    };
    auto addBox = [&](const Box& box, bool collision) {
        boxes_.push_back(box);
        if (collision) {
            if (!bi.collisionCount) bi.collisionFirst = u32(boxes_.size() - 1);
            ++bi.collisionCount;
        } else {
            if (!bi.selectionCount) bi.selectionFirst = u32(boxes_.size() - 1);
            ++bi.selectionCount;
        }
    };
    const Box kFull{{0, 0, 0}, {1, 1, 1}};

    if (shortName == "water" || shortName == "bubble_column") {
        fluidCube("block/water_still", TintWater, "water");
        bi.layer = LayerTranslucent;
    } else if (shortName == "lava") {
        fluidCube("block/lava_still", TintNone, "lava");
    } else if (const BlockDef* d = im.def(shortName)) {
        std::vector<const Case*> matched;
        for (const Case& c : d->cases)
            if (caseMatch(c, props)) {
                if (c.ref.model >= 0) matched.push_back(&c);
                if (!d->multipart) break;
            }
        // Weighted lists: variant v takes entry v (a multipart's parts share one seed: with power-of-two list sizes,
        // a part of n entries takes v * n / count, the same pick as its own nextInt(n)).
        u32 variants = 1;
        for (const Case* c : matched) variants = std::max(variants, u32(c->refs.size()));
        auto refsFor = [&](u32 v) {
            std::vector<ModelRef> refs;
            for (const Case* c : matched) {
                const u32 n = u32(c->refs.size());
                refs.push_back(c->refs[d->multipart ? v * n / variants : std::min(v, n - 1)]);
            }
            return refs;
        };
        auto isCube = [&](const std::vector<ModelRef>& refs) { return refs.size() == 1 && isFullCube(im.models[refs[0].model]); };
        auto cubeFaces = [&](const ModelRef& r, CubeFace out[6]) {
            const Element& e = im.models[r.model].elements[0];
            u8 layer = LayerOpaque;
            for (int t = 0; t < 6; ++t) {
                int s = 0;
                while (s < 5 && !(rotVariant(kDir[s], r.x, r.y) == kDir[t])) ++s;
                const Face& f = e.faces[s];
                int rot = 0;
                if (!r.uvlock) {
                    I3 a = rotVariant(kU[s], r.x, r.y);
                    rot = a == kU[t] ? 0 : a == kV[t] ? 1 : a == -kU[t] ? 2 : 3;
                }
                rot = (rot + f.rot) & 3;
                out[t] = {u16(f.tex), u8(rot), tintForBlock(shortName, props, f.tintIndex)};
                layer = std::max(layer, im.texClass[f.tex]);
            }
            return layer;
        };
        auto bakeQuads = [&](const std::vector<ModelRef>& refs, bool shapes) {
            u8 layer = LayerOpaque;
            const bool solid = !passable(shortName);
            for (const ModelRef& r : refs)
                for (const Element& e : im.models[r.model].elements) {
                    for (int dir = 0; dir < 6; ++dir) {
                        ModelQuad q;
                        if ((e.faceMask & (1 << dir)) && im.bakeFace(e, dir, r, shortName, props, q)) {
                            quads_.push_back(q);
                            layer = std::max(layer, q.layer);
                        }
                    }
                    if (!shapes) continue;
                    if (bi.selectionCount < 16) addBox(elementBox(e, r), false);
                    if (solid && !e.rotated && bi.collisionCount < 16) addBox(elementBox(e, r), true);
                }
            return layer;
        };
        const std::vector<ModelRef> refs = refsFor(0);
        if (isCube(refs)) {
            bi.kind = KindCube;
            bi.layer = cubeFaces(refs[0], bi.faces);
            bi.occludes = bi.layer == LayerOpaque;
            addBox(kFull, true);
            addBox(kFull, false);
        } else {
            u32 first = u32(quads_.size());
            bi.layer = bakeQuads(refs, true);
            bi.quadFirst = first;
            bi.quadCount = u32(quads_.size()) - first;
            bi.kind = bi.quadCount ? KindModel : KindAir;
        }
        if (variants > 1 && variants <= 255 && bi.kind != KindAir) {
            bi.variantFirst = u32(variants_.size());
            bi.variantCount = u8(variants);
            bi.variantMultipart = d->multipart;
            for (u32 v = 0; v < variants; ++v) {
                BlockVariant bv;
                bv.weight = d->multipart ? 1 : matched[0]->weights[std::min<size_t>(v, matched[0]->weights.size() - 1)];
                const std::vector<ModelRef> rv = refsFor(v);
                if (v == 0) {
                    std::copy(std::begin(bi.faces), std::end(bi.faces), bv.faces);
                    bv.quadFirst = bi.quadFirst, bv.quadCount = bi.quadCount;
                } else if (bi.kind == KindCube && isCube(rv)) {
                    cubeFaces(rv[0], bv.faces);
                } else if (bi.kind == KindModel && !isCube(rv)) {
                    bv.quadFirst = u32(quads_.size());
                    bakeQuads(rv, false);
                    bv.quadCount = u32(quads_.size()) - bv.quadFirst;
                } else {
                    bv = variants_[bi.variantFirst];  // a different kind of model: keep the first
                }
                variants_.push_back(bv);
            }
        }
    }
    bi.waterlogged = propIs("waterlogged", "true") || shortName == "kelp" || shortName == "kelp_plant" ||
                     shortName == "seagrass" || shortName == "tall_seagrass" || shortName == "bubble_column";
    bi.emission = emissionFor(shortName, props);
    bi.emissiveRendering = shortName == "magma_block" ||
                           ((shortName == "sculk_sensor" || shortName == "calibrated_sculk_sensor") && propIs("sculk_sensor_phase", "active"));
    const bool dims = bi.fluid || bi.waterlogged || shortName.ends_with("leaves") || shortName == "cobweb" ||
                      shortName == "ice" || shortName == "frosted_ice";
    bi.opacity = bi.occludes ? 15 : dims ? 1 : 0;
    bi.replaceable = bi.fluid || bi.kind == KindAir || replaceableName(shortName, props);
    bi.icon = bi.kind == KindCube ? bi.faces[3].tex : bi.quadCount ? quads_[bi.quadFirst].tex : 0;
    if (const BlockShapes::Entry* e = shapes_ ? shapes_->find(key) : nullptr) {  // the game's own shapes
        bi.collisionCount = bi.selectionCount = 0;
        for (const Box& b : shapes_->shape(e->outline)) addBox(b, false);
        for (const Box& b : shapes_->shape(e->collision)) addBox(b, true);
    }

    bi.leaves = shortName.ends_with("_leaves");
    // Blocks.java .offsetType(...); SpeleothemBlock / SmallDripleafBlock change the limits.
    static const std::unordered_set<std::string_view> kOffsetXZ = {
        "mangrove_propagule", "tall_seagrass", "dandelion", "golden_dandelion", "torchflower", "poppy", "blue_orchid", "allium",
        "azure_bluet", "red_tulip", "orange_tulip", "white_tulip", "pink_tulip", "oxeye_daisy", "cornflower", "wither_rose",
        "lily_of_the_valley", "sunflower", "lilac", "rose_bush", "peony", "tall_grass", "large_fern", "pitcher_plant",
        "bamboo_sapling", "bamboo", "warped_roots", "nether_sprouts", "crimson_roots", "pointed_dripstone", "sulfur_spike",
        "hanging_roots", "open_eyeblossom", "closed_eyeblossom"};
    static const std::unordered_set<std::string_view> kOffsetXYZ = {"short_grass", "fern", "short_dry_grass", "tall_dry_grass",
                                                                    "small_dripleaf"};
    bi.offsetType = kOffsetXZ.contains(shortName) ? 1 : kOffsetXYZ.contains(shortName) ? 2 : 0;
    if (shortName == "pointed_dripstone" || shortName == "sulfur_spike") bi.maxOffsetH = 0.125f;
    if (shortName == "small_dripleaf") bi.maxOffsetV = 0.1f;
    blocks_.push_back(bi);
    names_.push_back(std::string(key));
    blockCount_ = blocks_.size();
    return u16(blocks_.size() - 1);
}
