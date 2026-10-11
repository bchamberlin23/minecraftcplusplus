// Intel XeSS SDK 3.0.2 backend (libxess.dll, libxess_fg.dll, libxell.dll; import libraries delay-loaded):
//   XeSS Super Resolution 3: Intel-optimised path on Arc / Xe, the cross-vendor SM 6.4 (DP4a) path elsewhere.
//   XeSS Frame Generation: proxy swap chain; on non-Intel GPUs one generated frame per rendered frame (2x) and
//   pacing on an internal high-priority queue. UI: hudless + UI texture composition (our UI layer satisfies
//   Final = UI.rgb + (1 - UI.a) * Hudless.rgb).
//   Xe Low Latency (XeLL): required by XeSS-FG; on non-Intel GPUs it only works together with XeSS-FG, so it is
//   active exactly while XeSS-FG owns the swap chain.
#include "render/upscalers/upscalers.h"

#if defined(MCPP_HAVE_XESS)
#include <xell/xell.h>
#include <xell/xell_d3d12.h>
#include <xess/xess.h>
#include <xess/xess_d3d12.h>
#include <xess_fg/xefg_swapchain.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#endif

#include <algorithm>
#include <cstring>

namespace upscale {

#if defined(MCPP_HAVE_XESS)
namespace {

void xessLog(const char* message, xess_logging_level_t level) {
    if (level >= XESS_LOGGING_LEVEL_WARNING) LOG("xess %s: %s", level == XESS_LOGGING_LEVEL_ERROR ? "error" : "warning", message);
}
void xellLog(const char* message, xell_logging_level_t level) {
    if (level >= XELL_LOGGING_LEVEL_WARNING) LOG("xell %s: %s", level == XELL_LOGGING_LEVEL_ERROR ? "error" : "warning", message);
}
void xefgLog(const char* message, xefg_swapchain_logging_level_t level, void*) {
    if (level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING)
        LOG("xess-fg %s: %s", level == XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR ? "error" : "warning", message);
}

float jitterSign() {
    static const float s = [] {
        const char* v = std::getenv("MCPP_XESS_JITTER_SIGN");
        return v ? float(std::atof(v)) : -1.0f;
    }();
    return s;
}

class Xess final : public Backend, public ::Upscaler {
public:
    explicit Xess(Upscalers& hub) : Backend(hub) {}

    void initDevice(IDXGIAdapter1*, Caps& caps) override {
        auto fail = [&](const std::string& why) { caps.why[int(UpscalerKind::Xess)] = why; };
        std::wstring path;
        if (!exeDirFile(L"libxess.dll", path)) return fail("libxess.dll missing (tools/fetch_sdks.ps1)");
        if (!verifySignature(path)) return fail("libxess.dll signature check failed");
        if (!LoadLibraryW(path.c_str())) return fail("libxess.dll failed to load");  // delay-load binds to it
        xess_version_t v{};
        xessGetVersion(&v);
        char ver[64];
        std::snprintf(ver, sizeof(ver), "XeSS-SR %u.%u.%u", v.major, v.minor, v.patch);
        if (xessD3D12CreateContext(hub.device(), &ctx_) != XESS_RESULT_SUCCESS || !ctx_)
            return fail("XeSS-SR not supported on this GPU (needs SM 6.4 / DP4a)");
        xessSetLoggingCallback(ctx_, XESS_LOGGING_LEVEL_WARNING, xessLog);
        if (xessIsOptimalDriver(ctx_) == XESS_RESULT_WARNING_OLD_DRIVER) LOG("xess: a newer GPU driver is recommended");
        caps.upscaler[int(UpscalerKind::Xess)] = true;
        caps.version[int(UpscalerKind::Xess)] = ver;
        // Pre-build the shaders in the background (initialisation later waits for them).
        xessD3D12BuildPipelines(ctx_, nullptr, false, initFlags());

        // XeSS-FG + XeLL.
        std::wstring fgPath, llPath;
        if (!exeDirFile(L"libxess_fg.dll", fgPath) || !exeDirFile(L"libxell.dll", llPath)) return;
        if (!verifySignature(fgPath) || !verifySignature(llPath)) {
            LOG("xess: frame generation DLL signature check failed");
            return;
        }
        if (!LoadLibraryW(llPath.c_str()) || !LoadLibraryW(fgPath.c_str())) return;
        if (xellD3D12CreateContext(hub.device(), &xell_) != XELL_RESULT_SUCCESS || !xell_) {
            LOG("xess: XeLL not available on this GPU / driver");
            xell_ = nullptr;
            return;
        }
        xellSetLoggingCallback(xell_, XELL_LOGGING_LEVEL_WARNING, xellLog);
        if (!createFgContext()) return;
        xefg_swapchain_properties_t props{};
        xefgSwapChainGetProperties(fg_, &props);
        xefg_swapchain_version_t fv{};
        xefgSwapChainGetVersion(&fv);
        xell_version_t lv{};
        xellGetVersion(&lv);
        LOG("xess: %s, XeSS-FG %u.%u.%u (up to %u generated frames), XeLL %u.%u.%u", ver, fv.major, fv.minor, fv.patch,
            props.maxSupportedInterpolations, lv.major, lv.minor, lv.patch);
        caps.frameGen[int(FrameGenKind::Xess)] = true;
        caps.maxMultiplier[int(FrameGenKind::Xess)] = 1 + std::max(1u, props.maxSupportedInterpolations);
        if (!caps.lowLatency[1]) caps.lowLatencyName = "Intel XeLL (with XeSS-FG)";
    }

    void shutdown() override {
        destroyFg();
        if (xell_) xellDestroyContext(xell_);
        xell_ = nullptr;
        if (ctx_) xessDestroyContext(ctx_);
        ctx_ = nullptr;
    }

    // ---- XeSS-SR ----
    ::Upscaler* upscaler() override { return ctx_ ? this : nullptr; }
    const char* name() const override { return "XeSS"; }
    float scale(UpscaleQuality q) const override {
        if (!ctx_) return 1.0f;
        xess_2d_t out{std::max(hub.r.width(), 1u), std::max(hub.r.height(), 1u)}, opt{}, mn{}, mx{};
        if (xessGetOptimalInputResolution(ctx_, &out, setting(q), &opt, &mn, &mx) != XESS_RESULT_SUCCESS || !opt.x) return 1.0f;
        return std::clamp(float(opt.x) / float(out.x), 0.33f, 1.0f);
    }
    void setQuality(UpscaleQuality q) override { quality_ = q; }
    float mipBiasOffset() const override { return 0.0f; }  // XeSS guidance: log2(render / output)

    void evaluate(const UpscaleInputs& in) override {
        if (!ctx_) return;
        if (!inited_ || initW_ != in.outputSize[0] || initH_ != in.outputSize[1] || initQuality_ != quality_) {
            hub.waitIdle();  // re-initialisation: no XeSS work may be pending
            xess_d3d12_init_params_t p{};
            p.outputResolution = {in.outputSize[0], in.outputSize[1]};
            p.qualitySetting = setting(quality_);
            p.initFlags = initFlags();
            if (xessD3D12Init(ctx_, &p) != XESS_RESULT_SUCCESS) {
                if (warned_++ < 5) LOG("xess: xessD3D12Init failed");
                return;
            }
            xessSetJitterScale(ctx_, jitterSign(), jitterSign());
            inited_ = true, initW_ = in.outputSize[0], initH_ = in.outputSize[1], initQuality_ = quality_;
            LOG("xess: initialised for %ux%u", initW_, initH_);
        }
        xessSetVelocityScale(ctx_, float(in.renderSize[0]), float(in.renderSize[1]));  // UV units -> pixels
        ID3D12Resource* reactive = hub.reactiveMask(in, 0.6f);
        xess_d3d12_execute_params_t e{};
        e.pColorTexture = in.color;
        e.pVelocityTexture = in.motion;
        e.pDepthTexture = in.depth;
        e.pExposureScaleTexture = (initFlags() & XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE) ? in.exposure : nullptr;
        e.pResponsivePixelMaskTexture = reactive;
        e.pOutputTexture = in.output;
        e.jitterOffsetX = in.jitter[0];
        e.jitterOffsetY = in.jitter[1];
        e.exposureScale = 1.0f;
        e.resetHistory = in.reset ? 1 : 0;
        e.inputWidth = in.renderSize[0];
        e.inputHeight = in.renderSize[1];
        if (xessD3D12Execute(ctx_, in.list, &e) != XESS_RESULT_SUCCESS && warned_++ < 5) LOG("xess: execute failed");
    }

    // ---- XeSS-FG ----
    bool createSwapChain(HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& sd, bool frameGen, ComPtr<IDXGISwapChain3>& out) override {
        if (!frameGen || !fg_) return false;
        xefg_swapchain_d3d12_init_params_t p{};
        p.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;
        p.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
        p.uiMode = XEFG_SWAPCHAIN_UI_MODE_HUDLESS_UITEXTURE;
        ComPtr<IDXGIFactory2> f2;
        hub.factory()->QueryInterface(IID_PPV_ARGS(&f2));
        const xefg_swapchain_result_t r =
            xefgSwapChainD3D12InitFromSwapChainDesc(fg_, hwnd, &sd, nullptr, hub.nativeQueue(), f2.Get(), &p);
        if (r < 0) {
            LOG("xess-fg: swap chain initialisation failed (%d)", int(r));
            return false;
        }
        ComPtr<IDXGISwapChain4> sc;
        if (xefgSwapChainD3D12GetSwapChainPtr(fg_, IID_PPV_ARGS(&sc)) < 0 || FAILED(sc.As(&out))) return false;
        xefgSwapChainSetUiCompositionState(fg_, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);
        swapChainLive_ = true;
        return true;
    }
    void releaseSwapChain() override {
        if (!swapChainLive_) return;
        destroyFg();        // releases the native swap chain (all proxy references are gone by now)
        createFgContext();  // ready for the next time it is selected
    }
    void setFrameGen(bool on, u32 multiplier) override {
        if (!fg_ || !swapChainLive_) return;
        if (on) {  // XeLL must be enabled before frame generation
            applySleepMode(true);
            xefgSwapChainSetNumInterpolatedFrames(fg_, std::max(1u, multiplier - 1));
        }
        xefgSwapChainSetEnabled(fg_, on ? 1 : 0);
        fgOn_ = on;
    }
    void tagFrame(const FrameGenInputs& in) override {
        if (!fg_ || !fgOn_) return;
        const CameraFrame& c = *in.cam;
        auto tag = [&](xefg_swapchain_resource_type_t type, ID3D12Resource* res, D3D12_RESOURCE_STATES state, u32 w, u32 h) {
            xefg_swapchain_d3d12_resource_data_t d{};
            d.type = type;
            d.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
            d.resourceBase = {0, 0};
            d.resourceSize = {w, h};
            d.pResource = res;
            d.incomingState = state;
            if (xefgSwapChainD3D12TagFrameResource(fg_, in.list, in.frameId, &d) < 0 && warned_++ < 5)
                LOG("xess-fg: tagging resource %d failed", int(type));
        };
        tag(XEFG_SWAPCHAIN_RES_DEPTH, in.depth, in.depthState, c.renderSize[0], c.renderSize[1]);
        tag(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, in.motion, in.motionState, c.renderSize[0], c.renderSize[1]);
        tag(XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, in.hudless, in.hudlessState, c.outputSize[0], c.outputSize[1]);
        tag(XEFG_SWAPCHAIN_RES_UI, in.ui, in.uiState, c.outputSize[0], c.outputSize[1]);
        xefg_swapchain_frame_constant_data_t k{};
        // World -> view with the camera translation (row-vector, row-major = Mat4 memory), projection without jitter.
        Mat4 view = c.view;
        for (int r = 0; r < 3; ++r)
            view(r, 3) = -float(double(c.view(r, 0)) * c.pos[0] + double(c.view(r, 1)) * c.pos[1] + double(c.view(r, 2)) * c.pos[2]);
        std::memcpy(k.viewMatrix, view.m, 64);
        std::memcpy(k.projectionMatrix, c.proj.m, 64);
        k.jitterOffsetX = jitterSign() * c.jitterPx[0];
        k.jitterOffsetY = jitterSign() * c.jitterPx[1];
        k.motionVectorScaleX = float(c.renderSize[0]);  // UV units -> pixels
        k.motionVectorScaleY = float(c.renderSize[1]);
        k.resetHistory = c.reset ? 1 : 0;
        k.frameRenderTime = c.frameTimeMs;
        xefgSwapChainTagFrameConstants(fg_, in.frameId, &k);
    }
    void beforePresent(u32 frameId) override {
        if (fg_ && swapChainLive_) xefgSwapChainSetPresentId(fg_, frameId);
    }
    void afterPresent(u32, u32& framesPresented) override {
        framesPresented = 1;
        if (!fg_ || !swapChainLive_) return;
        xefg_swapchain_present_status_t st{};
        if (xefgSwapChainGetLastPresentStatus(fg_, &st) >= 0) framesPresented = std::max(1u, st.framesPresented);
    }

    // ---- XeLL (only meaningful with XeSS-FG on non-Intel GPUs) ----
    bool ownsLatency() const override { return xell_ && swapChainLive_; }
    void setLatency(LowLatencyMode, u32 fpsLimit) override {
        fpsLimit_ = fpsLimit;
        if (swapChainLive_) applySleepMode(true);
    }
    bool limitsFrameRate() const override { return ownsLatency() && fpsLimit_; }
    void sleep(u32 frameId) override {
        if (ownsLatency()) xellSleep(xell_, frameId);
    }
    void marker(Marker m, u32 frameId) override {
        if (!ownsLatency()) return;
        static const xell_latency_marker_type_t map[] = {XELL_SIMULATION_START, XELL_SIMULATION_END, XELL_RENDERSUBMIT_START,
                                                         XELL_RENDERSUBMIT_END, XELL_PRESENT_START, XELL_PRESENT_END};
        xellAddMarkerData(xell_, frameId, map[m]);
    }
    std::string stats() const override {
        if (!ownsLatency()) return {};
        std::string s = "Intel XeLL On";
        xell_frame_report_t reports[64]{};
        if (xellGetFramesReports(xell_, reports) == XELL_RESULT_SUCCESS) {
            double sum = 0;
            int n = 0;
            for (const auto& r : reports)
                if (r.m_sim_start_ts && r.m_present_end_ts > r.m_sim_start_ts) sum += double(r.m_present_end_ts - r.m_sim_start_ts), ++n;
            if (n) {
                char b[96];
                std::snprintf(b, sizeof(b), " (sim start -> present %.1f ms, %d frames)", sum / n / 1e6, n);
                s += b;
            }
        }
        return s;
    }

private:
    static xess_quality_settings_t setting(UpscaleQuality q) {
        switch (q) {
            case UpscaleQuality::UltraQualityPlus: return XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS;
            case UpscaleQuality::UltraQuality: return XESS_QUALITY_SETTING_ULTRA_QUALITY;
            case UpscaleQuality::Quality: return XESS_QUALITY_SETTING_QUALITY;
            case UpscaleQuality::Balanced: return XESS_QUALITY_SETTING_BALANCED;
            case UpscaleQuality::Performance: return XESS_QUALITY_SETTING_PERFORMANCE;
            case UpscaleQuality::UltraPerformance: return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
            default: return XESS_QUALITY_SETTING_AA;
        }
    }
    static u32 initFlags() {
        // The exposure texture path returned images scaled by the exposure (verified: ~0.6x darker after the
        // pipeline's own exposure), so XeSS gets the unexposed HDR colour with scale 1 unless MCPP_XESS_EXPOSURE=1.
        static const bool noExposure = std::getenv("MCPP_XESS_EXPOSURE") == nullptr;
        return XESS_INIT_FLAG_INVERTED_DEPTH | (noExposure ? 0u : XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE) |
               XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
    }
    bool createFgContext() {
        if (!xell_) return false;
        if (xefgSwapChainD3D12CreateContext(hub.device(), &fg_) < 0 || !fg_) {
            LOG("xess-fg: not supported on this GPU");
            fg_ = nullptr;
            return false;
        }
        xefgSwapChainSetLoggingCallback(fg_, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, xefgLog, nullptr);
        xefgSwapChainSetLatencyReduction(fg_, xell_);
        xefgSwapChainD3D12BuildPipelines(fg_, nullptr, 0, XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH);
        return true;
    }
    void destroyFg() {
        if (fg_) {
            if (swapChainLive_) applySleepMode(false);
            const xefg_swapchain_result_t r = xefgSwapChainDestroy(fg_);
            if (r < 0) LOG("xess-fg: destroy failed (%d)", int(r));
        }
        fg_ = nullptr;
        swapChainLive_ = fgOn_ = false;
    }
    void applySleepMode(bool on) {
        if (!xell_) return;
        xell_sleep_params_t p{};
        p.bLowLatencyMode = on ? 1 : 0;
        p.minimumIntervalUs = on && fpsLimit_ ? u32(1e6 / double(fpsLimit_)) : 0;
        if (std::memcmp(&p, &sleep_, sizeof(p)) == 0) return;
        hub.waitIdle();  // XeLL: no GPU work in flight while the mode changes
        xellSetSleepMode(xell_, &p);
        sleep_ = p;
    }

    xess_context_handle_t ctx_ = nullptr;
    xefg_swapchain_handle_t fg_ = nullptr;
    xell_context_handle_t xell_ = nullptr;
    xell_sleep_params_t sleep_{};
    bool inited_ = false, swapChainLive_ = false, fgOn_ = false;
    u32 initW_ = 0, initH_ = 0, fpsLimit_ = 0;
    UpscaleQuality quality_ = UpscaleQuality::Native, initQuality_ = UpscaleQuality::Native;
    int warned_ = 0;
};

}  // namespace

std::unique_ptr<Backend> makeXess(Upscalers& hub) { return std::make_unique<Xess>(hub); }

#else

namespace {
class NoXess final : public Backend {
public:
    using Backend::Backend;
    void initDevice(IDXGIAdapter1*, Caps& caps) override { caps.why[int(UpscalerKind::Xess)] = "built without the XeSS SDK"; }
};
}  // namespace
std::unique_ptr<Backend> makeXess(Upscalers& hub) { return std::make_unique<NoXess>(hub); }

#endif

}  // namespace upscale
