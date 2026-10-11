#include "worldgen/gen_service.h"

#include "worldgen/feature_api.h"
#include "worldgen/structures.h"

#include <windows.h>

#include <algorithm>
#include <array>

namespace wg {

namespace {
constexpr size_t kTerrainQueueTarget = 64;  // keep the workers fed without over-committing
constexpr unsigned kDecorationThreads = 4;
}

GenService::GenService(const ChunkGenerator& generator, const FeatureSystem* features, ExistsFn exists, LoadFn load,
                       unsigned threads)
    : generator_(generator), features_(features), exists_(std::move(exists)), load_(std::move(load)) {
    for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this] { terrainWorker(); });
    for (unsigned i = 0; i < kDecorationThreads; ++i) threads_.emplace_back([this] { decorationWorker(); });
}

GenService::~GenService() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    terrainCv_.notify_all();
    decorateCv_.notify_all();
    for (auto& t : threads_) t.join();
}

GenService::Entry* GenService::find(int cx, int cz) {
    auto it = chunks_.find(key(cx, cz));
    return it == chunks_.end() ? nullptr : &it->second;
}

bool GenService::existsInWorld(int cx, int cz) {
    const u64 k = key(cx, cz);
    if (missing_.count(k)) return false;
    if (exists_(cx, cz)) return true;
    missing_.insert(k);
    return false;
}

bool GenService::hasTerrain(int cx, int cz) {
    if (Entry* e = find(cx, cz)) return e->external || e->state >= State::Terrain;
    if (!existsInWorld(cx, cz)) return false;
    Entry& e = chunks_[key(cx, cz)];  // a saved chunk next to generated ones: read-only neighbour
    e.chunk = std::make_unique<GenChunk>();
    load_(cx, cz, *e.chunk);
    e.external = true;
    e.state = State::Decorated;
    return true;
}

void GenService::setWanted(const std::vector<std::pair<int, int>>& chunks) {
    std::lock_guard lock(mutex_);
    wanted_ = chunks;
    scanFrom_ = 0;
    missing_.clear();
    // Drop finished work far from the area of interest (never anything in progress).
    if (!wanted_.empty()) {
        const auto [ox, oz] = wanted_.front();
        int reach = 0;
        for (const auto& c : wanted_) reach = std::max({reach, std::abs(c.first - ox), std::abs(c.second - oz)});
        reach += 4;
        for (auto it = chunks_.begin(); it != chunks_.end();) {
            const int cx = int(u32(it->first >> 32)), cz = int(u32(it->first));
            const bool distant = std::abs(cx - ox) > reach || std::abs(cz - oz) > reach;
            const bool busy = it->second.state == State::Generating || it->second.state == State::Decorating ||
                              it->second.pins > 0;
            if (distant && !busy && (it->second.state != State::Queued))
                it = chunks_.erase(it);
            else
                ++it;
        }
    }
    terrainQueue_.clear();
    for (auto it = chunks_.begin(); it != chunks_.end();)  // re-queued below in priority order
        it = it->second.state == State::Queued ? chunks_.erase(it) : std::next(it);
    schedule();
}

// Walks the wanted chunks nearest first and advances whatever blocks each one.
// Runs after every finished job, under mutex_: starts from the first unfinished wanted chunk and looks at a
// bounded number of unfinished ones, so it stays cheap however many chunks are wanted.
void GenService::schedule() {
    bool prefixDone = true;
    int pendingSeen = 0;
    for (size_t i = scanFrom_; i < wanted_.size() && pendingSeen < 1000000; ++i) {
        if (terrainQueue_.size() >= kTerrainQueueTarget) break;
        const auto [cx, cz] = wanted_[i];
        Entry* target = find(cx, cz);
        if ((target && (target->delivered || target->external)) || (!target && existsInWorld(cx, cz))) {
            if (prefixDone) scanFrom_ = i + 1;
            continue;
        }
        prefixDone = false;
        ++pendingSeen;
        bool ready = true;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const int ncx = cx + dx, ncz = cz + dz;
                Entry* n = find(ncx, ncz);
                if (n && n->state == State::Decorated) continue;
                if (!n && hasTerrain(ncx, ncz)) continue;  // saved chunk: already decorated
                ready = false;
                if (n && n->state != State::Terrain) continue;  // queued / generating / decorating
                bool neighbourhood = true;                       // can n be decorated yet?
                for (int ez = -1; ez <= 1; ++ez)
                    for (int ex = -1; ex <= 1; ++ex)
                        if (!hasTerrain(ncx + ex, ncz + ez)) {
                            neighbourhood = false;
                            if (!find(ncx + ex, ncz + ez)) {
                                chunks_[key(ncx + ex, ncz + ez)].state = State::Queued;
                                terrainQueue_.push_back({ncx + ex, ncz + ez});
                            }
                        }
                n = find(ncx, ncz);
                // Decorations within two chunks of each other would share chunks: one at a time there.
                const bool overlaps = std::any_of(decorating_.begin(), decorating_.end(), [&](const auto& d) {
                    return std::abs(d.first - ncx) <= 2 && std::abs(d.second - ncz) <= 2;
                });
                if (neighbourhood && n && n->state == State::Terrain && !overlaps) {
                    decorating_.push_back({ncx, ncz});
                    n->state = State::Decorating;
                    decorateQueue_.push_back({ncx, ncz});
                }
            }
        const StructureSystem* structures = features_ ? features_->structures() : nullptr;
        if (ready && structures) {  // post-processing reads the neighbours: wait until no decoration writes them
            std::array<GenChunk*, 9> region{};
            for (int dz = -1; dz <= 1 && ready; ++dz)
                for (int dx = -1; dx <= 1 && ready; ++dx) {
                    Entry* n = find(cx + dx, cz + dz);
                    ready = n && n->chunk && n->pins == 0;
                    if (ready) region[size_t((dz + 1) * 3 + (dx + 1))] = n->chunk.get();
                }
            if (ready) {
                auto out = std::make_unique<GenChunk>(*region[4]);
                region[4] = out.get();
                structures->postProcess(region);  // LevelChunk.postProcessGeneration, on the delivered copy
                done_.push_back(std::move(out));
                find(cx, cz)->delivered = true;
            }
        } else if (ready) {
            Entry& e = chunks_[key(cx, cz)];
            done_.push_back(std::make_unique<GenChunk>(*e.chunk));
            e.delivered = true;
        }
    }
    terrainCv_.notify_all();
    decorateCv_.notify_all();
}

std::vector<std::unique_ptr<GenChunk>> GenService::take(size_t max) {
    std::lock_guard lock(mutex_);
    std::vector<std::unique_ptr<GenChunk>> out;
    while (!done_.empty() && out.size() < max) {
        out.push_back(std::move(done_.back()));
        done_.pop_back();
    }
    return out;
}

std::string GenService::debugCounts() const {
    std::lock_guard lock(mutex_);
    size_t states[5] = {}, delivered = 0, external = 0;
    for (const auto& [k, e] : chunks_) ++states[int(e.state)], delivered += e.delivered, external += e.external;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "queued %zu, generating %zu, terrain %zu, decorating %zu, decorated %zu (delivered %zu, "
                  "external %zu) | queues %zu terrain, %zu decorate, %zu decorating, %zu done | scan from %zu of %zu | "
                  "decoration %.1f ms avg",
                  states[0], states[1], states[2], states[3], states[4], delivered, external, terrainQueue_.size(),
                  decorateQueue_.size(), decorating_.size(), done_.size(), scanFrom_, wanted_.size(),
                  decorated_ ? decorateMs_ / double(decorated_) : 0.0);
    return buf;
}

size_t GenService::pending() const {
    std::lock_guard lock(mutex_);
    size_t n = terrainQueue_.size() + decorateQueue_.size();
    for (const auto& [k, e] : chunks_) n += e.state == State::Generating || e.state == State::Decorating;
    return n;
}

double GenService::averageMs() const {
    std::lock_guard lock(mutex_);
    return generated_ ? totalMs_ / double(generated_) : 0.0;
}

void GenService::terrainWorker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    DensityContext ctx;
    for (;;) {
        std::pair<int, int> c;
        {
            std::unique_lock lock(mutex_);
            terrainCv_.wait(lock, [&] { return quit_ || !terrainQueue_.empty(); });
            if (quit_) return;
            c = terrainQueue_.front();
            terrainQueue_.pop_front();
            Entry* e = find(c.first, c.second);
            if (!e || e->state != State::Queued) continue;
            e->state = State::Generating;
        }
        Timer t;
        auto chunk = std::make_unique<GenChunk>();
        const StructureSystem* structures = features_ ? features_->structures() : nullptr;
        const auto beardifier = structures ? structures->beardifier(c.first, c.second) : nullptr;  // terrain adaptation
        generator_.generate(c.first, c.second, *chunk, ctx, nullptr, beardifier.get());
        std::lock_guard lock(mutex_);
        totalMs_ += t.ms();
        ++generated_;
        Entry& e = chunks_[key(c.first, c.second)];
        e.chunk = std::move(chunk);
        e.state = State::Terrain;
        schedule();
    }
}

void GenService::decorationWorker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    DensityContext ctx;
    std::array<GenChunk*, 9> region{};
    int regionCx = 0, regionCz = 0;
    // Biomes come from the chunks being decorated (generated with them); sampling the climate noise for each of
    // the thousands of lookups features make costs ~40x more. Outside the 3x3 chunks: the biome source.
    const NoiseBiomeFn noiseBiome = [&](int qx, int qy, int qz) -> u16 {
        const int dx = (qx >> 2) - regionCx, dz = (qz >> 2) - regionCz;
        if (dx >= -1 && dx <= 1 && dz >= -1 && dz <= 1)
            if (const GenChunk* c = region[size_t((dz + 1) * 3 + (dx + 1))]; c && !c->biomes.empty())
                return c->biome(qx & 3, qy - (c->minY >> 2), qz & 3);
        return generator_.noiseBiome(ctx, qx, qy, qz);
    };
    for (;;) {
        region.fill(nullptr);
        std::pair<int, int> c;
        {
            std::unique_lock lock(mutex_);
            decorateCv_.wait(lock, [&] { return quit_ || !decorateQueue_.empty(); });
            if (quit_) return;
            c = decorateQueue_.front();
            decorateQueue_.pop_front();
            regionCx = c.first, regionCz = c.second;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    Entry* n = find(c.first + dx, c.second + dz);
                    region[size_t((dz + 1) * 3 + (dx + 1))] = n ? n->chunk.get() : nullptr;
                    if (n) ++n->pins;
                }
        }
        Timer t;
        if (features_ && region[4]) features_->decorate(region, generator_.zoom(), noiseBiome);
        std::lock_guard lock(mutex_);
        decorateMs_ += t.ms(), ++decorated_;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
                if (Entry* n = find(c.first + dx, c.second + dz)) --n->pins;
        if (Entry* e = find(c.first, c.second)) e->state = State::Decorated;
        std::erase(decorating_, c);
        schedule();
    }
}

}  // namespace wg
