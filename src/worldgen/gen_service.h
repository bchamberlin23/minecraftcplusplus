#pragma once
// Background chunk generation, staged like the game's chunk statuses:
//   terrain   - biomes, noise, surface, carvers (parallel workers)
//   decorated - features placed; needs terrain in the chunk's 3x3 neighbourhood and may write
//               into those neighbours, so concurrent decorations keep their 3x3 areas disjoint
//   delivered - every chunk in the 3x3 is decorated, so nothing will change it any more
// Chunks that already exist in the world are used as read-only neighbours.
#include "worldgen/generator.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace wg {

class FeatureSystem;

class GenService {
public:
    // existing(cx, cz): the world already has this chunk; load(cx, cz, out): copy it (any thread).
    using ExistsFn = std::function<bool(int cx, int cz)>;
    using LoadFn = std::function<void(int cx, int cz, GenChunk& out)>;

    GenService(const ChunkGenerator& generator, const FeatureSystem* features, ExistsFn exists, LoadFn load,
               unsigned threads);
    ~GenService();

    // Chunks to deliver, nearest first; replaces the previous request. Work far from the
    // first entry is dropped.
    void setWanted(const std::vector<std::pair<int, int>>& chunks);
    std::vector<std::unique_ptr<GenChunk>> take(size_t max);
    size_t pending() const;
    double averageMs() const;
    std::string debugCounts() const;  // states of the tracked chunks (profiling)

private:
    enum class State { Queued, Generating, Terrain, Decorating, Decorated };
    struct Entry {
        std::unique_ptr<GenChunk> chunk;
        State state = State::Queued;
        bool external = false;  // copied from the world: read-only, never delivered
        bool delivered = false;
        int pins = 0;  // decorations currently reading/writing this chunk
    };
    static u64 key(int cx, int cz) { return (u64(u32(cx)) << 32) | u32(cz); }
    Entry* find(int cx, int cz);
    bool hasTerrain(int cx, int cz);  // generated, or present in the world (loaded on demand)
    bool existsInWorld(int cx, int cz);  // exists_, remembering misses until the next setWanted
    void schedule();                  // under mutex_
    void terrainWorker();
    void decorationWorker();

    const ChunkGenerator& generator_;
    const FeatureSystem* features_;
    ExistsFn exists_;
    LoadFn load_;
    std::vector<std::thread> threads_;
    mutable std::mutex mutex_;
    std::condition_variable terrainCv_, decorateCv_;
    std::unordered_map<u64, Entry> chunks_;
    std::vector<std::pair<int, int>> wanted_;
    size_t scanFrom_ = 0;               // wanted_ before this index are all delivered or in the world
    std::unordered_set<u64> missing_;   // not in the world as of the last setWanted
    std::deque<std::pair<int, int>> terrainQueue_, decorateQueue_;
    std::vector<std::pair<int, int>> decorating_;  // queued or running decorations
    std::vector<std::unique_ptr<GenChunk>> done_;
    double totalMs_ = 0, decorateMs_ = 0;
    size_t decorated_ = 0;
    size_t generated_ = 0;
    bool quit_ = false;
};

}  // namespace wg
