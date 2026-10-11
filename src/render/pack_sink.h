#pragma once
// How extension passes (entities, block entities, sky, particles, the hand...) draw while an OptiFine / Iris shader
// pack is active (render/packs): instead of their own pipelines they hand vanilla-style geometry to the pack's
// gbuffers programs through PassContext::pack. Passes that ignore it still draw normally into an LDR layer that the
// pack pipeline merges into colortex0 (vanilla-lit, but visible).
#include "core/common.h"

#include <string_view>

struct ID3D12Resource;

// Vanilla's vertex (position, colour, texture, overlay, lightmap, normal) plus the Iris extras packs read.
struct PackVertex {
    float pos[3];      // camera-relative world position
    u32 color;         // 0xAABBGGRR
    float uv[2];       // texture coordinates in the draw's texture (block atlas: PackSink::atlasUv)
    u32 light;         // block | sky << 16, vanilla UV2 units (light level * 16, 0..240)
    u32 overlay;       // vanilla UV1: u | v << 16 (hurt / white flash; 0 | 10 << 16 = none)
    u32 normal;        // snorm8 x | y << 8 | z << 16
    i32 entity;        // mc_Entity.x (block id for block-like geometry, -1 none)
    float midUv[2];    // mc_midTexCoord (centre of the sprite)
};
static_assert(sizeof(PackVertex) == 48);

inline u32 packNormal(float x, float y, float z) {
    auto c = [](float v) { return u32(u8(i8(v < -1 ? -127 : v > 1 ? 127 : v * 127.0f))); };
    return c(x) | c(y) << 8 | c(z) << 16;
}

// Iris gbuffers program roles (fallbacks applied by the pack).
enum class PackRole : u8 {
    SkyBasic, SkyTextured, Clouds, Weather, Entities, EntitiesTranslucent, EntitiesGlowing, Block, BlockTranslucent,
    BeaconBeam, Particles, ParticlesTranslucent, Hand, HandTranslucent, ArmorGlint, SpiderEyes, Lightning, Line,
    Textured, Basic, DamagedBlock, ShadowEntities, ShadowBlock
};
// Vanilla blend of the geometry (programs may override with blend.<program> in shaders.properties).
enum class PackBlend : u8 { Opaque, Translucent, Additive, AdditiveAlpha, Glint, Crumbling };

struct PackDraw {
    PackRole role = PackRole::Textured;
    PackBlend blend = PackBlend::Opaque;
    u8 renderStage = 0;                 // Iris renderStage uniform (MC_RENDER_STAGE_*), 0 = from the role
    bool depthTest = true, depthWrite = true, cull = true;
    bool depthEqual = false;            // only on the geometry drawn just before (glint: vanilla EQUAL), no write
    bool hand = false;                  // drawn with the first-person hand (its projection and pass), any role
    bool quads = true;                  // 4 vertices per quad (else a triangle list)
    bool blockAtlas = false;            // texture = the block atlas (uv via atlasUv)
    ID3D12Resource* texture = nullptr;  // Texture2D (RGBA8) when not the block atlas; nullptr = white
    const PackVertex* vertices = nullptr;
    u32 vertexCount = 0;
    i32 entityId = -1, blockEntityId = -1, itemId = -1;  // entityId / blockEntityId / currentRenderedItemId uniforms
    float entityColor[4] = {0, 0, 0, 0};               // entityColor uniform (hurt / flash overlay)
};

class PackSink {
public:
    virtual ~PackSink() = default;
    virtual void draw(const PackDraw& d) = 0;
    // Block texture layer + coordinates inside it (0..1) -> block atlas coordinates. False if unknown.
    virtual bool atlasUv(u32 layer, float u, float v, float out[2]) const = 0;
    // Ids from the pack's entity.properties / item.properties / block.properties (-1 when not mapped).
    virtual int entityId(std::string_view entityType) const = 0;  // "minecraft:zombie"
    virtual int itemId(std::string_view item) const = 0;          // "minecraft:diamond_sword"
    virtual int blockId(u16 blockState) const = 0;                // BlockRegistry state id
    virtual bool vanillaClouds() const = 0;  // the pack draws the game's clouds (shaders.properties clouds != off)
    // Enchantment glint texture (entity / armour glint, item glint) for PackRole::ArmorGlint draws with
    // PackBlend::Glint; uv = the vanilla glint texture transform applied on the CPU (it wraps). nullptr: none.
    virtual ID3D12Resource* glintTexture(bool item) const { (void)item; return nullptr; }
};
