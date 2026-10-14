#pragma once
// Path tracer ("Shaders: Path Traced"), internal to the renderer. It extends the built-in shader pipeline (shading.h):
// the G-buffer still gives the primary surfaces (hybrid), and the world within the traced radius is lit by inline ray
// queries (DXR 1.1) against acceleration structures of the full-detail sections; what lies beyond stays rasterized
// and lit by the pipeline as usual.
//
//   BLAS   one per full-detail section in the radius, built on the GPU from its cube and model quads (BlasGenCS
//          expands them to triangles), PREFER_FAST_TRACE + compaction (the compacted copy replaces it a few frames
//          later); rebuilt only when the section's mesh changes (a new renderer slot), freed when it leaves the radius.
//   TLAS   every frame over the sections in the radius, camera-relative translations (precision far from the origin).
//
// Memory: BLASes live in big pooled buffers (RangeAllocator, 256-byte units); frees wait until the GPU is done.
#include "render/shading.h"

struct Renderer::PathTracer {
    static constexpr u32 kTableSize = 24;          // SRVs / UAVs per path tracer table (space4)
    static constexpr u32 kMaxSections = 16384;     // sections in the acceleration structure at once
    static constexpr u32 kMaxJobs = 128;           // BLAS builds per frame
    static constexpr u32 kQuadsPerFrame = 600000;  // quads expanded and built per frame (startup: they queue)
    static constexpr u64 kPoolBytes = u64(256) << 20;
    static constexpr u32 kConstantBytes = 256;
    // Render-resolution targets (pathtrace.hlsl gPtTex / gPtRw, t8.. / u8.. space4).
    enum Target : u32 { TgtLight, TgtHist0, TgtHist1, TgtGeom0, TgtGeom1, TgtNormalRough, TgtDiffAlbedo, TgtSpecAlbedo,
                        TgtCount };
    static constexpr u32 kTargetBase = 8;
    static constexpr u32 kReservoirBase = 16;       // two R32G32B32A32_UINT reservoir targets (u16, u17)
    // Emissive lights: records per section (pt index), this frame's list, and how near a section must be to count.
    static constexpr u32 kSectionLights = 256;
    static constexpr u32 kFrameLights = 1 << 16;
    static constexpr float kLightRange = 96.0f;

    Renderer& r;
    explicit PathTracer(Renderer& renderer) : r(renderer) {}
    ~PathTracer();
    static bool supported(ID3D12Device* device, std::string* why);

    bool create();         // pipelines, buffers, descriptors
    bool createTargets();  // render-resolution targets (after the pipeline's own)
    void meshAdded(u32 slot);
    void meshRemoved(u32 slot);

    // Per frame, from Shading::record: BLAS builds + TLAS first, the lighting after the deferred pass.
    void build(ID3D12GraphicsCommandList* list, const Shading::FrameInput& in, D3D12_GPU_VIRTUAL_ADDRESS cb, u32 activeCount);
    void trace(ID3D12GraphicsCommandList* list, const Shading::FrameInput& in, D3D12_GPU_VIRTUAL_ADDRESS cb);
    // DLSS Ray Reconstruction replaces the built-in denoiser and the upscaler (nullptr: not available / not selected).
    Upscaler* rayReconstruction() const;
    // Its guide buffers (render resolution, UNORDERED_ACCESS).
    void fillGuides(UpscaleInputs& u) const;
    // Translucent terrain with ray traced reflections: binds what TerrainFwdPtPS reads, returns its pipeline.
    ID3D12PipelineState* bindTranslucent(ID3D12GraphicsCommandList* list, u32 frame);
    std::string status() const;

    // ---- acceleration structure memory ----
    struct AsAlloc {
        u32 pool = UINT32_MAX;
        u64 offset = 0, size = 0;  // bytes (256-byte aligned)
        bool valid() const { return pool != UINT32_MAX; }
    };
    struct Pool {
        ComPtr<ID3D12Resource> buffer;
        RangeAllocator alloc;  // 256-byte units
    };
    AsAlloc allocAs(u64 bytes);
    void freeAs(AsAlloc& a, bool recording);  // recording: freed during this frame's recording
    D3D12_GPU_VIRTUAL_ADDRESS asVa(const AsAlloc& a) const { return pools_[a.pool].buffer->GetGPUVirtualAddress() + a.offset; }
    void retire(ComPtr<ID3D12Resource>& res);  // released once the GPU no longer uses it
    void collect(u32 frame);                  // releases what waited for frame slot `frame`

    // A section with an acceleration structure (index = "pt index": light tables, reservoirs).
    struct Entry {
        u32 slot = UINT32_MAX;
        u32 gen = 0;           // bumps when the entry is reused (stale compaction results are ignored)
        AsAlloc blas;          // the BLAS in use (compacted once known)
        u32 kindMap = 0;       // geometry index -> kind, 2 bits each
        u64 lastSeen = 0;      // frame counter when last inside the radius
        bool compacted = false;
    };
    std::vector<Entry> entries_;
    std::vector<u32> freeEntries_;
    std::vector<u32> slotEntry_;  // renderer slot -> entry (UINT32_MAX: none)
    struct Compaction {
        u32 entry, gen, job;
    };
    std::vector<Compaction> compactions_[Renderer::kFrames];  // builds whose compacted size arrives with that frame
    struct Freed {
        u64 fence;  // 0: wait for the frame slot below
        u32 frame;
        AsAlloc a;
    };
    std::vector<Freed> freed_;
    std::vector<std::pair<u64, ComPtr<ID3D12Resource>>> graveyard_;
    void dropEntry(u32 e, bool recording);

    ComPtr<ID3D12Device5> dev5_;
    ComPtr<ID3D12PipelineState> psoBlasGen_, psoTrace_, psoAccum_, psoClear_, psoGather_, psoCdf_, psoCompose_;
    bool rrUsed_ = false;  // DLSS-RR denoised the last frame
    // Entities (RenderPass::rtEntities): one BLAS rebuilt every frame from the posed triangles.
    static constexpr u32 kEntityQuads = 1 << 18;
    bool buildEntities(ID3D12GraphicsCommandList4* list, u32 frame, D3D12_GPU_VIRTUAL_ADDRESS cb, D3D12_GPU_VIRTUAL_ADDRESS ptCb);
    ComPtr<ID3D12PipelineState> psoEntity_;
    ComPtr<ID3D12Resource> entityVerts_, entityBlas_, entityScratch_;
    u64 entityBlasBytes_ = 0, entityScratchBytes_ = 0;
    ID3D12Resource *entQuadsSrc_ = nullptr, *entFrameSrc_ = nullptr, *entAtlasSrc_ = nullptr;
    u32 entityQuads_ = 0;
    ComPtr<ID3D12PipelineState> psoFwd_;
    ComPtr<ID3D12Resource> hitDist_;  // R32_FLOAT specular hit distance (u19)
    ComPtr<ID3D12Resource> targets_[TgtCount], reservoirs_[2];
    ComPtr<ID3D12Resource> lights_, lightCounts_, frameLights_, frameLightCount_, lightSections_, lightPower_, frameSecs_,
        frameSecCdf_, lightSlot_;
    u8* lightSectionsMapped_ = nullptr;
    u32 lightSectionCount_ = 0;
    u32 histSlot_ = 0;
    bool histValid_ = false;
    double prevCam_[3] = {};
    std::vector<Pool> pools_;
    ComPtr<ID3D12Resource> jobs_, instances_, constants_;  // upload, one slice per frame
    u8 *jobsMapped_ = nullptr, *instancesMapped_ = nullptr, *constantsMapped_ = nullptr;
    ComPtr<ID3D12Resource> vertices_, scratch_, tlasScratch_, postbuild_, postbuildReadback_;
    u64 vertexBytes_ = 0, scratchBytes_ = 0, tlasScratchBytes_ = 0;
    ComPtr<ID3D12Resource> tlas_[Renderer::kFrames];
    u64 tlasBytes_[Renderer::kFrames] = {};
    u32 descBase_ = UINT32_MAX;  // [kTableSize SRVs][kTableSize UAVs]
    u64 frame_ = 0;
    u32 curFrame_ = 0;  // frame slot being recorded
    u32 instanceCount_ = 0, builtThisFrame_ = 0, queued_ = 0;
    u64 asBytes_ = 0;  // live BLAS bytes
    bool haveTlas_ = false;
    float radius_ = 0;
    u32 debug_ = 0;
    std::vector<std::pair<float, u32>> inRadius_;  // (distance, slot) of the full-detail sections in the radius
    struct Level0 {
        i32 x, y, z;
        u32 slot;
    };
    std::vector<Level0> level0_;  // every full-detail mesh (dense: the per-frame scan stays in cache)
    std::vector<u32> level0Index_;
    bool level0Dirty_ = true;
    u64 scanFrame_ = 0;
    double scanCam_[3] = {};
    float scanRadius_ = 0;
    double cpuMs_ = 0, partMs_[3] = {};
};
