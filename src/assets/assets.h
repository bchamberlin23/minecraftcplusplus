#pragma once
#include "core/common.h"

#include <atomic>
#include <deque>
#include <memory>

enum RenderLayer : u8 { LayerOpaque = 0, LayerCutout = 1, LayerTranslucent = 2, LayerCount = 3 };
enum BlockKind : u8 { KindAir = 0, KindCube = 1, KindModel = 2 };
// Block tints (BlockColors): fixed entries of the colour table (mesher.h ColorTable). Grass, foliage, water and dry
// foliage take the biome's colour in the world; redstone wire by its power, stems by their age.
enum Tint : u8 {
    TintNone = 0, TintGrass, TintFoliage, TintBirch, TintSpruce, TintWater, TintLilyPad, TintDryFoliage, TintAttachedStem,
    TintRedstone,                // + power 0..15
    TintStem = TintRedstone + 16,  // + age 0..7
    TintCount = TintStem + 8
};
u32 fixedTintRgb(u8 tint);  // 0xRRGGBB (biome tints: their defaults)

// Face order everywhere: 0 down(-Y) 1 up(+Y) 2 north(-Z) 3 south(+Z) 4 west(-X) 5 east(+X).
struct CubeFace {
    u16 tex = 0;  // texture array layer
    u8 rot = 0;   // texture rotation in 90 degree steps
    u8 tint = 0;
};

// One baked quad of a non-cube block model, in block-local space [0,1].
// Vertices are in "Z" order (0,1,2 / 2,1,3) with front-facing winding.
struct ModelQuad {
    float pos[4][3];
    float uv[4][2];
    u16 tex;
    u8 tint;
    u8 shade;  // 0..255 directional shade (CardinalLighting.DEFAULT of shadeFace)
    i8 shadeFace;  // the face direction whose cardinal shade it takes (nearest to its normal, or the override)
    i8 cull;   // face direction whose neighbour hides this quad, or -1
    u8 layer;
    u8 emission = 0;  // the element's light_emission (its block light is at least this)
};

// Axis-aligned box in block-local units [0,1].
struct Box {
    float min[3], max[3];
};

struct BlockInfo {
    BlockKind kind = KindAir;
    u8 layer = LayerOpaque;
    bool occludes = false;     // opaque full cube: hides neighbouring faces
    bool waterlogged = false;  // cell also contains a water cube
    bool fluid = false;        // water / lava source or flow
    bool replaceable = false;  // placing a block here overwrites it (air, fluids, grass, snow layer...)
    u8 emission = 0;           // block light emitted, 0..15
    bool emissiveRendering = false;  // BlockBehaviour.emissiveRendering: drawn full bright (magma, active sculk sensor)
    u8 opacity = 0;            // light absorbed when passing through (15 = opaque)
    u16 cullGroup = 0;         // same group + non-opaque => shared faces are hidden (glass, water)
    bool leaves = false;       // a LeavesBlock (the See-Through Leaves option)
    CubeFace faces[6];
    u32 quadFirst = 0, quadCount = 0;
    u32 collisionFirst = 0, selectionFirst = 0;  // into BlockRegistry::boxes()
    u8 collisionCount = 0, selectionCount = 0;
    u16 icon = 0;  // texture layer used for UI icons
    // Weighted model lists in the blockstate (grass / stone / sand rotations...): BlockRegistry::variantAt picks one
    // per position; 0 = a single model (the fields above).
    u8 variantCount = 0;
    bool variantMultipart = false;
    u32 variantFirst = 0;
    // BlockBehaviour.OffsetType (flowers, grass, bamboo...): 0 none, 1 XZ, 2 XYZ; see BlockRegistry::offset.
    u8 offsetType = 0;
    float maxOffsetH = 0.25f, maxOffsetV = 0.2f;
};

// One entry of a weighted model list (BlockInfo::variantCount).
struct BlockVariant {
    CubeFace faces[6];
    u32 quadFirst = 0, quadCount = 0;
    u32 weight = 1;
};

constexpr int kTexSize = 16;
constexpr int kTexMips = 5;
constexpr size_t kTexMipOffset[kTexMips] = {0, 1024, 1280, 1344, 1360};
constexpr size_t kTexLayerBytes = 1364;  // 16x16 + 8x8 + 4x4 + 2x2 + 1x1 RGBA8

// Finds the newest installed release jar (falls back to the most recently modified one).
fs::path findMinecraftJar(const fs::path& minecraftDir, std::string* versionOut);
// The jar's world data version (version.json "world_version"), or 0.
int gameDataVersion(const fs::path& jar);

// Maps block states ("minecraft:oak_log[axis=x]") to compact ids with baked render data.
// Asset data is read from the player's own Minecraft jar at startup; nothing is shipped.
class GameReports;
class BlockShapes;

class BlockRegistry {
public:
    BlockRegistry();
    ~BlockRegistry();

    // reports (optional): completes partial state keys with the block's default properties.
    bool init(const fs::path& jarPath, const GameReports* reports = nullptr);
    // Server resource packs (ui/server_config.cpp): re-reads every texture layer from the client asset stack as it is
    // now (same layers; texturePixels() changes). Main thread, with no other thread creating states.
    bool reloadTextures();
    // The game's exact selection / collision shapes for states created from now on (else: from the models).
    void setShapes(const BlockShapes* shapes) { shapes_ = shapes; }

    // Thread-safe. Properties inside [...] must be sorted by key.
    u16 idFor(std::string_view stateKey);
    // Same block with one property changed (or added); returns `id` if the block has no such state.
    u16 withProperty(u16 id, std::string_view key, std::string_view value);
    u16 withPropertyUncached(u16 id, std::string_view key, std::string_view value);
    const std::string& name(u16 id) const;  // full state key
    std::string_view property(u16 id, std::string_view key) const;

    // Storage never moves once allocated, so readers on other threads may keep using
    // existing entries while new states are created.
    const BlockInfo* infos() const { return blocks_.data(); }
    size_t blockCount() const { return blockCount_; }
    const ModelQuad& modelQuad(u32 i) const { return quads_[i]; }
    const Box& box(u32 i) const { return boxes_[i]; }
    // The model variant of state `id` at block (x, y, z), seeded like ModelBlockRenderer (null: a single model).
    const BlockVariant* variantAt(u16 id, i32 x, i32 y, i32 z) const;
    // BlockState.getOffset at block (x, z): the model / shape offset in blocks.
    static void offset(const BlockInfo& b, i32 x, i32 z, float out[3]);
    u16 waterId() const { return waterId_; }

    u32 textureCount() const;  // layers, including the extra frames of animated textures
    // Animated textures (water, lava, fire, portal, sea lantern...): for game tick `tick`, remap[layer] = the layer
    // holding that texture's current frame (identity for static textures). False if nothing is animated.
    bool textureRemap(u64 tick, std::vector<u32>& remap) const;
    std::vector<std::string> textureNames() const;  // by texture layer ("block/stone"); layer 0 = missing
    u16 textureLayer(std::string_view name) const;   // "block/water_flow" -> its layer (0: missing)
    const std::vector<u8>& texturePixels() const;  // textureCount() * kTexLayerBytes

private:
    struct Impl;
    u16 create(std::string_view stateKey);

    std::unique_ptr<Impl> impl_;
    std::vector<BlockInfo> blocks_;  // reserved to the id limit up front: never reallocates
    std::atomic<size_t> blockCount_{0};
    std::deque<ModelQuad> quads_;    // deque: push_back keeps existing elements in place
    std::deque<Box> boxes_;
    std::deque<BlockVariant> variants_;
    std::deque<std::string> names_;
    u16 waterId_ = 0;
    const GameReports* reports_ = nullptr;
    const BlockShapes* shapes_ = nullptr;
};
