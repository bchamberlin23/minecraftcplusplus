#pragma once
// Built-in shader pipeline (internal to the renderer). Frame structure with shaders on:
//
//   cull (camera) + shadow cull (cascades)            compute, GPU-driven like the main view
//   shadow maps         D32 Texture2DArray, one slice per cascade, light-facing faces culled for opaque cubes
//   sky stage           vanilla sky into LDR layer A (R8G8B8A8, sRGB values)
//   G-buffer            opaque / cutout / model terrain: albedo (sRGB) + ao/material, normal + light levels
//   World stage         entities into LDR layer B (pipeline-aware colours, see entity.hlsl)
//   deferred            compute: lighting, shadows, sky -> HDR, aerial perspective -> scene (RGBA16F) + copy
//   translucent         water (SSR, refraction, absorption) and glass, forward into the scene
//   AfterTranslucent    clouds / weather into LDR layer A (cleared), premultiplied
//   volumetric          compute, 1/2 or 1/4 resolution ray march through the shadow map
//   composite           compute: volumetrics, underwater fog, late layer
//   TAA                 compute: jittered scene + reprojected history -> history (RGBA16F, ping-pong)
//   bloom + exposure    compute: 6-level chain, eye adaptation
//   tonemap             exposure, bloom, sharpening, AgX -> swap chain; then overlays and the Hud stage
#include "core/math.h"
#include "render/renderer.h"
#include "render/upscaler.h"

struct Renderer::Shading {
    // Texture / UAV slots of the descriptor tables (shading.hlsli kTex* / kRw*).
    enum Tex : u32 { TexDepth, TexAlbedo, TexNormal, TexLdrSky, TexLdrFwd, TexScene, TexSceneCopy, TexLinDepth, TexVol,
                     TexHist0, TexHist1, TexAo, TexBloom0, TexMotion = 18, TexExposure, TexUi, TexEntNormal, TexCount = 24 };
    enum Rw : u32 { RwScene, RwSceneCopy, RwLinDepth, RwVol, RwHist0, RwHist1, RwAo, RwBloom0, RwMotion = 13, RwExposure,
                    RwCount = 24 };
    static constexpr u32 kBloomLevels = 6;
    static constexpr u32 kShadowSegCap = 98304;  // indirect commands per (cascade, layer)
    static constexpr u32 kMaxMarks = 24;
    static constexpr u32 kConstantBytes = 1024;

    Renderer& r;
    explicit Shading(Renderer& renderer) : r(renderer), taa_(*this) {}

    // The built-in temporal upscaler (TAA at render scale 1).
    struct Taa : Upscaler {
        Shading& s;
        explicit Taa(Shading& shading) : s(shading) {}
        const char* name() const override { return "TAA"; }
        void evaluate(const UpscaleInputs& in) override;
    };
    Taa taa_;

    bool create();                       // device objects that do not depend on the size or settings
    bool createTargets();                // size-dependent targets (also on resize)
    bool createShadowMap();              // settings-dependent
    void setMaterials(const std::vector<std::string>& names);

    // Per frame.
    struct FrameInput {
        const CameraState* cam;
        float viewDistance;
        Mat4 vp, vpNoJitter;
        Vec3 fwd, right, up;
        float jitter[2];
        u32 frameIndex;  // ctx.frame slot
        // Columns of the inverse view rotation (camera-relative position per view-space x, y, z): right, up and fwd
        // unless a warp (CameraState::warp) skews the view.
        Vec3 rayRight, rayUp, rayFwd;
    };
    void prepare(const FrameInput& in);  // constants for this frame
    void fillPassContext(PassContext& pc, const FrameInput& in) const;  // jitter, motion, shadow casters
    void record(ID3D12GraphicsCommandList* list, const FrameInput& in, bool haveWorld, D3D12_GPU_VIRTUAL_ADDRESS cb,
                D3D12_GPU_VIRTUAL_ADDRESS activeVa, u32 activeCount, PassContext& pc, ID3D12Resource* backbuffer,
                D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv);
    void compositeUi(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS cb, D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv);
    bool uiComposite();
    u32 renderSize(u32 output) const;
    void readTimings(u32 frame);
    void logTimings() const;

    // Helpers.
    void mark(ID3D12GraphicsCommandList* list, const char* name);
    void barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
    void uavBarrier(ID3D12Resource* res = nullptr);
    void flushBarriers(ID3D12GraphicsCommandList* list);
    void bindCompute(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS cb);
    void bindGraphics(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS cb);
    void passConstants(ID3D12GraphicsCommandList* list, bool compute, const u32 (&v)[8]);
    ComPtr<ID3D12Resource> texture(DXGI_FORMAT format, u32 w, u32 h, u16 slices, D3D12_RESOURCE_FLAGS flags,
                                   D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear);
    void srv(u32 slot, ID3D12Resource* res, DXGI_FORMAT format);
    void uav(u32 slot, ID3D12Resource* res, DXGI_FORMAT format);

    // Device objects.
    ComPtr<ID3D12PipelineState> psoGbuf_, psoGbufCutout_, psoGbufModel_, psoShadow_, psoShadowCutout_, psoShadowModel_,
        psoFwd_, psoTonemap_;
    ComPtr<ID3D12PipelineState> psoShadowCull_, psoDeferred_, psoVolumetric_, psoComposite_, psoTaa_, psoBloomDown_,
        psoBloomUp_, psoExposure_, psoAo_, psoMotion_, psoDepthUp_, psoUiComposite_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_, dsvHeap_;
    u32 rtvStride_ = 0, dsvStride_ = 0;
    u32 descBase_ = UINT32_MAX;  // [TexCount SRVs][shadow map][materials][RwCount UAVs][state buffer]
    ComPtr<ID3D12Resource> constants_;  // upload, kConstantBytes per frame
    u8* constantsMapped_ = nullptr;
    ComPtr<ID3D12Resource> materials_, state_, shadowArgs_, shadowCounts_, shadowCountsZero_;
    ComPtr<ID3D12QueryHeap> queries_;
    ComPtr<ID3D12Resource> queryReadback_;
    const char* markNames_[Renderer::kFrames][kMaxMarks] = {};
    u32 markCount_[Renderer::kFrames] = {};

    // Targets. width_ / height_ = render (internal) resolution; outW_ / outH_ = swap chain.
    u32 width_ = 0, height_ = 0, renderW_ = 0, renderH_ = 0, outW_ = 0, outH_ = 0;
    ComPtr<ID3D12Resource> sceneDepth_, motion_, exposureTex_, ui_;
    ComPtr<ID3D12Resource> albedo_, normal_, ldrSky_, ldrFwd_, entNormal_, scene_, sceneCopy_, linDepth_, vol_, hist_[2], ao_;
    ComPtr<ID3D12Resource> bloom_[kBloomLevels];
    u32 bloomW_[kBloomLevels] = {}, bloomH_[kBloomLevels] = {};
    ComPtr<ID3D12Resource> shadowMap_;
    u32 shadowRes_ = 0, shadowSlices_ = 0;
    bool shadowFresh_ = true;  // all cascades must render (new map)

    // Per-cascade light-space definition, world anchored (re-expressed camera-relative every frame).
    struct Cascade {
        double axis[3][3] = {};  // right, up, forward (away from the light)
        double center[3] = {};   // snapped light-space centre: x, y and the near plane along forward
        double radius = 1, depth = 1;
        bool valid = false;
    };
    Cascade cascades_[4];
    float shadowMat_[4][16] = {};
    u32 updateMask_ = 0;
    float lightDir_[3] = {0, 1, 0};

    // Temporal state.
    u32 frameCounter_ = 0;
    u32 histSlot_ = 0;
    bool historyValid_ = false;
    double prevCam_[3] = {};
    Mat4 prevVpNoJitter_;
    float prevViewProj_[16] = {};  // this frame's camera-relative position -> previous clip (ShadeConstants::prevViewProj)
    double startTime_ = 0, lastTime_ = 0;
    float dt_ = 0;
    bool exposureReset_ = true;

    // Barrier batch.
    D3D12_RESOURCE_BARRIER barriers_[24];
    u32 barrierCount_ = 0;

    // Timing statistics: rolling per-pass samples for medians at exit.
    static constexpr u32 kTimingHistory = 256;
    std::vector<float> timingSamples_[RenderStats::kMaxPasses];
    const char* timingNames_[RenderStats::kMaxPasses] = {};
    u32 timingFrames_ = 0;
    double cpuMs_ = 0, passCpuMs_ = 0;  // prepare + record, summed; the extension passes' share of it
    u64 cpuFrames_ = 0;
};
