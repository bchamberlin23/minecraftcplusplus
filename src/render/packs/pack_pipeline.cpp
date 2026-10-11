// Shader pack pipeline: loading (background translation + DXIL + PSOs), GPU resources. Per-frame recording is in
// pack_frame.cpp, program helpers in pack_programs.cpp.
#include "render/packs/pack_programs.h"

#include "assets/assets.h"
#include "render/packs/pack_api.h"
#include "render/upscalers/upscalers.h"

#include <stb/stb_image.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <set>

#include "PackDepthFlipPS.h"
#include "PackDepthUpPS.h"
#include "PackMotionCS.h"
#include "PackUpCopyPS.h"
#include "PackDepthToGlCS.h"
#include "PackMergePS.h"
#include "PackMergeVS.h"
#include "PackMipCS.h"
#include "PackShadowCullCS.h"

using namespace packs;

namespace packs {

DXGI_FORMAT dxgiFormat(TexFormat f) {
    switch (f) {
        case TexFormat::RGBA8: case TexFormat::RGB8: case TexFormat::RGB5_A1: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case TexFormat::R8: return DXGI_FORMAT_R8_UNORM;
        case TexFormat::RG8: return DXGI_FORMAT_R8G8_UNORM;
        case TexFormat::RGBA16: case TexFormat::RGB16: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case TexFormat::R16: return DXGI_FORMAT_R16_UNORM;
        case TexFormat::RG16: return DXGI_FORMAT_R16G16_UNORM;
        case TexFormat::RGBA16F: case TexFormat::RGB16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case TexFormat::R16F: return DXGI_FORMAT_R16_FLOAT;
        case TexFormat::RG16F: return DXGI_FORMAT_R16G16_FLOAT;
        case TexFormat::RGBA32F: case TexFormat::RGB32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case TexFormat::R32F: return DXGI_FORMAT_R32_FLOAT;
        case TexFormat::RG32F: return DXGI_FORMAT_R32G32_FLOAT;
        case TexFormat::R11F_G11F_B10F: case TexFormat::RGB9_E5: return DXGI_FORMAT_R11G11B10_FLOAT;
        case TexFormat::RGB10_A2: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case TexFormat::RGBA8_SNORM: case TexFormat::RGB8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
        case TexFormat::R8_SNORM: return DXGI_FORMAT_R8_SNORM;
        case TexFormat::RG8_SNORM: return DXGI_FORMAT_R8G8_SNORM;
        case TexFormat::RGBA16_SNORM: case TexFormat::RGB16_SNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
        case TexFormat::R16_SNORM: return DXGI_FORMAT_R16_SNORM;
        case TexFormat::RG16_SNORM: return DXGI_FORMAT_R16G16_SNORM;
        case TexFormat::R8I: return DXGI_FORMAT_R8_SINT;
        case TexFormat::RG8I: return DXGI_FORMAT_R8G8_SINT;
        case TexFormat::RGBA8I: case TexFormat::RGB8I: return DXGI_FORMAT_R8G8B8A8_SINT;
        case TexFormat::R8UI: return DXGI_FORMAT_R8_UINT;
        case TexFormat::RG8UI: return DXGI_FORMAT_R8G8_UINT;
        case TexFormat::RGBA8UI: case TexFormat::RGB8UI: return DXGI_FORMAT_R8G8B8A8_UINT;
        case TexFormat::R16I: return DXGI_FORMAT_R16_SINT;
        case TexFormat::RG16I: return DXGI_FORMAT_R16G16_SINT;
        case TexFormat::RGBA16I: case TexFormat::RGB16I: return DXGI_FORMAT_R16G16B16A16_SINT;
        case TexFormat::R16UI: return DXGI_FORMAT_R16_UINT;
        case TexFormat::RG16UI: return DXGI_FORMAT_R16G16_UINT;
        case TexFormat::RGBA16UI: case TexFormat::RGB16UI: return DXGI_FORMAT_R16G16B16A16_UINT;
        case TexFormat::R32I: return DXGI_FORMAT_R32_SINT;
        case TexFormat::RG32I: return DXGI_FORMAT_R32G32_SINT;
        case TexFormat::RGBA32I: case TexFormat::RGB32I: return DXGI_FORMAT_R32G32B32A32_SINT;
        case TexFormat::R32UI: return DXGI_FORMAT_R32_UINT;
        case TexFormat::RG32UI: return DXGI_FORMAT_R32G32_UINT;
        case TexFormat::RGBA32UI: case TexFormat::RGB32UI: return DXGI_FORMAT_R32G32B32A32_UINT;
        default: return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

bool formatIsInt(TexFormat f) {
    switch (f) {
        case TexFormat::R8I: case TexFormat::RG8I: case TexFormat::RGBA8I: case TexFormat::RGB8I:
        case TexFormat::R8UI: case TexFormat::RG8UI: case TexFormat::RGBA8UI: case TexFormat::RGB8UI:
        case TexFormat::R16I: case TexFormat::RG16I: case TexFormat::RGBA16I: case TexFormat::RGB16I:
        case TexFormat::R16UI: case TexFormat::RG16UI: case TexFormat::RGBA16UI: case TexFormat::RGB16UI:
        case TexFormat::R32I: case TexFormat::RG32I: case TexFormat::RGBA32I: case TexFormat::RGB32I:
        case TexFormat::R32UI: case TexFormat::RG32UI: case TexFormat::RGBA32UI: case TexFormat::RGB32UI: return true;
        default: return false;
    }
}

const char* rtType(TexFormat f) {
    if (!formatIsInt(f)) return "float4";
    switch (f) {
        case TexFormat::R8UI: case TexFormat::RG8UI: case TexFormat::RGBA8UI: case TexFormat::RGB8UI: case TexFormat::R16UI:
        case TexFormat::RG16UI: case TexFormat::RGBA16UI: case TexFormat::RGB16UI: case TexFormat::R32UI:
        case TexFormat::RG32UI: case TexFormat::RGBA32UI: case TexFormat::RGB32UI: return "uint4";
        default: return "int4";
    }
}

}  // namespace packs

namespace {

bool check(HRESULT hr, const char* what, std::string* error = nullptr) {
    if (SUCCEEDED(hr)) return true;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    LOG("error: shader pack: %s", buf);
    if (error && error->empty()) *error = buf;
    return false;
}

fs::path cacheDirectory() {
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    return fs::path(local ? local : L".") / "minecraftCplusplus" / "shadercache" / "packs";
}

D3D12_BLEND blendFactor(BlendFactor f, bool alpha) {
    switch (f) {
        case BlendFactor::Zero: return D3D12_BLEND_ZERO;
        case BlendFactor::One: return D3D12_BLEND_ONE;
        case BlendFactor::SrcColor: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case BlendFactor::DstColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case BlendFactor::OneMinusDstColor: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case BlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::DstAlpha: return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
        case BlendFactor::SrcAlphaSaturate: return alpha ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA_SAT;
    }
    return D3D12_BLEND_ONE;
}

D3D12_SAMPLER_DESC samplerDesc(Samp k) {
    D3D12_SAMPLER_DESC s{};
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.MaxAnisotropy = 1;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    auto mode = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    switch (k) {
        case Samp::PointClamp: s.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; break;
        case Samp::LinearClamp: s.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT; break;
        case Samp::LinearMipClamp: s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; break;
        case Samp::PointMipClamp: s.Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR; break;
        case Samp::PointWrap: s.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; mode = D3D12_TEXTURE_ADDRESS_MODE_WRAP; break;
        case Samp::LinearWrap: s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; mode = D3D12_TEXTURE_ADDRESS_MODE_WRAP; break;
        case Samp::AtlasWrap:  // the vanilla atlas: nearest texels, linear between mip levels (4 levels)
            s.Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
            mode = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            s.MaxLOD = 4;
            break;
        case Samp::ShadowCmpLinear:
            s.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
            s.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            break;
        case Samp::ShadowCmpPoint:
            s.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
            s.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            break;
        default: break;
    }
    s.AddressU = s.AddressV = s.AddressW = mode;
    return s;
}

bool serializeRoot(ID3D12Device* dev, const D3D12_ROOT_SIGNATURE_DESC& rd, ComPtr<ID3D12RootSignature>& out,
                   std::string& error) {
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        error = std::string("root signature: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    return check(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out)),
                 "CreateRootSignature", &error);
}

}  // namespace

// ==== lifecycle ==========================================================================================================

Renderer::Packs::Packs(Renderer& renderer)
    : r(renderer), game_(std::make_unique<PackGameState>()), player_(std::make_unique<PackPlayerState>()) {}

Renderer::Packs::~Packs() {
    ++generation_;
    if (loader_.joinable()) loader_.join();
    if (pipe_) logTimings(*pipe_);
}

void Renderer::Packs::request(const fs::path& path, const LoadOptions& options) {
    const u64 gen = ++generation_;
    requestPath_ = path, requestOptions_ = options;
    requestOptions_.dimension = game_->dimension;
    {
        std::lock_guard lock(mutex_);
        status_.loading = path.filename().string();
        status_.error.clear();
        disableRequested_ = false;
    }
    if (loader_.joinable()) loader_.join();  // a previous load finishes first (its result is discarded)
    loader_ = std::thread([this, path, options = requestOptions_, gen] { loadThread(path, options, gen); });
}

void Renderer::Packs::disable() {
    ++generation_;
    requestPath_.clear();
    std::lock_guard lock(mutex_);
    disableRequested_ = true;
    status_.loading.clear();
}

Renderer::Packs::Status Renderer::Packs::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

void Renderer::Packs::setGameState(const PackGameState& s) {
    const bool dimensionChanged = s.dimension != game_->dimension;
    *game_ = s;
    // Iris reloads the pack for the new dimension's folder (world0 / world-1 / world1 / dimension.properties).
    if (dimensionChanged && !requestPath_.empty()) request(fs::path(requestPath_), requestOptions_);
}

void Renderer::Packs::setPlayerState(const PackPlayerState& s) { *player_ = s; }

void Renderer::Packs::loadThread(fs::path path, LoadOptions options, u64 generation) {
    Timer t;
    auto def = std::make_unique<PackDefinition>();
    std::string error;
    bool ok = def->load(path, options, error);
    std::unique_ptr<Pipe> pipe;
    if (ok && generation_ == generation) ok = build(std::move(def), pipe, error);
    std::lock_guard lock(mutex_);
    if (generation_ != generation) return;
    status_.loading.clear();
    if (!ok) {
        status_.error = path.filename().string() + ": " + error;
        LOG("error: shader pack %s (keeping the current pipeline)", status_.error.c_str());
        return;
    }
    status_.loadMs = t.ms();
    pendingPipe_ = std::move(pipe);
}

void Renderer::Packs::beginFrame() {
    std::unique_ptr<Pipe> next;
    bool disable = false;
    {
        std::lock_guard lock(mutex_);
        next = std::move(pendingPipe_);
        disable = disableRequested_;
        disableRequested_ = false;
    }
    if (disable && pipe_) {
        r.waitIdle();
        LOG("shader pack %s disabled", pipe_->name.c_str());
        logTimings(*pipe_);
        pipe_.reset();
        casters_.clear();
        std::lock_guard lock(mutex_);
        status_.pack.clear();
    }
    if (!next) {
        if (pipe_) {
            resize();  // an upscaler switched on / off or its render size changed
            updateLayerTable(*pipe_);
        }
        return;
    }
    r.waitIdle();
    std::string error;
    if (!finalize(*next, error)) {
        std::lock_guard lock(mutex_);
        status_.error = next->name + ": " + error;
        LOG("error: shader pack %s (keeping the current pipeline)", status_.error.c_str());
        return;
    }
    if (pipe_) logTimings(*pipe_);
    pipe_ = std::move(next);
    casters_.clear();
    r.resetTemporal();
    std::lock_guard lock(mutex_);
    status_.pack = pipe_->name;
    status_.warnings = pipe_->warnings;
    LOG("shader pack %s active (%.0f ms to load)", pipe_->name.c_str(), status_.loadMs);
}

void Renderer::Packs::resize() {
    if (!pipe_) return;
    u32 w, h;
    bool scaled;
    targetSize(w, h, scaled);
    if (pipe_->width != w || pipe_->height != h || pipe_->scaled != scaled || pipe_->outW != r.width_ ||
        pipe_->outH != r.height_) {
        r.waitIdle();
        createTargets(*pipe_);
        pipe_->upHistory = false;
        if (scaled != pipe_->scaled) LOG("shader pack: %ux%u", w, h);
    }
}

// A vendor upscaler (DLSS / FSR / XeSS) is selected: the pack renders at its render size (see Pipe::scaled).
void Renderer::Packs::targetSize(u32& w, u32& h, bool& scaled) const {
    scaled = r.upscaler_ != nullptr && r.upscalers_;
    float scale = 1.0f;
    if (scaled) {
        const VideoSettings& v = r.upscalers_->settings();
        ScaleCache& c = scaleCache_;
        if (c.kind != int(v.upscaler) || c.quality != int(v.quality) || c.w != r.width_ || c.h != r.height_) {
            c.kind = int(v.upscaler), c.quality = int(v.quality), c.w = r.width_, c.h = r.height_;
            c.scale = std::clamp(r.upscalers_->renderScale(v.upscaler, v.quality), 0.33f, 1.0f);
        }
        scale = c.scale;
    }
    w = std::max(1u, u32(float(r.width_) * scale + 0.5f));
    h = std::max(1u, u32(float(r.height_) * scale + 0.5f));
}

// ==== build (background thread) ===========================================================================================

bool Renderer::Packs::build(std::unique_ptr<PackDefinition> defPtr, std::unique_ptr<Pipe>& out, std::string& error) {
    ID3D12Device* dev = r.device_.Get();
    auto pipe = std::make_unique<Pipe>();
    Pipe& p = *pipe;
    p.def = std::move(defPtr);
    PackDefinition& def = *p.def;
    p.name = def.source.name();
    p.warnings = def.warnings;

    // ---- root signatures ----
    {
        D3D12_DESCRIPTOR_RANGE vsTex{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaxStageTextures, 0, 0, 0};
        D3D12_DESCRIPTOR_RANGE psTex = vsTex, vsSamp{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kMaxStageTextures, 0, 0, 0};
        D3D12_DESCRIPTOR_RANGE psSamp = vsSamp;
        D3D12_DESCRIPTOR_RANGE drawTex{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 2, 0};
        D3D12_DESCRIPTOR_RANGE drawSamp{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 3, 0, 2, 0};
        D3D12_DESCRIPTOR_RANGE images{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kMaxImages, 0, 0, 0};
        D3D12_DESCRIPTOR_RANGE buffers{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kMaxBuffers, 0, 1, 0};
        D3D12_ROOT_PARAMETER params[kRootCount + 1]{};
        auto cbv = [&](u32 i, u32 reg, D3D12_SHADER_VISIBILITY vis) {
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[i].Descriptor.ShaderRegister = reg;
            params[i].ShaderVisibility = vis;
        };
        auto srv = [&](u32 i, u32 reg, u32 space) {
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            params[i].Descriptor.ShaderRegister = reg;
            params[i].Descriptor.RegisterSpace = space;
            params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        };
        auto table = [&](u32 i, D3D12_DESCRIPTOR_RANGE* range, D3D12_SHADER_VISIBILITY vis) {
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i].DescriptorTable.NumDescriptorRanges = 1;
            params[i].DescriptorTable.pDescriptorRanges = range;
            params[i].ShaderVisibility = vis;
        };
        cbv(kRootVsUniforms, 0, D3D12_SHADER_VISIBILITY_VERTEX);
        cbv(kRootPsUniforms, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        params[kRootDraw].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[kRootDraw].Constants.ShaderRegister = 1;
        params[kRootDraw].Constants.Num32BitValues = 2;
        cbv(kRootFrame, 2, D3D12_SHADER_VISIBILITY_ALL);
        srv(kRootGeometry, 0, 1);
        srv(kRootSections, 1, 1);
        srv(kRootColors, 2, 1);
        srv(kRootRemap, 3, 1);
        srv(kRootLayers, 4, 1);
        table(kRootVsTextures, &vsTex, D3D12_SHADER_VISIBILITY_VERTEX);
        table(kRootPsTextures, &psTex, D3D12_SHADER_VISIBILITY_PIXEL);
        table(kRootVsSamplers, &vsSamp, D3D12_SHADER_VISIBILITY_VERTEX);
        table(kRootPsSamplers, &psSamp, D3D12_SHADER_VISIBILITY_PIXEL);
        table(kRootDrawTextures, &drawTex, D3D12_SHADER_VISIBILITY_ALL);
        table(kRootDrawSamplers, &drawSamp, D3D12_SHADER_VISIBILITY_ALL);
        table(kRootImages, &images, D3D12_SHADER_VISIBILITY_ALL);
        table(kRootBuffers, &buffers, D3D12_SHADER_VISIBILITY_ALL);
        // b3: per-pass constants (alpha test) for the fragment wrapper.
        params[kRootCount].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[kRootCount].Constants.ShaderRegister = 3;
        params[kRootCount].Constants.Num32BitValues = 4;
        params[kRootCount].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = kRootCount + 1;
        rd.pParameters = params;
        if (!serializeRoot(dev, rd, p.root, error)) return false;

        D3D12_DESCRIPTOR_RANGE cTex{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaxStageTextures, 0, 0, 0};
        D3D12_DESCRIPTOR_RANGE cSamp{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kMaxStageTextures, 0, 0, 0};
        D3D12_DESCRIPTOR_RANGE cImg = images, cBuf = buffers, cDrawTex = drawTex, cDrawSamp = drawSamp;
        D3D12_ROOT_PARAMETER cp[kCCount]{};
        cp[kCUniforms].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        cp[kCUniforms].Descriptor.ShaderRegister = 0;
        auto ctable = [&](u32 i, D3D12_DESCRIPTOR_RANGE* range) {
            cp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            cp[i].DescriptorTable.NumDescriptorRanges = 1;
            cp[i].DescriptorTable.pDescriptorRanges = range;
        };
        ctable(kCTextures, &cTex);
        ctable(kCSamplers, &cSamp);
        ctable(kCImages, &cImg);
        ctable(kCBuffers, &cBuf);
        ctable(kCDrawTextures, &cDrawTex);
        ctable(kCDrawSamplers, &cDrawSamp);
        cp[kCFrame].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        cp[kCFrame].Descriptor.ShaderRegister = 2;
        D3D12_ROOT_SIGNATURE_DESC cd{};
        cd.NumParameters = kCCount;
        cd.pParameters = cp;
        if (!serializeRoot(dev, cd, p.computeRoot, error)) return false;

        D3D12_DESCRIPTOR_RANGE uSrc{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, uDst{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER up[7]{};
        up[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        up[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        up[1].DescriptorTable.NumDescriptorRanges = 1;
        up[1].DescriptorTable.pDescriptorRanges = &uSrc;
        up[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        up[2].DescriptorTable.NumDescriptorRanges = 1;
        up[2].DescriptorTable.pDescriptorRanges = &uDst;
        up[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        up[3].Descriptor.ShaderRegister = 1;
        up[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        up[4].Descriptor.ShaderRegister = 2;
        up[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        up[5].Descriptor.ShaderRegister = 1;
        up[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        up[6].Descriptor.ShaderRegister = 2;
        D3D12_ROOT_SIGNATURE_DESC ud{};
        ud.NumParameters = 7;
        ud.pParameters = up;
        if (!serializeRoot(dev, ud, p.utilRoot, error)) return false;

        D3D12_INDIRECT_ARGUMENT_DESC args[2]{};
        args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        args[0].Constant.RootParameterIndex = kRootDraw;
        args[0].Constant.Num32BitValuesToSet = 2;
        args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC sd{};
        sd.ByteStride = 28;
        sd.NumArgumentDescs = 2;
        sd.pArgumentDescs = args;
        if (!check(dev->CreateCommandSignature(&sd, p.root.Get(), IID_PPV_ARGS(&p.cmdSig)), "CreateCommandSignature", &error))
            return false;

        auto computePso = [&](const void* code, size_t size, ComPtr<ID3D12PipelineState>& o) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
            d.pRootSignature = p.utilRoot.Get();
            d.CS = {code, size};
            return check(dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&o)), "utility pipeline", &error);
        };
        if (!computePso(g_PackDepthToGlCS, sizeof(g_PackDepthToGlCS), p.psoDepthToGl) ||
            !computePso(g_PackMipCS, sizeof(g_PackMipCS), p.psoMip) ||
            !computePso(g_PackShadowCullCS, sizeof(g_PackShadowCullCS), p.psoShadowCull) ||
            !computePso(g_PackMotionCS, sizeof(g_PackMotionCS), p.psoMotion))
            return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC m{};
        m.pRootSignature = p.utilRoot.Get();
        m.VS = {g_PackMergeVS, sizeof(g_PackMergeVS)};
        m.PS = {g_PackMergePS, sizeof(g_PackMergePS)};
        auto& rt = m.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA, rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA, rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ZERO, rt.DestBlendAlpha = D3D12_BLEND_ONE, rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        m.SampleMask = UINT_MAX;
        m.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        m.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        m.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        m.NumRenderTargets = 1;
        m.RTVFormats[0] = dxgiFormat(def.settings.targets[0].format);
        m.SampleDesc.Count = 1;
        if (!check(dev->CreateGraphicsPipelineState(&m, IID_PPV_ARGS(&p.psoMerge)), "merge pipeline", &error)) return false;
        m.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;  // a buffer as is onto the back buffer
        rt.BlendEnable = FALSE;
        if (!check(dev->CreateGraphicsPipelineState(&m, IID_PPV_ARGS(&p.psoCopy)), "copy pipeline", &error)) return false;
        m.PS = {g_PackUpCopyPS, sizeof(g_PackUpCopyPS)};  // the upscaler's output onto the back buffer (no flip)
        if (!check(dev->CreateGraphicsPipelineState(&m, IID_PPV_ARGS(&p.psoUpCopy)), "upscale copy pipeline", &error))
            return false;
        m.PS = {g_PackMergePS, sizeof(g_PackMergePS)};
        rt.BlendEnable = TRUE;
        // Depth the D3D way up (no colour target, writes depth everywhere).
        m.PS = {g_PackDepthFlipPS, sizeof(g_PackDepthFlipPS)};
        m.NumRenderTargets = 0;
        m.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        m.BlendState.RenderTarget[0].BlendEnable = FALSE;
        m.DepthStencilState.DepthEnable = TRUE;
        m.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        m.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        m.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        if (!check(dev->CreateGraphicsPipelineState(&m, IID_PPV_ARGS(&p.psoDepthFlip)), "depth flip pipeline", &error))
            return false;
        m.PS = {g_PackDepthUpPS, sizeof(g_PackDepthUpPS)};
        if (!check(dev->CreateGraphicsPipelineState(&m, IID_PPV_ARGS(&p.psoDepthUp)), "depth upsample pipeline", &error))
            return false;
        p.shadowArgs = r.createBuffer(u64(Pipe::kShadowSegCap) * 4 * 28, D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        p.shadowCounts = r.createBuffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!p.shadowArgs || !p.shadowCounts) return error = "shadow cull buffers", false;
    }

    // ---- descriptor heaps ----
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = p.cpuCap = 4096;
        if (!check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.cpuHeap)), "cpu heap", &error)) return false;
        hd.NumDescriptors = Pipe::kGpuSlice * Renderer::kFrames;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.gpuHeap)), "gpu heap", &error)) return false;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        hd.NumDescriptors = 2048;
        if (!check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.samplerHeap)), "sampler heap", &error)) return false;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 128;
        if (!check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.rtvHeap)), "rtv heap", &error)) return false;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 4;
        if (!check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.dsvHeap)), "dsv heap", &error)) return false;
        p.srvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        p.samplerStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        p.rtvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        p.dsvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        // Null views first.
        auto cpu = [&](u32 i) { return D3D12_CPU_DESCRIPTOR_HANDLE{p.cpuHeap->GetCPUDescriptorHandleForHeapStart().ptr + size_t(i) * p.srvStride}; };
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        p.nullSrv = p.cpuCount++;
        dev->CreateShaderResourceView(nullptr, &sv, cpu(p.nullSrv));
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = 1;
        p.nullSrv3d = p.cpuCount++;
        dev->CreateShaderResourceView(nullptr, &sv, cpu(p.nullSrv3d));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        p.nullUav = p.cpuCount++;
        dev->CreateUnorderedAccessView(nullptr, nullptr, &uv, cpu(p.nullUav));
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uv.Texture3D.WSize = 1;
        p.nullUav3d = p.cpuCount++;
        dev->CreateUnorderedAccessView(nullptr, nullptr, &uv, cpu(p.nullUav3d));
        // Sampler kinds at the start of the sampler heap (per-stage tables are written after them).
        for (u32 k = 0; k < u32(Samp::Count); ++k) {
            D3D12_SAMPLER_DESC sd = samplerDesc(Samp(k));
            dev->CreateSampler(&sd, {p.samplerHeap->GetCPUDescriptorHandleForHeapStart().ptr + size_t(k) * p.samplerStride});
            p.samplerKind.push_back(Samp(k));
        }
        p.samplerCount = u32(Samp::Count);
        // Upload ring.
        p.upload = r.createBuffer(Pipe::kUploadSlice * Renderer::kFrames, D3D12_HEAP_TYPE_UPLOAD);
        if (!p.upload || FAILED(p.upload->Map(0, nullptr, reinterpret_cast<void**>(&p.uploadMapped))))
            return error = "upload buffer", false;
    }

    // ---- programs ----
    std::vector<const Program*> list;
    for (auto& g : def.gbuffers)
        if (g && g->ok()) list.push_back(&*g);
    auto addFamily = [&](const std::vector<Program>& fam) {
        for (const Program& prog : fam) {
            if (prog.enabled && prog.vs.present && prog.fs.present) {
                if (prog.ok()) list.push_back(&prog);
                else p.warnings.push_back(prog.name + " failed to translate: " + (prog.vs.error.empty() ? prog.fs.error : prog.vs.error).substr(0, 300));
            }
            if (auto it = def.computes.find(prog.name); it != def.computes.end())
                for (const Program& c : it->second) {
                    if (c.ok()) list.push_back(&c);
                    else if (c.enabled) p.warnings.push_back(c.name + " failed to translate: " + c.cs.error.substr(0, 300));
                }
        }
    };
    for (auto* fam : {&def.setup, &def.begin, &def.shadowcomp, &def.prepare, &def.deferred, &def.composite}) addFamily(*fam);
    if (def.final_ && def.final_->ok()) list.push_back(&*def.final_);
    // Programs the terrain uses (cube / model adapters); everything in gbuffers gets the generic adapter.
    std::set<const Program*> terrainUse;
    for (Gb g : {Gb::TerrainSolid, Gb::TerrainCutout, Gb::Water, Gb::DamagedBlock, Gb::Shadow, Gb::ShadowSolid,
                 Gb::ShadowCutout, Gb::ShadowWater})
        if (const Program* prog = def.resolve(g)) terrainUse.insert(prog);

    struct Job {
        u16 program;
        int kind;  // 0..3 VS adapter, 4 PS, 5 CS
    };
    std::vector<Job> jobs;
    p.programs.resize(list.size());
    for (size_t i = 0; i < list.size(); ++i) {
        const Program& prog = *list[i];
        ProgramBin& b = p.programs[i];
        b.src = &prog;
        b.name = prog.name;
        b.compute = prog.cs.present;
        b.final = prog.name == "final";
        b.shadow = prog.name.compare(0, 6, "shadow") == 0 && prog.name.compare(0, 10, "shadowcomp") != 0;
        b.gbuffers = prog.name.compare(0, 9, "gbuffers_") == 0 || b.shadow;
        b.depthMode = b.shadow ? 2 : b.gbuffers ? (prog.name == "gbuffers_hand" || prog.name == "gbuffers_hand_water" ? 1 : 0) : 3;
        p.programIndex[prog.name] = u16(i);
        if (b.compute) {
            jobs.push_back({u16(i), 5});
            continue;
        }
        // Draw buffers: the directive, else the locations the fragment shader writes.
        b.drawBuffers = prog.drawBuffers;
        int maxLoc = -1;
        for (const StageVar& v : prog.fs.hlsl.refl.outputs) maxLoc = std::max<int>(maxLoc, int(v.location + std::max(v.arraySize, 1u) - 1));
        if (b.drawBuffers.empty() && !b.final)
            for (int k = 0; k <= maxLoc && k < 8; ++k) b.drawBuffers.push_back(k);
        if (b.final) b.drawBuffers = {0};
        b.drawBuffers.resize(std::min<size_t>(b.drawBuffers.size(), 8));
        b.psWritten.assign(b.drawBuffers.size(), 0);
        for (const StageVar& v : prog.fs.hlsl.refl.outputs)
            for (u32 k = v.location; k < v.location + std::max(v.arraySize, 1u); ++k)
                if (k < b.psWritten.size()) b.psWritten[k] = 1;
        if (b.gbuffers) {
            jobs.push_back({u16(i), int(Adapter::Generic)});
            if (terrainUse.count(&prog)) {
                jobs.push_back({u16(i), int(Adapter::Cube)});
                jobs.push_back({u16(i), int(Adapter::Model)});
            }
        } else {
            jobs.push_back({u16(i), int(Adapter::Fullscreen)});
        }
        jobs.push_back({u16(i), 4});
    }

    // DXIL in parallel.
    const fs::path cache = cacheDirectory();
    std::atomic<size_t> next{0};
    std::mutex errMutex;
    std::string firstError;
    std::atomic<u32> cached{0}, compiled{0};
    Timer dxcTimer;
    auto work = [&] {
        for (size_t j; (j = next++) < jobs.size();) {
            const Job& job = jobs[j];
            ProgramBin& b = p.programs[job.program];
            const Program& prog = *b.src;
            StageBin* st;
            std::string src, entry, profile;
            if (job.kind == 5) {
                st = &b.cs;
                src = prog.cs.hlsl.hlsl;
                entry = "mcpp_cs", profile = "cs_6_0";
            } else if (job.kind == 4) {
                st = &b.ps;
                std::vector<std::string> rtTypes;
                for (int db : b.drawBuffers) {
                    TexFormat f = b.final ? TexFormat::RGBA8
                                          : b.shadow ? def.settings.shadow.colorFormat[std::clamp(db, 0, 1)]
                                                     : def.settings.targets[size_t(std::clamp(db, 0, 15))].format;
                    rtTypes.push_back(rtType(f));
                }
                src = prog.fs.hlsl.hlsl + "\n" + adapterSource(Adapter::Fullscreen) + "\n" +
                      pixelWrapper(prog.fs.hlsl.refl, b.depthMode, b.final, rtTypes);
                entry = "mcpp_ps", profile = "ps_6_0";
            } else {
                st = &b.vs[size_t(job.kind)];
                src = prog.vs.hlsl.hlsl + "\n" + adapterSource(Adapter(job.kind)) + "\n" +
                      vertexWrapper(prog.vs.hlsl.refl, prog.fs.hlsl.refl, b.depthMode, !b.final);
                entry = "mcpp_vs", profile = "vs_6_0";
            }
            DxilResult res;
            if (!compileHlsl(src, entry.c_str(), profile.c_str(), cache, res)) {
                std::lock_guard lock(errMutex);
                std::string msg = prog.name + " (" + profile + "): " + res.error.substr(0, 600);
                p.warnings.push_back(msg);
                if (firstError.empty()) firstError = msg;
                continue;
            }
            (res.cached ? cached : compiled)++;
            st->present = true;
            st->code = std::move(res.code);
        }
    };
    {
        unsigned n = std::max(1u, std::min(std::thread::hardware_concurrency(), unsigned(jobs.size())));
        std::vector<std::thread> threads;
        for (unsigned t = 1; t < n; ++t) threads.emplace_back(work);
        work();
        for (auto& t : threads) t.join();
    }
    LOG("shader pack %s: %zu DXIL stages (%u from cache) in %.0f ms", p.name.c_str(), jobs.size(), cached.load(),
        dxcTimer.ms());

    // Reflection, uniforms, resources, samplers per stage.
    std::set<std::string> unknownUniforms;
    auto samplerTable = [&](const std::vector<Samp>& kinds) -> u32 {
        if (kinds.empty()) return 0;
        const u32 first = p.samplerCount;
        if (first + kinds.size() > 2048) return 0;
        for (size_t k = 0; k < kinds.size(); ++k) {
            D3D12_SAMPLER_DESC sd = samplerDesc(kinds[k]);
            dev->CreateSampler(&sd, {p.samplerHeap->GetCPUDescriptorHandleForHeapStart().ptr + size_t(first + k) * p.samplerStride});
            p.samplerKind.push_back(kinds[k]);
        }
        p.samplerCount += u32(kinds.size());
        return first;
    };
    auto setupStage = [&](StageBin& st, const StageReflection& refl, const Program& prog, bool gbuffers) {
        if (!st.present) return;
        st.refl = refl;
        std::vector<std::string> unknown;
        resolveUniforms(refl, p.uniforms, st.uniforms, &unknown);
        for (auto& u : unknown) unknownUniforms.insert(u);
        std::vector<Samp> kinds;
        std::vector<Samp> drawKinds = {gbuffers ? Samp::AtlasWrap : Samp::LinearClamp, Samp::AtlasWrap, Samp::AtlasWrap};
        for (const TextureBinding& t : refl.textures) {
            if (t.space == 2) {
                st.usesDrawTextures = true;
                continue;
            }
            std::string warn;
            ResRef ref = resolveTexture(def, prog.name, t.name, refl.textures, &warn);
            if (!warn.empty()) p.warnings.push_back(warn);
            if (t.reg >= st.textures.size()) st.textures.resize(t.reg + 1);
            st.textures[t.reg] = ref;
            if (t.reg >= kinds.size()) kinds.resize(t.reg + 1, Samp::PointClamp);
            kinds[t.reg] = samplerFor(t, ref, def, &prog);
        }
        if (st.textures.size() > kMaxStageTextures) p.warnings.push_back(prog.name + ": too many textures");
        st.samplerTable = samplerTable(kinds);
        st.drawSamplerTable = samplerTable(drawKinds);
        for (const ImageBinding& im : refl.images) {
            if (im.reg >= st.images.size()) st.images.resize(im.reg + 1);
            st.images[im.reg] = resolveImage(def, im.name);
            if (st.images[im.reg].kind == Res::None) p.warnings.push_back(prog.name + ": unknown image " + im.name);
        }
    };
    for (ProgramBin& b : p.programs) {
        const Program& prog = *b.src;
        if (b.compute) {
            setupStage(b.cs, prog.cs.hlsl.refl, prog, false);
            continue;
        }
        for (auto& v : b.vs) setupStage(v, prog.vs.hlsl.refl, prog, b.gbuffers);
        setupStage(b.ps, prog.fs.hlsl.refl, prog, b.gbuffers);
    }
    // Custom uniforms.
    for (const CustomUniform& u : def.uniforms) p.custom_.add(u.type, u.name, u.expr, u.isVariable);
    for (const std::string& e : p.custom_.errors()) p.warnings.push_back("custom uniform: " + e);

    // Id maps.
    loadIdMap(def.source, "block.properties", def.macros, "block.", p.blockIds);
    loadIdMap(def.source, "item.properties", def.macros, "item.", p.itemIds);
    loadIdMap(def.source, "entity.properties", def.macros, "entity.", p.entityIds);
    for (const IdMapEntry& e : p.itemIds) p.itemIdByName.emplace(e.name, e.id);
    for (const IdMapEntry& e : p.entityIds) p.entityIdByName.emplace(e.name, e.id);

    if (!firstError.empty()) return error = "DXC: " + firstError, false;  // like Iris: the pack does not load
    for (const std::string& w : p.warnings) LOG("shader pack %s: warning: %s", p.name.c_str(), w.c_str());
    out = std::move(pipe);
    return true;
}
