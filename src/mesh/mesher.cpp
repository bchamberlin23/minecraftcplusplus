#include "mesh/mesher.h"

#include "assets/assets.h"
#include "world/biome_colors.h"
#include "world/world.h"

#include <algorithm>
#include <atomic>
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

namespace {
std::atomic<bool> gSmoothLighting{true}, gCutoutLeaves{true};
}  // namespace

void setMeshOptions(const MeshOptions& o) {
    gSmoothLighting.store(o.smoothLighting), gCutoutLeaves.store(o.cutoutLeaves);
}

MeshOptions meshOptions() { return {gSmoothLighting.load(), gCutoutLeaves.load()}; }

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
                const bool renderUp = !same(up), renderDown = !same(down) && !infos[ids[down]].occludes;
                const bool renderSide[4] = {!same(north), !same(south), !same(west), !same(east)};
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
                if (renderUp && !(infos[ids[up]].occludes && std::min(std::min(hNW, hSW), std::min(hSE, hNE)) >= 1.0f)) {
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
                    if (!renderSide[k] || infos[ids[q]].occludes) continue;
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
              SectionMesh& out, const ColumnColors* colors) {
    const bool smooth = gSmoothLighting.load(std::memory_order_relaxed), cutoutLeaves = gCutoutLeaves.load(std::memory_order_relaxed);
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
                        const u8 layer = bc.leaves && !cutoutLeaves ? u8(LayerOpaque) : bc.layer;
                        if (hidden && skirts && d >= 2 && bc.layer != LayerTranslucent && onBorder(d, x, y, z) &&
                            (!infos[sc.cube[p + kStrideY]].occludes || !infos[sc.cube[p + off + kStrideY]].occludes))
                            hidden = false;
                        if (!hidden) {
                            const CubeFace& f = bc.faces[d];
                            const int q = p + off;  // the cell the face looks into
                            const u32 lq = light ? light[q] : kFullSky;
                            u32 ao = 0, sky = 0, block = 0;
                            if (light && bc.layer != LayerTranslucent && smooth) {
                                // Smooth lighting: each corner averages the four cells in front of it (dark solid
                                // cells take the centre's light) and darkens by the solid cells beside it; a corner
                                // behind two solid sides counts as solid itself.
                                const int sa = d < 4 ? kStrideX : kStrideZ, sb = d < 2 ? kStrideZ : kStrideY;
                                for (int corner = 0; corner < 4; ++corner) {
                                    const int s1 = q + ((corner & 1) ? sa : -sa), s2 = q + ((corner >> 1) ? sb : -sb);
                                    const bool o1 = sc.solid[s1], o2 = sc.solid[s2];
                                    const int sc3 = s1 + ((corner >> 1) ? sb : -sb);
                                    const u32 n = u32(o1) + u32(o2) + u32(o1 && o2 ? 1 : sc.solid[sc3]);
                                    u32 l1 = light[s1], l2 = light[s2], l3 = o1 && o2 ? l1 : light[sc3];
                                    if (!l1) l1 = lq;
                                    if (!l2) l2 = lq;
                                    if (!l3) l3 = lq;
                                    const u32 cs = ((l1 >> 4) + (l2 >> 4) + (l3 >> 4) + (lq >> 4)) * 4;
                                    const u32 cb = ((l1 & 15) + (l2 & 15) + (l3 & 15) + (lq & 15)) * 4;
                                    ao |= n << (corner * 2);
                                    sky |= cs << (corner * 8);
                                    block |= cb << (corner * 8);
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
                                         (u32(d) << 20) | (rot << 23) | ((k.color & 15) << 25) | (((k.color >> 16) & 1) << 29),
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
                    for (u32 qi = b.quadFirst; qi < b.quadFirst + b.quadCount; ++qi) {
                        const ModelQuad& m = registry.modelQuad(qi);
                        if (m.cull >= 0 && infos[ids[p + kDirOffset[m.cull]]].occludes) continue;
                        u16 pos[12], uv[8];
                        const float base[3] = {float(x), float(y), float(z)};
                        for (int v = 0; v < 4; ++v) {
                            for (int k = 0; k < 3; ++k)
                                pos[v * 3 + k] =
                                    u16(std::clamp((m.pos[v][k] + base[k] + 8.0f) * 2048.0f, 0.0f, 65535.0f));
                            for (int k = 0; k < 2; ++k)
                                uv[v * 2 + k] = u16(std::clamp(m.uv[v][k], 0.0f, 1.0f) * 65535.0f + 0.5f);
                        }
                        for (int w = 0; w < 6; ++w) sc.model.push_back(u32(pos[w * 2]) | (u32(pos[w * 2 + 1]) << 16));
                        for (int w = 0; w < 4; ++w) sc.model.push_back(u32(uv[w * 2]) | (u32(uv[w * 2 + 1]) << 16));
                        sc.model.push_back(u32(m.tex) | (u32(m.tint & 7) << 16) | (u32(m.shade) << 19));
                        const int kind = biomeKind(m.tint);
                        const u32 color = colors && kind >= 0 ? colors->index[kind][(z + 1) * 18 + (x + 1)] : m.tint;
                        sc.model.push_back((light ? light[p] : kFullSky) | (color << 8));
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
