// Jigsaw structures: StructureTemplatePool, the pool element types (single, legacy_single, feature, list, empty),
// PoolElementStructurePiece, JigsawPlacement.addPieces (with the game's free-space shapes, placement priorities,
// rigid / terrain matching projection, the village expansion hack and pool aliases) and JigsawStructure.
#include "worldgen/jigsaw.h"

#include "worldgen/structures.h"

#include <algorithm>
#include <climits>
#include <deque>
#include <map>

namespace wg::st {

// ---- pool elements -------------------------------------------------------------------------------------------

void PoolElement::setRigid(bool v) {
    rigid = v;
    for (auto& e : elements) e->setRigid(v);
}

BlockPos PoolElement::size(Rot r) const {
    switch (kind) {
    case Kind::Single:
    case Kind::Legacy: return tmpl->sizeFor(r);
    case Kind::List: {
        BlockPos s{0, 0, 0};
        for (const auto& e : elements) {
            const BlockPos es = e->size(r);
            s = {std::max(s.x, es.x), std::max(s.y, es.y), std::max(s.z, es.z)};
        }
        return s;
    }
    default: return {0, 0, 0};
    }
}

BBox PoolElement::box(const BlockPos& pos, Rot r) const {
    switch (kind) {
    case Kind::Single:
    case Kind::Legacy: return templateBox(pos, r, {0, 0, 0}, Mir::None, tmpl->size);
    case Kind::Feature: return BBox(pos.x, pos.y, pos.z, pos.x, pos.y, pos.z);
    case Kind::List: {
        std::optional<BBox> b;
        for (const auto& e : elements) {
            if (e->kind == Kind::Empty) continue;
            const BBox eb = e->box(pos, r);
            if (b) b->encapsulate(eb);
            else b = eb;
        }
        return b.value_or(BBox(pos));
    }
    default: return BBox(pos);
    }
}

std::vector<JigsawInfo> PoolElement::shuffledJigsaws(const SB& b, const BlockPos& pos, Rot r, RandomSource& random) const {
    switch (kind) {
    case Kind::Single:
    case Kind::Legacy: {
        std::vector<JigsawInfo> j = templateJigsaws(*tmpl, pos, r, b);
        for (size_t i = j.size(); i > 1; --i) std::swap(j[i - 1], j[size_t(random.nextInt(int(i)))]);
        std::stable_sort(j.begin(), j.end(), [](const JigsawInfo& a, const JigsawInfo& c) { return a.selectionPriority > c.selectionPriority; });
        return j;
    }
    case Kind::Feature: {
        JigsawInfo j;
        j.pos = pos;
        j.state = b.with(b.jigsaw, "orientation", "down_south");
        j.rollable = true;
        j.name.clear();  // null
        j.pool = "minecraft:empty";
        j.target = "minecraft:empty";
        j.front = Down, j.top = South;
        return {j};
    }
    case Kind::List: return elements.front()->shuffledJigsaws(b, pos, r, random);
    default: return {};
    }
}

bool PoolElement::place(PlaceCtx& c, const BlockPos& pos, const BlockPos& ref, Rot r, const BBox& chunkBB, RandomSource& random,
                        bool applyWaterlogging) const {
    switch (kind) {
    case Kind::Empty: return true;
    case Kind::Feature: {
        const FeatureEnv* fe = c.env.featureEnv();
        return fe && feature ? fe->placePlaced(*feature, c.level, random, pos) : false;
    }
    case Kind::List:
        for (const auto& e : elements)
            if (!e->place(c, pos, ref, r, chunkBB, random, applyWaterlogging)) return false;
        return true;
    default: break;
    }
    // SinglePoolElement.getSettings
    PlaceSettings s;
    s.box = chunkBB;
    s.rotation = r;
    s.knownShape = true;
    s.ignoreEntities = false;
    s.finalizeEntities = true;
    s.applyWaterlogging = overrideWaterlogging.value_or(applyWaterlogging);
    static const std::shared_ptr<const Processor> ignoreStructure = blockIgnore(c.env, {"minecraft:structure_block"});
    static const std::shared_ptr<const Processor> ignoreStructureAndAir =
        blockIgnore(c.env, {"minecraft:air", "minecraft:structure_block"});
    if (kind == Kind::Single) s.processors.push_back(ignoreStructure);
    s.processors.push_back(jigsawReplacement());
    for (const auto& p : processors) s.processors.push_back(p);
    if (!rigid) {
        static const std::shared_ptr<const Processor> grav = gravity(HType::WorldSurfaceWG, -1);
        s.processors.push_back(grav);
    }
    if (kind == Kind::Legacy) s.processors.push_back(ignoreStructureAndAir);
    return placeTemplate(c, *tmpl, pos, ref, s, random);
}

std::string PoolElement::describe() const {
    switch (kind) {
    case Kind::Single: return "single " + location;
    case Kind::Legacy: return "legacy " + location;
    case Kind::Feature: return "feature";
    case Kind::List: return "list[" + (elements.empty() ? std::string() : elements.front()->describe()) + "...]";
    default: return "empty";
    }
}

int Pool::getMaxSize() const {
    int v = maxSize.load(std::memory_order_relaxed);
    if (v != INT32_MIN) return v;
    int m = 0;
    bool any = false;
    for (const PoolElement* e : templates) {
        if (e->kind == PoolElement::Kind::Empty) continue;
        m = std::max(m, e->box({0, 0, 0}, Rot::None).ySpan());
        any = true;
    }
    v = any ? m : 0;
    maxSize.store(v, std::memory_order_relaxed);
    return v;
}

std::unique_ptr<PoolElement> Pools::parseElement(const json& j) const {
    auto e = std::make_unique<PoolElement>();
    const std::string type = DataPack::normalize(j.value("element_type", std::string()));
    e->rigid = j.value("projection", std::string("rigid")) == "rigid";
    if (type == "single_pool_element" || type == "legacy_single_pool_element") {
        e->kind = type == "single_pool_element" ? PoolElement::Kind::Single : PoolElement::Kind::Legacy;
        e->location = j.value("location", std::string());
        e->tmpl = env_.templates->get(e->location);
        if (j.contains("processors")) e->processors = parseProcessors(env_, j["processors"]);
        if (auto it = j.find("override_liquid_settings"); it != j.end() && it->is_string())
            e->overrideWaterlogging = it->get<std::string>() == "apply_waterlogging";
    } else if (type == "feature_pool_element") {
        e->kind = PoolElement::Kind::Feature;
        if (const FeatureEnv* fe = env_.featureEnv()) e->feature = fe->placed(j.at("feature"));
    } else if (type == "list_pool_element") {
        e->kind = PoolElement::Kind::List;
        for (const json& c : j.at("elements")) e->elements.push_back(parseElement(c));
        e->setRigid(e->rigid);
    } else {
        e->kind = PoolElement::Kind::Empty;
        e->rigid = false;  // EmptyPoolElement: TERRAIN_MATCHING
    }
    return e;
}

const Pool* Pools::get(const std::string& rawId) const {
    const std::string id = rawId.find(':') == std::string::npos ? "minecraft:" + rawId : rawId;
    std::lock_guard lock(mutex_);
    if (auto it = pools_.find(id); it != pools_.end()) return it->second.get();
    const json* j = env_.data.worldgen("template_pool", id);
    if (!j) {
        pools_[id] = nullptr;
        return nullptr;
    }
    auto p = std::make_unique<Pool>();
    p->id = id;
    p->fallback = j->value("fallback", std::string("minecraft:empty"));
    if (p->fallback.find(':') == std::string::npos) p->fallback = "minecraft:" + p->fallback;
    static const PoolElement kEmpty;
    for (const json& ej : j->value("elements", json::array())) {
        std::unique_ptr<PoolElement> e = parseElement(ej.at("element"));
        const int weight = ej.value("weight", 1);
        const PoolElement* ptr = e->kind == PoolElement::Kind::Empty ? &kEmpty : e.get();
        for (int i = 0; i < weight; ++i) p->templates.push_back(ptr);
        if (e->kind != PoolElement::Kind::Empty) p->owned.push_back(std::move(e));
    }
    return (pools_[id] = std::move(p)).get();
}

// ---- JigsawPlacement --------------------------------------------------------------------------------------------

namespace {

// Free space for children (the game's VoxelShape "free"): a base box minus the boxes of the pieces placed in it.
// Every box is block aligned, so "the target box (deflated by 0.25) is inside the free shape" is: inside the base box
// and overlapping none of the removed boxes.
struct Free {
    int x0, y0, z0, x1, y1, z1;  // half-open base box
    std::vector<BBox> removed;
    bool fits(const BBox& t) const {
        if (t.minX < x0 || t.minY < y0 || t.minZ < z0 || t.maxX + 1 > x1 || t.maxY + 1 > y1 || t.maxZ + 1 > z1) return false;
        for (const BBox& r : removed)
            if (r.intersects(t)) return false;
        return true;
    }
};

struct PieceState {
    PoolPiece* piece;
    std::shared_ptr<Free> free;
    int depth;
};

// SequencedPriorityIterator: highest priority first, FIFO within a priority.
class PriorityQueue {
public:
    void add(PieceState s, int priority) { queues_[priority].push_back(std::move(s)); }
    bool next(PieceState& out) {
        for (auto it = queues_.rbegin(); it != queues_.rend(); ++it)
            if (!it->second.empty()) {
                out = std::move(it->second.front());
                it->second.pop_front();
                return true;
            }
        return false;
    }

private:
    std::map<int, std::deque<PieceState>> queues_;
};

using AliasMap = std::unordered_map<std::string, std::string>;

struct Placer {
    const StructureEnv& env;
    const Pools& pools;
    GenCtx& ctx;
    int maxDepth;
    bool expansionHack;
    bool waterlogging;
    const AliasMap& aliases;
    std::vector<std::unique_ptr<PoolPiece>>& pieces;
    RandomSource& random;
    PriorityQueue placing;

    std::string lookup(const std::string& pool) const {
        auto it = aliases.find(pool);
        return it == aliases.end() ? pool : it->second;
    }

    void tryPlacingChildren(PoolPiece& source, const std::shared_ptr<Free>& contextFree, int depth) {
        const PoolElement& sourceElement = *source.element;
        const BlockPos sourceBoxPos = source.position;
        const Rot sourceRot = source.rot;
        const bool sourceRigid = sourceElement.rigid;
        std::shared_ptr<Free> sourceFree;
        const BBox sourceBB = source.box;
        const int sourceBoxY = sourceBB.minY;
        const SB& b = env.b;
        for (const JigsawInfo& sj : sourceElement.shuffledJigsaws(b, sourceBoxPos, sourceRot, random)) {
            const Direction sourceDir = sj.front;
            const BlockPos sourceJigsawPos = sj.pos;
            const BlockPos targetJigsawPos = relative(sourceJigsawPos, sourceDir);
            const int sourceJigsawLocalY = sourceJigsawPos.y - sourceBoxY;
            int sourceJigsawBaseHeight = INT32_MIN;
            const std::string poolName = lookup(sj.pool);
            const Pool* targetPool = pools.get(poolName);
            if (!targetPool) continue;
            if (targetPool->size() == 0 && poolName != "minecraft:empty") continue;
            const Pool* fallback = pools.get(targetPool->fallback);
            if (!fallback) continue;
            if (fallback->size() == 0 && targetPool->fallback != "minecraft:empty") continue;
            const bool attachInside = sourceBB.isInside(targetJigsawPos);
            std::shared_ptr<Free> childrenFree;
            if (attachInside) {
                if (!sourceFree) {
                    sourceFree = std::make_shared<Free>(Free{sourceBB.minX, sourceBB.minY, sourceBB.minZ, sourceBB.maxX + 1,
                                                             sourceBB.maxY + 1, sourceBB.maxZ + 1, {}});
                }
                childrenFree = sourceFree;
            } else {
                childrenFree = contextFree;
            }
            std::vector<const PoolElement*> targets;
            auto shuffled = [&](const Pool& p) {
                std::vector<const PoolElement*> v = p.templates;
                for (size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[size_t(random.nextInt(int(i)))]);
                return v;
            };
            if (depth != maxDepth) targets = shuffled(*targetPool);
            const std::vector<const PoolElement*> fb = shuffled(*fallback);
            targets.insert(targets.end(), fb.begin(), fb.end());
            const int placementPriority = sj.placementPriority;
            bool placed = false;
            for (const PoolElement* targetElement : targets) {
                if (targetElement->kind == PoolElement::Kind::Empty) break;
                for (Rot targetRot : shuffledRotations(random)) {
                    const std::vector<JigsawInfo> targetJigsaws = targetElement->shuffledJigsaws(b, {0, 0, 0}, targetRot, random);
                    const BBox hackBox = targetElement->box({0, 0, 0}, targetRot);
                    int expandTo = 0;
                    if (expansionHack && hackBox.ySpan() <= 16) {
                        for (const JigsawInfo& tj : targetJigsaws) {
                            int v = 0;
                            if (hackBox.isInside(relative(tj.pos, tj.front))) {
                                const Pool* childPool = pools.get(lookup(tj.pool));
                                const Pool* childFallback = childPool ? pools.get(childPool->fallback) : nullptr;
                                const int a = childPool ? childPool->getMaxSize() : 0;
                                const int c = childFallback ? childFallback->getMaxSize() : 0;
                                v = std::max(a, c);
                            }
                            expandTo = std::max(expandTo, v);
                        }
                    }
                    for (const JigsawInfo& tj : targetJigsaws) {
                        // JigsawBlock.canAttach
                        if (sj.front != opposite(tj.front)) continue;
                        if (!sj.rollable && sj.top != tj.top) continue;
                        if (!tj.name.empty() && sj.target != tj.name) continue;
                        const BlockPos targetLocal = tj.pos;
                        const BlockPos rawTargetBoxPos{targetJigsawPos.x - targetLocal.x, targetJigsawPos.y - targetLocal.y,
                                                       targetJigsawPos.z - targetLocal.z};
                        const BBox rawTargetBB = targetElement->box(rawTargetBoxPos, targetRot);
                        const int rawTargetY = rawTargetBB.minY;
                        const bool targetRigid = targetElement->rigid;
                        const int targetJigsawLocalY = targetLocal.y;
                        const int deltaY = sourceJigsawLocalY - targetJigsawLocalY + stepY(sj.front);
                        int targetBoxY;
                        if (sourceRigid && targetRigid) {
                            targetBoxY = sourceBoxY + deltaY;
                        } else {
                            if (sourceJigsawBaseHeight == INT32_MIN)
                                sourceJigsawBaseHeight = ctx.firstFreeHeight(sourceJigsawPos.x, sourceJigsawPos.z, HType::WorldSurfaceWG);
                            targetBoxY = sourceJigsawBaseHeight - targetJigsawLocalY;
                        }
                        const int yOffset = targetBoxY - rawTargetY;
                        BBox targetBB = rawTargetBB.moved(0, yOffset, 0);
                        const BlockPos targetBoxPos = rawTargetBoxPos.offset(0, yOffset, 0);
                        if (expandTo > 0) {
                            const int newSize = std::max(expandTo + 1, targetBB.maxY - targetBB.minY);
                            targetBB.encapsulate(BlockPos{targetBB.minX, targetBB.minY + newSize, targetBB.minZ});
                        }
                        if (!childrenFree->fits(targetBB)) continue;
                        childrenFree->removed.push_back(targetBB);
                        const int sourceGround = source.groundLevelDelta;
                        const int targetGround = targetRigid ? sourceGround - deltaY : targetElement->groundLevelDelta();
                        auto targetPiece = std::make_unique<PoolPiece>(targetElement, targetBoxPos, targetGround, targetRot,
                                                                       targetBB, waterlogging);
                        int junctionY;
                        if (sourceRigid) {
                            junctionY = sourceBoxY + sourceJigsawLocalY;
                        } else if (targetRigid) {
                            junctionY = targetBoxY + targetJigsawLocalY;
                        } else {
                            if (sourceJigsawBaseHeight == INT32_MIN)
                                sourceJigsawBaseHeight = ctx.firstFreeHeight(sourceJigsawPos.x, sourceJigsawPos.z, HType::WorldSurfaceWG);
                            junctionY = sourceJigsawBaseHeight + deltaY / 2;
                        }
                        source.junctions.push_back({targetJigsawPos.x, junctionY - sourceJigsawLocalY + sourceGround,
                                                    targetJigsawPos.z, deltaY, targetRigid});
                        targetPiece->junctions.push_back({sourceJigsawPos.x, junctionY - targetJigsawLocalY + targetGround,
                                                          sourceJigsawPos.z, -deltaY, sourceRigid});
                        PoolPiece* raw = targetPiece.get();
                        pieces.push_back(std::move(targetPiece));
                        if (depth + 1 <= maxDepth) placing.add({raw, childrenFree, depth + 1}, placementPriority);
                        placed = true;
                        break;
                    }
                    if (placed) break;
                }
                if (placed) break;
            }
        }
    }
};

// PoolAliasBinding.forEachResolved over a JSON binding.
void resolveAlias(const json& j, RandomSource& r, AliasMap& out) {
    const std::string type = DataPack::normalize(j.value("type", std::string()));
    auto ns = [](std::string s) { return s.find(':') == std::string::npos ? "minecraft:" + s : s; };
    auto weighted = [&](const json& list) -> const json* {
        int total = 0;
        for (const json& e : list) total += e.value("weight", 1);
        if (total <= 0) return nullptr;
        int sel = r.nextInt(total);
        for (const json& e : list) {
            sel -= e.value("weight", 1);
            if (sel < 0) return &e;
        }
        return nullptr;
    };
    if (type == "direct") {
        out.emplace(ns(j.value("alias", std::string())), ns(j.value("target", std::string())));
    } else if (type == "random") {
        if (const json* e = weighted(j.at("targets"))) out.emplace(ns(j.value("alias", std::string())), ns(e->at("data").get<std::string>()));
    } else if (type == "random_group") {
        if (const json* e = weighted(j.at("groups")))
            for (const json& b : e->at("data")) resolveAlias(b, r, out);
    }
}

class JigsawStructure final : public Structure {
public:
    JigsawStructure(const json& def, StructureEnv& env) : env_(env), pools_(*env.pools) {
        startPool_ = def.value("start_pool", std::string());
        if (startPool_.find(':') == std::string::npos) startPool_ = "minecraft:" + startPool_;
        if (def.contains("start_jigsaw_name")) {
            startJigsaw_ = def["start_jigsaw_name"].get<std::string>();
            if (startJigsaw_->find(':') == std::string::npos) startJigsaw_ = "minecraft:" + *startJigsaw_;
        }
        maxDepth_ = def.value("size", 0);
        if (const FeatureEnv* fe = env.featureEnv()) startHeight_ = fe->heightProvider(def.at("start_height"));
        expansionHack_ = def.value("use_expansion_hack", false);
        if (def.contains("project_start_to_heightmap")) project_ = htypeFromName(def["project_start_to_heightmap"].get<std::string>());
        const json& md = def.at("max_distance_from_center");
        if (md.is_number()) {
            maxH_ = maxV_ = md.get<int>();
        } else {
            maxH_ = md.value("horizontal", 80);
            maxV_ = md.value("vertical", 4064);  // DimensionType.Y_SIZE
        }
        aliases_ = def.value("pool_aliases", json::array());
        if (auto it = def.find("dimension_padding"); it != def.end()) {
            if (it->is_number()) padBottom_ = padTop_ = it->get<int>();
            else padBottom_ = it->value("bottom", 0), padTop_ = it->value("top", 0);
        }
        waterlogging_ = def.value("liquid_settings", std::string("apply_waterlogging")) == "apply_waterlogging";
    }

    std::optional<Stub> findGenerationPoint(GenCtx& c) const override {
        const int height = startHeight_ ? startHeight_->sample(c.random, c.env.genMinY(), c.env.genDepth()) : 0;
        const BlockPos startPos{c.minBlockX(), height, c.minBlockZ()};
        // PoolAliasLookup.create
        auto aliases = std::make_shared<AliasMap>();
        if (!aliases_.empty()) {
            LegacyRandom base(c.seed);
            const auto factory = base.forkPositional();
            const auto r = factory->at(startPos.x, startPos.y, startPos.z);
            for (const json& binding : aliases_) resolveAlias(binding, *r, *aliases);
        }
        auto lookup = [&](const std::string& p) {
            auto it = aliases->find(p);
            return it == aliases->end() ? p : it->second;
        };
        RandomSource& random = c.random;
        const Rot centerRot = randomRotation(random);
        const Pool* centerPool = pools_.get(lookup(startPool_));
        if (!centerPool) centerPool = pools_.get(startPool_);
        if (!centerPool || centerPool->templates.empty()) return std::nullopt;
        const PoolElement* centerElement = centerPool->templates[size_t(random.nextInt(centerPool->size()))];
        if (centerElement->kind == PoolElement::Kind::Empty) return std::nullopt;
        BlockPos anchored = startPos;
        if (startJigsaw_) {
            bool found = false;
            for (const JigsawInfo& j : centerElement->shuffledJigsaws(env_.b, startPos, centerRot, random))
                if (j.name == *startJigsaw_) {
                    anchored = j.pos;
                    found = true;
                    break;
                }
            if (!found) {
                LOG("structures: no starting jigsaw %s found in start pool %s", startJigsaw_->c_str(), startPool_.c_str());
                return std::nullopt;
            }
        }
        const BlockPos localAnchor{anchored.x - startPos.x, anchored.y - startPos.y, anchored.z - startPos.z};
        const BlockPos adjusted{startPos.x - localAnchor.x, startPos.y - localAnchor.y, startPos.z - localAnchor.z};
        auto centerPiece = std::make_shared<std::unique_ptr<PoolPiece>>(std::make_unique<PoolPiece>(
            centerElement, adjusted, centerElement->groundLevelDelta(), centerRot, centerElement->box(adjusted, centerRot), waterlogging_));
        const BBox box = (*centerPiece)->box;
        const int centerX = (box.maxX + box.minX) / 2, centerZ = (box.maxZ + box.minZ) / 2;
        int bottomY;
        if (!project_) {
            bottomY = adjusted.y;
        } else {
            if (!c.couldStructureExistInColumn(centerX, centerZ, c.minY(), c.maxY() - 1)) return std::nullopt;
            bottomY = startPos.y + c.firstFreeHeight(centerX, centerZ, *project_);
        }
        const int oldGroundY = box.minY + (*centerPiece)->groundLevelDelta;
        (*centerPiece)->move(0, bottomY - oldGroundY, 0);
        if (padBottom_ || padTop_) {
            const BBox& cb = (*centerPiece)->box;
            if (cb.minY < c.minY() + padBottom_ || cb.maxY > c.maxY() - 1 - padTop_) return std::nullopt;
        }
        const int centerY = bottomY + localAnchor.y;
        GenCtx* ctx = &c;
        const JigsawStructure* self = this;
        // The game's `box` is the centre piece's own BoundingBox object: the free space excludes it where it was moved to.
        const BBox movedBox = (*centerPiece)->box;
        return Stub{{centerX, centerY, centerZ}, [self, ctx, centerPiece, aliases, centerX, centerY, centerZ, box = movedBox](PiecesBuilder& builder) {
                        std::vector<std::unique_ptr<PoolPiece>> pieces;
                        PoolPiece* center = centerPiece->get();
                        pieces.push_back(std::move(*centerPiece));
                        if (self->maxDepth_ > 0) {
                            const int minY = ctx->minY(), maxYIncl = ctx->maxY() - 1;
                            auto free = std::make_shared<Free>();
                            free->x0 = centerX - self->maxH_, free->x1 = centerX + self->maxH_ + 1;
                            free->z0 = centerZ - self->maxH_, free->z1 = centerZ + self->maxH_ + 1;
                            free->y0 = std::max(centerY - self->maxV_, minY + self->padBottom_);
                            free->y1 = std::min(centerY + self->maxV_ + 1, maxYIncl + 1 - self->padTop_);
                            free->removed.push_back(box);
                            Placer placer{self->env_, self->pools_, *ctx, self->maxDepth_, self->expansionHack_, self->waterlogging_,
                                          *aliases, pieces, ctx->random, {}};
                            placer.tryPlacingChildren(*center, free, 0);
                            PieceState st;
                            while (placer.placing.next(st)) placer.tryPlacingChildren(*st.piece, st.free, st.depth);
                        }
                        for (auto& p : pieces) builder.add(std::move(p));
                    }};
    }

private:
    StructureEnv& env_;
    const Pools& pools_;
    std::string startPool_;
    std::optional<std::string> startJigsaw_;
    int maxDepth_ = 0;
    std::unique_ptr<HeightProvider> startHeight_;
    bool expansionHack_ = false;
    std::optional<HType> project_;
    int maxH_ = 80, maxV_ = 80;
    json aliases_;
    int padBottom_ = 0, padTop_ = 0;
    bool waterlogging_ = true;
};

}  // namespace

void registerJigsaw() {
    registerStructureType("minecraft:jigsaw", [](const json& def, StructureEnv& env) -> std::unique_ptr<Structure> {
        return std::make_unique<JigsawStructure>(def, env);
    });
}

}  // namespace wg::st
