// End cities: the game's EndCityStructure and EndCityPieces (the recursive section generators - house towers, towers,
// bridges with the end ship, fat towers - with their collision checks, and the template pieces with their chests,
// sentry shulkers and the ship's elytra item frame).
#include "worldgen/structure_template.h"

namespace wg::st {

namespace {

// Level.isInSpawnableBounds
bool isInSpawnableBounds(const BlockPos& p) {
    return p.y >= -20000000 && p.y < 20000000 && p.x >= -30000000 && p.z >= -30000000 && p.x < 30000000 && p.z < 30000000;
}

NbtValue nbtDouble(double v) {
    NbtValue n = NbtValue::make(TagDouble);
    n.d = v;
    return n;
}
NbtValue nbtFloat(float v) {
    NbtValue n = NbtValue::make(TagFloat);
    n.d = v;
    return n;
}
NbtValue nbtByte(int v) {
    NbtValue n = NbtValue::make(TagByte);
    n.i = v;
    return n;
}
NbtValue nbtInt(int v) {
    NbtValue n = NbtValue::make(TagInt);
    n.i = v;
    return n;
}
NbtValue nbtString(std::string_view v) {
    NbtValue n = NbtValue::make(TagString);
    n.s = v;
    return n;
}
NbtValue nbtList(u8 type, std::vector<NbtValue> items) {
    NbtValue n = NbtValue::make(TagList);
    n.listType = type;
    n.items = std::move(items);
    return n;
}

class EndCityPiece final : public TemplatePiece {
public:
    EndCityPiece(const StructureEnv& env, const char* name, const BlockPos& pos, Rot r, bool overwrite)
        : TemplatePiece("minecraft:ecp", 0, env.templates->get(std::string("minecraft:end_city/") + name), makeSettings(env, overwrite, r),
                        pos) {}

    static PlaceSettings makeSettings(const StructureEnv& env, bool overwrite, Rot r) {
        PlaceSettings s;
        s.ignoreEntities = true;
        // BlockIgnoreProcessor.STRUCTURE_BLOCK when overwriting, else STRUCTURE_AND_AIR
        s.processors.push_back(overwrite ? blockIgnore(env, {"minecraft:structure_block"})
                                         : blockIgnore(env, {"minecraft:air", "minecraft:structure_block"}));
        s.rotation = r;
        return s;
    }

protected:
    void handleDataMarker(PlaceCtx& level, const std::string& marker, const BlockPos& pos, RandomSource& random, const BBox& chunkBB) override {
        if (marker.starts_with("Chest")) {
            const BlockPos chest = pos.offset(0, -1, 0);
            // RandomizableContainer.setBlockEntityLootTable: a seed only when a container is there
            if (chunkBB.isInside(chest) && isRandomizableContainer(level.b.blockName(level.get(chest))))
                level.setLootTable(chest, "minecraft:chests/end_city_treasure", random.nextLong());
        } else if (chunkBB.isInside(pos) && isInSpawnableBounds(pos)) {
            if (marker.starts_with("Sentry")) {
                // Shulker.setPos(x + 0.5, y, z + 0.5) snaps to (floor(x) + 0.5, floor(y + 0.5), floor(z) + 0.5): unchanged
                NbtValue e = NbtValue::make(TagCompound);
                e.set("id", nbtString("minecraft:shulker"));
                e.set("Pos", nbtList(TagDouble, {nbtDouble(pos.x + 0.5), nbtDouble(pos.y), nbtDouble(pos.z + 0.5)}));
                level.addEntity(std::move(e));
            } else if (marker.starts_with("Elytra")) {
                addElytraFrame(level, pos, rotate(settings.rotation, South));
            }
        }
    }

private:
    // new ItemFrame(level, pos, facing) with an elytra, as the entity saves itself (HangingEntity.recalculateBoundingBox
    // puts it at the center of ItemFrame.createBoundingBox).
    static void addElytraFrame(PlaceCtx& level, const BlockPos& pos, Direction facing) {
        const double px = pos.x + 0.5 - 0.46875 * stepX(facing);
        const double py = pos.y + 0.5 - 0.46875 * stepY(facing);
        const double pz = pos.z + 0.5 - 0.46875 * stepZ(facing);
        const double sx = axisX(facing) ? 0.0625 : 0.75, sy = 0.75, sz = axisZ(facing) ? 0.0625 : 0.75;  // horizontal facing
        auto center = [](double c, double size) {  // AABB.ofSize then getCenter (Mth.lerp(0.5, min, max))
            const double lo = c - size / 2.0, hi = c + size / 2.0;
            return lo + 0.5 * (hi - lo);
        };
        NbtValue e = NbtValue::make(TagCompound);
        e.set("id", nbtString("minecraft:item_frame"));
        e.set("Pos", nbtList(TagDouble, {nbtDouble(center(px, sx)), nbtDouble(center(py, sy)), nbtDouble(center(pz, sz))}));
        e.set("Rotation", nbtList(TagFloat, {nbtFloat(float(to2D(facing) * 90)), nbtFloat(0.0f)}));
        NbtValue blockPos = NbtValue::make(TagIntArray);
        blockPos.ints = {pos.x, pos.y, pos.z};
        e.set("block_pos", std::move(blockPos));
        NbtValue item = NbtValue::make(TagCompound);
        item.set("id", nbtString("minecraft:elytra"));
        item.set("count", nbtInt(1));
        e.set("Item", std::move(item));
        e.set("ItemRotation", nbtByte(0));
        e.set("ItemDropChance", nbtFloat(1.0f));
        e.set("Facing", nbtByte(int(facing)));  // Direction.get3DDataValue (Direction is in the game's 3D order)
        e.set("Invisible", nbtByte(0));
        e.set("Fixed", nbtByte(0));
        level.addEntity(std::move(e));
    }
};

using PieceList = std::vector<std::unique_ptr<Piece>>;

// EndCityPieces' section generators and recursion. The game keeps them in static singletons reset by startHouseTower;
// the only state is TOWER_BRIDGE_GENERATOR.shipCreated.
class EndCityBuilder {
public:
    EndCityBuilder(const StructureEnv& env, RandomSource& r) : env_(env), random(r) {}

    void startHouseTower(const BlockPos& origin, Rot rotation, PieceList& pieces) {
        shipCreated = false;
        EndCityPiece* lastPiece = addHelper(pieces, std::make_unique<EndCityPiece>(env_, "base_floor", origin, rotation, true));
        lastPiece = addHelper(pieces, addPiece(lastPiece, {-1, 0, -1}, "second_floor_1", rotation, false));
        lastPiece = addHelper(pieces, addPiece(lastPiece, {-1, 4, -1}, "third_floor_1", rotation, false));
        lastPiece = addHelper(pieces, addPiece(lastPiece, {-1, 8, -1}, "third_roof", rotation, true));
        recursiveChildren(&EndCityBuilder::tower, 1, lastPiece, {0, 0, 0}, pieces);
    }

private:
    using Generator = bool (EndCityBuilder::*)(int genDepth, EndCityPiece* parent, const BlockPos& offset, PieceList& pieces);

    const StructureEnv& env_;
    RandomSource& random;
    bool shipCreated = false;

    static EndCityPiece* addHelper(PieceList& pieces, std::unique_ptr<EndCityPiece> piece) {
        EndCityPiece* p = piece.get();
        pieces.push_back(std::move(piece));
        return p;
    }

    // The child placed at the parent's template position, moved by parent.template().calculateConnectedPosition(
    // parent.placeSettings(), offset, child.placeSettings(), BlockPos.ZERO).
    std::unique_ptr<EndCityPiece> addPiece(EndCityPiece* parent, const BlockPos& offset, const char* name, Rot rotation, bool overwrite) {
        auto child = std::make_unique<EndCityPiece>(env_, name, parent->templatePosition, rotation, overwrite);
        const PlaceSettings& ps = parent->settings;
        const PlaceSettings& cs = child->settings;
        const BlockPos a = transformPos(offset, ps.mirror, ps.rotation, ps.pivot);
        const BlockPos b = transformPos({0, 0, 0}, cs.mirror, cs.rotation, cs.pivot);
        child->move(a.x - b.x, a.y - b.y, a.z - b.z);
        return child;
    }

    static Piece* findCollisionPiece(const PieceList& pieces, const BBox& box) {  // StructurePiece.findCollisionPiece
        for (const auto& p : pieces)
            if (p->box.intersects(box)) return p.get();
        return nullptr;
    }

    bool recursiveChildren(Generator generator, int genDepth, EndCityPiece* parent, const BlockPos& offset, PieceList& pieces) {
        if (genDepth > 8) return false;
        PieceList childPieces;
        if ((this->*generator)(genDepth, parent, offset, childPieces)) {
            bool collision = false;
            const int childTag = random.nextInt();
            for (const auto& child : childPieces) {
                child->genDepth = childTag;
                const Piece* collisionPiece = findCollisionPiece(pieces, child->box);
                if (collisionPiece && collisionPiece->genDepth != parent->genDepth) {
                    collision = true;
                    break;
                }
            }
            if (!collision) {
                for (auto& child : childPieces) pieces.push_back(std::move(child));
                return true;
            }
        }
        return false;
    }

    // HOUSE_TOWER_GENERATOR
    bool houseTower(int genDepth, EndCityPiece* parent, const BlockPos& offset, PieceList& pieces) {
        if (genDepth > 8) return false;
        const Rot rotation = parent->settings.rotation;
        EndCityPiece* lastPiece = addHelper(pieces, addPiece(parent, offset, "base_floor", rotation, true));
        const int numFloors = random.nextInt(3);
        if (numFloors == 0) {
            addHelper(pieces, addPiece(lastPiece, {-1, 4, -1}, "base_roof", rotation, true));
        } else if (numFloors == 1) {
            lastPiece = addHelper(pieces, addPiece(lastPiece, {-1, 0, -1}, "second_floor_2", rotation, false));
            lastPiece = addHelper(pieces, addPiece(lastPiece, {-1, 8, -1}, "second_roof", rotation, false));
            recursiveChildren(&EndCityBuilder::tower, genDepth + 1, lastPiece, {0, 0, 0}, pieces);
        } else if (numFloors == 2) {
            lastPiece = addHelper(pieces, addPiece(lastPiece, {-1, 0, -1}, "second_floor_2", rotation, false));
            lastPiece = addHelper(pieces, addPiece(lastPiece, {-1, 4, -1}, "third_floor_2", rotation, false));
            lastPiece = addHelper(pieces, addPiece(lastPiece, {-1, 8, -1}, "third_roof", rotation, true));
            recursiveChildren(&EndCityBuilder::tower, genDepth + 1, lastPiece, {0, 0, 0}, pieces);
        }
        return true;
    }

    // TOWER_GENERATOR
    bool tower(int genDepth, EndCityPiece* parent, const BlockPos&, PieceList& pieces) {
        static const std::pair<Rot, BlockPos> kTowerBridges[4] = {
            {Rot::None, {1, -1, 0}}, {Rot::CW90, {6, -1, 1}}, {Rot::CCW90, {0, -1, 5}}, {Rot::CW180, {5, -1, 6}}};
        const Rot rotation = parent->settings.rotation;
        EndCityPiece* lastPiece = parent;
        const int baseX = 3 + random.nextInt(2);
        const int baseZ = 3 + random.nextInt(2);
        lastPiece = addHelper(pieces, addPiece(lastPiece, {baseX, -3, baseZ}, "tower_base", rotation, true));
        lastPiece = addHelper(pieces, addPiece(lastPiece, {0, 7, 0}, "tower_piece", rotation, true));
        EndCityPiece* bridgePiece = random.nextInt(3) == 0 ? lastPiece : nullptr;
        const int towerHeight = 1 + random.nextInt(3);
        for (int i = 0; i < towerHeight; ++i) {
            lastPiece = addHelper(pieces, addPiece(lastPiece, {0, 4, 0}, "tower_piece", rotation, true));
            if (i < towerHeight - 1 && random.nextBoolean()) bridgePiece = lastPiece;
        }
        if (bridgePiece) {
            for (const auto& [bridgeRot, bridgePos] : kTowerBridges) {
                if (random.nextBoolean()) {
                    EndCityPiece* bridgeStart =
                        addHelper(pieces, addPiece(bridgePiece, bridgePos, "bridge_end", rotated(rotation, bridgeRot), true));
                    recursiveChildren(&EndCityBuilder::towerBridge, genDepth + 1, bridgeStart, {0, 0, 0}, pieces);
                }
            }
            addHelper(pieces, addPiece(lastPiece, {-1, 4, -1}, "tower_top", rotation, true));
        } else {
            if (genDepth != 7) return recursiveChildren(&EndCityBuilder::fatTower, genDepth + 1, lastPiece, {0, 0, 0}, pieces);
            addHelper(pieces, addPiece(lastPiece, {-1, 4, -1}, "tower_top", rotation, true));
        }
        return true;
    }

    // TOWER_BRIDGE_GENERATOR
    bool towerBridge(int genDepth, EndCityPiece* parent, const BlockPos&, PieceList& pieces) {
        const Rot rotation = parent->settings.rotation;
        const int bridgeLength = random.nextInt(4) + 1;
        EndCityPiece* lastPiece = addHelper(pieces, addPiece(parent, {0, 0, -4}, "bridge_piece", rotation, true));
        lastPiece->genDepth = -1;
        int nextY = 0;
        for (int i = 0; i < bridgeLength; ++i) {
            if (random.nextBoolean()) {
                lastPiece = addHelper(pieces, addPiece(lastPiece, {0, nextY, -4}, "bridge_piece", rotation, true));
                nextY = 0;
            } else {
                if (random.nextBoolean())
                    lastPiece = addHelper(pieces, addPiece(lastPiece, {0, nextY, -4}, "bridge_steep_stairs", rotation, true));
                else
                    lastPiece = addHelper(pieces, addPiece(lastPiece, {0, nextY, -8}, "bridge_gentle_stairs", rotation, true));
                nextY = 4;
            }
        }
        if (!shipCreated && random.nextInt(10 - genDepth) == 0) {
            const int shipX = -8 + random.nextInt(8);
            const int shipZ = -70 + random.nextInt(10);
            addHelper(pieces, addPiece(lastPiece, {shipX, nextY, shipZ}, "ship", rotation, true));
            shipCreated = true;
        } else if (!recursiveChildren(&EndCityBuilder::houseTower, genDepth + 1, lastPiece, {-3, nextY + 1, -11}, pieces)) {
            return false;
        }
        lastPiece = addHelper(pieces, addPiece(lastPiece, {4, nextY, 0}, "bridge_end", rotated(rotation, Rot::CW180), true));
        lastPiece->genDepth = -1;
        return true;
    }

    // FAT_TOWER_GENERATOR
    bool fatTower(int genDepth, EndCityPiece* parent, const BlockPos&, PieceList& pieces) {
        static const std::pair<Rot, BlockPos> kFatTowerBridges[4] = {
            {Rot::None, {4, -1, 0}}, {Rot::CW90, {12, -1, 4}}, {Rot::CCW90, {0, -1, 8}}, {Rot::CW180, {8, -1, 12}}};
        const Rot rotation = parent->settings.rotation;
        EndCityPiece* lastPiece = addHelper(pieces, addPiece(parent, {-3, 4, -3}, "fat_tower_base", rotation, true));
        lastPiece = addHelper(pieces, addPiece(lastPiece, {0, 4, 0}, "fat_tower_middle", rotation, true));
        for (int i = 0; i < 2 && random.nextInt(3) != 0; ++i) {
            lastPiece = addHelper(pieces, addPiece(lastPiece, {0, 8, 0}, "fat_tower_middle", rotation, true));
            for (const auto& [bridgeRot, bridgePos] : kFatTowerBridges) {
                if (random.nextBoolean()) {
                    EndCityPiece* bridgeStart =
                        addHelper(pieces, addPiece(lastPiece, bridgePos, "bridge_end", rotated(rotation, bridgeRot), true));
                    recursiveChildren(&EndCityBuilder::towerBridge, genDepth + 1, bridgeStart, {0, 0, 0}, pieces);
                }
            }
        }
        addHelper(pieces, addPiece(lastPiece, {-2, 8, -2}, "fat_tower_top", rotation, true));
        return true;
    }
};

class EndCityStructure final : public Structure {
public:
    std::optional<Stub> findGenerationPoint(GenCtx& c) const override {
        const int bx = c.minBlockX() + 7, bz = c.minBlockZ() + 7;
        // couldValidBiomeExistInTerrainColumn: getMinY() - 1 .. getMaxY() (inclusive)
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
                        PieceList pieces;
                        EndCityBuilder(ctx->env, ctx->random).startHouseTower(start, rotation, pieces);
                        for (auto& p : pieces) b.add(std::move(p));
                    }};
    }
};

}  // namespace

void registerEndCity() {
    registerStructureType("minecraft:end_city", [](const json&, StructureEnv&) -> std::unique_ptr<Structure> {
        return std::make_unique<EndCityStructure>();
    });
}

}  // namespace wg::st
