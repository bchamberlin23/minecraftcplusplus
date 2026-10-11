#pragma once
// Jigsaw structures (villages, outposts, ancient cities, trail ruins, trial chambers, bastions, abandoned camps):
// template pools, pool elements, PoolElementStructurePiece and JigsawPlacement.
#include "worldgen/structure_template.h"

#include <atomic>

namespace wg {
class PlacedFeature;
}

namespace wg::st {

struct Junction {  // JigsawJunction
    int sourceX, sourceGroundY, sourceZ, deltaY;
    bool destRigid;
};

class PoolElement {
public:
    enum class Kind : u8 { Empty, Single, Legacy, Feature, List };
    Kind kind = Kind::Empty;
    bool rigid = false;  // projection
    std::string location;
    const Template* tmpl = nullptr;
    ProcessorList processors;
    std::optional<bool> overrideWaterlogging;
    std::shared_ptr<const PlacedFeature> feature;
    std::vector<std::unique_ptr<PoolElement>> elements;  // list elements

    BlockPos size(Rot r) const;
    BBox box(const BlockPos& pos, Rot r) const;
    std::vector<JigsawInfo> shuffledJigsaws(const SB& b, const BlockPos& pos, Rot r, RandomSource& random) const;
    bool place(PlaceCtx& c, const BlockPos& pos, const BlockPos& ref, Rot r, const BBox& chunkBB, RandomSource& random,
               bool applyWaterlogging) const;
    int groundLevelDelta() const { return 1; }
    void setRigid(bool v);
    std::string describe() const;  // for reports
};

struct Pool {
    std::string id;
    std::vector<const PoolElement*> templates;  // expanded by weight
    std::vector<std::unique_ptr<PoolElement>> owned;
    std::string fallback;
    mutable std::atomic<int> maxSize{INT32_MIN};
    int size() const { return int(templates.size()); }
    int getMaxSize() const;
};

class PoolPiece final : public Piece {
public:
    PoolPiece(const PoolElement* e, const BlockPos& pos, int groundDelta, Rot r, const BBox& b, bool waterlogging)
        : Piece("minecraft:jigsaw", 0, b), element(e), position(pos), groundLevelDelta(groundDelta), rot(r),
          applyWaterlogging(waterlogging) {}
    const PoolElement* element;
    BlockPos position;
    int groundLevelDelta;
    Rot rot;
    bool applyWaterlogging;
    std::vector<Junction> junctions;

    void postProcess(PlaceCtx& c, RandomSource& random, const BBox& chunkBB, const BlockPos& ref) override {
        element->place(c, position, ref, rot, chunkBB, random, applyWaterlogging);
    }
    void move(int dx, int dy, int dz) override {
        Piece::move(dx, dy, dz);
        position = position.offset(dx, dy, dz);
    }
};

// Template pools by id ("minecraft:village/plains/houses"), parsed on first use. Thread-safe.
class Pools {
public:
    explicit Pools(const StructureEnv& env) : env_(env) {}
    const Pool* get(const std::string& id) const;  // null when missing

private:
    std::unique_ptr<PoolElement> parseElement(const json& j) const;
    const StructureEnv& env_;
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, std::unique_ptr<Pool>> pools_;
};

}  // namespace wg::st
