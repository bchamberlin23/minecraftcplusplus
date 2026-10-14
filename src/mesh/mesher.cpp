#include "mesh/mesher.h"

#include "assets/assets.h"
#include "world/biome_colors.h"
#include "world/world.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace {

constexpr int kStrideY = kPad * kPad, kStrideZ = kPad, kStrideX = 1;
constexpr int kDir[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
constexpr int kDirOffset[6] = {-kStrideY, kStrideY, -kStrideZ, kStrideZ, -kStrideX, kStrideX};

// Slice s, row j, column i on the plane of face d -> grid coordinates.
// Columns run along the quad's A axis and rows along its B axis (must match TerrainVS).
inline void slicePos(int d, int s, int i, int j, int& x, int& y, int& z) {
    if (d < 2)
        x = i, y = s, z = j;
    else if (d < 4)
        x = i, y = j, z = s;
    else
        x = s, y = j, z = i;
}

// True when the face of cell (x,y,z) in direction d lies on the grid border.
inline bool onBorder(int d, int x, int y, int z) {
    switch (d) {
        case 0: return y == 0;
        case 1: return y == 15;
        case 2: return z == 0;
        case 3: return z == 15;
        case 4: return x == 0;
        default: return x == 15;
    }
}

struct Key {  // greedy-merge key of a face: everything a merged quad must share; a == 0: no face
    u32 a, sky, block, color;
    bool operator==(const Key&) const = default;
};

struct Scratch {
    u16 cube[kPadCells];
    u8 solid[kPadCells];  // full collision cube: darkens corners (the game's shade brightness 0.2)
    u32 cubeRow[kPad][kPad], occRow[kPad][kPad];  // per padded (y, z) row: bit x + 1 = cube cell / occluding cell
    u16 center[4096];
    Key keys[256];
    std::vector<CubeQuad> buckets[18];
    std::vector<u32> model;
};

Scratch& scratch() {
    thread_local std::unique_ptr<Scratch> s = std::make_unique<Scratch>();
    return *s;
}

// The border cells that come from neighbour (dx, dy, dz) (each -1, 0 or 1, not all 0).
void fillBorder(const World& world, int cx, int sy, int cz, int dx, int dy, int dz, u16* ids, u8* light) {
    const World::Region* r = nullptr;
    const World::Section* s = world.section(cx + dx, sy + dy, cz + dz, &r);
    const World::LightView lv = world.light(cx + dx, sy + dy, cz + dz);
    auto range = [](int d, int& lo, int& hi) { d < 0 ? (lo = hi = -1) : d > 0 ? (lo = hi = 16) : (lo = 0, hi = 15); };
    int x0, x1, y0, y1, z0, z1;
    range(dx, x0, x1), range(dy, y0, y1), range(dz, z0, z1);
    if ((dx || dz) && !world.hasChunk(cx + dx, cz + dz)) {
        // Not loaded (the edge of the world, or still streaming): mirror our own edge cells, so no walls of
        // water or faces show toward terrain the LOD draws there. Real neighbours refresh this section later.
        for (int y = y0; y <= y1; ++y)
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x) {
                    const int src = padIndex(std::clamp(x, 0, 15), std::clamp(y, -1, 16), std::clamp(z, 0, 15));
                    ids[padIndex(x, y, z)] = ids[src];
                    light[padIndex(x, y, z)] = light[src];
                }
        return;
    }
    for (int y = y0; y <= y1; ++y)
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                const int nx = x & 15, ny = y & 15, nz = z & 15;  // -1 -> 15, 16 -> 0
                const int p = padIndex(x, y, z);
                if (s) ids[p] = World::blockAt(*r, *s, u32(ny * 256 + nz * 16 + nx));
                light[p] = lv.at(nx, ny, nz);
            }
}

struct ColorTableData {
    std::mutex mutex;
    std::vector<u32> colors = [] {
        std::vector<u32> c(TintCount);
        for (int t = 0; t < TintCount; ++t) c[t] = fixedTintRgb(u8(t));
        return c;
    }();
    std::unordered_map<u32, u16> indices;
    std::atomic<size_t> size{TintCount};
};
ColorTableData& colorTable() {
    static ColorTableData t;
    return t;
}

// Which biome colour a face tint takes (-1: a fixed tint).
inline int biomeKind(u32 tint) {
    return tint == TintGrass ? BiomeColors::Grass : tint == TintFoliage ? BiomeColors::Foliage : tint == TintWater ? BiomeColors::Water
         : tint == TintDryFoliage ? BiomeColors::DryFoliage : -1;
}

}  // namespace

u16 ColorTable::index(u32 rgb) {
    thread_local std::unordered_map<u32, u16> cache;  // colours are few: most lookups never lock
    if (auto it = cache.find(rgb); it != cache.end()) return it->second;
    ColorTableData& t = colorTable();
    std::lock_guard lock(t.mutex);
    auto [it, inserted] = t.indices.try_emplace(rgb, u16(0));
    if (inserted) {
        if (t.colors.size() >= kCapacity) {
            t.indices.erase(it);
            return 0;
        }
        it->second = u16(t.colors.size());
        t.colors.push_back(rgb);
        t.size = t.colors.size();
    }
    cache.emplace(rgb, it->second);
    return it->second;
}

size_t ColorTable::size() { return colorTable().size.load(); }

void ColorTable::read(size_t first, size_t count, u32* out) {
    ColorTableData& t = colorTable();
    std::lock_guard lock(t.mutex);
    std::copy(t.colors.begin() + ptrdiff_t(first), t.colors.begin() + ptrdiff_t(first + count), out);
}

void gatherSection(const World& world, int cx, int sy, int cz, u16* padded, u8* light) {
    std::fill(padded, padded + kPadCells, u16(0));
    std::fill(light, light + kPadCells, kFullSky);
    const World::Region* r = nullptr;
    const World::Section* s = world.section(cx, sy, cz, &r);
    if (!s) return;
    Scratch& sc = scratch();
    World::decode(*r, *s, sc.center);
    const World::LightView lv = world.light(cx, sy, cz);
    for (int y = 0; y < 16; ++y)
        for (int z = 0; z < 16; ++z) {
            std::memcpy(&padded[padIndex(0, y, z)], &sc.center[y * 256 + z * 16], 32);
            for (int x = 0; x < 16; ++x) light[padIndex(x, y, z)] = lv.at(x, y, z);
        }
    // Straight up and down first: mirrored borders of unloaded neighbours copy from those rows.
    fillBorder(world, cx, sy, cz, 0, -1, 0, padded, light);
    fillBorder(world, cx, sy, cz, 0, 1, 0, padded, light);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
                if (dx || dz) fillBorder(world, cx, sy, cz, dx, dy, dz, padded, light);
}

void gatherRing(const World& world, int cx, int sy, int cz, const u16* padded, u16* ring) {
    const int ox = cx * 16, oy = sy * 16, oz = cz * 16;
    int lastKey[3] = {INT_MIN, 0, 0};  // the section last looked up (most ring cells share one)
    int loaded = 0;
    const World::Region* region = nullptr;
    const World::Section* section = nullptr;
    for (int face = 0; face < 6; ++face)
        for (int v = -1; v <= 16; ++v)
            for (int u = -1; u <= 16; ++u) {
                const int out = (face & 1) ? 17 : -2;
                const int x = face >= 4 ? out : u, y = face < 2 ? out : v, z = face < 2 ? v : face < 4 ? out : u;
                const int wx = ox + x, wy = oy + y, wz = oz + z;
                if (lastKey[0] != wx >> 4 || lastKey[1] != wy >> 4 || lastKey[2] != wz >> 4) {
                    lastKey[0] = wx >> 4, lastKey[1] = wy >> 4, lastKey[2] = wz >> 4;
                    loaded = world.hasChunk(wx >> 4, wz >> 4);
                    section = loaded ? world.section(wx >> 4, wy >> 4, wz >> 4, &region) : nullptr;
                }
                u16& out16 = ring[face * 324 + (v + 1) * 18 + (u + 1)];
                if (!loaded) out16 = padded[padIndex(std::clamp(x, -1, 16), std::clamp(y, -1, 16), std::clamp(z, -1, 16))];
                else out16 = section ? World::blockAt(*region, *section, u32(((wy & 15) << 8) | ((wz & 15) << 4) | (wx & 15))) : 0;
            }
}

namespace {
std::atomic<bool> gSmoothLighting{true}, gCutoutLeaves{true}, gNetherShade{false};
}  // namespace

void setMeshOptions(const MeshOptions& o) {
    gSmoothLighting.store(o.smoothLighting), gCutoutLeaves.store(o.cutoutLeaves), gNetherShade.store(o.netherShade);
}

MeshOptions meshOptions() { return {gSmoothLighting.load(), gCutoutLeaves.load(), gNetherShade.load()}; }

namespace {
// A cell's fluid (FluidState): 0 none, else type (1 water, 2 lava) | amount (1..8) << 2 | falling << 6. Waterlogged
// blocks hold a water source; LiquidBlock level 0 is the source, 1..7 flowing (amount 8 - level), 8.. falling.
u8 fluidCode(const BlockRegistry& registry, u16 id) {
    thread_local std::vector<i16> cache;
    if (cache.size() <= id) cache.resize(size_t(id) + 1, -1);
    i16& c = cache[id];
    if (c >= 0) return u8(c);
    const BlockInfo& b = registry.infos()[id];
    u8 code = 0;
    if (b.fluid) {
        const bool lava = registry.name(id).find("lava") != std::string::npos;
        const std::string_view lv = registry.property(id, "level");
        const int level = lv.empty() ? 0 : std::atoi(std::string(lv).c_str());
        const int amount = level == 0 || level >= 8 ? 8 : 8 - level;
        code = u8((lava ? 2 : 1) | (amount << 2) | (level >= 8 ? 64 : 0));
    } else if (b.waterlogged) {
        code = u8(1 | (8 << 2));
    }
    c = code;
    return code;
}
}  // namespace

// ---- BlockModelLighter: smooth lighting of model quads ----------------------------------------------------
namespace {
constexpr int kStep[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};

// Block.shouldRenderFace beyond full cubes: the face occlusion shapes of the partial blocks that can occlude (slabs,
// stairs, carpets, farmland, dirt paths; the other non-full blocks occlude nothing here, like those with noOcclusion)
// as 16 x 16 masks of their outline boxes on each face, and the skipRendering rules of panes / bars, slime / honey
// blocks and mangrove roots. Cached per block state on each meshing thread.
class FaceShapes {
public:
    using Mask = std::bitset<256>;
    enum Kind : i8 { None, Occluder, Bars, SameSkips, MangroveRoots };
    FaceShapes(const BlockRegistry& registry) : registry_(registry) {}
    Kind kind(u16 id) {
        if (id >= kind_.size()) kind_.resize(size_t(id) + 1024, -1), base_.resize(size_t(id) + 1024);  // states are added at runtime
        i8& k = kind_[id];
        if (k >= 0) return Kind(k);
        const std::string& n = registry_.name(id);
        std::string b = n.substr(0, n.find('['));
        if (b.starts_with("minecraft:")) b.erase(0, 10);
        base_[id] = b;
        k = b.ends_with("_slab") || b.ends_with("_stairs") || b.ends_with("_carpet") || b == "farmland" || b == "dirt_path" ? Occluder
            : b == "iron_bars" || b == "glass_pane" || b.ends_with("_stained_glass_pane") || b.ends_with("copper_bars") ? Bars
            : b == "slime_block" || b == "honey_block"                                                                ? SameSkips
            : b == "mangrove_roots"                                                                                   ? MangroveRoots
                                                                                                                     : None;
        return Kind(k);
    }
    const Mask& mask(u16 id, int d) {  // Shapes.getFaceShape of the outline on face d
        auto [it, fresh] = masks_.try_emplace(id);
        if (fresh) {
            const BlockInfo& b = registry_.infos()[id];
            for (int dir = 0; dir < 6; ++dir) {
                const int a = dir < 2 ? 1 : dir < 4 ? 2 : 0, u = a == 0 ? 2 : 0, v = a == 1 ? 2 : 1;
                for (int k = 0; k < b.selectionCount; ++k) {
                    const Box& box = registry_.box(b.selectionFirst + k);
                    if (dir & 1 ? box.max[a] < 1.0f : box.min[a] > 0.0f) continue;  // not touching that face
                    for (int j = 0; j < 16; ++j)
                        for (int i = 0; i < 16; ++i) {
                            const float cu = (float(i) + 0.5f) / 16.0f, cv = (float(j) + 0.5f) / 16.0f;
                            if (cu > box.min[u] && cu < box.max[u] && cv > box.min[v] && cv < box.max[v]) it->second[dir].set(size_t(j * 16 + i));
                        }
                }
            }
        }
        return it->second[d];
    }
    // A full-block face occluder other than an opaque cube (a slab's bottom, a carpet's underside).
    bool fullFace(u16 id, int d) { return kind(id) == Occluder && mask(id, d).all(); }
    // Block.shouldRenderFace(self, neighbour, d) for a face of a non-cube block.
    bool shouldRenderFace(u16 self, u16 neighbour, int d, const BlockInfo* infos) {
        if (infos[neighbour].occludes || fullFace(neighbour, d ^ 1)) return false;
        if (skipRendering(self, neighbour, d)) return false;
        if (kind(neighbour) != Occluder || kind(self) != Occluder) return true;  // an empty face shape on either side
        const Mask& own = mask(self, d);
        return own.none() || (own & ~mask(neighbour, d ^ 1)).any();
    }

private:
    bool skipRendering(u16 self, u16 neighbour, int d) {
        const Kind k = kind(self);
        if (k == None || k == Occluder) return false;
        kind(neighbour);
        const bool same = base_[self] == base_[neighbour];
        if (k == SameSkips) return same;  // HalfTransparentBlock
        if (k == MangroveRoots) return d < 2 && base_[neighbour] == "mangrove_roots";
        if (!same) return false;  // IronBarsBlock: vertically always, sideways when both connect
        if (d < 2) return true;
        static const char* const kSide[6] = {"", "", "north", "south", "west", "east"};
        return registry_.property(self, kSide[d]) == "true" && registry_.property(neighbour, kSide[d ^ 1]) == "true";
    }
    const BlockRegistry& registry_;
    std::vector<i8> kind_;
    std::vector<std::string> base_;
    std::unordered_map<u16, std::array<Mask, 6>> masks_;
};
FaceShapes& faceShapes(const BlockRegistry& registry) {
    thread_local std::unique_ptr<FaceShapes> shapes;
    thread_local const BlockRegistry* owner = nullptr;
    if (!shapes || owner != &registry) shapes = std::make_unique<FaceShapes>(registry), owner = &registry;
    return *shapes;
}
// SizeInfo: the quad's min / max along a direction's axis (DOWN = minY ... EAST = maxX), then the flipped (1 - v) ones.
enum { SD, SU, SN, SS, SW, SE, FD, FU, FN, FS, FW, FE };
struct Adjacency {
    int corners[4];
    int weights[4][8];
};
constexpr Adjacency kAdjacency[6] = {
    {{4, 5, 2, 3}, {{FW, SS, FW, FS, SW, FS, SW, SS}, {FW, SN, FW, FN, SW, FN, SW, SN}, {FE, SN, FE, FN, SE, FN, SE, SN}, {FE, SS, FE, FS, SE, FS, SE, SS}}},
    {{5, 4, 2, 3}, {{SE, SS, SE, FS, FE, FS, FE, SS}, {SE, SN, SE, FN, FE, FN, FE, SN}, {SW, SN, SW, FN, FW, FN, FW, SN}, {SW, SS, SW, FS, FW, FS, FW, SS}}},
    {{1, 0, 5, 4}, {{SU, FW, SU, SW, FU, SW, FU, FW}, {SU, FE, SU, SE, FU, SE, FU, FE}, {SD, FE, SD, SE, FD, SE, FD, FE}, {SD, FW, SD, SW, FD, SW, FD, FW}}},
    {{4, 5, 0, 1}, {{SU, FW, FU, FW, FU, SW, SU, SW}, {SD, FW, FD, FW, FD, SW, SD, SW}, {SD, FE, FD, FE, FD, SE, SD, SE}, {SU, FE, FU, FE, FU, SE, SU, SE}}},
    {{1, 0, 2, 3}, {{SU, SS, SU, FS, FU, FS, FU, SS}, {SU, SN, SU, FN, FU, FN, FU, SN}, {SD, SN, SD, FN, FD, FN, FD, SN}, {SD, SS, SD, FS, FD, FS, FD, SS}}},
    {{0, 1, 2, 3}, {{FD, SS, FD, FS, SD, FS, SD, SS}, {FD, SN, FD, FN, SD, FN, SD, SN}, {FU, SN, FU, FN, SU, FN, SU, SN}, {FU, SS, FU, FS, SU, FS, SU, SS}}},
};
constexpr int kRemap[6][4] = {{0, 1, 2, 3}, {2, 3, 0, 1}, {3, 0, 1, 2}, {0, 1, 2, 3}, {3, 0, 1, 2}, {1, 2, 3, 0}};  // AmbientVertexRemap
// FaceInfo: the corner of the quad's box (x, y, z: 0 min, 1 max) each baked vertex sits at.
constexpr u8 kFaceInfo[6][4][3] = {{{0, 0, 1}, {0, 0, 0}, {1, 0, 0}, {1, 0, 1}}, {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}},
                                   {{1, 1, 0}, {1, 0, 0}, {0, 0, 0}, {0, 1, 0}}, {{0, 1, 1}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}},
                                   {{0, 1, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 1}}, {{1, 1, 1}, {1, 0, 1}, {1, 0, 0}, {1, 1, 0}}};

// LightCoordsUtil packing: block << 4 | sky << 20 (smooth values: 0..240 in bytes 0 and 2).
u32 javaLight(u8 cell) { return u32(cell & 15) << 4 | u32(cell >> 4) << 20; }
u32 smoothBlend(u32 n1, u32 n2, u32 n3, u32 center) {
    auto sky = [](u32 c) { return (c >> 20) & 15; };
    auto block = [](u32 c) { return (c >> 4) & 15; };
    if (sky(center) > 2 || block(center) > 2) {
        for (u32* n : {&n1, &n2, &n3}) {
            if (*n == 0) *n = center;
            else if (sky(*n) == 0) *n |= center & 0xFF0000;
        }
    }
    return ((n1 + n2 + n3 + center) >> 2) & 0xFF00FF;
}
u32 smoothWeightedBlend(const u32 c[4], const float w[4]) {
    float sky = 0, block = 0;
    for (int i = 0; i < 4; ++i) sky += float((c[i] >> 16) & 0xFF) * w[i], block += float(c[i] & 0xFF) * w[i];
    return (u32(int(block)) & 0xFF) | (u32(int(sky)) & 0xFF) << 16;
}

// The light grid of a padded section as BlockModelLighter sees it.
struct LightGrid {
    const u16* ids;
    const u8* light;
    const BlockInfo* infos;
    const u8* solid;  // getShadeBrightness 0.2
    const u16* ring = nullptr;  // gatherRing: the ids two cells out
    int cell(int x, int y, int z) const {  // clamped to the one-cell border (lookups further out read the border)
        return padIndex(std::clamp(x, -1, 16), std::clamp(y, -1, 16), std::clamp(z, -1, 16));
    }
    u16 idAt(int x, int y, int z) const {  // the block id, from the ring two cells out of a face
        const bool inside = x >= -1 && x <= 16 && y >= -1 && y <= 16 && z >= -1 && z <= 16;
        if (inside || !ring) return ids[cell(x, y, z)];
        const int face = y < -1 ? 0 : y > 16 ? 1 : z < -1 ? 2 : z > 16 ? 3 : x < -1 ? 4 : 5;
        const int u = std::clamp(face >= 4 ? z : x, -1, 16), v = std::clamp(face < 2 ? z : y, -1, 16);
        return ring[face * 324 + (v + 1) * 18 + (u + 1)];
    }
    u32 coords(int c, const BlockInfo& state) const {  // LightCoordsUtil.getLightCoords
        if (state.emissiveRendering) return 0xF000F0u;
        u32 l = javaLight(light[c]);
        if (((l >> 4) & 15) < state.emission) l = (l & 0xFF0000) | (u32(state.emission) << 4);
        return l;
    }
    float shade(int c) const { return solid[c] ? 0.2f : 1.0f; }
};

// BlockModelLighter.prepareQuadAmbientOcclusion for a quad (vertices pos, block-local, our vertex order) facing `dir` of
// the block at cell (x, y, z): per vertex smooth light coords and ambient occlusion brightness. Returns shadeCenter == 0.2.
bool smoothQuad(const LightGrid& g, int x, int y, int z, int dir, const float pos[4][3], bool fullCollision, u32 outLight[4],
                float outAo[4]) {
    // prepareQuadShape
    float lo[3] = {32, 32, 32}, hi[3] = {-32, -32, -32};
    for (int v = 0; v < 4; ++v)
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], pos[v][k]), hi[k] = std::max(hi[k], pos[v][k]);
    const int axis = dir < 2 ? 1 : dir < 4 ? 2 : 0;
    const bool partial = dir < 2   ? lo[0] >= 1e-4f || lo[2] >= 1e-4f || hi[0] <= 0.9999f || hi[2] <= 0.9999f
                         : dir < 4 ? lo[0] >= 1e-4f || lo[1] >= 1e-4f || hi[0] <= 0.9999f || hi[1] <= 0.9999f
                                   : lo[1] >= 1e-4f || lo[2] >= 1e-4f || hi[1] <= 0.9999f || hi[2] <= 0.9999f;
    const bool cubic = lo[axis] == hi[axis] && (((dir & 1) ? hi[axis] > 0.9999f : lo[axis] < 1e-4f) || fullCollision);
    // Our vertices -> the FaceInfo vertex at the same corner of the quad's box.
    int javaIndex[4];
    for (int v = 0; v < 4; ++v) {
        javaIndex[v] = v;
        for (int i = 0; i < 4; ++i) {
            bool match = true;
            for (int k = 0; k < 3 && match; ++k)
                if (k != axis && hi[k] - lo[k] > 1e-5f) match = (pos[v][k] > (lo[k] + hi[k]) * 0.5f) == (kFaceInfo[dir][i][k] != 0);
            if (match) {
                javaIndex[v] = i;
                break;
            }
        }
    }
    float shape[12];
    shape[SW] = lo[0], shape[SE] = hi[0], shape[SD] = lo[1], shape[SU] = hi[1], shape[SN] = lo[2], shape[SS] = hi[2];
    for (int k = 0; k < 6; ++k) shape[6 + k] = 1.0f - shape[k];
    const int* s = kStep[dir];
    const int bx = x + (cubic ? s[0] : 0), by = y + (cubic ? s[1] : 0), bz = z + (cubic ? s[2] : 0);
    const Adjacency& info = kAdjacency[dir];
    u32 l[4];
    float sh[4];
    bool permeable[4];
    for (int k = 0; k < 4; ++k) {
        const int* c = kStep[info.corners[k]];
        const int ci = g.cell(bx + c[0], by + c[1], bz + c[2]);
        l[k] = g.coords(ci, g.infos[g.ids[ci]]);
        sh[k] = g.shade(ci);
        permeable[k] = !g.infos[g.idAt(bx + c[0] + s[0], by + c[1] + s[1], bz + c[2] + s[2])].occludes;  // isLightPermeable
    }
    auto corner = [&](int a, int b, float& shade, u32& lc) {
        if (!permeable[b] && !permeable[a]) {
            shade = sh[0], lc = l[0];
            return;
        }
        const int* ca = kStep[info.corners[a]];
        const int* cb = kStep[info.corners[b]];
        const int ci = g.cell(bx + ca[0] + cb[0], by + ca[1] + cb[1], bz + ca[2] + cb[2]);
        shade = g.shade(ci), lc = g.coords(ci, g.infos[g.ids[ci]]);
    };
    float s02, s03, s12, s13;
    u32 l02, l03, l12, l13;
    corner(0, 2, s02, l02), corner(0, 3, s03, l03), corner(1, 2, s12, l12), corner(1, 3, s13, l13);
    const int center = g.cell(x, y, z), next = g.cell(x + s[0], y + s[1], z + s[2]);
    u32 lightCenter = g.coords(center, g.infos[g.ids[center]]);
    if (cubic || !g.infos[g.ids[next]].occludes) lightCenter = g.coords(next, g.infos[g.ids[next]]);
    const float shadeCenter = g.shade(cubic ? g.cell(bx, by, bz) : center);
    const int* remap = kRemap[dir];
    const float t[4] = {(sh[3] + sh[0] + s03 + shadeCenter) * 0.25f, (sh[2] + sh[0] + s02 + shadeCenter) * 0.25f,
                        (sh[2] + sh[1] + s12 + shadeCenter) * 0.25f, (sh[3] + sh[1] + s13 + shadeCenter) * 0.25f};
    const u32 tc[4] = {smoothBlend(l[3], l[0], l03, lightCenter), smoothBlend(l[2], l[0], l02, lightCenter),
                       smoothBlend(l[2], l[1], l12, lightCenter), smoothBlend(l[3], l[1], l13, lightCenter)};
    u32 vertLight[4];
    float vertAo[4];
    for (int k = 0; k < 4; ++k) {
        if (partial) {
            const int* w = info.weights[k];
            const float wt[4] = {shape[w[0]] * shape[w[1]], shape[w[2]] * shape[w[3]], shape[w[4]] * shape[w[5]], shape[w[6]] * shape[w[7]]};
            vertAo[remap[k]] = std::clamp(t[0] * wt[0] + t[1] * wt[1] + t[2] * wt[2] + t[3] * wt[3], 0.0f, 1.0f);
            vertLight[remap[k]] = smoothWeightedBlend(tc, wt);
        } else {
            vertAo[remap[k]] = t[k];
            vertLight[remap[k]] = tc[k];
        }
    }
    for (int v = 0; v < 4; ++v) outLight[v] = vertLight[javaIndex[v]], outAo[v] = vertAo[javaIndex[v]];
    return shadeCenter < 0.5f;
}
}  // namespace

// FluidRenderer.tesselate: each fluid cell's faces with the corner heights averaged from its neighbours, the top
// textured by its flow (FlowingFluid.getFlow), the sides by the flowing texture (the overlay against glass / leaves).
static void fluidFaces(const u16* ids, const u8* light, const BlockRegistry& registry, const ColumnColors* colors,
                std::vector<CubeQuad>* buckets) {
    const BlockInfo* infos = registry.infos();
    static const u16 waterStill = registry.textureLayer("block/water_still"), waterFlow = registry.textureLayer("block/water_flow"),
                     waterOverlay = registry.textureLayer("block/water_overlay"), lavaStill = registry.textureLayer("block/lava_still"),
                     lavaFlow = registry.textureLayer("block/lava_flow");
    auto code = [&](int p) { return fluidCode(registry, ids[p]); };
    auto type = [&](int p) { return code(p) & 3; };
    auto amount = [&](int p) { return float((code(p) >> 2) & 15) / 9.0f; };  // getOwnHeight
    auto solid = [&](int p) { return infos[ids[p]].collisionCount > 0 && !infos[ids[p]].fluid; };  // BlockState.isSolid
    // BlockState.getFaceOcclusionShape(face) covering 0..1 x 0..h of that face (h: the height up the side faces; the
    // whole face for up / down), from the collision boxes of blocks that can occlude (opaque, not leaves)
    auto covers = [&](int q, int face, float h) {
        const BlockInfo& b = infos[ids[q]];
        if (b.occludes) return true;
        if (b.fluid || b.leaves || b.layer != LayerOpaque || !b.collisionCount) return false;
        const int axis = face < 2 ? 1 : face < 4 ? 2 : 0;
        const bool positive = face & 1;
        const int ua = axis == 1 ? 0 : axis == 2 ? 0 : 2, va = axis == 1 ? 2 : 1;  // the face's in-plane axes
        const float vMax = axis == 1 ? 1.0f : h;
        constexpr int kSamples = 8;
        for (int i = 0; i < kSamples; ++i)
            for (int j = 0; j < kSamples; ++j) {
                const float u = (float(i) + 0.5f) / kSamples, v = (float(j) + 0.5f) / kSamples * vMax;
                bool in = false;
                for (u32 k = 0; k < b.collisionCount && !in; ++k) {
                    const Box& c = registry.box(b.collisionFirst + k);
                    const bool onFace = positive ? c.max[axis] >= 1.0f - 1e-4f : c.min[axis] <= 1e-4f;
                    in = onFace && u >= c.min[ua] && u <= c.max[ua] && v >= c.min[va] && v <= c.max[va];
                }
                if (!in) return false;
            }
        return true;
    };
    for (int y = 0; y < 16; ++y)
        for (int z = 0; z < 16; ++z)
            for (int x = 0; x < 16; ++x) {
                const int p = padIndex(x, y, z);
                const int t = type(p);
                if (!t) continue;
                auto same = [&](int q) { return type(q) == t; };
                auto height = [&](int q) {  // getHeight: 1 under the same fluid, the own height, else 0 / -1 (solid)
                    if (same(q)) return same(q + kStrideY) ? 1.0f : amount(q);
                    return solid(q) ? -1.0f : 0.0f;
                };
                const int up = p + kStrideY, down = p - kStrideY, north = p - kStrideZ, south = p + kStrideZ, west = p - kStrideX,
                          east = p + kStrideX;
                // FluidRenderer.shouldRenderFace: not the same fluid there, not hidden by the block's own face
                // (isFaceOccludedBySelf: a waterlogged stair's back, a slab's bottom); down also not over a covered top
                const bool renderUp = !same(up), renderDown = !same(down) && !covers(p, 0, 1.0f) && !covers(down, 1, 1.0f);
                const bool renderSide[4] = {!same(north) && !covers(p, 2, 1.0f), !same(south) && !covers(p, 3, 1.0f),
                                            !same(west) && !covers(p, 4, 1.0f), !same(east) && !covers(p, 5, 1.0f)};
                if (!renderUp && !renderDown && !renderSide[0] && !renderSide[1] && !renderSide[2] && !renderSide[3]) continue;
                const float self = height(p);
                float hNE = 1, hNW = 1, hSE = 1, hSW = 1;
                if (self < 1.0f) {
                    const float hN = height(north), hS = height(south), hE = height(east), hW = height(west);
                    auto average = [&](float h2, float h1, int corner) {  // calculateAverageHeight
                        if (h1 >= 1.0f || h2 >= 1.0f) return 1.0f;
                        float sum = 0, weight = 0;
                        auto add = [&](float h) {
                            if (h >= 0.8f) sum += h * 10.0f, weight += 10.0f;
                            else if (h >= 0.0f) sum += h, weight += 1.0f;
                        };
                        if (h1 > 0.0f || h2 > 0.0f) {
                            const float c = height(corner);
                            if (c >= 1.0f) return 1.0f;
                            add(c);
                        }
                        add(self), add(h1), add(h2);
                        return weight > 0 ? sum / weight : 0.0f;
                    };
                    hNE = average(hN, hE, north + kStrideX), hNW = average(hN, hW, north - kStrideX);
                    hSE = average(hS, hE, south + kStrideX), hSW = average(hS, hW, south - kStrideX);
                }
                auto hb = [](float h) { return u32(std::clamp(int(std::lround(h * 240.0f)), 0, 255)); };
                // getLightCoords: the brighter of the cell and the one above (sky and block separately)
                auto lightOf = [&](int a, int b) {
                    const u8 la = light[a], lb = light[b];
                    return u32(std::max(la >> 4, lb >> 4) << 4 | std::max(la & 15, lb & 15));
                };
                const int kind = t == 1 ? BiomeColors::Water : -1;
                const u32 color = t == 1 ? (colors ? colors->index[kind][(z + 1) * 18 + (x + 1)] : 5u) : 0u;
                const u8 layer = t == 1 ? u8(LayerTranslucent) : infos[ids[p]].fluid ? infos[ids[p]].layer : u8(LayerOpaque);
                auto emit = [&](int face, u32 tex, u32 angle, u32 heights, u32 lit) {
                    const CubeQuad q{u32(x) | (u32(y) << 4) | (u32(z) << 8) | (u32(face) << 20) | ((color & 15) << 25) | (1u << 30),
                                     tex | (angle << 12) | (((color & 0xFFFF) >> 4) << 20), heights, lit};
                    buckets[layer * 6 + face].push_back(q);
                };
                const u32 still = t == 1 ? waterStill : lavaStill, flowTex = t == 1 ? waterFlow : lavaFlow;
                const u32 lit = lightOf(p, up);
                if (renderUp && !(std::min(std::min(hNW, hSW), std::min(hSE, hNE)) >= 1.0f && covers(up, 0, 1.0f))) {
                    // FlowingFluid.getFlow: the height differences to the neighbours (over a drop: to the fluid below it)
                    double fx = 0, fz = 0;
                    const float own = amount(p);
                    static constexpr int kStep[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
                    const int nq[4] = {north, south, west, east};
                    for (int k = 0; k < 4; ++k) {
                        const int q = nq[k];
                        if (type(q) != 0 && type(q) != t) continue;  // affectsFlow: empty or the same fluid
                        float nh = type(q) ? amount(q) : 0.0f, dist = 0;
                        if (nh == 0.0f) {
                            // BLOCKS_FLUID_FLOW: blocks_motion (approximated by a collision shape) and signs
                            const bool blocks = solid(q) || registry.name(ids[q]).find("_sign") != std::string::npos;
                            if (!blocks && (type(q - kStrideY) == 0 || type(q - kStrideY) == t)) {
                                const float below = type(q - kStrideY) ? amount(q - kStrideY) : 0.0f;
                                if (below > 0.0f) dist = own - (below - 0.8888889f);
                            }
                        } else {
                            dist = own - nh;
                        }
                        fx += kStep[k][0] * dist, fz += kStep[k][1] * dist;
                    }
                    u32 angle = 255, tex = still;
                    if (fx != 0 || fz != 0) {
                        double a = std::atan2(fz, fx) - 3.14159265358979 / 2;
                        a -= std::floor(a / (2 * 3.14159265358979)) * 2 * 3.14159265358979;
                        angle = u32(std::lround(a / (2 * 3.14159265358979) * 255.0)) % 255, tex = flowTex;
                    }
                    emit(1, tex, angle, hb(hNW) | hb(hNE) << 8 | hb(hSW) << 16 | hb(hSE) << 24, lit);
                }
                if (renderDown) emit(0, still, 255, 0, lightOf(down, p));
                // Sides: N, S, W, E with their two top heights by the face's A axis (+x for N / S, +z for W / E)
                const int sideFace[4] = {2, 3, 4, 5}, sideCell[4] = {north, south, west, east};
                const float top[4][2] = {{hNW, hNE}, {hSW, hSE}, {hNW, hSW}, {hNE, hSE}};
                for (int k = 0; k < 4; ++k) {
                    const int q = sideCell[k];
                    // isFaceOccludedByNeighbor: the neighbour's face covers the side up to the fluid's height
                    if (!renderSide[k] || covers(q, sideFace[k] ^ 1, std::max(top[k][0], top[k][1]))) continue;
                    u32 tex = flowTex;
                    if (t == 1) {  // HalfTransparentBlock / LeavesBlock beside it: the overlay texture
                        const std::string& n = registry.name(ids[q]);
                        const bool half = infos[ids[q]].leaves || (n.find("glass") != std::string::npos && n.find("pane") == std::string::npos) ||
                                          n.starts_with("minecraft:ice") || n.starts_with("minecraft:slime_block") ||
                                          n.starts_with("minecraft:honey_block") || n.starts_with("minecraft:frosted_ice");
                        if (half && waterOverlay) tex = waterOverlay;
                    }
                    emit(sideFace[k], tex, 255, hb(top[k][0]) | hb(top[k][1]) << 8, lit);
                }
            }
}

void meshGrid(const u16* ids, const u8* light, const BlockRegistry& registry, bool models, bool skirts,
              SectionMesh& out, const ColumnColors* colors, const i32* origin, const u16* ring) {
    const bool smooth = gSmoothLighting.load(std::memory_order_relaxed), cutoutLeaves = gCutoutLeaves.load(std::memory_order_relaxed);
    const bool netherShade = gNetherShade.load(std::memory_order_relaxed);
    Scratch& sc = scratch();
    const BlockInfo* infos = registry.infos();
    const u16 water = registry.waterId();

    // Full-collision cells darken corners (smooth lighting only; LOD meshes have no light). Cached per block id.
    thread_local std::vector<i8> solidOf;
    if (solidOf.size() < registry.blockCount()) solidOf.resize(registry.blockCount(), -1);
    const bool fluids = models && light;  // full detail: fluids are shaped by FluidRenderer (fluidFaces), not cubes
    for (int i = 0; i < kPadCells; ++i) {
        const BlockInfo& b = infos[ids[i]];
        sc.cube[i] = b.kind == KindCube && !(fluids && b.fluid) ? ids[i] : b.waterlogged && !fluids ? water : 0;
        if (!light) continue;
        i8& solid = solidOf[ids[i]];
        if (solid < 0) {
            bool full = b.occludes;
            if (!full && b.collisionCount == 1) {
                const Box& box = registry.box(b.collisionFirst);
                full = box.min[0] <= 0 && box.min[1] <= 0 && box.min[2] <= 0 && box.max[0] >= 1 && box.max[1] >= 1 &&
                       box.max[2] >= 1;
            }
            // getShadeBrightness overrides: TransparentBlock (glass, copper grates), barrier, light and structure void
            // never darken; mud, soul sand and a full snow layer do.
            const std::string& n = registry.name(ids[i]);
            const std::string_view s = std::string_view(n).substr(0, n.find('['));
            if (s == "minecraft:glass" || s.ends_with("_stained_glass") || s == "minecraft:tinted_glass" || s.ends_with("copper_grate") ||
                s == "minecraft:barrier" || s == "minecraft:light" || s == "minecraft:structure_void")
                full = false;
            else if (s == "minecraft:mud" || s == "minecraft:soul_sand" || (s == "minecraft:snow" && registry.property(ids[i], "layers") == "8"))
                full = true;
            solid = full;
        }
        sc.solid[i] = u8(solid);
    }
    for (auto& b : sc.buckets) b.clear();
    sc.model.clear();

    // Row masks: a slice can only have faces where a cube cell meets a non-occluding neighbour.
    for (int y = 0; y < kPad; ++y)
        for (int z = 0; z < kPad; ++z) {
            u32 cube = 0, occ = 0;
            const int base = y * kPad * kPad + z * kPad;
            for (int x = 0; x < kPad; ++x) {
                const u16 c = sc.cube[base + x];
                cube |= u32(c != 0) << x;
                occ |= u32(infos[c].occludes) << x;
            }
            sc.cubeRow[y][z] = cube, sc.occRow[y][z] = occ;
        }
    auto sliceHasFaces = [&](int d, int s) {
        constexpr u32 kInner = 0x1FFFEu;  // bits 1..16: x 0..15
        u32 acc = 0;
        for (int a = 1; a <= 16; ++a)
            switch (d) {
                case 0: acc |= sc.cubeRow[s + 1][a] & ~sc.occRow[s][a] & kInner; break;
                case 1: acc |= sc.cubeRow[s + 1][a] & ~sc.occRow[s + 2][a] & kInner; break;
                case 2: acc |= sc.cubeRow[a][s + 1] & ~sc.occRow[a][s] & kInner; break;
                case 3: acc |= sc.cubeRow[a][s + 1] & ~sc.occRow[a][s + 2] & kInner; break;
                case 4:
                    for (int z = 1; z <= 16; ++z) acc |= (sc.cubeRow[a][z] >> (s + 1)) & ~(sc.occRow[a][z] >> s) & 1u;
                    break;
                default:
                    for (int z = 1; z <= 16; ++z) acc |= (sc.cubeRow[a][z] >> (s + 1)) & ~(sc.occRow[a][z] >> (s + 2)) & 1u;
                    break;
            }
        return acc != 0;
    };

    // Greedy-merge visible cube faces slice by slice.
    for (int d = 0; d < 6; ++d) {
        const int off = kDirOffset[d];
        for (int s = 0; s < 16; ++s) {
            // Skirts may show hidden faces on the border slices; elsewhere skip slices without candidates.
            if (!(skirts && (s == 0 || s == 15)) && !sliceHasFaces(d, s)) continue;
            bool any = false;
            for (int j = 0; j < 16; ++j)
                for (int i = 0; i < 16; ++i) {
                    int x, y, z;
                    slicePos(d, s, i, j, x, y, z);
                    int p = padIndex(x, y, z);
                    Key key{};
                    if (u16 c = sc.cube[p]) {
                        u16 n = sc.cube[p + off];
                        const BlockInfo& bc = infos[c];
                        const BlockInfo& bn = infos[n];
                        bool hidden = bn.occludes || (n != 0 && bc.layer != LayerOpaque && bn.cullGroup == bc.cullGroup);
                        // Leaves: the faces between them show with See-Through Leaves (full detail; LOD keeps culling them).
                        if (bc.leaves && light) hidden = bn.occludes || (!cutoutLeaves && bn.leaves);
                        else if (bc.leaves && !cutoutLeaves) hidden = hidden || bn.leaves;
                        if (!hidden && models && faceShapes(registry).fullFace(ids[p + off], d ^ 1)) hidden = true;
                        const u8 layer = bc.leaves && !cutoutLeaves ? u8(LayerOpaque) : bc.layer;
                        if (hidden && skirts && d >= 2 && bc.layer != LayerTranslucent && onBorder(d, x, y, z) &&
                            (!infos[sc.cube[p + kStrideY]].occludes || !infos[sc.cube[p + off + kStrideY]].occludes))
                            hidden = false;
                        if (!hidden) {
                            const BlockVariant* bv =
                                origin && bc.variantCount ? registry.variantAt(c, origin[0] + x, origin[1] + y, origin[2] + z) : nullptr;
                            const CubeFace& f = bv ? bv->faces[d] : bc.faces[d];
                            const int q = p + off;  // the cell the face looks into
                            const u32 lq = light ? light[q] : kFullSky;
                            u32 ao = 0, sky = 0, block = 0;
                            bool centerShade = false;
                            // Light-emitting blocks are lit flat (ModelBlockRenderer.tesselateBlock: no ambient occlusion
                            // with a light emission), their block light at least their emission; emissiveRendering ones
                            // full bright.
                            if (light && bc.emissiveRendering) {
                                sky = block = 0xF0F0F0F0u;
                            } else if (light && bc.emission) {
                                sky = ((lq >> 4) * 16) * 0x01010101u;
                                block = (std::max(lq & 15, u32(bc.emission)) * 16) * 0x01010101u;
                            } else if (light && smooth && !bc.fluid && bc.modelAo) {
                                // Smooth lighting (BlockModelLighter.prepareQuadAmbientOcclusion of a full face): per
                                // corner light and the count of darkening cells beside it (2 bits; the cell in front
                                // goes in centerShade, geo bit 31).
                                const float nx = std::max(float(kStep[d][0]), 0.0f), ny = std::max(float(kStep[d][1]), 0.0f),
                                            nz = std::max(float(kStep[d][2]), 0.0f);
                                float corners[4][3];
                                for (int corner = 0; corner < 4; ++corner) {
                                    const float ca = float(corner & 1), cb = float(corner >> 1);
                                    corners[corner][0] = d < 4 ? ca : nx;
                                    corners[corner][1] = d < 2 ? ny : cb;
                                    corners[corner][2] = d < 2 ? cb : d < 4 ? nz : ca;
                                }
                                u32 cl[4];
                                float faceAo[4];
                                const LightGrid grid{ids, light, infos, sc.solid, ring};
                                centerShade = smoothQuad(grid, x, y, z, d, corners, true, cl, faceAo);
                                for (int corner = 0; corner < 4; ++corner) {
                                    const u32 n = u32(std::lround((1.0f - faceAo[corner]) * 5.0f)) - (centerShade ? 1u : 0u);
                                    ao |= std::min(n, 3u) << (corner * 2);
                                    sky |= std::min<u32>((cl[corner] >> 16) & 0xFF, 240) << (corner * 8);
                                    block |= std::min<u32>(cl[corner] & 0xFF, 240) << (corner * 8);
                                }
                            } else {
                                sky = ((lq >> 4) * 16) * 0x01010101u;
                                block = ((lq & 15) * 16) * 0x01010101u;
                            }
                            key.a = 1 + (u32(f.tex) | (u32(f.rot) << 16) | (u32(layer) << 21) | (ao << 23));
                            key.sky = sky, key.block = block;
                            const int kind = biomeKind(f.tint);
                            key.color = colors && kind >= 0 ? colors->index[kind][(z + 1) * 18 + (x + 1)] : f.tint;
                            // Fluid surface (no more of the same fluid above): the game draws it 2/16 lower.
                            if (bc.fluid && sc.cube[p + kStrideY] != c && d != 0) key.color |= 1u << 16;
                            if (centerShade) key.color |= 1u << 17;
                            any = true;
                        }
                    }
                    sc.keys[j * 16 + i] = key;
                }
            if (!any) continue;
            for (int j = 0; j < 16; ++j)
                for (int i = 0; i < 16; ++i) {
                    const Key k = sc.keys[j * 16 + i];
                    if (!k.a) continue;
                    int w = 1;
                    while (i + w < 16 && sc.keys[j * 16 + i + w] == k) ++w;
                    int h = 1;
                    for (; j + h < 16; ++h) {
                        bool rowMatches = true;
                        for (int t = 0; t < w; ++t)
                            if (sc.keys[(j + h) * 16 + i + t] != k) {
                                rowMatches = false;
                                break;
                            }
                        if (!rowMatches) break;
                    }
                    for (int hh = 0; hh < h; ++hh)
                        for (int t = 0; t < w; ++t) sc.keys[(j + hh) * 16 + i + t] = Key{};
                    const u32 v = k.a - 1;
                    const u32 tex = v & 0xFFF, rot = (v >> 16) & 3, layer = (v >> 21) & 3, ao = (v >> 23) & 0xFF;
                    int x, y, z;
                    slicePos(d, s, i, j, x, y, z);
                    const CubeQuad q{u32(x) | (u32(y) << 4) | (u32(z) << 8) | (u32(w - 1) << 12) | (u32(h - 1) << 16) |
                                         (u32(d) << 20) | (rot << 23) | ((k.color & 15) << 25) | (((k.color >> 16) & 1) << 29) | (((k.color >> 17) & 1) << 31),
                                     tex | (ao << 12) | (((k.color & 0xFFFF) >> 4) << 20), k.sky, k.block};
                    sc.buckets[layer * 6 + d].push_back(q);
                    i += w - 1;
                }
        }
    }

    if (fluids) fluidFaces(ids, light, registry, colors, sc.buckets);

    // Non-cube block models: baked quads, culled against occluding neighbours.
    if (models) {
        for (int y = 0; y < 16; ++y)
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x) {
                    int p = padIndex(x, y, z);
                    const BlockInfo& b = infos[ids[p]];
                    if (b.kind != KindModel) continue;
                    u32 quadFirst = b.quadFirst, quadCount = b.quadCount;
                    float offset[3] = {};
                    if (origin) {
                        if (const BlockVariant* bv = b.variantCount ? registry.variantAt(ids[p], origin[0] + x, origin[1] + y, origin[2] + z) : nullptr)
                            quadFirst = bv->quadFirst, quadCount = bv->quadCount;
                        BlockRegistry::offset(b, origin[0] + x, origin[2] + z, offset);
                    }
                    // ModelBlockRenderer.tesselateBlock: smooth lighting unless the block emits light or its model opts
                    // out; else flat (BlockModelLighter.prepareQuadFlat).
                    const bool ao = light && smooth && b.emission == 0 && b.modelAo;
                    bool fullCollision = false;  // isCollisionShapeFullBlock: one box filling the cell
                    if (b.collisionCount == 1) {
                        const Box& cb = registry.box(b.collisionFirst);
                        fullCollision = cb.min[0] <= 0 && cb.min[1] <= 0 && cb.min[2] <= 0 && cb.max[0] >= 1 && cb.max[1] >= 1 &&
                                        cb.max[2] >= 1;
                    }
                    const LightGrid grid{ids, light, infos, sc.solid, ring};
                    FaceShapes& shapes = faceShapes(registry);
                    for (u32 qi = quadFirst; qi < quadFirst + quadCount; ++qi) {
                        const ModelQuad& m = registry.modelQuad(qi);
                        if (m.cull >= 0 && !shapes.shouldRenderFace(ids[p], ids[p + kDirOffset[m.cull]], m.cull, infos)) continue;
                        u32 cornerLight[4], cornerAo[4] = {255, 255, 255, 255};  // smooth coords per our vertex
                        if (ao) {
                            float vertAo[4];
                            smoothQuad(grid, x, y, z, m.dir, m.pos, fullCollision, cornerLight, vertAo);
                            for (int v = 0; v < 4; ++v) cornerAo[v] = u32(std::clamp(vertAo[v], 0.0f, 1.0f) * 255.0f + 0.5f);
                        } else if (light) {  // prepareQuadFlat: a culled face by the cell it faces, else its own (or the
                                             // next if it lies on the block's boundary)
                            int lc = p;
                            if (m.cull >= 0) {
                                lc = p + kDirOffset[m.cull];
                            } else {
                                const int dir = m.dir, axis = dir < 2 ? 1 : dir < 4 ? 2 : 0;
                                float lo = 32, hi = -32;
                                for (int v = 0; v < 4; ++v) lo = std::min(lo, m.pos[v][axis]), hi = std::max(hi, m.pos[v][axis]);
                                if (lo == hi && (((dir & 1) ? hi > 0.9999f : lo < 1e-4f) || fullCollision))
                                    lc = grid.cell(x + kStep[dir][0], y + kStep[dir][1], z + kStep[dir][2]);
                            }
                            for (u32& v : cornerLight) v = grid.coords(lc, b);
                        } else {
                            for (u32& v : cornerLight) v = 0xF000F0u;
                        }
                        u16 pos[12], uv[8];
                        const float base[3] = {float(x) + offset[0], float(y) + offset[1], float(z) + offset[2]};
                        for (int v = 0; v < 4; ++v) {
                            for (int k = 0; k < 3; ++k)
                                pos[v * 3 + k] =
                                    u16(std::clamp((m.pos[v][k] + base[k] + 8.0f) * 2048.0f, 0.0f, 65535.0f));
                            for (int k = 0; k < 2; ++k)
                                uv[v * 2 + k] = u16(std::clamp(m.uv[v][k], 0.0f, 1.0f) * 65535.0f + 0.5f);
                        }
                        for (int w = 0; w < 6; ++w) sc.model.push_back(u32(pos[w * 2]) | (u32(pos[w * 2 + 1]) << 16));
                        for (int w = 0; w < 4; ++w) sc.model.push_back(u32(uv[w * 2]) | (u32(uv[w * 2 + 1]) << 16));
                        const u32 shade = netherShade && m.shadeFace < 2 ? 230u : m.shade;  // CardinalLighting.NETHER
                        sc.model.push_back(u32(m.tex) | (u32(m.tint & 7) << 16) | (shade << 19));
                        const int kind = biomeKind(m.tint);
                        const u32 color = colors && kind >= 0 ? colors->index[kind][(z + 1) * 18 + (x + 1)] : m.tint;
                        // The element's light_emission (QuadInstance.getLightCoordsWithEmission: both levels at least it).
                        u32 skyBytes = 0, blockBytes = 0, aoBytes = 0, skySum = 0, blockSum = 0;
                        for (int v = 0; v < 4; ++v) {
                            u32 sky = (cornerLight[v] >> 16) & 0xFF, blk = cornerLight[v] & 0xFF;
                            if (m.emission) sky = std::max<u32>(sky >> 4, m.emission) << 4, blk = std::max<u32>(blk >> 4, m.emission) << 4;
                            skyBytes |= std::min<u32>(sky, 240) << (v * 8), blockBytes |= std::min<u32>(blk, 240) << (v * 8);
                            aoBytes |= cornerAo[v] << (v * 8);
                            skySum += sky, blockSum += blk;
                        }
                        const u32 lit = ((skySum / 4 + 8) / 16) << 4 | std::min<u32>((blockSum / 4 + 8) / 16, 15);
                        sc.model.push_back(std::min<u32>(lit, 0xFF) | (color << 8));
                        sc.model.push_back(skyBytes);
                        sc.model.push_back(blockBytes);
                        sc.model.push_back(aoBytes);
                    }
                }
    }

    out = {};
    size_t total = 0;
    for (int r = 0; r < 18; ++r) total += sc.buckets[r].size();
    out.cubeQuads.reserve(total);
    for (int r = 0; r < 18; ++r) {
        out.counts[r] = u32(sc.buckets[r].size());
        out.cubeQuads.insert(out.cubeQuads.end(), sc.buckets[r].begin(), sc.buckets[r].end());
    }
    out.counts[kModelRange] = u32(sc.model.size() / kModelQuadWords);
    out.modelQuads.assign(sc.model.begin(), sc.model.end());
}
