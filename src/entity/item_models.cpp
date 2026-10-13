#include "entity/item_models.h"

#include "entity/entities.h"
#include "entity/entity_assets.h"
#include "entity/skin_cache.h"
#include "ui/resources.h"
#include "ui/sprites.h"

#include <windows.h>
#include <urlmon.h>
#include <stb/stb_image.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <array>
#include <fstream>
#include <functional>
#include <thread>

#pragma comment(lib, "urlmon.lib")

namespace ent {

namespace {

using json = nlohmann::json;
constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;

std::string strip(std::string s) {
    const size_t c = s.find(':');
    return c == std::string::npos ? s : s.substr(c + 1);
}
// Resource id -> "namespace:path" (default namespace minecraft) and its file in a resource pack.
std::string normId(const std::string& id) { return id.find(':') == std::string::npos ? "minecraft:" + id : id; }
std::string resPath(const std::string& id, const char* kind, const char* ext) {
    const std::string n = normId(id);
    const size_t c = n.find(':');
    return "assets/" + n.substr(0, c) + "/" + kind + "/" + n.substr(c + 1) + ext;
}
float num(const json& v, float d) { return v.is_number() ? v.get<float>() : d; }
// Member of an object, or null (never throws on other types).
const json& at(const json& j, const char* key) {
    static const json kNull;
    if (!j.is_object()) return kNull;
    auto it = j.find(key);
    return it == j.end() ? kNull : *it;
}
bool vec3(const json& v, float out[3]) {
    if (!v.is_array() || v.size() != 3) return false;
    for (int i = 0; i < 3; ++i) out[i] = num(v[size_t(i)], 0);
    return true;
}

// Quaternion [x, y, z, w] -> rotation matrix (JOML Quaternionf.get(Matrix3f)).
Xf quatXf(const float q[4]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    Xf r;
    r.m[0] = 1 - 2 * (y * y + z * z), r.m[1] = 2 * (x * y - z * w), r.m[2] = 2 * (x * z + y * w);
    r.m[4] = 2 * (x * y + z * w), r.m[5] = 1 - 2 * (x * x + z * z), r.m[6] = 2 * (y * z - x * w);
    r.m[8] = 2 * (x * z - y * w), r.m[9] = 2 * (y * z + x * w), r.m[10] = 1 - 2 * (x * x + y * y);
    r.m[3] = r.m[7] = r.m[11] = 0;
    return r;
}
bool quatFrom(const json& v, float out[4]) {
    if (v.is_array() && v.size() == 4) {
        for (int i = 0; i < 4; ++i) out[i] = num(v[size_t(i)], 0);
        return true;
    }
    if (v.is_object() && v.contains("axis")) {  // axis-angle (degrees)
        float a[3] = {0, 1, 0};
        vec3(v["axis"], a);
        const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (l > 0) a[0] /= l, a[1] /= l, a[2] /= l;
        const float ang = num(v.value("angle", json()), 0) * kDeg * 0.5f;
        out[0] = a[0] * std::sin(ang), out[1] = a[1] * std::sin(ang), out[2] = a[2] * std::sin(ang), out[3] = std::cos(ang);
        return true;
    }
    return false;
}

// Transformation (translation, left_rotation, scale, right_rotation) or a 16-float matrix (row-major).
Xf transformation(const json& t) {
    if (t.is_array() && t.size() == 16) {
        Xf r;
        for (int i = 0; i < 12; ++i) r.m[i] = num(t[size_t(i)], 0);
        return r;
    }
    if (!t.is_object()) return {};
    float tr[3] = {0, 0, 0}, sc[3] = {1, 1, 1}, l[4] = {0, 0, 0, 1}, r[4] = {0, 0, 0, 1};
    vec3(t.value("translation", json()), tr);
    vec3(t.value("scale", json()), sc);
    quatFrom(t.value("left_rotation", json()), l);
    quatFrom(t.value("right_rotation", json()), r);
    return xfMul(xfMul(xfMul(xfTranslate(tr[0], tr[1], tr[2]), quatXf(l)), xfScale(sc[0], sc[1], sc[2])), quatXf(r));
}

// ItemTransform.apply: translate, rotationXYZ, scale, translate(-0.5); left hand: mirrored.
Xf displayXf(const float rot[3], const float trans[3], const float scale[3], bool left, bool none) {
    if (none) return xfTranslate(-0.5f, -0.5f, -0.5f);
    const float tx = left ? -trans[0] : trans[0];
    const float ry = left ? -rot[1] : rot[1], rz = left ? -rot[2] : rot[2];
    Xf m = xfTranslate(tx, trans[1], trans[2]);
    m = xfMul(m, xfRotX(rot[0] * kDeg));
    m = xfMul(m, xfRotY(ry * kDeg));
    m = xfMul(m, xfRotZ(rz * kDeg));
    m = xfMul(m, xfScale(scale[0], scale[1], scale[2]));
    return xfMul(m, xfTranslate(-0.5f, -0.5f, -0.5f));
}

const char* kDisplayNames[9] = {"firstperson_righthand", "firstperson_lefthand", "thirdperson_righthand",
                                "thirdperson_lefthand",  "head",                 "gui",
                                "ground",                "fixed",                "on_shelf"};
const char* kFaceNames[6] = {"down", "up", "north", "south", "west", "east"};
const float kNormals[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};

u32 abgrOf(u32 argb) { return (argb & 0xFF00FF00u) | ((argb & 0xFF) << 16) | ((argb >> 16) & 0xFF); }

struct Face {
    std::string tex;
    float uv[4] = {};
    bool hasUv = false;
    int rot = 0, tint = -1;
};
struct Element {
    float from[3] = {0, 0, 0}, to[3] = {16, 16, 16};
    bool rotated = false;
    float origin[3] = {8, 8, 8};
    float euler[3] = {};
    int axis = -1;
    float angle = 0;
    bool rescale = false;
    bool shade = true;
    Face faces[6];
    u8 mask = 0;
};
struct JModel {
    bool generated = false;
    std::vector<std::string> layers;
    std::vector<Element> elements;
    struct Tr {
        float rot[3] = {0, 0, 0}, trans[3] = {0, 0, 0}, scale[3] = {1, 1, 1};
        bool set = false;
    } display[9];
    std::string particle;
};

void rotate3(float v[3], int axis, float a) {
    const float c = std::cos(a), s = std::sin(a);
    float x = v[0], y = v[1], z = v[2];
    if (axis == 0) v[1] = y * c - z * s, v[2] = y * s + z * c;
    else if (axis == 1) v[0] = x * c + z * s, v[2] = -x * s + z * c;
    else v[0] = x * c - y * s, v[1] = x * s + y * c;
}

}  // namespace

Xf itemTransformation(const nlohmann::json& t) { return transformation(t); }

Xf xfMul(const Xf& a, const Xf& b) {
    Xf r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            r.m[i * 4 + j] = a.m[i * 4] * b.m[j] + a.m[i * 4 + 1] * b.m[4 + j] + a.m[i * 4 + 2] * b.m[8 + j];
        r.m[i * 4 + 3] = a.m[i * 4] * b.m[3] + a.m[i * 4 + 1] * b.m[7] + a.m[i * 4 + 2] * b.m[11] + a.m[i * 4 + 3];
    }
    return r;
}
Xf xfTranslate(float x, float y, float z) { return {{1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z}}; }
Xf xfScale(float x, float y, float z) { return {{x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0}}; }
Xf xfRotX(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {{1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0}};
}
Xf xfRotY(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {{c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0}};
}
Xf xfRotZ(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {{c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0}};
}
void xfPoint(const Xf& m, const float in[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = m.m[i * 4] * in[0] + m.m[i * 4 + 1] * in[1] + m.m[i * 4 + 2] * in[2] + m.m[i * 4 + 3];
}
u32 packNormal(float x, float y, float z) {
    const float l = std::sqrt(x * x + y * y + z * z);
    if (l > 0) x /= l, y /= l, z /= l;
    auto c = [](float v) { return u32(u8(i8(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f)))); };
    return c(x) | (c(y) << 8) | (c(z) << 16);
}

struct ItemModels::Impl {
    ItemModels* self = nullptr;
    const ui::Resources* res = nullptr;
    const EntityAssets* assets = nullptr;
    std::vector<std::string> itemNames;
    fs::path componentReports;
    std::unordered_map<std::string, json> jsonCache;
    struct Tex {
        int rect = -1;
        ui::Image img;  // first frame
    };
    struct Animated {  // .mcmeta animations (SpriteContents.AnimatedTexture): the current frame is copied in each tick
        ui::Sprites::Animation anim;
        ui::Image strip;
        int x = 0, y = 0;
        u64 shown = 0;
    };
    std::vector<Animated> animated;
    std::unordered_map<std::string, Tex> textures;
    std::unordered_map<std::string, std::shared_ptr<JModel>> models;
    std::unordered_map<std::string, ItemMesh> namedMeshes;
    std::unordered_map<std::string, ItemMesh> useMeshes;  // mesh(stack, context): by the evaluated parts
    std::unordered_map<int, std::string> itemParticles;
    std::unordered_map<int, Equippable> equippables;
    std::unordered_map<std::string, std::vector<EquipmentLayer>> equipLayers;
    std::unordered_map<std::string, std::string> particleTextures;
    std::unordered_map<int, HandInfo> handInfos;
    std::vector<std::array<const std::vector<EquipmentLayer>*, 4>> itemLayers;  // by item id
    ui::Image grass, foliage;
    ItemMesh emptyMesh;
    bool storeFull = false;

    const json& jsonAt(const std::string& path) {
        auto it = jsonCache.find(path);
        if (it == jsonCache.end()) it = jsonCache.emplace(path, res->json(path)).first;
        return it->second;
    }

    Tex* tex(const std::string& rawName) {
        if (rawName.empty()) return nullptr;
        const std::string key = normId(rawName);
        auto it = textures.find(key);
        if (it != textures.end()) return it->second.rect >= 0 ? &it->second : nullptr;
        Tex& t = textures[key];
        ui::Image img;
        if (!ui::atlasTextureImage(*res, key, img) || img.w <= 0 || img.h <= 0) return nullptr;
        const std::string meta = resPath(key, "textures", ".png.mcmeta");
        ui::Sprites::Animation anim;
        const bool animatedTex = res->exists(meta) && ui::Sprites::parseAnimation(img, res->json(meta), anim);
        // strips without .mcmeta: first frame (entity textures are whole: the end crystal beam is 16 x 256)
        const bool entityTex = key.compare(key.find(':') + 1, 7, "entity/") == 0;
        int w = img.w, h = (!entityTex && img.h > img.w && img.h % img.w == 0) ? img.w : img.h;
        if (animatedTex) {
            w = anim.fw, h = anim.fh;
            anim.frameAt(img, 0, t.img.rgba);
            t.img.w = w, t.img.h = h;
        } else {
            t.img.w = w, t.img.h = h;
            t.img.rgba.assign(img.rgba.begin(), img.rgba.begin() + size_t(w) * h * 4);
        }
        int x, y;
        if (!self->atlas_.alloc(w, h, x, y)) {
            LOG("visuals: item atlas full (%s not loaded)", key.c_str());
            return nullptr;
        }
        self->atlas_.blit(x, y, w, h, t.img.rgba.data(), w * 4);
        if (animatedTex) {
            const u64 shown = anim.stateAt(0);
            animated.push_back({std::move(anim), std::move(img), x, y, shown});
        }
        bool translucent = false;
        for (size_t i = 3; i < t.img.rgba.size(); i += 4) translucent |= t.img.rgba[i] > 25 && t.img.rgba[i] < 230;
        const float W = float(self->atlas_.width()), H = float(self->atlas_.height());
        self->rects_.push_back({float(x) / W, float(y) / H, float(w) / W, float(h) / H, w, h, translucent});
        t.rect = int(self->rects_.size() - 1);
        return &t;
    }

    std::shared_ptr<JModel> model(const std::string& rawName) {
        const std::string name = rawName.empty() ? rawName : normId(rawName);
        if (auto it = models.find(name); it != models.end()) return it->second;
        auto m = std::make_shared<JModel>();
        models[name] = m;
        std::unordered_map<std::string, std::string> vars;
        const json* elements = nullptr;
        std::string cur = name;
        for (int guard = 0; guard < 32 && !cur.empty(); ++guard) {
            if (cur == "minecraft:builtin/generated") {
                m->generated = true;
                break;
            }
            const json& j = jsonAt(resPath(cur, "models", ".json"));
            if (!j.is_object()) break;
            if (j.contains("textures") && j["textures"].is_object())
                for (const auto& t : j["textures"].items()) {
                    std::string v = t.value().is_string() ? t.value().get<std::string>()
                                    : t.value().is_object() ? t.value().value("sprite", "") : "";
                    vars.emplace(t.key(), v);
                }
            if (!elements && j.contains("elements") && j["elements"].is_array()) elements = &j["elements"];
            if (j.contains("display") && j["display"].is_object())
                for (int c = 0; c < 9; ++c) {
                    if (m->display[c].set || !j["display"].contains(kDisplayNames[c])) continue;
                    const json& d = j["display"][kDisplayNames[c]];
                    JModel::Tr& tr = m->display[c];
                    vec3(d.value("rotation", json()), tr.rot);
                    if (vec3(d.value("translation", json()), tr.trans))
                        for (float& v : tr.trans) v = std::clamp(v * 0.0625f, -5.0f, 5.0f);
                    if (vec3(d.value("scale", json()), tr.scale))
                        for (float& v : tr.scale) v = std::clamp(v, -4.0f, 4.0f);
                    tr.set = true;
                }
            cur = j.contains("parent") && j["parent"].is_string() ? normId(j["parent"].get<std::string>()) : "";
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
            for (int i = 0; i < 5; ++i) {
                auto it = vars.find("layer" + std::to_string(i));
                if (it == vars.end()) break;
                m->layers.push_back(resolve(it->second));
            }
            return m;
        }
        if (elements)
            for (const json& ej : *elements) {
                Element e;
                vec3(ej.value("from", json()), e.from);
                vec3(ej.value("to", json()), e.to);
                if (ej.contains("rotation") && ej["rotation"].is_object()) {
                    const json& r = ej["rotation"];
                    e.rotated = true;
                    vec3(r.value("origin", json()), e.origin);
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
                            for (int k = 0; k < 4; ++k) f.uv[k] = num(fj["uv"][size_t(k)], 0);
                        }
                        f.rot = ((int(num(fj.value("rotation", json()), 0)) / 90) % 4 + 4) % 4;
                        f.tint = int(num(fj.value("tintindex", json()), -1));
                        e.mask |= u8(1 << d);
                    }
                m->elements.push_back(e);
            }
        return m;
    }

    // --- quad output --------------------------------------------------------------------------------------
    struct Out {
        std::vector<MeshQuad> cutout, translucent;
    };

    void quad(Out& out, const Xf& local, const float p[4][3], const float uv[4][2], const float n[3], u32 tex, u32 color,
              u32 flags, bool translucent) {
        MeshQuad q{};
        for (int k = 0; k < 4; ++k) {
            xfPoint(local, p[k], q.pos[k]);
            q.uv[k][0] = uv[k][0], q.uv[k][1] = uv[k][1];
        }
        float nn[3];
        for (int i = 0; i < 3; ++i) nn[i] = local.m[i * 4] * n[0] + local.m[i * 4 + 1] * n[1] + local.m[i * 4 + 2] * n[2];
        q.normal = packNormal(nn[0], nn[1], nn[2]);
        q.color = color, q.tex = tex, q.flags = flags;
        (translucent ? out.translucent : out.cutout).push_back(q);
    }

    u32 tintColor(const std::vector<u32>& tints, int idx) {
        return idx >= 0 && idx < int(tints.size()) ? (0xFF000000u | abgrOf(tints[size_t(idx)])) : 0xFFFFFFFFu;
    }

    // ItemModelGenerator: front / back faces and one side face per opaque pixel edge.
    void generated(Out& out, const JModel& m, const Xf& local, const std::vector<u32>& tints) {
        for (size_t li = 0; li < m.layers.size(); ++li) {
            Tex* t = tex(m.layers[li]);
            if (!t) continue;
            const Rect& r = self->rects_[size_t(t->rect)];
            const u32 color = tintColor(tints, int(li));
            const bool tr = r.translucent;
            auto auv = [&](float u, float v, float* o) { o[0] = r.u0 + u / 16.0f * r.du, o[1] = r.v0 + v / 16.0f * r.dv; };
            {  // south (front) z = 8.5, north (back) z = 7.5
                const float z1 = 8.5f / 16, z0 = 7.5f / 16;
                const float ps[4][3] = {{0, 1, z1}, {0, 0, z1}, {1, 0, z1}, {1, 1, z1}};
                float uvs[4][2];
                auv(0, 0, uvs[0]), auv(0, 16, uvs[1]), auv(16, 16, uvs[2]), auv(16, 0, uvs[3]);
                quad(out, local, ps, uvs, kNormals[3], kTexItems, color, 0, tr);
                const float pn[4][3] = {{1, 1, z0}, {1, 0, z0}, {0, 0, z0}, {0, 1, z0}};
                float uvn[4][2];
                auv(16, 0, uvn[0]), auv(16, 16, uvn[1]), auv(0, 16, uvn[2]), auv(0, 0, uvn[3]);
                quad(out, local, pn, uvn, kNormals[2], kTexItems, color, 0, tr);
            }
            const int w = t->img.w, h = t->img.h;
            auto opaque = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && t->img.rgba[(size_t(y) * w + x) * 4 + 3] != 0; };
            const float xs = 1.0f / float(w), ys = 1.0f / float(h);
            const float z0 = 7.5f / 16, z1 = 8.5f / 16;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    if (!opaque(x, y)) continue;
                    // Texel (x, y) spans model x [x, x+1] * xs and model y [1 - (y+1) ys, 1 - y ys].
                    const float x0 = float(x) * xs, x1 = float(x + 1) * xs;
                    const float yTop = 1.0f - float(y) * ys, yBot = 1.0f - float(y + 1) * ys;
                    float uvs[4][2];
                    const float pu0 = (float(x) + 0.1f) * 16.0f / float(w), pu1 = (float(x) + 0.9f) * 16.0f / float(w);
                    const float pv0 = (float(y) + 0.1f) * 16.0f / float(h), pv1 = (float(y) + 0.9f) * 16.0f / float(h);
                    auv(pu0, pv0, uvs[0]), auv(pu0, pv1, uvs[1]), auv(pu1, pv1, uvs[2]), auv(pu1, pv0, uvs[3]);
                    if (!opaque(x, y - 1)) {  // up
                        const float p[4][3] = {{x0, yTop, z0}, {x0, yTop, z1}, {x1, yTop, z1}, {x1, yTop, z0}};
                        quad(out, local, p, uvs, kNormals[1], kTexItems, color, 0, tr);
                    }
                    if (!opaque(x, y + 1)) {  // down
                        const float p[4][3] = {{x0, yBot, z1}, {x0, yBot, z0}, {x1, yBot, z0}, {x1, yBot, z1}};
                        quad(out, local, p, uvs, kNormals[0], kTexItems, color, 0, tr);
                    }
                    if (!opaque(x - 1, y)) {  // west
                        const float p[4][3] = {{x0, yTop, z0}, {x0, yBot, z0}, {x0, yBot, z1}, {x0, yTop, z1}};
                        quad(out, local, p, uvs, kNormals[4], kTexItems, color, 0, tr);
                    }
                    if (!opaque(x + 1, y)) {  // east
                        const float p[4][3] = {{x1, yTop, z1}, {x1, yBot, z1}, {x1, yBot, z0}, {x1, yTop, z0}};
                        quad(out, local, p, uvs, kNormals[5], kTexItems, color, 0, tr);
                    }
                }
        }
    }

    // Cuboid elements (FaceBakery vertex order and UV rules).
    void cuboids(Out& out, const JModel& m, const Xf& local, const std::vector<u32>& tints) {
        for (const Element& e : m.elements) {
            const float* lo = e.from;
            const float* hi = e.to;
            for (int d = 0; d < 6; ++d) {
                if (!(e.mask & (1 << d))) continue;
                const Face& f = e.faces[d];
                Tex* t = tex(f.tex);
                if (!t) continue;
                const Rect& r = self->rects_[size_t(t->rect)];
                float p[4][3];
                auto set = [&](int k, float x, float y, float z) { p[k][0] = x, p[k][1] = y, p[k][2] = z; };
                switch (d) {
                    case 0: set(0, lo[0], lo[1], hi[2]), set(1, lo[0], lo[1], lo[2]), set(2, hi[0], lo[1], lo[2]), set(3, hi[0], lo[1], hi[2]); break;
                    case 1: set(0, lo[0], hi[1], lo[2]), set(1, lo[0], hi[1], hi[2]), set(2, hi[0], hi[1], hi[2]), set(3, hi[0], hi[1], lo[2]); break;
                    case 2: set(0, hi[0], hi[1], lo[2]), set(1, hi[0], lo[1], lo[2]), set(2, lo[0], lo[1], lo[2]), set(3, lo[0], hi[1], lo[2]); break;
                    case 3: set(0, lo[0], hi[1], hi[2]), set(1, lo[0], lo[1], hi[2]), set(2, hi[0], lo[1], hi[2]), set(3, hi[0], hi[1], hi[2]); break;
                    case 4: set(0, lo[0], hi[1], lo[2]), set(1, lo[0], lo[1], lo[2]), set(2, lo[0], lo[1], hi[2]), set(3, lo[0], hi[1], hi[2]); break;
                    default: set(0, hi[0], hi[1], hi[2]), set(1, hi[0], lo[1], hi[2]), set(2, hi[0], lo[1], lo[2]), set(3, hi[0], hi[1], lo[2]); break;
                }
                float uv[4];
                if (f.hasUv) {
                    std::copy(f.uv, f.uv + 4, uv);
                } else {
                    const float tbl[6][4] = {{lo[0], 16 - hi[2], hi[0], 16 - lo[2]}, {lo[0], lo[2], hi[0], hi[2]},
                                             {16 - hi[0], 16 - hi[1], 16 - lo[0], 16 - lo[1]}, {lo[0], 16 - hi[1], hi[0], 16 - lo[1]},
                                             {lo[2], 16 - hi[1], hi[2], 16 - lo[1]}, {16 - hi[2], 16 - hi[1], 16 - lo[2], 16 - lo[1]}};
                    std::copy(tbl[d], tbl[d] + 4, uv);
                }
                const float corner[4][2] = {{uv[0], uv[1]}, {uv[0], uv[3]}, {uv[2], uv[3]}, {uv[2], uv[1]}};
                float n[3] = {kNormals[d][0], kNormals[d][1], kNormals[d][2]};
                float pos[4][3], uvs[4][2];
                for (int k = 0; k < 4; ++k) {
                    float v[3] = {p[k][0], p[k][1], p[k][2]};
                    if (e.rotated) {
                        float o[3] = {v[0] - e.origin[0], v[1] - e.origin[1], v[2] - e.origin[2]};
                        if (e.axis >= 0) {
                            rotate3(o, e.axis, e.angle * kDeg);
                            if (e.rescale && std::abs(e.angle) > 1e-3f) {
                                const float sc = 1.0f / std::cos(std::abs(e.angle) * kDeg);
                                for (int ax = 0; ax < 3; ++ax)
                                    if (ax != e.axis) o[ax] *= sc;
                            }
                        } else {
                            rotate3(o, 2, e.euler[2] * kDeg), rotate3(o, 1, e.euler[1] * kDeg), rotate3(o, 0, e.euler[0] * kDeg);
                        }
                        for (int a = 0; a < 3; ++a) v[a] = o[a] + e.origin[a];
                    }
                    for (int a = 0; a < 3; ++a) pos[k][a] = v[a] / 16.0f;
                    const int ci = (k + f.rot) % 4;
                    uvs[k][0] = r.u0 + corner[ci][0] / 16.0f * r.du, uvs[k][1] = r.v0 + corner[ci][1] / 16.0f * r.dv;
                }
                if (e.rotated) {
                    if (e.axis >= 0) rotate3(n, e.axis, e.angle * kDeg);
                    else rotate3(n, 2, e.euler[2] * kDeg), rotate3(n, 1, e.euler[1] * kDeg), rotate3(n, 0, e.euler[0] * kDeg);
                }
                quad(out, local, pos, uvs, n, kTexItems, tintColor(tints, f.tint), e.shade ? 0 : kQuadNoShade, r.translucent);
            }
        }
    }

    // Entity model layer (special item renderers draw these with no extra transform), at its initial pose unless
    // `pose` changes it (returning false leaves the part's own quads out; its children stay). Texture: an entity atlas
    // texture, or an item atlas rect (itemRect >= 0: downloaded skins).
    using PoseFn = std::function<bool(const ModelPart& part, size_t index, PartPose& pose)>;
    bool entityModel(Out& out, const std::string& layer, const std::string& texture, const Xf& local, u32 color,
                     bool translucent = false, const PoseFn& pose = {}, int itemRect = -1) {
        const int mi = assets->model(layer);
        if (mi < 0) return false;
        float u0, v0, du, dv;
        u32 texId;
        if (itemRect >= 0) {
            const Rect& r = self->rects_[size_t(itemRect)];
            u0 = r.u0, v0 = r.v0, du = r.du, dv = r.dv, texId = kTexItems;
        } else if (const int ti = assets->texture(texture); ti >= 0) {
            const TexRect& tr = assets->rect(ti);
            u0 = tr.u0, v0 = tr.v0, du = tr.du, dv = tr.dv, texId = kTexEntity;
        } else if (Tex* t = tex(texture)) {  // not in the entity atlas (shield patterns): the item atlas
            const Rect& r = self->rects_[size_t(t->rect)];
            u0 = r.u0, v0 = r.v0, du = r.du, dv = r.dv, texId = kTexItems;
        } else {
            return false;
        }
        const auto& m = assets->modelAt(mi);
        std::vector<Xf> mats(m.parts.size());
        for (size_t i = 0; i < m.parts.size(); ++i) {
            const ModelPart& part = m.parts[i];
            PartPose p = part.pose;
            const bool draw = !pose || pose(part, i, p);
            Xf l = xfTranslate(p.x / 16.0f, p.y / 16.0f, p.z / 16.0f);
            if (p.zRot != 0) l = xfMul(l, xfRotZ(p.zRot));
            if (p.yRot != 0) l = xfMul(l, xfRotY(p.yRot));
            if (p.xRot != 0) l = xfMul(l, xfRotX(p.xRot));
            if (p.xScale != 1 || p.yScale != 1 || p.zScale != 1) l = xfMul(l, xfScale(p.xScale, p.yScale, p.zScale));
            mats[i] = xfMul(part.parent >= 0 ? mats[size_t(part.parent)] : local, l);
            if (!draw) continue;
            for (u32 q = part.quadFirst; q < part.quadFirst + part.quadCount; ++q) {
                const GpuQuad& g = assets->quads()[q];
                float pos[4][3], uvs[4][2];
                for (int k = 0; k < 4; ++k) {
                    for (int a = 0; a < 3; ++a) pos[k][a] = g.pos[k][a] / 16.0f;
                    uvs[k][0] = u0 + g.uv[k][0] * du, uvs[k][1] = v0 + g.uv[k][1] * dv;
                }
                quad(out, mats[i], pos, uvs, g.normal, texId, color, 0, translucent);
            }
        }
        return true;
    }

    // --- item definitions -----------------------------------------------------------------------------------
    struct Part {
        std::string model;
        std::vector<u32> tints;
        const json* special = nullptr;  // the special node's "model" object
        ui::ItemStack stack;            // special models: the stack whose components they show
        Xf local;
    };

    u32 colormap(const ui::Image& img, float temp, float down) {
        if (img.w < 256 || img.h < 256) return 0xFF7FB238;
        temp = std::clamp(temp, 0.f, 1.f);
        down = std::clamp(down, 0.f, 1.f) * temp;
        const u8* p = img.at(int((1.0f - temp) * 255.0f), int((1.0f - down) * 255.0f));
        return 0xFF000000u | (u32(p[0]) << 16) | (u32(p[1]) << 8) | p[2];
    }

    // The item definition of a stack (item_model component), evaluated into parts; `key` collects what the mesh
    // depends on (the chosen leaves, tints, transforms and special data).
    void evaluateStack(const ui::ItemStack& s, const ui::ItemContext& c, const Xf& local, std::vector<Part>& out,
                       std::string& key, int depth) {
        if (s.empty() || depth > 4) return;
        const std::string id = ui::itemModelId(*self->items_, s);
        if (id.empty()) return;
        const json& def = jsonAt(resPath(id, "items", ".json"));
        if (def.is_object()) evaluate(at(def, "model"), s, c, local, out, key, depth);
    }

    void evaluate(const json& n, const ui::ItemStack& s, const ui::ItemContext& c, const Xf& local, std::vector<Part>& out,
                  std::string& key, int depth) {
        if (!n.is_object() || depth > 24) return;
        const Xf here = n.contains("transformation") ? xfMul(local, transformation(n["transformation"])) : local;
        const std::string type = strip(n.value("type", ""));
        auto keyPart = [&](const void* node, const Xf& m) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "|%p:", node);
            key += buf;
            key.append(reinterpret_cast<const char*>(m.m), sizeof(m.m));
        };
        if (type == "model") {
            Part p;
            p.model = n.value("model", "");
            p.local = here;
            if (n.contains("tints") && n["tints"].is_array())
                for (const json& t : n["tints"])
                    p.tints.push_back(ui::itemTint(*self->items_, t, s, c, [&](bool isFoliage, float temp, float down) {
                        return colormap(isFoliage ? foliage : grass, temp, down);
                    }));
            keyPart(&n, here);
            for (u32 t : p.tints) key.append(reinterpret_cast<const char*>(&t), sizeof(t));
            out.push_back(std::move(p));
        } else if (type == "composite") {
            if (n.contains("models") && n["models"].is_array())
                for (const json& m : n["models"]) evaluate(m, s, c, here, out, key, depth + 1);
        } else if (type == "condition" || type == "select" || type == "range_dispatch") {
            if (const json* next = ui::chooseItemModel(*self->items_, n, s, c)) evaluate(*next, s, c, here, out, key, depth + 1);
        } else if (type == "special") {
            const json& sm = at(n, "model");
            if (!sm.is_object()) return;
            Part p;
            p.model = n.value("base", "");
            p.local = here;
            p.special = &sm;
            p.stack = s;
            keyPart(&n, here);
            key += self->specialKey(sm, s);
            out.push_back(std::move(p));
        } else if (type == "bundle/selected_item") {  // BundleSelectedItemSpecialRenderer: the selected item's layers
            ui::ItemStack selected;
            if (ui::bundleSelectedItem(*self->items_, s, selected)) {
                key += "|bundle";
                evaluateStack(selected, c, here, out, key, depth + 1);
            }
        }
    }

    // --- special model renderers (SpecialModelRenderers) ------------------------------------------------------
    // Skins of player heads with a profile: textures from the profile's skin URL, downloaded once into
    // %LOCALAPPDATA%\minecraftCplusplus\skins and loaded into the item atlas.
    struct Skin {
        std::atomic<int> state{0};  // 0 new, 1 downloading, 2 file ready, 3 failed
        fs::path file;
        int rect = -1;  // item atlas rect once loaded
    };
    std::unordered_map<std::string, std::unique_ptr<Skin>> skins;  // by URL

    static std::string base64Decode(std::string_view in) {
        std::string out;
        int val = 0, bits = -8;
        for (char ch : in) {
            int d;
            if (ch >= 'A' && ch <= 'Z') d = ch - 'A';
            else if (ch >= 'a' && ch <= 'z') d = ch - 'a' + 26;
            else if (ch >= '0' && ch <= '9') d = ch - '0' + 52;
            else if (ch == '+' || ch == '-') d = 62;
            else if (ch == '/' || ch == '_') d = 63;
            else continue;
            val = (val << 6) | d, bits += 6;
            if (bits >= 0) out += char((val >> bits) & 0xFF), bits -= 8;
        }
        return out;
    }

    // The skin of a profile's "textures" property (base64 JSON naming the skin URL): its item atlas rect, -1 while it
    // downloads or when there is none.
    int profileSkin(const std::string& textures) {
        if (textures.empty()) return -1;
        const json j = json::parse(base64Decode(textures), nullptr, false);
        const std::string url = at(at(at(j, "textures"), "SKIN"), "url").is_string()
                                    ? at(at(at(j, "textures"), "SKIN"), "url").get<std::string>() : std::string();
        // Only the game's own texture server (the property is data from the server, possibly unsigned).
        if (!url.starts_with("http://textures.minecraft.net/texture/") && !url.starts_with("https://textures.minecraft.net/texture/"))
            return -1;
        auto& entry = skins[url];
        if (!entry) {
            entry = std::make_unique<Skin>();
            const std::string hash = url.substr(url.rfind('/') + 1);
            bool safe = !hash.empty() && hash.size() <= 128;
            for (char ch : hash) safe = safe && std::isalnum(u8(ch)) != 0;
            const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
            if (!safe || !local) {
                entry->state = 3;
                return -1;
            }
            const fs::path dir = fs::path(local) / L"minecraftCplusplus" / L"skins";
            entry->file = dir / (hash + ".png");
            std::error_code ec;
            if (fs::exists(entry->file, ec)) {
                entry->state = 2;
            } else {
                entry->state = 1;
                fs::create_directories(dir, ec);
                std::thread([e = entry.get(), url] {
                    const fs::path tmp = fs::path(e->file).concat(".download");
                    const bool ok = URLDownloadToFileW(nullptr, std::wstring(url.begin(), url.end()).c_str(), tmp.c_str(), 0, nullptr) == S_OK;
                    std::error_code ec2;
                    if (ok) fs::rename(tmp, e->file, ec2);
                    e->state = ok && !ec2 ? 2 : 3;
                }).detach();
            }
        }
        if (entry->state == 2 && entry->rect < 0) {
            entry->state = 3;  // unless it loads below
            std::vector<u8> bytes;
            int w = 0, h = 0, comp = 0;
            u8* px = readFile(entry->file, bytes) ? stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &comp, 4) : nullptr;
            if (px && w == 64 && (h == 64 || h == 32)) {
                // SkinTextureDownloader.processLegacySkin: 64 x 32 skins grow to 64 x 64 (the limbs, which heads do
                // not use, are not mirrored over); opaque base layers; a fully opaque legacy hat layer is see-through.
                std::vector<u8> rgba(64 * 64 * 4, 0);
                std::memcpy(rgba.data(), px, size_t(w) * h * 4);
                auto alpha = [&](int x, int y) -> u8& { return rgba[(size_t(y) * 64 + x) * 4 + 3]; };
                auto noAlpha = [&](int x0, int y0, int x1, int y1) {
                    for (int y = y0; y < y1; ++y)
                        for (int x = x0; x < x1; ++x) alpha(x, y) = 255;
                };
                noAlpha(0, 0, 32, 16);
                if (h == 32) {  // doNotchTransparencyHack
                    bool translucent = false;
                    for (int y = 0; y < 32 && !translucent; ++y)
                        for (int x = 32; x < 64 && !translucent; ++x) translucent = alpha(x, y) < 128;
                    if (!translucent)
                        for (int y = 0; y < 32; ++y)
                            for (int x = 32; x < 64; ++x) alpha(x, y) = 0;
                }
                noAlpha(0, 16, 64, 32);
                noAlpha(16, 48, 48, 64);
                int x, y;
                if (self->atlas_.alloc(64, 64, x, y)) {
                    self->atlas_.blit(x, y, 64, 64, rgba.data(), 64 * 4);
                    const float W = float(self->atlas_.width()), H = float(self->atlas_.height());
                    self->rects_.push_back({float(x) / W, float(y) / H, 64.0f / W, 64.0f / H, 64, 64, false});
                    entry->rect = int(self->rects_.size() - 1);
                    entry->state = 2;
                }
            }
            if (px) stbi_image_free(px);
        }
        return entry->rect;
    }
    int skinState(const std::string& textures) {  // for mesh keys: a skin that finishes downloading changes the mesh
        if (textures.empty()) return 0;
        const json j = json::parse(base64Decode(textures), nullptr, false);
        const json& url = at(at(at(j, "textures"), "SKIN"), "url");
        auto it = url.is_string() ? skins.find(url.get<std::string>()) : skins.end();
        return it == skins.end() ? 0 : it->second->state.load() * 2 + (it->second->rect >= 0 ? 1 : 0);
    }

    // DefaultPlayerSkin.get(profile): the profile's id (none: the nil UUID) picks one of the 18 default skins.
    static std::string defaultSkin(const ui::ProfileInfo& prof) {
        static const char* kSkins[9] = {"alex", "ari", "efe", "kai", "makena", "noor", "steve", "sunny", "zuri"};
        const u8 nil[16] = {};
        const int i = defaultSkinIndex(prof.hasId ? prof.uuid : nil);
        return std::string(i < 9 ? "entity/player/slim/" : "entity/player/wide/") + kSkins[i % 9];
    }

    bool special(Out& out, const Part& p) {
        const json& sm = *p.special;
        const std::string type = strip(sm.value("type", ""));
        const ui::ItemStack& s = p.stack;
        const ui::Items& items = *self->items_;
        const Xf& l = p.local;
        const u32 white = 0xFFFFFFFF;
        auto tinted = [](u32 rgb) { return 0xFF000000u | abgrOf(rgb); };
        // "minecraft:textures/entity/x.png" -> "entity/x"; plain ids go below `dir`.
        auto texPath = [](std::string t, const char* dir) {
            t = strip(t);
            if (t.starts_with("textures/")) {
                t = t.substr(9);
                if (t.ends_with(".png")) t.resize(t.size() - 4);
                return t;
            }
            return std::string(dir) + t;
        };
        // BannerRenderer.submitPatterns: the base layer in the base colour, then up to 16 pattern layers.
        auto patterns = [&](const std::string& layer, const char* dir, int base, const std::vector<ui::BannerLayer>& layers,
                            const PoseFn& pose) {
            entityModel(out, layer, std::string(dir) + "base", l, tinted(ui::dyeDiffuse(base)), true, pose);
            for (size_t i = 0; i < layers.size() && i < 16; ++i)
                entityModel(out, layer, std::string(dir) + strip(layers[i].pattern), l, tinted(ui::dyeDiffuse(layers[i].dye)), true, pose);
        };
        if (type == "shield") {  // ShieldSpecialRenderer
            const std::vector<ui::BannerLayer> layers = ui::bannerPatterns(items, s);
            const int base = ui::baseColor(items, s);
            const bool patterned = !layers.empty() || base >= 0;
            const bool ok = entityModel(out, "shield#main", patterned ? "entity/shield/shield_base" : "entity/shield/shield_base_nopattern", l, white);
            if (ok && patterned) patterns("shield#main", "entity/shield/", base < 0 ? 0 : base, layers, {});
            return ok;
        }
        if (type == "banner") {  // BannerSpecialRenderer -> BannerRenderer.submitSpecial (phase 0)
            int base = 0;
            for (int d = 0; d < 16; ++d)
                if (sm.value("color", "white") == ui::dyeName(d)) base = d;
            const bool wall = sm.value("attachment", "ground") == "wall";
            const std::string pole = wall ? "wall_banner#main" : "standing_banner#main", flag = wall ? "wall_banner#flag" : "standing_banner#flag";
            const PoseFn phase0 = [](const ModelPart& part, size_t, PartPose& pp) {  // BannerFlagModel.setupAnim(0)
                if (part.name == "flag") pp.xRot = (-0.0125f + 0.01f) * kPi;
                return true;
            };
            const bool a = entityModel(out, pole, "entity/banner/banner_base", l, white);
            const bool b = entityModel(out, flag, "entity/banner/banner_base", l, white, false, phase0);
            if (b) patterns(flag, "entity/banner/", base, ui::bannerPatterns(items, s), phase0);
            return a || b;
        }
        if (type == "trident") return entityModel(out, "trident#main", "entity/trident/trident", l, white);
        if (type == "chest") {  // ChestSpecialRenderer: the lid opened by `openness`
            const std::string kind = sm.value("chest_type", "single");
            const char* layer = kind == "left" ? "double_chest_left#main" : kind == "right" ? "double_chest_right#main" : "chest#main";
            std::string tex = strip(sm.value("texture", "normal"));
            if (kind == "left" || kind == "right") tex += "_" + kind;
            const float open = num(sm.value("openness", json()), 0.0f);
            return entityModel(out, layer, "entity/chest/" + tex, l, white, false, [open](const ModelPart& part, size_t, PartPose& pp) {
                if (part.name == "lid" || part.name == "lock") pp.xRot = -(open * kPi / 2);
                return true;
            });
        }
        if (type == "shulker_box") {  // ShulkerBoxRenderer.ShulkerBoxModel.setupAnim(openness)
            const float open = num(sm.value("openness", json()), 0.0f);
            return entityModel(out, "shulker_box#main", "entity/shulker/" + strip(sm.value("texture", "shulker")), l, white, false,
                               [open](const ModelPart& part, size_t, PartPose& pp) {
                                   if (part.name == "lid") pp.x = 0, pp.y = 24.0f - open * 8.0f, pp.z = 0, pp.yRot = 270.0f * open * kDeg;
                                   return true;
                               });
        }
        if (type == "head" || type == "player_head") {  // SkullSpecialRenderer / PlayerHeadSpecialRenderer
            static const char* kHeads[][3] = {{"skeleton", "skeleton_skull#main", "entity/skeleton/skeleton"},
                                              {"wither_skeleton", "wither_skeleton_skull#main", "entity/skeleton/wither_skeleton"},
                                              {"zombie", "zombie_head#main", "entity/zombie/zombie"},
                                              {"creeper", "creeper_head#main", "entity/creeper/creeper"},
                                              {"dragon", "dragon_skull#main", "entity/enderdragon/dragon"},
                                              {"piglin", "piglin_head#main", "entity/piglin/piglin"},
                                              {"player", "player_head#main", "entity/player/wide/steve"}};
            const std::string kind = type == "player_head" ? "player" : strip(sm.value("kind", ""));
            const float anim = num(sm.value("animation", json()), 0.0f);
            const PoseFn pose = [anim](const ModelPart& part, size_t, PartPose& pp) {
                if (part.name == "jaw") pp.xRot = float(std::sin(anim * kPi * 0.2f) + 1.0) * 0.2f;  // DragonHeadModel
                if (part.name == "left_ear") pp.zRot = float(-(std::cos(anim * kPi * 0.2f * 1.2f) + 2.5)) * 0.2f;  // PiglinHeadModel
                if (part.name == "right_ear") pp.zRot = float(std::cos(anim * kPi * 0.2f) + 2.5) * 0.2f;
                return true;
            };
            for (auto& h : kHeads) {
                if (kind != h[0]) continue;
                std::string tex = sm.contains("texture") ? "entity/" + strip(sm.value("texture", "")) : std::string(h[2]);
                int rect = -1;
                ui::ProfileInfo prof;
                if (type == "player_head" && ui::profile(items, s, prof)) {
                    // ResolvableProfile.Dynamic (only a name or only an id): the textures the profile service knows
                    if (prof.textures.empty() && prof.skinTexture.empty() && prof.hasId != !prof.name.empty())
                        prof.textures = resolveProfileTextures(prof.name, prof.hasId ? prof.uuid : nullptr);
                    rect = profileSkin(prof.textures);
                    if (!prof.skinTexture.empty()) tex = prof.skinTexture, rect = -1;  // the profile's skin patch
                    else if (rect < 0) tex = defaultSkin(prof);
                }
                return entityModel(out, h[1], tex, l, white, false, pose, rect);
            }
            return false;
        }
        if (type == "conduit") return entityModel(out, "conduit#shell", "entity/conduit/base", l, white);  // ConduitSpecialRenderer
        if (type == "bell") return entityModel(out, "bell#main", "entity/bell/bell_body", l, white);
        if (type == "book") {  // BookSpecialRenderer: BookModel.setupAnim(open angle, page flips)
            const float open = num(sm.value("open_angle", json()), 0.0f) * kDeg;
            const float f1 = num(sm.value("page1", json()), 0.0f), f2 = num(sm.value("page2", json()), 0.0f);
            return entityModel(out, "book#main", "entity/enchanting_table_book", l, white, false,
                               [=](const ModelPart& part, size_t, PartPose& pp) {
                                   const std::string& n = part.name;
                                   if (n == "left_lid") pp.yRot = kPi + open;
                                   else if (n == "right_lid") pp.yRot = -open;
                                   else if (n == "left_pages") pp.yRot = open, pp.x = std::sin(open);
                                   else if (n == "right_pages") pp.yRot = -open, pp.x = std::sin(open);
                                   else if (n == "flip_page1") pp.yRot = open - open * 2.0f * f1, pp.x = std::sin(open);
                                   else if (n == "flip_page2") pp.yRot = open - open * 2.0f * f2, pp.x = std::sin(open);
                                   return true;
                               });
        }
        if (type == "decorated_pot") {  // DecoratedPotRenderer.submit: neck, top, bottom, then the four sides
            int sherds[4];
            ui::potDecorations(items, s, sherds);
            const bool a = entityModel(out, "decorated_pot_base#main", "entity/decorated_pot/decorated_pot_base", l, white);
            static const char* kSides[4] = {"back", "left", "right", "front"};
            for (int i = 0; i < 4; ++i) {
                std::string tex = "entity/decorated_pot/decorated_pot_side";
                const std::string item = sherds[i] >= 0 ? strip(items.name(sherds[i])) : "";
                if (item.ends_with("_pottery_sherd")) {
                    const std::string pattern = "entity/decorated_pot/" + item.substr(0, item.size() - 14) + "_pottery_pattern";
                    if (assets->texture(pattern) >= 0) tex = pattern;
                }
                const std::string side = kSides[i];
                entityModel(out, "decorated_pot_sides#main", tex, l, white, false,
                            [&side](const ModelPart& part, size_t, PartPose&) { return part.name == side; });
            }
            return a;
        }
        if (type == "copper_golem_statue") {  // CopperGolemStatueModel.setupAnim: the root at y 0, turned upside down
            const std::string pose = sm.value("pose", "standing");
            const char* layer = pose == "sitting" ? "copper_golem_sitting#main" : pose == "running" ? "copper_golem_running#main"
                                : pose == "star" ? "copper_golem_star#main" : "copper_golem#main";
            return entityModel(out, layer, texPath(sm.value("texture", ""), "entity/"), l, white, false,
                               [](const ModelPart&, size_t i, PartPose& pp) {
                                   if (i == 0) pp.y = 0, pp.zRot = kPi;
                                   return true;
                               });
        }
        if (type == "end_cube") {  // EndCubeSpecialRenderer: the end portal / gateway effect's dark sky on a unit cube
            const float uv[4][2] = {};
            for (int d = 0; d < 6; ++d) {
                float pts[4][3];
                const float* n = kNormals[d];
                const int ax = d < 2 ? 1 : d < 4 ? 2 : 0, s0 = (ax + 1) % 3, s1 = (ax + 2) % 3;
                const float c = n[ax] > 0 ? 1.0f : 0.0f;
                const float corners[4][2] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
                for (int k = 0; k < 4; ++k) pts[k][ax] = c, pts[k][s0] = corners[k][0], pts[k][s1] = corners[k][1];
                quad(out, l, pts, uv, n, kTexSolid, 0xFF000000u | abgrOf(0x111727), kQuadNoShade, false);
            }
            return true;
        }
        return false;
    }

    // Builds a mesh from parts; quads are appended to the store.
    ItemMesh build(const std::vector<Part>& parts) {
        ItemMesh mesh;
        for (const Part& p : parts) {
            if (mesh.partCount >= 4) break;
            std::shared_ptr<JModel> m = model(p.model);
            Out out;
            bool any = false;
            if (p.special) any = special(out, p);
            if (!any) {
                if (m->generated) generated(out, *m, p.local, p.tints);
                else if (!m->elements.empty()) cuboids(out, *m, p.local, p.tints);
                else if (!m->particle.empty()) {  // other special renderers: the particle texture, extruded
                    JModel flatModel;
                    flatModel.generated = true;
                    flatModel.layers.push_back(m->particle);
                    generated(out, flatModel, p.local, p.tints);
                }
            }
            if (out.cutout.empty() && out.translucent.empty()) continue;
            const u32 total = u32(out.cutout.size() + out.translucent.size());
            if (self->store_.size() + self->uploaded_ + total > kMaxQuads) {
                if (!storeFull) LOG("visuals: item mesh store full (%u quads)", kMaxQuads);
                storeFull = true;
                break;
            }
            ItemMesh::Part& part = mesh.parts[mesh.partCount++];
            const u32 base = self->uploaded_ + u32(self->store_.size());
            part.first = base, part.count = u32(out.cutout.size());
            part.tfirst = base + part.count, part.tcount = u32(out.translucent.size());
            self->store_.insert(self->store_.end(), out.cutout.begin(), out.cutout.end());
            self->store_.insert(self->store_.end(), out.translucent.begin(), out.translucent.end());
            for (int c = 0; c < 9; ++c) {
                const bool left = c == 1 || c == 3;
                int src = c;
                if (left && !m->display[c].set) src = c - 1;  // left hand defaults to the right hand transform
                const JModel::Tr& tr = m->display[src];
                part.display[c] = displayXf(tr.rot, tr.trans, tr.scale, left, !tr.set);
            }
            part.display[size_t(Display::None)] = xfTranslate(-0.5f, -0.5f, -0.5f);
            // ItemStackRenderState.getModelBoundingBox (GROUND display) for dropped items.
            float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
            for (const auto* v : {&out.cutout, &out.translucent})
                for (const MeshQuad& q : *v)
                    for (int k = 0; k < 4; ++k) {
                        float p[3];
                        xfPoint(part.display[size_t(Display::Ground)], q.pos[k], p);
                        for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], p[a]), hi[a] = std::max(hi[a], p[a]);
                    }
            if (mesh.partCount == 1) mesh.groundMinY = lo[1], mesh.groundDepth = hi[2] - lo[2];
            else mesh.groundMinY = std::min(mesh.groundMinY, lo[1]), mesh.groundDepth = std::max(mesh.groundDepth, hi[2] - lo[2]);
            float sLo = 1e9f, sHi = -1e9f;  // the ON_SHELF display's vertical extent
            for (const auto* v : {&out.cutout, &out.translucent})
                for (const MeshQuad& q : *v)
                    for (int k = 0; k < 4; ++k) {
                        float p[3];
                        xfPoint(part.display[size_t(Display::OnShelf)], q.pos[k], p);
                        sLo = std::min(sLo, p[1]), sHi = std::max(sHi, p[1]);
                    }
            if (mesh.partCount == 1) mesh.shelfMinY = sLo, mesh.shelfMaxY = sHi;
            else mesh.shelfMinY = std::min(mesh.shelfMinY, sLo), mesh.shelfMaxY = std::max(mesh.shelfMaxY, sHi);
        }
        return mesh;
    }
};

ItemModels::ItemModels() : impl_(std::make_unique<Impl>()) { impl_->self = this; }
ItemModels::~ItemModels() = default;

bool ItemModels::init(const ui::Resources& res, const EntityAssets& assets, const std::vector<std::string>& itemNames,
                      const fs::path& componentReports) {
    impl_->res = &res;
    impl_->assets = &assets;
    impl_->itemNames = itemNames;
    impl_->componentReports = componentReports;
    atlas_.init(2048, 2048);
    store_.reserve(1 << 14);
    res.image("assets/minecraft/textures/colormap/grass.png", impl_->grass);
    res.image("assets/minecraft/textures/colormap/foliage.png", impl_->foliage);
    // A white texel for untextured quads (name tag backgrounds).
    const u8 white[4 * 4 * 4] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    int x, y;
    if (atlas_.alloc(4, 4, x, y)) {
        atlas_.blit(x, y, 4, 4, white, 16);
        const float W = float(atlas_.width()), H = float(atlas_.height());
        rects_.push_back({(float(x) + 1.5f) / W, (float(y) + 1.5f) / H, 1.0f / W, 1.0f / H, 4, 4, false});
    }
    return true;
}

const ui::Resources* ItemModels::resources() const { return impl_->res; }

int ItemModels::texture(const std::string& path) {
    Impl::Tex* t = impl_->tex(path);
    return t ? t->rect : -1;
}

void ItemModels::tick(u64 tick) {
    std::vector<u8> pixels;
    for (Impl::Animated& a : impl_->animated) {
        const u64 state = a.anim.stateAt(tick);
        if (state == a.shown) continue;
        a.shown = state;
        a.anim.frameAt(a.strip, tick, pixels);
        atlas_.blit(a.x, a.y, a.anim.fw, a.anim.fh, pixels.data(), a.anim.fw * 4);
    }
}

ui::ItemStack ItemModels::plainStack(int item, i32 dye) {
    ui::ItemStack s;
    s.item = item, s.count = 1, s.dyedColor = dye;
    return s;
}

const ItemMesh& ItemModels::mesh(const ui::ItemStack& s, const ui::ItemContext& c) {
    if (s.empty() || !items_ || size_t(s.item) >= impl_->itemNames.size()) return impl_->emptyMesh;
    std::vector<Impl::Part> parts;
    std::string key;
    key.reserve(128);
    try {
        impl_->evaluateStack(s, c, Xf{}, parts, key, 0);
    } catch (const std::exception& ex) {
        LOG("visuals: item model of %s: %s", impl_->itemNames[size_t(s.item)].c_str(), ex.what());
        return impl_->emptyMesh;
    }
    auto it = impl_->useMeshes.find(key);
    if (it != impl_->useMeshes.end()) return it->second;
    ItemMesh m;
    try {
        m = impl_->build(parts);
    } catch (const std::exception& ex) {
        LOG("visuals: item model of %s: %s", impl_->itemNames[size_t(s.item)].c_str(), ex.what());
    }
    return impl_->useMeshes.emplace(std::move(key), m).first->second;
}

std::string ItemModels::specialKey(const nlohmann::json& sm, const ui::ItemStack& s) {
    if (!items_) return "";
    const std::string type = strip(sm.value("type", ""));
    std::string key = "|" + type;
    if (type == "shield" || type == "banner") {
        key += std::to_string(ui::baseColor(*items_, s));
        for (const ui::BannerLayer& l : ui::bannerPatterns(*items_, s)) key += "," + l.pattern + ":" + std::to_string(l.dye);
    } else if (type == "player_head") {
        ui::ProfileInfo prof;
        if (ui::profile(*items_, s, prof)) {
            if (prof.textures.empty() && prof.skinTexture.empty() && prof.hasId != !prof.name.empty()) {
                int resolving = 0;
                prof.textures = resolveProfileTextures(prof.name, prof.hasId ? prof.uuid : nullptr, &resolving);
                key += "r" + std::to_string(resolving);
            }
            key += prof.skinTexture + "#" + prof.textures + "#" + std::to_string(impl_->skinState(prof.textures));
            key.append(reinterpret_cast<const char*>(prof.uuid), prof.hasId ? 16 : 0);
        }
    } else if (type == "decorated_pot") {
        int sherds[4];
        ui::potDecorations(*items_, s, sherds);
        for (int i : sherds) key += "," + std::to_string(i);
    }
    return key;
}

bool ItemModels::specialQuads(const nlohmann::json& sm, const ui::ItemStack& s, const Xf& local, std::vector<MeshQuad>& out) {
    if (!items_) return false;
    Impl::Part p;
    p.special = &sm;
    p.stack = s;
    p.local = local;
    Impl::Out o;
    try {
        if (!impl_->special(o, p)) return false;
    } catch (const std::exception&) {
        return false;
    }
    out.insert(out.end(), o.cutout.begin(), o.cutout.end());
    out.insert(out.end(), o.translucent.begin(), o.translucent.end());
    return true;
}

const ItemMesh& ItemModels::headMesh(const ui::ItemStack& s) {
    static const json kPlayerHead = {{"type", "minecraft:player_head"}};
    const std::string key = "head" + specialKey(kPlayerHead, s);
    auto it = impl_->namedMeshes.find(key);
    if (it != impl_->namedMeshes.end()) return it->second;
    Impl::Part p;
    p.special = &kPlayerHead;
    p.stack = s;
    return impl_->namedMeshes.emplace(key, impl_->build({p})).first->second;
}

ItemModels::AtlasView ItemModels::atlasView(u32 tex) const {
    if (tex == kTexItems) return {atlas_.data(), atlas_.width(), atlas_.height()};
    if (tex == kTexEntity) return {impl_->assets->atlas().data(), int(impl_->assets->atlasWidth()), int(impl_->assets->atlasHeight())};
    return {};
}

std::string ItemModels::itemParticleTexture(int item) {
    if (item < 0 || size_t(item) >= impl_->itemNames.size() || !items_) return {};
    auto it = impl_->itemParticles.find(item);
    if (it != impl_->itemParticles.end()) return it->second;
    std::string result;
    try {
        std::vector<Impl::Part> parts;
        std::string key;
        ui::ItemContext c;
        c.display = ui::ItemDisplay::Ground;
        impl_->evaluateStack(plainStack(item), c, Xf{}, parts, key, 0);
        if (!parts.empty() && !parts[0].model.empty()) {
            const auto m = impl_->model(parts[0].model);
            result = strip(!m->particle.empty() ? m->particle : m->layers.empty() ? std::string() : m->layers[0]);
        }
    } catch (const std::exception&) {
    }
    return impl_->itemParticles.emplace(item, result).first->second;
}

const ItemMesh& ItemModels::blockModelMesh(const std::string& model) {
    auto it = impl_->namedMeshes.find(model);
    if (it != impl_->namedMeshes.end()) return it->second;
    Impl::Part p;
    p.model = model;
    return impl_->namedMeshes.emplace(model, impl_->build({p})).first->second;
}

const ItemMesh& ItemModels::entityLayerMesh(const std::string& layer, const std::string& texture) {
    const std::string key = "layer:" + layer + ":" + texture;
    auto it = impl_->namedMeshes.find(key);
    if (it != impl_->namedMeshes.end()) return it->second;
    ItemMesh mesh;
    Impl::Out out;
    if (impl_->entityModel(out, layer, texture, Xf{}, 0xFFFFFFFF) && !out.cutout.empty() &&
        store_.size() + uploaded_ + out.cutout.size() <= kMaxQuads) {
        ItemMesh::Part& part = mesh.parts[mesh.partCount++];
        part.first = uploaded_ + u32(store_.size()), part.count = u32(out.cutout.size());
        part.tfirst = part.first + part.count;
        store_.insert(store_.end(), out.cutout.begin(), out.cutout.end());
    }
    return impl_->namedMeshes.emplace(key, mesh).first->second;
}

bool ItemModels::skullLayer(const std::string& itemName, std::string& layer, std::string& texture) {
    static const char* kHeads[][3] = {{"skeleton_skull", "skeleton_skull#main", "entity/skeleton/skeleton"},
                                      {"wither_skeleton_skull", "wither_skeleton_skull#main", "entity/skeleton/wither_skeleton"},
                                      {"zombie_head", "zombie_head#main", "entity/zombie/zombie"},
                                      {"creeper_head", "creeper_head#main", "entity/creeper/creeper"},
                                      {"dragon_head", "dragon_skull#main", "entity/enderdragon/dragon"},
                                      {"piglin_head", "piglin_head#main", "entity/piglin/piglin"},
                                      {"player_head", "player_head#main", "entity/player/wide/steve"}};
    for (auto& h : kHeads)
        if (itemName == h[0]) {
            layer = h[1], texture = h[2];
            return true;
        }
    return false;
}

std::string ItemModels::itemName(int item) const {
    if (item < 0 || size_t(item) >= impl_->itemNames.size()) return {};
    return strip(impl_->itemNames[size_t(item)]);
}

const ItemModels::HandInfo& ItemModels::handInfo(int item) {
    auto it = impl_->handInfos.find(item);
    if (it != impl_->handInfos.end()) return it->second;
    HandInfo h;
    if (item >= 0 && size_t(item) < impl_->itemNames.size()) {
        const std::string name = strip(impl_->itemNames[size_t(item)]);
        std::ifstream f(impl_->componentReports / (name + ".json"));
        if (f) {
            json j = json::parse(f, nullptr, false);
            const json& mods = at(at(j, "components"), "minecraft:attribute_modifiers");
            if (mods.is_array())
                for (const json& m : mods)
                    if (m.is_object() && strip(m.value("type", "")) == "attack_speed" && m.value("operation", "") == "add_value")
                        h.attackSpeed += num(m.value("amount", json()), 0);
        }
        const json& def = impl_->jsonAt("assets/minecraft/items/" + name + ".json");
        if (def.is_object()) {
            h.swapScale = num(def.value("swap_animation_scale", json()), 1.0f);
            h.swapAnimation = def.value("hand_animation_on_swap", true);
        }
    }
    return impl_->handInfos.emplace(item, h).first->second;
}

const ItemMesh& ItemModels::paintingMesh(const std::string& assetId, int width, int height) {
    const std::string key = "painting:" + assetId + ":" + std::to_string(width) + "x" + std::to_string(height);
    auto it = impl_->namedMeshes.find(key);
    if (it != impl_->namedMeshes.end()) return it->second;
    ItemMesh mesh;
    Impl::Tex* front = impl_->tex("painting/" + strip(assetId));
    Impl::Tex* back = impl_->tex("painting/back");
    if (front && back && width > 0 && height > 0 && store_.size() + uploaded_ + u32(6 * width * height) <= kMaxQuads) {
        // PaintingRenderer.renderPainting: one segment per block (front at z = -1/32 facing -z, the whole back sprite at
        // +1/32, edges from the back sprite's first pixel row / column on the outer segments). kPaintingSegmentQuads
        // quads per segment in segment order (x + y * width); edges inside the painting stay empty (zero area).
        const Rect& f = rects_[size_t(front->rect)];
        const Rect& b = rects_[size_t(back->rect)];
        const float e = 1.0f / 32, offX = -float(width) / 2, offY = -float(height) / 2;
        const float bu0 = b.u0, bu1 = b.u0 + b.du, bv0 = b.v0, bv1 = b.v0 + b.dv, tbv1 = b.v0 + b.dv / 16.0f, lru1 = b.u0 + b.du / 16.0f;
        Impl::Out out;
        const Xf id;
        auto Q = [&](const float p[4][3], const float uv[4][2], float nx, float ny, float nz) {
            const float n[3] = {nx, ny, nz};
            impl_->quad(out, id, p, uv, n, kTexItems, 0xFFFFFFFF, 0, false);
        };
        auto none = [&] {
            const float p[4][3] = {}, uv[4][2] = {};
            Q(p, uv, 0, 0, 1);
        };
        for (int sy = 0; sy < height; ++sy)
            for (int sx = 0; sx < width; ++sx) {
                const float x0 = offX + float(sx + 1), x1 = offX + float(sx), y0 = offY + float(sy + 1), y1 = offY + float(sy);
                const float fu0 = f.u0 + f.du * float(width - sx) / float(width), fu1 = f.u0 + f.du * float(width - sx - 1) / float(width);
                const float fv0 = f.v0 + f.dv * float(height - sy) / float(height), fv1 = f.v0 + f.dv * float(height - sy - 1) / float(height);
                {
                    const float p[4][3] = {{x0, y1, -e}, {x1, y1, -e}, {x1, y0, -e}, {x0, y0, -e}};
                    const float uv[4][2] = {{fu1, fv0}, {fu0, fv0}, {fu0, fv1}, {fu1, fv1}};
                    Q(p, uv, 0, 0, -1);
                }
                {
                    const float p[4][3] = {{x0, y0, e}, {x1, y0, e}, {x1, y1, e}, {x0, y1, e}};
                    const float uv[4][2] = {{bu1, bv0}, {bu0, bv0}, {bu0, bv1}, {bu1, bv1}};
                    Q(p, uv, 0, 0, 1);
                }
                if (sy == height - 1) {
                    const float p[4][3] = {{x0, y0, -e}, {x1, y0, -e}, {x1, y0, e}, {x0, y0, e}};
                    const float uv[4][2] = {{bu0, bv0}, {bu1, bv0}, {bu1, tbv1}, {bu0, tbv1}};
                    Q(p, uv, 0, 1, 0);
                } else {
                    none();
                }
                if (sy == 0) {
                    const float p[4][3] = {{x0, y1, e}, {x1, y1, e}, {x1, y1, -e}, {x0, y1, -e}};
                    const float uv[4][2] = {{bu0, bv0}, {bu1, bv0}, {bu1, tbv1}, {bu0, tbv1}};
                    Q(p, uv, 0, -1, 0);
                } else {
                    none();
                }
                if (sx == width - 1) {
                    const float p[4][3] = {{x0, y0, e}, {x0, y1, e}, {x0, y1, -e}, {x0, y0, -e}};
                    const float uv[4][2] = {{lru1, bv0}, {lru1, bv1}, {bu0, bv1}, {bu0, bv0}};
                    Q(p, uv, -1, 0, 0);
                } else {
                    none();
                }
                if (sx == 0) {
                    const float p[4][3] = {{x1, y0, -e}, {x1, y1, -e}, {x1, y1, e}, {x1, y0, e}};
                    const float uv[4][2] = {{lru1, bv0}, {lru1, bv1}, {bu0, bv1}, {bu0, bv0}};
                    Q(p, uv, 1, 0, 0);
                } else {
                    none();
                }
            }
        ItemMesh::Part& part = mesh.parts[mesh.partCount++];
        part.first = uploaded_ + u32(store_.size()), part.count = u32(out.cutout.size());
        part.tfirst = part.first + part.count, part.tcount = 0;
        store_.insert(store_.end(), out.cutout.begin(), out.cutout.end());
    }
    return impl_->namedMeshes.emplace(key, mesh).first->second;
}

u32 ItemModels::addQuads(const MeshQuad* quads, u32 count) {
    if (store_.size() + uploaded_ + count > kMaxQuads) return UINT32_MAX;
    const u32 first = uploaded_ + u32(store_.size());
    store_.insert(store_.end(), quads, quads + count);
    return first;
}

bool ItemModels::takePending(const MeshQuad*& data, u32& count, u32& firstOut) {
    if (store_.empty()) return false;
    data = store_.data(), count = u32(store_.size()), firstOut = uploaded_;
    return true;
}

void ItemModels::releasePending() {
    uploaded_ += u32(store_.size());
    store_.clear();
}

const ItemModels::Equippable& ItemModels::equippable(int item) {
    auto it = impl_->equippables.find(item);
    if (it != impl_->equippables.end()) return it->second;
    Equippable e;
    if (item >= 0 && size_t(item) < impl_->itemNames.size()) {
        const std::string name = strip(impl_->itemNames[size_t(item)]);
        // The game's default item components (data generator reports, cached with the other reports).
        std::ifstream f(impl_->componentReports / (name + ".json"));
        if (f) {
            json j = json::parse(f, nullptr, false);
            const json& eq = at(at(j, "components"), "minecraft:equippable");
            if (eq.is_object()) {
                static const char* kSlots[8] = {"mainhand", "offhand", "feet", "legs", "chest", "head", "body", "saddle"};
                const std::string slot = eq.value("slot", "");
                for (int s = 0; s < 8; ++s)
                    if (slot == kSlots[s]) e.slot = s;
                e.asset = strip(eq.value("asset_id", ""));
            }
        } else {  // no reports: armor by name
            static const std::pair<const char*, int> kPieces[] = {{"_helmet", 5}, {"_chestplate", 4}, {"_leggings", 3}, {"_boots", 2}};
            static const std::pair<const char*, const char*> kMats[] = {
                {"leather_", "leather"}, {"chainmail_", "chainmail"}, {"iron_", "iron"},       {"golden_", "gold"},
                {"diamond_", "diamond"}, {"netherite_", "netherite"}, {"copper_", "copper"}, {"turtle_", "turtle_scute"}};
            for (auto [suffix, slot] : kPieces)
                if (name.ends_with(suffix))
                    for (auto [prefix, asset] : kMats)
                        if (name.starts_with(prefix)) e.slot = slot, e.asset = asset;
            if (name == "elytra") e.slot = 4, e.asset = "elytra";
            if (name == "carved_pumpkin" || name.ends_with("_skull") || name.ends_with("_head")) e.slot = 5;
        }
    }
    return impl_->equippables.emplace(item, e).first->second;
}

const std::vector<EquipmentLayer>& ItemModels::equipmentLayers(const std::string& asset, const char* layerType) {
    const std::string key = asset + "|" + layerType;
    auto it = impl_->equipLayers.find(key);
    if (it != impl_->equipLayers.end()) return it->second;
    std::vector<EquipmentLayer> layers;
    const json& j = impl_->jsonAt("assets/minecraft/equipment/" + asset + ".json");
    if (j.is_object() && j.contains("layers") && j["layers"].contains(layerType))
        for (const json& l : j["layers"][layerType]) {
            EquipmentLayer e;
            e.tex = impl_->assets->texture(std::string("entity/equipment/") + layerType + "/" + strip(l.value("texture", "")));
            if (l.contains("dyeable")) {
                e.dyeable = true;
                const json& d = l["dyeable"];
                e.undyed = d.is_object() && d.contains("color_when_undyed") ? i32(d["color_when_undyed"].get<i64>() & 0xFFFFFF) : -1;
            }
            e.playerTexture = l.value("use_player_texture", false);
            e.path = std::string("entity/equipment/") + layerType + "/" + strip(l.value("texture", ""));
            layers.push_back(e);
        }
    return impl_->equipLayers.emplace(key, std::move(layers)).first->second;
}

const std::vector<EquipmentLayer>& ItemModels::itemLayers(int item, int type) {
    static const std::vector<EquipmentLayer> kNone;
    if (item < 0 || size_t(item) >= impl_->itemNames.size() || type < 0 || type > 3) return kNone;
    if (impl_->itemLayers.empty()) impl_->itemLayers.assign(impl_->itemNames.size(), {nullptr, nullptr, nullptr, nullptr});
    auto& slot = impl_->itemLayers[size_t(item)][size_t(type)];
    if (!slot) {
        static const char* kTypes[4] = {"humanoid", "humanoid_leggings", "humanoid_baby", "wings"};
        const Equippable& eq = equippable(item);
        slot = eq.asset.empty() ? &kNone : &equipmentLayers(eq.asset, kTypes[type]);
    }
    return *slot;
}

bool ItemModels::mountLayer(std::string_view entity, int slot, bool baby, const char*& model, const char*& layerType) {
    struct Row {
        const char* entity;
        int slot;
        const char *model, *babyModel, *layer;
    };
    // The renderers' SimpleEquipmentLayer / LlamaDecorLayer registrations (no baby model: babies draw nothing).
    static const Row kRows[] = {
        {"horse", 6, "horse_armor#main", nullptr, "horse_body"},
        {"horse", 7, "horse#saddle", nullptr, "horse_saddle"},
        {"donkey", 7, "donkey#saddle", nullptr, "donkey_saddle"},
        {"mule", 7, "mule#saddle", nullptr, "mule_saddle"},
        {"zombie_horse", 6, "undead_horse_armor#main", nullptr, "horse_body"},
        {"zombie_horse", 7, "zombie_horse#saddle", nullptr, "zombie_horse_saddle"},
        {"skeleton_horse", 6, "undead_horse_armor#main", nullptr, "horse_body"},
        {"skeleton_horse", 7, "skeleton_horse#saddle", nullptr, "skeleton_horse_saddle"},
        {"llama", 6, "llama#decor", nullptr, "llama_body"},
        {"trader_llama", 6, "llama#decor", "llama_baby#decor", "llama_body"},  // babies: the trader decor only
        {"pig", 7, "pig#saddle", nullptr, "pig_saddle"},
        {"strider", 7, "strider#saddle", nullptr, "strider_saddle"},
        {"camel", 7, "camel#saddle", nullptr, "camel_saddle"},
        {"camel_husk", 7, "camel_husk#saddle", nullptr, "camel_husk_saddle"},
        {"wolf", 6, "wolf_armor#main", nullptr, "wolf_body"},
        {"happy_ghast", 6, "happy_ghast_harness#main", "happy_ghast_baby_harness#main", "happy_ghast_body"},
        {"nautilus", 6, "nautilus_armor#main", nullptr, "nautilus_body"},
        {"nautilus", 7, "nautilus#saddle", nullptr, "nautilus_saddle"},
    };
    if (entity.starts_with("minecraft:")) entity.remove_prefix(10);
    for (const Row& r : kRows)
        if (r.slot == slot && entity == r.entity) {
            model = baby ? r.babyModel : r.model;
            layerType = r.layer;
            return model != nullptr;
        }
    return false;
}

std::string ItemModels::blockParticleTexture(const std::string& blockName) {
    auto it = impl_->particleTextures.find(blockName);
    if (it != impl_->particleTextures.end()) return it->second;
    std::string result;
    try {
    const json& bs = impl_->jsonAt("assets/minecraft/blockstates/" + strip(blockName) + ".json");
    std::string modelName;
    if (bs.is_object()) {
        auto first = [&](const json& v) -> std::string {
            if (v.is_array() && !v.empty()) return v[0].value("model", "");
            if (v.is_object()) return v.value("model", "");
            return "";
        };
        if (bs.contains("variants") && bs["variants"].is_object() && !bs["variants"].empty())
            modelName = first(bs["variants"].begin().value());
        else if (bs.contains("multipart") && bs["multipart"].is_array() && !bs["multipart"].empty())
            modelName = first(bs["multipart"][0].value("apply", json()));
    }
    if (!modelName.empty()) result = strip(impl_->model(modelName)->particle);
    } catch (const std::exception& ex) {
        LOG("visuals: block particle texture of %s: %s", blockName.c_str(), ex.what());
    }
    return impl_->particleTextures.emplace(blockName, result).first->second;
}

}  // namespace ent
