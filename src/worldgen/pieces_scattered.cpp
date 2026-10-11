// Scattered structures: desert pyramids, jungle temples, swamp huts, igloos and buried treasure (the game's
// DesertPyramidPiece / Structure, JungleTemplePiece, SwampHutPiece, IglooPieces and BuriedTreasurePieces).
#include "worldgen/structure_template.h"

#include <algorithm>

namespace wg::st {

namespace {

// ---- desert pyramid ---------------------------------------------------------------------------------------

class DesertPyramidPiece final : public ScatteredPiece {
public:
    DesertPyramidPiece(RandomSource& random, int west, int north)
        : ScatteredPiece("minecraft:tedp", west, 64, north, 21, 15, 21, randomHorizontal(random)) {}

    bool hasPlacedChest[4] = {};
    std::vector<BlockPos> potentialSuspiciousSandWorldPositions;
    BlockPos randomCollapsedRoofPos{0, 0, 0};

    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos& referencePos) override {
        (void)referencePos;
        if (updateHeightPositionToLowestGroundHeight(level, -random.nextInt(3))) {
        generateBox(
        level, chunkBB, 0, -4, 0, width - 1, 0, depth - 1, B("minecraft:sandstone"), B("minecraft:sandstone"), false
        );

        for (int pos = 1; pos <= 9; pos++) {
        generateBox(
        level,
        chunkBB,
        pos,
        pos,
        pos,
        width - 1 - pos,
        pos,
        depth - 1 - pos,
        B("minecraft:sandstone"),
        B("minecraft:sandstone"),
        false
        );
        generateBox(
        level,
        chunkBB,
        pos + 1,
        pos,
        pos + 1,
        width - 2 - pos,
        pos,
        depth - 2 - pos,
        B("minecraft:air"),
        B("minecraft:air"),
        false
        );
        }

        for (int x = 0; x < width; x++) {
        for (int z = 0; z < depth; z++) {
        int startY = -5;
        fillColumnDown(level, B("minecraft:sandstone"), x, -5, z, chunkBB);
        }
        }

        St northStairs = B("minecraft:sandstone_stairs").with("facing", North);
        St southStairs = B("minecraft:sandstone_stairs").with("facing", South);
        St eastStairs = B("minecraft:sandstone_stairs").with("facing", East);
        St westStairs = B("minecraft:sandstone_stairs").with("facing", West);
        generateBox(level, chunkBB, 0, 0, 0, 4, 9, 4, B("minecraft:sandstone"), B("minecraft:air"), false);
        generateBox(level, chunkBB, 1, 10, 1, 3, 10, 3, B("minecraft:sandstone"), B("minecraft:sandstone"), false);
        placeBlock(level, northStairs, 2, 10, 0, chunkBB);
        placeBlock(level, southStairs, 2, 10, 4, chunkBB);
        placeBlock(level, eastStairs, 0, 10, 2, chunkBB);
        placeBlock(level, westStairs, 4, 10, 2, chunkBB);
        generateBox(
        level, chunkBB, width - 5, 0, 0, width - 1, 9, 4, B("minecraft:sandstone"), B("minecraft:air"), false
        );
        generateBox(
        level, chunkBB, width - 4, 10, 1, width - 2, 10, 3, B("minecraft:sandstone"), B("minecraft:sandstone"), false
        );
        placeBlock(level, northStairs, width - 3, 10, 0, chunkBB);
        placeBlock(level, southStairs, width - 3, 10, 4, chunkBB);
        placeBlock(level, eastStairs, width - 5, 10, 2, chunkBB);
        placeBlock(level, westStairs, width - 1, 10, 2, chunkBB);
        generateBox(level, chunkBB, 8, 0, 0, 12, 4, 4, B("minecraft:sandstone"), B("minecraft:air"), false);
        generateBox(level, chunkBB, 9, 1, 0, 11, 3, 4, B("minecraft:air"), B("minecraft:air"), false);
        placeBlock(level, B("minecraft:cut_sandstone"), 9, 1, 1, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), 9, 2, 1, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), 9, 3, 1, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), 10, 3, 1, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), 11, 3, 1, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), 11, 2, 1, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), 11, 1, 1, chunkBB);
        generateBox(level, chunkBB, 4, 1, 1, 8, 3, 3, B("minecraft:sandstone"), B("minecraft:air"), false);
        generateBox(level, chunkBB, 4, 1, 2, 8, 2, 2, B("minecraft:air"), B("minecraft:air"), false);
        generateBox(level, chunkBB, 12, 1, 1, 16, 3, 3, B("minecraft:sandstone"), B("minecraft:air"), false);
        generateBox(level, chunkBB, 12, 1, 2, 16, 2, 2, B("minecraft:air"), B("minecraft:air"), false);
        generateBox(
        level, chunkBB, 5, 4, 5, width - 6, 4, depth - 6, B("minecraft:sandstone"), B("minecraft:sandstone"), false
        );
        generateBox(level, chunkBB, 9, 4, 9, 11, 4, 11, B("minecraft:air"), B("minecraft:air"), false);
        generateBox(level, chunkBB, 8, 1, 8, 8, 3, 8, B("minecraft:cut_sandstone"), B("minecraft:cut_sandstone"), false);
        generateBox(level, chunkBB, 12, 1, 8, 12, 3, 8, B("minecraft:cut_sandstone"), B("minecraft:cut_sandstone"), false);
        generateBox(level, chunkBB, 8, 1, 12, 8, 3, 12, B("minecraft:cut_sandstone"), B("minecraft:cut_sandstone"), false);
        generateBox(level, chunkBB, 12, 1, 12, 12, 3, 12, B("minecraft:cut_sandstone"), B("minecraft:cut_sandstone"), false);
        generateBox(level, chunkBB, 1, 1, 5, 4, 4, 11, B("minecraft:sandstone"), B("minecraft:sandstone"), false);
        generateBox(
        level, chunkBB, width - 5, 1, 5, width - 2, 4, 11, B("minecraft:sandstone"), B("minecraft:sandstone"), false
        );
        generateBox(level, chunkBB, 6, 7, 9, 6, 7, 11, B("minecraft:sandstone"), B("minecraft:sandstone"), false);
        generateBox(
        level, chunkBB, width - 7, 7, 9, width - 7, 7, 11, B("minecraft:sandstone"), B("minecraft:sandstone"), false
        );
        generateBox(level, chunkBB, 5, 5, 9, 5, 7, 11, B("minecraft:cut_sandstone"), B("minecraft:cut_sandstone"), false);
        generateBox(
        level,
        chunkBB,
        width - 6,
        5,
        9,
        width - 6,
        7,
        11,
        B("minecraft:cut_sandstone"),
        B("minecraft:cut_sandstone"),
        false
        );
        placeBlock(level, B("minecraft:air"), 5, 5, 10, chunkBB);
        placeBlock(level, B("minecraft:air"), 5, 6, 10, chunkBB);
        placeBlock(level, B("minecraft:air"), 6, 6, 10, chunkBB);
        placeBlock(level, B("minecraft:air"), width - 6, 5, 10, chunkBB);
        placeBlock(level, B("minecraft:air"), width - 6, 6, 10, chunkBB);
        placeBlock(level, B("minecraft:air"), width - 7, 6, 10, chunkBB);
        generateBox(level, chunkBB, 2, 4, 4, 2, 6, 4, B("minecraft:air"), B("minecraft:air"), false);
        generateBox(level, chunkBB, width - 3, 4, 4, width - 3, 6, 4, B("minecraft:air"), B("minecraft:air"), false);
        placeBlock(level, northStairs, 2, 4, 5, chunkBB);
        placeBlock(level, northStairs, 2, 3, 4, chunkBB);
        placeBlock(level, northStairs, width - 3, 4, 5, chunkBB);
        placeBlock(level, northStairs, width - 3, 3, 4, chunkBB);
        generateBox(level, chunkBB, 1, 1, 3, 2, 2, 3, B("minecraft:sandstone"), B("minecraft:sandstone"), false);
        generateBox(
        level, chunkBB, width - 3, 1, 3, width - 2, 2, 3, B("minecraft:sandstone"), B("minecraft:sandstone"), false
        );
        placeBlock(level, B("minecraft:sandstone"), 1, 1, 2, chunkBB);
        placeBlock(level, B("minecraft:sandstone"), width - 2, 1, 2, chunkBB);
        placeBlock(level, B("minecraft:sandstone_slab"), 1, 2, 2, chunkBB);
        placeBlock(level, B("minecraft:sandstone_slab"), width - 2, 2, 2, chunkBB);
        placeBlock(level, westStairs, 2, 1, 2, chunkBB);
        placeBlock(level, eastStairs, width - 3, 1, 2, chunkBB);
        generateBox(level, chunkBB, 4, 3, 5, 4, 3, 17, B("minecraft:sandstone"), B("minecraft:sandstone"), false);
        generateBox(
        level, chunkBB, width - 5, 3, 5, width - 5, 3, 17, B("minecraft:sandstone"), B("minecraft:sandstone"), false
        );
        generateBox(level, chunkBB, 3, 1, 5, 4, 2, 16, B("minecraft:air"), B("minecraft:air"), false);
        generateBox(level, chunkBB, width - 6, 1, 5, width - 5, 2, 16, B("minecraft:air"), B("minecraft:air"), false);

        for (int z = 5; z <= 17; z += 2) {
        placeBlock(level, B("minecraft:cut_sandstone"), 4, 1, z, chunkBB);
        placeBlock(level, B("minecraft:chiseled_sandstone"), 4, 2, z, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), width - 5, 1, z, chunkBB);
        placeBlock(level, B("minecraft:chiseled_sandstone"), width - 5, 2, z, chunkBB);
        }

        placeBlock(level, B("minecraft:orange_terracotta"), 10, 0, 7, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), 10, 0, 8, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), 9, 0, 9, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), 11, 0, 9, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), 8, 0, 10, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), 12, 0, 10, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), 7, 0, 10, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), 13, 0, 10, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), 9, 0, 11, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), 11, 0, 11, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), 10, 0, 12, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), 10, 0, 13, chunkBB);
        placeBlock(level, B("minecraft:blue_terracotta"), 10, 0, 10, chunkBB);

        for (int x = 0; x <= width - 1; x += width - 1) {
        placeBlock(level, B("minecraft:cut_sandstone"), x, 2, 1, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x, 2, 2, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), x, 2, 3, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), x, 3, 1, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x, 3, 2, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), x, 3, 3, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x, 4, 1, chunkBB);
        placeBlock(level, B("minecraft:chiseled_sandstone"), x, 4, 2, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x, 4, 3, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), x, 5, 1, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x, 5, 2, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), x, 5, 3, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x, 6, 1, chunkBB);
        placeBlock(level, B("minecraft:chiseled_sandstone"), x, 6, 2, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x, 6, 3, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x, 7, 1, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x, 7, 2, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x, 7, 3, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), x, 8, 1, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), x, 8, 2, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), x, 8, 3, chunkBB);
        }

        for (int x = 2; x <= width - 3; x += width - 3 - 2) {
        placeBlock(level, B("minecraft:cut_sandstone"), x - 1, 2, 0, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x, 2, 0, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), x + 1, 2, 0, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), x - 1, 3, 0, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x, 3, 0, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), x + 1, 3, 0, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x - 1, 4, 0, chunkBB);
        placeBlock(level, B("minecraft:chiseled_sandstone"), x, 4, 0, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x + 1, 4, 0, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), x - 1, 5, 0, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x, 5, 0, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), x + 1, 5, 0, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x - 1, 6, 0, chunkBB);
        placeBlock(level, B("minecraft:chiseled_sandstone"), x, 6, 0, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x + 1, 6, 0, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x - 1, 7, 0, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x, 7, 0, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), x + 1, 7, 0, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), x - 1, 8, 0, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), x, 8, 0, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), x + 1, 8, 0, chunkBB);
        }

        generateBox(level, chunkBB, 8, 4, 0, 12, 6, 0, B("minecraft:cut_sandstone"), B("minecraft:cut_sandstone"), false);
        placeBlock(level, B("minecraft:air"), 8, 6, 0, chunkBB);
        placeBlock(level, B("minecraft:air"), 12, 6, 0, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), 9, 5, 0, chunkBB);
        placeBlock(level, B("minecraft:chiseled_sandstone"), 10, 5, 0, chunkBB);
        placeBlock(level, B("minecraft:orange_terracotta"), 11, 5, 0, chunkBB);
        generateBox(level, chunkBB, 8, -14, 8, 12, -11, 12, B("minecraft:cut_sandstone"), B("minecraft:cut_sandstone"), false);
        generateBox(
        level, chunkBB, 8, -10, 8, 12, -10, 12, B("minecraft:chiseled_sandstone"), B("minecraft:chiseled_sandstone"), false
        );
        generateBox(level, chunkBB, 8, -9, 8, 12, -9, 12, B("minecraft:cut_sandstone"), B("minecraft:cut_sandstone"), false);
        generateBox(level, chunkBB, 8, -8, 8, 12, -1, 12, B("minecraft:sandstone"), B("minecraft:sandstone"), false);
        generateBox(level, chunkBB, 9, -11, 9, 11, -1, 11, B("minecraft:air"), B("minecraft:air"), false);
        placeBlock(level, B("minecraft:stone_pressure_plate"), 10, -11, 10, chunkBB);
        generateBox(level, chunkBB, 9, -13, 9, 11, -13, 11, B("minecraft:tnt"), B("minecraft:air"), false);
        placeBlock(level, B("minecraft:air"), 8, -11, 10, chunkBB);
        placeBlock(level, B("minecraft:air"), 8, -10, 10, chunkBB);
        placeBlock(level, B("minecraft:chiseled_sandstone"), 7, -10, 10, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), 7, -11, 10, chunkBB);
        placeBlock(level, B("minecraft:air"), 12, -11, 10, chunkBB);
        placeBlock(level, B("minecraft:air"), 12, -10, 10, chunkBB);
        placeBlock(level, B("minecraft:chiseled_sandstone"), 13, -10, 10, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), 13, -11, 10, chunkBB);
        placeBlock(level, B("minecraft:air"), 10, -11, 8, chunkBB);
        placeBlock(level, B("minecraft:air"), 10, -10, 8, chunkBB);
        placeBlock(level, B("minecraft:chiseled_sandstone"), 10, -10, 7, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), 10, -11, 7, chunkBB);
        placeBlock(level, B("minecraft:air"), 10, -11, 12, chunkBB);
        placeBlock(level, B("minecraft:air"), 10, -10, 12, chunkBB);
        placeBlock(level, B("minecraft:chiseled_sandstone"), 10, -10, 13, chunkBB);
        placeBlock(level, B("minecraft:cut_sandstone"), 10, -11, 13, chunkBB);

        for (Direction direction : kPlaneHorizontal) {
        if (!hasPlacedChest[to2D(direction)]) {
        int xo = stepX(direction) * 2;
        int zo = stepZ(direction) * 2;
        hasPlacedChest[to2D(direction)] = createChest(
        level, chunkBB, random, 10 + xo, -11, 10 + zo);
        }
        }

        addCellar(level, chunkBB);
        }
    }

private:
    void addCellar(PlaceCtx& level, const BBox& chunkBB) {
    BlockPos roomCenter = BlockPos{16, -4, 13};
    addCellarStairs(roomCenter, level, chunkBB);
    addCellarRoom(roomCenter, level, chunkBB);
    }

    void addCellarStairs(BlockPos roomCenter, PlaceCtx& level, const BBox& chunkBB) {
    int x = roomCenter.x;
    int y = roomCenter.y;
    int z = roomCenter.z;
    St sandStoneStairs = B("minecraft:sandstone_stairs");
    placeBlock(level, sandStoneStairs.rotate(Rot::CCW90), 13, -1, 17, chunkBB);
    placeBlock(level, sandStoneStairs.rotate(Rot::CCW90), 14, -2, 17, chunkBB);
    placeBlock(level, sandStoneStairs.rotate(Rot::CCW90), 15, -3, 17, chunkBB);
    St sand = B("minecraft:sand");
    St sandStone = B("minecraft:sandstone");
    bool variant = level.random().nextBoolean();
    placeBlock(level, sand, x - 4, y + 4, z + 4, chunkBB);
    placeBlock(level, sand, x - 3, y + 4, z + 4, chunkBB);
    placeBlock(level, sand, x - 2, y + 4, z + 4, chunkBB);
    placeBlock(level, sand, x - 1, y + 4, z + 4, chunkBB);
    placeBlock(level, sand, x, y + 4, z + 4, chunkBB);
    placeBlock(level, sand, x - 2, y + 3, z + 4, chunkBB);
    placeBlock(level, variant ? sand : sandStone, x - 1, y + 3, z + 4, chunkBB);
    placeBlock(level, !variant ? sand : sandStone, x, y + 3, z + 4, chunkBB);
    placeBlock(level, sand, x - 1, y + 2, z + 4, chunkBB);
    placeBlock(level, sandStone, x, y + 2, z + 4, chunkBB);
    placeBlock(level, sand, x, y + 1, z + 4, chunkBB);
    }

    void addCellarRoom(BlockPos roomCenter, PlaceCtx& level, const BBox& chunkBB) {
    int x = roomCenter.x;
    int y = roomCenter.y;
    int z = roomCenter.z;
    St cutSandStone = B("minecraft:cut_sandstone");
    St hieroglyphsSandStone = B("minecraft:chiseled_sandstone");
    generateBox(level, chunkBB, x - 3, y + 1, z - 3, x - 3, y + 1, z + 2, cutSandStone, cutSandStone, true);
    generateBox(level, chunkBB, x + 3, y + 1, z - 3, x + 3, y + 1, z + 2, cutSandStone, cutSandStone, true);
    generateBox(level, chunkBB, x - 3, y + 1, z - 3, x + 3, y + 1, z - 2, cutSandStone, cutSandStone, true);
    generateBox(level, chunkBB, x - 3, y + 1, z + 3, x + 3, y + 1, z + 3, cutSandStone, cutSandStone, true);
    generateBox(level, chunkBB, x - 3, y + 2, z - 3, x - 3, y + 2, z + 2, hieroglyphsSandStone, hieroglyphsSandStone, true);
    generateBox(level, chunkBB, x + 3, y + 2, z - 3, x + 3, y + 2, z + 2, hieroglyphsSandStone, hieroglyphsSandStone, true);
    generateBox(level, chunkBB, x - 3, y + 2, z - 3, x + 3, y + 2, z - 2, hieroglyphsSandStone, hieroglyphsSandStone, true);
    generateBox(level, chunkBB, x - 3, y + 2, z + 3, x + 3, y + 2, z + 3, hieroglyphsSandStone, hieroglyphsSandStone, true);
    generateBox(level, chunkBB, x - 3, -1, z - 3, x - 3, -1, z + 2, cutSandStone, cutSandStone, true);
    generateBox(level, chunkBB, x + 3, -1, z - 3, x + 3, -1, z + 2, cutSandStone, cutSandStone, true);
    generateBox(level, chunkBB, x - 3, -1, z - 3, x + 3, -1, z - 2, cutSandStone, cutSandStone, true);
    generateBox(level, chunkBB, x - 3, -1, z + 3, x + 3, -1, z + 3, cutSandStone, cutSandStone, true);
    placeSandBox(x - 2, y + 1, z - 2, x + 2, y + 3, z + 2);
    placeCollapsedRoof(level, chunkBB, x - 2, y + 4, z - 2, x + 2, z + 2);
    St orangeTeracotta = B("minecraft:orange_terracotta");
    St blueTeracotta = B("minecraft:blue_terracotta");
    placeBlock(level, blueTeracotta, x, y, z, chunkBB);
    placeBlock(level, orangeTeracotta, x + 1, y, z - 1, chunkBB);
    placeBlock(level, orangeTeracotta, x + 1, y, z + 1, chunkBB);
    placeBlock(level, orangeTeracotta, x - 1, y, z - 1, chunkBB);
    placeBlock(level, orangeTeracotta, x - 1, y, z + 1, chunkBB);
    placeBlock(level, orangeTeracotta, x + 2, y, z, chunkBB);
    placeBlock(level, orangeTeracotta, x - 2, y, z, chunkBB);
    placeBlock(level, orangeTeracotta, x, y, z + 2, chunkBB);
    placeBlock(level, orangeTeracotta, x, y, z - 2, chunkBB);
    placeBlock(level, orangeTeracotta, x + 3, y, z, chunkBB);
    placeSand(x + 3, y + 1, z);
    placeSand(x + 3, y + 2, z);
    placeBlock(level, cutSandStone, x + 4, y + 1, z, chunkBB);
    placeBlock(level, hieroglyphsSandStone, x + 4, y + 2, z, chunkBB);
    placeBlock(level, orangeTeracotta, x - 3, y, z, chunkBB);
    placeSand(x - 3, y + 1, z);
    placeSand(x - 3, y + 2, z);
    placeBlock(level, cutSandStone, x - 4, y + 1, z, chunkBB);
    placeBlock(level, hieroglyphsSandStone, x - 4, y + 2, z, chunkBB);
    placeBlock(level, orangeTeracotta, x, y, z + 3, chunkBB);
    placeSand(x, y + 1, z + 3);
    placeSand(x, y + 2, z + 3);
    placeBlock(level, orangeTeracotta, x, y, z - 3, chunkBB);
    placeSand(x, y + 1, z - 3);
    placeSand(x, y + 2, z - 3);
    placeBlock(level, cutSandStone, x, y + 1, z - 4, chunkBB);
    placeBlock(level, hieroglyphsSandStone, x, -2, z - 4, chunkBB);
    }

    void placeSand(int x, int y, int z) {
    BlockPos worldPos = getWorldPos(x, y, z);
    potentialSuspiciousSandWorldPositions.push_back(worldPos);
    }

    void placeSandBox(int x0, int y0, int z0, int x1, int y1, int z1) {
    for (int y = y0; y <= y1; y++) {
    for (int x = x0; x <= x1; x++) {
    for (int z = z0; z <= z1; z++) {
    placeSand(x, y, z);
    }
    }
    }
    }

    void placeCollapsedRoofPiece(PlaceCtx& level, int x, int y, int z, const BBox& chunkBB) {
    if (level.random().nextFloat() < 0.33F) {
    St blockState = B("minecraft:sandstone");
    placeBlock(level, blockState, x, y, z, chunkBB);
    } else {
    St blockState = B("minecraft:sand");
    placeBlock(level, blockState, x, y, z, chunkBB);
    }
    }

    void placeCollapsedRoof(PlaceCtx& level, const BBox& chunkBB, int x0, int y0, int z0, int x1, int z1) {
    for (int x = x0; x <= x1; x++) {
    for (int z = z0; z <= z1; z++) {
    placeCollapsedRoofPiece(level, x, y0, z, chunkBB);
    }
    }

    LegacyRandom base(level.seed());
    const auto factory = base.forkPositional();
    const BlockPos at = getWorldPos(x0, y0, z0);
    const auto randomPtr = factory->at(at.x, at.y, at.z);
    RandomSource& random = *randomPtr;
    int roofPosX = random.nextIntBetweenInclusive(x0, x1);
    int roofPosZ = random.nextIntBetweenInclusive(z0, z1);
    randomCollapsedRoofPos = BlockPos{getWorldX(roofPosX, roofPosZ), getWorldY(y0), getWorldZ(roofPosX, roofPosZ)};
    }
};

class DesertPyramidStructure final : public Structure {
public:
    std::optional<Stub> findGenerationPoint(GenCtx& c) const override {
        // SinglePieceStructure
        if (!c.couldValidBiomeExistOnTopOfChunkCenter()) return std::nullopt;
        if (c.lowestY(21, 21) < c.seaLevel()) return std::nullopt;
        const int x = c.middleBlockX(), z = c.middleBlockZ();
        GenCtx* ctx = &c;
        return Stub{{x, c.firstOccupiedHeight(x, z, HType::WorldSurfaceWG), z}, [ctx](PiecesBuilder& b) {
                        b.add(std::make_unique<DesertPyramidPiece>(ctx->random, ctx->minBlockX(), ctx->minBlockZ()));
                    }};
    }

    void afterPlace(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const std::vector<std::unique_ptr<Piece>>& pieces) const override {
        std::vector<BlockPos> unique;  // SortedArraySet by Vec3i.compareTo (y, then z, then x)
        auto less = [](const BlockPos& a, const BlockPos& b) {
            if (a.y != b.y) return a.y < b.y;
            if (a.z != b.z) return a.z < b.z;
            return a.x < b.x;
        };
        auto placeSuspiciousSand = [&](const BlockPos& p) {
            if (chunkBB.isInside(p)) level.set(p, B("minecraft:suspicious_sand"));
        };
        for (const auto& piece : pieces)
            if (auto* dp = dynamic_cast<DesertPyramidPiece*>(piece.get())) {
                unique.insert(unique.end(), dp->potentialSuspiciousSandWorldPositions.begin(), dp->potentialSuspiciousSandWorldPositions.end());
                placeSuspiciousSand(dp->randomCollapsedRoofPos);
            }
        std::sort(unique.begin(), unique.end(), less);
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        std::vector<BlockPos> shuffled = unique;
        BBox all = pieces.front()->box;
        for (const auto& p : pieces) all.encapsulate(p->box);
        const BlockPos centre = all.center();
        LegacyRandom base(level.seed());
        const auto factory = base.forkPositional();
        const auto r = factory->at(centre.x, centre.y, centre.z);
        for (size_t i = shuffled.size(); i > 1; --i) std::swap(shuffled[i - 1], shuffled[size_t(r->nextInt(int(i)))]);
        int toPlace = std::min(int(unique.size()), r->nextInt(5, 8));
        for (const BlockPos& p : shuffled) {
            if (toPlace > 0) {
                --toPlace;
                placeSuspiciousSand(p);
            } else if (chunkBB.isInside(p)) {
                level.set(p, B("minecraft:sand"));
            }
        }
    }
};

// ---- jungle temple ------------------------------------------------------------------------------------------

class JungleTemplePiece final : public ScatteredPiece {
public:
    JungleTemplePiece(RandomSource& random, int west, int north)
        : ScatteredPiece("minecraft:tejp", west, 64, north, 12, 10, 15, randomHorizontal(random)) {}

    bool placedMainChest = false, placedHiddenChest = false, placedTrap1 = false, placedTrap2 = false;

    struct MossStoneSelector final : BlockSelector {
        void select(RandomSource& random, int, int, int, bool) override {
            next = random.nextFloat() < 0.4f ? B("minecraft:cobblestone") : B("minecraft:mossy_cobblestone");
        }
    };

    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos& referencePos) override {
        (void)referencePos;
        MossStoneSelector STONE_SELECTOR;
        if (updateAverageGroundHeight(level, chunkBB, 0)) {
        generateBox(level, chunkBB, 0, -4, 0, width - 1, 0, depth - 1, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 2, 1, 2, 9, 2, 2, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 2, 1, 12, 9, 2, 12, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 2, 1, 3, 2, 2, 11, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 9, 1, 3, 9, 2, 11, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 1, 3, 1, 10, 6, 1, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 1, 3, 13, 10, 6, 13, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 1, 3, 2, 1, 6, 12, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 10, 3, 2, 10, 6, 12, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 2, 3, 2, 9, 3, 12, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 2, 6, 2, 9, 6, 12, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 3, 7, 3, 8, 7, 11, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 4, 8, 4, 7, 8, 10, false, random, STONE_SELECTOR);
        generateAirBox(level, chunkBB, 3, 1, 3, 8, 2, 11);
        generateAirBox(level, chunkBB, 4, 3, 6, 7, 3, 9);
        generateAirBox(level, chunkBB, 2, 4, 2, 9, 5, 12);
        generateAirBox(level, chunkBB, 4, 6, 5, 7, 6, 9);
        generateAirBox(level, chunkBB, 5, 7, 6, 6, 7, 8);
        generateAirBox(level, chunkBB, 5, 1, 2, 6, 2, 2);
        generateAirBox(level, chunkBB, 5, 2, 12, 6, 2, 12);
        generateAirBox(level, chunkBB, 5, 5, 1, 6, 5, 1);
        generateAirBox(level, chunkBB, 5, 5, 13, 6, 5, 13);
        placeBlock(level, B("minecraft:air"), 1, 5, 5, chunkBB);
        placeBlock(level, B("minecraft:air"), 10, 5, 5, chunkBB);
        placeBlock(level, B("minecraft:air"), 1, 5, 9, chunkBB);
        placeBlock(level, B("minecraft:air"), 10, 5, 9, chunkBB);

        for (int z = 0; z <= 14; z += 14) {
        generateBox(level, chunkBB, 2, 4, z, 2, 5, z, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 4, 4, z, 4, 5, z, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 7, 4, z, 7, 5, z, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 9, 4, z, 9, 5, z, false, random, STONE_SELECTOR);
        }

        generateBox(level, chunkBB, 5, 6, 0, 6, 6, 0, false, random, STONE_SELECTOR);

        for (int x = 0; x <= 11; x += 11) {
        for (int z = 2; z <= 12; z += 2) {
        generateBox(level, chunkBB, x, 4, z, x, 5, z, false, random, STONE_SELECTOR);
        }

        generateBox(level, chunkBB, x, 6, 5, x, 6, 5, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, x, 6, 9, x, 6, 9, false, random, STONE_SELECTOR);
        }

        generateBox(level, chunkBB, 2, 7, 2, 2, 9, 2, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 9, 7, 2, 9, 9, 2, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 2, 7, 12, 2, 9, 12, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 9, 7, 12, 9, 9, 12, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 4, 9, 4, 4, 9, 4, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 7, 9, 4, 7, 9, 4, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 4, 9, 10, 4, 9, 10, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 7, 9, 10, 7, 9, 10, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 5, 9, 7, 6, 9, 7, false, random, STONE_SELECTOR);
        St eastStairs = B("minecraft:cobblestone_stairs").with("facing", East);
        St westStairs = B("minecraft:cobblestone_stairs").with("facing", West);
        St southStairs = B("minecraft:cobblestone_stairs").with("facing", South);
        St northStairs = B("minecraft:cobblestone_stairs").with("facing", North);
        placeBlock(level, northStairs, 5, 9, 6, chunkBB);
        placeBlock(level, northStairs, 6, 9, 6, chunkBB);
        placeBlock(level, southStairs, 5, 9, 8, chunkBB);
        placeBlock(level, southStairs, 6, 9, 8, chunkBB);
        placeBlock(level, northStairs, 4, 0, 0, chunkBB);
        placeBlock(level, northStairs, 5, 0, 0, chunkBB);
        placeBlock(level, northStairs, 6, 0, 0, chunkBB);
        placeBlock(level, northStairs, 7, 0, 0, chunkBB);
        placeBlock(level, northStairs, 4, 1, 8, chunkBB);
        placeBlock(level, northStairs, 4, 2, 9, chunkBB);
        placeBlock(level, northStairs, 4, 3, 10, chunkBB);
        placeBlock(level, northStairs, 7, 1, 8, chunkBB);
        placeBlock(level, northStairs, 7, 2, 9, chunkBB);
        placeBlock(level, northStairs, 7, 3, 10, chunkBB);
        generateBox(level, chunkBB, 4, 1, 9, 4, 1, 9, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 7, 1, 9, 7, 1, 9, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 4, 1, 10, 7, 2, 10, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 5, 4, 5, 6, 4, 5, false, random, STONE_SELECTOR);
        placeBlock(level, eastStairs, 4, 4, 5, chunkBB);
        placeBlock(level, westStairs, 7, 4, 5, chunkBB);

        for (int i = 0; i < 4; i++) {
        placeBlock(level, southStairs, 5, 0 - i, 6 + i, chunkBB);
        placeBlock(level, southStairs, 6, 0 - i, 6 + i, chunkBB);
        generateAirBox(level, chunkBB, 5, 0 - i, 7 + i, 6, 0 - i, 9 + i);
        }

        generateAirBox(level, chunkBB, 1, -3, 12, 10, -1, 13);
        generateAirBox(level, chunkBB, 1, -3, 1, 3, -1, 13);
        generateAirBox(level, chunkBB, 1, -3, 1, 9, -1, 5);

        for (int z = 1; z <= 13; z += 2) {
        generateBox(level, chunkBB, 1, -3, z, 1, -2, z, false, random, STONE_SELECTOR);
        }

        for (int z = 2; z <= 12; z += 2) {
        generateBox(level, chunkBB, 1, -1, z, 3, -1, z, false, random, STONE_SELECTOR);
        }

        generateBox(level, chunkBB, 2, -2, 1, 5, -2, 1, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 7, -2, 1, 9, -2, 1, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 6, -3, 1, 6, -3, 1, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 6, -1, 1, 6, -1, 1, false, random, STONE_SELECTOR);
        placeBlock(
        level,
        B("minecraft:tripwire_hook").with("facing", East).with("attached", true),
        1,
        -3,
        8,
        chunkBB
        );
        placeBlock(
        level,
        B("minecraft:tripwire_hook").with("facing", West).with("attached", true),
        4,
        -3,
        8,
        chunkBB
        );
        placeBlock(
        level,
        B("minecraft:tripwire").with("east", true).with("west", true).with("attached", true),
        2,
        -3,
        8,
        chunkBB
        );
        placeBlock(
        level,
        B("minecraft:tripwire").with("east", true).with("west", true).with("attached", true),
        3,
        -3,
        8,
        chunkBB
        );
        St redstoneWireNS = B("minecraft:redstone_wire")
        .with("north", "side")
        .with("south", "side");
        placeBlock(level, redstoneWireNS, 5, -3, 7, chunkBB);
        placeBlock(level, redstoneWireNS, 5, -3, 6, chunkBB);
        placeBlock(level, redstoneWireNS, 5, -3, 5, chunkBB);
        placeBlock(level, redstoneWireNS, 5, -3, 4, chunkBB);
        placeBlock(level, redstoneWireNS, 5, -3, 3, chunkBB);
        placeBlock(level, redstoneWireNS, 5, -3, 2, chunkBB);
        placeBlock(
        level,
        B("minecraft:redstone_wire").with("north", "side").with("west", "side"),
        5,
        -3,
        1,
        chunkBB
        );
        placeBlock(
        level,
        B("minecraft:redstone_wire").with("east", "side").with("west", "side"),
        4,
        -3,
        1,
        chunkBB
        );
        placeBlock(level, B("minecraft:mossy_cobblestone"), 3, -3, 1, chunkBB);
        if (!placedTrap1) {
        placedTrap1 = createDispenser(level, chunkBB, random, 3, -2, 1, North);
        }

        placeBlock(level, B("minecraft:vine").with("south", true), 3, -2, 2, chunkBB);
        placeBlock(
        level,
        B("minecraft:tripwire_hook").with("facing", North).with("attached", true),
        7,
        -3,
        1,
        chunkBB
        );
        placeBlock(
        level,
        B("minecraft:tripwire_hook").with("facing", South).with("attached", true),
        7,
        -3,
        5,
        chunkBB
        );
        placeBlock(
        level,
        B("minecraft:tripwire").with("north", true).with("south", true).with("attached", true),
        7,
        -3,
        2,
        chunkBB
        );
        placeBlock(
        level,
        B("minecraft:tripwire").with("north", true).with("south", true).with("attached", true),
        7,
        -3,
        3,
        chunkBB
        );
        placeBlock(
        level,
        B("minecraft:tripwire").with("north", true).with("south", true).with("attached", true),
        7,
        -3,
        4,
        chunkBB
        );
        placeBlock(
        level,
        B("minecraft:redstone_wire").with("east", "side").with("west", "side"),
        8,
        -3,
        6,
        chunkBB
        );
        placeBlock(
        level,
        B("minecraft:redstone_wire").with("west", "side").with("south", "side"),
        9,
        -3,
        6,
        chunkBB
        );
        placeBlock(
        level,
        B("minecraft:redstone_wire").with("north", "side").with("south", "up"),
        9,
        -3,
        5,
        chunkBB
        );
        placeBlock(level, B("minecraft:mossy_cobblestone"), 9, -3, 4, chunkBB);
        placeBlock(level, redstoneWireNS, 9, -2, 4, chunkBB);
        if (!placedTrap2) {
        placedTrap2 = createDispenser(level, chunkBB, random, 9, -2, 3, West);
        }

        placeBlock(level, B("minecraft:vine").with("east", true), 8, -1, 3, chunkBB);
        placeBlock(level, B("minecraft:vine").with("east", true), 8, -2, 3, chunkBB);
        if (!placedMainChest) {
        placedMainChest = createChest(level, chunkBB, random, 8, -3, 3);
        }

        placeBlock(level, B("minecraft:mossy_cobblestone"), 9, -3, 2, chunkBB);
        placeBlock(level, B("minecraft:mossy_cobblestone"), 8, -3, 1, chunkBB);
        placeBlock(level, B("minecraft:mossy_cobblestone"), 4, -3, 5, chunkBB);
        placeBlock(level, B("minecraft:mossy_cobblestone"), 5, -2, 5, chunkBB);
        placeBlock(level, B("minecraft:mossy_cobblestone"), 5, -1, 5, chunkBB);
        placeBlock(level, B("minecraft:mossy_cobblestone"), 6, -3, 5, chunkBB);
        placeBlock(level, B("minecraft:mossy_cobblestone"), 7, -2, 5, chunkBB);
        placeBlock(level, B("minecraft:mossy_cobblestone"), 7, -1, 5, chunkBB);
        placeBlock(level, B("minecraft:mossy_cobblestone"), 8, -3, 5, chunkBB);
        generateBox(level, chunkBB, 9, -1, 1, 9, -1, 5, false, random, STONE_SELECTOR);
        generateAirBox(level, chunkBB, 8, -3, 8, 10, -1, 10);
        placeBlock(level, B("minecraft:chiseled_stone_bricks"), 8, -2, 11, chunkBB);
        placeBlock(level, B("minecraft:chiseled_stone_bricks"), 9, -2, 11, chunkBB);
        placeBlock(level, B("minecraft:chiseled_stone_bricks"), 10, -2, 11, chunkBB);
        St lever = B("minecraft:lever").with("facing", North).with("face", "wall");
        placeBlock(level, lever, 8, -2, 12, chunkBB);
        placeBlock(level, lever, 9, -2, 12, chunkBB);
        placeBlock(level, lever, 10, -2, 12, chunkBB);
        generateBox(level, chunkBB, 8, -3, 8, 8, -3, 10, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 10, -3, 8, 10, -3, 10, false, random, STONE_SELECTOR);
        placeBlock(level, B("minecraft:mossy_cobblestone"), 10, -2, 9, chunkBB);
        placeBlock(level, redstoneWireNS, 8, -2, 9, chunkBB);
        placeBlock(level, redstoneWireNS, 8, -2, 10, chunkBB);
        placeBlock(
        level,
        B("minecraft:redstone_wire")
        .with("north", "side")
        .with("south", "side")
        .with("east", "side")
        .with("west", "side"),
        10,
        -1,
        9,
        chunkBB
        );
        placeBlock(level, B("minecraft:sticky_piston").with("facing", Up), 9, -2, 8, chunkBB);
        placeBlock(level, B("minecraft:sticky_piston").with("facing", West), 10, -2, 8, chunkBB);
        placeBlock(level, B("minecraft:sticky_piston").with("facing", West), 10, -1, 8, chunkBB);
        placeBlock(level, B("minecraft:repeater").with("facing", North), 10, -2, 10, chunkBB);
        if (!placedHiddenChest) {
        placedHiddenChest = createChest(level, chunkBB, random, 9, -3, 10);
        }
        }
    }
};

class JungleTempleStructure final : public Structure {
public:
    std::optional<Stub> findGenerationPoint(GenCtx& c) const override {
        if (!c.couldValidBiomeExistOnTopOfChunkCenter()) return std::nullopt;
        if (c.lowestY(12, 15) < c.seaLevel()) return std::nullopt;
        const int x = c.middleBlockX(), z = c.middleBlockZ();
        GenCtx* ctx = &c;
        return Stub{{x, c.firstOccupiedHeight(x, z, HType::WorldSurfaceWG), z}, [ctx](PiecesBuilder& b) {
                        b.add(std::make_unique<JungleTemplePiece>(ctx->random, ctx->minBlockX(), ctx->minBlockZ()));
                    }};
    }
};

// ---- swamp hut ------------------------------------------------------------------------------------------------

class SwampHutPiece final : public ScatteredPiece {
public:
    SwampHutPiece(RandomSource& random, int west, int north)
        : ScatteredPiece("minecraft:tesh", west, 64, north, 7, 7, 9, randomHorizontal(random)) {}

    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos& referencePos) override {
        (void)referencePos, (void)random;
        if (updateAverageGroundHeight(level, chunkBB, 0)) {
        generateBox(level, chunkBB, 1, 1, 1, 5, 1, 7, B("minecraft:spruce_planks"), B("minecraft:spruce_planks"), false);
        generateBox(level, chunkBB, 1, 4, 2, 5, 4, 7, B("minecraft:spruce_planks"), B("minecraft:spruce_planks"), false);
        generateBox(level, chunkBB, 2, 1, 0, 4, 1, 0, B("minecraft:spruce_planks"), B("minecraft:spruce_planks"), false);
        generateBox(level, chunkBB, 2, 2, 2, 3, 3, 2, B("minecraft:spruce_planks"), B("minecraft:spruce_planks"), false);
        generateBox(level, chunkBB, 1, 2, 3, 1, 3, 6, B("minecraft:spruce_planks"), B("minecraft:spruce_planks"), false);
        generateBox(level, chunkBB, 5, 2, 3, 5, 3, 6, B("minecraft:spruce_planks"), B("minecraft:spruce_planks"), false);
        generateBox(level, chunkBB, 2, 2, 7, 4, 3, 7, B("minecraft:spruce_planks"), B("minecraft:spruce_planks"), false);
        generateBox(level, chunkBB, 1, 0, 2, 1, 3, 2, B("minecraft:oak_log"), B("minecraft:oak_log"), false);
        generateBox(level, chunkBB, 5, 0, 2, 5, 3, 2, B("minecraft:oak_log"), B("minecraft:oak_log"), false);
        generateBox(level, chunkBB, 1, 0, 7, 1, 3, 7, B("minecraft:oak_log"), B("minecraft:oak_log"), false);
        generateBox(level, chunkBB, 5, 0, 7, 5, 3, 7, B("minecraft:oak_log"), B("minecraft:oak_log"), false);
        placeBlock(level, B("minecraft:oak_fence"), 2, 3, 2, chunkBB);
        placeBlock(level, B("minecraft:oak_fence"), 3, 3, 7, chunkBB);
        placeBlock(level, B("minecraft:air"), 1, 3, 4, chunkBB);
        placeBlock(level, B("minecraft:air"), 5, 3, 4, chunkBB);
        placeBlock(level, B("minecraft:air"), 5, 3, 5, chunkBB);
        placeBlock(level, B("minecraft:potted_red_mushroom"), 1, 3, 5, chunkBB);
        placeBlock(level, B("minecraft:crafting_table"), 3, 2, 6, chunkBB);
        placeBlock(level, B("minecraft:cauldron"), 4, 2, 6, chunkBB);
        placeBlock(level, B("minecraft:oak_fence"), 1, 2, 1, chunkBB);
        placeBlock(level, B("minecraft:oak_fence"), 5, 2, 1, chunkBB);
        St northStairs = B("minecraft:spruce_stairs").with("facing", North);
        St eastStairs = B("minecraft:spruce_stairs").with("facing", East);
        St westStairs = B("minecraft:spruce_stairs").with("facing", West);
        St southStairs = B("minecraft:spruce_stairs").with("facing", South);
        generateBox(level, chunkBB, 0, 4, 1, 6, 4, 1, northStairs, northStairs, false);
        generateBox(level, chunkBB, 0, 4, 2, 0, 4, 7, eastStairs, eastStairs, false);
        generateBox(level, chunkBB, 6, 4, 2, 6, 4, 7, westStairs, westStairs, false);
        generateBox(level, chunkBB, 0, 4, 8, 6, 4, 8, southStairs, southStairs, false);
        placeBlock(level, northStairs.with("shape", "outer_right"), 0, 4, 1, chunkBB);
        placeBlock(level, northStairs.with("shape", "outer_left"), 6, 4, 1, chunkBB);
        placeBlock(level, southStairs.with("shape", "outer_left"), 0, 4, 8, chunkBB);
        placeBlock(level, southStairs.with("shape", "outer_right"), 6, 4, 8, chunkBB);

        for (int z = 2; z <= 7; z += 5) {
        for (int x = 1; x <= 5; x += 4) {
        fillColumnDown(level, B("minecraft:oak_log"), x, -1, z, chunkBB);
        }
        }
        }
    }
};

class SwampHutStructure final : public Structure {
public:
    std::optional<Stub> findGenerationPoint(GenCtx& c) const override {
        GenCtx* ctx = &c;
        return onTopOfChunkCenter(c, HType::WorldSurfaceWG, [ctx](PiecesBuilder& b) {
            b.add(std::make_unique<SwampHutPiece>(ctx->random, ctx->minBlockX(), ctx->minBlockZ()));
        });
    }
};

// ---- igloo ------------------------------------------------------------------------------------------------------

class IglooPiece final : public TemplatePiece {
public:
    enum Kind { Top, Ladder, Laboratory };
    static const char* location(Kind k) {
        return k == Top ? "minecraft:igloo/top" : k == Ladder ? "minecraft:igloo/middle" : "minecraft:igloo/bottom";
    }
    static BlockPos pivot(Kind k) { return k == Top ? BlockPos{3, 5, 5} : k == Ladder ? BlockPos{1, 3, 1} : BlockPos{3, 6, 7}; }
    static BlockPos offset(Kind k) { return k == Top ? BlockPos{0, 0, 0} : k == Ladder ? BlockPos{2, -3, 4} : BlockPos{0, -3, -2}; }
    static PlaceSettings makeSettings(const StructureEnv& env, Rot r, Kind k) {
        PlaceSettings s;
        s.rotation = r;
        s.mirror = Mir::None;
        s.pivot = pivot(k);
        s.processors.push_back(blockIgnore(env, {"minecraft:structure_block"}));
        s.applyWaterlogging = false;
        return s;
    }

    IglooPiece(const StructureEnv& env, Kind k, const BlockPos& position, Rot r, int depth)
        : TemplatePiece("minecraft:iglu", 0, env.templates->get(location(k)), makeSettings(env, r, k),
                        position.offset(offset(k).x, offset(k).y - depth, offset(k).z)),
          kind(k) {}
    Kind kind;

    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos& referencePos) override {
        const PlaceSettings s = makeSettings(level.env, settings.rotation, kind);
        const BlockPos off = offset(kind);
        const BlockPos rel = transformPos({3 - off.x, 0, -off.z}, s.mirror, s.rotation, s.pivot);
        const BlockPos entrance = templatePosition.offset(rel.x, rel.y, rel.z);
        const int h = level.height(Heightmap::WorldSurfaceWG, entrance.x, entrance.z);
        const BlockPos old = templatePosition;
        templatePosition = templatePosition.offset(0, h - 90 - 1, 0);
        TemplatePiece::postProcess(level, random, chunkBB, referencePos);
        if (kind == Top) {
            const BlockPos tr = transformPos({3, 0, 5}, s.mirror, s.rotation, s.pivot);
            const BlockPos trapDoor = templatePosition.offset(tr.x, tr.y, tr.z);
            const St below = level.get(trapDoor.offset(0, -1, 0));
            if (!below.isAir() && !below.is("minecraft:ladder")) level.set(trapDoor, B("minecraft:snow_block"));
        }
        templatePosition = old;
    }

protected:
    void handleDataMarker(PlaceCtx& level, const std::string& marker, const BlockPos& pos, RandomSource& random, const BBox&) override {
        if (marker != "chest") return;
        level.set(pos, B("minecraft:air"));
        if (level.get(pos.offset(0, -1, 0)).is("minecraft:chest")) random.nextLong();
    }
};

class IglooStructure final : public Structure {
public:
    std::optional<Stub> findGenerationPoint(GenCtx& c) const override {
        GenCtx* ctx = &c;
        return onTopOfChunkCenter(c, HType::WorldSurfaceWG, [ctx](PiecesBuilder& b) {
            RandomSource& random = ctx->random;
            const BlockPos start{ctx->minBlockX(), 90, ctx->minBlockZ()};
            const Rot r = randomRotation(random);
            if (random.nextDouble() < 0.5) {
                const int depth = random.nextInt(8) + 4;
                b.add(std::make_unique<IglooPiece>(ctx->env, IglooPiece::Laboratory, start, r, depth * 3));
                for (int i = 0; i < depth - 1; ++i) b.add(std::make_unique<IglooPiece>(ctx->env, IglooPiece::Ladder, start, r, i * 3));
            }
            b.add(std::make_unique<IglooPiece>(ctx->env, IglooPiece::Top, start, r, 0));
        });
    }
};

// ---- buried treasure -------------------------------------------------------------------------------------------

class BuriedTreasurePiece final : public Piece {
public:
    explicit BuriedTreasurePiece(const BlockPos& p) : Piece("minecraft:btp", 0, BBox(p)) {}

    static bool isLiquid(St s) { return s.is("minecraft:water") || s.is("minecraft:lava"); }

    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        const int y = level.height(Heightmap::OceanFloorWG, box.minX, box.minZ);
        BlockPos pos{box.minX, y, box.minZ};
        while (pos.y > level.minY()) {
            const St current = level.get(pos);
            const St below = level.get(pos.offset(0, -1, 0));
            if (below.is("minecraft:sandstone") || below.is("minecraft:stone") || below.is("minecraft:andesite") ||
                below.is("minecraft:granite") || below.is("minecraft:diorite")) {
                const u16 soft = !current.isAir() && !isLiquid(current) ? u16(current) : u16(B("minecraft:sand"));
                for (int d = 0; d < 6; ++d) {  // Direction.values()
                    const Direction dir = Direction(d);
                    const BlockPos rp = relative(pos, dir);
                    const St rs = level.get(rp);
                    if (rs.isAir() || isLiquid(rs)) {
                        const St brs = level.get(rp.offset(0, -1, 0));
                        if ((brs.isAir() || isLiquid(brs)) && dir != Up)
                            level.set(rp, below);
                        else
                            level.set(rp, soft);
                    }
                }
                box = BBox(pos);
                createChest(level, chunkBB, random, pos, std::nullopt);
                return;
            }
            pos.y -= 1;
        }
    }
};

class BuriedTreasureStructure final : public Structure {
public:
    std::optional<Stub> findGenerationPoint(GenCtx& c) const override {
        GenCtx* ctx = &c;
        return onTopOfChunkCenter(c, HType::OceanFloorWG, [ctx](PiecesBuilder& b) {
            b.add(std::make_unique<BuriedTreasurePiece>(BlockPos{ctx->minBlockX() + 9, 90, ctx->minBlockZ() + 9}));
        });
    }
};

template <class T>
StructureFactory simple() {
    return [](const json&, StructureEnv&) -> std::unique_ptr<Structure> { return std::make_unique<T>(); };
}

}  // namespace

void registerScattered() {
    registerStructureType("minecraft:desert_pyramid", simple<DesertPyramidStructure>());
    registerStructureType("minecraft:jungle_temple", simple<JungleTempleStructure>());
    registerStructureType("minecraft:swamp_hut", simple<SwampHutStructure>());
    registerStructureType("minecraft:igloo", simple<IglooStructure>());
    registerStructureType("minecraft:buried_treasure", simple<BuriedTreasureStructure>());
}

}  // namespace wg::st
