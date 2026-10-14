#pragma once
// 3D item models the way the game bakes them for the world (held items, item frames, heads, dropped items):
// the item definition (assets/minecraft/items/<item>.json) selects block / item models; flat "generated" models
// are extruded by their texture's alpha like ItemModelGenerator (front, back and one side face per opaque edge
// pixel); models with elements become cuboid quads; "special" models (shield, trident, chests, heads, shulker
// boxes, banners) use the game's entity model layers. Each part keeps its display transforms
// (display.firstperson_righthand, thirdperson_*, head, ground, fixed) and local "transformation".
//
// Meshes are built lazily on first use (main thread); their quads go to an append-only store the renderer uploads.
// Textures (item/, block/, painting/, particle/, misc/) are packed into one RGBA atlas (ui::Atlas) on demand.
//
// Equipment: which slot / equipment asset an item equips to (the game's item component reports), and the layers
// of an equipment asset (assets/minecraft/equipment/<asset>.json).
#include "core/common.h"
#include "ui/canvas.h"
#include "ui/item_props.h"

#include <memory>
#include <unordered_map>

namespace ui {
class Resources;
}

namespace ent {

class EntityAssets;

// One textured quad of a static mesh (shaders/visuals.hlsl reads the same layout).
struct MeshQuad {
    float pos[4][3];  // mesh space (blocks)
    float uv[4][2];   // atlas-normalized
    u32 normal;       // snorm8 x, y, z
    u32 color;        // 0xAABBGGRR tint
    u32 tex;          // kTexItems / kTexFont / kTexSolid / kTexEntity
    u32 flags;        // kQuadNoShade
};
static_assert(sizeof(MeshQuad) == 96);
constexpr u32 kTexItems = 0xFFFFFFFF, kTexFont = 0xFFFFFFFE, kTexSolid = 0xFFFFFFFD, kTexEntity = 0xFFFFFFFC;
constexpr u32 kQuadNoShade = 1;
// Corners 1 and 2 take the grey level in flags bits 8..15 instead of the colour's RGB (a vertex-colour gradient).
constexpr u32 kQuadGreyGradient = 2;

// Row-major 3x4 affine transform.
struct Xf {
    float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
};
Xf xfMul(const Xf& a, const Xf& b);
Xf xfTranslate(float x, float y, float z);
Xf xfScale(float x, float y, float z);
Xf xfRotX(float radians);
Xf xfRotY(float radians);
Xf xfRotZ(float radians);
// An item model "transformation" (translation, left_rotation, scale, right_rotation, or a 4x4 matrix).
Xf itemTransformation(const nlohmann::json& t);
void xfPoint(const Xf& m, const float in[3], float out[3]);
u32 packNormal(float x, float y, float z);

// None: ItemDisplayContext.NONE (no model transform, only the centring).
enum class Display : u8 { FirstRight, FirstLeft, ThirdRight, ThirdLeft, Head, Gui, Ground, Fixed, OnShelf, None, Count };

struct ItemMesh {
    struct Part {
        u32 first = 0, count = 0;    // cutout quads in the store
        u32 tfirst = 0, tcount = 0;  // translucent quads
        Xf display[size_t(Display::Count)];  // ItemTransform.apply (incl. the -0.5 centring and left hand mirror)
    };
    Part parts[4];
    int partCount = 0;
    float groundMinY = 0, groundDepth = 0;  // model bounding box in the GROUND display (ItemEntityRenderer)
    float shelfMinY = 0, shelfMaxY = 0;     // and in the ON_SHELF display (ShelfRenderer)
    bool empty() const { return partCount == 0; }
};

struct EquipmentLayer {
    int tex = -1;          // entity atlas texture
    i32 undyed = -1;       // dyeable: colour when undyed (RGB), -1 not dyeable
    bool dyeable = false;
    bool playerTexture = false;
    std::string path;      // texture below textures/ ("entity/equipment/humanoid/iron")
};

class ItemModels {
public:
    ItemModels();
    ~ItemModels();
    bool init(const ui::Resources& res, const EntityAssets& assets, const std::vector<std::string>& itemNames,
              const fs::path& componentReports);

    // The game's item registry (data components of stacks); before the first mesh.
    void setItems(const ui::Items* items) { items_ = items; }
    const ui::Items* items() const { return items_; }
    const ui::Resources* resources() const;  // the game's resources (with the packs)
    // Mesh of an item stack in a display context (ItemModelResolver.updateForTopItem): the item definition named by
    // the stack (item_model component) with its condition / select / range_dispatch properties, tints and special
    // renderers evaluated for the stack's components and the context. Never null; empty() when nothing is drawn.
    const ItemMesh& mesh(const ui::ItemStack& s, const ui::ItemContext& c);
    // A plain stack of an item (no components) with an optional dyed colour.
    static ui::ItemStack plainStack(int item, i32 dye = -1);
    // GUI icons (ui/icons.cpp) draw special models from the same geometry: the quads of a "special" item model node
    // for a stack (model space, `local` applied; textures in the item (kTexItems) or entity (kTexEntity) atlas), and a
    // key of the stack data they depend on.
    bool specialQuads(const nlohmann::json& node, const ui::ItemStack& s, const Xf& local, std::vector<MeshQuad>& out);
    std::string specialKey(const nlohmann::json& node, const ui::ItemStack& s);
    // A player head's skull model with the skin of the stack's profile (SkullBlockRenderer: player heads placed as
    // blocks), model space, no display transform.
    const ItemMesh& headMesh(const ui::ItemStack& s);
    struct AtlasView {
        const u8* rgba = nullptr;  // w * h * 4
        int w = 0, h = 0;
    };
    AtlasView atlasView(u32 tex) const;  // kTexItems / kTexEntity
    // Texture of the item's particles (BreakingItemParticle): the model's particle texture, else its first layer.
    std::string itemParticleTexture(int item);
    const ItemMesh& blockModelMesh(const std::string& model);  // "block/item_frame" (display transforms unused)
    // An entity model layer at its initial pose (worn skulls: CustomHeadLayer draws the skull model directly).
    const ItemMesh& entityLayerMesh(const std::string& layer, const std::string& texture);
    // Worn head of a skull / head item ("skeleton_skull" ...): its model layer and texture; false if not a head.
    static bool skullLayer(const std::string& itemName, std::string& layer, std::string& texture);
    // Item behaviour from the default components / item definition: attack speed (attribute), swap animation.
    struct HandInfo {
        float attackSpeed = 4.0f;  // Attributes.ATTACK_SPEED with the item's modifiers
        float swapScale = 1.0f;    // item definition "swap_animation_scale"
        bool swapAnimation = true; // "hand_animation_on_swap"
    };
    const HandInfo& handInfo(int item);
    std::string itemName(int item) const;  // without namespace ("" if unknown)
    // Painting: front (variant texture), back and edges; width x height blocks centred at the origin, facing -z.
    // One part, kPaintingSegmentQuads quads per block segment in segment order (x + y * width): segments are lit
    // separately (PaintingRenderState.lightCoordsPerBlock).
    const ItemMesh& paintingMesh(const std::string& assetId, int width, int height);
    static constexpr u32 kPaintingSegmentQuads = 6;

    // Textures in the item atlas (path below textures/, e.g. "particle/flame"): atlas rect index or -1.
    int texture(const std::string& path);
    // Animated textures (.mcmeta) in the atlas show their frame for this client tick (interpolated ones blended).
    void tick(u64 tick);
    struct Rect {
        float u0, v0, du, dv;
        int w, h;
        bool translucent;
    };
    const Rect& rect(int i) const { return rects_[size_t(i)]; }
    ui::Atlas& atlas() { return atlas_; }

    // Arbitrary static quads (name tag text); index of the first or UINT32_MAX when the store is full.
    u32 addQuads(const MeshQuad* quads, u32 count);
    // Quads appended since the last call (the renderer copies them to the GPU at `firstOut`).
    bool takePending(const MeshQuad*& data, u32& count, u32& firstOut);
    void releasePending();
    u32 quadCount() const { return u32(store_.size()); }
    // A quad by its store index (uploaded or pending; GUI entity previews draw them on the CPU).
    const MeshQuad& quad(u32 i) const { return i < uploaded_ ? uploadedQuads_[i] : store_[i - uploaded_]; }
    static constexpr u32 kMaxQuads = 1 << 18;

    // Equipment: slot (EquipmentSlot ordinal: 0 main hand .. 5 head, 6 body) and asset; false if not equippable.
    struct Equippable {
        int slot = -1;
        std::string asset;  // "diamond"
    };
    const Equippable& equippable(int item);
    // Layers of an equipment asset for a layer type ("humanoid", "humanoid_leggings", "humanoid_baby", "wings").
    const std::vector<EquipmentLayer>& equipmentLayers(const std::string& asset, const char* layerType);
    // Same for an item's own equipment asset, cached by item (per frame use): type 0 humanoid, 1 humanoid_leggings,
    // 2 humanoid_baby, 3 wings.
    const std::vector<EquipmentLayer>& itemLayers(int item, int type);
    // Mount / animal equipment (SimpleEquipmentLayer, LlamaDecorLayer): for an entity type ("horse") and
    // EquipmentSlot 6 (body) or 7 (saddle), the model layer ("horse_armor#main") and equipment layer type
    // ("horse_body"); false when the entity draws nothing for that slot.
    static bool mountLayer(std::string_view entity, int slot, bool baby, const char*& model, const char*& layerType);

    // Particle texture of a block model (blockstates -> model -> "particle"), e.g. "block/dirt"; "" if unknown.
    std::string blockParticleTexture(const std::string& blockName);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    const ui::Items* items_ = nullptr;
    ui::Atlas atlas_;
    std::vector<Rect> rects_;
    std::vector<MeshQuad> store_;
    u32 uploaded_ = 0;
    std::vector<MeshQuad> uploadedQuads_;  // CPU copy of the uploaded ones
    friend struct Impl;
};

}  // namespace ent
