#include "render/pathtrace.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "BlasGenCS.h"
#include "PathTraceCS.h"
#include "PtAccumCS.h"
#include "PtComposeCS.h"
#include "TerrainFwdPtPS.h"
#include "TerrainFwdVS.h"
#include "render/upscalers/upscalers.h"
#include "PtClearCS.h"
#include "EntityGenCS.h"
#include "LightGatherCS.h"
#include "LightCdfCS.h"

namespace {

constexpr D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr D3D12_RESOURCE_STATES kAs = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
constexpr u32 kJobBytes = 32;
constexpr u32 kInstanceBytes = sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
constexpr u32 kNone = UINT32_MAX;

// Root parameters of the renderer's root signature (renderer.cpp createPipelines; shading.cpp Root).
enum Root : u32 { RootDraw, RootFrame, RootQuads, RootSections, RootBlockTex, RootCounts, RootArgs, RootActive, RootColors,
                  RootShade, RootSrvTable, RootUavTable, RootPass, RootRemap, RootTlas, RootModelQuads, RootPtSrv, RootPtUav,
                  RootPtConstants };

// Must match cbuffer PathTrace in pathtrace.hlsl.
struct PtConstants {
    float params[4];
    u32 opts[4];
    u32 jobs[4];
    u32 slots[4];
    float move[4];
    u32 lights[4];
    u32 entity[4];
    u32 entity2[4];
};
static_assert(sizeof(PtConstants) <= Renderer::PathTracer::kConstantBytes);

struct BuildJob {
    u32 slot, firstQuad, vertexBase, ptIndex;
    u32 counts[4];
};
static_assert(sizeof(BuildJob) == kJobBytes);

bool check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    LOG("error: path tracing: %s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    return false;
}

u32 g_descBase = UINT32_MAX;  // the descriptor tables outlive a path tracer (the renderer's heap never frees)

}  // namespace

// ---- setup --------------------------------------------------------------------------------------------------------

bool Renderer::PathTracer::supported(ID3D12Device* device, std::string* why) {
    ComPtr<ID3D12Device5> d5;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&d5))) ||
        FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))) ||
        o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
        if (why) *why = "needs a GPU with DirectX Raytracing 1.1 (inline ray tracing)";
        return false;
    }
    return true;
}

bool Renderer::pathTracingSupported(std::string* why) const { return PathTracer::supported(device_.Get(), why); }

std::string Renderer::pathTracingStatus() const {
    std::string why;
    if (!pathTracingSupported(&why)) return why;
    return pt_ ? pt_->status() : std::string("off");
}

Renderer::PathTracer::~PathTracer() {
    r.waitIdle();
    LOG("path tracing: off (%u sections had acceleration structures, %.0f MB)", instanceCount_, double(asBytes_) / 1048576.0);
}

bool Renderer::PathTracer::create() {
    if (!check(r.device_.As(&dev5_), "ID3D12Device5")) return false;
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
        r.device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
        LOG("path tracing: DXR tier %d.%d, inline ray queries", int(o5.RaytracingTier) / 10, int(o5.RaytracingTier) % 10);
    }
    if (const char* v = std::getenv("MCPP_PT_DEBUG")) debug_ = u32(std::atoi(v));
    auto compute = [&](const void* cs, size_t size, ComPtr<ID3D12PipelineState>& out) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = r.rootSig_.Get();
        d.CS = {cs, size};
        return check(r.device_->CreateComputePipelineState(&d, IID_PPV_ARGS(&out)), "compute pipeline");
    };
    if (!compute(g_BlasGenCS, sizeof(g_BlasGenCS), psoBlasGen_) || !compute(g_PathTraceCS, sizeof(g_PathTraceCS), psoTrace_) ||
        !compute(g_PtAccumCS, sizeof(g_PtAccumCS), psoAccum_) || !compute(g_PtComposeCS, sizeof(g_PtComposeCS), psoCompose_) || !compute(g_PtClearCS, sizeof(g_PtClearCS), psoClear_) ||
        !compute(g_LightGatherCS, sizeof(g_LightGatherCS), psoGather_) || !compute(g_LightCdfCS, sizeof(g_LightCdfCS), psoCdf_) ||
        !compute(g_EntityGenCS, sizeof(g_EntityGenCS), psoEntity_))
        return false;

    {  // translucent terrain with traced reflections (as Shading's psoFwd_)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = r.rootSig_.Get();
        d.VS = {g_TerrainFwdVS, sizeof(g_TerrainFwdVS)};
        d.PS = {g_TerrainFwdPtPS, sizeof(g_TerrainFwdPtPS)};
        auto& rt = d.BlendState.RenderTarget[0];
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        d.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.NumRenderTargets = 1;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.DepthStencilState.DepthEnable = TRUE;
        d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        d.SampleDesc.Count = 1;
        if (!check(r.device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&psoFwd_)), "translucent pipeline")) return false;
    }
    jobs_ = r.createBuffer(u64(kJobBytes) * kMaxJobs * Renderer::kFrames, D3D12_HEAP_TYPE_UPLOAD);
    instances_ = r.createBuffer(u64(kInstanceBytes) * kMaxSections * Renderer::kFrames, D3D12_HEAP_TYPE_UPLOAD);
    constants_ = r.createBuffer(u64(kConstantBytes) * Renderer::kFrames, D3D12_HEAP_TYPE_UPLOAD);
    vertices_ = r.createBuffer(u64(kQuadsPerFrame) * 4 * 12, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    entityVerts_ = r.createBuffer(u64(kEntityQuads) * 4 * 12, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!entityVerts_) return false;
    postbuild_ = r.createBuffer(u64(kMaxJobs) * 8 * Renderer::kFrames, D3D12_HEAP_TYPE_DEFAULT,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    postbuildReadback_ = r.createBuffer(u64(kMaxJobs) * 8 * Renderer::kFrames, D3D12_HEAP_TYPE_READBACK);
    const auto uavFlag = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    lights_ = r.createBuffer(u64(kMaxSections) * kSectionLights * 8, D3D12_HEAP_TYPE_DEFAULT, uavFlag);
    lightSlot_ = r.createBuffer(u64(kMaxSections) * 4, D3D12_HEAP_TYPE_DEFAULT, uavFlag);
    lightCounts_ = r.createBuffer(u64(kMaxSections) * 4, D3D12_HEAP_TYPE_DEFAULT, uavFlag);
    frameLights_ = r.createBuffer(u64(kFrameLights) * 4, D3D12_HEAP_TYPE_DEFAULT, uavFlag);
    frameLightCount_ = r.createBuffer(256, D3D12_HEAP_TYPE_DEFAULT, uavFlag);
    lightSections_ = r.createBuffer(u64(kMaxSections) * 8 * Renderer::kFrames, D3D12_HEAP_TYPE_UPLOAD);
    lightPower_ = r.createBuffer(u64(kMaxSections) * 4, D3D12_HEAP_TYPE_DEFAULT, uavFlag);
    frameSecs_ = r.createBuffer(u64(kMaxSections) * 16, D3D12_HEAP_TYPE_DEFAULT, uavFlag);
    frameSecCdf_ = r.createBuffer(u64(kMaxSections) * 4, D3D12_HEAP_TYPE_DEFAULT, uavFlag);
    if (!jobs_ || !instances_ || !constants_ || !vertices_ || !postbuild_ || !postbuildReadback_ || !lights_ || !lightCounts_ ||
        !frameLights_ || !frameLightCount_ || !lightSections_ || !lightPower_ || !frameSecs_ || !frameSecCdf_ || !lightSlot_)
        return false;
    lightSections_->Map(0, nullptr, reinterpret_cast<void**>(&lightSectionsMapped_));
    jobs_->Map(0, nullptr, reinterpret_cast<void**>(&jobsMapped_));
    instances_->Map(0, nullptr, reinterpret_cast<void**>(&instancesMapped_));
    constants_->Map(0, nullptr, reinterpret_cast<void**>(&constantsMapped_));

    if (g_descBase == UINT32_MAX) g_descBase = r.allocDescriptors(2 * kTableSize);
    descBase_ = g_descBase;
    if (descBase_ == UINT32_MAX) return check(E_OUTOFMEMORY, "descriptors");
    for (u32 i = 0; i < kTableSize; ++i) {  // null views until used
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = DXGI_FORMAT_R32_UINT;
        s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Buffer.NumElements = 1;
        r.device_->CreateShaderResourceView(nullptr, &s, r.cpuDescriptor(descBase_ + i));
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_R32_UINT;
        u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements = 1;
        r.device_->CreateUnorderedAccessView(nullptr, nullptr, &u, r.cpuDescriptor(descBase_ + kTableSize + i));
    }
    {  // t1 space4: the entity atlas (a null 2D view until entities are traced)
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture2D.MipLevels = 1;
        r.device_->CreateShaderResourceView(nullptr, &s, r.cpuDescriptor(descBase_ + 1));
    }
    {  // t0 space4: build jobs (all frame slices; gPtJobs.y = first job of this frame)
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = DXGI_FORMAT_UNKNOWN;
        s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Buffer.NumElements = kMaxJobs * Renderer::kFrames;
        s.Buffer.StructureByteStride = kJobBytes;
        r.device_->CreateShaderResourceView(jobs_.Get(), &s, r.cpuDescriptor(descBase_));
        s.Buffer.NumElements = kMaxSections * Renderer::kFrames;  // t5: light sections (frame slices)
        s.Buffer.StructureByteStride = 8;
        r.device_->CreateShaderResourceView(lightSections_.Get(), &s, r.cpuDescriptor(descBase_ + 5));
        // u1..u7: lights, light counts, frame light list, its count, light power, frame sections, their CDF
        ID3D12Resource* bufs[7] = {lights_.Get(),      lightCounts_.Get(), frameLights_.Get(), frameLightCount_.Get(),
                                   lightPower_.Get(), frameSecs_.Get(),   frameSecCdf_.Get()};
        const u32 elems[7] = {kMaxSections * kSectionLights, kMaxSections, kFrameLights, 64, kMaxSections, kMaxSections,
                              kMaxSections};
        const u32 strides[7] = {8, 4, 4, 4, 4, 16, 4};
        for (u32 i = 0; i < 8; ++i) {  // + u18: light slots
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = DXGI_FORMAT_UNKNOWN;
            u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            u.Buffer.NumElements = i < 7 ? elems[i] : kMaxSections;
            u.Buffer.StructureByteStride = i < 7 ? strides[i] : 4;
            r.device_->CreateUnorderedAccessView(i < 7 ? bufs[i] : lightSlot_.Get(), nullptr, &u,
                                                 r.cpuDescriptor(descBase_ + kTableSize + (i < 7 ? 1 + i : 18)));
        }
    }
    slotEntry_.assign(r.slotCapacity_, kNone);
    level0Index_.assign(r.slotCapacity_, kNone);
    entries_.clear();
    freeEntries_.clear();
    for (u32 slot = 0; slot < r.slotHigh_; ++slot)  // the full-detail meshes there already are
        if (r.slots_[slot].cubeCount + r.slots_[slot].modelCount) meshAdded(slot);
    LOG("path tracing: on (radius %.0f blocks, %u bounce%s, %u spp)", r.shaderSettings_.ptRadius, r.shaderSettings_.ptBounces,
        r.shaderSettings_.ptBounces == 1 ? "" : "s", r.shaderSettings_.ptSamples);
    return true;
}

bool Renderer::PathTracer::createTargets() {
    r.waitIdle();
    Shading& sh = *r.shading_;
    const u32 w = sh.renderW_, h = sh.renderH_;
    static const DXGI_FORMAT kFormats[TgtCount] = {
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT};
    for (u32 i = 0; i < TgtCount; ++i) {
        targets_[i] = sh.texture(kFormats[i], w, h, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kUav, nullptr);
        if (!targets_[i]) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = kFormats[i];
        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture2D.MipLevels = 1;
        r.device_->CreateShaderResourceView(targets_[i].Get(), &s, r.cpuDescriptor(descBase_ + kTargetBase + i));
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = kFormats[i];
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        r.device_->CreateUnorderedAccessView(targets_[i].Get(), nullptr, &u,
                                             r.cpuDescriptor(descBase_ + kTableSize + kTargetBase + i));
    }
    {
        hitDist_ = sh.texture(DXGI_FORMAT_R32_FLOAT, w, h, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kUav, nullptr);
        if (!hitDist_) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_R32_FLOAT;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        r.device_->CreateUnorderedAccessView(hitDist_.Get(), nullptr, &u, r.cpuDescriptor(descBase_ + kTableSize + 19));
    }
    for (u32 i = 0; i < 2; ++i) {
        reservoirs_[i] = sh.texture(DXGI_FORMAT_R32G32B32A32_UINT, w, h, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kUav,
                                    nullptr);
        if (!reservoirs_[i]) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        r.device_->CreateUnorderedAccessView(reservoirs_[i].Get(), nullptr, &u,
                                             r.cpuDescriptor(descBase_ + kTableSize + kReservoirBase + i));
    }
    histValid_ = false;
    return true;
}

// ---- memory -------------------------------------------------------------------------------------------------------

Renderer::PathTracer::AsAlloc Renderer::PathTracer::allocAs(u64 bytes) {
    const u64 units = (bytes + 255) / 256;
    for (u32 p = 0; p < pools_.size(); ++p) {
        const u64 at = pools_[p].alloc.alloc(units);
        if (at != RangeAllocator::kFail) {
            asBytes_ += units * 256;
            return {p, at * 256, units * 256};
        }
    }
    Pool pool;
    const u64 size = std::max(kPoolBytes, units * 256);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!check(r.device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kAs, nullptr, IID_PPV_ARGS(&pool.buffer)),
               "acceleration structure pool"))
        return {};
    pool.alloc.reset(size / 256);
    pools_.push_back(std::move(pool));
    LOG("path tracing: acceleration structure pool %zu (%.0f MB)", pools_.size(), double(size) / 1048576.0);
    const u64 at = pools_.back().alloc.alloc(units);
    asBytes_ += units * 256;
    return {u32(pools_.size() - 1), at * 256, units * 256};
}

void Renderer::PathTracer::freeAs(AsAlloc& a, bool recording) {
    if (!a.valid()) return;
    // Recorded this frame: free once this frame slot comes round again (the GPU has finished everything up to it).
    // Otherwise (a mesh removed between frames): once everything submitted so far has finished.
    freed_.push_back({recording ? 0 : r.fenceCounter_, curFrame_, a});
    a = {};
}

void Renderer::PathTracer::retire(ComPtr<ID3D12Resource>& res) {
    if (res) graveyard_.push_back({r.fenceCounter_ + 1, std::move(res)});
    res.Reset();
}

void Renderer::PathTracer::collect(u32 frame) {
    const u64 done = r.fence_->GetCompletedValue();
    for (size_t i = 0; i < freed_.size();) {
        Freed& f = freed_[i];
        if ((f.fence == 0 && f.frame == frame) || (f.fence != 0 && done >= f.fence)) {
            pools_[f.a.pool].alloc.free(f.a.offset / 256, f.a.size / 256);
            asBytes_ -= f.a.size;
            f = freed_.back();
            freed_.pop_back();
        } else {
            ++i;
        }
    }
    std::erase_if(graveyard_, [&](const auto& g) { return done >= g.first; });
}

void Renderer::PathTracer::dropEntry(u32 e, bool recording) {
    Entry& en = entries_[e];
    freeAs(en.blas, recording);
    if (en.slot < slotEntry_.size()) slotEntry_[en.slot] = kNone;
    en.slot = kNone;
    ++en.gen;
    en.compacted = false;
    freeEntries_.push_back(e);
}

void Renderer::PathTracer::meshAdded(u32 slot) {
    if (slot >= slotEntry_.size()) slotEntry_.resize(std::max<size_t>(slot + 1, r.slotCapacity_), kNone);
    if (slot >= level0Index_.size()) level0Index_.resize(std::max<size_t>(slot + 1, r.slotCapacity_), kNone);
    if (slotEntry_[slot] != kNone) dropEntry(slotEntry_[slot], false);  // (removeMesh normally came first)
    const Slot& s = r.slots_[slot];
    if (s.origin[3] != 0 || level0Index_[slot] != kNone) return;
    level0Index_[slot] = u32(level0_.size());
    level0_.push_back({s.origin[0], s.origin[1], s.origin[2], slot});
    level0Dirty_ = true;
}

void Renderer::PathTracer::meshRemoved(u32 slot) {
    if (slot < slotEntry_.size() && slotEntry_[slot] != kNone) dropEntry(slotEntry_[slot], false);
    if (slot < level0Index_.size() && level0Index_[slot] != kNone) {
        const u32 i = level0Index_[slot];
        level0Index_[level0_.back().slot] = i;
        level0_[i] = level0_.back();
        level0_.pop_back();
        level0Index_[slot] = kNone;
        level0Dirty_ = true;
    }
}

// ---- per frame ----------------------------------------------------------------------------------------------------

void Renderer::PathTracer::build(ID3D12GraphicsCommandList* list, const Shading::FrameInput& in, D3D12_GPU_VIRTUAL_ADDRESS cb,
                                 u32 activeCount) {
    const u32 fi = in.frameIndex;
    const Timer timer;
    curFrame_ = fi;
    ++frame_;
    collect(fi);
    ComPtr<ID3D12GraphicsCommandList4> list4;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list4)))) return;
    if (slotEntry_.size() < r.slotCapacity_) slotEntry_.resize(r.slotCapacity_, kNone);
    const CameraState& cam = *in.cam;
    const ShaderSettings& set = r.shaderSettings_;
    Shading& sh = *r.shading_;

    // ---- the sections in the radius: instances for those with a BLAS, build candidates for the others ----
    struct Candidate {
        float d;
        u32 slot;
    };
    thread_local std::vector<Candidate> cand;
    thread_local std::vector<u32> visible;
    thread_local std::vector<std::pair<u32, float>> lightSections;  // (pt index, distance)
    cand.clear(), visible.clear(), lightSections.clear();
    const float radius = set.ptRadius;
    {  // The full-detail sections in the radius: walked again when the cut changes or the camera has moved a few
       // blocks (the active list is long - every LOD node - and its slots are scattered in memory).
        const double mx = cam.x - scanCam_[0], my = cam.y - scanCam_[1], mz = cam.z - scanCam_[2];
        if ((level0Dirty_ && frame_ >= scanFrame_ + 4) || scanRadius_ != radius || mx * mx + my * my + mz * mz > 16.0) {
            level0Dirty_ = false, scanRadius_ = radius, scanFrame_ = frame_;
            scanCam_[0] = cam.x, scanCam_[1] = cam.y, scanCam_[2] = cam.z;
            inRadius_.clear();
            float farthest = 0;
            for (const Level0& l : level0_) {
                const double dx = l.x + 8 - cam.x, dy = l.y + 8 - cam.y, dz = l.z + 8 - cam.z;
                const double dh2 = dx * dx + dz * dz;
                farthest = std::max(farthest, float(dh2));
                const double d2 = dh2 + dy * dy;
                if (d2 <= double(radius + 18.0f) * (radius + 18.0f)) inRadius_.push_back({float(std::sqrt(d2)), l.slot});
            }
            // beyond the full-detail sections there is nothing to trace against
            radius_ = std::min(radius, std::sqrt(farthest));
        }
    }
    for (const auto& [d, slot] : inRadius_) {
        const u32 e = slotEntry_[slot];
        if (e == kNone) {
            cand.push_back({d, slot});
        } else {
            entries_[e].lastSeen = frame_, visible.push_back(e);
            if (d <= kLightRange) lightSections.push_back({e, d});
        }
    }
    for (u32 e = 0; e < entries_.size(); ++e)  // left the radius (or the cut) a while ago
        if (entries_[e].slot != kNone && entries_[e].lastSeen + 8 < frame_) dropEntry(e, true);

    partMs_[0] += timer.ms();
    // ---- compaction: sizes of the builds recorded the last time this frame slot was used ----
    if (!compactions_[fi].empty()) {
        D3D12_RANGE range{size_t(fi) * kMaxJobs * 8, size_t(fi + 1) * kMaxJobs * 8}, none{0, 0};
        u8* p = nullptr;
        if (SUCCEEDED(postbuildReadback_->Map(0, &range, reinterpret_cast<void**>(&p)))) {
            for (const Compaction& c : compactions_[fi]) {
                Entry& en = entries_[c.entry];
                if (en.gen != c.gen || en.slot == kNone || !en.blas.valid()) continue;
                u64 size;
                std::memcpy(&size, p + range.Begin + size_t(c.job) * 8, 8);
                if (!size || size >= en.blas.size) {
                    en.compacted = true;
                    continue;
                }
                AsAlloc packed = allocAs(size);
                if (!packed.valid()) continue;
                list4->CopyRaytracingAccelerationStructure(asVa(packed), asVa(en.blas),
                                                           D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
                freeAs(en.blas, true);
                en.blas = packed;
                en.compacted = true;
            }
            postbuildReadback_->Unmap(0, &none);
        }
        compactions_[fi].clear();
    }

    partMs_[1] += timer.ms();
    // ---- new BLASes, nearest first ----
    std::sort(cand.begin(), cand.end(), [](const Candidate& a, const Candidate& b) { return a.d < b.d; });
    BuildJob jobs[kMaxJobs];  // copied to the upload heap below (never read back from it)
    float jobDist[kMaxJobs];
    u32 jobCount = 0, quads = 0;
    for (const Candidate& c : cand) {
        if (jobCount == kMaxJobs || (entries_.size() - freeEntries_.size()) >= kMaxSections) break;
        const Slot& s = r.slots_[c.slot];
        const u32 counts[4] = {std::min(s.opaque, 65536u), std::min(s.cutout, 65536u), std::min(s.translucent, 65536u),
                               u32(std::min<u64>(s.modelCount, 65536))};
        const u32 n = counts[0] + counts[1] + counts[2] + counts[3];
        if (jobCount && quads + n > kQuadsPerFrame) break;
        if (n == 0 || quads + n > kQuadsPerFrame) continue;
        u32 e;
        if (!freeEntries_.empty()) e = freeEntries_.back(), freeEntries_.pop_back();
        else e = u32(entries_.size()), entries_.emplace_back();
        Entry& en = entries_[e];
        en.slot = c.slot, en.lastSeen = frame_, en.compacted = false, en.kindMap = 0;
        slotEntry_[c.slot] = e;
        jobDist[jobCount] = c.d;
        BuildJob& j = jobs[jobCount++];
        j = {c.slot, quads, quads * 4, e, {counts[0], counts[1], counts[2], counts[3]}};
        quads += n;
    }
    queued_ = u32(cand.size()) - jobCount;
    std::memcpy(jobsMapped_ + size_t(fi) * kMaxJobs * kJobBytes, jobs, size_t(jobCount) * kJobBytes);
    builtThisFrame_ = jobCount;

    PtConstants k{};
    const double move[3] = {cam.x - prevCam_[0], cam.y - prevCam_[1], cam.z - prevCam_[2]};
    if (!sh.historyValid_ || std::abs(move[0]) + std::abs(move[1]) + std::abs(move[2]) > 64.0) histValid_ = false;
    prevCam_[0] = cam.x, prevCam_[1] = cam.y, prevCam_[2] = cam.z;
    k.params[0] = radius_, k.params[1] = 16.0f, k.params[2] = histValid_ ? 1.0f : 0.0f, k.params[3] = 24.0f;
    k.opts[0] = debug_, k.opts[1] = set.ptBounces, k.opts[2] = set.ptSamples, k.opts[3] = u32(frame_);
    k.jobs[0] = jobCount, k.jobs[1] = fi * kMaxJobs, k.jobs[2] = quads, k.jobs[3] = 0;
    k.slots[0] = histSlot_, k.slots[1] = histSlot_ ^ 1;
    for (int c = 0; c < 3; ++c) k.move[c] = float(move[c]);
    k.lights[1] = fi * kMaxSections, k.lights[2] = kFrameLights;
    std::memcpy(constantsMapped_ + size_t(fi) * kConstantBytes, &k, sizeof(k));
    const D3D12_GPU_VIRTUAL_ADDRESS ptCb = constants_->GetGPUVirtualAddress() + u64(fi) * kConstantBytes;

    if (jobCount) {
        // Triangles of every section built this frame.
        sh.bindCompute(list, cb);
        list->SetPipelineState(psoBlasGen_.Get());
        list->SetComputeRootShaderResourceView(RootQuads, r.cubeQuads_->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(RootModelQuads, r.modelQuads_->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(RootSections, r.sections_->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(RootRemap, r.remap_->GetGPUVirtualAddress() + u64(fi) * kRemapCapacity * 4);
        list->SetComputeRootUnorderedAccessView(RootCounts, vertices_->GetGPUVirtualAddress());
        list->SetComputeRootDescriptorTable(RootBlockTex, r.srvHeap_->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootDescriptorTable(RootPtSrv, r.gpuDescriptor(descBase_));
        list->SetComputeRootDescriptorTable(RootPtUav, r.gpuDescriptor(descBase_ + kTableSize));
        list->SetComputeRootConstantBufferView(RootPtConstants, ptCb);
        list->SetPipelineState(psoClear_.Get());
        list->Dispatch((jobCount + 63) / 64, 1, 1);
        {
            D3D12_RESOURCE_BARRIER u{};
            u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            list->ResourceBarrier(1, &u);
        }
        list->SetPipelineState(psoBlasGen_.Get());
        list->Dispatch((quads + 63) / 64, 1, 1);
        D3D12_RESOURCE_BARRIER b[2]{};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition.pResource = vertices_.Get();
        b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[0].Transition.StateBefore = kUav;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b[1] = b[0];
        b[1].Transition.pResource = postbuild_.Get();
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b[1].Transition.StateAfter = kUav;
        list->ResourceBarrier(2, b);

        // Inputs and sizes first (one scratch region per build: they may run concurrently).
        struct Build {
            D3D12_RAYTRACING_GEOMETRY_DESC geo[4];
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs;
            u64 scratch;
        };
        thread_local std::vector<Build> builds;
        builds.resize(jobCount);
        u64 scratchTotal = 0;
        for (u32 j = 0; j < jobCount; ++j) {
            Build& bd = builds[j];
            Entry& en = entries_[jobs[j].ptIndex];
            u32 n = 0;
            u64 vtx = jobs[j].vertexBase;
            for (u32 kind = 0; kind < 4; ++kind) {
                const u32 count = jobs[j].counts[kind];
                if (!count) continue;
                D3D12_RAYTRACING_GEOMETRY_DESC& g = bd.geo[n];
                g = {};
                g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                g.Flags = kind == 0 ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
                g.Triangles.IndexBuffer = r.indexBuffer_->GetGPUVirtualAddress();
                g.Triangles.IndexCount = count * 6;
                g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
                g.Triangles.VertexBuffer = {vertices_->GetGPUVirtualAddress() + vtx * 12, 12};
                g.Triangles.VertexCount = count * 4;
                g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                en.kindMap |= kind << (2 * n);
                ++n;
                vtx += u64(count) * 4;
            }
            bd.inputs = {};
            bd.inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            bd.inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
                              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
            bd.inputs.NumDescs = n;
            bd.inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            bd.inputs.pGeometryDescs = bd.geo;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
            dev5_->GetRaytracingAccelerationStructurePrebuildInfo(&bd.inputs, &info);
            en.blas = allocAs(info.ResultDataMaxSizeInBytes);
            bd.scratch = scratchTotal;
            scratchTotal += (info.ScratchDataSizeInBytes + 255) & ~u64(255);
        }
        if (scratchTotal > scratchBytes_) {
            retire(scratch_);
            scratchBytes_ = std::max(scratchTotal, scratchBytes_ * 2);
            scratch_ = r.createBuffer(scratchBytes_, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        }
        for (u32 j = 0; j < jobCount; ++j) {
            Entry& en = entries_[jobs[j].ptIndex];
            if (!en.blas.valid() || !scratch_) {
                dropEntry(jobs[j].ptIndex, true);
                continue;
            }
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
            d.DestAccelerationStructureData = asVa(en.blas);
            d.Inputs = builds[j].inputs;
            d.ScratchAccelerationStructureData = scratch_->GetGPUVirtualAddress() + builds[j].scratch;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC post{};
            post.DestBuffer = postbuild_->GetGPUVirtualAddress() + (u64(fi) * kMaxJobs + j) * 8;
            post.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
            list4->BuildRaytracingAccelerationStructure(&d, 1, &post);
            compactions_[fi].push_back({jobs[j].ptIndex, en.gen, j});
            visible.push_back(jobs[j].ptIndex);
            if (jobDist[j] <= kLightRange) lightSections.push_back({jobs[j].ptIndex, jobDist[j]});
        }
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        list->ResourceBarrier(1, &uav);
        b[0].Transition.pResource = postbuild_.Get();
        b[0].Transition.StateBefore = kUav;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, b);
        list->CopyBufferRegion(postbuildReadback_.Get(), u64(fi) * kMaxJobs * 8, postbuild_.Get(), u64(fi) * kMaxJobs * 8,
                               u64(jobCount) * 8);
    } else {
        D3D12_RESOURCE_BARRIER uav{};  // compaction copies before the TLAS reads them
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        list->ResourceBarrier(1, &uav);
    }

    partMs_[2] += timer.ms();
    // ---- entities: one BLAS of this frame's posed triangles (rebuilt every frame) ----
    const bool entities = buildEntities(list4.Get(), fi, cb, ptCb);

    // ---- TLAS: every section in the radius with a BLAS, camera-relative ----
    // Built in cached memory, then copied: the descriptor's bit fields would read back from the write-combined upload
    // heap (microseconds each).
    thread_local std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
    instances.resize(std::min<size_t>(visible.size(), kMaxSections));
    D3D12_RAYTRACING_INSTANCE_DESC* inst = instances.data();
    u32 count = 0;
    for (u32 e : visible) {
        const Entry& en = entries_[e];
        if (en.slot == kNone || !en.blas.valid() || count == kMaxSections) continue;
        const Slot& s = r.slots_[en.slot];
        D3D12_RAYTRACING_INSTANCE_DESC& d = inst[count++];
        d = {};
        d.Transform[0][0] = d.Transform[1][1] = d.Transform[2][2] = 1.0f;
        d.Transform[0][3] = float(double(s.origin[0]) - cam.x);
        d.Transform[1][3] = float(double(s.origin[1]) - cam.y);
        d.Transform[2][3] = float(double(s.origin[2]) - cam.z);
        d.InstanceID = en.slot;
        d.InstanceMask = 1;
        d.InstanceContributionToHitGroupIndex = en.kindMap;
        d.AccelerationStructure = asVa(en.blas);
    }
    instanceCount_ = count;
    if (entities && count < instances.size() + 1) {
        instances.resize(count + 1);
        inst = instances.data();
        D3D12_RAYTRACING_INSTANCE_DESC& d = inst[count++];
        d = {};
        d.Transform[0][0] = d.Transform[1][1] = d.Transform[2][2] = 1.0f;  // already camera-relative
        d.InstanceID = 0xFFFFFF;
        d.InstanceMask = 1;
        d.AccelerationStructure = entityBlas_->GetGPUVirtualAddress();
    }
    std::memcpy(instancesMapped_ + size_t(fi) * kMaxSections * kInstanceBytes, inst, size_t(count) * kInstanceBytes);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tin{};
    tin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tin.NumDescs = count;
    tin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tin.InstanceDescs = instances_->GetGPUVirtualAddress() + u64(fi) * kMaxSections * kInstanceBytes;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tinfo{};
    dev5_->GetRaytracingAccelerationStructurePrebuildInfo(&tin, &tinfo);
    if (!tlas_[fi] || tlasBytes_[fi] < tinfo.ResultDataMaxSizeInBytes) {
        tlasBytes_[fi] = std::max<u64>(tinfo.ResultDataMaxSizeInBytes * 3 / 2, 65536);
        tlas_[fi].Reset();  // the frame that used it last has finished
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = tlasBytes_[fi];
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(r.device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kAs, nullptr, IID_PPV_ARGS(&tlas_[fi])), "TLAS");
    }
    if (tinfo.ScratchDataSizeInBytes > tlasScratchBytes_) {
        retire(tlasScratch_);
        tlasScratchBytes_ = tinfo.ScratchDataSizeInBytes * 3 / 2;
        tlasScratch_ = r.createBuffer(tlasScratchBytes_, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }
    haveTlas_ = tlas_[fi] && tlasScratch_;
    if (!haveTlas_) return;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td{};
    td.DestAccelerationStructureData = tlas_[fi]->GetGPUVirtualAddress();
    td.Inputs = tin;
    td.ScratchAccelerationStructureData = tlasScratch_->GetGPUVirtualAddress();
    list4->BuildRaytracingAccelerationStructure(&td, 0, nullptr);

    // ---- this frame's emissive light list: the lights of the sections near the camera ----
    lightSectionCount_ = u32(std::min<size_t>(lightSections.size(), kMaxSections));
    static_assert(sizeof(std::pair<u32, float>) == 8);
    std::memcpy(lightSectionsMapped_ + size_t(fi) * kMaxSections * 8, lightSections.data(), size_t(lightSectionCount_) * 8);
    reinterpret_cast<PtConstants*>(constantsMapped_ + size_t(fi) * kConstantBytes)->lights[0] = lightSectionCount_;
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = frameLightCount_.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &b);
        list->CopyBufferRegion(frameLightCount_.Get(), 0, r.zeros_.Get(), 0, 16);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = kUav;
        list->ResourceBarrier(1, &b);
    }
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &uav);  // BLAS builds / compaction, the TLAS, light counts
    if (lightSectionCount_) {
        sh.bindCompute(list, cb);
        list->SetComputeRootDescriptorTable(RootPtSrv, r.gpuDescriptor(descBase_));
        list->SetComputeRootDescriptorTable(RootPtUav, r.gpuDescriptor(descBase_ + kTableSize));
        list->SetComputeRootConstantBufferView(RootPtConstants, ptCb);
        list->SetPipelineState(psoGather_.Get());
        list->Dispatch((lightSectionCount_ + 63) / 64, 1, 1);
        list->ResourceBarrier(1, &uav);
        list->SetPipelineState(psoCdf_.Get());
        list->Dispatch(1, 1, 1);
        list->ResourceBarrier(1, &uav);
    }
    cpuMs_ += timer.ms();
    if (debug_ && frame_ % 120 == 0) {
        LOG("path tracing: %s | build cpu %.3f ms/frame (%.3f %.3f %.3f) | active %u", status().c_str(), cpuMs_ / 120.0,
            partMs_[0] / 120.0, partMs_[1] / 120.0, partMs_[2] / 120.0, activeCount);
        cpuMs_ = partMs_[0] = partMs_[1] = partMs_[2] = 0;
    }
}

void Renderer::PathTracer::trace(ID3D12GraphicsCommandList* list, const Shading::FrameInput& in, D3D12_GPU_VIRTUAL_ADDRESS cb) {
    if (!haveTlas_) return;
    const u32 fi = in.frameIndex;
    Shading& sh = *r.shading_;
    sh.bindCompute(list, cb);
    list->SetPipelineState(psoTrace_.Get());
    list->SetComputeRootShaderResourceView(RootTlas, tlas_[fi]->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(RootQuads, r.cubeQuads_->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(RootModelQuads, r.modelQuads_->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(RootSections, r.sections_->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(RootColors, r.colors_->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(RootRemap, r.remap_->GetGPUVirtualAddress() + u64(fi) * kRemapCapacity * 4);
    list->SetComputeRootDescriptorTable(RootBlockTex, r.srvHeap_->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootDescriptorTable(RootPtSrv, r.gpuDescriptor(descBase_));
    list->SetComputeRootDescriptorTable(RootPtUav, r.gpuDescriptor(descBase_ + kTableSize));
    list->SetComputeRootConstantBufferView(RootPtConstants, constants_->GetGPUVirtualAddress() + u64(fi) * kConstantBytes);
    const u32 gx = (sh.width_ + 7) / 8, gy = (sh.height_ + 7) / 8;
    list->Dispatch(gx, gy, 1);
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &uav);
    if (debug_ == 1 || debug_ == 2) return;
    // DLSS Ray Reconstruction denoises the noisy radiance itself; else the built-in temporal accumulation.
    const bool rr = rayReconstruction() != nullptr;
    if (rr != rrUsed_) histValid_ = false, rrUsed_ = rr;
    list->SetPipelineState(rr ? psoCompose_.Get() : psoAccum_.Get());
    list->Dispatch(gx, gy, 1);
    histSlot_ ^= 1;
    histValid_ = true;
}

Upscaler* Renderer::PathTracer::rayReconstruction() const { return r.upscalers_->rayReconstruction(); }

void Renderer::PathTracer::fillGuides(UpscaleInputs& u) const {
    u.diffuseAlbedo = targets_[TgtDiffAlbedo].Get();
    u.specularAlbedo = targets_[TgtSpecAlbedo].Get();
    u.normalRoughness = targets_[TgtNormalRough].Get();
    u.specularHitDistance = hitDist_.Get();
    u.guideState = kUav;
}

ID3D12PipelineState* Renderer::PathTracer::bindTranslucent(ID3D12GraphicsCommandList* list, u32 frame) {
    if (!haveTlas_ || debug_ == 1 || debug_ == 2) return nullptr;
    list->SetGraphicsRootShaderResourceView(RootTlas, tlas_[frame]->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(RootModelQuads, r.modelQuads_->GetGPUVirtualAddress());
    list->SetGraphicsRootDescriptorTable(RootPtSrv, r.gpuDescriptor(descBase_));
    list->SetGraphicsRootDescriptorTable(RootPtUav, r.gpuDescriptor(descBase_ + kTableSize));
    list->SetGraphicsRootConstantBufferView(RootPtConstants, constants_->GetGPUVirtualAddress() + u64(frame) * kConstantBytes);
    return psoFwd_.Get();
}

bool Renderer::PathTracer::buildEntities(ID3D12GraphicsCommandList4* list, u32 fi, D3D12_GPU_VIRTUAL_ADDRESS cb,
                                         D3D12_GPU_VIRTUAL_ADDRESS ptCb) {
    RtEntities ents;
    bool found = false;
    for (RenderPass* p : r.passes_[int(PassStage::World)])
        if (p->rtEntities(fi, ents)) {
            found = true;
            break;
        }
    if (!found || ents.refCount + ents.freeCount == 0) return false;
    const u32 quads = std::min<u32>(ents.refCount + ents.freeCount, kEntityQuads);
    if (ents.refCount > quads) ents.refCount = quads;
    ents.freeCount = quads - ents.refCount;
    if (ents.modelQuads != entQuadsSrc_ || ents.frameData != entFrameSrc_) {  // raw views (t6, t7 space4), once
        r.waitIdle();
        entQuadsSrc_ = ents.modelQuads, entFrameSrc_ = ents.frameData;
        ID3D12Resource* res[2] = {ents.modelQuads, ents.frameData};
        for (u32 i = 0; i < 2; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC s{};
            s.Format = DXGI_FORMAT_R32_TYPELESS;
            s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            s.Buffer.NumElements = u32(res[i]->GetDesc().Width / 4);
            s.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            r.device_->CreateShaderResourceView(res[i], &s, r.cpuDescriptor(descBase_ + 6 + i));
        }
    }
    if (ents.atlas && ents.atlas != entAtlasSrc_) {  // t1: the entity atlas (entity hits' albedo)
        r.waitIdle();
        entAtlasSrc_ = ents.atlas;
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture2D.MipLevels = 1;
        r.device_->CreateShaderResourceView(ents.atlas, &s, r.cpuDescriptor(descBase_ + 1));
    }
    auto* k = reinterpret_cast<PtConstants*>(constantsMapped_ + size_t(fi) * kConstantBytes);
    const u32 e[4] = {u32(ents.matrices), u32(ents.instances), u32(ents.refs), ents.refCount};
    const u32 e2[4] = {ents.freeCount, u32(ents.freeQuads), 0, 0};
    std::memcpy(k->entity, e, 16);
    std::memcpy(k->entity2, e2, 16);

    Shading& sh = *r.shading_;
    sh.bindCompute(list, cb);
    list->SetPipelineState(psoEntity_.Get());
    list->SetComputeRootUnorderedAccessView(RootCounts, entityVerts_->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(RootPtSrv, r.gpuDescriptor(descBase_));
    list->SetComputeRootDescriptorTable(RootPtUav, r.gpuDescriptor(descBase_ + kTableSize));
    list->SetComputeRootConstantBufferView(RootPtConstants, ptCb);
    list->Dispatch((quads + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = entityVerts_.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = kUav;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &b);

    // Index buffer: 65536 quads at most per geometry.
    D3D12_RAYTRACING_GEOMETRY_DESC geo[(kEntityQuads + 65535) / 65536]{};
    u32 n = 0;
    for (u32 first = 0; first < quads; first += 65536, ++n) {
        const u32 count = std::min(quads - first, 65536u);
        D3D12_RAYTRACING_GEOMETRY_DESC& g = geo[n];
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;  // alpha tested (rtEntityOpaque): cutout texels are holes
        g.Triangles.IndexBuffer = r.indexBuffer_->GetGPUVirtualAddress();
        g.Triangles.IndexCount = count * 6;
        g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        g.Triangles.VertexBuffer = {entityVerts_->GetGPUVirtualAddress() + u64(first) * 48, 12};
        g.Triangles.VertexCount = count * 4;
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    in.NumDescs = n;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.pGeometryDescs = geo;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    if (!entityBlas_ || entityBlasBytes_ < info.ResultDataMaxSizeInBytes) {
        retire(entityBlas_);
        entityBlasBytes_ = info.ResultDataMaxSizeInBytes * 3 / 2;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = entityBlasBytes_;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!check(r.device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kAs, nullptr, IID_PPV_ARGS(&entityBlas_)),
                   "entity BLAS"))
            return false;
    }
    if (info.ScratchDataSizeInBytes > entityScratchBytes_) {
        retire(entityScratch_);
        entityScratchBytes_ = info.ScratchDataSizeInBytes * 3 / 2;
        entityScratch_ = r.createBuffer(entityScratchBytes_, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }
    if (!entityScratch_) return false;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
    d.DestAccelerationStructureData = entityBlas_->GetGPUVirtualAddress();
    d.Inputs = in;
    d.ScratchAccelerationStructureData = entityScratch_->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = entityBlas_.Get();
    list->ResourceBarrier(1, &uav);
    entityQuads_ = quads;
    return true;
}

std::string Renderer::PathTracer::status() const {
    char b[256];
    std::snprintf(b, sizeof(b), "%s | %u sections traced (radius %.0f), %.0f MB acceleration structures, %u light sections%s",
                  rrUsed_ ? "denoiser DLSS Ray Reconstruction" : "built-in denoiser", instanceCount_, radius_,
                  double(asBytes_) / 1048576.0, lightSectionCount_, queued_ ? ", building..." : "");
    return b;
}
