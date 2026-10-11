#pragma once
// Block entities the game draws with its block entity renderers: chests (all materials, double halves, lid
// animation), sign and hanging sign text, banners with their pattern layers, mob heads and player heads (default
// skins), shulker boxes, bells, decorated pots with sherds, conduits, enchanting table and lectern books,
// campfire and shelf items and copper golem statues.
//
// Which blocks have one follows from the block states in World (like the game's client, which creates block
// entities from block states); their extra data (sign text, banner patterns, ...) comes from
// World::blockEntities (saves, server packets). Geometry uses the game's baked model layers and the entity
// texture atlas (entity/entity_assets.*); sign text uses the game's font.
//
// Performance: only chunks within the game's block entity view distance (64 blocks) are tracked. Static block
// entities are baked per 16^3 section into one GPU arena (banner flags wave in the vertex shader), so steady
// frames only cull sections and issue six draws; block entities that are animating (opening chests, ringing
// bells, enchanting books, ...) are rebuilt on the CPU each frame. World edits arrive through
// World::takeBlockChanges; light is re-sampled round-robin.
#include "core/common.h"
#include "entity/entity_assets.h"
#include "render/range_allocator.h"
#include "render/renderer.h"
#include "ui/font.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

class World;
class BlockRegistry;
class GameReports;
namespace net {
class Client;
}
namespace ent {
class EntityRenderer;
}

namespace be {

// One quad as the shader reads it (shaders/blockentity.hlsl).
struct BeQuad {
    float pos[4][3];  // static: relative to the section origin; dynamic: camera-relative
    float uv[4][2];
    u32 normal;       // snorm8 x, y, z
    u32 color;        // 0xAABBGGRR tint
    u32 light;        // sky << 4 | block; 256: full bright; 512: no diffuse shading; 1024: only within 16 blocks
    u32 tex;          // texture: kTexAtlas, kTexFont, kTexSolid or a block texture array layer
    float pivot[3];   // banner flag wave: rotation pivot (same space as pos)
    u32 wave;         // 0, or 1 << 31 | phase offset (0..99) << 16 | axis angle (u16 of a full turn)
};
static_assert(sizeof(BeQuad) == 112);
constexpr u32 kTexAtlas = 0xFFFFFFFF, kTexFont = 0xFFFFFFFE, kTexSolid = 0xFFFFFFFD;

enum class Kind : u8 {
    None, Chest, Sign, HangingSign, Banner, Skull, Shulker, Bell, Pot, Conduit, Enchanting, Lectern, Campfire, Shelf,
    Statue
};

class BlockEntityRenderer : public RenderPass {
public:
    BlockEntityRenderer(World& world, BlockRegistry& registry, const GameReports& reports, const ent::EntityAssets& assets,
                        const ent::EntityRenderer& entityRenderer);
    ~BlockEntityRenderer() override;
    // The game's font for sign text (before create()).
    bool load(const fs::path& jar, const fs::path& minecraftDir);
    // Network: block entity data, chunk block entity lists and block events (before Client::start).
    void attach(net::Client& client);

    bool create(Renderer& renderer) override;
    void draw(const PassContext& ctx) override;

    struct Stats {
        double cpuMs = 0;  // last frame: updates, culling, dynamic rebuilds, uploads
        u32 tracked = 0, sections = 0, visibleSections = 0, staticQuads = 0, dynamic = 0, dynamicQuads = 0;
    };
    const Stats& stats() const { return stats_; }

    // One decoded sign side.
    struct SignSide {
        ui::Text lines[4];
        u8 color = 15;  // dye id (black)
        bool glow = false;
    };
    // Extra data of one block entity, from its NBT.
    struct Data {
        SignSide front, back;
        std::vector<std::pair<int, u8>> patterns;  // banner layers: (atlas texture, dye id)
        int skin = -1;                              // player head texture
        int sherds[4] = {-1, -1, -1, -1};           // decorated pot sides: back, left, right, front
        std::string items[4];                       // campfire / shelf items by slot (item names)
        bool alignBottom = false;                   // shelf
    };

private:
    struct BlockDef {
        Kind kind = Kind::None;
        bool wall = false;
        u8 facing = 3;    // Direction 3D value: 0 down, 1 up, 2 north, 3 south, 4 west, 5 east
        u8 rotation = 0;  // 0..15 (standing signs, banners, heads)
        u8 variant = 0;   // chest material, head type, dye (+1, 0 = none), oxidation
        u8 part = 0;      // chest type (0 single, 1 left, 2 right), statue pose
        bool flag = false;  // lectern has_book, head powered
    };
    struct Instance {
        i32 x, y, z;
        u16 block;
        u8 light;        // sky << 4 | block
        i32 data = -1;   // index into Section::data
    };
    struct Section {
        int cx = 0, sy = 0, cz = 0;
        std::vector<Instance> list;
        std::vector<Data> data;
        std::vector<u32> dynamic;  // instances drawn per frame (animating)
        bool dirty = true;
        // GPU: [cutout][text][translucent] quads at `offset` in the arena
        u64 offset = 0, allocated = 0;
        u32 counts[3] = {};
    };
    struct Anim {
        float open = 0, oOpen = 0;  // chest lid / shulker progress
        bool shouldOpen = false;
        int bellTicks = 0;
        i8 shakeDir = -1;
        i64 wobbleStart = -1000;
        u8 wobbleStyle = 0;
        // enchanting table book
        float flip = 0, oFlip = 0, flipT = 0, flipA = 0, book = 0, oBook = 0, rot = 0, oRot = 0, tRot = 0;
        int time = 0;
        // conduit
        bool active = false, hunting = false;
        float activeRot = 0;
        int tickCount = 0;
        int skullTicks = 0;
        bool dynamic = false;  // drawn per frame at the last section build
    };
    struct Emit {  // quad output
        std::vector<BeQuad>* out[3];  // cutout, text, translucent
        double origin[3];             // world position that quads are relative to
        float camRight[3], camUp[3], camFwd[3];
    };

    // block_entities.cpp: tracking, data, animation, geometry
    void initAssets();
    const BlockDef& def(u16 block);
    BlockDef classify(u16 block) const;
    void updateTracking(double camX, double camY, double camZ);
    void scanColumn(int cx, int cz);
    void dropColumn(int cx, int cz);
    void decode(const std::string& nbt, const BlockDef& d, Data& out) const;
    void tick();
    void handleEvent(int x, int y, int z, int type, int param);
    bool isDynamic(const Instance& in, const BlockDef& d) const;
    void refreshLight();
    void emitInstance(Emit& e, const Instance& in, const Data* data, float pt) const;
    void buildSection(Section& s, std::vector<BeQuad> out[3]) const;
    u8 sampleLight(int x, int y, int z) const;
    void applyNetwork();

    // block_entity_render.cpp: GPU
    void uploadSections(const PassContext& ctx);

    World& world_;
    BlockRegistry& registry_;
    const GameReports& reports_;
    const ent::EntityAssets& assets_;
    const ent::EntityRenderer& entityRenderer_;
    Renderer* renderer_ = nullptr;

    // Fonts (own copy of the game's font and atlas: sign text is drawn in the world).
    std::unique_ptr<ui::Resources> res_;
    ui::Atlas fontAtlas_;
    std::unique_ptr<ui::Font> font_;
    mutable ui::Canvas canvas_;

    // Block state classification, by registry id (grown lazily).
    std::vector<BlockDef> defs_;
    std::vector<u8> defKnown_;

    // Tracked chunk columns around the camera and their sections with block entities.
    std::unordered_set<u64> columns_;
    std::unordered_map<u64, std::vector<u64>> columnSections_;  // column -> section keys
    std::unordered_map<u64, Section> sections_;
    int centerCx_ = INT32_MIN, centerCz_ = INT32_MIN;
    std::vector<u64> changes_;
    size_t lightCursor_ = 0;
    std::vector<u64> sectionOrder_;  // for the round-robin light refresh

    std::unordered_map<u64, Anim> anims_;  // by block position
    double tickClock_ = 0, lastTime_ = 0;
    i64 gameTicks_ = 0;
    float partial_ = 0;
    u32 random_ = 0x9E3779B9u;
    bool xmas_ = false;  // Christmas chest textures
    double cam_[3] = {};

    // Network: decoded on the network thread, applied on the main thread.
    struct NetBlockEntity {
        int x, y, z;
        std::string nbt;
    };
    struct NetChunk {
        int cx, cz;
        std::vector<NetBlockEntity> list;
    };
    struct NetEvent {
        int x, y, z, type, param;
    };
    std::mutex netMutex_;
    std::vector<NetChunk> netChunks_;
    std::vector<NetBlockEntity> netData_;
    std::vector<NetEvent> netEvents_;

    // Model layers and textures (resolved once).
    struct Models {
        int chest = -1, chestLeft = -1, chestRight = -1, shulker = -1, bell = -1, book = -1;
        int conduitShell = -1, conduitCage = -1, conduitWind = -1, conduitEye = -1;
        int potBase = -1, potSides = -1;
        int bannerStanding = -1, bannerStandingFlag = -1, bannerWall = -1, bannerWallFlag = -1;
        int skull[7] = {-1, -1, -1, -1, -1, -1, -1};  // skeleton, wither, zombie, creeper, dragon, piglin, player
        int statue[4] = {-1, -1, -1, -1};              // standing, running, sitting, star
    } m_;
    struct Textures {
        int chest[8][3];   // material x (single, left, right)
        int shulker[17];   // default, then by dye
        int bell = -1, book = -1, bannerBase = -1, bannerPatternBase = -1;
        int conduitBase = -1, conduitCage = -1, conduitWind = -1, conduitWindV = -1, conduitOpen = -1, conduitClosed = -1;
        int potBase = -1, potSide = -1;
        int skull[7] = {-1, -1, -1, -1, -1, -1, -1};
        int playerSkins[18];
        int statue[4] = {-1, -1, -1, -1};
    } t_;
    mutable std::unordered_map<std::string, int> itemBlocks_;  // item name -> block id (0: not a block)

    // GPU
    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> psoCutout_, psoText_, psoTranslucent_;
    ComPtr<ID3D12Resource> arena_, upload_, fontTex_;
    u8* mapped_ = nullptr;
    u32 fontDescriptor_ = 0;
    bool fontUploaded_ = false;
    RangeAllocator alloc_;
    struct PendingFree {
        u64 offset, count, frame;
    };
    std::vector<PendingFree> frees_;
    u64 frame_ = 0;
    bool arenaFull_ = false;
    std::vector<BeQuad> scratch_[3];
    std::vector<BeQuad> dyn_[3];
    // Shader packs (PassContext::pack): every visible instance as vanilla-style vertices, per instance (blockEntityId).
    std::vector<BeQuad> packQuads_[3];
    void packInstance(class PackSink& pack, const Emit& e, const Instance& in, const Data* data, float pt);

    Stats stats_;
    double cpuTotal_ = 0;
    u64 frames_ = 0;
    static constexpr u32 kHistory = 256;
    float historyMs_[kHistory] = {};
};

}  // namespace be
