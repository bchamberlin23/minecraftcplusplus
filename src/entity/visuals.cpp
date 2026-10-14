#include "entity/visuals.h"

#include "audio/audio.h"

#include "assets/asset_source.h"
#include "assets/assets.h"
#include "blockentity/block_entities.h"
#include "assets/game_reports.h"
#include "core/math.h"
#include "entity/entities.h"
#include "entity/entity_render.h"
#include "game/game.h"
#include "game/item_rules.h"
#include "net/client.h"
#include "render/pack_sink.h"
#include "ui/font.h"
#include "ui/items.h"
#include "ui/mp_hud.h"
#include "ui/resources.h"
#include "ui/ui.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <shared_mutex>

#include "ParticleCutoutMvPS.h"
#include "ParticleCutoutPS.h"
#include "ParticleTranslucentPS.h"
#include "ParticleVS.h"
#include "VisualsCutoutMvPS.h"
#include "VisualsCutoutPS.h"
#include "VisualsOutlinePS.h"
#include "VisualsShadowPS.h"
#include "VisualsShadowVS.h"
#include "VisualsGlintPS.h"
#include "VisualsCrumblingPS.h"
#include "VisualsTextPS.h"
#include "VisualsTranslucentPS.h"
#include "VisualsVS.h"

namespace ent {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;

// Per-frame upload slice: three constant buffers, instances, dynamic quads, particles, staging for copies.
constexpr u32 kMaxInstances = 1 << 14, kMaxDynamic = 1 << 15, kMaxParticles = 16384;
constexpr u64 kCbBytes = 1024;
constexpr u64 kInstOffset = kCbBytes * 4;  // frame constants: world, HUD stage, hand, outline (no jitter)
constexpr u64 kDynOffset = kInstOffset + u64(kMaxInstances) * 128;
constexpr u64 kParticleOffset = kDynOffset + u64(kMaxDynamic) * sizeof(MeshQuad);
constexpr u64 kStageOffset = (kParticleOffset + u64(kMaxParticles) * sizeof(fx::GpuParticle) + 511) & ~u64(511);
constexpr u64 kStageBytes = u64(12) << 20;
constexpr u64 kFrameBytes = (kStageOffset + kStageBytes + 65535) & ~u64(65535);

struct GpuInst {
    float m[12];
    u32 color, light, overlay, flags, first, count, prefix, pad;
    float prev[12];  // last frame's transform (motion vectors; = m when unknown)
};
static_assert(sizeof(GpuInst) == 128);

struct FrameCb {
    float viewProj[16];
    float fogColor[4];
    float fogParams[4];
    float light0[4], light1[4];
    float camRight[4], camUp[4];
    float glintRect[4];
    float glint[4];
    float lightmap[3][4];
    float prevViewProj[16];  // shader pipeline: this frame's camera-relative position -> previous clip (unjittered)
    float motion[4];         // PassContext::motionScale
    float shadowMat[4][16];  // shadow cascades
    float fogRender[4];      // the render distance fog: start, end
};
static_assert(sizeof(FrameCb) <= kCbBytes);

u32 abgr(u32 rgb, u32 a = 255) { return (a << 24) | ((rgb & 255) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 255); }

bool check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    LOG("visuals: %s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    return false;
}

float lerpAngle(float a, float from, float to) {
    float d = std::fmod(to - from, 360.0f);
    if (d >= 180) d -= 360;
    if (d < -180) d += 360;
    return from + a * d;
}
float wrapDegrees(float d) {
    d = std::fmod(d, 360.0f);
    if (d >= 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return d;
}

// Camera yRot / xRot (degrees) from the view direction (the view-projection's w row): display billboards.
void cameraAngles(const PassContext& ctx, float& yaw, float& pitch) {
    float f[3], l = 0;
    for (int c = 0; c < 3; ++c) f[c] = ctx.viewProj[c * 4 + 3], l += f[c] * f[c];
    l = std::sqrt(std::max(l, 1e-12f));
    yaw = std::atan2(-f[0], f[2]) / kDeg, pitch = std::asin(std::clamp(-f[1] / l, -1.0f, 1.0f)) / kDeg;
}

// Instance list of one draw (contiguous in the upload slice after copying).
// key: identifies the instance across frames (motion vectors: Impl::flush finds last frame's transform), 0 = none.
struct List {
    std::vector<GpuInst> v;
    std::vector<u64> keys;
    u32 base = 0, quads = 0;
    u8 tag = 0;
    void clear() { v.clear(), keys.clear(), base = 0, quads = 0; }
    void add(const Xf& m, u32 first, u32 count, u32 light, u32 color = 0xFFFFFFFF, u32 flags = 0, u64 key = 0) {
        if (!count) return;
        GpuInst g;
        std::memcpy(g.m, m.m, sizeof(g.m));
        g.color = color, g.light = light, g.overlay = 0, g.flags = flags, g.first = first, g.count = count, g.prefix = 0, g.pad = 0;
        std::memcpy(g.prev, m.m, sizeof(g.prev));
        v.push_back(g);
        keys.push_back(key ? key * 4 + tag : 0);
    }
};
struct Lists {
    List cut, tr, glint;
    u64 key = 0;  // instance keys of the pushes that follow (begin): key << 8 | push sequence
    u32 seq = 0;
    Lists() { tr.tag = 1, glint.tag = 2; }
    void clear() { cut.clear(), tr.clear(), glint.clear(); }
    void begin(u64 k) { key = k, seq = 0; }
    u64 next() { return key ? (key << 8) | (++seq & 255) : 0; }
};

void entityBox(const Entity& e, float& w, float& h);  // below: the entity's width and height
}  // namespace

struct StagePass : RenderPass {
    Visuals::Impl* impl = nullptr;
    int stage = 0;  // 0 update (Sky), 1 World, 2 AfterTranslucent, 3 Hud
    bool create(Renderer& r) override;
    void draw(const PassContext& ctx) override;
    void prepare(const PassContext& ctx) override;
    void drawShadow(const ShadowContext& sc) override;
};

struct Visuals::Impl {
    Visuals& self;
    EntityWorld& world;
    EntityRenderer& er;
    const EntityAssets& assets;
    std::unique_ptr<ui::Resources> res;
    std::unique_ptr<ui::Items> items;
    ItemModels models;
    std::unique_ptr<fx::Particles> particles;
    double animClock = 0;  // seconds of updates: the client ticks of the item atlas animations
    u64 rocketTicks = 0;   // firework rocket trails emitted up to this tick
    ui::Atlas fontAtlas;
    std::unique_ptr<ui::Font> font;
    ui::Canvas canvas;
    bool loaded = false;

    Game* game = nullptr;
    ui::Ui* ui = nullptr;
    ItemRules* rules = nullptr;
    std::string playerName;
    std::function<int()> gameMode;
    bool ambientNight = false, ambientNether = false;
    int skyDarken = 0;

    // GPU
    Renderer* renderer = nullptr;
    bool gpuTried = false, gpuOk = false;
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> psoCut, psoTr, psoGlint, psoTextSee, psoText, psoPartCut, psoPartTr;
    // Shader pipeline: with the motion target (World / AfterTranslucent stages) and shadow casters.
    ComPtr<ID3D12PipelineState> psoCutMv, psoTrMv, psoGlintMv, psoPartCutMv, psoShadow;
    // Block cracks: multiplied into the colour; with the shader pipeline alpha blended into the entity layer.
    ComPtr<ID3D12PipelineState> psoCrumble, psoCrumbleMv;
    ComPtr<ID3D12PipelineState> psoAdd;  // BlendFunction.LIGHTNING: src alpha + dst (lightning bolts)
    ComPtr<ID3D12PipelineState> psoOutline;  // silhouettes into the entity outline target (no depth)
    ComPtr<ID3D12Resource> store, upload, itemTex, fontTex;
    u8* mapped = nullptr;
    u32 descTable = UINT32_MAX;  // items, font
    StagePass passes[4];

    // Per frame.
    u64 frameCounter = 0;
    u32 frameIndex = UINT32_MAX;
    u8* frameBase = nullptr;
    u64 stageUsed = 0;
    u32 instUsed = 0, dynUsed = 0;
    std::vector<MeshQuad> dyn;
    Lists worldLists, hudLists, shadowLists;  // shadowLists: casters outside the view (shadow maps only)
    std::vector<std::pair<u32, Lists>> glowLists;  // glowing frames / paintings / items by outline colour (0xRRGGBB)
    u64 updatedFrame = ~u64(0), preparedFrame = ~u64(0), handWorldFrame = ~u64(0);
    // Shader pipeline: particles are built in the World stage, where the opaque ones are drawn into the lit entity
    // layer (in the late LDR layer they were vanilla-lit and came out far too dark next to the HDR-lit terrain).
    u64 particleFrame = ~u64(0);
    u32 particleOpaque = 0, particleTotal = 0;
    // Motion vectors: last frame's transform of each keyed instance (camera-relative to that frame's camera).
    struct PrevInst {
        u64 frame;
        float m[12];
        double cam[3];
    };
    std::unordered_map<u64, PrevInst> prevInst;
    double frameCam[3] = {};
    bool prevHandValid = false;
    float prevHandVp[16] = {};  // last frame's hand projection (unjittered) when the hand was in the World stage
    List tagSee, tagNormal;
    List cracks;  // block destruction overlays (LevelRenderer crumbling), drawn after the World stage's translucent list
    // Lightning bolts (LightningBoltRenderer): additive quads in the AfterTranslucent stage. The shape's seed is the
    // client's own (LightningBolt.seed), re-rolled with each flash like LightningBolt.tick.
    List bolts;
    struct Bolt {
        u64 seed = 0;
        int life = 2, flashes = 1, age = -1;
    };
    std::unordered_map<int, Bolt> boltStates;
    u64 boltRandom = 0x2545F4914F6CDD1Dull;
    void buildBolts(const PassContext& ctx);
    // Particles of entity events and animate packets: TrackingEmitter (critical hits, totems) and the entities'
    // handleEntityEvent bursts (poof, hearts, villager moods, teleport portal).
    struct Emitter {
        int entity;
        const char* type;
        int life, lifetime;
    };
    std::vector<Emitter> emitters;
    std::mt19937 fxRandom{0xC0FFEEu};
    void entityEffect(const Entity& e, int event, bool animate);
    void mobTick(const Entity& e);  // the client-side particles / sounds of entity ticks (aiStep's isClientSide parts)
    std::unordered_map<int, u8> wasOnGround;  // AbstractCubeMob.wasOnGround (landing particles)
    std::unordered_map<int, u8> wasInWater;   // Entity.wasTouchingWater (the splash on entering)
    int activationTicks = 0, activationItem = -1;  // LocalPlayer.itemActivation (the totem popping): ticks left, item
    float activationOff[2] = {};
    bool localWasInWater = false, localFirstTick = true;
    // Entity.doWaterSplashEffect's particles and spawnSprintParticle (world lock held).
    void waterAndSprint(const double pos[3], const double vel[3], float width, bool inWater, bool& wasIn, bool sprinting);
    std::unordered_map<int, int> stunnedTicks;  // Ravager.stunnedTick (event 39)
    std::unordered_map<int, std::pair<u8, int>> pandaState;  // was sneezing, the last EAT_COUNTER seen
    std::unordered_map<int, std::pair<int, int>> dragonPhase;  // the client's DragonPhaseInstance and its tick counter
    void dragonTick(const Entity& e);
    struct StareState { i64 creepy = -1, staredAt = 0, lastSound = INT64_MIN / 2; };
    std::unordered_map<int, StareState> stareState;  // Enderman DATA_CREEPY / DATA_STARED_AT as last seen, lastStareSound
    std::unordered_map<int, std::pair<int, u64>> stateStart;  // pose / sniffer state and the age it began (animation times)
    std::unordered_map<int, std::pair<int, int>> breezeState;  // Breeze.jumpTrailStartedTick, soundTick
    // LivingEntity.breakItem: the item's break sound and five shards from the eyes (item: network id).
    void breakItem(int item, const double pos[3], double eyeHeight, float yaw, float pitch, bool silent, audio::Category category);
    bool emit(Emitter& em);  // TrackingEmitter.tick; false when the emitter is done
    void tickEmitters();
    bool crackDraw = false;
    int crackRects[2][10] = {{-2, -2, -2, -2, -2, -2, -2, -2, -2, -2}, {-2, -2, -2, -2, -2, -2, -2, -2, -2, -2}};
    void buildCracks(const PassContext& ctx);
    void buildFlames(const double cam[3], float pt);
    void buildBeams(const double cam[3], float pt);
    void crystalBeam(const double start[3], const double delta[3], float time, u32 light, const double cam[3]);
    std::unordered_map<int, int> dragonCrystal;  // EnderDragon.nearestCrystal (the client's own, checkCrystals)
    double lastTime = 0;
    float partial = 0;
    int glintRect = -1;

    // Local player (FirstPersonHandsAndItems, swing, view bobbing, the third-person avatar).
    struct HandState {
        int item = -1;
        i32 dye = -1;
        bool foil = false;
        std::string key;  // count and components but damage (ItemStack.matchesIgnoringComponents(ignoreSwapAnimation))
        float height = 0, oHeight = 0;
    } mainHand, offHand;
    int itemSwapTicker = 0, lastMainItem = -2, lastSwapResets = 0;
    bool swinging = false;
    int swingTime = 0, swingTicks = 6;  // swingTicks: this swing's duration (the weapon's attack_animation)
    int swingHand = 0, swingType = 1;   // the swinging hand (0 main, 1 off) and its SwingAnimationType
    float attackAnim = 0, oAttackAnim = 0;
    float xBob = 0, yBob = 0, xBobO = 0, yBobO = 0, walkDist = 0, walkDistO = 0, bob = 0, bobO = 0;
    double tickClock = 0;
    double lastTickPos[3] = {};
    bool haveTickPos = false;
    Entity avatar;
    bool avatarReady = false;
    std::vector<i8> skullCache;  // item id -> 1 skull, 0 not, -1 unknown
    std::vector<const ItemMesh*> skullMeshes;  // item id -> worn skull model
    const ItemMesh* frameMeshes[4] = {};       // item / glow frame, without / with a map
    // Text quads in the static store (name tags, map decoration names): background quad, then the glyphs.
    struct Tag {
        u32 first = UINT32_MAX, glyphs = 0;
        float width = 0;
    };
    // Maps in item frames: the map image in the item atlas and its quad (MapRenderer), by map id.
    struct MapTex {
        int ax = -1, ay = -1;
        u32 version = ~0u, quad = UINT32_MAX;
    };
    std::unordered_map<int, MapTex> mapTextures;
    const MapTex* mapTexture(int id);  // nullptr: no map data (yet)
    // MapRenderer.render after the map image: decoration sprites (frameOnly: those item frames show) and their names,
    // as dynamic quads; `im` maps the map's 0..128 space.
    void pushMapDecorations(List& cut, List& tr, const Xf& im, int mapId, bool frameOnly, u32 light);
    std::unordered_map<std::string, Tag> mapLabels;  // decoration names (component NBT) -> text quads
    const Tag& mapLabel(const std::string& nbt);
    std::vector<Xf> armMats;                   // scratch: arm part matrices

    // Name tags: text quads per name in the static store.
    std::unordered_map<std::string, Tag> tags;
    // Text displays: the laid out text per entity (background quad first, then the glyphs, font units), drawn from
    // the per-frame quads; rebuilt when the text, line width or style changes.
    struct TextBlock {
        u32 version = 0;
        bool valid = false;
        float width = 0, height = 0;
        std::vector<MeshQuad> quads;
    };
    std::unordered_map<int, TextBlock> textBlocks;
    const TextBlock& textBlock(int id, const DisplayState& s);

    // Paintings: painting_variant network id -> asset / size.
    struct PaintingInfo {
        std::string asset;
        int w = 1, h = 1;
        bool known = false;
        const ItemMesh* mesh = nullptr;
    };
    std::vector<PaintingInfo> paintings, inlinePaintings;

    // Timing history (exit report): CPU per frame over all stages, particles alive.
    static constexpr u32 kHistory = 512;
    float historyMs[kHistory] = {}, historyParticleMs[kHistory] = {};
    u32 historyParticles[kHistory] = {}, historyItems[kHistory] = {}, historyTags[kHistory] = {};
    u64 historyFrames = 0;
    double frameMs = 0, frameParticleMs = 0;
    void report() const;

    Impl(Visuals& v, EntityWorld& w, EntityRenderer& r) : self(v), world(w), er(r), assets(w.assets()) {
        er.outlineExtra = [this](const PassContext& ctx) { drawOutline(ctx); };
    }
    ~Impl() { report(); }
    Stats& stats() { return self.stats_; }

    // --- GPU ---------------------------------------------------------------------------------------------
    bool createGpu(Renderer& r);
    void uploadPending(const PassContext& ctx);
    void beginFrame(const PassContext& ctx);
    void writeCb(int slot, const PassContext& ctx, const float* viewProj, bool pipeline, bool hand,
                 const float* prevViewProj = nullptr);
    bool flush(List& l, bool hand = false);  // hand: camera-attached (no camera translation between frames)
    void prepareWorld(const PassContext& ctx);  // World stage lists (+ the hand with the pipeline), uploaded
    bool worldDraw[3] = {}, handDraw[3] = {}, shadowDraw[2] = {};
    void drawShadow(const ShadowContext& sc);
    void bindCommon(const PassContext& ctx);
    void drawList(const PassContext& ctx, ID3D12PipelineState* pso, const List& l, int cb);
    void drawOutline(const PassContext& ctx);

    // --- CPU ---------------------------------------------------------------------------------------------
    void update();
    void tickLocal();
    void buildWorld(const PassContext& ctx);
    void buildTags(const PassContext& ctx);
    // jittered: drawn in the World stage (shader pipeline: the projection jitter; noJitter receives the plain one).
    void buildHand(const PassContext& ctx, float viewProj[16], bool jittered = false, float* noJitter = nullptr);
    void pushItem(Lists& L, const ItemMesh& mesh, Display d, const Xf& m, u32 light, bool foil);
    void pushRaw(Lists& L, const ItemMesh& mesh, const Xf& m, u32 light, u32 color = 0xFFFFFFFF);
    u32 lightAt(double x, double y, double z) const;
    bool isSkull(int item);
    const Tag& tag(const std::string& name);
    const Tag& tag(const ui::Text& text);  // styled (team colours), cached by its spans
    const PaintingInfo* painting(int id);
    void heldStack(int slot, int& item, i32& dye, bool& foil, std::string* key = nullptr) const;
    int chargeOf(int menuSlot) const;
    // Item model contexts (ItemModelResolver.updateForLiving / updateForNonLiving): the stack an entity shows (its
    // whole stack when the packets carried one) and the entity as the item's owner.
    ui::ItemStack plainScratch;
    const ui::ItemStack& stackOf(const std::shared_ptr<const ui::ItemStack>& full, int item, i32 dye);
    ui::ItemContext contextOf(const Entity& e, ui::ItemDisplay display, float pt) const;
    void updateItemOwner(float pt);  // ui::itemWorld().player and the local player's per-stack state
    net::Client* client = nullptr;
    // Block entity items (campfire / shelf / vault / brushed items, player heads with profiles): their stacks from
    // the block entity NBT (cached by the NBT), this frame's list.
    std::unordered_map<std::string, ui::ItemStack> nbtStacks;
    std::vector<be::BlockEntityRenderer::ShownItem> shownItems;
    void buildBlockEntityItems(const double cam[3]);

    // --- shader packs (PassContext::pack): lists and particles as vanilla-style vertices -----------------
    std::vector<MeshQuad> cpuStore;  // CPU copy of the static quad store (packs pull quads on the CPU)
    std::vector<fx::GpuParticle> packParticleBuf;
    u64 packHandFrame = ~u64(0);     // frame whose first-person hand went to the pack (the Hud pass skips it)
    // glint: the item glint texture with the vanilla glint transform of the mesh-space position (PackBlend::Glint).
    void packList(PackSink& pack, const List& l, PackRole role, PackBlend blend, bool glint = false, bool hand = false);
    void packParticles(PackSink& pack, const PassContext& ctx, const fx::GpuParticle* pts, u32 count, bool translucent);
};

// ---------------------------------------------------------------------------------------------------------

Visuals::Visuals(EntityWorld& world, EntityRenderer& entityRenderer)
    : impl_(std::make_unique<Impl>(*this, world, entityRenderer)) {}
Visuals::~Visuals() = default;

bool Visuals::load(const fs::path& jar, const fs::path& minecraftDir, const GameReports& reports,
                   const fs::path& componentReports) {
    Timer t;
    Impl& m = *impl_;
    m.res = std::make_unique<ui::Resources>();
    if (!m.res->open(jar, minecraftDir)) {
        LOG("visuals: client resources unavailable");
        return false;
    }
    m.items = std::make_unique<ui::Items>();
    if (m.items->init(reports, *m.res)) EntityWorld::setItemDecoder(m.items.get());
    std::vector<std::string> names(size_t(std::max(0, m.items->count())));
    for (int i = 0; i < m.items->count(); ++i) names[size_t(i)] = m.items->name(i);
    m.models.init(*m.res, m.assets, names, componentReports);
    m.models.setItems(m.items.get());
    ui::setItemComponentReports(componentReports);
    m.er.setItemModels(&m.models);
    m.glintRect = m.models.texture("misc/enchanted_glint_item");
    m.fontAtlas.init(1024, 1024);
    m.font = std::make_unique<ui::Font>();
    if (!m.font->load(*m.res, m.fontAtlas)) m.font.reset();
    m.particles = std::make_unique<fx::Particles>(m.world.world(), m.world.registry(), reports, m.models);
    m.particles->init(*m.res);
    m.particles->readItem = [items = m.items.get()](net::Reader& r, int& item) {  // ItemParticleOption
        ui::ItemStack stack;
        const bool ok = items->readTemplate(r, stack);
        item = stack.item;
        return ok;
    };
    m.world.readParticle = [p = m.particles.get()](net::Reader& r, std::string& raw) { return p->readEncoded(r, raw); };
    m.world.paintingSize = [&m](int variant, int& w, int& h) {
        const Impl::PaintingInfo* p = m.painting(variant);
        if (!p) return false;
        w = p->w, h = p->h;
        return true;
    };
    m.world.spawnParticle = [p = m.particles.get()](const std::string& raw, double x, double y, double z, double vx, double vy, double vz) {
        p->spawnEncoded(raw, x, y, z, vx, vy, vz);
    };
    m.world.onEntityEvent = [&m](const Entity& e, int event) {
        m.entityEffect(e, event, false);
        // FireworkRocketEntity.handleEntityEvent: 17 explodes
        if (event != 17 || !m.particles || !e.info || e.info->name != "minecraft:firework_rocket") return;
        std::shared_lock lock(m.world.world().mutex());
        m.particles->fireworks(e.pos, e.vel, e.fireworks, !e.silent);
    };
    m.world.onAnimate = [&m](const Entity& e, int action) { m.entityEffect(e, action, true); };
    m.particles->entityPosition = [&world = m.world](int id, double pos[3]) {  // vibrations travelling to an entity
        const Entity* e = world.entity(id);
        if (e) std::copy(e->pos, e->pos + 3, pos);
        return e != nullptr;
    };
    m.particles->onElderGuardian = [&world = m.world] { world.mobAppearances.push_back(nowSeconds()); };
    m.particles->nearestPlayer = [&m](const double at[3], double maxDist, double out[4]) {
        double best = maxDist * maxDist;
        bool found = false;
        auto consider = [&](const double pos[3], double yd) {
            const double dx = pos[0] - at[0], dy = pos[1] - at[1], dz = pos[2] - at[2], d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best) best = d2, found = true, out[0] = pos[0], out[1] = pos[1], out[2] = pos[2], out[3] = yd;
        };
        if (m.game && !(m.gameMode && m.gameMode() == 3)) consider(m.game->player().pos, m.game->player().vel[1]);
        for (const auto& [id, e] : m.world.entities())
            if (e.category == Category::Player) consider(e.pos, e.vel[1]);
        return found;
    };
    m.loaded = true;
    LOG("visuals %7.1f ms  item models, font and particles from the game", t.ms());
    return true;
}

void Visuals::attach(net::Client& client) {
    impl_->client = &client;
    if (impl_->particles) impl_->particles->attach(client);
}

bool Visuals::addPasses(Renderer& renderer) {
    Impl& m = *impl_;
    if (!m.loaded) return false;
    const PassStage stages[4] = {PassStage::Sky, PassStage::World, PassStage::AfterTranslucent, PassStage::Hud};
    for (int i = 0; i < 4; ++i) {
        m.passes[i].impl = &m;
        m.passes[i].stage = i;
        if (!renderer.addPass(&m.passes[i], stages[i])) return false;
    }
    return true;
}

void Visuals::setAmbientEnv(bool night, bool nether, int skyDarken) {
    impl_->ambientNight = night, impl_->ambientNether = nether, impl_->skyDarken = skyDarken;
}

void Visuals::setParticleSources(std::function<bool(std::string_view, std::string_view)> blockTag, std::function<u32(int, int)> foliage) {
    if (!impl_->particles) return;
    impl_->particles->blockTag = std::move(blockTag);
    impl_->particles->foliageColor = std::move(foliage);
}

void Visuals::setWeatherSources(std::function<int(int, int, int)> precipitation, std::function<int(int, int)> motionBlocking) {
    if (!impl_->particles) return;
    impl_->particles->precipitationAt = std::move(precipitation);
    impl_->particles->motionBlocking = std::move(motionBlocking);
}

void Visuals::setItemRules(ItemRules* rules) {
    impl_->rules = rules;
    impl_->er.setRules(rules);
}

void Visuals::setPlayer(Game& game, ui::Ui& ui, const std::string& name, std::function<int()> gameMode) {
    Impl& m = *impl_;
    m.game = &game;
    // PotentSulfurBlockEntity.LAUNCH_ENTITY_TICKER for the local player (its movement is ours): a push up while in an
    // erupting geyser's column, until it rises fast enough.
    game.blockEntityTick = [&m](Player& p) {
        if (!m.particles || p.noClip || p.flying || m.game->vehicleId >= 0 || m.game->dead) return;
        const AABB body = p.box();
        for (const fx::Particles::GeyserColumn& c : m.particles->geyserColumns()) {
            const AABB col{{double(c.x), double(c.y0), double(c.z)}, {double(c.x + 1), double(c.y0 + c.height), double(c.z + 1)}};
            if (!body.intersects(col)) continue;
            if (p.vel[1] < 0.3 + c.waterBlocks * 0.1) p.vel[1] += 0.2;
        }
    };
    m.ui = &ui;
    ui.setItemModels(&m.models);
    m.playerName = name;
    m.gameMode = std::move(gameMode);
    game.onItemUsed = [&m](int hand) { (hand == 0 ? m.mainHand : m.offHand).height = 0; };  // itemUsed
    game.onSwing = [&m](int hand, int type) {  // LivingEntity.swing: SwingState.startIfAble (not in a swing's first half)
        if (!m.swinging || m.swingTime > m.swingTicks / 2 || m.swingTime <= 0)
            m.swingTime = -1, m.swinging = true, m.swingTicks = std::max(m.game->swingTicks, 1), m.swingHand = hand,
            m.swingType = type;
    };
    game.onBlockChanged = [&m, prev = std::move(game.onBlockChanged)](bool place, int x, int y, int z, u16 state) {
        if (prev) prev(place, x, y, z, state);
        if (!place && m.particles) {  // Block.playerWillDestroy -> level event 2001, played locally
            std::shared_lock lock(m.world.world().mutex());
            m.particles->blockDestroyed(x, y, z, state);
        }
    };
    game.onParticle = [&m](std::string_view type, const double pos[3], const double vel[3]) {
        if (!m.particles) return;
        std::shared_lock lock(m.world.world().mutex());
        m.particles->spawn(type, pos[0], pos[1], pos[2], vel[0], vel[1], vel[2]);
    };
    game.onColoredParticle = [&m](std::string_view type, const double pos[3], u32 argb) {
        if (!m.particles) return;
        std::shared_lock lock(m.world.world().mutex());
        m.particles->spawnColored(type, pos[0], pos[1], pos[2], 0, 0, 0, argb);
    };
    game.onLevelEvent = [&m](int type, int x, int y, int z, int data) {
        if (!m.particles) return;
        std::shared_lock lock(m.world.world().mutex());
        m.particles->localLevelEvent(type, x, y, z, data);
    };
    game.onBlockParticle = [&m](u16 state, const double pos[3], const double vel[3]) {
        if (!m.particles) return;
        std::shared_lock lock(m.world.world().mutex());
        m.particles->spawnBlock("block", m.world.registry().name(state), pos[0], pos[1], pos[2], vel[0], vel[1], vel[2]);
    };
    game.onRedstoneOreParticles = [&m](int x, int y, int z) {
        if (!m.particles) return;
        std::shared_lock lock(m.world.world().mutex());
        m.particles->redstoneOre(x, y, z);
    };
    game.onItemParticle = [&m](int item, const double pos[3], const double vel[3]) {  // eating: item shards
        if (!m.particles) return;
        std::shared_lock lock(m.world.world().mutex());
        m.particles->itemShard(item, pos[0], pos[1], pos[2], vel[0], vel[1], vel[2]);
    };
    // ClientLevel.addBreakingBlockEffects when the player starts breaking a block: a shard off the hit face.
    game.onBreakingEffects = [&m, prev = std::move(game.onBreakingEffects)](int x, int y, int z, int face, bool sound) {
        if (prev) prev(x, y, z, face, sound);
        if (m.particles) {
            std::shared_lock lock(m.world.world().mutex());
            m.particles->blockCrack(x, y, z, face);
        }
    };
}

bool StagePass::create(Renderer& r) { return impl->createGpu(r); }

void StagePass::prepare(const PassContext& ctx) {
    Visuals::Impl& m = *impl;
    if (!m.gpuOk || ctx.pack) return;  // shader packs: everything in draw
    m.beginFrame(ctx);
    Timer t;
    if (stage == 0) {
        m.update();
        m.updatedFrame = m.frameCounter;
        m.stats().updateMs = t.ms();
    } else if (stage == 1) {
        m.prepareWorld(ctx);
    }
    m.frameMs += t.ms();
}

void StagePass::drawShadow(const ShadowContext& sc) {
    if (stage == 1 && impl->gpuOk) impl->drawShadow(sc);
}

void StagePass::draw(const PassContext& ctx) {
    Visuals::Impl& m = *impl;
    if (!m.gpuOk) return;
    m.beginFrame(ctx);
    Timer t;
    struct Acc {
        Visuals::Impl& m;
        Timer& t;
        ~Acc() { m.frameMs += t.ms(); }
    } acc{m, t};
    switch (stage) {
        case 0:
            if (m.updatedFrame == m.frameCounter) break;  // done in prepare
            m.update();
            m.stats().updateMs = t.ms();
            break;
        case 1: {
            if (ctx.pack) {  // shader pack: items through gbuffers_entities, the first-person hand through gbuffers_hand
                m.worldLists.clear(), m.shadowLists.clear();
                m.buildWorld(ctx);
                m.uploadPending(ctx);
                m.packList(*ctx.pack, m.worldLists.cut, PackRole::Entities, PackBlend::Opaque);
                m.packList(*ctx.pack, m.worldLists.tr, PackRole::EntitiesTranslucent, PackBlend::Translucent);
                m.packList(*ctx.pack, m.worldLists.glint, PackRole::ArmorGlint, PackBlend::Glint, true);
                m.cracks.clear();
                m.buildCracks(ctx);
                m.uploadPending(ctx);
                m.packList(*ctx.pack, m.cracks, PackRole::EntitiesTranslucent, PackBlend::Translucent);
                m.hudLists.clear();
                float vp[16];
                m.buildHand(ctx, vp);
                m.uploadPending(ctx);
                m.packList(*ctx.pack, m.hudLists.cut, PackRole::Hand, PackBlend::Opaque);
                m.packList(*ctx.pack, m.hudLists.tr, PackRole::HandTranslucent, PackBlend::Translucent);
                m.packList(*ctx.pack, m.hudLists.glint, PackRole::ArmorGlint, PackBlend::Glint, true, true);
                m.packHandFrame = m.frameCounter;
                m.stats().worldMs = t.ms();
                break;
            }
            if (m.preparedFrame != m.frameCounter) m.prepareWorld(ctx);
            m.bindCommon(ctx);
            const bool mv = ctx.motionRtv.ptr != 0;  // shader pipeline: motion vectors into the second target
            if (mv) {
                const D3D12_CPU_DESCRIPTOR_HANDLE rts[2] = {ctx.colorRtv, ctx.motionRtv};
                ctx.list->OMSetRenderTargets(2, rts, FALSE, &ctx.dsv);
            }
            if (m.worldDraw[0]) m.drawList(ctx, (mv ? m.psoCutMv : m.psoCut).Get(), m.worldLists.cut, 0);
            if (m.worldDraw[1]) m.drawList(ctx, (mv ? m.psoTrMv : m.psoTr).Get(), m.worldLists.tr, 0);
            if (m.worldDraw[2]) m.drawList(ctx, (mv ? m.psoGlintMv : m.psoGlint).Get(), m.worldLists.glint, 0);
            if (m.crackDraw) m.drawList(ctx, (mv ? m.psoCrumbleMv : m.psoCrumble).Get(), m.cracks, 0);
            if (m.handWorldFrame == m.frameCounter && mv) {
                // First-person hand as part of the scene (lit, anti-aliased, motion vectors for TAA / upscalers /
                // frame generation): own depth range in front of the world, like the Hud stage below.
                D3D12_VIEWPORT vpHand{0, 0, float(ctx.width), float(ctx.height), 0.9f, 1.0f};
                ctx.list->RSSetViewports(1, &vpHand);
                if (m.handDraw[0]) m.drawList(ctx, m.psoCutMv.Get(), m.hudLists.cut, 2);
                if (m.handDraw[1]) m.drawList(ctx, m.psoTrMv.Get(), m.hudLists.tr, 2);
                if (m.handDraw[2]) m.drawList(ctx, m.psoGlintMv.Get(), m.hudLists.glint, 2);
                D3D12_VIEWPORT full{0, 0, float(ctx.width), float(ctx.height), 0.0f, 1.0f};
                ctx.list->RSSetViewports(1, &full);
            }
            if (m.particles && ctx.shading && mv) {
                const double cam[3] = {ctx.camX, ctx.camY, ctx.camZ};
                auto* out = reinterpret_cast<fx::GpuParticle*>(m.frameBase + kParticleOffset);
                m.particleTotal = m.particles->build(out, kMaxParticles, cam, m.particleOpaque);
                m.frameParticleMs += m.particles->stats().buildMs;
                m.particleFrame = m.frameCounter;
                m.uploadPending(ctx);
                if (m.particleOpaque) {
                    const D3D12_GPU_VIRTUAL_ADDRESS va = m.upload->GetGPUVirtualAddress() + u64(ctx.frame) * kFrameBytes;
                    ctx.list->SetGraphicsRootConstantBufferView(0, va);  // the World stage's (pipeline) constants
                    const u32 c[4] = {0, 0, 0, 0};
                    ctx.list->SetPipelineState(m.psoPartCutMv.Get());
                    ctx.list->SetGraphicsRoot32BitConstants(1, 4, c, 0);
                    ctx.list->DrawInstanced(m.particleOpaque * 6, 1, 0, 0);
                }
            }
            if (mv) ctx.list->OMSetRenderTargets(1, &ctx.colorRtv, FALSE, &ctx.dsv);
            m.stats().worldMs = t.ms();
            break;
        }
        case 2: {
            m.tagSee.clear(), m.tagNormal.clear(), m.bolts.clear();
            m.buildTags(ctx);
            m.buildBolts(ctx);
            // Particles.
            const double cam[3] = {ctx.camX, ctx.camY, ctx.camZ};
            u32 opaque = 0, total = 0;
            if (m.particles && ctx.pack) {  // shader pack: particles through gbuffers_particles / textured
                m.packParticleBuf.resize(kMaxParticles);
                total = m.particles->build(m.packParticleBuf.data(), kMaxParticles, cam, opaque);
                m.frameParticleMs += m.particles->stats().buildMs;
                m.packParticles(*ctx.pack, ctx, m.packParticleBuf.data(), opaque, false);
                m.packParticles(*ctx.pack, ctx, m.packParticleBuf.data() + opaque, total - opaque, true);
                opaque = total = 0;  // nothing left for the GPU path below
            } else if (m.particles && m.particleFrame == m.frameCounter) {  // built (opaque drawn) in the World stage
                opaque = m.particleOpaque, total = m.particleTotal;
            } else if (m.particles) {
                auto* out = reinterpret_cast<fx::GpuParticle*>(m.frameBase + kParticleOffset);
                total = m.particles->build(out, kMaxParticles, cam, opaque);
                m.frameParticleMs += m.particles->stats().buildMs;
            }
            m.uploadPending(ctx);
            m.writeCb(1, ctx, ctx.viewProj, false, false);
            const bool a = m.flush(m.tagSee), b = m.flush(m.tagNormal), bolt = m.flush(m.bolts);
            m.bindCommon(ctx);
            ID3D12GraphicsCommandList* list = ctx.list;
            const D3D12_GPU_VIRTUAL_ADDRESS va = m.upload->GetGPUVirtualAddress() + u64(ctx.frame) * kFrameBytes;
            list->SetGraphicsRootConstantBufferView(0, va + kCbBytes);
            auto particleDraw = [&](ID3D12PipelineState* pso, u32 first, u32 count) {
                if (!count) return;
                const u32 c[4] = {first, 0, 0, 0};
                list->SetPipelineState(pso);
                list->SetGraphicsRoot32BitConstants(1, 4, c, 0);
                list->DrawInstanced(count * 6, 1, 0, 0);
            };
            if (m.particleFrame == m.frameCounter) {
                // opaque particles were drawn in the World stage
            } else if (ctx.motionRtv.ptr) {  // shader pipeline: opaque particles write their motion
                const D3D12_CPU_DESCRIPTOR_HANDLE rts[2] = {ctx.colorRtv, ctx.motionRtv};
                list->OMSetRenderTargets(2, rts, FALSE, &ctx.dsv);
                particleDraw(m.psoPartCutMv.Get(), 0, opaque);
                list->OMSetRenderTargets(1, &ctx.colorRtv, FALSE, &ctx.dsv);
            } else {
                particleDraw(m.psoPartCut.Get(), 0, opaque);
            }
            particleDraw(m.psoPartTr.Get(), opaque, total - opaque);
            if (bolt) m.drawList(ctx, m.psoAdd.Get(), m.bolts, 1);
            if (a) m.drawList(ctx, m.psoTextSee.Get(), m.tagSee, 1);
            if (b) m.drawList(ctx, m.psoText.Get(), m.tagNormal, 1);
            m.stats().particles = total;
            m.stats().afterMs = t.ms();
            break;
        }
        case 3: {
            if (m.packHandFrame == m.frameCounter || m.handWorldFrame == m.frameCounter) {  // drawn in the World stage
                m.stats().hudMs = t.ms();
                break;
            }
            m.hudLists.clear();
            float vp[16];
            m.buildHand(ctx, vp);
            if (m.hudLists.cut.v.empty() && m.hudLists.tr.v.empty()) {
                m.stats().hudMs = t.ms();
                break;
            }
            m.uploadPending(ctx);
            m.writeCb(2, ctx, vp, false, true);
            const bool a = m.flush(m.hudLists.cut), b = m.flush(m.hudLists.tr), c = m.flush(m.hudLists.glint);
            m.bindCommon(ctx);
            // Own depth range: the hand is always in front of the world (FirstPersonHandsAndItemsRenderer draws
            // after clearing depth; reversed Z puts it in [0.9, 1] here, above any world depth).
            D3D12_VIEWPORT vpHand{0, 0, float(ctx.width), float(ctx.height), 0.9f, 1.0f};
            ctx.list->RSSetViewports(1, &vpHand);
            if (a) m.drawList(ctx, m.psoCut.Get(), m.hudLists.cut, 2);
            if (b) m.drawList(ctx, m.psoTr.Get(), m.hudLists.tr, 2);
            if (c) m.drawList(ctx, m.psoGlint.Get(), m.hudLists.glint, 2);
            D3D12_VIEWPORT full{0, 0, float(ctx.width), float(ctx.height), 0.0f, 1.0f};
            ctx.list->RSSetViewports(1, &full);
            m.stats().hudMs = t.ms();
            break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// GPU

bool Visuals::Impl::createGpu(Renderer& r) {
    if (gpuTried) return gpuOk;
    gpuTried = true;
    renderer = &r;
    ID3D12Device* device = r.device();
    if (er.atlasDescriptor() == UINT32_MAX) {
        LOG("visuals: entity atlas unavailable");
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV, ranges[0].NumDescriptors = 2, ranges[0].BaseShaderRegister = 4;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV, ranges[1].NumDescriptors = 1, ranges[1].BaseShaderRegister = 6;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV, ranges[2].NumDescriptors = 1, ranges[2].BaseShaderRegister = 7;
    D3D12_ROOT_PARAMETER params[9]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.Num32BitValues = 4;
    for (int i = 0; i < 4; ++i) {
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2 + i].Descriptor.ShaderRegister = UINT(i);
        params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    for (int i = 0; i < 3; ++i) {
        params[6 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[6 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[6 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[6 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_STATIC_SAMPLER_DESC samp[2]{};
    samp[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp[0].AddressU = samp[0].AddressV = samp[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp[0].MaxLOD = D3D12_FLOAT32_MAX;
    samp[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samp[1] = samp[0];
    samp[1].Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    samp[1].AddressU = samp[1].AddressV = samp[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp[1].ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 9;
    rd.pParameters = params;
    rd.NumStaticSamplers = 2;
    rd.pStaticSamplers = samp;
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        LOG("visuals: root signature: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    if (!check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig)),
               "CreateRootSignature"))
        return false;
    enum Blend { Opaque, Alpha, Glint, Crumble, Add };
    // targets: 1 colour; 2 colour + motion (written); 3 colour + motion (kept); 0 shadow map (depth only)
    auto pso = [&](const void* vs, size_t vsSize, const void* ps, size_t psSize, Blend blend, bool depthTest, bool depthWrite,
                   ComPtr<ID3D12PipelineState>& out, int targets = 1) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = rootSig.Get();
        d.VS = {vs, vsSize};
        d.PS = {ps, psSize};
        auto& rt = d.BlendState.RenderTarget[0];
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (blend == Alpha) {
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_SRC_ALPHA, rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA, rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE, rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA, rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        } else if (blend == Glint) {  // GLINT_TRANSPARENCY: src colour * src colour + dst (alpha untouched)
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_SRC_COLOR, rt.DestBlend = D3D12_BLEND_ONE, rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ZERO, rt.DestBlendAlpha = D3D12_BLEND_ONE, rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        } else if (blend == Crumble) {  // CRUMBLING: dst colour * src + src colour * dst (alpha untouched)
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_DEST_COLOR, rt.DestBlend = D3D12_BLEND_SRC_COLOR, rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ZERO, rt.DestBlendAlpha = D3D12_BLEND_ONE, rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        } else if (blend == Add) {  // LIGHTNING: src colour * src alpha + dst (alpha untouched: additive in a premultiplied layer)
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_SRC_ALPHA, rt.DestBlend = D3D12_BLEND_ONE, rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ZERO, rt.DestBlendAlpha = D3D12_BLEND_ONE, rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.DepthStencilState.DepthEnable = depthTest ? TRUE : FALSE;
        d.DepthStencilState.DepthWriteMask = depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reversed Z
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = targets == 0 ? 0 : targets == 1 || targets == 4 ? 1 : 2;
        d.RTVFormats[0] = targets ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_UNKNOWN;
        if (targets == 2 || targets == 3) {
            d.RTVFormats[1] = RenderPass::kMotionFormat;
            d.BlendState.IndependentBlendEnable = TRUE;
            d.BlendState.RenderTarget[1].RenderTargetWriteMask = targets == 2 ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
        }
        if (targets == 0) {  // shadow caster: depth only, no depth clip, slope bias (thin quads, no culling)
            d.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
            d.RasterizerState.DepthClipEnable = FALSE;
            d.RasterizerState.SlopeScaledDepthBias = 1.5f;
            d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        }
        d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        if (targets == 4) d.DSVFormat = DXGI_FORMAT_UNKNOWN;  // the outline target, no depth
        d.SampleDesc.Count = 1;
        return check(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)), "CreateGraphicsPipelineState");
    };
#define SH(n) g_##n, sizeof(g_##n)
    if (!pso(SH(VisualsVS), SH(VisualsCutoutPS), Opaque, true, true, psoCut) ||
        !pso(SH(VisualsVS), SH(VisualsTranslucentPS), Alpha, true, false, psoTr) ||
        !pso(SH(VisualsVS), SH(VisualsGlintPS), Glint, true, false, psoGlint) ||
        !pso(SH(VisualsVS), SH(VisualsTextPS), Alpha, false, false, psoTextSee) ||
        !pso(SH(VisualsVS), SH(VisualsTextPS), Alpha, true, false, psoText) ||
        !pso(SH(ParticleVS), SH(ParticleCutoutPS), Opaque, true, true, psoPartCut) ||
        !pso(SH(ParticleVS), SH(ParticleTranslucentPS), Alpha, true, false, psoPartTr) ||
        !pso(SH(VisualsVS), SH(VisualsCutoutMvPS), Opaque, true, true, psoCutMv, 2) ||
        !pso(SH(VisualsVS), SH(VisualsTranslucentPS), Alpha, true, false, psoTrMv, 3) ||
        !pso(SH(VisualsVS), SH(VisualsGlintPS), Glint, true, false, psoGlintMv, 3) ||
        !pso(SH(VisualsVS), SH(VisualsCrumblingPS), Crumble, true, false, psoCrumble) ||
        !pso(SH(VisualsVS), SH(VisualsCrumblingPS), Alpha, true, false, psoCrumbleMv, 3) ||
        !pso(SH(VisualsVS), SH(VisualsTranslucentPS), Add, true, false, psoAdd) ||
        !pso(SH(ParticleVS), SH(ParticleCutoutMvPS), Opaque, true, true, psoPartCutMv, 2) ||
        !pso(SH(VisualsShadowVS), SH(VisualsShadowPS), Opaque, true, true, psoShadow, 0) ||
        !pso(SH(VisualsVS), SH(VisualsOutlinePS), Opaque, false, false, psoOutline, 4))
        return false;
#undef SH
    auto buffer = [&](u64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp{heap};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = size;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(&out)),
                     "CreateCommittedResource");
    };
    if (!buffer(u64(ItemModels::kMaxQuads) * sizeof(MeshQuad), D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, store) ||
        !buffer(kFrameBytes * Renderer::kFrames, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, upload))
        return false;
    D3D12_RANGE none{0, 0};
    if (!check(upload->Map(0, &none, reinterpret_cast<void**>(&mapped)), "Map")) return false;
    auto texture = [&](ui::Atlas& a, ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = u32(a.width()), td.Height = u32(a.height());
        td.DepthOrArraySize = 1, td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        if (!check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&out)),
                   "CreateCommittedResource(texture)") ||
            !r.uploadTexture(out.Get(), {a.data()}))
            return false;
        int y0, y1;
        while (a.takeDirty(y0, y1, 1 << 16)) {}
        return true;
    };
    if (!texture(models.atlas(), itemTex) || !texture(fontAtlas, fontTex)) return false;
    descTable = r.allocDescriptors(2);
    if (descTable == UINT32_MAX) {
        LOG("visuals: no free descriptors");
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(itemTex.Get(), &sd, r.cpuDescriptor(descTable));
    device->CreateShaderResourceView(fontTex.Get(), &sd, r.cpuDescriptor(descTable + 1));
    dyn.reserve(kMaxDynamic);
    for (List* l : {&worldLists.cut, &worldLists.tr, &worldLists.glint, &hudLists.cut, &hudLists.tr, &hudLists.glint, &tagSee, &tagNormal,
                    &shadowLists.cut, &shadowLists.tr})
        l->v.reserve(1024);
    gpuOk = true;
    er.setItemEntitiesExternal(true);  // dropped items as 3D item models from now on
    return true;
}

void Visuals::Impl::report() const {
    const u32 n = u32(std::min<u64>(historyFrames, kHistory));
    if (!n) return;
    std::vector<float> v(historyMs, historyMs + n);
    std::sort(v.begin(), v.end());
    double sum = 0, psum = 0, parts = 0, items = 0, tagsN = 0;
    for (u32 i = 0; i < n; ++i)
        sum += historyMs[i], psum += historyParticleMs[i], parts += historyParticles[i], items += historyItems[i], tagsN += historyTags[i];
    LOG("visuals: last %u frames: CPU %.3f ms avg, p99 %.3f, max %.3f (particles sim+build %.3f ms) | %.0f particles, %.0f "
        "items, %.0f name tags per frame",
        n, sum / n, v[std::min<size_t>(n - 1, size_t(0.99 * (n - 1) + 0.5))], v[n - 1], psum / n, parts / n, items / n, tagsN / n);
}

void Visuals::Impl::beginFrame(const PassContext& ctx) {
    if (frameIndex == ctx.frame && frameBase) return;
    if (frameBase && frameCounter > 2) {  // close the previous frame's timing
        const u32 h = u32(historyFrames % kHistory);
        historyMs[h] = float(frameMs), historyParticleMs[h] = float(frameParticleMs);
        historyParticles[h] = self.stats_.particles, historyItems[h] = self.stats_.items, historyTags[h] = self.stats_.tags;
        ++historyFrames;
    }
    frameMs = 0, frameParticleMs = 0;
    frameIndex = ctx.frame;
    frameBase = mapped + u64(ctx.frame) * kFrameBytes;
    stageUsed = 0, instUsed = 0, dynUsed = 0;
    dyn.clear();
    ++frameCounter;
}

// New mesh quads (static store), item atlas and font rows: copied from this frame's staging area.
void Visuals::Impl::uploadPending(const PassContext& ctx) {
    ID3D12GraphicsCommandList* list = ctx.list;
    u8* stage = frameBase + kStageOffset;
    const u64 stageVa = u64(ctx.frame) * kFrameBytes + kStageOffset;
    auto barrier = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from, b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
    };
    const MeshQuad* data;
    u32 count, first;
    if (models.takePending(data, count, first)) {
        const u64 bytes = u64(count) * sizeof(MeshQuad);
        if (stageUsed + bytes <= kStageBytes) {
            std::memcpy(stage + stageUsed, data, bytes);
            barrier(store.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(store.Get(), u64(first) * sizeof(MeshQuad), upload.Get(), stageVa + stageUsed, bytes);
            barrier(store.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            stageUsed = (stageUsed + bytes + 511) & ~u64(511);
            if (cpuStore.size() < size_t(first) + count) cpuStore.resize(size_t(first) + count);
            std::memcpy(cpuStore.data() + first, data, bytes);
            models.releasePending();
        }
    }
    auto rows = [&](ui::Atlas& a, ID3D12Resource* tex) {
        const u64 pitch = u64(a.width()) * 4;
        int y0, y1;
        bool any = false;
        while (stageUsed + pitch <= kStageBytes && a.takeDirty(y0, y1, int((kStageBytes - stageUsed) / pitch))) {
            if (!any) barrier(tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            any = true;
            for (int y = y0; y < y1; ++y) std::memcpy(stage + stageUsed + u64(y - y0) * pitch, a.data() + u64(y) * pitch, pitch);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = tex;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = upload.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = stageVa + stageUsed;
            src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, u32(a.width()), u32(y1 - y0), 1, u32(pitch)};
            list->CopyTextureRegion(&dst, 0, u32(y0), 0, &src, nullptr);
            stageUsed = (stageUsed + u64(y1 - y0) * pitch + 511) & ~u64(511);
        }
        if (any) barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    };
    rows(models.atlas(), itemTex.Get());
    rows(fontAtlas, fontTex.Get());
}

void Visuals::Impl::writeCb(int slot, const PassContext& ctx, const float* viewProj, bool pipeline, bool hand,
                            const float* prevViewProj) {
    FrameCb k{};
    if (const float* pv = prevViewProj ? prevViewProj : ctx.prevViewProj) std::memcpy(k.prevViewProj, pv, sizeof(k.prevViewProj));
    std::memcpy(k.motion, ctx.motionScale, sizeof(k.motion));
    if (ctx.shadowMats) std::memcpy(k.shadowMat, ctx.shadowMats, sizeof(k.shadowMat));
    std::memcpy(k.viewProj, viewProj, sizeof(k.viewProj));
    std::memcpy(k.lightmap, ctx.lightmap, sizeof(k.lightmap));
    std::memcpy(k.fogColor, ctx.fogColor, sizeof(k.fogColor));
    k.fogParams[0] = ctx.fogStart, k.fogParams[1] = ctx.fogEnd, k.fogParams[2] = ctx.daylight;
    k.fogRender[0] = ctx.fogRenderStart, k.fogRender[1] = ctx.fogRenderEnd;
    k.fogParams[3] = pipeline ? 1.0f : 0.0f;
    const float l0[3] = {0.2f, 1.0f, -0.7f}, l1[3] = {-0.2f, 1.0f, 0.7f};
    const float n0 = 1.0f / std::sqrt(0.04f + 1.0f + 0.49f);
    for (int a = 0; a < 3; ++a) k.light0[a] = l0[a] * n0, k.light1[a] = l1[a] * n0;
    auto vp = [&](int r, int c) { return ctx.viewProj[c * 4 + r]; };
    for (int r = 0; r < 2; ++r) {  // camera right / up from the view-projection rows
        float l = 0;
        for (int c = 0; c < 3; ++c) l += vp(r, c) * vp(r, c);
        l = std::sqrt(std::max(l, 1e-12f));
        for (int c = 0; c < 3; ++c) (r == 0 ? k.camRight : k.camUp)[c] = vp(r, c) / l;
    }
    if (glintRect >= 0) {
        const ItemModels::Rect& g = models.rect(glintRect);
        k.glintRect[0] = g.u0, k.glintRect[1] = g.v0, k.glintRect[2] = g.du, k.glintRect[3] = g.dv;
    }
    // TextureTransform.setupGlintTexturing: offsets scrolling with time (the Glint Speed).
    const double ms = nowSeconds() * 1000.0 * 8.0 * glintOptions().speed;
    k.glint[0] = -float(std::fmod(ms, 110000.0) / 110000.0), k.glint[1] = float(std::fmod(ms, 30000.0) / 30000.0);
    k.glint[2] = glintRect >= 0 ? glintOptions().strength : 0.0f;
    k.glint[3] = hand ? 1.0f : 0.0f;
    std::memcpy(frameBase + kCbBytes * u64(slot), &k, sizeof(k));
}

// World stage lists (items, frames, paintings, held items), their shadow-only casters and, with the shader pipeline,
// the first-person hand; instances and dynamic quads uploaded (prepare, so the shadow maps can use them).
void Visuals::Impl::prepareWorld(const PassContext& ctx) {
    preparedFrame = frameCounter;
    frameCam[0] = ctx.camX, frameCam[1] = ctx.camY, frameCam[2] = ctx.camZ;
    worldLists.clear(), shadowLists.clear(), cracks.clear();
    glowLists.clear();
    buildWorld(ctx);
    buildCracks(ctx);
    const bool handInWorld = (ctx.shading || ctx.handInScene) && ctx.prevViewProj;
    float handVp[16], handPlain[16];
    if (handInWorld) {
        hudLists.clear();
        buildHand(ctx, handVp, true, handPlain);
    }
    uploadPending(ctx);
    writeCb(0, ctx, ctx.viewProj, ctx.shading, false);
    worldDraw[0] = flush(worldLists.cut), worldDraw[1] = flush(worldLists.tr), worldDraw[2] = flush(worldLists.glint);
    bool glow = false;
    for (auto& [color, gl] : glowLists) glow |= flush(gl.cut) | flush(gl.tr);
    if (glow) writeCb(3, ctx, ctx.viewProjNoJitter ? ctx.viewProjNoJitter : ctx.viewProj, false, false);
    er.outlineExtraActive = glow;
    crackDraw = flush(cracks);
    shadowDraw[0] = flush(shadowLists.cut), shadowDraw[1] = flush(shadowLists.tr);
    if (handInWorld && !(hudLists.cut.v.empty() && hudLists.tr.v.empty())) {
        writeCb(2, ctx, handVp, ctx.shading, true, prevHandValid ? prevHandVp : handPlain);
        handDraw[0] = flush(hudLists.cut, true), handDraw[1] = flush(hudLists.tr, true), handDraw[2] = flush(hudLists.glint, true);
        handWorldFrame = frameCounter;
        std::memcpy(prevHandVp, handPlain, sizeof(prevHandVp));
        prevHandValid = true;
    } else {
        prevHandValid = false;
    }
    if (frameCounter % 256 == 0)  // forget instances not drawn for a while
        for (auto it = prevInst.begin(); it != prevInst.end();) it = it->second.frame + 8 < frameCounter ? prevInst.erase(it) : std::next(it);
}

// Shadow casters: items, frames, paintings, held items (World lists and the shadow-only ones), alpha-tested.
void Visuals::Impl::drawShadow(const ShadowContext& sc) {
    if (preparedFrame != frameCounter) return;
    const List* lists[4] = {&worldLists.cut, &worldLists.tr, &shadowLists.cut, &shadowLists.tr};
    const bool ok[4] = {worldDraw[0], worldDraw[1], shadowDraw[0], shadowDraw[1]};
    if (!ok[0] && !ok[1] && !ok[2] && !ok[3]) return;
    PassContext pc{};
    pc.list = sc.list, pc.frame = sc.frame, pc.blockTextures = sc.blockTextures;
    bindCommon(pc);
    ID3D12GraphicsCommandList* list = sc.list;
    const D3D12_GPU_VIRTUAL_ADDRESS va = upload->GetGPUVirtualAddress() + u64(sc.frame) * kFrameBytes;
    list->SetGraphicsRootConstantBufferView(0, va);
    list->SetPipelineState(psoShadow.Get());
    for (int i = 0; i < 4; ++i) {
        if (!ok[i]) continue;
        const u32 c[4] = {lists[i]->base, u32(lists[i]->v.size()), 0, sc.cascade};
        list->SetGraphicsRoot32BitConstants(1, 4, c, 0);
        list->DrawInstanced(lists[i]->quads * 6, 1, 0, 0);
    }
}

// Copies a list into the slice's instance array with prefix sums; false when empty or out of room.
bool Visuals::Impl::flush(List& l, bool hand) {
    if (l.v.empty()) return false;
    if (instUsed + l.v.size() > kMaxInstances) return false;
    u32 prefix = 0;
    for (size_t i = 0; i < l.v.size(); ++i) {
        GpuInst& g = l.v[i];
        g.prefix = prefix, prefix += g.count;
        // Last frame's transform of the same instance, moved into this frame's camera space (motion vectors).
        if (const u64 key = i < l.keys.size() ? l.keys[i] : 0) {
            PrevInst& pi = prevInst[key];
            if (pi.frame + 1 == frameCounter) {
                std::memcpy(g.prev, pi.m, sizeof(g.prev));
                if (!hand)
                    for (int a = 0; a < 3; ++a) g.prev[a * 4 + 3] += float(pi.cam[a] - frameCam[a]);
            }
            pi.frame = frameCounter;
            std::memcpy(pi.m, g.m, sizeof(pi.m));
            std::copy(frameCam, frameCam + 3, pi.cam);
        }
    }
    l.base = instUsed;
    l.quads = prefix;
    std::memcpy(frameBase + kInstOffset + u64(instUsed) * sizeof(GpuInst), l.v.data(), l.v.size() * sizeof(GpuInst));
    instUsed += u32(l.v.size());
    self.stats_.instances += u32(l.v.size());
    return prefix > 0;
}

void Visuals::Impl::bindCommon(const PassContext& ctx) {
    ID3D12GraphicsCommandList* list = ctx.list;
    // Dynamic quads written so far this frame.
    if (!dyn.empty()) {
        const u32 n = u32(std::min<size_t>(dyn.size(), kMaxDynamic));
        if (n > dynUsed) {
            std::memcpy(frameBase + kDynOffset + u64(dynUsed) * sizeof(MeshQuad), dyn.data() + dynUsed, u64(n - dynUsed) * sizeof(MeshQuad));
            dynUsed = n;
        }
    }
    const D3D12_GPU_VIRTUAL_ADDRESS va = upload->GetGPUVirtualAddress() + u64(ctx.frame) * kFrameBytes;
    ID3D12DescriptorHeap* heaps[] = {renderer->descriptorHeap()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(rootSig.Get());
    list->SetGraphicsRootShaderResourceView(2, store->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(3, va + kDynOffset);
    list->SetGraphicsRootShaderResourceView(4, va + kInstOffset);
    list->SetGraphicsRootShaderResourceView(5, va + kParticleOffset);
    list->SetGraphicsRootDescriptorTable(6, renderer->gpuDescriptor(descTable));
    list->SetGraphicsRootDescriptorTable(7, renderer->gpuDescriptor(er.atlasDescriptor()));
    list->SetGraphicsRootDescriptorTable(8, ctx.blockTextures);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// EntityRenderer's outline pass: the glowing frames / paintings / items as silhouettes in their outline colour.
void Visuals::Impl::drawOutline(const PassContext& ctx) {
    if (preparedFrame != frameCounter) return;
    bindCommon(ctx);
    for (const auto& [color, gl] : glowLists)
        for (const List* l : {&gl.cut, &gl.tr}) {
            if (!l->quads) continue;
            const D3D12_GPU_VIRTUAL_ADDRESS va = upload->GetGPUVirtualAddress() + u64(ctx.frame) * kFrameBytes;
            ctx.list->SetGraphicsRootConstantBufferView(0, va + kCbBytes * 3);
            const u32 c[4] = {l->base, u32(l->v.size()), color, 0};
            ctx.list->SetPipelineState(psoOutline.Get());
            ctx.list->SetGraphicsRoot32BitConstants(1, 4, c, 0);
            ctx.list->DrawInstanced(l->quads * 6, 1, 0, 0);
        }
}

void Visuals::Impl::drawList(const PassContext& ctx, ID3D12PipelineState* pso, const List& l, int cb) {
    ID3D12GraphicsCommandList* list = ctx.list;
    const D3D12_GPU_VIRTUAL_ADDRESS va = upload->GetGPUVirtualAddress() + u64(ctx.frame) * kFrameBytes;
    list->SetGraphicsRootConstantBufferView(0, va + kCbBytes * u64(cb));
    const u32 c[4] = {l.base, u32(l.v.size()), 0, 0};
    list->SetPipelineState(pso);
    list->SetGraphicsRoot32BitConstants(1, 4, c, 0);
    list->DrawInstanced(l.quads * 6, 1, 0, 0);
}

// ---------------------------------------------------------------------------------------------------------
// Helpers

u32 Visuals::Impl::lightAt(double x, double y, double z) const {
    const World& w = world.world();
    const int bx = int(std::floor(x)), by = int(std::floor(y)), bz = int(std::floor(z));
    if (by < w.minSectionY() * 16 || by >= (w.maxSectionY() + 1) * 16) return 0xF0;
    return w.light(bx >> 4, by >> 4, bz >> 4).at(bx & 15, by & 15, bz & 15);
}

bool Visuals::Impl::isSkull(int item) {
    if (item < 0) return false;
    if (skullCache.size() <= size_t(item)) skullCache.resize(size_t(item) + 1, -1);
    i8& c = skullCache[size_t(item)];
    if (c < 0) {
        std::string layer, tex;
        c = ItemModels::skullLayer(models.itemName(item), layer, tex) ? 1 : 0;
    }
    return c == 1;
}

void Visuals::Impl::pushItem(Lists& L, const ItemMesh& mesh, Display d, const Xf& m, u32 light, bool foil) {
    for (int i = 0; i < mesh.partCount; ++i) {
        const ItemMesh::Part& p = mesh.parts[i];
        const Xf x = xfMul(m, p.display[size_t(d)]);
        const u64 k = L.next();
        L.cut.add(x, p.first, p.count, light, 0xFFFFFFFF, 0, k);
        L.tr.add(x, p.tfirst, p.tcount, light, 0xFFFFFFFF, 0, k);
        if (foil) L.glint.add(x, p.first, p.count, light, 0xFFFFFFFF, 0, k), L.glint.add(x, p.tfirst, p.tcount, light, 0xFFFFFFFF, 0, k ? k ^ 128 : 0);
    }
    ++self.stats_.items;
}

void Visuals::Impl::pushRaw(Lists& L, const ItemMesh& mesh, const Xf& m, u32 light, u32 color) {
    for (int i = 0; i < mesh.partCount; ++i) {
        const ItemMesh::Part& p = mesh.parts[i];
        const u64 k = L.next();
        L.cut.add(m, p.first, p.count, light, color, 0, k);
        L.tr.add(m, p.tfirst, p.tcount, light, color, 0, k);
    }
}

void Visuals::Impl::heldStack(int slot, int& item, i32& dye, bool& foil, std::string* key) const {
    item = -1, dye = -1, foil = false;
    if (key) key->clear();
    if (!ui) return;
    if (const ui::ItemStack* s = ui->playerSlot(slot)) {
        item = s->item;
        dye = s->dyedColor >= 0 ? i32(s->dyedColor & 0xFFFFFF) : -1;
        foil = items && items->foil(*s);
        if (key && !s->empty()) {  // the count and the components that play the swap animation (all but damage)
            static const int damage = items ? items->componentType("damage") : -1;
            *key = std::to_string(s->count);
            for (const auto& c : s->added)
                if (c.type != damage) *key += "|" + std::to_string(c.type) + ":" + c.data;
            for (int t : s->removed)
                if (t != damage) *key += "|-" + std::to_string(t);
        }
    }
}

// CampfireRenderer (FIXED), ShelfRenderer (ON_SHELF, the model's bounding box aligned), VaultRenderer and
// BrushableBlockRenderer items with their whole stacks, and SkullBlockRenderer player heads with their profile's skin.
namespace {
// ItemEntityRenderer.submitMultipleFromCount: the copies' offsets come from a LegacyRandomSource seeded with
// ItemClusterRenderState.getSeedForItemStack (the item's id plus its damage); next() is nextFloat() * 2 - 1.
struct ClusterRandom {
    u64 seed;
    explicit ClusterRandom(i64 s) : seed((u64(s) ^ 0x5DEECE66Dull) & ((1ull << 48) - 1)) {}
    float next() {
        seed = (seed * 0x5DEECE66Dull + 0xBull) & ((1ull << 48) - 1);
        return float(seed >> 24) / float(1 << 24) * 2.0f - 1.0f;
    }
};
}  // namespace

void Visuals::Impl::buildBlockEntityItems(const double cam[3]) {
    const be::BlockEntityRenderer* ber = be::BlockEntityRenderer::instance();
    if (!ber || !items) return;
    shownItems.clear();
    ber->shownItems(shownItems);
    if (nbtStacks.size() > 4096) nbtStacks.clear();
    for (const be::BlockEntityRenderer::ShownItem& si : shownItems) {
        auto it = nbtStacks.find(*si.nbt);
        if (it == nbtStacks.end()) {
            ui::ItemStack st;
            if (!ui::itemStackFromNbt(*items, *si.nbt, st)) st = ui::ItemStack{};
            it = nbtStacks.emplace(*si.nbt, std::move(st)).first;
        }
        const ui::ItemStack& st = it->second;
        if (st.empty()) continue;
        Xf m;
        std::copy(si.m, si.m + 12, m.m);
        for (int a = 0; a < 3; ++a) m.m[a * 4 + 3] += float(double(si.origin[a]) - cam[a]);
        worldLists.begin((u64(u32(si.origin[0] * 31 + si.origin[2])) << 20 ^ u64(u32(si.origin[1])) << 4 ^ u64(si.seed)) | 3);
        if (si.kind == 1) {  // a placed player head
            pushRaw(worldLists, models.headMesh(st), m, si.light);
            continue;
        }
        ui::ItemContext c;
        c.display = si.context == 0 ? ui::ItemDisplay::Fixed : si.context == 1 ? ui::ItemDisplay::OnShelf : ui::ItemDisplay::Ground;
        c.seed = si.seed;
        const ItemMesh& mesh = models.mesh(st, c);
        if (mesh.empty()) continue;
        const Display d = si.context == 0 ? Display::Fixed : si.context == 1 ? Display::OnShelf : Display::Ground;
        if (si.context == 1) {  // ShelfRenderer: bottom (or centre) of the model's box at the slot
            float offsetY = -mesh.shelfMinY;
            if (!si.shelfBottom) offsetY -= (mesh.shelfMaxY - mesh.shelfMinY) / 2.0f;
            m = xfMul(m, xfTranslate(0, offsetY, 0));
        }
        const bool foil = items->foil(st);
        if (si.context != 2) {
            pushItem(worldLists, mesh, d, m, si.light, foil);
            continue;
        }
        // VaultRenderer: ItemEntityRenderer.renderMultipleFromCount (copies by the stack's count, like dropped items).
        const int amount = st.count > 48 ? 5 : st.count > 32 ? 4 : st.count > 16 ? 3 : st.count > 1 ? 2 : 1;
        ClusterRandom random(i64(st.item) + st.damage);
        auto rnd = [&] { return random.next(); };
        if (mesh.groundDepth > 0.0625f) {
            pushItem(worldLists, mesh, d, m, si.light, foil);
            for (int i = 1; i < amount; ++i)
                pushItem(worldLists, mesh, d, xfMul(m, xfTranslate(rnd() * 0.15f, rnd() * 0.15f, rnd() * 0.15f)), si.light, foil);
        } else {
            const float offsetZ = mesh.groundDepth * 1.5f;
            m = xfMul(m, xfTranslate(0, 0, -(offsetZ * float(amount - 1) / 2.0f)));
            pushItem(worldLists, mesh, d, m, si.light, foil);
            m = xfMul(m, xfTranslate(0, 0, offsetZ));
            for (int i = 1; i < amount; ++i) {
                pushItem(worldLists, mesh, d, xfMul(m, xfTranslate(rnd() * 0.075f, rnd() * 0.075f, 0)), si.light, foil);
                m = xfMul(m, xfTranslate(0, 0, offsetZ));
            }
        }
    }
}

void Visuals::Impl::pushMapDecorations(List& cut, List& tr, const Xf& im, int mapId, bool frameOnly, u32 light) {
    const std::vector<ui::MapDecor>* decorations = ui::mapDecorations(mapId);
    if (!decorations) return;
    int count = 0;
    for (const ui::MapDecor& d : *decorations) {
        bool onFrame = false;
        const char* sprite = ui::mapDecorationSprite(d.type, &onFrame);
        if (!sprite || (frameOnly && !onFrame)) continue;
        const int rect = models.texture(sprite);
        if (rect >= 0 && dyn.size() < kMaxDynamic) {
            Xf m = xfMul(im, xfTranslate(float(d.x) / 2.0f + 64.0f, float(d.y) / 2.0f + 64.0f, -0.02f));
            m = xfMul(m, xfRotZ(float(d.rot) * 22.5f * kDeg));
            m = xfMul(m, xfScale(4.0f, 4.0f, 3.0f));
            m = xfMul(m, xfTranslate(-0.125f, 0.125f, 0));
            const ItemModels::Rect& r = models.rect(rect);
            const float z = float(count) * -0.001f;
            MeshQuad q{};
            const float pos[4][3] = {{-1, 1, z}, {1, 1, z}, {1, -1, z}, {-1, -1, z}};
            const float uv[4][2] = {{r.u0, r.v0}, {r.u0 + r.du, r.v0}, {r.u0 + r.du, r.v0 + r.dv}, {r.u0, r.v0 + r.dv}};
            std::memcpy(q.pos, pos, sizeof(pos)), std::memcpy(q.uv, uv, sizeof(uv));
            q.normal = packNormal(0, 0, -1), q.color = 0xFFFFFFFF, q.tex = kTexItems, q.flags = kQuadNoShade;
            const u32 first = u32(dyn.size());
            dyn.push_back(q);
            cut.add(m, first, 1, light, 0xFFFFFFFF, 1);
        }
        if (!d.name.empty()) {  // the name centred below, at most 6 / 9 scale, on the text background
            const Tag& t = mapLabel(d.name);
            if (t.first != UINT32_MAX && t.width > 0) {
                const float sc = std::clamp(25.0f / t.width, 0.0f, 6.0f / 9.0f);
                Xf m = xfMul(im, xfTranslate(float(d.x) / 2.0f + 64.0f - t.width * sc / 2.0f, float(d.y) / 2.0f + 64.0f + 4.0f, -0.025f));
                m = xfMul(m, xfScale(sc, sc, -1.0f));
                m = xfMul(m, xfTranslate(0, 0, 0.1f));
                tr.add(m, t.first, 1, light, 0x80000000u, 0);
                tr.add(m, t.first + 1, t.glyphs, light, 0xFFFFFFFF, 0);
            }
        }
        ++count;
    }
}

// A map decoration's name as text quads (font units, left aligned at 0; the background quad first, then the glyphs).
const Visuals::Impl::Tag& Visuals::Impl::mapLabel(const std::string& nbt) {
    auto it = mapLabels.find(nbt);
    if (it != mapLabels.end()) return it->second;
    Tag t;
    if (font && mapLabels.size() < 1024) {
        canvas.begin(1u << 14, 1u << 14, 1);
        const ui::Text text = ui::parseComponent(nbt, &res->lang());
        t.width = font->widthF(text);
        font->draw(canvas, text, 0, 0, 0xFFFFFFFF, false);
        std::vector<MeshQuad> quads;
        MeshQuad bg{};  // Font background: x - 1 .. x + width, y - 1 .. y + 9, behind the glyphs
        const float corners[4][2] = {{-1, -1}, {-1, 9}, {t.width, 9}, {t.width, -1}};
        for (int k = 0; k < 4; ++k) bg.pos[k][0] = corners[k][0], bg.pos[k][1] = corners[k][1], bg.pos[k][2] = -0.01f;  // Font: the background at -0.01 (UNDER_EFFECT_DEPTH)
        bg.normal = packNormal(0, 0, -1), bg.color = 0xFFFFFFFF, bg.tex = kTexSolid, bg.flags = kQuadNoShade;
        quads.push_back(bg);
        const auto& v = canvas.vertices();
        for (size_t k = 0; k + 5 < v.size(); k += 6) {
            const ui::UiVertex* c[4] = {&v[k], &v[k + 1], &v[k + 2], &v[k + 5]};
            MeshQuad q{};
            for (int j = 0; j < 4; ++j) {
                q.pos[j][0] = c[j]->x, q.pos[j][1] = c[j]->y, q.pos[j][2] = 0;
                q.uv[j][0] = c[j]->u, q.uv[j][1] = c[j]->v;
            }
            q.normal = packNormal(0, 0, -1);
            q.color = c[0]->color;
            q.tex = c[0]->mode == ui::ModeSolid ? kTexSolid : kTexFont;
            q.flags = kQuadNoShade;
            quads.push_back(q);
        }
        t.first = models.addQuads(quads.data(), u32(quads.size()));
        t.glyphs = u32(quads.size() - 1);
    }
    return mapLabels.emplace(nbt, t).first->second;
}

const ui::ItemStack& Visuals::Impl::stackOf(const std::shared_ptr<const ui::ItemStack>& full, int item, i32 dye) {
    if (full && full->item == item) return *full;
    plainScratch = ItemModels::plainStack(item, dye);
    return plainScratch;
}

ui::ItemContext Visuals::Impl::contextOf(const Entity& e, ui::ItemDisplay display, float pt) const {
    ui::ItemContext c;
    c.display = display;
    if (e.id == EntityRenderer::kLocalPlayerId) {
        c.owner = ui::itemWorld().player;
        c.seed = ui::itemWorld().localId + int(display);
        return c;
    }
    ui::ItemOwner& o = c.owner;
    o.present = true;
    o.id = e.id;
    o.type = e.info ? e.info->name : std::string();
    o.living = e.category == Category::Living || e.category == Category::Player;
    o.player = e.category == Category::Player;
    o.local = false;
    o.viewEntity = e.id == world.viewEntity;
    for (int a = 0; a < 3; ++a) o.pos[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt;
    o.mainArm = e.mainArm;
    static const int frame = assets.typeId("minecraft:item_frame"), glowFrame = assets.typeId("minecraft:glow_item_frame");
    if (o.living) {  // LivingEntity.getVisualRotationYInDegrees: the body rotation
        o.yaw = e.prevBodyYaw + (e.bodyYaw - e.prevBodyYaw) * pt;
    } else if (e.type == frame || e.type == glowFrame) {  // ItemFrame.getVisualRotationYInDegrees
        static const int k2d[6] = {-1, -1, 2, 0, 1, 3};  // Direction 3D -> 2D data value (down / up: -1)
        const int dir = std::clamp(e.direction, 0, 5);
        const int correction = dir == 0 ? -90 : dir == 1 ? 90 : 0;
        o.yaw = wrapDegrees(float(180 + k2d[dir] * 90 + e.rotation * 45 + correction));
    } else if (e.category == Category::Item) {  // ItemEntity: 180 - getSpin(age + 0.5) in degrees
        o.yaw = 180.0f - ((float(e.age) + 0.5f) / 20.0f + e.bobOffset) / (2.0f * kPi) * 360.0f;
    } else {
        o.yaw = e.prevYaw + (e.yaw - e.prevYaw) * pt;
    }
    c.seed = o.living ? e.id + int(display) : e.id;
    return c;
}

void Visuals::Impl::updateItemOwner(float pt) {
    ui::ItemWorld& w = ui::itemWorld();
    ui::updateItemWorld();
    if (client && items && client->inPlay() && !items->registriesSet()) items->setRegistries(*client);
    ui::ItemOwner& o = w.player;
    if (!game || !avatarReady) {
        o.present = false;
        return;
    }
    o.present = true;
    o.living = o.player = o.local = true;
    o.id = w.localId;
    o.type = "minecraft:player";
    std::copy(avatar.pos, avatar.pos + 3, o.pos);
    o.yaw = avatar.prevBodyYaw + (avatar.bodyYaw - avatar.prevBodyYaw) * pt;
    o.mainArm = avatar.mainArm;
    o.viewEntity = world.viewEntity < 0 || world.viewEntity == w.localId;
    o.usingThis = o.fishing = o.selected = o.carried = false;  // per stack
    w.selectedSlot = 36 + game->selectedSlot();
    w.useSlot = game->use.active ? (game->use.hand ? 45 : w.selectedSlot) : -1;
    w.useTicks = float(game->use.ticksUsing());
    w.useRemaining = float(game->use.remaining);
    // FishingHookRenderer.getHoldingArm: the main hand when it holds a fishing rod, else the off hand; only while our
    // hook is out.
    w.fishingSlot = -1;
    static const int bobber = assets.typeId("minecraft:fishing_bobber");
    for (const auto& [id, e] : world.entities())
        if (e.type == bobber && e.direction == w.localId && w.localId >= 0) {
            int item;
            i32 dye;
            bool foil;
            heldStack(w.selectedSlot, item, dye, foil);
            w.fishingSlot = models.itemName(item) == "fishing_rod" ? w.selectedSlot : 45;
            break;
        }
    // Cooldown.get: getCooldownPercent(stack, 0): the fraction steps once a tick
    w.cooldown = [g = game, it = items.get()](const ui::ItemStack& s) { return g->cooldownPercent(it->cooldownGroup(s), 0.0f); };
}

const Visuals::Impl::PaintingInfo* Visuals::Impl::painting(int id) {
    if (id >= EntityWorld::kInlinePainting) {  // a definition sent with the entity
        const size_t k = size_t(id - EntityWorld::kInlinePainting);
        if (k >= world.inlinePaintings.size()) return nullptr;
        if (inlinePaintings.size() <= k) inlinePaintings.resize(k + 1);
        PaintingInfo& p = inlinePaintings[k];
        if (!p.known) {
            const EntityWorld::InlinePainting& d = world.inlinePaintings[k];
            p.known = true, p.asset = d.asset, p.w = d.w, p.h = d.h;
        }
        return &p;
    }
    if (id < 0 || id > 4096) return nullptr;
    if (paintings.size() <= size_t(id)) paintings.resize(size_t(id) + 1);
    PaintingInfo& p = paintings[size_t(id)];
    if (!p.known) {
        p.known = true;
        const auto& list = assets.variants("painting_variant");
        if (size_t(id) < list.size()) {
            const std::string path = "data/minecraft/painting_variant/" + list[size_t(id)].name + ".json";
            nlohmann::json j = res->json(path);
            if (!j.is_object()) {
                std::vector<u8> bytes;
                if (AssetSource::data().read(path, bytes)) j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
            }
            if (j.is_object()) {
                p.asset = j.value("asset_id", "minecraft:" + list[size_t(id)].name);
                p.w = std::clamp(j.value("width", 1), 1, 16), p.h = std::clamp(j.value("height", 1), 1, 16);
            } else {
                p.asset = list[size_t(id)].name;
            }
        }
    }
    return p.asset.empty() ? nullptr : &p;
}

const Visuals::Impl::Tag& Visuals::Impl::tag(const std::string& name) { return tag(ui::Text(name)); }

const Visuals::Impl::Tag& Visuals::Impl::tag(const ui::Text& text) {
    std::string name;  // the cache key: each span with its look
    for (const ui::Span& sp : text.spans)
        name += sp.text + '\x1f' + std::to_string(sp.style.color) + char('0' + sp.style.bold + 1) + char('0' + sp.style.italic + 1) +
                char('0' + sp.style.underlined + 1) + char('0' + sp.style.strikethrough + 1) + char('0' + sp.style.obfuscated + 1) + '\x1e';
    auto it = tags.find(name);
    if (it != tags.end()) return it->second;
    Tag t;
    if (font && tags.size() < 4096) {
        canvas.begin(1u << 14, 1u << 14, 1);
        t.width = font->widthF(text);
        const float x0 = -t.width / 2.0f;
        font->draw(canvas, text, x0, 0, 0xFFFFFFFF, false);
        std::vector<MeshQuad> quads;
        // Background (Font: x - 1 .. x + width, y - 1 .. y + 9), then the glyphs.
        MeshQuad bg{};
        const float bx0 = x0 - 1, bx1 = x0 + t.width, by0 = -1, by1 = 9;
        const float corners[4][2] = {{bx0, by0}, {bx0, by1}, {bx1, by1}, {bx1, by0}};
        for (int k = 0; k < 4; ++k) bg.pos[k][0] = corners[k][0], bg.pos[k][1] = corners[k][1], bg.pos[k][2] = 0.01f;  // behind the text (Font effect depth)
        bg.normal = packNormal(0, 0, -1), bg.color = 0xFFFFFFFF, bg.tex = kTexSolid, bg.flags = kQuadNoShade;
        quads.push_back(bg);
        const auto& v = canvas.vertices();
        for (size_t k = 0; k + 5 < v.size(); k += 6) {
            const ui::UiVertex* c[4] = {&v[k], &v[k + 1], &v[k + 2], &v[k + 5]};
            MeshQuad q{};
            for (int j = 0; j < 4; ++j) {
                q.pos[j][0] = c[j]->x, q.pos[j][1] = c[j]->y, q.pos[j][2] = 0;
                q.uv[j][0] = c[j]->u, q.uv[j][1] = c[j]->v;
            }
            q.normal = packNormal(0, 0, -1);
            q.color = c[0]->color;
            q.tex = c[0]->mode == ui::ModeSolid ? kTexSolid : kTexFont;
            q.flags = kQuadNoShade;
            quads.push_back(q);
        }
        t.first = models.addQuads(quads.data(), u32(quads.size()));
        t.glyphs = u32(quads.size() - 1);
    }
    return tags.emplace(name, t).first->second;
}

// MapTextureManager: the map's colours as a 128 x 128 image (refreshed with each update) and MapRenderer's quad
// (0..128 at z -0.01, map space).
const Visuals::Impl::MapTex* Visuals::Impl::mapTexture(int id) {
    u32 version = 0;
    const std::vector<u8>* colors = ui::mapColors(id, version);
    if (!colors || colors->size() < 128 * 128) return nullptr;
    MapTex& t = mapTextures[id];
    if (t.ax < 0) {
        ui::Atlas& atlas = models.atlas();
        if (!atlas.alloc(128, 128, t.ax, t.ay)) {
            mapTextures.erase(id);
            return nullptr;
        }
        const float W = float(atlas.width()), H = float(atlas.height());
        const float u0 = float(t.ax) / W, v0 = float(t.ay) / H, u1 = float(t.ax + 128) / W, v1 = float(t.ay + 128) / H;
        MeshQuad q{};
        const float corners[4][4] = {{0, 128, u0, v1}, {128, 128, u1, v1}, {128, 0, u1, v0}, {0, 0, u0, v0}};
        for (int k = 0; k < 4; ++k)
            q.pos[k][0] = corners[k][0], q.pos[k][1] = corners[k][1], q.pos[k][2] = -0.01f, q.uv[k][0] = corners[k][2],
            q.uv[k][1] = corners[k][3];
        q.normal = packNormal(0, 0, -1), q.color = 0xFFFFFFFF, q.tex = kTexItems, q.flags = kQuadNoShade;
        t.quad = models.addQuads(&q, 1);
    }
    if (t.quad == UINT32_MAX) return nullptr;
    if (t.version != version) {
        t.version = version;
        std::vector<u8> px(128 * 128 * 4);
        for (size_t i = 0; i < 128 * 128; ++i) {
            const u32 c = ui::mapColorRgba((*colors)[i]);
            std::memcpy(&px[i * 4], &c, 4);
        }
        models.atlas().blit(t.ax, t.ay, 128, 128, px.data(), 128 * 4);
    }
    return &t;
}

// TextDisplayRenderer.splitLines + submitInner's layout: lines wrapped at the line width, aligned within the widest
// line; the background covers (-1, -1) .. (width, height).
const Visuals::Impl::TextBlock& Visuals::Impl::textBlock(int id, const DisplayState& s) {
    TextBlock& b = textBlocks[id];
    if (b.valid && b.version == s.textVersion) return b;
    b = TextBlock{};
    b.valid = true, b.version = s.textVersion;
    if (!font) return b;
    const ui::Text text = s.text.empty() ? ui::Text() : ui::parseComponent(s.text, &res->lang());
    const std::vector<ui::Text> lines = font->split(text, s.lineWidth);
    std::vector<int> widths;
    int width = 0;
    for (const ui::Text& l : lines) widths.push_back(font->width(l)), width = std::max(width, widths.back());
    const int height = int(lines.size()) * 10 - 1;
    b.width = float(width), b.height = float(height);
    MeshQuad bg{};
    const float corners[4][2] = {{-1, -1}, {-1, float(height)}, {float(width), float(height)}, {float(width), -1}};
    for (int k = 0; k < 4; ++k) bg.pos[k][0] = corners[k][0], bg.pos[k][1] = corners[k][1], bg.pos[k][2] = -0.01f;  // behind
    bg.normal = packNormal(0, 0, 1), bg.color = 0xFFFFFFFF, bg.tex = kTexSolid, bg.flags = kQuadNoShade;
    b.quads.push_back(bg);
    canvas.begin(1u << 14, 1u << 14, 1);
    float y = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const float x = (s.style & 8) ? 0.0f : (s.style & 16) ? float(width - widths[i]) : float(width) / 2.0f - float(widths[i]) / 2.0f;
        font->draw(canvas, lines[i], x, y, 0xFFFFFFFF, (s.style & 1) != 0);
        y += 10;
    }
    const auto& v = canvas.vertices();
    for (size_t k = 0; k + 5 < v.size(); k += 6) {
        const ui::UiVertex* c[4] = {&v[k], &v[k + 1], &v[k + 2], &v[k + 5]};
        MeshQuad q{};
        for (int j = 0; j < 4; ++j) {
            q.pos[j][0] = c[j]->x, q.pos[j][1] = c[j]->y, q.pos[j][2] = 0;
            q.uv[j][0] = c[j]->u, q.uv[j][1] = c[j]->v;
        }
        q.normal = packNormal(0, 0, 1);
        q.color = c[0]->color;
        q.tex = c[0]->mode == ui::ModeSolid ? kTexSolid : kTexFont;
        q.flags = kQuadNoShade;
        b.quads.push_back(q);
    }
    return b;
}

// ---------------------------------------------------------------------------------------------------------
// Local player state (20 Hz)

void Visuals::Impl::tickLocal() {
    Player& p = game->player();
    // LivingEntity.SwingState.tick (the swing's duration: 6, or the weapon's attack_animation).
    oAttackAnim = attackAnim;
    if (swinging) {
        if (++swingTime >= swingTicks) swingTime = 0, swinging = false;
    } else {
        swingTime = 0;
    }
    attackAnim = float(swingTime) / float(swingTicks);
    // LocalPlayer.aiStep: hand sway follows the view.
    const float yawDeg = p.yaw / kDeg, pitchDeg = p.pitch / kDeg;
    xBobO = xBob, yBobO = yBob;
    xBob += (pitchDeg - xBob) * 0.5f;
    yBob += (yawDeg - yBob) * 0.5f;
    // Walk distance and bob (ClientAvatarState).
    double dist = 0;
    if (haveTickPos) {
        const double dx = p.pos[0] - lastTickPos[0], dz = p.pos[2] - lastTickPos[2];
        dist = std::sqrt(dx * dx + dz * dz);
        if (dist > 10) dist = 0;
    }
    std::copy(p.pos, p.pos + 3, lastTickPos);
    haveTickPos = true;
    walkDistO = walkDist;
    walkDist += float(dist) * 0.6f;  // LocalPlayer.move: addWalkedDistance, on the ground or not
    bobO = bob;
    // AbstractClientPlayer.updateBob: on the ground, alive and not swimming, the horizontal velocity
    const float tBob = p.onGround && !p.swimming && !game->dead ? std::min(0.1f, float(std::sqrt(p.vel[0] * p.vel[0] + p.vel[2] * p.vel[2]))) : 0.0f;
    bob += (tBob - bob) * 0.4f;
    // FirstPersonHandsAndItems.tick.
    int selected = game->selectedSlot();
    int nextMain, nextOff;
    i32 dyeMain, dyeOff;
    bool foilMain, foilOff;
    std::string keyMain, keyOff;
    heldStack(36 + selected, nextMain, dyeMain, foilMain, &keyMain);
    heldStack(45, nextOff, dyeOff, foilOff, &keyOff);
    ++itemSwapTicker;
    if (game->itemSwapResets != lastSwapResets) itemSwapTicker = 0, lastSwapResets = game->itemSwapResets;  // an attack
    if (nextMain != lastMainItem) {  // Player.tick: a different item resets the attack strength / swap tickers
        if (lastMainItem != -2) itemSwapTicker = 0;
        lastMainItem = nextMain;
    }
    mainHand.oHeight = mainHand.height, offHand.oHeight = offHand.height;
    // shouldInstantlyReplaceVisibleItem: the same stack but its damage, or an item without the swap animation
    auto sameVisible = [&](const HandState& h, int item, const std::string& key) {
        if (h.item == item && h.key == key) return true;
        return item >= 0 && !models.handInfo(item).swapAnimation;
    };
    auto show = [](HandState& h, int item, i32 dye, bool foil, const std::string& key) { h.item = item, h.dye = dye, h.foil = foil, h.key = key; };
    if (sameVisible(mainHand, nextMain, keyMain)) show(mainHand, nextMain, dyeMain, foilMain, keyMain);
    if (sameVisible(offHand, nextOff, keyOff)) show(offHand, nextOff, dyeOff, foilOff, keyOff);
    // Player.getItemSwapScale: over the live attack speed attribute's delay
    const float delay = game ? game->attackStrengthDelay() : 20.0f / 4.0f;
    const float swap = std::clamp((float(itemSwapTicker) + 1.0f) / delay, 0.0f, 1.0f);
    if (game->handsBusy) {  // FirstPersonHandsAndItems.tick: busy hands (paddling) go down
        mainHand.height = std::clamp(mainHand.height - 0.4f, 0.0f, 1.0f);
        offHand.height = std::clamp(offHand.height - 0.4f, 0.0f, 1.0f);
    } else {
        const float mainTarget = (mainHand.item != nextMain || mainHand.key != keyMain) ? 0.0f : swap * swap * swap;
        const float offTarget = (offHand.item != nextOff || offHand.key != keyOff) ? 0.0f : 1.0f;
        mainHand.height += std::clamp(mainTarget - mainHand.height, -0.4f, 0.4f);
        offHand.height += std::clamp(offTarget - offHand.height, -0.4f, 0.4f);
    }
    if (mainHand.height < 0.1f) show(mainHand, nextMain, dyeMain, foilMain, keyMain);
    if (offHand.height < 0.1f) show(offHand, nextOff, dyeOff, foilOff, keyOff);

    // Third-person avatar: LivingEntity body rotation and walk animation.
    Entity& e = avatar;
    e.prevBodyYaw = e.bodyYaw;
    e.walkSpeedOld = e.walkSpeed;
    const float target = std::min(float(dist) * 4.0f, 1.0f);
    e.walkSpeed += (target - e.walkSpeed) * 0.4f;
    e.walkPos += e.walkSpeed;
    float bodyTarget = e.bodyYaw;
    if (dist * dist > 0.0025000002) {
        const double dx = p.pos[0] - p.prev[0], dz = p.pos[2] - p.prev[2];
        const float walkDirection = float(std::atan2(dz, dx)) / kDeg - 90.0f;
        const float diff = std::abs(wrapDegrees(wrapDegrees(yawDeg) - walkDirection));
        bodyTarget = (95.0f < diff && diff < 265.0f) ? walkDirection - 180.0f : walkDirection;
    }
    if (swinging) bodyTarget = yawDeg;
    e.bodyYaw += wrapDegrees(bodyTarget - e.bodyYaw) * 0.3f;
    const float headDiff = wrapDegrees(yawDeg - e.bodyYaw);
    if (std::abs(headDiff) > 50.0f) e.bodyYaw += headDiff - (headDiff > 0 ? 50.0f : -50.0f);
    while (e.bodyYaw - e.prevBodyYaw < -180.0f) e.prevBodyYaw -= 360.0f;
    while (e.bodyYaw - e.prevBodyYaw >= 180.0f) e.prevBodyYaw += 360.0f;
    ++e.age;
    const double move[3] = {p.pos[0] - p.prev[0], p.pos[1] - p.prev[1], p.pos[2] - p.prev[2]};
    tickPoseAnimation(e, move, p.pose == Player::Pose::FallFlying, p.pose == Player::Pose::Crouching);
    // LocalPlayer: the cloak from the position before the move, the bob, the walked distance (LocalPlayer.move).
    const float horizontal = float(std::sqrt(move[0] * move[0] + move[2] * move[2]));
    // AbstractClientPlayer: no bob while dead or dying
    tickCloak(e, p.prev, p.onGround && !p.swimming && !(game && game->dead) ? std::min(0.1f, horizontal) : 0.0f);
    e.walkDist += horizontal * 0.6f;
}

void Visuals::Impl::update() {
    const double now = nowSeconds();
    const double dt = lastTime > 0 ? std::clamp(now - lastTime, 0.0, 0.25) : 0.0;
    lastTime = now;
    self.stats_.items = self.stats_.tags = self.stats_.instances = 0;
    if (game) {
        tickClock += dt;
        int n = 0;
        while (tickClock >= 0.05 && n < 5) tickClock -= 0.05, ++n, tickLocal(), tickEmitters();
        if (tickClock >= 0.05) tickClock = 0;
        partial = float(tickClock / 0.05);
        // The avatar for the third-person camera (drawn by the entity renderer this frame).
        Entity& e = avatar;
        if (!avatarReady) {
            const int type = assets.typeId("minecraft:player");
            e.type = type, e.info = assets.type(type), e.category = Category::Player, e.id = EntityRenderer::kLocalPlayerId;
            e.skinParts = 0x7F, e.mainArm = 1;
            avatarReady = e.info != nullptr;
        }
        const CameraState c = game->camera();
        const Player& p = game->player();
        e.pos[0] = c.x, e.pos[1] = c.y - p.eyeHeight(), e.pos[2] = c.z;
        std::copy(e.pos, e.pos + 3, e.prevPos);
        e.yaw = e.prevYaw = p.yaw / kDeg;
        e.headYaw = e.prevHeadYaw = e.yaw;
        e.pitch = e.prevPitch = p.pitch / kDeg;
        // Pose: crouching 5, swimming / crawling 3, gliding 1 (net.minecraft.world.entity.Pose).
        e.pose = p.pose == Player::Pose::Crouching ? 5 : p.pose == Player::Pose::Swimming ? 3 : p.pose == Player::Pose::FallFlying ? 1
                 : p.pose == Player::Pose::SpinAttack ? 4 : 0;
        e.sharedFlags = u8((p.pose == Player::Pose::FallFlying ? 0x80 : 0) | (ui && ui->onFire() ? 0x01 : 0));
        e.ticksFrozen = ui ? ui->ticksFrozen() : 0;
        e.inWater = p.inWater;
        e.bedFacing = -1;
        if (game->sleeping) {  // lying in the bed (setPosToBed)
            e.pose = 2;
            const int* b = game->bedPos;
            e.pos[0] = b[0] + 0.5, e.pos[1] = b[1] + 0.6875, e.pos[2] = b[2] + 0.5;
            std::copy(e.pos, e.pos + 3, e.prevPos);
            std::shared_lock lock(world.world().mutex());
            e.bedFacing = bedFacingAt(world.world(), world.registry(), b[0], b[1], b[2]);
        }
        e.vehicle = game->vehicleId;  // the sitting pose
        // The item use (third-person arm poses): LivingEntity flags and ticks using the item.
        e.livingFlags = u8((game->use.active ? 1 | (game->use.hand ? 2 : 0) : 0) | (p.spinFlag ? 4 : 0));
        e.useTicks = game->use.ticksUsing();
        e.chargedCrossbow = u8((chargeOf(36 + game->selectedSlot()) ? 1 : 0) | (chargeOf(45) ? 2 : 0));
        e.swingTime = swinging ? std::max(swingTime, 0) : -1;
        e.swingDuration = swingTicks;
        e.swingHand = swingHand, e.swingType = swingType;
        e.kineticHitTicks = game->kineticHitTicks;
        e.mainArm = ui ? ui->settings().mainHand : 1;
        e.arrowCount = world.self.arrowCount, e.stingerCount = world.self.stingerCount;  // our own synched data
        e.shoulderParrot[0] = world.self.shoulderParrot[0], e.shoulderParrot[1] = world.self.shoulderParrot[1];
        if (const PlayerInfo* info = world.playerNamed(playerName)) e.skin = info->skin, e.slim = info->slim, e.cape = info->cape;
        else if (e.skin < 0) e.skin = assets.texture("entity/player/wide/steve"), e.slim = false;
        auto equip = [&](int slot, int menuSlot) {
            bool foil;
            heldStack(menuSlot, e.equipment[slot], e.equipDye[slot], foil);
            e.equipFoil = u8((e.equipFoil & ~(1 << slot)) | (foil ? 1 << slot : 0));
            const ui::ItemStack* st = ui ? ui->playerSlot(menuSlot) : nullptr;
            if (!st) e.equipStack[slot] = nullptr;
            else if (!e.equipStack[slot] || !e.equipStack[slot]->sameItemSameComponents(*st)) e.equipStack[slot] = std::make_shared<ui::ItemStack>(*st);
        };
        equip(0, 36 + game->selectedSlot());
        equip(1, 45);
        equip(5, 5), equip(4, 6), equip(3, 7), equip(2, 8);
        const bool spectator = gameMode && gameMode() == 3;
        er.setLocalPlayer(avatarReady && game->cameraMode != 0 && !spectator ? &avatar : nullptr);
        er.setLocalPlayerShadow(avatarReady && game->cameraMode == 0 && !spectator ? &avatar : nullptr);  // first person
        er.setLocalHolder(ui::itemWorld().localId, avatarReady ? &avatar : nullptr, game->cameraMode == 0, ui ? ui->settings().fov : 70.0f);
    }
    updateItemOwner(partial);
    animClock += dt;
    models.tick(u64(animClock * 20.0));  // animated textures in the item atlas (block items, vibration particles)
    if (be::BlockEntityRenderer* ber = const_cast<be::BlockEntityRenderer*>(be::BlockEntityRenderer::instance()); ber && particles && !ber->particle) {
        Impl& m = *this;
        // Block entity tick effects (called with the world lock held): straight into the particle system.
        ber->particle = [p = m.particles.get()](std::string_view type, double x, double y, double z, double vx, double vy, double vz) {
            p->spawn(type, x, y, z, vx, vy, vz);
        };
        ber->playerPosition = [&m](const std::string& uuid, double pos[3], float& height) {
            if (m.client && uuid == m.client->playerUuid() && m.game) {  // ourselves
                std::copy(m.game->player().pos, m.game->player().pos + 3, pos), height = 1.8f;
                return true;
            }
            for (const auto& [id, e] : m.world.entities())
                if (e.category == Category::Player && std::memcmp(e.uuid, uuid.data(), 16) == 0) {
                    std::copy(e.pos, e.pos + 3, pos);
                    float w;
                    entityBox(e, w, height);
                    return true;
                }
            return false;
        };
    }
    // FireworkRocketEntity.tick on the client: a spark behind the rocket every tick.
    for (; particles && rocketTicks < u64(animClock * 20.0); ++rocketTicks) {
        static std::normal_distribution<double> gauss;
        static std::mt19937 rng(17);
        std::shared_lock lock(world.world().mutex());
        for (const auto& [id, e] : world.entities()) {
            if (e.info && e.info->name == "minecraft:firework_rocket")
                particles->spawn("firework", e.pos[0], e.pos[1], e.pos[2], gauss(rng) * 0.05, -(e.pos[1] - e.prevPos[1]) * 0.5,
                                 gauss(rng) * 0.05);
            mobTick(e);
        }
        if (activationTicks > 0) --activationTicks;
        if (game && !(gameMode && gameMode() == 3)) {  // the local player (not a spectator)
            const Player& p = game->player();
            if (localFirstTick) localWasInWater = p.inWater, localFirstTick = false;  // no splash on the first tick
            // canSpawnSprintParticle: not while flying (LocalPlayer), alive
            waterAndSprint(p.pos, p.vel, 0.6f, p.inWater, localWasInWater, p.sprinting && !p.sneaking && !p.inLava && !p.flying && !game->dead);
        }
    }
    if (particles) {
        // ClientLevel.getMarkerParticleTarget: a creative player holding a barrier or a light marks those blocks.
        std::string marker;
        if (game && gameMode && gameMode() == 1) {
            int item;
            i32 dye;
            bool foil;
            heldStack(36 + game->selectedSlot(), item, dye, foil);
            const std::string n = item >= 0 ? models.itemName(item) : std::string();
            if (n == "barrier" || n == "light") marker = n;
        }
        particles->setAmbientEnv(ambientNight, ambientNether, skyDarken, std::move(marker));
        const CameraState c = game ? game->viewCamera() : CameraState{};
        const double cam[3] = {c.x, c.y, c.z};
        double eye[3] = {};
        const bool scoping = game && game->cameraMode == 0 && game->scoping();
        if (scoping) eye[0] = game->player().pos[0], eye[1] = game->player().pos[1] + game->player().eyeHeight(), eye[2] = game->player().pos[2];
        particles->setView(c.yaw, c.pitch, scoping ? eye : nullptr);
        particles->frame(dt, cam);
        frameParticleMs += particles->stats().tickMs;
    }
}

// ---------------------------------------------------------------------------------------------------------
// World stage: held / worn items, item frames, paintings, dropped items

// EntityRenderDispatcher.submit / FlameFeatureRenderer: a burning entity (shared flag 0) gets stacked fire_0 / fire_1
// quads turned with the camera's yaw, 1.4 x its width, block light 15 (entityCutoutCull on the block atlas).
void Visuals::Impl::buildFlames(const double cam[3], float pt) {
    static int fire[2] = {-2, -2};
    if (fire[0] == -2) fire[0] = models.texture("block/fire_0"), fire[1] = models.texture("block/fire_1");
    if (fire[0] < 0 || fire[1] < 0 || !game) return;
    const CameraState c = game->viewCamera();
    const float sy = std::sin(c.yaw), cy = std::cos(c.yaw);
    const float right[3] = {-cy, 0, -sy}, back[3] = {sy, 0, -cy};  // camera right and towards the camera, horizontal
    auto flame = [&](const Entity& e) {
        float w, h;
        entityBox(e, w, h);
        if (w <= 0 || h <= 0) return;
        double p[3];
        for (int a = 0; a < 3; ++a) p[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt;
        const float s = w * 1.4f;
        float hh = h / s, r = 0.5f, yo = 0, zo = 0.3f - float(int(hh)) * 0.02f;
        const u32 first = u32(dyn.size());
        for (int ss = 0; hh > 0.0f && dyn.size() < kMaxDynamic; ++ss) {
            const ItemModels::Rect& t = models.rect(fire[ss % 2]);
            float u0 = t.u0, u1 = t.u0 + t.du;
            const float v0 = t.v0, v1 = t.v0 + t.dv;
            if (ss / 2 % 2 == 0) std::swap(u0, u1);
            const float corner[4][2] = {{-r, -yo}, {r, -yo}, {r, 1.4f - yo}, {-r, 1.4f - yo}};
            const float uv[4][2] = {{u1, v1}, {u0, v1}, {u0, v0}, {u1, v0}};
            MeshQuad q{};
            for (int k = 0; k < 4; ++k) {
                for (int a = 0; a < 3; ++a) q.pos[k][a] = (right[a] * corner[k][0] + back[a] * zo) * s;
                q.pos[k][1] = corner[k][1] * s;
                q.uv[k][0] = uv[k][0], q.uv[k][1] = uv[k][1];
            }
            q.normal = packNormal(0, 1, 0), q.color = 0xFFFFFFFF, q.tex = kTexItems, q.flags = kQuadNoShade;
            dyn.push_back(q);
            hh -= 0.45f, yo -= 0.45f, r *= 0.9f, zo -= 0.03f;
        }
        const u32 light = (lightAt(p[0], p[1] + h * 0.5, p[2]) & ~15u) | 15u | 512u;  // LightCoordsUtil.withBlock(light, 15)
        worldLists.cut.add(xfTranslate(float(p[0] - cam[0]), float(p[1] - cam[1]), float(p[2] - cam[2])), first, u32(dyn.size()) - first,
                           light, 0xFFFFFFFF, 1);
    };
    for (const auto& [id, e] : world.entities())
        // Entity.displayFireAnimation: on fire and not a spectator (invisible ones burn visibly too)
        if (e.info && (e.sharedFlags & 0x01) && !(world.isSpectator && world.isSpectator(e))) flame(e);
    if (avatarReady && game->cameraMode != 0 && (avatar.sharedFlags & 0x01)) flame(avatar);
}

// GuardianRenderer.renderBeam: from the eyes to the target's middle, two crossed strips turning with the attack time
// and an end cap, coloured from purple to yellow by the attack scale, full bright (entityCutout, guardian_beam: the
// texture repeats 2.5 times per block, so each strip is cut where it wraps).
void Visuals::Impl::buildBeams(const double cam[3], float pt) {
    auto lerpPos = [&](const Entity& e, double out[3]) {
        for (int a = 0; a < 3; ++a) out[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt;
    };
    auto crystalY = [](float t) {  // EndCrystalRenderer.getY
        float hh = std::sin(t * 0.2f) / 2.0f + 0.5f;
        hh = (hh * hh + hh) * 0.4f;
        return hh - 1.4f;
    };
    for (const auto& [id, e] : world.entities()) {
        if (!e.info) continue;
        double p[3], start[3], delta[3];
        if (e.info->name == "minecraft:end_crystal" && dataFloat(e, "DATA_BEAM_TARGET") > 0.5f) {  // EndCrystalRenderer
            const i64 packed = dataInt(e, "DATA_BEAM_TARGET");
            const double target[3] = {double(packed >> 38) + 0.5, double(packed << 52 >> 52) + 0.5, double(packed << 26 >> 38) + 0.5};
            lerpPos(e, p);
            const float t = float(e.age) + pt;
            for (int a = 0; a < 3; ++a) start[a] = target[a] + (a == 1 ? 2.0 : 0.0), delta[a] = p[a] - target[a];
            delta[1] += crystalY(t);
            crystalBeam(start, delta, t, lightAt(p[0], p[1], p[2]), cam);
        } else if (e.info->name == "minecraft:ender_dragon") {  // EnderDragonRenderer: to the crystal healing it
            auto c = dragonCrystal.find(id);
            if (c == dragonCrystal.end()) continue;
            auto ce = world.entities().find(c->second);
            if (ce == world.entities().end()) continue;
            double cp[3];
            lerpPos(e, p), lerpPos(ce->second, cp);
            cp[1] += crystalY(float(ce->second.age) + pt);
            for (int a = 0; a < 3; ++a) start[a] = p[a] + (a == 1 ? 2.0 : 0.0), delta[a] = cp[a] - p[a];
            crystalBeam(start, delta, float(e.age) + pt, lightAt(p[0], p[1], p[2]), cam);
        }
    }
    static int beamTex = -2;
    if (beamTex == -2) beamTex = models.texture("entity/guardian/guardian_beam");
    if (beamTex < 0 || !game) return;
    const ItemModels::Rect& tr = models.rect(beamTex);
    for (const auto& [id, e] : world.entities()) {
        if (!e.info || e.attackTarget == 0 || (e.info->name != "minecraft:guardian" && e.info->name != "minecraft:elder_guardian")) continue;
        double target[3];
        if (auto t = world.entities().find(e.attackTarget); t != world.entities().end() && t->second.info) {
            for (int a = 0; a < 3; ++a) target[a] = t->second.prevPos[a] + (t->second.pos[a] - t->second.prevPos[a]) * pt;
            double tw, th;
            entitySize(t->second, tw, th);
            target[1] += th * 0.5;
        } else if (e.attackTarget == world.selfId) {  // ourselves
            const Player& p = game->player();
            for (int a = 0; a < 3; ++a) target[a] = p.prev[a] + (p.pos[a] - p.prev[a]) * pt;
            target[1] += p.height(p.pose) * 0.5;
        } else {
            continue;
        }
        double eye[3];
        for (int a = 0; a < 3; ++a) eye[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt;
        eye[1] += e.info->eye;
        double d[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
        const double len0 = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len0 < 1e-6) continue;
        for (double& c : d) c /= len0;
        const float length = float(len0 + 1.0);
        const float duration = e.info->name == "minecraft:elder_guardian" ? 60.0f : 80.0f;
        const float time = float(e.attackTime) + pt, scale = time / duration, texVOff = std::fmod(time * 0.5f, 1.0f);
        // poseStack.rotate Y by pi/2 - atan2(z, x), then X by acos(y): local +Y along the beam
        const float xRot = std::acos(float(std::clamp(d[1], -1.0, 1.0))), yRot = kPi / 2 - std::atan2(float(d[2]), float(d[0]));
        auto rotate = [&](float x, float y, float z, float out[3]) {
            const float y1 = y * std::cos(xRot) - z * std::sin(xRot), z1 = y * std::sin(xRot) + z * std::cos(xRot);  // Axis.XP
            out[0] = x * std::cos(yRot) + z1 * std::sin(yRot), out[1] = y1, out[2] = -x * std::sin(yRot) + z1 * std::cos(yRot);  // Axis.YP
        };
        const float rot = time * 0.05f * -1.5f, cs = scale * scale;
        const u32 r = u32(64 + int(cs * 191.0f)), g = u32(32 + int(cs * 191.0f)), b = u32(128 - int(cs * 64.0f));
        const u32 color = 0xFF000000u | (b << 16) | (g << 8) | r;  // ABGR
        auto at = [&](float ang, float rad, float out[2]) { out[0] = std::cos(rot + ang) * rad, out[1] = std::sin(rot + ang) * rad; };
        const u32 first = u32(dyn.size());
        auto quad = [&](const float p[4][3], const float uv[4][2]) {
            if (dyn.size() >= kMaxDynamic) return;
            MeshQuad q{};
            for (int k = 0; k < 4; ++k) {
                rotate(p[k][0], p[k][1], p[k][2], q.pos[k]);
                q.uv[k][0] = tr.u0 + uv[k][0] * tr.du, q.uv[k][1] = tr.v0 + uv[k][1] * tr.dv;
            }
            q.normal = packNormal(0, 1, 0), q.color = color, q.tex = kTexItems, q.flags = kQuadNoShade;
            dyn.push_back(q);
        };
        // The two strips: u 0.4999 .. 0 across, v from minV at the eyes to maxV at the far end, cut at whole v.
        const float minV = -1.0f + texVOff, maxV = minV + length * 2.5f;
        auto strip = [&](const float a[2], const float c[2]) {
            for (float v = minV; v < maxV;) {
                const float cell = std::floor(v), next = std::min(cell + 1.0f, maxV);
                const float y0 = (v - minV) / (maxV - minV) * length, y1 = (next - minV) / (maxV - minV) * length;
                const float p[4][3] = {{a[0], y1, a[1]}, {a[0], y0, a[1]}, {c[0], y0, c[1]}, {c[0], y1, c[1]}};
                const float lo = v - cell, hi = next - cell;
                const float uv[4][2] = {{0.4999f, hi}, {0.4999f, lo}, {0.0f, lo}, {0.0f, hi}};
                quad(p, uv);
                v = next;
            }
        };
        float w[2], ea[2], n[2], s[2];
        at(kPi, 0.2f, w), at(0.0f, 0.2f, ea), at(kPi / 2, 0.2f, n), at(kPi * 3 / 2, 0.2f, s);
        strip(w, ea), strip(n, s);
        float wn[2], en[2], es[2], ws[2];  // the end cap
        at(kPi * 3 / 4, 0.282f, wn), at(kPi / 4, 0.282f, en), at(kPi * 7 / 4, 0.282f, es), at(kPi * 5 / 4, 0.282f, ws);
        const float vBase = int(std::floor(time)) % 2 == 0 ? 0.5f : 0.0f;
        const float cap[4][3] = {{wn[0], length, wn[1]}, {en[0], length, en[1]}, {es[0], length, es[1]}, {ws[0], length, ws[1]}};
        const float capUv[4][2] = {{0.5f, vBase + 0.5f}, {1.0f, vBase + 0.5f}, {1.0f, vBase}, {0.5f, vBase}};
        quad(cap, capUv);
        worldLists.cut.add(xfTranslate(float(eye[0] - cam[0]), float(eye[1] - cam[1]), float(eye[2] - cam[2])), first, u32(dyn.size()) - first,
                           0xFFu | 512u, 0xFFFFFFFF, 1);  // full bright
    }
}

// EnderDragonRenderer.submitCrystalBeams: from `start` (already 2 up) along `delta`, an 8-sided cone (0.15 wide at the
// start, 0.75 at the end) scrolling with the time, black at the start to white at the end (vertex colours: here
// stepped along slices, also cut where the 16 x 256 texture wraps).
void Visuals::Impl::crystalBeam(const double start[3], const double delta[3], float time, u32 light, const double cam[3]) {
    static int beamTex = -2;
    if (beamTex == -2) beamTex = models.texture("entity/end_crystal/end_crystal_beam");
    if (beamTex < 0) return;
    const ItemModels::Rect& tr = models.rect(beamTex);
    const float dx = float(delta[0]), dy = float(delta[1]), dz = float(delta[2]);
    const float horizontal = std::sqrt(dx * dx + dz * dz), length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length < 1e-4f) return;
    const float yRot = -std::atan2(dz, dx) - kPi / 2, xRot = -std::atan2(horizontal, dy) - kPi / 2;
    auto rotate = [&](float x, float y, float z, float out[3]) {
        const float y1 = y * std::cos(xRot) - z * std::sin(xRot), z1 = y * std::sin(xRot) + z * std::cos(xRot);  // Axis.XP
        out[0] = x * std::cos(yRot) + z1 * std::sin(yRot), out[1] = y1, out[2] = -x * std::sin(yRot) + z1 * std::cos(yRot);  // Axis.YP
    };
    const float v0 = -time * 0.01f, v1 = length / 32.0f - time * 0.01f;
    std::vector<float> cuts = {0.0f, 1.0f};
    for (int i = 1; i < 12; ++i) cuts.push_back(i / 12.0f);
    for (float c = std::floor(std::min(v0, v1)) + 1.0f; c < std::max(v0, v1); c += 1.0f) cuts.push_back((c - v0) / (v1 - v0));
    std::sort(cuts.begin(), cuts.end());
    const u32 first = u32(dyn.size());
    for (size_t k = 0; k + 1 < cuts.size(); ++k) {
        const float ta = cuts[k], tb = cuts[k + 1];
        if (tb - ta < 1e-5f) continue;
        const float va = v0 + (v1 - v0) * ta, vb = v0 + (v1 - v0) * tb, cell = std::floor((va + vb) * 0.5f);
        const float ra = 0.2f + 0.8f * ta, rb = 0.2f + 0.8f * tb;
        const u32 g = u32(std::clamp((ta + tb) * 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
        float lastSin = 0.0f, lastCos = 0.75f, lastU = 0.0f;
        for (int i = 1; i <= 8 && dyn.size() < kMaxDynamic; ++i) {
            const float sn = std::sin(i * kPi * 2 / 8.0f) * 0.75f, cs = std::cos(i * kPi * 2 / 8.0f) * 0.75f, u = i / 8.0f;
            const float p[4][3] = {{lastSin * ra, lastCos * ra, ta * length}, {lastSin * rb, lastCos * rb, tb * length},
                                   {sn * rb, cs * rb, tb * length}, {sn * ra, cs * ra, ta * length}};
            const float uv[4][2] = {{lastU, va - cell}, {lastU, vb - cell}, {u, vb - cell}, {u, va - cell}};
            MeshQuad q{};
            for (int c = 0; c < 4; ++c) {
                rotate(p[c][0], p[c][1], p[c][2], q.pos[c]);
                q.uv[c][0] = tr.u0 + uv[c][0] * tr.du, q.uv[c][1] = tr.v0 + uv[c][1] * tr.dv;
            }
            q.normal = packNormal(0, -1, 0), q.color = 0xFF000000u | g << 16 | g << 8 | g, q.tex = kTexItems, q.flags = kQuadNoShade;
            dyn.push_back(q);
            lastSin = sn, lastCos = cs, lastU = u;
        }
    }
    worldLists.cut.add(xfTranslate(float(start[0] - cam[0]), float(start[1] - cam[1]), float(start[2] - cam[2])), first,
                       u32(dyn.size()) - first, light | 512u, 0xFFFFFFFF, 1);
}

void Visuals::Impl::buildWorld(const PassContext& ctx) {
    const double cam[3] = {ctx.camX, ctx.camY, ctx.camZ};
    const float pt = std::clamp(world.partialTick(), 0.0f, 1.0f);
    const auto& entities = world.entities();
    std::shared_lock lock(world.world().mutex());
    // ItemInHandLayer / CustomHeadLayer from the entity renderer's attachment points.
    for (const EntityRenderer::Attachment& a : er.attachments()) {
        const Entity* e = nullptr;
        if (a.entity == EntityRenderer::kLocalPlayerId) e = &avatar;
        else if (auto it = entities.find(a.entity); it != entities.end()) e = &it->second;
        if (!e) continue;
        Lists& L = a.shadowOnly ? shadowLists : worldLists;  // shadow-only casters (first-person player, outside the view)
        const bool rightHanded = ent::rightHanded(*e);
        for (int k = 0; k < 2; ++k) {
            if (!(a.valid & (1 << k))) continue;
            const int slot = (k == 0) == rightHanded ? 0 : 1;  // right arm holds the main hand when right handed
            const int item = e->equipment[slot];
            if (item < 0) continue;
            ui::ItemContext ic = contextOf(*e, k == 0 ? ui::ItemDisplay::ThirdRight : ui::ItemDisplay::ThirdLeft, pt);
            ic.owner.usingThis = (e->livingFlags & 1) && ((e->livingFlags & 2) ? 1 : 0) == slot;
            ic.owner.useTicks = float(e->useTicks);
            if (e->id == EntityRenderer::kLocalPlayerId) {  // the local player: its own use state
                const ui::ItemWorld& w = ui::itemWorld();
                ic.owner.usingThis = w.useSlot >= 0 && w.useSlot == (slot == 0 ? w.selectedSlot : 45);
                ic.owner.useTicks = w.useTicks, ic.owner.remaining = w.useRemaining;
                ic.owner.fishing = w.fishingSlot >= 0 && w.fishingSlot == (slot == 0 ? w.selectedSlot : 45);
                ic.owner.selected = slot == 0;
            }
            // PlayerItemInHandLayer.renderItemHeldToEye: a spyglass in use (not swinging) is drawn at the head, its
            // pitch clamped to -30..90 degrees, translateToHead and 2.5 px to the side
            const float swingNow = e->swingTime >= 0 ? (float(e->swingTime) + pt) / float(std::max(e->swingDuration, 1)) : 0.0f;
            if (ic.owner.usingThis && swingNow < 1.0e-5f && (a.valid & 4) && models.itemName(item) == "minecraft:spyglass") {
                const float pitchNow = e->prevPitch + (e->pitch - e->prevPitch) * pt;
                Xf m;
                std::memcpy(m.m, a.head, sizeof(m.m));
                m = xfMul(m, xfRotX((std::clamp(pitchNow, -30.0f, 90.0f) - pitchNow) * kDeg));
                m = xfMul(m, xfTranslate(0, -0.25f + a.headY, 0));
                m = xfMul(m, xfRotY(180.0f * kDeg));
                m = xfMul(m, xfScale(0.625f, -0.625f, -0.625f));
                m = xfMul(m, xfTranslate((k == 1 ? -2.5f : 2.5f) / 16.0f, -0.0625f, 0));
                L.begin((u64(u32(a.entity)) << 4 | 2) + 3 + k);
                pushItem(L, models.mesh(stackOf(e->equipStack[slot], item, e->equipDye[slot]), contextOf(*e, ui::ItemDisplay::Head, pt)),
                         Display::Head, m, a.light, (e->equipFoil >> slot) & 1);
                continue;
            }
            Xf m;
            std::memcpy(m.m, a.hand[k], sizeof(m.m));
            m = xfMul(m, xfRotX(-90.0f * kDeg));
            m = xfMul(m, xfRotY(180.0f * kDeg));
            const float ox = a.babyOffset ? 0.0f : 1.0f, oy = a.babyOffset ? 1.0f : 2.0f, oz = a.babyOffset ? -4.5f : -10.0f;
            m = xfMul(m, xfTranslate((k == 1 ? -1.0f : 1.0f) * ox / 16.0f, oy / 16.0f, oz / 16.0f));
            {
                const bool armRight = k == 0;
                const float swingAnim = e->swingTime >= 0 ? (float(e->swingTime) + pt) / float(std::max(e->swingDuration, 1)) : 0.0f;
                auto kineticOf = [&](int s) {
                    KineticParams p;
                    if (rules && e->equipment[s] >= 0) {
                        const ItemRules::Components& c = rules->components(models.itemName(e->equipment[s]));
                        p = {c.kineticValid, c.kineticDelay, c.kineticDismount, c.kineticKnockback, c.kineticDamage, c.kineticForward};
                    }
                    return p;
                };
                const bool swingArmRight = (e->swingHand == 0) == rightHanded;
                const int type = e->swingType >= 0 ? e->swingType : models.itemName(e->equipment[e->swingHand & 1]).ends_with("_spear") ? 2 : 1;
                if (swingAnim > 0 && type == 2 && swingArmRight == armRight) {  // thirdPersonAttackItem
                    const float attack = ease::inQuad(ease::progress(swingAnim, 0.05f, 0.2f));
                    const float retract = ease::inOutExpo(ease::progress(swingAnim, 0.4f, 1.0f));
                    m = xfMul(m, xfMul(xfTranslate(0, -0.125f, 0.125f), xfMul(xfRotX(-70.0f * (attack - retract) * kDeg),
                                                                              xfTranslate(0, 0.125f, -0.125f))));
                    m = xfMul(m, xfTranslate(0, kineticOf(0).forward * (attack - retract), 0));
                }
                const bool using_ = (e->livingFlags & 1) != 0;
                const bool useArmRight = ((e->livingFlags & 2) != 0) != rightHanded;
                const KineticParams kp = kineticOf(slot);
                if (using_ && useArmRight == armRight && kp.valid) {  // ArmPose.SPEAR.animateUseItem: thirdPersonUseItem
                    const float timeHeld = float(e->useTicks) + pt;
                    const float attack = ease::inQuad(ease::progress(swingAnim, 0.05f, 0.2f));
                    const float retract = ease::inOutExpo(ease::progress(swingAnim, 0.4f, 1.0f));
                    const SpearUse u = spearUse(kp, timeHeld);
                    const float inv = armRight ? 1.0f : -1.0f;
                    const float raiseModified = 1.0f - ease::outBack(1.0f - u.raise);
                    const float hit = spearHitFeedback(e->kineticHitTicks >= 0 ? float(e->kineticHitTicks) + pt : -1.0f);
                    m = xfMul(m, xfTranslate(0, -hit * 0.4f, -kp.forward * (raiseModified - u.raiseBack) + hit));
                    const float xr = 70.0f * (u.raise - u.raiseBack) - 40.0f * (attack - retract);
                    m = xfMul(m, xfMul(xfTranslate(0, -0.03125f, 0.125f), xfMul(xfRotX(-xr * kDeg), xfTranslate(0, 0.03125f, -0.125f))));
                    const float yr = inv * 90.0f * (u.raise - u.sway + 3.0f * retract + attack);
                    m = xfMul(m, xfMul(xfTranslate(0, 0, 0.125f), xfMul(xfRotY(yr * kDeg), xfTranslate(0, 0, -0.125f))));
                }
            }
            L.begin((u64(u32(a.entity)) << 4 | u64(k)) + 1);
            pushItem(L, models.mesh(stackOf(e->equipStack[slot], item, e->equipDye[slot]), ic),
                     k == 0 ? Display::ThirdRight : Display::ThirdLeft, m, a.light, (e->equipFoil >> slot) & 1);
        }
        if (a.valid & 8) {  // HoldingEntityRenderState: the main hand item with the ground transform
            Xf m;
            std::memcpy(m.m, a.item, sizeof(m.m));
            L.begin((u64(u32(a.entity)) << 4 | 3) + 1);
            pushItem(L, models.mesh(stackOf(e->equipStack[0], e->equipment[0], e->equipDye[0]), contextOf(*e, ui::ItemDisplay::Ground, pt)),
                     Display::Ground, m, a.light, e->equipFoil & 1);
        }
        const int head = e->equipment[5];
        if ((a.valid & 4) && head >= 0) {
            const ItemModels::Equippable& eq = models.equippable(head);
            if (eq.asset.empty()) {  // armour is drawn as a model layer
                Xf m;
                std::memcpy(m.m, a.head, sizeof(m.m));
                L.begin((u64(u32(a.entity)) << 4 | 2) + 1);
                if (isSkull(head)) {  // wornHeadType: the skull model, scaled 1.1875
                    if (skullMeshes.size() <= size_t(head)) skullMeshes.resize(size_t(head) + 1, nullptr);
                    if (!skullMeshes[size_t(head)]) {
                        std::string layer, tex;
                        ItemModels::skullLayer(models.itemName(head), layer, tex);
                        skullMeshes[size_t(head)] = &models.entityLayerMesh(layer, tex);
                    }
                    m = xfMul(m, xfTranslate(0, a.skullY, 0));
                    m = xfMul(m, xfScale(1.1875f, 1.1875f, 1.1875f));
                    pushRaw(L, *skullMeshes[size_t(head)], m, a.light);
                } else {  // translateToHead
                    m = xfMul(m, xfTranslate(0, -0.25f + a.headY, 0));
                    m = xfMul(m, xfRotY(180.0f * kDeg));
                    m = xfMul(m, xfScale(0.625f, -0.625f, -0.625f));
                    pushItem(L, models.mesh(stackOf(e->equipStack[5], head, e->equipDye[5]), contextOf(*e, ui::ItemDisplay::Head, pt)),
                             Display::Head, m, a.light, (e->equipFoil >> 5) & 1);
                }
            }
        }
    }
    buildBlockEntityItems(cam);
    // Item frames, paintings, dropped items.
    static int frameType = -2, glowFrameType = -2, paintingType = -2;
    static const int ominousType = assets.typeId("minecraft:ominous_item_spawner");
    if (frameType == -2)
        frameType = assets.typeId("minecraft:item_frame"), glowFrameType = assets.typeId("minecraft:glow_item_frame"),
        paintingType = assets.typeId("minecraft:painting");
    const float maxDist = std::min({ctx.fogEnd, ctx.fogRenderEnd, 128.0f});
    auto vp = [&](int r, int c) { return ctx.viewProj[c * 4 + r]; };
    float planes[5][4];
    for (int c = 0; c < 4; ++c) {
        planes[0][c] = vp(3, c) + vp(0, c), planes[1][c] = vp(3, c) - vp(0, c);
        planes[2][c] = vp(3, c) + vp(1, c), planes[3][c] = vp(3, c) - vp(1, c);
        planes[4][c] = vp(3, c) - vp(2, c);
    }
    auto visible = [&](float x, float y, float z, float r) {
        if (x * x + y * y + z * z > (maxDist + r) * (maxDist + r)) return false;
        for (const auto& pl : planes) {
            const float len = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
            if (pl[0] * x + pl[1] * y + pl[2] * z + pl[3] < -r * len) return false;
        }
        return true;
    };
    // Shadow casters outside the view (shader pipeline): inside a cascade drawn this frame.
    auto inShadow = [&](float x, float y, float z, float r) {
        if (ctx.pack || !ctx.shadowMats) return false;
        for (u32 c = 0; c < 4; ++c) {
            if (!((ctx.shadowMask >> c) & 1)) continue;
            const float* m = ctx.shadowMats[c];
            bool in = true;
            for (int row = 0; row < 3 && in; ++row) {
                const float v = m[row] * x + m[4 + row] * y + m[8 + row] * z + m[12 + row];
                const float len = std::sqrt(m[row] * m[row] + m[4 + row] * m[4 + row] + m[8 + row] * m[8 + row]);
                in = row < 2 ? std::abs(v) <= 1 + r * len : v <= 1 + r * len;
            }
            if (in) return true;
        }
        return false;
    };
    static const int kStep[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    static const int itemDisplayType = assets.typeId("minecraft:item_display");
    float camYaw, camPitch;
    cameraAngles(ctx, camYaw, camPitch);
    struct GlowCopy {  // what an entity pushed into the World lists, copied to its outline colour's lists afterwards
        Impl& m;
        int color;
        size_t cut, tr;
        ~GlowCopy() {
            if (color < 0) return;
            auto it = std::find_if(m.glowLists.begin(), m.glowLists.end(), [&](const auto& g) { return g.first == u32(color); });
            if (it == m.glowLists.end()) it = m.glowLists.insert(m.glowLists.end(), {u32(color), Lists{}});
            for (size_t i = cut; i < m.worldLists.cut.v.size(); ++i) it->second.cut.v.push_back(m.worldLists.cut.v[i]), it->second.cut.keys.push_back(0);
            for (size_t i = tr; i < m.worldLists.tr.v.size(); ++i) it->second.tr.v.push_back(m.worldLists.tr.v[i]), it->second.tr.keys.push_back(0);
        }
    };
    for (const auto& [id, e] : entities) {
        const GlowCopy glowCopy{*this, er.outlineColor && !ctx.pack ? er.outlineColor(e) : -1, worldLists.cut.v.size(), worldLists.tr.v.size()};
        if (e.type == itemDisplayType && e.itemId >= 0) {  // DisplayRenderer.ItemDisplayRenderer
            double p[3];
            for (int a = 0; a < 3; ++a) p[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt - cam[a];
            const float d2 = float(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]), range = e.display.viewRange * 64.0f;
            Xf m;
            if (d2 > range * range || !displayMatrix(e, pt, camYaw, camPitch, m.m)) continue;
            m.m[3] += float(p[0]), m.m[7] += float(p[1]), m.m[11] += float(p[2]);
            // Culled like the entity renderer when the display has a size (width and height), else always drawn.
            const DisplayState& s = e.display;
            if (s.width > 0 && s.height > 0 &&
                !visible(float(p[0]), float(p[1]) + s.height * 0.5f, float(p[2]), std::max(s.width, s.height)))
                continue;
            static const Display kContext[10] = {Display::None,  Display::ThirdLeft, Display::ThirdRight, Display::FirstLeft,
                                                 Display::FirstRight, Display::Head, Display::Gui,     Display::Ground,
                                                 Display::Fixed, Display::OnShelf};
            const Display ctxt = kContext[std::clamp(s.itemContext, 0, 9)];
            worldLists.begin(u64(u32(id)) << 4 | 3);
            const ui::ItemContext ic = contextOf(e, ui::ItemDisplay(std::clamp(s.itemContext, 0, 9)), pt);
            pushItem(worldLists, models.mesh(stackOf(e.itemStack, e.itemId, e.itemDye), ic), ctxt, xfMul(m, xfRotY(kPi)),
                     displayLight(e, lightAt(e.pos[0], e.pos[1], e.pos[2])), e.itemFoil);
            continue;
        }
        const bool frame = e.type == frameType || e.type == glowFrameType;
        // ThrownItemRenderer / FireworkEntityRenderer: the projectile's item (ItemSupplier.getItem: its synced stack, or
        // the type's default item when that was never sent), scale, full bright for the eye and the fireballs
        struct Thrown {
            const char* type;
            const char* item;
            float scale;
            bool fullBright, firework;
            int typeId = -2, itemId = -1;
        };
        static Thrown thrownDefs[] = {{"snowball", "snowball", 1, false, false}, {"egg", "egg", 1, false, false},
                                      {"ender_pearl", "ender_pearl", 1, false, false},
                                      {"experience_bottle", "experience_bottle", 1, false, false},
                                      {"splash_potion", "splash_potion", 1, false, false},
                                      {"lingering_potion", "lingering_potion", 1, false, false},
                                      {"eye_of_ender", "ender_eye", 1, true, false}, {"fireball", "fire_charge", 3, true, false},
                                      {"small_fireball", "fire_charge", 0.75f, true, false},
                                      {"firework_rocket", "firework_rocket", 1, false, true}};
        if (thrownDefs[0].typeId == -2)
            for (Thrown& t : thrownDefs) {
                t.typeId = assets.typeId(std::string("minecraft:") + t.type);
                const auto& names = assets.itemNames();
                t.itemId = int(std::find(names.begin(), names.end(), std::string(t.item)) - names.begin());
                if (t.itemId >= int(names.size())) t.itemId = -1;
            }
        const Thrown* thrown = nullptr;
        for (const Thrown& t : thrownDefs)
            if (e.type == t.typeId) thrown = &t;
        if (!frame && e.type != paintingType && e.category != Category::Item && e.type != ominousType && !thrown) continue;
        double p[3];
        for (int a = 0; a < 3; ++a) p[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt;
        if (e.pickupBy != INT_MIN) {  // ItemPickupParticleGroup: towards the collector's middle, (time / 3)^2
            double to[3];
            if (world.pickupTarget(e.pickupBy, pt, to)) {
            } else if (game) {  // the local player
                const Player& pl = game->player();
                for (int a = 0; a < 3; ++a) to[a] = pl.prev[a] + (pl.pos[a] - pl.prev[a]) * pt;
                to[1] += pl.eyeHeight() / 2.0;
            } else {
                std::copy(p, p + 3, to);
            }
            float t = (float(e.pickupLife) + pt) / 3.0f;
            t = std::min(t * t, 1.0f);
            for (int a = 0; a < 3; ++a) p[a] += (to[a] - p[a]) * t;
        }
        const float rx = float(p[0] - cam[0]), ry = float(p[1] - cam[1]), rz = float(p[2] - cam[2]);
        const float radius = e.type == paintingType ? 4.0f : 1.0f;
        Lists* target = &worldLists;
        if (!visible(rx, ry, rz, radius)) {
            if (!inShadow(rx, ry, rz, radius)) continue;
            target = &shadowLists;
        }
        Lists& L = *target;
        L.begin(u64(u32(id)) << 4 | 3);
        u32 light = lightAt(p[0], p[1], p[2]);
        if (thrown) {  // scaled, turned to the camera (camera.orientation), the item in its GROUND display
            const int item = e.itemId >= 0 ? e.itemId : thrown->itemId;
            if (item < 0) continue;
            if (thrown->fullBright) light |= 0x0Fu;  // getBlockLightLevel: 15
            const ItemMesh& mesh = models.mesh(stackOf(e.itemId >= 0 ? e.itemStack : nullptr, item, e.itemDye),
                                               contextOf(e, ui::ItemDisplay::Ground, pt));
            if (mesh.empty()) continue;
            Xf m = xfMul(xfTranslate(rx, ry, rz), xfScale(thrown->scale, thrown->scale, thrown->scale));
            m = xfMul(xfMul(m, xfRotY((180.0f - camYaw) * kDeg)), xfRotX(-camPitch * kDeg));
            if (thrown->firework && dataInt(e, "DATA_SHOT_AT_ANGLE"))  // shot at an angle: Z 180, Y 180, X 90
                m = xfMul(xfMul(xfMul(m, xfRotZ(kPi)), xfRotY(kPi)), xfRotX(kPi / 2));
            pushItem(L, mesh, Display::Ground, m, light, e.itemFoil);
            continue;
        }
        if (frame) {  // ItemFrameRenderer
            const int dir = std::clamp(e.direction, 0, 5);
            Xf m = xfTranslate(rx, ry, rz);
            m = xfMul(m, xfTranslate(kStep[dir][0] * 0.46875f, kStep[dir][1] * 0.46875f, kStep[dir][2] * 0.46875f));
            float xRot = 0, yRot = 180.0f;
            static const float kYRot[6] = {0, 0, 180, 0, 90, 270};  // Direction.toYRot
            if (dir >= 2) yRot = 180.0f - kYRot[dir];
            else xRot = dir == 1 ? -90.0f : 90.0f;
            m = xfMul(m, xfRotX(xRot * kDeg));
            m = xfMul(m, xfRotY(yRot * kDeg));
            const bool glow = e.type == glowFrameType;
            const bool invisible = (e.sharedFlags & 0x20) != 0;
            const MapTex* mapTex = e.itemId >= 0 && e.mapId >= 0 ? mapTexture(e.mapId) : nullptr;  // a map with data
            const bool map = mapTex != nullptr;
            u32 frameLight = light;
            if (glow) frameLight = (light & 0xF0) | std::max<u32>(light & 15, 5);
            if (!invisible) {
                const ItemMesh*& fm = frameMeshes[(glow ? 2 : 0) + (map ? 1 : 0)];
                if (!fm) fm = &models.blockModelMesh(std::string("block/") + (glow ? "glow_item_frame" : "item_frame") + (map ? "_map" : ""));
                pushRaw(L, *fm, xfMul(m, xfTranslate(-0.5f, -0.5f, -0.5f)), frameLight);
            }
            if (map) {  // MapRenderer: the 128 x 128 map image (decorations are not drawn)
                Xf im = xfMul(m, xfTranslate(0, 0, invisible ? 0.5f : 0.4375f));
                im = xfMul(im, xfRotZ(float(e.rotation % 4 * 2) * 45.0f * kDeg));
                im = xfMul(im, xfRotZ(kPi));
                im = xfMul(im, xfScale(1.0f / 128, 1.0f / 128, 1.0f / 128));
                im = xfMul(im, xfTranslate(-64, -64, -1));
                L.cut.add(im, mapTex->quad, 1, (glow ? 0xFDu : frameLight) | 512u, 0xFFFFFFFF, 0, L.next());
                pushMapDecorations(L.cut, L.tr, im, e.mapId, true, (glow ? 0xFDu : frameLight) | 512u);
            } else if (e.itemId >= 0) {
                Xf im = xfMul(m, xfTranslate(0, 0, invisible ? 0.5f : 0.4375f));
                im = xfMul(im, xfRotZ(float(e.rotation % 8) * 45.0f * kDeg));
                im = xfMul(im, xfScale(0.5f, 0.5f, 0.5f));
                pushItem(L, models.mesh(stackOf(e.itemStack, e.itemId, e.itemDye), contextOf(e, ui::ItemDisplay::Fixed, pt)),
                         Display::Fixed, im, glow ? 0xFFu : light, e.itemFoil);
            }
            continue;
        }
        if (e.type == paintingType) {  // PaintingRenderer
            PaintingInfo* info = const_cast<PaintingInfo*>(painting(e.painting));
            if (!info) continue;
            if (!info->mesh) info->mesh = &models.paintingMesh(info->asset, info->w, info->h);
            static const int k2d[6] = {0, 0, 2, 0, 1, 3};  // 3D data value -> 2D data value (S W N E)
            const int dir = std::clamp(e.direction, 0, 5), d2 = k2d[dir];
            Xf m = xfMul(xfTranslate(rx, ry, rz), xfRotY(float(180 - d2 * 90) * kDeg));
            if (info->mesh->empty()) continue;
            // PaintingRenderer.extractRenderState: each segment lit by the block it covers.
            const ItemMesh::Part& part = info->mesh->parts[0];
            const int w = info->w, h = info->h;
            if (part.count < u32(w * h) * ItemModels::kPaintingSegmentQuads) continue;
            for (int sy = 0; sy < h; ++sy)
                for (int sx = 0; sx < w; ++sx) {
                    const double ox = sx - w / 2.0 + 0.5, oy = sy - h / 2.0 + 0.5;
                    double bx = std::floor(e.pos[0]), bz = std::floor(e.pos[2]);
                    if (dir == 2) bx = std::floor(e.pos[0] + ox);       // north
                    else if (dir == 4) bz = std::floor(e.pos[2] - ox);  // west
                    else if (dir == 3) bx = std::floor(e.pos[0] - ox);  // south
                    else if (dir == 5) bz = std::floor(e.pos[2] + ox);  // east
                    const u32 seg = u32(sx + sy * w) * ItemModels::kPaintingSegmentQuads;
                    L.cut.add(m, part.first + seg, ItemModels::kPaintingSegmentQuads, lightAt(bx, std::floor(e.pos[1] + oy), bz),
                              0xFFFFFFFF, 0, L.next());
                }
            continue;
        }
        // ItemEntityRenderer: bob, spin and several copies for larger stacks (GROUND display).
        if (e.itemId < 0) continue;
        const ItemMesh& mesh = models.mesh(stackOf(e.itemStack, e.itemId, e.itemDye), contextOf(e, ui::ItemDisplay::Ground, pt));
        if (mesh.empty()) continue;
        const float age = float(e.age) + pt;
        Xf m;
        if (e.type == ominousType) {  // OminousItemSpawnerRenderer: grows in over 50 ticks, spins 40 degrees a tick
            const float s = std::min(age, 50.0f) / 50.0f;
            m = xfMul(xfTranslate(rx, ry, rz), xfScale(s, s, s));
            m = xfMul(m, xfRotY(wrapDegrees(age * 40.0f) * kDeg));
            light = 0xFF;
        } else {
            const float bobY = std::sin(age / 10.0f + e.bobOffset) * 0.1f + 0.1f;
            const float spin = age / 20.0f + e.bobOffset;
            // The game draws item entities back-face culled, so from inside one nothing of it shows: a dropped item
            // starts at the dropper's eyes (eye height - 0.3) and would otherwise fill the screen for a moment.
            const float cy = ry + bobY + 0.2f;
            if (rx * rx + cy * cy + rz * rz < 0.3f * 0.3f) continue;
            m = xfTranslate(rx, ry + bobY + (-mesh.groundMinY + 0.0625f), rz);
            m = xfMul(m, xfRotY(spin));
        }
        const int c = e.itemCount;
        const int amount = c > 48 ? 5 : c > 32 ? 4 : c > 16 ? 3 : c > 1 ? 2 : 1;
        ClusterRandom random(i64(e.itemId) + (e.itemStack ? e.itemStack->damage : 0));
        auto rnd = [&] { return random.next(); };
        if (mesh.groundDepth > 0.0625f) {
            pushItem(L, mesh, Display::Ground, m, light, e.itemFoil);
            for (int i = 1; i < amount; ++i)
                pushItem(L, mesh, Display::Ground, xfMul(m, xfTranslate(rnd() * 0.15f, rnd() * 0.15f, rnd() * 0.15f)), light,
                         e.itemFoil);
        } else {
            const float offsetZ = mesh.groundDepth * 1.5f;
            m = xfMul(m, xfTranslate(0, 0, -(offsetZ * float(amount - 1) / 2.0f)));
            pushItem(L, mesh, Display::Ground, m, light, e.itemFoil);
            m = xfMul(m, xfTranslate(0, 0, offsetZ));
            for (int i = 1; i < amount; ++i) {
                pushItem(L, mesh, Display::Ground, xfMul(m, xfTranslate(rnd() * 0.075f, rnd() * 0.075f, 0)), light, e.itemFoil);
                m = xfMul(m, xfTranslate(0, 0, offsetZ));
            }
        }
    }
    buildFlames(cam, pt);  // world lock held (lightAt)
    buildBeams(cam, pt);
}

// ---------------------------------------------------------------------------------------------------------
// Name tags (EntityRenderer.submitNameDisplay / SubmitNodeCollection.submitNameTag)

void Visuals::Impl::buildTags(const PassContext& ctx) {
    if (!font) return;
    const double cam[3] = {ctx.camX, ctx.camY, ctx.camZ};
    const float pt = std::clamp(world.partialTick(), 0.0f, 1.0f);
    auto vp = [&](int r, int c) { return ctx.viewProj[c * 4 + r]; };
    float right[3], up[3], fwd[3];
    for (int r = 0; r < 3; ++r) {
        const int row = r == 2 ? 3 : r;
        float l = 0;
        for (int c = 0; c < 3; ++c) l += vp(row, c) * vp(row, c);
        l = std::sqrt(std::max(l, 1e-12f));
        float* out = r == 0 ? right : r == 1 ? up : fwd;
        for (int c = 0; c < 3; ++c) out[c] = vp(row, c) / l;
    }
    // Crosshair entity (named mobs show their name when looked at): Minecraft.crosshairPickEntity (the entity reach).
    const int targeted = game ? game->crosshairEntity : -1;
    std::shared_lock lock(world.world().mutex());
    // Text displays (DisplayRenderer.TextDisplayRenderer): the display's pose, then rotate(Y, pi) and scale(-0.025);
    // one-sided (the text pipelines cull back faces), see-through ones without the depth test.
    static const int textDisplayType = assets.typeId("minecraft:text_display");
    float camYaw, camPitch;
    cameraAngles(ctx, camYaw, camPitch);
    for (const auto& [id, e] : world.entities()) {
        if (e.type != textDisplayType) continue;
        double p[3];
        for (int a = 0; a < 3; ++a) p[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt - cam[a];
        const float range = e.display.viewRange * 64.0f;
        Xf m;
        if (float(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) > range * range || !displayMatrix(e, pt, camYaw, camPitch, m.m))
            continue;
        m.m[3] += float(p[0]), m.m[7] += float(p[1]), m.m[11] += float(p[2]);
        const TextBlock& t = textBlock(id, e.display);
        m = xfMul(m, xfRotY(kPi));
        m = xfMul(m, xfScale(-0.025f, -0.025f, -0.025f));
        m = xfMul(m, xfTranslate(1.0f - t.width / 2.0f, -t.height, 0));
        const float c0[3] = {m.m[0], m.m[4], m.m[8]}, c1[3] = {m.m[1], m.m[5], m.m[9]};
        const float n[3] = {c0[1] * c1[2] - c0[2] * c1[1], c0[2] * c1[0] - c0[0] * c1[2], c0[0] * c1[1] - c0[1] * c1[0]};
        if (n[0] * m.m[3] + n[1] * m.m[7] + n[2] * m.m[11] <= 0) continue;  // seen from behind
        u32 background;
        int opacity;
        displayTextColors(e, pt, background, opacity);
        if (e.display.style & 4) background = ui ? ui->textBackground(0.25f) : 0x3F000000u;  // the default background
        u32 alpha = u8(opacity);
        if ((alpha & 0xFC) == 0) alpha = 255;  // Font.adjustColor
        const u32 first = u32(dyn.size());
        if (first + t.quads.size() > kMaxDynamic) continue;
        dyn.insert(dyn.end(), t.quads.begin(), t.quads.end());
        const u32 light = displayLight(e, lightAt(e.pos[0], e.pos[1], e.pos[2])) | 512u;
        List& L = (e.display.style & 2) ? tagSee : tagNormal;
        if (background) L.add(m, first, 1, light, abgr(background & 0xFFFFFF, background >> 24), 1);
        L.add(m, first + 1, u32(t.quads.size()) - 1, light, (alpha << 24) | 0xFFFFFF, 1);
    }
    if (frameCounter % 256 == 0)  // forget the text of removed displays
        for (auto it = textBlocks.begin(); it != textBlocks.end();)
            it = world.entities().count(it->first) ? std::next(it) : textBlocks.erase(it);
    static const int frameTypes[2] = {assets.typeId("minecraft:item_frame"), assets.typeId("minecraft:glow_item_frame")};
    for (const auto& [id, e] : world.entities()) {
        if (!e.info) continue;  // (dying entities keep their tags until removed)
        // ItemFrameRenderer.shouldShowName / getNameTag: only the targeted frame's item, when it has a custom name
        const bool frame = e.type >= 0 && (e.type == frameTypes[0] || e.type == frameTypes[1]);
        if (frame && !(id == targeted && e.itemStack && e.itemStack->hasCustomName && !(ui && ui->hudHidden()))) continue;
        if (!frame && e.name.empty() && !e.nameVisible) continue;  // (CustomNameVisible alone: the type's name)
        const bool player = e.category == Category::Player;
        char uuidText[37];
        std::snprintf(uuidText, sizeof(uuidText), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", e.uuid[0], e.uuid[1],
                      e.uuid[2], e.uuid[3], e.uuid[4], e.uuid[5], e.uuid[6], e.uuid[7], e.uuid[8], e.uuid[9], e.uuid[10], e.uuid[11], e.uuid[12],
                      e.uuid[13], e.uuid[14], e.uuid[15]);
        const std::string member = player ? e.name : std::string(uuidText);
        if (!frame) {
            // EntityRenderer / MobRenderer / AvatarRenderer.shouldShowName: shown always (players) or named and looked at;
            // ArmorStandRenderer: only with CustomNameVisible (invisible ones too: holograms).
            if (e.info->name == "minecraft:armor_stand") {
                if (!e.nameVisible) continue;
            } else {
                if (!player && !e.nameVisible && id != targeted) continue;
                if (player || e.category == Category::Living) {  // LivingEntityRenderer.shouldShowName
                    const bool visible = !(e.sharedFlags & 0x20) || (gameMode && gameMode() == 3) || ui::seesInvisible(playerName, member);
                    const int team = ui::teamNameTagShown(playerName, member, visible);
                    if (team == 0 || (team < 0 && (!visible || e.passengerCount > 0 || (ui && ui->hudHidden())))) continue;
                }
            }
        }
        double p[3];
        for (int a = 0; a < 3; ++a) p[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt;
        if (frame) {  // ItemFrameRenderer.getRenderOffset: (0.3 x step, -0.25, 0.3 z step)
            static const int kStepXZ[6][2] = {{0, 0}, {0, 0}, {0, -1}, {0, 1}, {-1, 0}, {1, 0}};
            const int dir = std::clamp(e.direction, 0, 5);
            p[0] += kStepXZ[dir][0] * 0.3, p[1] -= 0.25, p[2] += kStepXZ[dir][1] * 0.3;
        }
        const bool discrete = (e.sharedFlags & 0x02) != 0;  // Entity.isDiscrete: isShiftKeyDown (FLAG_SHIFT_KEY_DOWN)
        double bw, bh;  // the current dimensions (poses, sleeping, age and scale): the name tag attachment's height
        entitySize(e, bw, bh);
        // the NAME_TAG attachment: the entity's height (AT_HEIGHT), the sniffer's nameTagOffset(2.05) scaled with it
        const float h = float(e.info->name == "minecraft:sniffer" ? bh * 2.05 / 1.75 : bh);
        const float x = float(p[0] - cam[0]), y = float(p[1] - cam[1]) + h + 0.5f, z = float(p[2] - cam[2]);
        // EntityRenderDispatcher.distanceToSqr: from the entity's position (not the tag's)
        const float ox = float(p[0] - cam[0]), oy = float(p[1] - cam[1]), oz = float(p[2] - cam[2]);
        const float d2 = ox * ox + oy * oy + oz * oz;
        // EntityRenderer: < 64 blocks; LivingEntityRenderer.shouldShowName: a sneaking one only within 32
        const bool living = (e.category == Category::Living || e.category == Category::Player) &&
                            e.info->name != "minecraft:armor_stand";  // ArmorStandRenderer: its own rule
        if (!(d2 < 64.0f * 64.0f) || (living && discrete && d2 >= 1024.0f)) continue;
        if (fwd[0] * x + fwd[1] * y + fwd[2] * z < -1.0f) continue;  // behind the camera
        // Entity.getDisplayName: the team's prefix, colour and suffix (members: a player's name, else the UUID).
        const u32 light = lightAt(p[0], p[1] + e.info->eye, p[2]) | 512u;
        // SubmitNodeCollection.submitNameTag: background getBackgroundOpacity(0.25), text max((bg + 0.75) / 2, 0.5)
        const u32 bgColor = ui ? ui->textBackground(0.25f) : 0x3F000000u;
        const float textAlpha = std::max((float(bgColor >> 24) / 255.0f + 0.75f) * 0.5f, 0.5f);
        const u32 textHalf = (u32(textAlpha * 255.0f + 0.5f) << 24) | 0xFFFFFFu;
        auto submit = [&](const Tag& t, float yUp) {  // EntityRenderer.submitNameTag at the attachment, raised by yUp
            if (t.first == UINT32_MAX) return;
            Xf m;
            const float s = 0.025f;
            for (int r = 0; r < 3; ++r) m.m[r * 4] = right[r] * s, m.m[r * 4 + 1] = -up[r] * s, m.m[r * 4 + 2] = fwd[r] * s;
            m.m[3] = x, m.m[7] = y + yUp, m.m[11] = z;
            if (!discrete) {
                tagSee.add(m, t.first, 1, light, bgColor);
                tagSee.add(m, t.first + 1, t.glyphs, light, textHalf);
                // lightCoordsWithEmission(light, 2): block and sky light at least 2
                const u32 bright = (light & ~0xFFu) | (std::max<u32>((light >> 4) & 15, 2) << 4) | std::max<u32>(light & 15, 2);
                tagNormal.add(m, t.first + 1, t.glyphs, bright, 0xFFFFFFFFu);
            } else {
                tagNormal.add(m, t.first, 1, light, bgColor);
                tagNormal.add(m, t.first + 1, t.glyphs, light, textHalf);
            }
        };
        // EntityRenderer.submitNameDisplay: the below-name score (within 10 blocks) first, the name 9 * 1.15 px above.
        float raise = 0;
        ui::Text score;
        if (d2 < 100.0f && ui::belowNameText(member, score)) submit(tag(score), 0), raise = 9.0f * 1.15f * 0.025f;
        // Entity.getName: the custom name, else the type's description
        const ui::Text name = !e.name.empty() || !res ? ui::Text(e.name)
                                                      : ui::translatable("entity." + std::string(e.info->name).replace(e.info->name.find(':'), 1, "."), {}, &res->lang());
        submit(tag(frame ? e.itemStack->customName : ui::teamFormattedName(member, name)), raise);
        ++self.stats_.tags;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Lightning bolts (LightningBoltRenderer)

namespace {
// LegacyRandomSource (java.util.Random): the bolt's shape from its seed.
struct JavaRandom {
    u64 seed;
    explicit JavaRandom(u64 s) : seed((s ^ 0x5DEECE66Dull) & ((1ull << 48) - 1)) {}
    int next(int bits) {
        seed = (seed * 0x5DEECE66Dull + 0xBull) & ((1ull << 48) - 1);
        return int(i64(seed) >> (48 - bits));
    }
    float nextFloat() { return float(next(24)) / float(1 << 24); }
    int nextInt(int bound) {
        if ((bound & -bound) == bound) return int((i64(bound) * i64(next(31))) >> 31);
        int bits, val;
        do bits = next(31), val = bits % bound;
        while (bits - val + (bound - 1) < 0);
        return val;
    }
};
}  // namespace

void Visuals::Impl::buildBolts(const PassContext& ctx) {
    static const int boltType = assets.typeId("minecraft:lightning_bolt");
    const double cam[3] = {ctx.camX, ctx.camY, ctx.camZ};
    const float pt = std::clamp(world.partialTick(), 0.0f, 1.0f);
    auto rnd = [&] {  // the client's own randomness (seeds, flash counts)
        boltRandom ^= boltRandom << 13, boltRandom ^= boltRandom >> 7, boltRandom ^= boltRandom << 17;
        return boltRandom;
    };
    for (const auto& [id, e] : world.entities()) {
        if (e.type != boltType) continue;
        Bolt& b = boltStates[id];
        if (b.age < 0) b.age = e.age, b.seed = rnd(), b.flashes = int(rnd() % 3) + 1;
        for (; b.age < e.age; ++b.age)  // LightningBolt.tick (client): flashes re-roll the shape
            if (--b.life < 0 && b.flashes > 0 && b.life < -int(rnd() % 10)) --b.flashes, b.life = 1, b.seed = rnd();
        if (b.life < 0 && b.flashes == 0) continue;  // discarded on the client
        float xOffsets[8], zOffsets[8], xOffset = 0, zOffset = 0;
        JavaRandom r0(b.seed);
        for (int h = 7; h >= 0; --h) {
            xOffsets[h] = xOffset, zOffsets[h] = zOffset;
            xOffset += float(r0.nextInt(11) - 5), zOffset += float(r0.nextInt(11) - 5);
        }
        const u32 first = u32(dyn.size());
        auto quad = [&](float sx, float sz, float ex, float ez, int seg, float top, float bottom, bool rx, bool rz, bool lx, bool lz) {
            MeshQuad q{};
            const float y0 = float(seg * 16), y1 = float((seg + 1) * 16);
            const float p[4][3] = {{sx + (rx ? bottom : -bottom), y0, sz + (rz ? bottom : -bottom)},
                                   {ex + (rx ? top : -top), y1, ez + (rz ? top : -top)},
                                   {ex + (lx ? top : -top), y1, ez + (lz ? top : -top)},
                                   {sx + (lx ? bottom : -bottom), y0, sz + (lz ? bottom : -bottom)}};
            std::memcpy(q.pos, p, sizeof(p));
            q.normal = packNormal(0, 1, 0), q.color = 0x4C7F7373u;  // (0.45, 0.45, 0.5, 0.3)
            q.tex = kTexSolid, q.flags = kQuadNoShade;
            dyn.push_back(q);
        };
        for (int layer = 0; layer < 4 && dyn.size() + 64 <= kMaxDynamic; ++layer) {
            JavaRandom r(b.seed);
            for (int branch = 0; branch < 3; ++branch) {
                const bool trunk = branch == 0;
                const int start = 7 - branch, end = trunk ? 0 : start - 3 + 1;
                float sx = xOffsets[start] - xOffset, sz = zOffsets[start] - zOffset;
                for (int seg = start; seg >= end; --seg) {
                    const float ex = sx, ez = sz;
                    const int spread = trunk ? 5 : 15;
                    sx += float(r.nextInt(spread * 2 + 1) - spread), sz += float(r.nextInt(spread * 2 + 1) - spread);
                    float top = 0.1f + float(layer) * 0.2f, bottom = top;
                    if (trunk) top *= float(seg) * 0.1f + 1.0f, bottom *= float(seg - 1) * 0.1f + 1.0f;
                    quad(sx, sz, ex, ez, seg, top, bottom, false, false, true, false);
                    quad(sx, sz, ex, ez, seg, top, bottom, true, false, true, true);
                    quad(sx, sz, ex, ez, seg, top, bottom, true, true, false, true);
                    quad(sx, sz, ex, ez, seg, top, bottom, false, true, false, false);
                }
            }
        }
        double p[3];
        for (int a = 0; a < 3; ++a) p[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt - cam[a];
        bolts.add(xfTranslate(float(p[0]), float(p[1]), float(p[2])), first, u32(dyn.size()) - first, 256u | 512u, 0xFFFFFFFF, 1);
    }
    // EnderDragonRenderer.submitRays: a dying dragon's light rays (dragon_rays: additive, white at the centre fading to
    // transparent magenta; the triangles drawn here as slices with stepped colours).
    static const int dragonType = assets.typeId("minecraft:ender_dragon");
    for (const auto& [id, e] : world.entities()) {
        if (e.type != dragonType || e.sp.t[1] <= 0) continue;
        const float deathTime = (float(e.sp.t[1]) + pt) / 200.0f;
        auto sample = [&](int delay, float out[2]) {  // DragonFlightHistory.get (dead: no interpolation)
            const int head = std::max(e.sp.historyPos, 0);
            const float* y = e.sp.history[(head - delay - 1) & 63];
            out[0] = y[0], out[1] = y[1];
        };
        float h7[2], h5[2], h10[2];
        sample(7, h7), sample(5, h5), sample(10, h10);
        Xf pose = xfRotY(-h7[1] * kDeg);
        pose = xfMul(pose, xfRotX((h5[0] - h10[0]) * 10.0f * kDeg));
        pose = xfMul(pose, xfTranslate(0, 0, 1));
        pose = xfMul(pose, xfScale(-1, -1, 1));
        pose = xfMul(pose, xfTranslate(0, -1.501f, 0));
        pose = xfMul(pose, xfTranslate(0, -1, -2));
        const float overDrive = std::min(deathTime > 0.8f ? (deathTime - 0.8f) / 0.2f : 0.0f, 1.0f);
        const float inner[4] = {1, 1, 1, 1.0f - overDrive}, outer[4] = {1, 0, 1, 0};
        JavaRandom r(432);
        const int rays = int(std::floor((deathTime + deathTime * deathTime) / 2.0f * 60.0f));
        const u32 first = u32(dyn.size());
        auto apply = [](const Xf& m, const float v[3], float out[3]) {
            for (int k = 0; k < 3; ++k) out[k] = m.m[k * 4] * v[0] + m.m[k * 4 + 1] * v[1] + m.m[k * 4 + 2] * v[2] + m.m[k * 4 + 3];
        };
        constexpr float kHalfSqrt3 = 0.8660254f;
        for (int i = 0; i < rays && dyn.size() + 12 <= kMaxDynamic; ++i) {
            const float a = r.nextFloat() * 6.2831853f, b = r.nextFloat() * 6.2831853f, c = r.nextFloat() * 6.2831853f;
            const float d = r.nextFloat() * 6.2831853f, f = r.nextFloat() * 6.2831853f, g = r.nextFloat() * 6.2831853f + deathTime * 1.5707963f;
            pose = xfMul(pose, xfMul(xfMul(xfRotX(a), xfRotY(b)), xfMul(xfRotZ(c), xfMul(xfMul(xfRotX(d), xfRotY(f)), xfRotZ(g)))));
            const float length = r.nextFloat() * 20.0f + 5.0f + overDrive * 10.0f, width = r.nextFloat() * 2.0f + 1.0f + overDrive * 2.0f;
            const float L[3] = {-kHalfSqrt3 * width, length, -0.5f * width}, Rr[3] = {kHalfSqrt3 * width, length, -0.5f * width},
                        B[3] = {0.0f, length, width};
            const float* tris[3][2] = {{L, Rr}, {Rr, B}, {B, L}};
            for (const auto& tri : tris)
                for (int s = 0; s < 4; ++s) {
                    const float t0 = float(s) / 4.0f, t1 = float(s + 1) / 4.0f, tm = (t0 + t1) * 0.5f;
                    float v[4][3];
                    const float pts[4][3] = {{tri[0][0] * t0, tri[0][1] * t0, tri[0][2] * t0}, {tri[0][0] * t1, tri[0][1] * t1, tri[0][2] * t1},
                                             {tri[1][0] * t1, tri[1][1] * t1, tri[1][2] * t1}, {tri[1][0] * t0, tri[1][1] * t0, tri[1][2] * t0}};
                    for (int k = 0; k < 4; ++k) apply(pose, pts[k], v[k]);
                    MeshQuad q{};
                    std::memcpy(q.pos, v, sizeof(v));
                    u32 col = 0;
                    for (int k = 0; k < 4; ++k) {
                        const float ch = inner[k] + (outer[k] - inner[k]) * tm;
                        col |= u32(std::clamp(ch, 0.0f, 1.0f) * 255.0f + 0.5f) << (k == 3 ? 24 : k * 8);  // 0xAABBGGRR
                    }
                    q.color = col, q.normal = packNormal(0, 1, 0), q.tex = kTexSolid, q.flags = kQuadNoShade;
                    dyn.push_back(q);
                }
        }
        double p[3];
        for (int k = 0; k < 3; ++k) p[k] = e.prevPos[k] + (e.pos[k] - e.prevPos[k]) * pt - cam[k];
        if (u32(dyn.size()) > first) bolts.add(xfTranslate(float(p[0]), float(p[1]), float(p[2])), first, u32(dyn.size()) - first, 256u | 512u, 0xFFFFFFFF, 1);
    }
    if (frameCounter % 64 == 0)  // forget removed bolts
        for (auto it = boltStates.begin(); it != boltStates.end();)
            it = world.entities().count(it->first) ? std::next(it) : boltStates.erase(it);
}

// ---------------------------------------------------------------------------------------------------------
// Entity event / animate particles

namespace {
void entityBox(const Entity& e, float& w, float& h) {
    double dw, dh;
    entitySize(e, dw, dh);
    w = float(dw), h = float(dh);
}
}  // namespace

void Visuals::Impl::entityEffect(const Entity& e, int event, bool animate) {
    if (!particles || !e.info) return;
    if (animate) {  // ClientPacketListener.handleAnimate
        if (event == 1 || event == 2) {
            emitters.push_back({e.id, event == 1 ? "crit" : "enchanted_hit", 0, 3});
            if (!emit(emitters.back())) emitters.pop_back();  // TrackingEmitter ticks once when created
        }
        return;
    }
    if (event == 35) {  // totem of undying: the emitter and TOTEM_USE in the entity's sound source
        if (audio::AudioSystem* a = audio::instance())
            a->play("minecraft:item.totem.use", e.category == ent::Category::Player ? audio::Category::Player : audio::Category::Hostile,
                    e.pos[0], e.pos[1], e.pos[2], 1.0f, 1.0f);
        emitters.push_back({e.id, "totem_of_undying", 0, 30});
        if (!emit(emitters.back())) emitters.pop_back();
        return;
    }
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::normal_distribution<double> g(0.0, 1.0);
    float w, h;
    entityBox(e, w, h);
    const std::string& n = e.info->name;
    // n particles at getRandomX(1), getRandomY() + dy, getRandomZ(1), gaussian * 0.02 motion (offset back by `back`).
    auto burst = [&](const char* type, int count, double dy, double back) {
        std::shared_lock lock(world.world().mutex());
        for (int i = 0; i < count; ++i) {
            const double xa = g(fxRandom) * 0.02, ya = g(fxRandom) * 0.02, za = g(fxRandom) * 0.02;
            const double x = e.pos[0] + (2 * u(fxRandom) - 1) * w, y = e.pos[1] + u(fxRandom) * h + dy,
                         z = e.pos[2] + (2 * u(fxRandom) - 1) * w;
            particles->spawn(type, x - xa * back, y - ya * back, z - za * back, xa, ya, za);
        }
    };
    const bool villager = n == "minecraft:villager";
    const bool tamable = n == "minecraft:wolf" || n == "minecraft:cat" || n == "minecraft:parrot" || n.ends_with("horse") ||
                         n == "minecraft:donkey" || n == "minecraft:mule" || n.ends_with("llama") || n == "minecraft:camel" ||
                         n == "minecraft:nautilus";
    std::shared_lock lock0(world.world().mutex(), std::defer_lock);
    auto R = [&] { return u(fxRandom); };
    auto at = [&](double s) {  // getRandomX(s), getRandomY(), getRandomZ(s)
        return std::array<double, 3>{e.pos[0] + (2 * R() - 1) * w * s, e.pos[1] + R() * h, e.pos[2] + (2 * R() - 1) * w * s};
    };
    audio::AudioSystem* au = e.silent ? nullptr : audio::instance();
    const audio::Category cat = e.category == Category::Player ? audio::Category::Player : audio::Category::Hostile;
    if (!animate && event >= 47 && event <= 68) {  // LivingEntity: items breaking, honey, drowning
        static const int kBreakSlot[22] = {0, 1, 5, 4, 3, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 6, -1, -1, 7};  // 47..68
        const int slot = kBreakSlot[event - 47];
        if (slot >= 0 && e.equipment[slot] >= 0) {
            breakItem(e.equipment[slot], e.pos, e.info->eye, e.yaw, e.pitch, e.silent, cat);
            return;
        }
        if (event == 53 || event == 54) {  // HoneyBlock.showSlideParticles / showJumpParticles
            lock0.lock();
            for (int i = 0; i < (event == 53 ? 5 : 10); ++i) particles->spawnBlock("block", "minecraft:honey_block", e.pos[0], e.pos[1], e.pos[2], 0, 0, 0);
            return;
        }
        if (event == 67) {  // makeDrownParticles
            lock0.lock();
            for (int i = 0; i < 8; ++i)
                particles->spawn("bubble", e.pos[0] + (R() - R()), e.pos[1] + (R() - R()), e.pos[2] + (R() - R()), e.vel[0], e.vel[1], e.vel[2]);
            return;
        }
    }
    if (!animate && event == 3 && (n == "minecraft:snowball" || n == "minecraft:egg" || n == "minecraft:blue_egg" || n == "minecraft:brown_egg")) {
        lock0.lock();  // Snowball / ThrownEgg: the item's shards where it hit
        for (int i = 0; i < 8; ++i) {
            if (n == "minecraft:snowball") {
                if (e.itemId >= 0) particles->itemShard(e.itemId, e.pos[0], e.pos[1], e.pos[2], 0, 0, 0);
                else particles->spawn("item_snowball", e.pos[0], e.pos[1], e.pos[2], 0, 0, 0);
            } else if (e.itemId >= 0) {
                particles->itemShard(e.itemId, e.pos[0], e.pos[1], e.pos[2], (R() - 0.5) * 0.08, (R() - 0.5) * 0.08, (R() - 0.5) * 0.08);
            }
        }
        return;
    }
    if (!animate && event == 0 && n == "minecraft:arrow") {  // Arrow: its potion's colour bursting
        const i64 color = dataInt(e, "ID_EFFECT_COLOR", -1);
        if (color != -1) {
            lock0.lock();
            for (int i = 0; i < 20; ++i) {
                const auto p = at(0.5);
                particles->spawnColored("entity_effect", p[0], p[1], p[2], 0, 0, 0, 0xFF000000u | u32(color & 0xFFFFFF));
            }
        }
        return;
    }
    if (!animate && event == 38 && n == "minecraft:dolphin") {  // Dolphin.addParticlesAroundSelf
        lock0.lock();
        for (int i = 0; i < 7; ++i) {
            const auto p = at(1.0);
            particles->spawn("happy_villager", p[0], p[1] + 0.2, p[2], g(fxRandom) * 0.01, g(fxRandom) * 0.01, g(fxRandom) * 0.01);
        }
        return;
    }
    if (!animate && event == 15 && n == "minecraft:witch") {  // Witch: drinking
        lock0.lock();
        const int count = int(fxRandom() % 35) + 10;
        for (int i = 0; i < count; ++i)
            particles->spawn("witch", e.pos[0] + g(fxRandom) * 0.13, e.pos[1] + h + 0.5 + g(fxRandom) * 0.13, e.pos[2] + g(fxRandom) * 0.13, 0, 0, 0);
        return;
    }
    if (!animate && event == 45 && n == "minecraft:fox" && e.equipment[0] >= 0) {  // Fox: eating what it holds
        lock0.lock();
        const float yaw = e.yaw * kDeg, pitch = e.pitch * kDeg;
        const double lx = -std::sin(yaw) * std::cos(pitch), lz = std::cos(yaw) * std::cos(pitch);
        for (int i = 0; i < 8; ++i) {
            double d[3] = {(R() - 0.5) * 0.1, R() * 0.1 + 0.1, 0.0};
            const double y1 = d[1] * std::cos(-pitch) - d[2] * std::sin(-pitch), z1 = d[1] * std::sin(-pitch) + d[2] * std::cos(-pitch);  // xRot
            const double x2 = d[0] * std::cos(-yaw) + z1 * std::sin(-yaw), z2 = -d[0] * std::sin(-yaw) + z1 * std::cos(-yaw);       // yRot
            particles->itemShard(e.equipment[0], e.pos[0] + lx / 2.0, e.pos[1], e.pos[2] + lz / 2.0, x2, y1 + 0.05, z2);
        }
        return;
    }
    if (!animate && (event == 40 || event == 41) && n == "minecraft:ocelot") {  // Ocelot.spawnTrustingParticles
        burst(event == 41 ? "heart" : "smoke", 7, 0.5, 0);
        return;
    }
    if (!animate && event == 69 && n == "minecraft:ravager") {  // Ravager.addRoarParticleEffects
        lock0.lock();
        for (int i = 0; i < 40; ++i) particles->spawn("poof", e.pos[0], e.pos[1] + h / 2, e.pos[2], g(fxRandom) * 0.2, g(fxRandom) * 0.2, g(fxRandom) * 0.2);
        return;
    }
    if (!animate && event == 39 && n == "minecraft:ravager") stunnedTicks[e.id] = 40;
    if (!animate && au) {  // playLocalSound in handleEntityEvent
        if (event == 16 && n == "minecraft:zombie_villager")
            au->play("minecraft:entity.zombie_villager.cure", audio::Category::Hostile, e.pos[0], e.pos[1] + e.info->eye, e.pos[2], float(1 + R()), float(R() * 0.7 + 0.3));
        else if (event == 64 && n == "minecraft:armadillo")
            au->play("minecraft:entity.armadillo.peek", audio::Category::Neutral, e.pos[0], e.pos[1], e.pos[2], 1.0f, 1.0f);
        else if (event == 4 && n == "minecraft:evoker_fangs")
            au->play("minecraft:entity.evoker_fangs.attack", audio::Category::Hostile, e.pos[0], e.pos[1], e.pos[2], 1.0f, float(R() * 0.2 + 0.85));
    }
    if (event == 60 || event == 20) burst("poof", 20, 0, 10);  // LivingEntity.makePoofParticles, Mob.spawnAnim
    else if (event == 18 && e.category == Category::Living) burst("heart", 7, 0.5, 0);  // Animal: in love
    else if ((event == 6 || event == 7) && tamable) burst(event == 7 ? "heart" : "smoke", 7, 0.5, 0);  // taming
    else if (villager && event == 12) burst("heart", 5, 1.0, 0);  // Villager.addParticlesAroundSelf
    else if (villager && event == 13) burst("angry_villager", 5, 1.0, 0);
    else if (villager && event == 14) burst("happy_villager", 5, 1.0, 0);
    else if (villager && event == 42) burst("splash", 5, 1.0, 0);
    else if (event == 46 && e.category != Category::Other) {  // LivingEntity: teleport (chorus fruit, endermen)
        std::shared_lock lock(world.world().mutex());
        for (int i = 0; i < 128; ++i) {
            const double d = i / 127.0;
            const float xa = (float(u(fxRandom)) - 0.5f) * 0.2f, ya = (float(u(fxRandom)) - 0.5f) * 0.2f,
                        za = (float(u(fxRandom)) - 0.5f) * 0.2f;
            const double x = e.prevPos[0] + (e.pos[0] - e.prevPos[0]) * d + (u(fxRandom) - 0.5) * w * 2.0;
            const double y = e.prevPos[1] + (e.pos[1] - e.prevPos[1]) * d + u(fxRandom) * h;
            const double z = e.prevPos[2] + (e.pos[2] - e.prevPos[2]) * d + (u(fxRandom) - 0.5) * w * 2.0;
            particles->spawn("portal", x, y, z, xa, ya, za);
        }
    }
}

void Visuals::Impl::breakItem(int item, const double pos[3], double eyeHeight, float yawDeg, float pitchDeg, bool silent, audio::Category category) {
    if (item < 0 || !particles) return;
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const std::string name = models.itemName(item);
    if (audio::AudioSystem* a = silent ? nullptr : audio::instance()) {  // DataComponents.BREAK_SOUND
        const char* sound = name == "shield" ? "minecraft:item.shield.break" : name == "wolf_armor" ? "minecraft:item.wolf_armor.break" : "minecraft:entity.item.break";
        a->play(sound, category, pos[0], pos[1], pos[2], 0.8f, float(0.8 + u(fxRandom) * 0.4));
    }
    const double yaw = -yawDeg * kDeg, pitch = -pitchDeg * kDeg;
    auto rot = [&](double v[3]) {  // Vec3.xRot(-xRot).yRot(-yRot)
        const double y1 = v[1] * std::cos(pitch) + v[2] * std::sin(pitch), z1 = v[2] * std::cos(pitch) - v[1] * std::sin(pitch);
        const double x2 = v[0] * std::cos(yaw) + z1 * std::sin(yaw), z2 = z1 * std::cos(yaw) - v[0] * std::sin(yaw);
        v[0] = x2, v[1] = y1, v[2] = z2;
    };
    std::shared_lock lock(world.world().mutex());
    for (int i = 0; i < 5; ++i) {  // spawnItemParticles
        double d[3] = {(u(fxRandom) - 0.5) * 0.1, u(fxRandom) * 0.1 + 0.1, 0.0};
        rot(d);
        double p[3] = {(u(fxRandom) - 0.5) * 0.3, -u(fxRandom) * 0.6 - 0.3, 0.6};
        rot(p);
        particles->itemShard(item, pos[0] + p[0], pos[1] + eyeHeight + p[1], pos[2] + p[2], d[0], d[1] + 0.05, d[2]);
    }
}

void Visuals::predictUse(const Entity& e) {
    Impl& m = *impl_;
    if (!m.game || !m.particles || !e.info) return;
    int item;
    i32 dye;
    bool foil;
    m.heldStack(36 + m.game->selectedSlot(), item, dye, foil);
    const std::string held = item >= 0 ? m.models.itemName(item) : std::string();
    const std::string& n = e.info->name;
    audio::AudioSystem* a = audio::instance();
    if (n == "minecraft:sheep" && held.ends_with("_dye") && a) {  // DyeItem.interactLivingEntity: a new colour on unsheared wool
        static const char* kColors[16] = {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                                          "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};
        const i64 wool = dataInt(e, "DATA_WOOL_ID");
        int color = -1;
        for (int i = 0; i < 16; ++i)
            if (held == std::string(kColors[i]) + "_dye") color = i;
        if (color >= 0 && !(wool & 0x10) && (wool & 15) != color)
            a->play("minecraft:item.dye.use", audio::Category::Player, e.pos[0], e.pos[1], e.pos[2], 1.0f, 1.0f);
        return;
    }
    if (held == "bucket" && !e.baby && a && (n == "minecraft:cow" || n == "minecraft:mooshroom" || n == "minecraft:goat")) {
        // AbstractCow / Goat.mobInteract: player.playSound(the milking sound) at the player
        const char* sound = n != "minecraft:goat" ? "minecraft:entity.cow.milk"
                            : dataInt(e, "DATA_IS_SCREAMING_GOAT") ? "minecraft:entity.goat.screaming.milk"
                                                                    : "minecraft:entity.goat.milk";
        const double* p = m.game->player().pos;
        a->play(sound, audio::Category::Player, p[0], p[1], p[2], 1.0f, 1.0f);
        return;
    }
    if (n == "minecraft:creeper" && (held == "flint_and_steel" || held == "fire_charge") && a) {  // Creeper.mobInteract
        std::uniform_real_distribution<float> u(0.8f, 1.2f);
        a->play(held == "fire_charge" ? "minecraft:item.firecharge.use" : "minecraft:item.flintandsteel.use", audio::Category::Hostile,
                e.pos[0], e.pos[1], e.pos[2], 1.0f, u(m.fxRandom));
        return;
    }
    if (n == "minecraft:allay" && a) {  // Allay.mobInteract: duplicating, giving, taking (the user's own sounds)
        if (dataInt(e, "DATA_DANCING") && held == "amethyst_shard" && dataInt(e, "DATA_CAN_DUPLICATE"))
            a->play("minecraft:block.amethyst_block.chime", audio::Category::Neutral, e.pos[0], e.pos[1], e.pos[2], 2.0f, 1.0f);
        else if (e.equipment[0] < 0 && item >= 0)
            a->play("minecraft:entity.allay.item_given", audio::Category::Neutral, e.pos[0], e.pos[1], e.pos[2], 2.0f, 1.0f);
        else if (e.equipment[0] >= 0 && item < 0)
            a->play("minecraft:entity.allay.item_taken", audio::Category::Neutral, e.pos[0], e.pos[1], e.pos[2], 2.0f, 1.0f);
        return;
    }
    if (!e.baby || item < 0) return;
    // AbstractHorse / Llama / Camel.handleEating, fed to a baby (fedFood): a happy villager particle as it ages up.
    const std::string& food = held;
    bool fed = false;
    if (n == "minecraft:horse" || n == "minecraft:donkey" || n == "minecraft:mule")
        for (const char* f : {"wheat", "sugar", "hay_block", "apple", "carrot", "golden_carrot", "golden_apple", "enchanted_golden_apple"})
            fed |= food == f;
    else if (n == "minecraft:llama" || n == "minecraft:trader_llama")
        fed = food == "wheat" || food == "hay_block";
    else if (n == "minecraft:camel")
        fed = food == "cactus";
    if (!fed) return;
    float w, h;
    entityBox(e, w, h);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::shared_lock lock(m.world.world().mutex());
    m.particles->spawn("happy_villager", e.pos[0] + (2 * u(m.fxRandom) - 1) * w, e.pos[1] + h * u(m.fxRandom) + 0.5,
                       e.pos[2] + (2 * u(m.fxRandom) - 1) * w, 0, 0, 0);
}

void Visuals::setVideo(int particleStatus, int weatherRadius) {
    if (impl_->particles) impl_->particles->setVideo(particleStatus, weatherRadius);
}

void Visuals::localEntityEvent(int event) {  // our own entity's events (LocalPlayer.handleEntityEvent)
    Impl& m = *impl_;
    if (!m.game || !m.particles) return;
    const Player& p = m.game->player();
    if (event == 35) {  // displayItemActivation(findTotem): the totem in a hand, else a totem of undying
        static int totem = -2;
        if (totem == -2) {
            totem = -1;
            for (int i = 0; i < 8192 && totem < 0; ++i)
                if (m.models.itemName(i) == "totem_of_undying") totem = i;
        }
        int item = -1;
        i32 dye;
        bool foil;
        for (int slot : {36 + m.game->selectedSlot(), 45}) {
            int it;
            m.heldStack(slot, it, dye, foil);
            if (it >= 0 && m.models.itemName(it) == "totem_of_undying") {
                item = it;
                break;
            }
        }
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        m.activationItem = item >= 0 ? item : totem;
        m.activationTicks = 40;
        m.activationOff[0] = u(m.fxRandom) * 2.0f - 1.0f, m.activationOff[1] = u(m.fxRandom) * 2.0f - 1.0f;
        return;
    }
    static const int kSlots[22] = {-2, 45, 5, 6, 7, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};  // 47..
    if (event >= 47 && event <= 52) {
        const int slot = kSlots[event - 47] == -2 ? 36 + m.game->selectedSlot() : kSlots[event - 47];
        int item;
        i32 dye;
        bool foil;
        m.heldStack(slot, item, dye, foil);
        m.breakItem(item, p.pos, p.eyeHeight(), p.yaw / kDeg, p.pitch / kDeg, false, audio::Category::Player);
    } else if (event == 53 || event == 54 || event == 67) {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        std::shared_lock lock(m.world.world().mutex());
        if (event == 67)
            for (int i = 0; i < 8; ++i)
                m.particles->spawn("bubble", p.pos[0] + (u(m.fxRandom) - u(m.fxRandom)), p.pos[1] + (u(m.fxRandom) - u(m.fxRandom)),
                                   p.pos[2] + (u(m.fxRandom) - u(m.fxRandom)), p.vel[0], p.vel[1], p.vel[2]);
        else
            for (int i = 0; i < (event == 53 ? 5 : 10); ++i)
                m.particles->spawnBlock("block", "minecraft:honey_block", p.pos[0], p.pos[1], p.pos[2], 0, 0, 0);
    }
}

void Visuals::Impl::waterAndSprint(const double pos[3], const double vel[3], float width, bool inWater, bool& wasIn, bool sprinting) {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    if (inWater && !wasIn) {  // doWaterSplashEffect: bubbles and splashes over the surface
        const double yt = std::floor(pos[1]) + 1.0;
        const int n = int(1.0f + width * 20.0f);
        for (int i = 0; i < n; ++i)
            particles->spawn("bubble", pos[0] + (u(fxRandom) * 2 - 1) * width, yt, pos[2] + (u(fxRandom) * 2 - 1) * width, vel[0],
                             vel[1] - u(fxRandom) * 0.2, vel[2]);
        for (int i = 0; i < n; ++i)
            particles->spawn("splash", pos[0] + (u(fxRandom) * 2 - 1) * width, yt, pos[2] + (u(fxRandom) * 2 - 1) * width, vel[0], vel[1], vel[2]);
    }
    wasIn = inWater;
    if (sprinting && !inWater) particles->sprintDust(pos[0], pos[1], pos[2], width, vel);
}

// EnderDragon.aiStep (client): the explosions while dying, else the phase's doClientTick.
void Visuals::Impl::dragonTick(const Entity& e) {
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::normal_distribution<double> gau(0.0, 1.0);
    auto R = [&] { return uni(fxRandom); };
    if (e.health <= 0) {
        particles->spawn("explosion", e.pos[0] + (R() - 0.5) * 8.0, e.pos[1] + 2.0 + (R() - 0.5) * 4.0, e.pos[2] + (R() - 0.5) * 8.0, 0, 0, 0);
        return;
    }
    {  // checkCrystals: drop a removed crystal; one tick in ten, the nearest within the box grown by 32
        auto it = dragonCrystal.find(e.id);
        if (it != dragonCrystal.end() && !world.entities().count(it->second)) dragonCrystal.erase(it);
        if (R() < 0.1) {
            int nearest = 0;
            double best = 1e300;
            for (const auto& [cid, c] : world.entities()) {
                if (!c.info || c.info->name != "minecraft:end_crystal") continue;
                const double dx = c.pos[0] - e.pos[0], dy = c.pos[1] - e.pos[1], dz = c.pos[2] - e.pos[2];
                const double hw = e.info->width * 0.5 + 32.0 + c.info->width * 0.5;
                if (std::abs(dx) >= hw || std::abs(dz) >= hw || dy <= -32.0 - c.info->height || dy >= e.info->height + 32.0) continue;
                if (const double d2 = dx * dx + dy * dy + dz * dz; d2 < best) best = d2, nearest = cid;
            }
            if (nearest) dragonCrystal[e.id] = nearest;
            else dragonCrystal.erase(e.id);
        }
    }
    if (e.mobFlags & 1) return;  // NoAI: no flight history, no phase ticks
    // EnderDragonPhaseManager.setPhase on DATA_PHASE: a new phase begins with its counters at 0.
    auto& [phase, ticks] = dragonPhase[e.id];
    const int now = int(dataInt(e, "DATA_PHASE"));
    if (now != phase) phase = now, ticks = 0;
    const bool sitting = phase >= 5 && phase <= 7;
    auto head = [&](double out[3]) {  // the head part's centre, at its middle height (getY(0.5))
        double parts[8][6];
        dragonPartBoxes(e, parts);
        for (int a = 0; a < 3; ++a) out[a] = e.pos[a] + (parts[0][a] + parts[0][a + 3]) * 0.5;
    };
    auto look = [&](double out[3]) {  // getHeadLookVector(1)
        float xRot = e.pitch;
        if (phase == 3 || phase == 4) {  // LANDING / TAKEOFF: towards the podium (the fight origin is 0, 0 here)
            int top = 319;
            while (top > -64 && world.registry().name(world.world().block(0, top, 0)).ends_with("air")) --top;
            const double dx = 0.5 - e.pos[0], dy = top + 1.5 - e.pos[1], dz = 0.5 - e.pos[2];
            const float dist = std::max(float(std::sqrt(dx * dx + dy * dy + dz * dz)) / 4.0f, 1.0f);
            xRot = -(6.0f / dist) * 1.5f * 5.0f;
        } else if (sitting) {
            xRot = -45.0f;
        }
        const float f = xRot * kDeg, g = -e.yaw * kDeg;  // Entity.calculateViewVector
        out[0] = std::sin(g) * std::cos(f), out[1] = -std::sin(f), out[2] = std::cos(g) * std::cos(f);
    };
    // The phases turn their look vector with look.yRot(..) and drop the result, so every particle uses the same one.
    if (phase == 9) {  // DragonDeathPhase
        if (ticks++ % 10 == 0)
            particles->spawn("explosion_emitter", e.pos[0] + (R() - 0.5) * 8.0, e.pos[1] + 2.0 + (R() - 0.5) * 4.0, e.pos[2] + (R() - 0.5) * 8.0, 0, 0, 0);
    } else if (phase == 3) {  // DragonLandingPhase
        double h[3], l[3];
        head(h), look(l);
        for (int i = 0; i < 8; ++i)
            particles->spawnPower("dragon_breath", h[0] + gau(fxRandom) / 2.0, h[1] + gau(fxRandom) / 2.0, h[2] + gau(fxRandom) / 2.0,
                                  -l[0] * 0.08f + e.vel[0], -l[1] * 0.3f + e.vel[1], -l[2] * 0.08f + e.vel[2], 1.0f);
    } else if (phase == 5) {  // DragonSittingFlamingPhase
        if (++ticks % 2 == 0 && ticks < 10) {
            double h[3], l[3];
            head(h), look(l);
            for (int i = 0; i < 8; ++i) {
                const double px = h[0] + gau(fxRandom) / 2.0, py = h[1] + gau(fxRandom) / 2.0, pz = h[2] + gau(fxRandom) / 2.0;
                for (int j = 0; j < 6; ++j)
                    particles->spawnPower("dragon_breath", px, py, pz, -l[0] * 0.08f * j, -l[1] * 0.6f, -l[2] * 0.08f * j, 1.0f);
            }
        }
    } else if (phase == 7 && !e.silent) {  // DragonSittingAttackingPhase: a growl every tick
        if (audio::AudioSystem* a = audio::instance())
            a->play("minecraft:entity.ender_dragon.growl", audio::Category::Hostile, e.pos[0], e.pos[1], e.pos[2], 2.5f, 0.8f + float(R()) * 0.3f);
    }
}

void Visuals::Impl::mobTick(const Entity& e) {
    if (e.info && e.info->name == "minecraft:ender_dragon") return dragonTick(e);
    if (!e.info || e.dying) return;
    {
        u8& was = wasInWater[e.id];
        bool w0 = was != 0;
        const bool sprint = e.category == Category::Player && (e.sharedFlags & 0x08) && !(e.sharedFlags & 0x02) && e.pose != 5;
        waterAndSprint(e.pos, e.vel, e.info->width, e.inWater, w0, sprint);
        was = w0;
    }
    const std::string& n = e.info->name;
    float w, h;
    entityBox(e, w, h);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::normal_distribution<double> gau(0.0, 1.0);
    auto R = [&] { return uni(fxRandom); };
    auto G = [&] { return gau(fxRandom); };
    auto rx = [&](double s) { return e.pos[0] + (2 * R() - 1) * w * s; };  // Entity.getRandomX / Y / Z
    auto ry = [&] { return e.pos[1] + h * R(); };
    auto rz = [&](double s) { return e.pos[2] + (2 * R() - 1) * w * s; };
    audio::AudioSystem* a = e.silent ? nullptr : audio::instance();
    auto sound = [&](const char* id, audio::Category c, double x, double y, double z, float vol, float pitch) {
        if (a) a->play(id, c, x, y, z, vol, pitch);
    };
    if (n == "minecraft:blaze") {  // Blaze.aiStep
        if (int(fxRandom() % 24) == 0)
            sound("minecraft:entity.blaze.burn", audio::Category::Hostile, e.pos[0] + 0.5, e.pos[1] + 0.5, e.pos[2] + 0.5, float(1 + R()), float(R() * 0.7 + 0.3));
        for (int i = 0; i < 2; ++i) particles->spawn("large_smoke", rx(0.5), ry(), rz(0.5), 0, 0, 0);
    } else if (n == "minecraft:enderman" || n == "minecraft:endermite") {  // Enderman / Endermite.aiStep
        const double dy = n == "minecraft:enderman" ? -0.25 : 0.0;
        for (int i = 0; i < 2; ++i) particles->spawn("portal", rx(0.5), ry() + dy, rz(0.5), (R() - 0.5) * 2.0, -R(), (R() - 0.5) * 2.0);
        if (n == "minecraft:enderman") {  // onSyncedDataUpdated(DATA_CREEPY) while stared at: playStareSound
            auto& st = stareState[e.id];
            const i64 creepy = dataInt(e, "DATA_CREEPY"), stared = dataInt(e, "DATA_STARED_AT");
            // DATA_CREEPY is applied before DATA_STARED_AT, so it sees the stared-at value from before the update.
            if (st.creepy >= 0 && creepy != st.creepy && st.staredAt && i64(e.age) >= st.lastSound + 400) {
                st.lastSound = i64(e.age);
                sound("minecraft:entity.enderman.stare", audio::Category::Hostile, e.pos[0], e.pos[1] + 2.55, e.pos[2], 2.5f, 1.0f);
            }
            st.creepy = creepy, st.staredAt = stared;
        }
    } else if (n == "minecraft:glow_squid") {  // GlowSquid.aiStep
        particles->spawn("glow", rx(0.6), ry(), rz(0.6), 0, 0, 0);
    } else if (n == "minecraft:dolphin") {  // Dolphin.tick: the trail while swimming fast
        const double v2 = e.vel[0] * e.vel[0] + e.vel[1] * e.vel[1] + e.vel[2] * e.vel[2];
        if (e.inWater && v2 > 0.03) {
            const float yaw = e.yaw * kDeg, pitch = e.pitch * kDeg;
            const double vx = -std::sin(yaw) * std::cos(pitch), vy = -std::sin(pitch), vz = std::cos(yaw) * std::cos(pitch);
            const float c = std::cos(yaw) * 0.3f, s = std::sin(yaw) * 0.3f, mult = 1.2f - float(R()) * 0.7f;
            for (int i = 0; i < 2; ++i) {
                particles->spawn("dolphin", e.pos[0] - vx * mult + c, e.pos[1] - vy, e.pos[2] - vz * mult + s, 0, 0, 0);
                particles->spawn("dolphin", e.pos[0] - vx * mult - c, e.pos[1] - vy, e.pos[2] - vz * mult - s, 0, 0, 0);
            }
        }
    } else if (n == "minecraft:bee") {  // Bee.tick: nectar drips while it carries some
        if ((dataInt(e, "DATA_FLAGS_ID") & 8) && R() < 0.05) {
            const int count = int(fxRandom() % 2) + 1;
            for (int i = 0; i < count; ++i)
                particles->spawn("falling_nectar", e.pos[0] - 0.3 + R() * 0.6, e.pos[1] + h * 0.5, e.pos[2] - 0.3 + R() * 0.6, 0, 0, 0);
        }
    } else if (n == "minecraft:phantom") {  // Phantom.tick: the flap sound at the top of the beat, mycelium off the wings
        const float off = float(e.id * 3);
        const float anim = std::cos((off + float(e.age)) * 7.448451f * kDeg + kPi);
        const float next = std::cos((off + float(e.age) + 1.0f) * 7.448451f * kDeg + kPi);
        if (anim > 0.0f && next <= 0.0f)
            sound("minecraft:entity.phantom.flap", audio::Category::Hostile, e.pos[0], e.pos[1], e.pos[2], float(0.95 + R() * 0.05), float(0.95 + R() * 0.05));
        const float width = w * 1.48f;
        const float c = std::cos(e.yaw * kDeg) * width, s = std::sin(e.yaw * kDeg) * width;
        const float hh = (0.3f + anim * 0.45f) * h * 2.5f;
        particles->spawn("mycelium", e.pos[0] + c, e.pos[1] + hh, e.pos[2] + s, 0, 0, 0);
        particles->spawn("mycelium", e.pos[0] - c, e.pos[1] + hh, e.pos[2] - s, 0, 0, 0);
    } else if (n == "minecraft:arrow" || n == "minecraft:spectral_arrow" || n == "minecraft:trident") {  // AbstractArrow.tick
        if (!e.inGround) {
            if (e.inWater)
                for (int i = 0; i < 4; ++i)
                    particles->spawn("bubble", e.pos[0] - e.vel[0] * 0.25, e.pos[1] - e.vel[1] * 0.25, e.pos[2] - e.vel[2] * 0.25, e.vel[0], e.vel[1], e.vel[2]);
            if (dataInt(e, "ID_FLAGS") & 1)  // a critical arrow
                for (int i = 0; i < 4; ++i)
                    particles->spawn("crit", e.pos[0] + e.vel[0] * i / 4.0, e.pos[1] + e.vel[1] * i / 4.0, e.pos[2] + e.vel[2] * i / 4.0, -e.vel[0],
                                     -e.vel[1] + 0.2, -e.vel[2]);
            if (n == "minecraft:spectral_arrow") particles->spawnColored("effect", e.pos[0], e.pos[1], e.pos[2], 0, 0, 0, 0xFFFFFFFF);
        }
    } else if (n == "minecraft:fireball" || n == "minecraft:small_fireball" || n == "minecraft:wither_skull" ||
               n == "minecraft:dragon_fireball") {  // AbstractHurtingProjectile.tick: the trail (and bubbles in water)
        if (e.inWater)
            for (int i = 0; i < 4; ++i)
                particles->spawn("bubble", e.pos[0] - e.vel[0] * 0.25, e.pos[1] - e.vel[1] * 0.25, e.pos[2] - e.vel[2] * 0.25, e.vel[0], e.vel[1], e.vel[2]);
        if (n == "minecraft:dragon_fireball") particles->spawnPower("dragon_breath", e.pos[0], e.pos[1] + 0.5, e.pos[2], 0, 0, 0, 1.0f);
        else particles->spawn("smoke", e.pos[0], e.pos[1] + 0.5, e.pos[2], 0, 0, 0);
    } else if (n == "minecraft:shulker_bullet") {  // ShulkerBullet.tick
        particles->spawn("end_rod", e.pos[0] - e.vel[0], e.pos[1] - e.vel[1] + 0.15, e.pos[2] - e.vel[2], 0, 0, 0);
    } else if (n == "minecraft:eye_of_ender") {  // EyeOfEnder.spawnParticles
        const double ox = e.pos[0] - e.vel[0] * 0.25, oy = e.pos[1] - e.vel[1] * 0.25, oz = e.pos[2] - e.vel[2] * 0.25;
        if (e.inWater)
            for (int i = 0; i < 4; ++i) particles->spawn("bubble", ox, oy, oz, e.vel[0], e.vel[1], e.vel[2]);
        else
            particles->spawn("portal", ox + R() * 0.6 - 0.3, oy - 0.5, oz + R() * 0.6 - 0.3, e.vel[0], e.vel[1], e.vel[2]);
    } else if (n == "minecraft:guardian" || n == "minecraft:elder_guardian") {  // Guardian.aiStep (client)
        const bool moving = dataInt(e, "DATA_ID_MOVING") != 0;
        if (!e.inWater) {  // flopping on land
            u8& touched = wasOnGround[e.id];
            if (e.vel[1] > 0.0 && touched)
                sound(n == "minecraft:guardian" ? "minecraft:entity.guardian.flop" : "minecraft:entity.elder_guardian.flop", audio::Category::Hostile,
                      e.pos[0], e.pos[1], e.pos[2], 1.0f, 1.0f);
            touched = e.vel[1] < 0.0 && e.onGround;
        } else if (moving) {
            const float yaw = e.yaw * kDeg, pitch = e.pitch * kDeg;
            const double vx = -std::sin(yaw) * std::cos(pitch), vy = -std::sin(pitch), vz = std::cos(yaw) * std::cos(pitch);
            for (int i = 0; i < 2; ++i) particles->spawn("bubble", rx(0.5) - vx * 1.5, ry() - vy * 1.5, rz(0.5) - vz * 1.5, 0, 0, 0);
        }
        if (e.attackTarget != 0) {  // the beam's bubbles towards the target
            auto t = world.entities().find(e.attackTarget);
            if (t != world.entities().end() && t->second.info) {
                const double duration = n == "minecraft:elder_guardian" ? 60.0 : 80.0;
                const double scale = std::min(double(e.attackTime) / duration, 1.0);  // getAttackAnimationScale
                const double eye = e.pos[1] + e.info->eye;
                double dx = t->second.pos[0] - e.pos[0], dy = t->second.pos[1] + t->second.info->height * 0.5 - eye, dz = t->second.pos[2] - e.pos[2];
                const double dd = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (dd > 0) {
                    dx /= dd, dy /= dd, dz /= dd;
                    for (double dist = R(); dist < dd;) {
                        dist += 1.8 - scale + R() * (1.7 - scale);
                        particles->spawn("bubble", e.pos[0] + dx * dist, eye + dy * dist, e.pos[2] + dz * dist, 0, 0, 0);
                    }
                }
            }
        }
    } else if (n == "minecraft:warden") {  // Warden.tick (client): the heartbeat, digging particles while emerging / digging
        const float anger = std::clamp(float(dataInt(e, "CLIENT_ANGER_LEVEL")) / 80.0f, 0.0f, 1.0f);
        const int delay = 40 - int(std::floor(anger * 30.0f));
        if (delay > 0 && e.age % u64(delay) == 0)
            sound("minecraft:entity.warden.heartbeat", audio::Category::Hostile, e.pos[0], e.pos[1], e.pos[2], 5.0f, float((R() - R()) * 0.2 + 1.0));
        auto& st = stateStart[e.id];
        if (st.first != e.pose) st = {e.pose, e.age};
        if ((e.pose == 13 || e.pose == 14) && double(e.age - st.second) * 50.0 < 4500.0) {
            const int bx = int(std::floor(e.pos[0])), by = int(std::floor(e.pos[1] - 1e-5)), bz = int(std::floor(e.pos[2]));
            const u16 below = world.world().block(bx, by, bz);
            if (below) {
                const std::string key = world.registry().name(below);
                for (int i = 0; i < 30; ++i)
                    particles->spawnBlock("block", key, e.pos[0] + (R() * 1.4 - 0.7), e.pos[1], e.pos[2] + (R() * 1.4 - 0.7), 0, 0, 0);
            }
        }
    } else if (n == "minecraft:breeze") {  // Breeze.tick: ground particles, the jump trail, the whirl sound
        auto& [trail, soundTick] = breezeState[e.id];
        const int bx = int(std::floor(e.pos[0])), bz = int(std::floor(e.pos[2]));
        u16 ground = world.world().block(bx, int(std::floor(e.pos[1])), bz);
        if (!ground) ground = world.world().block(bx, int(std::floor(e.pos[1] - 1e-5)), bz);
        const std::string key = ground ? world.registry().name(ground) : std::string();
        auto groundParticles = [&](int amount) {
            if (e.vehicle >= 0 || key.empty()) return;
            for (int i = 0; i < amount; ++i) particles->spawnBlock("block", key, e.pos[0], e.pos[1], e.pos[2], 0, 0, 0);
        };
        if (e.pose == 16 || e.pose == 17 || e.pose == 0) trail = 0, groundParticles(1);  // shooting, inhaling, standing
        else if (e.pose == 15) groundParticles(20);                                     // sliding
        else if (e.pose == 6 && ++trail <= 5 && !key.empty())                            // long jumping
            for (int i = 0; i < 3; ++i)
                particles->spawnBlock("block", key, e.pos[0] + e.vel[0], e.pos[1] + e.vel[1] + 0.1, e.pos[2] + e.vel[2], 0, 0, 0);
        soundTick = soundTick == 0 ? 1 + int(fxRandom() % 80) : soundTick - 1;
        if (soundTick == 0)
            sound("minecraft:entity.breeze.whirl", audio::Category::Hostile, e.pos[0], e.pos[1], e.pos[2], float(0.8 + 0.2 * R()), float(0.7 + 0.4 * R()));
    } else if (n == "minecraft:sniffer") {  // Sniffer.tick: digging particles, the searching sound
        auto& st = stateStart[e.id];
        if (st.first != e.snifferState) st = {e.snifferState, e.age};
        const double ms = double(e.age - st.second) * 50.0;
        if (e.snifferState == 5 && ms > 1700.0 && ms < 6000.0) {  // DIGGING: at the head block's floor
            const float yaw = e.yaw * kDeg;
            const double hx = e.pos[0] - std::sin(yaw) * 2.25, hz = e.pos[2] + std::cos(yaw) * 2.25;
            const int bx = int(std::floor(hx)), by = int(std::floor(e.pos[1] + 0.2)), bz = int(std::floor(hz));
            const u16 below = world.world().block(bx, by - 1, bz);
            if (below) {
                for (int i = 0; i < 30; ++i)
                    particles->spawnBlock("block", world.registry().name(below), bx + 0.5, by + 0.5 - 0.65, bz + 0.5, 0, 0, 0);
                if (a && e.age % 10 == 0) a->blockHitSound(below, audio::Category::Neutral, e.pos, 0.5f, 0.5f);
            }
        } else if (e.snifferState == 4 && e.age % 20 == 0) {  // SEARCHING
            sound("minecraft:entity.sniffer.searching", audio::Category::Neutral, e.pos[0], e.pos[1], e.pos[2], 1.0f, 1.0f);
        }
    } else if (n == "minecraft:panda") {  // Panda.tick: afterSneeze (when the sneeze ends), addEatingParticles
        auto& ps = pandaState[e.id];
        const bool sneezing = (dataInt(e, "DATA_ID_FLAGS") & 2) != 0;
        const float eyeY = float(e.pos[1]) + h * 0.85f;
        if (ps.first && !sneezing) {
            const float by = e.bodyYaw * kDeg;
            particles->spawn("sneeze", e.pos[0] - (w + 1.0f) * 0.5 * std::sin(by), eyeY - 0.1f, e.pos[2] + (w + 1.0f) * 0.5 * std::cos(by), e.vel[0], 0,
                             e.vel[2]);
        }
        ps.first = sneezing;
        const int eat = int(dataInt(e, "EAT_COUNTER"));
        if (eat > 0 && eat != ps.second && eat % 5 == 0 && e.equipment[0] >= 0) {
            const float xr = -e.pitch * kDeg, yr = -e.yaw * kDeg, br = -e.bodyYaw * kDeg;
            for (int i = 0; i < 6; ++i) {
                double vx = (R() - 0.5) * 0.1, vy = R() * 0.1 + 0.1, vz = (R() - 0.5) * 0.1;
                double t = vy * std::cos(xr) + vz * std::sin(xr);  // Vec3.xRot
                vz = vz * std::cos(xr) - vy * std::sin(xr), vy = t;
                t = vx * std::cos(yr) + vz * std::sin(yr);  // Vec3.yRot
                vz = vz * std::cos(yr) - vx * std::sin(yr), vx = t;
                const double px = (R() - 0.5) * 0.8, py = -R() * 0.6 - 0.3, pz = 1.0 + (R() - 0.5) * 0.4;
                particles->itemShard(e.equipment[0], e.pos[0] + px * std::cos(br) + pz * std::sin(br), eyeY + 1.0 + py,
                                     e.pos[2] + pz * std::cos(br) - px * std::sin(br), vx, vy + 0.05, vz);
            }
        }
        ps.second = eat;
    } else if (n == "minecraft:ravager") {  // Ravager.aiStep: stunEffect while stunned
        auto it = stunnedTicks.find(e.id);
        if (it != stunnedTicks.end()) {
            if (fxRandom() % 6 == 0) {
                const float by = e.bodyYaw * kDeg;
                particles->spawnColored("entity_effect", e.pos[0] - w * std::sin(by) + (R() * 0.6 - 0.3), e.pos[1] + h - 0.3,
                                        e.pos[2] + w * std::cos(by) + (R() * 0.6 - 0.3), 0, 0, 0, 0xFF7F8392);
            }
            if (--it->second <= 0) stunnedTicks.erase(it);
        }
    } else if (n == "minecraft:slime" || n == "minecraft:magma_cube") {  // AbstractCubeMob.tick: particles on landing
        u8& was = wasOnGround[e.id];
        if (e.onGround && !was) {
            const float size = w * 2.0f, radius = size / 2.0f;
            for (int i = 0; i < int(size * 16.0f); ++i) {
                const float dir = float(R()) * 6.2831853f, d = float(R()) * 0.5f + 0.5f;
                particles->spawn(n == "minecraft:slime" ? "item_slime" : "flame", e.pos[0] + std::sin(dir) * radius * d, e.pos[1],
                                 e.pos[2] + std::cos(dir) * radius * d, 0, 0, 0);
            }
        }
        was = e.onGround;
    } else if (n == "minecraft:evoker" || n == "minecraft:illusioner") {  // SpellcasterIllager.tick: the spell in its hands
        const int spell = int(dataInt(e, "DATA_SPELL_CASTING_ID"));
        static const u32 kSpell[6] = {0xFF000000, 0xFFB3B3CC, 0xFF664D59, 0xFFB38033, 0xFF4D4DCC, 0xFF1A1A33};
        if (spell > 0 && spell < 6) {
            const float angle = e.bodyYaw * kDeg + std::cos(float(e.age) * 0.6662f) * 0.25f;
            const float c = std::cos(angle), s = std::sin(angle);
            particles->spawnColored("entity_effect", e.pos[0] + c * 0.6, e.pos[1] + 1.8, e.pos[2] + s * 0.6, 0, 0, 0, kSpell[spell]);
            particles->spawnColored("entity_effect", e.pos[0] - c * 0.6, e.pos[1] + 1.8, e.pos[2] - s * 0.6, 0, 0, 0, kSpell[spell]);
        }
    } else if (n == "minecraft:wither") {  // WitherBoss.aiStep: smoke at the heads, powered / invulnerable sparkles
        const bool powered = e.health <= 150.0f;
        for (int i = 0; i < 3; ++i) {
            const float angle = (e.bodyYaw + 180.0f * float(i - 1)) * kDeg;
            const double hx = i == 0 ? e.pos[0] : e.pos[0] + std::cos(angle) * 1.3, hz = i == 0 ? e.pos[2] : e.pos[2] + std::sin(angle) * 1.3;
            const double hy = e.pos[1] + (i == 0 ? 3.0 : 2.2);
            particles->spawn("smoke", hx + G() * 0.3, hy + G() * 0.3, hz + G() * 0.3, 0, 0, 0);
            if (powered && fxRandom() % 4 == 0) particles->spawnColored("entity_effect", hx + G() * 0.3, hy + G() * 0.3, hz + G() * 0.3, 0, 0, 0, 0xFFB3B380);
        }
        if (dataInt(e, "DATA_ID_INV") > 0)
            for (int i = 0; i < 3; ++i)
                particles->spawnColored("entity_effect", e.pos[0] + G(), e.pos[1] + R() * 3.3, e.pos[2] + G(), 0, 0, 0, 0xFFB3B3E6);
    }
}

bool Visuals::Impl::emit(Emitter& em) {
    auto it = world.entities().find(em.entity);
    if (it == world.entities().end() || !particles) return false;
    const Entity& e = it->second;
    float w, h;
    entityBox(e, w, h);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::shared_lock lock(world.world().mutex());
    for (int i = 0; i < 16; ++i) {
        const double xa = u(fxRandom), ya = u(fxRandom), za = u(fxRandom);
        if (xa * xa + ya * ya + za * za > 1.0) continue;
        particles->spawn(em.type, e.pos[0] + w * xa / 4.0, e.pos[1] + h * (0.5 + ya / 4.0), e.pos[2] + w * za / 4.0, xa, ya + 0.2, za);
    }
    return ++em.life < em.lifetime;
}

void Visuals::Impl::tickEmitters() {
    for (size_t i = 0; i < emitters.size();)
        if (emit(emitters[i])) ++i;
        else emitters.erase(emitters.begin() + ptrdiff_t(i));
}

// ---------------------------------------------------------------------------------------------------------
// First-person hands (HUD stage)

void Visuals::Impl::buildHand(const PassContext& ctx, float viewProj[16], bool jittered, float* noJitter) {
    if (!game || !ui || !avatarReady) return;
    // ScreenEffectRenderer.renderItemActivationAnimation (unless F1) in any view; the hands only in first person,
    // not as a spectator (no hands), with F1 or asleep (GameRenderer.renderItemInHand).
    const bool activation = activationTicks > 0 && activationItem >= 0 && !ui->hudHidden();
    const bool hands = game->cameraMode == 0 && !(gameMode && gameMode() == 3) && !ui->hudHidden() && !ui->inBed();
    if (!activation && !hands) return;
    const CameraState cam = game->handCamera();  // the hand bobs by itself (bobView below), not with the world
    // Projection like the renderer's, without jitter.
    const float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    const Vec3 fwd{-sy * cp, -sp, cy * cp};
    const Vec3 right{-cy, 0, -sy};  // from the yaw alone (fwd x up flips at +-90 degrees of pitch)
    const Vec3 up = cross(right, fwd);
    Mat4 proj = perspectiveReverseZ(ui->handFovRadians(), float(ctx.width) / float(std::max(ctx.height, 1u)), 0.05f);
    if (noJitter) std::memcpy(noJitter, (proj * viewFromBasis(right, up, fwd)).m, sizeof(float) * 16);
    if (jittered) proj(0, 2) = ctx.jitter[0], proj(1, 2) = ctx.jitter[1];
    const Mat4 vpm = proj * viewFromBasis(right, up, fwd);
    std::memcpy(viewProj, vpm.m, sizeof(float) * 16);
    hudLists.begin(u64(1) << 40);  // instance keys (motion vectors)
    // Vanilla view space (x right, y up, -z forward) -> camera-relative world.
    Xf view;
    const Vec3 back = fwd * -1.0f;
    view.m[0] = right.x, view.m[1] = up.x, view.m[2] = back.x;
    view.m[4] = right.y, view.m[5] = up.y, view.m[6] = back.y;
    view.m[8] = right.z, view.m[9] = up.z, view.m[10] = back.z;

    const float pt = partial;
    const Player& p = game->player();
    std::shared_lock lock(world.world().mutex());
    const u32 light = lightAt(cam.x, cam.y, cam.z);
    lock.unlock();
    if (activation) {  // the item zooming in, spinning, out again over 40 ticks (full bright)
        const float scale = (float(40 - activationTicks) + pt) / 40.0f, ts = scale * scale, tc = scale * ts;
        const float smooth = 10.25f * tc * ts - 24.95f * ts * ts + 25.5f * tc - 13.8f * ts + 4.0f * scale;
        const float pis = smooth * kPi;
        const float aspect = float(ctx.width) / float(std::max(ctx.height, 1u));
        const float offX = activationOff[0] * 0.3f * aspect, offY = activationOff[1] * 0.3f;
        Xf m = xfMul(view, xfTranslate(offX * std::abs(std::sin(pis * 2.0f)), offY * std::abs(std::sin(pis * 2.0f)), -10.0f + 9.0f * std::sin(pis)));
        m = xfMul(m, xfScale(0.8f, 0.8f, 0.8f));
        m = xfMul(m, xfRotY(900.0f * std::abs(std::sin(pis)) * kDeg));
        m = xfMul(m, xfRotX(6.0f * std::cos(scale * 8.0f) * kDeg));
        m = xfMul(m, xfRotZ(6.0f * std::cos(scale * 8.0f) * kDeg));
        hudLists.begin((u64(1) << 40) | 0xA000);
        pushItem(hudLists, models.mesh(stackOf(nullptr, activationItem, -1), contextOf(avatar, ui::ItemDisplay::Fixed, pt)), Display::Fixed, m,
                 0xFF, false);
    }
    if (!hands) return;

    // GameRenderer.renderItemInHand: bobHurt, then bobView (the game's walk distance and bob).
    Xf pose = view;
    {
        const float gpt = game->partialTick();
        if (game->dead) {
            const float duration = std::min(float(game->deathTime) + gpt, 20.0f);
            pose = xfMul(pose, xfRotZ((40.0f - 8000.0f / (duration + 200.0f)) * kDeg));
        }
        if (game->hurtTime > 0) {
            float hurt = (float(game->hurtTime) - gpt) / float(game->hurtDuration);
            hurt = std::sin(hurt * hurt * hurt * hurt * kPi);
            pose = xfMul(pose, xfRotY(-game->hurtDir * kDeg));
            pose = xfMul(pose, xfRotZ(-hurt * 14.0f * game->damageTilt * kDeg));
            pose = xfMul(pose, xfRotY(game->hurtDir * kDeg));
        }
        if (game->bobView) {
            const float wd = -(game->walkDist + (game->walkDist - game->walkDistO) * gpt);
            const float b = game->bobO + (game->bob - game->bobO) * gpt;
            pose = xfMul(pose, xfTranslate(std::sin(wd * kPi) * b * 0.5f, -std::abs(std::cos(wd * kPi) * b), 0));
            pose = xfMul(pose, xfRotZ(std::sin(wd * kPi) * b * 3.0f * kDeg));
            pose = xfMul(pose, xfRotX(std::abs(std::cos(wd * kPi - 0.2f) * b) * 5.0f * kDeg));
        }
    }
    // submitHandsWithItems: sway against the lagging view.
    const float viewX = p.pitch / kDeg, viewY = p.yaw / kDeg;
    const float xb = xBobO + (xBob - xBobO) * pt, yb = yBobO + (yBob - yBobO) * pt;
    pose = xfMul(pose, xfRotX((viewX - xb) * 0.1f * kDeg));
    pose = xfMul(pose, xfRotY((viewY - yb) * 0.1f * kDeg));
    float attack = 0;
    {
        float diff = attackAnim - oAttackAnim;
        if (diff < 0) diff += 1.0f;
        attack = swinging || swingTime > 0 || attackAnim > 0 ? oAttackAnim + diff * pt : 0.0f;
    }

    // AvatarRenderer.renderRightHand / renderLeftHand: the arm part with its sleeve (resetPose, zRot = +-0.1).
    auto drawHand = [&](const Xf& ps, bool rightArm) {
        const int mi = assets.model(avatar.slim ? "player_slim#main" : "player#main");
        if (mi < 0 || avatar.skin < 0) return;
        const Model& model = assets.modelAt(mi);
        const i16 arm = model.role(rightArm ? RightArm : LeftArm);
        if (arm < 0) return;
        const TexRect& tr = assets.rect(avatar.skin);
        std::vector<Xf>& mats = armMats;
        if (mats.size() < model.parts.size()) mats.resize(model.parts.size());
        const u32 first = u32(dyn.size());
        for (size_t i = size_t(arm); i < model.parts.size(); ++i) {
            const ModelPart& part = model.parts[i];
            if (i != size_t(arm)) {  // descendants only (pre-order: they follow the arm until the subtree ends)
                bool under = false;
                for (i32 q = part.parent; q >= 0; q = model.parts[size_t(q)].parent)
                    if (q == arm) under = true;
                if (!under) break;
            }
            // the sleeves follow the Skin Customization (PlayerModelPart LEFT_SLEEVE 2, RIGHT_SLEEVE 3)
            if ((part.name == "left_sleeve" && !(ui->settings().modelParts & 4)) || (part.name == "right_sleeve" && !(ui->settings().modelParts & 8)))
                continue;
            PartPose pp = part.pose;
            if (i == size_t(arm)) pp.zRot = rightArm ? 0.1f : -0.1f, pp.xRot = pp.yRot = 0;
            Xf l = xfTranslate(pp.x / 16.0f, pp.y / 16.0f, pp.z / 16.0f);
            if (pp.zRot != 0) l = xfMul(l, xfRotZ(pp.zRot));
            if (pp.yRot != 0) l = xfMul(l, xfRotY(pp.yRot));
            if (pp.xRot != 0) l = xfMul(l, xfRotX(pp.xRot));
            if (pp.xScale != 1 || pp.yScale != 1 || pp.zScale != 1) l = xfMul(l, xfScale(pp.xScale, pp.yScale, pp.zScale));
            mats[i] = i == size_t(arm) ? l : xfMul(mats[size_t(part.parent)], l);
            for (u32 q = part.quadFirst; q < part.quadFirst + part.quadCount && dyn.size() < kMaxDynamic; ++q) {
                const GpuQuad& g = assets.quads()[q];
                MeshQuad mq{};
                for (int k = 0; k < 4; ++k) {
                    const float pos[3] = {g.pos[k][0] / 16.0f, g.pos[k][1] / 16.0f, g.pos[k][2] / 16.0f};
                    xfPoint(mats[i], pos, mq.pos[k]);
                    mq.uv[k][0] = tr.u0 + g.uv[k][0] * tr.du, mq.uv[k][1] = tr.v0 + g.uv[k][1] * tr.dv;
                }
                float n[3];
                for (int r = 0; r < 3; ++r)
                    n[r] = mats[i].m[r * 4] * g.normal[0] + mats[i].m[r * 4 + 1] * g.normal[1] + mats[i].m[r * 4 + 2] * g.normal[2];
                mq.normal = packNormal(n[0], n[1], n[2]);
                mq.color = 0xFFFFFFFF, mq.tex = kTexEntity, mq.flags = 0;
                dyn.push_back(mq);
            }
        }
        hudLists.cut.add(ps, first, u32(dyn.size()) - first, light, 0xFFFFFFFF, 1, hudLists.next());
    };
    auto renderArm = [&](Xf ps, float inverseArmHeight, float att, bool rightArm) {
        // renderPlayerArm + AvatarRenderer.renderHand.
        const float inv = rightArm ? 1.0f : -1.0f;
        const float sq = std::sqrt(att);
        const float xs = -0.3f * std::sin(sq * kPi), ys = 0.4f * std::sin(sq * kPi * 2), zs = -0.4f * std::sin(att * kPi);
        ps = xfMul(ps, xfTranslate(inv * (xs + 0.64000005f), ys - 0.6f + inverseArmHeight * -0.6f, zs - 0.71999997f));
        ps = xfMul(ps, xfRotY(inv * 45.0f * kDeg));
        const float zRot = std::sin(att * att * kPi), yRot = std::sin(sq * kPi);
        ps = xfMul(ps, xfRotY(inv * yRot * 70.0f * kDeg));
        ps = xfMul(ps, xfRotZ(inv * zRot * -20.0f * kDeg));
        ps = xfMul(ps, xfTranslate(inv * -1.0f, 3.6f, 3.5f));
        ps = xfMul(ps, xfRotZ(inv * 120.0f * kDeg));
        ps = xfMul(ps, xfRotX(200.0f * kDeg));
        ps = xfMul(ps, xfRotY(inv * -135.0f * kDeg));
        ps = xfMul(ps, xfTranslate(inv * 5.6f, 0, 0));
        drawHand(ps, rightArm);
    };
    // Maps in hand (FirstPersonHandsAndItemsRenderer.renderMap): the background (checkerboard when the map has data),
    // the map image and all its decorations, 0.38 blocks wide.
    auto renderMap = [&](Xf ps, int mapId) {
        ps = xfMul(ps, xfRotY(kPi));
        ps = xfMul(ps, xfRotZ(kPi));
        ps = xfMul(ps, xfScale(0.38f, 0.38f, 0.38f));
        ps = xfMul(ps, xfTranslate(-0.5f, -0.5f, 0));
        ps = xfMul(ps, xfScale(1.0f / 128, 1.0f / 128, 1.0f / 128));
        const MapTex* data = mapId >= 0 ? mapTexture(mapId) : nullptr;
        const int bgRect = models.texture(data ? "map/map_background_checkerboard" : "map/map_background");
        if (bgRect >= 0 && dyn.size() < kMaxDynamic) {
            const ItemModels::Rect& r = models.rect(bgRect);
            MeshQuad q{};
            const float pos[4][3] = {{-7, 135, 0}, {135, 135, 0}, {135, -7, 0}, {-7, -7, 0}};
            const float uv[4][2] = {{r.u0, r.v0 + r.dv}, {r.u0 + r.du, r.v0 + r.dv}, {r.u0 + r.du, r.v0}, {r.u0, r.v0}};
            std::memcpy(q.pos, pos, sizeof(pos)), std::memcpy(q.uv, uv, sizeof(uv));
            q.normal = packNormal(0, 0, -1), q.color = 0xFFFFFFFF, q.tex = kTexItems, q.flags = kQuadNoShade;
            const u32 first = u32(dyn.size());
            dyn.push_back(q);
            hudLists.cut.add(ps, first, 1, light | 512u, 0xFFFFFFFF, 1, hudLists.next());
        }
        if (!data) return;
        hudLists.cut.add(ps, data->quad, 1, light | 512u, 0xFFFFFFFF, 0, hudLists.next());
        pushMapDecorations(hudLists.cut, hudLists.tr, ps, mapId, false, light | 512u);
    };
    // renderTwoHandedMap: the map held with both hands, tilted up as the player looks down.
    auto renderTwoHandedMap = [&](Xf ps, float inverseArmHeight, float att, int mapId) {
        const float sq = std::sqrt(att);
        const float ySwing = -0.2f * std::sin(att * kPi), zSwing = -0.4f * std::sin(sq * kPi);
        ps = xfMul(ps, xfTranslate(0, -ySwing / 2.0f, zSwing));
        float tilt = std::clamp(1.0f - (p.pitch / kDeg) / 45.0f + 0.1f, 0.0f, 1.0f);  // calculateMapTilt
        tilt = -std::cos(tilt * kPi) * 0.5f + 0.5f;
        ps = xfMul(ps, xfTranslate(0, 0.04f + inverseArmHeight * -1.2f + tilt * -0.5f, -0.72f));
        ps = xfMul(ps, xfRotX(tilt * -85.0f * kDeg));
        if (!(avatar.sharedFlags & 0x20)) {
            const Xf hands = xfMul(ps, xfRotY(90.0f * kDeg));
            for (int k = 0; k < 2; ++k) {  // renderMapHand: right, then left
                const float inv = k == 0 ? 1.0f : -1.0f;
                Xf h = xfMul(hands, xfRotY(92.0f * kDeg));
                h = xfMul(h, xfRotX(45.0f * kDeg));
                h = xfMul(h, xfRotZ(inv * -41.0f * kDeg));
                h = xfMul(h, xfTranslate(inv * 0.3f, -1.1f, 0.45f));
                drawHand(h, k == 0);
            }
        }
        ps = xfMul(ps, xfRotX(std::sin(sq * kPi) * 20.0f * kDeg));
        ps = xfMul(ps, xfScale(2.0f, 2.0f, 2.0f));
        renderMap(ps, mapId);
    };
    // renderOneHandedMap: the arm, and the map beside it.
    auto renderOneHandedMap = [&](Xf ps, float inverseArmHeight, float att, bool rightArm, int mapId) {
        const float inv = rightArm ? 1.0f : -1.0f;
        ps = xfMul(ps, xfTranslate(inv * 0.125f, -0.125f, 0));
        if (!(avatar.sharedFlags & 0x20)) renderArm(xfMul(ps, xfRotZ(inv * 10.0f * kDeg)), inverseArmHeight, att, rightArm);
        ps = xfMul(ps, xfTranslate(inv * 0.51f, -0.08f + inverseArmHeight * -1.2f, -0.75f));
        const float sq = std::sqrt(att), xSwing = std::sin(sq * kPi);
        ps = xfMul(ps, xfTranslate(inv * -0.5f * xSwing, 0.4f * std::sin(sq * kPi * 2) - 0.3f * xSwing, -0.3f * std::sin(att * kPi)));
        ps = xfMul(ps, xfRotX(xSwing * -45.0f * kDeg));
        ps = xfMul(ps, xfRotY(inv * xSwing * -30.0f * kDeg));
        renderMap(ps, mapId);
    };
    // FirstPersonHandsAndItemsRenderer.submitArmWithItem: the item use animations (eat / drink, bow, crossbow,
    // trident, brush, blocking with other items), else the attack swing of the swinging hand.
    const Game::ItemUse& use = game->use;
    auto applyItemArm = [&](Xf& ps, int inv, float inverseArmHeight) {  // applyItemArmTransform
        ps = xfMul(ps, xfTranslate(float(inv) * 0.56f, -0.52f + inverseArmHeight * -0.6f, -0.72f));
    };
    auto swingArm = [&](Xf& ps, int inv, float att) {  // swingArm + applyItemArmAttackTransform
        const float sq = std::sqrt(att);
        ps = xfMul(ps, xfTranslate(float(inv) * -0.4f * std::sin(sq * kPi), 0.2f * std::sin(sq * kPi * 2), -0.2f * std::sin(att * kPi)));
        const float ySwing = std::sin(att * att * kPi);
        ps = xfMul(ps, xfRotY(float(inv) * (45.0f + ySwing * -20.0f) * kDeg));
        const float xz = std::sin(sq * kPi);
        ps = xfMul(ps, xfRotZ(float(inv) * xz * -20.0f * kDeg));
        ps = xfMul(ps, xfRotX(xz * -80.0f * kDeg));
        ps = xfMul(ps, xfRotY(float(inv) * -45.0f * kDeg));
    };
    // Bow / crossbow / trident pull: the drawn-back shake and stretch (rotate(Axis.YN, invert * 45) at the end).
    auto pullTransform = [&](Xf& ps, int inv, float power, float timeHeld, float zPush) {
        if (power > 0.1f) {
            const float shake = std::sin((timeHeld - 0.1f) * 1.3f) * (power - 0.1f);
            ps = xfMul(ps, xfTranslate(0, shake * 0.004f, 0));
        }
        ps = xfMul(ps, xfTranslate(0, 0, power * zPush));
        ps = xfMul(ps, xfScale(1.0f, 1.0f, 1.0f + power * 0.2f));
        ps = xfMul(ps, xfRotY(-float(inv) * 45.0f * kDeg));
    };
    // SpearAnimations.firstPersonUse / firstPersonAttack.
    auto spearFirstPersonUse = [&](Xf& ps, int inv, float timeHeld, const KineticParams& k) {
        if (!k.valid) return;
        const SpearUse u = spearUse(k, timeHeld);
        const float fi = float(inv);
        ps = xfMul(ps, xfTranslate(fi * (u.raise * 0.15f + u.raiseEnd * -0.05f + u.sway * -0.1f + u.swaySlow * 0.005f),
                                   u.raise * -0.075f + u.raiseMiddle * 0.075f + u.swayFast * 0.01f,
                                   u.raiseStart * 0.05f + u.raiseEnd * -0.05f + u.swaySlow * 0.005f));
        const float xr = -65.0f * ease::inOutBack(u.raise) - 35.0f * u.lower + 100.0f * u.raiseBack - 0.5f * u.swayFast;
        ps = xfMul(ps, xfMul(xfTranslate(0, 0.1f, 0), xfMul(xfRotX(xr * kDeg), xfTranslate(0, -0.1f, 0))));
        const float yr = fi * (-90.0f * ease::progress(u.raise, 0.5f, 0.55f) + 90.0f * u.sway + 2.0f * u.swaySlow);
        ps = xfMul(ps, xfMul(xfTranslate(fi * 0.15f, 0, 0), xfMul(xfRotY(-yr * kDeg), xfTranslate(-fi * 0.15f, 0, 0))));
        const float hitTicks = game->kineticHitTicks >= 0 ? float(game->kineticHitTicks) + pt : -1.0f;
        ps = xfMul(ps, xfTranslate(0, -spearHitFeedback(hitTicks), 0));
    };
    auto spearFirstPersonAttack = [&](Xf& ps, int inv, float att) {
        const float start = ease::inOutSine(ease::progress(att, 0.0f, 0.05f));
        const float middle = ease::outBack(ease::progress(att, 0.05f, 0.2f));
        const float end = ease::inOutExpo(ease::progress(att, 0.4f, 1.0f));
        ps = xfMul(ps, xfTranslate(float(inv) * 0.1f * (start - middle), -0.075f * (start - end), 0.65f * (start - middle)));
        ps = xfMul(ps, xfRotX(-70.0f * (start - end) * kDeg));
        ps = xfMul(ps, xfTranslate(0, 0, -0.25f * (end - middle)));
    };
    auto renderItem = [&](Xf ps, const HandState& h, float inverseArmHeight, float att, bool rightArm, int hand) {
        if (const ui::ItemStack* held = ui->playerSlot(hand == 0 ? 36 + game->selectedSlot() : 45);
            held && held->item == h.item && ui::findComponent(*items, *held, "map_id")) {  // itemStack.has(MAP_ID)
            const int mapId = ui::mapId(*items, *held);
            if (hand == 0 && offHand.item < 0) renderTwoHandedMap(ps, inverseArmHeight, att, mapId);
            else renderOneHandedMap(ps, inverseArmHeight, att, rightArm, mapId);
            return;
        }
        const int inv = rightArm ? 1 : -1;
        const std::string name = models.itemName(h.item);
        const int charge = chargeOf(hand == 0 ? 36 + game->selectedSlot() : 45);
        // isUsingItem && useItemRemainingTicks > 0 && the use is this hand's
        const bool usingThis = use.active && use.remaining > 0 && use.hand == hand && use.item == h.item;
        const float remaining = float(use.remaining), duration = float(use.duration);
        // The item model's context: the local player holding this hand's stack (using_item, use_duration, crossbow/pull,
        // use_cycle, fishing_rod/cast, compass needles ...).
        const ui::ItemWorld& w = ui::itemWorld();
        const int menuSlot = hand == 0 ? 36 + game->selectedSlot() : 45;
        const ui::ItemStack* live = ui->playerSlot(menuSlot);
        const ui::ItemStack& stack = live && live->item == h.item ? *live : stackOf(nullptr, h.item, h.dye);
        ui::ItemContext ic;
        ic.display = rightArm ? ui::ItemDisplay::FirstRight : ui::ItemDisplay::FirstLeft;
        ic.owner = w.player;
        ic.seed = w.localId + int(ic.display);
        if (use.active && use.hand == hand && use.item == h.item)
            ic.owner.usingThis = true, ic.owner.useTicks = float(use.ticksUsing()), ic.owner.remaining = remaining;
        ic.owner.fishing = w.fishingSlot == menuSlot;
        ic.owner.selected = hand == 0;
        ic.owner.cooldown = w.cooldown ? w.cooldown(stack) : 0.0f;
        if (name == "crossbow") {
            applyItemArm(ps, inv, inverseArmHeight);
            if (usingThis && !charge) {
                ps = xfMul(ps, xfTranslate(float(inv) * -0.4785682f, -0.094387f, 0.05731531f));
                ps = xfMul(ps, xfRotX(-11.935f * kDeg));
                ps = xfMul(ps, xfRotY(float(inv) * 65.3f * kDeg));
                ps = xfMul(ps, xfRotZ(float(inv) * -9.785f * kDeg));
                const float timeHeld = duration - (remaining - pt + 1.0f);
                // the stack's CrossbowItem.getChargeDuration (Quick Charge)
                const float chargeTicks = float(std::max(1, ui::crossbowChargeTicks(ui->items(), stack)));
                pullTransform(ps, inv, std::min(timeHeld / chargeTicks, 1.0f), timeHeld, 0.04f);
            } else {
                swingArm(ps, inv, att);
                if (charge && att < 0.001f && hand == 0) {
                    ps = xfMul(ps, xfTranslate(float(inv) * -0.641864f, 0, 0));
                    ps = xfMul(ps, xfRotY(float(inv) * 10.0f * kDeg));
                }
            }
        } else if (usingThis) {
            const auto anim = ItemRules::Anim(use.anim);
            const bool customArm = anim == ItemRules::Anim::Eat || anim == ItemRules::Anim::Drink || anim == ItemRules::Anim::Spear;
            if (!customArm) applyItemArm(ps, inv, inverseArmHeight);
            switch (anim) {
                case ItemRules::Anim::Eat:
                case ItemRules::Anim::Drink: {  // applyEatTransform
                    const float curr = remaining - pt + 1.0f, scaled = curr / std::max(duration, 1.0f);
                    if (scaled < 0.8f) ps = xfMul(ps, xfTranslate(0, std::abs(std::cos(curr / 4.0f * kPi) * 0.1f), 0));
                    const float jiggle = 1.0f - std::pow(scaled, 27.0f);
                    ps = xfMul(ps, xfTranslate(jiggle * 0.6f * float(inv), jiggle * -0.5f, 0));
                    ps = xfMul(ps, xfRotY(float(inv) * jiggle * 90.0f * kDeg));
                    ps = xfMul(ps, xfRotX(jiggle * 10.0f * kDeg));
                    ps = xfMul(ps, xfRotZ(float(inv) * jiggle * 30.0f * kDeg));
                    applyItemArm(ps, inv, inverseArmHeight);
                    break;
                }
                case ItemRules::Anim::Block:
                    if (name != "shield") {
                        ps = xfMul(ps, xfTranslate(float(inv) * -0.14142136f, 0.08f, 0.14142136f));
                        ps = xfMul(ps, xfRotX(-102.25f * kDeg));
                        ps = xfMul(ps, xfRotY(float(inv) * 13.365f * kDeg));
                        ps = xfMul(ps, xfRotZ(float(inv) * 78.05f * kDeg));
                    }
                    break;
                case ItemRules::Anim::Bow: {
                    ps = xfMul(ps, xfTranslate(float(inv) * -0.2785682f, 0.18344387f, 0.15731531f));
                    ps = xfMul(ps, xfRotX(-13.935f * kDeg));
                    ps = xfMul(ps, xfRotY(float(inv) * 35.3f * kDeg));
                    ps = xfMul(ps, xfRotZ(float(inv) * -9.785f * kDeg));
                    const float timeHeld = duration - (remaining - pt + 1.0f);
                    float power = timeHeld / 20.0f;
                    power = std::min((power * power + power * 2.0f) / 3.0f, 1.0f);
                    pullTransform(ps, inv, power, timeHeld, 0.04f);
                    break;
                }
                case ItemRules::Anim::Trident: {
                    ps = xfMul(ps, xfTranslate(float(inv) * -0.5f, 0.7f, 0.1f));
                    ps = xfMul(ps, xfRotX(-55.0f * kDeg));
                    ps = xfMul(ps, xfRotY(float(inv) * 35.3f * kDeg));
                    ps = xfMul(ps, xfRotZ(float(inv) * -9.785f * kDeg));
                    const float timeHeld = duration - (remaining - pt + 1.0f);
                    pullTransform(ps, inv, std::min(timeHeld / 10.0f, 1.0f), timeHeld, 0.2f);
                    break;
                }
                case ItemRules::Anim::Brush: {  // applyBrushTransform
                    const float delta = std::fmod(remaining, 10.0f) - pt + 1.0f;
                    const float swipe = -15.0f + 75.0f * std::cos((1.0f - delta / 10.0f) * 2.0f * kPi);
                    if (!rightArm) {
                        ps = xfMul(ps, xfTranslate(0.1f, 0.83f, 0.35f));
                        ps = xfMul(ps, xfRotX(-80.0f * kDeg));
                        ps = xfMul(ps, xfRotY(-90.0f * kDeg));
                        ps = xfMul(ps, xfRotX(swipe * kDeg));
                        ps = xfMul(ps, xfTranslate(-0.3f, 0.22f, 0.35f));
                    } else {
                        ps = xfMul(ps, xfTranslate(-0.25f, 0.22f, 0.35f));
                        ps = xfMul(ps, xfRotX(-80.0f * kDeg));
                        ps = xfMul(ps, xfRotY(90.0f * kDeg));
                        ps = xfMul(ps, xfRotX(swipe * kDeg));
                    }
                    break;
                }
                case ItemRules::Anim::Bundle: swingArm(ps, inv, att); break;
                case ItemRules::Anim::Spear: {
                    ps = xfMul(ps, xfTranslate(float(inv) * 0.56f, -0.52f, -0.72f));
                    spearFirstPersonUse(ps, inv, duration - (remaining - pt + 1.0f), game->kinetic[hand & 1]);
                    break;
                }
                default: break;
            }
        } else if (game->player().spinFlag) {  // isAutoSpinAttack (riptide)
            applyItemArm(ps, inv, inverseArmHeight);
            ps = xfMul(ps, xfTranslate(float(inv) * -0.4f, 0.8f, 0.3f));
            ps = xfMul(ps, xfRotY(float(inv) * 65.0f * kDeg));
            ps = xfMul(ps, xfRotZ(float(inv) * -85.0f * kDeg));
        } else {
            applyItemArm(ps, inv, inverseArmHeight);
            if (att > 0 && swingType == 1) swingArm(ps, inv, att);
            else if (att > 0 && swingType == 2) spearFirstPersonAttack(ps, inv, att);
        }
        pushItem(hudLists, models.mesh(stack, ic), rightArm ? Display::FirstRight : Display::FirstLeft, ps, light, h.foil);
    };
    // LocalPlayer.isScoping: the spyglass overlay replaces the hands.
    if (use.active && ItemRules::Anim(use.anim) == ItemRules::Anim::Spyglass) return;
    // evaluateWhichHandsToRender: a bow or crossbow in use is drawn alone; a charged crossbow hides the other hand.
    bool drawMain = true, drawOff = true;
    {
        const std::string mainName = mainHand.item >= 0 ? models.itemName(mainHand.item) : "";
        const std::string offName = offHand.item >= 0 ? models.itemName(offHand.item) : "";
        const bool bowOrCrossbow = mainName == "bow" || offName == "bow" || mainName == "crossbow" || offName == "crossbow";
        const bool mainCharged = mainName == "crossbow" && chargeOf(36 + game->selectedSlot());
        const bool offCharged = offName == "crossbow" && chargeOf(45);
        if (bowOrCrossbow) {
            if (!use.active) {
                drawOff = !mainCharged;
            } else {
                const std::string used = models.itemName(use.item);
                if (used != "bow" && used != "crossbow") drawOff = !(use.hand == 0 && offCharged);
                else drawMain = use.hand == 0, drawOff = use.hand == 1;
            }
        }
    }
    // Main hand (right arm), then the off hand.
    const float mainSwap = mainHand.item >= 0 ? models.handInfo(mainHand.item).swapScale : 1.0f;
    const float offSwap = offHand.item >= 0 ? models.handInfo(offHand.item).swapScale : 1.0f;
    const float mainInv = mainSwap * (1.0f - (mainHand.oHeight + (mainHand.height - mainHand.oHeight) * pt));
    const float offInv = offSwap * (1.0f - (offHand.oHeight + (offHand.height - offHand.oHeight) * pt));
    // The attack swing belongs to the swinging hand; the main hand is on the main arm (Main Hand option).
    const bool mainRight = !ui || ui->settings().mainHand != 0;
    const float mainAttack = swingHand == 0 ? attack : 0.0f, offAttack = swingHand == 1 ? attack : 0.0f;
    if (drawMain) {
        if (mainHand.item < 0) {
            if (!(avatar.sharedFlags & 0x20)) renderArm(pose, mainInv, mainAttack, mainRight);  // not while invisible
        }
        else renderItem(pose, mainHand, mainInv, mainAttack, mainRight, 0);
    }
    if (drawOff && offHand.item >= 0) renderItem(pose, offHand, offInv, offAttack, !mainRight, 1);
}

// CrossbowItem.isCharged / the charge_type property of a player inventory menu slot: 0 none, 1 arrow, 2 rocket.
int Visuals::Impl::chargeOf(int menuSlot) const {
    const ui::ItemStack* s = ui ? ui->playerSlot(menuSlot) : nullptr;
    if (!s || !items) return 0;
    static int type = -2;
    if (type == -2) type = items->componentType("charged_projectiles");
    for (const auto& c : s->added) {
        if (c.type != type) continue;
        net::Reader r(reinterpret_cast<const u8*>(c.data.data()), c.data.size());
        if (r.varInt() <= 0) return 0;
        r.varInt();
        const int item = r.varInt();
        return r.ok && items->name(item) == "minecraft:firework_rocket" ? 2 : 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------
// Block destruction overlays (LevelRenderer: the block's model drawn with the crumbling render type and the
// destroy stage texture projected onto each face, SheetedDecalTextureGenerator), multiplied into the colour like the
// CRUMBLING pipeline. Shader packs and the shader pipeline blend translucently: the OIT variant of the textures
// (the same look with alpha).

void Visuals::Impl::buildCracks(const PassContext& ctx) {
    if (!game || game->destructions().empty()) return;
    // One overlay per block: the furthest stage of everyone breaking it (the progress set's last entry).
    std::unordered_map<u64, const Game::Destruction*> byPos;
    for (const auto& [id, d] : game->destructions()) {
        const double dx = d.x + 0.5 - ctx.camX, dy = d.y + 0.5 - ctx.camY, dz = d.z + 0.5 - ctx.camZ;
        if (dx * dx + dy * dy + dz * dz > 1024.0) continue;
        const u64 key = (u64(u32(d.x)) << 40) ^ (u64(u32(d.y) & 0xFFF) << 28) ^ u64(u32(d.z) & 0xFFFFFFF);
        auto [it, added] = byPos.emplace(key, &d);
        if (!added && it->second->stage < d.stage) it->second = &d;
    }
    BlockRegistry& reg = world.registry();
    std::shared_lock lock(world.world().mutex());
    for (const auto& [key, d] : byPos) {
        const bool oit = ctx.pack != nullptr || ctx.shading;
        int& rect = crackRects[oit][std::clamp(d->stage, 0, 9)];
        if (rect == -2)
            rect = models.texture(std::string(oit ? "block/destroy_oit_stage_" : "block/destroy_stage_") +
                                  std::to_string(std::clamp(d->stage, 0, 9)));
        if (rect < 0) continue;
        const ItemModels::Rect& r = models.rect(rect);
        const u16 block = world.world().block(d->x, d->y, d->z);
        const BlockInfo& b = reg.infos()[block];
        // Block entity models (ModelFeatureRenderer's crumbling pass over the renderer's parts): chests and the like
        // have no block model of their own.
        static std::vector<be::BeQuad> beQuads;
        beQuads.clear();
        if (const be::BlockEntityRenderer* ber = be::BlockEntityRenderer::instance()) ber->crackQuads(d->x, d->y, d->z, beQuads);
        if ((b.kind == KindAir && beQuads.empty()) || (b.fluid && !b.waterlogged)) continue;
        const u32 first = u32(dyn.size());
        auto quad = [&](const float (&p)[4][3]) {  // corners in order around the quad
            if (dyn.size() >= kMaxDynamic) return;
            float e1[3], e2[3], n[3];
            for (int a = 0; a < 3; ++a) e1[a] = p[1][a] - p[0][a], e2[a] = p[2][a] - p[0][a];
            n[0] = e1[1] * e2[2] - e1[2] * e2[1], n[1] = e1[2] * e2[0] - e1[0] * e2[2], n[2] = e1[0] * e2[1] - e1[1] * e2[0];
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len < 1e-9f) return;
            for (float& c : n) c /= len;
            const int axis = std::abs(n[1]) >= std::abs(n[0]) && std::abs(n[1]) >= std::abs(n[2]) ? 1
                             : std::abs(n[2]) >= std::abs(n[0])                                   ? 2
                                                                                                  : 0;
            MeshQuad q{};
            // SheetedDecalTextureGenerator repeats the texture per block: each quad in the cell of its centre.
            float cell[3];
            for (int a = 0; a < 3; ++a) cell[a] = std::floor((p[0][a] + p[1][a] + p[2][a] + p[3][a]) * 0.25f);
            for (int k = 0; k < 4; ++k) {
                for (int a = 0; a < 3; ++a) q.pos[k][a] = p[k][a] + n[a] * 0.002f;  // polygon offset
                const float x = std::clamp(p[k][0] - cell[0], 0.0f, 1.0f), y = std::clamp(p[k][1] - cell[1], 0.0f, 1.0f),
                            z = std::clamp(p[k][2] - cell[2], 0.0f, 1.0f);
                // SheetedDecalTextureGenerator: the block-local position turned by rotateY(pi), rotateX(-pi/2) and the
                // nearest direction's rotation, (u, v) = -(x, y) (repeating)
                const bool pos = n[axis] > 0;
                const float u = axis == 1 ? x : axis == 2 ? (pos ? x : 1.0f - x) : (pos ? z : 1.0f - z);
                const float v = axis == 1 ? (pos ? z : 1.0f - z) : 1.0f - y;
                q.uv[k][0] = r.u0 + u * r.du, q.uv[k][1] = r.v0 + v * r.dv;
            }
            q.normal = packNormal(n[0], n[1], n[2]);
            q.color = 0xFFFFFFFF, q.tex = kTexItems, q.flags = kQuadNoShade;
            dyn.push_back(q);
        };
        if (b.kind == KindCube) {
            static constexpr float kFaces[6][4][3] = {
                {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}, {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}},
                {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}, {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}},
                {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}, {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}};
            for (const auto& f : kFaces) quad(f);
        } else {
            for (u32 i = b.quadFirst; i < b.quadFirst + b.quadCount; ++i) {
                const ModelQuad& mq = reg.modelQuad(i);
                const float p[4][3] = {{mq.pos[0][0], mq.pos[0][1], mq.pos[0][2]}, {mq.pos[1][0], mq.pos[1][1], mq.pos[1][2]},
                                       {mq.pos[3][0], mq.pos[3][1], mq.pos[3][2]}, {mq.pos[2][0], mq.pos[2][1], mq.pos[2][2]}};
                quad(p);  // "Z" order (0 1 2 / 2 1 3) -> around (0 1 3 2)
            }
        }
        for (const be::BeQuad& bq : beQuads) {
            const float p[4][3] = {{bq.pos[0][0], bq.pos[0][1], bq.pos[0][2]}, {bq.pos[1][0], bq.pos[1][1], bq.pos[1][2]},
                                   {bq.pos[2][0], bq.pos[2][1], bq.pos[2][2]}, {bq.pos[3][0], bq.pos[3][1], bq.pos[3][2]}};
            quad(p);
        }
        const Xf m = xfTranslate(float(d->x - ctx.camX), float(d->y - ctx.camY), float(d->z - ctx.camZ));
        cracks.add(m, first, u32(dyn.size()) - first, 256 | 512, 0xFFFFFFFF, 1);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Shader packs

void Visuals::Impl::packList(PackSink& pack, const List& l, PackRole role, PackBlend blend, bool glint, bool hand) {
    // [texture]: 0 items atlas, 1 font, 2 solid (white), 3 entity atlas, 4 block texture (block atlas); glint: 0
    static thread_local std::vector<PackVertex> verts[5];
    if (glint && (l.v.empty() || !pack.glintTexture(true))) return;
    float scroll[2] = {};
    if (glint) {  // TextureTransform.setupGlintTexturing (the Glint Speed), as writeCb
        const double ms = nowSeconds() * 1000.0 * 8.0 * glintOptions().speed;
        scroll[0] = -float(std::fmod(ms, 110000.0) / 110000.0), scroll[1] = float(std::fmod(ms, 30000.0) / 30000.0);
    }
    auto unpack = [](u32 v, int shift) { return float(i8((v >> shift) & 255)) / 127.0f; };
    for (const GpuInst& g : l.v) {
        const bool dynamic = (g.flags & 1) != 0;
        const bool bright = (g.light & 256) != 0;
        const u32 sky = bright ? 15 : (g.light >> 4) & 15, block = bright ? 15 : g.light & 15;
        for (u32 i = 0; i < g.count; ++i) {
            const u32 index = g.first + i;
            const MeshQuad* q = dynamic ? (index < dyn.size() ? &dyn[index] : nullptr)
                                        : (index < cpuStore.size() ? &cpuStore[index] : nullptr);
            if (!q) break;
            const int t = glint ? 0 : q->tex == kTexItems ? 0 : q->tex == kTexFont ? 1 : q->tex == kTexSolid ? 2 : q->tex == kTexEntity ? 3 : 4;
            PackVertex v{};
            u32 color = 0;
            for (int c = 0; c < 4; ++c)
                color |= (((q->color >> (c * 8)) & 255) * ((g.color >> (c * 8)) & 255) / 255) << (c * 8);
            v.color = color;
            v.light = (block << 4) | ((sky << 4) << 16);
            v.overlay = (g.overlay & 255) ? 3u << 16 : 10u << 16;
            const float nl[3] = {unpack(q->normal, 0), unpack(q->normal, 8), unpack(q->normal, 16)};
            float n[3];
            for (int r = 0; r < 3; ++r) n[r] = g.m[r * 4] * nl[0] + g.m[r * 4 + 1] * nl[1] + g.m[r * 4 + 2] * nl[2];
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-6f;
            v.normal = packNormal(n[0] / len, n[1] / len, n[2] / len);
            v.entity = -1;
            float uv[4][2];
            for (int c = 0; c < 4; ++c) {
                uv[c][0] = q->uv[c][0], uv[c][1] = q->uv[c][1];
                if (t == 4 && !pack.atlasUv(q->tex & 0xFFFF, q->uv[c][0], q->uv[c][1], uv[c])) uv[c][0] = uv[c][1] = 0;
                if (glint) {  // visuals.hlsl VisualsVS: item-space position, rotated 10 degrees, scrolled
                    const float gx = q->pos[c][0] * 0.5f + q->pos[c][2] * 0.25f, gy = q->pos[c][1] * 0.5f + q->pos[c][2] * 0.25f;
                    const float gc = 0.98480775f, gs = 0.17364818f;
                    uv[c][0] = gx * gc - gy * gs + scroll[0], uv[c][1] = gx * gs + gy * gc + scroll[1];
                }
            }
            if (glint) v.color = glintOptions().white(), v.light = 240 | (240u << 16);  // alpha: the Glint Strength
            v.midUv[0] = (uv[0][0] + uv[2][0]) * 0.5f, v.midUv[1] = (uv[0][1] + uv[2][1]) * 0.5f;
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 3; ++r)
                    v.pos[r] = g.m[r * 4] * q->pos[c][0] + g.m[r * 4 + 1] * q->pos[c][1] + g.m[r * 4 + 2] * q->pos[c][2] + g.m[r * 4 + 3];
                v.uv[0] = uv[c][0], v.uv[1] = uv[c][1];
                verts[t].push_back(v);
            }
        }
    }
    for (int t = 0; t < 5; ++t) {
        std::vector<PackVertex>& v = verts[t];
        if (v.empty()) continue;
        PackDraw d;
        d.role = role, d.blend = blend;
        d.cull = blend == PackBlend::Opaque;
        d.blockAtlas = t == 4;
        d.texture = t == 0 ? itemTex.Get() : t == 1 ? fontTex.Get() : t == 3 ? er.atlasTexture() : nullptr;
        if (glint) d.texture = pack.glintTexture(true), d.depthWrite = false, d.depthEqual = true, d.hand = hand, d.cull = true;
        d.vertices = v.data(), d.vertexCount = u32(v.size());
        pack.draw(d);
        v.clear();
    }
}

void Visuals::Impl::packParticles(PackSink& pack, const PassContext& ctx, const fx::GpuParticle* pts, u32 count,
                                  bool translucent) {
    if (!count) return;
    // Camera right / up (world space) from the view-projection rows.
    auto vp = [&](int r, int c) { return ctx.viewProj[c * 4 + r]; };  // as BlockEntityRenderer::draw
    float right[3], up[3];
    for (int r = 0; r < 2; ++r) {
        float* out = r == 0 ? right : up;
        float l = 0;
        for (int c = 0; c < 3; ++c) l += vp(r, c) * vp(r, c);
        l = std::sqrt(std::max(l, 1e-12f));
        for (int c = 0; c < 3; ++c) out[c] = vp(r, c) / l;
    }
    static thread_local std::vector<PackVertex> verts[2];  // 0 items atlas, 1 block atlas
    static const float kXY[4][2] = {{1, -1}, {1, 1}, {-1, 1}, {-1, -1}};
    for (u32 i = 0; i < count; ++i) {
        const fx::GpuParticle& pt = pts[i];
        const int t = pt.tex == kTexItems ? 0 : 1;
        const bool bright = (pt.light & 256) != 0;
        const u32 sky = bright ? 15 : (pt.light >> 4) & 15, block = bright ? 15 : pt.light & 15;
        PackVertex v{};
        v.color = pt.color;
        v.light = (block << 4) | ((sky << 4) << 16);
        v.overlay = 10u << 16;
        v.normal = packNormal(0, 1, 0);
        v.entity = -1;
        const float cr = std::cos(pt.roll), sr = std::sin(pt.roll);
        float uv[4][2];
        for (int c = 0; c < 4; ++c) {
            const float x = kXY[c][0], y = kXY[c][1];
            uv[c][0] = x > 0 ? pt.uv[2] : pt.uv[0], uv[c][1] = y > 0 ? pt.uv[1] : pt.uv[3];
            if (t == 1 && !pack.atlasUv(pt.tex & 0xFFFF, uv[c][0], uv[c][1], uv[c])) uv[c][0] = uv[c][1] = 0;
        }
        v.midUv[0] = (uv[0][0] + uv[2][0]) * 0.5f, v.midUv[1] = (uv[0][1] + uv[2][1]) * 0.5f;
        for (int c = 0; c < 4; ++c) {
            const float x = kXY[c][0], y = kXY[c][1];
            const float rx = (x * cr - y * sr) * pt.size, ry = (x * sr + y * cr) * pt.size;
            for (int a = 0; a < 3; ++a) v.pos[a] = pt.pos[a] + right[a] * rx + up[a] * ry;
            v.uv[0] = uv[c][0], v.uv[1] = uv[c][1];
            verts[t].push_back(v);
        }
    }
    for (int t = 0; t < 2; ++t) {
        std::vector<PackVertex>& v = verts[t];
        if (v.empty()) continue;
        PackDraw d;
        d.role = translucent ? PackRole::ParticlesTranslucent : PackRole::Particles;
        d.blend = translucent ? PackBlend::Translucent : PackBlend::Opaque;
        d.depthWrite = !translucent;
        d.cull = false;
        d.blockAtlas = t == 1;
        d.texture = t == 0 ? itemTex.Get() : nullptr;
        d.vertices = v.data(), d.vertexCount = u32(v.size());
        pack.draw(d);
        v.clear();
    }
}

}  // namespace ent
