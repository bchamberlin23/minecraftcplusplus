#include "entity/visuals.h"

#include "assets/asset_source.h"
#include "assets/assets.h"
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
constexpr u32 kMaxInstances = 1 << 14, kMaxDynamic = 1 << 13, kMaxParticles = 16384;
constexpr u64 kCbBytes = 1024;
constexpr u64 kInstOffset = kCbBytes * 3;
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
    std::string playerName;
    std::function<int()> gameMode;

    // GPU
    Renderer* renderer = nullptr;
    bool gpuTried = false, gpuOk = false;
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> psoCut, psoTr, psoGlint, psoTextSee, psoText, psoPartCut, psoPartTr;
    // Shader pipeline: with the motion target (World / AfterTranslucent stages) and shadow casters.
    ComPtr<ID3D12PipelineState> psoCutMv, psoTrMv, psoGlintMv, psoPartCutMv, psoShadow;
    // Block cracks: multiplied into the colour; with the shader pipeline alpha blended into the entity layer.
    ComPtr<ID3D12PipelineState> psoCrumble, psoCrumbleMv;
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
    bool crackDraw = false;
    int crackRects[2][10] = {{-2, -2, -2, -2, -2, -2, -2, -2, -2, -2}, {-2, -2, -2, -2, -2, -2, -2, -2, -2, -2}};
    void buildCracks(const PassContext& ctx);
    double lastTime = 0;
    float partial = 0;
    int glintRect = -1;

    // Local player (FirstPersonHandsAndItems, swing, view bobbing, the third-person avatar).
    struct HandState {
        int item = -1;
        i32 dye = -1;
        bool foil = false;
        float height = 0, oHeight = 0;
    } mainHand, offHand;
    int itemSwapTicker = 0, lastMainItem = -2;
    bool swinging = false;
    int swingTime = 0;
    float attackAnim = 0, oAttackAnim = 0;
    float xBob = 0, yBob = 0, xBobO = 0, yBobO = 0, walkDist = 0, walkDistO = 0, bob = 0, bobO = 0;
    double tickClock = 0;
    double lastTickPos[3] = {};
    bool haveTickPos = false;
    Entity avatar;
    bool avatarReady = false;
    std::vector<i8> skullCache;  // item id -> 1 skull, 0 not, -1 unknown
    std::vector<const ItemMesh*> skullMeshes;  // item id -> worn skull model
    std::vector<i8> mapItem;                   // item id -> 1 filled map, 0 not, -1 unknown
    const ItemMesh* frameMeshes[4] = {};       // item / glow frame, without / with a map
    std::vector<Xf> armMats;                   // scratch: arm part matrices

    // Name tags: text quads per name in the static store.
    struct Tag {
        u32 first = UINT32_MAX, glyphs = 0;
        float width = 0;
    };
    std::unordered_map<std::string, Tag> tags;

    // Paintings: painting_variant network id -> asset / size.
    struct PaintingInfo {
        std::string asset;
        int w = 1, h = 1;
        bool known = false;
        const ItemMesh* mesh = nullptr;
    };
    std::vector<PaintingInfo> paintings;

    // Timing history (exit report): CPU per frame over all stages, particles alive.
    static constexpr u32 kHistory = 512;
    float historyMs[kHistory] = {}, historyParticleMs[kHistory] = {};
    u32 historyParticles[kHistory] = {}, historyItems[kHistory] = {}, historyTags[kHistory] = {};
    u64 historyFrames = 0;
    double frameMs = 0, frameParticleMs = 0;
    void report() const;

    Impl(Visuals& v, EntityWorld& w, EntityRenderer& r) : self(v), world(w), er(r), assets(w.assets()) {}
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
    const PaintingInfo* painting(int id);
    void heldStack(int slot, int& item, i32& dye, bool& foil) const;
    int chargeOf(int menuSlot) const;

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
    m.world.onEntityEvent = [&m](const Entity& e, int event) {  // FireworkRocketEntity.handleEntityEvent: 17 explodes
        if (event != 17 || !m.particles || !e.info || e.info->name != "minecraft:firework_rocket") return;
        std::shared_lock lock(m.world.world().mutex());
        m.particles->fireworks(e.pos, e.vel, e.fireworks, !e.silent);
    };
    m.particles->entityPosition = [&world = m.world](int id, double pos[3]) {  // vibrations travelling to an entity
        const Entity* e = world.entity(id);
        if (e) std::copy(e->pos, e->pos + 3, pos);
        return e != nullptr;
    };
    m.loaded = true;
    LOG("visuals %7.1f ms  item models, font and particles from the game", t.ms());
    return true;
}

void Visuals::attach(net::Client& client) {
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

void Visuals::setPlayer(Game& game, ui::Ui& ui, const std::string& name, std::function<int()> gameMode) {
    Impl& m = *impl_;
    m.game = &game;
    m.ui = &ui;
    ui.setItemModels(&m.models);
    m.playerName = name;
    m.gameMode = std::move(gameMode);
    game.onSwing = [&m] {  // LivingEntity.swing
        if (!m.swinging || m.swingTime >= 3 || m.swingTime < 0) m.swingTime = -1, m.swinging = true;
    };
    game.onBlockChanged = [&m, prev = std::move(game.onBlockChanged)](bool place, int x, int y, int z, u16 state) {
        if (prev) prev(place, x, y, z, state);
        if (!place && m.particles) {  // Block.playerWillDestroy -> level event 2001, played locally
            std::shared_lock lock(m.world.world().mutex());
            m.particles->blockDestroyed(x, y, z, state);
        }
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
            m.tagSee.clear(), m.tagNormal.clear();
            m.buildTags(ctx);
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
            const bool a = m.flush(m.tagSee), b = m.flush(m.tagNormal);
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
    enum Blend { Opaque, Alpha, Glint, Crumble };
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
        }
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.DepthStencilState.DepthEnable = depthTest ? TRUE : FALSE;
        d.DepthStencilState.DepthWriteMask = depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reversed Z
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = targets == 0 ? 0 : targets == 1 ? 1 : 2;
        d.RTVFormats[0] = targets ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_UNKNOWN;
        if (targets >= 2) {
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
        !pso(SH(ParticleVS), SH(ParticleCutoutMvPS), Opaque, true, true, psoPartCutMv, 2) ||
        !pso(SH(VisualsShadowVS), SH(VisualsShadowPS), Opaque, true, true, psoShadow, 0))
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
    // TextureTransform.setupGlintTexturing: offsets scrolling with time (glint speed 0.5).
    const double ms = nowSeconds() * 1000.0 * 8.0 * 0.5;
    k.glint[0] = -float(std::fmod(ms, 110000.0) / 110000.0), k.glint[1] = float(std::fmod(ms, 30000.0) / 30000.0);
    k.glint[2] = glintRect >= 0 ? 0.75f : 0.0f;
    k.glint[3] = hand ? 1.0f : 0.0f;
    std::memcpy(frameBase + kCbBytes * u64(slot), &k, sizeof(k));
}

// World stage lists (items, frames, paintings, held items), their shadow-only casters and, with the shader pipeline,
// the first-person hand; instances and dynamic quads uploaded (prepare, so the shadow maps can use them).
void Visuals::Impl::prepareWorld(const PassContext& ctx) {
    preparedFrame = frameCounter;
    frameCam[0] = ctx.camX, frameCam[1] = ctx.camY, frameCam[2] = ctx.camZ;
    worldLists.clear(), shadowLists.clear(), cracks.clear();
    buildWorld(ctx);
    buildCracks(ctx);
    const bool handInWorld = ctx.shading && ctx.prevViewProj;
    float handVp[16], handPlain[16];
    if (handInWorld) {
        hudLists.clear();
        buildHand(ctx, handVp, true, handPlain);
    }
    uploadPending(ctx);
    writeCb(0, ctx, ctx.viewProj, ctx.shading, false);
    worldDraw[0] = flush(worldLists.cut), worldDraw[1] = flush(worldLists.tr), worldDraw[2] = flush(worldLists.glint);
    crackDraw = flush(cracks);
    shadowDraw[0] = flush(shadowLists.cut), shadowDraw[1] = flush(shadowLists.tr);
    if (handInWorld && !(hudLists.cut.v.empty() && hudLists.tr.v.empty())) {
        writeCb(2, ctx, handVp, true, true, prevHandValid ? prevHandVp : handPlain);
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

void Visuals::Impl::heldStack(int slot, int& item, i32& dye, bool& foil) const {
    item = -1, dye = -1, foil = false;
    if (!ui) return;
    if (const ui::ItemStack* s = ui->playerSlot(slot)) {
        item = s->item;
        dye = s->dyedColor >= 0 ? i32(s->dyedColor & 0xFFFFFF) : -1;
        foil = items && items->foil(*s);
    }
}

const Visuals::Impl::PaintingInfo* Visuals::Impl::painting(int id) {
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

const Visuals::Impl::Tag& Visuals::Impl::tag(const std::string& name) {
    auto it = tags.find(name);
    if (it != tags.end()) return it->second;
    Tag t;
    if (font && tags.size() < 4096) {
        canvas.begin(1u << 14, 1u << 14, 1);
        const ui::Text text(name);
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

// ---------------------------------------------------------------------------------------------------------
// Local player state (20 Hz)

void Visuals::Impl::tickLocal() {
    Player& p = game->player();
    // LivingEntity.updateSwingTime (swing duration 6).
    oAttackAnim = attackAnim;
    if (swinging) {
        if (++swingTime >= 6) swingTime = 0, swinging = false;
    } else {
        swingTime = 0;
    }
    attackAnim = float(swingTime) / 6.0f;
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
    if (p.onGround) walkDist += float(dist) * 0.6f;
    bobO = bob;
    const float tBob = p.onGround && !p.flying && !p.inWater ? std::min(0.1f, float(dist)) : 0.0f;
    bob += (tBob - bob) * 0.4f;
    // FirstPersonHandsAndItems.tick.
    int selected = game->selectedSlot();
    int nextMain, nextOff;
    i32 dyeMain, dyeOff;
    bool foilMain, foilOff;
    heldStack(36 + selected, nextMain, dyeMain, foilMain);
    heldStack(45, nextOff, dyeOff, foilOff);
    ++itemSwapTicker;
    if (nextMain != lastMainItem) {  // Player.tick: a different item resets the attack strength / swap tickers
        if (lastMainItem != -2) itemSwapTicker = 0;
        lastMainItem = nextMain;
    }
    mainHand.oHeight = mainHand.height, offHand.oHeight = offHand.height;
    auto sameVisible = [&](const HandState& h, int item, i32 dye) {
        if (h.item == item && h.dye == dye) return true;
        return item >= 0 && !models.handInfo(item).swapAnimation;
    };
    if (sameVisible(mainHand, nextMain, dyeMain)) mainHand.item = nextMain, mainHand.dye = dyeMain, mainHand.foil = foilMain;
    if (sameVisible(offHand, nextOff, dyeOff)) offHand.item = nextOff, offHand.dye = dyeOff, offHand.foil = foilOff;
    const float speed = nextMain >= 0 ? models.handInfo(nextMain).attackSpeed : 4.0f;
    const float delay = 20.0f / std::max(speed, 0.01f);
    const float swap = std::clamp((float(itemSwapTicker) + 1.0f) / delay, 0.0f, 1.0f);
    if (game->handsBusy) {  // FirstPersonHandsAndItems.tick: busy hands (paddling) go down
        mainHand.height = std::clamp(mainHand.height - 0.4f, 0.0f, 1.0f);
        offHand.height = std::clamp(offHand.height - 0.4f, 0.0f, 1.0f);
    } else {
        const float mainTarget = (mainHand.item != nextMain || mainHand.dye != dyeMain) ? 0.0f : swap * swap * swap;
        const float offTarget = (offHand.item != nextOff || offHand.dye != dyeOff) ? 0.0f : 1.0f;
        mainHand.height += std::clamp(mainTarget - mainHand.height, -0.4f, 0.4f);
        offHand.height += std::clamp(offTarget - offHand.height, -0.4f, 0.4f);
    }
    if (mainHand.height < 0.1f) mainHand.item = nextMain, mainHand.dye = dyeMain, mainHand.foil = foilMain;
    if (offHand.height < 0.1f) offHand.item = nextOff, offHand.dye = dyeOff, offHand.foil = foilOff;

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
}

void Visuals::Impl::update() {
    const double now = nowSeconds();
    const double dt = lastTime > 0 ? std::clamp(now - lastTime, 0.0, 0.25) : 0.0;
    lastTime = now;
    self.stats_.items = self.stats_.tags = self.stats_.instances = 0;
    if (game) {
        tickClock += dt;
        int n = 0;
        while (tickClock >= 0.05 && n < 5) tickClock -= 0.05, ++n, tickLocal();
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
        e.pose = p.pose == Player::Pose::Crouching ? 5 : p.pose == Player::Pose::Swimming ? 3 : p.pose == Player::Pose::FallFlying ? 1 : 0;
        e.sharedFlags = u8(p.pose == Player::Pose::FallFlying ? 0x80 : 0);
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
        e.livingFlags = game->use.active ? u8(1 | (game->use.hand ? 2 : 0)) : 0;
        e.useTicks = game->use.ticksUsing();
        e.chargedCrossbow = u8((chargeOf(36 + game->selectedSlot()) ? 1 : 0) | (chargeOf(45) ? 2 : 0));
        e.swingTime = swinging ? std::max(swingTime, 0) : -1;
        e.swingDuration = 6;
        if (const PlayerInfo* info = world.playerNamed(playerName)) e.skin = info->skin, e.slim = info->slim;
        else if (e.skin < 0) e.skin = assets.texture("entity/player/wide/steve"), e.slim = false;
        auto equip = [&](int slot, int menuSlot) {
            bool foil;
            heldStack(menuSlot, e.equipment[slot], e.equipDye[slot], foil);
            e.equipFoil = u8((e.equipFoil & ~(1 << slot)) | (foil ? 1 << slot : 0));
        };
        equip(0, 36 + game->selectedSlot());
        equip(1, 45);
        equip(5, 5), equip(4, 6), equip(3, 7), equip(2, 8);
        const bool spectator = gameMode && gameMode() == 3;
        er.setLocalPlayer(avatarReady && game->cameraMode != 0 && !spectator ? &avatar : nullptr);
        er.setLocalPlayerShadow(avatarReady && game->cameraMode == 0 && !spectator ? &avatar : nullptr);  // first person
    }
    animClock += dt;
    models.tick(u64(animClock * 20.0));  // animated textures in the item atlas (block items, vibration particles)
    // FireworkRocketEntity.tick on the client: a spark behind the rocket every tick.
    for (; particles && rocketTicks < u64(animClock * 20.0); ++rocketTicks) {
        static std::normal_distribution<double> gauss;
        static std::mt19937 rng(17);
        std::shared_lock lock(world.world().mutex());
        for (const auto& [id, e] : world.entities())
            if (e.info && e.info->name == "minecraft:firework_rocket")
                particles->spawn("firework", e.pos[0], e.pos[1], e.pos[2], gauss(rng) * 0.05, -(e.pos[1] - e.prevPos[1]) * 0.5,
                                 gauss(rng) * 0.05);
    }
    if (particles) {
        const CameraState c = game ? game->viewCamera() : CameraState{};
        const double cam[3] = {c.x, c.y, c.z};
        particles->frame(dt, cam);
        frameParticleMs += particles->stats().tickMs;
    }
}

// ---------------------------------------------------------------------------------------------------------
// World stage: held / worn items, item frames, paintings, dropped items

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
        const bool rightHanded = e->mainArm != 0;
        for (int k = 0; k < 2; ++k) {
            if (!(a.valid & (1 << k))) continue;
            const int slot = (k == 0) == rightHanded ? 0 : 1;  // right arm holds the main hand when right handed
            const int item = e->equipment[slot];
            if (item < 0) continue;
            Xf m;
            std::memcpy(m.m, a.hand[k], sizeof(m.m));
            m = xfMul(m, xfRotX(-90.0f * kDeg));
            m = xfMul(m, xfRotY(180.0f * kDeg));
            const float ox = a.babyOffset ? 0.0f : 1.0f, oy = a.babyOffset ? 1.0f : 2.0f, oz = a.babyOffset ? -4.5f : -10.0f;
            m = xfMul(m, xfTranslate((k == 1 ? -1.0f : 1.0f) * ox / 16.0f, oy / 16.0f, oz / 16.0f));
            L.begin((u64(u32(a.entity)) << 4 | u64(k)) + 1);
            pushItem(L, models.mesh(item, e->equipDye[slot], false), k == 0 ? Display::ThirdRight : Display::ThirdLeft,
                     m, a.light, (e->equipFoil >> slot) & 1);
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
                    m = xfMul(m, xfScale(1.1875f, 1.1875f, 1.1875f));
                    pushRaw(L, *skullMeshes[size_t(head)], m, a.light);
                } else {  // translateToHead
                    m = xfMul(m, xfTranslate(0, -0.25f, 0));
                    m = xfMul(m, xfRotY(180.0f * kDeg));
                    m = xfMul(m, xfScale(0.625f, -0.625f, -0.625f));
                    pushItem(L, models.mesh(head, e->equipDye[5], false), Display::Head, m, a.light, (e->equipFoil >> 5) & 1);
                }
            }
        }
    }
    // Item frames, paintings, dropped items.
    static int frameType = -2, glowFrameType = -2, paintingType = -2;
    if (frameType == -2)
        frameType = assets.typeId("minecraft:item_frame"), glowFrameType = assets.typeId("minecraft:glow_item_frame"),
        paintingType = assets.typeId("minecraft:painting");
    const float maxDist = std::min(ctx.fogEnd, 128.0f);
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
    for (const auto& [id, e] : entities) {
        const bool frame = e.type == frameType || e.type == glowFrameType;
        if (!frame && e.type != paintingType && e.category != Category::Item) continue;
        double p[3];
        for (int a = 0; a < 3; ++a) p[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt;
        const float rx = float(p[0] - cam[0]), ry = float(p[1] - cam[1]), rz = float(p[2] - cam[2]);
        const float radius = e.type == paintingType ? 4.0f : 1.0f;
        Lists* target = &worldLists;
        if (!visible(rx, ry, rz, radius)) {
            if (!inShadow(rx, ry, rz, radius)) continue;
            target = &shadowLists;
        }
        Lists& L = *target;
        L.begin(u64(u32(id)) << 4 | 3);
        const u32 light = lightAt(p[0], p[1], p[2]);
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
            bool map = false;
            if (e.itemId >= 0) {
                if (mapItem.size() <= size_t(e.itemId)) mapItem.resize(size_t(e.itemId) + 1, -1);
                i8& mi = mapItem[size_t(e.itemId)];
                if (mi < 0) mi = models.itemName(e.itemId) == "filled_map" ? 1 : 0;
                map = mi == 1;
            }
            u32 frameLight = light;
            if (glow) frameLight = (light & 0xF0) | std::max<u32>(light & 15, 5);
            if (!invisible) {
                const ItemMesh*& fm = frameMeshes[(glow ? 2 : 0) + (map ? 1 : 0)];
                if (!fm) fm = &models.blockModelMesh(std::string("block/") + (glow ? "glow_item_frame" : "item_frame") + (map ? "_map" : ""));
                pushRaw(L, *fm, xfMul(m, xfTranslate(-0.5f, -0.5f, -0.5f)), frameLight);
            }
            if (e.itemId >= 0) {
                Xf im = xfMul(m, xfTranslate(0, 0, invisible ? 0.5f : 0.4375f));
                im = xfMul(im, xfRotZ(float(e.rotation % 8) * 45.0f * kDeg));
                im = xfMul(im, xfScale(0.5f, 0.5f, 0.5f));
                pushItem(L, models.mesh(e.itemId, e.itemDye, true), Display::Fixed, im, glow ? 0xFFu : light, e.itemFoil);
            }
            continue;
        }
        if (e.type == paintingType) {  // PaintingRenderer
            PaintingInfo* info = const_cast<PaintingInfo*>(painting(e.painting));
            if (!info) continue;
            if (!info->mesh) info->mesh = &models.paintingMesh(info->asset, info->w, info->h);
            static const int k2d[6] = {0, 0, 2, 0, 1, 3};  // 3D data value -> 2D data value (S W N E)
            const int d2 = k2d[std::clamp(e.direction, 0, 5)];
            Xf m = xfMul(xfTranslate(rx, ry, rz), xfRotY(float(180 - d2 * 90) * kDeg));
            pushRaw(L, *info->mesh, m, light);
            continue;
        }
        // ItemEntityRenderer: bob, spin and several copies for larger stacks (GROUND display).
        if (e.itemId < 0) continue;
        const ItemMesh& mesh = models.mesh(e.itemId, e.itemDye, true);
        if (mesh.empty()) continue;
        const float age = float(e.age) + pt;
        const float bobY = std::sin(age / 10.0f + e.bobOffset) * 0.1f + 0.1f;
        const float spin = age / 20.0f + e.bobOffset;
        Xf m = xfTranslate(rx, ry + bobY + (-mesh.groundMinY + 0.0625f), rz);
        m = xfMul(m, xfRotY(spin));
        const int c = e.itemCount;
        const int amount = c > 48 ? 5 : c > 32 ? 4 : c > 16 ? 3 : c > 1 ? 2 : 1;
        u32 rng = u32(e.itemId) * 2654435761u + 7;
        auto rnd = [&] {
            rng = rng * 1664525u + 1013904223u;
            return float(rng >> 8) / float(1 << 24) * 2.0f - 1.0f;
        };
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
    // Crosshair entity (named mobs show their name when looked at).
    int targeted = -1;
    if (game) {
        const CameraState c = game->camera();
        const double eye[3] = {c.x, c.y, c.z}, cp = std::cos(c.pitch);
        const double dir[3] = {-std::sin(c.yaw) * cp, -std::sin(c.pitch), std::cos(c.yaw) * cp};
        const double limit = game->target().hit ? std::min(game->target().distance, 5.0) : 5.0;
        targeted = world.pick(eye, dir, limit).id;
    }
    std::shared_lock lock(world.world().mutex());
    for (const auto& [id, e] : world.entities()) {
        if (!e.info || e.name.empty() || e.deathTime > 0) continue;
        if (e.category == Category::Living && (e.sharedFlags & 0x20)) continue;
        const bool player = e.category == Category::Player;
        if (!player && !(e.nameVisible || id == targeted)) continue;
        if (!player && e.category != Category::Living && !e.nameVisible) continue;
        double p[3];
        for (int a = 0; a < 3; ++a) p[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt;
        const bool discrete = e.pose == 5;  // crouching
        float h = e.info->height;
        if (player && discrete) h = 1.5f;
        if (player && (e.pose == 1 || e.pose == 3 || e.pose == 4)) h = 0.6f;  // gliding, swimming / crawling, riptide
        if (e.baby && e.category == Category::Living) h *= 0.5f;
        if (e.pose == 2) h = 0.2f;  // sleeping (LivingEntity.SLEEPING_DIMENSIONS)
        const float x = float(p[0] - cam[0]), y = float(p[1] - cam[1]) + h + 0.5f, z = float(p[2] - cam[2]);
        const float d2 = x * x + y * y + z * z;
        if (d2 > 64.0f * 64.0f || (discrete && d2 >= 1024.0f)) continue;
        if (fwd[0] * x + fwd[1] * y + fwd[2] * z < -1.0f) continue;  // behind the camera
        const Tag& t = tag(e.name);
        if (t.first == UINT32_MAX) continue;
        Xf m;
        const float s = 0.025f;
        for (int r = 0; r < 3; ++r) m.m[r * 4] = right[r] * s, m.m[r * 4 + 1] = -up[r] * s, m.m[r * 4 + 2] = fwd[r] * s;
        m.m[3] = x, m.m[7] = y, m.m[11] = z;
        const u32 light = lightAt(p[0], p[1] + e.info->eye, p[2]) | 512u;
        const u32 textHalf = 0x80FFFFFFu, bgColor = 0x40000000u;  // ABGR: white at 0.5, black at 0.25
        if (!discrete) {
            tagSee.add(m, t.first, 1, light, bgColor);
            tagSee.add(m, t.first + 1, t.glyphs, light, textHalf);
            const u32 bright = (light & ~15u) | std::max<u32>(light & 15, 2);  // lightCoordsWithEmission(light, 2)
            tagNormal.add(m, t.first + 1, t.glyphs, bright, 0xFFFFFFFFu);
        } else {
            tagNormal.add(m, t.first, 1, light, bgColor);
            tagNormal.add(m, t.first + 1, t.glyphs, light, textHalf);
        }
        ++self.stats_.tags;
    }
}

// ---------------------------------------------------------------------------------------------------------
// First-person hands (HUD stage)

void Visuals::Impl::buildHand(const PassContext& ctx, float viewProj[16], bool jittered, float* noJitter) {
    if (!game || !ui || game->cameraMode != 0 || !avatarReady) return;
    if (gameMode && gameMode() == 3) return;  // spectators have no hands
    if (ui->hudHidden() || ui->inBed()) return;  // GameRenderer.renderItemInHand: F1 and sleeping hide them too
    const CameraState cam = game->handCamera();  // the hand bobs by itself (bobView below), not with the world
    // Projection like the renderer's, without jitter.
    const float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    const Vec3 fwd{-sy * cp, -sp, cy * cp};
    const Vec3 right = normalize(cross(fwd, {0, 1, 0}));
    const Vec3 up = cross(right, fwd);
    Mat4 proj = perspectiveReverseZ(ui->fovRadians(), float(ctx.width) / float(std::max(ctx.height, 1u)), 0.05f);
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
            pose = xfMul(pose, xfRotZ(-hurt * 14.0f * kDeg));
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
        // The arm part with its sleeve (resetPose, zRot = +-0.1).
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
    auto renderItem = [&](Xf ps, const HandState& h, float inverseArmHeight, float att, bool rightArm, int hand) {
        const int inv = rightArm ? 1 : -1;
        const std::string name = models.itemName(h.item);
        const int charge = chargeOf(hand == 0 ? 36 + game->selectedSlot() : 45);
        // isUsingItem && useItemRemainingTicks > 0 && the use is this hand's
        const bool usingThis = use.active && use.remaining > 0 && use.hand == hand && use.item == h.item;
        const float remaining = float(use.remaining), duration = float(use.duration);
        ItemModels::UseState us;  // the item model's using_item / use_duration / crossbow/pull / use_cycle
        if (use.active && use.hand == hand && use.item == h.item) {
            us.usingItem = true;
            us.useTicks = float(use.ticksUsing());
            us.pull = us.useTicks / 25.0f;  // CrossbowItem.getChargeDuration (no Quick Charge)
            us.remaining = remaining;
        }
        us.charge = charge;
        if (name == "crossbow") {
            applyItemArm(ps, inv, inverseArmHeight);
            if (usingThis && !charge) {
                ps = xfMul(ps, xfTranslate(float(inv) * -0.4785682f, -0.094387f, 0.05731531f));
                ps = xfMul(ps, xfRotX(-11.935f * kDeg));
                ps = xfMul(ps, xfRotY(float(inv) * 65.3f * kDeg));
                ps = xfMul(ps, xfRotZ(float(inv) * -9.785f * kDeg));
                const float timeHeld = duration - (remaining - pt + 1.0f);
                pullTransform(ps, inv, std::min(timeHeld / 25.0f, 1.0f), timeHeld, 0.04f);
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
                case ItemRules::Anim::Spear: ps = xfMul(ps, xfTranslate(float(inv) * 0.56f, -0.52f, -0.72f)); break;
                default: break;
            }
        } else {
            applyItemArm(ps, inv, inverseArmHeight);
            if (att > 0) swingArm(ps, inv, att);
        }
        pushItem(hudLists, models.mesh(h.item, h.dye, false, us), rightArm ? Display::FirstRight : Display::FirstLeft, ps, light,
                 h.foil);
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
    if (drawMain) {
        if (mainHand.item < 0) renderArm(pose, mainInv, attack, true);
        else renderItem(pose, mainHand, mainInv, attack, true, 0);
    }
    if (drawOff && offHand.item >= 0) renderItem(pose, offHand, offInv, 0.0f, false, 1);
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
        if (b.kind == KindAir || (b.fluid && !b.waterlogged)) continue;
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
            for (int k = 0; k < 4; ++k) {
                for (int a = 0; a < 3; ++a) q.pos[k][a] = p[k][a] + n[a] * 0.002f;  // polygon offset
                const float x = std::clamp(p[k][0], 0.0f, 1.0f), y = std::clamp(p[k][1], 0.0f, 1.0f),
                            z = std::clamp(p[k][2], 0.0f, 1.0f);
                const float u = axis == 1 ? x : axis == 2 ? x : z, v = axis == 1 ? z : 1.0f - y;
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
    if (glint) {  // TextureTransform.setupGlintTexturing (glint speed 0.5), as writeCb
        const double ms = nowSeconds() * 1000.0 * 8.0 * 0.5;
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
            if (glint) v.color = 0xBFFFFFFF, v.light = 240 | (240u << 16);  // alpha: glint strength 0.75 (option default)
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
