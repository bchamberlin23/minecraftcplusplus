#include "entity/item_models.h"

#include "entity/entity_assets.h"
#include "ui/resources.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <fstream>

namespace ent {

namespace {

using json = nlohmann::json;
constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;

std::string strip(std::string s) {
    const size_t c = s.find(':');
    return c == std::string::npos ? s : s.substr(c + 1);
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

const char* kDisplayNames[8] = {"firstperson_righthand", "firstperson_lefthand", "thirdperson_righthand",
                                "thirdperson_lefthand",  "head",                 "gui",
                                "ground",                "fixed"};
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
    } display[8];
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
    std::unordered_map<std::string, Tex> textures;
    std::unordered_map<std::string, std::shared_ptr<JModel>> models;
    std::unordered_map<u64, ItemMesh> meshes;
    std::unordered_map<std::string, ItemMesh> namedMeshes;
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
        const std::string key = strip(rawName);
        auto it = textures.find(key);
        if (it != textures.end()) return it->second.rect >= 0 ? &it->second : nullptr;
        Tex& t = textures[key];
        ui::Image img;
        if (!res->image("assets/minecraft/textures/" + key + ".png", img) || img.w <= 0 || img.h <= 0) return nullptr;
        const int w = img.w, h = (img.h > img.w && img.h % img.w == 0) ? img.w : img.h;  // animated strips: first frame
        t.img.w = w, t.img.h = h;
        t.img.rgba.assign(img.rgba.begin(), img.rgba.begin() + size_t(w) * h * 4);
        int x, y;
        if (!self->atlas_.alloc(w, h, x, y)) {
            LOG("visuals: item atlas full (%s not loaded)", key.c_str());
            return nullptr;
        }
        self->atlas_.blit(x, y, w, h, t.img.rgba.data(), w * 4);
        bool translucent = false;
        for (size_t i = 3; i < t.img.rgba.size(); i += 4) translucent |= t.img.rgba[i] > 25 && t.img.rgba[i] < 230;
        const float W = float(self->atlas_.width()), H = float(self->atlas_.height());
        self->rects_.push_back({float(x) / W, float(y) / H, float(w) / W, float(h) / H, w, h, translucent});
        t.rect = int(self->rects_.size() - 1);
        return &t;
    }

    std::shared_ptr<JModel> model(const std::string& rawName) {
        const std::string name = strip(rawName);
        if (auto it = models.find(name); it != models.end()) return it->second;
        auto m = std::make_shared<JModel>();
        models[name] = m;
        std::unordered_map<std::string, std::string> vars;
        const json* elements = nullptr;
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
            if (j.contains("display") && j["display"].is_object())
                for (int c = 0; c < 8; ++c) {
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

    // Entity model layer posed at its initial pose (special item renderers draw these with no extra transform).
    bool entityModel(Out& out, const std::string& layer, const std::string& texture, const Xf& local, u32 color) {
        const int mi = assets->model(layer), ti = assets->texture(texture);
        if (mi < 0 || ti < 0) return false;
        const auto& m = assets->modelAt(mi);
        const TexRect& tr = assets->rect(ti);
        std::vector<Xf> mats(m.parts.size());
        for (size_t i = 0; i < m.parts.size(); ++i) {
            const ModelPart& part = m.parts[i];
            const PartPose& p = part.pose;
            Xf l = xfTranslate(p.x / 16.0f, p.y / 16.0f, p.z / 16.0f);
            if (p.zRot != 0) l = xfMul(l, xfRotZ(p.zRot));
            if (p.yRot != 0) l = xfMul(l, xfRotY(p.yRot));
            if (p.xRot != 0) l = xfMul(l, xfRotX(p.xRot));
            if (p.xScale != 1 || p.yScale != 1 || p.zScale != 1) l = xfMul(l, xfScale(p.xScale, p.yScale, p.zScale));
            mats[i] = xfMul(part.parent >= 0 ? mats[size_t(part.parent)] : local, l);
            for (u32 q = part.quadFirst; q < part.quadFirst + part.quadCount; ++q) {
                const GpuQuad& g = assets->quads()[q];
                float pos[4][3], uvs[4][2];
                for (int k = 0; k < 4; ++k) {
                    for (int a = 0; a < 3; ++a) pos[k][a] = g.pos[k][a] / 16.0f;
                    uvs[k][0] = tr.u0 + g.uv[k][0] * tr.du, uvs[k][1] = tr.v0 + g.uv[k][1] * tr.dv;
                }
                quad(out, mats[i], pos, uvs, g.normal, kTexEntity, color, 0, false);
            }
        }
        return true;
    }

    // --- item definitions -----------------------------------------------------------------------------------
    struct Part {
        std::string model;
        std::vector<u32> tints;
        std::string special, specialTex, specialKind;
        u32 specialColor = 0xFFFFFF;
        Xf local;
    };

    u32 colormap(const ui::Image& img, float temp, float down) {
        if (img.w < 256 || img.h < 256) return 0xFF7FB238;
        temp = std::clamp(temp, 0.f, 1.f);
        down = std::clamp(down, 0.f, 1.f) * temp;
        const u8* p = img.at(int((1.0f - temp) * 255.0f), int((1.0f - down) * 255.0f));
        return 0xFF000000u | (u32(p[0]) << 16) | (u32(p[1]) << 8) | p[2];
    }
    u32 tint(const json& t, i32 dye) {
        const std::string type = strip(t.value("type", ""));
        auto def = [&](const char* k, u32 d) { return t.contains(k) && t[k].is_number() ? u32(t[k].get<i64>()) : d; };
        if (type == "constant") return def("value", 0xFFFFFFFF);
        if (type == "dye") return dye >= 0 ? 0xFF000000u | u32(dye) : def("default", 0xFFFFFFFF);
        if (type == "grass") return colormap(grass, num(t.value("temperature", json()), 0.5f), num(t.value("downfall", json()), 1.f));
        if (type == "foliage") return colormap(foliage, num(t.value("temperature", json()), 0.5f), num(t.value("downfall", json()), 1.f));
        return def("default", 0xFFFFFFFF);
    }

    void evaluate(const json& n, i32 dye, bool flat, const Xf& local, std::vector<Part>& out, int depth) {
        if (!n.is_object() || depth > 16) return;
        const Xf here = n.contains("transformation") ? xfMul(local, transformation(n["transformation"])) : local;
        const std::string type = strip(n.value("type", ""));
        if (type == "model") {
            Part p;
            p.model = n.value("model", "");
            p.local = here;
            if (n.contains("tints"))
                for (const json& t : n["tints"]) p.tints.push_back(tint(t, dye));
            out.push_back(std::move(p));
        } else if (type == "composite") {
            if (n.contains("models"))
                for (const json& m : n["models"]) evaluate(m, dye, flat, here, out, depth + 1);
        } else if (type == "condition") {
            evaluate(n.value("on_false", json()), dye, flat, here, out, depth + 1);
        } else if (type == "select") {
            const std::string prop = strip(n.value("property", ""));
            const json* pick = nullptr;
            if (prop == "display_context" && n.contains("cases")) {
                for (const json& c : n["cases"]) {
                    const json& when = c.value("when", json());
                    auto match = [&](const std::string& s) {
                        return flat ? (s == "gui" || s == "ground" || s == "fixed") : (s.find("person") != std::string::npos);
                    };
                    bool hit = false;
                    if (when.is_string()) hit = match(when.get<std::string>());
                    else if (when.is_array())
                        for (const json& w : when) hit |= w.is_string() && match(w.get<std::string>());
                    if (hit) {
                        pick = &at(c, "model");
                        break;
                    }
                }
            }
            if (pick) evaluate(*pick, dye, flat, here, out, depth + 1);
            else if (n.contains("fallback")) evaluate(n["fallback"], dye, flat, here, out, depth + 1);
            else if (n.contains("cases") && !n["cases"].empty()) evaluate(n["cases"][0].value("model", json()), dye, flat, here, out, depth + 1);
        } else if (type == "range_dispatch") {
            const json* best = nullptr;
            float bestT = -1e30f;
            if (n.contains("entries"))
                for (const json& e : n["entries"]) {
                    const float t = num(e.value("threshold", json()), 0);
                    if (t <= 0 && t >= bestT) bestT = t, best = &e;
                }
            if (best) evaluate((*best).value("model", json()), dye, flat, here, out, depth + 1);
            else if (n.contains("fallback")) evaluate(n["fallback"], dye, flat, here, out, depth + 1);
        } else if (type == "special") {
            Part p;
            p.model = n.value("base", "");
            p.local = here;
            const json& sm = at(n, "model");
            if (!sm.is_object()) return;
            p.special = strip(sm.value("type", ""));
            p.specialTex = strip(sm.value("texture", ""));
            p.specialKind = sm.value("kind", "");
            if (p.special == "banner") {
                static const std::pair<const char*, u32> kDye[] = {
                    {"white", 0xF9FFFE}, {"orange", 0xF9801D}, {"magenta", 0xC74EBD}, {"light_blue", 0x3AB3DA},
                    {"yellow", 0xFED83D}, {"lime", 0x80C71F}, {"pink", 0xF38BAA}, {"gray", 0x474F52},
                    {"light_gray", 0x9D9D97}, {"cyan", 0x169C9C}, {"purple", 0x8932B8}, {"blue", 0x3C44AA},
                    {"brown", 0x835432}, {"green", 0x5E7C16}, {"red", 0xB02E26}, {"black", 0x1D1D21}};
                for (auto [name, c] : kDye)
                    if (sm.value("color", "white") == name) p.specialColor = c;
            }
            out.push_back(std::move(p));
        }
    }

    bool special(Out& out, const Part& p) {
        const Xf& l = p.local;
        if (p.special == "shield") return entityModel(out, "shield#main", "entity/shield/shield_base_nopattern", l, 0xFFFFFFFF);
        if (p.special == "trident") return entityModel(out, "trident#main", "entity/trident/trident", l, 0xFFFFFFFF);
        if (p.special == "chest") return entityModel(out, "chest#main", "entity/chest/" + (p.specialTex.empty() ? "normal" : p.specialTex), l, 0xFFFFFFFF);
        if (p.special == "shulker_box")
            return entityModel(out, "shulker_box#main", "entity/shulker/" + (p.specialTex.empty() ? "shulker" : p.specialTex), l, 0xFFFFFFFF);
        if (p.special == "head" || p.special == "player_head") {
            static const char* kHeads[][3] = {{"skeleton", "skeleton_skull#main", "entity/skeleton/skeleton"},
                                              {"wither_skeleton", "wither_skeleton_skull#main", "entity/skeleton/wither_skeleton"},
                                              {"zombie", "zombie_head#main", "entity/zombie/zombie"},
                                              {"creeper", "creeper_head#main", "entity/creeper/creeper"},
                                              {"dragon", "dragon_skull#main", "entity/enderdragon/dragon"},
                                              {"piglin", "piglin_head#main", "entity/piglin/piglin"},
                                              {"player", "player_head#main", "entity/player/wide/steve"}};
            const std::string kind = p.special == "player_head" ? "player" : p.specialKind;
            for (auto& h : kHeads)
                if (kind == h[0]) return entityModel(out, h[1], h[2], l, 0xFFFFFFFF);
            return false;
        }
        if (p.special == "banner") {
            const bool a = entityModel(out, "standing_banner#main", "entity/banner/banner_base", l, 0xFFFFFFFF);
            const u32 c = 0xFF000000u | abgrOf(p.specialColor);
            const bool b = entityModel(out, "standing_banner#flag", "entity/banner/base", l, c);
            return a || b;
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
            if (!p.special.empty()) any = special(out, p);
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
            for (int c = 0; c < 8; ++c) {
                const bool left = c == 1 || c == 3;
                int src = c;
                if (left && !m->display[c].set) src = c - 1;  // left hand defaults to the right hand transform
                const JModel::Tr& tr = m->display[src];
                part.display[c] = displayXf(tr.rot, tr.trans, tr.scale, left, !tr.set);
            }
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

int ItemModels::texture(const std::string& path) {
    Impl::Tex* t = impl_->tex(path);
    return t ? t->rect : -1;
}

const ItemMesh& ItemModels::mesh(int item, i32 dye, bool flat) {
    if (item < 0 || size_t(item) >= impl_->itemNames.size()) return impl_->emptyMesh;
    const u64 key = (u64(u32(item)) << 32) | (u64(u32(dye + 1) & 0x7FFFFFFF) << 1) | u64(flat);
    auto it = impl_->meshes.find(key);
    if (it != impl_->meshes.end()) return it->second;
    const std::string name = strip(impl_->itemNames[size_t(item)]);
    std::vector<Impl::Part> parts;
    ItemMesh mesh;
    try {
        const json& def = impl_->jsonAt("assets/minecraft/items/" + name + ".json");
        if (def.is_object()) impl_->evaluate(def.value("model", json()), dye, flat, Xf{}, parts, 0);
        mesh = impl_->build(parts);
    } catch (const std::exception& ex) {
        LOG("visuals: item model of %s: %s", name.c_str(), ex.what());
    }
    return impl_->meshes.emplace(key, mesh).first->second;
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
    if (front && back && width > 0 && height > 0 && store_.size() + uploaded_ + 6 <= kMaxQuads) {
        // PaintingRenderer.renderPainting: front at z = -1/32 facing -z, back at +1/32, edges from the back sprite.
        const Rect& f = rects_[size_t(front->rect)];
        const Rect& b = rects_[size_t(back->rect)];
        const float W = float(width), H = float(height), x0 = -W / 2, x1 = W / 2, y0 = -H / 2, y1 = H / 2, e = 1.0f / 32;
        Impl::Out out;
        const Xf id;
        auto Q = [&](const float p[4][3], const float uv[4][2], float nx, float ny, float nz) {
            const float n[3] = {nx, ny, nz};
            impl_->quad(out, id, p, uv, n, kTexItems, 0xFFFFFFFF, 0, false);
        };
        {  // front: texture u runs from x1 (u0) to x0 (u1)... seen from -z
            const float p[4][3] = {{x1, y0, -e}, {x0, y0, -e}, {x0, y1, -e}, {x1, y1, -e}};
            const float uv[4][2] = {{f.u0, f.v0 + f.dv}, {f.u0 + f.du, f.v0 + f.dv}, {f.u0 + f.du, f.v0}, {f.u0, f.v0}};
            Q(p, uv, 0, 0, -1);
        }
        {
            const float p[4][3] = {{x1, y1, e}, {x0, y1, e}, {x0, y0, e}, {x1, y0, e}};
            const float uv[4][2] = {{b.u0 + b.du, b.v0}, {b.u0, b.v0}, {b.u0, b.v0 + b.dv}, {b.u0 + b.du, b.v0 + b.dv}};
            Q(p, uv, 0, 0, 1);
        }
        const float tv1 = b.v0 + b.dv / 16.0f, lu1 = b.u0 + b.du / 16.0f;
        {  // top / bottom
            const float p[4][3] = {{x1, y1, -e}, {x0, y1, -e}, {x0, y1, e}, {x1, y1, e}};
            const float uv[4][2] = {{b.u0, b.v0}, {b.u0 + b.du, b.v0}, {b.u0 + b.du, tv1}, {b.u0, tv1}};
            Q(p, uv, 0, 1, 0);
            const float q[4][3] = {{x1, y0, e}, {x0, y0, e}, {x0, y0, -e}, {x1, y0, -e}};
            Q(q, uv, 0, -1, 0);
        }
        {  // sides
            const float p[4][3] = {{x1, y1, e}, {x1, y0, e}, {x1, y0, -e}, {x1, y1, -e}};
            const float uv[4][2] = {{lu1, b.v0}, {lu1, b.v0 + b.dv}, {b.u0, b.v0 + b.dv}, {b.u0, b.v0}};
            Q(p, uv, -1, 0, 0);
            const float q[4][3] = {{x0, y1, -e}, {x0, y0, -e}, {x0, y0, e}, {x0, y1, e}};
            Q(q, uv, 1, 0, 0);
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
