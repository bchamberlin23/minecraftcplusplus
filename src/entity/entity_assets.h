#pragma once
// Entity geometry, textures and type data, all taken from the player's installed game:
//  - model layers: the client's own LayerDefinitions, baked and dumped to JSON once per version by a tiny
//    Java program run against the client jar with the launcher's Java runtime (cached);
//  - entity type dimensions, update intervals and synched-data ids, dumped by the same program;
//  - textures (textures/entity/**, textures/item/**) packed into one RGBA atlas;
//  - mob variant registries (data/minecraft/*_variant/*.json).
#include "core/common.h"

#include <unordered_map>

class GameReports;

namespace ent {

// Model parts the animation code addresses, by part name.
enum Role : u8 {
    RoleNone = 0,
    Head, Hat, Body, RightArm, LeftArm, RightLeg, LeftLeg,
    RightHindLeg, LeftHindLeg, RightFrontLeg, LeftFrontLeg,
    RightMiddleHindLeg, LeftMiddleHindLeg, RightMiddleFrontLeg, LeftMiddleFrontLeg,
    RightWing, LeftWing, Jacket, RightSleeve, LeftSleeve, RightPants, LeftPants,
    BasePlate, RightBodyStick, LeftBodyStick, ShoulderStick, Tail, RightFoot, LeftFoot,
    RoleCount
};

struct PartPose {
    float x = 0, y = 0, z = 0;           // pivot in model pixels (1/16 block)
    float xRot = 0, yRot = 0, zRot = 0;  // radians, applied Z * Y * X like ModelPart
    float xScale = 1, yScale = 1, zScale = 1;
};

struct ModelPart {
    i32 parent = -1;  // parts are in pre-order: parent < child
    Role role = RoleNone;
    PartPose pose;    // initial pose
    u32 quadFirst = 0, quadCount = 0;  // into EntityAssets::quads()
    std::string name;  // the game's part name ("lid", "flag"), for block entity animation
};

struct Model {
    std::string name;  // "minecraft:cow#main"
    std::vector<ModelPart> parts;
    i16 roles[RoleCount];  // part index or -1
    u32 quadCount = 0;
    bool baby = false;  // a baby layer ("cow_baby#main")
    i16 role(Role r) const { return roles[r]; }
    struct NameHash {
        using is_transparent = void;
        size_t operator()(std::string_view s) const { return std::hash<std::string_view>()(s); }
    };
    // By part name (ModelPart.createPartLookup: shallowest first); "root" is the top part.
    std::unordered_map<std::string, i16, NameHash, std::equal_to<>> partIndex;
    i16 part(std::string_view name) const {
        auto it = partIndex.find(name);
        return it == partIndex.end() ? i16(-1) : it->second;
    }
};

// A keyframe animation (AnimationDefinition) as the game bakes it: positions in model pixels (y down), rotations in
// radians, scales as offsets from 1.
struct Keyframe {
    float t;
    u8 catmullRom;
    float pre[3], post[3];
};
struct KeyChannel {
    std::string bone;
    u8 target;  // 0 position, 1 rotation, 2 scale
    std::vector<Keyframe> keys;
};
struct KeyAnimation {
    float length = 0;
    bool looping = false;
    std::vector<KeyChannel> channels;
};

// One face of a model cube: model-pixel positions (part space) and normalized texture coordinates.
struct GpuQuad {
    float pos[4][3];
    float uv[4][2];
    float normal[3];
    u32 part;  // index of the part in its model
};
static_assert(sizeof(GpuQuad) == 96);

struct TexRect {
    float u0 = 0, v0 = 0, du = 0, dv = 0;  // atlas-normalized
    int w = 0, h = 0;
};

struct TypeInfo {
    std::string name;  // "minecraft:cow"
    float width = 0.6f, height = 1.8f, eye = 1.62f;
    int updateInterval = 3;
    std::unordered_map<std::string, int> data;  // "Class.FIELD" -> synched data id
    int dataId(std::string_view field) const;  // by field name (any class), -1 if absent
};

struct Variant {
    std::string name;   // registry key without namespace, e.g. "temperate"
    int tex = -1, babyTex = -1;
    std::string model;  // "normal", "cold", "warm"
    int wild = -1, tame = -1, angry = -1, babyWild = -1, babyTame = -1, babyAngry = -1;  // wolves
};

class EntityAssets {
public:
    // cacheDir: e.g. %LOCALAPPDATA%\minecraftCplusplus\models (the version is appended).
    bool load(const fs::path& minecraftDir, const std::string& version, const fs::path& jar, const fs::path& cacheDir,
              const GameReports& reports);

    int model(std::string_view layer) const;  // "minecraft:cow#main" or "cow#main"; -1 if missing
    const Model& modelAt(int i) const { return models_[size_t(i)]; }
    const std::vector<GpuQuad>& quads() const { return quads_; }

    int texture(std::string_view path) const;  // "entity/cow/cow_temperate" (no extension); -1 if missing
    int itemTexture(std::string_view item) const;  // item name without namespace; -1 if missing
    const TexRect& rect(int tex) const { return rects_[size_t(tex)]; }
    const std::vector<u8>& atlas() const { return atlas_; }
    u32 atlasWidth() const { return atlasW_; }
    u32 atlasHeight() const { return atlasH_; }

    const TypeInfo* type(int protocolId) const;
    int typeId(std::string_view name) const;  // protocol id or -1
    const std::vector<TypeInfo>& types() const { return types_; }  // by protocol id
    const std::vector<std::string>& itemNames() const { return items_; }  // by protocol id

    // Keyframe animations by "<definitions class>.<field>", e.g. "WardenAnimation.WARDEN_ROAR"; nullptr if missing.
    const KeyAnimation* animation(std::string_view name) const;

    // Data-driven variant registries; network ids follow the registry order (entries sorted by name).
    const std::vector<Variant>& variants(std::string_view registry) const;  // "cow_variant"

private:
    bool loadImpl(const fs::path& minecraftDir, const std::string& version, const fs::path& jar,
                  const fs::path& cacheDir, const GameReports& reports);
    bool parseModels(const std::vector<u8>& json);
    bool parseAnimations(const std::vector<u8>& json);
    bool loadTextures(const fs::path& jar);

    std::vector<Model> models_;
    std::unordered_map<std::string, int> modelIndex_;
    std::vector<GpuQuad> quads_;
    std::vector<TexRect> rects_;
    std::unordered_map<std::string, int> texIndex_;
    std::vector<u8> atlas_;
    u32 atlasW_ = 0, atlasH_ = 0;
    std::vector<TypeInfo> types_;
    std::unordered_map<std::string, int> typeIndex_;
    std::vector<std::string> items_;
    std::unordered_map<std::string, std::vector<Variant>> variants_;
    std::unordered_map<std::string, KeyAnimation> animations_;
};

}  // namespace ent
