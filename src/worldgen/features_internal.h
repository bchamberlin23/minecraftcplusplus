#pragma once
// Private to features.cpp and features_blocks.cpp: the block facts behind BlockFacts (block indices,
// block sets for tags and holder sets, per-state shape/fluid facts, BlockState.canSurvive) and the
// per-thread decoration context.
#include "worldgen/feature_api.h"

#include <atomic>
#include <bitset>
#include <mutex>
#include <unordered_map>

namespace wg {

constexpr int kMaxBlockKinds = 4096;  // distinct block names (not states)
using BlockBits = std::bitset<kMaxBlockKinds>;

// Fluid types in a fact word (FluidState): none, water, flowing water, lava, flowing lava.
enum FluidType : u8 { FluidNone = 0, FluidWater, FluidFlowingWater, FluidLava, FluidFlowingLava };

// Per-state fact bits.
namespace fact {
constexpr u64 Air = 1ull << 0;
constexpr u64 Solid = 1ull << 1;          // BlockState.isSolid()
constexpr u64 Replaceable = 1ull << 2;    // canBeReplaced()
constexpr u64 MotionTag = 1ull << 3;      // #blocks_motion_in_heightmap
constexpr u64 MotionNoLeaves = 1ull << 4; // #blocks_motion_in_heightmap_no_leaves
constexpr u64 SolidRender = 1ull << 5;
constexpr int SturdyShift = 6;            // 6 bits: support face full, per direction
constexpr int CollisionFullShift = 12;    // 6 bits: collision face full
constexpr int OcclusionFullShift = 18;    // 6 bits: face occlusion shape full
constexpr u64 CollisionUpAny = 1ull << 24;
constexpr u64 CenterDown = 1ull << 25;    // support face DOWN covers the centre (SupportType.CENTER)
constexpr u64 CenterUp = 1ull << 26;
constexpr u64 Liquid = 1ull << 27;        // legacy liquid(): water and lava blocks
constexpr u64 CannotReplace = 1ull << 28; // #features_cannot_replace
constexpr u64 BlocksMotion = 1ull << 29;
constexpr int FluidShift = 32;            // 3 bits type, 4 bits amount, 1 bit falling
constexpr u64 FluidMask = 0xFFull << FluidShift;
constexpr u64 Done = 1ull << 63;
}  // namespace fact

struct BlockFacts::Impl {
    Impl(BlockRegistry& blocks, const DataPack& data);

    BlockRegistry& blocks;
    const DataPack& data;

    // Dense index of a block name ("minecraft:stone"), created on first use.
    u16 blockIndex(std::string_view name) const;
    u16 blockIndexLocked(std::string_view name) const;
    u16 blockOf(u16 state) const;  // the state's block index
    const std::string& blockName(u16 index) const;
    bool is(u16 state, u16 blockIndex) const { return blockOf(state) == blockIndex; }
    bool in(u16 state, const BlockBits& set) const { return set.test(blockOf(state)); }

    // A block tag ("minecraft:logs", "#logs"...) as a set, cached. Never null.
    const BlockBits* tag(std::string_view name) const;
    const BlockBits* tagLocked(std::string_view name) const;
    // Block tag members in the game's order (nested tags expanded in place, duplicates dropped).
    std::vector<std::string> tagOrdered(std::string_view kind, std::string_view name) const;
    // A HolderSet<Block> from JSON: "minecraft:x", "#minecraft:tag" or a list of ids.
    BlockBits holderSet(const nlohmann::json& j) const;
    std::vector<std::string> holderList(const nlohmann::json& j) const;  // same, ordered
    bool fluidInTag(u16 state, std::string_view fluidTag) const;

    u64 facts(u16 state) const;
    bool has(u16 state, u64 bits) const { return (facts(state) & bits) != 0; }
    FluidType fluidType(u16 state) const { return FluidType((facts(state) >> fact::FluidShift) & 7); }
    int fluidAmount(u16 state) const { return int((facts(state) >> (fact::FluidShift + 3)) & 15); }
    u64 fluidCode(u16 state) const { return facts(state) & fact::FluidMask; }
    bool fluidIsSource(u16 state) const {
        const FluidType t = fluidType(state);
        return t == FluidWater || t == FluidLava;
    }
    bool isWaterFluid(u16 state) const {  // FluidTags.WATER
        const FluidType t = fluidType(state);
        return t == FluidWater || t == FluidFlowingWater;
    }
    bool sturdy(u16 state, int dir) const { return (facts(state) >> (fact::SturdyShift + dir)) & 1; }
    bool collisionFull(u16 state, int dir) const { return (facts(state) >> (fact::CollisionFullShift + dir)) & 1; }
    bool occlusionFull(u16 state, int dir) const { return (facts(state) >> (fact::OcclusionFullShift + dir)) & 1; }
    // MultifaceBlock.canAttachTo: the neighbour's support or collision face towards us is full.
    bool canAttachTo(u16 neighbour, int dirTowardsNeighbour) const {
        const int opposite = dirTowardsNeighbour ^ 1;
        return sturdy(neighbour, opposite) || collisionFull(neighbour, opposite);
    }

    bool canSurvive(const GenRegion& level, u16 state, const BlockPos& p) const;

    // Frequently tested blocks.
    u16 bAir = 0, bWater = 0, bLava = 0, bBedrock = 0, bSnow = 0, bDirtPath = 0, bVine = 0, bIce = 0,
        bMagma = 0, bSculk = 0, bSculkCatalyst = 0, bMovingPiston = 0, bCactus = 0, bSugarCane = 0;
    const BlockBits *tagFire = nullptr, *tagReplaceable = nullptr;

private:
    u64 compute(u16 state) const;
    int surviveRule(u16 block) const;

    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, u16> blockIds_;
    mutable std::vector<std::string> blockNames_;
    mutable std::unordered_map<std::string, std::unique_ptr<BlockBits>> tags_;
    std::unique_ptr<std::atomic<u16>[]> stateBlock_;  // state -> block index + 1
    std::unique_ptr<std::atomic<u64>[]> stateFacts_;
    std::unique_ptr<std::atomic<i16>[]> survive_;     // block index -> rule + 1
    struct Lists;
    std::shared_ptr<const Lists> lists_;
};

// Per-thread state while a chunk is being decorated.
struct DecorationContext {
    const GenRegion* level = nullptr;
    int lastReadY = 0;  // y of the latest GenRegion::block read (positionless RuleTest::test)
    RandomSource* regionRandom = nullptr;  // WorldGenRegion.getRandom()
};
DecorationContext& decorationContext();

// Full state key ("minecraft:x[a=b,...]") from a block state JSON: "minecraft:x", {"Name","Properties"} or
// {"id","properties"}.
std::string stateKeyOf(const nlohmann::json& j);
std::string_view baseName(std::string_view stateKey);
// Properties of a full state key, in key order.
std::vector<std::pair<std::string_view, std::string_view>> stateProperties(std::string_view stateKey);

}  // namespace wg
