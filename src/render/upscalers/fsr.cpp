// AMD FidelityFX / FSR SDK 2.3 backend (FSR API, amd_fidelityfx_loader_dx12.dll + effect DLLs):
//   FSR Upscaling: the provider is picked by the loader for this GPU (FSR 4.x ML on Radeon RX 7000/9000, FSR 3.1.x
//   analytical everywhere else, e.g. on NVIDIA). Inputs: HDR colour, reversed-Z infinite depth, UV motion vectors
//   (scaled to pixels), the pipeline's exposure texture and our R8 reactive mask.
//   FSR Frame Generation: frame interpolation swap chain (FSR swap chain 3.1.7) + frame generation context; the UI
//   layer is registered with the swap chain (premultiplied, double-buffered) and blended over real and generated
//   frames, so the back buffer we present is the hudless image. Dispatch through the frame generation callback.
//   Anti-Lag 2: AMD driver extension (amdxc64.dll) - only on AMD GPUs.
#include "render/upscalers/upscalers.h"

#ifndef _WINDOWS
#define _WINDOWS  // ffx_api_loader.h keys its Windows path on this
#endif
#include <api/include/dx12/ffx_api_dx12.h>
#include <api/include/ffx_api.h>
#include <api/include/ffx_api_loader.h>
#include <framegeneration/include/dx12/ffx_api_framegeneration_dx12.h>
#include <framegeneration/include/ffx_framegeneration.h>
#include <upscalers/include/ffx_upscale.h>
#include <antilag2/ffx_antilag2_dx12.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace upscale {
namespace {

ffxFunctions g_ffx{};

void ffxLog(uint32_t type, const wchar_t* message) {
    char buf[1024];
    WideCharToMultiByte(CP_UTF8, 0, message, -1, buf, sizeof(buf), nullptr, nullptr);
    LOG("fsr %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", buf);
}

template <typename T>
T desc(ffxStructType_t type) {
    T d{};
    d.header.type = type;
    return d;
}

FfxApiResource resource(ID3D12Resource* r, uint32_t state) {
    FfxApiResource res = ffxApiGetResourceDX12(r, state);
    if (r && r->GetDesc().Format == DXGI_FORMAT_R32_TYPELESS) res.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT;
    return res;
}

float jitterSign() {
    static const float s = [] {
        const char* v = std::getenv("MCPP_FSR_JITTER_SIGN");
        return v ? float(std::atof(v)) : -1.0f;
    }();
    return s;
}

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list->ResourceBarrier(1, &b);
}

class Fsr final : public Backend, public ::Upscaler {
public:
    explicit Fsr(Upscalers& hub) : Backend(hub) {}
    ~Fsr() override { destroyUpscaler(); }

    void initDevice(IDXGIAdapter1*, Caps& caps) override {
        auto fail = [&](const std::string& why) { caps.why[int(UpscalerKind::Fsr)] = why; };
        if (hub.vendorId() == 0x1002) {  // Anti-Lag 2 is an AMD driver extension
            if (AMD::AntiLag2DX12::Initialize(&antiLag_, hub.device()) == S_OK) {
                antiLagOk_ = true;
                caps.lowLatency[int(LowLatencyMode::On)] = true;
                caps.lowLatencyName = "AMD Anti-Lag 2";
            }
        }
        std::wstring path;
        if (!exeDirFile(L"amd_fidelityfx_loader_dx12.dll", path))
            return fail("amd_fidelityfx_loader_dx12.dll missing (tools/fetch_sdks.ps1)");
        if (!verifySignature(path)) return fail("amd_fidelityfx_loader_dx12.dll signature check failed");
        module_ = LoadLibraryW(path.c_str());
        if (!module_) return fail("amd_fidelityfx_loader_dx12.dll failed to load");
        ffxLoadFunctions(&g_ffx, module_);
        if (!g_ffx.CreateContext || !g_ffx.Query || !g_ffx.Dispatch) return fail("FSR API exports missing");

        auto versions = [&](uint64_t type) {
            auto q = desc<ffxQueryDescGetVersions>(FFX_API_QUERY_DESC_TYPE_GET_VERSIONS);
            q.createDescType = type;
            q.device = hub.device();
            uint64_t count = 0;
            q.outputCount = &count;
            g_ffx.Query(nullptr, &q.header);
            std::vector<const char*> names(count);
            std::vector<uint64_t> ids(count);
            q.versionNames = names.data(), q.versionIds = ids.data();
            if (count) g_ffx.Query(nullptr, &q.header);
            std::string s;
            for (const char* n : names) s += std::string(s.empty() ? "" : ", ") + (n ? n : "?");
            return std::make_pair(count, s);
        };
        const auto up = versions(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE);
        const auto fg = versions(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
        LOG("fsr: upscaler providers: %s | frame generation providers: %s", up.second.c_str(), fg.second.c_str());
        if (up.first) {
            caps.upscaler[int(UpscalerKind::Fsr)] = true;
            caps.version[int(UpscalerKind::Fsr)] = "SDK 2.3.0";
        } else {
            fail("no FSR upscaler provider for this GPU");
        }
        if (fg.first && exeDirFile(L"amd_fidelityfx_framegeneration_dx12.dll", path)) {
            caps.frameGen[int(FrameGenKind::Fsr)] = true;
            caps.maxMultiplier[int(FrameGenKind::Fsr)] = 2;
            fgOk_ = true;
        }
    }

    void shutdown() override {
        destroyUpscaler();
        destroyFrameGen();
        if (antiLagOk_) AMD::AntiLag2DX12::DeInitialize(&antiLag_);
        antiLagOk_ = false;
    }

    // ---- upscaler ----
    ::Upscaler* upscaler() override { return g_ffx.CreateContext ? this : nullptr; }
    const char* name() const override { return "FSR"; }
    float scale(UpscaleQuality q) const override {
        auto be = desc<ffxCreateBackendDX12Desc>(FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12);
        be.device = hub.device();
        auto d = desc<ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode>(FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE);
        d.header.pNext = &be.header;
        d.qualityMode = mode(q);
        float ratio = 1.0f;
        d.pOutUpscaleRatio = &ratio;
        if (!g_ffx.Query || g_ffx.Query(nullptr, &d.header) != FFX_API_RETURN_OK || ratio < 1.0f) return 1.0f;
        return std::clamp(1.0f / ratio, 0.33f, 1.0f);
    }
    void setQuality(UpscaleQuality q) override { quality_ = q; }
    float mipBiasOffset() const override { return -1.0f; }  // FSR guidance: log2(render / display) - 1

    void evaluate(const UpscaleInputs& in) override {
        if (!ensureUpscaler(in.outputSize[0], in.outputSize[1])) return;
        ID3D12Resource* reactive = hub.reactiveMask(in, 0.5f);
        const auto depthState = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        barrier(in.list, in.depth, depthState, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        auto d = desc<ffxDispatchDescUpscale>(FFX_API_DISPATCH_DESC_TYPE_UPSCALE);
        d.commandList = in.list;
        d.color = resource(in.color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        d.depth = resource(in.depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        d.motionVectors = resource(in.motion, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        d.exposure = resource(in.exposure, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        d.reactive = resource(reactive, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        d.output = resource(in.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {jitterSign() * in.jitter[0], jitterSign() * in.jitter[1]};
        d.motionVectorScale = {float(in.renderSize[0]), float(in.renderSize[1])};  // UV units -> pixels
        d.renderSize = {in.renderSize[0], in.renderSize[1]};
        d.upscaleSize = {in.outputSize[0], in.outputSize[1]};
        d.enableSharpening = false;  // the pipeline sharpens after tonemapping (Video Settings: Sharpness)
        d.sharpness = 0;
        d.frameTimeDelta = in.frameTimeMs;
        d.preExposure = 1.0f;
        d.reset = in.reset;
        d.cameraNear = in.nearZ;
        d.cameraFar = 100000.0f;  // infinite projection (FFX_UPSCALE_ENABLE_DEPTH_INFINITE)
        d.cameraFovAngleVertical = in.fovY;
        d.viewSpaceToMetersFactor = 1.0f;  // one block = one metre
        if (g_ffx.Dispatch(&upscaler_, &d.header) != FFX_API_RETURN_OK && warned_++ < 5) LOG("fsr: upscale dispatch failed");
        barrier(in.list, in.depth, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, depthState);
    }

    // ---- frame generation (FSR FG swap chain) ----
    bool createSwapChain(HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& sd, bool frameGen, ComPtr<IDXGISwapChain3>& out) override {
        if (!frameGen || !fgOk_) return false;
        std::lock_guard lock(mutex_);
        DXGI_SWAP_CHAIN_DESC1 d1 = sd;
        IDXGISwapChain4* sc = nullptr;
        auto ver = desc<ffxCreateContextDescFrameGenerationSwapChainVersionDX12>(
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12);
        ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
        auto c = desc<ffxCreateContextDescFrameGenerationSwapChainForHwndDX12>(
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12);
        c.header.pNext = &ver.header;
        c.swapchain = &sc;
        c.hwnd = hwnd;
        c.desc = &d1;
        c.fullscreenDesc = nullptr;
        c.dxgiFactory = hub.factory();
        c.gameQueue = hub.nativeQueue();
        if (g_ffx.CreateContext(&swapCtx_, &c.header, nullptr) != FFX_API_RETURN_OK || !sc) {
            LOG("fsr: frame generation swap chain creation failed");
            swapCtx_ = nullptr;
            return false;
        }
        swap_ = rawSwap_ = sc;
        out.Attach(sc);  // the context's reference is ours; released by Renderer before releaseSwapChain

        auto be = desc<ffxCreateBackendDX12Desc>(FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12);
        be.device = hub.device();
        auto fver = desc<ffxCreateContextDescFrameGenerationVersion>(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION);
        fver.version = FFX_FRAMEGENERATION_VERSION;
        auto fg = desc<ffxCreateContextDescFrameGeneration>(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
        fg.header.pNext = &fver.header;
        fver.header.pNext = &be.header;
        fg.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
        fg.displaySize = {sd.Width, sd.Height};
        fg.maxRenderSize = {sd.Width, sd.Height};
        fg.backBufferFormat = ffxApiGetSurfaceFormatDX12(sd.Format);
        if (g_ffx.CreateContext(&fgCtx_, &fg.header, nullptr) != FFX_API_RETURN_OK) {
            LOG("fsr: frame generation context creation failed");
            fgCtx_ = nullptr;
            return true;  // the swap chain still works (pass-through)
        }
        auto v = desc<ffxQueryGetProviderVersion>(FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION);
        if (g_ffx.Query(&fgCtx_, &v.header) == FFX_API_RETURN_OK && v.versionName) fgVersion_ = v.versionName;
        LOG("fsr: frame generation %s on its frame interpolation swap chain", fgVersion_.c_str());
        frameId_ = 0;
        return true;
    }
    void beforeReleaseSwapChain() override { destroyFrameGen(); }
    void releaseSwapChain() override {
        // The app's references are gone. Streamline's proxy (wrapped on top for presentCommon) leaves one reference
        // on the proxy swap chain behind; drop it so the real swap chain is destroyed and the window is free again.
        if (!rawSwap_) return;
        rawSwap_->AddRef();
        ULONG left = rawSwap_->Release();
        if (left) LOG("fsr: releasing %lu leftover swap chain reference(s)", left);
        while (left) left = rawSwap_->Release();
        rawSwap_ = nullptr;
    }

    void setFrameGen(bool on, u32) override { fgOn_ = on; }
    bool composesUi() const override { return swapCtx_ != nullptr; }

    void tagFrame(const FrameGenInputs& in) override {
        if (!swapCtx_ || !fgCtx_) return;
        std::lock_guard lock(mutex_);
        const CameraFrame& c = *in.cam;
        // The UI layer, blended by the swap chain over every presented frame (it keeps its own copy).
        auto ui = desc<ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12>(
            FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12);
        ui.uiResource = resource(in.ui, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        ui.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA |
                   FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
        g_ffx.Configure(&swapCtx_, &ui.header);

        ++frameId_;  // must advance by exactly one per frame
        auto cfg = desc<ffxConfigureDescFrameGeneration>(FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION);
        cfg.swapChain = swap_;
        cfg.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* params, void* ctx) -> ffxReturnCode_t {
            return g_ffx.Dispatch(static_cast<ffxContext*>(ctx), &params->header);
        };
        cfg.frameGenerationCallbackUserContext = &fgCtx_;
        cfg.frameGenerationEnabled = fgOn_;
        cfg.allowAsyncWorkloads = false;
        cfg.generationRect = {0, 0, int32_t(c.outputSize[0]), int32_t(c.outputSize[1])};
        cfg.frameID = frameId_;
        if (g_ffx.Configure(&fgCtx_, &cfg.header) != FFX_API_RETURN_OK && warned_++ < 5) LOG("fsr: frame generation configure failed");

        barrier(in.list, in.depth, in.depthState, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        auto p = desc<ffxDispatchDescFrameGenerationPrepareV2>(FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2);
        p.frameID = frameId_;
        p.commandList = in.list;
        p.renderSize = {c.renderSize[0], c.renderSize[1]};
        p.jitterOffset = {jitterSign() * c.jitterPx[0], jitterSign() * c.jitterPx[1]};
        p.motionVectorScale = {float(c.renderSize[0]), float(c.renderSize[1])};
        p.frameTimeDelta = c.frameTimeMs;
        p.reset = c.reset;
        p.cameraNear = c.nearZ;
        p.cameraFar = c.farZ;
        p.cameraFovAngleVertical = c.fovY;
        p.viewSpaceToMetersFactor = 1.0f;
        p.depth = resource(in.depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        p.motionVectors = resource(in.motion, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        for (int k = 0; k < 3; ++k) {
            p.cameraPosition[k] = float(c.pos[k]);
            p.cameraUp[k] = c.up[k], p.cameraRight[k] = c.right[k], p.cameraForward[k] = c.fwd[k];
        }
        if (g_ffx.Dispatch(&fgCtx_, &p.header) != FFX_API_RETURN_OK && warned_++ < 5) LOG("fsr: frame generation prepare failed");
        barrier(in.list, in.depth, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, in.depthState);
    }
    void afterPresent(u32, u32& framesPresented) override { framesPresented = fgOn_ && fgCtx_ ? 2 : 1; }

    // ---- Anti-Lag 2 ----
    bool ownsLatency() const override { return antiLagOk_ && antiLagOn_; }
    void setLatency(LowLatencyMode m, u32 fpsLimit) override {
        antiLagOn_ = m != LowLatencyMode::Off;
        fpsLimit_ = fpsLimit;
    }
    bool limitsFrameRate() const override { return antiLagOk_ && antiLagOn_ && fpsLimit_; }
    void sleep(u32) override {
        if (antiLagOk_) AMD::AntiLag2DX12::Update(&antiLag_, antiLagOn_, antiLagOn_ ? fpsLimit_ : 0);
    }
    void marker(Marker m, u32) override {
        if (antiLagOk_ && antiLagOn_ && m == SubmitEnd) AMD::AntiLag2DX12::MarkEndOfFrameRendering(&antiLag_);
    }
    std::string stats() const override {
        std::string s;
        if (antiLagOk_) s = std::string("AMD Anti-Lag 2 ") + (antiLagOn_ ? "On" : "Off");
        if (!upVersion_.empty()) s += (s.empty() ? "" : ", ") + std::string("FSR upscaler ") + upVersion_;
        if (!fgVersion_.empty()) s += (s.empty() ? "" : ", ") + std::string("FSR frame generation ") + fgVersion_;
        return s;
    }

private:
    static uint32_t mode(UpscaleQuality q) {
        switch (q) {
            case UpscaleQuality::Quality: return FFX_UPSCALE_QUALITY_MODE_QUALITY;
            case UpscaleQuality::Balanced: return FFX_UPSCALE_QUALITY_MODE_BALANCED;
            case UpscaleQuality::Performance: return FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
            case UpscaleQuality::UltraPerformance: return FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
            default: return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
        }
    }
    bool ensureUpscaler(u32 w, u32 h) {
        if (upscaler_ && upW_ == w && upH_ == h) return true;
        if (upscaler_) {  // output size changed: nothing in flight uses the old context (Renderer::resize waited)
            hub.waitIdle();
            destroyUpscaler();
        }
        auto be = desc<ffxCreateBackendDX12Desc>(FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12);
        be.device = hub.device();
        auto ver = desc<ffxCreateContextDescUpscaleVersion>(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION);
        ver.version = FFX_UPSCALER_VERSION;
        auto c = desc<ffxCreateContextDescUpscale>(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE);
        c.header.pNext = &ver.header;
        ver.header.pNext = &be.header;
        c.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
        if (std::getenv("MCPP_FSR_DEBUG")) c.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
        c.maxRenderSize = {w, h};
        c.maxUpscaleSize = {w, h};
        c.fpMessage = ffxLog;
        if (g_ffx.CreateContext(&upscaler_, &c.header, nullptr) != FFX_API_RETURN_OK) {
            if (warned_++ < 5) LOG("fsr: upscaler context creation failed");
            upscaler_ = nullptr;
            return false;
        }
        upW_ = w, upH_ = h;
        auto v = desc<ffxQueryGetProviderVersion>(FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION);
        if (g_ffx.Query(&upscaler_, &v.header) == FFX_API_RETURN_OK && v.versionName && upVersion_ != v.versionName) {
            upVersion_ = v.versionName;
            LOG("fsr: upscaler %s at %ux%u", upVersion_.c_str(), w, h);
        }
        return true;
    }
    void destroyUpscaler() {
        if (upscaler_ && g_ffx.DestroyContext) g_ffx.DestroyContext(&upscaler_, nullptr);
        upscaler_ = nullptr;
    }
    void destroyFrameGen() {
        std::lock_guard lock(mutex_);
        if (fgCtx_) {  // disable first: waits for the swap chain's interpolation / UI work (SDK shutdown order)
            auto cfg = desc<ffxConfigureDescFrameGeneration>(FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION);
            cfg.swapChain = swap_;
            cfg.frameGenerationEnabled = false;
            g_ffx.Configure(&fgCtx_, &cfg.header);
            g_ffx.DestroyContext(&fgCtx_, nullptr);
            fgCtx_ = nullptr;
        }
        if (swapCtx_) {
            g_ffx.DestroyContext(&swapCtx_, nullptr);
            swapCtx_ = nullptr;
        }
        swap_ = nullptr;
    }

    HMODULE module_ = nullptr;
    ffxContext upscaler_ = nullptr, fgCtx_ = nullptr, swapCtx_ = nullptr;
    IDXGISwapChain4* swap_ = nullptr;
    IDXGISwapChain4* rawSwap_ = nullptr;  // not owned
    std::mutex mutex_;
    u32 upW_ = 0, upH_ = 0;
    u64 frameId_ = 0;
    bool fgOk_ = false, fgOn_ = false;
    UpscaleQuality quality_ = UpscaleQuality::Native;
    std::string upVersion_, fgVersion_;
    int warned_ = 0;
    AMD::AntiLag2DX12::Context antiLag_;
    bool antiLagOk_ = false, antiLagOn_ = false;
    u32 fpsLimit_ = 0;
};

}  // namespace

std::unique_ptr<Backend> makeFsr(Upscalers& hub) { return std::make_unique<Fsr>(hub); }

}  // namespace upscale
