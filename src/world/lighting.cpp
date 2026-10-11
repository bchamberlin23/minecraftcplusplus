#include "world/lighting.h"

#include "assets/assets.h"

#include <deque>

namespace {
constexpr int kDir[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
}

// One light channel (sky or block) with a per-update cache of writable section arrays.
struct Lighting::Channel {
    World& world;
    bool sky;
    std::unordered_set<u64>& dirty;
    int minY, maxY;  // block y range [minY, maxY)
    std::unordered_map<u64, World::LightArray*> cache;
    // Optional direct-indexed cache for the 3x3 chunks around (windowCx, windowCz) (chunk lighting).
    int windowCx = INT_MIN, windowCz = 0, windowMinSy = 0, windowSections = 0;
    std::vector<World::LightArray*> window;
    std::vector<u8> windowKnown;

    void useWindow(int cx, int cz) {
        windowCx = cx, windowCz = cz, windowMinSy = minY >> 4, windowSections = (maxY - minY) >> 4;
        window.assign(size_t(9) * windowSections, nullptr);
        windowKnown.assign(window.size(), 0);
    }
    World::LightArray* lookup(int x, int y, int z) {
        const World::Region* r = nullptr;
        const World::Column* c = world.column(x >> 4, z >> 4, &r);
        return c && c->count ? world.lightArray(x >> 4, y >> 4, z >> 4, sky) : nullptr;
    }
    World::LightArray* array(int x, int y, int z) {
        if (windowCx != INT_MIN) {
            const int dx = (x >> 4) - windowCx, dz = (z >> 4) - windowCz;
            if (dx >= -1 && dx <= 1 && dz >= -1 && dz <= 1) {
                const size_t i = size_t((dz + 1) * 3 + dx + 1) * windowSections + size_t((y >> 4) - windowMinSy);
                if (!windowKnown[i]) window[i] = lookup(x, y, z), windowKnown[i] = 1;
                return window[i];
            }
        }
        const u64 k = sectionKey(x >> 4, y >> 4, z >> 4);
        auto it = cache.find(k);
        if (it != cache.end()) return it->second;
        World::LightArray* a = lookup(x, y, z);
        cache.emplace(k, a);
        return a;
    }
    int get(int x, int y, int z) {  // -1 outside the loaded world
        if (y < minY) return 0;
        if (y >= maxY) return sky ? 15 : 0;
        World::LightArray* a = array(x, y, z);
        if (!a) return -1;
        const int i = ((y & 15) << 8) | ((z & 15) << 4) | (x & 15);
        const u8 b = (*a)[i >> 1];
        return (i & 1) ? b >> 4 : b & 15;
    }
    void set(int x, int y, int z, int v) {
        World::LightArray* a = array(x, y, z);
        if (!a) return;
        const int i = ((y & 15) << 8) | ((z & 15) << 4) | (x & 15);
        u8& b = (*a)[i >> 1];
        b = (i & 1) ? u8((b & 0x0F) | (v << 4)) : u8((b & 0xF0) | v);
        // Faces of neighbouring cells sample this light: dirty the section and any neighbour it borders.
        const int cx = x >> 4, sy = y >> 4, cz = z >> 4, lx = x & 15, ly = y & 15, lz = z & 15;
        dirty.insert(sectionKey(cx, sy, cz));
        if (lx == 0) dirty.insert(sectionKey(cx - 1, sy, cz));
        if (lx == 15) dirty.insert(sectionKey(cx + 1, sy, cz));
        if (ly == 0) dirty.insert(sectionKey(cx, sy - 1, cz));
        if (ly == 15) dirty.insert(sectionKey(cx, sy + 1, cz));
        if (lz == 0) dirty.insert(sectionKey(cx, sy, cz - 1));
        if (lz == 15) dirty.insert(sectionKey(cx, sy, cz + 1));
    }
};

void Lighting::relight(bool sky, int x, int y, int z, std::unordered_set<u64>& dirty) {
    Channel ch{world_, sky, dirty, world_.minSectionY() * 16, (world_.maxSectionY() + 1) * 16, {}};
    const BlockInfo* infos = registry_.infos();
    struct Node {
        int x, y, z, level;
    };
    std::deque<Node> removal, flood;

    const int old = ch.get(x, y, z);
    if (old < 0) return;
    // 1. Remove the light at the changed cell and everything that was lit through it.
    if (old > 0) {
        ch.set(x, y, z, 0);
        removal.push_back({x, y, z, old});
    }
    while (!removal.empty()) {
        const Node n = removal.front();
        removal.pop_front();
        for (int d = 0; d < 6; ++d) {
            const int nx = n.x + kDir[d][0], ny = n.y + kDir[d][1], nz = n.z + kDir[d][2];
            const int l = ch.get(nx, ny, nz);
            if (l <= 0) continue;
            const bool dependent = l < n.level || (sky && d == 0 && n.level == 15 && l == 15);
            if (dependent) {
                ch.set(nx, ny, nz, 0);
                removal.push_back({nx, ny, nz, l});
            } else {
                flood.push_back({nx, ny, nz, l});  // independently lit: re-floods the cleared area
            }
        }
    }
    // 2. New sources: an emitting block, and every lit neighbour flowing into the cell.
    const u8 emission = sky ? 0 : infos[world_.block(x, y, z)].emission;
    if (emission > 0 && emission > ch.get(x, y, z)) {
        ch.set(x, y, z, emission);
        flood.push_back({x, y, z, emission});
    }
    for (int d = 0; d < 6; ++d) {
        const int nx = x + kDir[d][0], ny = y + kDir[d][1], nz = z + kDir[d][2];
        if (ch.get(nx, ny, nz) > 0) flood.push_back({nx, ny, nz, 0});
    }
    // 3. Flood fill.
    while (!flood.empty()) {
        const Node n = flood.front();
        flood.pop_front();
        const int l = ch.get(n.x, n.y, n.z);
        if (l <= 0) continue;
        for (int d = 0; d < 6; ++d) {
            const int nx = n.x + kDir[d][0], ny = n.y + kDir[d][1], nz = n.z + kDir[d][2];
            if (ny < ch.minY || ny >= ch.maxY) continue;
            const int opacity = infos[world_.block(nx, ny, nz)].opacity;
            if (opacity >= 15) continue;
            const int nl = (sky && d == 0 && l == 15 && opacity == 0) ? 15 : l - std::max(1, opacity);
            if (nl <= 0) continue;
            const int cur = ch.get(nx, ny, nz);
            if (cur < 0 || cur >= nl) continue;
            ch.set(nx, ny, nz, nl);
            flood.push_back({nx, ny, nz, nl});
        }
    }
}

void Lighting::initChunk(int cx, int cz, std::unordered_set<u64>& dirty) {
    const BlockInfo* infos = registry_.infos();
    const int minSy = world_.minSectionY(), maxSy = world_.maxSectionY();
    const int minY = minSy * 16, maxY = (maxSy + 1) * 16, H = maxY - minY, x0 = cx * 16, z0 = cz * 16;
    constexpr int kDir[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    struct Node {
        int x, y, z;
    };

    // Decode the column once and find, per column, the lowest y still open to the sky.
    thread_local std::vector<u8> opacity;
    opacity.assign(size_t(H) * 256, 0);
    thread_local u16 ids[4096];
    thread_local std::vector<Node> emitters;
    emitters.clear();
    for (int sy = minSy; sy <= maxSy; ++sy) {
        const World::Region* r = nullptr;
        const World::Section* s = world_.section(cx, sy, cz, &r);
        if (!s) continue;
        World::decode(*r, *s, ids);
        for (int i = 0; i < 4096; ++i) {
            const BlockInfo& b = infos[ids[i]];
            opacity[size_t(sy * 16 - minY + (i >> 8)) * 256 + (i & 255)] = b.opacity;
            if (b.emission) emitters.push_back({x0 + (i & 15), sy * 16 + (i >> 8), z0 + ((i >> 4) & 15)});
        }
    }
    int open[256];  // sky reaches y >= open[z * 16 + x] at full strength
    for (int c = 0; c < 256; ++c) {
        int y = H;
        while (y > 0 && opacity[size_t(y - 1) * 256 + c] == 0) --y;
        open[c] = minY + y;
    }

    for (const bool sky : {true, false}) {
        std::vector<World::LightArray*> arrays;
        for (int sy = minSy; sy <= maxSy; ++sy) {
            arrays.push_back(world_.lightArray(cx, sy, cz, sky));
            arrays.back()->fill(0);
        }
        auto put = [&](int x, int y, int z, int v) {
            const int i = ((y & 15) << 8) | (z << 4) | x;
            u8& cell = (*arrays[size_t((y >> 4) - minSy)])[i >> 1];
            cell = (i & 1) ? u8((cell & 0x0F) | (v << 4)) : u8((cell & 0xF0) | v);
        };
        Channel ch{world_, sky, dirty, minY, maxY, {}};
        ch.useWindow(cx, cz);
        // Cells of this chunk go straight to its arrays (all of its sections are dirtied at the end); only light
        // spilling into neighbours goes through the channel.
        auto inside = [&](int x, int z) { return (x >> 4) == cx && (z >> 4) == cz; };
        auto getL = [&](int x, int y, int z) -> int {
            if (y < minY) return 0;
            if (y >= maxY) return sky ? 15 : 0;
            if (!inside(x, z)) return ch.get(x, y, z);
            const int i = ((y & 15) << 8) | ((z & 15) << 4) | (x & 15);
            const u8 b = (*arrays[size_t((y >> 4) - minSy)])[i >> 1];
            return (i & 1) ? b >> 4 : b & 15;
        };
        auto setL = [&](int x, int y, int z, int v) {
            if (inside(x, z))
                put(x & 15, y, z & 15, v);
            else
                ch.set(x, y, z, v);
        };
        auto opacityAt = [&](int x, int y, int z) -> int {
            if (inside(x, z)) return opacity[size_t(y - minY) * 256 + (z & 15) * 16 + (x & 15)];
            return infos[world_.block(x, y, z)].opacity;
        };
        thread_local std::vector<Node> flood;
        flood.clear();
        if (sky) {
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x) {
                    const int h = open[z * 16 + x];
                    for (int y = h; y < maxY; ++y) put(x, y, z, 15);
                    // Light continues into the block below the open run if it only dims (water, leaves).
                    if (h > minY && opacity[size_t(h - 1 - minY) * 256 + z * 16 + x] < 15) flood.push_back({x0 + x, h, z0 + z});
                    // Sideways under overhangs: open cells next to a column that is covered at that height.
                    for (int d = 2; d < 6; ++d) {
                        const int nx = x + kDir[d][0], nz = z + kDir[d][2];
                        int top;
                        if (nx >= 0 && nx < 16 && nz >= 0 && nz < 16) {
                            top = open[nz * 16 + nx];
                        } else {
                            top = h;  // neighbour chunk: probe its light at our open heights
                            for (int y = h; y < maxY && ch.get(x0 + nx, y, z0 + nz) >= 0; ++y)
                                if (ch.get(x0 + nx, y, z0 + nz) < 14 && infos[world_.block(x0 + nx, y, z0 + nz)].opacity < 15)
                                    top = y + 1;
                        }
                        for (int y = h; y < std::min(top, maxY); ++y) flood.push_back({x0 + x, y, z0 + z});
                    }
                }
        } else {
            for (const Node& e : emitters) {
                put(e.x - x0, e.y, e.z - z0, infos[world_.block(e.x, e.y, e.z)].emission);
                flood.push_back(e);
            }
        }
        // Light already in the neighbouring chunks flows in across the borders.
        for (int y = minY; y < maxY; ++y)
            for (int k = 0; k < 16; ++k) {
                const int border[4][2] = {{-1, k}, {16, k}, {k, -1}, {k, 16}};
                for (const auto& b : border)
                    if (ch.get(x0 + b[0], y, z0 + b[1]) > 1) flood.push_back({x0 + b[0], y, z0 + b[1]});
            }

        for (size_t head = 0; head < flood.size(); ++head) {  // breadth first; the vector is the queue
            const Node n = flood[head];
            const int l = getL(n.x, n.y, n.z);
            if (l <= 0) continue;
            for (int d = 0; d < 6; ++d) {
                const int nx = n.x + kDir[d][0], ny = n.y + kDir[d][1], nz = n.z + kDir[d][2];
                if (ny < minY || ny >= maxY) continue;
                const int op = opacityAt(nx, ny, nz);
                if (op >= 15) continue;
                const int nl = (sky && d == 0 && l == 15 && op == 0) ? 15 : l - std::max(1, op);
                if (nl <= 0) continue;
                const int cur = getL(nx, ny, nz);
                if (cur < 0 || cur >= nl) continue;
                setL(nx, ny, nz, nl);
                flood.push_back({nx, ny, nz});
            }
        }
    }
    for (int sy = minSy; sy <= maxSy; ++sy)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
                if (dx == 0 || dz == 0) dirty.insert(sectionKey(cx + dx, sy, cz + dz));
}

void Lighting::blockChanged(int x, int y, int z, std::unordered_set<u64>& dirty) {
    relight(false, x, y, z, dirty);
    relight(true, x, y, z, dirty);
}
