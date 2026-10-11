#pragma once
// Draws every visible entity in three calls: posed model cubes (vertex pulling from one static quad buffer,
// per-part matrices + per-layer instances + quad references uploaded each frame), translucent model layers,
// and CPU-built free quads (item entities, falling blocks, thrown-item sprites).
#include "entity/entities.h"
#include "entity/entity_anim.h"
#include "render/renderer.h"

namespace ent {

class EntityRenderer : public RenderPass {
public:
    explicit EntityRenderer(EntityWorld& world);
    ~EntityRenderer() override;
    bool create(Renderer& renderer) override;
    void draw(const PassContext& ctx) override;

    struct Stats {
        double cpuMs = 0;  // last frame: culling, animation, matrices and uploads
        u32 entities = 0, drawn = 0, instances = 0, quads = 0, freeQuads = 0;
    };
    const Stats& stats() const { return stats_; }
    // Shader-visible descriptor of the entity texture atlas (after create(); UINT32_MAX before).
    u32 atlasDescriptor() const { return atlas_ ? atlasDescriptor_ : UINT32_MAX; }

    enum class Kind : u8 { None, Mob, Item, Block, Sprite, Box };
    struct LayerDef {
        int model = -1, babyModel = -1, tex = -1, babyTex = -1;
        u8 flags = 0;
    };
    struct TypeDef {
        Kind kind = Kind::None;
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
        u8 flags;
    };

private:
    struct Mat34 {
        float m[12];  // row-major 3x4
    };
    void buildDefs();
    int resolve(const Entity& e, const TypeDef& d, Layer* out);
    int resolveUncached(const Entity& e, const TypeDef& d, Layer* out) const;
    bool modelEntity(const Entity& e, const TypeDef& d, float pt, const double cam[3], u32 light);
    void freeEntity(const Entity& e, const TypeDef& d, float pt, const double cam[3], u32 light, const float* camRight);
    void emitBlock(u16 block, const Mat34& m, u32 light, u32 overlay);
    void emitFree(const float pos[4][3], const float uv[4][2], const float n[3], u32 tex, u32 color, u32 light,
                  u32 overlay);

    EntityWorld& world_;
    const EntityAssets& assets_;
    Renderer* renderer_ = nullptr;
    std::vector<TypeDef> defs_;  // by entity type id

    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> psoCutout_, psoTranslucent_, psoFree_;
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
    std::vector<i32> itemBlocks_;      // item id -> block id (0: not a block), -1 unresolved
    struct CachedLayers {
        Layer layers[6];
        u8 count = 0;
    };
    std::unordered_map<u64, CachedLayers> layerCache_;  // resolved layers by type + variant state

    Stats stats_;
    double cpuTotal_ = 0;
    u64 frames_ = 0, entityFrames_ = 0;
    static constexpr u32 kHistory = 256;  // recent frames for the exit report (steady state)
    float historyMs_[kHistory] = {}, historyWorldMs_[kHistory] = {};
    u32 historyDrawn_[kHistory] = {}, historyQuads_[kHistory] = {};
};

}  // namespace ent
