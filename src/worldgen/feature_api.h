#pragma once
// Shared API for biome decoration (the game's features): the writable 3x3 chunk region, the
// decoration random, and the building blocks configured features are made of (block state
// providers, block predicates, value providers). Implemented across features.cpp (framework,
// placement, simple features), features_trees.cpp and features_misc.cpp.
#include "worldgen/climate.h"
#include "worldgen/density.h"
#include "worldgen/gen_chunk.h"

#include <array>
#include <functional>
#include <memory>

class BlockRegistry;
class GameReports;

namespace wg {

struct BlockPos {
    int x = 0, y = 0, z = 0;
    BlockPos offset(int dx, int dy, int dz) const { return {x + dx, y + dy, z + dz}; }
    bool operator==(const BlockPos&) const = default;
};

// Directions in the game's order: down, up, north, south, west, east.
enum Direction : u8 { Down = 0, Up, North, South, West, East };
constexpr int kDirOffset[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};

enum class Heightmap : u8 { WorldSurfaceWG, OceanFloorWG, WorldSurface, MotionBlocking, MotionBlockingNoLeaves, OceanFloor };

class GenRegion;

// Block facts features need, cached per block state id. Thread-safe.
class BlockFacts {
public:
    BlockFacts(BlockRegistry& blocks, const DataPack& data);
    bool inTag(u16 id, std::string_view blockTag) const;  // "minecraft:logs", "logs", "#minecraft:logs"
    bool isAir(u16 id) const;                             // air, cave_air, void_air
    bool isFluid(u16 id) const;                           // has a fluid state (incl. waterlogged)
    bool isWater(u16 id) const;                           // water source/flow or waterlogged
    bool isSourceWater(u16 id) const;
    bool blocksMotion(u16 id) const;                      // BlockState.blocksMotion()
    bool isSolid(u16 id) const;                           // BlockState.isSolid() (the legacy solidity flag)
    bool canBeReplacedByFeature(u16 id) const;            // not in #features_cannot_replace
    // More BlockState facts (features.cpp).
    bool is(u16 id, std::string_view blockName) const;   // a state of that block ("minecraft:stone" or "stone")
    bool canBeReplaced(u16 id) const;                     // BlockState.canBeReplaced() (#replaceable)
    bool isSolidRender(u16 id) const;
    bool isFaceSturdy(u16 id, Direction face) const;     // isFaceSturdy(.., face, SupportType.FULL)
    bool isCollisionFaceFull(u16 id, Direction face) const;
    // BlockState.canSurvive at p (vegetation, saplings, cactus, sugar cane, ...; true for other blocks).
    bool canSurvive(const GenRegion& level, u16 id, const BlockPos& p) const;
    BlockRegistry& blocks() const { return blocks_; }

    struct Impl;
    const Impl& impl() const { return *impl_; }

private:
    std::shared_ptr<Impl> impl_;
    BlockRegistry& blocks_;
};

// The game's WorldGenRegion around a chunk being decorated: chunks[(dz + 1) * 3 + (dx + 1)],
// centre = chunks[4]. Reads outside the 3x3 area return air; writes outside are refused.
class GenRegion {
public:
    GenRegion(const std::array<GenChunk*, 9>& chunks, const BlockFacts& facts, const BiomeZoom& zoom,
              const NoiseBiomeFn& noiseBiome, i64 seed, int seaLevel);
    u16 block(int x, int y, int z) const;
    u16 block(const BlockPos& p) const { return block(p.x, p.y, p.z); }
    bool setBlock(const BlockPos& p, u16 id);  // keeps all heightmaps up to date
    bool isEmpty(const BlockPos& p) const;     // air
    int height(Heightmap type, int x, int z) const;  // getHeight: one above the top matching block
    u16 biome(const BlockPos& p) const;
    bool inside(const BlockPos& p) const;      // within the 3x3 chunks and the height range
    int minY() const;
    int maxY() const;  // exclusive
    int seaLevel() const { return seaLevel_; }
    i64 seed() const { return seed_; }
    const BlockFacts& facts() const { return facts_; }
    GenChunk* chunk(int cx, int cz) const;

private:
    std::array<GenChunk*, 9> chunks_;
    const BlockFacts& facts_;
    const BiomeZoom& zoom_;
    const NoiseBiomeFn& noiseBiome_;
    i64 seed_;
    int seaLevel_;
};

// The decoration random: the game's WorldgenRandom wrapping a XoroshiroRandomSource (bit-based
// nextInt/nextDouble on top of xoroshiro bits), with the decoration/feature seed helpers.
class WorldgenRandom final : public RandomSource {
public:
    explicit WorldgenRandom(std::unique_ptr<RandomSource> inner);
    i64 setDecorationSeed(i64 worldSeed, int blockX, int blockZ);
    void setFeatureSeed(i64 decorationSeed, int index, int step);
    std::unique_ptr<RandomSource> fork() override;
    std::unique_ptr<PositionalRandomFactory> forkPositional() override;
    void setSeed(i64 seed) override;
    i32 nextInt() override;
    i32 nextInt(i32 bound) override;
    i64 nextLong() override;
    bool nextBoolean() override;
    float nextFloat() override;
    double nextDouble() override;
    void consumeCount(int rounds) override;

private:
    i32 next(int bits);
    std::unique_ptr<RandomSource> inner_;
};

// ---- configured building blocks (parsed once from JSON, then evaluated) ------------------
class IntProvider {
public:
    virtual ~IntProvider() = default;
    virtual i32 sample(RandomSource& r) const = 0;
    virtual i32 minValue() const = 0;
    virtual i32 maxValue() const = 0;
};
class FloatProvider {
public:
    virtual ~FloatProvider() = default;
    virtual float sample(RandomSource& r) const = 0;
};
class HeightProvider {
public:
    virtual ~HeightProvider() = default;
    virtual i32 sample(RandomSource& r, int minY, int height) const = 0;  // world gen height context
};
class StateProvider {  // BlockStateProvider
public:
    static constexpr u16 kNone = 0xFFFF;  // getOptionalState's null
    virtual ~StateProvider() = default;
    // Reads the level being decorated on this thread when the provider needs it (rule_based...).
    virtual u16 state(RandomSource& r, const BlockPos& p) const = 0;
    // Level-aware forms (BlockStateProvider.getState / getOptionalState); optionalState may return kNone.
    virtual u16 state(const GenRegion& level, RandomSource& r, const BlockPos& p) const { (void)level; return state(r, p); }
    virtual u16 optionalState(const GenRegion& level, RandomSource& r, const BlockPos& p) const { return state(level, r, p); }
};
class BlockPredicate {
public:
    virtual ~BlockPredicate() = default;
    virtual bool test(const GenRegion& level, const BlockPos& p) const = 0;
};
class RuleTest {  // ore targets: tag_match, block_match, blockstate_match, random_block_match...
public:
    virtual ~RuleTest() = default;
    // Prefer the positional form: height_match tests the position's y. Without one, the y of this
    // thread's latest GenRegion::block read is used.
    virtual bool test(u16 state, RandomSource& r) const = 0;
    virtual bool test(u16 state, const BlockPos& p, RandomSource& r) const { (void)p; return test(state, r); }
};

class ConfiguredFeature {
public:
    virtual ~ConfiguredFeature() = default;
    virtual bool place(GenRegion& level, RandomSource& random, const BlockPos& origin) const = 0;
};
class PlacedFeature;  // configured feature + placement modifiers (features.cpp)

// Everything a feature factory needs to resolve its JSON configuration.
class FeatureEnv {
public:
    virtual ~FeatureEnv() = default;
    virtual const NoiseState& noise() const = 0;
    virtual const DataPack& data() const = 0;
    virtual const BlockFacts& facts() const = 0;
    virtual BlockRegistry& blocks() const = 0;
    virtual u16 state(const nlohmann::json& stateJson) const = 0;  // {"Name","Properties"} or string
    virtual u16 defaultState(std::string_view blockName) const = 0;
    virtual std::unique_ptr<IntProvider> intProvider(const nlohmann::json& j) const = 0;
    virtual std::unique_ptr<FloatProvider> floatProvider(const nlohmann::json& j) const = 0;
    virtual std::unique_ptr<HeightProvider> heightProvider(const nlohmann::json& j) const = 0;
    virtual std::unique_ptr<StateProvider> stateProvider(const nlohmann::json& j) const = 0;
    virtual std::unique_ptr<BlockPredicate> blockPredicate(const nlohmann::json& j) const = 0;
    virtual std::unique_ptr<RuleTest> ruleTest(const nlohmann::json& j) const = 0;
    // A configured feature reference or inline object (shared per registry id).
    virtual std::shared_ptr<const ConfiguredFeature> configured(const nlohmann::json& j) const = 0;
    // A placed feature reference or inline object; place() applies its placement modifiers.
    virtual std::shared_ptr<const PlacedFeature> placed(const nlohmann::json& j) const = 0;
    virtual bool placePlaced(const PlacedFeature& f, GenRegion& level, RandomSource& r, const BlockPos& origin) const = 0;
};

// Feature types ("minecraft:tree", "minecraft:ore", ...) -> factory from their "config" JSON.
using FeatureFactory = std::function<std::unique_ptr<ConfiguredFeature>(const nlohmann::json& config, const FeatureEnv& env)>;
class FeatureTypes {
public:
    void add(std::string type, FeatureFactory factory) { factories_.emplace_back(std::move(type), std::move(factory)); }
    const FeatureFactory* find(std::string_view type) const;

private:
    std::vector<std::pair<std::string, FeatureFactory>> factories_;
};
void registerCoreFeatures(FeatureTypes& types);  // features.cpp
void registerTreeFeatures(FeatureTypes& types);  // features_trees.cpp
void registerMiscFeatures(FeatureTypes& types);  // features_misc.cpp

class StructureSystem;  // structures.h

// Biome decoration for a whole dimension (the game's ChunkGenerator.applyBiomeDecoration).
class FeatureSystem {
public:
    // possibleBiomes: the biome source's biomes in its order (FeatureSorter input order).
    FeatureSystem(const NoiseState& noise, BlockRegistry& blocks, BiomeTable& biomes,
                  const std::vector<u16>& possibleBiomes);
    ~FeatureSystem();
    // Decorates chunks[4] (its 8 neighbours must have terrain; features may write into them).
    void decorate(const std::array<GenChunk*, 9>& chunks, const BiomeZoom& zoom, const NoiseBiomeFn& noiseBiome) const;
    // Structures placed in every decoration step before the step's features (null: none).
    void setStructures(const StructureSystem* structures);
    const StructureSystem* structures() const;
    // For structures: the parsing environment (rule tests, providers, placed features) and Biome.getTemperature.
    const FeatureEnv& env() const;
    float temperature(u16 biome, const BlockPos& p) const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace wg
