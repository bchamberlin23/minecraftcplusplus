#include "world/terrain.h"

#include "assets/assets.h"
#include "core/jobs.h"
#include "render/renderer.h"
#include "world/lighting.h"
#include "world/world.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cmath>

namespace {
std::atomic<u64> gBuildNs[5];  // profiling: level-1 grids, coarser grids + far, padding, meshing, colours
constexpr float kLoadMargin = 64.0f;            // stream detail slightly beyond the refine distance
constexpr float kEvictMargin = 192.0f;          // hysteresis before dropping detail
constexpr size_t kBatch = 1024;                 // sections per worker batch
constexpr u64 kUploadBudget = u64(32) << 20;    // bytes of new detail geometry per frame

struct Grid {
    int sx = 0, sy = 0, sz = 0;
    std::vector<u16> v;
    void resize(int x, int y, int z) {
        sx = x, sy = y, sz = z;
        v.assign(size_t(x) * y * z, 0);
    }
    u16& at(int x, int y, int z) { return v[(size_t(y) * sz + z) * sx + x]; }
    u16 at(int x, int y, int z) const { return v[(size_t(y) * sz + z) * sx + x]; }
};

u64 regionKey(int rx, int rz) { return (u64(u32(rx)) << 32) | u32(rz); }
}  // namespace

Terrain::Terrain(const World& world, const BlockRegistry& registry, Renderer& renderer, float detailRadius)
    : world_(world), registry_(registry), renderer_(renderer), detailRadius_(detailRadius) {
    minSy_ = world.minSectionY();
    worldMinY_ = minSy_ * 16;
    const int sectionsY = world.maxSectionY() - minSy_ + 1;
    heightBlocks_ = sectionsY * 16;

    const BlockInfo* infos = registry.infos();
    water_ = registry.waterId();
    for (size_t id = 0; id < registry.blockCount() && !solidFill_; ++id)
        if (infos[id].occludes) solidFill_ = u16(id);
    syncRegions();
    worker_ = std::thread([this] { workerMain(); });
}

int Terrain::regionAt(int rx, int rz) {
    auto [it, inserted] = regionIndex_.try_emplace(regionKey(rx, rz), int(regions_.size()));
    if (!inserted) return it->second;
    RegionLod& r = regions_.emplace_back();
    r.rx = rx, r.rz = rz;
    const int sectionsY = heightBlocks_ / 16;
    for (int level = 0; level <= kMaxLevel; ++level) {
        const int per = 32 >> level;
        r.ny[level] = sectionsY;  // every level is 16 blocks tall
        if (level > 0) r.nodes[level].assign(size_t(per) * per * r.ny[level], kAbsent);  // level 0: on first use
    }
    return it->second;
}

// Attaches the regions the world gained since the last call (possibly far-only until now).
void Terrain::syncRegions() {
    const auto& src = world_.regions();
    for (size_t i = syncedWorldRegions_; i < src.size(); ++i) {
        RegionLod& r = regions_[size_t(regionAt(src[i]->rx, src[i]->rz))];
        r.world = int(i);
        for (int idx = 0; idx < 1024; ++idx) {
            const World::Column& c = src[i]->columns[idx];
            for (int k = 0; k < c.count; ++k) {
                const int ny = c.minY + k - minSy_;
                if (src[i]->sections[c.first + k].paletteSize && ny >= 0 && ny < r.ny[0])
                    r.at(0, idx & 31, ny, idx >> 5) = kNotReady;
            }
        }
    }
    syncedWorldRegions_ = src.size();
}

void Terrain::markDirty(int region) {
    const double now = nowSeconds();
    auto [it, inserted] = dirtyRegions_.try_emplace(region, now, now);
    it->second.second = now;
}

Terrain::CoarseJob Terrain::coarseJob(int region) const {
    const RegionLod& r = regions_[size_t(region)];
    CoarseJob job{region, r.world >= 0 ? world_.regions()[size_t(r.world)].get() : nullptr, {}, r.farData};
    std::copy(std::begin(r.ny), std::end(r.ny), job.ny);
    job.rx = r.rx, job.rz = r.rz;
    return job;
}

Terrain::~Terrain() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    farCv_.notify_all();
    worker_.join();
    for (auto& t : farThreads_) t.join();
}

float Terrain::distance(int level, double x0, double y0, double z0, double px, double py, double pz) const {
    const double s = double(16 << level);  // node width; nodes are 16 blocks tall
    const double dx = std::max({x0 - px, 0.0, px - (x0 + s)});
    const double dy = std::max({y0 - py, 0.0, py - (y0 + 16.0)});
    const double dz = std::max({z0 - pz, 0.0, pz - (z0 + s)});
    return float(std::sqrt(dx * dx + dy * dy + dz * dz));
}

// Downsamples one region into LOD grids and meshes every non-empty node of levels 1..kMaxLevel, one 16-block
// layer at a time (plus a row above and below for the mesher's border). Level-1 cells take a cube from their
// 2x2 column footprint at every block height; each coarser level merges 2x2 cells of the previous one. Chunk
// columns the world lacks take the far columns from their level up. Pure with respect to Terrain state, so it
// runs on worker threads.
void Terrain::buildRegion(const CoarseJob& job, CoarseBuild& out, bool lockWorld, int layerFrom, int layerTo) const {
    const BlockInfo* infos = registry_.infos();
    auto cubeOf = [&](u16 id) -> u16 {
        const BlockInfo& b = infos[id];
        return b.kind == KindCube ? id : b.waterlogged ? water_ : 0;
    };
    thread_local Grid grids[kMaxLevel + 1];  // per level: (512 >> level)^2 columns x 18 rows
    thread_local std::vector<u8> occludes;  // per block id (states may be added at any time: refresh per build)
    occludes.resize(registry_.blockCount());
    for (size_t id = 0; id < occludes.size(); ++id) occludes[id] = infos[id].occludes;
    thread_local u16 ids[4096];
    thread_local u16 padded[kPadCells];
    out.region = job.region;
    out.source = job.source;
    out.farLevel = job.farData ? job.farData->level : 0;
    for (int level = 1; level <= kMaxLevel; ++level)
        out.nodes[level].assign(size_t(32 >> level) * (32 >> level) * job.ny[level], kAbsent);

    bool present[1024] = {};
    if (job.source) {
        std::shared_lock<std::shared_mutex> lock;
        if (lockWorld) lock = std::shared_lock(world_.mutex());
        for (int idx = 0; idx < 1024; ++idx) present[idx] = job.source->columns[idx].count > 0;
    }
    const int farLevel = job.farData ? job.farData->level : kMaxLevel + 1;
    const int first = job.source ? 1 : farLevel;  // finest level with data
    for (int idx = 0; idx < 1024; ++idx) out.colLevel[idx] = present[idx] || !job.farData ? 0 : u8(farLevel);
    if (first > kMaxLevel) return;

    // Tints of a coarse node: the biome at each cell column's centre, from the world where it has the chunk,
    // else from the far columns (no blending: a cell is already 2+ blocks wide). The same for every layer.
    thread_local std::vector<ColumnColors> nodeColors[kMaxLevel + 1];
    thread_local std::vector<u8> nodeColorsDone[kMaxLevel + 1];
    for (int level = 1; level <= kMaxLevel; ++level) {
        nodeColors[level].resize(size_t(32 >> level) * (32 >> level));
        nodeColorsDone[level].assign(size_t(32 >> level) * (32 >> level), 0);
    }
    auto colorsFor = [&](int level, int nx, int nz) -> const ColumnColors& {
        const size_t node = size_t(nz) * (32 >> level) + nx;
        ColumnColors& c = nodeColors[level][node];
        if (nodeColorsDone[level][node]) return c;
        nodeColorsDone[level][node] = 1;
        static constexpr u16 kDefaults[BiomeColors::KindCount] = {1, 2, 5};
        const int s = 1 << level;
        std::shared_lock<std::shared_mutex> lock;
        if (lockWorld && job.source) lock = std::shared_lock(world_.mutex());
        u8 lastBiome = World::kUnknownBiome;
        u16 last[BiomeColors::KindCount] = {kDefaults[0], kDefaults[1], kDefaults[2]};
        for (int pz = -1; pz <= 16; ++pz)
            for (int px = -1; px <= 16; ++px) {
                const int lx = std::clamp((nx * 16 + px) * s + s / 2, 0, 511), lz = std::clamp((nz * 16 + pz) * s + s / 2, 0, 511);
                u8 b = World::kUnknownBiome;
                if (job.source && present[(lz >> 4) * 32 + (lx >> 4)])
                    b = job.source->biomes[size_t((lz >> 4) * 32 + (lx >> 4)) * 16 + ((lz >> 2) & 3) * 4 + ((lx >> 2) & 3)];
                else if (job.farData && !job.farData->biomes.empty())
                    b = job.farData->biomes[size_t(lz >> job.farData->level) * job.farData->n + size_t(lx >> job.farData->level)];
                if (b != lastBiome) {
                    lastBiome = b;
                    for (int k = 0; k < BiomeColors::KindCount; ++k)
                        last[k] = b == World::kUnknownBiome ? kDefaults[k]
                                                            : ColorTable::index(biomeColors_->color(b, BiomeColors::Kind(k)));
                }
                for (int k = 0; k < BiomeColors::KindCount; ++k) c.index[k][(pz + 1) * 18 + (px + 1)] = last[k];
            }
        return c;
    };

    const int layers = heightBlocks_ / 16;
    auto lap = [t = std::chrono::steady_clock::now()](int phase) mutable {
        const auto now = std::chrono::steady_clock::now();
        gBuildNs[phase] += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(now - t).count());
        t = now;
    };
    for (int ly = std::max(layerFrom, 0); ly < std::min(layerTo, layers); ++ly) {
        const int row0 = ly * 16 - 1;  // block row (from the bottom of the world) of grid row 0
        lap(3);
        for (int level = 1; level <= kMaxLevel; ++level) {
            if (level == 2) lap(0);
            Grid& dst = grids[level];
            dst.resize(512 >> level, 18, 512 >> level);
            if (level == 1 && job.source) {
                const World::Region& region = *job.source;
                for (int idx = 0; idx < 1024; ++idx) {
                    if (!present[idx]) continue;
                    std::shared_lock<std::shared_mutex> lock;
                    if (lockWorld) lock = std::shared_lock(world_.mutex());  // edits may move section storage
                    const World::Column c = region.columns[idx];
                    const int bx = (idx & 31) * 8, bz = (idx >> 5) * 8;
                    for (int sy = ly - 1; sy <= ly + 1; ++sy) {
                        const int k = sy + minSy_ - c.minY;
                        if (k < 0 || k >= c.count) continue;
                        const World::Section& sec = region.sections[c.first + k];
                        const int yFrom = std::max(0, row0 - sy * 16), yTo = std::min(15, row0 + 17 - sy * 16);
                        if (!sec.paletteSize || yFrom > yTo) continue;
                        if (sy == ly) World::decode(region, sec, ids);
                        for (int y = yFrom; y <= yTo; ++y)
                            for (int z = 0; z < 8; ++z)
                                for (int x = 0; x < 8; ++x) {
                                    u16 best = 0;
                                    for (int q = 0; q < 4 && !best; ++q) {
                                        const u32 i = u32(y * 256 + (z * 2 + (q >> 1)) * 16 + x * 2 + (q & 1));
                                        best = cubeOf(sy == ly ? ids[i] : World::blockAt(region, sec, i));
                                    }
                                    dst.at(bx + x, sy * 16 + y - row0, bz + z) = best;
                                }
                    }
                }
            } else if (level > 1 && level - 1 >= first) {
                const Grid& src = grids[level - 1];
                for (int y = 0; y < 18; ++y)
                    for (int z = 0; z < dst.sz; ++z)
                        for (int x = 0; x < dst.sx; ++x) {
                            u16 best = 0;
                            for (int q = 0; q < 4 && !best; ++q) best = src.at(x * 2 + (q & 1), y, z * 2 + (q >> 1));
                            dst.at(x, y, z) = best;
                        }
            }
            if (level != farLevel) continue;
            // Far columns where the world lacks the chunk (a level-5 cell spans 2x2 chunks: fill only its gaps).
            const FarColumns& f = *job.farData;
            const int s = 1 << level, per = 512 >> level;
            for (int z = 0; z < per; ++z)
                for (int x = 0; x < per; ++x) {
                    int absent = 0, total = 0;
                    for (int cz = (z * s) >> 4; cz <= ((z + 1) * s - 1) >> 4; ++cz)
                        for (int cx = (x * s) >> 4; cx <= ((x + 1) * s - 1) >> 4; ++cx)
                            ++total, absent += !present[cz * 32 + cx];
                    if (!absent) continue;
                    auto [b, e] = f.column(x, z);
                    for (const FarColumns::Run* r = b; r != e; ++r) {
                        const int from = std::max(0, r->y - worldMinY_ - row0), to = std::min(17, r->y + int(r->count) - 1 - worldMinY_ - row0);
                        const u16 cube = cubeOf(r->block);
                        for (int y = from; y <= to; ++y) {
                            u16& v = dst.at(x, y, z);
                            if (absent == total || !v) v = cube;
                        }
                    }
                }
        }

        lap(1);
        // Nothing in this layer or the rows next to it (sky): no nodes.
        {
            const Grid& g = grids[std::max(first, 1)];
            if (std::all_of(g.v.begin(), g.v.end(), [](u16 v) { return v == 0; })) continue;
        }
        for (int level = std::max(first, 1); level <= kMaxLevel; ++level) {
            const Grid& g = grids[level];
            const int per = 32 >> level;
            for (int nz = 0; nz < per; ++nz)
                for (int nx = 0; nx < per; ++nx) {
                    bool any = false, open = false;  // open: some cell (or border cell) lets a face show
                    for (int py = -1; py <= 16; ++py) {
                        const int by = ly * 16 + py;  // block row from the bottom of the world
                        for (int pz = -1; pz <= 16; ++pz) {
                            u16* row = &padded[padIndex(-1, py, pz)];
                            const int gz = nz * 16 + pz, gx0 = nx * 16;
                            if (by >= heightBlocks_) {  // sky
                                std::fill(row, row + 18, u16(0));
                                open = true;
                                continue;
                            }
                            if (by < 0 || gz < 0 || gz >= g.sz) {  // below the world / unknown neighbour region
                                std::fill(row, row + 18, solidFill_);
                                continue;
                            }
                            const u16* src = &g.v[(size_t(py + 1) * g.sz + gz) * g.sx];
                            row[0] = gx0 > 0 ? src[gx0 - 1] : solidFill_;
                            std::memcpy(row + 1, src + gx0, 16 * sizeof(u16));
                            row[17] = gx0 + 16 < g.sx ? src[gx0 + 16] : solidFill_;
                            const bool interior = py >= 0 && py < 16 && pz >= 0 && pz < 16;
                            for (int i = 0; i < 18; ++i) {
                                open |= !occludes[row[i]];
                                if (interior && i > 0 && i < 17) any |= row[i] != 0;
                            }
                        }
                    }
                    if (!any) continue;
                    out.nodes[level][(size_t(ly) * per + nz) * per + nx] = Renderer::kNoSlot;
                    lap(2);
                    if (!open) continue;  // solid through and through (underground): no faces to mesh
                    SectionMesh mesh;
                    const ColumnColors* colors = biomeColors_ ? &colorsFor(level, nx, nz) : nullptr;
                    lap(4);
                    meshGrid(padded, nullptr, registry_, false, true, mesh, colors);
                    lap(3);
                    if (!mesh.empty()) out.meshes.push_back({{level, nx, ly, nz}, std::move(mesh)});
                }
        }
    }
}

void Terrain::applyCoarse(CoarseBuild& b, bool replace) {
    RegionLod& r = regions_[b.region];
    markCut(b.region);  // its LOD meshes (slots) are replaced below
    const World::Region* source = r.world >= 0 ? world_.regions()[size_t(r.world)].get() : nullptr;
    if (b.source != source || b.farLevel != (r.farData ? r.farData->level : 0)) {  // built from outdated inputs
        markDirty(b.region);
        return;
    }
    std::copy(std::begin(b.colLevel), std::end(b.colLevel), r.colLevel);
    for (int level = 1; level <= kMaxLevel; ++level) {
        const int per = 32 >> level;
        r.refine[level].assign(size_t(per) * per, 0);
        for (int idx = 0; idx < 1024; ++idx) {
            u8& m = r.refine[level][size_t(((idx >> 5) >> level) * per + ((idx & 31) >> level))];
            m = std::max(m, r.colLevel[idx]);
        }
    }
    for (int level = 1; level <= kMaxLevel; ++level) {
        if (replace)
            for (u32 s : r.nodes[level])
                if (isSlot(s)) renderer_.removeMesh(s);
        r.nodes[level] = std::move(b.nodes[level]);
    }
    for (auto& [c, m] : b.meshes) {
        const int level = c[0], size = 16 << level;
        r.at(level, c[1], c[2], c[3]) =
            renderer_.addMesh(m, level, r.rx * 512 + c[1] * size, worldMinY_ + c[2] * 16, r.rz * 512 + c[3] * size);
        m = {};
    }
    // Sections created by edits since the build started must stay reachable from the LOD tree.
    for (int ny = 0; ny < r.ny[0] && !r.nodes[0].empty(); ++ny)
        for (int z = 0; z < 32; ++z)
            for (int x = 0; x < 32; ++x)
                if (r.at(0, x, ny, z) != kAbsent)
                    for (int level = 1; level <= kMaxLevel; ++level) {
                        u32& a = r.at(level, x >> level, ny, z >> level);
                        if (a == kAbsent) a = Renderer::kNoSlot;
                    }
}

bool Terrain::buildCoarse() {
    std::vector<CoarseBuild> builds(regions_.size());
    std::vector<CoarseJob> jobs;
    for (size_t i = 0; i < regions_.size(); ++i) jobs.push_back(coarseJob(int(i)));
    // Every 16-block layer builds independently: spread (region, layer group) pieces over all cores, then merge.
    Timer bt;
    constexpr int kLayersPerPiece = 4;
    const int pieces = (heightBlocks_ / 16 + kLayersPerPiece - 1) / kLayersPerPiece;
    std::vector<CoarseBuild> parts(regions_.size() * size_t(pieces));
    Jobs::parallelFor(parts.size(), [&](size_t i) {
        const size_t region = i / size_t(pieces);
        const int piece = int(i % size_t(pieces));
        buildRegion(jobs[region], parts[i], false, piece * kLayersPerPiece, (piece + 1) * kLayersPerPiece);
    });
    for (size_t region = 0; region < regions_.size(); ++region) {
        CoarseBuild& b = builds[region];
        b = std::move(parts[region * size_t(pieces)]);
        for (int piece = 1; piece < pieces; ++piece) {
            CoarseBuild& p = parts[region * size_t(pieces) + size_t(piece)];
            for (int level = 1; level <= kMaxLevel; ++level)
                for (size_t n = 0; n < b.nodes[level].size(); ++n)
                    if (p.nodes[level][n] != kAbsent) b.nodes[level][n] = p.nodes[level][n];
            for (auto& m : p.meshes) b.meshes.push_back(std::move(m));
        }
    }
    LOG("lod build %.1f ms wall | cpu: level-1 grids %.0f ms, coarser + far %.0f ms, padding %.0f ms, colours %.0f ms, "
        "meshing %.0f ms",
        bt.ms(), gBuildNs[0] / 1e6, gBuildNs[1] / 1e6, gBuildNs[2] / 1e6, gBuildNs[4] / 1e6, gBuildNs[3] / 1e6);

    u64 cubeQuads = 0;
    size_t nodes = 0;
    for (const auto& b : builds)
        for (const auto& [c, m] : b.meshes) cubeQuads += m.cubeQuads.size(), ++nodes;

    // Full-detail budget: columns within the eviction radius, with generous per-column headroom.
    // Coarse levels get 50% headroom for rebuilds after edits.
    const double chunkRadius = (detailRadius_ + kEvictMargin) / 16.0 + 2.0;
    const double columns = 3.14159 * chunkRadius * chunkRadius;
    const u64 detailCube = u64(columns * 4096), detailModel = u64(columns * 2048);
    u32 slots = u32(nodes * 3 / 2 + columns * 24 + 4096);
    u64 farQuads = 0;
    if (farRadius_ > 0) {  // far regions: mostly levels 4-5 (a few hundred quads), a few near ones at level 2
        const double r = farRadius_ / 512.0 + 2.0, regions = 3.14159 * r * r;
        farQuads = u64(regions * 4000) + (u64(4) << 20);
        slots += u32(regions * 80);
    }
    if (!renderer_.createWorldStorage(cubeQuads * 3 / 2 + detailCube + farQuads, detailModel, slots)) return false;

    for (auto& b : builds) applyCoarse(b, false);
    renderer_.flushUploads();
    stats_.coarseNodes = nodes;
    return true;
}

void Terrain::workerMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::vector<Request> batch;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return quit_ || !queue_.empty() || !coarseQueue_.empty(); });
            if (quit_) return;
            if (queue_.empty()) {  // full-detail work first; coarse rebuilds when idle
                const CoarseJob job = coarseQueue_.back();
                coarseQueue_.pop_back();
                lock.unlock();
                CoarseBuild build;
                buildRegion(job, build, true);
                lock.lock();
                coarseDone_.push_back(std::move(build));
                continue;
            }
            batch.swap(queue_);
            queue_.clear();
        }
        std::vector<Result> results(batch.size());
        Jobs::parallelFor(batch.size(), [&](size_t i) {
            thread_local u16 padded[kPadCells];
            thread_local u8 light[kPadCells];
            thread_local ColumnColors colors;
            const Request& r = batch[i];
            {
                std::shared_lock lock(world_.mutex());  // the main thread may be editing
                gatherSection(world_, r.cx, r.sy, r.cz, padded, light);
                if (biomeColors_) gatherColumnColors(world_, *biomeColors_, r.cx, r.cz, colors);
            }
            results[i].req = r;
            meshGrid(padded, light, registry_, true, false, results[i].mesh, biomeColors_ ? &colors : nullptr);
        });
        {
            std::lock_guard lock(mutex_);
            for (auto& r : results) done_.push_back(std::move(r));
            busy_ = false;
        }
        cv_.notify_all();
    }
}

void Terrain::collectResults(double x, double y, double z, bool unlimited) {
    {
        std::lock_guard lock(mutex_);
        for (auto& r : done_) pendingUpload_.push_back(std::move(r));
        done_.clear();
    }
    const float evict = detailRadius_ + kEvictMargin;
    u64 uploaded = 0;
    size_t n = 0;
    for (; n < pendingUpload_.size() && (unlimited || uploaded < kUploadBudget); ++n) {
        Result& r = pendingUpload_[n];
        --inFlight_;
        auto stamp = stamps_.find(Lighting::sectionKey(r.req.cx, r.req.sy, r.req.cz));
        if (stamp != stamps_.end() && stamp->second != r.req.stamp) continue;  // edited since: remesh() owns it
        u32& state = detailState(r.req);
        if (r.req.refresh) {  // swap the mesh of a section that stayed resident
            if (!isSlot(state) && state != Renderer::kNoSlot) continue;
            if (isSlot(state)) renderer_.removeMesh(state);
            state = r.mesh.empty() ? Renderer::kNoSlot
                                   : renderer_.addMesh(r.mesh, 0, r.req.cx * 16, r.req.sy * 16, r.req.cz * 16);
            uploaded += r.mesh.cubeQuads.size() * kCubeQuadBytes + r.mesh.modelQuads.size() * 4;
            markCut(r.req.region);
            continue;
        }
        if (distance(0, r.req.cx * 16.0, r.req.sy * 16.0, r.req.cz * 16.0, x, y, z) > evict) {
            state = kNotReady;
            continue;
        }
        u32 slot = Renderer::kNoSlot;
        if (!r.mesh.empty()) {
            slot = renderer_.addMesh(r.mesh, 0, r.req.cx * 16, r.req.sy * 16, r.req.cz * 16);
            uploaded += r.mesh.cubeQuads.size() * kCubeQuadBytes + r.mesh.modelQuads.size() * 4;
            if (slot == Renderer::kNoSlot) {
                static bool warned = false;
                if (!warned) LOG("warning: detail geometry budget exhausted; some sections will be missing");
                warned = true;
            }
        }
        state = slot;
        resident_.push_back(r.req);
        markCut(r.req.region);
    }
    pendingUpload_.erase(pendingUpload_.begin(), pendingUpload_.begin() + n);
}

void Terrain::rescan(double x, double y, double z) {
    scanned_ = true;
    scanX_ = x, scanY_ = y, scanZ_ = z;
    const float load = detailRadius_ + kLoadMargin, evict = detailRadius_ + kEvictMargin;

    size_t keep = 0;
    for (size_t i = 0; i < resident_.size(); ++i) {
        const Request& r = resident_[i];
        if (distance(0, r.cx * 16.0, r.sy * 16.0, r.cz * 16.0, x, y, z) > evict) {
            u32& state = detailState(r);
            if (isSlot(state)) renderer_.removeMesh(state);
            state = kNotReady;
            markCut(r.region);
        } else {
            resident_[keep++] = r;
        }
    }
    resident_.resize(keep);

    wanted_.clear();
    const int reach = int(load / 16) + 1;
    const int ccx = int(std::floor(x / 16)), ccz = int(std::floor(z / 16));
    for (int cz = ccz - reach; cz <= ccz + reach; ++cz)
        for (int cx = ccx - reach; cx <= ccx + reach; ++cx) {
            if (distance(0, cx * 16.0, y - 8.0, cz * 16.0, x, y, z) > load) continue;  // column outside the circle
            auto it = regionIndex_.find(regionKey(cx >> 5, cz >> 5));
            if (it == regionIndex_.end()) continue;
            RegionLod& r = regions_[it->second];
            for (int ny = 0; ny < r.ny[0]; ++ny) {
                if (r.at(0, cx & 31, ny, cz & 31) != kNotReady) continue;
                const float d = distance(0, cx * 16.0, (ny + minSy_) * 16.0, cz * 16.0, x, y, z);
                if (d <= load) wanted_.push_back({it->second, cx, ny + minSy_, cz, d});
            }
        }
    std::sort(wanted_.begin(), wanted_.end(), [](const Request& a, const Request& b) { return a.dist > b.dist; });
}

void Terrain::dispatch() {
    std::lock_guard lock(mutex_);
    if (busy_ || (wanted_.empty() && refresh_.empty())) return;
    size_t n = 0;
    while (n < kBatch && !refresh_.empty()) {
        Request& w = refresh_.back();
        auto stamp = stamps_.find(Lighting::sectionKey(w.cx, w.sy, w.cz));
        w.stamp = stamp == stamps_.end() ? 0 : stamp->second;
        queue_.push_back(w);
        refresh_.pop_back();
        ++n;
    }
    while (n < kBatch && !wanted_.empty()) {
        Request& w = wanted_.back();
        if (detailState(w) != kNotReady) {  // meshed by an edit since the scan
            wanted_.pop_back();
            continue;
        }
        ++n;
        auto stamp = stamps_.find(Lighting::sectionKey(w.cx, w.sy, w.cz));
        w.stamp = stamp == stamps_.end() ? 0 : stamp->second;
        detailState(w) = kInFlight;
        queue_.push_back(wanted_.back());
        wanted_.pop_back();
    }
    if (n == 0) return;
    inFlight_ += n;
    busy_ = true;
    cv_.notify_all();
}

void Terrain::collect(const RegionLod& r, int level, int x, int z, double px, double pz, std::vector<u32>& out,
                      double& slack) const {
    const int size = 16 << level, per = 32 >> level;
    const double x0 = r.rx * 512.0 + x * size, z0 = r.rz * 512.0 + z * size;
    const double dx = std::max({x0 - px, 0.0, px - (x0 + size)}), dz = std::max({z0 - pz, 0.0, pz - (z0 + size)});
    const double t = threshold(level);
    if (level > 0) slack = std::min(slack, std::abs(std::sqrt(dx * dx + dz * dz) - t));
    const bool refine = level > 0 && dx * dx + dz * dz < t * t &&
                        (r.refine[level].empty() || r.refine[level][size_t(z) * per + x] < level);
    if (refine && level > 1) {
        for (int i = 0; i < 4; ++i) collect(r, level - 1, x * 2 + (i & 1), z * 2 + (i >> 1), px, pz, out, slack);
        return;
    }
    for (int ny = 0; ny < r.ny[level]; ++ny) {
        const u32 s = r.at(level, x, ny, z);
        if (s == kAbsent) continue;  // nothing in this layer here, nor in its children
        if (refine) {  // level 1 -> full-detail sections, per layer once all four are meshed
            bool ready = true;
            for (int i = 0; i < 4 && ready; ++i) {
                const u32 c = r.at(0, x * 2 + (i & 1), ny, z * 2 + (i >> 1));
                ready = c != kNotReady && c != kInFlight;
            }
            if (ready) {
                for (int i = 0; i < 4; ++i)
                    if (const u32 c = r.at(0, x * 2 + (i & 1), ny, z * 2 + (i >> 1)); isSlot(c)) out.push_back(c);
                continue;
            }
        }
        if (isSlot(s)) out.push_back(s);
    }
}

void Terrain::remesh(const std::vector<u64>& sections) {
    thread_local u16 padded[kPadCells];
    thread_local u8 light[kPadCells];
    for (u64 k : sections) {
        int cx, sy, cz;
        Lighting::unpackSectionKey(k, cx, sy, cz);
        const int ny = sy - minSy_;
        auto it = regionIndex_.find(regionKey(cx >> 5, cz >> 5));
        if (it == regionIndex_.end() || ny < 0 || ny >= regions_[it->second].ny[0]) continue;
        RegionLod& r = regions_[it->second];
        u32& state = r.at(0, cx & 31, ny, cz & 31);
        ++stamps_[k];
        if (!world_.section(cx, sy, cz, nullptr)) continue;  // light-only change in empty space
        const bool resident = isSlot(state) || state == Renderer::kNoSlot;
        if (isSlot(state)) renderer_.removeMesh(state);
        gatherSection(world_, cx, sy, cz, padded, light);
        thread_local ColumnColors colors;
        if (biomeColors_) gatherColumnColors(world_, *biomeColors_, cx, cz, colors);
        SectionMesh mesh;
        meshGrid(padded, light, registry_, true, false, mesh, biomeColors_ ? &colors : nullptr);
        state = mesh.empty() ? Renderer::kNoSlot : renderer_.addMesh(mesh, 0, cx * 16, sy * 16, cz * 16);
        markDirty(it->second);
        markCut(it->second);
        if (!resident) resident_.push_back({it->second, cx, sy, cz, 0.0f, 0});
        // New sections must be reachable from the LOD tree.
        for (int level = 1; level <= kMaxLevel; ++level) {
            u32& a = r.at(level, (cx & 31) >> level, ny, (cz & 31) >> level);
            if (a == kAbsent) a = Renderer::kNoSlot;
        }
    }
}

void Terrain::addChunk(int cx, int cz, const std::vector<u64>& dirtySections) {
    syncRegions();
    auto it = regionIndex_.find(regionKey(cx >> 5, cz >> 5));
    const World::Region* wr = nullptr;
    const World::Column* col = world_.column(cx, cz, &wr);
    if (it == regionIndex_.end() || !col) return;
    RegionLod& r = regions_[size_t(it->second)];
    for (int k = 0; k < col->count; ++k) {
        const int ny = col->minY + k - minSy_;
        if (!wr->sections[col->first + k].paletteSize || ny < 0 || ny >= r.ny[0]) continue;
        u32& state = r.at(0, cx & 31, ny, cz & 31);
        if (state == kAbsent) state = kNotReady;
        for (int level = 1; level <= kMaxLevel; ++level) {
            u32& a = r.at(level, (cx & 31) >> level, ny, (cz & 31) >> level);
            if (a == kAbsent) a = Renderer::kNoSlot;
        }
    }
    markDirty(it->second);
    for (u64 k : dirtySections) {
        int scx, ssy, scz;
        Lighting::unpackSectionKey(k, scx, ssy, scz);
        auto rit = regionIndex_.find(regionKey(scx >> 5, scz >> 5));
        const int ny = ssy - minSy_;
        if ((scx == cx && scz == cz) || rit == regionIndex_.end() || ny < 0 || ny >= r.ny[0]) continue;
        const u32 state = regions_[size_t(rit->second)].at(0, scx & 31, ny, scz & 31);
        if (isSlot(state) || state == Renderer::kNoSlot) refresh_.push_back({rit->second, scx, ssy, scz, 0.0f, 0, true});
    }
    scanned_ = false;  // the next update streams the new sections in
    markCut(it->second);
}

void Terrain::removeChunk(int cx, int cz) {
    auto it = regionIndex_.find(regionKey(cx >> 5, cz >> 5));
    if (it == regionIndex_.end()) return;
    RegionLod& r = regions_[size_t(it->second)];
    for (int ny = 0; ny < r.ny[0]; ++ny) {
        u32& state = r.at(0, cx & 31, ny, cz & 31);
        if (state == kInFlight) ++stamps_[Lighting::sectionKey(cx, ny + minSy_, cz)];  // drop the result
        if (isSlot(state)) renderer_.removeMesh(state);
        state = kAbsent;
    }
    std::erase_if(resident_, [&](const Request& q) { return q.cx == cx && q.cz == cz; });
    markDirty(it->second);
    markCut(it->second);
}

void Terrain::refresh(const std::vector<u64>& sections) {
    for (u64 k : sections) {
        int cx, sy, cz;
        Lighting::unpackSectionKey(k, cx, sy, cz);
        auto it = regionIndex_.find(regionKey(cx >> 5, cz >> 5));
        const int ny = sy - minSy_;
        if (it == regionIndex_.end() || ny < 0 || ny >= regions_[size_t(it->second)].ny[0]) continue;
        const u32 state = regions_[size_t(it->second)].at(0, cx & 31, ny, cz & 31);
        if (isSlot(state) || state == Renderer::kNoSlot) refresh_.push_back({it->second, cx, sy, cz, 0.0f, 0, true});
    }
}

void Terrain::update(double x, double y, double z, bool waitForDetail) {
    static const bool profile = std::getenv("MCPP_TERRAIN_PROFILE") != nullptr;
    Timer tTotal, tPart;
    double partMs[6] = {};
    int farApplied = 0;
    // Rebuild distant LOD for regions that have been quiet for a moment after edits.
    {
        std::lock_guard lock(mutex_);
        if (!coarseDone_.empty()) {
            applyCoarse(coarseDone_.back(), true);
            coarseDone_.pop_back();
        }
        const double now = nowSeconds();
        for (auto it = dirtyRegions_.begin(); it != dirtyRegions_.end();) {
            // After a quiet moment, or regularly while chunks keep arriving.
            if (now - it->second.second > 1.5 || now - it->second.first > 4.0) {
                coarseQueue_.push_back(coarseJob(it->first));
                it = dirtyRegions_.erase(it);
                cv_.notify_all();
            } else {
                ++it;
            }
        }
    }
    partMs[0] = tPart.ms(), tPart = Timer();
    // Far terrain: a few regions per frame (each uploads all of its meshes), then keep the queue current.
    if (farGen_) {
        std::vector<FarResult> ready;
        {
            std::lock_guard lock(mutex_);
            for (int i = 0; i < 4 && !farDone_.empty(); ++i) {
                ready.push_back(std::move(farDone_.back()));
                farDone_.pop_back();
            }
        }
        for (FarResult& f : ready) {
            RegionLod& r = regions_[size_t(f.build.region)];
            r.farQueued = false;
            if (!farActive_ || f.epoch != farEpoch_) continue;
            if (r.farData && r.farData->level <= f.grid->level) continue;
            r.farData = std::move(f.grid);
            applyCoarse(f.build, true);
            ++farApplied;
        }
        partMs[1] = tPart.ms(), tPart = Timer();
        if (farActive_) scheduleFar(x, z);
        partMs[2] = tPart.ms();
    }
    tPart = Timer();
    for (;;) {
        collectResults(x, y, z, waitForDetail);
        const double mx = x - scanX_, my = y - scanY_, mz = z - scanZ_;
        if (!scanned_ || mx * mx + my * my + mz * mz > 64.0) rescan(x, y, z);
        dispatch();
        if (!waitForDetail) break;
        std::unique_lock lock(mutex_);
        if (wanted_.empty() && !busy_ && done_.empty() && pendingUpload_.empty()) break;
        cv_.wait_for(lock, std::chrono::milliseconds(2), [&] { return !done_.empty(); });
    }
    partMs[3] = tPart.ms(), tPart = Timer();

    // The cut depends only on camera position and residency, not on view direction. Incremental: a region is walked
    // again only when its nodes changed (markCut) or the camera moved past its slack (a level boundary).
    {
        const Timer tc;
        cutParts_.resize(regions_.size());
        cutCache_.resize(regions_.size());
        size_t walked = 0, byDirty = 0, byMove = 0;
        double minSlack = 1e30;
        const bool global = cutDirty_;
        for (size_t i = 0; i < regions_.size(); ++i) {
            CutCache& c = cutCache_[i];
            const double mx = x - c.x, mz = z - c.z;
            if (!cutDirty_ && !c.dirty && mx * mx + mz * mz < c.slack * c.slack) continue;
            byDirty += c.dirty, byMove += !c.dirty;
            minSlack = std::min(minSlack, c.slack);
            std::vector<u32>& part = cutParts_[i];
            part.clear();
            double slack = 1e30;
            collect(regions_[i], kMaxLevel, 0, 0, x, z, part, slack);
            c = {x, z, slack, false};
            ++walked;
        }
        cutDirty_ = false;
        if (walked) {
            active_.clear();
            for (const auto& part : cutParts_) active_.insert(active_.end(), part.begin(), part.end());
            renderer_.setActive(active_);
        }
        if (profile && tc.ms() > 1.0)
            LOG("terrain cut: %.2f ms | %zu of %zu regions walked (global %d, dirty %zu, moved %zu, min slack %.2f), %zu active",
                tc.ms(), walked, regions_.size(), int(global), byDirty, byMove, minSlack, active_.size());
    }
    partMs[4] = tPart.ms();
    if (profile && tTotal.ms() > 4.0)
        LOG("terrain update %.1f ms | coarse %.1f, far apply %.1f (%d regions), far schedule %.1f, detail %.1f, cut %.1f",
            tTotal.ms(), partMs[0], partMs[1], farApplied, partMs[2], partMs[3], partMs[4]);

    stats_.detailResident = resident_.size();
    stats_.detailPending = wanted_.size() + inFlight_;
    stats_.active = active_.size();
    if (farGen_) {
        stats_.farRegions = 0;
        for (const RegionLod& r : regions_) stats_.farRegions += r.farData != nullptr;
        std::lock_guard lock(mutex_);
        stats_.farPending = farQueue_.size() + farInFlight_;
    }
}

void Terrain::enableFar(FarGenFn fn, int genMinY, unsigned threads) {
    if (farRadius_ <= 0) return;
    farGen_ = std::move(fn);
    farMinY_ = genMinY;
    for (unsigned i = 0; i < threads; ++i) farThreads_.emplace_back([this] { farMain(); });
}

void Terrain::setFarActive(bool active) {
    if (active == farActive_) return;
    farActive_ = active;
    ++farEpoch_;
    {
        std::lock_guard lock(mutex_);
        for (const FarJob& j : farQueue_) regions_[size_t(j.region)].farQueued = false;
        farQueue_.clear();
    }
    if (!active)
        for (size_t i = 0; i < regions_.size(); ++i)
            if (regions_[i].farData) {
                dropFar(int(i));
                if (regions_[i].world >= 0) markDirty(int(i));  // rebuild from its own chunks only
            }
    farScanTime_ = -1e9;  // rescan right away when it comes back
}

void Terrain::stopFar() {
    {
        std::lock_guard lock(mutex_);
        farStop_ = true;
    }
    farCv_.notify_all();
    for (auto& t : farThreads_) t.join();
    farThreads_.clear();
}

void Terrain::farMain() {
    // Same priority as chunk generation: a far region covers ~1000 chunks for the cost of one, so it must not starve
    // while chunks are generated nearby (it did at LOWEST: the distant view stayed empty during play).
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        FarJob job;
        {
            std::unique_lock lock(mutex_);
            farCv_.wait(lock, [&] { return quit_ || farStop_ || !farQueue_.empty(); });
            if (quit_ || farStop_) return;
            job = std::move(farQueue_.back());
            farQueue_.pop_back();
            ++farInFlight_;
        }
        Timer t;
        auto grid = std::make_shared<FarGrid>();
        const FarGrid* prev = job.coarse.farData.get();
        farGen_(job.rx, job.rz, job.level, prev && prev->level == job.level + 1 ? prev : nullptr, *grid);
        grid->level = job.level;
        FarResult res;
        res.epoch = job.epoch;
        job.coarse.farData = grid;
        buildRegion(job.coarse, res.build, true);
        res.grid = std::move(grid);
        std::lock_guard lock(mutex_);
        stats_.farMs[job.level] += t.ms();
        ++stats_.farJobs[job.level];
        --farInFlight_;
        farDone_.push_back(std::move(res));
    }
}

void Terrain::dropFar(int region) {
    RegionLod& r = regions_[size_t(region)];
    for (int level = 1; level <= kMaxLevel; ++level) {
        for (u32 s : r.nodes[level])
            if (isSlot(s)) renderer_.removeMesh(s);
        std::vector<u32>().swap(r.nodes[level]);  // freed (reads see kAbsent; allocated again on first write)
        std::vector<u8>().swap(r.refine[level]);
    }
    r.farData.reset();
    std::fill(std::begin(r.colLevel), std::end(r.colLevel), u8(0));
    markCut(region);
}

// Queues far regions within the radius, nearest first: every region at the coarsest level, then refinements
// down to the level the cut will show at that distance. Regions the world fully covers need none.
void Terrain::scheduleFar(double x, double z) {
    const double now = nowSeconds();
    const double mx = x - farScanX_, mz = z - farScanZ_;
    if (mx * mx + mz * mz < 128.0 * 128.0 && now - farScanTime_ < 2.0) return;
    farScanX_ = x, farScanZ_ = z, farScanTime_ = now;
    static const bool profile = std::getenv("MCPP_TERRAIN_PROFILE") != nullptr;
    Timer tp;
    double ms[4] = {};
    size_t dropped = 0;
    {
        std::lock_guard lock(mutex_);  // unstarted jobs are re-planned below
        for (const FarJob& j : farQueue_) regions_[size_t(j.region)].farQueued = false;
        farQueue_.clear();
    }
    auto regionDistance = [&](int rx, int rz) {
        const double dx = std::max({rx * 512.0 - x, 0.0, x - (rx * 512.0 + 512.0)});
        const double dz = std::max({rz * 512.0 - z, 0.0, z - (rz * 512.0 + 512.0)});
        return float(std::sqrt(dx * dx + dz * dz));
    };
    ms[0] = tp.ms(), tp = Timer();
    std::vector<FarJob> jobs;
    double regionMs = 0;
    size_t created = 0;
    const int reach = int(farRadius_ / 512.0f) + 1;
    const int crx = int(std::floor(x / 512.0)), crz = int(std::floor(z / 512.0));
    for (int rz = crz - reach; rz <= crz + reach; ++rz)
        for (int rx = crx - reach; rx <= crx + reach; ++rx) {
            const float d = regionDistance(rx, rz);
            if (d > farRadius_) continue;
            const size_t before = regions_.size();
            const Timer ta;
            const int idx = regionAt(rx, rz);
            regionMs += ta.ms(), created += regions_.size() != before;
            RegionLod& r = regions_[size_t(idx)];
            if (r.farQueued) continue;  // being generated
            if (r.world >= 0) {
                const World::Region& wr = *world_.regions()[size_t(r.world)];
                int covered = 0;
                for (const World::Column& c : wr.columns) covered += c.count > 0;
                if (covered == 1024) continue;
            }
            int target = 1;
            while (target < kMaxLevel && d >= threshold(target + 1)) ++target;
            const int have = r.farData ? r.farData->level : kMaxLevel + 1;
            if (have <= target) continue;
            // Far-only regions get the cheap coarsest level first so the horizon fills quickly; regions with world
            // chunks go straight to their level (a coarse far grid would hold back refining their world part).
            // Refine one level at a time (each finer level costs ~8x more), nearest first.
            const bool firstPass = !r.farData && r.world < 0 && target < kMaxLevel;
            const int level = firstPass ? kMaxLevel : r.farData ? std::max(target, have - 1) : target;
            const float priority = r.world >= 0 ? d * 0.5f : firstPass || !r.farData ? d : d * 2.0f;
            jobs.push_back({idx, rx, rz, level, priority, coarseJob(idx), farEpoch_});
        }
    ms[1] = tp.ms(), tp = Timer();
    for (size_t i = 0; i < regions_.size(); ++i) {
        const RegionLod& r = regions_[i];
        if (r.farData && !r.farQueued && r.world < 0 && regionDistance(r.rx, r.rz) > farRadius_ + 1024.0f)
            dropFar(int(i)), ++dropped;
    }
    ms[2] = tp.ms(), tp = Timer();
    std::sort(jobs.begin(), jobs.end(), [](const FarJob& a, const FarJob& b) { return a.priority > b.priority; });
    const size_t jobCount = jobs.size();
    {
        std::lock_guard lock(mutex_);
        for (const FarJob& j : jobs) regions_[size_t(j.region)].farQueued = true;
        farQueue_ = std::move(jobs);
    }
    farCv_.notify_all();
    ms[3] = tp.ms();
    if (profile) {  // memory by kind (MCPP_TERRAIN_PROFILE)
        size_t farBytes = 0, nodeBytes = 0, level0 = 0, farRegions = 0, fine = 0;
        for (const RegionLod& r : regions_) {
            for (int level = 0; level <= kMaxLevel; ++level) nodeBytes += r.nodes[level].capacity() * 4;
            level0 += !r.nodes[0].empty();
            if (r.farData) {
                ++farRegions, fine += r.farData->level <= 2;
                farBytes += r.farData->start.capacity() * 4 + r.farData->runs.capacity() * sizeof(FarGrid::Run) +
                            r.farData->biomes.capacity();
            }
        }
        size_t farDone = 0, coarseDone = 0, doneN = 0, meshBytes = 0;
        {
            std::lock_guard lock(mutex_);
            farDone = farDone_.size(), coarseDone = coarseDone_.size(), doneN = done_.size();
            for (const FarResult& f : farDone_)
                for (const auto& [c, m] : f.build.meshes) meshBytes += m.cubeQuads.capacity() * kCubeQuadBytes + m.modelQuads.capacity() * 4;
        }
        LOG("terrain memory: %zu regions (%zu with level 0), nodes %.0f MB, far data %.0f MB in %zu regions (%zu at level <= 2)"
            " | queues: far done %zu (%.0f MB meshes), coarse done %zu, detail done %zu, pending upload %zu, far queue %zu",
            regions_.size(), level0, nodeBytes / 1048576.0, farBytes / 1048576.0, farRegions, fine, farDone,
            meshBytes / 1048576.0, coarseDone, doneN, pendingUpload_.size(), farQueue_.size());
    }
    if (profile && ms[0] + ms[1] + ms[2] + ms[3] > 4.0)
        LOG("far schedule: lock+clear %.1f, plan %.1f (new regions %zu in %.1f ms, %zu jobs), drop %.1f (%zu regions), "
            "sort+queue %.1f ms", ms[0], ms[1], created, regionMs, jobCount, ms[2], dropped, ms[3]);
}
