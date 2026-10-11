#include "entity/visuals.h"

#include "assets/asset_source.h"
#include "assets/assets.h"
#include "assets/game_reports.h"
#include "core/math.h"
#include "entity/entities.h"
#include "entity/entity_render.h"
#include "game/game.h"
#include "net/client.h"
#include "ui/font.h"
#include "ui/items.h"
#include "ui/resources.h"
#include "ui/ui.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <shared_mutex>

#include "ParticleCutoutPS.h"
#include "ParticleTranslucentPS.h"
#include "ParticleVS.h"
#include "VisualsCutoutPS.h"
#include "VisualsGlintPS.h"
#include "VisualsTextPS.h"
#include "VisualsTranslucentPS.h"
#include "VisualsVS.h"

namespace ent {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;

// Per-frame upload slice: three constant buffers, instances, dynamic quads, particles, staging for copies.
constexpr u32 kMaxInstances = 1 << 14, kMaxDynamic = 1 << 13, kMaxParticles = 16384;
constexpr u64 kCbBytes = 256;
constexpr u64 kInstOffset = kCbBytes * 3;
constexpr u64 kDynOffset = kInstOffset + u64(kMaxInstances) * 80;
constexpr u64 kParticleOffset = kDynOffset + u64(kMaxDynamic) * sizeof(MeshQuad);
constexpr u64 kStageOffset = (kParticleOffset + u64(kMaxParticles) * sizeof(fx::GpuParticle) + 511) & ~u64(511);
constexpr u64 kStageBytes = u64(12) << 20;
constexpr u64 kFrameBytes = (kStageOffset + kStageBytes + 65535) & ~u64(65535);

struct GpuInst {
    float m[12];
    u32 color, light, overlay, flags, first, count, prefix, pad;
};
static_assert(sizeof(GpuInst) == 80);

struct FrameCb {
    float viewProj[16];
    float fogColor[4];
    float fogParams[4];
    float light0[4], light1[4];
    float camRight[4], camUp[4];
    float glintRect[4];
    float glint[4];
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
struct List {
    std::vector<GpuInst> v;
    u32 base = 0, quads = 0;
    void clear() { v.clear(), base = 0, quads = 0; }
    void add(const Xf& m, u32 first, u32 count, u32 light, u32 color = 0xFFFFFFFF, u32 flags = 0) {
        if (!count) return;
        GpuInst g;
        std::memcpy(g.m, m.m, sizeof(g.m));
        g.color = color, g.light = light, g.overlay = 0, g.flags = flags, g.first = first, g.count = count, g.prefix = 0, g.pad = 0;
        v.push_back(g);
    }
};
struct Lists {
    List cut, tr, glint;
    void clear() { cut.clear(), tr.clear(), glint.clear(); }
};

}  // namespace

struct StagePass : RenderPass {
    Visuals::Impl* impl = nullptr;
    int stage = 0;  // 0 update (Sky), 1 World, 2 AfterTranslucent, 3 Hud
    bool create(Renderer& r) override;
    void draw(const PassContext& ctx) override;
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
    Lists worldLists, hudLists;
    List tagSee, tagNormal;
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
    };
    std::vector<PaintingInfo> paintings;

    Impl(Visuals& v, EntityWorld& w, EntityRenderer& r) : self(v), world(w), er(r), assets(w.assets()) {}
    Stats& stats() { return self.stats_; }

    // --- GPU ---------------------------------------------------------------------------------------------
    bool createGpu(Renderer& r);
    void uploadPending(const PassContext& ctx);
    void beginFrame(const PassContext& ctx);
    void writeCb(int slot, const PassContext& ctx, const float* viewProj, bool pipeline, bool hand);
    bool flush(List& l);
    void bindCommon(const PassContext& ctx);
    void drawList(const PassContext& ctx, ID3D12PipelineState* pso, const List& l, int cb);

    // --- CPU ---------------------------------------------------------------------------------------------
    void update();
    void tickLocal();
    void buildWorld(const PassContext& ctx);
    void buildTags(const PassContext& ctx);
    void buildHand(const PassContext& ctx, float viewProj[16]);
    void pushItem(Lists& L, const ItemMesh& mesh, Display d, const Xf& m, u32 light, bool foil);
    void pushRaw(Lists& L, const ItemMesh& mesh, const Xf& m, u32 light, u32 color = 0xFFFFFFFF);
    u32 lightAt(double x, double y, double z) const;
    bool isSkull(int item);
    const Tag& tag(const std::string& name);
    const PaintingInfo* painting(int id);
    void heldStack(int slot, int& item, i32& dye, bool& foil) const;
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
}

bool StagePass::create(Renderer& r) { return impl->createGpu(r); }

void StagePass::draw(const PassContext& ctx) {
    Visuals::Impl& m = *impl;
    if (!m.gpuOk) return;
    Timer t;
    m.beginFrame(ctx);
    switch (stage) {
        case 0:
            m.update();
            m.stats().updateMs = t.ms();
            break;
        case 1: {
            m.worldLists.clear();
            m.buildWorld(ctx);
            m.uploadPending(ctx);
            m.writeCb(0, ctx, ctx.viewProj, ctx.shading, false);
            const bool a = m.flush(m.worldLists.cut), b = m.flush(m.worldLists.tr), c = m.flush(m.worldLists.glint);
            m.bindCommon(ctx);
            if (a) m.drawList(ctx, m.psoCut.Get(), m.worldLists.cut, 0);
            if (b) m.drawList(ctx, m.psoTr.Get(), m.worldLists.tr, 0);
            if (c) m.drawList(ctx, m.psoGlint.Get(), m.worldLists.glint, 0);
            m.stats().worldMs = t.ms();
            break;
        }
        case 2: {
            m.tagSee.clear(), m.tagNormal.clear();
            m.buildTags(ctx);
            // Particles.
            const double cam[3] = {ctx.camX, ctx.camY, ctx.camZ};
            u32 opaque = 0, total = 0;
            if (m.particles) {
                auto* out = reinterpret_cast<fx::GpuParticle*>(m.frameBase + kParticleOffset);
                total = m.particles->build(out, kMaxParticles, cam, opaque);
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
            particleDraw(m.psoPartCut.Get(), 0, opaque);
            particleDraw(m.psoPartTr.Get(), opaque, total - opaque);
            if (a) m.drawList(ctx, m.psoTextSee.Get(), m.tagSee, 1);
            if (b) m.drawList(ctx, m.psoText.Get(), m.tagNormal, 1);
            m.stats().particles = total;
            m.stats().afterMs = t.ms();
            break;
        }
        case 3: {
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
    enum Blend { Opaque, Alpha, Glint };
    auto pso = [&](const void* vs, size_t vsSize, const void* ps, size_t psSize, Blend blend, bool depthTest, bool depthWrite,
                   ComPtr<ID3D12PipelineState>& out) {
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
        }
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.DepthStencilState.DepthEnable = depthTest ? TRUE : FALSE;
        d.DepthStencilState.DepthWriteMask = depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reversed Z
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
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
        !pso(SH(ParticleVS), SH(ParticleTranslucentPS), Alpha, true, false, psoPartTr))
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
    for (List* l : {&worldLists.cut, &worldLists.tr, &worldLists.glint, &hudLists.cut, &hudLists.tr, &hudLists.glint, &tagSee, &tagNormal})
        l->v.reserve(1024);
    gpuOk = true;
    return true;
}

void Visuals::Impl::beginFrame(const PassContext& ctx) {
    if (frameIndex == ctx.frame && frameBase) return;
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

void Visuals::Impl::writeCb(int slot, const PassContext& ctx, const float* viewProj, bool pipeline, bool hand) {
    FrameCb k{};
    std::memcpy(k.viewProj, viewProj, sizeof(k.viewProj));
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

// Copies a list into the slice's instance array with prefix sums; false when empty or out of room.
bool Visuals::Impl::flush(List& l) {
    if (l.v.empty()) return false;
    if (instUsed + l.v.size() > kMaxInstances) return false;
    u32 prefix = 0;
    for (GpuInst& g : l.v) g.prefix = prefix, prefix += g.count;
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
        L.cut.add(x, p.first, p.count, light);
        L.tr.add(x, p.tfirst, p.tcount, light);
        if (foil) L.glint.add(x, p.first, p.count, light), L.glint.add(x, p.tfirst, p.tcount, light);
    }
    ++self.stats_.items;
}

void Visuals::Impl::pushRaw(Lists& L, const ItemMesh& mesh, const Xf& m, u32 light, u32 color) {
    for (int i = 0; i < mesh.partCount; ++i) {
        const ItemMesh::Part& p = mesh.parts[i];
        L.cut.add(m, p.first, p.count, light, color);
        L.tr.add(m, p.tfirst, p.tcount, light, color);
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
    const float mainTarget = (mainHand.item != nextMain || mainHand.dye != dyeMain) ? 0.0f : swap * swap * swap;
    const float offTarget = (offHand.item != nextOff || offHand.dye != dyeOff) ? 0.0f : 1.0f;
    mainHand.height += std::clamp(mainTarget - mainHand.height, -0.4f, 0.4f);
    offHand.height += std::clamp(offTarget - offHand.height, -0.4f, 0.4f);
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
        e.pose = p.sneaking ? 5 : 0;
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
    }
    if (particles) {
        const CameraState c = game ? game->viewCamera() : CameraState{};
        const double cam[3] = {c.x, c.y, c.z};
        particles->frame(dt, cam);
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
            pushItem(worldLists, models.mesh(item, e->equipDye[slot], false), k == 0 ? Display::ThirdRight : Display::ThirdLeft,
                     m, a.light, (e->equipFoil >> slot) & 1);
        }
        const int head = e->equipment[5];
        if ((a.valid & 4) && head >= 0) {
            const ItemModels::Equippable& eq = models.equippable(head);
            if (eq.asset.empty()) {  // armour is drawn as a model layer
                Xf m;
                std::memcpy(m.m, a.head, sizeof(m.m));
                if (isSkull(head)) {  // wornHeadType: the skull model, scaled 1.1875
                    std::string layer, tex;
                    ItemModels::skullLayer(models.itemName(head), layer, tex);
                    m = xfMul(m, xfScale(1.1875f, 1.1875f, 1.1875f));
                    pushRaw(worldLists, models.entityLayerMesh(layer, tex), m, a.light);
                } else {  // translateToHead
                    m = xfMul(m, xfTranslate(0, -0.25f, 0));
                    m = xfMul(m, xfRotY(180.0f * kDeg));
                    m = xfMul(m, xfScale(0.625f, -0.625f, -0.625f));
                    pushItem(worldLists, models.mesh(head, e->equipDye[5], false), Display::Head, m, a.light, (e->equipFoil >> 5) & 1);
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
    static const int kStep[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    for (const auto& [id, e] : entities) {
        const bool frame = e.type == frameType || e.type == glowFrameType;
        if (!frame && e.type != paintingType && e.category != Category::Item) continue;
        double p[3];
        for (int a = 0; a < 3; ++a) p[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt;
        const float rx = float(p[0] - cam[0]), ry = float(p[1] - cam[1]), rz = float(p[2] - cam[2]);
        if (!visible(rx, ry, rz, e.type == paintingType ? 4.0f : 1.0f)) continue;
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
            const std::string itemName = e.itemId >= 0 ? models.itemName(e.itemId) : std::string();
            const bool map = itemName == "filled_map";
            u32 frameLight = light;
            if (glow) frameLight = (light & 0xF0) | std::max<u32>(light & 15, 5);
            if (!invisible) {
                const ItemMesh& fm = models.blockModelMesh(std::string("block/") + (glow ? "glow_item_frame" : "item_frame") + (map ? "_map" : ""));
                pushRaw(worldLists, fm, xfMul(m, xfTranslate(-0.5f, -0.5f, -0.5f)), frameLight);
            }
            if (e.itemId >= 0) {
                Xf im = xfMul(m, xfTranslate(0, 0, invisible ? 0.5f : 0.4375f));
                im = xfMul(im, xfRotZ(float(e.rotation % 8) * 45.0f * kDeg));
                im = xfMul(im, xfScale(0.5f, 0.5f, 0.5f));
                pushItem(worldLists, models.mesh(e.itemId, e.itemDye, true), Display::Fixed, im, glow ? 0xFFu : light, e.itemFoil);
            }
            continue;
        }
        if (e.type == paintingType) {  // PaintingRenderer
            const PaintingInfo* info = painting(e.painting);
            if (!info) continue;
            static const int k2d[6] = {0, 0, 2, 0, 1, 3};  // 3D data value -> 2D data value (S W N E)
            const int d2 = k2d[std::clamp(e.direction, 0, 5)];
            Xf m = xfMul(xfTranslate(rx, ry, rz), xfRotY(float(180 - d2 * 90) * kDeg));
            pushRaw(worldLists, models.paintingMesh(info->asset, info->w, info->h), m, light);
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
            pushItem(worldLists, mesh, Display::Ground, m, light, e.itemFoil);
            for (int i = 1; i < amount; ++i)
                pushItem(worldLists, mesh, Display::Ground, xfMul(m, xfTranslate(rnd() * 0.15f, rnd() * 0.15f, rnd() * 0.15f)), light,
                         e.itemFoil);
        } else {
            const float offsetZ = mesh.groundDepth * 1.5f;
            m = xfMul(m, xfTranslate(0, 0, -(offsetZ * float(amount - 1) / 2.0f)));
            pushItem(worldLists, mesh, Display::Ground, m, light, e.itemFoil);
            m = xfMul(m, xfTranslate(0, 0, offsetZ));
            for (int i = 1; i < amount; ++i) {
                pushItem(worldLists, mesh, Display::Ground, xfMul(m, xfTranslate(rnd() * 0.075f, rnd() * 0.075f, 0)), light, e.itemFoil);
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
        if (e.baby && e.category == Category::Living) h *= 0.5f;
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

void Visuals::Impl::buildHand(const PassContext& ctx, float viewProj[16]) {
    if (!game || !ui || game->cameraMode != 0 || !avatarReady) return;
    if (gameMode && gameMode() == 3) return;  // spectators have no hands
    const CameraState cam = game->viewCamera();
    // Projection like the renderer's, without jitter.
    const float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    const Vec3 fwd{-sy * cp, -sp, cy * cp};
    const Vec3 right = normalize(cross(fwd, {0, 1, 0}));
    const Vec3 up = cross(right, fwd);
    const Mat4 proj = perspectiveReverseZ(ui->fovRadians(), float(ctx.width) / float(std::max(ctx.height, 1u)), 0.05f);
    const Mat4 vpm = proj * viewFromBasis(right, up, fwd);
    std::memcpy(viewProj, vpm.m, sizeof(float) * 16);
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

    // GameRenderer.bobView (bobHurt: no damage tilt here).
    Xf pose = view;
    {
        const float wd = -(walkDist + (walkDist - walkDistO) * pt);
        const float b = bobO + (bob - bobO) * pt;
        pose = xfMul(pose, xfTranslate(std::sin(wd * kPi) * b * 0.5f, -std::abs(std::cos(wd * kPi) * b), 0));
        pose = xfMul(pose, xfRotZ(std::sin(wd * kPi) * b * 3.0f * kDeg));
        pose = xfMul(pose, xfRotX(std::abs(std::cos(wd * kPi - 0.2f) * b) * 5.0f * kDeg));
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
        std::vector<Xf> mats(model.parts.size());
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
        hudLists.cut.add(ps, first, u32(dyn.size()) - first, light, 0xFFFFFFFF, 1);
    };
    auto renderItem = [&](Xf ps, const HandState& h, float inverseArmHeight, float att, bool rightArm, bool swingThis) {
        const int inv = rightArm ? 1 : -1;
        ps = xfMul(ps, xfTranslate(float(inv) * 0.56f, -0.52f + inverseArmHeight * -0.6f, -0.72f));  // applyItemArmTransform
        if (swingThis) {  // swingArm
            const float sq = std::sqrt(att);
            ps = xfMul(ps, xfTranslate(float(inv) * -0.4f * std::sin(sq * kPi), 0.2f * std::sin(sq * kPi * 2), -0.2f * std::sin(att * kPi)));
            const float ySwing = std::sin(att * att * kPi);
            ps = xfMul(ps, xfRotY(float(inv) * (45.0f + ySwing * -20.0f) * kDeg));
            const float xz = std::sin(sq * kPi);
            ps = xfMul(ps, xfRotZ(float(inv) * xz * -20.0f * kDeg));
            ps = xfMul(ps, xfRotX(xz * -80.0f * kDeg));
            ps = xfMul(ps, xfRotY(float(inv) * -45.0f * kDeg));
        }
        pushItem(hudLists, models.mesh(h.item, h.dye, false), rightArm ? Display::FirstRight : Display::FirstLeft, ps, light, h.foil);
    };
    // Main hand (right arm), then the off hand.
    const float mainSwap = mainHand.item >= 0 ? models.handInfo(mainHand.item).swapScale : 1.0f;
    const float offSwap = offHand.item >= 0 ? models.handInfo(offHand.item).swapScale : 1.0f;
    const float mainInv = mainSwap * (1.0f - (mainHand.oHeight + (mainHand.height - mainHand.oHeight) * pt));
    const float offInv = offSwap * (1.0f - (offHand.oHeight + (offHand.height - offHand.oHeight) * pt));
    if (mainHand.item < 0) renderArm(pose, mainInv, attack, true);
    else renderItem(pose, mainHand, mainInv, attack, true, true);
    if (offHand.item >= 0) renderItem(pose, offHand, offInv, 0.0f, false, false);
}

}  // namespace ent
