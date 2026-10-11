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
    std::vector<u32> colors{0xFFFFFF, 0x91BD59, 0x77AB2F, 0x80A755, 0x619961, 0x3F76E4, 0x208030, 0x8B1A00};
    std::unordered_map<u32, u16> indices;
    std::atomic<size_t> size{8};
};
ColorTableData& colorTable() {
    static ColorTableData t;
    return t;
}

// Which biome colour a face tint takes (-1: a fixed tint).
inline int biomeKind(u32 tint) { return tint == 1 ? BiomeColors::Grass : tint == 2 ? BiomeColors::Foliage : tint == 5 ? BiomeColors::Water : -1; }

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

void meshGrid(const u16* ids, const u8* light, const BlockRegistry& registry, bool models, bool skirts,
              SectionMesh& out, const ColumnColors* colors) {
    Scratch& sc = scratch();
    const BlockInfo* infos = registry.infos();
    const u16 water = registry.waterId();

    // Full-collision cells darken corners (smooth lighting only; LOD meshes have no light). Cached per block id.
    thread_local std::vector<i8> solidOf;
    if (solidOf.size() < registry.blockCount()) solidOf.resize(registry.blockCount(), -1);
    for (int i = 0; i < kPadCells; ++i) {
        const BlockInfo& b = infos[ids[i]];
        sc.cube[i] = b.kind == KindCube ? ids[i] : b.waterlogged ? water : 0;
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
                        if (hidden && skirts && d >= 2 && bc.layer != LayerTranslucent && onBorder(d, x, y, z) &&
                            (!infos[sc.cube[p + kStrideY]].occludes || !infos[sc.cube[p + off + kStrideY]].occludes))
                            hidden = false;
                        if (!hidden) {
                            const CubeFace& f = bc.faces[d];
                            const int q = p + off;  // the cell the face looks into
                            const u32 lq = light ? light[q] : kFullSky;
                            u32 ao = 0, sky = 0, block = 0;
                            if (light && bc.layer != LayerTranslucent) {
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
                            key.a = 1 + (u32(f.tex) | (u32(f.rot) << 16) | (u32(bc.layer) << 21) | (ao << 23));
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
                        sc.model.push_back(u32(m.tex) | (u32(m.tint) << 16) | (u32(m.shade) << 19));
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
