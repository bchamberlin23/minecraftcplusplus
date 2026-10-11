#pragma once
// Structure templates (data/<ns>/structure/*.nbt): loading, the game's StructureTemplate transforms, the
// structure processors (data driven processor lists and the built-in ones) and StructureTemplate.placeInWorld.
#include "worldgen/structure_core.h"
#include "world/nbt_dom.h"

#include <mutex>

namespace wg::st {

struct TemplateBlock {
    BlockPos pos;
    u16 state;
    const NbtValue* nbt;  // block entity data, or null
};

struct JigsawInfo {  // StructureTemplate.JigsawBlockInfo (positions relative to the template)
    BlockPos pos;
    u16 state;          // the jigsaw block state (orientation)
    bool rollable;      // joint type
    std::string name;   // "" for none (feature elements)
    std::string pool;
    std::string target;
    int placementPriority = 0, selectionPriority = 0;
    Direction front = North, top = Up;
};

struct Template {
    std::string id;
    BlockPos size;
    struct Palette {
        std::vector<TemplateBlock> blocks;  // full blocks, other blocks, block entities; each by y, x, z
        std::vector<JigsawInfo> jigsaws;    // the jigsaw blocks, in block order
        std::vector<size_t> structureBlocks, jigsawBlocks;  // indices into blocks
    };
    std::vector<Palette> palettes;
    bool hasEntities = false;
    std::vector<std::unique_ptr<NbtValue>> nbtStore;

    // StructureTemplate.getSize(rotation)
    BlockPos sizeFor(Rot r) const { return (r == Rot::CW90 || r == Rot::CCW90) ? BlockPos{size.z, size.y, size.x} : size; }
};

// StructureTemplate.transform (block positions).
BlockPos transformPos(const BlockPos& p, Mir m, Rot r, const BlockPos& pivot);
// StructureTemplate.getZeroPositionWithTransform
BlockPos zeroPositionWithTransform(const BlockPos& zero, Mir m, Rot r, int sizeX, int sizeZ);
// StructureTemplate.getBoundingBox
BBox templateBox(const BlockPos& position, Rot r, const BlockPos& pivot, Mir m, const BlockPos& size);

class TemplateManager {
public:
    explicit TemplateManager(const StructureEnv& env) : env_(env) {}
    // "minecraft:village/plains/houses/plains_small_house_1"; never null (an empty template when missing).
    const Template* get(std::string_view id) const;

private:
    const StructureEnv& env_;
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, std::unique_ptr<Template>> cache_;
};

// ---- processors --------------------------------------------------------------------------------------
struct ProcInfo {  // StructureTemplate.StructureBlockInfo during processing
    BlockPos pos;
    u16 state;
    bool hasNbt;
    const NbtValue* nbt;  // the template's data (null when it was replaced or cleared)
    bool operator==(const ProcInfo& o) const { return pos == o.pos && state == o.state && hasNbt == o.hasNbt && nbt == o.nbt; }
};

struct PlaceSettings;

class Processor {
public:
    virtual ~Processor() = default;
    // processBlock; false removes the block.
    virtual bool process(PlaceCtx& c, const BlockPos& target, const BlockPos& ref, const BlockPos& templateRel, ProcInfo& info,
                         const PlaceSettings& s) const {
        (void)c, (void)target, (void)ref, (void)templateRel, (void)info, (void)s;
        return true;
    }
    virtual void finalize(PlaceCtx& c, const BlockPos& position, const BlockPos& ref, const std::vector<ProcInfo>& originals,
                          std::vector<ProcInfo>& processed, const PlaceSettings& s) const {
        (void)c, (void)position, (void)ref, (void)originals, (void)processed, (void)s;
    }
    virtual bool entirePiece() const { return false; }
};
using ProcessorList = std::vector<std::shared_ptr<const Processor>>;

// A processor list from JSON: a "minecraft:..." processor_list id, {"processors": [...]} or [...].
ProcessorList parseProcessors(const StructureEnv& env, const json& j);
// Built-in processors.
std::shared_ptr<const Processor> blockIgnore(const StructureEnv& env, std::vector<std::string> blocks);
std::shared_ptr<const Processor> jigsawReplacement();
std::shared_ptr<const Processor> gravity(HType h, int offset);
std::shared_ptr<const Processor> blockRot(float integrity);
std::shared_ptr<const Processor> blockAge(float mossiness);
std::shared_ptr<const Processor> blackstoneReplace();
std::shared_ptr<const Processor> lavaSubmerged();

struct PlaceSettings {  // StructurePlaceSettings
    Mir mirror = Mir::None;
    Rot rotation = Rot::None;
    BlockPos pivot{0, 0, 0};
    bool ignoreEntities = false;
    std::optional<BBox> box;
    bool applyWaterlogging = true;  // LiquidSettings.APPLY_WATERLOGGING
    RandomSource* random = nullptr; // null: a random seeded from each position
    ProcessorList processors;
    bool knownShape = false;
    bool finalizeEntities = false;

    // getRandom(pos).nextInt(palettes) - consumes `random` when one is set.
    const Template::Palette& palette(const Template& t, const BlockPos& pos) const;
    float randomFloatAt(const BlockPos& p) const;  // getRandom(pos).nextFloat() (block_rot / block_age use the full random)
};

// StructureTemplate.placeInWorld (updateMode ignored: blocks are written straight to the region).
bool placeTemplate(PlaceCtx& c, const Template& t, const BlockPos& position, const BlockPos& ref, const PlaceSettings& s,
                   RandomSource& random);
// StructureTemplate.filterBlocks(position, settings, block): blocks of one kind with world positions (rotation only,
// like the game) and the template's nbt.
std::vector<ProcInfo> filterBlocks(const Template& t, const BlockPos& position, const PlaceSettings& s, u16 blockState,
                                   const SB& b);
// StructureTemplate.getJigsaws(position, rotation): rotated jigsaw blocks offset by position.
std::vector<JigsawInfo> templateJigsaws(const Template& t, const BlockPos& position, Rot r, const SB& b);

// TemplateStructurePiece: one template placed with settings (igloos, shipwrecks, ocean ruins, ruined portals...).
class TemplatePiece : public Piece {
public:
    TemplatePiece(const char* type, int genDepth, const Template* t, PlaceSettings s, const BlockPos& pos)
        : Piece(type, genDepth, templateBox(pos, s.rotation, s.pivot, s.mirror, t->size)), tmpl(t), settings(std::move(s)),
          templatePosition(pos) {
        setOrientation(North);
    }
    const Template* tmpl;
    PlaceSettings settings;
    BlockPos templatePosition;

    void postProcess(PlaceCtx& c, RandomSource& random, const BBox& chunkBB, const BlockPos& ref) override;
    void move(int dx, int dy, int dz) override {
        Piece::move(dx, dy, dz);
        templatePosition = templatePosition.offset(dx, dy, dz);
    }

protected:
    virtual void handleDataMarker(PlaceCtx& c, const std::string& marker, const BlockPos& pos, RandomSource& random,
                                  const BBox& chunkBB) = 0;
};

// Block.updateFromNeighbourShapes / updateShapeAtEdge for the blocks whose shape changes during world generation.
u16 updateShape(PlaceCtx& c, u16 state, const BlockPos& pos, Direction dir, const BlockPos& neighbourPos, u16 neighbour);
u16 updateFromNeighbourShapes(PlaceCtx& c, u16 state, const BlockPos& pos);

}  // namespace wg::st
