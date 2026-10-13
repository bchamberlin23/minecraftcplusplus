#pragma once
#include "core/common.h"
#include "render/range_allocator.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>

struct SectionMesh;
class Upscaler;
class Upscalers;
using Microsoft::WRL::ComPtr;

struct CameraState {
    double x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0;  // radians, Minecraft convention (yaw 0 = +Z, pitch > 0 = down)
    float fovY = 1.2f;
    // Optional view basis replacing the one from yaw / pitch (world-space right, up, forward): the view bobbing and
    // hurt tilt (GameRenderer.bobView / bobHurt) roll and nod the world view; yaw / pitch stay the unbobbed look.
    bool basisSet = false;
    float right[3] = {}, up[3] = {}, fwd[3] = {};
    // Nausea / nether portal warp (GameRenderer.renderLevel): a linear map of view space (+x right, +y up, +z forward;
    // row-major 3x3) applied before the projection of the world (not the first-person hand).
    bool warpSet = false;
    float warp[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

struct LineVertex {
    float x, y, z;  // camera-relative world position, or NDC xy for screen lines
    u32 color;      // 0xAABBGGRR
    float wx = 0, wy = 0;  // world lines: line width / screen width and height (rendertype_lines LineWidth / ScreenSize)
};
struct SpriteVertex {
    float x, y, u, v;  // NDC position, texture coordinates
    u32 layer;         // texture layer, 0xFFFFFFFF = solid colour
    u32 color;         // 0xAABBGGRR
};

// Drawing that lives outside the terrain system (entities, UI) plugs in as passes. Each frame they record
// into the frame's command list with the back buffer and depth bound and the full-screen viewport set.
enum class PassStage;
struct PassContext {
    ID3D12GraphicsCommandList* list;
    u32 frame;                  // 0..Renderer::kFrames-1: which per-frame slice of an upload buffer is free
    u32 width, height;
    const float* viewProj;      // 4x4 row-major; multiplies (world position - camera position, 1) as a column
    double camX, camY, camZ;    // camera position
    float fogColor[4];          // linear 0..1
    float fogStart, fogEnd;     // blocks from the camera
    float daylight;             // 0..1 sky light scale
    D3D12_GPU_DESCRIPTOR_HANDLE blockTextures;  // Texture2DArray of 16x16 block textures (BlockInfo layers)
    // Shader pipeline on: World / AfterTranslucent / Sky passes draw into LDR layers (same R8G8B8A8_UNORM format
    // as the swap chain) that the pipeline converts to HDR; World passes may then output pipeline-aware colours
    // (see entity.hlsl). Hud passes always draw onto the final (tonemapped) image.
    bool shading = false;
    // StructuredBuffer<uint>: block texture layer -> layer of its current animation frame (see setTextureRemap).
    D3D12_GPU_VIRTUAL_ADDRESS textureRemap = 0;
    float lightmap[3][4] = {};  // Lightmap::pack: for passes that light with the game's lightmap (lightmap.hlsli)
    PassStage stage{};  // the stage being drawn (a pass may be registered for several)
    bool uiLayer = false;  // Hud passes draw into a separate UI layer (composited later), not onto the scene
    // An OptiFine / Iris shader pack is active (render/packs): passes that support it hand vanilla-style geometry to
    // the pack's gbuffers programs (render/pack_sink.h); others draw as usual into a layer the pack merges.
    class PackSink* pack = nullptr;

    // ---- shader pipeline extras (zero / null otherwise) ----
    // Projection jitter of this frame (NDC offsets added to the projection's x / y): passes with their own
    // projection (the first-person hand) apply it too so the temporal passes see consistent samples.
    float jitter[2] = {};
    // Motion vectors (World and AfterTranslucent stages). Geometry that moves on its own (entities, particles, the
    // hand, animated block entities) writes per-pixel motion: such passes bind {colorRtv, motionRtv} with dsv
    // themselves, use PSOs with a second R16G16_FLOAT target (kMotionFormat) and write prevUv - curUv (UV units,
    // +y down) with curUv = SV_Position.xy * motionScale.xy + motionScale.zw and prevUv from prevViewProj; they rebind
    // {colorRtv} before returning. Pixels nobody writes get camera motion. motionRtv.ptr == 0: not available.
    D3D12_CPU_DESCRIPTOR_HANDLE colorRtv{}, motionRtv{}, dsv{};
    const float* prevViewProj = nullptr;  // camera-relative position (this frame's camera) -> previous clip, unjittered
    float motionScale[4] = {};
    // Shadow casters (pipeline): the cascades drawn this frame (bit c of shadowMask) and their camera-relative
    // light matrices (column-major like viewProj; clip x, y in -1..1, z 0..1 from the light). Passes that cast
    // shadows keep geometry for casters inside them (see RenderPass::drawShadow).
    u32 shadowCascades = 0, shadowMask = 0;
    const float (*shadowMats)[16] = nullptr;
    const float* viewProjNoJitter = nullptr;  // viewProj without the temporal jitter (passes drawn after TAA / upscaling)
    // Hud stage: the finished scene (the back buffer, R8G8B8A8, in RENDER_TARGET state) for passes that read it.
    ID3D12Resource* sceneColor = nullptr;
};
// One shadow cascade being drawn: depth only (D32, LESS_EQUAL, no depth clip), viewport / DSV bound.
struct ShadowContext {
    ID3D12GraphicsCommandList* list;
    u32 frame;                    // like PassContext::frame
    u32 cascade;
    const float* matrix;          // PassContext::shadowMats[cascade]
    D3D12_GPU_DESCRIPTOR_HANDLE blockTextures;
    D3D12_GPU_VIRTUAL_ADDRESS textureRemap;
};
// Posed entity geometry of this frame for the path tracer (Shaders: Path Traced), in entity.hlsl's layouts: model
// quads (static, 96 bytes: pos[4] in model pixels, uv[4], normal, part) referenced by (quad, instance) pairs whose
// instance (32 bytes, matrixBase first) selects the part matrices (3 float4 rows: part space -> camera-relative
// blocks), and free quads (128 bytes, camera-relative pos[4] first). Offsets are bytes into `frameData`.
struct RtEntities {
    ID3D12Resource* modelQuads = nullptr;
    ID3D12Resource* frameData = nullptr;
    u64 matrices = 0, instances = 0, refs = 0, freeQuads = 0;
    u32 refCount = 0, freeCount = 0;
};
class RenderPass {
public:
    static constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT;
    virtual ~RenderPass() = default;
    virtual bool create(class Renderer& renderer) = 0;  // once, from Renderer::addPass
    virtual void draw(const PassContext& ctx) = 0;       // pass-owned root signature / PSO / heaps as needed
    // Once per frame before anything is recorded (not with a shader pack), in stage order (Sky passes first):
    // passes that cast shadows build this frame's geometry here so drawShadow and draw reuse it.
    virtual void prepare(const PassContext&) {}
    // Pipeline shadow maps, once per updated cascade, after prepare (World-stage passes only).
    virtual void drawShadow(const ShadowContext&) {}
    // Path tracing: this frame's posed geometry after prepare (World-stage passes); false = none.
    virtual bool rtEntities(u32 /*frame*/, RtEntities&) { return false; }
};
enum class PassStage {
    Sky,    // first: after the clear, before any terrain (depth test/write off is up to the pass)
    World,  // after opaque, cutout and model terrain, before translucent terrain (depth test + write)
    Hud,    // after the built-in overlays (crosshair, hotbar)
    AfterTranslucent,  // after translucent terrain, before the overlays (clouds, weather; depth test)
    AfterUi,           // onto the final back buffer after the UI layer is composited (blends that read the scene,
                       // like the inverting crosshair); with the vanilla path it follows Hud directly
};

// Built-in shader pipeline ("shaders"): HDR + AgX tonemap, cascaded sun / moon shadows, lit water with
// reflections, volumetric light, bloom, eye adaptation, TAA. Off = the vanilla look, unchanged.
// PathTraced: the High pipeline with the world near the camera path traced (render/pathtrace.h).
enum class ShaderPreset : u8 { Off, Low, Medium, High, Ultra, PathTraced };
// Path tracing quality presets (Shaders: Path Traced): bounces, samples per pixel, radius.
enum class PathTraceQuality : u8 { Low, Medium, High, Ultra, Count };
struct ShaderSettings {
    u32 cascades = 3;              // shadow cascades (0..4)
    u32 shadowRes = 2048;          // per cascade
    float shadowSplits[4] = {16, 56, 160, 0};  // far distance of each cascade (blocks)
    u32 shadowTaps = 8;            // PCF taps (4, 8, 16)
    bool pcss = false;             // contact-hardening soft shadows
    u32 volumetricSteps = 16;      // 0 = no volumetric light
    u32 volumetricShift = 2;       // 1 = half, 2 = quarter resolution (bilateral upsample + TAA)
    u32 ssrSteps = 24;             // water screen-space reflection steps (0 = sky only)
    bool waving = true;            // waving leaves and plants
    bool ssao = false;             // screen-space ambient occlusion on top of the baked AO
    float bloom = 0.25f;           // strength of the (thresholded) bloom
    float sharpen = 0.35f;
    float exposure = 1.0f;         // exposure compensation
    float renderScale = 1.0f;      // internal resolution / output resolution (0.33..1), upscaled temporally
    float sunPathTilt = 0.6f;      // radians the sun / moon path is tilted towards the south (packs: ~30-40 deg)
    // Path tracing (preset PathTraced): sections within ptRadius blocks are ray traced (render/pathtrace.h).
    bool pathTrace = false;
    u32 ptBounces = 1;             // diffuse bounces after the primary surface (1..2)
    u32 ptSamples = 1;             // paths per pixel
    float ptRadius = 256;          // blocks (clamped to the full-detail render distance)
    static ShaderSettings preset(ShaderPreset p);
    void setPathTraceQuality(PathTraceQuality q);
};
// Sky state the pipeline lights with; set each frame by the sky system.
// Inputs of the game's lightmap (shaders/lightmap.hlsli), from the dimension's environment attributes.
struct Lightmap {
    float ambient[3] = {10 / 255.0f, 10 / 255.0f, 10 / 255.0f};  // visual/ambient_light_color
    float brightness = 0.5f;                                     // the brightness (gamma) option
    float skyColor[3] = {1, 1, 1};                               // visual/sky_light_color
    float skyFactor = 1.0f;                                      // visual/sky_light_factor
    float blockTint[3] = {1.0f, 216 / 255.0f, 140 / 255.0f};      // visual/block_light_tint
    float blockFactor = 1.4f;                                    // 1.4 + flicker
    void pack(float out[3][4]) const {
        for (int c = 0; c < 3; ++c) out[0][c] = ambient[c], out[1][c] = skyColor[c], out[2][c] = blockTint[c];
        out[0][3] = brightness, out[1][3] = skyFactor, out[2][3] = blockFactor;
    }
};

// The camera entity's mob effects as the view shows them (FogRenderer's fog environments, the lightmap inputs),
// gathered by the UI from the local player each frame and applied by the sky system.
struct ViewEffects {
    float blindFog = -1;      // BlindnessFogEnvironment: min(1, duration / 20) (1: infinite); < 0 without blindness
    float blindDarkness = 0;  // BlindnessFogEnvironment.getModifiedDarkness (fog colour darkening)
    bool darkness = false;    // has the darkness effect
    float darknessBlend = 0;  // its blend factor (fade in / out)
    float darknessScale = 0;  // LightmapRenderStateExtractor.calculateDarknessScale (the pulse)
    float darknessEffectScale = 1;  // the Darkness Pulsing option
    float fogBrighten = 0;    // night vision scale when it brightens the fog (not with darkness)
    float lightmapNightVision = 0;  // night vision scale, or the water vision with conduit power
    float waterVision = 0;    // LocalPlayer.getWaterVision
    bool fireResistance = false, spectator = false;
};

struct SkyLighting {
    float sunAngle = 0, moonAngle = 0;  // the game's celestial angles (radians; 0 = noon / midnight overhead)
    float skyColor[3] = {0.47f, 0.65f, 1.0f};  // vanilla sky colour (sRGB 0..1)
    float rain = 0, thunder = 0;
    bool overworld = true;   // sun / moon / weather exist
    bool cameraInWater = false;
    bool skyHidden = false;  // a fog environment hides the sky (lava, powder snow, blindness, darkness, boss fog)
    float dim = 1.0f;        // the darkness effect's pulse as a light scale (1: none)
};

struct RenderStats {
    float gpuMs = 0;
    u32 commands[4] = {};  // indirect draws issued per pass: opaque, cutout, translucent, model
    // Shader pipeline: per-pass GPU time (ms, last completed frame) and the pass names.
    static constexpr int kMaxPasses = 20;
    u32 passCount = 0;
    const char* passNames[kMaxPasses] = {};
    float passMs[kMaxPasses] = {};
};

// GPU-driven D3D12 renderer. Meshes live in big GPU arenas addressed by slots; each frame
// the caller supplies the list of active slots (the LOD cut), one compute dispatch culls
// them and writes ExecuteIndirect commands, and four indirect calls draw the world.
class Renderer {
public:
    static constexpr u32 kNoSlot = 0xFFFFFFFF;
    static constexpr u32 kFrames = 3;

    struct Shading;  // shader pipeline state (opaque; render/shading.h)
    Renderer();
    ~Renderer();
    bool init(HWND hwnd, u32 width, u32 height, bool debug, bool vsync);
    void resize(u32 width, u32 height);
    bool uploadTextures(const std::vector<u8>& pixels, u32 layers);

    // World storage: capacities in quads / mesh slots.
    bool createWorldStorage(u64 cubeQuads, u64 modelQuads, u32 slots);
    // Replaces a world storage buffer with a larger copy (GPU copy, waits for the GPU; pending uploads follow).
    bool growBuffer(ComPtr<ID3D12Resource>& buffer, u64 oldBytes, u64 newBytes, const char* what);
    u32 addMesh(const SectionMesh& mesh, int level, i32 x, i32 y, i32 z);  // kNoSlot if out of space (after growing)
    void removeMesh(u32 slot);
    // The LOD cut's slots. Sections entering or leaving it cross-fade (dithered) over fadeSeconds; a section replaced
    // by one at the same place and level (an edit, new light) swaps at once.
    void setActive(const std::vector<u32>& slots);
    void setFadeSeconds(float seconds) { fadeSeconds_ = std::max(seconds, 0.0f); }
    void beginWorldReveal(double x, double z);  // stagger entrance fades outward on join/large teleport
    void flushUploads();  // submit staged copies now (used while loading)

    // HUD and world overlays for the next frame.
    void setOverlay(std::vector<LineVertex> worldLines, std::vector<LineVertex> screenLines,
                    std::vector<SpriteVertex> sprites) {
        worldLines_ = std::move(worldLines), screenLines_ = std::move(screenLines), sprites_ = std::move(sprites);
    }
    void render(const CameraState& cam, float viewDistance, const fs::path* screenshot);
    // Atmosphere for the next frames: fog / clear colour (linear 0..1) and the daylight factor that
    // scales sky light in the terrain shaders (1 = noon). Set by the sky system.
    void setAtmosphere(const float fogColor[4], float daylight) {
        std::copy(fogColor, fogColor + 4, fogColor_);
        daylight_ = daylight;
    }
    void setLightmap(const Lightmap& l) { lightmap_ = l; }
    // Block texture options: Texture Filtering (0 none, 1 RGSS, 2 anisotropic), Mipmap Levels (0..4), Anisotropic
    // Filtering (1..3: 2x / 4x / 8x).
    void setTextureFiltering(int mode, int mipLevels, int anisotropyBit) {
        textureFiltering_ = u32(std::clamp(mode, 0, 2)) | (u32(std::clamp(mipLevels, 0, 4)) << 2) | (u32(std::clamp(anisotropyBit, 1, 3)) << 5);
    }
    const Lightmap& lightmap() const { return lightmap_; }
    // Caps on the fog distances (blocks) this frame, e.g. a boss bar's world fog (AtmosphericFogEnvironment:
    // start 10, end 96); infinity: the view distance's own fog.
    void setFogLimit(float start, float end) { fogLimit_[0] = start, fogLimit_[1] = end; }

    // Shader pipeline. setShaderPreset can be called any time (the UI's options screen); Off restores vanilla.
    void setShaderPreset(ShaderPreset preset);
    ShaderPreset shaderPreset() const { return shaderPreset_; }
    void setShaderSettings(const ShaderSettings& settings);  // fine tuning on top of a preset (not Off)
    const ShaderSettings& shaderSettings() const { return shaderSettings_; }
    static bool parseShaderPreset(std::string_view name, ShaderPreset& out);  // off|low|medium|high|ultra|pathtraced
    static const char* shaderPresetName(ShaderPreset preset);
    void setSkyLighting(const SkyLighting& sky) { skyLighting_ = sky; }
    // Tilt the sky should apply to the sun / moon / stars so they match the shadows (0 when shaders are off).
    float sunPathTilt() const { return shaderPreset_ == ShaderPreset::Off ? 0.0f : shaderSettings_.sunPathTilt; }
    // Block texture names by array layer ("block/stone"): materials for the pipeline (foliage, emissive, water).
    void setTextureNames(const std::vector<std::string>& names);
    // Animated block textures: layer -> current frame's layer (identity for static textures). Cheap when unchanged.
    void setTextureRemap(const std::vector<u32>& remap);
    // Temporal upscaler for the shader pipeline (not owned; nullptr = the built-in TAA). See render/upscaler.h.
    void setUpscaler(Upscaler* upscaler) { upscaler_ = upscaler; }
    // Vendor upscalers / frame generation / low latency (render/upscalers/upscalers.h); created by init.
    Upscalers& upscalers() { return *upscalers_; }
    // OptiFine / Iris shader packs (API: render/packs/pack_api.h); while one is active it replaces the pipeline above.
    struct Packs;
    Packs& packs();  // created on first use
    // Path tracing (render/pathtrace.h): available on this GPU (DXR 1.1 inline ray tracing), else why not; and what it
    // is doing right now (denoiser, sections in the acceleration structure) for the options screen.
    struct PathTracer;
    bool pathTracingSupported(std::string* why = nullptr) const;
    std::string pathTracingStatus() const;
    // Main loop, once per frame before input and simulation: latency sleep (Reflex / XeLL), frame limiter, markers.
    void beginFrame();
    // History of the temporal passes is invalid (camera cut, dimension change); the next frame resets it.
    void resetTemporal();
    void setVsync(bool vsync) { vsync_ = vsync; }
    // Internal (render) resolution of the shader pipeline; the output size when shaders are off.
    u32 renderWidth() const;
    u32 renderHeight() const;

    // Extension passes (not owned; must outlive the renderer's last frame).
    bool addPass(RenderPass* pass, PassStage stage);
    ID3D12Device* device() const { return device_.Get(); }
    ID3D12DescriptorHeap* descriptorHeap() const { return srvHeap_.Get(); }  // shader-visible CBV/SRV/UAV heap
    u32 allocDescriptors(u32 count);  // indices into descriptorHeap(); UINT32_MAX when full
    D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor(u32 index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptor(u32 index) const;
    // Fills a texture created in COPY_DEST state (all subresources, D3D12 subresource order, tightly packed
    // rows), waits for the copy and leaves it in PIXEL_SHADER_RESOURCE state. Blocks; for loading time.
    bool uploadTexture(ID3D12Resource* texture, const std::vector<const u8*>& subresources);
    // Same for a DEFAULT-heap buffer created in COPY_DEST state; leaves it in `after`.
    bool uploadBuffer(ID3D12Resource* buffer, const void* data, u64 size, D3D12_RESOURCE_STATES after);
    u32 width() const { return width_; }
    u32 height() const { return height_; }

    const RenderStats& stats() const { return stats_; }
    const std::string& adapterName() const { return adapterName_; }
    u64 arenaBytesUsed() const;
    u64 arenaBytesTotal() const;

private:
    friend class Upscalers;
    static constexpr u64 kStagingBytes = u64(256) << 20;
    static constexpr u32 kMaxActive = 1 << 18;

    struct Copy {
        ID3D12Resource* dst;
        u64 dstOffset, srcOffset, size;
    };
    struct Slot {
        u64 cubeOffset = 0, cubeCount = 0, modelOffset = 0, modelCount = 0;
        i32 origin[4] = {};  // xyz in blocks, w = LOD level
        u32 translucent = 0;  // translucent quads
        u32 opaque = 0, cutout = 0;  // opaque / cutout cube quads (the path tracer's geometries)
        float fade = 0;       // GpuSection::fade
        bool inCut = false, retiring = false, freeLater = false;
    };
    void freeSlot(u32 slot);
    void setFade(u32 slot, float fade);
    float fadeNow() const;
    float fadeProgress(float fade, float now) const;
    // Active slots with translucent quads, far to near: blending needs back-to-front order between sections.
    void sortTranslucent(const CameraState& cam, u32 activeCount);

    ComPtr<ID3D12Resource> createBuffer(u64 size, D3D12_HEAP_TYPE heap,
                                        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
    // Swap chain (one place for frame generation SDKs: factory_ creates it on queue_; R8G8B8A8_UNORM, kFrames
    // buffers, flip-discard, tearing when available; back buffers fetched in createTargets).
    static constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool createSwapChain(HWND hwnd, u32 width, u32 height);
    void resizeSwapChain(u32 width, u32 height);
    void present();
    void createTargets();
    void recreateSwapChain();  // frame generation changed: new (proxy) swap chain, same size
    bool createPipelines();
    void transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
    void waitFence(u64 value);
    void waitIdle();
    void stage(ID3D12Resource* dst, u64 dstOffset, const void* src, u64 size);
    void recordCopies();
    void printDebugMessages();

    ComPtr<ID3D12InfoQueue> infoQueue_;
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapchain_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_, dsvHeap_, srvHeap_;
    u32 rtvStride_ = 0;
    ComPtr<ID3D12Resource> backbuffers_[kFrames], depth_;
    ComPtr<ID3D12CommandAllocator> allocators_[kFrames];
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    u64 frameFence_[kFrames] = {};
    u64 fenceCounter_ = 0;

    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> psoOpaque_, psoCutout_, psoTranslucent_, psoModel_, psoCull_;
    ComPtr<ID3D12PipelineState> psoWorldLines_, psoScreenLines_, psoSprites_;  // world lines: wide (6 vertices a line)
    static constexpr u64 kOverlayBytes = u64(1) << 20;  // per frame
    ComPtr<ID3D12Resource> overlay_;
    u8* overlayMapped_ = nullptr;
    std::vector<LineVertex> worldLines_, screenLines_;
    std::vector<SpriteVertex> sprites_;
    ComPtr<ID3D12CommandSignature> cmdSig_;

    ComPtr<ID3D12Resource> constants_, activeLists_;
    u8* constantsMapped_ = nullptr;
    u32* activeMapped_ = nullptr;
    ComPtr<ID3D12Resource> indexBuffer_, texture_, cubeQuads_, modelQuads_, sections_, args_, counts_, zeros_;
    ComPtr<ID3D12Resource> colors_;  // tint colour table (ColorTable), uploaded as it grows
    size_t colorsUploaded_ = 0;
    ComPtr<ID3D12Resource> countsReadback_, timestampReadback_, screenshotReadback_;
    ComPtr<ID3D12QueryHeap> timestamps_;
    u64 timestampFreq_ = 1;

    // Upload ring: copies are recorded at the start of the next frame (or flushed while loading).
    ComPtr<ID3D12Resource> staging_;
    u8* stagingMapped_ = nullptr;
    u64 stagingHead_ = 0;
    // Ring rules: only this frame's copies [frameStagingStart_, stagingHead_) (possibly wrapped) are unsubmitted;
    // stagingLimit_ = end of the free space ahead; wrapFence_ = frame to wait for before reusing the ring's start.
    u64 stagingLimit_ = kStagingBytes;
    u64 frameStagingStart_ = 0;
    u64 wrapFence_ = 0;
    std::vector<Copy> copies_;

    RangeAllocator cubeAlloc_, modelAlloc_;
    std::vector<Slot> slots_;
    std::vector<u32> freeSlots_;
    u32 slotCapacity_ = 0, slotHigh_ = 0;
    std::vector<u32> active_;
    std::vector<u32> drawList_;  // active_ + sections still fading out
    struct Retiring {
        u32 slot;
        float end;
    };
    std::vector<Retiring> retiring_;
    std::vector<u32> cutMark_;
    u32 cutStamp_ = 0;
    float fadeSeconds_ = 0.75f;  // the game's chunkSectionFadeInTime default
    u32 fadeFrame_ = 0;
    float revealStart_ = 0;
    double revealX_ = 0, revealZ_ = 0;
    float entranceTime(const Slot& slot, float now) const;
    std::vector<u32> translucent_;
    u64 activeVersion_ = 0, sortedVersion_ = ~u64(0);
    double sortedCam_[3] = {};
    u32 segBase_[4] = {}, segCap_[4] = {};

    std::vector<RenderPass*> passes_[5];
    float fogColor_[4] = {0.62f, 0.76f, 1.0f, 1.0f};
    float daylight_ = 1.0f;
    Lightmap lightmap_;
    u32 textureFiltering_ = 4u << 2 | 2u << 5;  // see setTextureFiltering
    float fogLimit_[2] = {1e30f, 1e30f};
    u32 descriptorSize_ = 0, descriptorsUsed_ = 1, descriptorCapacity_ = 0;  // 0: block textures
    void runPasses(PassStage stage, const PassContext& ctx);
    void preparePasses(const PassContext& ctx);    // RenderPass::prepare, each pass once
    void runShadowPasses(const ShadowContext& sc);  // RenderPass::drawShadow of the World-stage passes
    void submitAndWait();  // close + execute list_ (recorded from allocators_[0]) and wait

    std::unique_ptr<Shading> shading_;
    std::unique_ptr<PathTracer> pt_;  // Shaders: Path Traced only
    ShaderPreset shaderPreset_ = ShaderPreset::High;
    ShaderSettings shaderSettings_;
    SkyLighting skyLighting_;
    std::vector<std::string> textureNames_;
    Upscaler* upscaler_ = nullptr;
    static constexpr u32 kRemapCapacity = 16384;  // texture layers
    ComPtr<ID3D12Resource> remap_;                 // upload heap, one slice per frame
    u32* remapMapped_ = nullptr;
    std::vector<u32> remapCpu_;
    u32 remapVersion_ = 1, remapSliceVersion_[kFrames] = {};

    u32 width_ = 0, height_ = 0;
    bool vsync_ = false, tearing_ = false;
    RenderStats stats_;
    std::string adapterName_;
    HWND hwnd_ = nullptr;
    u32 jitterPhase_ = 0;
    std::unique_ptr<Packs> packs_;
    std::unique_ptr<Upscalers> upscalers_;  // last: destroyed first (after ~Renderer released the swap chain)
};
