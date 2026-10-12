#include "render/upscalers/upscalers.h"

#include "render/packs/pack_pipeline.h"
#include "render/shading.h"

#include <softpub.h>
#include <wintrust.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

#include "ReactiveCS.h"

namespace upscale {

bool exeDirFile(const wchar_t* name, std::wstring& fullPath) {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    fullPath = fs::path(exe).parent_path() / name;
    return GetFileAttributesW(fullPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool verifySignature(const std::wstring& path) {
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    if (status != ERROR_SUCCESS) LOG("upscalers: %s is not validly signed (0x%08lx)", fs::path(path).filename().string().c_str(), status);
    return status == ERROR_SUCCESS;
}

void transpose(const Mat4& m, float out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = m(r, c);
}

namespace {

// General 4x4 inverse in double (column-vector Mat4).
Mat4 inverse(const Mat4& a) {
    double m[16], inv[16];
    for (int i = 0; i < 16; ++i) m[i] = a.m[i];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 r;
    if (std::abs(det) < 1e-30) return r;
    det = 1.0 / det;
    for (int i = 0; i < 16; ++i) r.m[i] = float(inv[i] * det);
    return r;
}

}  // namespace
}  // namespace upscale

using namespace upscale;

Upscalers::Upscalers(Renderer& renderer) : r(renderer) {
    dlss_ = makeDlss(*this);
    fsr_ = makeFsr(*this);
    xess_ = makeXess(*this);
}

Upscalers::~Upscalers() {
    if (timer_) CloseHandle(timer_);
}

Backend* Upscalers::backend(UpscalerKind k) const {
    switch (k) {
        case UpscalerKind::Dlss: return dlss_.get();
        case UpscalerKind::Fsr: return fsr_.get();
        case UpscalerKind::Xess: return xess_.get();
        default: return nullptr;
    }
}

::Upscaler* Upscalers::rayReconstruction() const {
    return settings_.upscaler == UpscalerKind::Dlss && caps_.rayReconstruction ? dlss_->rayReconstruction() : nullptr;
}

Backend* Upscalers::backend(FrameGenKind k) const {
    switch (k) {
        case FrameGenKind::Dlss: return dlss_.get();
        case FrameGenKind::Fsr: return fsr_.get();
        case FrameGenKind::Xess: return xess_.get();
        default: return nullptr;
    }
}

const char* Upscalers::upscalerName(UpscalerKind k) {
    static const char* n[] = {"TAA (built-in)", "NVIDIA DLSS", "AMD FSR", "Intel XeSS"};
    return n[int(k)];
}

const char* Upscalers::frameGenName(FrameGenKind k) {
    static const char* n[] = {"Off", "DLSS Frame Generation", "FSR Frame Generation", "XeSS Frame Generation"};
    return n[int(k)];
}

std::vector<UpscaleQuality> Upscalers::qualityModes(UpscalerKind k) const {
    using Q = UpscaleQuality;
    switch (k) {
        case UpscalerKind::Xess:
            return {Q::Native, Q::UltraQualityPlus, Q::UltraQuality, Q::Quality, Q::Balanced, Q::Performance, Q::UltraPerformance};
        default: return {Q::Native, Q::Quality, Q::Balanced, Q::Performance, Q::UltraPerformance};
    }
}

const char* Upscalers::qualityName(UpscalerKind k, UpscaleQuality q) const {
    switch (q) {
        case UpscaleQuality::Native:
            return k == UpscalerKind::Dlss ? "DLAA" : k == UpscalerKind::Taa ? "Native" : "Native AA";
        case UpscaleQuality::UltraQualityPlus: return "Ultra Quality Plus";
        case UpscaleQuality::UltraQuality: return "Ultra Quality";
        case UpscaleQuality::Quality: return "Quality";
        case UpscaleQuality::Balanced: return "Balanced";
        case UpscaleQuality::Performance: return "Performance";
        case UpscaleQuality::UltraPerformance: return "Ultra Performance";
        default: return "?";
    }
}

float Upscalers::renderScale(UpscalerKind k, UpscaleQuality q) const {
    if (Backend* b = backend(k)) return b->scale(q);
    switch (q) {  // built-in TAA(U): the usual ratios
        case UpscaleQuality::UltraQualityPlus: return 1 / 1.3f;
        case UpscaleQuality::UltraQuality: return 1 / 1.5f;
        case UpscaleQuality::Quality: return 1 / 1.5f;
        case UpscaleQuality::Balanced: return 1 / 1.7f;
        case UpscaleQuality::Performance: return 0.5f;
        case UpscaleQuality::UltraPerformance: return 1 / 3.0f;
        default: return 1.0f;
    }
}

// ---- device / swap chain -------------------------------------------------------------------------------------

void Upscalers::initDevice(IDXGIAdapter1* adapter) {
    DXGI_ADAPTER_DESC1 desc{};
    if (adapter) adapter->GetDesc1(&desc);
    vendorId_ = desc.VendorId, luid_ = desc.AdapterLuid;
    // XeSS (~0.15 s) initialises on its own thread while Streamline attaches to the device (~0.9 s).
    const Timer timer;
    upscale::Caps xessCaps;
    std::thread xessInit([&] { xess_->initDevice(adapter, xessCaps); });
    dlss_->initDevice(adapter, caps_);
    fsr_->initDevice(adapter, caps_);
    xessInit.join();
    constexpr int x = int(UpscalerKind::Xess), xf = int(FrameGenKind::Xess);
    caps_.upscaler[x] = xessCaps.upscaler[x], caps_.version[x] = xessCaps.version[x], caps_.why[x] = xessCaps.why[x];
    caps_.frameGen[xf] = xessCaps.frameGen[xf], caps_.maxMultiplier[xf] = xessCaps.maxMultiplier[xf];
    if (!caps_.lowLatency[1] && xessCaps.lowLatencyName[0]) caps_.lowLatencyName = xessCaps.lowLatencyName;
    std::string line = "upscalers:";
    for (int k = 1; k < int(UpscalerKind::Count); ++k)
        line += std::string(" ") + upscalerName(UpscalerKind(k)) + " " +
                (caps_.upscaler[k] ? caps_.version[k] : "unavailable (" + caps_.why[k] + ")") + " |";
    line += " frame generation:";
    for (int k = 1; k < int(FrameGenKind::Count); ++k)
        if (caps_.frameGen[k]) line += std::string(" ") + frameGenName(FrameGenKind(k)) + " (up to " +
                                       std::to_string(caps_.maxMultiplier[k]) + "x)";
    if (caps_.lowLatency[1]) line += std::string(" | low latency: ") + caps_.lowLatencyName;
    line += " | " + std::to_string(int(timer.ms())) + " ms";
    LOG("%s", line.c_str());

    // Reactive mask pass: own root signature (t0 table, u0 table, 4 constants).
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV, ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV, ranges[1].NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &ranges[0]};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &ranges[1]};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.Num32BitValues = 4;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 3, rd.pParameters = params;
    ComPtr<ID3DBlob> blob, err;
    if (SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) &&
        SUCCEEDED(r.device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&reactiveRoot_)))) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
        cd.pRootSignature = reactiveRoot_.Get();
        cd.CS = {g_ReactiveCS, sizeof(g_ReactiveCS)};
        r.device_->CreateComputePipelineState(&cd, IID_PPV_ARGS(&reactivePso_));
    }
}

bool Upscalers::createCommandQueue(const D3D12_COMMAND_QUEUE_DESC& desc, ComPtr<ID3D12CommandQueue>& out) {
    return dlss_->createCommandQueue(desc, out);
}

ID3D12CommandQueue* Upscalers::nativeQueue() const {
    return nativeQueue_ ? nativeQueue_.Get() : r.queue_.Get();
}

bool Upscalers::createSwapChain(HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& desc, ComPtr<IDXGISwapChain3>& out) {
    fg_ = swapOwner_ = nullptr;
    const FrameGenKind want = effectiveFrameGen();
    if (Backend* b = backend(want)) {
        if (b->createSwapChain(hwnd, desc, true, out)) {
            fg_ = swapOwner_ = b;
            menuPaused_ = false;
            // FSR's swap chain gets Streamline's proxy on top (presentCommon: Reflex / PCL / SL garbage collection).
            // Not XeSS-FG: its destroy requires every external reference gone, and SL's proxy keeps one.
            if (b == fsr_.get()) dlss_->wrapSwapChain(out);
            b->setFrameGen(true, settings_.frameGenMultiplier);
            LOG("frame generation: %s %ux on", frameGenName(want), settings_.frameGenMultiplier);
            return true;
        }
        LOG("frame generation: %s could not create its swap chain; off", frameGenName(want));
    }
    // Plain swap chain (through Streamline's proxy when it is loaded, so Reflex / PCL keep working).
    if (dlss_->createSwapChain(hwnd, desc, false, out)) {
        swapOwner_ = dlss_.get();
        return true;
    }
    return false;
}

void Upscalers::releaseSwapChain(ComPtr<IDXGISwapChain3>& swapchain) {
    if (fg_) fg_->setFrameGen(false, 1);
    if (swapOwner_) swapOwner_->beforeReleaseSwapChain();
    swapchain.Reset();
    if (swapOwner_) swapOwner_->releaseSwapChain();
    fg_ = swapOwner_ = nullptr;
}

void Upscalers::shutdown() {
    for (Backend* b : {xess_.get(), fsr_.get(), dlss_.get()}) b->shutdown();
}

FrameGenKind Upscalers::effectiveFrameGen() const {
    // Frame generation needs the shader pipeline's motion vectors, depth, hudless image and UI layer.
    if (settings_.shaders == 0 || !caps_.frameGen[int(settings_.frameGen)]) return FrameGenKind::Off;
    return settings_.frameGen;
}

// ---- settings ----------------------------------------------------------------------------------------------

VideoSettings Upscalers::apply(VideoSettings s) {
    s.shaders = std::min<u8>(s.shaders, u8(ShaderPreset::PathTraced));
    if (s.shaders == u8(ShaderPreset::PathTraced) && !r.pathTracingSupported()) s.shaders = u8(ShaderPreset::Ultra);
    if (!caps_.upscaler[int(s.upscaler)]) s.upscaler = UpscalerKind::Taa;
    {  // quality the vendor offers (nearest otherwise)
        const auto modes = qualityModes(s.upscaler);
        if (std::find(modes.begin(), modes.end(), s.quality) == modes.end())
            s.quality = s.quality == UpscaleQuality::UltraQualityPlus || s.quality == UpscaleQuality::UltraQuality
                            ? UpscaleQuality::Quality
                            : UpscaleQuality::Native;
    }
    s.sharpness = std::clamp(s.sharpness, 0.0f, 1.0f);
    if (!caps_.frameGen[int(s.frameGen)]) s.frameGen = FrameGenKind::Off;
    s.frameGenMultiplier = s.frameGen == FrameGenKind::Off ? 2 : std::clamp(s.frameGenMultiplier, 2u, caps_.maxMultiplier[int(s.frameGen)]);
    if (!caps_.lowLatency[int(s.lowLatency)]) s.lowLatency = caps_.lowLatency[1] && s.lowLatency == LowLatencyMode::Boost ? LowLatencyMode::On : LowLatencyMode::Off;
    s.fpsLimit = s.fpsLimit ? std::clamp(s.fpsLimit, 20u, 1000u) : 0;

    const VideoSettings old = settings_;
    const FrameGenKind oldFg = effectiveFrameGen();
    settings_ = s;

    // Shader preset, then the render scale and sharpening on top of it.
    const ShaderPreset preset = ShaderPreset(s.shaders);
    if (preset != r.shaderPreset()) r.setShaderPreset(preset);
    s.pathTracing = std::min<u8>(s.pathTracing, 3);
    if (preset == ShaderPreset::PathTraced) {  // bounces, samples and radius of the path tracing quality
        ShaderSettings ss = r.shaderSettings(), q = ss;
        q.setPathTraceQuality(PathTraceQuality(s.pathTracing));
        if (q.ptBounces != ss.ptBounces || q.ptSamples != ss.ptSamples || q.ptRadius != ss.ptRadius) r.setShaderSettings(q);
    }
    if (preset != ShaderPreset::Off) {
        ShaderSettings ss = r.shaderSettings();
        const float scale = s.upscaler == UpscalerKind::Taa && s.renderScaleOverride > 0 ? s.renderScaleOverride
                                                                                         : renderScale(s.upscaler, s.quality);
        if (ss.renderScale != scale || ss.sharpen != s.sharpness) {
            ss.renderScale = scale, ss.sharpen = s.sharpness;
            r.setShaderSettings(ss);
        }
    }
    Backend* up = backend(s.upscaler);
    if (up) up->setQuality(s.quality);
    r.setUpscaler(up ? up->upscaler() : nullptr);
    if (old.upscaler != s.upscaler || old.quality != s.quality) r.resetTemporal();
    const float ratio = r.renderWidth() ? float(r.renderWidth()) / float(std::max(r.width(), 1u)) : 1.0f;
    mipBias_ = preset == ShaderPreset::Off ? 0.0f : std::log2(std::min(ratio, 1.0f)) + (up ? up->mipBiasOffset() : 0.0f);

    // Frame generation: a different SDK needs its own swap chain; the multiplier changes live.
    const FrameGenKind newFg = effectiveFrameGen();
    if (newFg != oldFg && r.swapchain_) {
        r.recreateSwapChain();
    } else if (fg_ && !menuPaused_ && s.frameGenMultiplier != old.frameGenMultiplier) {
        fg_->setFrameGen(true, s.frameGenMultiplier);
    }
    r.setVsync(s.vsync);
    applyLatency();
    LOG("video: %s", status().c_str());
    return s;
}

void Upscalers::applyLatency() {
    LowLatencyMode mode = settings_.lowLatency;
    // DLSS Frame Generation requires Reflex; XeSS-FG requires XeLL and no other latency reduction next to it.
    if (fg_ == dlss_.get() && mode == LowLatencyMode::Off) mode = LowLatencyMode::On;
    const bool xell = fg_ == xess_.get();
    dlss_->setLatency(xell ? LowLatencyMode::Off : mode, xell ? 0 : settings_.fpsLimit);
    fsr_->setLatency(xell ? LowLatencyMode::Off : mode, xell ? 0 : settings_.fpsLimit);
    xess_->setLatency(xell ? LowLatencyMode::On : LowLatencyMode::Off, settings_.fpsLimit);
}

void Upscalers::waitIdle() { r.waitIdle(); }

std::string Upscalers::status() const {
    char buf[512];
    const UpscalerKind k = settings_.upscaler;
    const bool on = r.shaderPreset() != ShaderPreset::Off;
    int n = std::snprintf(buf, sizeof(buf), "%s %s%s%s | %ux%u -> %ux%u", upscalerName(k), qualityName(k, settings_.quality),
                          caps_.version[int(k)].empty() ? "" : " ", caps_.version[int(k)].c_str(), r.renderWidth(),
                          r.renderHeight(), r.width(), r.height());
    if (!on) n = std::snprintf(buf, sizeof(buf), "shaders off: no upscaling | %ux%u", r.width(), r.height());
    if (on && r.shaderPreset() == ShaderPreset::PathTraced)
        n += std::snprintf(buf + n, sizeof(buf) - n, " | path traced %s, %s", VideoSettings::pathTracingKey(settings_.pathTracing),
                           rayReconstruction() ? "DLSS Ray Reconstruction" : "built-in denoiser");
    if (fg_)
        n += std::snprintf(buf + n, sizeof(buf) - n, " | %s %ux", frameGenName(effectiveFrameGen()), settings_.frameGenMultiplier);
    for (Backend* b : {dlss_.get(), fsr_.get(), xess_.get()})
        if (b->ownsLatency()) n += std::snprintf(buf + n, sizeof(buf) - n, " | %s", b->stats().c_str());
    if (settings_.fpsLimit) n += std::snprintf(buf + n, sizeof(buf) - n, " | limit %u fps", settings_.fpsLimit);
    if (settings_.vsync) std::snprintf(buf + n, sizeof(buf) - n, " | vsync");
    return buf;
}

void Upscalers::logStats() const {
    for (Backend* b : {dlss_.get(), fsr_.get(), xess_.get()}) {
        const std::string s = b->stats();
        if (!s.empty()) LOG("upscalers: %s", s.c_str());
    }
    if (presentedTotal_ > 0) LOG("upscalers: %.0f frames presented (rendered + generated)", presentedTotal_);
}

// ---- per frame ---------------------------------------------------------------------------------------------

void Upscalers::limitFrameRate() {
    if (!settings_.fpsLimit) return;
    for (Backend* b : {dlss_.get(), fsr_.get(), xess_.get()})
        if (b->limitsFrameRate()) return;  // Reflex / XeLL pace the frames themselves
    // Rendered frames per second = limit / frames presented per rendered frame.
    const double interval = double(fg_ ? settings_.frameGenMultiplier : 1) / double(settings_.fpsLimit);
    double now = nowSeconds();
    if (nextFrameTime_ == 0 || now - nextFrameTime_ > 0.25) nextFrameTime_ = now;
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    while ((now = nowSeconds()) < nextFrameTime_) {
        const double left = nextFrameTime_ - now;
        if (left > 0.0015 && timer_) {
            LARGE_INTEGER due;
            due.QuadPart = -LONGLONG((left - 0.001) * 1e7);
            SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0);
            WaitForSingleObject(timer_, INFINITE);
        } else {
            YieldProcessor();
        }
    }
    nextFrameTime_ += interval;
}

void Upscalers::beginFrame() {
    ++frameId_;
    frameBegun_ = true;
    limitFrameRate();
    for (Backend* b : {dlss_.get(), fsr_.get(), xess_.get()}) b->sleep(frameId_);
    for (Backend* b : {dlss_.get(), fsr_.get(), xess_.get()}) b->marker(Backend::SimStart, frameId_);
}

void Upscalers::submitStart() {
    if (!frameBegun_) beginFrame();  // callers that do not run the main loop (tools, tests)
    frameBegun_ = false;
    for (Backend* b : {dlss_.get(), fsr_.get(), xess_.get()}) {
        b->marker(Backend::SimEnd, frameId_);
        b->marker(Backend::SubmitStart, frameId_);
    }
}

void Upscalers::submitEnd() {
    for (Backend* b : {dlss_.get(), fsr_.get(), xess_.get()}) b->marker(Backend::SubmitEnd, frameId_);
}

void Upscalers::setCamera(CameraFrame c) {
    const Mat4 vp = c.proj * c.view;
    const double move[3] = {c.pos[0] - prevPos_[0], c.pos[1] - prevPos_[1], c.pos[2] - prevPos_[2]};
    const bool pack = r.packs_ && r.packs_->active();  // a shader pack: the built-in pipeline's history does not apply
    c.reset = !havePrev_ || (!pack && r.shading_ && !r.shading_->historyValid_) ||
              std::abs(move[0]) + std::abs(move[1]) + std::abs(move[2]) > 64.0;
    Mat4 t;
    t(0, 0) = t(1, 1) = t(2, 2) = t(3, 3) = 1;
    t(0, 3) = float(move[0]), t(1, 3) = float(move[1]), t(2, 3) = float(move[2]);
    const Mat4 prevVp = c.reset ? vp : prevViewProj_ * t;
    c.clipToPrevClip = prevVp * inverse(vp);
    c.prevClipToClip = inverse(c.clipToPrevClip);
    static double last = 0;
    const double now = nowSeconds();
    c.frameTimeMs = last ? float(std::clamp((now - last) * 1000.0, 0.1, 200.0)) : 16.6f;
    last = now;
    prevViewProj_ = vp;
    std::copy(c.pos, c.pos + 3, prevPos_);
    havePrev_ = true;
    cam_ = c;
}

u32 Upscalers::jitterPhases(u32 renderW, u32 outputW) const {
    // FSR / XeSS / DLSS guidance: a Halton(2,3) sequence of about 8 * (output / render)^2 samples per cycle.
    const float ratio = float(outputW) / float(std::max(renderW, 1u));
    return std::max(8u, u32(std::ceil(8.0f * ratio * ratio)));
}

void Upscalers::setMenuActive(bool menu) {
    if (!fg_ || menu == menuPaused_) return;
    menuPaused_ = menu;
    fg_->setFrameGen(!menu, settings_.frameGenMultiplier);
}

bool Upscalers::uiComposedBySwapChain() const { return fg_ && fg_->composesUi(); }

bool Upscalers::ensureHudless() {
    if (hudless_ && hudlessW_ == r.width_ && hudlessH_ == r.height_) return true;
    hudless_.Reset();
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = r.width_, d.Height = r.height_, d.DepthOrArraySize = 1, d.MipLevels = 1;
    d.Format = Renderer::kSwapChainFormat;
    d.SampleDesc.Count = 1;
    if (FAILED(r.device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                                  nullptr, IID_PPV_ARGS(&hudless_))))
        return false;
    hudless_->SetName(L"upscalers hudless");
    hudlessW_ = r.width_, hudlessH_ = r.height_;
    return true;
}

void Upscalers::beforeUiComposite(ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer) {
    if (!fg_ || !ensureHudless()) return;
    D3D12_RESOURCE_BARRIER b[2]{};
    for (auto& x : b) x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition = {backbuffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    b[1].Transition = {hudless_.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(2, b);
    list->CopyResource(hudless_.Get(), backbuffer);
    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    list->ResourceBarrier(2, b);
}

void Upscalers::endFrame(ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer) {
    if (!fg_ || !r.shading_ || !hudless_) return;
    FrameGenInputs in;
    in.list = list;
    in.depth = r.shading_->sceneDepth_.Get(), in.depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    in.motion = r.shading_->motion_.Get(), in.motionState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    if (r.packs_ && r.packs_->active()) {  // a shader pack (render/packs, upscaling path): its depth and motion
        if (!r.packs_->frameGenInputs(in.depth, in.motion)) return;
        in.depthState = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    }
    in.hudless = hudless_.Get(), in.hudlessState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    in.ui = r.shading_->ui_.Get(), in.uiState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    in.backbuffer = backbuffer;
    in.cam = &cam_;
    in.frameId = frameId_;
    fg_->tagFrame(in);
}

void Upscalers::present(IDXGISwapChain3* swapchain, bool vsync, bool tearing) {
    for (Backend* b : {dlss_.get(), fsr_.get(), xess_.get()}) b->marker(Backend::PresentStart, frameId_);
    if (fg_) fg_->beforePresent(frameId_);
    swapchain->Present(vsync ? 1 : 0, (!vsync && tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
    for (Backend* b : {dlss_.get(), fsr_.get(), xess_.get()}) b->marker(Backend::PresentEnd, frameId_);
    framesPresented_ = 1;
    if (fg_) fg_->afterPresent(frameId_, framesPresented_);
    presentedTotal_ += framesPresented_;
}

// ---- reactive mask (shared by the SDK upscalers) --------------------------------------------------------------

ID3D12Resource* Upscalers::reactiveMask(const UpscaleInputs& in, float scale) {
    if (!reactivePso_ || !in.reactive) return nullptr;
    const u32 w = in.renderSize[0], h = in.renderSize[1];
    if (!reactive_ || reactiveW_ != w || reactiveH_ != h) {
        reactive_.Reset();
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w, d.Height = h, d.DepthOrArraySize = 1, d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R8_UNORM;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(r.device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                                      nullptr, IID_PPV_ARGS(&reactive_))))
            return nullptr;
        reactive_->SetName(L"upscalers reactive mask");
        reactiveW_ = w, reactiveH_ = h;
        if (reactiveDesc_ == UINT32_MAX) reactiveDesc_ = r.allocDescriptors(2);
        if (reactiveDesc_ == UINT32_MAX) return nullptr;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        r.device_->CreateShaderResourceView(in.reactive, &sv, r.cpuDescriptor(reactiveDesc_));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R8_UNORM;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        r.device_->CreateUnorderedAccessView(reactive_.Get(), nullptr, &uv, r.cpuDescriptor(reactiveDesc_ + 1));
        reactiveSrc_ = in.reactive;
    }
    if (reactiveSrc_ != in.reactive) {  // the pipeline re-created its targets at the same size
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        r.device_->CreateShaderResourceView(in.reactive, &sv, r.cpuDescriptor(reactiveDesc_));
        reactiveSrc_ = in.reactive;
    }
    ID3D12GraphicsCommandList* list = in.list;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {reactive_.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    list->ResourceBarrier(1, &b);
    ID3D12DescriptorHeap* heaps[] = {r.srvHeap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(reactiveRoot_.Get());
    list->SetPipelineState(reactivePso_.Get());
    list->SetComputeRootDescriptorTable(0, r.gpuDescriptor(reactiveDesc_));
    list->SetComputeRootDescriptorTable(1, r.gpuDescriptor(reactiveDesc_ + 1));
    u32 consts[4] = {w, h};
    std::memcpy(&consts[2], &scale, 4);
    list->SetComputeRoot32BitConstants(2, 4, consts, 0);
    list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    return reactive_.Get();
}

// ---- small accessors for the backends --------------------------------------------------------------------

ID3D12Device* Upscalers::device() const { return r.device_.Get(); }
ID3D12CommandQueue* Upscalers::queue() const { return r.queue_.Get(); }
IDXGIFactory6* Upscalers::factory() const { return r.factory_.Get(); }

void Upscalers::resizeSwapChain(IDXGISwapChain3* swapchain, u32 width, u32 height, DXGI_FORMAT format, UINT flags) {
    // DLSS-G: frame generation must be off while the swap chain changes (deadlock avoidance, SL guide 12.0).
    if (fg_) fg_->setFrameGen(false, 1);
    swapchain->ResizeBuffers(Renderer::kFrames, width, height, format, flags);
    if (fg_ && !menuPaused_) fg_->setFrameGen(true, settings_.frameGenMultiplier);
    havePrev_ = false;
}
