#include "ui/icons.h"

#include "ui/sprites.h"

#include <cmath>
#include <functional>

namespace ui {

namespace {
constexpr float kPi = 3.14159265f;
using json = nlohmann::json;

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float& at(V3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }

V3 rotX(V3 v, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {v.x, v.y * c - v.z * s, v.y * s + v.z * c};
}
V3 rotY(V3 v, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}
V3 rotZ(V3 v, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}
// Quaternionf.rotationXYZ(x, y, z) applied to v: Z first, then Y, then X.
V3 rotXYZ(V3 v, const float deg[3]) {
    return rotX(rotY(rotZ(v, deg[2] * kPi / 180), deg[1] * kPi / 180), deg[0] * kPi / 180);
}

// Item lights (Lighting.ITEMS_3D) in the space of the GUI display transform (x right, y up, z to the viewer).
const V3 kLight0 = {-0.93343929f, 0.26269472f, -0.24430016f};
const V3 kLight1 = {-0.10357136f, 0.97660684f, 0.18844643f};

float num(const json& v, float d) { return v.is_number() ? v.get<float>() : d; }
V3 vec(const json& v, V3 d) {
    return v.is_array() && v.size() == 3 ? V3{num(v[0], 0), num(v[1], 0), num(v[2], 0)} : d;
}
std::string strip(std::string s) {
    const size_t c = s.find(':');
    return c == std::string::npos ? s : s.substr(c + 1);
}

struct Tex {
    int w = 0, h = 0;
    std::vector<u8> rgba;  // the frame shown
    bool translucent = false;
    // Animated (.mcmeta): the strip and the frame (state) in rgba.
    bool animated = false;
    Sprites::Animation anim;
    Image strip;
    u64 shown = 0;
};

struct Face {
    std::string tex;
    float uv[4] = {};
    bool hasUv = false;
    int rot = 0, tint = -1;
};
struct Element {
    V3 from, to;
    bool rotated = false;
    V3 origin{8, 8, 8};
    float euler[3] = {};
    int axis = -1;
    float angle = 0;
    bool rescale = false;
    bool shade = true;
    Face faces[6];
    u8 mask = 0;
};
struct Model {
    bool generated = false;
    std::vector<std::string> layers;  // generated: texture per layer
    std::vector<Element> elements;
    float rot[3] = {}, trans[3] = {}, scale[3] = {1, 1, 1};
    bool sideLit = true;
    std::string particle;
};

const char* kFaceNames[6] = {"down", "up", "north", "south", "west", "east"};
const V3 kNormals[6] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
}  // namespace

struct ItemIcons::Impl {
    const Resources* res = nullptr;
    const Items* items = nullptr;
    Atlas* atlas = nullptr;
    std::unordered_map<std::string, json> jsonCache;
    std::unordered_map<std::string, Tex> texCache;
    std::unordered_map<std::string, std::shared_ptr<Model>> models;
    std::unordered_map<u64, std::array<float, 4>> icons;  // key -> uv (u0 < 0: nothing)
    // Icons drawn with animated textures: rasterized again into their atlas slot when a frame changes.
    struct AnimatedIcon {
        ItemStack stack;
        int x, y;
    };
    std::vector<AnimatedIcon> animatedIcons;
    bool touchedAnimated = false;  // set by texture() while an icon is rasterized
    Image grass, foliage;
    int size = 32;

    const json& jsonAt(const std::string& path) {
        auto it = jsonCache.find(path);
        if (it == jsonCache.end()) it = jsonCache.emplace(path, res->json(path)).first;
        return it->second;
    }

    const Tex& texture(const std::string& name) {
        const std::string key = strip(name);
        auto it = texCache.find(key);
        if (it != texCache.end()) {
            touchedAnimated |= it->second.animated;
            return it->second;
        }
        Tex& t = texCache[key];
        Image img;
        const std::string path = "assets/minecraft/textures/" + key + ".png";
        if (res->image(path, img) && img.w > 0) {
            if (res->exists(path + ".mcmeta") && Sprites::parseAnimation(img, res->json(path + ".mcmeta"), t.anim)) {
                t.animated = touchedAnimated = true;
                t.w = t.anim.fw, t.h = t.anim.fh;
                t.anim.frameAt(img, 0, t.rgba);
                t.shown = t.anim.stateAt(0);
                for (size_t i = 3; i < img.rgba.size(); i += 4) t.translucent |= img.rgba[i] > 25 && img.rgba[i] < 230;
                t.strip = std::move(img);
                return t;
            }
            t.w = img.w, t.h = std::min(img.h, img.w);  // strips without .mcmeta: first frame
            t.rgba.assign(img.rgba.begin(), img.rgba.begin() + size_t(t.w) * t.h * 4);
            for (size_t i = 3; i < t.rgba.size(); i += 4) t.translucent |= t.rgba[i] > 25 && t.rgba[i] < 230;
        }
        return t;
    }

    std::shared_ptr<Model> model(const std::string& rawName) {
        const std::string name = strip(rawName);
        if (auto it = models.find(name); it != models.end()) return it->second;
        auto m = std::make_shared<Model>();
        models[name] = m;
        std::unordered_map<std::string, std::string> vars;
        const json* elements = nullptr;
        bool haveDisplay = false, haveLight = false;
        std::string cur = name;
        for (int guard = 0; guard < 32 && !cur.empty(); ++guard) {
            if (cur == "builtin/generated") {
                m->generated = true;
                break;
            }
            const json& j = jsonAt("assets/minecraft/models/" + cur + ".json");
            if (!j.is_object()) break;
            if (j.contains("textures") && j["textures"].is_object())
                for (const auto& t : j["textures"].items()) {
                    std::string v = t.value().is_string() ? t.value().get<std::string>()
                                    : t.value().is_object() ? t.value().value("sprite", "") : "";
                    vars.emplace(t.key(), v);
                }
            if (!elements && j.contains("elements") && j["elements"].is_array()) elements = &j["elements"];
            if (!haveDisplay && j.contains("display") && j["display"].contains("gui")) {
                const json& g = j["display"]["gui"];
                const V3 r = vec(g.value("rotation", json()), {}), t = vec(g.value("translation", json()), {}),
                         s = vec(g.value("scale", json()), {1, 1, 1});
                m->rot[0] = r.x, m->rot[1] = r.y, m->rot[2] = r.z;
                m->trans[0] = std::clamp(t.x, -80.f, 80.f), m->trans[1] = std::clamp(t.y, -80.f, 80.f);
                m->trans[2] = std::clamp(t.z, -80.f, 80.f);
                m->scale[0] = std::clamp(s.x, -4.f, 4.f), m->scale[1] = std::clamp(s.y, -4.f, 4.f);
                m->scale[2] = std::clamp(s.z, -4.f, 4.f);
                haveDisplay = true;
            }
            if (!haveLight && j.contains("gui_light")) {
                m->sideLit = j.value("gui_light", "side") == "side";
                haveLight = true;
            }
            cur = j.contains("parent") && j["parent"].is_string() ? strip(j["parent"].get<std::string>()) : "";
        }
        auto resolve = [&](std::string ref) {
            for (int g = 0; g < 16 && !ref.empty() && ref[0] == '#'; ++g) {
                auto it = vars.find(ref.substr(1));
                ref = it == vars.end() ? "" : it->second;
            }
            return ref;
        };
        m->particle = resolve("#particle");
        if (m->generated) {
            for (int i = 0;; ++i) {
                auto it = vars.find("layer" + std::to_string(i));
                if (it == vars.end()) break;
                m->layers.push_back(resolve(it->second));
            }
            return m;
        }
        if (elements)
            for (const json& ej : *elements) {
                Element e;
                e.from = vec(ej.value("from", json()), {0, 0, 0});
                e.to = vec(ej.value("to", json()), {16, 16, 16});
                if (ej.contains("rotation") && ej["rotation"].is_object()) {
                    const json& r = ej["rotation"];
                    e.rotated = true;
                    e.origin = vec(r.value("origin", json()), {8, 8, 8});
                    if (r.contains("axis")) {
                        const std::string a = r.value("axis", "y");
                        e.axis = a == "x" ? 0 : a == "y" ? 1 : 2;
                        e.angle = num(r.value("angle", json()), 0);
                        e.rescale = r.value("rescale", false);
                    } else {
                        e.euler[0] = num(r.value("x", json()), 0), e.euler[1] = num(r.value("y", json()), 0);
                        e.euler[2] = num(r.value("z", json()), 0);
                    }
                }
                e.shade = ej.value("shade", true);
                if (ej.contains("faces"))
                    for (int d = 0; d < 6; ++d) {
                        if (!ej["faces"].contains(kFaceNames[d])) continue;
                        const json& fj = ej["faces"][kFaceNames[d]];
                        Face& f = e.faces[d];
                        f.tex = resolve(fj.value("texture", ""));
                        if (fj.contains("uv") && fj["uv"].is_array() && fj["uv"].size() == 4) {
                            f.hasUv = true;
                            for (int k = 0; k < 4; ++k) f.uv[k] = num(fj["uv"][k], 0);
                        }
                        f.rot = ((int(num(fj.value("rotation", json()), 0)) / 90) % 4 + 4) % 4;
                        f.tint = int(num(fj.value("tintindex", json()), -1));
                        e.mask |= u8(1 << d);
                    }
                m->elements.push_back(e);
            }
        return m;
    }

    // --- item definition -> models ------------------------------------------------------------------
    struct Part {
        std::string model;
        std::vector<u32> tints;  // ARGB per tint index
        std::string special;     // "chest:<texture>", "shulker_box:<texture>"
    };

    u32 colormap(const Image& img, float temp, float down) {
        if (img.w < 256 || img.h < 256) return 0xFF7FB238;
        temp = std::clamp(temp, 0.f, 1.f);
        down = std::clamp(down, 0.f, 1.f) * temp;
        const int x = int((1.0f - temp) * 255.0f), y = int((1.0f - down) * 255.0f);
        const u8* p = img.at(x, y);
        return argb(255, p[0], p[1], p[2]);
    }

    u32 tint(const json& t, const ItemStack& s) {
        const std::string type = strip(t.value("type", ""));
        auto def = [&](const char* k, u32 d) { return t.contains(k) && t[k].is_number() ? u32(t[k].get<i64>()) : d; };
        if (type == "constant") return def("value", 0xFFFFFFFF);
        if (type == "dye") return s.dyedColor >= 0 ? 0xFF000000 | u32(s.dyedColor) : def("default", 0xFFFFFFFF);
        if (type == "potion") return s.potionColor >= 0 ? 0xFF000000 | u32(s.potionColor) : def("default", 0xFFFFFFFF);
        if (type == "grass") return colormap(grass, num(t.value("temperature", json()), 0.5f), num(t.value("downfall", json()), 1.f));
        if (type == "foliage") return colormap(foliage, num(t.value("temperature", json()), 0.5f), num(t.value("downfall", json()), 1.f));
        return def("default", 0xFFFFFFFF);
    }

    bool hasComponent(const ItemStack& s, const std::string& name) {
        const int type = items->componentType(strip(name));
        for (const auto& c : s.added)
            if (c.type == type) return true;
        return false;
    }

    void evaluate(const json& n, const ItemStack& s, std::vector<Part>& out, int depth) {
        if (!n.is_object() || depth > 16) return;
        const std::string type = strip(n.value("type", ""));
        if (type == "model") {
            Part p{n.value("model", ""), {}, {}};
            if (n.contains("tints"))
                for (const json& t : n["tints"]) p.tints.push_back(tint(t, s));
            out.push_back(std::move(p));
        } else if (type == "composite") {
            if (n.contains("models"))
                for (const json& m : n["models"]) evaluate(m, s, out, depth + 1);
        } else if (type == "condition") {
            bool value = false;
            const std::string prop = strip(n.value("property", ""));
            if (prop == "has_component") value = hasComponent(s, n.value("component", ""));
            else if (prop == "damaged") value = s.damage > 0;
            evaluate(n.value(value ? "on_true" : "on_false", json()), s, out, depth + 1);
        } else if (type == "select") {
            if (n.contains("fallback")) evaluate(n["fallback"], s, out, depth + 1);
            else if (n.contains("cases") && !n["cases"].empty()) evaluate(n["cases"][0].value("model", json()), s, out, depth + 1);
        } else if (type == "range_dispatch") {
            float value = 0;
            if (strip(n.value("property", "")) == "damage") {
                const int md = items->maxDamage(s);
                value = md > 0 ? float(s.damage) / float(md) : 0.f;
            }
            value *= num(n.value("scale", json()), 1.f);
            const json* best = nullptr;
            float bestT = -1e30f;
            if (n.contains("entries"))
                for (const json& e : n["entries"]) {
                    const float t = num(e.value("threshold", json()), 0);
                    if (t <= value && t >= bestT) bestT = t, best = &e;
                }
            if (best) evaluate((*best).value("model", json()), s, out, depth + 1);
            else if (n.contains("fallback")) evaluate(n["fallback"], s, out, depth + 1);
            else if (n.contains("entries") && !n["entries"].empty()) evaluate(n["entries"][0].value("model", json()), s, out, depth + 1);
        } else if (type == "special") {
            Part p{n.value("base", ""), {}, {}};
            const json& sm = n.value("model", json());
            const std::string st = strip(sm.value("type", ""));
            if (st == "chest") p.special = "chest:" + strip(sm.value("texture", "normal"));
            if (st == "shulker_box") p.special = "shulker_box:" + strip(sm.value("texture", "shulker"));
            if (st == "head") p.special = "head:" + sm.value("kind", "skeleton");
            if (st == "player_head") p.special = "head:player";
            if (st == "shield") p.special = "shield:";
            if (st == "banner") {
                p.special = "banner:" + sm.value("color", "white");
                static const std::pair<const char*, u32> kDye[] = {
                    {"white", 0xF9FFFE}, {"orange", 0xF9801D}, {"magenta", 0xC74EBD}, {"light_blue", 0x3AB3DA},
                    {"yellow", 0xFED83D}, {"lime", 0x80C71F}, {"pink", 0xF38BAA}, {"gray", 0x474F52},
                    {"light_gray", 0x9D9D97}, {"cyan", 0x169C9C}, {"purple", 0x8932B8}, {"blue", 0x3C44AA},
                    {"brown", 0x835432}, {"green", 0x5E7C16}, {"red", 0xB02E26}, {"black", 0x1D1D21}};
                u32 color = 0xFFFFFF;
                for (auto [n, c] : kDye)
                    if (sm.value("color", "white") == n) color = c;
                p.tints.push_back(0xFF000000 | color);
            }
            out.push_back(std::move(p));
        }
    }

    // --- rasterizer ------------------------------------------------------------------------------------
    struct Raster {
        int n = 0;
        std::vector<float> depth;
        std::vector<float> color;  // rgba 0..1, straight alpha
        void init(int size) {
            n = size;
            depth.assign(size_t(n) * n, -1e30f);
            color.assign(size_t(n) * n * 4, 0.f);
        }
        void blend(size_t i, const float c[4]) {
            float* d = &color[i * 4];
            const float a = c[3], ia = d[3] * (1 - a), oa = a + ia;
            if (oa <= 0) return;
            for (int k = 0; k < 3; ++k) d[k] = (c[k] * a + d[k] * ia) / oa;
            d[3] = oa;
        }
    };
    struct Vtx {
        float x, y, z, u, v;  // pixel coordinates; texel coordinates
    };

    static void triangle(Raster& r, const Vtx& a, const Vtx& b, const Vtx& c, const Tex& t, const float mul[4]) {
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::abs(area) < 1e-6f || t.w <= 0) return;
        const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
        const int x1 = std::min(r.n - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}))));
        const int y1 = std::min(r.n - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
        for (int py = y0; py <= y1; ++py)
            for (int px = x0; px <= x1; ++px) {
                const float x = float(px) + 0.5f, y = float(py) + 0.5f;
                float w0 = ((b.x - x) * (c.y - y) - (b.y - y) * (c.x - x)) / area;
                float w1 = ((c.x - x) * (a.y - y) - (c.y - y) * (a.x - x)) / area;
                float w2 = 1 - w0 - w1;
                const float eps = -1e-5f;
                if (w0 < eps || w1 < eps || w2 < eps) continue;
                const float z = a.z * w0 + b.z * w1 + c.z * w2;
                const size_t i = size_t(py) * r.n + px;
                if (z <= r.depth[i]) continue;
                const float u = a.u * w0 + b.u * w1 + c.u * w2, v = a.v * w0 + b.v * w1 + c.v * w2;
                const int tx = std::clamp(int(std::floor(u)), 0, t.w - 1), ty = std::clamp(int(std::floor(v)), 0, t.h - 1);
                const u8* p = &t.rgba[(size_t(ty) * t.w + tx) * 4];
                if (p[3] < 26) continue;  // alpha < 0.1: discarded like the item cutout shader
                const float col[4] = {p[0] / 255.f * mul[0], p[1] / 255.f * mul[1], p[2] / 255.f * mul[2], p[3] / 255.f * mul[3]};
                if (col[3] >= 0.99f) {
                    std::memcpy(&r.color[i * 4], col, sizeof(col));
                    r.depth[i] = z;
                } else {
                    r.blend(i, col);
                }
            }
    }

    // A box with entity-model UVs (ModelPart.Cube): texture offset (u, v), size w x h x d, in model pixels.
    // A box with entity-model UVs (ModelPart.Cube): texture offset (u, v), size w x h x d (texture pixels), placed
    // at `from` with `size` in model pixels.
    void entityBox(Model& m, const std::string& tex, V3 from, V3 size, int u, int v, V3 texSize = {}, int tint = -1) {
        Element e;
        e.from = from, e.to = from + size;
        if (texSize.x == 0) texSize = size;
        const float w = texSize.x, h = texSize.y, d = texSize.z;
        const Tex& t = texture(tex);
        const float su = t.w ? 16.0f / float(t.w) : 0.25f, sv = t.h ? 16.0f / float(t.h) : 0.25f;
        auto set = [&](int dir, float u0, float v0, float u1, float v1) {
            Face& f = e.faces[dir];
            f.tex = tex;
            f.hasUv = true;
            f.tint = tint;
            f.uv[0] = u0 * su, f.uv[1] = v0 * sv, f.uv[2] = u1 * su, f.uv[3] = v1 * sv;
            e.mask |= u8(1 << dir);
        };
        const float U = float(u), Vv = float(v);
        set(0, U + d + w, Vv, U + d + w + w, Vv + d);          // down
        set(1, U + d, Vv + d, U + d + w, Vv);                  // up
        // Entity models face the other way: their front (u + d) ends up on the item's south side.
        set(3, U + d, Vv + d, U + d + w, Vv + d + h);          // south
        set(2, U + d + w + d, Vv + d, U + d + w + d + w, Vv + d + h);  // north
        set(5, U + d + w, Vv + d, U + d + w + d, Vv + d + h);  // east
        set(4, U, Vv + d, U + d, Vv + d + h);                  // west
        m.elements.push_back(e);
    }

    std::shared_ptr<Model> specialModel(const std::string& special, const std::shared_ptr<Model>& base) {
        auto m = std::make_shared<Model>(*base);
        m->elements.clear();
        m->generated = false;
        const size_t c = special.find(':');
        const std::string kind = special.substr(0, c), texName = special.substr(c + 1);
        if (kind == "chest") {
            // ChestModel: bottom (1,0,1) 14x10x14 at uv (0,19); lid (1,9,1) 14x5x14 at uv (0,0); lock (7,7,15) 2x4x1.
            const std::string tex = "entity/chest/" + texName;
            entityBox(*m, tex, {1, 0, 1}, {14, 10, 14}, 0, 19);
            entityBox(*m, tex, {1, 9, 1}, {14, 5, 14}, 0, 0);
            entityBox(*m, tex, {7, 7, 15}, {2, 4, 1}, 0, 0);
        } else if (kind == "shulker_box") {
            const std::string tex = "entity/shulker/" + texName;
            entityBox(*m, tex, {0, 0, 0}, {16, 8, 16}, 0, 28);
            entityBox(*m, tex, {0, 4, 0}, {16, 12, 16}, 0, 0);
        } else if (kind == "head") {
            // SkullModel: an 8x8x8 head (uv 0,0); players and zombies add the hat layer (uv 32,0).
            static const std::pair<const char*, const char*> kTex[] = {
                {"skeleton", "entity/skeleton/skeleton"}, {"wither_skeleton", "entity/skeleton/wither_skeleton"},
                {"zombie", "entity/zombie/zombie"},       {"creeper", "entity/creeper/creeper"},
                {"player", "entity/player/wide/steve"},   {"piglin", "entity/piglin/piglin"}};
            std::string tex;
            for (auto [k, t] : kTex)
                if (texName == k) tex = t;
            if (tex.empty()) return base;  // dragon: flat fallback
            if (texName == "piglin") {
                entityBox(*m, tex, {3, 0, 4}, {10, 8, 8}, 0, 0);
            } else {
                entityBox(*m, tex, {4, 0, 4}, {8, 8, 8}, 0, 0);
                if (texName == "player" || texName == "zombie")
                    entityBox(*m, tex, {3.75f, -0.25f, 3.75f}, {8.5f, 8.5f, 8.5f}, 32, 0, {8, 8, 8});
            }
        } else if (kind == "shield") {
            // ShieldModel: the plate (12x22x1, uv 0,0) and the handle (2x6x6, uv 26,0).
            const std::string tex = "entity/shield/shield_base_nopattern";
            entityBox(*m, tex, {2, -3, 8}, {12, 22, 1}, 0, 0);
            entityBox(*m, tex, {7, 5, 2}, {2, 6, 6}, 26, 0);
        } else if (kind == "banner") {
            // BannerModel: flag 20x40x1 (uv 0,0, tinted by the base colour), pole 2x42x2 (uv 44,0), bar 20x2x2 (uv 0,42).
            const std::string tex = "entity/banner/base";
            entityBox(*m, tex, {-2, -11, 8}, {20, 40, 1}, 0, 0, {}, 0);
            entityBox(*m, tex, {7, -13, 6}, {2, 42, 2}, 44, 0);
            entityBox(*m, tex, {-2, 27, 6}, {20, 2, 2}, 0, 42);
        }
        return m;
    }

    void drawModel(Raster& r, const Model& m, const std::vector<u32>& tints) {
        auto tintOf = [&](int idx) -> u32 { return idx >= 0 && idx < int(tints.size()) ? tints[size_t(idx)] : 0xFFFFFFFF; };
        if (m.generated) {
            for (size_t li = 0; li < m.layers.size(); ++li) {
                const Tex& t = texture(m.layers[li]);
                if (t.w <= 0) continue;
                const u32 tc = tintOf(int(li));
                const float mul[4] = {((tc >> 16) & 255) / 255.f, ((tc >> 8) & 255) / 255.f, (tc & 255) / 255.f, 1.f};
                for (int y = 0; y < r.n; ++y)
                    for (int x = 0; x < r.n; ++x) {
                        const int tx = (x * t.w) / r.n, ty = (y * t.h) / r.n;
                        const u8* p = &t.rgba[(size_t(ty) * t.w + tx) * 4];
                        if (p[3] < 26) continue;
                        const float col[4] = {p[0] / 255.f * mul[0], p[1] / 255.f * mul[1], p[2] / 255.f * mul[2], p[3] / 255.f};
                        r.blend(size_t(y) * r.n + x, col);
                    }
            }
            return;
        }
        struct Quad {
            Vtx v[4];
            const Tex* tex;
            float mul[4];
            float z;
        };
        std::vector<Quad> quads;
        const float N = float(r.n);
        for (const Element& e : m.elements) {
            const V3 lo = e.from, hi = e.to;
            for (int d = 0; d < 6; ++d) {
                if (!(e.mask & (1 << d))) continue;
                const Face& f = e.faces[d];
                // FaceInfo vertex order per direction (vanilla).
                V3 p[4];
                switch (d) {
                    case 0: p[0] = {lo.x, lo.y, hi.z}, p[1] = {lo.x, lo.y, lo.z}, p[2] = {hi.x, lo.y, lo.z}, p[3] = {hi.x, lo.y, hi.z}; break;
                    case 1: p[0] = {lo.x, hi.y, lo.z}, p[1] = {lo.x, hi.y, hi.z}, p[2] = {hi.x, hi.y, hi.z}, p[3] = {hi.x, hi.y, lo.z}; break;
                    case 2: p[0] = {hi.x, hi.y, lo.z}, p[1] = {hi.x, lo.y, lo.z}, p[2] = {lo.x, lo.y, lo.z}, p[3] = {lo.x, hi.y, lo.z}; break;
                    case 3: p[0] = {lo.x, hi.y, hi.z}, p[1] = {lo.x, lo.y, hi.z}, p[2] = {hi.x, lo.y, hi.z}, p[3] = {hi.x, hi.y, hi.z}; break;
                    case 4: p[0] = {lo.x, hi.y, lo.z}, p[1] = {lo.x, lo.y, lo.z}, p[2] = {lo.x, lo.y, hi.z}, p[3] = {lo.x, hi.y, hi.z}; break;
                    default: p[0] = {hi.x, hi.y, hi.z}, p[1] = {hi.x, lo.y, hi.z}, p[2] = {hi.x, lo.y, lo.z}, p[3] = {hi.x, hi.y, lo.z}; break;
                }
                float uv[4];
                if (f.hasUv) {
                    std::copy(f.uv, f.uv + 4, uv);
                } else {
                    const float tbl[6][4] = {{lo.x, 16 - hi.z, hi.x, 16 - lo.z}, {lo.x, lo.z, hi.x, hi.z},
                                             {16 - hi.x, 16 - hi.y, 16 - lo.x, 16 - lo.y}, {lo.x, 16 - hi.y, hi.x, 16 - lo.y},
                                             {lo.z, 16 - hi.y, hi.z, 16 - lo.y}, {16 - hi.z, 16 - hi.y, 16 - lo.z, 16 - lo.y}};
                    std::copy(tbl[d], tbl[d] + 4, uv);
                }
                const float corner[4][2] = {{uv[0], uv[1]}, {uv[0], uv[3]}, {uv[2], uv[3]}, {uv[2], uv[1]}};
                V3 normal = kNormals[d];
                const Tex& t = texture(f.tex);
                Quad q;
                q.tex = &t;
                for (int k = 0; k < 4; ++k) {
                    V3 v = p[k];
                    if (e.rotated) {
                        V3 o = v - e.origin;
                        if (e.axis >= 0) {
                            const float a = e.angle * kPi / 180;
                            o = e.axis == 0 ? rotX(o, a) : e.axis == 1 ? rotY(o, a) : rotZ(o, a);
                            if (e.rescale && std::abs(e.angle) > 1e-3f) {
                                const float sc = 1.0f / std::cos(std::abs(e.angle) * kPi / 180);
                                for (int ax = 0; ax < 3; ++ax)
                                    if (ax != e.axis) at(o, ax) *= sc;
                            }
                        } else {
                            o = rotXYZ(o, e.euler);
                        }
                        v = o + e.origin;
                    }
                    // Display transform: translate, rotate, scale around the model centre.
                    V3 c = (v * (1.0f / 16.0f)) - V3{0.5f, 0.5f, 0.5f};
                    c = {c.x * m.scale[0], c.y * m.scale[1], c.z * m.scale[2]};
                    c = rotXYZ(c, m.rot) + V3{m.trans[0] / 16, m.trans[1] / 16, m.trans[2] / 16};
                    const int ci = (k + f.rot) % 4;
                    q.v[k] = {(0.5f + c.x) * N, (0.5f - c.y) * N, c.z, corner[ci][0] / 16.0f * float(t.w),
                              corner[ci][1] / 16.0f * float(t.h)};
                }
                if (e.rotated) normal = e.axis >= 0 ? (e.axis == 0 ? rotX(normal, e.angle * kPi / 180)
                                                       : e.axis == 1 ? rotY(normal, e.angle * kPi / 180)
                                                                     : rotZ(normal, e.angle * kPi / 180))
                                                    : rotXYZ(normal, e.euler);
                V3 n = rotXYZ({normal.x * m.scale[0], normal.y * m.scale[1], normal.z * m.scale[2]}, m.rot);
                const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                if (len > 0) n = n * (1 / len);
                if (n.z < -1e-4f) continue;  // back face
                float light = 1;
                if (m.sideLit && e.shade) {
                    const float l0 = std::max(0.f, n.x * kLight0.x + n.y * kLight0.y + n.z * kLight0.z);
                    const float l1 = std::max(0.f, n.x * kLight1.x + n.y * kLight1.y + n.z * kLight1.z);
                    light = std::min(1.0f, (l0 + l1) * 0.6f + 0.4f);
                }
                const u32 tc = tintOf(f.tint);
                q.mul[0] = ((tc >> 16) & 255) / 255.f * light, q.mul[1] = ((tc >> 8) & 255) / 255.f * light;
                q.mul[2] = (tc & 255) / 255.f * light, q.mul[3] = 1;
                q.z = (q.v[0].z + q.v[1].z + q.v[2].z + q.v[3].z) / 4;
                quads.push_back(q);
            }
        }
        std::stable_sort(quads.begin(), quads.end(), [](const Quad& a, const Quad& b) { return a.z < b.z; });
        for (const Quad& q : quads) {
            triangle(r, q.v[0], q.v[1], q.v[2], *q.tex, q.mul);
            triangle(r, q.v[0], q.v[2], q.v[3], *q.tex, q.mul);
        }
    }

    bool rasterize(const ItemStack& s, Raster& r) {
        const std::string name = strip(items->name(s.item));
        const json& def = jsonAt("assets/minecraft/items/" + name + ".json");
        std::vector<Part> parts;
        if (def.is_object()) evaluate(def.value("model", json()), s, parts, 0);
        r.init(size);
        bool any = false;
        for (const Part& p : parts) {
            std::shared_ptr<Model> m = model(p.model);
            if (!p.special.empty()) m = specialModel(p.special, m);
            if (!m->generated && m->elements.empty() && !m->particle.empty()) {  // other special renderers: flat particle
                auto flat = std::make_shared<Model>();
                flat->generated = true;
                flat->layers.push_back(m->particle);
                m = flat;
            }
            drawModel(r, *m, p.tints);
            any = true;
        }
        return any;
    }

    bool render(const ItemStack& s, float uv[4]) {
        Raster r;
        touchedAnimated = false;
        if (!rasterize(s, r)) return false;
        int x, y;
        if (!atlas->alloc(size, size, x, y)) {
            if (!atlas->full()) return false;
            atlas->reset();  // full: start over (icons re-render on demand)
            icons.clear(), animatedIcons.clear();
            if (!atlas->alloc(size, size, x, y)) return false;
        }
        if (touchedAnimated) animatedIcons.push_back({s, x, y});
        write(r, x, y);
        const float W = float(atlas->width()), H = float(atlas->height());
        uv[0] = float(x) / W, uv[1] = float(y) / H, uv[2] = float(x + size) / W, uv[3] = float(y + size) / H;
        return true;
    }

    void write(const Raster& r, int x, int y) {
        for (int py = 0; py < size; ++py)
            for (int px = 0; px < size; ++px) {
                const float* c = &r.color[(size_t(py) * size + px) * 4];
                u8* o = atlas->pixel(x + px, y + py);
                for (int k = 0; k < 4; ++k) o[k] = u8(std::clamp(c[k], 0.f, 1.f) * 255.f + 0.5f);
            }
        atlas->markDirty(y, y + size);
    }
};

ItemIcons::ItemIcons() : impl_(std::make_unique<Impl>()) {}
ItemIcons::~ItemIcons() = default;

void ItemIcons::init(const Resources& res, const Items& items, Atlas& atlas) {
    impl_->res = &res;
    impl_->items = &items;
    impl_->atlas = &atlas;
    res.image("assets/minecraft/textures/colormap/grass.png", impl_->grass);
    res.image("assets/minecraft/textures/colormap/foliage.png", impl_->foliage);
}

void ItemIcons::reload() {
    Impl& m = *impl_;
    m.jsonCache.clear(), m.texCache.clear(), m.models.clear();
    m.icons.clear(), m.animatedIcons.clear();
    m.atlas->reset();
    m.res->image("assets/minecraft/textures/colormap/grass.png", m.grass);
    m.res->image("assets/minecraft/textures/colormap/foliage.png", m.foliage);
}

void ItemIcons::setScale(int guiScale) {
    const int size = std::clamp(16 * guiScale, 16, 160);
    if (size == size_) return;
    size_ = impl_->size = size;
    impl_->icons.clear(), impl_->animatedIcons.clear();
    impl_->atlas->reset();
}

void ItemIcons::tick(u64 tick) {
    Impl& m = *impl_;
    bool changed = false;
    for (auto& [key, t] : m.texCache) {
        if (!t.animated) continue;
        const u64 state = t.anim.stateAt(tick);
        if (state == t.shown) continue;
        t.shown = state;
        t.anim.frameAt(t.strip, tick, t.rgba);
        changed = true;
    }
    if (!changed) return;
    m.size = size_;
    for (const Impl::AnimatedIcon& a : m.animatedIcons) {
        Impl::Raster r;
        if (m.rasterize(a.stack, r)) m.write(r, a.x, a.y);
    }
}

bool ItemIcons::icon(const ItemStack& s, float uv[4]) {
    if (s.empty()) return false;
    impl_->size = size_;
    u64 key = u64(u32(s.item)) | (u64(u32(s.dyedColor + 1) & 0xFFFFFF) << 24);
    key ^= u64(u32(s.potionColor + 1)) << 40;
    for (const auto& c : s.added)  // model-selecting components
        if (impl_->items->componentType("lodestone_tracker") == c.type) key ^= u64(1) << 63;
    if (s.damage > 0) key ^= u64(std::min(s.damage, 4095)) << 48;
    auto it = impl_->icons.find(key);
    if (it == impl_->icons.end()) {
        std::array<float, 4> r{-1, -1, -1, -1};
        if (!impl_->render(s, r.data())) r[0] = -1;
        it = impl_->icons.emplace(key, r).first;
    }
    if (it->second[0] < 0) return false;
    std::copy(it->second.begin(), it->second.end(), uv);
    return true;
}

}  // namespace ui
