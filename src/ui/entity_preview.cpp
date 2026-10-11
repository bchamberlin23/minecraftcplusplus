#include "ui/entity_preview.h"

#include "entity/entities.h"
#include "entity/item_models.h"
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

u32 argbOf(i32 rgb) { return 0xFF000000u | u32(rgb); }

// Equipment layers of one asset as preview layers (dyeable layers: the item's dye, else the undyed colour).
void addEquipment(UiState& s, PreviewEntity& out, int model, const std::string& asset, const char* layerType, i64 dye) {
    if (!s.itemModels || model < 0 || asset.empty()) return;
    for (const ent::EquipmentLayer& l : s.itemModels->equipmentLayers(asset, layerType)) {
        if (l.path.empty()) continue;
        u32 color = 0xFFFFFFFF;
        if (l.dyeable) {
            const i64 c = dye >= 0 ? dye : l.undyed;
            if (c < 0) continue;
            color = argbOf(i32(c & 0xFFFFFF));
        }
        out.layers.push_back({model, l.path, color});
    }
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
    // HumanoidArmorLayer (chest, legs, feet, head like the renderer), WingsLayer, ItemInHandLayer.
    const auto& slots = s.inventory.slots;
    auto stack = [&](int i) -> const ItemStack* {
        return i >= 0 && size_t(i) < slots.size() && !slots[size_t(i)].empty() ? &slots[size_t(i)] : nullptr;
    };
    if (s.itemModels) {
        static const char* kPieces[4] = {"#helmet", "#chestplate", "#leggings", "#boots"};
        static const int kOrder[4][2] = {{6, 4}, {7, 3}, {8, 2}, {5, 5}};  // menu slot, EquipmentSlot
        const std::string set = slim ? "player_slim" : "player";
        for (auto [menuSlot, slot] : kOrder) {
            const ItemStack* st = stack(menuSlot);
            if (!st) continue;
            const ent::ItemModels::Equippable& eq = s.itemModels->equippable(st->item);
            if (eq.slot != slot || eq.asset.empty()) continue;
            addEquipment(s, out, a.model(set + kPieces[5 - slot]), eq.asset, slot == 3 ? "humanoid_leggings" : "humanoid",
                         st->dyedColor);
        }
        if (const ItemStack* chest = stack(6)) {
            const ent::ItemModels::Equippable& eq = s.itemModels->equippable(chest->item);
            const size_t before = out.layers.size();
            if (eq.slot == 4) addEquipment(s, out, a.model("elytra#main"), eq.asset, "wings", -1);
            for (size_t i = before; i < out.layers.size(); ++i) out.layers[i].wings = true;
        }
    }
    if (const ItemStack* main = stack(36 + s.game->selectedSlot())) out.held[0] = *main;
    if (const ItemStack* off = stack(45)) out.held[1] = *off;
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
    // SimpleEquipmentLayer / LlamaDecorLayer: body armour or carpet, then the saddle.
    if (s.itemModels)
        for (int slot : {6, 7}) {
            const char *modelName, *layerType;
            if (!ent::ItemModels::mountLayer(n, slot, e.baby, modelName, layerType)) continue;
            const int item = e.equipment[slot];
            std::string asset;
            if (item >= 0 && s.itemModels->equippable(item).slot == slot) asset = s.itemModels->equippable(item).asset;
            if (slot == 6 && e.baby && n.ends_with("llama")) asset.clear();
            if (asset.empty() && slot == 6 && n == "trader_llama") asset = e.baby ? "trader_llama_baby" : "trader_llama";
            addEquipment(s, out, a.model(modelName), asset, layerType, e.equipDye[slot]);
        }
    out.anim = anim;
    out.height = e.info->height;
    out.state.baby = e.baby;
    if (!e.hasChest) out.hiddenParts = {"left_chest", "right_chest"};
    if (s.game->vehicleId != entityId) out.hiddenParts.insert(out.hiddenParts.end(), {"left_saddle_line", "right_saddle_line"});
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
        TexMode mode = ModeGui;
    };
    std::vector<Face> faces;
    std::vector<ent::PartPose> poses;
    std::vector<M34> mats;
    M34 hand[2];  // ItemInHandLayer anchors: the base model's right / left arm
    bool haveHand[2] = {};
    for (size_t li = 0; li < pe.layers.size(); ++li) {
        const PreviewLayer& layer = pe.layers[li];
        if (layer.model < 0) continue;
        const Sprite* tex = s.sprites.texture(layer.texture);
        if (!tex) continue;
        const ent::Model& model = a.modelAt(layer.model);
        poses.resize(model.parts.size());
        for (size_t i = 0; i < model.parts.size(); ++i) poses[i] = model.parts[i].pose;
        u64 hidden = 0;
        M34 layerRoot = root;
        if (layer.wings) {  // WingsLayer: translate(0, 0, 0.125); ElytraModel at rest (ElytraAnimationState targets)
            layerRoot = mul(root, translate(0, 0, 0.125f));
            const bool c = st.crouching;
            const float rx = c ? kPi * 2.0f / 9.0f : kPi / 12, ry = c ? 0.08726646f : 0.0f, rz = c ? -kPi / 4 : -kPi / 12;
            const i16 lw = model.role(ent::LeftWing), rw = model.role(ent::RightWing);
            if (lw >= 0) poses[size_t(lw)].y = c ? 3.0f : 0.0f, poses[size_t(lw)].xRot = rx, poses[size_t(lw)].yRot = ry, poses[size_t(lw)].zRot = rz;
            if (rw >= 0) poses[size_t(rw)].y = c ? 3.0f : 0.0f, poses[size_t(rw)].xRot = rx, poses[size_t(rw)].yRot = -ry, poses[size_t(rw)].zRot = -rz;
        } else {
            hidden = ent::animate(pe.anim, st, model, poses.data());
        }
        mats.resize(model.parts.size());
        std::vector<bool> partHidden(model.parts.size(), false);
        for (size_t i = 0; i < model.parts.size(); ++i) {
            const ent::ModelPart& part = model.parts[i];
            const bool parentHidden = part.parent >= 0 && partHidden[size_t(part.parent)];
            partHidden[i] = parentHidden || (part.role != ent::RoleNone && (hidden >> part.role) & 1) ||
                            std::find(pe.hiddenParts.begin(), pe.hiddenParts.end(), part.name) != pe.hiddenParts.end();
            mats[i] = mul(part.parent >= 0 ? mats[size_t(part.parent)] : layerRoot, partLocal(poses[i]));
            if (li == 0 && (part.role == ent::RightArm || part.role == ent::LeftArm) && !partHidden[i]) {
                const int k = part.role == ent::RightArm ? 0 : 1;
                hand[k] = mats[i], haveHand[k] = true;
            }
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
    // ItemInHandLayer: the held items as flat cards with their inventory icon (flat items: the third-person display
    // transform of the item model; block items: a small card facing the viewer at the transformed centre).
    for (int k = 0; k < 2 && s.itemModels; ++k) {
        const ItemStack& held = pe.held[k];
        float iuv[4];
        if (!haveHand[k] || held.empty() || !s.icons.icon(held, iuv)) continue;
        const ent::ItemMesh& mesh = s.itemModels->mesh(held.item, i32(held.dyedColor), false);
        if (mesh.empty()) continue;
        M34 m = mul(hand[k], rotX(-kPi / 2));
        m = mul(m, rotY(kPi));
        m = mul(m, translate((k == 1 ? -1.0f : 1.0f) / 16.0f, 2.0f / 16.0f, -10.0f / 16.0f));
        M34 disp;
        std::copy(mesh.parts[0].display[size_t(k == 0 ? ent::Display::ThirdRight : ent::Display::ThirdLeft)].m,
                  mesh.parts[0].display[size_t(k == 0 ? ent::Display::ThirdRight : ent::Display::ThirdLeft)].m + 12, disp.m);
        m = mul(m, disp);
        Face f;
        f.mode = ModeIcon;
        f.color = 0xFFFFFFFF;
        const float iu[4] = {iuv[0], iuv[2], iuv[2], iuv[0]}, iv[4] = {iuv[3], iuv[3], iuv[1], iuv[1]};
        if (s.items.isBlockItem(held.item)) {
            const float centre[3] = {0.5f, 0.5f, 0.5f};
            float o[3];
            xform(m, centre, o);
            const float h = 0.16f;
            const float dx[4] = {-h, h, h, -h}, dy[4] = {h, h, -h, -h};
            for (int q = 0; q < 4; ++q) f.xy[q][0] = cx + (o[0] + dx[q]) * size, f.xy[q][1] = cy + (o[1] + dy[q]) * size;
            f.depth = o[2] - 0.05f;
        } else {
            const float corner[4][3] = {{0, 0, 0.5f}, {1, 0, 0.5f}, {1, 1, 0.5f}, {0, 1, 0.5f}};
            f.depth = 0;
            for (int q = 0; q < 4; ++q) {
                float o[3];
                xform(m, corner[q], o);
                f.xy[q][0] = cx + o[0] * size, f.xy[q][1] = cy + o[1] * size;
                f.depth += o[2] / 4.0f;
            }
        }
        for (int q = 0; q < 4; ++q) f.uv[q][0] = iu[q], f.uv[q][1] = iv[q];
        faces.push_back(f);
    }
    // Far first; layers of the same part (hat, jacket) sit slightly nearer and draw after the base at equal depth.
    std::stable_sort(faces.begin(), faces.end(), [](const Face& l, const Face& r) { return l.depth > r.depth; });
    c.pushScissor(float(x0), float(y0), float(x1), float(y1));
    for (const Face& f : faces) c.quad4(f.xy, f.uv, f.color, f.mode);
    c.popScissor();
}

}  // namespace ui
