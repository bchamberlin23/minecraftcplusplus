#include "ui/entity_preview.h"

#include "entity/entities.h"
#include "ui/frontend.h"
#include "ui/ui_state.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {
constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;

struct M34 {
    float m[12];
};
M34 mul(const M34& a, const M34& b) {
    M34 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            float v = a.m[i * 4] * b.m[j] + a.m[i * 4 + 1] * b.m[4 + j] + a.m[i * 4 + 2] * b.m[8 + j];
            if (j == 3) v += a.m[i * 4 + 3];
            r.m[i * 4 + j] = v;
        }
    return r;
}
M34 ident() { return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}; }
M34 translate(float x, float y, float z) { return {{1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z}}; }
M34 scale(float x, float y, float z) { return {{x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0}}; }
M34 rotX(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {{1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0}};
}
M34 rotY(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {{c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0}};
}
M34 rotZ(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {{c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0}};
}
M34 partLocal(const ent::PartPose& p) {  // ModelPart.translateAndRotate
    M34 r = translate(p.x / 16.0f, p.y / 16.0f, p.z / 16.0f);
    if (p.zRot != 0) r = mul(r, rotZ(p.zRot));
    if (p.yRot != 0) r = mul(r, rotY(p.yRot));
    if (p.xRot != 0) r = mul(r, rotX(p.xRot));
    if (p.xScale != 1 || p.yScale != 1 || p.zScale != 1) r = mul(r, scale(p.xScale, p.yScale, p.zScale));
    return r;
}
void xform(const M34& m, const float in[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = m.m[i * 4] * in[0] + m.m[i * 4 + 1] * in[1] + m.m[i * 4 + 2] * in[2] + m.m[i * 4 + 3];
}
void xformDir(const M34& m, const float in[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = m.m[i * 4] * in[0] + m.m[i * 4 + 1] * in[1] + m.m[i * 4 + 2] * in[2];
    const float l = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (l > 0) out[0] /= l, out[1] /= l, out[2] /= l;
}

const char* kSkins[9] = {"alex", "ari", "efe", "kai", "makena", "noor", "steve", "sunny", "zuri"};

// The default skin texture path of an EntityAssets texture index (players carry indices).
std::string skinPath(const ent::EntityAssets& a, int tex, bool& slim) {
    for (int s = 0; s < 2; ++s)
        for (const char* n : kSkins) {
            const std::string p = std::string("entity/player/") + (s ? "slim/" : "wide/") + n;
            if (a.texture(p) == tex) {
                slim = s != 0;
                return p;
            }
        }
    slim = false;
    return "entity/player/wide/steve";
}
}  // namespace

bool previewLocalPlayer(UiState& s, PreviewEntity& out) {
    if (!s.entities) return false;
    const ent::EntityAssets& a = s.entities->assets();
    bool slim = false;
    std::string tex = "entity/player/wide/steve";
    if (const ent::PlayerInfo* info = s.entities->playerNamed(s.fe->cfg.playerName); info && info->skin >= 0)
        tex = skinPath(a, info->skin, slim);
    const int model = a.model(slim ? "player_slim#main" : "player#main");
    if (model < 0) return false;
    out = PreviewEntity{};
    out.layers.push_back({model, tex});
    out.anim = ent::Anim::Humanoid;
    out.height = 1.8f;
    out.modelScale = 0.9375f;
    out.state.skinParts = 0x7F;
    out.state.crouching = s.game->player().sneaking;
    return true;
}

bool previewEntity(UiState& s, int entityId, PreviewEntity& out) {
    if (!s.entities) return false;
    const auto& all = s.entities->entities();
    auto it = all.find(entityId);
    if (it == all.end() || !it->second.info) return false;
    const ent::Entity& e = it->second;
    const ent::EntityAssets& a = s.entities->assets();
    std::string n = e.info->name.substr(e.info->name.find(':') + 1);
    std::string model = n, tex;
    ent::Anim anim = ent::Anim::Head;
    if (n == "horse") {
        static const char* kColors[7] = {"white", "creamy", "chestnut", "brown", "black", "gray", "darkbrown"};
        tex = std::string("entity/horse/horse_") + kColors[std::clamp(e.variant, 0, 6)];
        anim = ent::Anim::Quadruped;
    } else if (n == "donkey" || n == "mule") {
        tex = "entity/horse/" + n, anim = ent::Anim::Quadruped;
    } else if (n == "skeleton_horse") {
        tex = "entity/horse/horse_skeleton", anim = ent::Anim::Quadruped;
    } else if (n == "zombie_horse") {
        tex = "entity/horse/horse_zombie", anim = ent::Anim::Quadruped;
    } else if (n == "llama" || n == "trader_llama") {
        static const char* kColors[4] = {"creamy", "white", "brown", "gray"};
        tex = std::string("entity/llama/llama_") + kColors[std::clamp(e.variant, 0, 3)];
    } else if (n == "camel") {
        tex = "entity/camel/camel";
    } else if (n == "camel_husk") {
        model = "camel", tex = "entity/camel/camel_husk";
    } else {
        return false;
    }
    int mi = -1;
    if (e.baby) {
        mi = a.model(model + "_baby#main");
        if (mi >= 0) tex += "_baby";
    }
    if (mi < 0) mi = a.model(model + "#main");
    if (mi < 0) return false;
    out = PreviewEntity{};
    out.layers.push_back({mi, tex});
    if (n == "horse" && e.variant2 > 0 && e.variant2 <= 4) {
        static const char* kMarks[5] = {"", "white", "whitefield", "whitedots", "blackdots"};
        out.layers.push_back({mi, std::string("entity/horse/horse_markings_") + kMarks[e.variant2] + (e.baby ? "_baby" : "")});
    }
    out.anim = anim;
    out.height = e.info->height;
    out.state.baby = e.baby;
    return true;
}

void drawEntityFollowsMouse(UiState& s, Canvas& c, const PreviewEntity& pe, int x0, int y0, int x1, int y1, float size,
                            float offsetY, float mouseX, float mouseY) {
    if (!s.entities || pe.layers.empty()) return;
    const ent::EntityAssets& a = s.entities->assets();
    const float cx = float(x0 + x1) / 2.0f, cy = float(y0 + y1) / 2.0f;
    const float xAngle = std::atan((cx - mouseX) / 40.0f), yAngle = std::atan((cy - mouseY) / 40.0f);
    // LivingEntityRenderState from the mouse: bodyRot 180 + x * 20, head yaw x * 20 (relative), pitch -y * 20.
    ent::AnimState st = pe.state;
    st.headYaw = xAngle * 20.0f;
    st.pitch = -yAngle * 20.0f;
    const float bodyRot = 180.0f + xAngle * 20.0f;
    // GuiEntityRenderer: translate(0, height / 2 + offsetY), rotate Z(pi) * X(yAngle * 20 deg); the picture is
    // centred on the box and scaled by size (z flipped: larger z is nearer).
    const M34 view = mul(mul(translate(0, pe.height / 2.0f + offsetY, 0), rotZ(kPi)), rotX(yAngle * 20.0f * kDeg));
    // LivingEntityRenderer: rotY(180 - bodyRot), scale(-1, -1, 1), the renderer's scale, translate(0, -1.501, 0).
    M34 root = mul(view, rotY((180.0f - bodyRot) * kDeg));
    root = mul(root, scale(-pe.modelScale, -pe.modelScale, pe.modelScale));
    root = mul(root, translate(0, -1.501f, 0));

    struct Face {
        float xy[4][2], uv[4][2], depth;
        u32 color;
    };
    std::vector<Face> faces;
    std::vector<ent::PartPose> poses;
    std::vector<M34> mats;
    for (const PreviewLayer& layer : pe.layers) {
        if (layer.model < 0) continue;
        const Sprite* tex = s.sprites.texture(layer.texture);
        if (!tex) continue;
        const ent::Model& model = a.modelAt(layer.model);
        poses.resize(model.parts.size());
        for (size_t i = 0; i < model.parts.size(); ++i) poses[i] = model.parts[i].pose;
        const u64 hidden = ent::animate(pe.anim, st, model, poses.data());
        mats.resize(model.parts.size());
        std::vector<bool> partHidden(model.parts.size(), false);
        for (size_t i = 0; i < model.parts.size(); ++i) {
            const ent::ModelPart& part = model.parts[i];
            const bool parentHidden = part.parent >= 0 && partHidden[size_t(part.parent)];
            partHidden[i] = parentHidden || (part.role != ent::RoleNone && (hidden >> part.role) & 1);
            mats[i] = mul(part.parent >= 0 ? mats[size_t(part.parent)] : root, partLocal(poses[i]));
            if (partHidden[i]) continue;
            for (u32 q = part.quadFirst; q < part.quadFirst + part.quadCount; ++q) {
                const ent::GpuQuad& g = a.quads()[q];
                float n[3];
                xformDir(mats[i], g.normal, n);
                if (n[2] >= -1e-4f) continue;  // facing away (nearer is -z before the picture's z flip)
                Face f;
                float depth = 0;
                for (int k = 0; k < 4; ++k) {
                    const float p[3] = {g.pos[k][0] / 16.0f, g.pos[k][1] / 16.0f, g.pos[k][2] / 16.0f};
                    float o[3];
                    xform(mats[i], p, o);
                    f.xy[k][0] = cx + o[0] * size, f.xy[k][1] = cy + o[1] * size;
                    depth += o[2];
                    f.uv[k][0] = tex->u0 + g.uv[k][0] * (tex->u1 - tex->u0);
                    f.uv[k][1] = tex->v0 + g.uv[k][1] * (tex->v1 - tex->v0);
                }
                f.depth = depth / 4.0f;
                // ENTITY_IN_UI lighting: two lights from the front above, 40 % ambient (view space, y down, -z near).
                static const float L0[3] = {0.16f, -0.79f, -0.59f}, L1[3] = {-0.16f, -0.79f, -0.59f};
                const float d0 = std::max(0.0f, n[0] * L0[0] + n[1] * L0[1] + n[2] * L0[2]);
                const float d1 = std::max(0.0f, n[0] * L1[0] + n[1] * L1[1] + n[2] * L1[2]);
                const float light = std::min(1.0f, 0.4f + 0.6f * (d0 + d1));
                f.color = scaleRgb(layer.color, light);
                faces.push_back(f);
            }
        }
    }
    // Far first; layers of the same part (hat, jacket) sit slightly nearer and draw after the base at equal depth.
    std::stable_sort(faces.begin(), faces.end(), [](const Face& l, const Face& r) { return l.depth > r.depth; });
    c.pushScissor(float(x0), float(y0), float(x1), float(y1));
    for (const Face& f : faces) c.quad4(f.xy, f.uv, f.color, ModeGui);
    c.popScissor();
}

}  // namespace ui
