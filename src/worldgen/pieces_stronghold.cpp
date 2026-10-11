// Strongholds: the game's StrongholdStructure and StrongholdPieces (weighted corridor/room selection with place
// limits, the imposed five-way crossing after the start stairs, small doors, libraries, prison halls, the portal room).
#include "worldgen/structure_core.h"

namespace wg::st {

namespace {

enum class Kind { Straight, PrisonHall, LeftTurn, RightTurn, RoomCrossing, StraightStairsDown, StairsDown, FiveCrossing, ChestCorridor,
                  Library, PortalRoom, None };

struct PieceWeight {
    Kind kind;
    int weight, placeCount = 0, maxPlaceCount;
    bool doPlace(int depth) const {
        const bool base = maxPlaceCount == 0 || placeCount < maxPlaceCount;
        if (kind == Kind::Library) return base && depth > 4;
        if (kind == Kind::PortalRoom) return base && depth > 5;
        return base;
    }
    bool isValid() const { return maxPlaceCount == 0 || placeCount < maxPlaceCount; }
};

enum class Door { Opening, WoodDoor, Grates, IronDoor };

struct SmoothStoneSelector final : Piece::BlockSelector {
    void select(RandomSource& random, int, int, int, bool edge) override {
        if (edge) {
            const float s = random.nextFloat();
            if (s < 0.2f) next = B("minecraft:cracked_stone_bricks");
            else if (s < 0.5f) next = B("minecraft:mossy_stone_bricks");
            else if (s < 0.55f) next = B("minecraft:infested_stone_bricks");
            else next = B("minecraft:stone_bricks");
        } else {
            next = B("minecraft:cave_air");
        }
    }
};

class StartPiece;

class StrongholdPiece : public Piece {
public:
    StrongholdPiece(const char* id, int genDepth, const BBox& b) : Piece(id, genDepth, b) {}
    Door entryDoor = Door::Opening;

protected:
    St CAVE_AIR() const { return B("minecraft:cave_air"); }
    void generateSmallDoor(PlaceCtx& level, RandomSource&, const BBox& chunkBB, Door door, int footX, int footY, int footZ);
    static Door randomSmallDoor(RandomSource& random) {
        switch (random.nextInt(5)) {
        case 2: return Door::WoodDoor;
        case 3: return Door::Grates;
        case 4: return Door::IronDoor;
        default: return Door::Opening;
        }
    }
    Piece* generateSmallDoorChildForward(StartPiece& start, PiecesBuilder& builder, RandomSource& random, int xOff, int yOff);
    Piece* generateSmallDoorChildLeft(StartPiece& start, PiecesBuilder& builder, RandomSource& random, int yOff, int zOff);
    Piece* generateSmallDoorChildRight(StartPiece& start, PiecesBuilder& builder, RandomSource& random, int yOff, int zOff);
    static bool isOkBox(const BBox& b) { return b.minY > 10; }
};

// ---- stairs down / start ---------------------------------------------------------------------------------

class StairsDown : public StrongholdPiece {
public:
    bool isSource;
    StairsDown(const char* id, int genDepth, int west, int north, Direction direction)
        : StrongholdPiece(id, genDepth, makeBoundingBox(west, 64, north, direction, 5, 11, 5)), isSource(true) {
        setOrientation(direction);
        entryDoor = Door::Opening;
    }
    StairsDown(int genDepth, RandomSource& random, const BBox& b, Direction direction)
        : StrongholdPiece("minecraft:shsd", genDepth, b), isSource(false) {
        setOrientation(direction);
        entryDoor = randomSmallDoor(random);
    }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override;
    static StrongholdPiece* createPiece(PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -1, -7, 0, 5, 11, 5, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new StairsDown(depth, random, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        SmoothStoneSelector sel;
        generateBox(level, chunkBB, 0, 0, 0, 4, 10, 4, true, random, sel);
        generateSmallDoor(level, random, chunkBB, entryDoor, 1, 7, 0);
        generateSmallDoor(level, random, chunkBB, Door::Opening, 1, 1, 4);
        const St bricks = B("minecraft:stone_bricks"), slab = B("minecraft:smooth_stone_slab");
        placeBlock(level, bricks, 2, 6, 1, chunkBB);
        placeBlock(level, bricks, 1, 5, 1, chunkBB);
        placeBlock(level, slab, 1, 6, 1, chunkBB);
        placeBlock(level, bricks, 1, 5, 2, chunkBB);
        placeBlock(level, bricks, 1, 4, 3, chunkBB);
        placeBlock(level, slab, 1, 5, 3, chunkBB);
        placeBlock(level, bricks, 2, 4, 3, chunkBB);
        placeBlock(level, bricks, 3, 3, 3, chunkBB);
        placeBlock(level, slab, 3, 4, 3, chunkBB);
        placeBlock(level, bricks, 3, 3, 2, chunkBB);
        placeBlock(level, bricks, 3, 2, 1, chunkBB);
        placeBlock(level, slab, 3, 3, 1, chunkBB);
        placeBlock(level, bricks, 2, 2, 1, chunkBB);
        placeBlock(level, bricks, 1, 1, 1, chunkBB);
        placeBlock(level, slab, 1, 2, 1, chunkBB);
        placeBlock(level, bricks, 1, 1, 2, chunkBB);
        placeBlock(level, slab, 1, 1, 3, chunkBB);
    }
};

class StartPiece final : public StairsDown {
public:
    StartPiece(RandomSource& random, int west, int north) : StairsDown("minecraft:shstart", 0, west, north, randomHorizontal(random)) {}
    // StrongholdPieces' static generation state, per stronghold.
    std::vector<PieceWeight> weights;
    std::vector<size_t> currentPieces;  // indices into weights
    int totalWeight = 0;
    Kind imposedPiece = Kind::None;
    int previousPiece = -1;
    Piece* portalRoomPiece = nullptr;
    std::vector<Piece*> pendingChildren;

    void resetPieces() {
        weights = {{Kind::Straight, 40, 0, 0},     {Kind::PrisonHall, 5, 0, 5},  {Kind::LeftTurn, 20, 0, 0},
                   {Kind::RightTurn, 20, 0, 0},    {Kind::RoomCrossing, 10, 0, 6}, {Kind::StraightStairsDown, 5, 0, 5},
                   {Kind::StairsDown, 5, 0, 5},    {Kind::FiveCrossing, 5, 0, 4}, {Kind::ChestCorridor, 5, 0, 4},
                   {Kind::Library, 10, 0, 2},      {Kind::PortalRoom, 20, 0, 1}};
        currentPieces.clear();
        for (size_t i = 0; i < weights.size(); ++i) currentPieces.push_back(i);
        imposedPiece = Kind::None;
    }
    bool updatePieceWeight() {
        bool any = false;
        totalWeight = 0;
        for (size_t i : currentPieces) {
            const PieceWeight& p = weights[i];
            if (p.maxPlaceCount > 0 && p.placeCount < p.maxPlaceCount) any = true;
            totalWeight += p.weight;
        }
        return any;
    }
};

void StairsDown::addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) {
    if (isSource) static_cast<StartPiece&>(start).imposedPiece = Kind::FiveCrossing;
    generateSmallDoorChildForward(static_cast<StartPiece&>(start), builder, random, 1, 1);
}

// ---- pieces ----------------------------------------------------------------------------------------------------

class ChestCorridor final : public StrongholdPiece {
public:
    bool hasPlacedChest = false;
    ChestCorridor(int genDepth, RandomSource& random, const BBox& b, Direction d) : StrongholdPiece("minecraft:shcc", genDepth, b) {
        setOrientation(d);
        entryDoor = randomSmallDoor(random);
    }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        generateSmallDoorChildForward(static_cast<StartPiece&>(start), builder, random, 1, 1);
    }
    static StrongholdPiece* createPiece(PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -1, -1, 0, 5, 5, 7, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new ChestCorridor(depth, random, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        SmoothStoneSelector sel;
        generateBox(level, chunkBB, 0, 0, 0, 4, 4, 6, true, random, sel);
        generateSmallDoor(level, random, chunkBB, entryDoor, 1, 1, 0);
        generateSmallDoor(level, random, chunkBB, Door::Opening, 1, 1, 6);
        generateBox(level, chunkBB, 3, 1, 2, 3, 1, 4, B("minecraft:stone_bricks"), B("minecraft:stone_bricks"), false);
        placeBlock(level, B("minecraft:stone_brick_slab"), 3, 1, 1, chunkBB);
        placeBlock(level, B("minecraft:stone_brick_slab"), 3, 1, 5, chunkBB);
        placeBlock(level, B("minecraft:stone_brick_slab"), 3, 2, 2, chunkBB);
        placeBlock(level, B("minecraft:stone_brick_slab"), 3, 2, 4, chunkBB);
        for (int z = 2; z <= 4; ++z) placeBlock(level, B("minecraft:stone_brick_slab"), 2, 1, z, chunkBB);
        if (!hasPlacedChest && chunkBB.isInside(getWorldPos(3, 2, 3))) {
            hasPlacedChest = true;
            createChest(level, chunkBB, random, 3, 2, 3);
        }
    }
};

class FillerCorridor final : public StrongholdPiece {
public:
    int steps;
    FillerCorridor(int genDepth, const BBox& b, Direction d) : StrongholdPiece("minecraft:shfc", genDepth, b) {
        setOrientation(d);
        steps = d != North && d != South ? b.xSpan() : b.zSpan();
    }
    static std::optional<BBox> findPieceBox(PiecesBuilder& builder, int x, int y, int z, Direction d) {
        BBox b = BBox::orient(x, y, z, -1, -1, 0, 5, 5, 4, d);
        Piece* collision = builder.findCollisionPiece(b);
        if (!collision) return std::nullopt;
        if (collision->box.minY == b.minY)
            for (int depth = 2; depth >= 1; --depth) {
                b = BBox::orient(x, y, z, -1, -1, 0, 5, 5, depth, d);
                if (!collision->box.intersects(b)) return BBox::orient(x, y, z, -1, -1, 0, 5, 5, depth + 1, d);
            }
        return std::nullopt;
    }
    void postProcess(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const BlockPos&) override {
        const St bricks = B("minecraft:stone_bricks"), air = CAVE_AIR();
        for (int i = 0; i < steps; ++i) {
            for (int x = 0; x <= 4; ++x) placeBlock(level, bricks, x, 0, i, chunkBB);
            for (int y = 1; y <= 3; ++y) {
                placeBlock(level, bricks, 0, y, i, chunkBB);
                placeBlock(level, air, 1, y, i, chunkBB);
                placeBlock(level, air, 2, y, i, chunkBB);
                placeBlock(level, air, 3, y, i, chunkBB);
                placeBlock(level, bricks, 4, y, i, chunkBB);
            }
            for (int x = 0; x <= 4; ++x) placeBlock(level, bricks, x, 4, i, chunkBB);
        }
    }
};

class FiveCrossing final : public StrongholdPiece {
public:
    bool leftLow, leftHigh, rightLow, rightHigh;
    FiveCrossing(int genDepth, RandomSource& random, const BBox& b, Direction d) : StrongholdPiece("minecraft:sh5c", genDepth, b) {
        setOrientation(d);
        entryDoor = randomSmallDoor(random);
        leftLow = random.nextBoolean();
        leftHigh = random.nextBoolean();
        rightLow = random.nextBoolean();
        rightHigh = random.nextInt(3) > 0;
    }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        int zOffA = 3, zOffB = 5;
        const auto o = orientation();
        if (o == West || o == North) zOffA = 8 - zOffA, zOffB = 8 - zOffB;
        StartPiece& s = static_cast<StartPiece&>(start);
        generateSmallDoorChildForward(s, builder, random, 5, 1);
        if (leftLow) generateSmallDoorChildLeft(s, builder, random, zOffA, 1);
        if (leftHigh) generateSmallDoorChildLeft(s, builder, random, zOffB, 7);
        if (rightLow) generateSmallDoorChildRight(s, builder, random, zOffA, 1);
        if (rightHigh) generateSmallDoorChildRight(s, builder, random, zOffB, 7);
    }
    static StrongholdPiece* createPiece(PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -4, -3, 0, 10, 9, 11, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new FiveCrossing(depth, random, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        SmoothStoneSelector sel;
        const St air = CAVE_AIR(), slab = B("minecraft:smooth_stone_slab");
        generateBox(level, chunkBB, 0, 0, 0, 9, 8, 10, true, random, sel);
        generateSmallDoor(level, random, chunkBB, entryDoor, 4, 3, 0);
        if (leftLow) generateBox(level, chunkBB, 0, 3, 1, 0, 5, 3, air, air, false);
        if (rightLow) generateBox(level, chunkBB, 9, 3, 1, 9, 5, 3, air, air, false);
        if (leftHigh) generateBox(level, chunkBB, 0, 5, 7, 0, 7, 9, air, air, false);
        if (rightHigh) generateBox(level, chunkBB, 9, 5, 7, 9, 7, 9, air, air, false);
        generateBox(level, chunkBB, 5, 1, 10, 7, 3, 10, air, air, false);
        generateBox(level, chunkBB, 1, 2, 1, 8, 2, 6, false, random, sel);
        generateBox(level, chunkBB, 4, 1, 5, 4, 4, 9, false, random, sel);
        generateBox(level, chunkBB, 8, 1, 5, 8, 4, 9, false, random, sel);
        generateBox(level, chunkBB, 1, 4, 7, 3, 4, 9, false, random, sel);
        generateBox(level, chunkBB, 1, 3, 5, 3, 3, 6, false, random, sel);
        generateBox(level, chunkBB, 1, 3, 4, 3, 3, 4, slab, slab, false);
        generateBox(level, chunkBB, 1, 4, 6, 3, 4, 6, slab, slab, false);
        generateBox(level, chunkBB, 5, 1, 7, 7, 1, 8, false, random, sel);
        generateBox(level, chunkBB, 5, 1, 9, 7, 1, 9, slab, slab, false);
        generateBox(level, chunkBB, 5, 2, 7, 7, 2, 7, slab, slab, false);
        generateBox(level, chunkBB, 4, 5, 7, 4, 5, 9, slab, slab, false);
        generateBox(level, chunkBB, 8, 5, 7, 8, 5, 9, slab, slab, false);
        generateBox(level, chunkBB, 5, 5, 7, 7, 5, 9, slab.with("type", "double"), slab.with("type", "double"), false);
        placeBlock(level, B("minecraft:wall_torch").with("facing", South), 6, 5, 6, chunkBB);
    }
};

class Turn : public StrongholdPiece {
public:
    Turn(const char* id, int genDepth, RandomSource& random, const BBox& b, Direction d, bool left)
        : StrongholdPiece(id, genDepth, b), left_(left) {
        setOrientation(d);
        entryDoor = randomSmallDoor(random);
    }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        const auto o = orientation();
        const bool flip = o != North && o != East;
        StartPiece& s = static_cast<StartPiece&>(start);
        if (left_ != flip) generateSmallDoorChildLeft(s, builder, random, 1, 1);
        else generateSmallDoorChildRight(s, builder, random, 1, 1);
    }
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        SmoothStoneSelector sel;
        generateBox(level, chunkBB, 0, 0, 0, 4, 4, 4, true, random, sel);
        generateSmallDoor(level, random, chunkBB, entryDoor, 1, 1, 0);
        const auto o = orientation();
        const bool flip = o != North && o != East;
        const St air = CAVE_AIR();
        // LeftTurn opens x=0 when not flipped, RightTurn x=4.
        if (left_ != flip) generateBox(level, chunkBB, 0, 1, 1, 0, 3, 3, air, air, false);
        else generateBox(level, chunkBB, 4, 1, 1, 4, 3, 3, air, air, false);
    }
    static StrongholdPiece* createPiece(PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d, int depth, bool left) {
        const BBox b = BBox::orient(x, y, z, -1, -1, 0, 5, 5, 5, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new Turn(left ? "minecraft:shlt" : "minecraft:shrt", depth, random, b, d, left)
                                                           : nullptr;
    }

private:
    bool left_;
};

class Library final : public StrongholdPiece {
public:
    bool isTall;
    Library(int genDepth, RandomSource& random, const BBox& b, Direction d) : StrongholdPiece("minecraft:shli", genDepth, b) {
        setOrientation(d);
        entryDoor = randomSmallDoor(random);
        isTall = b.ySpan() > 6;
    }
    static StrongholdPiece* createPiece(PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d, int depth) {
        BBox b = BBox::orient(x, y, z, -4, -1, 0, 14, 11, 15, d);
        if (!isOkBox(b) || builder.findCollisionPiece(b)) {
            b = BBox::orient(x, y, z, -4, -1, 0, 14, 6, 15, d);
            if (!isOkBox(b) || builder.findCollisionPiece(b)) return nullptr;
        }
        return new Library(depth, random, b, d);
    }
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        SmoothStoneSelector sel;
        const int currentHeight = isTall ? 11 : 6;
        const St planks = B("minecraft:oak_planks"), shelf = B("minecraft:bookshelf");
        generateBox(level, chunkBB, 0, 0, 0, 13, currentHeight - 1, 14, true, random, sel);
        generateSmallDoor(level, random, chunkBB, entryDoor, 4, 1, 0);
        generateMaybeBox(level, chunkBB, random, 0.07f, 2, 1, 1, 11, 4, 13, B("minecraft:cobweb"), B("minecraft:cobweb"), false, false);
        for (int d = 1; d <= 13; ++d) {
            if ((d - 1) % 4 == 0) {
                generateBox(level, chunkBB, 1, 1, d, 1, 4, d, planks, planks, false);
                generateBox(level, chunkBB, 12, 1, d, 12, 4, d, planks, planks, false);
                placeBlock(level, B("minecraft:wall_torch").with("facing", East), 2, 3, d, chunkBB);
                placeBlock(level, B("minecraft:wall_torch").with("facing", West), 11, 3, d, chunkBB);
                if (isTall) {
                    generateBox(level, chunkBB, 1, 6, d, 1, 9, d, planks, planks, false);
                    generateBox(level, chunkBB, 12, 6, d, 12, 9, d, planks, planks, false);
                }
            } else {
                generateBox(level, chunkBB, 1, 1, d, 1, 4, d, shelf, shelf, false);
                generateBox(level, chunkBB, 12, 1, d, 12, 4, d, shelf, shelf, false);
                if (isTall) {
                    generateBox(level, chunkBB, 1, 6, d, 1, 9, d, shelf, shelf, false);
                    generateBox(level, chunkBB, 12, 6, d, 12, 9, d, shelf, shelf, false);
                }
            }
        }
        for (int dx = 3; dx < 12; dx += 2) {
            generateBox(level, chunkBB, 3, 1, dx, 4, 3, dx, shelf, shelf, false);
            generateBox(level, chunkBB, 6, 1, dx, 7, 3, dx, shelf, shelf, false);
            generateBox(level, chunkBB, 9, 1, dx, 10, 3, dx, shelf, shelf, false);
        }
        if (isTall) {
            generateBox(level, chunkBB, 1, 5, 1, 3, 5, 13, planks, planks, false);
            generateBox(level, chunkBB, 10, 5, 1, 12, 5, 13, planks, planks, false);
            generateBox(level, chunkBB, 4, 5, 1, 9, 5, 2, planks, planks, false);
            generateBox(level, chunkBB, 4, 5, 12, 9, 5, 13, planks, planks, false);
            placeBlock(level, planks, 9, 5, 11, chunkBB);
            placeBlock(level, planks, 8, 5, 11, chunkBB);
            placeBlock(level, planks, 9, 5, 10, chunkBB);
            const St fence = B("minecraft:oak_fence");
            const St weFence = fence.with("west", true).with("east", true);
            const St nsFence = fence.with("north", true).with("south", true);
            generateBox(level, chunkBB, 3, 6, 3, 3, 6, 11, nsFence, nsFence, false);
            generateBox(level, chunkBB, 10, 6, 3, 10, 6, 9, nsFence, nsFence, false);
            generateBox(level, chunkBB, 4, 6, 2, 9, 6, 2, weFence, weFence, false);
            generateBox(level, chunkBB, 4, 6, 12, 7, 6, 12, weFence, weFence, false);
            placeBlock(level, fence.with("north", true).with("east", true), 3, 6, 2, chunkBB);
            placeBlock(level, fence.with("south", true).with("east", true), 3, 6, 12, chunkBB);
            placeBlock(level, fence.with("north", true).with("west", true), 10, 6, 2, chunkBB);
            for (int i = 0; i <= 2; ++i) {
                placeBlock(level, fence.with("south", true).with("west", true), 8 + i, 6, 12 - i, chunkBB);
                if (i != 2) placeBlock(level, fence.with("north", true).with("east", true), 8 + i, 6, 11 - i, chunkBB);
            }
            const St ladder = B("minecraft:ladder").with("facing", South);
            for (int y = 1; y <= 7; ++y) placeBlock(level, ladder, 10, y, 13, chunkBB);
            const St eFence = fence.with("east", true);
            placeBlock(level, eFence, 6, 9, 7, chunkBB);
            const St wFence = fence.with("west", true);
            placeBlock(level, wFence, 7, 9, 7, chunkBB);
            placeBlock(level, eFence, 6, 8, 7, chunkBB);
            placeBlock(level, wFence, 7, 8, 7, chunkBB);
            const St nsweFence = nsFence.with("west", true).with("east", true);
            placeBlock(level, nsweFence, 6, 7, 7, chunkBB);
            placeBlock(level, nsweFence, 7, 7, 7, chunkBB);
            placeBlock(level, eFence, 5, 7, 7, chunkBB);
            placeBlock(level, wFence, 8, 7, 7, chunkBB);
            placeBlock(level, eFence.with("north", true), 6, 7, 6, chunkBB);
            placeBlock(level, eFence.with("south", true), 6, 7, 8, chunkBB);
            placeBlock(level, wFence.with("north", true), 7, 7, 6, chunkBB);
            placeBlock(level, wFence.with("south", true), 7, 7, 8, chunkBB);
            const St torch = B("minecraft:torch");
            placeBlock(level, torch, 5, 8, 7, chunkBB);
            placeBlock(level, torch, 8, 8, 7, chunkBB);
            placeBlock(level, torch, 6, 8, 6, chunkBB);
            placeBlock(level, torch, 6, 8, 8, chunkBB);
            placeBlock(level, torch, 7, 8, 6, chunkBB);
            placeBlock(level, torch, 7, 8, 8, chunkBB);
        }
        createChest(level, chunkBB, random, 3, 3, 5);
        if (isTall) {
            placeBlock(level, CAVE_AIR(), 12, 9, 1, chunkBB);
            createChest(level, chunkBB, random, 12, 8, 1);
        }
    }
};

class PortalRoom final : public StrongholdPiece {
public:
    bool hasPlacedSpawner = false;
    PortalRoom(int genDepth, const BBox& b, Direction d) : StrongholdPiece("minecraft:shpr", genDepth, b) { setOrientation(d); }
    void addChildren(Piece& start, PiecesBuilder&, RandomSource&) override { static_cast<StartPiece&>(start).portalRoomPiece = this; }
    static StrongholdPiece* createPiece(PiecesBuilder& builder, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -4, -1, 0, 11, 8, 16, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new PortalRoom(depth, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        SmoothStoneSelector sel;
        const St lava = B("minecraft:lava");
        generateBox(level, chunkBB, 0, 0, 0, 10, 7, 15, false, random, sel);
        generateSmallDoor(level, random, chunkBB, Door::Grates, 4, 1, 0);
        generateBox(level, chunkBB, 1, 6, 1, 1, 6, 14, false, random, sel);
        generateBox(level, chunkBB, 9, 6, 1, 9, 6, 14, false, random, sel);
        generateBox(level, chunkBB, 2, 6, 1, 8, 6, 2, false, random, sel);
        generateBox(level, chunkBB, 2, 6, 14, 8, 6, 14, false, random, sel);
        generateBox(level, chunkBB, 1, 1, 1, 2, 1, 4, false, random, sel);
        generateBox(level, chunkBB, 8, 1, 1, 9, 1, 4, false, random, sel);
        generateBox(level, chunkBB, 1, 1, 1, 1, 1, 3, lava, lava, false);
        generateBox(level, chunkBB, 9, 1, 1, 9, 1, 3, lava, lava, false);
        generateBox(level, chunkBB, 3, 1, 8, 7, 1, 12, false, random, sel);
        generateBox(level, chunkBB, 4, 1, 9, 6, 1, 11, lava, lava, false);
        const St nsBars = B("minecraft:iron_bars").with("north", true).with("south", true);
        const St weBars = B("minecraft:iron_bars").with("west", true).with("east", true);
        for (int z = 3; z < 14; z += 2) {
            generateBox(level, chunkBB, 0, 3, z, 0, 4, z, nsBars, nsBars, false);
            generateBox(level, chunkBB, 10, 3, z, 10, 4, z, nsBars, nsBars, false);
        }
        for (int x = 2; x < 9; x += 2) generateBox(level, chunkBB, x, 3, 15, x, 4, 15, weBars, weBars, false);
        const St stairs = B("minecraft:stone_brick_stairs").with("facing", North);
        generateBox(level, chunkBB, 4, 1, 5, 6, 1, 7, false, random, sel);
        generateBox(level, chunkBB, 4, 2, 6, 6, 2, 7, false, random, sel);
        generateBox(level, chunkBB, 4, 3, 7, 6, 3, 7, false, random, sel);
        for (int x = 4; x <= 6; ++x) {
            placeBlock(level, stairs, x, 1, 4, chunkBB);
            placeBlock(level, stairs, x, 2, 5, chunkBB);
            placeBlock(level, stairs, x, 3, 6, chunkBB);
        }
        const St frame = B("minecraft:end_portal_frame");
        const St northFrame = frame.with("facing", North), southFrame = frame.with("facing", South);
        const St eastFrame = frame.with("facing", East), westFrame = frame.with("facing", West);
        bool allEyes = true;
        bool eyes[12];
        for (bool& e : eyes) {
            e = random.nextFloat() > 0.9f;
            allEyes &= e;
        }
        placeBlock(level, northFrame.with("eye", eyes[0]), 4, 3, 8, chunkBB);
        placeBlock(level, northFrame.with("eye", eyes[1]), 5, 3, 8, chunkBB);
        placeBlock(level, northFrame.with("eye", eyes[2]), 6, 3, 8, chunkBB);
        placeBlock(level, southFrame.with("eye", eyes[3]), 4, 3, 12, chunkBB);
        placeBlock(level, southFrame.with("eye", eyes[4]), 5, 3, 12, chunkBB);
        placeBlock(level, southFrame.with("eye", eyes[5]), 6, 3, 12, chunkBB);
        placeBlock(level, eastFrame.with("eye", eyes[6]), 3, 3, 9, chunkBB);
        placeBlock(level, eastFrame.with("eye", eyes[7]), 3, 3, 10, chunkBB);
        placeBlock(level, eastFrame.with("eye", eyes[8]), 3, 3, 11, chunkBB);
        placeBlock(level, westFrame.with("eye", eyes[9]), 7, 3, 9, chunkBB);
        placeBlock(level, westFrame.with("eye", eyes[10]), 7, 3, 10, chunkBB);
        placeBlock(level, westFrame.with("eye", eyes[11]), 7, 3, 11, chunkBB);
        if (allEyes) {
            const St portal = B("minecraft:end_portal");
            for (int z = 9; z <= 11; ++z)
                for (int x = 4; x <= 6; ++x) placeBlock(level, portal, x, 3, z, chunkBB);
        }
        if (!hasPlacedSpawner) {
            const BlockPos pos = getWorldPos(5, 3, 6);
            if (chunkBB.isInside(pos)) {
                hasPlacedSpawner = true;
                level.set(pos, B("minecraft:spawner"));
            }
        }
    }
};

class PrisonHall final : public StrongholdPiece {
public:
    PrisonHall(int genDepth, RandomSource& random, const BBox& b, Direction d) : StrongholdPiece("minecraft:shph", genDepth, b) {
        setOrientation(d);
        entryDoor = randomSmallDoor(random);
    }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        generateSmallDoorChildForward(static_cast<StartPiece&>(start), builder, random, 1, 1);
    }
    static StrongholdPiece* createPiece(PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -1, -1, 0, 9, 5, 11, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new PrisonHall(depth, random, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        SmoothStoneSelector sel;
        const St air = CAVE_AIR(), bars = B("minecraft:iron_bars");
        generateBox(level, chunkBB, 0, 0, 0, 8, 4, 10, true, random, sel);
        generateSmallDoor(level, random, chunkBB, entryDoor, 1, 1, 0);
        generateBox(level, chunkBB, 1, 1, 10, 3, 3, 10, air, air, false);
        generateBox(level, chunkBB, 4, 1, 1, 4, 3, 1, false, random, sel);
        generateBox(level, chunkBB, 4, 1, 3, 4, 3, 3, false, random, sel);
        generateBox(level, chunkBB, 4, 1, 7, 4, 3, 7, false, random, sel);
        generateBox(level, chunkBB, 4, 1, 9, 4, 3, 9, false, random, sel);
        for (int y = 1; y <= 3; ++y) {
            placeBlock(level, bars.with("north", true).with("south", true), 4, y, 4, chunkBB);
            placeBlock(level, bars.with("north", true).with("south", true).with("east", true), 4, y, 5, chunkBB);
            placeBlock(level, bars.with("north", true).with("south", true), 4, y, 6, chunkBB);
            placeBlock(level, bars.with("west", true).with("east", true), 5, y, 5, chunkBB);
            placeBlock(level, bars.with("west", true).with("east", true), 6, y, 5, chunkBB);
            placeBlock(level, bars.with("west", true).with("east", true), 7, y, 5, chunkBB);
        }
        placeBlock(level, bars.with("north", true).with("south", true), 4, 3, 2, chunkBB);
        placeBlock(level, bars.with("north", true).with("south", true), 4, 3, 8, chunkBB);
        const St doorBottom = B("minecraft:iron_door").with("facing", West);
        const St doorTop = doorBottom.with("half", "upper");
        placeBlock(level, doorBottom, 4, 1, 2, chunkBB);
        placeBlock(level, doorTop, 4, 2, 2, chunkBB);
        placeBlock(level, doorBottom, 4, 1, 8, chunkBB);
        placeBlock(level, doorTop, 4, 2, 8, chunkBB);
    }
};

class RoomCrossing final : public StrongholdPiece {
public:
    int type;
    RoomCrossing(int genDepth, RandomSource& random, const BBox& b, Direction d) : StrongholdPiece("minecraft:shrc", genDepth, b) {
        setOrientation(d);
        entryDoor = randomSmallDoor(random);
        type = random.nextInt(5);
    }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        StartPiece& s = static_cast<StartPiece&>(start);
        generateSmallDoorChildForward(s, builder, random, 4, 1);
        generateSmallDoorChildLeft(s, builder, random, 1, 4);
        generateSmallDoorChildRight(s, builder, random, 1, 4);
    }
    static StrongholdPiece* createPiece(PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -4, -1, 0, 11, 7, 11, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new RoomCrossing(depth, random, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        SmoothStoneSelector sel;
        const St air = CAVE_AIR(), bricks = B("minecraft:stone_bricks"), slab = B("minecraft:smooth_stone_slab"),
                 cobble = B("minecraft:cobblestone"), planks = B("minecraft:oak_planks");
        generateBox(level, chunkBB, 0, 0, 0, 10, 6, 10, true, random, sel);
        generateSmallDoor(level, random, chunkBB, entryDoor, 4, 1, 0);
        generateBox(level, chunkBB, 4, 1, 10, 6, 3, 10, air, air, false);
        generateBox(level, chunkBB, 0, 1, 4, 0, 3, 6, air, air, false);
        generateBox(level, chunkBB, 10, 1, 4, 10, 3, 6, air, air, false);
        switch (type) {
        case 0:
            placeBlock(level, bricks, 5, 1, 5, chunkBB);
            placeBlock(level, bricks, 5, 2, 5, chunkBB);
            placeBlock(level, bricks, 5, 3, 5, chunkBB);
            placeBlock(level, B("minecraft:wall_torch").with("facing", West), 4, 3, 5, chunkBB);
            placeBlock(level, B("minecraft:wall_torch").with("facing", East), 6, 3, 5, chunkBB);
            placeBlock(level, B("minecraft:wall_torch").with("facing", South), 5, 3, 4, chunkBB);
            placeBlock(level, B("minecraft:wall_torch").with("facing", North), 5, 3, 6, chunkBB);
            placeBlock(level, slab, 4, 1, 4, chunkBB);
            placeBlock(level, slab, 4, 1, 5, chunkBB);
            placeBlock(level, slab, 4, 1, 6, chunkBB);
            placeBlock(level, slab, 6, 1, 4, chunkBB);
            placeBlock(level, slab, 6, 1, 5, chunkBB);
            placeBlock(level, slab, 6, 1, 6, chunkBB);
            placeBlock(level, slab, 5, 1, 4, chunkBB);
            placeBlock(level, slab, 5, 1, 6, chunkBB);
            break;
        case 1:
            for (int i = 0; i < 5; ++i) {
                placeBlock(level, bricks, 3, 1, 3 + i, chunkBB);
                placeBlock(level, bricks, 7, 1, 3 + i, chunkBB);
                placeBlock(level, bricks, 3 + i, 1, 3, chunkBB);
                placeBlock(level, bricks, 3 + i, 1, 7, chunkBB);
            }
            placeBlock(level, bricks, 5, 1, 5, chunkBB);
            placeBlock(level, bricks, 5, 2, 5, chunkBB);
            placeBlock(level, bricks, 5, 3, 5, chunkBB);
            placeBlock(level, B("minecraft:water"), 5, 4, 5, chunkBB);
            break;
        case 2: {
            for (int z = 1; z <= 9; ++z) {
                placeBlock(level, cobble, 1, 3, z, chunkBB);
                placeBlock(level, cobble, 9, 3, z, chunkBB);
            }
            for (int x = 1; x <= 9; ++x) {
                placeBlock(level, cobble, x, 3, 1, chunkBB);
                placeBlock(level, cobble, x, 3, 9, chunkBB);
            }
            placeBlock(level, cobble, 5, 1, 4, chunkBB);
            placeBlock(level, cobble, 5, 1, 6, chunkBB);
            placeBlock(level, cobble, 5, 3, 4, chunkBB);
            placeBlock(level, cobble, 5, 3, 6, chunkBB);
            placeBlock(level, cobble, 4, 1, 5, chunkBB);
            placeBlock(level, cobble, 6, 1, 5, chunkBB);
            placeBlock(level, cobble, 4, 3, 5, chunkBB);
            placeBlock(level, cobble, 6, 3, 5, chunkBB);
            for (int y = 1; y <= 3; ++y) {
                placeBlock(level, cobble, 4, y, 4, chunkBB);
                placeBlock(level, cobble, 6, y, 4, chunkBB);
                placeBlock(level, cobble, 4, y, 6, chunkBB);
                placeBlock(level, cobble, 6, y, 6, chunkBB);
            }
            placeBlock(level, B("minecraft:wall_torch"), 5, 3, 5, chunkBB);
            for (int z = 2; z <= 8; ++z) {
                placeBlock(level, planks, 2, 3, z, chunkBB);
                placeBlock(level, planks, 3, 3, z, chunkBB);
                if (z <= 3 || z >= 7) {
                    placeBlock(level, planks, 4, 3, z, chunkBB);
                    placeBlock(level, planks, 5, 3, z, chunkBB);
                    placeBlock(level, planks, 6, 3, z, chunkBB);
                }
                placeBlock(level, planks, 7, 3, z, chunkBB);
                placeBlock(level, planks, 8, 3, z, chunkBB);
            }
            const St ladder = B("minecraft:ladder").with("facing", West);
            placeBlock(level, ladder, 9, 1, 3, chunkBB);
            placeBlock(level, ladder, 9, 2, 3, chunkBB);
            placeBlock(level, ladder, 9, 3, 3, chunkBB);
            createChest(level, chunkBB, random, 3, 4, 8);
            break;
        }
        default: break;
        }
    }
};

class Straight final : public StrongholdPiece {
public:
    bool leftChild, rightChild;
    Straight(int genDepth, RandomSource& random, const BBox& b, Direction d) : StrongholdPiece("minecraft:shs", genDepth, b) {
        setOrientation(d);
        entryDoor = randomSmallDoor(random);
        leftChild = random.nextInt(2) == 0;
        rightChild = random.nextInt(2) == 0;
    }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        StartPiece& s = static_cast<StartPiece&>(start);
        generateSmallDoorChildForward(s, builder, random, 1, 1);
        if (leftChild) generateSmallDoorChildLeft(s, builder, random, 1, 2);
        if (rightChild) generateSmallDoorChildRight(s, builder, random, 1, 2);
    }
    static StrongholdPiece* createPiece(PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -1, -1, 0, 5, 5, 7, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new Straight(depth, random, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        SmoothStoneSelector sel;
        const St air = CAVE_AIR();
        generateBox(level, chunkBB, 0, 0, 0, 4, 4, 6, true, random, sel);
        generateSmallDoor(level, random, chunkBB, entryDoor, 1, 1, 0);
        generateSmallDoor(level, random, chunkBB, Door::Opening, 1, 1, 6);
        const St eastTorch = B("minecraft:wall_torch").with("facing", East);
        const St westTorch = B("minecraft:wall_torch").with("facing", West);
        maybeGenerateBlock(level, chunkBB, random, 0.1f, 1, 2, 1, eastTorch);
        maybeGenerateBlock(level, chunkBB, random, 0.1f, 3, 2, 1, westTorch);
        maybeGenerateBlock(level, chunkBB, random, 0.1f, 1, 2, 5, eastTorch);
        maybeGenerateBlock(level, chunkBB, random, 0.1f, 3, 2, 5, westTorch);
        if (leftChild) generateBox(level, chunkBB, 0, 1, 2, 0, 3, 4, air, air, false);
        if (rightChild) generateBox(level, chunkBB, 4, 1, 2, 4, 3, 4, air, air, false);
    }
};

class StraightStairsDown final : public StrongholdPiece {
public:
    StraightStairsDown(int genDepth, RandomSource& random, const BBox& b, Direction d) : StrongholdPiece("minecraft:shssd", genDepth, b) {
        setOrientation(d);
        entryDoor = randomSmallDoor(random);
    }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        generateSmallDoorChildForward(static_cast<StartPiece&>(start), builder, random, 1, 1);
    }
    static StrongholdPiece* createPiece(PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -1, -7, 0, 5, 11, 8, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new StraightStairsDown(depth, random, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        SmoothStoneSelector sel;
        generateBox(level, chunkBB, 0, 0, 0, 4, 10, 7, true, random, sel);
        generateSmallDoor(level, random, chunkBB, entryDoor, 1, 7, 0);
        generateSmallDoor(level, random, chunkBB, Door::Opening, 1, 1, 7);
        const St stairs = B("minecraft:cobblestone_stairs").with("facing", South), bricks = B("minecraft:stone_bricks");
        for (int i = 0; i < 6; ++i) {
            placeBlock(level, stairs, 1, 6 - i, 1 + i, chunkBB);
            placeBlock(level, stairs, 2, 6 - i, 1 + i, chunkBB);
            placeBlock(level, stairs, 3, 6 - i, 1 + i, chunkBB);
            if (i < 5) {
                placeBlock(level, bricks, 1, 5 - i, 1 + i, chunkBB);
                placeBlock(level, bricks, 2, 5 - i, 1 + i, chunkBB);
                placeBlock(level, bricks, 3, 5 - i, 1 + i, chunkBB);
            }
        }
    }
};

// ---- generation ------------------------------------------------------------------------------------------------

StrongholdPiece* findAndCreatePieceFactory(Kind k, PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d,
                                           int depth) {
    switch (k) {
    case Kind::Straight: return Straight::createPiece(builder, random, x, y, z, d, depth);
    case Kind::PrisonHall: return PrisonHall::createPiece(builder, random, x, y, z, d, depth);
    case Kind::LeftTurn: return Turn::createPiece(builder, random, x, y, z, d, depth, true);
    case Kind::RightTurn: return Turn::createPiece(builder, random, x, y, z, d, depth, false);
    case Kind::RoomCrossing: return RoomCrossing::createPiece(builder, random, x, y, z, d, depth);
    case Kind::StraightStairsDown: return StraightStairsDown::createPiece(builder, random, x, y, z, d, depth);
    case Kind::StairsDown: return StairsDown::createPiece(builder, random, x, y, z, d, depth);
    case Kind::FiveCrossing: return FiveCrossing::createPiece(builder, random, x, y, z, d, depth);
    case Kind::ChestCorridor: return ChestCorridor::createPiece(builder, random, x, y, z, d, depth);
    case Kind::Library: return Library::createPiece(builder, random, x, y, z, d, depth);
    case Kind::PortalRoom: return PortalRoom::createPiece(builder, x, y, z, d, depth);
    default: return nullptr;
    }
}

Piece* generatePieceFromSmallDoor(StartPiece& start, PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d,
                                  int depth) {
    if (!start.updatePieceWeight()) return nullptr;
    if (start.imposedPiece != Kind::None) {
        StrongholdPiece* p = findAndCreatePieceFactory(start.imposedPiece, builder, random, x, y, z, d, depth);
        start.imposedPiece = Kind::None;
        if (p) return p;
    }
    for (int attempts = 0; attempts < 5;) {
        ++attempts;
        int selection = random.nextInt(start.totalWeight);
        for (size_t ci = 0; ci < start.currentPieces.size(); ++ci) {
            const size_t wi = start.currentPieces[ci];
            PieceWeight& w = start.weights[wi];
            selection -= w.weight;
            if (selection < 0) {
                if (!w.doPlace(depth) || int(wi) == start.previousPiece) break;
                StrongholdPiece* p = findAndCreatePieceFactory(w.kind, builder, random, x, y, z, d, depth);
                if (p) {
                    ++w.placeCount;
                    start.previousPiece = int(wi);
                    if (!w.isValid()) start.currentPieces.erase(start.currentPieces.begin() + std::ptrdiff_t(ci));
                    return p;
                }
            }
        }
    }
    const auto b = FillerCorridor::findPieceBox(builder, x, y, z, d);
    return b && b->minY > 1 ? new FillerCorridor(depth, *b, d) : nullptr;
}

Piece* generateAndAddPiece(StartPiece& start, PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d, int depth) {
    if (depth > 50) return nullptr;
    if (std::abs(x - start.box.minX) > 112 || std::abs(z - start.box.minZ) > 112) return nullptr;
    Piece* p = generatePieceFromSmallDoor(start, builder, random, x, y, z, d, depth + 1);
    if (p) {
        builder.add(std::unique_ptr<Piece>(p));
        start.pendingChildren.push_back(p);
    }
    return p;
}

Piece* StrongholdPiece::generateSmallDoorChildForward(StartPiece& start, PiecesBuilder& builder, RandomSource& random, int xOff, int yOff) {
    const auto o = orientation();
    if (!o) return nullptr;
    switch (*o) {
    case North: return generateAndAddPiece(start, builder, random, box.minX + xOff, box.minY + yOff, box.minZ - 1, *o, genDepth);
    case South: return generateAndAddPiece(start, builder, random, box.minX + xOff, box.minY + yOff, box.maxZ + 1, *o, genDepth);
    case West: return generateAndAddPiece(start, builder, random, box.minX - 1, box.minY + yOff, box.minZ + xOff, *o, genDepth);
    case East: return generateAndAddPiece(start, builder, random, box.maxX + 1, box.minY + yOff, box.minZ + xOff, *o, genDepth);
    default: return nullptr;
    }
}

Piece* StrongholdPiece::generateSmallDoorChildLeft(StartPiece& start, PiecesBuilder& builder, RandomSource& random, int yOff, int zOff) {
    const auto o = orientation();
    if (!o) return nullptr;
    switch (*o) {
    case North:
    case South: return generateAndAddPiece(start, builder, random, box.minX - 1, box.minY + yOff, box.minZ + zOff, West, genDepth);
    case West:
    case East: return generateAndAddPiece(start, builder, random, box.minX + zOff, box.minY + yOff, box.minZ - 1, North, genDepth);
    default: return nullptr;
    }
}

Piece* StrongholdPiece::generateSmallDoorChildRight(StartPiece& start, PiecesBuilder& builder, RandomSource& random, int yOff, int zOff) {
    const auto o = orientation();
    if (!o) return nullptr;
    switch (*o) {
    case North:
    case South: return generateAndAddPiece(start, builder, random, box.maxX + 1, box.minY + yOff, box.minZ + zOff, East, genDepth);
    case West:
    case East: return generateAndAddPiece(start, builder, random, box.minX + zOff, box.minY + yOff, box.maxZ + 1, South, genDepth);
    default: return nullptr;
    }
}

void StrongholdPiece::generateSmallDoor(PlaceCtx& level, RandomSource&, const BBox& chunkBB, Door door, int footX, int footY, int footZ) {
    const St bricks = B("minecraft:stone_bricks");
    switch (door) {
    case Door::Opening:
        generateBox(level, chunkBB, footX, footY, footZ, footX + 3 - 1, footY + 3 - 1, footZ, CAVE_AIR(), CAVE_AIR(), false);
        break;
    case Door::WoodDoor:
        placeBlock(level, bricks, footX, footY, footZ, chunkBB);
        placeBlock(level, bricks, footX, footY + 1, footZ, chunkBB);
        placeBlock(level, bricks, footX, footY + 2, footZ, chunkBB);
        placeBlock(level, bricks, footX + 1, footY + 2, footZ, chunkBB);
        placeBlock(level, bricks, footX + 2, footY + 2, footZ, chunkBB);
        placeBlock(level, bricks, footX + 2, footY + 1, footZ, chunkBB);
        placeBlock(level, bricks, footX + 2, footY, footZ, chunkBB);
        placeBlock(level, B("minecraft:oak_door"), footX + 1, footY, footZ, chunkBB);
        placeBlock(level, B("minecraft:oak_door").with("half", "upper"), footX + 1, footY + 1, footZ, chunkBB);
        break;
    case Door::Grates: {
        const St bars = B("minecraft:iron_bars");
        placeBlock(level, CAVE_AIR(), footX + 1, footY, footZ, chunkBB);
        placeBlock(level, CAVE_AIR(), footX + 1, footY + 1, footZ, chunkBB);
        placeBlock(level, bars.with("west", true), footX, footY, footZ, chunkBB);
        placeBlock(level, bars.with("west", true), footX, footY + 1, footZ, chunkBB);
        placeBlock(level, bars.with("east", true).with("west", true), footX, footY + 2, footZ, chunkBB);
        placeBlock(level, bars.with("east", true).with("west", true), footX + 1, footY + 2, footZ, chunkBB);
        placeBlock(level, bars.with("east", true).with("west", true), footX + 2, footY + 2, footZ, chunkBB);
        placeBlock(level, bars.with("east", true), footX + 2, footY + 1, footZ, chunkBB);
        placeBlock(level, bars.with("east", true), footX + 2, footY, footZ, chunkBB);
        break;
    }
    case Door::IronDoor:
        placeBlock(level, bricks, footX, footY, footZ, chunkBB);
        placeBlock(level, bricks, footX, footY + 1, footZ, chunkBB);
        placeBlock(level, bricks, footX, footY + 2, footZ, chunkBB);
        placeBlock(level, bricks, footX + 1, footY + 2, footZ, chunkBB);
        placeBlock(level, bricks, footX + 2, footY + 2, footZ, chunkBB);
        placeBlock(level, bricks, footX + 2, footY + 1, footZ, chunkBB);
        placeBlock(level, bricks, footX + 2, footY, footZ, chunkBB);
        placeBlock(level, B("minecraft:iron_door"), footX + 1, footY, footZ, chunkBB);
        placeBlock(level, B("minecraft:iron_door").with("half", "upper"), footX + 1, footY + 1, footZ, chunkBB);
        placeBlock(level, B("minecraft:stone_button").with("facing", North), footX + 2, footY + 1, footZ + 1, chunkBB);
        placeBlock(level, B("minecraft:stone_button").with("facing", South), footX + 2, footY + 1, footZ - 1, chunkBB);
        break;
    }
}

class StrongholdStructure final : public Structure {
public:
    std::optional<Stub> findGenerationPoint(GenCtx& c) const override {
        GenCtx* ctx = &c;
        return Stub{{c.minBlockX(), 0, c.minBlockZ()}, [ctx](PiecesBuilder& builder) {
                        int tries = 0;
                        StartPiece* start = nullptr;
                        do {
                            builder.pieces.clear();
                            // setLargeFeatureSeed(seed + tries++, chunkX, chunkZ)
                            const i64 seed = jadd(ctx->seed, i64(tries++));
                            ctx->random.setSeed(seed);
                            const i64 xs = ctx->random.nextLong(), zs = ctx->random.nextLong();
                            ctx->random.setSeed(jmul(i64(ctx->chunkX), xs) ^ jmul(i64(ctx->chunkZ), zs) ^ seed);
                            auto sp = std::make_unique<StartPiece>(ctx->random, ctx->minBlockX() + 2, ctx->minBlockZ() + 2);
                            start = sp.get();
                            start->resetPieces();
                            builder.add(std::move(sp));
                            start->addChildren(*start, builder, ctx->random);
                            while (!start->pendingChildren.empty()) {
                                const int pos = ctx->random.nextInt(int(start->pendingChildren.size()));
                                Piece* p = start->pendingChildren[size_t(pos)];
                                start->pendingChildren.erase(start->pendingChildren.begin() + pos);
                                p->addChildren(*start, builder, ctx->random);
                            }
                            builder.moveBelowSeaLevel(ctx->seaLevel(), ctx->minY(), ctx->random, 10);
                        } while (builder.empty() || !start->portalRoomPiece);
                    }};
    }
};

}  // namespace

void registerStronghold() {
    registerStructureType("minecraft:stronghold", [](const json&, StructureEnv&) -> std::unique_ptr<Structure> {
        return std::make_unique<StrongholdStructure>();
    });
}

}  // namespace wg::st
