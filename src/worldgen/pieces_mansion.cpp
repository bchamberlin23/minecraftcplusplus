// Woodland mansions: the game's WoodlandMansionStructure and WoodlandMansionPieces (the mansion grid with recursive
// corridors and room identification, outer walls, roofs, carpets, indoor walls and the room templates per floor).
// Evokers, vindicators and allays are entities and are not placed here.
#include "worldgen/structure_template.h"

#include <list>

namespace wg::st {

namespace {

BlockPos above(const BlockPos& p, int n = 1) { return p.offset(0, n, 0); }
BlockPos rotatePos(const BlockPos& p, Rot r) {  // BlockPos.rotate
    switch (r) {
    case Rot::CW90: return {-p.z, p.y, p.x};
    case Rot::CW180: return {-p.x, p.y, -p.z};
    case Rot::CCW90: return {p.z, p.y, -p.x};
    default: return p;
    }
}

struct SimpleGrid {
    int width, height, valueIfOutside;
    std::vector<int> grid;
    SimpleGrid(int w, int h, int outside) : width(w), height(h), valueIfOutside(outside), grid(size_t(w) * h, 0) {}
    void set(int x, int y, int v) {
        if (x >= 0 && x < width && y >= 0 && y < height) grid[size_t(x) * height + y] = v;
    }
    void set(int x0, int y0, int x1, int y1, int v) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) set(x, y, v);
    }
    int get(int x, int y) const { return x >= 0 && x < width && y >= 0 && y < height ? grid[size_t(x) * height + y] : valueIfOutside; }
    void setif(int x, int y, int ifValue, int v) {
        if (get(x, y) == ifValue) set(x, y, v);
    }
    bool edgesTo(int x, int y, int v) const { return get(x - 1, y) == v || get(x + 1, y) == v || get(x, y + 1) == v || get(x, y - 1) == v; }
};

struct MansionGrid {
    RandomSource& random;
    SimpleGrid baseGrid, thirdFloorGrid;
    std::vector<SimpleGrid> floorRooms;
    int entranceX = 7, entranceY = 4;

    explicit MansionGrid(RandomSource& r) : random(r), baseGrid(11, 11, 5), thirdFloorGrid(11, 11, 5) {
        baseGrid.set(entranceX, entranceY, entranceX + 1, entranceY + 1, 3);
        baseGrid.set(entranceX - 1, entranceY, entranceX - 1, entranceY + 1, 2);
        baseGrid.set(entranceX + 2, entranceY - 2, entranceX + 3, entranceY + 3, 5);
        baseGrid.set(entranceX + 1, entranceY - 2, entranceX + 1, entranceY - 1, 1);
        baseGrid.set(entranceX + 1, entranceY + 2, entranceX + 1, entranceY + 3, 1);
        baseGrid.set(entranceX - 1, entranceY - 1, 1);
        baseGrid.set(entranceX - 1, entranceY + 2, 1);
        baseGrid.set(0, 0, 11, 1, 5);
        baseGrid.set(0, 9, 11, 11, 5);
        recursiveCorridor(baseGrid, entranceX, entranceY - 2, West, 6);
        recursiveCorridor(baseGrid, entranceX, entranceY + 3, West, 6);
        recursiveCorridor(baseGrid, entranceX - 2, entranceY - 1, West, 3);
        recursiveCorridor(baseGrid, entranceX - 2, entranceY + 2, West, 3);
        while (cleanEdges(baseGrid)) {
        }
        floorRooms.assign(3, SimpleGrid(11, 11, 5));
        identifyRooms(baseGrid, floorRooms[0]);
        identifyRooms(baseGrid, floorRooms[1]);
        floorRooms[0].set(entranceX + 1, entranceY, entranceX + 1, entranceY + 1, 8388608);
        floorRooms[1].set(entranceX + 1, entranceY, entranceX + 1, entranceY + 1, 8388608);
        thirdFloorGrid = SimpleGrid(baseGrid.width, baseGrid.height, 5);
        setupThirdFloor();
        identifyRooms(thirdFloorGrid, floorRooms[2]);
    }

    static bool isHouse(const SimpleGrid& g, int x, int y) {
        const int v = g.get(x, y);
        return v == 1 || v == 2 || v == 3 || v == 4;
    }
    bool isRoomId(const SimpleGrid&, int x, int y, int floor, int roomId) const { return (floorRooms[size_t(floor)].get(x, y) & 65535) == roomId; }
    std::optional<Direction> get1x2RoomDirection(const SimpleGrid& g, int x, int y, int floor, int roomId) const {
        for (Direction d : kPlaneHorizontal)
            if (isRoomId(g, x + stepX(d), y + stepZ(d), floor, roomId)) return d;
        return std::nullopt;
    }

    void recursiveCorridor(SimpleGrid& g, int x, int y, Direction heading, int depth) {
        if (depth <= 0) return;
        g.set(x, y, 1);
        g.setif(x + stepX(heading), y + stepZ(heading), 0, 1);
        for (int attempts = 0; attempts < 8; ++attempts) {
            const Direction nextDir = from2D(random.nextInt(4));
            if (nextDir != opposite(heading) && (nextDir != East || !random.nextBoolean())) {
                const int nx = x + stepX(heading), ny = y + stepZ(heading);
                if (g.get(nx + stepX(nextDir), ny + stepZ(nextDir)) == 0 && g.get(nx + stepX(nextDir) * 2, ny + stepZ(nextDir) * 2) == 0) {
                    recursiveCorridor(g, x + stepX(heading) + stepX(nextDir), y + stepZ(heading) + stepZ(nextDir), nextDir, depth - 1);
                    break;
                }
            }
        }
        const Direction cw = clockWise(heading), ccw = counterClockWise(heading);
        g.setif(x + stepX(cw), y + stepZ(cw), 0, 2);
        g.setif(x + stepX(ccw), y + stepZ(ccw), 0, 2);
        g.setif(x + stepX(heading) + stepX(cw), y + stepZ(heading) + stepZ(cw), 0, 2);
        g.setif(x + stepX(heading) + stepX(ccw), y + stepZ(heading) + stepZ(ccw), 0, 2);
        g.setif(x + stepX(heading) * 2, y + stepZ(heading) * 2, 0, 2);
        g.setif(x + stepX(cw) * 2, y + stepZ(cw) * 2, 0, 2);
        g.setif(x + stepX(ccw) * 2, y + stepZ(ccw) * 2, 0, 2);
    }

    bool cleanEdges(SimpleGrid& g) {
        bool touched = false;
        for (int y = 0; y < g.height; ++y)
            for (int x = 0; x < g.width; ++x) {
                if (g.get(x, y) != 0) continue;
                int direct = isHouse(g, x + 1, y) + isHouse(g, x - 1, y) + isHouse(g, x, y + 1) + isHouse(g, x, y - 1);
                if (direct >= 3) {
                    g.set(x, y, 2);
                    touched = true;
                } else if (direct == 2) {
                    const int diag = isHouse(g, x + 1, y + 1) + isHouse(g, x - 1, y + 1) + isHouse(g, x + 1, y - 1) + isHouse(g, x - 1, y - 1);
                    if (diag <= 1) {
                        g.set(x, y, 2);
                        touched = true;
                    }
                }
            }
        return touched;
    }

    void setupThirdFloor() {
        std::vector<std::pair<int, int>> potential;
        SimpleGrid& floor = floorRooms[1];
        for (int y = 0; y < thirdFloorGrid.height; ++y)
            for (int x = 0; x < thirdFloorGrid.width; ++x) {
                const int data = floor.get(x, y);
                if ((data & 983040) == 131072 && (data & 2097152) == 2097152) potential.push_back({x, y});
            }
        if (potential.empty()) {
            thirdFloorGrid.set(0, 0, thirdFloorGrid.width, thirdFloorGrid.height, 5);
            return;
        }
        const auto [rx, ry] = potential[size_t(random.nextInt(int(potential.size())))];
        const int data = floor.get(rx, ry);
        floor.set(rx, ry, data | 4194304);
        const Direction roomDir = get1x2RoomDirection(baseGrid, rx, ry, 1, data & 65535).value_or(North);
        const int endX = rx + stepX(roomDir), endY = ry + stepZ(roomDir);
        for (int y = 0; y < thirdFloorGrid.height; ++y)
            for (int x = 0; x < thirdFloorGrid.width; ++x) {
                if (!isHouse(baseGrid, x, y)) {
                    thirdFloorGrid.set(x, y, 5);
                } else if (x == rx && y == ry) {
                    thirdFloorGrid.set(x, y, 3);
                } else if (x == endX && y == endY) {
                    thirdFloorGrid.set(x, y, 3);
                    floorRooms[2].set(x, y, 8388608);
                }
            }
        std::vector<Direction> corridors;
        for (Direction d : kPlaneHorizontal)
            if (thirdFloorGrid.get(endX + stepX(d), endY + stepZ(d)) == 0) corridors.push_back(d);
        if (corridors.empty()) {
            thirdFloorGrid.set(0, 0, thirdFloorGrid.width, thirdFloorGrid.height, 5);
            floor.set(rx, ry, data);
        } else {
            const Direction cd = corridors[size_t(random.nextInt(int(corridors.size())))];
            recursiveCorridor(thirdFloorGrid, endX + stepX(cd), endY + stepZ(cd), cd, 4);
            while (cleanEdges(thirdFloorGrid)) {
            }
        }
    }

    void identifyRooms(const SimpleGrid& from, SimpleGrid& rooms) {
        std::vector<std::pair<int, int>> pos;
        for (int y = 0; y < from.height; ++y)
            for (int x = 0; x < from.width; ++x)
                if (from.get(x, y) == 2) pos.push_back({x, y});
        for (size_t i = pos.size(); i > 1; --i) std::swap(pos[i - 1], pos[size_t(random.nextInt(int(i)))]);
        int roomId = 10;
        for (const auto& [xx, y] : pos) {
            if (rooms.get(xx, y) != 0) continue;
            int x0 = xx, x1 = xx, y0 = y, y1 = y, type = 65536;
            if (rooms.get(xx + 1, y) == 0 && rooms.get(xx, y + 1) == 0 && rooms.get(xx + 1, y + 1) == 0 && from.get(xx + 1, y) == 2 &&
                from.get(xx, y + 1) == 2 && from.get(xx + 1, y + 1) == 2) {
                x1 = xx + 1, y1 = y + 1, type = 262144;
            } else if (rooms.get(xx - 1, y) == 0 && rooms.get(xx, y + 1) == 0 && rooms.get(xx - 1, y + 1) == 0 && from.get(xx - 1, y) == 2 &&
                       from.get(xx, y + 1) == 2 && from.get(xx - 1, y + 1) == 2) {
                x0 = xx - 1, y1 = y + 1, type = 262144;
            } else if (rooms.get(xx - 1, y) == 0 && rooms.get(xx, y - 1) == 0 && rooms.get(xx - 1, y - 1) == 0 && from.get(xx - 1, y) == 2 &&
                       from.get(xx, y - 1) == 2 && from.get(xx - 1, y - 1) == 2) {
                x0 = xx - 1, y0 = y - 1, type = 262144;
            } else if (rooms.get(xx + 1, y) == 0 && from.get(xx + 1, y) == 2) {
                x1 = xx + 1, type = 131072;
            } else if (rooms.get(xx, y + 1) == 0 && from.get(xx, y + 1) == 2) {
                y1 = y + 1, type = 131072;
            } else if (rooms.get(xx - 1, y) == 0 && from.get(xx - 1, y) == 2) {
                x0 = xx - 1, type = 131072;
            } else if (rooms.get(xx, y - 1) == 0 && from.get(xx, y - 1) == 2) {
                y0 = y - 1, type = 131072;
            }
            int doorX = random.nextBoolean() ? x0 : x1;
            int doorY = random.nextBoolean() ? y0 : y1;
            int doorFlag = 2097152;
            if (!from.edgesTo(doorX, doorY, 1)) {
                doorX = doorX == x0 ? x1 : x0;
                doorY = doorY == y0 ? y1 : y0;
                if (!from.edgesTo(doorX, doorY, 1)) {
                    doorY = doorY == y0 ? y1 : y0;
                    if (!from.edgesTo(doorX, doorY, 1)) {
                        doorX = doorX == x0 ? x1 : x0;
                        doorY = doorY == y0 ? y1 : y0;
                        if (!from.edgesTo(doorX, doorY, 1)) {
                            doorFlag = 0;
                            doorX = x0;
                            doorY = y0;
                        }
                    }
                }
            }
            for (int ry = y0; ry <= y1; ++ry)
                for (int rx = x0; rx <= x1; ++rx) {
                    if (rx == doorX && ry == doorY) rooms.set(rx, ry, 1048576 | doorFlag | type | roomId);
                    else rooms.set(rx, ry, type | roomId);
                }
            ++roomId;
        }
    }
};

// FirstFloorRoomCollection / SecondFloorRoomCollection (the third floor uses the second's).
struct FloorRooms {
    bool first;
    std::string get1x1(RandomSource& r) const { return first ? "1x1_a" + std::to_string(r.nextInt(5) + 1) : "1x1_b" + std::to_string(r.nextInt(5) + 1); }
    std::string get1x1Secret(RandomSource& r) const { return "1x1_as" + std::to_string(r.nextInt(4) + 1); }
    std::string get1x2SideEntrance(RandomSource& r, bool stairs) const {
        if (first) return "1x2_a" + std::to_string(r.nextInt(9) + 1);
        return stairs ? "1x2_c_stairs" : "1x2_c" + std::to_string(r.nextInt(4) + 1);
    }
    std::string get1x2FrontEntrance(RandomSource& r, bool stairs) const {
        if (first) return "1x2_b" + std::to_string(r.nextInt(5) + 1);
        return stairs ? "1x2_d_stairs" : "1x2_d" + std::to_string(r.nextInt(5) + 1);
    }
    std::string get1x2Secret(RandomSource& r) const {
        return first ? "1x2_s" + std::to_string(r.nextInt(2) + 1) : "1x2_se" + std::to_string(r.nextInt(1) + 1);
    }
    std::string get2x2(RandomSource& r) const { return first ? "2x2_a" + std::to_string(r.nextInt(4) + 1) : "2x2_b" + std::to_string(r.nextInt(5) + 1); }
    std::string get2x2Secret(RandomSource&) const { return "2x2_s1"; }
};

class WoodlandMansionPiece final : public TemplatePiece {
public:
    WoodlandMansionPiece(const StructureEnv& env, const std::string& name, const BlockPos& pos, Rot r, Mir m = Mir::None)
        : TemplatePiece("minecraft:wmp", 0, env.templates->get("minecraft:woodland_mansion/" + name), makeSettings(env, m, r), pos) {}

    static PlaceSettings makeSettings(const StructureEnv& env, Mir m, Rot r) {
        PlaceSettings s;
        s.ignoreEntities = true;
        s.rotation = r;
        s.mirror = m;
        s.processors.push_back(blockIgnore(env, {"minecraft:structure_block"}));
        return s;
    }

protected:
    void handleDataMarker(PlaceCtx& level, const std::string& marker, const BlockPos& pos, RandomSource& random, const BBox& chunkBB) override {
        if (marker.starts_with("Chest")) {
            const Rot rot = settings.rotation;
            St chest = B("minecraft:chest");
            if (marker == "ChestWest") chest = chest.with("facing", rotate(rot, West));
            else if (marker == "ChestEast") chest = chest.with("facing", rotate(rot, East));
            else if (marker == "ChestSouth") chest = chest.with("facing", rotate(rot, South));
            else if (marker == "ChestNorth") chest = chest.with("facing", rotate(rot, North));
            createChest(level, chunkBB, random, pos, chest);
            return;
        }
        int mobs = 0;
        if (marker == "Mage" || marker == "Warrior") mobs = 1;
        else if (marker == "Group of Allays") mobs = level.random().nextInt(3) + 1;
        if (mobs > 0) level.set(pos, B("minecraft:air"));  // after each spawned mob
    }
};

struct PlacementData {
    Rot rotation;
    BlockPos position;
    std::string wallType;
};

class MansionPiecePlacer {
public:
    MansionPiecePlacer(const StructureEnv& env, RandomSource& r) : env_(env), random(r) {}
    using Pieces = std::vector<std::unique_ptr<Piece>>;

    void createMansion(const BlockPos& origin, Rot rotation, Pieces& pieces, MansionGrid& mansion) {
        PlacementData data{rotation, origin, "wall_flat"};
        PlacementData secondData;
        entrance(pieces, data);
        secondData.position = above(data.position, 8);
        secondData.rotation = data.rotation;
        secondData.wallType = "wall_window";
        const SimpleGrid& baseGrid = mansion.baseGrid;
        const SimpleGrid& thirdGrid = mansion.thirdFloorGrid;
        startX = mansion.entranceX + 1;
        startY = mansion.entranceY + 1;
        const int endX = mansion.entranceX + 1, endY = mansion.entranceY;
        traverseOuterWalls(pieces, data, baseGrid, South, startX, startY, endX, endY);
        traverseOuterWalls(pieces, secondData, baseGrid, South, startX, startY, endX, endY);
        PlacementData thirdData{data.rotation, above(data.position, 19), "wall_window"};
        bool done = false;
        for (int y = 0; y < thirdGrid.height && !done; ++y)
            for (int x = thirdGrid.width - 1; x >= 0 && !done; --x)
                if (MansionGrid::isHouse(thirdGrid, x, y)) {
                    thirdData.position = relative(thirdData.position, rotate(rotation, South), 8 + (y - startY) * 8);
                    thirdData.position = relative(thirdData.position, rotate(rotation, East), (x - startX) * 8);
                    traverseWallPiece(pieces, thirdData);
                    traverseOuterWalls(pieces, thirdData, thirdGrid, South, x, y, x, y);
                    done = true;
                }
        createRoof(pieces, above(origin, 16), rotation, baseGrid, &thirdGrid);
        createRoof(pieces, above(origin, 27), rotation, thirdGrid, nullptr);
        const FloorRooms collections[3] = {{true}, {false}, {false}};
        for (int floorNum = 0; floorNum < 3; ++floorNum) {
            const BlockPos floorOrigin = above(origin, 8 * floorNum + (floorNum == 2 ? 3 : 0));
            const SimpleGrid& rooms = mansion.floorRooms[size_t(floorNum)];
            const SimpleGrid& grid = floorNum == 2 ? thirdGrid : baseGrid;
            const std::string southPiece = floorNum == 0 ? "carpet_south_1" : "carpet_south_2";
            const std::string westPiece = floorNum == 0 ? "carpet_west_1" : "carpet_west_2";
            for (int y = 0; y < grid.height; ++y)
                for (int xx = 0; xx < grid.width; ++xx) {
                    if (grid.get(xx, y) != 1) continue;
                    BlockPos pos = relative(floorOrigin, rotate(rotation, South), 8 + (y - startY) * 8);
                    pos = relative(pos, rotate(rotation, East), (xx - startX) * 8);
                    add(pieces, "corridor_floor", pos, rotation);
                    if (grid.get(xx, y - 1) == 1 || (rooms.get(xx, y - 1) & 8388608) == 8388608)
                        add(pieces, "carpet_north", above(relative(pos, rotate(rotation, East), 1)), rotation);
                    if (grid.get(xx + 1, y) == 1 || (rooms.get(xx + 1, y) & 8388608) == 8388608)
                        add(pieces, "carpet_east", above(relative(relative(pos, rotate(rotation, South), 1), rotate(rotation, East), 5)), rotation);
                    if (grid.get(xx, y + 1) == 1 || (rooms.get(xx, y + 1) & 8388608) == 8388608)
                        add(pieces, southPiece, relative(relative(pos, rotate(rotation, South), 5), rotate(rotation, West), 1), rotation);
                    if (grid.get(xx - 1, y) == 1 || (rooms.get(xx - 1, y) & 8388608) == 8388608)
                        add(pieces, westPiece, relative(relative(pos, rotate(rotation, West), 1), rotate(rotation, North), 1), rotation);
                }
            const std::string wallPiece = floorNum == 0 ? "indoors_wall_1" : "indoors_wall_2";
            const std::string doorPiece = floorNum == 0 ? "indoors_door_1" : "indoors_door_2";
            std::vector<Direction> doorDirs;
            for (int y = 0; y < grid.height; ++y)
                for (int xxx = 0; xxx < grid.width; ++xxx) {
                    bool thirdFloorStartRoom = floorNum == 2 && grid.get(xxx, y) == 3;
                    if (grid.get(xxx, y) != 2 && !thirdFloorStartRoom) continue;
                    const int roomData = rooms.get(xxx, y);
                    const int roomType = roomData & 983040;
                    const int roomId = roomData & 65535;
                    thirdFloorStartRoom = thirdFloorStartRoom && (roomData & 8388608) == 8388608;
                    doorDirs.clear();
                    if ((roomData & 2097152) == 2097152)
                        for (Direction d : kPlaneHorizontal)
                            if (grid.get(xxx + stepX(d), y + stepZ(d)) == 1) doorDirs.push_back(d);
                    std::optional<Direction> doorDir;
                    if (!doorDirs.empty()) doorDir = doorDirs[size_t(random.nextInt(int(doorDirs.size())))];
                    else if ((roomData & 1048576) == 1048576) doorDir = Up;
                    BlockPos roomPos = relative(floorOrigin, rotate(rotation, South), 8 + (y - startY) * 8);
                    roomPos = relative(roomPos, rotate(rotation, East), -1 + (xxx - startX) * 8);
                    if (MansionGrid::isHouse(grid, xxx - 1, y) && !mansion.isRoomId(grid, xxx - 1, y, floorNum, roomId))
                        add(pieces, doorDir == West ? doorPiece : wallPiece, roomPos, rotation);
                    if (grid.get(xxx + 1, y) == 1 && !thirdFloorStartRoom)
                        add(pieces, doorDir == East ? doorPiece : wallPiece, relative(roomPos, rotate(rotation, East), 8), rotation);
                    if (MansionGrid::isHouse(grid, xxx, y + 1) && !mansion.isRoomId(grid, xxx, y + 1, floorNum, roomId)) {
                        BlockPos p = relative(roomPos, rotate(rotation, South), 7);
                        p = relative(p, rotate(rotation, East), 7);
                        add(pieces, doorDir == South ? doorPiece : wallPiece, p, rotated(rotation, Rot::CW90));
                    }
                    if (grid.get(xxx, y - 1) == 1 && !thirdFloorStartRoom) {
                        BlockPos p = relative(roomPos, rotate(rotation, North), 1);
                        p = relative(p, rotate(rotation, East), 7);
                        add(pieces, doorDir == North ? doorPiece : wallPiece, p, rotated(rotation, Rot::CW90));
                    }
                    const FloorRooms& coll = collections[floorNum];
                    if (roomType == 65536) {
                        addRoom1x1(pieces, roomPos, rotation, doorDir, coll);
                    } else if (roomType == 131072 && doorDir) {
                        const auto roomDir = mansion.get1x2RoomDirection(grid, xxx, y, floorNum, roomId);
                        addRoom1x2(pieces, roomPos, rotation, roomDir, *doorDir, coll, (roomData & 4194304) == 4194304);
                    } else if (roomType == 262144 && doorDir && *doorDir != Up) {
                        Direction roomDir = clockWise(*doorDir);
                        if (!mansion.isRoomId(grid, xxx + stepX(roomDir), y + stepZ(roomDir), floorNum, roomId)) roomDir = opposite(roomDir);
                        addRoom2x2(pieces, roomPos, rotation, roomDir, *doorDir, coll);
                    } else if (roomType == 262144 && doorDir == Up) {
                        add(pieces, coll.get2x2Secret(random), relative(roomPos, rotate(rotation, East), 1), rotation, Mir::None);
                    }
                }
        }
    }

private:
    void add(Pieces& pieces, const std::string& name, const BlockPos& pos, Rot r, Mir m = Mir::None) {
        pieces.push_back(std::make_unique<WoodlandMansionPiece>(env_, name, pos, r, m));
    }

    void traverseOuterWalls(Pieces& pieces, PlacementData& data, const SimpleGrid& grid, Direction gridDirection, int sx, int sy, int endX,
                            int endY) {
        int gx = sx, gy = sy;
        const Direction startDirection = gridDirection;
        do {
            if (!MansionGrid::isHouse(grid, gx + stepX(gridDirection), gy + stepZ(gridDirection))) {
                traverseTurn(pieces, data);
                gridDirection = clockWise(gridDirection);
                if (gx != endX || gy != endY || startDirection != gridDirection) traverseWallPiece(pieces, data);
            } else if (MansionGrid::isHouse(grid, gx + stepX(gridDirection), gy + stepZ(gridDirection)) &&
                       MansionGrid::isHouse(grid, gx + stepX(gridDirection) + stepX(counterClockWise(gridDirection)),
                                            gy + stepZ(gridDirection) + stepZ(counterClockWise(gridDirection)))) {
                traverseInnerTurn(pieces, data);
                gx += stepX(gridDirection);
                gy += stepZ(gridDirection);
                gridDirection = counterClockWise(gridDirection);
            } else {
                gx += stepX(gridDirection);
                gy += stepZ(gridDirection);
                if (gx != endX || gy != endY || startDirection != gridDirection) traverseWallPiece(pieces, data);
            }
        } while (gx != endX || gy != endY || startDirection != gridDirection);
    }

    void createRoof(Pieces& pieces, const BlockPos& roofOrigin, Rot rotation, const SimpleGrid& grid, const SimpleGrid* aboveGrid) {
        auto cell = [&](int x, int y) {
            BlockPos p = relative(roofOrigin, rotate(rotation, South), 8 + (y - startY) * 8);
            return relative(p, rotate(rotation, East), (x - startX) * 8);
        };
        auto house = [](const SimpleGrid& g, int x, int y) { return MansionGrid::isHouse(g, x, y); };
        auto E = [&](const BlockPos& p, int n) { return relative(p, rotate(rotation, East), n); };
        auto Sth = [&](const BlockPos& p, int n) { return relative(p, rotate(rotation, South), n); };
        auto W = [&](const BlockPos& p, int n) { return relative(p, rotate(rotation, West), n); };
        auto N = [&](const BlockPos& p, int n) { return relative(p, rotate(rotation, North), n); };
        for (int y = 0; y < grid.height; ++y)
            for (int x = 0; x < grid.width; ++x) {
                const BlockPos position = cell(x, y);
                const bool isAbove = aboveGrid && house(*aboveGrid, x, y);
                if (!house(grid, x, y) || isAbove) continue;
                add(pieces, "roof", above(position, 3), rotation);
                if (!house(grid, x + 1, y)) add(pieces, "roof_front", E(position, 6), rotation);
                if (!house(grid, x - 1, y)) add(pieces, "roof_front", Sth(E(position, 0), 7), rotated(rotation, Rot::CW180));
                if (!house(grid, x, y - 1)) add(pieces, "roof_front", W(position, 1), rotated(rotation, Rot::CCW90));
                if (!house(grid, x, y + 1)) add(pieces, "roof_front", Sth(E(position, 6), 6), rotated(rotation, Rot::CW90));
            }
        if (aboveGrid)
            for (int y = 0; y < grid.height; ++y)
                for (int xx = 0; xx < grid.width; ++xx) {
                    const BlockPos p = cell(xx, y);
                    if (!house(grid, xx, y) || !house(*aboveGrid, xx, y)) continue;
                    if (!house(grid, xx + 1, y)) add(pieces, "small_wall", E(p, 7), rotation);
                    if (!house(grid, xx - 1, y)) add(pieces, "small_wall", Sth(W(p, 1), 6), rotated(rotation, Rot::CW180));
                    if (!house(grid, xx, y - 1)) add(pieces, "small_wall", N(W(p, 0), 1), rotated(rotation, Rot::CCW90));
                    if (!house(grid, xx, y + 1)) add(pieces, "small_wall", Sth(E(p, 6), 7), rotated(rotation, Rot::CW90));
                    if (!house(grid, xx + 1, y)) {
                        if (!house(grid, xx, y - 1)) add(pieces, "small_wall_corner", N(E(p, 7), 2), rotation);
                        if (!house(grid, xx, y + 1)) add(pieces, "small_wall_corner", Sth(E(p, 8), 7), rotated(rotation, Rot::CW90));
                    }
                    if (!house(grid, xx - 1, y)) {
                        if (!house(grid, xx, y - 1)) add(pieces, "small_wall_corner", N(W(p, 2), 1), rotated(rotation, Rot::CCW90));
                        if (!house(grid, xx, y + 1)) add(pieces, "small_wall_corner", Sth(W(p, 1), 8), rotated(rotation, Rot::CW180));
                    }
                }
        for (int y = 0; y < grid.height; ++y)
            for (int xxx = 0; xxx < grid.width; ++xxx) {
                const BlockPos p = cell(xxx, y);
                const bool isAbove = aboveGrid && house(*aboveGrid, xxx, y);
                if (!house(grid, xxx, y) || isAbove) continue;
                if (!house(grid, xxx + 1, y)) {
                    const BlockPos p2 = E(p, 6);
                    if (!house(grid, xxx, y + 1)) add(pieces, "roof_corner", Sth(p2, 6), rotation);
                    else if (house(grid, xxx + 1, y + 1)) add(pieces, "roof_inner_corner", Sth(p2, 5), rotation);
                    if (!house(grid, xxx, y - 1)) add(pieces, "roof_corner", p2, rotated(rotation, Rot::CCW90));
                    else if (house(grid, xxx + 1, y - 1)) add(pieces, "roof_inner_corner", N(E(p, 9), 2), rotated(rotation, Rot::CW90));
                }
                if (!house(grid, xxx - 1, y)) {
                    const BlockPos p2 = Sth(E(p, 0), 0);
                    if (!house(grid, xxx, y + 1)) add(pieces, "roof_corner", Sth(p2, 6), rotated(rotation, Rot::CW90));
                    else if (house(grid, xxx - 1, y + 1)) add(pieces, "roof_inner_corner", W(Sth(p2, 8), 3), rotated(rotation, Rot::CCW90));
                    if (!house(grid, xxx, y - 1)) add(pieces, "roof_corner", p2, rotated(rotation, Rot::CW180));
                    else if (house(grid, xxx - 1, y - 1)) add(pieces, "roof_inner_corner", Sth(p2, 1), rotated(rotation, Rot::CW180));
                }
            }
    }

    void entrance(Pieces& pieces, PlacementData& data) {
        add(pieces, "entrance", relative(data.position, rotate(data.rotation, West), 9), data.rotation);
        data.position = relative(data.position, rotate(data.rotation, South), 16);
    }
    void traverseWallPiece(Pieces& pieces, PlacementData& data) {
        add(pieces, data.wallType, relative(data.position, rotate(data.rotation, East), 7), data.rotation);
        data.position = relative(data.position, rotate(data.rotation, South), 8);
    }
    void traverseTurn(Pieces& pieces, PlacementData& data) {
        data.position = relative(data.position, rotate(data.rotation, South), -1);
        add(pieces, "wall_corner", data.position, data.rotation);
        data.position = relative(data.position, rotate(data.rotation, South), -7);
        data.position = relative(data.position, rotate(data.rotation, West), -6);
        data.rotation = rotated(data.rotation, Rot::CW90);
    }
    void traverseInnerTurn(Pieces&, PlacementData& data) {
        data.position = relative(data.position, rotate(data.rotation, South), 6);
        data.position = relative(data.position, rotate(data.rotation, East), 8);
        data.rotation = rotated(data.rotation, Rot::CCW90);
    }

    void addRoom1x1(Pieces& pieces, const BlockPos& roomPos, Rot rotation, std::optional<Direction> doorDir, const FloorRooms& rooms) {
        Rot pieceRot = Rot::None;
        std::string type = rooms.get1x1(random);
        if (doorDir != East) {
            if (doorDir == North) pieceRot = rotated(pieceRot, Rot::CCW90);
            else if (doorDir == West) pieceRot = rotated(pieceRot, Rot::CW180);
            else if (doorDir == South) pieceRot = rotated(pieceRot, Rot::CW90);
            else type = rooms.get1x1Secret(random);
        }
        BlockPos o = zeroPositionWithTransform({1, 0, 0}, Mir::None, pieceRot, 7, 7);
        pieceRot = rotated(pieceRot, rotation);
        o = rotatePos(o, rotation);
        add(pieces, type, roomPos.offset(o.x, 0, o.z), pieceRot);
    }

    void addRoom1x2(Pieces& pieces, const BlockPos& roomPos, Rot rotation, std::optional<Direction> roomDir, Direction doorDir,
                    const FloorRooms& rooms, bool stairs) {
        auto E = [&](const BlockPos& p, int n) { return relative(p, rotate(rotation, East), n); };
        auto Sth = [&](const BlockPos& p, int n) { return relative(p, rotate(rotation, South), n); };
        auto N = [&](const BlockPos& p, int n) { return relative(p, rotate(rotation, North), n); };
        auto W = [&](const BlockPos& p, int n) { return relative(p, rotate(rotation, West), n); };
        if (doorDir == East && roomDir == South) add(pieces, rooms.get1x2SideEntrance(random, stairs), E(roomPos, 1), rotation);
        else if (doorDir == East && roomDir == North) add(pieces, rooms.get1x2SideEntrance(random, stairs), Sth(E(roomPos, 1), 6), rotation, Mir::LeftRight);
        else if (doorDir == West && roomDir == North) add(pieces, rooms.get1x2SideEntrance(random, stairs), Sth(E(roomPos, 7), 6), rotated(rotation, Rot::CW180));
        else if (doorDir == West && roomDir == South) add(pieces, rooms.get1x2SideEntrance(random, stairs), E(roomPos, 7), rotation, Mir::FrontBack);
        else if (doorDir == South && roomDir == East) add(pieces, rooms.get1x2SideEntrance(random, stairs), E(roomPos, 1), rotated(rotation, Rot::CW90), Mir::LeftRight);
        else if (doorDir == South && roomDir == West) add(pieces, rooms.get1x2SideEntrance(random, stairs), E(roomPos, 7), rotated(rotation, Rot::CW90));
        else if (doorDir == North && roomDir == West) add(pieces, rooms.get1x2SideEntrance(random, stairs), Sth(E(roomPos, 7), 6), rotated(rotation, Rot::CW90), Mir::FrontBack);
        else if (doorDir == North && roomDir == East) add(pieces, rooms.get1x2SideEntrance(random, stairs), Sth(E(roomPos, 1), 6), rotated(rotation, Rot::CCW90));
        else if (doorDir == South && roomDir == North) add(pieces, rooms.get1x2FrontEntrance(random, stairs), N(E(roomPos, 1), 8), rotation);
        else if (doorDir == North && roomDir == South) add(pieces, rooms.get1x2FrontEntrance(random, stairs), Sth(E(roomPos, 7), 14), rotated(rotation, Rot::CW180));
        else if (doorDir == West && roomDir == East) add(pieces, rooms.get1x2FrontEntrance(random, stairs), E(roomPos, 15), rotated(rotation, Rot::CW90));
        else if (doorDir == East && roomDir == West) add(pieces, rooms.get1x2FrontEntrance(random, stairs), Sth(W(roomPos, 7), 6), rotated(rotation, Rot::CCW90));
        else if (doorDir == Up && roomDir == East) add(pieces, rooms.get1x2Secret(random), E(roomPos, 15), rotated(rotation, Rot::CW90));
        else if (doorDir == Up && roomDir == South) add(pieces, rooms.get1x2Secret(random), N(E(roomPos, 1), 0), rotation);
    }

    void addRoom2x2(Pieces& pieces, const BlockPos& roomPos, Rot rotation, Direction roomDir, Direction doorDir, const FloorRooms& rooms) {
        int east = 0, south = 0;
        Rot rot = rotation;
        Mir mirror = Mir::None;
        if (doorDir == East && roomDir == South) east = -7;
        else if (doorDir == East && roomDir == North) east = -7, south = 6, mirror = Mir::LeftRight;
        else if (doorDir == North && roomDir == East) east = 1, south = 14, rot = rotated(rotation, Rot::CCW90);
        else if (doorDir == North && roomDir == West) east = 7, south = 14, rot = rotated(rotation, Rot::CCW90), mirror = Mir::LeftRight;
        else if (doorDir == South && roomDir == West) east = 7, south = -8, rot = rotated(rotation, Rot::CW90);
        else if (doorDir == South && roomDir == East) east = 1, south = -8, rot = rotated(rotation, Rot::CW90), mirror = Mir::LeftRight;
        else if (doorDir == West && roomDir == North) east = 15, south = 6, rot = rotated(rotation, Rot::CW180);
        else if (doorDir == West && roomDir == South) east = 15, mirror = Mir::FrontBack;
        BlockPos pos = relative(roomPos, rotate(rotation, East), east);
        pos = relative(pos, rotate(rotation, South), south);
        add(pieces, rooms.get2x2(random), pos, rot, mirror);
    }

    const StructureEnv& env_;
    RandomSource& random;
    int startX = 0, startY = 0;
};

class WoodlandMansionStructure final : public Structure {
public:
    std::optional<Stub> findGenerationPoint(GenCtx& c) const override {
        const int bx = c.minBlockX() + 7, bz = c.minBlockZ() + 7;
        if (!c.couldStructureExistInColumn(bx, bz, c.minY() - 1, c.maxY() - 1)) return std::nullopt;
        const Rot rotation = randomRotation(c.random);
        // Structure.getLowestYIn5by5Box
        int ox = 5, oz = 5;
        if (rotation == Rot::CW90) ox = -5;
        else if (rotation == Rot::CW180) ox = -5, oz = -5;
        else if (rotation == Rot::CCW90) oz = -5;
        const BlockPos start{bx, c.lowestY(bx, bz, ox, oz), bz};
        if (start.y < 60) return std::nullopt;
        GenCtx* ctx = &c;
        return Stub{start, [ctx, start, rotation](PiecesBuilder& b) {
                        MansionGrid grid(ctx->random);
                        MansionPiecePlacer placer(ctx->env, ctx->random);
                        placer.createMansion(start, rotation, b.pieces, grid);
                    }};
    }

    void afterPlace(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const std::vector<std::unique_ptr<Piece>>& pieces) const override {
        const int minY = level.minY();
        BBox all = pieces.front()->box;
        for (const auto& p : pieces) all.encapsulate(p->box);
        const int yStart = all.minY;
        for (int x = chunkBB.minX; x <= chunkBB.maxX; ++x)
            for (int z = chunkBB.minZ; z <= chunkBB.maxZ; ++z) {
                const BlockPos p{x, yStart, z};
                if (level.get(p).isAir() || !all.isInside(p)) continue;
                bool inside = false;
                for (const auto& piece : pieces)
                    if (piece->box.isInside(p)) {
                        inside = true;
                        break;
                    }
                if (!inside) continue;
                for (int y = yStart - 1; y > minY; --y) {
                    const BlockPos q{x, y, z};
                    if (!level.get(q).isAir() && !level.b.liquid(level.get(q))) break;
                    level.set(q, B("minecraft:cobblestone"));
                }
            }
    }
};

}  // namespace

void registerMansion() {
    registerStructureType("minecraft:woodland_mansion", [](const json&, StructureEnv&) -> std::unique_ptr<Structure> {
        return std::make_unique<WoodlandMansionStructure>();
    });
}

}  // namespace wg::st
