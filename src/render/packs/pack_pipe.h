#pragma once
// Internal GPU state of a loaded shader pack (pack_pipeline.cpp builds it, pack_frame.cpp records with it).
#include "render/packs/pack_pipeline.h"

#include <map>
#include <unordered_map>

namespace packs {

// Root signature parameters (graphics); the compute root signature uses its own (kC*).
enum : u32 {
    kRootVsUniforms,  // CBV b0 space0 (VERTEX)
    kRootPsUniforms,  // CBV b0 space0 (PIXEL)
    kRootDraw,        // 2 constants b1: quad offset, section (ExecuteIndirect)
    kRootFrame,       // CBV b2: McppFrame (adapter.hlsli)
    kRootGeometry,    // SRV t0 space1: quads / model words / generic vertices
    kRootSections,    // SRV t1 space1
    kRootColors,      // SRV t2 space1
    kRootRemap,       // SRV t3 space1
    kRootLayers,      // SRV t4 space1
    kRootVsTextures,  // table t0.. space0 (VERTEX)
    kRootPsTextures,  // table t0.. space0 (PIXEL)
    kRootVsSamplers,  // table s0.. space0 (VERTEX)
    kRootPsSamplers,  // table s0.. space0 (PIXEL)
    kRootDrawTextures,  // table t0..2 space2: gtexture, normals, specular
    kRootDrawSamplers,  // table s0..2 space2
    kRootImages,      // table u0..15 space0
    kRootBuffers,     // table u0..7 space1
    kRootCount
};
enum : u32 { kCUniforms, kCTextures, kCSamplers, kCImages, kCBuffers, kCDrawTextures, kCDrawSamplers, kCFrame, kCCount };
constexpr u32 kMaxStageTextures = 48, kMaxImages = 16, kMaxBuffers = 8;

enum class Adapter : u8 { Cube, Model, Generic, Fullscreen, Count };

// Where a sampler / image name points.
enum class Res : u8 {
    None, Color, Depth0, Depth1, Depth2, Shadow0, Shadow1, ShadowColor, Noise, Lightmap, Normals, Specular, Atlas,
    Custom, Image, White, DepthFar, DrawTexture
};
struct ResRef {
    Res kind = Res::None;
    u16 index = 0;
};

// Sampler kinds (sampler heap entries, built once).
enum class Samp : u8 {
    PointClamp, LinearClamp, LinearMipClamp, PointWrap, LinearWrap, AtlasWrap, ShadowCmpLinear, ShadowCmpPoint,
    PointMipClamp, Count
};

struct StageBin {
    bool present = false;
    std::vector<u8> code;
    StageReflection refl;
    std::vector<UniformSlot> uniforms;
    std::vector<ResRef> textures;  // per refl.textures entry (space0 ones; space2 bound per draw)
    std::vector<ResRef> images;
    u32 samplerTable = UINT32_MAX;  // first descriptor in the sampler heap
    u32 drawSamplerTable = UINT32_MAX;
    bool usesDrawTextures = false;
};

struct ProgramBin {
    const Program* src = nullptr;
    std::string name;
    bool gbuffers = false, shadow = false, compute = false, final = false;
    std::array<StageBin, size_t(Adapter::Count)> vs;  // per adapter variant
    StageBin ps, cs;
    std::vector<int> drawBuffers;   // colortex (or shadowcolor) indices per output location
    int depthMode = 0;              // adapter.hlsli mcppClip mode
    std::vector<u8> psWritten;      // per output location: 1 if the fragment shader writes it
};

struct Target {
    ComPtr<ID3D12Resource> tex[2];  // main, alt (flip)
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    u32 w = 0, h = 0, mips = 1;
    bool alt = false;                 // has an alternate texture (written by a composite-style pass)
    u32 srv[2] = {}, uav[2] = {};     // CPU heap indices (srv / uav of mip 0)
    u32 rtv[2] = {};                  // RTV heap indices
    std::vector<u32> mipSrv[2], mipUav[2];
    std::array<float, 4> clear = {0, 0, 0, 0};
    bool clearEnabled = true;
    bool isInt = false;
    D3D12_RESOURCE_STATES state[2] = {D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET};
};

struct Texture {
    ComPtr<ID3D12Resource> res;
    u32 srv = 0;  // CPU heap
    D3D12_SRV_DIMENSION dim = D3D12_SRV_DIMENSION_TEXTURE2D;
    bool blur = false, clamp = false;
};

struct Image {
    ComPtr<ID3D12Resource> res;
    u32 srv = 0, uav = 0;
    std::string name, sampler;
    bool clear = true;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};

struct PsoKey {
    u16 program;
    u8 adapter, blend, depth, cull, targets;
    bool operator<(const PsoKey& o) const {
        return std::tie(program, adapter, blend, depth, cull, targets) <
               std::tie(o.program, o.adapter, o.blend, o.depth, o.cull, o.targets);
    }
};

DXGI_FORMAT dxgiFormat(TexFormat f);
bool formatIsInt(TexFormat f);

}  // namespace packs

struct Renderer::Packs::Pipe {
    std::unique_ptr<packs::PackDefinition> def;
    std::string name;

    ComPtr<ID3D12RootSignature> root, computeRoot, utilRoot;
    ComPtr<ID3D12CommandSignature> cmdSig;
    ComPtr<ID3D12PipelineState> psoDepthToGl, psoMip, psoShadowCull, psoMerge, psoDepthFlip, psoCopy;
    ComPtr<ID3D12Resource> shadowArgs, shadowCounts;  // shadow cull output (4 segments)
    static constexpr u32 kShadowSegCap = 65536;

    // Descriptor heaps: CPU staging (views created once), a GPU ring (per-frame tables), samplers.
    ComPtr<ID3D12DescriptorHeap> cpuHeap, gpuHeap, samplerHeap, rtvHeap, dsvHeap;
    u32 cpuCount = 0, cpuCap = 0, srvStride = 0, samplerStride = 0, rtvStride = 0, dsvStride = 0, rtvCount = 0;
    u32 samplerCount = 0, samplerKinds[size_t(packs::Samp::Count)] = {};
    static constexpr u32 kGpuSlice = 16384;  // shader-visible descriptors per frame slot
    u32 gpuHead = 0;                         // within the current frame's slice
    u32 nullSrv = 0, nullUav = 0, nullSrv3d = 0, nullUav3d = 0;

    // Upload ring (uniform blocks, generic vertices, frame constants): kFrames slices.
    static constexpr u64 kUploadSlice = u64(24) << 20;
    ComPtr<ID3D12Resource> upload;
    u8* uploadMapped = nullptr;
    u64 uploadHead = 0;
    u32 frameSlot = 0;

    // Targets.
    u32 width = 0, height = 0;
    packs::Target color[16];
    packs::Target shadowColor[2];
    ComPtr<ID3D12Resource> depthTex[3];  // GL depth copies (R32F)
    u32 depthSrv[3] = {}, depthUav[3] = {};
    ComPtr<ID3D12Resource> shadowDepth[2];  // shadowtex0 (D32), shadowtex1 (copy before translucent)
    u32 shadowSrv[2] = {}, shadowDsv = 0, shadowRes = 0;
    bool shadowEnabled = false;
    ComPtr<ID3D12Resource> fallbackLayer;  // LDR layer for passes that draw without the pack (merged into colortex0)
    u32 fallbackSrv = 0, fallbackRtv = 0;
    ComPtr<ID3D12Resource> flippedDepth;   // the scene depth the D3D way up (for those passes), D32
    u32 flippedDsv = 1;
    D3D12_RESOURCE_STATES fallbackState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES depthTexState[3] = {D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                              D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE};
    D3D12_RESOURCE_STATES shadowState[2] = {D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE};
    u32 sceneDepthSrv = 0;                 // the renderer's depth (R32F view, reversed-Z)

    // Textures.
    packs::Texture atlas, normals, specular, noise, lightmap, white, depthFar;
    std::vector<packs::Texture> custom;            // by CustomTexture index
    std::vector<packs::Image> images;              // custom images
    std::vector<ComPtr<ID3D12Resource>> buffers;   // bufferObject.N
    u32 bufferUav[packs::kMaxBuffers] = {};
    ComPtr<ID3D12Resource> lightmapUpload;
    u8* lightmapMapped = nullptr;
    ComPtr<ID3D12Resource> quadIndex;              // PackSink quads: 0 1 2, 0 2 3 per 4 vertices (kQuadIndexQuads)
    static constexpr u32 kQuadIndexQuads = 65536;
    ComPtr<ID3D12Resource> layerTable;             // per texture layer: mc_Entity, emission
    std::vector<u32> layerCpu;
    std::vector<u8> layerExact;
    u32 layerTableSize = 0;
    size_t layerTableBlocks = 0;                   // registry states mapped so far
    u32 atlasTilesPerRow = 64, atlasW = 0, atlasH = 0;

    // Programs.
    std::vector<packs::ProgramBin> programs;
    std::unordered_map<std::string, u16> programIndex;  // name -> programs index
    std::map<packs::PsoKey, ComPtr<ID3D12PipelineState>> psos;
    std::map<std::string, ComPtr<ID3D12PipelineState>> computePsos;

    // Uniforms.
    packs::UniformStore uniforms;
    packs::CustomUniforms custom_;
    std::vector<packs::IdMapEntry> blockIds, itemIds, entityIds;
    std::unordered_map<std::string, int> itemIdByName, entityIdByName;
    std::vector<int> blockIdByState;  // BlockRegistry state -> block.properties id (-1)

    // Frame state.
    bool flipped[16] = {};
    u64 frameNumber = 0;
    double startTime = 0, lastTime = 0;
    float frameTime = 0;
    double prevCam[3] = {};
    float prevView[16] = {}, prevProj[16] = {};
    bool havePrev = false;
    float eyeBrightnessSmooth[2] = {0, 240};
    float wetness = 0, centerDepthSmooth = 0;
    ID3D12GraphicsCommandList* list = nullptr;  // while recording
    D3D12_GPU_VIRTUAL_ADDRESS frameCb = 0;      // McppFrame
    PassContext* pc = nullptr;
    int phase = 0;                              // 0 opaque gbuffers, 1 translucent gbuffers (post-deferred flips)
    bool inShadow = false;
    std::vector<std::string> warnings;

    // GPU timings: timestamps between the passes (per frame slot), medians logged when the pack is replaced or at exit.
    static constexpr u32 kMaxMarks = 64;
    ComPtr<ID3D12QueryHeap> timeQueries;
    ComPtr<ID3D12Resource> timeReadback;
    u32 markCount[Renderer::kFrames] = {};
    const char* markNames[Renderer::kFrames][kMaxMarks] = {};
    std::vector<std::pair<std::string, std::vector<float>>> timing;  // pass -> samples (ms), in pass order
    std::vector<float> timingTotal;
    u64 timingFrames = 0;
};
