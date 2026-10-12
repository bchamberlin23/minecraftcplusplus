#pragma once
// Draws every visible entity in three calls: posed model cubes (vertex pulling from one static quad buffer,
// per-part matrices + per-layer instances + quad references uploaded each frame), translucent model layers,
// and CPU-built free quads (item entities, falling blocks, thrown-item sprites).
#include "entity/entities.h"
#include "entity/entity_anim.h"
#include "entity/item_models.h"
#include "render/pack_sink.h"
#include "render/renderer.h"

namespace ent {

class EntityRenderer : public RenderPass {
public:
    explicit EntityRenderer(EntityWorld& world);
    ~EntityRenderer() override;
    bool create(Renderer& renderer) override;
    void draw(const PassContext& ctx) override;
    // Shader pipeline: entities are posed once in prepare (camera-visible ones and shadow casters inside the updated
    // cascades), then drawn into the shadow maps (drawShadow) and the World stage (draw, with motion vectors from
    // the previous frame's part matrices kept per entity).
    void prepare(const PassContext& ctx) override;
    void drawShadow(const ShadowContext& sc) override;

    struct Stats {
        double cpuMs = 0;  // last frame: culling, animation, matrices and uploads
        u32 entities = 0, drawn = 0, instances = 0, quads = 0, freeQuads = 0;
    };
    const Stats& stats() const { return stats_; }
    // Shader-visible descriptor of the entity texture atlas (after create(); UINT32_MAX before).
    u32 atlasDescriptor() const { return atlas_ ? atlasDescriptor_ : UINT32_MAX; }
    ID3D12Resource* atlasTexture() const { return atlas_.Get(); }  // shader packs

    // Equipment (armor, elytra) from the item models' equipment data; dropped items are left to the visuals pass.
    void setItemModels(ItemModels* items) { items_ = items; }
    void setItemEntitiesExternal(bool external) { externalItems_ = external; }  // the visuals pass draws item entities
    // The local player, drawn like any other player while the camera is detached (F5); nullptr: not drawn.
    void setLocalPlayer(const Entity* e) { localPlayer_ = e; }
    // First-person camera: the local player is not drawn but still casts its shadow (shader pipeline).
    void setLocalPlayerShadow(const Entity* e) { localShadow_ = e; }
    static constexpr i32 kLocalPlayerId = INT32_MIN;
    // Model attachment points of the entities drawn in the last frame (camera-relative part matrices, blocks):
    // ItemInHandLayer / CustomHeadLayer of the visuals pass.
    struct Attachment {
        i32 entity;
        float hand[2][12];  // right, left arm (ArmedModel.translateToHand)
        float head[12];     // head part (HeadedModel.translateToHead, with CustomHeadLayer.Transforms' scale)
        float item[12];     // the main hand item of HoldingEntityRenderState layers (fox mouth, crossed arms...; ground)
        float headY;        // CustomHeadLayer.Transforms: yOffset (items) and skullYOffset (skulls)
        float skullY;
        u32 light;
        u8 valid;           // 1 right arm, 2 left arm, 4 head, 8 item
        bool babyOffset;    // ItemInHandLayer baby offsets
        bool shadowOnly;    // posed only as a shadow caster (outside the view, or the first-person local player)
    };
    const std::vector<Attachment>& attachments() const { return attachments_; }

    enum class Kind : u8 { None, Mob, Item, Block, Sprite, Box };
    struct LayerDef {
        int model = -1, babyModel = -1, tex = -1, babyTex = -1;
        u16 flags = 0;
    };
    struct TypeDef {
        Kind kind = Kind::None;
        int armor[4] = {-1, -1, -1, -1}, armorBaby[4] = {-1, -1, -1, -1};  // helmet, chestplate, leggings, boots
        int armorSlim[4] = {-1, -1, -1, -1};                                 // players: player_slim armor
        Anim anim = Anim::Head;
        u8 special = 0;  // variant / layer logic selector
        LayerDef base;
        LayerDef extra[3];
        int extraCount = 0;
        float scale = 1.0f;      // the renderer's own scale()
        float flip = 90.0f;      // death roll
        int sprite = -1;         // atlas texture of thrown items / sprites
        float spriteScale = 0.5f;
        std::string block;       // block drawn by Kind::Block when there is no block state
        mutable u16 blockId = 0;
    };
    struct Layer {
        int model, tex;
        u32 color;
        u16 flags;
        float scroll[2] = {};  // Scroll layers: texture offset (fractions of the texture)
    };
    // A mob type drawn without an entity (spawner display entities): its layers with the default variants, its
    // animation and the renderer's scale. Returns the layer count (0: not drawn as a model).
    int displayLayers(int type, Layer* out, int max, Anim& anim, float& scale) const;

private:
    struct Mat34 {
        float m[12];  // row-major 3x4
    };
    void buildDefs();
    int resolve(const Entity& e, const TypeDef& d, Layer* out);
    int resolveUncached(const Entity& e, const TypeDef& d, Layer* out) const;
    bool modelEntity(const Entity& e, const TypeDef& d, float pt, const double cam[3], u32 light);
    void speciesRotations(const Entity& e, const TypeDef& d, float pt, float pitch, Mat34& root) const;
    void mobPoses(const Entity& e, const TypeDef& d, const double cam[3], AnimState& s) const;
    int speciesLayers(const Entity& e, const TypeDef& d, float pt, Layer* layers, int n, int max) const;
    void blockDecorations(const Entity& e, const TypeDef& d, const Model& m, float pt, const Mat34& root, u32 light,
                          u32 overlay);
    int tex(const char* path) const;    // entity texture by path ("creeper/creeper_armor"), cached by literal
    int model(const char* layer) const;  // model layer ("creeper#armor"), cached by literal
    void freeEntity(const Entity& e, const TypeDef& d, float pt, const double cam[3], u32 light, const float* camRight);
    void emitBlock(u16 block, const Mat34& m, u32 light, u32 overlay);
    void emitFree(const float pos[4][3], const float uv[4][2], const float n[3], u32 tex, u32 color, u32 light,
                  u32 overlay);
    void build(const PassContext& ctx);   // CPU: cull, pose, upload (draw or prepare)
    void record(const PassContext& ctx);  // the World-stage draws of the last build

    EntityWorld& world_;
    const EntityAssets& assets_;
    Renderer* renderer_ = nullptr;
    std::vector<TypeDef> defs_;  // by entity type id

    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> psoCutout_, psoTranslucent_, psoFree_, psoGlint_, psoAdditive_, psoAdditiveMv_;
    // Shader pipeline: with a motion target (PassContext::motionRtv) and shadow casters (depth only).
    ComPtr<ID3D12PipelineState> psoCutoutMv_, psoTranslucentMv_, psoFreeMv_, psoGlintMv_, psoShadow_, psoShadowFree_;
    ComPtr<ID3D12Resource> quads_, atlas_, upload_;
    u8* mapped_ = nullptr;
    u32 atlasDescriptor_ = 0;

    // Per-frame write cursors into the mapped upload slice.
    u8* frameBase_ = nullptr;
    u32 matCount_ = 0, instCount_ = 0, refCount_ = 0, freeCount_ = 0;
    std::vector<Mat34> mats_;           // scratch: part matrices of the layer being built
    std::vector<PartPose> poses_;
    std::vector<u8> hidden_;
    std::vector<u32> translucent_;      // instance ids drawn after the cutout pass
    std::vector<u32> glint_;            // (quad, instance) of enchanted armour, drawn last with the glint
    std::vector<u32> additive_;         // (quad, instance) of additive layers (energy swirl), after the translucent ones
    std::vector<i32> itemBlocks_;      // item id -> block id (0: not a block), -1 unresolved
    struct CachedLayers {
        Layer layers[6];
        u8 count = 0;
    };
    std::unordered_map<u64, CachedLayers> layerCache_;  // resolved layers by type + variant state
    int equipmentLayers(const Entity& e, const TypeDef& d, Layer* out, int max);
    ItemModels* items_ = nullptr;
    bool externalItems_ = false;
    const Entity* localPlayer_ = nullptr;
    const Entity* localShadow_ = nullptr;

    // Last build: reference ranges. Refs: [cutout: camera | shadow only][translucent: camera | shadow only][glint].
    bool prepared_ = false, built_ = false, shadowOnly_ = false;
    u32 cutoutRefs_ = 0, glintRefs_ = 0, additiveRefs_ = 0, camCutout_ = 0, camTranslucent_ = 0, camFree_ = 0;
    struct Caster {  // one entity's geometry for the shadow maps (camera-relative bounding sphere)
        float c[3], r;
        u32 ref, refN, tr, trN, free, freeN;
    };
    std::vector<Caster> casters_;
    // Motion vectors: last frame's part matrices (camera-relative to that frame's camera) and position per entity.
    struct PrevPose {
        u64 frame = 0;
        u32 hash = 0;
        double cam[3] = {}, pos[3] = {};
        std::vector<Mat34> mats;
    };
    std::unordered_map<i32, PrevPose> prev_;
    std::vector<Mat34> entMats_;  // scratch: all part matrices of the entity being posed
    u32 entHash_ = 0;
    u64 buildFrame_ = 0;
    float freeMotion_[3] = {};    // emitFree: current - previous position (blocks) of the entity being built
    float camYaw_ = 0, camPitch_ = 0;  // camera yRot / xRot (degrees) of the frame being built: display billboards
    std::vector<Attachment> attachments_;
    std::vector<int> ridden_;  // entities with a passenger this frame (EquineSaddleModel riding lines)
    int elytraModel_ = -1, elytraBabyModel_ = -1;

    // Shader packs (PassContext::pack): vanilla-style vertices per entity for the pack's gbuffers_entities programs.
    PackSink* pack_ = nullptr;
    std::vector<PackVertex> packVerts_[3];  // model cutout, model translucent, free quads (entity atlas)
    std::vector<PackVertex> packBlock_;     // free quads with block textures (block atlas)
    u32 packOverlay_ = 0;
    void packQuad(int bucket, const float pos[4][3], const float uv[4][2], const float n[3], u32 color, u32 light,
                  u32 overlay);
    void packFlush(const Entity& e);

    Stats stats_;
    double cpuTotal_ = 0;
    u64 frames_ = 0, entityFrames_ = 0;
    static constexpr u32 kHistory = 256;  // recent frames for the exit report (steady state)
    float historyMs_[kHistory] = {}, historyWorldMs_[kHistory] = {};
    u32 historyDrawn_[kHistory] = {}, historyQuads_[kHistory] = {};
};

}  // namespace ent
