// NVIDIA Streamline 2.14 backend: DLSS Super Resolution / DLAA, DLSS Frame Generation + Multi Frame Generation,
// Reflex low latency + frame limiter, PC Latency (PCL) markers.
//
// Integration: manual hooking (ProgrammingGuideManualHooking.md). sl.interposer.dll is verified (NVIDIA signature)
// and loaded at runtime; the renderer keeps native D3D12 / DXGI objects and only the hooked calls go through SL
// proxies: the command queue is created on the proxy device and the swap chain on the proxy factory. DLSS-G is
// loaded only while it is the selected frame generation; the swap chain is re-created whenever that changes
// (DLSS-G guide section 18). Resources are tagged per frame (eUseFrameBasedResourceTagging): the upscaler's inputs
// locally at slEvaluateFeature, the frame generation inputs (depth, motion, hudless, UI colour + alpha) globally
// with their state at Present.
#include "render/upscalers/upscalers.h"

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>

#include <algorithm>
#include <cstring>
#include <thread>

namespace {

// Core API resolved from sl.interposer.dll (the feature helpers in sl_*.h go through slGetFeatureFunction below).
struct SlApi {
    HMODULE module = nullptr;
    PFun_slInit* init = nullptr;
    PFun_slShutdown* shutdown = nullptr;
    PFun_slIsFeatureSupported* isFeatureSupported = nullptr;
    PFun_slSetFeatureLoaded* setFeatureLoaded = nullptr;
    PFun_slSetTagForFrame* setTagForFrame = nullptr;
    PFun_slSetConstants* setConstants = nullptr;
    PFun_slEvaluateFeature* evaluateFeature = nullptr;
    PFun_slUpgradeInterface* upgradeInterface = nullptr;
    PFun_slGetNativeInterface* getNativeInterface = nullptr;
    PFun_slGetFeatureFunction* getFeatureFunction = nullptr;
    PFun_slGetNewFrameToken* getNewFrameToken = nullptr;
    PFun_slSetD3DDevice* setD3DDevice = nullptr;
    PFun_slGetFeatureVersion* getFeatureVersion = nullptr;
    PFun_slFreeResources* freeResources = nullptr;
} g_sl;

void slLog(sl::LogType type, const char* msg) {
    if (type == sl::LogType::eInfo && !std::getenv("MCPP_SL_VERBOSE")) return;
    std::string m = msg;
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    LOG("streamline %s: %s", type == sl::LogType::eError ? "error" : type == sl::LogType::eWarn ? "warning" : "info",
        m.c_str());
}

const char* slResultName(sl::Result r) {
    switch (r) {
        case sl::Result::eOk: return "ok";
        case sl::Result::eErrorDriverOutOfDate: return "driver out of date";
        case sl::Result::eErrorOSOutOfDate: return "Windows out of date";
        case sl::Result::eErrorOSDisabledHWS: return "hardware-accelerated GPU scheduling is off";
        case sl::Result::eErrorNoSupportedAdapterFound: return "no supported adapter";
        case sl::Result::eErrorAdapterNotSupported: return "adapter not supported";
        case sl::Result::eErrorFeatureMissing: return "plugin missing";
        case sl::Result::eErrorFeatureNotSupported: return "not supported";
        case sl::Result::eErrorFeatureFailedToLoad: return "plugin failed to load";
        default: return "error";
    }
}

sl::float4x4 slMatrix(const Mat4& m) {  // Mat4 memory (column-major, column vectors) == SL row-major, row vectors
    sl::float4x4 r;
    for (int i = 0; i < 4; ++i) r.row[i] = sl::float4(m.m[i * 4], m.m[i * 4 + 1], m.m[i * 4 + 2], m.m[i * 4 + 3]);
    return r;
}

}  // namespace

// The sl_*.h helpers (slDLSSSetOptions, slReflexSleep, ...) import feature functions through this symbol, normally
// provided by sl.interposer.lib; the DLL is loaded dynamically here, so forward it.
extern "C" sl::Result slGetFeatureFunction(sl::Feature feature, const char* functionName, void*& function) {
    return g_sl.getFeatureFunction ? g_sl.getFeatureFunction(feature, functionName, function) : sl::Result::eErrorNotInitialized;
}

namespace upscale {
namespace {

constexpr u32 kViewport = 0;  // DLSS-G expects viewport id 0

// The device-independent part of the start-up: verify and load sl.interposer.dll, slInit (plugin signature checks,
// ~0.7 s). Runs on the preinit thread while the game loads its assets. "" = ok, else why DLSS is unavailable.
std::string bootStreamline() {
    std::wstring path;
    if (!exeDirFile(L"sl.interposer.dll", path)) return "sl.interposer.dll missing (tools/fetch_sdks.ps1)";
    if (!sl::security::verifyEmbeddedSignature(path.c_str())) return "sl.interposer.dll is not signed by NVIDIA";
    g_sl.module = LoadLibraryW(path.c_str());
    if (!g_sl.module) return "sl.interposer.dll failed to load";
    auto get = [](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(g_sl.module, name));
        return fn != nullptr;
    };
    if (!(get(g_sl.init, "slInit") && get(g_sl.shutdown, "slShutdown") &&
          get(g_sl.isFeatureSupported, "slIsFeatureSupported") && get(g_sl.setFeatureLoaded, "slSetFeatureLoaded") &&
          get(g_sl.setTagForFrame, "slSetTagForFrame") && get(g_sl.setConstants, "slSetConstants") &&
          get(g_sl.evaluateFeature, "slEvaluateFeature") && get(g_sl.upgradeInterface, "slUpgradeInterface") &&
          get(g_sl.getNativeInterface, "slGetNativeInterface") && get(g_sl.getFeatureFunction, "slGetFeatureFunction") &&
          get(g_sl.getNewFrameToken, "slGetNewFrameToken") && get(g_sl.setD3DDevice, "slSetD3DDevice") &&
          get(g_sl.getFeatureVersion, "slGetFeatureVersion") && get(g_sl.freeResources, "slFreeResources")))
        return "sl.interposer.dll exports missing";

    std::wstring dir = fs::path(path).parent_path().wstring();
    const wchar_t* paths[] = {dir.c_str()};
    const sl::Feature features[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = std::getenv("MCPP_SL_VERBOSE") ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = nullptr;
    pref.logMessageCallback = slLog;
    // Manual hooking, frame-based tagging; no over-the-air plugin downloads (the shipped, verified DLLs only).
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = u32(std::size(features));
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "minecraftCplusplus 1.0";
    pref.projectId = "6c1f7a2e-9d43-4b6e-8a2d-3f5b9c0e71a4";
    pref.renderAPI = sl::RenderAPI::eD3D12;
    if (sl::Result res = g_sl.init(pref, sl::kSDKVersion); res != sl::Result::eOk)
        return std::string("slInit: ") + slResultName(res);
    return {};
}

struct Boot {
    std::thread thread;
    std::string result;
    bool started = false;
    ~Boot() {
        if (thread.joinable()) thread.join();  // a run that never created the renderer
    }
} g_boot;

bool hasNvidiaAdapter() {
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return false;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (d.VendorId == 0x10DE && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return true;
    }
    return false;
}

// Joins the preinit thread, or boots now when there was none.
std::string finishBoot() {
    if (!g_boot.started) {
        g_boot.started = true;
        g_boot.result = bootStreamline();
    } else if (g_boot.thread.joinable()) {
        g_boot.thread.join();
    }
    return g_boot.result;
}

class Dlss final : public Backend, public ::Upscaler {
public:
    explicit Dlss(Upscalers& hub) : Backend(hub) {}

    // ---- init ----
    void initDevice(IDXGIAdapter1*, Caps& caps) override {
        auto fail = [&](const std::string& why) { caps.why[int(UpscalerKind::Dlss)] = why; };
        const std::string booted = finishBoot();  // slInit, usually already done on the preinit thread
        if (hub.vendorId() != 0x10DE) {
            if (booted.empty() && g_sl.shutdown) g_sl.shutdown();
            return fail("not an NVIDIA GPU");
        }
        if (!booted.empty()) return fail(booted);
        initialized_ = true;
        if (sl::Result res = g_sl.setD3DDevice(hub.device()); res != sl::Result::eOk)
            return fail(std::string("slSetD3DDevice: ") + slResultName(res));

        sl::AdapterInfo ai{};
        LUID luid = hub.luid();
        ai.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
        ai.deviceLUIDSizeInBytes = sizeof(LUID);
        auto supported = [&](sl::Feature f, const char* name) {
            const sl::Result res = g_sl.isFeatureSupported(f, ai);
            if (res != sl::Result::eOk) LOG("streamline: %s unavailable: %s", name, slResultName(res));
            return res == sl::Result::eOk;
        };
        dlssOk_ = supported(sl::kFeatureDLSS, "DLSS");
        dlssgOk_ = supported(sl::kFeatureDLSS_G, "DLSS Frame Generation");
        reflexOk_ = supported(sl::kFeatureReflex, "Reflex");
        pclOk_ = supported(sl::kFeaturePCL, "PC Latency");
        active_ = true;

        sl::FeatureVersion v{};
        if (g_sl.getFeatureVersion(sl::kFeatureDLSS, v) == sl::Result::eOk)
            version_ = "SL " + v.versionSL.toStr() + ", DLSS " + v.versionNGX.toStr();
        if (dlssOk_) {
            caps.upscaler[int(UpscalerKind::Dlss)] = true;
            caps.version[int(UpscalerKind::Dlss)] = version_;
        } else {
            fail("DLSS not supported on this GPU / driver");
        }
        if (dlssgOk_) {
            sl::DLSSGState st{};
            u32 maxGen = 1;
            if (slDLSSGGetState(kViewport, st, nullptr) == sl::Result::eOk) maxGen = std::max(1u, st.numFramesToGenerateMax);
            if (g_sl.getFeatureVersion(sl::kFeatureDLSS_G, v) == sl::Result::eOk)
                LOG("streamline: DLSS Frame Generation %s, up to %u generated frames per rendered frame",
                    v.versionNGX.toStr().c_str(), maxGen);
            caps.frameGen[int(FrameGenKind::Dlss)] = true;
            caps.maxMultiplier[int(FrameGenKind::Dlss)] = 1 + maxGen;
            // DLSS-G is loaded on demand (only while selected): the swap chain then gets its proxy.
            g_sl.setFeatureLoaded(sl::kFeatureDLSS_G, false);
            dlssgLoaded_ = false;
        }
        if (reflexOk_) {
            sl::ReflexState rs{};
            if (slReflexGetState(rs) == sl::Result::eOk && rs.lowLatencyAvailable) {
                caps.lowLatency[int(LowLatencyMode::On)] = caps.lowLatency[int(LowLatencyMode::Boost)] = true;
                caps.lowLatencyName = "NVIDIA Reflex";
                lowLatencyAvailable_ = true;
            }
            setLatency(LowLatencyMode::Off, 0);  // must be called once
        }
        if (pclOk_) {
            sl::PCLState ps{};
            if (slPCLGetState(ps) == sl::Result::eOk) pingMessage_ = ps.statsWindowMessage;
        }
    }

    void shutdown() override {
        if (initialized_ && g_sl.shutdown) g_sl.shutdown();
        initialized_ = active_ = false;
        proxyFactory_.Reset();
        proxyDevice_.Reset();
    }

    bool createCommandQueue(const D3D12_COMMAND_QUEUE_DESC& desc, ComPtr<ID3D12CommandQueue>& out) override {
        if (!active_) return false;
        ID3D12Device* d = hub.device();
        d->AddRef();  // slUpgradeInterface wraps (and takes) this reference
        if (g_sl.upgradeInterface(reinterpret_cast<void**>(&d)) != sl::Result::eOk) {
            d->Release();
            return false;
        }
        proxyDevice_.Attach(d);
        if (FAILED(proxyDevice_->CreateCommandQueue(&desc, IID_PPV_ARGS(&out)))) return false;
        ComPtr<ID3D12CommandQueue> native;
        if (g_sl.getNativeInterface(out.Get(), reinterpret_cast<void**>(native.GetAddressOf())) == sl::Result::eOk)
            hub.setNativeQueue(native.Get());
        IDXGIFactory* f = hub.factory();
        f->AddRef();
        if (g_sl.upgradeInterface(reinterpret_cast<void**>(&f)) == sl::Result::eOk) {
            ComPtr<IDXGIFactory> pf;
            pf.Attach(f);
            pf.As(&proxyFactory_);
        } else {
            f->Release();
        }
        return true;
    }

    // ---- swap chain (DLSS-G proxy, or a plain one through SL so Reflex / PCL see Present) ----
    bool createSwapChain(HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& desc, bool frameGen, ComPtr<IDXGISwapChain3>& out) override {
        if (!active_ || !proxyFactory_ || (frameGen && !dlssgOk_)) return false;
        if (dlssgOk_ && dlssgLoaded_ != frameGen) {
            g_sl.setFeatureLoaded(sl::kFeatureDLSS_G, frameGen);
            dlssgLoaded_ = frameGen;
        }
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(proxyFactory_->CreateSwapChainForHwnd(hub.queue(), hwnd, &desc, nullptr, nullptr, &sc1))) return false;
        if (FAILED(sc1.As(&out))) return false;
        frameGenSwapChain_ = frameGen;
        return true;
    }
    void wrapSwapChain(ComPtr<IDXGISwapChain3>& sc) override {
        if (!active_ || !sc) return;
        if (dlssgOk_ && dlssgLoaded_) {
            g_sl.setFeatureLoaded(sl::kFeatureDLSS_G, false);
            dlssgLoaded_ = false;
        }
        IDXGISwapChain* p = sc.Get();
        p->AddRef();  // the proxy takes this reference
        if (g_sl.upgradeInterface(reinterpret_cast<void**>(&p)) != sl::Result::eOk) {
            p->Release();
            return;
        }
        ComPtr<IDXGISwapChain> proxy;
        proxy.Attach(p);
        ComPtr<IDXGISwapChain3> proxy3;
        if (SUCCEEDED(proxy.As(&proxy3))) sc = proxy3;
    }
    void releaseSwapChain() override {
        if (frameGenSwapChain_) {
            g_sl.freeResources(sl::kFeatureDLSS_G, kViewport);
            frameGenSwapChain_ = false;
        }
        if (dlssgOk_ && dlssgLoaded_) {
            g_sl.setFeatureLoaded(sl::kFeatureDLSS_G, false);
            dlssgLoaded_ = false;
        }
    }

    void setFrameGen(bool on, u32 multiplier) override {
        if (!dlssgLoaded_) return;
        sl::DLSSGOptions o{};
        o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        o.numFramesToGenerate = std::max(1u, multiplier - 1);
        o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        o.numBackBuffers = Renderer::kFrames;
        o.enableUserInterfaceRecomposition = sl::Boolean::eTrue;  // hudless + UI colour/alpha are tagged
        if (slDLSSGSetOptions(kViewport, o) != sl::Result::eOk) LOG("streamline: slDLSSGSetOptions failed");
        fgOn_ = on;
        fgMultiplier_ = multiplier;
    }

    void tagFrame(const FrameGenInputs& in) override {
        if (!fgOn_ || !token()) return;
        setConstants(*in.cam);
        const CameraFrame& c = *in.cam;
        sl::Extent renderExt{0, 0, c.renderSize[0], c.renderSize[1]}, outExt{0, 0, c.outputSize[0], c.outputSize[1]};
        sl::Resource depth{sl::ResourceType::eTex2d, in.depth, nullptr, nullptr, u32(in.depthState)};
        sl::Resource motion{sl::ResourceType::eTex2d, in.motion, nullptr, nullptr, u32(in.motionState)};
        sl::Resource hudless{sl::ResourceType::eTex2d, in.hudless, nullptr, nullptr, u32(in.hudlessState)};
        sl::Resource ui{sl::ResourceType::eTex2d, in.ui, nullptr, nullptr, u32(in.uiState)};
        const sl::ResourceTag tags[] = {
            {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExt},
            {&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderExt},
            {&hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &outExt},
            {&ui, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &outExt},
            {nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &outExt},  // full extent
        };
        if (g_sl.setTagForFrame(*token_, kViewport, tags, u32(std::size(tags)), in.list) != sl::Result::eOk)
            warnOnce("slSetTagForFrame (frame generation) failed");
    }

    void afterPresent(u32, u32& framesPresented) override {
        framesPresented = 1;
        if (!fgOn_) return;
        sl::DLSSGState st{};
        if (slDLSSGGetState(kViewport, st, nullptr) == sl::Result::eOk) {
            framesPresented = std::max(1u, st.numFramesActuallyPresented);
            if (st.status != sl::DLSSGStatus::eOk && st.status != lastStatus_) {
                LOG("streamline: DLSS-G status 0x%x (see sl_dlss_g.h DLSSGStatus)", u32(st.status));
                lastStatus_ = st.status;
            }
        }
    }

    // ---- super resolution ----
    ::Upscaler* upscaler() override { return dlssOk_ ? this : nullptr; }
    const char* name() const override { return "DLSS"; }
    float scale(UpscaleQuality q) const override {
        if (!dlssOk_) return 1.0f;
        sl::DLSSOptions o{};
        o.mode = mode(q);
        o.outputWidth = std::max(hub.r.width(), 1u), o.outputHeight = std::max(hub.r.height(), 1u);
        sl::DLSSOptimalSettings s{};
        if (slDLSSGetOptimalSettings(o, s) != sl::Result::eOk || !s.optimalRenderWidth) return 1.0f;
        return std::clamp(float(s.optimalRenderWidth) / float(o.outputWidth), 0.33f, 1.0f);
    }
    void setQuality(UpscaleQuality q) override { quality_ = q; }
    float mipBiasOffset() const override { return 0.0f; }  // log2(render / output), DLSS guidance

    void evaluate(const UpscaleInputs& in) override {
        if (!token()) return;
        setConstants(hub.camera());
        sl::DLSSOptions o{};
        o.mode = mode(quality_);
        o.outputWidth = in.outputSize[0], o.outputHeight = in.outputSize[1];
        o.colorBuffersHDR = sl::Boolean::eTrue;
        o.useAutoExposure = sl::Boolean::eFalse;  // the pipeline's exposure texture is tagged
        o.preExposure = 1.0f;
        if (o.mode != options_.mode || o.outputWidth != options_.outputWidth || o.outputHeight != options_.outputHeight) {
            if (slDLSSSetOptions(kViewport, o) != sl::Result::eOk) warnOnce("slDLSSSetOptions failed");
            options_ = o;
        }
        ID3D12Resource* reactive = hub.reactiveMask(in, 0.5f);
        sl::Extent renderExt{0, 0, in.renderSize[0], in.renderSize[1]}, outExt{0, 0, in.outputSize[0], in.outputSize[1]};
        sl::Extent one{0, 0, 1, 1};
        const u32 srv = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        sl::Resource color{sl::ResourceType::eTex2d, in.color, nullptr, nullptr, srv};
        sl::Resource output{sl::ResourceType::eTex2d, in.output, nullptr, nullptr, u32(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        sl::Resource depth{sl::ResourceType::eTex2d, in.depth, nullptr, nullptr,
                           u32(D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)};
        sl::Resource motion{sl::ResourceType::eTex2d, in.motion, nullptr, nullptr, srv};
        sl::Resource exposure{sl::ResourceType::eTex2d, in.exposure, nullptr, nullptr, srv};
        sl::Resource bias{sl::ResourceType::eTex2d, reactive, nullptr, nullptr, srv};
        sl::ResourceTag tags[] = {
            {&color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExt},
            {&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &outExt},
            {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExt},
            {&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExt},
            {&exposure, sl::kBufferTypeExposure, sl::ResourceLifecycle::eValidUntilEvaluate, &one},
            {&bias, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExt},
        };
        const u32 tagCount = reactive ? 6 : 5;
        sl::ViewportHandle vp(kViewport);
        const sl::BaseStructure* inputs[8] = {&vp};
        for (u32 i = 0; i < tagCount; ++i) inputs[i + 1] = &tags[i];
        if (g_sl.evaluateFeature(sl::kFeatureDLSS, *token_, inputs, tagCount + 1, in.list) != sl::Result::eOk)
            warnOnce("slEvaluateFeature(DLSS) failed");
    }

    // ---- Reflex / PCL ----
    bool ownsLatency() const override { return reflexOk_ && mode_ != LowLatencyMode::Off; }
    void setLatency(LowLatencyMode m, u32 fpsLimit) override {
        if (!reflexOk_) return;
        sl::ReflexOptions o{};
        o.mode = m == LowLatencyMode::Boost ? sl::ReflexMode::eLowLatencyWithBoost
                 : m == LowLatencyMode::On  ? sl::ReflexMode::eLowLatency
                                            : sl::ReflexMode::eOff;
        if (!lowLatencyAvailable_) o.mode = sl::ReflexMode::eOff;
        // The limit counts rendered frames; with frame generation the display gets `multiplier` times as many.
        const u32 mult = fgOn_ ? fgMultiplier_ : 1;
        o.frameLimitUs = fpsLimit ? u32(1e6 * double(mult) / double(fpsLimit)) : 0;
        if (slReflexSetOptions(o) != sl::Result::eOk) LOG("streamline: slReflexSetOptions failed");
        mode_ = m, fpsLimit_ = fpsLimit;
    }
    bool limitsFrameRate() const override { return reflexOk_ && fpsLimit_ > 0; }
    void sleep(u32 frameId) override {
        if (!active_) return;
        token_ = nullptr;
        if (g_sl.getNewFrameToken(token_, &frameId) != sl::Result::eOk) token_ = nullptr;
        tokenFrame_ = frameId;
        constantsFrame_ = ~0u;
        if (token_ && reflexOk_) slReflexSleep(*token_);  // always called; a no-op when Reflex is off
    }
    void marker(Marker m, u32 frameId) override {
        if (!pclOk_ || !token_ || frameId != tokenFrame_) return;
        static const sl::PCLMarker map[] = {sl::PCLMarker::eSimulationStart, sl::PCLMarker::eSimulationEnd,
                                            sl::PCLMarker::eRenderSubmitStart, sl::PCLMarker::eRenderSubmitEnd,
                                            sl::PCLMarker::ePresentStart, sl::PCLMarker::ePresentEnd};
        slPCLSetMarker(map[m], *token_);
    }
    std::string stats() const override {
        if (!reflexOk_) return {};
        std::string s = std::string("Reflex ") + (mode_ == LowLatencyMode::Boost ? "On + Boost" : mode_ == LowLatencyMode::On ? "On" : "Off");
        sl::ReflexState rs{};
        if (slReflexGetState(rs) == sl::Result::eOk && rs.latencyReportAvailable) {
            double sum = 0, gpu = 0;
            int n = 0;
            for (const sl::ReflexReport& r : rs.frameReport)
                if (r.simStartTime && r.gpuRenderEndTime > r.simStartTime) {
                    sum += double(r.gpuRenderEndTime - r.simStartTime);
                    gpu += double(r.gpuActiveRenderTimeUs);
                    ++n;
                }
            if (n) {
                char b[128];
                std::snprintf(b, sizeof(b), " (sim start -> GPU done %.1f ms, GPU busy %.2f ms, %d frames)", sum / n / 1000.0,
                              gpu / n / 1000.0, n);
                s += b;
            }
        }
        return s;
    }

private:
    static sl::DLSSMode mode(UpscaleQuality q) {
        switch (q) {
            case UpscaleQuality::Native: return sl::DLSSMode::eDLAA;
            case UpscaleQuality::UltraQualityPlus:
            case UpscaleQuality::UltraQuality:
            case UpscaleQuality::Quality: return sl::DLSSMode::eMaxQuality;
            case UpscaleQuality::Balanced: return sl::DLSSMode::eBalanced;
            case UpscaleQuality::Performance: return sl::DLSSMode::eMaxPerformance;
            case UpscaleQuality::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
            default: return sl::DLSSMode::eDLAA;
        }
    }
    bool token() {
        if (!token_ || tokenFrame_ != hub.frameId()) sleep(hub.frameId());  // frame begun without beginFrame
        return token_ != nullptr;
    }
    // Common constants, once per frame (DLSS and DLSS-G share them).
    void setConstants(const CameraFrame& c) {
        if (constantsFrame_ == tokenFrame_) return;
        constantsFrame_ = tokenFrame_;
        static const float jitterSign = [] {
            const char* v = std::getenv("MCPP_DLSS_JITTER_SIGN");
            return v ? float(std::atof(v)) : -1.0f;
        }();
        sl::Constants k{};
        k.cameraViewToClip = slMatrix(c.proj);
        // Clip -> view for the infinite reversed-Z projection.
        Mat4 inv;
        inv(0, 0) = 1.0f / c.proj(0, 0), inv(1, 1) = 1.0f / c.proj(1, 1);
        inv(2, 3) = 1.0f, inv(3, 2) = 1.0f / c.nearZ;
        k.clipToCameraView = slMatrix(inv);
        k.clipToLensClip = slMatrix([] { Mat4 i; i(0, 0) = i(1, 1) = i(2, 2) = i(3, 3) = 1; return i; }());
        k.clipToPrevClip = slMatrix(c.clipToPrevClip);
        k.prevClipToClip = slMatrix(c.prevClipToClip);
        k.jitterOffset = sl::float2(jitterSign * c.jitterPx[0], jitterSign * c.jitterPx[1]);
        k.mvecScale = sl::float2(1.0f, 1.0f);  // motion is already in UV units (prev - cur)
        k.cameraPinholeOffset = sl::float2(0, 0);
        k.cameraPos = sl::float3(float(c.pos[0]), float(c.pos[1]), float(c.pos[2]));
        k.cameraUp = sl::float3(c.up[0], c.up[1], c.up[2]);
        k.cameraRight = sl::float3(c.right[0], c.right[1], c.right[2]);
        k.cameraFwd = sl::float3(c.fwd[0], c.fwd[1], c.fwd[2]);
        k.cameraNear = c.nearZ;
        k.cameraFar = c.farZ;
        k.cameraFOV = c.fovY;
        k.cameraAspectRatio = c.aspect;
        k.motionVectorsInvalidValue = 0.0f;
        k.depthInverted = sl::Boolean::eTrue;
        k.cameraMotionIncluded = sl::Boolean::eTrue;
        k.motionVectors3D = sl::Boolean::eFalse;
        k.reset = c.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        k.orthographicProjection = sl::Boolean::eFalse;
        k.motionVectorsDilated = sl::Boolean::eFalse;
        k.motionVectorsJittered = sl::Boolean::eFalse;
        if (g_sl.setConstants(k, *token_, kViewport) != sl::Result::eOk) warnOnce("slSetConstants failed");
    }
    void warnOnce(const char* what) {
        if (warned_++ < 5) LOG("streamline: %s", what);
    }

    bool initialized_ = false, active_ = false;
    bool dlssOk_ = false, dlssgOk_ = false, reflexOk_ = false, pclOk_ = false, lowLatencyAvailable_ = false;
    bool dlssgLoaded_ = true, frameGenSwapChain_ = false, fgOn_ = false;
    u32 fgMultiplier_ = 2, fpsLimit_ = 0, pingMessage_ = 0;
    LowLatencyMode mode_ = LowLatencyMode::Off;
    UpscaleQuality quality_ = UpscaleQuality::Native;
    sl::DLSSOptions options_{};
    sl::DLSSGStatus lastStatus_ = sl::DLSSGStatus::eOk;
    sl::FrameToken* token_ = nullptr;
    u32 tokenFrame_ = ~0u, constantsFrame_ = ~0u;
    int warned_ = 0;
    std::string version_;
    ComPtr<ID3D12Device> proxyDevice_;
    ComPtr<IDXGIFactory2> proxyFactory_;
};

}  // namespace

void preinit() {
    g_boot.started = true;
    g_boot.thread = std::thread([] {
        g_boot.result = hasNvidiaAdapter() ? bootStreamline() : std::string("not an NVIDIA GPU");
    });
}

std::unique_ptr<Backend> makeDlss(Upscalers& hub) { return std::make_unique<Dlss>(hub); }

}  // namespace upscale
