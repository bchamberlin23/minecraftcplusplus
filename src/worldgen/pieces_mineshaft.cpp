// Mineshafts (normal and mesa): the game's MineshaftStructure and MineshaftPieces (rooms, corridors with supports,
// rails, cobwebs, chest minecarts and cave spider spawners, crossings, stairs).
#include "worldgen/structure_core.h"

#include <list>

namespace wg::st {

namespace {

struct ShaftType {
    bool mesa;
    St wood() const { return B(mesa ? "minecraft:dark_oak_log" : "minecraft:oak_log"); }
    St planks() const { return B(mesa ? "minecraft:dark_oak_planks" : "minecraft:oak_planks"); }
    St fence() const { return B(mesa ? "minecraft:dark_oak_fence" : "minecraft:oak_fence"); }
};

St caveAir() { return B("minecraft:cave_air"); }

bool fallingBlock(St s) {  // FallingBlock subclasses
    const std::string_view n = sb().blockName(s);
    return n == "minecraft:sand" || n == "minecraft:red_sand" || n == "minecraft:gravel" || n.ends_with("_concrete_powder") ||
           n == "minecraft:anvil" || n == "minecraft:chipped_anvil" || n == "minecraft:damaged_anvil" || n == "minecraft:dragon_egg";
}

class MineShaftPiece : public Piece {
public:
    MineShaftPiece(const char* id, int genDepth, ShaftType t, const BBox& b) : Piece(id, genDepth, b), type(t) {}
    ShaftType type;

protected:
    bool canBeReplaced(PlaceCtx& level, int x, int y, int z, const BBox& chunkBB) override {
        const St state = S(getBlock(level, x, y, z, chunkBB));
        return !state.is(sb().blockName(type.planks())) && !state.is(sb().blockName(type.wood())) &&
               !state.is(sb().blockName(type.fence())) && !state.is("minecraft:iron_chain");
    }

    bool isSupportingBox(PlaceCtx& level, const BBox& chunkBB, int x0, int x1, int y1, int z0) {
        for (int x = x0; x <= x1; ++x)
            if (S(getBlock(level, x, y1 + 1, z0, chunkBB)).isAir()) return false;
        return true;
    }

    bool isInInvalidLocation(PlaceCtx& level, const BBox& chunkBB) {
        const int x0 = std::max(box.minX - 1, chunkBB.minX), y0 = std::max(box.minY - 1, chunkBB.minY),
                  z0 = std::max(box.minZ - 1, chunkBB.minZ);
        const int x1 = std::min(box.maxX + 1, chunkBB.maxX), y1 = std::min(box.maxY + 1, chunkBB.maxY),
                  z1 = std::min(box.maxZ + 1, chunkBB.maxZ);
        const BlockPos centre{(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2};
        if (level.env.biomeInTag(level.level.biome(centre), "minecraft:mineshaft_blocking")) return true;
        const SB& b = level.b;
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z) {
                if (b.liquid(level.get(x, y0, z))) return true;
                if (b.liquid(level.get(x, y1, z))) return true;
            }
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y) {
                if (b.liquid(level.get(x, y, z0))) return true;
                if (b.liquid(level.get(x, y, z1))) return true;
            }
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y) {
                if (b.liquid(level.get(x0, y, z))) return true;
                if (b.liquid(level.get(x1, y, z))) return true;
            }
        return false;
    }

    void setPlanksBlock(PlaceCtx& level, const BBox& chunkBB, St planks, int x, int y, int z) {
        if (!isInterior(level, x, y, z, chunkBB)) return;
        const BlockPos pos = getWorldPos(x, y, z);
        if (!level.b.f.sturdy(level.get(pos), Up)) level.set(pos, planks);
    }
};

MineShaftPiece* generateAndAddPiece(Piece& startPiece, PiecesBuilder& builder, RandomSource& random, int footX, int footY, int footZ,
                                    Direction direction, int depth);

// ---- corridor ------------------------------------------------------------------------------------------

class MineShaftCorridor final : public MineShaftPiece {
public:
    bool hasRails, spiderCorridor, hasPlacedSpider = false;
    int numSections;

    MineShaftCorridor(int genDepth, RandomSource& random, const BBox& b, Direction direction, ShaftType t)
        : MineShaftPiece("minecraft:mscorridor", genDepth, t, b) {
        setOrientation(direction);
        hasRails = random.nextInt(3) == 0;
        spiderCorridor = !hasRails && random.nextInt(23) == 0;
        numSections = axisZ(direction) ? b.zSpan() / 5 : b.xSpan() / 5;
    }

    static std::optional<BBox> findCorridorSize(PiecesBuilder& builder, RandomSource& random, int footX, int footY, int footZ,
                                                Direction direction) {
        for (int length = random.nextInt(3) + 2; length > 0; --length) {
            const int l = length * 5;
            BBox b;
            switch (direction) {
            case South: b = BBox(0, 0, 0, 2, 2, l - 1); break;
            case West: b = BBox(-(l - 1), 0, 0, 0, 2, 2); break;
            case East: b = BBox(0, 0, 0, l - 1, 2, 2); break;
            default: b = BBox(0, 0, -(l - 1), 2, 2, 0); break;
            }
            b.move(footX, footY, footZ);
            if (!builder.findCollisionPiece(b)) return b;
        }
        return std::nullopt;
    }

    void addChildren(Piece& startPiece, PiecesBuilder& builder, RandomSource& random) override {
        const int depth = genDepth;
        const int endSelection = random.nextInt(4);
        const std::optional<Direction> o = orientation();
        if (o) {
            switch (*o) {
            case South:
                if (endSelection <= 1)
                    generateAndAddPiece(startPiece, builder, random, box.minX, box.minY - 1 + random.nextInt(3), box.maxZ + 1, *o, depth);
                else if (endSelection == 2)
                    generateAndAddPiece(startPiece, builder, random, box.minX - 1, box.minY - 1 + random.nextInt(3), box.maxZ - 3, West, depth);
                else
                    generateAndAddPiece(startPiece, builder, random, box.maxX + 1, box.minY - 1 + random.nextInt(3), box.maxZ - 3, East, depth);
                break;
            case West:
                if (endSelection <= 1)
                    generateAndAddPiece(startPiece, builder, random, box.minX - 1, box.minY - 1 + random.nextInt(3), box.minZ, *o, depth);
                else if (endSelection == 2)
                    generateAndAddPiece(startPiece, builder, random, box.minX, box.minY - 1 + random.nextInt(3), box.minZ - 1, North, depth);
                else
                    generateAndAddPiece(startPiece, builder, random, box.minX, box.minY - 1 + random.nextInt(3), box.maxZ + 1, South, depth);
                break;
            case East:
                if (endSelection <= 1)
                    generateAndAddPiece(startPiece, builder, random, box.maxX + 1, box.minY - 1 + random.nextInt(3), box.minZ, *o, depth);
                else if (endSelection == 2)
                    generateAndAddPiece(startPiece, builder, random, box.maxX - 3, box.minY - 1 + random.nextInt(3), box.minZ - 1, North, depth);
                else
                    generateAndAddPiece(startPiece, builder, random, box.maxX - 3, box.minY - 1 + random.nextInt(3), box.maxZ + 1, South, depth);
                break;
            default:  // North
                if (endSelection <= 1)
                    generateAndAddPiece(startPiece, builder, random, box.minX, box.minY - 1 + random.nextInt(3), box.minZ - 1, *o, depth);
                else if (endSelection == 2)
                    generateAndAddPiece(startPiece, builder, random, box.minX - 1, box.minY - 1 + random.nextInt(3), box.minZ, West, depth);
                else
                    generateAndAddPiece(startPiece, builder, random, box.maxX + 1, box.minY - 1 + random.nextInt(3), box.minZ, East, depth);
                break;
            }
        }
        if (depth < 8) {
            if (o != North && o != South) {
                for (int x = box.minX + 3; x + 3 <= box.maxX; x += 5) {
                    const int selection = random.nextInt(5);
                    if (selection == 0)
                        generateAndAddPiece(startPiece, builder, random, x, box.minY, box.minZ - 1, North, depth + 1);
                    else if (selection == 1)
                        generateAndAddPiece(startPiece, builder, random, x, box.minY, box.maxZ + 1, South, depth + 1);
                }
            } else {
                for (int z = box.minZ + 3; z + 3 <= box.maxZ; z += 5) {
                    const int selection = random.nextInt(5);
                    if (selection == 0)
                        generateAndAddPiece(startPiece, builder, random, box.minX - 1, box.minY, z, West, depth + 1);
                    else if (selection == 1)
                        generateAndAddPiece(startPiece, builder, random, box.maxX + 1, box.minY, z, East, depth + 1);
                }
            }
        }
    }

    // A chest minecart on a rail.
    bool createMinecartChest(PlaceCtx& level, const BBox& chunkBB, RandomSource& random, int x, int y, int z) {
        const BlockPos pos = getWorldPos(x, y, z);
        if (chunkBB.isInside(pos) && level.get(pos).isAir() && !level.get(pos.offset(0, -1, 0)).isAir()) {
            const St state = B("minecraft:rail").with("shape", random.nextBoolean() ? "north_south" : "east_west");
            placeBlock(level, state, x, y, z, chunkBB);
            // MinecartChest at the block's centre with its loot table.
            NbtValue cart = NbtValue::make(TagCompound);
            NbtValue id = NbtValue::make(TagString);
            id.s = "minecraft:chest_minecart";
            cart.set("id", std::move(id));
            NbtValue at = NbtValue::make(TagList);
            at.listType = TagDouble;
            for (int v : {pos.x, pos.y, pos.z}) {
                NbtValue d = NbtValue::make(TagDouble);
                d.d = v + 0.5;
                at.items.push_back(d);
            }
            cart.set("Pos", std::move(at));
            NbtValue table = NbtValue::make(TagString);
            table.s = "minecraft:chests/abandoned_mineshaft";
            cart.set("LootTable", std::move(table));
            NbtValue seed = NbtValue::make(TagLong);
            seed.i = random.nextLong();
            cart.set("LootTableSeed", std::move(seed));
            level.addEntity(std::move(cart));
            return true;
        }
        return false;
    }

    void postProcess(PlaceCtx& level, RandomSource& random, const BBox& chunkBB, const BlockPos&) override {
        if (isInInvalidLocation(level, chunkBB)) return;
        const int length = numSections * 5 - 1;
        const St planks = type.planks();
        const St CAVE_AIR = caveAir();
        generateBox(level, chunkBB, 0, 0, 0, 2, 1, length, CAVE_AIR, CAVE_AIR, false);
        generateMaybeBox(level, chunkBB, random, 0.8f, 0, 2, 0, 2, 2, length, CAVE_AIR, CAVE_AIR, false, false);
        if (spiderCorridor)
            generateMaybeBox(level, chunkBB, random, 0.6f, 0, 0, 0, 2, 1, length, B("minecraft:cobweb"), CAVE_AIR, false, true);
        for (int section = 0; section < numSections; ++section) {
            const int z = 2 + section * 5;
            placeSupport(level, chunkBB, 0, 0, z, 2, 2, random);
            maybePlaceCobWeb(level, chunkBB, random, 0.1f, 0, 2, z - 1);
            maybePlaceCobWeb(level, chunkBB, random, 0.1f, 2, 2, z - 1);
            maybePlaceCobWeb(level, chunkBB, random, 0.1f, 0, 2, z + 1);
            maybePlaceCobWeb(level, chunkBB, random, 0.1f, 2, 2, z + 1);
            maybePlaceCobWeb(level, chunkBB, random, 0.05f, 0, 2, z - 2);
            maybePlaceCobWeb(level, chunkBB, random, 0.05f, 2, 2, z - 2);
            maybePlaceCobWeb(level, chunkBB, random, 0.05f, 0, 2, z + 2);
            maybePlaceCobWeb(level, chunkBB, random, 0.05f, 2, 2, z + 2);
            if (random.nextInt(100) == 0) createMinecartChest(level, chunkBB, random, 2, 0, z - 1);
            if (random.nextInt(100) == 0) createMinecartChest(level, chunkBB, random, 0, 0, z + 1);
            if (spiderCorridor && !hasPlacedSpider) {
                const int newZ = z - 1 + random.nextInt(3);
                const BlockPos pos = getWorldPos(1, 0, newZ);
                if (chunkBB.isInside(pos) && isInterior(level, 1, 0, newZ, chunkBB)) {
                    hasPlacedSpider = true;
                    level.set(pos, B("minecraft:spawner"));
                    if (level.get(pos).is("minecraft:spawner")) level.setSpawner(pos, "minecraft:cave_spider");
                }
            }
        }
        for (int x = 0; x <= 2; ++x)
            for (int zx = 0; zx <= length; ++zx) setPlanksBlock(level, chunkBB, planks, x, -1, zx);
        placeDoubleLowerOrUpperSupport(level, chunkBB, 0, -1, 2);
        if (numSections > 1) placeDoubleLowerOrUpperSupport(level, chunkBB, 0, -1, length - 2);
        if (hasRails) {
            const St state = B("minecraft:rail").with("shape", "north_south");
            for (int zx = 0; zx <= length; ++zx) {
                const St floor = S(getBlock(level, 1, -1, zx, chunkBB));
                if (!floor.isAir() && level.b.solidRender(floor)) {
                    const float probability = isInterior(level, 1, 0, zx, chunkBB) ? 0.7f : 0.9f;
                    maybeGenerateBlock(level, chunkBB, random, probability, 1, 0, zx, state);
                }
            }
        }
    }

private:
    void placeDoubleLowerOrUpperSupport(PlaceCtx& level, const BBox& chunkBB, int x, int y, int z) {
        const St wood = type.wood();
        const std::string_view plank = sb().blockName(type.planks());
        if (S(getBlock(level, x, y, z, chunkBB)).is(plank)) fillPillarDownOrChainUp(level, wood, x, y, z, chunkBB);
        if (S(getBlock(level, x + 2, y, z, chunkBB)).is(plank)) fillPillarDownOrChainUp(level, wood, x + 2, y, z, chunkBB);
    }

    bool canPlaceColumnOnTopOf(PlaceCtx& level, St below) { return level.b.f.sturdy(below, Up); }
    bool canHangChainBelow(PlaceCtx& level, St above) { return level.b.f.has(above, fact::CenterDown) && !fallingBlock(above); }

    void fillPillarDownOrChainUp(PlaceCtx& level, St pillar, int x, int y, int z, const BBox& chunkBB) {
        BlockPos pos = getWorldPos(x, y, z);
        if (!chunkBB.isInside(pos)) return;
        const int worldY = pos.y;
        int distance = 1;
        bool checkBelow = true;
        for (bool checkAbove = true; checkBelow || checkAbove; ++distance) {
            if (checkBelow) {
                pos.y = worldY - distance;
                const St below = level.get(pos);
                const bool emptyBelow = isReplaceableByStructures(level, below) && !below.is("minecraft:lava");
                if (!emptyBelow && canPlaceColumnOnTopOf(level, below)) {
                    for (int py = worldY - distance + 1; py < worldY; ++py) level.set({pos.x, py, pos.z}, pillar);
                    return;
                }
                checkBelow = distance <= 20 && emptyBelow && pos.y > level.minY() + 1;
            }
            if (checkAbove) {
                pos.y = worldY + distance;
                const St above = level.get(pos);
                const bool emptyAbove = isReplaceableByStructures(level, above);
                if (!emptyAbove && canHangChainBelow(level, above)) {
                    level.set({pos.x, worldY + 1, pos.z}, type.fence());
                    for (int py = worldY + 2; py < worldY + distance; ++py) level.set({pos.x, py, pos.z}, B("minecraft:iron_chain"));
                    return;
                }
                checkAbove = distance <= 50 && emptyAbove && pos.y < level.maxY() - 1;
            }
        }
    }

    void placeSupport(PlaceCtx& level, const BBox& chunkBB, int x0, int y0, int z, int y1, int x1, RandomSource& random) {
        if (!isSupportingBox(level, chunkBB, x0, x1, y1, z)) {
            return;
        }
        const St planks = type.planks(), fence = type.fence(), CAVE_AIR = caveAir();
        generateBox(level, chunkBB, x0, y0, z, x0, y1 - 1, z, fence.with("west", true), CAVE_AIR, false);
        generateBox(level, chunkBB, x1, y0, z, x1, y1 - 1, z, fence.with("east", true), CAVE_AIR, false);
        if (random.nextInt(4) == 0) {
            generateBox(level, chunkBB, x0, y1, z, x0, y1, z, planks, CAVE_AIR, false);
            generateBox(level, chunkBB, x1, y1, z, x1, y1, z, planks, CAVE_AIR, false);
        } else {
            generateBox(level, chunkBB, x0, y1, z, x1, y1, z, planks, CAVE_AIR, false);
            maybeGenerateBlock(level, chunkBB, random, 0.05f, x0 + 1, y1, z - 1, B("minecraft:wall_torch").with("facing", South));
            maybeGenerateBlock(level, chunkBB, random, 0.05f, x0 + 1, y1, z + 1, B("minecraft:wall_torch").with("facing", North));
        }
    }

    void maybePlaceCobWeb(PlaceCtx& level, const BBox& chunkBB, RandomSource& random, float probability, int x, int y, int z) {
        if (isInterior(level, x, y, z, chunkBB) && random.nextFloat() < probability && hasSturdyNeighbours(level, chunkBB, x, y, z, 2))
            placeBlock(level, B("minecraft:cobweb"), x, y, z, chunkBB);
    }

    bool hasSturdyNeighbours(PlaceCtx& level, const BBox& chunkBB, int x, int y, int z, int count) {
        const BlockPos p = getWorldPos(x, y, z);
        int sturdy = 0;
        for (int d = 0; d < 6; ++d) {
            const BlockPos n = relative(p, Direction(d));
            if (chunkBB.isInside(n) && level.b.f.sturdy(level.get(n), opposite(Direction(d))))
                if (++sturdy >= count) return true;
        }
        return false;
    }

    // MineShaftCorridor.fillColumnDown (supports stand on sturdy ground only).
    void fillColumnDownShaft(PlaceCtx& level, St s, int x, int startY, int z, const BBox& chunkBB) {
        BlockPos pos = getWorldPos(x, startY, z);
        if (!chunkBB.isInside(pos)) return;
        const int worldY = pos.y;
        while (isReplaceableByStructures(level, level.get(pos)) && pos.y > level.minY() + 1) --pos.y;
        if (canPlaceColumnOnTopOf(level, level.get(pos)))
            while (pos.y < worldY) {
                ++pos.y;
                level.set(pos, s);
            }
    }
};

// ---- crossing ------------------------------------------------------------------------------------------

class MineShaftCrossing final : public MineShaftPiece {
public:
    std::optional<Direction> direction;
    bool isTwoFloored;

    MineShaftCrossing(int genDepth, const BBox& b, std::optional<Direction> d, ShaftType t)
        : MineShaftPiece("minecraft:mscrossing", genDepth, t, b), direction(d), isTwoFloored(b.ySpan() > 3) {}

    static std::optional<BBox> findCrossing(PiecesBuilder& builder, RandomSource& random, int footX, int footY, int footZ,
                                            Direction direction) {
        const int y1 = random.nextInt(4) == 0 ? 6 : 2;
        BBox b;
        switch (direction) {
        case South: b = BBox(-1, 0, 0, 3, y1, 4); break;
        case West: b = BBox(-4, 0, -1, 0, y1, 3); break;
        case East: b = BBox(0, 0, -1, 4, y1, 3); break;
        default: b = BBox(-1, 0, -4, 3, y1, 0); break;
        }
        b.move(footX, footY, footZ);
        if (builder.findCollisionPiece(b)) return std::nullopt;
        return b;
    }

    void addChildren(Piece& startPiece, PiecesBuilder& builder, RandomSource& random) override {
        const int depth = genDepth;
        switch (direction.value_or(North)) {
        case South:
            generateAndAddPiece(startPiece, builder, random, box.minX + 1, box.minY, box.maxZ + 1, South, depth);
            generateAndAddPiece(startPiece, builder, random, box.minX - 1, box.minY, box.minZ + 1, West, depth);
            generateAndAddPiece(startPiece, builder, random, box.maxX + 1, box.minY, box.minZ + 1, East, depth);
            break;
        case West:
            generateAndAddPiece(startPiece, builder, random, box.minX + 1, box.minY, box.minZ - 1, North, depth);
            generateAndAddPiece(startPiece, builder, random, box.minX + 1, box.minY, box.maxZ + 1, South, depth);
            generateAndAddPiece(startPiece, builder, random, box.minX - 1, box.minY, box.minZ + 1, West, depth);
            break;
        case East:
            generateAndAddPiece(startPiece, builder, random, box.minX + 1, box.minY, box.minZ - 1, North, depth);
            generateAndAddPiece(startPiece, builder, random, box.minX + 1, box.minY, box.maxZ + 1, South, depth);
            generateAndAddPiece(startPiece, builder, random, box.maxX + 1, box.minY, box.minZ + 1, East, depth);
            break;
        default:
            generateAndAddPiece(startPiece, builder, random, box.minX + 1, box.minY, box.minZ - 1, North, depth);
            generateAndAddPiece(startPiece, builder, random, box.minX - 1, box.minY, box.minZ + 1, West, depth);
            generateAndAddPiece(startPiece, builder, random, box.maxX + 1, box.minY, box.minZ + 1, East, depth);
            break;
        }
        if (isTwoFloored) {
            if (random.nextBoolean())
                generateAndAddPiece(startPiece, builder, random, box.minX + 1, box.minY + 3 + 1, box.minZ - 1, North, depth);
            if (random.nextBoolean())
                generateAndAddPiece(startPiece, builder, random, box.minX - 1, box.minY + 3 + 1, box.minZ + 1, West, depth);
            if (random.nextBoolean())
                generateAndAddPiece(startPiece, builder, random, box.maxX + 1, box.minY + 3 + 1, box.minZ + 1, East, depth);
            if (random.nextBoolean())
                generateAndAddPiece(startPiece, builder, random, box.minX + 1, box.minY + 3 + 1, box.maxZ + 1, South, depth);
        }
    }

    void postProcess(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const BlockPos&) override {
        if (isInInvalidLocation(level, chunkBB)) return;
        const St planks = type.planks(), CAVE_AIR = caveAir();
        if (isTwoFloored) {
            generateBox(level, chunkBB, box.minX + 1, box.minY, box.minZ, box.maxX - 1, box.minY + 3 - 1, box.maxZ, CAVE_AIR, CAVE_AIR, false);
            generateBox(level, chunkBB, box.minX, box.minY, box.minZ + 1, box.maxX, box.minY + 3 - 1, box.maxZ - 1, CAVE_AIR, CAVE_AIR, false);
            generateBox(level, chunkBB, box.minX + 1, box.maxY - 2, box.minZ, box.maxX - 1, box.maxY, box.maxZ, CAVE_AIR, CAVE_AIR, false);
            generateBox(level, chunkBB, box.minX, box.maxY - 2, box.minZ + 1, box.maxX, box.maxY, box.maxZ - 1, CAVE_AIR, CAVE_AIR, false);
            generateBox(level, chunkBB, box.minX + 1, box.minY + 3, box.minZ + 1, box.maxX - 1, box.minY + 3, box.maxZ - 1, CAVE_AIR, CAVE_AIR, false);
        } else {
            generateBox(level, chunkBB, box.minX + 1, box.minY, box.minZ, box.maxX - 1, box.maxY, box.maxZ, CAVE_AIR, CAVE_AIR, false);
            generateBox(level, chunkBB, box.minX, box.minY, box.minZ + 1, box.maxX, box.maxY, box.maxZ - 1, CAVE_AIR, CAVE_AIR, false);
        }
        placeSupportPillar(level, chunkBB, box.minX + 1, box.minY, box.minZ + 1, box.maxY);
        placeSupportPillar(level, chunkBB, box.minX + 1, box.minY, box.maxZ - 1, box.maxY);
        placeSupportPillar(level, chunkBB, box.maxX - 1, box.minY, box.minZ + 1, box.maxY);
        placeSupportPillar(level, chunkBB, box.maxX - 1, box.minY, box.maxZ - 1, box.maxY);
        const int y = box.minY - 1;
        for (int x = box.minX; x <= box.maxX; ++x)
            for (int z = box.minZ; z <= box.maxZ; ++z) setPlanksBlock(level, chunkBB, planks, x, y, z);
    }

private:
    void placeSupportPillar(PlaceCtx& level, const BBox& chunkBB, int x, int y0, int z, int y1) {
        if (!S(getBlock(level, x, y1 + 1, z, chunkBB)).isAir())
            generateBox(level, chunkBB, x, y0, z, x, y1, z, type.planks(), caveAir(), false);
    }
};

// ---- room ------------------------------------------------------------------------------------------------

class MineShaftRoom final : public MineShaftPiece {
public:
    std::list<BBox> childEntranceBoxes;

    MineShaftRoom(int genDepth, RandomSource& random, int west, int north, ShaftType t)
        : MineShaftPiece("minecraft:msroom", genDepth, t, makeBox(random, west, north)) {}

    static BBox makeBox(RandomSource& random, int west, int north) {
        const int x1 = west + 7 + random.nextInt(6);
        const int y1 = 54 + random.nextInt(6);
        const int z1 = north + 7 + random.nextInt(6);
        return BBox(west, 50, north, x1, y1, z1);
    }

    void addChildren(Piece& startPiece, PiecesBuilder& builder, RandomSource& random) override {
        const int depth = genDepth;
        int heightSpace = box.ySpan() - 3 - 1;
        if (heightSpace <= 0) heightSpace = 1;
        int pos = 0;
        while (pos < box.xSpan()) {
            pos += random.nextInt(box.xSpan());
            if (pos + 3 > box.xSpan()) break;
            if (Piece* child = generateAndAddPiece(startPiece, builder, random, box.minX + pos, box.minY + random.nextInt(heightSpace) + 1,
                                                   box.minZ - 1, North, depth)) {
                const BBox& cb = child->box;
                childEntranceBoxes.push_back(BBox(cb.minX, cb.minY, box.minZ, cb.maxX, cb.maxY, box.minZ + 1));
            }
            pos += 4;
        }
        pos = 0;
        while (pos < box.xSpan()) {
            pos += random.nextInt(box.xSpan());
            if (pos + 3 > box.xSpan()) break;
            if (Piece* child = generateAndAddPiece(startPiece, builder, random, box.minX + pos, box.minY + random.nextInt(heightSpace) + 1,
                                                   box.maxZ + 1, South, depth)) {
                const BBox& cb = child->box;
                childEntranceBoxes.push_back(BBox(cb.minX, cb.minY, box.maxZ - 1, cb.maxX, cb.maxY, box.maxZ));
            }
            pos += 4;
        }
        pos = 0;
        while (pos < box.zSpan()) {
            pos += random.nextInt(box.zSpan());
            if (pos + 3 > box.zSpan()) break;
            if (Piece* child = generateAndAddPiece(startPiece, builder, random, box.minX - 1, box.minY + random.nextInt(heightSpace) + 1,
                                                   box.minZ + pos, West, depth)) {
                const BBox& cb = child->box;
                childEntranceBoxes.push_back(BBox(box.minX, cb.minY, cb.minZ, box.minX + 1, cb.maxY, cb.maxZ));
            }
            pos += 4;
        }
        pos = 0;
        while (pos < box.zSpan()) {
            pos += random.nextInt(box.zSpan());
            if (pos + 3 > box.zSpan()) break;
            if (Piece* child = generateAndAddPiece(startPiece, builder, random, box.maxX + 1, box.minY + random.nextInt(heightSpace) + 1,
                                                   box.minZ + pos, East, depth)) {
                const BBox& cb = child->box;
                childEntranceBoxes.push_back(BBox(box.maxX - 1, cb.minY, cb.minZ, box.maxX, cb.maxY, cb.maxZ));
            }
            pos += 4;
        }
    }

    void postProcess(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const BlockPos&) override {
        if (isInInvalidLocation(level, chunkBB)) return;
        const St CAVE_AIR = caveAir();
        generateBox(level, chunkBB, box.minX, box.minY + 1, box.minZ, box.maxX, std::min(box.minY + 3, box.maxY), box.maxZ, CAVE_AIR,
                    CAVE_AIR, false);
        for (const BBox& e : childEntranceBoxes)
            generateBox(level, chunkBB, e.minX, e.maxY - 2, e.minZ, e.maxX, e.maxY, e.maxZ, CAVE_AIR, CAVE_AIR, false);
        generateUpperHalfSphere(level, chunkBB, box.minX, box.minY + 4, box.minZ, box.maxX, box.maxY, box.maxZ, CAVE_AIR, false);
    }

    void move(int dx, int dy, int dz) override {
        MineShaftPiece::move(dx, dy, dz);
        for (BBox& b : childEntranceBoxes) b.move(dx, dy, dz);
    }
};

// ---- stairs ----------------------------------------------------------------------------------------------

class MineShaftStairs final : public MineShaftPiece {
public:
    MineShaftStairs(int genDepth, const BBox& b, Direction direction, ShaftType t) : MineShaftPiece("minecraft:msstairs", genDepth, t, b) {
        setOrientation(direction);
    }

    static std::optional<BBox> findStairs(PiecesBuilder& builder, RandomSource&, int footX, int footY, int footZ, Direction direction) {
        BBox b;
        switch (direction) {
        case South: b = BBox(0, -5, 0, 2, 2, 8); break;
        case West: b = BBox(-8, -5, 0, 0, 2, 2); break;
        case East: b = BBox(0, -5, 0, 8, 2, 2); break;
        default: b = BBox(0, -5, -8, 2, 2, 0); break;
        }
        b.move(footX, footY, footZ);
        if (builder.findCollisionPiece(b)) return std::nullopt;
        return b;
    }

    void addChildren(Piece& startPiece, PiecesBuilder& builder, RandomSource& random) override {
        const int depth = genDepth;
        const std::optional<Direction> o = orientation();
        if (!o) return;
        switch (*o) {
        case South: generateAndAddPiece(startPiece, builder, random, box.minX, box.minY, box.maxZ + 1, South, depth); break;
        case West: generateAndAddPiece(startPiece, builder, random, box.minX - 1, box.minY, box.minZ, West, depth); break;
        case East: generateAndAddPiece(startPiece, builder, random, box.maxX + 1, box.minY, box.minZ, East, depth); break;
        default: generateAndAddPiece(startPiece, builder, random, box.minX, box.minY, box.minZ - 1, North, depth); break;
        }
    }

    void postProcess(PlaceCtx& level, RandomSource&, const BBox& chunkBB, const BlockPos&) override {
        if (isInInvalidLocation(level, chunkBB)) return;
        const St CAVE_AIR = caveAir();
        generateBox(level, chunkBB, 0, 5, 0, 2, 7, 1, CAVE_AIR, CAVE_AIR, false);
        generateBox(level, chunkBB, 0, 0, 7, 2, 2, 8, CAVE_AIR, CAVE_AIR, false);
        for (int i = 0; i < 5; ++i)
            generateBox(level, chunkBB, 0, 5 - i - (i < 4 ? 1 : 0), 2 + i, 2, 7 - i, 2 + i, CAVE_AIR, CAVE_AIR, false);
    }
};

MineShaftPiece* createRandomShaftPiece(PiecesBuilder& builder, RandomSource& random, int footX, int footY, int footZ, Direction direction,
                                       int genDepth, ShaftType type) {
    const int selection = random.nextInt(100);
    if (selection >= 80) {
        if (auto b = MineShaftCrossing::findCrossing(builder, random, footX, footY, footZ, direction))
            return new MineShaftCrossing(genDepth, *b, direction, type);
    } else if (selection >= 70) {
        if (auto b = MineShaftStairs::findStairs(builder, random, footX, footY, footZ, direction))
            return new MineShaftStairs(genDepth, *b, direction, type);
    } else {
        if (auto b = MineShaftCorridor::findCorridorSize(builder, random, footX, footY, footZ, direction))
            return new MineShaftCorridor(genDepth, random, *b, direction, type);
    }
    return nullptr;
}

MineShaftPiece* generateAndAddPiece(Piece& startPiece, PiecesBuilder& builder, RandomSource& random, int footX, int footY, int footZ,
                                    Direction direction, int depth) {
    if (depth > 8) return nullptr;
    if (std::abs(footX - startPiece.box.minX) > 80 || std::abs(footZ - startPiece.box.minZ) > 80) return nullptr;
    const ShaftType type = static_cast<MineShaftPiece&>(startPiece).type;
    MineShaftPiece* piece = createRandomShaftPiece(builder, random, footX, footY, footZ, direction, depth + 1, type);
    if (piece) {
        builder.add(std::unique_ptr<Piece>(piece));
        piece->addChildren(startPiece, builder, random);
    }
    return piece;
}

class MineshaftStructure final : public Structure {
public:
    explicit MineshaftStructure(bool mesa) : mesa_(mesa) {}

    std::optional<Stub> findGenerationPoint(GenCtx& c) const override {
        c.random.nextDouble();
        auto builder = std::make_shared<PiecesBuilder>();
        RandomSource& random = c.random;
        auto room = std::make_unique<MineShaftRoom>(0, random, c.minBlockX() + 2, c.minBlockZ() + 2, ShaftType{mesa_});
        MineShaftRoom* r = room.get();
        builder->add(std::move(room));
        r->addChildren(*r, *builder, random);
        const int seaLevel = c.seaLevel();
        int dy;
        if (mesa_) {
            const BlockPos centre = builder->boundingBox().center();
            const int surface = c.baseHeight(centre.x, centre.z, HType::WorldSurfaceWG);
            const int target = surface <= seaLevel ? seaLevel : random.nextInt(surface - seaLevel + 1) + seaLevel;
            dy = target - centre.y;
            builder->offsetPiecesVertically(dy);
        } else {
            dy = builder->moveBelowSeaLevel(seaLevel, c.minY(), random, 10);
        }
        return Stub{{c.middleBlockX(), 50 + dy, c.minBlockZ()}, [builder](PiecesBuilder& out) {
                        for (auto& p : builder->pieces) out.add(std::move(p));
                    }};
    }

private:
    bool mesa_;
};

}  // namespace

void registerMineshaft() {
    registerStructureType("minecraft:mineshaft", [](const json& def, StructureEnv&) -> std::unique_ptr<Structure> {
        return std::make_unique<MineshaftStructure>(def.value("mineshaft_type", std::string("normal")) == "mesa");
    });
}

}  // namespace wg::st
