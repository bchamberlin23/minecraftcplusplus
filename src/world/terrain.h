#pragma once
#include "core/common.h"
#include "mesh/mesher.h"
#include "world/biome_colors.h"
#include "world/far_columns.h"
#include "world/world.h"

#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

class BlockRegistry;
class Renderer;

struct TerrainStats {
    size_t coarseNodes = 0;     // LOD >= 1 nodes with geometry (always resident)
    size_t detailResident = 0;  // full-detail sections meshed around the camera
    size_t detailPending = 0;   // queued or being meshed
    size_t active = 0;          // nodes in the current LOD cut
    size_t farRegions = 0;      // regions with generated far terrain
    size_t farPending = 0;      // far regions queued or being generated
    double farMs[6] = {};       // total generate + build time per far level
    size_t farJobs[6] = {};
};

// Level-of-detail terrain. Level 0 is a full-detail 16^3 section; a level-k node covers 16 * 2^k blocks
// horizontally and 16 blocks vertically, with columns 2^k blocks wide and exact block heights (coarsening
// is horizontal only, so distant terrain keeps its true shape). Each 16-block layer is a quadtree.
// Levels >= 1 are built for the whole loaded world up front; level 0 streams in around the camera. A cut
// picks, for every point in the world, exactly one resident node, refining a node only when all of its
// children are ready, so streaming never opens holes. All node state lives in dense per-region arrays (a
// level-5 node spans exactly one region), so the cut is pure array walks.
class Terrain {
public:
    static constexpr int kMaxLevel = 5;

    Terrain(const World& world, const BlockRegistry& registry, Renderer& renderer, float detailRadius);
    ~Terrain();

    // Far terrain: coarse data for chunk columns the world does not have, generated per region at LOD
    // resolution (level >= 1) out to `radius` blocks on background threads, coarse levels first.
    // fn(rx, rz, level, coarser, out) fills `out` (FarColumns at that level, with biomes); coarser is the
    // region's data at level + 1 when there is one.
    // reserveFar (before buildCoarse) sizes GPU storage for it; enableFar starts it.
    using FarGenFn = std::function<void(int rx, int rz, int level, const FarColumns* coarser, FarColumns& out)>;
    // How far each coarser level is pushed out (level k >= 2 from detailRadius * quality * 2^(k-1)).
    void setLodQuality(float quality) { lodQuality_ = std::max(quality, 0.5f), cutDirty_ = true; }
    // Biome tints for meshes built from now on (null: the fixed default tints).
    void setBiomeColors(const BiomeColors* colors) { biomeColors_ = colors; }
    void reserveFar(float radius) { farRadius_ = radius; }
    void setDetailRadius(float blocks) { detailRadius_ = blocks, cutDirty_ = true; }  // full-detail radius (options)
    void enableFar(FarGenFn fn, int genMinY, unsigned threads);
    void stopFar();  // joins the far threads (before whatever fn uses goes away)
    void stopBackground();  // stop accepting work on exit; destructors join executing jobs
    // Far terrain shows the overworld: off (and dropped) while the player is in another dimension.
    void setFarActive(bool active);

    bool buildCoarse();
    void prime(double x, double y, double z);  // prepare a small playable centre, then stream detail outward
    void update(double x, double y, double z, bool waitForDetail);
    // Re-meshes full-detail sections right away after an edit (keys from Lighting::sectionKey).
    void remesh(const std::vector<u64>& sections);
    // A chunk was added to the world at runtime (generated): stream its sections, refresh the
    // neighbouring sections listed in dirtySections and rebuild the region's coarse levels.
    void addChunk(int cx, int cz, const std::vector<u64>& dirtySections);
    // A chunk was dropped from the world: frees its full-detail meshes; coarse levels rebuild.
    void removeChunk(int cx, int cz);
    // Whole dimension unload: free all meshes in one pass and invalidate background results.
    void clearChunks();
    // Re-meshes resident full-detail sections in the background (e.g. after light changes).
    void refresh(const std::vector<u64>& sections);
    const TerrainStats& stats() const { return stats_; }

private:
    // Node states besides a renderer slot (Renderer::kNoSlot = has blocks but no geometry).
    static constexpr u32 kAbsent = 0xFFFFFFFE;    // no blocks
    static constexpr u32 kNotReady = 0xFFFFFFFD;  // level 0: not meshed
    static constexpr u32 kInFlight = 0xFFFFFFFC;  // level 0: being meshed
    static bool isSlot(u32 s) { return s < kInFlight; }

    using FarGrid = FarColumns;
    struct RegionLod {
        int rx = 0, rz = 0;
        int world = -1;  // index in World::regions(); -1: far terrain only
        std::shared_ptr<const FarGrid> farData;
        bool farQueued = false;
        // Finest LOD with data per chunk column (0: world data or nothing, else the far level) and, per node of
        // each level, the maximum over its columns: a node refines only below that.
        u8 colLevel[1024] = {};
        std::vector<u8> refine[kMaxLevel + 1];
        int ny[kMaxLevel + 1] = {};
        std::vector<u32> nodes[kMaxLevel + 1];
        // Level 0 (full-detail sections) is allocated on first write: far-only regions never need it, and
        // allocating it for every region the far terrain reaches cost ~30 us each (100 ms after a teleport).
        u32& at(int level, int x, int y, int z) {
            const int per = 32 >> level;
            if (nodes[level].empty()) nodes[level].assign(size_t(per) * per * ny[level], kAbsent);
            return nodes[level][(size_t(y) * per + z) * per + x];
        }
        u32 at(int level, int x, int y, int z) const {
            const int per = 32 >> level;
            if (nodes[level].empty()) return kAbsent;
            return nodes[level][(size_t(y) * per + z) * per + x];
        }
    };
    struct Request {
        int region, cx, sy, cz;
        float dist;
        u32 stamp = 0;         // edit stamp at dispatch; stale results are dropped
        bool refresh = false;  // re-mesh of a resident section: swap the mesh in place
        u64 epoch = 0;         // dimension at dispatch
    };
    struct Result {
        Request req;
        SectionMesh mesh;
    };

    float distance(int level, double x0, double y0, double z0, double px, double py, double pz) const;
    // Distance below which a level-k node splits into level k-1.
    float threshold(int level) const {
        // Full detail ends at the render distance; the coarser steps keep their quality even when it is short.
        return level <= 1 ? detailRadius_ : std::max(detailRadius_, 512.0f) * lodQuality_ * float(1 << (level - 1));
    }
    float lodQuality_ = 2.0f;
    u32& detailState(const Request& r) { return regions_[r.region].at(0, r.cx & 31, r.sy - minSy_, r.cz & 31); }
    struct CoarseBuild {
        u64 epoch = 0;
        int region = 0;
        const World::Region* source = nullptr;  // what the build saw, to drop stale results
        int farLevel = 0;
        u8 colLevel[1024] = {};
        std::vector<u32> nodes[kMaxLevel + 1];  // levels >= 1: kAbsent or Renderer::kNoSlot
        std::vector<std::pair<std::array<int, 4>, SectionMesh>> meshes;  // (level, x, y, z)
    };
    struct CoarseJob {
        int region;
        const World::Region* source;  // null: far terrain only
        int ny[kMaxLevel + 1];
        std::shared_ptr<const FarGrid> farData;
        int rx = 0, rz = 0;
        u64 epoch = 0;
    };
    struct FarJob {
        int region, rx, rz, level;
        float priority;  // lower first
        CoarseJob coarse;
    };
    struct FarResult {
        std::shared_ptr<const FarGrid> grid;
        CoarseBuild build;
    };
    CoarseJob coarseJob(int region) const;
    // Layers [layerFrom, layerTo) (default: all); node states of other layers stay kAbsent.
    void buildRegion(const CoarseJob& job, CoarseBuild& out, bool lockWorld, int layerFrom = 0, int layerTo = 1 << 30) const;
    void syncRegions();
    int regionAt(int rx, int rz);  // creates the LOD state for a region on first use
    void scheduleFar(double x, double z);
    void dropFar(int region);
    void farMain();
    void applyCoarse(CoarseBuild& build, bool replace);
    void collectResults(double x, double y, double z, bool unlimited);
    void rescan(double x, double y, double z);
    void dispatch();
    void workerMain();
    // The cut of one region: a 2D quadtree walk by horizontal distance (all 16-block layers share it), taking
    // every layer's node at each chosen position.
    // Appends the region's nodes for a camera at (px, pz); slack = how far the camera can move horizontally before
    // any node's level would change (distance to the nearest refinement threshold).
    void collect(const RegionLod& r, int level, int x, int z, double px, double pz, std::vector<u32>& out,
                 double& slack) const;
    void markCut(int region) {
        if (size_t(region) < cutCache_.size()) cutCache_[size_t(region)].dirty = true;
    }

    const World& world_;
    const BlockRegistry& registry_;
    Renderer& renderer_;
    float detailRadius_;
    float loadRadiusLimit_ = 1e30f;  // only limited during prime()
    int minSy_ = 0, worldMinY_ = 0, heightBlocks_ = 0;
    u16 water_ = 0;
    u16 solidFill_ = 0;  // opaque cube used for unknown space beside a region

    std::deque<RegionLod> regions_;             // world regions and far-only regions; deque: stable while growing
    size_t syncedWorldRegions_ = 0;
    std::unordered_map<u64, int> regionIndex_;  // (rx, rz) -> index
    std::vector<Request> resident_;             // level-0 sections currently meshed
    std::vector<Request> wanted_;               // sorted far -> near; dispatched from the back
    std::vector<Request> refresh_;              // resident sections to re-mesh (neighbours of new chunks)
    std::unordered_set<u64> refreshQueued_;     // coalesce repeated light/edge updates before dispatch
    std::vector<Result> pendingUpload_;         // finished meshes waiting for upload budget
    double scanX_ = 0, scanY_ = 0, scanZ_ = 0;
    bool scanned_ = false;
    bool cutDirty_ = true;  // every region (markCut: one region)
    struct CutCache {
        double x = 0, z = 0, slack = -1;  // camera at the last walk; valid while it stays within slack
        bool dirty = true;
    };
    std::vector<CutCache> cutCache_;
    size_t inFlight_ = 0;
    u64 epoch_ = 0;  // main thread only; copied into jobs while dispatching
    std::unordered_map<u64, u32> stamps_;  // section -> edit count
    std::vector<u32> active_;
    std::vector<std::vector<u32>> cutParts_;  // per region: its part of active_
    TerrainStats stats_;

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Request> queue_;
    std::vector<Result> done_;
    std::vector<CoarseJob> coarseQueue_;  // regions to rebuild after edits
    std::vector<CoarseBuild> coarseDone_;
    std::unordered_map<int, std::pair<double, double>> dirtyRegions_;  // region -> (first, last) change time
    void markDirty(int region);
    bool busy_ = false, quit_ = false, farStop_ = false;

    FarGenFn farGen_;
    const BiomeColors* biomeColors_ = nullptr;
    float farRadius_ = 0;
    int farMinY_ = 0;
    std::vector<std::thread> farThreads_;
    std::condition_variable farCv_;
    std::vector<FarJob> farQueue_;  // sorted: next job at the back
    std::vector<FarResult> farDone_;
    size_t farInFlight_ = 0;
    double farScanX_ = 0, farScanZ_ = 0, farScanTime_ = -1e9;
    bool farActive_ = true;
};
