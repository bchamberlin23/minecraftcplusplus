#pragma once
// Vendor upscalers and frame generation behind one object owned by the Renderer:
//   NVIDIA Streamline 2.14 (dlss.cpp):  DLSS 4 Super Resolution / DLAA (transformer presets), DLSS Frame Generation and
//                                       Multi Frame Generation (2x-4x), Reflex + PC Latency markers
//   AMD FidelityFX SDK 2.3 (fsr.cpp):   FSR Upscaling (4.x ML on RDNA3/4, 3.1 analytical elsewhere), FSR Frame
//                                       Generation with its frame interpolation swap chain, Anti-Lag 2 (AMD only)
//   Intel XeSS SDK 3.0 (xess.cpp):      XeSS-SR (DP4a on non-Intel GPUs), XeSS-FG (2x on non-Intel GPUs), XeLL
// Every SDK is optional: its DLLs are loaded at runtime (see tools/fetch_sdks.ps1) and anything missing or unsupported
// on this GPU / driver is simply not offered (the built-in TAA is always there).
//
// Hooks in the Renderer (render/renderer.cpp): device / queue / swap chain creation, beginFrame (latency sleep +
// simulation markers, main loop), the per-frame camera, jitter sequence + texture mip bias, the hudless copy before
// the UI composite, frame generation tagging before the command list closes, and Present.
#include "core/math.h"
#include "render/renderer.h"
#include "render/upscaler.h"
#include "render/upscalers/video_settings.h"

#include <memory>
#include <string>

namespace upscale {

// Per-frame camera, set by Renderer::render (unjittered). Mat4 is column-major with column vectors, which as raw
// memory is exactly the row-major, row-vector layout Streamline / XeSS-FG expect (M^T).
struct CameraFrame {
    double pos[3] = {};
    float right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, 1};
    Mat4 view;            // rotation only: (world - camera) -> view
    Mat4 proj;            // view -> clip, no jitter (reversed Z, infinite far)
    Mat4 clipToPrevClip;  // current clip -> previous frame's clip (camera motion)
    Mat4 prevClipToClip;
    float fovY = 1.2f, aspect = 1.0f, nearZ = 0.05f, farZ = 100000.0f;
    float jitterPx[2] = {};  // sample offset from the pixel centre in render pixels (+y down), see UpscaleInputs
    u32 renderSize[2] = {}, outputSize[2] = {};
    float frameTimeMs = 16.6f;
    bool reset = false;
};

// What frame generation needs at the end of a frame (recorded into the frame's command list before it closes).
struct FrameGenInputs {
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* depth = nullptr;    // render res, R32_TYPELESS (reversed Z), state below
    D3D12_RESOURCE_STATES depthState{};
    ID3D12Resource* motion = nullptr;   // render res, R16G16_FLOAT UV units prev - cur
    D3D12_RESOURCE_STATES motionState{};
    ID3D12Resource* hudless = nullptr;  // output res, swap chain format, scene without UI
    D3D12_RESOURCE_STATES hudlessState{};
    ID3D12Resource* ui = nullptr;       // output res, premultiplied UI layer
    D3D12_RESOURCE_STATES uiState{};
    ID3D12Resource* backbuffer = nullptr;
    const CameraFrame* cam = nullptr;
    u32 frameId = 0;
};

class Backend;  // one per vendor SDK (dlss.cpp / fsr.cpp / xess.cpp)

// First thing in main: starts the device-independent part of the SDK start-up (Streamline's slInit, ~0.7 s) on a
// thread, so it overlaps the asset loading; Renderer::init waits for it.
void preinit();

// What this machine offers (filled at device creation).
struct Caps {
    bool upscaler[int(UpscalerKind::Count)] = {true, false, false, false};
    bool frameGen[int(FrameGenKind::Count)] = {true, false, false, false};
    u32 maxMultiplier[int(FrameGenKind::Count)] = {1, 1, 1, 1};  // presented frames per rendered frame
    bool lowLatency[int(LowLatencyMode::Count)] = {true, false, false};
    const char* lowLatencyName = "";     // "NVIDIA Reflex", "AMD Anti-Lag 2", "Intel XeLL" (with XeSS-FG)
    std::string version[int(UpscalerKind::Count)];
    std::string why[int(UpscalerKind::Count)];  // reason an SDK is unavailable
};

}  // namespace upscale

class Upscalers {
public:
    explicit Upscalers(Renderer& renderer);
    ~Upscalers();
    Upscalers(const Upscalers&) = delete;
    Upscalers& operator=(const Upscalers&) = delete;

    // ---- Renderer::init / shutdown ----
    void initDevice(IDXGIAdapter1* adapter);  // after the device exists, before the queue and the swap chain
    bool createCommandQueue(const D3D12_COMMAND_QUEUE_DESC& desc, ComPtr<ID3D12CommandQueue>& out);
    // The swap chain for the active frame generation (a vendor proxy) or a plain one; false = failed.
    bool createSwapChain(HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& desc, ComPtr<IDXGISwapChain3>& out);
    void releaseSwapChain(ComPtr<IDXGISwapChain3>& swapchain);  // GPU idle, back buffers released
    void shutdown();                                             // before the device goes away

    void resizeSwapChain(IDXGISwapChain3* swapchain, u32 width, u32 height, DXGI_FORMAT format, UINT flags);
    ID3D12Device* device() const;
    ID3D12CommandQueue* queue() const;  // the renderer's queue (Streamline's proxy when loaded)
    IDXGIFactory6* factory() const;     // native factory
    u32 vendorId() const { return vendorId_; }
    const LUID& luid() const { return luid_; }

    // ---- settings (Video Settings screen, command line, options.txt) ----
    const upscale::Caps& caps() const { return caps_; }
    const VideoSettings& settings() const { return settings_; }
    // Validates against caps, then applies everything live (shader preset, render scale, swap chain re-creation for
    // frame generation changes, latency modes). Returns the settings actually in effect.
    VideoSettings apply(VideoSettings s);
    static const char* upscalerName(UpscalerKind k);
    static const char* frameGenName(FrameGenKind k);
    const char* qualityName(UpscalerKind k, UpscaleQuality q) const;  // vendor naming (DLAA, Native AA, ...)
    std::vector<UpscaleQuality> qualityModes(UpscalerKind k) const;
    float renderScale(UpscalerKind k, UpscaleQuality q) const;  // render / output per axis
    std::string status() const;  // one line for the Video Settings screen / F3

    // ---- per frame ----
    void beginFrame();            // main loop, before input / simulation: latency sleep, frame limiter, sim markers
    void submitStart();           // Renderer::render entry: sim end + render submit start
    void setCamera(upscale::CameraFrame cam);
    const upscale::CameraFrame& camera() const { return cam_; }
    u32 jitterPhases(u32 renderW, u32 outputW) const;
    float mipBias() const { return mipBias_; }
    bool frameGenActive() const { return fg_ != nullptr; }
    void setMenuActive(bool menu);  // full-screen menus: frame generation off meanwhile (resources kept)
    bool uiComposedBySwapChain() const;  // FSR FG: the swap chain blends the UI layer over real + generated frames
    // Before the UI composite: copies the hudless back buffer (frame generation only). Back buffer in RENDER_TARGET.
    void beforeUiComposite(ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer);
    void endFrame(ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer);  // before Close: FG tags / prepare
    void submitEnd();
    void present(IDXGISwapChain3* swapchain, bool vsync, bool tearing);
    u32 lastFramesPresented() const { return framesPresented_; }
    // For the SDK upscalers: R8 reactive mask from the World layer's coverage (entities have no motion vectors).
    // Returned in ALL_SHADER_RESOURCE state; nullptr if unavailable.
    ID3D12Resource* reactiveMask(const UpscaleInputs& in, float scale);
    ID3D12CommandQueue* nativeQueue() const;  // the renderer's queue without Streamline's proxy
    void setNativeQueue(ID3D12CommandQueue* q) { nativeQueue_ = q; }
    FrameGenKind effectiveFrameGen() const;
    u32 frameId() const { return frameId_; }
    void waitIdle();  // GPU idle (SDK re-initialisation)
    double presentedFrames() const { return presentedTotal_; }  // since start (real + generated)
    void logStats() const;

    Renderer& r;

private:
    friend class upscale::Backend;
    void applyLatency();
    void limitFrameRate();
    upscale::Backend* backend(UpscalerKind k) const;
    upscale::Backend* backend(FrameGenKind k) const;
    bool ensureHudless();

    upscale::Caps caps_;
    VideoSettings settings_;
    std::unique_ptr<upscale::Backend> dlss_, fsr_, xess_;
    upscale::Backend* fg_ = nullptr;        // backend owning the current swap chain's frame generation
    upscale::Backend* swapOwner_ = nullptr; // backend that created the current swap chain (FG proxy), if any
    upscale::CameraFrame cam_;
    double prevPos_[3] = {};
    Mat4 prevViewProj_;
    bool havePrev_ = false;
    float mipBias_ = 0;
    u32 frameId_ = 1;
    bool frameBegun_ = false, menuPaused_ = false;
    double nextFrameTime_ = 0;
    ComPtr<ID3D12Resource> hudless_;
    u32 hudlessW_ = 0, hudlessH_ = 0;
    u32 framesPresented_ = 1;
    double presentedTotal_ = 0;
    HANDLE timer_ = nullptr;
    ComPtr<ID3D12CommandQueue> nativeQueue_;
    ComPtr<ID3D12RootSignature> reactiveRoot_;
    ComPtr<ID3D12PipelineState> reactivePso_;
    ComPtr<ID3D12Resource> reactive_;
    ID3D12Resource* reactiveSrc_ = nullptr;
    u32 reactiveW_ = 0, reactiveH_ = 0, reactiveDesc_ = UINT32_MAX;
    LUID luid_{};
    u32 vendorId_ = 0;
};

namespace upscale {

// Vendor SDK backend. Each implements the parts its SDK provides; the defaults mean "not supported".
class Backend {
public:
    explicit Backend(Upscalers& hub) : hub(hub) {}
    virtual ~Backend() = default;
    virtual void initDevice(IDXGIAdapter1* adapter, Caps& caps) = 0;
    virtual void shutdown() {}
    // Super resolution.
    virtual ::Upscaler* upscaler() { return nullptr; }
    virtual float scale(UpscaleQuality) const { return 1.0f; }
    virtual void setQuality(UpscaleQuality) {}
    virtual float mipBiasOffset() const { return 0.0f; }  // added to log2(render / output)
    // Frame generation (owns a proxy swap chain while selected).
    virtual bool createCommandQueue(const D3D12_COMMAND_QUEUE_DESC&, ComPtr<ID3D12CommandQueue>&) { return false; }
    // frameGen = false: a plain swap chain this SDK still wants to see (Streamline's proxy for Reflex / PCL).
    virtual bool createSwapChain(HWND, const DXGI_SWAP_CHAIN_DESC1&, bool /*frameGen*/, ComPtr<IDXGISwapChain3>&) {
        return false;
    }
    // Streamline only: put its proxy over another SDK's swap chain so Reflex / PCL / garbage collection see Present.
    virtual void wrapSwapChain(ComPtr<IDXGISwapChain3>&) {}
    virtual void beforeReleaseSwapChain() {}  // the app still holds the swap chain (FSR: destroy contexts first)
    virtual void releaseSwapChain() {}        // all app references released
    virtual void setFrameGen(bool /*on*/, u32 /*multiplier*/) {}
    virtual bool composesUi() const { return false; }
    virtual void tagFrame(const FrameGenInputs&) {}
    virtual void beforePresent(u32 /*frameId*/) {}
    virtual void afterPresent(u32 /*frameId*/, u32& framesPresented) { framesPresented = 1; }
    // Low latency (Reflex / Anti-Lag 2 / XeLL) and markers.
    virtual bool ownsLatency() const { return false; }  // this backend's low-latency mode is active
    virtual void setLatency(LowLatencyMode, u32 /*fpsLimit*/) {}
    virtual bool limitsFrameRate() const { return false; }
    virtual void sleep(u32 /*frameId*/) {}
    enum Marker { SimStart, SimEnd, SubmitStart, SubmitEnd, PresentStart, PresentEnd };
    virtual void marker(Marker, u32 /*frameId*/) {}
    virtual std::string stats() const { return {}; }

    Upscalers& hub;
};

std::unique_ptr<Backend> makeDlss(Upscalers& hub);
std::unique_ptr<Backend> makeFsr(Upscalers& hub);
std::unique_ptr<Backend> makeXess(Upscalers& hub);

// Shared helpers.
bool exeDirFile(const wchar_t* name, std::wstring& fullPath);  // file next to the exe exists
bool verifySignature(const std::wstring& path);                 // Authenticode (WinVerifyTrust) of a vendor DLL
void transpose(const Mat4& m, float out[16]);                   // row-major float[16] of M (column-vector math)

}  // namespace upscale
