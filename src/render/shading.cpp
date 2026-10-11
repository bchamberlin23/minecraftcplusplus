#include "render/shading.h"
#include "render/pathtrace.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "AoCS.h"
#include "BloomDownCS.h"
#include "BloomUpCS.h"
#include "CompositeCS.h"
#include "DeferredCS.h"
#include "DepthUpsamplePS.h"
#include "DetailGbufPS.h"
#include "DetailGbufVS.h"
#include "DetailShadowPS.h"
#include "DetailShadowVS.h"
#include "ExposureCS.h"
#include "MotionCS.h"
#include "ShadowCullCS.h"
#include "TaaCS.h"
#include "TerrainFwdPS.h"
#include "TerrainFwdVS.h"
#include "TerrainGbufPS.h"
#include "TerrainGbufPSCutout.h"
#include "TerrainGbufVS.h"
#include "TerrainShadowPSCutout.h"
#include "TerrainShadowVS.h"
#include "TonemapPS.h"
#include "TonemapVS.h"
#include "UiCompositePS.h"
#include "VolumetricCS.h"

namespace {

constexpr u32 kCommandBytes = 28;  // matches renderer.cpp
constexpr u32 kQuadIndexCapacity = 65536;
constexpr DXGI_FORMAT kHdr = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kLdr = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr float kCaveScale = 0.45f, kCaveScaleNoSun = 1.7f;  // vanilla light level 0 (linear) -> HDR minimum light
constexpr DXGI_FORMAT kAlbedo = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr float kNoMotion = 32768.0f;  // motion target clear: no pass wrote object motion here (MotionCS)  // sRGB-encoded values, decoded by the deferred pass
constexpr D3D12_RESOURCE_STATES kSrv = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr D3D12_RESOURCE_STATES kRt = D3D12_RESOURCE_STATE_RENDER_TARGET;
constexpr D3D12_RESOURCE_STATES kDepthWrite = D3D12_RESOURCE_STATE_DEPTH_WRITE;
constexpr D3D12_RESOURCE_STATES kDepthRead = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

// Root parameters of the renderer's root signature (renderer.cpp createPipelines).
enum Root : u32 { RootDraw, RootFrame, RootQuads, RootSections, RootBlockTex, RootCounts, RootArgs, RootActive, RootColors,
                  RootShade, RootSrvTable, RootUavTable, RootPass, RootRemap, RootTlas, RootModelQuads, RootPtSrv, RootPtUav,
                  RootPtConstants };

// Must match cbuffer Shade in shading.hlsli.
struct ShadeConstants {
    float shadowMat[4][16];
    float prevViewProj[16];
    float viewProjNoJitter[16];
    float rayX[4], rayY[4], rayZ[4];
    float cascade[4][4];
    float lightDir[4], lightColor[4], sunDir[4], ambient[4], blockColor[4], fog[4], water[4], waterScatter[4];
    float screen[4], jitter[4], time[4], skyZenith[4], skyHorizon[4], misc[4], moonDir[4], output[4];
    u32 opts[4], flags[4];
    float cave[4];
};
static_assert(sizeof(ShadeConstants) <= Renderer::Shading::kConstantBytes);

bool check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    LOG("error: shaders: %s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    return false;
}

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
u32 asUint(float f) {
    u32 u;
    std::memcpy(&u, &f, 4);
    return u;
}

// Material classes and waving modes (shading.hlsli kMat*).
enum : u32 { MatDefault, MatLeaves, MatPlant, MatEmissive, MatGlow, MatWater, MatGlass, MatIce };
u32 classify(std::string_view n) {
    if (n.substr(0, 6) != "block/") return MatDefault;
    n.remove_prefix(6);
    auto has = [&](std::string_view s) { return n.find(s) != std::string_view::npos; };
    auto starts = [&](std::string_view s) { return n.substr(0, s.size()) == s; };
    auto ends = [&](std::string_view s) { return n.size() >= s.size() && n.substr(n.size() - s.size()) == s; };
    auto any = [&](std::initializer_list<std::string_view> list) {
        for (std::string_view s : list)
            if (n == s) return true;
        return false;
    };
    const u32 wave1 = 1 << 4, wave2 = 2 << 4, wave3 = 3 << 4;
    if (any({"water_still", "water_flow", "water_overlay"})) return MatWater;
    if (starts("lava") || any({"glowstone", "sea_lantern", "shroomlight", "redstone_lamp_on", "beacon", "end_rod",
                               "fire_0", "fire_1", "soul_fire_0", "soul_fire_1", "campfire_fire", "soul_campfire_fire",
                               "nether_portal", "conduit", "end_gateway"}) ||
        has("froglight"))
        return MatGlow;
    if (any({"torch", "soul_torch", "redstone_torch", "lantern", "soul_lantern", "jack_o_lantern", "furnace_front_on",
             "smoker_front_on", "blast_furnace_front_on", "crying_obsidian", "respawn_anchor_top", "sea_pickle",
             "glow_lichen", "magma", "vault_front_on", "vault_front_ejecting", "creaking_heart_awake",
             "firefly_bush_emissive", "cave_vines_lit", "cave_vines_plant_lit", "sculk_catalyst_top_bloom",
             "sculk_catalyst_side_bloom", "redstone_ore_lit", "deepslate_redstone_ore_lit"}) ||
        (starts("copper_bulb") && has("_lit")) || (starts("trial_spawner") && has("active")) || starts("respawn_anchor_side"))
        return MatEmissive;
    if (ends("_leaves")) return MatLeaves | wave3;
    if (ends("_top") && (starts("tall_grass") || starts("large_fern") || starts("lilac") || starts("rose_bush") ||
                         starts("peony") || starts("tall_seagrass") || starts("pitcher_crop") || starts("tall_dry_grass")))
        return MatPlant | wave2;
    if (any({"sunflower_front", "sunflower_back", "sunflower_top"})) return MatPlant | wave2;
    if (ends("_bottom") && (starts("tall_grass") || starts("large_fern") || starts("lilac") || starts("rose_bush") ||
                            starts("peony") || starts("sunflower") || starts("tall_seagrass") || starts("pitcher_crop")))
        return MatPlant | wave1;
    if (any({"short_grass", "grass", "fern", "dead_bush", "dandelion", "poppy", "blue_orchid", "allium", "azure_bluet",
             "oxeye_daisy", "cornflower", "lily_of_the_valley", "wither_rose", "torchflower", "closed_eyeblossom",
             "open_eyeblossom", "seagrass", "bush", "firefly_bush", "short_dry_grass", "tall_dry_grass", "crimson_roots",
             "warped_roots", "nether_sprouts", "kelp", "kelp_plant", "cactus_flower"}) ||
        ends("_tulip") || ends("_sapling") || starts("sweet_berry_bush") || starts("wheat_stage") ||
        starts("carrots_stage") || starts("potatoes_stage") || starts("beetroots_stage"))
        return MatPlant | wave1;
    if (any({"vine", "weeping_vines", "weeping_vines_plant", "twisting_vines", "twisting_vines_plant", "hanging_roots",
             "pale_hanging_moss", "pale_hanging_moss_tip"}))
        return MatPlant | wave3;
    if (any({"pink_petals", "wildflowers", "leaf_litter"})) return MatPlant;
    if (n == "frosted_ice" || n == "ice") return MatIce;
    if (has("glass")) return MatGlass;
    return MatDefault;
}

}  // namespace

// ---- settings ---------------------------------------------------------------------------------------------

ShaderSettings ShaderSettings::preset(ShaderPreset p) {
    ShaderSettings s;
    switch (p) {
        case ShaderPreset::Off:
        case ShaderPreset::High: break;
        case ShaderPreset::Low:
            s.cascades = 1, s.shadowRes = 1536, s.shadowTaps = 4;
            s.shadowSplits[0] = 64;
            s.volumetricSteps = 0, s.ssrSteps = 0, s.waving = false;
            break;
        case ShaderPreset::Medium:
            s.cascades = 2, s.shadowRes = 1536, s.shadowTaps = 4;
            s.shadowSplits[0] = 24, s.shadowSplits[1] = 96;
            s.volumetricSteps = 8, s.volumetricShift = 2, s.ssrSteps = 16;
            break;
        case ShaderPreset::Ultra:
            s.cascades = 4, s.shadowRes = 2048, s.shadowTaps = 16, s.pcss = true;
            s.shadowSplits[0] = 14, s.shadowSplits[1] = 44, s.shadowSplits[2] = 128, s.shadowSplits[3] = 320;
            s.volumetricSteps = 20, s.volumetricShift = 1, s.ssrSteps = 40, s.ssao = true;
            break;
        case ShaderPreset::PathTraced:  // High for what lies beyond the traced radius (and the volumetrics)
            s.pathTrace = true;
            s.setPathTraceQuality(PathTraceQuality::High);
            break;
    }
    return s;
}

void ShaderSettings::setPathTraceQuality(PathTraceQuality q) {
    static const struct { u32 bounces, samples; float radius; } kQ[] = {{1, 1, 128}, {1, 1, 192}, {2, 1, 256}, {2, 2, 384}};
    const auto& v = kQ[std::min(int(q), 3)];
    ptBounces = v.bounces, ptSamples = v.samples, ptRadius = v.radius;
}

bool Renderer::parseShaderPreset(std::string_view name, ShaderPreset& out) {
    static const std::pair<std::string_view, ShaderPreset> kNames[] = {{"off", ShaderPreset::Off},
                                                                        {"low", ShaderPreset::Low},
                                                                        {"medium", ShaderPreset::Medium},
                                                                        {"high", ShaderPreset::High},
                                                                        {"ultra", ShaderPreset::Ultra},
                                                                        {"pathtraced", ShaderPreset::PathTraced}};
    for (const auto& [n, p] : kNames)
        if (n == name) return out = p, true;
    return false;
}

const char* Renderer::shaderPresetName(ShaderPreset p) {
    static const char* kNames[] = {"off", "low", "medium", "high", "ultra", "pathtraced"};
    return kNames[int(p)];
}

void Renderer::setShaderPreset(ShaderPreset preset) {
    shaderPreset_ = preset;
    if (preset != ShaderPreset::Off) setShaderSettings(ShaderSettings::preset(preset));
    else if (pt_) waitIdle(), pt_.reset();
}

void Renderer::setShaderSettings(const ShaderSettings& settings) {
    shaderSettings_ = settings;
    shaderSettings_.cascades = std::min(shaderSettings_.cascades, 4u);
    shaderSettings_.renderScale = std::clamp(shaderSettings_.renderScale, 0.33f, 1.0f);
    if (!device_ || shaderPreset_ == ShaderPreset::Off) return;  // created by init / on the next preset
    waitIdle();
    if (!shading_) {
        shading_ = std::make_unique<Shading>(*this);
        if (!shading_->create() || !shading_->createTargets()) {
            LOG("error: shaders unavailable; using the vanilla look");
            shading_.reset();
            shaderPreset_ = ShaderPreset::Off;
            return;
        }
        if (!textureNames_.empty()) shading_->setMaterials(textureNames_);
    }
    shading_->createShadowMap();
    if (shading_->renderW_ != shading_->renderSize(width_) || shading_->renderH_ != shading_->renderSize(height_))
        shading_->createTargets();
    // The path tracer exists while Shaders: Path Traced is selected (its acceleration structures free with it).
    if (shaderSettings_.pathTrace && !pt_ && pathTracingSupported()) {
        pt_ = std::make_unique<PathTracer>(*this);
        if (!pt_->create()) {
            LOG("error: path tracing unavailable; using the High pipeline");
            pt_.reset();
        }
    } else if (!shaderSettings_.pathTrace && pt_) {
        pt_.reset();
    }
    if (pt_) pt_->createTargets();
    shading_->historyValid_ = false;
    shading_->exposureReset_ = true;
}

void Renderer::setTextureNames(const std::vector<std::string>& names) {
    textureNames_ = names;
    if (shading_) shading_->setMaterials(names);
}

// ---- device objects ---------------------------------------------------------------------------------------

ComPtr<ID3D12Resource> Renderer::Shading::texture(DXGI_FORMAT format, u32 w, u32 h, u16 slices, D3D12_RESOURCE_FLAGS flags,
                                                  D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = std::max(w, 1u);
    d.Height = std::max(h, 1u);
    d.DepthOrArraySize = slices;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    ComPtr<ID3D12Resource> res;
    check(r.device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, clear, IID_PPV_ARGS(&res)), "texture");
    return res;
}

void Renderer::Shading::srv(u32 slot, ID3D12Resource* res, DXGI_FORMAT format) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = format;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    r.device_->CreateShaderResourceView(res, &d, r.cpuDescriptor(descBase_ + slot));
}

void Renderer::Shading::uav(u32 slot, ID3D12Resource* res, DXGI_FORMAT format) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = format;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    r.device_->CreateUnorderedAccessView(res, nullptr, &d, r.cpuDescriptor(descBase_ + TexCount + 2 + slot));
}

bool Renderer::Shading::create() {
    ID3D12Device* dev = r.device_.Get();
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 8;
    if (!check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_)), "rtv heap")) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 5;  // shadow cascades, scene depth
    if (!check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap_)), "dsv heap")) return false;
    rtvStride_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dsvStride_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    descBase_ = r.allocDescriptors(TexCount + 2 + RwCount + 1);
    if (descBase_ == UINT32_MAX) return check(E_OUTOFMEMORY, "descriptors");
    for (u32 i = 0; i < TexCount; ++i) srv(i, nullptr, kLdr);
    for (u32 i = 0; i < RwCount; ++i) uav(i, nullptr, kLdr);
    {  // null shadow map array until createShadowMap
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2DArray.MipLevels = 1;
        d.Texture2DArray.ArraySize = 1;
        dev->CreateShaderResourceView(nullptr, &d, r.cpuDescriptor(descBase_ + TexCount));
    }

    constants_ = r.createBuffer(u64(kConstantBytes) * Renderer::kFrames, D3D12_HEAP_TYPE_UPLOAD);
    if (!constants_) return false;
    constants_->Map(0, nullptr, reinterpret_cast<void**>(&constantsMapped_));
    state_ = r.createBuffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    shadowArgs_ = r.createBuffer(u64(kShadowSegCap) * 12 * kCommandBytes, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    shadowCounts_ = r.createBuffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!state_ || !shadowArgs_ || !shadowCounts_) return false;
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        d.Buffer.NumElements = 16;
        d.Buffer.StructureByteStride = 16;
        dev->CreateUnorderedAccessView(state_.Get(), nullptr, &d, r.cpuDescriptor(descBase_ + TexCount + 2 + RwCount));
    }
    // The state buffer stays in UNORDERED_ACCESS for good (it is never read any other way).
    r.allocators_[0]->Reset();
    r.list_->Reset(r.allocators_[0].Get(), nullptr);
    r.transition(state_.Get(), D3D12_RESOURCE_STATE_COMMON, kUav);
    r.submitAndWait();
    setMaterials({});

    D3D12_QUERY_HEAP_DESC qh{};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = kMaxMarks * Renderer::kFrames;
    if (!check(dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&queries_)), "query heap")) return false;
    queryReadback_ = r.createBuffer(u64(kMaxMarks) * 8 * Renderer::kFrames, D3D12_HEAP_TYPE_READBACK);
    for (auto& v : timingSamples_) v.reserve(kTimingHistory);

    // Pipelines (the renderer's root signature).
    auto graphics = [&](const void* vs, size_t vsSize, const void* ps, size_t psSize, u32 rtCount, const DXGI_FORMAT* rts,
                        bool depth, bool depthWrite, D3D12_COMPARISON_FUNC func, D3D12_CULL_MODE cull, bool blend,
                        bool shadow, ComPtr<ID3D12PipelineState>& out) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = r.rootSig_.Get();
        d.VS = {vs, vsSize};
        if (ps) d.PS = {ps, psSize};
        for (u32 i = 0; i < rtCount; ++i) {
            auto& rt = d.BlendState.RenderTarget[i];
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            if (blend) {
                rt.BlendEnable = TRUE;
                rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                rt.BlendOp = D3D12_BLEND_OP_ADD;
                rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            }
            d.RTVFormats[i] = rts[i];
        }
        d.NumRenderTargets = rtCount;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = cull;
        d.RasterizerState.DepthClipEnable = shadow ? FALSE : TRUE;  // shadows: casters before the near plane clamp
        if (shadow && cull == D3D12_CULL_MODE_NONE) d.RasterizerState.SlopeScaledDepthBias = 1.5f;
        d.DepthStencilState.DepthEnable = depth;
        d.DepthStencilState.DepthWriteMask = depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        d.DepthStencilState.DepthFunc = func;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.DSVFormat = depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
        d.SampleDesc.Count = 1;
        return check(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)), "graphics pipeline");
    };
    auto compute = [&](const void* cs, size_t size, ComPtr<ID3D12PipelineState>& out) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = r.rootSig_.Get();
        d.CS = {cs, size};
        return check(dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&out)), "compute pipeline");
    };
#define SH(name) g_##name, sizeof(g_##name)
    const DXGI_FORMAT gbuf[2] = {kAlbedo, kLdr}, hdr[1] = {kHdr}, ldr[1] = {kLdr};
    const auto ge = D3D12_COMPARISON_FUNC_GREATER_EQUAL, le = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    const auto back = D3D12_CULL_MODE_BACK, none = D3D12_CULL_MODE_NONE, front = D3D12_CULL_MODE_FRONT;
    return graphics(SH(TerrainGbufVS), SH(TerrainGbufPS), 2, gbuf, true, true, ge, back, false, false, psoGbuf_) &&
           graphics(SH(TerrainGbufVS), SH(TerrainGbufPSCutout), 2, gbuf, true, true, ge, back, false, false, psoGbufCutout_) &&
           graphics(SH(DetailGbufVS), SH(DetailGbufPS), 2, gbuf, true, true, ge, back, false, false, psoGbufModel_) &&
           graphics(SH(TerrainShadowVS), nullptr, 0, 0, nullptr, true, true, le, front, false, true, psoShadow_) &&
           // Leaves are closed volumes like opaque cubes (shared faces culled): back faces only, no acne.
           graphics(SH(TerrainShadowVS), SH(TerrainShadowPSCutout), 0, nullptr, true, true, le, front, false, true,
                    psoShadowCutout_) &&
           graphics(SH(DetailShadowVS), SH(DetailShadowPS), 0, nullptr, true, true, le, none, false, true, psoShadowModel_) &&
           graphics(SH(TerrainFwdVS), SH(TerrainFwdPS), 1, hdr, true, true, ge, none, true, false, psoFwd_) &&
           graphics(SH(TonemapVS), SH(TonemapPS), 1, ldr, false, false, ge, none, false, false, psoTonemap_) &&
           graphics(SH(TonemapVS), SH(DepthUpsamplePS), 0, nullptr, true, true, D3D12_COMPARISON_FUNC_ALWAYS, none, false,
                    false, psoDepthUp_) &&
           uiComposite() &&
           compute(SH(MotionCS), psoMotion_) &&
           compute(SH(ShadowCullCS), psoShadowCull_) && compute(SH(DeferredCS), psoDeferred_) && compute(SH(AoCS), psoAo_) &&
           compute(SH(VolumetricCS), psoVolumetric_) && compute(SH(CompositeCS), psoComposite_) &&
           compute(SH(TaaCS), psoTaa_) && compute(SH(BloomDownCS), psoBloomDown_) && compute(SH(BloomUpCS), psoBloomUp_) &&
           compute(SH(ExposureCS), psoExposure_);
#undef SH
}

void Renderer::Shading::setMaterials(const std::vector<std::string>& names) {
    std::vector<u32> table(std::max<size_t>(names.size(), 4096), 0);
    u32 counts[8] = {};
    for (size_t i = 0; i < names.size(); ++i) {
        table[i] = classify(names[i]);
        ++counts[table[i] & 7];
    }
    ComPtr<ID3D12Resource> buf = r.createBuffer(table.size() * 4, D3D12_HEAP_TYPE_DEFAULT);
    if (!buf || !r.uploadBuffer(buf.Get(), table.data(), table.size() * 4, kSrv)) return;
    materials_ = buf;
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.NumElements = u32(table.size());
    d.Buffer.StructureByteStride = 4;
    r.device_->CreateShaderResourceView(materials_.Get(), &d, r.cpuDescriptor(descBase_ + TexCount + 1));
    if (!names.empty())
        LOG("shaders: materials for %zu textures: %u foliage, %u plants, %u emissive, %u water, %u glass/ice", names.size(),
            counts[MatLeaves], counts[MatPlant], counts[MatEmissive] + counts[MatGlow], counts[MatWater],
            counts[MatGlass] + counts[MatIce]);
}

u32 Renderer::Shading::renderSize(u32 output) const {
    return std::max(1u, u32(float(output) * r.shaderSettings_.renderScale + 0.5f));
}

bool Renderer::Shading::createTargets() {
    r.waitIdle();
    outW_ = r.width_, outH_ = r.height_;
    renderW_ = renderSize(outW_), renderH_ = renderSize(outH_);
    width_ = renderW_, height_ = renderH_;
    const u32 w = width_, h = height_;
    const auto rt = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, ua = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // Scene depth: the renderer's own depth buffer at full resolution, a separate one when scaled.
    if (w == outW_ && h == outH_) {
        sceneDepth_ = r.depth_;
    } else {
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        sceneDepth_ = texture(DXGI_FORMAT_R32_TYPELESS, w, h, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, kDepthWrite, &cv);
        if (!sceneDepth_) return false;
    }
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_D32_FLOAT;
        d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        r.device_->CreateDepthStencilView(sceneDepth_.Get(), &d,
                                          {dsvHeap_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(4) * dsvStride_});
    }
    {
        D3D12_CLEAR_VALUE none{};  // motion target clear: kNoMotion (deferred.hlsl MotionCS)
        none.Format = DXGI_FORMAT_R16G16_FLOAT;
        none.Color[0] = none.Color[1] = kNoMotion;
        motion_ = texture(DXGI_FORMAT_R16G16_FLOAT, w, h, 1, ua | rt, kSrv, &none);
    }
    exposureTex_ = texture(DXGI_FORMAT_R32_FLOAT, 1, 1, 1, ua, kSrv, nullptr);
    {
        D3D12_CLEAR_VALUE zeroUi{};
        zeroUi.Format = kLdr;
        ui_ = texture(kLdr, outW_, outH_, 1, rt, kSrv, &zeroUi);
    }
    if (!motion_ || !exposureTex_ || !ui_) return false;
    albedo_ = texture(kAlbedo, w, h, 1, rt, kSrv, nullptr);
    normal_ = texture(kLdr, w, h, 1, rt, kSrv, nullptr);
    D3D12_CLEAR_VALUE zero{};
    zero.Format = kLdr;
    ldrSky_ = texture(kLdr, w, h, 1, rt, kSrv, &zero);
    ldrFwd_ = texture(kLdr, w, h, 1, rt, kSrv, &zero);
    scene_ = texture(kHdr, w, h, 1, rt | ua, kSrv, nullptr);
    sceneCopy_ = texture(kHdr, w, h, 1, ua, kSrv, nullptr);
    linDepth_ = texture(DXGI_FORMAT_R32_FLOAT, w, h, 1, ua, kSrv, nullptr);
    vol_ = texture(kHdr, (w + 1) / 2, (h + 1) / 2, 1, ua, kSrv, nullptr);
    ao_ = texture(DXGI_FORMAT_R8_UNORM, (w + 1) / 2, (h + 1) / 2, 1, ua, kSrv, nullptr);
    for (auto& t : hist_) t = texture(kHdr, outW_, outH_, 1, ua, kSrv, nullptr);
    for (u32 i = 0; i < kBloomLevels; ++i) {
        bloomW_[i] = std::max(1u, outW_ >> (i + 1)), bloomH_[i] = std::max(1u, outH_ >> (i + 1));
        bloom_[i] = texture(kHdr, bloomW_[i], bloomH_[i], 1, ua, kUav, nullptr);
        if (!bloom_[i]) return false;
    }
    if (!albedo_ || !normal_ || !ao_ || !ldrSky_ || !ldrFwd_ || !scene_ || !sceneCopy_ || !linDepth_ || !vol_ || !hist_[0] ||
        !hist_[1])
        return false;
    srv(TexDepth, sceneDepth_.Get(), DXGI_FORMAT_R32_FLOAT);
    srv(TexMotion, motion_.Get(), DXGI_FORMAT_R16G16_FLOAT);
    srv(TexExposure, exposureTex_.Get(), DXGI_FORMAT_R32_FLOAT);
    srv(TexUi, ui_.Get(), kLdr);
    uav(RwMotion, motion_.Get(), DXGI_FORMAT_R16G16_FLOAT);
    uav(RwExposure, exposureTex_.Get(), DXGI_FORMAT_R32_FLOAT);
    srv(TexAlbedo, albedo_.Get(), kAlbedo);
    srv(TexNormal, normal_.Get(), kLdr);
    srv(TexLdrSky, ldrSky_.Get(), kLdr);
    srv(TexLdrFwd, ldrFwd_.Get(), kLdr);
    srv(TexScene, scene_.Get(), kHdr);
    srv(TexSceneCopy, sceneCopy_.Get(), kHdr);
    srv(TexLinDepth, linDepth_.Get(), DXGI_FORMAT_R32_FLOAT);
    srv(TexVol, vol_.Get(), kHdr);
    srv(TexAo, ao_.Get(), DXGI_FORMAT_R8_UNORM);
    srv(TexHist0, hist_[0].Get(), kHdr);
    srv(TexHist1, hist_[1].Get(), kHdr);
    for (u32 i = 0; i < kBloomLevels; ++i) srv(TexBloom0 + i, bloom_[i].Get(), kHdr);
    uav(RwScene, scene_.Get(), kHdr);
    uav(RwSceneCopy, sceneCopy_.Get(), kHdr);
    uav(RwLinDepth, linDepth_.Get(), DXGI_FORMAT_R32_FLOAT);
    uav(RwVol, vol_.Get(), kHdr);
    uav(RwAo, ao_.Get(), DXGI_FORMAT_R8_UNORM);
    uav(RwHist0, hist_[0].Get(), kHdr);
    uav(RwHist1, hist_[1].Get(), kHdr);
    for (u32 i = 0; i < kBloomLevels; ++i) uav(RwBloom0 + i, bloom_[i].Get(), kHdr);
    const D3D12_CPU_DESCRIPTOR_HANDLE base = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    ID3D12Resource* rts[7] = {albedo_.Get(), normal_.Get(), ldrSky_.Get(), ldrFwd_.Get(), scene_.Get(), ui_.Get(), motion_.Get()};
    for (u32 i = 0; i < 7; ++i) r.device_->CreateRenderTargetView(rts[i], nullptr, {base.ptr + size_t(i) * rtvStride_});
    historyValid_ = false;
    return true;
}

bool Renderer::Shading::createShadowMap() {
    const ShaderSettings& s = r.shaderSettings_;
    const u32 slices = std::max(1u, s.cascades);
    if (shadowMap_ && shadowRes_ == s.shadowRes && shadowSlices_ == slices) return true;
    shadowMap_.Reset();
    shadowRes_ = s.shadowRes, shadowSlices_ = slices;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth = 1.0f;
    shadowMap_ = texture(DXGI_FORMAT_R32_TYPELESS, shadowRes_, shadowRes_, u16(slices),
                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, kSrv, &cv);
    if (!shadowMap_) return false;
    for (u32 c = 0; c < slices; ++c) {
        D3D12_DEPTH_STENCIL_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_D32_FLOAT;
        d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray.FirstArraySlice = c;
        d.Texture2DArray.ArraySize = 1;
        r.device_->CreateDepthStencilView(shadowMap_.Get(), &d,
                                          {dsvHeap_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(c) * dsvStride_});
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R32_FLOAT;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2DArray.MipLevels = 1;
    d.Texture2DArray.ArraySize = slices;
    r.device_->CreateShaderResourceView(shadowMap_.Get(), &d, r.cpuDescriptor(descBase_ + TexCount));
    shadowFresh_ = true;
    for (auto& c : cascades_) c.valid = false;
    return true;
}

// ---- per frame --------------------------------------------------------------------------------------------

void Renderer::Shading::prepare(const FrameInput& in) {
    const ShaderSettings& set = r.shaderSettings_;
    const SkyLighting& sky = r.skyLighting_;
    const CameraState& cam = *in.cam;
    const double now = nowSeconds();
    if (startTime_ == 0) startTime_ = lastTime_ = now;
    dt_ = float(std::min(now - lastTime_, 0.25));
    lastTime_ = now;
    ++frameCounter_;

    ShadeConstants k{};
    // Sun and moon: the game's path (east -> up -> west) tilted about the east-west axis.
    const float tilt = set.sunPathTilt, ct = std::cos(tilt), st = std::sin(tilt);
    auto body = [&](float angle, float out[4]) {
        const float x = -std::sin(angle), y = std::cos(angle);
        out[0] = x, out[1] = y * ct, out[2] = y * st;
    };
    float sun[4], moon[4];
    body(sky.sunAngle, sun);
    body(sky.moonAngle, moon);
    const float rain = sky.rain, thunder = sky.thunder;
    const float sunUp = smoothstep(-0.03f, 0.08f, sun[1]), moonUp = smoothstep(-0.03f, 0.08f, moon[1]) * (1 - sunUp);
    // Sunlight through the atmosphere (Kasten-Young air mass): white at noon, orange at the horizon.
    const float elevDeg = std::asin(std::clamp(sun[1], -1.0f, 1.0f)) * 57.29578f;
    const float airMass = 1.0f / (std::max(sun[1], 0.0f) + 0.50572f * std::pow(std::max(elevDeg + 6.07995f, 0.1f), -1.6364f));
    const float beta[3] = {0.035f, 0.075f, 0.17f};
    const float weatherDim = (1 - rain * 0.88f) * (1 - thunder * 0.5f);
    const bool lights = sky.overworld;
    float light[3];
    for (int c = 0; c < 3; ++c) {
        const float sunC = 3.4f * std::exp(-beta[c] * airMass) * sunUp;
        const float moonC = (c == 0 ? 0.10f : c == 1 ? 0.12f : 0.17f) * moonUp;
        light[c] = lights ? (sunC + moonC) * weatherDim : 0.0f;
    }
    const bool sunIsLight = sun[1] > moon[1];
    const float* ld = sunIsLight ? sun : moon;
    // Keep a little elevation so the shadow direction never goes edge-on.
    float lightDir[3] = {ld[0], std::max(ld[1], 0.05f), ld[2]};
    const float lll = std::sqrt(lightDir[0] * lightDir[0] + lightDir[1] * lightDir[1] + lightDir[2] * lightDir[2]);
    for (float& v : lightDir) v /= lll;
    std::copy(lightDir, lightDir + 3, lightDir_);
    const float night = 1.0f - smoothstep(-0.15f, 0.1f, sun[1]);

    std::copy(lightDir, lightDir + 3, k.lightDir);
    std::copy(light, light + 3, k.lightColor);
    k.lightColor[3] = sunIsLight && !sky.skyHidden ? 1.0f : 0.0f;  // the sun's glow in the sky (none behind fog)
    std::copy(sun, sun + 3, k.sunDir);
    k.sunDir[3] = lights ? 6.0f * (1 - rain) : 0.0f;
    std::copy(moon, moon + 3, k.moonDir);
    k.moonDir[3] = night;

    // Sky colours (vanilla, linear) drive the ambient light and the sky model.
    const float skyScale = 1.15f;
    float zen[3], hor[3];
    for (int c = 0; c < 3; ++c) {
        zen[c] = srgbToLinear(std::clamp(sky.skyColor[c], 0.0f, 1.0f));
        hor[c] = srgbToLinear(std::clamp(r.fogColor_[c], 0.0f, 1.0f));
        k.skyZenith[c] = zen[c] * skyScale;
        k.skyHorizon[c] = hor[c] * skyScale;
        const float moonAmbient = (c == 0 ? 0.018f : c == 1 ? 0.024f : 0.040f) * night;
        k.ambient[c] = (zen[c] * 0.5f + hor[c] * 0.3f) * (1 - night * 0.5f) + light[c] * 0.05f + moonAmbient;
    }
    {  // sky light is mostly blue but the bounce from the ground is not: keep the ambient half desaturated
        const float l = 0.2126f * k.ambient[0] + 0.7152f * k.ambient[1] + 0.0722f * k.ambient[2];
        for (int c = 0; c < 3; ++c) k.ambient[c] = (l + (k.ambient[c] - l) * 0.5f) * 1.2f;
    }
    k.ambient[3] = skyScale;
    k.skyHorizon[3] = std::min(in.viewDistance * 0.6f, r.fogLimit_[0]);  // vanilla fog start / end (fog environments)
    const float blockColor[3] = {1.0f, 0.56f, 0.26f};
    for (int c = 0; c < 3; ++c) k.blockColor[c] = blockColor[c];
    {  // Minimum (cave) light: the game's lightmap at light level 0 (the dimension's ambient light colour with the
       // brightness lift: #0a0a0a in the overworld, warm in the Nether, green-grey in the End), linear. Without a sun
       // it is the main light, so it counts for more there.
        const Lightmap& lm = r.lightmap_;
        float m = 0;
        for (int c = 0; c < 3; ++c) m = std::max(m, std::clamp(lm.ambient[c], 0.0f, 1.0f));
        const float inv = 1 - m, lift = m > 0 ? (1 - inv * inv * inv * inv) / m : 1.0f;
        const float scale = lights ? kCaveScale : kCaveScaleNoSun;
        for (int c = 0; c < 3; ++c) {
            const float a = std::clamp(lm.ambient[c], 0.0f, 1.0f);
            k.cave[c] = srgbToLinear(a + (a * lift - a) * lm.brightness) * scale;
        }
    }
    k.blockColor[3] = 0.2126f * k.cave[0] + 0.7152f * k.cave[1] + 0.0722f * k.cave[2];
    for (int c = 0; c < 3; ++c)  // the darkness effect's pulse dims every light like the lightmap's subtraction
        k.lightColor[c] *= sky.dim, k.ambient[c] *= sky.dim, k.blockColor[c] *= sky.dim, k.cave[c] *= sky.dim;

    k.fog[0] = (1.0f / 7000.0f) * (1 + rain * 7.0f + thunder * 4.0f);
    k.fog[1] = 1.0f / 110.0f;
    k.fog[2] = 63.0f;
    k.fog[3] = 0.5f * (1.0f + 1.2f * (1 - smoothstep(0.0f, 0.5f, sun[1]))) * (set.volumetricSteps && !sky.skyHidden ? 1.0f : 0.0f);
    const float waterAbsorb[3] = {0.32f, 0.075f, 0.05f};
    for (int c = 0; c < 3; ++c) {
        k.water[c] = waterAbsorb[c];
        const float tint[3] = {0.05f, 0.18f, 0.55f};
        k.waterScatter[c] = tint[c] * (k.ambient[c] + light[c] * 0.15f) * 0.3f;
    }
    k.water[3] = sky.cameraInWater ? 1.0f : 0.0f;
    k.waterScatter[3] = 0.04f;

    const float w = float(renderW_), h = float(renderH_);
    k.screen[0] = w, k.screen[1] = h, k.screen[2] = 1 / w, k.screen[3] = 1 / h;
    k.output[0] = float(outW_), k.output[1] = float(outH_), k.output[2] = 1.0f / float(outW_), k.output[3] = 1.0f / float(outH_);
    k.jitter[0] = in.jitter[0], k.jitter[1] = in.jitter[1];
    k.jitter[2] = float(cam.x - std::floor(cam.x / 2048.0) * 2048.0);
    k.jitter[3] = float(cam.z - std::floor(cam.z / 2048.0) * 2048.0);
    k.time[0] = float(std::fmod(now - startTime_, 3600.0));
    k.time[1] = rain, k.time[2] = thunder, k.time[3] = float(cam.y);
    k.misc[0] = set.exposure;
    k.misc[1] = set.bloom;
    k.misc[2] = 1.6f;  // emissive strength
    k.misc[3] = std::min(in.viewDistance, r.fogLimit_[1]);

    // View rays: camera-relative position = viewZ * (rayZ + ndc.x * rayX + ndc.y * rayY) for the jittered ndc.
    const float tanY = std::tan(cam.fovY * 0.5f), tanX = tanY * w / std::max(h, 1.0f);
    for (int c = 0; c < 3; ++c) {
        const float rx = (&in.rayRight.x)[c] * tanX, ry = (&in.rayUp.x)[c] * tanY;
        k.rayX[c] = rx, k.rayY[c] = ry;
        k.rayZ[c] = (&in.rayFwd.x)[c] - in.jitter[0] * rx - in.jitter[1] * ry;
    }
    std::memcpy(k.viewProjNoJitter, in.vpNoJitter.m, 64);
    // Previous frame: this frame's camera-relative position + (camera - previous camera) -> previous clip.
    const double move[3] = {cam.x - prevCam_[0], cam.y - prevCam_[1], cam.z - prevCam_[2]};
    if (std::abs(move[0]) + std::abs(move[1]) + std::abs(move[2]) > 64.0) historyValid_ = false;
    Mat4 t;
    t(0, 0) = t(1, 1) = t(2, 2) = t(3, 3) = 1;
    t(0, 3) = float(move[0]), t(1, 3) = float(move[1]), t(2, 3) = float(move[2]);
    const Mat4 prev = (historyValid_ ? prevVpNoJitter_ : in.vpNoJitter) * t;
    std::memcpy(k.prevViewProj, prev.m, 64);
    std::memcpy(prevViewProj_, prev.m, 64);
    prevVpNoJitter_ = in.vpNoJitter;
    prevCam_[0] = cam.x, prevCam_[1] = cam.y, prevCam_[2] = cam.z;

    // Shadow cascades: bounding spheres of the view slices, snapped to whole texels in world-anchored light space.
    const u32 cascades = lights && light[0] + light[1] + light[2] > 0 ? set.cascades : 0;
    updateMask_ = 0;
    if (cascades) {
        // Near cascades every frame; the outer ones alternate (static terrain, slow sun: invisible lag).
        updateMask_ = cascades <= 2 ? 3u : 3u | (cascades == 3 ? (frameCounter_ & 1 ? 4u : 0u) : (frameCounter_ & 1 ? 4u : 8u));
        if (shadowFresh_) updateMask_ = (1u << cascades) - 1;
        double f[3] = {-lightDir[0], -lightDir[1], -lightDir[2]};
        const double hint[3] = {std::abs(f[2]) < 0.9 ? 0.0 : 1.0, 0.0, std::abs(f[2]) < 0.9 ? 1.0 : 0.0};
        double rt[3] = {f[1] * hint[2] - f[2] * hint[1], f[2] * hint[0] - f[0] * hint[2], f[0] * hint[1] - f[1] * hint[0]};
        const double rl = std::sqrt(rt[0] * rt[0] + rt[1] * rt[1] + rt[2] * rt[2]);
        for (double& v : rt) v /= rl;
        const double up[3] = {rt[1] * f[2] - rt[2] * f[1], rt[2] * f[0] - rt[0] * f[2], rt[0] * f[1] - rt[1] * f[0]};
        const double k2 = double(tanX) * tanX + double(tanY) * tanY;
        const double camPos[3] = {cam.x, cam.y, cam.z};
        for (u32 c = 0; c < cascades; ++c) {
            Cascade& cs = cascades_[c];
            if (updateMask_ & (1u << c) || !cs.valid) {
                updateMask_ |= 1u << c;
                const double n = c == 0 ? 0.05 : set.shadowSplits[c - 1], fa = set.shadowSplits[c];
                const double zc = std::min((fa + n) * (1 + k2) * 0.5, fa);
                double radius = std::sqrt(fa * fa * k2 + (fa - zc) * (fa - zc));
                radius = std::ceil(radius * 8.0) / 8.0;
                const double center[3] = {cam.x + in.fwd.x * zc, cam.y + in.fwd.y * zc, cam.z + in.fwd.z * zc};
                const double texel = 2 * radius / shadowRes_;
                auto dot3 = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
                std::copy(rt, rt + 3, cs.axis[0]);
                std::copy(up, up + 3, cs.axis[1]);
                std::copy(f, f + 3, cs.axis[2]);
                cs.center[0] = std::floor(dot3(center, rt) / texel) * texel;
                cs.center[1] = std::floor(dot3(center, up) / texel) * texel;
                cs.center[2] = dot3(center, f) - radius - 32.0;
                cs.radius = radius;
                cs.depth = 2 * radius + 64.0;
                cs.valid = true;
            }
            // Camera-relative matrix: x' = (rel . R + cam . R - cx) / radius, z' = (rel . F + cam . F - z0) / depth.
            Mat4 m;
            for (int row = 0; row < 3; ++row) {
                const double scale = row < 2 ? 1.0 / cs.radius : 1.0 / cs.depth;
                const double* a = cs.axis[row];
                for (int col = 0; col < 3; ++col) m(row, col) = float(a[col] * scale);
                m(row, 3) = float((a[0] * camPos[0] + a[1] * camPos[1] + a[2] * camPos[2] - cs.center[row]) * scale);
            }
            m(3, 3) = 1;
            std::memcpy(shadowMat_[c], m.m, 64);
            const float texel = float(2 * cs.radius / shadowRes_);
            k.cascade[c][0] = texel;
            k.cascade[c][1] = float(cs.depth);
            k.cascade[c][2] = std::clamp(0.05f / texel, 1.0f, 3.0f);
            k.cascade[c][3] = 1.0f / float(shadowRes_);
        }
        shadowFresh_ = false;
    }
    std::memcpy(k.shadowMat, shadowMat_, sizeof(shadowMat_));
    k.lightDir[3] = cascades ? set.shadowSplits[cascades - 1] : 0.0f;

    k.opts[0] = cascades;
    k.opts[1] = std::clamp(set.shadowTaps, 1u, 16u);
    k.opts[2] = set.ssrSteps;
    k.opts[3] = std::max(set.volumetricSteps, 1u);
    k.flags[0] = (set.waving ? 1u : 0u) | (set.pcss ? 2u : 0u) | (set.volumetricSteps && cascades ? 8u : 0u) |
                 (set.ssrSteps ? 16u : 0u) | 32u | (set.ssao ? 4u : 0u);
    static const u32 debugView = [] {
        const char* v = std::getenv("MCPP_SHADERS_DEBUG");
        return v ? u32(std::atoi(v)) & 15 : 0u;
    }();
    static const u32 tonemap = [] {
        const char* v = std::getenv("MCPP_TONEMAP");
        return v ? u32(std::atoi(v)) & 3 : 0u;
    }();
    k.flags[0] |= debugView << 8 | tonemap << 12;
    k.flags[1] = frameCounter_;
    k.flags[2] = histSlot_;      // previous history
    k.flags[3] = histSlot_ ^ 1;  // written this frame
    std::memcpy(constantsMapped_ + size_t(in.frameIndex) * kConstantBytes, &k, sizeof(k));
}

void Renderer::Shading::fillPassContext(PassContext& pc, const FrameInput& in) const {
    pc.width = renderW_, pc.height = renderH_;
    pc.jitter[0] = in.jitter[0], pc.jitter[1] = in.jitter[1];
    pc.prevViewProj = prevViewProj_;
    // curUv = (pixel + jitterPixels()) / size (shading.hlsli): the unjittered position of the sample.
    pc.motionScale[0] = 1.0f / float(renderW_), pc.motionScale[1] = 1.0f / float(renderH_);
    pc.motionScale[2] = -in.jitter[0] * 0.5f, pc.motionScale[3] = in.jitter[1] * 0.5f;
    pc.shadowCascades = updateMask_ && shadowMap_ ? r.shaderSettings_.cascades : 0;
    pc.shadowMask = pc.shadowCascades ? updateMask_ : 0;
    pc.shadowMats = shadowMat_;
}

void Renderer::Shading::barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER& b = barriers_[barrierCount_++];
    b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
}

void Renderer::Shading::uavBarrier(ID3D12Resource* res) {
    D3D12_RESOURCE_BARRIER& b = barriers_[barrierCount_++];
    b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
}

void Renderer::Shading::flushBarriers(ID3D12GraphicsCommandList* list) {
    if (barrierCount_) list->ResourceBarrier(barrierCount_, barriers_);
    barrierCount_ = 0;
}

void Renderer::Shading::mark(ID3D12GraphicsCommandList* list, const char* name) {
    const u32 f = r.swapchain_->GetCurrentBackBufferIndex();
    u32& n = markCount_[f];
    if (n >= kMaxMarks) return;
    list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, f * kMaxMarks + n);
    markNames_[f][n++] = name;
}

void Renderer::Shading::bindCompute(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS cb) {
    const u32 frame = r.swapchain_->GetCurrentBackBufferIndex();
    list->SetComputeRootSignature(r.rootSig_.Get());
    list->SetComputeRootConstantBufferView(RootFrame, cb);
    list->SetComputeRootConstantBufferView(RootShade, constants_->GetGPUVirtualAddress() + u64(frame) * kConstantBytes);
    list->SetComputeRootDescriptorTable(RootSrvTable, r.gpuDescriptor(descBase_));
    list->SetComputeRootDescriptorTable(RootUavTable, r.gpuDescriptor(descBase_ + TexCount + 2));
}

void Renderer::Shading::bindGraphics(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS cb) {
    const u32 frame = r.swapchain_->GetCurrentBackBufferIndex();
    list->SetGraphicsRootSignature(r.rootSig_.Get());
    list->SetGraphicsRootConstantBufferView(RootFrame, cb);
    list->SetGraphicsRootConstantBufferView(RootShade, constants_->GetGPUVirtualAddress() + u64(frame) * kConstantBytes);
    list->SetGraphicsRootDescriptorTable(RootBlockTex, r.srvHeap_->GetGPUDescriptorHandleForHeapStart());
    list->SetGraphicsRootShaderResourceView(RootRemap, r.remap_->GetGPUVirtualAddress() + u64(frame) * kRemapCapacity * 4);
    list->SetGraphicsRootDescriptorTable(RootSrvTable, r.gpuDescriptor(descBase_));
    list->SetGraphicsRootDescriptorTable(RootUavTable, r.gpuDescriptor(descBase_ + TexCount + 2));
}

void Renderer::Shading::passConstants(ID3D12GraphicsCommandList* list, bool compute, const u32 (&v)[8]) {
    if (compute) list->SetComputeRoot32BitConstants(RootPass, 8, v, 0);
    else list->SetGraphicsRoot32BitConstants(RootPass, 8, v, 0);
}

void Renderer::Shading::record(ID3D12GraphicsCommandList* list, const FrameInput& in, bool haveWorld,
                               D3D12_GPU_VIRTUAL_ADDRESS cb, D3D12_GPU_VIRTUAL_ADDRESS activeVa, u32 activeCount,
                               PassContext& pc, ID3D12Resource* backbuffer, D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv) {
    const ShaderSettings& set = r.shaderSettings_;
    const u32 w = width_, h = height_;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvBase = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    auto rtv = [&](u32 i) { return D3D12_CPU_DESCRIPTOR_HANDLE{rtvBase.ptr + size_t(i) * rtvStride_}; };
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv{dsvHeap_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(4) * dsvStride_};
    const D3D12_VIEWPORT outViewport{0, 0, float(outW_), float(outH_), 0, 1};
    const D3D12_RECT outScissor{0, 0, LONG(outW_), LONG(outH_)};
    pc.width = w, pc.height = h;
    const D3D12_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
    ID3D12Resource* depth = sceneDepth_.Get();
    auto groups = [](u32 n, u32 g) { return (n + g - 1) / g; };
    auto bindWorldBuffers = [&] {
        list->SetGraphicsRootShaderResourceView(RootColors, r.colors_->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(RootSections, r.sections_->GetGPUVirtualAddress());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_INDEX_BUFFER_VIEW ibv{r.indexBuffer_->GetGPUVirtualAddress(), kQuadIndexCapacity * 6 * 4, DXGI_FORMAT_R32_UINT};
        list->IASetIndexBuffer(&ibv);
    };
    auto worldPass = [&](ID3D12PipelineState* pso, int seg, ID3D12Resource* quads) {
        list->SetPipelineState(pso);
        list->SetGraphicsRootShaderResourceView(RootQuads, quads->GetGPUVirtualAddress());
        list->ExecuteIndirect(r.cmdSig_.Get(), r.segCap_[seg], r.args_.Get(), u64(r.segBase_[seg]) * kCommandBytes,
                              r.counts_.Get(), u64(seg) * 4);
    };
    auto runStage = [&](PassStage stage) {
        if (r.passes_[int(stage)].empty()) return;
        Timer t;
        r.runPasses(stage, pc);
        passCpuMs_ += t.ms();
    };
    const u32 zero8[8] = {};

    // ---- path tracer: acceleration structures for this frame ----
    if (r.pt_) {
        r.pt_->build(list, in, cb, haveWorld ? activeCount : 0);
        mark(list, "accel build");
    }

    // ---- shadow maps ----
    const u32 cascades = updateMask_ ? set.cascades : 0;
    if (haveWorld && cascades && shadowMap_) {
        r.transition(shadowCounts_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(shadowCounts_.Get(), 0, r.zeros_.Get(), 0, 64);
        barrier(shadowCounts_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kUav);
        barrier(shadowArgs_.Get(), D3D12_RESOURCE_STATE_COMMON, kUav);
        flushBarriers(list);
        bindCompute(list, cb);
        list->SetPipelineState(psoShadowCull_.Get());
        list->SetComputeRootShaderResourceView(RootSections, r.sections_->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(RootCounts, shadowCounts_->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(RootArgs, shadowArgs_->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(RootActive, activeVa);
        const u32 cullConsts[8] = {0, updateMask_, kShadowSegCap};
        passConstants(list, true, cullConsts);
        list->Dispatch(groups(activeCount, 64), 1, 1);
        barrier(shadowCounts_.Get(), kUav, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        barrier(shadowArgs_.Get(), kUav, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        barrier(shadowMap_.Get(), kSrv, kDepthWrite);
        flushBarriers(list);
        mark(list, "shadow cull");

        bindGraphics(list, cb);
        bindWorldBuffers();
        const D3D12_VIEWPORT sv{0, 0, float(shadowRes_), float(shadowRes_), 0, 1};
        const D3D12_RECT ss{0, 0, LONG(shadowRes_), LONG(shadowRes_)};
        list->RSSetViewports(1, &sv);
        list->RSSetScissorRects(1, &ss);
        for (u32 c = 0; c < cascades; ++c) {
            if (!(updateMask_ & (1u << c))) continue;
            const D3D12_CPU_DESCRIPTOR_HANDLE d{dsvHeap_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(c) * dsvStride_};
            list->ClearDepthStencilView(d, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            list->OMSetRenderTargets(0, nullptr, FALSE, &d);
            const u32 consts[8] = {c};
            passConstants(list, false, consts);
            ID3D12PipelineState* psos[3] = {psoShadow_.Get(), psoShadowCutout_.Get(), psoShadowModel_.Get()};
            for (u32 kind = 0; kind < 3; ++kind) {
                list->SetPipelineState(psos[kind]);
                list->SetGraphicsRootShaderResourceView(RootQuads, (kind == 2 ? r.modelQuads_ : r.cubeQuads_)->GetGPUVirtualAddress());
                const u32 seg = c * 3 + kind;
                list->ExecuteIndirect(r.cmdSig_.Get(), kShadowSegCap, shadowArgs_.Get(), u64(seg) * kShadowSegCap * kCommandBytes,
                                      shadowCounts_.Get(), u64(seg) * 4);
            }
        }
        mark(list, "shadow maps");
        // Entities, block entities, items (RenderPass::drawShadow): this frame's geometry, per updated cascade.
        if (!r.passes_[int(PassStage::World)].empty()) {
            Timer t;
            ID3D12DescriptorHeap* heaps[] = {r.srvHeap_.Get()};
            for (u32 c = 0; c < cascades; ++c) {
                if (!(updateMask_ & (1u << c))) continue;
                const D3D12_CPU_DESCRIPTOR_HANDLE d{dsvHeap_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(c) * dsvStride_};
                list->OMSetRenderTargets(0, nullptr, FALSE, &d);
                list->RSSetViewports(1, &sv);
                list->RSSetScissorRects(1, &ss);
                list->SetDescriptorHeaps(1, heaps);
                r.runShadowPasses({list, in.frameIndex, c, shadowMat_[c], pc.blockTextures, pc.textureRemap});
            }
            passCpuMs_ += t.ms();
            mark(list, "shadow casters");
        }
        barrier(shadowMap_.Get(), kDepthWrite, kSrv);
        flushBarriers(list);
    }

    // ---- sky stage -> LDR layer A ----
    barrier(ldrSky_.Get(), kSrv, kRt);
    barrier(albedo_.Get(), kSrv, kRt);
    barrier(normal_.Get(), kSrv, kRt);
    barrier(ldrFwd_.Get(), kSrv, kRt);
    flushBarriers(list);
    {
        const float clear[4] = {0, 0, 0, 0};  // read as the fog colour where the sky stage draws nothing
        list->ClearRenderTargetView(rtv(2), clear, 0, nullptr);
    }
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE t = rtv(2);
        list->OMSetRenderTargets(1, &t, FALSE, &dsv);
    }
    runStage(PassStage::Sky);
    mark(list, "sky");

    // ---- G-buffer ----
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE t[2] = {rtv(0), rtv(1)};
        list->OMSetRenderTargets(2, t, FALSE, &dsv);
    }
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    bindGraphics(list, cb);
    passConstants(list, false, zero8);
    if (haveWorld) {
        bindWorldBuffers();
        worldPass(psoGbuf_.Get(), 0, r.cubeQuads_.Get());
        worldPass(psoGbufCutout_.Get(), 1, r.cubeQuads_.Get());
        worldPass(psoGbufModel_.Get(), 3, r.modelQuads_.Get());
    }
    mark(list, "g-buffer");

    // ---- World stage (entities) -> LDR layer B; moving geometry writes its motion (until MotionCS) ----
    barrier(motion_.Get(), kSrv, kRt);
    flushBarriers(list);
    {
        const float clear[4] = {0, 0, 0, 0}, noMotion[4] = {kNoMotion, kNoMotion, 0, 0};
        list->ClearRenderTargetView(rtv(3), clear, 0, nullptr);
        list->ClearRenderTargetView(rtv(6), noMotion, 0, nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE t = rtv(3);
        list->OMSetRenderTargets(1, &t, FALSE, &dsv);
        pc.colorRtv = t, pc.motionRtv = rtv(6), pc.dsv = dsv;
        runStage(PassStage::World);
    }
    mark(list, "entities");

    // ---- deferred lighting ----
    barrier(albedo_.Get(), kRt, kSrv);
    barrier(normal_.Get(), kRt, kSrv);
    barrier(ldrSky_.Get(), kRt, kSrv);
    barrier(ldrFwd_.Get(), kRt, kSrv);
    barrier(depth, kDepthWrite, kDepthRead);
    barrier(scene_.Get(), kSrv, kUav);
    barrier(sceneCopy_.Get(), kSrv, kUav);
    barrier(linDepth_.Get(), kSrv, kUav);
    flushBarriers(list);
    ID3D12DescriptorHeap* heaps[] = {r.srvHeap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    bindCompute(list, cb);
    passConstants(list, true, zero8);
    if (set.ssao) {
        barrier(ao_.Get(), kSrv, kUav);
        flushBarriers(list);
        list->SetPipelineState(psoAo_.Get());
        list->Dispatch(groups((w + 1) / 2, 8), groups((h + 1) / 2, 8), 1);
        barrier(ao_.Get(), kUav, kSrv);
        flushBarriers(list);
        mark(list, "ssao");
    }
    list->SetPipelineState(psoDeferred_.Get());
    list->Dispatch(groups(w, 8), groups(h, 8), 1);
    mark(list, "deferred");
    if (r.pt_) {  // the traced radius, over the deferred result
        uavBarrier(scene_.Get());
        uavBarrier(sceneCopy_.Get());
        uavBarrier(linDepth_.Get());
        flushBarriers(list);
        r.pt_->trace(list, in, cb);
        mark(list, "path trace");
    }

    // ---- translucent terrain (water, glass) ----
    barrier(scene_.Get(), kUav, kRt);
    barrier(sceneCopy_.Get(), kUav, kSrv);
    barrier(linDepth_.Get(), kUav, kSrv);
    barrier(depth, kDepthRead, kDepthWrite);
    flushBarriers(list);
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE t = rtv(4);
        list->OMSetRenderTargets(1, &t, FALSE, &dsv);
    }
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    bindGraphics(list, cb);
    passConstants(list, false, zero8);
    if (haveWorld) {
        bindWorldBuffers();
        worldPass(psoFwd_.Get(), 2, r.cubeQuads_.Get());
    }
    mark(list, "translucent");

    // ---- AfterTranslucent stage (clouds, weather) -> LDR layer A (cleared) ----
    barrier(ldrSky_.Get(), kSrv, kRt);
    flushBarriers(list);
    {
        const float clear[4] = {0, 0, 0, 0};
        list->ClearRenderTargetView(rtv(2), clear, 0, nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE t = rtv(2);
        list->OMSetRenderTargets(1, &t, FALSE, &dsv);
        pc.colorRtv = t;
        runStage(PassStage::AfterTranslucent);
        pc.colorRtv = {}, pc.motionRtv = {}, pc.dsv = {};
    }
    mark(list, "clouds+weather");

    // ---- volumetric light + composite ----
    barrier(ldrSky_.Get(), kRt, kSrv);
    barrier(depth, kDepthWrite, kDepthRead);
    barrier(scene_.Get(), kRt, kUav);
    const bool volumetric = set.volumetricSteps && cascades;
    if (volumetric) barrier(vol_.Get(), kSrv, kUav);
    flushBarriers(list);
    list->SetDescriptorHeaps(1, heaps);
    bindCompute(list, cb);
    const u32 shift = std::clamp(set.volumetricShift, 1u, 2u);
    const u32 volConsts[8] = {shift};
    passConstants(list, true, volConsts);
    if (volumetric) {
        list->SetPipelineState(psoVolumetric_.Get());
        list->Dispatch(groups((w + (1 << shift) - 1) >> shift, 8), groups((h + (1 << shift) - 1) >> shift, 8), 1);
        barrier(vol_.Get(), kUav, kSrv);
        flushBarriers(list);
        mark(list, "volumetric");
    }
    list->SetPipelineState(psoComposite_.Get());
    list->Dispatch(groups(w, 8), groups(h, 8), 1);
    mark(list, "composite");

    // ---- motion vectors ----
    barrier(scene_.Get(), kUav, kSrv);
    barrier(motion_.Get(), kRt, kUav);
    flushBarriers(list);
    list->SetPipelineState(psoMotion_.Get());
    list->Dispatch(groups(w, 8), groups(h, 8), 1);
    barrier(motion_.Get(), kUav, kSrv);
    flushBarriers(list);
    mark(list, "motion");

    // ---- temporal upscale (built-in TAA or an SDK upscaler) -> history (output resolution) ----
    const u32 cur = histSlot_ ^ 1;
    barrier(hist_[cur].Get(), kSrv, kUav);
    flushBarriers(list);
    {
        UpscaleInputs u;
        u.list = list;
        u.color = scene_.Get();
        u.depth = depth;
        u.motion = motion_.Get();
        u.reactive = ldrFwd_.Get();
        u.exposure = exposureTex_.Get();
        u.output = hist_[cur].Get();
        u.jitter[0] = -in.jitter[0] * 0.5f * float(w);
        u.jitter[1] = in.jitter[1] * 0.5f * float(h);
        u.renderSize[0] = w, u.renderSize[1] = h;
        u.outputSize[0] = outW_, u.outputSize[1] = outH_;
        u.fovY = in.cam->fovY;
        u.aspect = float(outW_) / float(std::max(outH_, 1u));
        u.frameTimeMs = dt_ * 1000.0f;
        u.reset = !historyValid_;
        Upscaler* up = r.upscaler_ ? r.upscaler_ : &taa_;
        up->evaluate(u);
        if (up != &taa_) {  // external: restore the pipeline's bindings
            list->SetDescriptorHeaps(1, heaps);
            bindCompute(list, cb);
        }
    }
    barrier(hist_[cur].Get(), kUav, kSrv);
    flushBarriers(list);
    mark(list, "taa/upscale");

    // ---- bloom + exposure ----
    {
        const u32 consts[8] = {TexHist0 + cur, asUint(dt_), exposureReset_ ? 1u : 0u};
        passConstants(list, true, consts);
        barrier(exposureTex_.Get(), kSrv, kUav);
        flushBarriers(list);
        list->SetPipelineState(psoExposure_.Get());
        list->Dispatch(1, 1, 1);
        exposureReset_ = false;
    }
    barrier(exposureTex_.Get(), kUav, kSrv);
    uavBarrier(state_.Get());
    flushBarriers(list);
    list->SetPipelineState(psoBloomDown_.Get());
    for (u32 i = 0; i < kBloomLevels; ++i) {
        const u32 consts[8] = {i == 0 ? TexHist0 + cur : RwBloom0 + i - 1, RwBloom0 + i, i == 0 ? 1u : 0u, 0,
                               bloomW_[i], bloomH_[i], i ? bloomW_[i - 1] : outW_, i ? bloomH_[i - 1] : outH_};
        passConstants(list, true, consts);
        list->Dispatch(groups(bloomW_[i], 8), groups(bloomH_[i], 8), 1);
        uavBarrier(bloom_[i].Get());
        flushBarriers(list);
    }
    list->SetPipelineState(psoBloomUp_.Get());
    for (int i = int(kBloomLevels) - 2; i >= 0; --i) {
        const u32 consts[8] = {RwBloom0 + u32(i) + 1, RwBloom0 + u32(i), asUint(1.0f), 0,
                               bloomW_[i], bloomH_[i], bloomW_[i + 1], bloomH_[i + 1]};
        passConstants(list, true, consts);
        list->Dispatch(groups(bloomW_[i], 8), groups(bloomH_[i], 8), 1);
        uavBarrier(bloom_[i].Get());
        flushBarriers(list);
    }
    uavBarrier(state_.Get());
    barrier(bloom_[0].Get(), kUav, kSrv);
    flushBarriers(list);
    mark(list, "bloom+exposure");

    // ---- tonemap -> swap chain (the "hudless" image) ----
    (void)backbuffer;
    list->OMSetRenderTargets(1, &backbufferRtv, FALSE, nullptr);
    list->RSSetViewports(1, &outViewport);
    list->RSSetScissorRects(1, &outScissor);
    bindGraphics(list, cb);
    const u32 tmConsts[8] = {cur, asUint(set.sharpen)};
    passConstants(list, false, tmConsts);
    list->SetPipelineState(psoTonemap_.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    mark(list, "tonemap");

    barrier(bloom_[0].Get(), kSrv, kUav);
    barrier(depth, kDepthRead, kDepthWrite);
    // ---- UI layer: the overlays and the Hud stage draw into it next (Renderer::render), then compositeUi ----
    barrier(ui_.Get(), kSrv, kRt);
    flushBarriers(list);
    D3D12_CPU_DESCRIPTOR_HANDLE overlayDsv = dsv;
    if (sceneDepth_.Get() != r.depth_.Get()) {  // output-resolution depth for the block outline
        overlayDsv = r.dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        barrier(depth, kDepthWrite, kDepthRead);
        flushBarriers(list);
        list->OMSetRenderTargets(0, nullptr, FALSE, &overlayDsv);
        list->SetPipelineState(psoDepthUp_.Get());
        list->DrawInstanced(3, 1, 0, 0);
        barrier(depth, kDepthRead, kDepthWrite);
        flushBarriers(list);
    }
    {
        const float clear[4] = {0, 0, 0, 0};
        list->ClearRenderTargetView(rtv(5), clear, 0, nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE t = rtv(5);
        list->OMSetRenderTargets(1, &t, FALSE, &overlayDsv);
    }
    pc.width = outW_, pc.height = outH_;
    histSlot_ = cur;
    historyValid_ = true;
}

bool Renderer::Shading::uiComposite() {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = r.rootSig_.Get();
    d.VS = {g_TonemapVS, sizeof(g_TonemapVS)};
    d.PS = {g_UiCompositePS, sizeof(g_UiCompositePS)};
    auto& rt = d.BlendState.RenderTarget[0];
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    rt.BlendEnable = TRUE;
    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = kLdr;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.SampleDesc.Count = 1;
    return check(r.device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&psoUiComposite_)), "ui composite pipeline");
}

// The UI layer over the tonemapped image. Frame generation: the back buffer before this call is the hudless image
// and the UI layer (ui_, premultiplied R8G8B8A8) the separate UI.
void Renderer::Shading::compositeUi(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS cb,
                                    D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv) {
    mark(list, "overlays+hud");
    barrier(ui_.Get(), kRt, kSrv);
    flushBarriers(list);
    ID3D12DescriptorHeap* heaps[] = {r.srvHeap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    bindGraphics(list, cb);
    const D3D12_VIEWPORT v{0, 0, float(outW_), float(outH_), 0, 1};
    const D3D12_RECT s{0, 0, LONG(outW_), LONG(outH_)};
    list->RSSetViewports(1, &v);
    list->RSSetScissorRects(1, &s);
    list->OMSetRenderTargets(1, &backbufferRtv, FALSE, nullptr);
    list->SetPipelineState(psoUiComposite_.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    mark(list, "ui composite");
}

// ---- built-in TAA as the default Upscaler ----

void Renderer::Shading::Taa::evaluate(const UpscaleInputs& in) {
    const u32 consts[8] = {in.reset ? 0u : 1u};
    s.passConstants(in.list, true, consts);
    in.list->SetPipelineState(s.psoTaa_.Get());
    in.list->Dispatch((in.outputSize[0] + 7) / 8, (in.outputSize[1] + 7) / 8, 1);
}

void Renderer::Shading::readTimings(u32 frame) {
    const u32 n = markCount_[frame];
    markCount_[frame] = 0;
    if (n < 2) return;
    D3D12_RANGE range{size_t(frame) * kMaxMarks * 8, size_t(frame) * kMaxMarks * 8 + n * 8}, none{0, 0};
    u8* p;
    if (FAILED(queryReadback_->Map(0, &range, reinterpret_cast<void**>(&p)))) return;
    u64 t[kMaxMarks];
    std::memcpy(t, p + range.Begin, n * 8);
    queryReadback_->Unmap(0, &none);
    RenderStats& s = r.stats_;
    s.passCount = std::min<u32>(n - 1, RenderStats::kMaxPasses);
    const double k = 1000.0 / double(r.timestampFreq_);
    for (u32 i = 0; i < s.passCount; ++i) {
        s.passNames[i] = markNames_[frame][i + 1];
        s.passMs[i] = t[i + 1] >= t[i] ? float(double(t[i + 1] - t[i]) * k) : 0.0f;
    }
    if (++timingFrames_ > 30) {  // skip start-up
        for (u32 i = 0; i < s.passCount; ++i) {
            timingNames_[i] = s.passNames[i];
            auto& v = timingSamples_[i];
            if (v.size() < kTimingHistory) v.push_back(s.passMs[i]);
            else v[timingFrames_ % kTimingHistory] = s.passMs[i];
        }
    }
}

void Renderer::Shading::logTimings() const {
    if (timingFrames_ <= 30) return;
    char line[1024];
    int len = std::snprintf(line, sizeof(line), "shaders %s: gpu ms per pass (median of %zu frames):",
                            Renderer::shaderPresetName(r.shaderPreset_), timingSamples_[0].size());
    float total = 0;
    for (u32 i = 0; i < RenderStats::kMaxPasses && timingNames_[i] && len < int(sizeof(line)) - 64; ++i) {
        std::vector<float> v = timingSamples_[i];
        if (v.empty()) continue;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        total += v[v.size() / 2];
        len += std::snprintf(line + len, sizeof(line) - len, " %s %.3f |", timingNames_[i], v[v.size() / 2]);
    }
    std::snprintf(line + len, sizeof(line) - len, " sum %.3f | pipeline cpu %.3f ms/frame (+ %.3f in Sky/World/Late passes)",
                  total, cpuFrames_ ? (cpuMs_ - passCpuMs_) / double(cpuFrames_) : 0.0,
                  cpuFrames_ ? passCpuMs_ / double(cpuFrames_) : 0.0);
    LOG("%s", line);
}
