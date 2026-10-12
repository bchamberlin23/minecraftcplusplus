// Nether fortresses and nether fossils: the game's NetherFortressStructure / NetherFortressPieces (weighted bridge and
// castle piece selection with place limits, bridge end fillers, the blaze spawner throne, nether wart rooms) and
// NetherFossilStructure / NetherFossilPieces (a random fossil template on a floor above the lava sea).
#include "worldgen/structure_template.h"

namespace wg::st {

namespace {

// ---- nether fortress ---------------------------------------------------------------------------------------------

enum class Kind { BridgeStraight, BridgeCrossing, RoomCrossing, StairsRoom, MonsterThrone, CastleEntrance, CastleSmallCorridor,
                  CastleSmallCorridorCrossing, CastleSmallCorridorRightTurn, CastleSmallCorridorLeftTurn, CastleCorridorStairs,
                  CastleCorridorTBalcony, CastleStalkRoom };

struct PieceWeight {
    Kind kind;
    int weight, placeCount = 0, maxPlaceCount;
    bool allowInRow = false;
    bool doPlace(int) const { return maxPlaceCount == 0 || placeCount < maxPlaceCount; }
    bool isValid() const { return maxPlaceCount == 0 || placeCount < maxPlaceCount; }
};

class StartPiece;

class NetherBridgePiece : public Piece {
public:
    NetherBridgePiece(const char* id, int genDepth, const BBox& b) : Piece(id, genDepth, b) {}

protected:
    // Every box of the fortress has the same edge and fill and keeps air.
    void fillBox(PlaceCtx& level, const BBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1, u16 s) {
        generateBox(level, chunkBB, x0, y0, z0, x1, y1, z1, s, s, false);
    }
    // The children of the Java code (their return values are never used).
    void generateChildForward(StartPiece& start, PiecesBuilder& builder, RandomSource& random, int xOff, int yOff, bool isCastle);
    void generateChildLeft(StartPiece& start, PiecesBuilder& builder, RandomSource& random, int yOff, int zOff, bool isCastle);
    void generateChildRight(StartPiece& start, PiecesBuilder& builder, RandomSource& random, int yOff, int zOff, bool isCastle);
    static bool isOkBox(const BBox& b) { return b.minY > 10; }

private:
    void generateAndAddPiece(StartPiece& start, PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d,
                             int depth, bool isCastle);
};

class BridgeCrossing : public NetherBridgePiece {
public:
    BridgeCrossing(int genDepth, const BBox& b, Direction d) : NetherBridgePiece("minecraft:nebcr", genDepth, b) { setOrientation(d); }
    // The start's constructor (the start keeps the bridge crossing's type id, like the game's).
    BridgeCrossing(int west, int north, Direction d)
        : NetherBridgePiece("minecraft:nebcr", 0, makeBoundingBox(west, 64, north, d, 19, 10, 19)) {
        setOrientation(d);
    }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override;
    static NetherBridgePiece* createPiece(PiecesBuilder& builder, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -8, -3, 0, 19, 10, 19, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new BridgeCrossing(depth, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const BlockPos&) override {
        const St nb = B("minecraft:nether_bricks"), air = B("minecraft:air");
        fillBox(level, chunkBB, 7, 3, 0, 11, 4, 18, nb);
        fillBox(level, chunkBB, 0, 3, 7, 18, 4, 11, nb);
        fillBox(level, chunkBB, 8, 5, 0, 10, 7, 18, air);
        fillBox(level, chunkBB, 0, 5, 8, 18, 7, 10, air);
        fillBox(level, chunkBB, 7, 5, 0, 7, 5, 7, nb);
        fillBox(level, chunkBB, 7, 5, 11, 7, 5, 18, nb);
        fillBox(level, chunkBB, 11, 5, 0, 11, 5, 7, nb);
        fillBox(level, chunkBB, 11, 5, 11, 11, 5, 18, nb);
        fillBox(level, chunkBB, 0, 5, 7, 7, 5, 7, nb);
        fillBox(level, chunkBB, 11, 5, 7, 18, 5, 7, nb);
        fillBox(level, chunkBB, 0, 5, 11, 7, 5, 11, nb);
        fillBox(level, chunkBB, 11, 5, 11, 18, 5, 11, nb);
        fillBox(level, chunkBB, 7, 2, 0, 11, 2, 5, nb);
        fillBox(level, chunkBB, 7, 2, 13, 11, 2, 18, nb);
        fillBox(level, chunkBB, 7, 0, 0, 11, 1, 3, nb);
        fillBox(level, chunkBB, 7, 0, 15, 11, 1, 18, nb);
        for (int x = 7; x <= 11; ++x) {
            for (int z = 0; z <= 2; ++z) {
                fillColumnDown(level, nb, x, -1, z, chunkBB);
                fillColumnDown(level, nb, x, -1, 18 - z, chunkBB);
            }
        }
        fillBox(level, chunkBB, 0, 2, 7, 5, 2, 11, nb);
        fillBox(level, chunkBB, 13, 2, 7, 18, 2, 11, nb);
        fillBox(level, chunkBB, 0, 0, 7, 3, 1, 11, nb);
        fillBox(level, chunkBB, 15, 0, 7, 18, 1, 11, nb);
        for (int x = 0; x <= 2; ++x) {
            for (int z = 7; z <= 11; ++z) {
                fillColumnDown(level, nb, x, -1, z, chunkBB);
                fillColumnDown(level, nb, 18 - x, -1, z, chunkBB);
            }
        }
    }
};

class StartPiece final : public BridgeCrossing {
public:
    StartPiece(RandomSource& random, int west, int north) : BridgeCrossing(west, north, randomHorizontal(random)) {
        bridgeWeights = {{Kind::BridgeStraight, 30, 0, 0, true}, {Kind::BridgeCrossing, 10, 0, 4}, {Kind::RoomCrossing, 10, 0, 4},
                         {Kind::StairsRoom, 10, 0, 3},           {Kind::MonsterThrone, 5, 0, 2},  {Kind::CastleEntrance, 5, 0, 1}};
        castleWeights = {{Kind::CastleSmallCorridor, 25, 0, 0, true},         {Kind::CastleSmallCorridorCrossing, 15, 0, 5},
                         {Kind::CastleSmallCorridorRightTurn, 5, 0, 10},      {Kind::CastleSmallCorridorLeftTurn, 5, 0, 10},
                         {Kind::CastleCorridorStairs, 10, 0, 3, true},        {Kind::CastleCorridorTBalcony, 7, 0, 2},
                         {Kind::CastleStalkRoom, 5, 0, 2}};
        for (PieceWeight& w : bridgeWeights) availableBridgePieces.push_back(&w);
        for (PieceWeight& w : castleWeights) availableCastlePieces.push_back(&w);
    }
    // NetherFortressPieces' static weights, per fortress (never resized: the lists point into them).
    std::vector<PieceWeight> bridgeWeights, castleWeights;
    std::vector<PieceWeight*> availableBridgePieces, availableCastlePieces;
    const PieceWeight* previousPiece = nullptr;
    std::vector<Piece*> pendingChildren;
};

void BridgeCrossing::addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) {
    StartPiece& s = static_cast<StartPiece&>(start);
    generateChildForward(s, builder, random, 8, 3, false);
    generateChildLeft(s, builder, random, 3, 8, false);
    generateChildRight(s, builder, random, 3, 8, false);
}

// ---- pieces ----------------------------------------------------------------------------------------------------

class BridgeEndFiller final : public NetherBridgePiece {
public:
    int selfSeed;
    BridgeEndFiller(int genDepth, RandomSource& random, const BBox& b, Direction d) : NetherBridgePiece("minecraft:nebef", genDepth, b) {
        setOrientation(d);
        selfSeed = random.nextInt();
    }
    static NetherBridgePiece* createPiece(PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -1, -3, 0, 5, 10, 8, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new BridgeEndFiller(depth, random, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const BlockPos&) override {
        const St nb = B("minecraft:nether_bricks");
        LegacyRandom selfRandom(selfSeed);  // RandomSource.createThreadLocalInstance(selfSeed)
        for (int x = 0; x <= 4; ++x) {
            for (int y = 3; y <= 4; ++y) {
                int z = selfRandom.nextInt(8);
                fillBox(level, chunkBB, x, y, 0, x, y, z, nb);
            }
        }
        int z = selfRandom.nextInt(8);
        fillBox(level, chunkBB, 0, 5, 0, 0, 5, z, nb);
        z = selfRandom.nextInt(8);
        fillBox(level, chunkBB, 4, 5, 0, 4, 5, z, nb);
        for (int x = 0; x <= 4; ++x) {
            int zx = selfRandom.nextInt(5);
            fillBox(level, chunkBB, x, 2, 0, x, 2, zx, nb);
        }
        for (int x = 0; x <= 4; ++x) {
            for (int y = 0; y <= 1; ++y) {
                int zx = selfRandom.nextInt(3);
                fillBox(level, chunkBB, x, y, 0, x, y, zx, nb);
            }
        }
    }
};

class BridgeStraight final : public NetherBridgePiece {
public:
    BridgeStraight(int genDepth, const BBox& b, Direction d) : NetherBridgePiece("minecraft:nebs", genDepth, b) { setOrientation(d); }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        generateChildForward(static_cast<StartPiece&>(start), builder, random, 1, 3, false);
    }
    static NetherBridgePiece* createPiece(PiecesBuilder& builder, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -1, -3, 0, 5, 10, 19, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new BridgeStraight(depth, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const BlockPos&) override {
        const St nb = B("minecraft:nether_bricks"), air = B("minecraft:air"), fence = B("minecraft:nether_brick_fence");
        fillBox(level, chunkBB, 0, 3, 0, 4, 4, 18, nb);
        fillBox(level, chunkBB, 1, 5, 0, 3, 7, 18, air);
        fillBox(level, chunkBB, 0, 5, 0, 0, 5, 18, nb);
        fillBox(level, chunkBB, 4, 5, 0, 4, 5, 18, nb);
        fillBox(level, chunkBB, 0, 2, 0, 4, 2, 5, nb);
        fillBox(level, chunkBB, 0, 2, 13, 4, 2, 18, nb);
        fillBox(level, chunkBB, 0, 0, 0, 4, 1, 3, nb);
        fillBox(level, chunkBB, 0, 0, 15, 4, 1, 18, nb);
        for (int x = 0; x <= 4; ++x) {
            for (int z = 0; z <= 2; ++z) {
                fillColumnDown(level, nb, x, -1, z, chunkBB);
                fillColumnDown(level, nb, x, -1, 18 - z, chunkBB);
            }
        }
        const St nsFence = fence.with("north", true).with("south", true);
        const St nseFence = nsFence.with("east", true);
        const St nswFence = nsFence.with("west", true);
        fillBox(level, chunkBB, 0, 1, 1, 0, 4, 1, nseFence);
        fillBox(level, chunkBB, 0, 3, 4, 0, 4, 4, nseFence);
        fillBox(level, chunkBB, 0, 3, 14, 0, 4, 14, nseFence);
        fillBox(level, chunkBB, 0, 1, 17, 0, 4, 17, nseFence);
        fillBox(level, chunkBB, 4, 1, 1, 4, 4, 1, nswFence);
        fillBox(level, chunkBB, 4, 3, 4, 4, 4, 4, nswFence);
        fillBox(level, chunkBB, 4, 3, 14, 4, 4, 14, nswFence);
        fillBox(level, chunkBB, 4, 1, 17, 4, 4, 17, nswFence);
    }
};

class CastleCorridorStairsPiece final : public NetherBridgePiece {
public:
    CastleCorridorStairsPiece(int genDepth, const BBox& b, Direction d) : NetherBridgePiece("minecraft:neccs", genDepth, b) {
        setOrientation(d);
    }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        generateChildForward(static_cast<StartPiece&>(start), builder, random, 1, 0, true);
    }
    static NetherBridgePiece* createPiece(PiecesBuilder& builder, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -1, -7, 0, 5, 14, 10, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new CastleCorridorStairsPiece(depth, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const BlockPos&) override {
        const St nb = B("minecraft:nether_bricks"), air = B("minecraft:air"), fence = B("minecraft:nether_brick_fence"),
                 stairBase = B("minecraft:nether_brick_stairs");
        const St stairs = stairBase.with("facing", South);
        const St nsFence = fence.with("north", true).with("south", true);
        for (int step = 0; step <= 9; ++step) {
            int floor = std::max(1, 7 - step);
            int roof = std::min(std::max(floor + 5, 14 - step), 13);
            int z = step;
            fillBox(level, chunkBB, 0, 0, z, 4, floor, z, nb);
            fillBox(level, chunkBB, 1, floor + 1, z, 3, roof - 1, z, air);
            if (step <= 6) {
                placeBlock(level, stairs, 1, floor + 1, z, chunkBB);
                placeBlock(level, stairs, 2, floor + 1, z, chunkBB);
                placeBlock(level, stairs, 3, floor + 1, z, chunkBB);
            }
            fillBox(level, chunkBB, 0, roof, z, 4, roof, z, nb);
            fillBox(level, chunkBB, 0, floor + 1, z, 0, roof - 1, z, nb);
            fillBox(level, chunkBB, 4, floor + 1, z, 4, roof - 1, z, nb);
            if ((step & 1) == 0) {
                fillBox(level, chunkBB, 0, floor + 2, z, 0, floor + 3, z, nsFence);
                fillBox(level, chunkBB, 4, floor + 2, z, 4, floor + 3, z, nsFence);
            }
            for (int x = 0; x <= 4; ++x) {
                fillColumnDown(level, nb, x, -1, z, chunkBB);
            }
        }
    }
};

class CastleCorridorTBalconyPiece final : public NetherBridgePiece {
public:
    CastleCorridorTBalconyPiece(int genDepth, const BBox& b, Direction d) : NetherBridgePiece("minecraft:nectb", genDepth, b) {
        setOrientation(d);
    }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        int zOff = 1;
        const auto o = orientation();
        if (o == West || o == North) zOff = 5;
        StartPiece& s = static_cast<StartPiece&>(start);
        // Each isCastle argument is drawn just before its call (Java argument evaluation).
        const bool leftCastle = random.nextInt(8) > 0;
        generateChildLeft(s, builder, random, 0, zOff, leftCastle);
        const bool rightCastle = random.nextInt(8) > 0;
        generateChildRight(s, builder, random, 0, zOff, rightCastle);
    }
    static NetherBridgePiece* createPiece(PiecesBuilder& builder, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -3, 0, 0, 9, 7, 9, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new CastleCorridorTBalconyPiece(depth, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const BlockPos&) override {
        const St nb = B("minecraft:nether_bricks"), air = B("minecraft:air"), fence = B("minecraft:nether_brick_fence");
        const St nsFence = fence.with("north", true).with("south", true);
        const St weFence = fence.with("west", true).with("east", true);
        fillBox(level, chunkBB, 0, 0, 0, 8, 1, 8, nb);
        fillBox(level, chunkBB, 0, 2, 0, 8, 5, 8, air);
        fillBox(level, chunkBB, 0, 6, 0, 8, 6, 5, nb);
        fillBox(level, chunkBB, 0, 2, 0, 2, 5, 0, nb);
        fillBox(level, chunkBB, 6, 2, 0, 8, 5, 0, nb);
        fillBox(level, chunkBB, 1, 3, 0, 1, 4, 0, weFence);
        fillBox(level, chunkBB, 7, 3, 0, 7, 4, 0, weFence);
        fillBox(level, chunkBB, 0, 2, 4, 8, 2, 8, nb);
        fillBox(level, chunkBB, 1, 1, 4, 2, 2, 4, air);
        fillBox(level, chunkBB, 6, 1, 4, 7, 2, 4, air);
        fillBox(level, chunkBB, 1, 3, 8, 7, 3, 8, weFence);
        placeBlock(level, fence.with("east", true).with("south", true), 0, 3, 8, chunkBB);
        placeBlock(level, fence.with("west", true).with("south", true), 8, 3, 8, chunkBB);
        fillBox(level, chunkBB, 0, 3, 6, 0, 3, 7, nsFence);
        fillBox(level, chunkBB, 8, 3, 6, 8, 3, 7, nsFence);
        fillBox(level, chunkBB, 0, 3, 4, 0, 5, 5, nb);
        fillBox(level, chunkBB, 8, 3, 4, 8, 5, 5, nb);
        fillBox(level, chunkBB, 1, 3, 5, 2, 5, 5, nb);
        fillBox(level, chunkBB, 6, 3, 5, 7, 5, 5, nb);
        fillBox(level, chunkBB, 1, 4, 5, 1, 5, 5, weFence);
        fillBox(level, chunkBB, 7, 4, 5, 7, 5, 5, weFence);
        for (int z = 0; z <= 5; ++z) {
            for (int x = 0; x <= 8; ++x) {
                fillColumnDown(level, nb, x, -1, z, chunkBB);
            }
        }
    }
};

class CastleEntrance final : public NetherBridgePiece {
public:
    CastleEntrance(int genDepth, const BBox& b, Direction d) : NetherBridgePiece("minecraft:nece", genDepth, b) { setOrientation(d); }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        generateChildForward(static_cast<StartPiece&>(start), builder, random, 5, 3, true);
    }
    static NetherBridgePiece* createPiece(PiecesBuilder& builder, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -5, -3, 0, 13, 14, 13, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new CastleEntrance(depth, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const BlockPos&) override {
        const St nb = B("minecraft:nether_bricks"), air = B("minecraft:air"), fence = B("minecraft:nether_brick_fence");
        fillBox(level, chunkBB, 0, 3, 0, 12, 4, 12, nb);
        fillBox(level, chunkBB, 0, 5, 0, 12, 13, 12, air);
        fillBox(level, chunkBB, 0, 5, 0, 1, 12, 12, nb);
        fillBox(level, chunkBB, 11, 5, 0, 12, 12, 12, nb);
        fillBox(level, chunkBB, 2, 5, 11, 4, 12, 12, nb);
        fillBox(level, chunkBB, 8, 5, 11, 10, 12, 12, nb);
        fillBox(level, chunkBB, 5, 9, 11, 7, 12, 12, nb);
        fillBox(level, chunkBB, 2, 5, 0, 4, 12, 1, nb);
        fillBox(level, chunkBB, 8, 5, 0, 10, 12, 1, nb);
        fillBox(level, chunkBB, 5, 9, 0, 7, 12, 1, nb);
        fillBox(level, chunkBB, 2, 11, 2, 10, 12, 10, nb);
        fillBox(level, chunkBB, 5, 8, 0, 7, 8, 0, fence);
        const St weFence = fence.with("west", true).with("east", true);
        const St nsFence = fence.with("north", true).with("south", true);
        for (int i = 1; i <= 11; i += 2) {
            fillBox(level, chunkBB, i, 10, 0, i, 11, 0, weFence);
            fillBox(level, chunkBB, i, 10, 12, i, 11, 12, weFence);
            fillBox(level, chunkBB, 0, 10, i, 0, 11, i, nsFence);
            fillBox(level, chunkBB, 12, 10, i, 12, 11, i, nsFence);
            placeBlock(level, nb, i, 13, 0, chunkBB);
            placeBlock(level, nb, i, 13, 12, chunkBB);
            placeBlock(level, nb, 0, 13, i, chunkBB);
            placeBlock(level, nb, 12, 13, i, chunkBB);
            if (i != 11) {
                placeBlock(level, weFence, i + 1, 13, 0, chunkBB);
                placeBlock(level, weFence, i + 1, 13, 12, chunkBB);
                placeBlock(level, nsFence, 0, 13, i + 1, chunkBB);
                placeBlock(level, nsFence, 12, 13, i + 1, chunkBB);
            }
        }
        placeBlock(level, fence.with("north", true).with("east", true), 0, 13, 0, chunkBB);
        placeBlock(level, fence.with("south", true).with("east", true), 0, 13, 12, chunkBB);
        placeBlock(level, fence.with("south", true).with("west", true), 12, 13, 12, chunkBB);
        placeBlock(level, fence.with("north", true).with("west", true), 12, 13, 0, chunkBB);
        for (int z = 3; z <= 9; z += 2) {
            fillBox(level, chunkBB, 1, 7, z, 1, 8, z, nsFence.with("west", true));
            fillBox(level, chunkBB, 11, 7, z, 11, 8, z, nsFence.with("east", true));
        }
        fillBox(level, chunkBB, 4, 2, 0, 8, 2, 12, nb);
        fillBox(level, chunkBB, 0, 2, 4, 12, 2, 8, nb);
        fillBox(level, chunkBB, 4, 0, 0, 8, 1, 3, nb);
        fillBox(level, chunkBB, 4, 0, 9, 8, 1, 12, nb);
        fillBox(level, chunkBB, 0, 0, 4, 3, 1, 8, nb);
        fillBox(level, chunkBB, 9, 0, 4, 12, 1, 8, nb);
        for (int x = 4; x <= 8; ++x) {
            for (int z = 0; z <= 2; ++z) {
                fillColumnDown(level, nb, x, -1, z, chunkBB);
                fillColumnDown(level, nb, x, -1, 12 - z, chunkBB);
            }
        }
        for (int x = 0; x <= 2; ++x) {
            for (int z = 4; z <= 8; ++z) {
                fillColumnDown(level, nb, x, -1, z, chunkBB);
                fillColumnDown(level, nb, 12 - x, -1, z, chunkBB);
            }
        }
        fillBox(level, chunkBB, 5, 5, 5, 7, 5, 7, nb);
        fillBox(level, chunkBB, 6, 1, 6, 6, 4, 6, air);
        placeBlock(level, nb, 6, 0, 6, chunkBB);
        placeBlock(level, B("minecraft:lava"), 6, 5, 6, chunkBB);
        // (plus a lava tick scheduled at getWorldPos(6, 5, 6) when inside chunkBB: scheduled ticks are not recorded)
    }
};

class CastleSmallCorridorCrossingPiece final : public NetherBridgePiece {
public:
    CastleSmallCorridorCrossingPiece(int genDepth, const BBox& b, Direction d) : NetherBridgePiece("minecraft:nescsc", genDepth, b) {
        setOrientation(d);
    }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        StartPiece& s = static_cast<StartPiece&>(start);
        generateChildForward(s, builder, random, 1, 0, true);
        generateChildLeft(s, builder, random, 0, 1, true);
        generateChildRight(s, builder, random, 0, 1, true);
    }
    static NetherBridgePiece* createPiece(PiecesBuilder& builder, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -1, 0, 0, 5, 7, 5, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new CastleSmallCorridorCrossingPiece(depth, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const BlockPos&) override {
        const St nb = B("minecraft:nether_bricks"), air = B("minecraft:air");
        fillBox(level, chunkBB, 0, 0, 0, 4, 1, 4, nb);
        fillBox(level, chunkBB, 0, 2, 0, 4, 5, 4, air);
        fillBox(level, chunkBB, 0, 2, 0, 0, 5, 0, nb);
        fillBox(level, chunkBB, 4, 2, 0, 4, 5, 0, nb);
        fillBox(level, chunkBB, 0, 2, 4, 0, 5, 4, nb);
        fillBox(level, chunkBB, 4, 2, 4, 4, 5, 4, nb);
        fillBox(level, chunkBB, 0, 6, 0, 4, 6, 4, nb);
        for (int x = 0; x <= 4; ++x) {
            for (int z = 0; z <= 4; ++z) {
                fillColumnDown(level, nb, x, -1, z, chunkBB);
            }
        }
    }
};

class CastleSmallCorridorLeftTurnPiece final : public NetherBridgePiece {
public:
    bool isNeedingChest;
    CastleSmallCorridorLeftTurnPiece(int genDepth, RandomSource& random, const BBox& b, Direction d)
        : NetherBridgePiece("minecraft:nesclt", genDepth, b) {
        setOrientation(d);
        isNeedingChest = random.nextInt(3) == 0;
    }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        generateChildLeft(static_cast<StartPiece&>(start), builder, random, 0, 1, true);
    }
    static NetherBridgePiece* createPiece(PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -1, 0, 0, 5, 7, 5, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new CastleSmallCorridorLeftTurnPiece(depth, random, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        const St nb = B("minecraft:nether_bricks"), air = B("minecraft:air"), fence = B("minecraft:nether_brick_fence");
        fillBox(level, chunkBB, 0, 0, 0, 4, 1, 4, nb);
        fillBox(level, chunkBB, 0, 2, 0, 4, 5, 4, air);
        const St weFence = fence.with("west", true).with("east", true);
        const St nsFence = fence.with("north", true).with("south", true);
        fillBox(level, chunkBB, 4, 2, 0, 4, 5, 4, nb);
        fillBox(level, chunkBB, 4, 3, 1, 4, 4, 1, nsFence);
        fillBox(level, chunkBB, 4, 3, 3, 4, 4, 3, nsFence);
        fillBox(level, chunkBB, 0, 2, 0, 0, 5, 0, nb);
        fillBox(level, chunkBB, 0, 2, 4, 3, 5, 4, nb);
        fillBox(level, chunkBB, 1, 3, 4, 1, 4, 4, weFence);
        fillBox(level, chunkBB, 3, 3, 4, 3, 4, 4, weFence);
        if (isNeedingChest && chunkBB.isInside(getWorldPos(3, 2, 3))) {
            isNeedingChest = false;
            createChest(level, chunkBB, random, 3, 2, 3, "minecraft:chests/nether_bridge");
        }
        fillBox(level, chunkBB, 0, 6, 0, 4, 6, 4, nb);
        for (int x = 0; x <= 4; ++x) {
            for (int z = 0; z <= 4; ++z) {
                fillColumnDown(level, nb, x, -1, z, chunkBB);
            }
        }
    }
};

class CastleSmallCorridorPiece final : public NetherBridgePiece {
public:
    CastleSmallCorridorPiece(int genDepth, const BBox& b, Direction d) : NetherBridgePiece("minecraft:nesc", genDepth, b) {
        setOrientation(d);
    }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        generateChildForward(static_cast<StartPiece&>(start), builder, random, 1, 0, true);
    }
    static NetherBridgePiece* createPiece(PiecesBuilder& builder, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -1, 0, 0, 5, 7, 5, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new CastleSmallCorridorPiece(depth, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const BlockPos&) override {
        const St nb = B("minecraft:nether_bricks"), air = B("minecraft:air"), fence = B("minecraft:nether_brick_fence");
        fillBox(level, chunkBB, 0, 0, 0, 4, 1, 4, nb);
        fillBox(level, chunkBB, 0, 2, 0, 4, 5, 4, air);
        const St nsFence = fence.with("north", true).with("south", true);
        fillBox(level, chunkBB, 0, 2, 0, 0, 5, 4, nb);
        fillBox(level, chunkBB, 4, 2, 0, 4, 5, 4, nb);
        fillBox(level, chunkBB, 0, 3, 1, 0, 4, 1, nsFence);
        fillBox(level, chunkBB, 0, 3, 3, 0, 4, 3, nsFence);
        fillBox(level, chunkBB, 4, 3, 1, 4, 4, 1, nsFence);
        fillBox(level, chunkBB, 4, 3, 3, 4, 4, 3, nsFence);
        fillBox(level, chunkBB, 0, 6, 0, 4, 6, 4, nb);
        for (int x = 0; x <= 4; ++x) {
            for (int z = 0; z <= 4; ++z) {
                fillColumnDown(level, nb, x, -1, z, chunkBB);
            }
        }
    }
};

class CastleSmallCorridorRightTurnPiece final : public NetherBridgePiece {
public:
    bool isNeedingChest;
    CastleSmallCorridorRightTurnPiece(int genDepth, RandomSource& random, const BBox& b, Direction d)
        : NetherBridgePiece("minecraft:nescrt", genDepth, b) {
        setOrientation(d);
        isNeedingChest = random.nextInt(3) == 0;
    }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        generateChildRight(static_cast<StartPiece&>(start), builder, random, 0, 1, true);
    }
    static NetherBridgePiece* createPiece(PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -1, 0, 0, 5, 7, 5, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new CastleSmallCorridorRightTurnPiece(depth, random, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        const St nb = B("minecraft:nether_bricks"), air = B("minecraft:air"), fence = B("minecraft:nether_brick_fence");
        fillBox(level, chunkBB, 0, 0, 0, 4, 1, 4, nb);
        fillBox(level, chunkBB, 0, 2, 0, 4, 5, 4, air);
        const St weFence = fence.with("west", true).with("east", true);
        const St nsFence = fence.with("north", true).with("south", true);
        fillBox(level, chunkBB, 0, 2, 0, 0, 5, 4, nb);
        fillBox(level, chunkBB, 0, 3, 1, 0, 4, 1, nsFence);
        fillBox(level, chunkBB, 0, 3, 3, 0, 4, 3, nsFence);
        fillBox(level, chunkBB, 4, 2, 0, 4, 5, 0, nb);
        fillBox(level, chunkBB, 1, 2, 4, 4, 5, 4, nb);
        fillBox(level, chunkBB, 1, 3, 4, 1, 4, 4, weFence);
        fillBox(level, chunkBB, 3, 3, 4, 3, 4, 4, weFence);
        if (isNeedingChest && chunkBB.isInside(getWorldPos(1, 2, 3))) {
            isNeedingChest = false;
            createChest(level, chunkBB, random, 1, 2, 3, "minecraft:chests/nether_bridge");
        }
        fillBox(level, chunkBB, 0, 6, 0, 4, 6, 4, nb);
        for (int x = 0; x <= 4; ++x) {
            for (int z = 0; z <= 4; ++z) {
                fillColumnDown(level, nb, x, -1, z, chunkBB);
            }
        }
    }
};

class CastleStalkRoom final : public NetherBridgePiece {
public:
    CastleStalkRoom(int genDepth, const BBox& b, Direction d) : NetherBridgePiece("minecraft:necsr", genDepth, b) { setOrientation(d); }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        StartPiece& s = static_cast<StartPiece&>(start);
        generateChildForward(s, builder, random, 5, 3, true);
        generateChildForward(s, builder, random, 5, 11, true);
    }
    static NetherBridgePiece* createPiece(PiecesBuilder& builder, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -5, -3, 0, 13, 14, 13, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new CastleStalkRoom(depth, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const BlockPos&) override {
        const St nb = B("minecraft:nether_bricks"), air = B("minecraft:air"), fence = B("minecraft:nether_brick_fence"),
                 stairBase = B("minecraft:nether_brick_stairs");
        fillBox(level, chunkBB, 0, 3, 0, 12, 4, 12, nb);
        fillBox(level, chunkBB, 0, 5, 0, 12, 13, 12, air);
        fillBox(level, chunkBB, 0, 5, 0, 1, 12, 12, nb);
        fillBox(level, chunkBB, 11, 5, 0, 12, 12, 12, nb);
        fillBox(level, chunkBB, 2, 5, 11, 4, 12, 12, nb);
        fillBox(level, chunkBB, 8, 5, 11, 10, 12, 12, nb);
        fillBox(level, chunkBB, 5, 9, 11, 7, 12, 12, nb);
        fillBox(level, chunkBB, 2, 5, 0, 4, 12, 1, nb);
        fillBox(level, chunkBB, 8, 5, 0, 10, 12, 1, nb);
        fillBox(level, chunkBB, 5, 9, 0, 7, 12, 1, nb);
        fillBox(level, chunkBB, 2, 11, 2, 10, 12, 10, nb);
        const St weFence = fence.with("west", true).with("east", true);
        const St nsFence = fence.with("north", true).with("south", true);
        const St nswFence = nsFence.with("west", true);
        const St nseFence = nsFence.with("east", true);
        for (int i = 1; i <= 11; i += 2) {
            fillBox(level, chunkBB, i, 10, 0, i, 11, 0, weFence);
            fillBox(level, chunkBB, i, 10, 12, i, 11, 12, weFence);
            fillBox(level, chunkBB, 0, 10, i, 0, 11, i, nsFence);
            fillBox(level, chunkBB, 12, 10, i, 12, 11, i, nsFence);
            placeBlock(level, nb, i, 13, 0, chunkBB);
            placeBlock(level, nb, i, 13, 12, chunkBB);
            placeBlock(level, nb, 0, 13, i, chunkBB);
            placeBlock(level, nb, 12, 13, i, chunkBB);
            if (i != 11) {
                placeBlock(level, weFence, i + 1, 13, 0, chunkBB);
                placeBlock(level, weFence, i + 1, 13, 12, chunkBB);
                placeBlock(level, nsFence, 0, 13, i + 1, chunkBB);
                placeBlock(level, nsFence, 12, 13, i + 1, chunkBB);
            }
        }
        placeBlock(level, fence.with("north", true).with("east", true), 0, 13, 0, chunkBB);
        placeBlock(level, fence.with("south", true).with("east", true), 0, 13, 12, chunkBB);
        placeBlock(level, fence.with("south", true).with("west", true), 12, 13, 12, chunkBB);
        placeBlock(level, fence.with("north", true).with("west", true), 12, 13, 0, chunkBB);
        for (int z = 3; z <= 9; z += 2) {
            fillBox(level, chunkBB, 1, 7, z, 1, 8, z, nswFence);
            fillBox(level, chunkBB, 11, 7, z, 11, 8, z, nseFence);
        }
        const St stairs = stairBase.with("facing", North);
        for (int i = 0; i <= 6; ++i) {
            int z = i + 4;
            for (int x = 5; x <= 7; ++x) {
                placeBlock(level, stairs, x, 5 + i, z, chunkBB);
            }
            if (z >= 5 && z <= 8) {
                fillBox(level, chunkBB, 5, 5, z, 7, i + 4, z, nb);
            } else if (z >= 9 && z <= 10) {
                fillBox(level, chunkBB, 5, 8, z, 7, i + 4, z, nb);
            }
            if (i >= 1) {
                fillBox(level, chunkBB, 5, 6 + i, z, 7, 9 + i, z, air);
            }
        }
        for (int x = 5; x <= 7; ++x) {
            placeBlock(level, stairs, x, 12, 11, chunkBB);
        }
        fillBox(level, chunkBB, 5, 6, 7, 5, 7, 7, nseFence);
        fillBox(level, chunkBB, 7, 6, 7, 7, 7, 7, nswFence);
        fillBox(level, chunkBB, 5, 13, 12, 7, 13, 12, air);
        fillBox(level, chunkBB, 2, 5, 2, 3, 5, 3, nb);
        fillBox(level, chunkBB, 2, 5, 9, 3, 5, 10, nb);
        fillBox(level, chunkBB, 2, 5, 4, 2, 5, 8, nb);
        fillBox(level, chunkBB, 9, 5, 2, 10, 5, 3, nb);
        fillBox(level, chunkBB, 9, 5, 9, 10, 5, 10, nb);
        fillBox(level, chunkBB, 10, 5, 4, 10, 5, 8, nb);
        const St eastStairs = stairs.with("facing", East);
        const St westStairs = stairs.with("facing", West);
        placeBlock(level, westStairs, 4, 5, 2, chunkBB);
        placeBlock(level, westStairs, 4, 5, 3, chunkBB);
        placeBlock(level, westStairs, 4, 5, 9, chunkBB);
        placeBlock(level, westStairs, 4, 5, 10, chunkBB);
        placeBlock(level, eastStairs, 8, 5, 2, chunkBB);
        placeBlock(level, eastStairs, 8, 5, 3, chunkBB);
        placeBlock(level, eastStairs, 8, 5, 9, chunkBB);
        placeBlock(level, eastStairs, 8, 5, 10, chunkBB);
        fillBox(level, chunkBB, 3, 4, 4, 4, 4, 8, B("minecraft:soul_sand"));
        fillBox(level, chunkBB, 8, 4, 4, 9, 4, 8, B("minecraft:soul_sand"));
        fillBox(level, chunkBB, 3, 5, 4, 4, 5, 8, B("minecraft:nether_wart"));
        fillBox(level, chunkBB, 8, 5, 4, 9, 5, 8, B("minecraft:nether_wart"));
        fillBox(level, chunkBB, 4, 2, 0, 8, 2, 12, nb);
        fillBox(level, chunkBB, 0, 2, 4, 12, 2, 8, nb);
        fillBox(level, chunkBB, 4, 0, 0, 8, 1, 3, nb);
        fillBox(level, chunkBB, 4, 0, 9, 8, 1, 12, nb);
        fillBox(level, chunkBB, 0, 0, 4, 3, 1, 8, nb);
        fillBox(level, chunkBB, 9, 0, 4, 12, 1, 8, nb);
        for (int x = 4; x <= 8; ++x) {
            for (int z = 0; z <= 2; ++z) {
                fillColumnDown(level, nb, x, -1, z, chunkBB);
                fillColumnDown(level, nb, x, -1, 12 - z, chunkBB);
            }
        }
        for (int x = 0; x <= 2; ++x) {
            for (int z = 4; z <= 8; ++z) {
                fillColumnDown(level, nb, x, -1, z, chunkBB);
                fillColumnDown(level, nb, 12 - x, -1, z, chunkBB);
            }
        }
    }
};

class MonsterThrone final : public NetherBridgePiece {
public:
    bool hasPlacedSpawner = false;
    MonsterThrone(int genDepth, const BBox& b, Direction d) : NetherBridgePiece("minecraft:nemt", genDepth, b) { setOrientation(d); }
    static NetherBridgePiece* createPiece(PiecesBuilder& builder, int x, int y, int z, int depth, Direction d) {
        const BBox b = BBox::orient(x, y, z, -2, 0, 0, 7, 8, 9, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new MonsterThrone(depth, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const BlockPos&) override {
        const St nb = B("minecraft:nether_bricks"), air = B("minecraft:air"), fence = B("minecraft:nether_brick_fence");
        fillBox(level, chunkBB, 0, 2, 0, 6, 7, 7, air);
        fillBox(level, chunkBB, 1, 0, 0, 5, 1, 7, nb);
        fillBox(level, chunkBB, 1, 2, 1, 5, 2, 7, nb);
        fillBox(level, chunkBB, 1, 3, 2, 5, 3, 7, nb);
        fillBox(level, chunkBB, 1, 4, 3, 5, 4, 7, nb);
        fillBox(level, chunkBB, 1, 2, 0, 1, 4, 2, nb);
        fillBox(level, chunkBB, 5, 2, 0, 5, 4, 2, nb);
        fillBox(level, chunkBB, 1, 5, 2, 1, 5, 3, nb);
        fillBox(level, chunkBB, 5, 5, 2, 5, 5, 3, nb);
        fillBox(level, chunkBB, 0, 5, 3, 0, 5, 8, nb);
        fillBox(level, chunkBB, 6, 5, 3, 6, 5, 8, nb);
        fillBox(level, chunkBB, 1, 5, 8, 5, 5, 8, nb);
        const St weFence = fence.with("west", true).with("east", true);
        const St nsFence = fence.with("north", true).with("south", true);
        placeBlock(level, fence.with("west", true), 1, 6, 3, chunkBB);
        placeBlock(level, fence.with("east", true), 5, 6, 3, chunkBB);
        placeBlock(level, fence.with("east", true).with("north", true), 0, 6, 3, chunkBB);
        placeBlock(level, fence.with("west", true).with("north", true), 6, 6, 3, chunkBB);
        fillBox(level, chunkBB, 0, 6, 4, 0, 6, 7, nsFence);
        fillBox(level, chunkBB, 6, 6, 4, 6, 6, 7, nsFence);
        placeBlock(level, fence.with("east", true).with("south", true), 0, 6, 8, chunkBB);
        placeBlock(level, fence.with("west", true).with("south", true), 6, 6, 8, chunkBB);
        fillBox(level, chunkBB, 1, 6, 8, 5, 6, 8, weFence);
        placeBlock(level, fence.with("east", true), 1, 7, 8, chunkBB);
        fillBox(level, chunkBB, 2, 7, 8, 4, 7, 8, weFence);
        placeBlock(level, fence.with("west", true), 5, 7, 8, chunkBB);
        placeBlock(level, fence.with("east", true), 2, 8, 8, chunkBB);
        placeBlock(level, weFence, 3, 8, 8, chunkBB);
        placeBlock(level, fence.with("west", true), 4, 8, 8, chunkBB);
        if (!hasPlacedSpawner) {
            const BlockPos pos = getWorldPos(3, 5, 5);
            if (chunkBB.isInside(pos)) {
                hasPlacedSpawner = true;
                level.set(pos, B("minecraft:spawner"));
                level.setSpawner(pos, "minecraft:blaze");  // SpawnerBlockEntity.setEntityId (no randomness)
            }
        }
        for (int x = 0; x <= 6; ++x) {
            for (int z = 0; z <= 6; ++z) {
                fillColumnDown(level, nb, x, -1, z, chunkBB);
            }
        }
    }
};

class RoomCrossing final : public NetherBridgePiece {
public:
    RoomCrossing(int genDepth, const BBox& b, Direction d) : NetherBridgePiece("minecraft:nerc", genDepth, b) { setOrientation(d); }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        StartPiece& s = static_cast<StartPiece&>(start);
        generateChildForward(s, builder, random, 2, 0, false);
        generateChildLeft(s, builder, random, 0, 2, false);
        generateChildRight(s, builder, random, 0, 2, false);
    }
    static NetherBridgePiece* createPiece(PiecesBuilder& builder, int x, int y, int z, Direction d, int depth) {
        const BBox b = BBox::orient(x, y, z, -2, 0, 0, 7, 9, 7, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new RoomCrossing(depth, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const BlockPos&) override {
        const St nb = B("minecraft:nether_bricks"), air = B("minecraft:air"), fence = B("minecraft:nether_brick_fence");
        fillBox(level, chunkBB, 0, 0, 0, 6, 1, 6, nb);
        fillBox(level, chunkBB, 0, 2, 0, 6, 7, 6, air);
        fillBox(level, chunkBB, 0, 2, 0, 1, 6, 0, nb);
        fillBox(level, chunkBB, 0, 2, 6, 1, 6, 6, nb);
        fillBox(level, chunkBB, 5, 2, 0, 6, 6, 0, nb);
        fillBox(level, chunkBB, 5, 2, 6, 6, 6, 6, nb);
        fillBox(level, chunkBB, 0, 2, 0, 0, 6, 1, nb);
        fillBox(level, chunkBB, 0, 2, 5, 0, 6, 6, nb);
        fillBox(level, chunkBB, 6, 2, 0, 6, 6, 1, nb);
        fillBox(level, chunkBB, 6, 2, 5, 6, 6, 6, nb);
        const St weFence = fence.with("west", true).with("east", true);
        const St nsFence = fence.with("north", true).with("south", true);
        fillBox(level, chunkBB, 2, 6, 0, 4, 6, 0, nb);
        fillBox(level, chunkBB, 2, 5, 0, 4, 5, 0, weFence);
        fillBox(level, chunkBB, 2, 6, 6, 4, 6, 6, nb);
        fillBox(level, chunkBB, 2, 5, 6, 4, 5, 6, weFence);
        fillBox(level, chunkBB, 0, 6, 2, 0, 6, 4, nb);
        fillBox(level, chunkBB, 0, 5, 2, 0, 5, 4, nsFence);
        fillBox(level, chunkBB, 6, 6, 2, 6, 6, 4, nb);
        fillBox(level, chunkBB, 6, 5, 2, 6, 5, 4, nsFence);
        for (int x = 0; x <= 6; ++x) {
            for (int z = 0; z <= 6; ++z) {
                fillColumnDown(level, nb, x, -1, z, chunkBB);
            }
        }
    }
};

class StairsRoom final : public NetherBridgePiece {
public:
    StairsRoom(int genDepth, const BBox& b, Direction d) : NetherBridgePiece("minecraft:nesr", genDepth, b) { setOrientation(d); }
    void addChildren(Piece& start, PiecesBuilder& builder, RandomSource& random) override {
        generateChildRight(static_cast<StartPiece&>(start), builder, random, 6, 2, false);
    }
    static NetherBridgePiece* createPiece(PiecesBuilder& builder, int x, int y, int z, int depth, Direction d) {
        const BBox b = BBox::orient(x, y, z, -2, 0, 0, 7, 11, 7, d);
        return isOkBox(b) && !builder.findCollisionPiece(b) ? new StairsRoom(depth, b, d) : nullptr;
    }
    void postProcess(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const BlockPos&) override {
        const St nb = B("minecraft:nether_bricks"), air = B("minecraft:air"), fence = B("minecraft:nether_brick_fence");
        fillBox(level, chunkBB, 0, 0, 0, 6, 1, 6, nb);
        fillBox(level, chunkBB, 0, 2, 0, 6, 10, 6, air);
        fillBox(level, chunkBB, 0, 2, 0, 1, 8, 0, nb);
        fillBox(level, chunkBB, 5, 2, 0, 6, 8, 0, nb);
        fillBox(level, chunkBB, 0, 2, 1, 0, 8, 6, nb);
        fillBox(level, chunkBB, 6, 2, 1, 6, 8, 6, nb);
        fillBox(level, chunkBB, 1, 2, 6, 5, 8, 6, nb);
        const St weFence = fence.with("west", true).with("east", true);
        const St nsFence = fence.with("north", true).with("south", true);
        fillBox(level, chunkBB, 0, 3, 2, 0, 5, 4, nsFence);
        fillBox(level, chunkBB, 6, 3, 2, 6, 5, 2, nsFence);
        fillBox(level, chunkBB, 6, 3, 4, 6, 5, 4, nsFence);
        placeBlock(level, nb, 5, 2, 5, chunkBB);
        fillBox(level, chunkBB, 4, 2, 5, 4, 3, 5, nb);
        fillBox(level, chunkBB, 3, 2, 5, 3, 4, 5, nb);
        fillBox(level, chunkBB, 2, 2, 5, 2, 5, 5, nb);
        fillBox(level, chunkBB, 1, 2, 5, 1, 6, 5, nb);
        fillBox(level, chunkBB, 1, 7, 1, 5, 7, 4, nb);
        fillBox(level, chunkBB, 6, 8, 2, 6, 8, 4, air);
        fillBox(level, chunkBB, 2, 6, 0, 4, 8, 0, nb);
        fillBox(level, chunkBB, 2, 5, 0, 4, 5, 0, weFence);
        for (int x = 0; x <= 6; ++x) {
            for (int z = 0; z <= 6; ++z) {
                fillColumnDown(level, nb, x, -1, z, chunkBB);
            }
        }
    }
};

// ---- generation ------------------------------------------------------------------------------------------------

NetherBridgePiece* findAndCreateBridgePieceFactory(Kind k, PiecesBuilder& builder, RandomSource& random, int x, int y, int z, Direction d,
                                                   int depth) {
    switch (k) {
    case Kind::BridgeStraight: return BridgeStraight::createPiece(builder, x, y, z, d, depth);
    case Kind::BridgeCrossing: return BridgeCrossing::createPiece(builder, x, y, z, d, depth);
    case Kind::RoomCrossing: return RoomCrossing::createPiece(builder, x, y, z, d, depth);
    case Kind::StairsRoom: return StairsRoom::createPiece(builder, x, y, z, depth, d);
    case Kind::MonsterThrone: return MonsterThrone::createPiece(builder, x, y, z, depth, d);
    case Kind::CastleEntrance: return CastleEntrance::createPiece(builder, x, y, z, d, depth);
    case Kind::CastleSmallCorridor: return CastleSmallCorridorPiece::createPiece(builder, x, y, z, d, depth);
    case Kind::CastleSmallCorridorRightTurn: return CastleSmallCorridorRightTurnPiece::createPiece(builder, random, x, y, z, d, depth);
    case Kind::CastleSmallCorridorLeftTurn: return CastleSmallCorridorLeftTurnPiece::createPiece(builder, random, x, y, z, d, depth);
    case Kind::CastleCorridorStairs: return CastleCorridorStairsPiece::createPiece(builder, x, y, z, d, depth);
    case Kind::CastleCorridorTBalcony: return CastleCorridorTBalconyPiece::createPiece(builder, x, y, z, d, depth);
    case Kind::CastleSmallCorridorCrossing: return CastleSmallCorridorCrossingPiece::createPiece(builder, x, y, z, d, depth);
    case Kind::CastleStalkRoom: return CastleStalkRoom::createPiece(builder, x, y, z, d, depth);
    }
    return nullptr;
}

// NetherBridgePiece.updatePieceWeight: the total weight, or -1 when no limited piece can be placed any more.
int updatePieceWeight(const std::vector<PieceWeight*>& currentPieces) {
    bool hasAnyPieces = false;
    int totalWeight = 0;
    for (const PieceWeight* p : currentPieces) {
        if (p->maxPlaceCount > 0 && p->placeCount < p->maxPlaceCount) hasAnyPieces = true;
        totalWeight += p->weight;
    }
    return hasAnyPieces ? totalWeight : -1;
}

NetherBridgePiece* generatePiece(StartPiece& start, std::vector<PieceWeight*>& currentPieces, PiecesBuilder& builder, RandomSource& random,
                                 int x, int y, int z, Direction d, int depth) {
    const int totalWeight = updatePieceWeight(currentPieces);
    const bool doStuff = totalWeight > 0 && depth <= 30;
    for (int numAttempts = 0; numAttempts < 5 && doStuff;) {
        ++numAttempts;
        int weightSelection = random.nextInt(totalWeight);
        for (size_t i = 0; i < currentPieces.size(); ++i) {
            PieceWeight* piece = currentPieces[i];
            weightSelection -= piece->weight;
            if (weightSelection < 0) {
                if (!piece->doPlace(depth) || (piece == start.previousPiece && !piece->allowInRow)) break;
                NetherBridgePiece* p = findAndCreateBridgePieceFactory(piece->kind, builder, random, x, y, z, d, depth);
                if (p) {
                    ++piece->placeCount;
                    start.previousPiece = piece;
                    if (!piece->isValid()) currentPieces.erase(currentPieces.begin() + std::ptrdiff_t(i));
                    return p;
                }
            }
        }
    }
    return BridgeEndFiller::createPiece(builder, random, x, y, z, d, depth);
}

void NetherBridgePiece::generateAndAddPiece(StartPiece& start, PiecesBuilder& builder, RandomSource& random, int x, int y, int z,
                                            Direction d, int depth, bool isCastle) {
    if (std::abs(x - start.box.minX) <= 112 && std::abs(z - start.box.minZ) <= 112) {
        std::vector<PieceWeight*>& availablePieces = isCastle ? start.availableCastlePieces : start.availableBridgePieces;
        Piece* p = generatePiece(start, availablePieces, builder, random, x, y, z, d, depth + 1);
        if (p) {
            builder.add(std::unique_ptr<Piece>(p));
            start.pendingChildren.push_back(p);
        }
    } else {
        // An end filler that is not added (its constructor still draws its seed).
        delete BridgeEndFiller::createPiece(builder, random, x, y, z, d, depth);
    }
}

void NetherBridgePiece::generateChildForward(StartPiece& start, PiecesBuilder& builder, RandomSource& random, int xOff, int yOff,
                                             bool isCastle) {
    const auto o = orientation();
    if (!o) return;
    switch (*o) {
    case North: generateAndAddPiece(start, builder, random, box.minX + xOff, box.minY + yOff, box.minZ - 1, *o, genDepth, isCastle); break;
    case South: generateAndAddPiece(start, builder, random, box.minX + xOff, box.minY + yOff, box.maxZ + 1, *o, genDepth, isCastle); break;
    case West: generateAndAddPiece(start, builder, random, box.minX - 1, box.minY + yOff, box.minZ + xOff, *o, genDepth, isCastle); break;
    case East: generateAndAddPiece(start, builder, random, box.maxX + 1, box.minY + yOff, box.minZ + xOff, *o, genDepth, isCastle); break;
    default: break;
    }
}

void NetherBridgePiece::generateChildLeft(StartPiece& start, PiecesBuilder& builder, RandomSource& random, int yOff, int zOff,
                                          bool isCastle) {
    const auto o = orientation();
    if (!o) return;
    switch (*o) {
    case North:
    case South: generateAndAddPiece(start, builder, random, box.minX - 1, box.minY + yOff, box.minZ + zOff, West, genDepth, isCastle); break;
    case West:
    case East: generateAndAddPiece(start, builder, random, box.minX + zOff, box.minY + yOff, box.minZ - 1, North, genDepth, isCastle); break;
    default: break;
    }
}

void NetherBridgePiece::generateChildRight(StartPiece& start, PiecesBuilder& builder, RandomSource& random, int yOff, int zOff,
                                           bool isCastle) {
    const auto o = orientation();
    if (!o) return;
    switch (*o) {
    case North:
    case South: generateAndAddPiece(start, builder, random, box.maxX + 1, box.minY + yOff, box.minZ + zOff, East, genDepth, isCastle); break;
    case West:
    case East: generateAndAddPiece(start, builder, random, box.minX + zOff, box.minY + yOff, box.maxZ + 1, South, genDepth, isCastle); break;
    default: break;
    }
}

class NetherFortressStructure final : public Structure {
public:
    std::optional<Stub> findGenerationPoint(GenCtx& c) const override {
        GenCtx* ctx = &c;
        return Stub{{c.minBlockX(), 64, c.minBlockZ()}, [ctx](PiecesBuilder& builder) {
                        auto sp = std::make_unique<StartPiece>(ctx->random, ctx->minBlockX() + 2, ctx->minBlockZ() + 2);
                        StartPiece* start = sp.get();
                        builder.add(std::move(sp));
                        start->addChildren(*start, builder, ctx->random);
                        while (!start->pendingChildren.empty()) {
                            const int pos = ctx->random.nextInt(int(start->pendingChildren.size()));
                            Piece* p = start->pendingChildren[size_t(pos)];
                            start->pendingChildren.erase(start->pendingChildren.begin() + pos);
                            p->addChildren(*start, builder, ctx->random);
                        }
                        builder.moveInsideHeights(ctx->random, 48, 70);
                    }};
    }
};

// ---- nether fossil ---------------------------------------------------------------------------------------------

class NetherFossilPiece final : public TemplatePiece {
public:
    static PlaceSettings makeSettings(const StructureEnv& env, Rot r) {
        PlaceSettings s;
        s.rotation = r;
        s.mirror = Mir::None;
        s.processors.push_back(blockIgnore(env, {"minecraft:air", "minecraft:structure_block"}));  // STRUCTURE_AND_AIR
        return s;
    }
    NetherFossilPiece(const StructureEnv& env, const std::string& location, const BlockPos& position, Rot r)
        : TemplatePiece("minecraft:nefos", 0, env.templates->get(location), makeSettings(env, r), position) {}

    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos& referencePos) override {
        const BBox fossilBB = templateBox(templatePosition, settings.rotation, settings.pivot, settings.mirror, tmpl->size);
        // The game grows the (per start) chunk box to the whole fossil; writes outside the region are still refused.
        BBox bb = chunkBB;
        bb.encapsulate(fossilBB);
        TemplatePiece::postProcess(level, random, bb, referencePos);
        placeDriedGhast(level, fossilBB, bb);
    }

protected:
    void handleDataMarker(PlaceCtx&, const std::string&, const BlockPos&, RandomSource&, const BBox&) override {}

private:
    void placeDriedGhast(PlaceCtx& level, const BBox& fossilBB, const BBox& chunkBB) {
        // RandomSource.createThreadLocalInstance(level seed).forkPositional().at(center)
        LegacyRandom base(level.seed());
        const BlockPos center = fossilBB.center();
        const auto positionalRandom = base.forkPositional()->at(center.x, center.y, center.z);
        if (positionalRandom->nextFloat() < 0.5f) {
            const int x = fossilBB.minX + positionalRandom->nextInt(fossilBB.xSpan());
            const int y = fossilBB.minY;
            const int z = fossilBB.minZ + positionalRandom->nextInt(fossilBB.zSpan());
            const BlockPos randomPos{x, y, z};
            if (level.get(randomPos).isAir() && chunkBB.isInside(randomPos))
                level.set(randomPos, B("minecraft:dried_ghast").rotate(randomRotation(*positionalRandom)));
        }
    }
};

class NetherFossilStructure final : public Structure {
public:
    NetherFossilStructure(const json& def, StructureEnv& env) {
        if (const FeatureEnv* fe = env.featureEnv()) height_ = fe->heightProvider(def.at("height"));
    }
    std::optional<Stub> findGenerationPoint(GenCtx& c) const override {
        WorldgenRandom& random = c.random;
        const int blockX = c.minBlockX() + random.nextInt(16);
        const int blockZ = c.minBlockZ() + random.nextInt(16);
        const int seaLevel = c.seaLevel();
        // WorldGenerationContext(generator, heightAccessor)
        const int genMinY = std::max(c.minY(), c.env.gen.genMinY());
        const int genDepth = std::min(c.env.height(), c.env.gen.genDepth());
        int y = height_ ? height_->sample(random, genMinY, genDepth) : 0;
        const std::vector<u16>& column = c.baseColumn(blockX, blockZ);
        const SB& b = c.env.b;
        auto getBlock = [&](int blockY) {  // NoiseColumn.getBlock
            const int i = blockY - c.minY();
            return i >= 0 && i < int(column.size()) ? column[size_t(i)] : b.air;
        };
        while (y > seaLevel) {
            const u16 current = getBlock(y);
            const u16 below = getBlock(--y);
            if (b.isAir(current) && (b.is(below, "minecraft:soul_sand") || c.env.facts.isFaceSturdy(below, Up))) break;
        }
        if (y <= seaLevel) return std::nullopt;
        const BlockPos position{blockX, y, blockZ};
        GenCtx* ctx = &c;
        return Stub{position, [ctx, position](PiecesBuilder& builder) {
                        // NetherFossilPieces.addPieces
                        const Rot nextRotation = randomRotation(ctx->random);
                        const std::string location = "minecraft:nether_fossils/fossil_" + std::to_string(ctx->random.nextInt(14) + 1);
                        builder.add(std::make_unique<NetherFossilPiece>(ctx->env, location, position, nextRotation));
                    }};
    }

private:
    std::unique_ptr<HeightProvider> height_;
};

}  // namespace

void registerNetherStructures() {
    registerStructureType("minecraft:fortress", [](const json&, StructureEnv&) -> std::unique_ptr<Structure> {
        return std::make_unique<NetherFortressStructure>();
    });
    registerStructureType("minecraft:nether_fossil", [](const json& def, StructureEnv& env) -> std::unique_ptr<Structure> {
        return std::make_unique<NetherFossilStructure>(def, env);
    });
}

}  // namespace wg::st
