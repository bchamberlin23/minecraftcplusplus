#pragma once
#include "core/common.h"

class World;
class BlockRegistry;
struct ColumnColors;

// Draw ranges per mesh: [layer * 6 + face] for greedy cube quads (opaque, cutout,
// translucent x 6 face directions), then one range of model quads.
constexpr int kRangeCount = 19;
constexpr int kModelRange = 18;
constexpr int kModelQuadWords = 15;  // u32s per model quad on the GPU (60 bytes)

// Meshing input: a 16^3 grid plus a one-cell border, indexed by padIndex.
constexpr int kPad = 18;
constexpr int kPadCells = kPad * kPad * kPad;
inline int padIndex(int x, int y, int z) { return (y + 1) * kPad * kPad + (z + 1) * kPad + (x + 1); }

// Tint colours that quads index (16 bits). Entries 0..TintCount-1 are the fixed tints of BlockInfo faces (assets.h
// Tint; the biome tints hold their defaults), biome colours are added on demand. Thread-safe and
// append-only: the renderer uploads entries as the table grows.
class ColorTable {
public:
    static constexpr u32 kCapacity = 65536;
    static u16 index(u32 rgb);  // 0xRRGGBB; a full table falls back to entry 0
    static size_t size();
    static void read(size_t first, size_t count, u32* out);
};

// Cube quad (16 bytes), smooth-lit like the game's default lighting:
//   geo   = x:4 y:4 z:4 (w-1):4 (h-1):4 face:3 rot:2 colour index low:4, fluid surface:1 (the top of water/lava
//           sits 2/16 lower: all corners of an up face, the top corners of a side face), fluid quad:1 (below),
//           centre shade:1 (the cell in front darkens every corner once more: BlockModelLighter's shadeCenter)
//   tex   = texture layer:12, per corner ambient occlusion (solid cells beside it, 0..3) 2 bits each,
//           colour index high:12
//   sky   = per corner sky light in 4.4 fixed point (average of the four cells in front of the corner)
//   block = per corner block light, same
// Corner c is (c & 1) along the quad's A axis and (c >> 1) along its B axis (see TerrainVS).
// Fluid quads (geo bit 30, full detail; FluidRenderer): one block face. sky = the top's corner heights (bytes by
// a + 2b, in 1/240 block) or a side's two top heights (bytes 0 / 1 by a); block = the block's light (sky << 4 |
// block); tex bits 12..19 = the top's flow angle (255: still, no rotation) instead of the ambient occlusion.
// Coordinates are in cells; LOD meshes scale cells by 2^level on the GPU.
// Model quads: words 0..5 positions, 6..9 UVs, 10 texture | shade << 19, 11 flat light (sky << 4 | block; the
// corners' average) | colour index << 8, 12 / 13 per corner sky / block light (4.4 fixed point, byte c), 14 per corner
// ambient occlusion brightness (byte c, 255 = none) (BlockModelLighter).
constexpr u8 kFullSky = 0xF0;
struct CubeQuad {
    u32 geo, tex, sky, block;
};
constexpr u32 kCubeQuadBytes = sizeof(CubeQuad);
struct SectionMesh {
    u32 counts[kRangeCount] = {};
    std::vector<CubeQuad> cubeQuads;  // ranges 0..17 back to back
    std::vector<u32> modelQuads;  // kModelQuadWords per quad
    bool empty() const { return cubeQuads.empty() && modelQuads.empty(); }
};

// Full-detail section plus a one-cell border from its 26 neighbours (missing ones read as air), with
// the game's stored light for the same cells.
void gatherSection(const World& world, int cx, int sy, int cz, u16* padded, u8* light);

// The video options meshes depend on: Smooth Lighting (corner light averaging and ambient occlusion; off: each face
// lit by the cell in front of it) and See-Through Leaves (off: leaves are opaque and hide the faces between them,
// ModelBlockRenderer.forceOpaque / LeavesBlock.skipRendering). Terrain::rebuildAll re-meshes after a change.
// netherShade: the dimension's cardinal_light is "nether" (CardinalLighting.NETHER: up and down faces 0.9).
struct MeshOptions {
    bool smoothLighting = true, cutoutLeaves = true, netherShade = false;
    bool operator==(const MeshOptions&) const = default;
};
void setMeshOptions(const MeshOptions& o);
MeshOptions meshOptions();

// Greedy-meshes a padded grid of block ids.
//  light: per-cell (sky << 4 | block), or null for open sky everywhere (LOD levels).
//  models: also emit non-cube block models (full detail only).
//  skirts: keep horizontal border faces in the surface band so seams between LOD levels never gap.
//  colors: biome tint colours per padded column (null: the fixed default tints).
//  origin: the section's world block position (full detail: per-position model variants and offsets).
void meshGrid(const u16* padded, const u8* light, const BlockRegistry& registry, bool models, bool skirts,
              SectionMesh& out, const ColumnColors* colors = nullptr, const i32* origin = nullptr);
