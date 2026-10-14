#pragma once
#include <array>
// Block entities the game draws with its block entity renderers: chests (all materials, double halves, lid
// animation), sign and hanging sign text, banners with their pattern layers, mob heads and player heads (default
// skins), shulker boxes, bells, decorated pots with sherds, conduits, enchanting table and lectern books,
// campfire and shelf items, copper golem statues, beacon beams, End portal and End gateway surfaces (and the
// gateway beam), the mob spinning in spawners and trial spawners, the vault's display item, the item in suspicious
// sand / gravel while it is brushed, and blocks moved by pistons (PistonMovingBlockEntity, simulated from the
// pistons' block events like the game's client does).
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
#include "game/player.h"
#include "entity/entity_assets.h"
#include "render/range_allocator.h"
#include "render/renderer.h"
#include "ui/font.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

class World;
class BlockRegistry;
class GameReports;
class ItemRules;
namespace net {
class Client;
struct BlockChange;
}
namespace ent {
class EntityRenderer;
}

namespace be {

struct M34;  // row-major 3x4 transform (block_entities.cpp)

// One quad as the shader reads it (shaders/blockentity.hlsl).
struct BeQuad {
    float pos[4][3];  // static: relative to the section origin; dynamic: camera-relative
    float uv[4][2];
    u32 normal;       // snorm8 x, y, z
    u32 color;        // 0xAABBGGRR tint
    u32 light;        // sky << 4 | block; 256: full bright; 512: no diffuse shading; 1024: only within 16 blocks;
                      // 2048: casts no shadow
    u32 tex;          // texture: kTexAtlas, kTexFont, kTexSolid, kTexAtlasRepeat, kTexEndPortal, kTexEndGateway or a
                      // block texture array layer
    float pivot[3];   // banner flag wave: rotation pivot (same space as pos); kTexAtlasRepeat: the texture's atlas
                      // rect u0, v0, du (dv in `normal`, as float bits)
    u32 wave;         // 0, or 1 << 31 | phase offset (0..99) << 16 | axis angle (u16 of a full turn)
};
static_assert(sizeof(BeQuad) == 112);
constexpr u32 kTexAtlas = 0xFFFFFFFF, kTexFont = 0xFFFFFFFE, kTexSolid = 0xFFFFFFFD;
// An entity atlas texture repeating over uv (beacon beams), and the end portal / gateway effect (screen space).
constexpr u32 kTexAtlasRepeat = 0xFFFFFFFC, kTexEndPortal = 0xFFFFFFFB, kTexEndGateway = 0xFFFFFFFA;

enum class Kind : u8 {
    None, Chest, Sign, HangingSign, Banner, Skull, Shulker, Bell, Pot, Conduit, Enchanting, Lectern, Campfire, Shelf,
    Statue, Beacon, EndPortal, EndGateway, Spawner, TrialSpawner, Vault, Brushable, TestInstance
};

class BlockEntityRenderer : public RenderPass {
public:
    // ShulkerBoxBlockEntity.moveCollidedEntities: the lid's sweep of an opening box this tick (min xyz, max xyz) and the
    // direction it opens to (0 down .. 5 east); entities in it are moved by its size + 0.01 that way.
    struct ShulkerPush {
        double box[6];
        int dir;
    };
    const std::vector<ShulkerPush>& shulkerPushes() const { return shulkerPushes_; }
    BlockEntityRenderer(World& world, BlockRegistry& registry, const GameReports& reports, const ent::EntityAssets& assets,
                        const ent::EntityRenderer& entityRenderer);
    ~BlockEntityRenderer() override;
    // The game's font for sign text (before create()).
    bool load(const fs::path& jar, const fs::path& minecraftDir);
    // Each frame: the effective render distance (chunks: beacon beams are seen that far, BeaconRenderer.getViewDistance)
    // and whether the player looks through a spyglass (the beam keeps its width, BeaconRenderer.submit).
    void setView(int renderChunks, bool scoping) { renderChunks_ = renderChunks, scoping_ = scoping; }
    void setGameTime(i64 t) { levelTime_ = t; }  // the level's game time (trial spawner next_mob_spawns_at)
    // Level.getNearestPlayer(x, y, z, maxDist, false): the nearest non-spectator player's feet (enchanting table book).
    std::function<bool(const double at[3], double maxDist, double out[3])> nearestPlayer;
    // AbstractSignRenderer.isOutlineVisible: glowing text outlines within 16 blocks of the camera entity's position
    // (its feet, world space), or always while scoping in first person.
    void setOutlineView(const double cameraEntity[3], bool firstPersonScoping) {
        std::copy(cameraEntity, cameraEntity + 3, outlineFrom_), outlineAlways_ = firstPersonScoping;
    }
    // Network: block entity data, chunk block entity lists and block events (before Client::start).
    void attach(net::Client& client);
    // Block events received so far, and the end of piston moves (main thread, before the session applies the block
    // updates that follow them: pistons read the blocks they move as they were when the event happened).
    void applyBlockEvents();
    // Piston push rules (push reactions, hardness); pistons do not animate without them.
    void setRules(const ItemRules* rules) { rules_ = rules; }
    // Client-side block changes (the moving_piston blocks and cleared cells a piston event makes on the game's
    // client too; the server sends the final blocks later). Called from applyBlockEvents.
    using BlockSink = std::function<void(const std::vector<net::BlockChange>&)>;
    void setBlockSink(BlockSink sink) { blockSink_ = std::move(sink); }
    // The moving blocks for the player's physics (MovingPistonBlock collisions, PistonMovingBlockEntity pushes).
    void pistonMovers(std::vector<Physics::Piston>& out);

    bool create(Renderer& renderer) override;
    void draw(const PassContext& ctx) override;
    // Shader pipeline: built once in prepare; shadow casters per cascade (sections inside it + animating ones), and
    // motion vectors for animating instances (last frame's corners) and waving banners.
    void prepare(const PassContext& ctx) override;
    void drawShadow(const ShadowContext& sc) override;

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
        std::string items[4];                       // campfire / shelf items by slot (item names); vault display
                                                    // item, brushable block item (slot 0)
        std::string itemNbt[4];                     // the same stacks as NBT compounds (payloads); player heads with a
                                                    // profile: a player_head stack carrying it (slot 0)
        bool alignBottom = false;                   // shelf
        int entity = -1;                            // spawner / trial spawner display entity (protocol id)
        int delay = 20;                             // spawner Delay
        i64 nextMobSpawnsAt = 0;                    // trial spawner next_mob_spawns_at (sent while active)
        i8 hitDir = -1;                             // brushable block hit_direction (3D data value)
        i64 age = -1;                               // end gateway Age (-1: unknown)
        u32 beam = 0;                               // test instance block: its beam colour (0xRRGGBB | 1 << 24), 0 none
        std::vector<std::string> connected;         // vault shared_data connected_players (16-byte UUIDs)
        double connectedRange = 4.5;                // vault shared_data connected_particles_range
    };
    // Client-side effects of the block entities' ticks (spawner / trial spawner / vault particles), called while
    // the world lock is held: a particle by type name, and a player's position (feet) and height by UUID.
    std::function<void(std::string_view type, double x, double y, double z, double vx, double vy, double vz)> particle;
    std::function<bool(const std::string& uuid, double pos[3], float& height)> playerPosition;
    // The UI (sign editor): the last renderer created, and the data of the block entity at a position (main thread;
    // nullptr when none is known).
    static const BlockEntityRenderer* instance();
    const Data* dataAt(int x, int y, int z) const;
    // AbstractSignEditScreen.setMessage -> SignBlockEntity.setText: the editor's lines on the sign's side while typing
    // (main thread); they stay until the block entity's data changes (the server's update after the editor closes).
    // nullptr lines: dropped at once.
    static void previewSign(int x, int y, int z, bool front, const std::array<std::string, 4>* lines);
    // The model quads of the block entity at (x, y, z) relative to its block origin (crumbling overlays while it is
    // being broken; no text, beams or portal effects). Empty when none is tracked there.
    void crackQuads(int x, int y, int z, std::vector<BeQuad>& out) const;

    // Items the block entities show (campfire / shelf items, the vault's display item, a brushed item) and player
    // heads with a profile: with an item drawer set they are left to it (the entity visuals draw them with the full
    // item models and downloaded skins), else drawn here as simple sprites / blocks and default skins.
    struct ShownItem {
        const std::string* nbt = nullptr;  // Data::itemNbt of the stack
        int kind = 0;                      // 0 an item, 1 a player head (the skull model at `m`, no display transform)
        int context = 0;                   // ItemDisplayContext: 0 fixed, 1 on_shelf, 2 ground (+ copies by count)
        bool shelfBottom = false;          // on_shelf: align the model's bottom (else its centre) at the origin
        float m[12];                       // pose relative to `origin` (row-major 3x4)
        int origin[3];                     // the block
        u32 light;                         // sky << 4 | block
        int seed = 0;
    };
    void setItemDrawer(bool on) { itemDrawer_ = on; }
    // This frame's shown items of the tracked block entities (main thread).
    void shownItems(std::vector<ShownItem>& out) const;

private:
    struct SignPreview {
        bool front = true;
        std::array<std::string, 4> lines;
        std::string nbt;  // the block entity's NBT the preview went over (a new one replaces the preview)
        bool seen = false;
    };
    std::unordered_map<u64, SignPreview> signPreview_;  // previewSign
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
        bool beams = false;        // has beacons / end gateways: their beams are drawn wherever the section is
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
        // beacon: beam sections (colour 0xRRGGBB, height) and pyramid levels
        std::vector<std::pair<u32, int>> beam;
        int levels = 0;
        // end gateway
        i64 age = -1;
        int cooldown = 0;
        // spawner / trial spawner mob and vault item spin (degrees)
        float spin = 0, oSpin = 0;
        int delay = -1;
        bool dynamic = false;  // drawn per frame at the last section build
    };
    // PistonMovingBlockEntity on the client: a block moved by a piston, at its destination (a moving_piston block,
    // set on the client like the game's client does: the server does not send those). Created from the piston's
    // block event (PistonBaseBlock.triggerEvent / moveBlocks); it moves over two ticks and stays until the server's
    // final block replaces the moving_piston, or places the moved block itself five ticks later.
    struct Moving {
        i32 x, y, z;
        u16 moved;     // the moved block state
        u8 dir;        // the piston's facing
        bool extending, source;
        double start;  // when the block event arrived (seconds)
        double gone;   // when the moving_piston was replaced (seconds; < 0: not yet)
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
    static bool ticking(const BlockDef& d);  // animates on its own tick (kept in anims_ while tracked)
    void updateTracking(double camX, double camY, double camZ);
    void scanColumn(int cx, int cz, bool farOnly = false);
    void dropColumn(int cx, int cz);
    void decode(const std::string& nbt, const BlockDef& d, Data& out) const;
    void tick();
    void handleEvent(int x, int y, int z, int type, int param);
    void pistonEvent(int x, int y, int z, int type, int param);
    float movingProgress(const Moving& m, double now) const;
    void emitMoving(Emit& e, const Moving& m, double now) const;
    void beaconTick(int x, int y, int z, Anim& a);
    void emitBeam(Emit& e, const M34& base, int tex, float scale, float animTime, int start, int height, u32 color,
                  float solidRadius, float glowRadius) const;
    void emitItem(Emit& e, const std::string& item, const M34& m, u32 light, int context) const;
    void emitDisplayEntity(Emit& e, int type, const M34& m, u32 light, float age) const;
    bool isDynamic(const Instance& in, const BlockDef& d) const;
    void refreshLight();
    void emitInstance(Emit& e, const Instance& in, const Data* data, float pt) const;
    void buildSection(Section& s, std::vector<BeQuad> out[3]) const;
    u8 sampleLight(int x, int y, int z) const;
    void applyNetwork();

    // block_entity_render.cpp: GPU
    void uploadSections(const PassContext& ctx);
    void build(const PassContext& ctx);   // CPU: tracking, culling, dynamic rebuilds, uploads
    void record(const PassContext& ctx);  // the World-stage draws of the last build
    void bindGpu(ID3D12GraphicsCommandList* list, u32 frame, D3D12_GPU_DESCRIPTOR_HANDLE blockTextures);

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
    std::unordered_map<u64, bool> columns_;  // column -> far (beyond the 64 blocks: only beacons / gateways tracked)
    int renderChunks_ = 12, trackedFar_ = 0;
    bool scoping_ = false;
    double outlineFrom_[3] = {};
    bool outlineAlways_ = false;
    std::unordered_map<u64, std::vector<u64>> columnSections_;  // column -> section keys
    std::unordered_map<u64, Section> sections_;
    int centerCx_ = INT32_MIN, centerCz_ = INT32_MIN;
    std::vector<u64> changes_;
    size_t lightCursor_ = 0;
    std::vector<u64> sectionOrder_;  // for the round-robin light refresh

    std::unordered_map<u64, Anim> anims_;  // by block position
    std::vector<ShulkerPush> shulkerPushes_;
    std::vector<Moving> moving_;
    bool shading_ = false;  // the last build drew for the shader pipeline
    const ItemRules* rules_ = nullptr;
    BlockSink blockSink_;
    double tickClock_ = 0, lastTime_ = 0;
    std::atomic<float> tickRate_{20.0f};  // ticking_state: the server's tick rate and freeze
    std::atomic<bool> frozen_{false};
    i64 gameTicks_ = 0;
    i64 levelTime_ = 0;
    float partial_ = 0;
    u32 random_ = 0x9E3779B9u;
    bool xmas_ = false;  // Christmas chest textures
    bool itemDrawer_ = false;
    mutable std::vector<ShownItem>* shownOut_ = nullptr;  // shownItems: emitItem / the player heads record here
    mutable const std::string* shownNbt_ = nullptr;      // the stack emitItem is called for
    mutable int shownSeed_ = 0;
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
        int block = -1;  // the block registry id it is for (ClientLevel.blockEvent: only while that block is there)
    };
    std::vector<std::string> blockNames_;  // the block registry by network id (from the reports)
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
        int beaconBeam = -1, gatewayBeam = -1, endSky = -1, endPortal = -1;
    } t_;
    mutable std::unordered_map<std::string, int> itemBlocks_;  // item name -> block id (0: not a block)

    // GPU
    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> psoCutout_, psoText_, psoTranslucent_;
    ComPtr<ID3D12PipelineState> psoCutoutMv_, psoTextMv_, psoTranslucentMv_, psoShadow_;  // motion target / shadow maps
    // Last build.
    bool prepared_ = false, built_ = false;
    u32 visCount_[3] = {}, quadTotal_[3] = {}, dynFirst_[3] = {}, dynCount_[3] = {};
    u32 shadowVis_[4][2] = {}, shadowQuads_[4][2] = {};  // per cascade: sections / quads (cutout, translucent)
    // Motion vectors of animating instances: last frame's corner positions (camera-relative to that frame's camera).
    struct PrevDyn {
        u64 frame = 0;
        double cam[3] = {};
        u32 counts[3] = {};
        std::vector<float> pos;
    };
    std::unordered_map<u64, PrevDyn> prevDyn_;  // by block position
    std::vector<float> dynPrev_[3];              // previous corners of dyn_[p] (12 floats per quad)
    u64 buildFrame_ = 0;
    float waveTime_ = 0;                         // banner wave time of the last build
    bool waveTimeValid_ = false;
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
    void packFlush(class PackSink& pack, u16 block);  // packQuads_ as the block's pack draws

    Stats stats_;
    double cpuTotal_ = 0;
    u64 frames_ = 0;
    static constexpr u32 kHistory = 256;
    float historyMs_[kHistory] = {};
};

}  // namespace be
