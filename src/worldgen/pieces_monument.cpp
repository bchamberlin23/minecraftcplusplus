// Ocean monuments: the game's OceanMonumentStructure and OceanMonumentPieces (room graph with random closed
// openings, room fitters, the core room, wings, penthouse, and the building shell). Elder guardians are entities and
// are not placed here.
#include "worldgen/structure_core.h"

namespace wg::st {

namespace {

struct RoomDefinition {
    int index;
    RoomDefinition* connections[6] = {};
    bool hasOpening[6] = {};
    bool claimed = false, isSource = false;
    int scanIndex = 0;
    explicit RoomDefinition(int i) : index(i) {}
    void setConnection(Direction d, RoomDefinition* def) {
        connections[int(d)] = def;
        def->connections[int(opposite(d))] = this;
    }
    void updateOpenings() {
        for (int i = 0; i < 6; ++i) hasOpening[i] = connections[i] != nullptr;
    }
    bool findSource(int scan) {
        if (isSource) return true;
        scanIndex = scan;
        for (int i = 0; i < 6; ++i)
            if (connections[i] && hasOpening[i] && connections[i]->scanIndex != scan && connections[i]->findSource(scan)) return true;
        return false;
    }
    bool isSpecial() const { return index >= 75; }
    int countOpenings() const {
        int c = 0;
        for (bool o : hasOpening) c += o;
        return c;
    }
};

class OceanMonumentPiece : public Piece {
public:
    OceanMonumentPiece(const char* id, Direction o, int genDepth, const BBox& b) : Piece(id, genDepth, b) {
        setOrientation(o);
        initBlocks();
    }
    OceanMonumentPiece(const char* id, int genDepth, Direction o, RoomDefinition* def, int w, int h, int d)
        : Piece(id, genDepth, makeRoomBox(o, def, w, h, d)), roomDefinition(def) {
        setOrientation(o);
        initBlocks();
    }
    RoomDefinition* roomDefinition = nullptr;

    static int getRoomIndex(int x, int y, int z) { return y * 25 + z * 5 + x; }
    static constexpr int GRIDROOM_SOURCE_INDEX = 2;          // getRoomIndex(2, 0, 0)
    static constexpr int GRIDROOM_TOP_CONNECT_INDEX = 52;    // getRoomIndex(2, 2, 0)
    static constexpr int GRIDROOM_LEFTWING_CONNECT_INDEX = 25;   // getRoomIndex(0, 1, 0)
    static constexpr int GRIDROOM_RIGHTWING_CONNECT_INDEX = 29;  // getRoomIndex(4, 1, 0)

protected:
    St BASE_GRAY{0}, BASE_LIGHT{0}, BASE_BLACK{0}, DOT_DECO_DATA{0}, LAMP_BLOCK{0}, FILL_BLOCK{0};
    void initBlocks() {
        BASE_GRAY = B("minecraft:prismarine");
        BASE_LIGHT = B("minecraft:prismarine_bricks");
        BASE_BLACK = B("minecraft:dark_prismarine");
        DOT_DECO_DATA = BASE_LIGHT;
        LAMP_BLOCK = B("minecraft:sea_lantern");
        FILL_BLOCK = B("minecraft:water");
    }

    static BBox makeRoomBox(Direction o, const RoomDefinition* def, int w, int h, int d) {
        const int roomX = def->index % 5, roomZ = def->index / 5 % 5, roomY = def->index / 25;
        BBox b = makeBoundingBox(0, 0, 0, o, w * 8, h * 4, d * 8);
        switch (o) {
        case North: b.move(roomX * 8, roomY * 4, -(roomZ + d) * 8 + 1); break;
        case South: b.move(roomX * 8, roomY * 4, roomZ * 8); break;
        case West: b.move(-(roomZ + d) * 8 + 1, roomY * 4, roomX * 8); break;
        default: b.move(roomZ * 8, roomY * 4, roomX * 8); break;
        }
        return b;
    }

    bool fillKeep(St s) const {
        return s.is("minecraft:ice") || s.is("minecraft:packed_ice") || s.is("minecraft:blue_ice") || s.is("minecraft:water");
    }

    void generateWaterBox(PlaceCtx& level, const BBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                for (int z = z0; z <= z1; ++z) {
                    const St block = S(getBlock(level, x, y, z, chunkBB));
                    if (fillKeep(block)) continue;
                    if (getWorldY(y) >= level.level.seaLevel() && block != FILL_BLOCK)
                        placeBlock(level, B("minecraft:air"), x, y, z, chunkBB);
                    else
                        placeBlock(level, FILL_BLOCK, x, y, z, chunkBB);
                }
    }

    void generateDefaultFloor(PlaceCtx& level, const BBox& chunkBB, int xOff, int zOff, bool downOpening) {
        if (downOpening) {
        generateBox(level, chunkBB, xOff + 0, 0, zOff + 0, xOff + 2, 0, zOff + 8 - 1, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, xOff + 5, 0, zOff + 0, xOff + 8 - 1, 0, zOff + 8 - 1, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, xOff + 3, 0, zOff + 0, xOff + 4, 0, zOff + 2, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, xOff + 3, 0, zOff + 5, xOff + 4, 0, zOff + 8 - 1, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, xOff + 3, 0, zOff + 2, xOff + 4, 0, zOff + 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, xOff + 3, 0, zOff + 5, xOff + 4, 0, zOff + 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, xOff + 2, 0, zOff + 3, xOff + 2, 0, zOff + 4, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, xOff + 5, 0, zOff + 3, xOff + 5, 0, zOff + 4, BASE_LIGHT, BASE_LIGHT, false);
        } else {
        generateBox(level, chunkBB, xOff + 0, 0, zOff + 0, xOff + 8 - 1, 0, zOff + 8 - 1, BASE_GRAY, BASE_GRAY, false);
        }
    }

    void generateBoxOnFillOnly(PlaceCtx& level, const BBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1, St targetBlock) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                for (int z = z0; z <= z1; ++z)
                    if (getBlock(level, x, y, z, chunkBB) == FILL_BLOCK) placeBlock(level, targetBlock, x, y, z, chunkBB);
    }

    bool chunkIntersects(const BBox& chunkBB, int x0, int z0, int x1, int z1) const {
        const int wx0 = worldX(x0, z0), wz0 = worldZ(x0, z0), wx1 = worldX(x1, z1), wz1 = worldZ(x1, z1);
        return chunkBB.intersectsXZ(std::min(wx0, wx1), std::min(wz0, wz1), std::max(wx0, wx1), std::max(wz0, wz1));
    }

    void spawnElder(PlaceCtx&, const BBox&, int, int, int) {}  // entity
};

class OceanMonumentCoreRoom final : public OceanMonumentPiece {
public:
    OceanMonumentCoreRoom(Direction o, RoomDefinition* d) : OceanMonumentPiece("minecraft:omcr", 1, o, d, 2, 2, 2) {}
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        (void)random;
        generateBoxOnFillOnly(level, chunkBB, 1, 8, 0, 14, 8, 14, BASE_GRAY);
        int y = 7;
        St block = BASE_LIGHT;
        generateBox(level, chunkBB, 0, 7, 0, 0, 7, 15, block, block, false);
        generateBox(level, chunkBB, 15, 7, 0, 15, 7, 15, block, block, false);
        generateBox(level, chunkBB, 1, 7, 0, 15, 7, 0, block, block, false);
        generateBox(level, chunkBB, 1, 7, 15, 14, 7, 15, block, block, false);

        for (int yx = 1; yx <= 6; yx++) {
        block = BASE_LIGHT;
        if (yx == 2 || yx == 6) {
        block = BASE_GRAY;
        }

        for (int x = 0; x <= 15; x += 15) {
        generateBox(level, chunkBB, x, yx, 0, x, yx, 1, block, block, false);
        generateBox(level, chunkBB, x, yx, 6, x, yx, 9, block, block, false);
        generateBox(level, chunkBB, x, yx, 14, x, yx, 15, block, block, false);
        }

        generateBox(level, chunkBB, 1, yx, 0, 1, yx, 0, block, block, false);
        generateBox(level, chunkBB, 6, yx, 0, 9, yx, 0, block, block, false);
        generateBox(level, chunkBB, 14, yx, 0, 14, yx, 0, block, block, false);
        generateBox(level, chunkBB, 1, yx, 15, 14, yx, 15, block, block, false);
        }

        generateBox(level, chunkBB, 6, 3, 6, 9, 6, 9, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 7, 4, 7, 8, 5, 8, B("minecraft:gold_block"), B("minecraft:gold_block"), false);

        for (int yx = 3; yx <= 6; yx += 3) {
        for (int x = 6; x <= 9; x += 3) {
        placeBlock(level, LAMP_BLOCK, x, yx, 6, chunkBB);
        placeBlock(level, LAMP_BLOCK, x, yx, 9, chunkBB);
        }
        }

        generateBox(level, chunkBB, 5, 1, 6, 5, 2, 6, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 1, 9, 5, 2, 9, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 10, 1, 6, 10, 2, 6, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 10, 1, 9, 10, 2, 9, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 1, 5, 6, 2, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 9, 1, 5, 9, 2, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 1, 10, 6, 2, 10, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 9, 1, 10, 9, 2, 10, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 2, 5, 5, 6, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 2, 10, 5, 6, 10, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 10, 2, 5, 10, 6, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 10, 2, 10, 10, 6, 10, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 7, 1, 5, 7, 6, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 10, 7, 1, 10, 7, 6, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 7, 9, 5, 7, 14, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 10, 7, 9, 10, 7, 14, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 7, 5, 6, 7, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 7, 10, 6, 7, 10, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 9, 7, 5, 14, 7, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 9, 7, 10, 14, 7, 10, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 2, 1, 2, 2, 1, 3, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 3, 1, 2, 3, 1, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 13, 1, 2, 13, 1, 3, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 12, 1, 2, 12, 1, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 2, 1, 12, 2, 1, 13, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 3, 1, 13, 3, 1, 13, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 13, 1, 12, 13, 1, 13, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 12, 1, 13, 12, 1, 13, BASE_LIGHT, BASE_LIGHT, false);
    }
};

class OceanMonumentDoubleXRoom final : public OceanMonumentPiece {
public:
    OceanMonumentDoubleXRoom(Direction o, RoomDefinition* d) : OceanMonumentPiece("minecraft:omdxr", 1, o, d, 2, 1, 1) {}
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        (void)random;
        RoomDefinition* east = roomDefinition->connections[int(East)];
        RoomDefinition* west = roomDefinition;
        if (roomDefinition->index / 25 > 0) {
        generateDefaultFloor(level, chunkBB, 8, 0, east->hasOpening[int(Down)]);
        generateDefaultFloor(level, chunkBB, 0, 0, west->hasOpening[int(Down)]);
        }

        if (west->connections[int(Up)] == nullptr) {
        generateBoxOnFillOnly(level, chunkBB, 1, 4, 1, 7, 4, 6, BASE_GRAY);
        }

        if (east->connections[int(Up)] == nullptr) {
        generateBoxOnFillOnly(level, chunkBB, 8, 4, 1, 14, 4, 6, BASE_GRAY);
        }

        generateBox(level, chunkBB, 0, 3, 0, 0, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 15, 3, 0, 15, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 3, 0, 15, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 3, 7, 14, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 2, 0, 0, 2, 7, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 15, 2, 0, 15, 2, 7, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 1, 2, 0, 15, 2, 0, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 1, 2, 7, 14, 2, 7, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 0, 1, 0, 0, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 15, 1, 0, 15, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 1, 0, 15, 1, 0, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 1, 7, 14, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 1, 0, 10, 1, 4, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 2, 0, 9, 2, 3, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 5, 3, 0, 10, 3, 4, BASE_LIGHT, BASE_LIGHT, false);
        placeBlock(level, LAMP_BLOCK, 6, 2, 3, chunkBB);
        placeBlock(level, LAMP_BLOCK, 9, 2, 3, chunkBB);
        if (west->hasOpening[int(South)]) {
        generateWaterBox(level, chunkBB, 3, 1, 0, 4, 2, 0);
        }

        if (west->hasOpening[int(North)]) {
        generateWaterBox(level, chunkBB, 3, 1, 7, 4, 2, 7);
        }

        if (west->hasOpening[int(West)]) {
        generateWaterBox(level, chunkBB, 0, 1, 3, 0, 2, 4);
        }

        if (east->hasOpening[int(South)]) {
        generateWaterBox(level, chunkBB, 11, 1, 0, 12, 2, 0);
        }

        if (east->hasOpening[int(North)]) {
        generateWaterBox(level, chunkBB, 11, 1, 7, 12, 2, 7);
        }

        if (east->hasOpening[int(East)]) {
        generateWaterBox(level, chunkBB, 15, 1, 3, 15, 2, 4);
        }
    }
};

class OceanMonumentDoubleXYRoom final : public OceanMonumentPiece {
public:
    OceanMonumentDoubleXYRoom(Direction o, RoomDefinition* d) : OceanMonumentPiece("minecraft:omdxyr", 1, o, d, 2, 2, 1) {}
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        (void)random;
        RoomDefinition* east = roomDefinition->connections[int(East)];
        RoomDefinition* west = roomDefinition;
        RoomDefinition* westUp = west->connections[int(Up)];
        RoomDefinition* eastUp = east->connections[int(Up)];
        if (roomDefinition->index / 25 > 0) {
        generateDefaultFloor(level, chunkBB, 8, 0, east->hasOpening[int(Down)]);
        generateDefaultFloor(level, chunkBB, 0, 0, west->hasOpening[int(Down)]);
        }

        if (westUp->connections[int(Up)] == nullptr) {
        generateBoxOnFillOnly(level, chunkBB, 1, 8, 1, 7, 8, 6, BASE_GRAY);
        }

        if (eastUp->connections[int(Up)] == nullptr) {
        generateBoxOnFillOnly(level, chunkBB, 8, 8, 1, 14, 8, 6, BASE_GRAY);
        }

        for (int y = 1; y <= 7; y++) {
        St block = BASE_LIGHT;
        if (y == 2 || y == 6) {
        block = BASE_GRAY;
        }

        generateBox(level, chunkBB, 0, y, 0, 0, y, 7, block, block, false);
        generateBox(level, chunkBB, 15, y, 0, 15, y, 7, block, block, false);
        generateBox(level, chunkBB, 1, y, 0, 15, y, 0, block, block, false);
        generateBox(level, chunkBB, 1, y, 7, 14, y, 7, block, block, false);
        }

        generateBox(level, chunkBB, 2, 1, 3, 2, 7, 4, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 3, 1, 2, 4, 7, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 3, 1, 5, 4, 7, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 13, 1, 3, 13, 7, 4, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 11, 1, 2, 12, 7, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 11, 1, 5, 12, 7, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 1, 3, 5, 3, 4, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 10, 1, 3, 10, 3, 4, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 7, 2, 10, 7, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 5, 2, 5, 7, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 10, 5, 2, 10, 7, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 5, 5, 5, 7, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 10, 5, 5, 10, 7, 5, BASE_LIGHT, BASE_LIGHT, false);
        placeBlock(level, BASE_LIGHT, 6, 6, 2, chunkBB);
        placeBlock(level, BASE_LIGHT, 9, 6, 2, chunkBB);
        placeBlock(level, BASE_LIGHT, 6, 6, 5, chunkBB);
        placeBlock(level, BASE_LIGHT, 9, 6, 5, chunkBB);
        generateBox(level, chunkBB, 5, 4, 3, 6, 4, 4, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 9, 4, 3, 10, 4, 4, BASE_LIGHT, BASE_LIGHT, false);
        placeBlock(level, LAMP_BLOCK, 5, 4, 2, chunkBB);
        placeBlock(level, LAMP_BLOCK, 5, 4, 5, chunkBB);
        placeBlock(level, LAMP_BLOCK, 10, 4, 2, chunkBB);
        placeBlock(level, LAMP_BLOCK, 10, 4, 5, chunkBB);
        if (west->hasOpening[int(South)]) {
        generateWaterBox(level, chunkBB, 3, 1, 0, 4, 2, 0);
        }

        if (west->hasOpening[int(North)]) {
        generateWaterBox(level, chunkBB, 3, 1, 7, 4, 2, 7);
        }

        if (west->hasOpening[int(West)]) {
        generateWaterBox(level, chunkBB, 0, 1, 3, 0, 2, 4);
        }

        if (east->hasOpening[int(South)]) {
        generateWaterBox(level, chunkBB, 11, 1, 0, 12, 2, 0);
        }

        if (east->hasOpening[int(North)]) {
        generateWaterBox(level, chunkBB, 11, 1, 7, 12, 2, 7);
        }

        if (east->hasOpening[int(East)]) {
        generateWaterBox(level, chunkBB, 15, 1, 3, 15, 2, 4);
        }

        if (westUp->hasOpening[int(South)]) {
        generateWaterBox(level, chunkBB, 3, 5, 0, 4, 6, 0);
        }

        if (westUp->hasOpening[int(North)]) {
        generateWaterBox(level, chunkBB, 3, 5, 7, 4, 6, 7);
        }

        if (westUp->hasOpening[int(West)]) {
        generateWaterBox(level, chunkBB, 0, 5, 3, 0, 6, 4);
        }

        if (eastUp->hasOpening[int(South)]) {
        generateWaterBox(level, chunkBB, 11, 5, 0, 12, 6, 0);
        }

        if (eastUp->hasOpening[int(North)]) {
        generateWaterBox(level, chunkBB, 11, 5, 7, 12, 6, 7);
        }

        if (eastUp->hasOpening[int(East)]) {
        generateWaterBox(level, chunkBB, 15, 5, 3, 15, 6, 4);
        }
    }
};

class OceanMonumentDoubleYRoom final : public OceanMonumentPiece {
public:
    OceanMonumentDoubleYRoom(Direction o, RoomDefinition* d) : OceanMonumentPiece("minecraft:omdyr", 1, o, d, 1, 2, 1) {}
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        (void)random;
        if (roomDefinition->index / 25 > 0) {
        generateDefaultFloor(level, chunkBB, 0, 0, roomDefinition->hasOpening[int(Down)]);
        }

        RoomDefinition* above = roomDefinition->connections[int(Up)];
        if (above->connections[int(Up)] == nullptr) {
        generateBoxOnFillOnly(level, chunkBB, 1, 8, 1, 6, 8, 6, BASE_GRAY);
        }

        generateBox(level, chunkBB, 0, 4, 0, 0, 4, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 7, 4, 0, 7, 4, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 4, 0, 6, 4, 0, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 4, 7, 6, 4, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 2, 4, 1, 2, 4, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 4, 2, 1, 4, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 4, 1, 5, 4, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 4, 2, 6, 4, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 2, 4, 5, 2, 4, 6, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 4, 5, 1, 4, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 4, 5, 5, 4, 6, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 4, 5, 6, 4, 5, BASE_LIGHT, BASE_LIGHT, false);
        RoomDefinition* definition = roomDefinition;

        for (int y = 1; y <= 5; y += 4) {
        int z = 0;
        if (definition->hasOpening[int(South)]) {
        generateBox(level, chunkBB, 2, y, z, 2, y + 2, z, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, y, z, 5, y + 2, z, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 3, y + 2, z, 4, y + 2, z, BASE_LIGHT, BASE_LIGHT, false);
        } else {
        generateBox(level, chunkBB, 0, y, z, 7, y + 2, z, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, y + 1, z, 7, y + 1, z, BASE_GRAY, BASE_GRAY, false);
        }

        int var13 = 7;
        if (definition->hasOpening[int(North)]) {
        generateBox(level, chunkBB, 2, y, var13, 2, y + 2, var13, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, y, var13, 5, y + 2, var13, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 3, y + 2, var13, 4, y + 2, var13, BASE_LIGHT, BASE_LIGHT, false);
        } else {
        generateBox(level, chunkBB, 0, y, var13, 7, y + 2, var13, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, y + 1, var13, 7, y + 1, var13, BASE_GRAY, BASE_GRAY, false);
        }

        int x = 0;
        if (definition->hasOpening[int(West)]) {
        generateBox(level, chunkBB, x, y, 2, x, y + 2, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, x, y, 5, x, y + 2, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, x, y + 2, 3, x, y + 2, 4, BASE_LIGHT, BASE_LIGHT, false);
        } else {
        generateBox(level, chunkBB, x, y, 0, x, y + 2, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, x, y + 1, 0, x, y + 1, 7, BASE_GRAY, BASE_GRAY, false);
        }

        int var14 = 7;
        if (definition->hasOpening[int(East)]) {
        generateBox(level, chunkBB, var14, y, 2, var14, y + 2, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, var14, y, 5, var14, y + 2, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, var14, y + 2, 3, var14, y + 2, 4, BASE_LIGHT, BASE_LIGHT, false);
        } else {
        generateBox(level, chunkBB, var14, y, 0, var14, y + 2, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, var14, y + 1, 0, var14, y + 1, 7, BASE_GRAY, BASE_GRAY, false);
        }

        definition = above;
        }
    }
};

class OceanMonumentDoubleYZRoom final : public OceanMonumentPiece {
public:
    OceanMonumentDoubleYZRoom(Direction o, RoomDefinition* d) : OceanMonumentPiece("minecraft:omdyzr", 1, o, d, 1, 2, 2) {}
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        (void)random;
        RoomDefinition* north = roomDefinition->connections[int(North)];
        RoomDefinition* south = roomDefinition;
        RoomDefinition* northUp = north->connections[int(Up)];
        RoomDefinition* southUp = south->connections[int(Up)];
        if (roomDefinition->index / 25 > 0) {
        generateDefaultFloor(level, chunkBB, 0, 8, north->hasOpening[int(Down)]);
        generateDefaultFloor(level, chunkBB, 0, 0, south->hasOpening[int(Down)]);
        }

        if (southUp->connections[int(Up)] == nullptr) {
        generateBoxOnFillOnly(level, chunkBB, 1, 8, 1, 6, 8, 7, BASE_GRAY);
        }

        if (northUp->connections[int(Up)] == nullptr) {
        generateBoxOnFillOnly(level, chunkBB, 1, 8, 8, 6, 8, 14, BASE_GRAY);
        }

        for (int y = 1; y <= 7; y++) {
        St block = BASE_LIGHT;
        if (y == 2 || y == 6) {
        block = BASE_GRAY;
        }

        generateBox(level, chunkBB, 0, y, 0, 0, y, 15, block, block, false);
        generateBox(level, chunkBB, 7, y, 0, 7, y, 15, block, block, false);
        generateBox(level, chunkBB, 1, y, 0, 6, y, 0, block, block, false);
        generateBox(level, chunkBB, 1, y, 15, 6, y, 15, block, block, false);
        }

        for (int y = 1; y <= 7; y++) {
        St block = BASE_BLACK;
        if (y == 2 || y == 6) {
        block = LAMP_BLOCK;
        }

        generateBox(level, chunkBB, 3, y, 7, 4, y, 8, block, block, false);
        }

        if (south->hasOpening[int(South)]) {
        generateWaterBox(level, chunkBB, 3, 1, 0, 4, 2, 0);
        }

        if (south->hasOpening[int(East)]) {
        generateWaterBox(level, chunkBB, 7, 1, 3, 7, 2, 4);
        }

        if (south->hasOpening[int(West)]) {
        generateWaterBox(level, chunkBB, 0, 1, 3, 0, 2, 4);
        }

        if (north->hasOpening[int(North)]) {
        generateWaterBox(level, chunkBB, 3, 1, 15, 4, 2, 15);
        }

        if (north->hasOpening[int(West)]) {
        generateWaterBox(level, chunkBB, 0, 1, 11, 0, 2, 12);
        }

        if (north->hasOpening[int(East)]) {
        generateWaterBox(level, chunkBB, 7, 1, 11, 7, 2, 12);
        }

        if (southUp->hasOpening[int(South)]) {
        generateWaterBox(level, chunkBB, 3, 5, 0, 4, 6, 0);
        }

        if (southUp->hasOpening[int(East)]) {
        generateWaterBox(level, chunkBB, 7, 5, 3, 7, 6, 4);
        generateBox(level, chunkBB, 5, 4, 2, 6, 4, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 1, 2, 6, 3, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 1, 5, 6, 3, 5, BASE_LIGHT, BASE_LIGHT, false);
        }

        if (southUp->hasOpening[int(West)]) {
        generateWaterBox(level, chunkBB, 0, 5, 3, 0, 6, 4);
        generateBox(level, chunkBB, 1, 4, 2, 2, 4, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 1, 2, 1, 3, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 1, 5, 1, 3, 5, BASE_LIGHT, BASE_LIGHT, false);
        }

        if (northUp->hasOpening[int(North)]) {
        generateWaterBox(level, chunkBB, 3, 5, 15, 4, 6, 15);
        }

        if (northUp->hasOpening[int(West)]) {
        generateWaterBox(level, chunkBB, 0, 5, 11, 0, 6, 12);
        generateBox(level, chunkBB, 1, 4, 10, 2, 4, 13, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 1, 10, 1, 3, 10, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 1, 13, 1, 3, 13, BASE_LIGHT, BASE_LIGHT, false);
        }

        if (northUp->hasOpening[int(East)]) {
        generateWaterBox(level, chunkBB, 7, 5, 11, 7, 6, 12);
        generateBox(level, chunkBB, 5, 4, 10, 6, 4, 13, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 1, 10, 6, 3, 10, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 1, 13, 6, 3, 13, BASE_LIGHT, BASE_LIGHT, false);
        }
    }
};

class OceanMonumentDoubleZRoom final : public OceanMonumentPiece {
public:
    OceanMonumentDoubleZRoom(Direction o, RoomDefinition* d) : OceanMonumentPiece("minecraft:omdzr", 1, o, d, 1, 1, 2) {}
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        (void)random;
        RoomDefinition* north = roomDefinition->connections[int(North)];
        RoomDefinition* south = roomDefinition;
        if (roomDefinition->index / 25 > 0) {
        generateDefaultFloor(level, chunkBB, 0, 8, north->hasOpening[int(Down)]);
        generateDefaultFloor(level, chunkBB, 0, 0, south->hasOpening[int(Down)]);
        }

        if (south->connections[int(Up)] == nullptr) {
        generateBoxOnFillOnly(level, chunkBB, 1, 4, 1, 6, 4, 7, BASE_GRAY);
        }

        if (north->connections[int(Up)] == nullptr) {
        generateBoxOnFillOnly(level, chunkBB, 1, 4, 8, 6, 4, 14, BASE_GRAY);
        }

        generateBox(level, chunkBB, 0, 3, 0, 0, 3, 15, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 7, 3, 0, 7, 3, 15, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 3, 0, 7, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 3, 15, 6, 3, 15, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 2, 0, 0, 2, 15, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 7, 2, 0, 7, 2, 15, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 1, 2, 0, 7, 2, 0, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 1, 2, 15, 6, 2, 15, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 0, 1, 0, 0, 1, 15, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 7, 1, 0, 7, 1, 15, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 1, 0, 7, 1, 0, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 1, 15, 6, 1, 15, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 1, 1, 1, 1, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 1, 1, 6, 1, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 3, 1, 1, 3, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 3, 1, 6, 3, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 1, 13, 1, 1, 14, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 1, 13, 6, 1, 14, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 3, 13, 1, 3, 14, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 3, 13, 6, 3, 14, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 2, 1, 6, 2, 3, 6, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 1, 6, 5, 3, 6, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 2, 1, 9, 2, 3, 9, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 1, 9, 5, 3, 9, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 3, 2, 6, 4, 2, 6, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 3, 2, 9, 4, 2, 9, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 2, 2, 7, 2, 2, 8, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 2, 7, 5, 2, 8, BASE_LIGHT, BASE_LIGHT, false);
        placeBlock(level, LAMP_BLOCK, 2, 2, 5, chunkBB);
        placeBlock(level, LAMP_BLOCK, 5, 2, 5, chunkBB);
        placeBlock(level, LAMP_BLOCK, 2, 2, 10, chunkBB);
        placeBlock(level, LAMP_BLOCK, 5, 2, 10, chunkBB);
        placeBlock(level, BASE_LIGHT, 2, 3, 5, chunkBB);
        placeBlock(level, BASE_LIGHT, 5, 3, 5, chunkBB);
        placeBlock(level, BASE_LIGHT, 2, 3, 10, chunkBB);
        placeBlock(level, BASE_LIGHT, 5, 3, 10, chunkBB);
        if (south->hasOpening[int(South)]) {
        generateWaterBox(level, chunkBB, 3, 1, 0, 4, 2, 0);
        }

        if (south->hasOpening[int(East)]) {
        generateWaterBox(level, chunkBB, 7, 1, 3, 7, 2, 4);
        }

        if (south->hasOpening[int(West)]) {
        generateWaterBox(level, chunkBB, 0, 1, 3, 0, 2, 4);
        }

        if (north->hasOpening[int(North)]) {
        generateWaterBox(level, chunkBB, 3, 1, 15, 4, 2, 15);
        }

        if (north->hasOpening[int(West)]) {
        generateWaterBox(level, chunkBB, 0, 1, 11, 0, 2, 12);
        }

        if (north->hasOpening[int(East)]) {
        generateWaterBox(level, chunkBB, 7, 1, 11, 7, 2, 12);
        }
    }
};

class OceanMonumentEntryRoom final : public OceanMonumentPiece {
public:
    OceanMonumentEntryRoom(Direction o, RoomDefinition* d) : OceanMonumentPiece("minecraft:omentry", 1, o, d, 1, 1, 1) {}
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        (void)random;
        generateBox(level, chunkBB, 0, 3, 0, 2, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 3, 0, 7, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 2, 0, 1, 2, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 2, 0, 7, 2, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 1, 0, 0, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 7, 1, 0, 7, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 1, 7, 7, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 1, 0, 2, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 1, 0, 6, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
        if (roomDefinition->hasOpening[int(North)]) {
        generateWaterBox(level, chunkBB, 3, 1, 7, 4, 2, 7);
        }

        if (roomDefinition->hasOpening[int(West)]) {
        generateWaterBox(level, chunkBB, 0, 1, 3, 1, 2, 4);
        }

        if (roomDefinition->hasOpening[int(East)]) {
        generateWaterBox(level, chunkBB, 6, 1, 3, 7, 2, 4);
        }
    }
};

class OceanMonumentPenthouse final : public OceanMonumentPiece {
public:
    OceanMonumentPenthouse(Direction o, const BBox& b) : OceanMonumentPiece("minecraft:ompenthouse", o, 1, b) {}
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        (void)random;
        generateBox(level, chunkBB, 2, -1, 2, 11, -1, 11, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, -1, 0, 1, -1, 11, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 12, -1, 0, 13, -1, 11, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 2, -1, 0, 11, -1, 1, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 2, -1, 12, 11, -1, 13, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 0, 0, 0, 0, 0, 13, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 13, 0, 0, 13, 0, 13, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 0, 0, 12, 0, 0, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 0, 13, 12, 0, 13, BASE_LIGHT, BASE_LIGHT, false);

        for (int i = 2; i <= 11; i += 3) {
        placeBlock(level, LAMP_BLOCK, 0, 0, i, chunkBB);
        placeBlock(level, LAMP_BLOCK, 13, 0, i, chunkBB);
        placeBlock(level, LAMP_BLOCK, i, 0, 0, chunkBB);
        }

        generateBox(level, chunkBB, 2, 0, 3, 4, 0, 9, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 9, 0, 3, 11, 0, 9, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 4, 0, 9, 9, 0, 11, BASE_LIGHT, BASE_LIGHT, false);
        placeBlock(level, BASE_LIGHT, 5, 0, 8, chunkBB);
        placeBlock(level, BASE_LIGHT, 8, 0, 8, chunkBB);
        placeBlock(level, BASE_LIGHT, 10, 0, 10, chunkBB);
        placeBlock(level, BASE_LIGHT, 3, 0, 10, chunkBB);
        generateBox(level, chunkBB, 3, 0, 3, 3, 0, 7, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 10, 0, 3, 10, 0, 7, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 6, 0, 10, 7, 0, 10, BASE_BLACK, BASE_BLACK, false);
        int x = 3;

        for (int i = 0; i < 2; i++) {
        for (int z = 2; z <= 8; z += 3) {
        generateBox(level, chunkBB, x, 0, z, x, 2, z, BASE_LIGHT, BASE_LIGHT, false);
        }

        x = 10;
        }

        generateBox(level, chunkBB, 5, 0, 10, 5, 2, 10, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 8, 0, 10, 8, 2, 10, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, -1, 7, 7, -1, 8, BASE_BLACK, BASE_BLACK, false);
        generateWaterBox(level, chunkBB, 6, -1, 3, 7, -1, 4);
        spawnElder(level, chunkBB, 6, 1, 6);
    }
};

class OceanMonumentSimpleRoom final : public OceanMonumentPiece {
public:
    OceanMonumentSimpleRoom(Direction o, RoomDefinition* d, RandomSource& random)
        : OceanMonumentPiece("minecraft:omsimple", 1, o, d, 1, 1, 1), mainDesign(random.nextInt(3)) {}
    int mainDesign;
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        if (roomDefinition->index / 25 > 0) {
        generateDefaultFloor(level, chunkBB, 0, 0, roomDefinition->hasOpening[int(Down)]);
        }

        if (roomDefinition->connections[int(Up)] == nullptr) {
        generateBoxOnFillOnly(level, chunkBB, 1, 4, 1, 6, 4, 6, BASE_GRAY);
        }

        bool centerPillar = mainDesign != 0
        && random.nextBoolean()
        && !roomDefinition->hasOpening[int(Down)]
        && !roomDefinition->hasOpening[int(Up)]
        && roomDefinition->countOpenings() > 1;
        if (mainDesign == 0) {
        generateBox(level, chunkBB, 0, 1, 0, 2, 1, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 3, 0, 2, 3, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 2, 0, 0, 2, 2, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 1, 2, 0, 2, 2, 0, BASE_GRAY, BASE_GRAY, false);
        placeBlock(level, LAMP_BLOCK, 1, 2, 1, chunkBB);
        generateBox(level, chunkBB, 5, 1, 0, 7, 1, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 3, 0, 7, 3, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 7, 2, 0, 7, 2, 2, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 5, 2, 0, 6, 2, 0, BASE_GRAY, BASE_GRAY, false);
        placeBlock(level, LAMP_BLOCK, 6, 2, 1, chunkBB);
        generateBox(level, chunkBB, 0, 1, 5, 2, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 3, 5, 2, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 2, 5, 0, 2, 7, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 1, 2, 7, 2, 2, 7, BASE_GRAY, BASE_GRAY, false);
        placeBlock(level, LAMP_BLOCK, 1, 2, 6, chunkBB);
        generateBox(level, chunkBB, 5, 1, 5, 7, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 3, 5, 7, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 7, 2, 5, 7, 2, 7, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 5, 2, 7, 6, 2, 7, BASE_GRAY, BASE_GRAY, false);
        placeBlock(level, LAMP_BLOCK, 6, 2, 6, chunkBB);
        if (roomDefinition->hasOpening[int(South)]) {
        generateBox(level, chunkBB, 3, 3, 0, 4, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
        } else {
        generateBox(level, chunkBB, 3, 3, 0, 4, 3, 1, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 3, 2, 0, 4, 2, 0, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 3, 1, 0, 4, 1, 1, BASE_LIGHT, BASE_LIGHT, false);
        }

        if (roomDefinition->hasOpening[int(North)]) {
        generateBox(level, chunkBB, 3, 3, 7, 4, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        } else {
        generateBox(level, chunkBB, 3, 3, 6, 4, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 3, 2, 7, 4, 2, 7, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 3, 1, 6, 4, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        }

        if (roomDefinition->hasOpening[int(West)]) {
        generateBox(level, chunkBB, 0, 3, 3, 0, 3, 4, BASE_LIGHT, BASE_LIGHT, false);
        } else {
        generateBox(level, chunkBB, 0, 3, 3, 1, 3, 4, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 2, 3, 0, 2, 4, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 0, 1, 3, 1, 1, 4, BASE_LIGHT, BASE_LIGHT, false);
        }

        if (roomDefinition->hasOpening[int(East)]) {
        generateBox(level, chunkBB, 7, 3, 3, 7, 3, 4, BASE_LIGHT, BASE_LIGHT, false);
        } else {
        generateBox(level, chunkBB, 6, 3, 3, 7, 3, 4, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 7, 2, 3, 7, 2, 4, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 6, 1, 3, 7, 1, 4, BASE_LIGHT, BASE_LIGHT, false);
        }
        } else if (mainDesign == 1) {
        generateBox(level, chunkBB, 2, 1, 2, 2, 3, 2, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 2, 1, 5, 2, 3, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 1, 5, 5, 3, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 5, 1, 2, 5, 3, 2, BASE_LIGHT, BASE_LIGHT, false);
        placeBlock(level, LAMP_BLOCK, 2, 2, 2, chunkBB);
        placeBlock(level, LAMP_BLOCK, 2, 2, 5, chunkBB);
        placeBlock(level, LAMP_BLOCK, 5, 2, 5, chunkBB);
        placeBlock(level, LAMP_BLOCK, 5, 2, 2, chunkBB);
        generateBox(level, chunkBB, 0, 1, 0, 1, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 1, 1, 0, 3, 1, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 1, 7, 1, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 1, 6, 0, 3, 6, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 1, 7, 7, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 7, 1, 6, 7, 3, 6, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 1, 0, 7, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 7, 1, 1, 7, 3, 1, BASE_LIGHT, BASE_LIGHT, false);
        placeBlock(level, BASE_GRAY, 1, 2, 0, chunkBB);
        placeBlock(level, BASE_GRAY, 0, 2, 1, chunkBB);
        placeBlock(level, BASE_GRAY, 1, 2, 7, chunkBB);
        placeBlock(level, BASE_GRAY, 0, 2, 6, chunkBB);
        placeBlock(level, BASE_GRAY, 6, 2, 7, chunkBB);
        placeBlock(level, BASE_GRAY, 7, 2, 6, chunkBB);
        placeBlock(level, BASE_GRAY, 6, 2, 0, chunkBB);
        placeBlock(level, BASE_GRAY, 7, 2, 1, chunkBB);
        if (!roomDefinition->hasOpening[int(South)]) {
        generateBox(level, chunkBB, 1, 3, 0, 6, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 2, 0, 6, 2, 0, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 1, 1, 0, 6, 1, 0, BASE_LIGHT, BASE_LIGHT, false);
        }

        if (!roomDefinition->hasOpening[int(North)]) {
        generateBox(level, chunkBB, 1, 3, 7, 6, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 2, 7, 6, 2, 7, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 1, 1, 7, 6, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        }

        if (!roomDefinition->hasOpening[int(West)]) {
        generateBox(level, chunkBB, 0, 3, 1, 0, 3, 6, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 2, 1, 0, 2, 6, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 0, 1, 1, 0, 1, 6, BASE_LIGHT, BASE_LIGHT, false);
        }

        if (!roomDefinition->hasOpening[int(East)]) {
        generateBox(level, chunkBB, 7, 3, 1, 7, 3, 6, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 7, 2, 1, 7, 2, 6, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 7, 1, 1, 7, 1, 6, BASE_LIGHT, BASE_LIGHT, false);
        }
        } else if (mainDesign == 2) {
        generateBox(level, chunkBB, 0, 1, 0, 0, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 7, 1, 0, 7, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 1, 0, 6, 1, 0, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 1, 7, 6, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 2, 0, 0, 2, 7, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 7, 2, 0, 7, 2, 7, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 1, 2, 0, 6, 2, 0, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 1, 2, 7, 6, 2, 7, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 0, 3, 0, 0, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 7, 3, 0, 7, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 3, 0, 6, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 3, 7, 6, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 1, 3, 0, 2, 4, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 7, 1, 3, 7, 2, 4, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 3, 1, 0, 4, 2, 0, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 3, 1, 7, 4, 2, 7, BASE_BLACK, BASE_BLACK, false);
        if (roomDefinition->hasOpening[int(South)]) {
        generateWaterBox(level, chunkBB, 3, 1, 0, 4, 2, 0);
        }

        if (roomDefinition->hasOpening[int(North)]) {
        generateWaterBox(level, chunkBB, 3, 1, 7, 4, 2, 7);
        }

        if (roomDefinition->hasOpening[int(West)]) {
        generateWaterBox(level, chunkBB, 0, 1, 3, 0, 2, 4);
        }

        if (roomDefinition->hasOpening[int(East)]) {
        generateWaterBox(level, chunkBB, 7, 1, 3, 7, 2, 4);
        }
        }

        if (centerPillar) {
        generateBox(level, chunkBB, 3, 1, 3, 4, 1, 4, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 3, 2, 3, 4, 2, 4, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 3, 3, 3, 4, 3, 4, BASE_LIGHT, BASE_LIGHT, false);
        }
    }
};

class OceanMonumentSimpleTopRoom final : public OceanMonumentPiece {
public:
    OceanMonumentSimpleTopRoom(Direction o, RoomDefinition* d) : OceanMonumentPiece("minecraft:omsimplet", 1, o, d, 1, 1, 1) {}
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        if (roomDefinition->index / 25 > 0) {
        generateDefaultFloor(level, chunkBB, 0, 0, roomDefinition->hasOpening[int(Down)]);
        }

        if (roomDefinition->connections[int(Up)] == nullptr) {
        generateBoxOnFillOnly(level, chunkBB, 1, 4, 1, 6, 4, 6, BASE_GRAY);
        }

        for (int x = 1; x <= 6; x++) {
        for (int z = 1; z <= 6; z++) {
        if (random.nextInt(3) != 0) {
        int y0 = 2 + (random.nextInt(4) == 0 ? 0 : 1);
        St wetSponge = B("minecraft:wet_sponge");
        generateBox(level, chunkBB, x, y0, z, x, 3, z, wetSponge, wetSponge, false);
        }
        }
        }

        generateBox(level, chunkBB, 0, 1, 0, 0, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 7, 1, 0, 7, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 1, 0, 6, 1, 0, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 1, 7, 6, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 2, 0, 0, 2, 7, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 7, 2, 0, 7, 2, 7, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 1, 2, 0, 6, 2, 0, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 1, 2, 7, 6, 2, 7, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 0, 3, 0, 0, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 7, 3, 0, 7, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 3, 0, 6, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 1, 3, 7, 6, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 0, 1, 3, 0, 2, 4, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 7, 1, 3, 7, 2, 4, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 3, 1, 0, 4, 2, 0, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 3, 1, 7, 4, 2, 7, BASE_BLACK, BASE_BLACK, false);
        if (roomDefinition->hasOpening[int(South)]) {
        generateWaterBox(level, chunkBB, 3, 1, 0, 4, 2, 0);
        }
    }
};

class OceanMonumentWingRoom final : public OceanMonumentPiece {
public:
    OceanMonumentWingRoom(Direction o, const BBox& b, int randomValue) : OceanMonumentPiece("minecraft:omwr", o, 1, b), mainDesign(randomValue & 1) {}
    int mainDesign;
    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        (void)random;
        if (mainDesign == 0) {
        for (int i = 0; i < 4; i++) {
        generateBox(level, chunkBB, 10 - i, 3 - i, 20 - i, 12 + i, 3 - i, 20, BASE_LIGHT, BASE_LIGHT, false);
        }

        generateBox(level, chunkBB, 7, 0, 6, 15, 0, 16, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 6, 0, 6, 6, 3, 20, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 16, 0, 6, 16, 3, 20, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 7, 1, 7, 7, 1, 20, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 15, 1, 7, 15, 1, 20, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 7, 1, 6, 9, 3, 6, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 13, 1, 6, 15, 3, 6, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 8, 1, 7, 9, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 13, 1, 7, 14, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 9, 0, 5, 13, 0, 5, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 10, 0, 7, 12, 0, 7, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 8, 0, 10, 8, 0, 12, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 14, 0, 10, 14, 0, 12, BASE_BLACK, BASE_BLACK, false);

        for (int z = 18; z >= 7; z -= 3) {
        placeBlock(level, LAMP_BLOCK, 6, 3, z, chunkBB);
        placeBlock(level, LAMP_BLOCK, 16, 3, z, chunkBB);
        }

        placeBlock(level, LAMP_BLOCK, 10, 0, 10, chunkBB);
        placeBlock(level, LAMP_BLOCK, 12, 0, 10, chunkBB);
        placeBlock(level, LAMP_BLOCK, 10, 0, 12, chunkBB);
        placeBlock(level, LAMP_BLOCK, 12, 0, 12, chunkBB);
        placeBlock(level, LAMP_BLOCK, 8, 3, 6, chunkBB);
        placeBlock(level, LAMP_BLOCK, 14, 3, 6, chunkBB);
        placeBlock(level, BASE_LIGHT, 4, 2, 4, chunkBB);
        placeBlock(level, LAMP_BLOCK, 4, 1, 4, chunkBB);
        placeBlock(level, BASE_LIGHT, 4, 0, 4, chunkBB);
        placeBlock(level, BASE_LIGHT, 18, 2, 4, chunkBB);
        placeBlock(level, LAMP_BLOCK, 18, 1, 4, chunkBB);
        placeBlock(level, BASE_LIGHT, 18, 0, 4, chunkBB);
        placeBlock(level, BASE_LIGHT, 4, 2, 18, chunkBB);
        placeBlock(level, LAMP_BLOCK, 4, 1, 18, chunkBB);
        placeBlock(level, BASE_LIGHT, 4, 0, 18, chunkBB);
        placeBlock(level, BASE_LIGHT, 18, 2, 18, chunkBB);
        placeBlock(level, LAMP_BLOCK, 18, 1, 18, chunkBB);
        placeBlock(level, BASE_LIGHT, 18, 0, 18, chunkBB);
        placeBlock(level, BASE_LIGHT, 9, 7, 20, chunkBB);
        placeBlock(level, BASE_LIGHT, 13, 7, 20, chunkBB);
        generateBox(level, chunkBB, 6, 0, 21, 7, 4, 21, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 15, 0, 21, 16, 4, 21, BASE_LIGHT, BASE_LIGHT, false);
        spawnElder(level, chunkBB, 11, 2, 16);
        } else if (mainDesign == 1) {
        generateBox(level, chunkBB, 9, 3, 18, 13, 3, 20, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 9, 0, 18, 9, 2, 18, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 13, 0, 18, 13, 2, 18, BASE_LIGHT, BASE_LIGHT, false);
        int x = 9;
        int z = 20;
        int y = 5;

        for (int i = 0; i < 2; i++) {
        placeBlock(level, BASE_LIGHT, x, 6, 20, chunkBB);
        placeBlock(level, LAMP_BLOCK, x, 5, 20, chunkBB);
        placeBlock(level, BASE_LIGHT, x, 4, 20, chunkBB);
        x = 13;
        }

        generateBox(level, chunkBB, 7, 3, 7, 15, 3, 14, BASE_LIGHT, BASE_LIGHT, false);
        int var14 = 10;

        for (int i = 0; i < 2; i++) {
        generateBox(level, chunkBB, var14, 0, 10, var14, 6, 10, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, var14, 0, 12, var14, 6, 12, BASE_LIGHT, BASE_LIGHT, false);
        placeBlock(level, LAMP_BLOCK, var14, 0, 10, chunkBB);
        placeBlock(level, LAMP_BLOCK, var14, 0, 12, chunkBB);
        placeBlock(level, LAMP_BLOCK, var14, 4, 10, chunkBB);
        placeBlock(level, LAMP_BLOCK, var14, 4, 12, chunkBB);
        var14 = 12;
        }

        var14 = 8;

        for (int i = 0; i < 2; i++) {
        generateBox(level, chunkBB, var14, 0, 7, var14, 2, 7, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, var14, 0, 14, var14, 2, 14, BASE_LIGHT, BASE_LIGHT, false);
        var14 = 14;
        }

        generateBox(level, chunkBB, 8, 3, 8, 8, 3, 13, BASE_BLACK, BASE_BLACK, false);
        generateBox(level, chunkBB, 14, 3, 8, 14, 3, 13, BASE_BLACK, BASE_BLACK, false);
        spawnElder(level, chunkBB, 11, 5, 13);
        }
    }
};

class MonumentBuilding final : public OceanMonumentPiece {
public:
    std::vector<std::unique_ptr<RoomDefinition>> rooms;  // owns the room graph
    RoomDefinition* sourceRoom = nullptr;
    RoomDefinition* coreRoom = nullptr;
    std::vector<std::unique_ptr<OceanMonumentPiece>> childPieces;

    MonumentBuilding(RandomSource& random, int west, int north, Direction direction)
        : OceanMonumentPiece("minecraft:omb", direction, 0, makeBoundingBox(west, 39, north, direction, 58, 23, 58)) {
        std::vector<RoomDefinition*> roomDefinitions = generateRoomGraph(random);
        sourceRoom->claimed = true;
        childPieces.push_back(std::make_unique<OceanMonumentEntryRoom>(direction, sourceRoom));
        childPieces.push_back(std::make_unique<OceanMonumentCoreRoom>(direction, coreRoom));
        for (RoomDefinition* definition : roomDefinitions) {
            if (definition->claimed || definition->isSpecial()) continue;
            for (int fitter = 0; fitter < 7; ++fitter)
                if (fits(fitter, definition)) {
                    childPieces.push_back(create(fitter, direction, definition, random));
                    break;
                }
        }
        const BlockPos offset = getWorldPos(9, 0, 22);
        for (auto& child : childPieces) child->box.move(offset.x, offset.y, offset.z);
        const BBox leftWing = BBox::fromCorners(getWorldPos(1, 1, 1), getWorldPos(23, 8, 21));
        const BBox rightWing = BBox::fromCorners(getWorldPos(34, 1, 1), getWorldPos(56, 8, 21));
        const BBox penthouse = BBox::fromCorners(getWorldPos(22, 13, 22), getWorldPos(35, 17, 35));
        int wingRandom = random.nextInt();
        childPieces.push_back(std::make_unique<OceanMonumentWingRoom>(direction, leftWing, wingRandom++));
        childPieces.push_back(std::make_unique<OceanMonumentWingRoom>(direction, rightWing, wingRandom++));
        childPieces.push_back(std::make_unique<OceanMonumentPenthouse>(direction, penthouse));
    }

    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos& referencePos) override {
        const int waterHeight = std::max(level.level.seaLevel(), 64) - box.minY;
        generateWaterBox(level, chunkBB, 0, 0, 0, 58, waterHeight, 58);
        generateWing(false, 0, level, random, chunkBB);
        generateWing(true, 33, level, random, chunkBB);
        generateEntranceArchs(level, random, chunkBB);
        generateEntranceWall(level, random, chunkBB);
        generateRoofPiece(level, random, chunkBB);
        generateLowerWall(level, random, chunkBB);
        generateMiddleWall(level, random, chunkBB);
        generateUpperWall(level, random, chunkBB);
        for (int pillarX = 0; pillarX < 7; ++pillarX) {
            int pillarZ = 0;
            while (pillarZ < 7) {
                if (pillarZ == 0 && pillarX == 3) pillarZ = 6;
                const int bx = pillarX * 9, bz = pillarZ * 9;
                for (int w = 0; w < 4; ++w)
                    for (int d = 0; d < 4; ++d) {
                        placeBlock(level, BASE_LIGHT, bx + w, 0, bz + d, chunkBB);
                        fillColumnDown(level, BASE_LIGHT, bx + w, -1, bz + d, chunkBB);
                    }
                if (pillarX != 0 && pillarX != 6) pillarZ += 6;
                else ++pillarZ;
            }
        }
        for (int i = 0; i < 5; ++i) {
            generateWaterBox(level, chunkBB, -1 - i, 0 + i * 2, -1 - i, -1 - i, 23, 58 + i);
            generateWaterBox(level, chunkBB, 58 + i, 0 + i * 2, -1 - i, 58 + i, 23, 58 + i);
            generateWaterBox(level, chunkBB, 0 - i, 0 + i * 2, -1 - i, 57 + i, 23, -1 - i);
            generateWaterBox(level, chunkBB, 0 - i, 0 + i * 2, 58 + i, 57 + i, 23, 58 + i);
        }
        for (auto& child : childPieces)
            if (child->box.intersects(chunkBB)) child->postProcess(level, random, chunkBB, referencePos);
    }

private:
    bool fits(int fitter, RoomDefinition* definition) const {
        RoomDefinition* d = definition;
        switch (fitter) {
        case 0:  // FitDoubleXYRoom
            if (d->hasOpening[int(East)] && !d->connections[int(East)]->claimed && d->hasOpening[int(Up)] && !d->connections[int(Up)]->claimed) {
                RoomDefinition* e = d->connections[int(East)];
                return e->hasOpening[int(Up)] && !e->connections[int(Up)]->claimed;
            }
            return false;
        case 1:  // FitDoubleYZRoom
            if (d->hasOpening[int(North)] && !d->connections[int(North)]->claimed && d->hasOpening[int(Up)] && !d->connections[int(Up)]->claimed) {
                RoomDefinition* n = d->connections[int(North)];
                return n->hasOpening[int(Up)] && !n->connections[int(Up)]->claimed;
            }
            return false;
        case 2: return d->hasOpening[int(North)] && !d->connections[int(North)]->claimed;  // FitDoubleZRoom
        case 3: return d->hasOpening[int(East)] && !d->connections[int(East)]->claimed;    // FitDoubleXRoom
        case 4: return d->hasOpening[int(Up)] && !d->connections[int(Up)]->claimed;        // FitDoubleYRoom
        case 5:  // FitSimpleTopRoom
            return !d->hasOpening[int(West)] && !d->hasOpening[int(East)] && !d->hasOpening[int(North)] && !d->hasOpening[int(South)] &&
                   !d->hasOpening[int(Up)];
        default: return true;  // FitSimpleRoom
        }
    }

    std::unique_ptr<OceanMonumentPiece> create(int fitter, Direction o, RoomDefinition* d, RandomSource& random) {
        switch (fitter) {
        case 0:
            d->claimed = true;
            d->connections[int(East)]->claimed = true;
            d->connections[int(Up)]->claimed = true;
            d->connections[int(East)]->connections[int(Up)]->claimed = true;
            return std::make_unique<OceanMonumentDoubleXYRoom>(o, d);
        case 1:
            d->claimed = true;
            d->connections[int(North)]->claimed = true;
            d->connections[int(Up)]->claimed = true;
            d->connections[int(North)]->connections[int(Up)]->claimed = true;
            return std::make_unique<OceanMonumentDoubleYZRoom>(o, d);
        case 2: {
            RoomDefinition* source = d;
            if (!d->hasOpening[int(North)] || d->connections[int(North)]->claimed) source = d->connections[int(South)];
            source->claimed = true;
            source->connections[int(North)]->claimed = true;
            return std::make_unique<OceanMonumentDoubleZRoom>(o, source);
        }
        case 3:
            d->claimed = true;
            d->connections[int(East)]->claimed = true;
            return std::make_unique<OceanMonumentDoubleXRoom>(o, d);
        case 4:
            d->claimed = true;
            d->connections[int(Up)]->claimed = true;
            return std::make_unique<OceanMonumentDoubleYRoom>(o, d);
        case 5:
            d->claimed = true;
            return std::make_unique<OceanMonumentSimpleTopRoom>(o, d);
        default:
            d->claimed = true;
            return std::make_unique<OceanMonumentSimpleRoom>(o, d, random);
        }
    }

    RoomDefinition* newRoom(int index) {
        rooms.push_back(std::make_unique<RoomDefinition>(index));
        return rooms.back().get();
    }

    std::vector<RoomDefinition*> generateRoomGraph(RandomSource& random) {
        RoomDefinition* roomGrid[75] = {};
        for (int x = 0; x < 5; ++x)
            for (int z = 0; z < 4; ++z) roomGrid[getRoomIndex(x, 0, z)] = newRoom(getRoomIndex(x, 0, z));
        for (int x = 0; x < 5; ++x)
            for (int z = 0; z < 4; ++z) roomGrid[getRoomIndex(x, 1, z)] = newRoom(getRoomIndex(x, 1, z));
        for (int x = 1; x < 4; ++x)
            for (int z = 0; z < 2; ++z) roomGrid[getRoomIndex(x, 2, z)] = newRoom(getRoomIndex(x, 2, z));
        sourceRoom = roomGrid[GRIDROOM_SOURCE_INDEX];
        for (int x = 0; x < 5; ++x)
            for (int z = 0; z < 5; ++z)
                for (int y = 0; y < 3; ++y) {
                    const int pos = getRoomIndex(x, y, z);
                    if (!roomGrid[pos]) continue;
                    for (int dd = 0; dd < 6; ++dd) {
                        const Direction direction = Direction(dd);
                        const int nx = x + stepX(direction), ny = y + stepY(direction), nz = z + stepZ(direction);
                        if (nx < 0 || nx >= 5 || nz < 0 || nz >= 5 || ny < 0 || ny >= 3) continue;
                        const int np = getRoomIndex(nx, ny, nz);
                        if (!roomGrid[np]) continue;
                        if (nz == z) roomGrid[pos]->setConnection(direction, roomGrid[np]);
                        else roomGrid[pos]->setConnection(opposite(direction), roomGrid[np]);
                    }
                }
        RoomDefinition* roofRoom = newRoom(1003);
        RoomDefinition* leftWing = newRoom(1001);
        RoomDefinition* rightWing = newRoom(1002);
        roomGrid[GRIDROOM_TOP_CONNECT_INDEX]->setConnection(Up, roofRoom);
        roomGrid[GRIDROOM_LEFTWING_CONNECT_INDEX]->setConnection(South, leftWing);
        roomGrid[GRIDROOM_RIGHTWING_CONNECT_INDEX]->setConnection(South, rightWing);
        roofRoom->claimed = leftWing->claimed = rightWing->claimed = true;
        sourceRoom->isSource = true;
        coreRoom = roomGrid[getRoomIndex(random.nextInt(4), 0, 2)];
        coreRoom->claimed = true;
        coreRoom->connections[int(East)]->claimed = true;
        coreRoom->connections[int(North)]->claimed = true;
        coreRoom->connections[int(East)]->connections[int(North)]->claimed = true;
        coreRoom->connections[int(Up)]->claimed = true;
        coreRoom->connections[int(East)]->connections[int(Up)]->claimed = true;
        coreRoom->connections[int(North)]->connections[int(Up)]->claimed = true;
        coreRoom->connections[int(East)]->connections[int(North)]->connections[int(Up)]->claimed = true;
        std::vector<RoomDefinition*> roomDefs;
        for (RoomDefinition* d : roomGrid)
            if (d) {
                d->updateOpenings();
                roomDefs.push_back(d);
            }
        roofRoom->updateOpenings();
        for (size_t i = roomDefs.size(); i > 1; --i) std::swap(roomDefs[i - 1], roomDefs[size_t(random.nextInt(int(i)))]);
        int scanIndex = 1;
        for (RoomDefinition* d : roomDefs) {
            int closeCount = 0, attemptCount = 0;
            while (closeCount < 2 && attemptCount < 5) {
                ++attemptCount;
                const int f = random.nextInt(6);
                if (!d->hasOpening[f]) continue;
                const int of = int(opposite(Direction(f)));
                d->hasOpening[f] = false;
                d->connections[f]->hasOpening[of] = false;
                if (d->findSource(scanIndex++) && d->connections[f]->findSource(scanIndex++)) {
                    ++closeCount;
                } else {
                    d->hasOpening[f] = true;
                    d->connections[f]->hasOpening[of] = true;
                }
            }
        }
        roomDefs.push_back(roofRoom);
        roomDefs.push_back(leftWing);
        roomDefs.push_back(rightWing);
        return roomDefs;
    }

    void generateWing(bool isFlipped, int xoff, PlaceCtx& level, RandomSource& random, const BBox& chunkBB) {
        (void)random;
        int sectionWidth = 24;
        if (chunkIntersects(chunkBB, xoff, 0, xoff + 23, 20)) {
        generateBox(level, chunkBB, xoff + 0, 0, 0, xoff + 24, 0, 20, BASE_GRAY, BASE_GRAY, false);
        generateWaterBox(level, chunkBB, xoff + 0, 1, 0, xoff + 24, 10, 20);

        for (int i = 0; i < 4; i++) {
        generateBox(level, chunkBB, xoff + i, i + 1, i, xoff + i, i + 1, 20, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, xoff + i + 7, i + 5, i + 7, xoff + i + 7, i + 5, 20, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, xoff + 17 - i, i + 5, i + 7, xoff + 17 - i, i + 5, 20, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, xoff + 24 - i, i + 1, i, xoff + 24 - i, i + 1, 20, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, xoff + i + 1, i + 1, i, xoff + 23 - i, i + 1, i, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, xoff + i + 8, i + 5, i + 7, xoff + 16 - i, i + 5, i + 7, BASE_LIGHT, BASE_LIGHT, false);
        }

        generateBox(level, chunkBB, xoff + 4, 4, 4, xoff + 6, 4, 20, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, xoff + 7, 4, 4, xoff + 17, 4, 6, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, xoff + 18, 4, 4, xoff + 20, 4, 20, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, xoff + 11, 8, 11, xoff + 13, 8, 20, BASE_GRAY, BASE_GRAY, false);
        placeBlock(level, DOT_DECO_DATA, xoff + 12, 9, 12, chunkBB);
        placeBlock(level, DOT_DECO_DATA, xoff + 12, 9, 15, chunkBB);
        placeBlock(level, DOT_DECO_DATA, xoff + 12, 9, 18, chunkBB);
        int leftPos = xoff + (isFlipped ? 19 : 5);
        int rightPos = xoff + (isFlipped ? 5 : 19);

        for (int z = 20; z >= 5; z -= 3) {
        placeBlock(level, DOT_DECO_DATA, leftPos, 5, z, chunkBB);
        }

        for (int z = 19; z >= 7; z -= 3) {
        placeBlock(level, DOT_DECO_DATA, rightPos, 5, z, chunkBB);
        }

        for (int i = 0; i < 4; i++) {
        int pos = isFlipped ? xoff + 24 - (17 - i * 3) : xoff + 17 - i * 3;
        placeBlock(level, DOT_DECO_DATA, pos, 5, 5, chunkBB);
        }

        placeBlock(level, DOT_DECO_DATA, rightPos, 5, 5, chunkBB);
        generateBox(level, chunkBB, xoff + 11, 1, 12, xoff + 13, 7, 12, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, xoff + 12, 1, 11, xoff + 12, 7, 13, BASE_GRAY, BASE_GRAY, false);
        }
    }
    void generateEntranceArchs(PlaceCtx& level, RandomSource& random, const BBox& chunkBB) {
        (void)random;
        if (chunkIntersects(chunkBB, 22, 5, 35, 17)) {
        generateWaterBox(level, chunkBB, 25, 0, 0, 32, 8, 20);

        for (int i = 0; i < 4; i++) {
        generateBox(level, chunkBB, 24, 2, 5 + i * 4, 24, 4, 5 + i * 4, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 22, 4, 5 + i * 4, 23, 4, 5 + i * 4, BASE_LIGHT, BASE_LIGHT, false);
        placeBlock(level, BASE_LIGHT, 25, 5, 5 + i * 4, chunkBB);
        placeBlock(level, BASE_LIGHT, 26, 6, 5 + i * 4, chunkBB);
        placeBlock(level, LAMP_BLOCK, 26, 5, 5 + i * 4, chunkBB);
        generateBox(level, chunkBB, 33, 2, 5 + i * 4, 33, 4, 5 + i * 4, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 34, 4, 5 + i * 4, 35, 4, 5 + i * 4, BASE_LIGHT, BASE_LIGHT, false);
        placeBlock(level, BASE_LIGHT, 32, 5, 5 + i * 4, chunkBB);
        placeBlock(level, BASE_LIGHT, 31, 6, 5 + i * 4, chunkBB);
        placeBlock(level, LAMP_BLOCK, 31, 5, 5 + i * 4, chunkBB);
        generateBox(level, chunkBB, 27, 6, 5 + i * 4, 30, 6, 5 + i * 4, BASE_GRAY, BASE_GRAY, false);
        }
        }
    }
    void generateEntranceWall(PlaceCtx& level, RandomSource& random, const BBox& chunkBB) {
        (void)random;
        if (chunkIntersects(chunkBB, 15, 20, 42, 21)) {
        generateBox(level, chunkBB, 15, 0, 21, 42, 0, 21, BASE_GRAY, BASE_GRAY, false);
        generateWaterBox(level, chunkBB, 26, 1, 21, 31, 3, 21);
        generateBox(level, chunkBB, 21, 12, 21, 36, 12, 21, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 17, 11, 21, 40, 11, 21, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 16, 10, 21, 41, 10, 21, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 15, 7, 21, 42, 9, 21, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 16, 6, 21, 41, 6, 21, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 17, 5, 21, 40, 5, 21, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 21, 4, 21, 36, 4, 21, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 22, 3, 21, 26, 3, 21, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 31, 3, 21, 35, 3, 21, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 23, 2, 21, 25, 2, 21, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 32, 2, 21, 34, 2, 21, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 28, 4, 20, 29, 4, 21, BASE_LIGHT, BASE_LIGHT, false);
        placeBlock(level, BASE_LIGHT, 27, 3, 21, chunkBB);
        placeBlock(level, BASE_LIGHT, 30, 3, 21, chunkBB);
        placeBlock(level, BASE_LIGHT, 26, 2, 21, chunkBB);
        placeBlock(level, BASE_LIGHT, 31, 2, 21, chunkBB);
        placeBlock(level, BASE_LIGHT, 25, 1, 21, chunkBB);
        placeBlock(level, BASE_LIGHT, 32, 1, 21, chunkBB);

        for (int i = 0; i < 7; i++) {
        placeBlock(level, BASE_BLACK, 28 - i, 6 + i, 21, chunkBB);
        placeBlock(level, BASE_BLACK, 29 + i, 6 + i, 21, chunkBB);
        }

        for (int i = 0; i < 4; i++) {
        placeBlock(level, BASE_BLACK, 28 - i, 9 + i, 21, chunkBB);
        placeBlock(level, BASE_BLACK, 29 + i, 9 + i, 21, chunkBB);
        }

        placeBlock(level, BASE_BLACK, 28, 12, 21, chunkBB);
        placeBlock(level, BASE_BLACK, 29, 12, 21, chunkBB);

        for (int i = 0; i < 3; i++) {
        placeBlock(level, BASE_BLACK, 22 - i * 2, 8, 21, chunkBB);
        placeBlock(level, BASE_BLACK, 22 - i * 2, 9, 21, chunkBB);
        placeBlock(level, BASE_BLACK, 35 + i * 2, 8, 21, chunkBB);
        placeBlock(level, BASE_BLACK, 35 + i * 2, 9, 21, chunkBB);
        }

        generateWaterBox(level, chunkBB, 15, 13, 21, 42, 15, 21);
        generateWaterBox(level, chunkBB, 15, 1, 21, 15, 6, 21);
        generateWaterBox(level, chunkBB, 16, 1, 21, 16, 5, 21);
        generateWaterBox(level, chunkBB, 17, 1, 21, 20, 4, 21);
        generateWaterBox(level, chunkBB, 21, 1, 21, 21, 3, 21);
        generateWaterBox(level, chunkBB, 22, 1, 21, 22, 2, 21);
        generateWaterBox(level, chunkBB, 23, 1, 21, 24, 1, 21);
        generateWaterBox(level, chunkBB, 42, 1, 21, 42, 6, 21);
        generateWaterBox(level, chunkBB, 41, 1, 21, 41, 5, 21);
        generateWaterBox(level, chunkBB, 37, 1, 21, 40, 4, 21);
        generateWaterBox(level, chunkBB, 36, 1, 21, 36, 3, 21);
        generateWaterBox(level, chunkBB, 33, 1, 21, 34, 1, 21);
        generateWaterBox(level, chunkBB, 35, 1, 21, 35, 2, 21);
        }
    }
    void generateRoofPiece(PlaceCtx& level, RandomSource& random, const BBox& chunkBB) {
        (void)random;
        if (chunkIntersects(chunkBB, 21, 21, 36, 36)) {
        generateBox(level, chunkBB, 21, 0, 22, 36, 0, 36, BASE_GRAY, BASE_GRAY, false);
        generateWaterBox(level, chunkBB, 21, 1, 22, 36, 23, 36);

        for (int i = 0; i < 4; i++) {
        generateBox(level, chunkBB, 21 + i, 13 + i, 21 + i, 36 - i, 13 + i, 21 + i, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 21 + i, 13 + i, 36 - i, 36 - i, 13 + i, 36 - i, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 21 + i, 13 + i, 22 + i, 21 + i, 13 + i, 35 - i, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 36 - i, 13 + i, 22 + i, 36 - i, 13 + i, 35 - i, BASE_LIGHT, BASE_LIGHT, false);
        }

        generateBox(level, chunkBB, 25, 16, 25, 32, 16, 32, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 25, 17, 25, 25, 19, 25, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 32, 17, 25, 32, 19, 25, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 25, 17, 32, 25, 19, 32, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 32, 17, 32, 32, 19, 32, BASE_LIGHT, BASE_LIGHT, false);
        placeBlock(level, BASE_LIGHT, 26, 20, 26, chunkBB);
        placeBlock(level, BASE_LIGHT, 27, 21, 27, chunkBB);
        placeBlock(level, LAMP_BLOCK, 27, 20, 27, chunkBB);
        placeBlock(level, BASE_LIGHT, 26, 20, 31, chunkBB);
        placeBlock(level, BASE_LIGHT, 27, 21, 30, chunkBB);
        placeBlock(level, LAMP_BLOCK, 27, 20, 30, chunkBB);
        placeBlock(level, BASE_LIGHT, 31, 20, 31, chunkBB);
        placeBlock(level, BASE_LIGHT, 30, 21, 30, chunkBB);
        placeBlock(level, LAMP_BLOCK, 30, 20, 30, chunkBB);
        placeBlock(level, BASE_LIGHT, 31, 20, 26, chunkBB);
        placeBlock(level, BASE_LIGHT, 30, 21, 27, chunkBB);
        placeBlock(level, LAMP_BLOCK, 30, 20, 27, chunkBB);
        generateBox(level, chunkBB, 28, 21, 27, 29, 21, 27, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 27, 21, 28, 27, 21, 29, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 28, 21, 30, 29, 21, 30, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 30, 21, 28, 30, 21, 29, BASE_GRAY, BASE_GRAY, false);
        }
    }
    void generateLowerWall(PlaceCtx& level, RandomSource& random, const BBox& chunkBB) {
        (void)random;
        if (chunkIntersects(chunkBB, 0, 21, 6, 58)) {
        generateBox(level, chunkBB, 0, 0, 21, 6, 0, 57, BASE_GRAY, BASE_GRAY, false);
        generateWaterBox(level, chunkBB, 0, 1, 21, 6, 7, 57);
        generateBox(level, chunkBB, 4, 4, 21, 6, 4, 53, BASE_GRAY, BASE_GRAY, false);

        for (int i = 0; i < 4; i++) {
        generateBox(level, chunkBB, i, i + 1, 21, i, i + 1, 57 - i, BASE_LIGHT, BASE_LIGHT, false);
        }

        for (int z = 23; z < 53; z += 3) {
        placeBlock(level, DOT_DECO_DATA, 5, 5, z, chunkBB);
        }

        placeBlock(level, DOT_DECO_DATA, 5, 5, 52, chunkBB);

        for (int i = 0; i < 4; i++) {
        generateBox(level, chunkBB, i, i + 1, 21, i, i + 1, 57 - i, BASE_LIGHT, BASE_LIGHT, false);
        }

        generateBox(level, chunkBB, 4, 1, 52, 6, 3, 52, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 5, 1, 51, 5, 3, 53, BASE_GRAY, BASE_GRAY, false);
        }

        if (chunkIntersects(chunkBB, 51, 21, 58, 58)) {
        generateBox(level, chunkBB, 51, 0, 21, 57, 0, 57, BASE_GRAY, BASE_GRAY, false);
        generateWaterBox(level, chunkBB, 51, 1, 21, 57, 7, 57);
        generateBox(level, chunkBB, 51, 4, 21, 53, 4, 53, BASE_GRAY, BASE_GRAY, false);

        for (int i = 0; i < 4; i++) {
        generateBox(level, chunkBB, 57 - i, i + 1, 21, 57 - i, i + 1, 57 - i, BASE_LIGHT, BASE_LIGHT, false);
        }

        for (int z = 23; z < 53; z += 3) {
        placeBlock(level, DOT_DECO_DATA, 52, 5, z, chunkBB);
        }

        placeBlock(level, DOT_DECO_DATA, 52, 5, 52, chunkBB);
        generateBox(level, chunkBB, 51, 1, 52, 53, 3, 52, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 52, 1, 51, 52, 3, 53, BASE_GRAY, BASE_GRAY, false);
        }

        if (chunkIntersects(chunkBB, 0, 51, 57, 57)) {
        generateBox(level, chunkBB, 7, 0, 51, 50, 0, 57, BASE_GRAY, BASE_GRAY, false);
        generateWaterBox(level, chunkBB, 7, 1, 51, 50, 10, 57);

        for (int i = 0; i < 4; i++) {
        generateBox(level, chunkBB, i + 1, i + 1, 57 - i, 56 - i, i + 1, 57 - i, BASE_LIGHT, BASE_LIGHT, false);
        }
        }
    }
    void generateMiddleWall(PlaceCtx& level, RandomSource& random, const BBox& chunkBB) {
        (void)random;
        if (chunkIntersects(chunkBB, 7, 21, 13, 50)) {
        generateBox(level, chunkBB, 7, 0, 21, 13, 0, 50, BASE_GRAY, BASE_GRAY, false);
        generateWaterBox(level, chunkBB, 7, 1, 21, 13, 10, 50);
        generateBox(level, chunkBB, 11, 8, 21, 13, 8, 53, BASE_GRAY, BASE_GRAY, false);

        for (int i = 0; i < 4; i++) {
        generateBox(level, chunkBB, i + 7, i + 5, 21, i + 7, i + 5, 54, BASE_LIGHT, BASE_LIGHT, false);
        }

        for (int z = 21; z <= 45; z += 3) {
        placeBlock(level, DOT_DECO_DATA, 12, 9, z, chunkBB);
        }
        }

        if (chunkIntersects(chunkBB, 44, 21, 50, 54)) {
        generateBox(level, chunkBB, 44, 0, 21, 50, 0, 50, BASE_GRAY, BASE_GRAY, false);
        generateWaterBox(level, chunkBB, 44, 1, 21, 50, 10, 50);
        generateBox(level, chunkBB, 44, 8, 21, 46, 8, 53, BASE_GRAY, BASE_GRAY, false);

        for (int i = 0; i < 4; i++) {
        generateBox(level, chunkBB, 50 - i, i + 5, 21, 50 - i, i + 5, 54, BASE_LIGHT, BASE_LIGHT, false);
        }

        for (int z = 21; z <= 45; z += 3) {
        placeBlock(level, DOT_DECO_DATA, 45, 9, z, chunkBB);
        }
        }

        if (chunkIntersects(chunkBB, 8, 44, 49, 54)) {
        generateBox(level, chunkBB, 14, 0, 44, 43, 0, 50, BASE_GRAY, BASE_GRAY, false);
        generateWaterBox(level, chunkBB, 14, 1, 44, 43, 10, 50);

        for (int x = 12; x <= 45; x += 3) {
        placeBlock(level, DOT_DECO_DATA, x, 9, 45, chunkBB);
        placeBlock(level, DOT_DECO_DATA, x, 9, 52, chunkBB);
        if (x == 12 || x == 18 || x == 24 || x == 33 || x == 39 || x == 45) {
        placeBlock(level, DOT_DECO_DATA, x, 9, 47, chunkBB);
        placeBlock(level, DOT_DECO_DATA, x, 9, 50, chunkBB);
        placeBlock(level, DOT_DECO_DATA, x, 10, 45, chunkBB);
        placeBlock(level, DOT_DECO_DATA, x, 10, 46, chunkBB);
        placeBlock(level, DOT_DECO_DATA, x, 10, 51, chunkBB);
        placeBlock(level, DOT_DECO_DATA, x, 10, 52, chunkBB);
        placeBlock(level, DOT_DECO_DATA, x, 11, 47, chunkBB);
        placeBlock(level, DOT_DECO_DATA, x, 11, 50, chunkBB);
        placeBlock(level, DOT_DECO_DATA, x, 12, 48, chunkBB);
        placeBlock(level, DOT_DECO_DATA, x, 12, 49, chunkBB);
        }
        }

        for (int i = 0; i < 3; i++) {
        generateBox(level, chunkBB, 8 + i, 5 + i, 54, 49 - i, 5 + i, 54, BASE_GRAY, BASE_GRAY, false);
        }

        generateBox(level, chunkBB, 11, 8, 54, 46, 8, 54, BASE_LIGHT, BASE_LIGHT, false);
        generateBox(level, chunkBB, 14, 8, 44, 43, 8, 53, BASE_GRAY, BASE_GRAY, false);
        }
    }
    void generateUpperWall(PlaceCtx& level, RandomSource& random, const BBox& chunkBB) {
        (void)random;
        if (chunkIntersects(chunkBB, 14, 21, 20, 43)) {
        generateBox(level, chunkBB, 14, 0, 21, 20, 0, 43, BASE_GRAY, BASE_GRAY, false);
        generateWaterBox(level, chunkBB, 14, 1, 22, 20, 14, 43);
        generateBox(level, chunkBB, 18, 12, 22, 20, 12, 39, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 18, 12, 21, 20, 12, 21, BASE_LIGHT, BASE_LIGHT, false);

        for (int i = 0; i < 4; i++) {
        generateBox(level, chunkBB, i + 14, i + 9, 21, i + 14, i + 9, 43 - i, BASE_LIGHT, BASE_LIGHT, false);
        }

        for (int z = 23; z <= 39; z += 3) {
        placeBlock(level, DOT_DECO_DATA, 19, 13, z, chunkBB);
        }
        }

        if (chunkIntersects(chunkBB, 37, 21, 43, 43)) {
        generateBox(level, chunkBB, 37, 0, 21, 43, 0, 43, BASE_GRAY, BASE_GRAY, false);
        generateWaterBox(level, chunkBB, 37, 1, 22, 43, 14, 43);
        generateBox(level, chunkBB, 37, 12, 22, 39, 12, 39, BASE_GRAY, BASE_GRAY, false);
        generateBox(level, chunkBB, 37, 12, 21, 39, 12, 21, BASE_LIGHT, BASE_LIGHT, false);

        for (int i = 0; i < 4; i++) {
        generateBox(level, chunkBB, 43 - i, i + 9, 21, 43 - i, i + 9, 43 - i, BASE_LIGHT, BASE_LIGHT, false);
        }

        for (int z = 23; z <= 39; z += 3) {
        placeBlock(level, DOT_DECO_DATA, 38, 13, z, chunkBB);
        }
        }

        if (chunkIntersects(chunkBB, 15, 37, 42, 43)) {
        generateBox(level, chunkBB, 21, 0, 37, 36, 0, 43, BASE_GRAY, BASE_GRAY, false);
        generateWaterBox(level, chunkBB, 21, 1, 37, 36, 14, 43);
        generateBox(level, chunkBB, 21, 12, 37, 36, 12, 39, BASE_GRAY, BASE_GRAY, false);

        for (int i = 0; i < 4; i++) {
        generateBox(level, chunkBB, 15 + i, i + 9, 43 - i, 42 - i, i + 9, 43 - i, BASE_LIGHT, BASE_LIGHT, false);
        }

        for (int x = 21; x <= 36; x += 3) {
        placeBlock(level, DOT_DECO_DATA, x, 13, 38, chunkBB);
        }
        }
    }
};

class OceanMonumentStructure final : public Structure {
public:
    std::optional<Stub> findGenerationPoint(GenCtx& c) const override {
        const int x = c.minBlockX() + 9, z = c.minBlockZ() + 9;
        // getBiomesWithin(x, seaLevel, z, 29): every biome around must allow the monument.
        const int y = c.seaLevel(), r = 29;
        const int qx0 = (x - r) >> 2, qy0 = (y - r) >> 2, qz0 = (z - r) >> 2, qx1 = (x + r) >> 2, qy1 = (y + r) >> 2, qz1 = (z + r) >> 2;
        for (int qz = qz0; qz <= qz1; ++qz)
            for (int qx = qx0; qx <= qx1; ++qx)
                for (int qy = qy0; qy <= qy1; ++qy)
                    if (!c.env.biomeInTag(c.noiseBiome(qx, qy, qz), "minecraft:required_ocean_monument_surrounding")) return std::nullopt;
        GenCtx* ctx = &c;
        return onTopOfChunkCenter(c, HType::OceanFloorWG, [ctx](PiecesBuilder& b) {
            const Direction o = randomHorizontal(ctx->random);
            b.add(std::make_unique<MonumentBuilding>(ctx->random, ctx->minBlockX() - 29, ctx->minBlockZ() - 29, o));
        });
    }
};

}  // namespace

void registerMonument() {
    registerStructureType("minecraft:ocean_monument", [](const json&, StructureEnv&) -> std::unique_ptr<Structure> {
        return std::make_unique<OceanMonumentStructure>();
    });
}

}  // namespace wg::st
