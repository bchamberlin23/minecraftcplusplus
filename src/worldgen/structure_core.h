#pragma once
// Private to the structure code (structures.cpp, structure_template.cpp, jigsaw.cpp, pieces_*.cpp): the game's
// structure building blocks - BoundingBox, Rotation/Mirror, block state transforms, StructurePiece with its
// placement helpers, Structure and StructureStart, and the generation / placement contexts.
#include "worldgen/feature_api.h"
#include "worldgen/features_internal.h"
#include "worldgen/generator.h"
#include "worldgen/rng.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace wg {
class FeatureSystem;
}

namespace wg::st {

using json = nlohmann::json;

// ---- directions, rotation, mirror -------------------------------------------------------------------
enum class Rot : u8 { None = 0, CW90 = 1, CW180 = 2, CCW90 = 3 };  // Rotation, in the game's order
enum class Mir : u8 { None = 0, LeftRight = 1, FrontBack = 2 };     // Mirror

constexpr Direction kPlaneHorizontal[4] = {North, East, South, West};  // Direction.Plane.HORIZONTAL
constexpr const char* kDirNames[6] = {"down", "up", "north", "south", "west", "east"};

inline Direction opposite(Direction d) {
    constexpr Direction o[6] = {Up, Down, South, North, East, West};
    return o[d];
}
inline Direction clockWise(Direction d) {  // around Y
    switch (d) {
    case North: return East;
    case East: return South;
    case South: return West;
    case West: return North;
    default: return d;
    }
}
inline Direction counterClockWise(Direction d) {
    switch (d) {
    case North: return West;
    case West: return South;
    case South: return East;
    case East: return North;
    default: return d;
    }
}
inline bool horizontal(Direction d) { return d >= North; }
inline bool axisX(Direction d) { return d == West || d == East; }
inline bool axisZ(Direction d) { return d == North || d == South; }
inline int stepX(Direction d) { return kDirOffset[d][0]; }
inline int stepY(Direction d) { return kDirOffset[d][1]; }
inline int stepZ(Direction d) { return kDirOffset[d][2]; }
inline BlockPos relative(const BlockPos& p, Direction d, int n = 1) {
    return {p.x + stepX(d) * n, p.y + stepY(d) * n, p.z + stepZ(d) * n};
}
inline Direction from2D(int v) {  // Direction.from2DDataValue
    constexpr Direction t[4] = {South, West, North, East};
    return t[std::abs(v % 4)];
}
inline int to2D(Direction d) {
    switch (d) {
    case South: return 0;
    case West: return 1;
    case North: return 2;
    case East: return 3;
    default: return -1;
    }
}
std::optional<Direction> dirFromName(std::string_view n);
inline Direction randomHorizontal(RandomSource& r) { return kPlaneHorizontal[r.nextInt(4)]; }

inline Direction rotate(Rot r, Direction d) {
    if (!horizontal(d)) return d;
    switch (r) {
    case Rot::CW90: return clockWise(d);
    case Rot::CW180: return opposite(d);
    case Rot::CCW90: return counterClockWise(d);
    default: return d;
    }
}
inline Direction mirror(Mir m, Direction d) {
    if (m == Mir::FrontBack && axisX(d)) return opposite(d);
    if (m == Mir::LeftRight && axisZ(d)) return opposite(d);
    return d;
}
inline Rot mirrorRotation(Mir m, Direction d) {  // Mirror.getRotation
    return (m == Mir::LeftRight && axisZ(d)) || (m == Mir::FrontBack && axisX(d)) ? Rot::CW180 : Rot::None;
}
inline Rot rotated(Rot a, Rot b) { return Rot((int(a) + int(b)) & 3); }  // Rotation.getRotated
inline Rot randomRotation(RandomSource& r) { return Rot(r.nextInt(4)); }   // Rotation.getRandom
std::vector<Rot> shuffledRotations(RandomSource& r);                       // Rotation.getShuffled
Rot rotFromName(std::string_view n);  // "NONE", "CLOCKWISE_90", "clockwise_90", "180"...
const char* rotName(Rot r);           // "NONE", "CLOCKWISE_90", "CLOCKWISE_180", "COUNTERCLOCKWISE_90"

// ---- BoundingBox -----------------------------------------------------------------------------------
struct BBox {
    int minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
    BBox() = default;
    BBox(int x0, int y0, int z0, int x1, int y1, int z1)
        : minX(std::min(x0, x1)), minY(std::min(y0, y1)), minZ(std::min(z0, z1)), maxX(std::max(x0, x1)),
          maxY(std::max(y0, y1)), maxZ(std::max(z0, z1)) {}
    explicit BBox(const BlockPos& p) : BBox(p.x, p.y, p.z, p.x, p.y, p.z) {}
    static BBox fromCorners(const BlockPos& a, const BlockPos& b) { return BBox(a.x, a.y, a.z, b.x, b.y, b.z); }
    static BBox infinite() { return {INT32_MIN, INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX, INT32_MAX}; }
    // BoundingBox.orientBox
    static BBox orient(int fx, int fy, int fz, int ox, int oy, int oz, int w, int h, int d, Direction dir) {
        switch (dir) {
        case North: return {fx + ox, fy + oy, fz - d + 1 + oz, fx + w - 1 + ox, fy + h - 1 + oy, fz + oz};
        case West: return {fx - d + 1 + oz, fy + oy, fz + ox, fx + oz, fy + h - 1 + oy, fz + w - 1 + ox};
        case East: return {fx + oz, fy + oy, fz + ox, fx + d - 1 + oz, fy + h - 1 + oy, fz + w - 1 + ox};
        default: return {fx + ox, fy + oy, fz + oz, fx + w - 1 + ox, fy + h - 1 + oy, fz + d - 1 + oz};
        }
    }
    bool intersects(const BBox& o) const {
        return maxX >= o.minX && minX <= o.maxX && maxZ >= o.minZ && minZ <= o.maxZ && maxY >= o.minY && minY <= o.maxY;
    }
    bool intersectsXZ(int x0, int z0, int x1, int z1) const { return maxX >= x0 && minX <= x1 && maxZ >= z0 && minZ <= z1; }
    bool isInside(int x, int y, int z) const {
        return x >= minX && x <= maxX && z >= minZ && z <= maxZ && y >= minY && y <= maxY;
    }
    bool isInside(const BlockPos& p) const { return isInside(p.x, p.y, p.z); }
    BBox& encapsulate(const BBox& o) {
        minX = std::min(minX, o.minX), minY = std::min(minY, o.minY), minZ = std::min(minZ, o.minZ);
        maxX = std::max(maxX, o.maxX), maxY = std::max(maxY, o.maxY), maxZ = std::max(maxZ, o.maxZ);
        return *this;
    }
    BBox& encapsulate(const BlockPos& p) { return encapsulate(BBox(p)); }
    BBox& move(int dx, int dy, int dz) {
        minX += dx, minY += dy, minZ += dz, maxX += dx, maxY += dy, maxZ += dz;
        return *this;
    }
    BBox moved(int dx, int dy, int dz) const { return BBox(*this).move(dx, dy, dz); }
    BBox inflatedBy(int a) const { return {minX - a, minY - a, minZ - a, maxX + a, maxY + a, maxZ + a}; }
    BBox inflatedBy(int ax, int ay, int az) const { return {minX - ax, minY - ay, minZ - az, maxX + ax, maxY + ay, maxZ + az}; }
    int xSpan() const { return maxX - minX + 1; }
    int ySpan() const { return maxY - minY + 1; }
    int zSpan() const { return maxZ - minZ + 1; }
    BlockPos center() const { return {minX + (maxX - minX + 1) / 2, minY + (maxY - minY + 1) / 2, minZ + (maxZ - minZ + 1) / 2}; }
    bool operator==(const BBox&) const = default;
};

// ---- block states -----------------------------------------------------------------------------------
// Block state helpers for structure code over BlockRegistry / BlockFacts. Thread-safe.
class SB {
public:
    SB(BlockRegistry& registry, const BlockFacts& facts);
    BlockRegistry& reg;
    const BlockFacts& facts;
    const BlockFacts::Impl& f;

    u16 def(std::string_view block) const;      // the block's default state ("minecraft:stone" or "stone")
    u16 key(std::string_view stateKey) const;   // a full or partial state key ("minecraft:oak_stairs[facing=east]")
    u16 state(const json& j) const;             // {"Name","Properties"} / string
    u16 with(u16 s, std::string_view k, std::string_view v) const;
    u16 with(u16 s, std::string_view k, bool v) const { return with(s, k, v ? std::string_view("true") : std::string_view("false")); }
    u16 with(u16 s, std::string_view k, int v) const { return with(s, k, std::string_view(std::to_string(v))); }
    u16 with(u16 s, std::string_view k, Direction d) const { return with(s, k, std::string_view(kDirNames[d])); }
    u16 with(u16 s, std::string_view k, const char* v) const { return with(s, k, std::string_view(v)); }
    std::string_view prop(u16 s, std::string_view k) const;
    bool has(u16 s, std::string_view k) const { return !prop(s, k).empty(); }
    std::optional<Direction> dir(u16 s, std::string_view k = "facing") const { return dirFromName(prop(s, k)); }
    const std::string& name(u16 s) const;
    std::string_view blockName(u16 s) const;  // "minecraft:stone"

    u16 blockIndex(std::string_view block) const { return f.blockIndex(block); }
    bool is(u16 s, std::string_view block) const { return f.is(s, f.blockIndex(block)); }
    bool isIdx(u16 s, u16 blockIdx) const { return f.is(s, blockIdx); }
    bool tag(u16 s, std::string_view t) const { return f.in(s, *f.tag(t)); }
    bool isAir(u16 s) const { return f.has(s, fact::Air); }
    bool liquid(u16 s) const { return f.has(s, fact::Liquid); }  // BlockState.liquid(): water / lava blocks
    bool solidRender(u16 s) const { return f.has(s, fact::SolidRender); }
    bool solid(u16 s) const { return f.has(s, fact::Solid); }
    bool blocksMotion(u16 s) const { return f.has(s, fact::BlocksMotion); }
    bool fluidEmpty(u16 s) const { return f.fluidType(s) == FluidNone; }
    bool fluidSource(u16 s) const { return f.fluidIsSource(s); }
    bool waterFluid(u16 s) const { return f.isWaterFluid(s); }
    bool fullCollision(u16 s) const;  // isCollisionShapeFullBlock
    bool fullOutline(u16 s) const;    // Block.isShapeFullBlock(getShape)

    u16 rotate(u16 s, Rot r) const;   // BlockState.rotate
    u16 mirror(u16 s, Mir m) const;   // BlockState.mirror
    u16 withPropertiesOf(u16 target, u16 from) const;  // Block.withPropertiesOf (target: a default state)

    u16 air, caveAir, water, lava, barrier, jigsaw, structureBlock, structureVoid;

private:
    u16 transform(u16 s, int op) const;
    u16 computeRotate(u16 s, Rot r) const;
    u16 computeMirror(u16 s, Mir m) const;
    mutable std::shared_mutex mutex_;
    mutable std::unordered_map<std::string, u16> defs_;
    std::unique_ptr<std::atomic<u32>[]> transforms_;  // state * 5 + op -> result + 1 (ops: rot 1..3, mirror 1..2)
};

// Loot containers: RandomizableContainer block entities (their template NBT gets a LootTableSeed).
bool isRandomizableContainer(std::string_view blockName);
// The block entity type id of a block that has one ("minecraft:spawner" -> "minecraft:mob_spawner").
std::string blockEntityId(std::string_view blockName);

// The block helpers of the structure environment working on this thread (set while starts are generated and pieces
// are placed), for the transliterated piece code: B("minecraft:stone").with("facing", North).
const SB& sb();
void setThreadSB(const SB* b);
struct St {
    u16 id;
    operator u16() const { return id; }
    St with(std::string_view k, std::string_view v) const { return {sb().with(id, k, v)}; }
    St with(std::string_view k, const char* v) const { return {sb().with(id, k, std::string_view(v))}; }
    St with(std::string_view k, bool v) const { return {sb().with(id, k, v)}; }
    St with(std::string_view k, int v) const { return {sb().with(id, k, v)}; }
    St with(std::string_view k, Direction d) const { return {sb().with(id, k, d)}; }
    St rotate(Rot r) const { return {sb().rotate(id, r)}; }
    St mirror(Mir m) const { return {sb().mirror(id, m)}; }
    bool is(std::string_view block) const { return sb().is(id, block); }
    bool isAir() const { return sb().isAir(id); }
};
inline St B(std::string_view block) { return {sb().def(block)}; }
inline St S(u16 id) { return {id}; }

// ---- environment --------------------------------------------------------------------------------------
class TemplateManager;
class StructureEnv;
class Pools;

// Heightmap types a structure asks the chunk generator for.
enum class HType : u8 { WorldSurfaceWG, OceanFloorWG, WorldSurface, MotionBlocking, MotionBlockingNoLeaves, OceanFloor };
HType htypeFromName(std::string_view n);  // "WORLD_SURFACE_WG"...

// Structure.GenerationContext, per start attempt.
struct GenCtx {
    const StructureEnv& env;
    DensityContext& dctx;
    i64 seed;
    int chunkX, chunkZ;
    WorldgenRandom random;                  // setLargeFeatureSeed(seed, chunkX, chunkZ)
    const std::vector<bool>* validBiomes;   // by biome id

    GenCtx(const StructureEnv& e, DensityContext& d, i64 s, int cx, int cz, const std::vector<bool>* biomes);
    int minBlockX() const { return chunkX * 16; }
    int minBlockZ() const { return chunkZ * 16; }
    int middleBlockX() const { return chunkX * 16 + 8; }
    int middleBlockZ() const { return chunkZ * 16 + 8; }
    int minY() const;
    int maxY() const;  // exclusive (LevelHeightAccessor.getMaxY() is inclusive: maxY() - 1)
    int seaLevel() const;
    bool validBiome(u16 b) const { return validBiomes && b < validBiomes->size() && (*validBiomes)[b]; }
    u16 noiseBiome(int qx, int qy, int qz) const;
    bool isValidBiomeAt(const BlockPos& p) const;
    bool couldStructureExistInColumn(int blockX, int blockZ, int minBlockY, int maxBlockY) const;
    bool couldValidBiomeExistOnTopOfChunkCenter() const;
    int baseHeight(int x, int z, HType t) const;  // ChunkGenerator.getBaseHeight / getFirstFreeHeight
    int firstFreeHeight(int x, int z, HType t) const { return baseHeight(x, z, t); }
    int firstOccupiedHeight(int x, int z, HType t) const { return baseHeight(x, z, t) - 1; }
    const std::vector<u16>& baseColumn(int x, int z) const;  // getBaseColumn: states from minY up
    int lowestY(int sizeX, int sizeZ) const;                   // Structure.getLowestY (chunk min corner)
    int lowestY(int minX, int minZ, int sizeX, int sizeZ) const;
    int meanFirstOccupiedHeight(int minX, int sizeX, int minZ, int sizeZ) const;
};

// The level a piece writes to while a chunk is decorated (WorldGenRegion with the structure helpers).
struct PlaceCtx {
    GenRegion& level;
    const StructureEnv& env;
    const SB& b;
    int chunkX, chunkZ;
    St get(const BlockPos& p) const { return {level.block(p)}; }
    St get(int x, int y, int z) const { return {level.block(x, y, z)}; }
    void set(const BlockPos& p, u16 s) const { level.setBlock(p, s); }
    int height(Heightmap t, int x, int z) const { return level.height(t, x, z); }
    int minY() const { return level.minY(); }
    int maxY() const { return level.maxY(); }  // exclusive
    void markPostProcess(const BlockPos& p) const;  // ChunkAccess.markPosForPostProcessing
    // Block entity data of what a piece placed, saved with the chunk: RandomizableContainer.setBlockEntityLootTable
    // (LootTable + LootTableSeed) and SpawnerBlockEntity.setEntityId (SpawnData; consumes no randomness).
    void setLootTable(const BlockPos& p, std::string_view table, i64 seed) const;
    void setSpawner(const BlockPos& p, std::string_view entity) const;
    // WorldGenRegion.addFreshEntity: an entity tag ({"id", "Pos", ...}) saved with the chunk it is in.
    void addEntity(NbtValue tag) const;
    RandomSource& random() const;                   // WorldGenRegion.getRandom()
    i64 seed() const;
};

// ---- pieces -------------------------------------------------------------------------------------------
class PiecesBuilder;

class Piece {
public:
    Piece(const char* typeId, int genDepth, const BBox& box) : type(typeId), genDepth(genDepth), box(box) {}
    virtual ~Piece() = default;

    const char* type;  // StructurePieceType id ("minecraft:jigsaw", "minecraft:msroom"...)
    int genDepth;
    BBox box;

    std::optional<Direction> orientation() const { return orientation_; }
    void setOrientation(std::optional<Direction> d);
    Rot rotation() const { return rotation_; }
    Mir mirror() const { return mirror_; }

    virtual void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) {
        (void)start, (void)builder, (void)random;
    }
    virtual void postProcess(PlaceCtx& c, RandomSource& random, const BBox& chunkBB, const BlockPos& referencePos) = 0;
    virtual void move(int dx, int dy, int dz) { box.move(dx, dy, dz); }
    bool isCloseToChunk(int cx, int cz, int distance) const {
        const int x = cx * 16, z = cz * 16;
        return box.intersectsXZ(x - distance, z - distance, x + 15 + distance, z + 15 + distance);
    }
    static BBox makeBoundingBox(int x, int y, int z, Direction d, int w, int h, int depth) {
        return axisZ(d) ? BBox(x, y, z, x + w - 1, y + h - 1, z + depth - 1) : BBox(x, y, z, x + depth - 1, y + h - 1, z + w - 1);
    }

protected:
    int worldX(int x, int z) const;
    int worldY(int y) const { return orientation_ ? y + box.minY : y; }
    int worldZ(int x, int z) const;
    BlockPos worldPos(int x, int y, int z) const { return {worldX(x, z), worldY(y), worldZ(x, z)}; }
    // The game's names, for the transliterated piece code.
    BlockPos getWorldPos(int x, int y, int z) const { return worldPos(x, y, z); }
    int getWorldX(int x, int z) const { return worldX(x, z); }
    int getWorldY(int y) const { return worldY(y); }
    int getWorldZ(int x, int z) const { return worldZ(x, z); }

    void placeBlock(PlaceCtx& c, u16 state, int x, int y, int z, const BBox& chunkBB);
    virtual bool canBeReplaced(PlaceCtx& c, int x, int y, int z, const BBox& chunkBB) {
        (void)c, (void)x, (void)y, (void)z, (void)chunkBB;
        return true;
    }
    u16 getBlock(PlaceCtx& c, int x, int y, int z, const BBox& chunkBB) const;
    bool isInterior(PlaceCtx& c, int x, int y, int z, const BBox& chunkBB) const;
    void generateAirBox(PlaceCtx& c, const BBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1);
    void generateBox(PlaceCtx& c, const BBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1, u16 edge,
                     u16 fill, bool skipAir);
    void generateBox(PlaceCtx& c, const BBox& chunkBB, const BBox& b, u16 edge, u16 fill, bool skipAir) {
        generateBox(c, chunkBB, b.minX, b.minY, b.minZ, b.maxX, b.maxY, b.maxZ, edge, fill, skipAir);
    }
public:
    struct BlockSelector {
        u16 next = 0;
        virtual ~BlockSelector() = default;
        virtual void select(RandomSource& r, int x, int y, int z, bool edge) = 0;
    };

protected:
    void generateBox(PlaceCtx& c, const BBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1, bool skipAir,
                     RandomSource& r, BlockSelector& sel);
    void generateBox(PlaceCtx& c, const BBox& chunkBB, const BBox& b, bool skipAir, RandomSource& r, BlockSelector& sel) {
        generateBox(c, chunkBB, b.minX, b.minY, b.minZ, b.maxX, b.maxY, b.maxZ, skipAir, r, sel);
    }
    void generateMaybeBox(PlaceCtx& c, const BBox& chunkBB, RandomSource& r, float probability, int x0, int y0, int z0,
                          int x1, int y1, int z1, u16 edge, u16 fill, bool skipAir, bool hasToBeInside);
    void maybeGenerateBlock(PlaceCtx& c, const BBox& chunkBB, RandomSource& r, float probability, int x, int y, int z, u16 s);
    void generateUpperHalfSphere(PlaceCtx& c, const BBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1, u16 fill,
                                 bool skipAir);
    void fillColumnDown(PlaceCtx& c, u16 s, int x, int startY, int z, const BBox& chunkBB);
    bool isReplaceableByStructures(PlaceCtx& c, u16 s) const;
    // StructurePiece.createChest: a chest (reoriented against walls unless `state`) with its loot table and a seed from r.
    bool createChest(PlaceCtx& c, const BBox& chunkBB, RandomSource& r, int x, int y, int z, std::string_view lootTable = {}) {
        return createChest(c, chunkBB, r, worldPos(x, y, z), std::nullopt, lootTable);
    }
    bool createChest(PlaceCtx& c, const BBox& chunkBB, RandomSource& r, const BlockPos& p, std::optional<u16> state,
                     std::string_view lootTable = {});
    bool createDispenser(PlaceCtx& c, const BBox& chunkBB, RandomSource& r, int x, int y, int z, Direction facing,
                         std::string_view lootTable = {});
    static u16 reorient(PlaceCtx& c, const BlockPos& p, u16 state);

private:
    std::optional<Direction> orientation_;
    Rot rotation_ = Rot::None;
    Mir mirror_ = Mir::None;
};

// ScatteredFeaturePiece: one piece on the ground, its height found when first placed.
class ScatteredPiece : public Piece {
public:
    ScatteredPiece(const char* type, int west, int floor, int north, int w, int h, int d, Direction dir)
        : Piece(type, 0, makeBoundingBox(west, floor, north, dir, w, h, d)), width(w), height(h), depth(d) {
        setOrientation(dir);
    }
    int width, height, depth;
    int heightPosition = -1;

protected:
    bool updateAverageGroundHeight(PlaceCtx& c, const BBox& chunkBB, int offset);
    bool updateHeightPositionToLowestGroundHeight(PlaceCtx& c, int offset);
};

class PiecesBuilder {
public:
    std::vector<std::unique_ptr<Piece>> pieces;
    void add(std::unique_ptr<Piece> p) { pieces.push_back(std::move(p)); }
    Piece* findCollisionPiece(const BBox& b) const {
        for (const auto& p : pieces)
            if (p->box.intersects(b)) return p.get();
        return nullptr;
    }
    BBox boundingBox() const;
    void offsetPiecesVertically(int dy) {
        for (auto& p : pieces) p->move(0, dy, 0);
    }
    int moveBelowSeaLevel(int seaLevel, int minY, RandomSource& r, int offset);
    void moveInsideHeights(RandomSource& r, int lowestAllowed, int highestAllowed);
    bool empty() const { return pieces.empty(); }
};

// ---- structures -------------------------------------------------------------------------------------------
enum class Adapt : u8 { None, Bury, BeardThin, BeardBox, Encapsulate };  // TerrainAdjustment

struct Stub {  // Structure.GenerationStub
    BlockPos position;
    std::function<void(PiecesBuilder&)> generator;
};

class Structure {
public:
    virtual ~Structure() = default;
    std::string id;            // "minecraft:village_plains"
    std::string type;          // "minecraft:jigsaw"
    std::vector<bool> biomes;  // by biome id
    int step = 4;              // GenerationStep.Decoration ordinal
    Adapt adapt = Adapt::None;

    virtual std::optional<Stub> findGenerationPoint(GenCtx& c) const = 0;
    // Structure.afterPlace
    virtual void afterPlace(PlaceCtx& c, RandomSource& r, const BBox& chunkBB, const std::vector<std::unique_ptr<Piece>>& pieces) const {
        (void)c, (void)r, (void)chunkBB, (void)pieces;
    }
    BBox adjustBoundingBox(const BBox& b) const { return adapt != Adapt::None ? b.inflatedBy(12) : b; }

protected:
    // Structure.onTopOfChunkCenter
    std::optional<Stub> onTopOfChunkCenter(GenCtx& c, HType h, std::function<void(PiecesBuilder&)> gen) const;
};

struct Start {
    const Structure* structure = nullptr;
    int chunkX = 0, chunkZ = 0;
    std::vector<std::unique_ptr<Piece>> pieces;
    BBox box;  // adjusted bounding box (StructureStart.getBoundingBox)
    mutable std::mutex placeMutex;  // pieces change while placed (ground heights found on first placement)
    void placeInChunk(PlaceCtx& c, RandomSource& r, const BBox& chunkBB) const;
};

// Structure types: "minecraft:jigsaw" -> factory(definition JSON).
using StructureFactory = std::function<std::unique_ptr<Structure>(const json& def, StructureEnv& env)>;
void registerStructureType(std::string type, StructureFactory f);
const StructureFactory* findStructureType(std::string_view type);
void registerJigsaw();          // jigsaw.cpp
void registerScattered();       // pieces_scattered.cpp
void registerMineshaft();       // pieces_mineshaft.cpp
void registerStronghold();      // pieces_stronghold.cpp
void registerMonument();        // pieces_monument.cpp
void registerMansion();         // pieces_mansion.cpp
void registerOceanStructures(); // pieces_ocean.cpp (ocean ruins, shipwrecks, ruined portals, buried treasure)
void registerNetherStructures(); // pieces_nether.cpp (nether fortresses, nether fossils)
void registerEndCity();          // pieces_end.cpp

// ---- shared world facts for structures --------------------------------------------------------------------
class StructureEnv {
public:
    StructureEnv(const ChunkGenerator& gen, const FeatureSystem* features, BlockRegistry& blocks, const DataPack& data, i64 seed);
    ~StructureEnv();

    const ChunkGenerator& gen;
    const FeatureSystem* features;
    const DataPack& data;
    i64 seed;
    BlockFacts facts;
    SB b;
    std::unique_ptr<TemplateManager> templates;
    std::unique_ptr<Pools> pools;

    int minY() const { return gen.minY(); }
    int height() const { return gen.height(); }
    int seaLevel() const { return gen.seaLevel(); }
    // WorldGenerationContext(generator, level): the generator's range within the level's (height providers).
    int genMinY() const { return std::max(gen.minY(), gen.genMinY()); }
    int genDepth() const { return std::min(gen.height(), gen.genDepth()); }
    BiomeTable& biomes() const { return const_cast<ChunkGenerator&>(gen).biomes(); }
    std::vector<bool> biomeSet(const json& holderSet) const;  // "#minecraft:tag", "minecraft:x", [..]
    bool biomeInTag(u16 biome, std::string_view tag) const;   // "minecraft:mineshaft_blocking" (cached)
    float temperature(u16 biome, const BlockPos& p) const;   // Biome.getTemperature
    // Cached base terrain heights (ChunkGenerator.getBaseHeight). Thread-safe.
    int baseHeight(DensityContext& ctx, int x, int z, HType t) const;
    const std::vector<u16>& baseColumn(DensityContext& ctx, int x, int z) const;  // thread-local cache

    const FeatureEnv* featureEnv() const;
    // ChunkAccess.markPosForPostProcessing: positions whose shape is updated when their chunk is finished.
    void markPostProcess(const BlockPos& p) const;
    std::vector<BlockPos> takeMarks(int cx, int cz) const;
    // Drops cached heights and post-process marks farther than radius chunks from (cx, cz) (deterministic: they are
    // recomputed if needed again). Thread-safe.
    void trimCaches(int cx, int cz, int radius) const;

private:
    struct HeightCache;
    std::unique_ptr<HeightCache> heights_;
    mutable std::mutex tagMutex_, markMutex_;
    mutable std::unordered_map<u64, std::vector<BlockPos>> marks_;
    mutable std::unordered_map<std::string, std::vector<bool>> biomeTags_;
};

}  // namespace wg::st
