#include "render/renderer.h"
#include "render/shading.h"
#include "render/packs/pack_pipeline.h"
#include "render/upscalers/upscalers.h"

#include "assets/assets.h"
#include "core/math.h"
#include "mesh/mesher.h"

#include <stb/stb_image_write.h>

#include <algorithm>
#include <cmath>

#include "CullCS.h"
#include "DetailPS.h"
#include "DetailVS.h"
#include "LinePS.h"
#include "LineVS.h"
#include "SpritePS.h"
#include "SpriteVS.h"
#include "TerrainPSCutout.h"
#include "TerrainPSOpaque.h"
#include "TerrainPSTranslucent.h"
#include "TerrainVS.h"

namespace {

constexpr u32 kQuadIndexCapacity = 65536;  // quads addressable by one indirect draw
constexpr u32 kCommandBytes = 28;          // 2 root constants + D3D12_DRAW_INDEXED_ARGUMENTS
constexpr u32 kConstantSlotBytes = 512;

// Must match common.hlsli.
struct FrameConstants {
    float viewProj[16];
    float frustum[5][4];
    i32 camBlock[4];
    float camFrac[4];
    float fogColor[4];
    float fogParams[4];
    float tints[8][4];
    u32 segBase[4];
    u32 segCap[4];
    u32 misc[4];
    float lightmap[3][4];
};
static_assert(sizeof(FrameConstants) == 432);

struct GpuSection {
    i32 origin[4];  // xyz in blocks, w = LOD level
    u32 ranges[kRangeCount][2];
};
static_assert(sizeof(GpuSection) == 168);

void setRgb(float* dst, u32 rgb) {
    dst[0] = float((rgb >> 16) & 255) / 255.0f;
    dst[1] = float((rgb >> 8) & 255) / 255.0f;
    dst[2] = float(rgb & 255) / 255.0f;
    dst[3] = 1.0f;
}

bool check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    LOG("error: %s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    return false;
}

}  // namespace

Renderer::Renderer() = default;

Renderer::~Renderer() {
    if (device_) waitIdle();
    if (shading_) shading_->logTimings();
    if (upscalers_) {  // vendor swap chains / contexts go before the device (Streamline: slShutdown first)
        upscalers_->logStats();
        for (auto& b : backbuffers_) b.Reset();
        upscalers_->releaseSwapChain(swapchain_);
        upscalers_->shutdown();
    }
    if (fenceEvent_) CloseHandle(fenceEvent_);
}

ComPtr<ID3D12Resource> Renderer::createBuffer(u64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<u64>(size, 256);
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    D3D12_RESOURCE_STATES state = heap == D3D12_HEAP_TYPE_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
                                  : heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                                     : D3D12_RESOURCE_STATE_COMMON;
    ComPtr<ID3D12Resource> r;
    check(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)),
          "CreateCommittedResource(buffer)");
    return r;
}

void Renderer::transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list_->ResourceBarrier(1, &b);
}

void Renderer::printDebugMessages() {
    if (!infoQueue_) return;
    for (u64 i = 0, n = infoQueue_->GetNumStoredMessages(); i < n; ++i) {
        SIZE_T size = 0;
        infoQueue_->GetMessage(i, nullptr, &size);
        std::vector<u8> buf(size);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (SUCCEEDED(infoQueue_->GetMessage(i, msg, &size)) && msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            LOG("d3d12: %.*s", int(msg->DescriptionByteLength), msg->pDescription);
    }
    infoQueue_->ClearStoredMessages();
}

void Renderer::waitFence(u64 value) {
    if (value && fence_->GetCompletedValue() < value) {
        fence_->SetEventOnCompletion(value, fenceEvent_);
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
}

void Renderer::waitIdle() {
    queue_->Signal(fence_.Get(), ++fenceCounter_);
    waitFence(fenceCounter_);
}

bool Renderer::init(HWND hwnd, u32 width, u32 height, bool debug, bool vsync) {
    width_ = width, height_ = height, vsync_ = vsync, hwnd_ = hwnd;
    if (debug) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    }
    if (!check(CreateDXGIFactory2(debug ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory_)), "CreateDXGIFactory2"))
        return false;

    // Prefer the discrete GPU (this machine also has an iGPU).
    ComPtr<IDXGIAdapter1> adapter;
    for (u32 i = 0; factory_->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                          IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)))) {
            char name[256];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            adapterName_ = name;
            break;
        }
    }
    if (!device_) {
        LOG("error: no D3D12 device");
        return false;
    }
    if (debug) device_.As(&infoQueue_);
    // DLSS / FSR / XeSS: SDKs that hook the queue and swap chain (Streamline) initialise before both exist.
    upscalers_ = std::make_unique<Upscalers>(*this);
    upscalers_->initDevice(adapter.Get());

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!upscalers_->createCommandQueue(qd, queue_) &&
        !check(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)), "CreateCommandQueue"))
        return false;

    if (!createSwapChain(hwnd, width, height)) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kFrames;
    device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 1024;
    device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap_));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap_));
    descriptorCapacity_ = hd.NumDescriptors;
    descriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (auto& a : allocators_)
        if (!check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)),
                   "CreateCommandAllocator"))
            return false;
    device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr, IID_PPV_ARGS(&list_));
    list_->Close();
    device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    createTargets();
    if (!createPipelines()) return false;

    constants_ = createBuffer(kConstantSlotBytes * kFrames * 2, D3D12_HEAP_TYPE_UPLOAD);  // + unjittered overlay copies
    constants_->Map(0, nullptr, reinterpret_cast<void**>(&constantsMapped_));
    activeLists_ = createBuffer(u64(kMaxActive) * 4 * kFrames, D3D12_HEAP_TYPE_UPLOAD);
    activeLists_->Map(0, nullptr, reinterpret_cast<void**>(&activeMapped_));
    remap_ = createBuffer(u64(kRemapCapacity) * 4 * kFrames, D3D12_HEAP_TYPE_UPLOAD);
    remap_->Map(0, nullptr, reinterpret_cast<void**>(&remapMapped_));
    remapCpu_.resize(kRemapCapacity);
    for (u32 i = 0; i < kRemapCapacity; ++i) remapCpu_[i] = i;
    overlay_ = createBuffer(kOverlayBytes * kFrames, D3D12_HEAP_TYPE_UPLOAD);
    overlay_->Map(0, nullptr, reinterpret_cast<void**>(&overlayMapped_));
    staging_ = createBuffer(kStagingBytes, D3D12_HEAP_TYPE_UPLOAD);
    staging_->Map(0, nullptr, reinterpret_cast<void**>(&stagingMapped_));
    counts_ = createBuffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    zeros_ = createBuffer(256, D3D12_HEAP_TYPE_UPLOAD);
    void* z;
    zeros_->Map(0, nullptr, &z);
    std::memset(z, 0, 256);
    zeros_->Unmap(0, nullptr);
    countsReadback_ = createBuffer(256 * kFrames, D3D12_HEAP_TYPE_READBACK);
    timestampReadback_ = createBuffer(16 * kFrames, D3D12_HEAP_TYPE_READBACK);
    D3D12_QUERY_HEAP_DESC qh{};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = 2 * kFrames;
    device_->CreateQueryHeap(&qh, IID_PPV_ARGS(&timestamps_));
    queue_->GetTimestampFrequency(&timestampFreq_);

    // Shared index buffer: quad i uses vertices 4i..4i+3 as (0,1,2)(2,1,3).
    std::vector<u32> indices(size_t(kQuadIndexCapacity) * 6);
    for (u32 q = 0; q < kQuadIndexCapacity; ++q) {
        const u32 pattern[6] = {0, 1, 2, 2, 1, 3};
        for (int k = 0; k < 6; ++k) indices[q * 6 + k] = q * 4 + pattern[k];
    }
    indexBuffer_ = createBuffer(indices.size() * 4, D3D12_HEAP_TYPE_DEFAULT);
    stage(indexBuffer_.Get(), 0, indices.data(), indices.size() * 4);
    flushUploads();
    if (shaderPreset_ != ShaderPreset::Off) setShaderSettings(shaderSettings_);
    return true;
}

// ---- swap chain: everything that creates, resizes or presents it is here (frame generation SDKs wrap / replace the
// factory, the swap chain and Present at these three points; back buffers are re-fetched in createTargets) ----

bool Renderer::createSwapChain(HWND hwnd, u32 width, u32 height) {
    ComPtr<IDXGIFactory5> f5;
    BOOL allowTearing = FALSE;
    if (SUCCEEDED(factory_.As(&f5)))
        f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
    tearing_ = allowTearing;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = kSwapChainFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    // Frame generation SDKs create their proxy swap chain here; otherwise (or if that fails) a plain one.
    if (!upscalers_->createSwapChain(hwnd, sd, swapchain_)) {
        ComPtr<IDXGISwapChain1> sc1;
        if (!check(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1), "CreateSwapChain"))
            return false;
        sc1.As(&swapchain_);
    }
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

// Frame generation switched (or its SDK wants a fresh swap chain): same size, new (proxy) swap chain.
void Renderer::recreateSwapChain() {
    waitIdle();
    for (auto& b : backbuffers_) b.Reset();
    depth_.Reset();
    upscalers_->releaseSwapChain(swapchain_);
    if (!createSwapChain(hwnd_, width_, height_)) LOG("error: could not re-create the swap chain");
    for (u64& f : frameFence_) f = 0;
    createTargets();
    if (shading_) shading_->createTargets();
    if (packs_) packs_->resize();
}

void Renderer::resizeSwapChain(u32 width, u32 height) {
    upscalers_->resizeSwapChain(swapchain_.Get(), width, height, kSwapChainFormat,
                                tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
}

void Renderer::present() { upscalers_->present(swapchain_.Get(), vsync_, tearing_); }

void Renderer::beginFrame() { upscalers_->beginFrame(); }

void Renderer::resetTemporal() {
    if (shading_) shading_->historyValid_ = false;
}

void Renderer::createTargets() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (u32 i = 0; i < kFrames; ++i) {
        swapchain_->GetBuffer(i, IID_PPV_ARGS(&backbuffers_[i]));
        device_->CreateRenderTargetView(backbuffers_[i].Get(), nullptr, {rtv.ptr + size_t(i) * rtvStride_});
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC dd{};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = width_;
    dd.Height = height_;
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_R32_TYPELESS;  // D32 view for depth, R32_FLOAT view for the shader pipeline
    dd.SampleDesc.Count = 1;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth = 0.0f;  // reversed Z
    device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                     IID_PPV_ARGS(&depth_));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(depth_.Get(), &dsvDesc, dsvHeap_->GetCPUDescriptorHandleForHeapStart());
}

void Renderer::resize(u32 width, u32 height) {
    if (width == 0 || height == 0 || (width == width_ && height == height_)) return;
    waitIdle();
    for (auto& b : backbuffers_) b.Reset();
    depth_.Reset();
    resizeSwapChain(width, height);
    width_ = width, height_ = height;
    createTargets();
    if (shading_) shading_->createTargets();
    if (packs_) packs_->resize();
}

bool Renderer::createPipelines() {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 2;

    // Shader pipeline tables: t0..23 space1 (textures), t0 space2 (shadow map array), t0 space3 (materials);
    // u0..23 space1 (storage textures), u0 space2 (state buffer). See shading.hlsli.
    D3D12_DESCRIPTOR_RANGE srvRanges[3]{}, uavRanges[2]{};
    const u32 srvCounts[3] = {Shading::TexCount, 1, 1}, uavCounts[2] = {Shading::RwCount, 1};
    for (u32 i = 0, offset = 0; i < 3; offset += srvCounts[i++]) {
        srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[i].NumDescriptors = srvCounts[i];
        srvRanges[i].RegisterSpace = i + 1;
        srvRanges[i].OffsetInDescriptorsFromTableStart = offset;
    }
    for (u32 i = 0, offset = 0; i < 2; offset += uavCounts[i++]) {
        uavRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRanges[i].NumDescriptors = uavCounts[i];
        uavRanges[i].RegisterSpace = i + 1;
        uavRanges[i].OffsetInDescriptorsFromTableStart = offset;
    }

    D3D12_ROOT_PARAMETER params[14]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b1: quadOffset, sectionIndex
    params[0].Constants.ShaderRegister = 1;
    params[0].Constants.Num32BitValues = 2;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b0: frame constants
    params[1].Descriptor.ShaderRegister = 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t0: quads
    params[2].Descriptor.ShaderRegister = 0;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t1: sections
    params[3].Descriptor.ShaderRegister = 1;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;  // t2: texture array
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges = &range;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u0: counts
    params[5].Descriptor.ShaderRegister = 0;
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u1: indirect args
    params[6].Descriptor.ShaderRegister = 1;
    params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t3: active slot list
    params[7].Descriptor.ShaderRegister = 3;
    params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t4: tint colour table
    params[8].Descriptor.ShaderRegister = 4;
    params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b2: shader pipeline constants
    params[9].Descriptor.ShaderRegister = 2;
    params[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;  // shader pipeline SRVs
    params[10].DescriptorTable.NumDescriptorRanges = 3;
    params[10].DescriptorTable.pDescriptorRanges = srvRanges;
    params[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;  // shader pipeline UAVs
    params[11].DescriptorTable.NumDescriptorRanges = 2;
    params[11].DescriptorTable.pDescriptorRanges = uavRanges;
    params[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b3: per-pass constants
    params[12].Constants.ShaderRegister = 3;
    params[12].Constants.Num32BitValues = 8;
    params[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t5: texture animation remap
    params[13].Descriptor.ShaderRegister = 5;

    D3D12_STATIC_SAMPLER_DESC samplers[4]{};
    D3D12_STATIC_SAMPLER_DESC& samp = samplers[0];
    samp.Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.MaxAnisotropy = 1;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    for (u32 i = 1; i < 4; ++i) {  // s1 shadow comparison, s2 linear clamp, s3 point clamp
        D3D12_STATIC_SAMPLER_DESC& s = samplers[i];
        s.Filter = i == 1   ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                   : i == 2 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
                            : D3D12_FILTER_MIN_MAG_MIP_POINT;
        s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s.ComparisonFunc = i == 1 ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_NEVER;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        s.MaxAnisotropy = 1;
        s.ShaderRegister = i;
        s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 14;
    rd.pParameters = params;
    rd.NumStaticSamplers = 4;
    rd.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        LOG("error: root signature: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    if (!check(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig_)),
               "CreateRootSignature"))
        return false;

    auto graphics = [&](const void* vs, size_t vsSize, const void* ps, size_t psSize, bool blend, bool depthWrite,
                        D3D12_CULL_MODE cull, ComPtr<ID3D12PipelineState>& out,
                        D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
                        bool depthTest = true) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = rootSig_.Get();
        d.VS = {vs, vsSize};
        d.PS = {ps, psSize};
        auto& rt = d.BlendState.RenderTarget[0];
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (blend) {
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = cull;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.DepthStencilState.DepthEnable = depthTest;
        d.DepthStencilState.DepthWriteMask = depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        d.PrimitiveTopologyType = topology;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        d.SampleDesc.Count = 1;
        return check(device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)), "CreateGraphicsPipelineState");
    };
    bool ok = graphics(g_TerrainVS, sizeof(g_TerrainVS), g_TerrainPSOpaque, sizeof(g_TerrainPSOpaque), false, true,
                       D3D12_CULL_MODE_BACK, psoOpaque_) &&
              graphics(g_TerrainVS, sizeof(g_TerrainVS), g_TerrainPSCutout, sizeof(g_TerrainPSCutout), false, true,
                       D3D12_CULL_MODE_BACK, psoCutout_) &&
              graphics(g_TerrainVS, sizeof(g_TerrainVS), g_TerrainPSTranslucent, sizeof(g_TerrainPSTranslucent), true,
                       false, D3D12_CULL_MODE_NONE, psoTranslucent_) &&
              graphics(g_DetailVS, sizeof(g_DetailVS), g_DetailPS, sizeof(g_DetailPS), false, true,
                       D3D12_CULL_MODE_BACK, psoModel_) &&
              graphics(g_LineVS, sizeof(g_LineVS), g_LinePS, sizeof(g_LinePS), true, false, D3D12_CULL_MODE_NONE,
                       psoWorldLines_, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE) &&
              graphics(g_LineVS, sizeof(g_LineVS), g_LinePS, sizeof(g_LinePS), true, false, D3D12_CULL_MODE_NONE,
                       psoScreenLines_, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, false) &&
              graphics(g_SpriteVS, sizeof(g_SpriteVS), g_SpritePS, sizeof(g_SpritePS), true, false,
                       D3D12_CULL_MODE_NONE, psoSprites_, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, false);
    if (!ok) return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
    cd.pRootSignature = rootSig_.Get();
    cd.CS = {g_CullCS, sizeof(g_CullCS)};
    if (!check(device_->CreateComputePipelineState(&cd, IID_PPV_ARGS(&psoCull_)), "CreateComputePipelineState"))
        return false;

    D3D12_INDIRECT_ARGUMENT_DESC args[2]{};
    args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    args[0].Constant.RootParameterIndex = 0;
    args[0].Constant.DestOffsetIn32BitValues = 0;
    args[0].Constant.Num32BitValuesToSet = 2;
    args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC sd{};
    sd.ByteStride = kCommandBytes;
    sd.NumArgumentDescs = 2;
    sd.pArgumentDescs = args;
    return check(device_->CreateCommandSignature(&sd, rootSig_.Get(), IID_PPV_ARGS(&cmdSig_)), "CreateCommandSignature");
}

// Copies bytes into the staging ring; the GPU copy is recorded at the start of the next frame.
// Contiguous writes coalesce into one copy.
void Renderer::stage(ID3D12Resource* dst, u64 dstOffset, const void* src, u64 size) {
    const u8* p = static_cast<const u8*>(src);
    while (size) {
        if (stagingHead_ == kStagingBytes) flushUploads();
        u64 n = std::min(size, kStagingBytes - stagingHead_);
        std::memcpy(stagingMapped_ + stagingHead_, p, n);
        if (!copies_.empty() && copies_.back().dst == dst && copies_.back().dstOffset + copies_.back().size == dstOffset &&
            copies_.back().srcOffset + copies_.back().size == stagingHead_)
            copies_.back().size += n;
        else
            copies_.push_back({dst, dstOffset, stagingHead_, n});
        stagingHead_ += n, dstOffset += n, p += n, size -= n;
    }
}

void Renderer::recordCopies() {
    if (copies_.empty()) return;
    // Buffers decay to COMMON between submissions: COMMON -> COPY_DEST -> COMMON (then promoted on read).
    std::vector<ID3D12Resource*> dsts;
    for (const Copy& c : copies_)
        if (std::find(dsts.begin(), dsts.end(), c.dst) == dsts.end()) dsts.push_back(c.dst);
    for (auto* d : dsts) transition(d, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    for (const Copy& c : copies_) list_->CopyBufferRegion(c.dst, c.dstOffset, staging_.Get(), c.srcOffset, c.size);
    for (auto* d : dsts) transition(d, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    copies_.clear();
}

void Renderer::flushUploads() {
    waitIdle();
    if (!copies_.empty()) {
        allocators_[0]->Reset();
        list_->Reset(allocators_[0].Get(), nullptr);
        recordCopies();
        list_->Close();
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        waitIdle();
    }
    stagingHead_ = 0;
    printDebugMessages();
}

bool Renderer::uploadTextures(const std::vector<u8>& pixels, u32 layers) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kTexSize;
    td.Height = kTexSize;
    td.DepthOrArraySize = u16(layers);
    td.MipLevels = kTexMips;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    if (!check(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&texture_)),
               "CreateCommittedResource(texture array)"))
        return false;

    const u32 subs = layers * kTexMips;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(subs);
    std::vector<UINT> rows(subs);
    std::vector<UINT64> rowBytes(subs);
    UINT64 total = 0;
    device_->GetCopyableFootprints(&td, 0, subs, 0, fp.data(), rows.data(), rowBytes.data(), &total);
    ComPtr<ID3D12Resource> staging = createBuffer(total, D3D12_HEAP_TYPE_UPLOAD);
    u8* mapped;
    staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (u32 l = 0; l < layers; ++l)
        for (u32 m = 0; m < u32(kTexMips); ++m) {
            u32 sub = m + l * kTexMips, size = kTexSize >> m;
            const u8* src = &pixels[size_t(l) * kTexLayerBytes + kTexMipOffset[m]];
            for (u32 y = 0; y < size; ++y)
                std::memcpy(mapped + fp[sub].Offset + size_t(y) * fp[sub].Footprint.RowPitch, src + y * size * 4, size * 4);
        }
    staging->Unmap(0, nullptr);

    waitIdle();
    allocators_[0]->Reset();
    list_->Reset(allocators_[0].Get(), nullptr);
    for (u32 sub = 0; sub < subs; ++sub) {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = texture_.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = sub;
        src.pResource = staging.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp[sub];
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    transition(texture_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    waitIdle();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2DArray.MipLevels = kTexMips;
    srv.Texture2DArray.ArraySize = layers;
    device_->CreateShaderResourceView(texture_.Get(), &srv, srvHeap_->GetCPUDescriptorHandleForHeapStart());
    printDebugMessages();
    return true;
}

bool Renderer::createWorldStorage(u64 cubeQuads, u64 modelQuads, u32 slots) {
    waitIdle();
    cubeQuads_ = createBuffer(cubeQuads * kCubeQuadBytes, D3D12_HEAP_TYPE_DEFAULT);
    modelQuads_ = createBuffer(modelQuads * kModelQuadWords * 4, D3D12_HEAP_TYPE_DEFAULT);
    sections_ = createBuffer(u64(slots) * sizeof(GpuSection), D3D12_HEAP_TYPE_DEFAULT);
    segCap_[0] = segCap_[1] = segCap_[2] = kMaxActive * 6 + 1024;
    segCap_[3] = kMaxActive + 1024;
    segBase_[0] = 0;
    for (int s = 1; s < 4; ++s) segBase_[s] = segBase_[s - 1] + segCap_[s - 1];
    args_ = createBuffer(u64(segBase_[3] + segCap_[3]) * kCommandBytes, D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    colors_ = createBuffer(u64(ColorTable::kCapacity) * 4, D3D12_HEAP_TYPE_DEFAULT);
    colorsUploaded_ = 0;
    if (!cubeQuads_ || !modelQuads_ || !sections_ || !args_ || !colors_) return false;
    cubeAlloc_.reset(cubeQuads);
    modelAlloc_.reset(modelQuads);
    slots_.assign(slots, {});
    freeSlots_.clear();
    slotCapacity_ = slots;
    slotHigh_ = 0;
    active_.clear();
    return true;
}

u32 Renderer::addMesh(const SectionMesh& m, int level, i32 x, i32 y, i32 z) {
    const u64 nc = m.cubeQuads.size(), nm = m.counts[kModelRange];
    if (nc + nm == 0) return kNoSlot;
    u64 co = 0, mo = 0;
    if (nc && (co = cubeAlloc_.alloc(nc)) == RangeAllocator::kFail) return kNoSlot;
    if (nm && (mo = modelAlloc_.alloc(nm)) == RangeAllocator::kFail) {
        if (nc) cubeAlloc_.free(co, nc);
        return kNoSlot;
    }
    u32 slot;
    if (!freeSlots_.empty()) {
        slot = freeSlots_.back();
        freeSlots_.pop_back();
    } else if (slotHigh_ < slotCapacity_) {
        slot = slotHigh_++;
    } else {
        if (nc) cubeAlloc_.free(co, nc);
        if (nm) modelAlloc_.free(mo, nm);
        return kNoSlot;
    }
    slots_[slot] = {co, nc, mo, nm};

    GpuSection g{};
    g.origin[0] = x, g.origin[1] = y, g.origin[2] = z, g.origin[3] = level;
    u64 off = co;
    for (int r = 0; r < kModelRange; ++r) {
        g.ranges[r][0] = u32(off);
        g.ranges[r][1] = m.counts[r];
        off += m.counts[r];
    }
    g.ranges[kModelRange][0] = u32(mo);
    g.ranges[kModelRange][1] = u32(nm);
    if (nc) stage(cubeQuads_.Get(), co * kCubeQuadBytes, m.cubeQuads.data(), nc * kCubeQuadBytes);
    if (nm) stage(modelQuads_.Get(), mo * kModelQuadWords * 4, m.modelQuads.data(), nm * kModelQuadWords * 4);
    stage(sections_.Get(), u64(slot) * sizeof(GpuSection), &g, sizeof(g));
    return slot;
}

void Renderer::removeMesh(u32 slot) {
    if (slot >= slotCapacity_) return;
    Slot& s = slots_[slot];
    if (s.cubeCount) cubeAlloc_.free(s.cubeOffset, s.cubeCount);
    if (s.modelCount) modelAlloc_.free(s.modelOffset, s.modelCount);
    s = {};
    freeSlots_.push_back(slot);
}

u64 Renderer::arenaBytesUsed() const {
    return cubeAlloc_.used() * kCubeQuadBytes + modelAlloc_.used() * kModelQuadWords * 4;
}
u64 Renderer::arenaBytesTotal() const {
    return cubeAlloc_.capacity() * kCubeQuadBytes + modelAlloc_.capacity() * kModelQuadWords * 4;
}

void Renderer::render(const CameraState& cam, float viewDistance, const fs::path* screenshot) {
    if (packs_) packs_->beginFrame();  // switches shader packs (a finished load, or back to the built-in pipeline)
    upscalers_->submitStart();
    const u32 fi = swapchain_->GetCurrentBackBufferIndex();
    waitFence(frameFence_[fi]);

    if (frameFence_[fi]) {
        D3D12_RANGE r{size_t(fi) * 256, size_t(fi) * 256 + 16}, none{0, 0};
        u8* p;
        if (SUCCEEDED(countsReadback_->Map(0, &r, reinterpret_cast<void**>(&p)))) {
            std::memcpy(stats_.commands, p + r.Begin, 16);
            countsReadback_->Unmap(0, &none);
        }
        D3D12_RANGE tr{size_t(fi) * 16, size_t(fi) * 16 + 16};
        if (SUCCEEDED(timestampReadback_->Map(0, &tr, reinterpret_cast<void**>(&p)))) {
            u64 t[2];
            std::memcpy(t, p + tr.Begin, 16);
            timestampReadback_->Unmap(0, &none);
            if (t[1] > t[0]) stats_.gpuMs = float(double(t[1] - t[0]) * 1000.0 / double(timestampFreq_));
        }
        if (shading_) shading_->readTimings(fi);
    }
    const bool packActive = packs_ && packs_->active();  // an OptiFine / Iris shader pack replaces the pipeline
    const bool shaded = !packActive && shading_ && shaderPreset_ != ShaderPreset::Off;
    bool packUi = false;  // a pack with frame generation: UI layer like the built-in pipeline (Packs::frameGenUi)
    if (!shaded) stats_.passCount = 0;

    // Camera-relative matrices: the world is translated by -camera on the GPU in integer
    // block units first, so precision does not degrade far from the origin.
    FrameConstants fc{};
    const float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    Vec3 fwd{-sy * cp, -sp, cy * cp};
    Vec3 right = normalize(cross(fwd, {0, 1, 0}));
    Vec3 up = cross(right, fwd);
    Mat4 proj = perspectiveReverseZ(cam.fovY, float(width_) / float(std::max(height_, 1u)), 0.05f);
    const Mat4 view = viewFromBasis(right, up, fwd);
    const Mat4 vpNoJitter = proj * view;
    float jitter[2] = {0, 0};
    if (shaded) {  // TAA / upscalers: sub-pixel Halton (2, 3) jitter, 8 * (output / render)^2 phases (vendor guidance)
        const u32 phases = upscalers_->jitterPhases(shading_->renderW_, width_);
        const u32 phase = jitterPhase_ = (jitterPhase_ + 1) % phases;
        auto halton = [](u32 i, u32 b) {
            float f = 1, r = 0;
            for (; i > 0; i /= b) f /= float(b), r += f * float(i % b);
            return r;
        };
        jitter[0] = (halton(phase + 1, 2) - 0.5f) * 2.0f / float(shading_->renderW_);
        jitter[1] = (halton(phase + 1, 3) - 0.5f) * 2.0f / float(shading_->renderH_);
        proj(0, 2) = jitter[0], proj(1, 2) = jitter[1];
    }
    Mat4 vp = proj * view;
    std::memcpy(fc.viewProj, vp.m, sizeof(fc.viewProj));
    for (int c = 0; c < 4; ++c) {
        fc.frustum[0][c] = vp(3, c) + vp(0, c);
        fc.frustum[1][c] = vp(3, c) - vp(0, c);
        fc.frustum[2][c] = vp(3, c) + vp(1, c);
        fc.frustum[3][c] = vp(3, c) - vp(1, c);
        fc.frustum[4][c] = vp(3, c) - vp(2, c);
    }
    const double bx = std::floor(cam.x), by = std::floor(cam.y), bz = std::floor(cam.z);
    fc.camBlock[0] = i32(bx), fc.camBlock[1] = i32(by), fc.camBlock[2] = i32(bz);
    fc.camFrac[0] = float(cam.x - bx), fc.camFrac[1] = float(cam.y - by), fc.camFrac[2] = float(cam.z - bz);
    std::memcpy(fc.fogColor, fogColor_, sizeof(fogColor_));
    fc.fogParams[0] = viewDistance * 0.6f;
    fc.fogParams[1] = viewDistance;
    fc.fogParams[2] = viewDistance;
    fc.fogParams[3] = daylight_;
    lightmap_.pack(fc.lightmap);
    const u32 tints[8] = {0xFFFFFF, 0x91BD59, 0x77AB2F, 0x80A755, 0x619961, 0x3F76E4, 0x208030, 0x8B1A00};
    for (int t = 0; t < 8; ++t) setRgb(fc.tints[t], tints[t]);
    std::memcpy(fc.segBase, segBase_, sizeof(segBase_));
    std::memcpy(fc.segCap, segCap_, sizeof(segCap_));
    const u32 activeCount = u32(std::min<size_t>(active_.size(), kMaxActive));
    fc.misc[0] = activeCount;
    {  // texture LOD bias for the upscaler's render scale (common.hlsli sampleBlock)
        const float bias = shaded ? upscalers_->mipBias() : 0.0f;
        std::memcpy(&fc.misc[1], &bias, 4);
    }
    std::memcpy(constantsMapped_ + size_t(fi) * kConstantSlotBytes, &fc, sizeof(fc));
    const D3D12_GPU_VIRTUAL_ADDRESS cb = constants_->GetGPUVirtualAddress() + u64(fi) * kConstantSlotBytes;
    // Overlays (block outline) draw after TAA: unjittered copy of the constants.
    D3D12_GPU_VIRTUAL_ADDRESS overlayCb = cb;
    if (shaded) {
        std::memcpy(fc.viewProj, vpNoJitter.m, sizeof(fc.viewProj));
        std::memcpy(constantsMapped_ + size_t(kFrames + fi) * kConstantSlotBytes, &fc, sizeof(fc));
        overlayCb = constants_->GetGPUVirtualAddress() + u64(kFrames + fi) * kConstantSlotBytes;
        std::memcpy(fc.viewProj, vp.m, sizeof(fc.viewProj));
    }
    Shading::FrameInput shadeIn{&cam, viewDistance, vp, vpNoJitter, fwd, right, up, {jitter[0], jitter[1]}, fi};
    if (shaded) {  // camera for the vendor SDKs (DLSS / FSR / XeSS constants, frame generation)
        upscale::CameraFrame c;
        c.pos[0] = cam.x, c.pos[1] = cam.y, c.pos[2] = cam.z;
        const Vec3 basis[3] = {right, up, fwd};
        for (int k = 0; k < 3; ++k) c.right[k] = (&basis[0].x)[k], c.up[k] = (&basis[1].x)[k], c.fwd[k] = (&basis[2].x)[k];
        c.view = view;
        c.proj = perspectiveReverseZ(cam.fovY, float(width_) / float(std::max(height_, 1u)), 0.05f);
        c.fovY = cam.fovY, c.aspect = float(width_) / float(std::max(height_, 1u));
        c.renderSize[0] = shading_->renderW_, c.renderSize[1] = shading_->renderH_;
        c.outputSize[0] = width_, c.outputSize[1] = height_;
        c.jitterPx[0] = -jitter[0] * 0.5f * float(shading_->renderW_);
        c.jitterPx[1] = jitter[1] * 0.5f * float(shading_->renderH_);
        upscalers_->setCamera(c);
    }
    Timer shadeTimer;
    if (shaded) shading_->prepare(shadeIn);
    double shadeCpu = shadeTimer.ms();
    if (activeCount) std::memcpy(activeMapped_ + size_t(fi) * kMaxActive, active_.data(), size_t(activeCount) * 4);
    const D3D12_GPU_VIRTUAL_ADDRESS activeVa = activeLists_->GetGPUVirtualAddress() + u64(fi) * kMaxActive * 4;
    const bool haveWorld = sections_ && activeCount;
    if (remapSliceVersion_[fi] != remapVersion_) {  // this frame's slice of the texture remap is stale
        std::memcpy(remapMapped_ + size_t(fi) * kRemapCapacity, remapCpu_.data(), size_t(kRemapCapacity) * 4);
        remapSliceVersion_[fi] = remapVersion_;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS remapVa = remap_->GetGPUVirtualAddress() + u64(fi) * kRemapCapacity * 4;

    // Tint colours added since the last frame (meshes staged this frame may use them).
    if (const size_t n = std::min<size_t>(ColorTable::size(), ColorTable::kCapacity); colors_ && n > colorsUploaded_) {
        std::vector<u32> fresh(n - colorsUploaded_);
        ColorTable::read(colorsUploaded_, fresh.size(), fresh.data());
        stage(colors_.Get(), colorsUploaded_ * 4, fresh.data(), fresh.size() * 4);
        colorsUploaded_ = n;
    }

    allocators_[fi]->Reset();
    list_->Reset(allocators_[fi].Get(), nullptr);
    list_->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, fi * 2);
    recordCopies();
    ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
    list_->SetDescriptorHeaps(1, heaps);
    if (shaded) shading_->mark(list_.Get(), "begin");

    if (haveWorld) {
        transition(counts_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list_->CopyBufferRegion(counts_.Get(), 0, zeros_.Get(), 0, 16);
        transition(counts_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(args_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list_->SetComputeRootSignature(rootSig_.Get());
        list_->SetPipelineState(psoCull_.Get());
        list_->SetComputeRootConstantBufferView(1, cb);
        list_->SetComputeRootShaderResourceView(3, sections_->GetGPUVirtualAddress());
        list_->SetComputeRootUnorderedAccessView(5, counts_->GetGPUVirtualAddress());
        list_->SetComputeRootUnorderedAccessView(6, args_->GetGPUVirtualAddress());
        list_->SetComputeRootShaderResourceView(7, activeVa);
        list_->Dispatch((activeCount + 63) / 64, 1, 1);
        transition(counts_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        transition(args_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }

    ID3D12Resource* bb = backbuffers_[fi].Get();
    transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtvHeap_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(fi) * rtvStride_};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_VIEWPORT vpRect{0, 0, float(width_), float(height_), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(width_), LONG(height_)};
    PassContext pc{list_.Get(), fi, width_, height_, fc.viewProj, cam.x, cam.y, cam.z, {}, fc.fogParams[0],
                   fc.fogParams[1], fc.fogParams[3], srvHeap_->GetGPUDescriptorHandleForHeapStart()};
    std::memcpy(pc.fogColor, fogColor_, sizeof(fogColor_));
    pc.shading = shaded;
    pc.textureRemap = remapVa;
    std::memcpy(pc.lightmap, fc.lightmap, sizeof(pc.lightmap));
    if (shaded) shading_->fillPassContext(pc, shadeIn);
    if (!packActive) preparePasses(pc);  // shadow casters: this frame's entity geometry before the shadow maps
    if (shaded) {
        shading_->mark(list_.Get(), "cull");
        shadeTimer = Timer();
        shading_->record(list_.Get(), shadeIn, haveWorld, cb, activeVa, activeCount, pc, bb, rtv);
        shading_->cpuMs_ += shadeCpu + shadeTimer.ms();  // includes the extension passes the pipeline runs
        ++shading_->cpuFrames_;
        // Overlays and the Hud stage draw into the pipeline's UI layer (bound by record), unjittered, then
        // compositeUi blends it over the tonemapped image.
        list_->SetDescriptorHeaps(1, heaps);
        list_->SetGraphicsRootSignature(rootSig_.Get());
        list_->SetGraphicsRootConstantBufferView(1, overlayCb);
        list_->SetGraphicsRootDescriptorTable(4, srvHeap_->GetGPUDescriptorHandleForHeapStart());
        list_->RSSetViewports(1, &vpRect);
        list_->RSSetScissorRects(1, &scissor);
    } else if (packActive) {
        // The pack renders the frame (final -> back buffer); overlays and the Hud stage then draw onto it.
        packs_->record(list_.Get(), {&cam, viewDistance, activeVa, activeCount, haveWorld, fi}, pc, bb, rtv);
        list_->SetDescriptorHeaps(1, heaps);
        list_->SetGraphicsRootSignature(rootSig_.Get());
        list_->SetGraphicsRootConstantBufferView(1, cb);
        list_->SetGraphicsRootDescriptorTable(4, srvHeap_->GetGPUDescriptorHandleForHeapStart());
        list_->RSSetViewports(1, &vpRect);
        list_->RSSetScissorRects(1, &scissor);
        packUi = packs_->frameGenUi();  // frame generation: the overlays and the Hud stage into the pipeline's UI layer
        if (packUi) {
            transition(shading_->ui_.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const D3D12_CPU_DESCRIPTOR_HANDLE ui{shading_->rtvHeap_->GetCPUDescriptorHandleForHeapStart().ptr +
                                                 size_t(5) * shading_->rtvStride_};
            const float clear[4] = {0, 0, 0, 0};
            list_->ClearRenderTargetView(ui, clear, 0, nullptr);
            list_->OMSetRenderTargets(1, &ui, FALSE, &dsv);
        } else {
            list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        }
    } else {
    list_->ClearRenderTargetView(rtv, fogColor_, 0, nullptr);
    list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    list_->RSSetViewports(1, &vpRect);
    list_->RSSetScissorRects(1, &scissor);

    auto bindWorld = [&] {
        list_->SetDescriptorHeaps(1, heaps);
        list_->SetGraphicsRootSignature(rootSig_.Get());
        list_->SetGraphicsRootConstantBufferView(1, cb);
        list_->SetGraphicsRootDescriptorTable(4, srvHeap_->GetGPUDescriptorHandleForHeapStart());
        list_->SetGraphicsRootShaderResourceView(13, remapVa);
        list_->RSSetViewports(1, &vpRect);
        list_->RSSetScissorRects(1, &scissor);
        list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        if (!haveWorld) return;
        list_->SetGraphicsRootShaderResourceView(8, colors_->GetGPUVirtualAddress());
        list_->SetGraphicsRootShaderResourceView(3, sections_->GetGPUVirtualAddress());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_INDEX_BUFFER_VIEW ibv{indexBuffer_->GetGPUVirtualAddress(), kQuadIndexCapacity * 6 * 4, DXGI_FORMAT_R32_UINT};
        list_->IASetIndexBuffer(&ibv);
    };
    if (!passes_[0].empty()) {
        runPasses(PassStage::Sky, pc);
    }
    bindWorld();
    {
        auto pass = [&](ID3D12PipelineState* pso, int seg, ID3D12Resource* quads) {
            if (!haveWorld) return;
            list_->SetPipelineState(pso);
            list_->SetGraphicsRootShaderResourceView(2, quads->GetGPUVirtualAddress());
            list_->ExecuteIndirect(cmdSig_.Get(), segCap_[seg], args_.Get(), u64(segBase_[seg]) * kCommandBytes,
                                   counts_.Get(), u64(seg) * 4);
        };
        pass(psoOpaque_.Get(), 0, cubeQuads_.Get());
        pass(psoCutout_.Get(), 1, cubeQuads_.Get());
        pass(psoModel_.Get(), 3, modelQuads_.Get());
        if (!passes_[1].empty()) {
            runPasses(PassStage::World, pc);
            bindWorld();
        }
        pass(psoTranslucent_.Get(), 2, cubeQuads_.Get());
        if (!passes_[3].empty()) {
            runPasses(PassStage::AfterTranslucent, pc);
            bindWorld();
        }
    }
    }  // vanilla

    // Overlays: vertices go into this frame's slice of the upload buffer.
    {
        u8* base = overlayMapped_ + u64(fi) * kOverlayBytes;
        const D3D12_GPU_VIRTUAL_ADDRESS va = overlay_->GetGPUVirtualAddress() + u64(fi) * kOverlayBytes;
        u64 used = 0;
        auto put = [&](const void* data, u64 bytes) -> D3D12_GPU_VIRTUAL_ADDRESS {
            if (!bytes || used + bytes > kOverlayBytes) return 0;
            std::memcpy(base + used, data, bytes);
            D3D12_GPU_VIRTUAL_ADDRESS at = va + used;
            used += (bytes + 255) & ~u64(255);
            return at;
        };
        auto draw = [&](ID3D12PipelineState* pso, D3D_PRIMITIVE_TOPOLOGY topo, D3D12_GPU_VIRTUAL_ADDRESS at, u32 count,
                        u32 mode) {
            if (!at) return;
            const u32 consts[2] = {0, mode};
            list_->SetPipelineState(pso);
            list_->IASetPrimitiveTopology(topo);
            list_->SetGraphicsRootShaderResourceView(2, at);
            list_->SetGraphicsRoot32BitConstants(0, 2, consts, 0);
            list_->DrawInstanced(count, 1, 0, 0);
        };
        draw(psoWorldLines_.Get(), D3D_PRIMITIVE_TOPOLOGY_LINELIST,
             put(worldLines_.data(), worldLines_.size() * sizeof(LineVertex)), u32(worldLines_.size()), 0);
        draw(psoSprites_.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
             put(sprites_.data(), sprites_.size() * sizeof(SpriteVertex)), u32(sprites_.size()), 1);
        draw(psoScreenLines_.Get(), D3D_PRIMITIVE_TOPOLOGY_LINELIST,
             put(screenLines_.data(), screenLines_.size() * sizeof(LineVertex)), u32(screenLines_.size()), 1);
    }
    pc.uiLayer = (shaded || packUi) && !upscalers_->uiComposedBySwapChain();  // we composite it, then AfterUi on top
    runPasses(PassStage::Hud, pc);
    if (!shaded && !packUi) runPasses(PassStage::AfterUi, pc);
    if (shaded || packUi) {
        upscalers_->beforeUiComposite(list_.Get(), bb);  // frame generation: keep the hudless image
        if (upscalers_->uiComposedBySwapChain()) {       // FSR FG blends the UI layer itself (real + generated frames)
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = shading_->ui_.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
            list_->ResourceBarrier(1, &b);
        } else {
            shading_->compositeUi(list_.Get(), overlayCb, rtv);
            runPasses(PassStage::AfterUi, pc);
        }
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT shotFp{};
    if (screenshot) {
        D3D12_RESOURCE_DESC bd = bb->GetDesc();
        UINT64 total = 0;
        device_->GetCopyableFootprints(&bd, 0, 1, 0, &shotFp, nullptr, nullptr, &total);
        if (!screenshotReadback_ || screenshotReadback_->GetDesc().Width < total)
            screenshotReadback_ = createBuffer(total, D3D12_HEAP_TYPE_READBACK);
        transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = screenshotReadback_.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = shotFp;
        src.pResource = bb;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    } else {
        transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }

    list_->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, fi * 2 + 1);
    list_->ResolveQueryData(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, fi * 2, 2, timestampReadback_.Get(), u64(fi) * 16);
    if (shading_ && shading_->markCount_[fi])
        list_->ResolveQueryData(shading_->queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, fi * Shading::kMaxMarks,
                                shading_->markCount_[fi], shading_->queryReadback_.Get(), u64(fi) * Shading::kMaxMarks * 8);
    if (haveWorld) {
        transition(counts_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list_->CopyBufferRegion(countsReadback_.Get(), u64(fi) * 256, counts_.Get(), 0, 16);
    } else {
        std::memset(stats_.commands, 0, sizeof(stats_.commands));
    }
    if (shaded || packUi) upscalers_->endFrame(list_.Get(), bb);  // frame generation inputs (tags / prepare pass)
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    upscalers_->submitEnd();
    present();
    frameFence_[fi] = ++fenceCounter_;
    queue_->Signal(fence_.Get(), fenceCounter_);
    printDebugMessages();
    // Wrap the staging ring between frames. Callers stage at most ~32 MB per frame, so data at
    // the start of the ring was consumed by frames that have long completed (<= 3 in flight).
    if (stagingHead_ > kStagingBytes - (u64(96) << 20)) stagingHead_ = 0;

    if (screenshot) {
        waitFence(frameFence_[fi]);
        u8* p;
        D3D12_RANGE none{0, 0};
        HRESULT hr = screenshotReadback_->Map(0, nullptr, reinterpret_cast<void**>(&p));
        if (SUCCEEDED(hr)) {
            std::vector<u8> img(size_t(width_) * height_ * 4);
            for (u32 y = 0; y < height_; ++y) {
                std::memcpy(&img[size_t(y) * width_ * 4], p + shotFp.Offset + size_t(y) * shotFp.Footprint.RowPitch,
                            width_ * 4);
                for (u32 x = 0; x < width_; ++x) img[(size_t(y) * width_ + x) * 4 + 3] = 255;
            }
            screenshotReadback_->Unmap(0, &none);
            if (!stbi_write_png(screenshot->string().c_str(), int(width_), int(height_), 4, img.data(), int(width_ * 4)))
                LOG("error: could not write %s", screenshot->string().c_str());
        } else {
            LOG("error: screenshot readback failed (0x%08lx)", static_cast<unsigned long>(hr));
        }
    }
}

u32 Renderer::renderWidth() const {
    if (packs_ && packs_->active()) return packs_->renderSize(false);
    return shading_ && shaderPreset_ != ShaderPreset::Off ? shading_->renderW_ : width_;
}
u32 Renderer::renderHeight() const {
    if (packs_ && packs_->active()) return packs_->renderSize(true);
    return shading_ && shaderPreset_ != ShaderPreset::Off ? shading_->renderH_ : height_;
}

void Renderer::setTextureRemap(const std::vector<u32>& remap) {
    const size_t n = std::min<size_t>(remap.size(), kRemapCapacity);
    if (std::memcmp(remapCpu_.data(), remap.data(), n * 4) == 0) return;
    std::memcpy(remapCpu_.data(), remap.data(), n * 4);
    ++remapVersion_;
}

bool Renderer::addPass(RenderPass* pass, PassStage stage) {
    if (!pass->create(*this)) return false;
    passes_[int(stage)].push_back(pass);
    return true;
}

void Renderer::preparePasses(const PassContext& ctx) {
    PassContext c = ctx;
    RenderPass* done[32];
    u32 n = 0;
    for (int stage : {int(PassStage::Sky), int(PassStage::World), int(PassStage::AfterTranslucent), int(PassStage::Hud),
                      int(PassStage::AfterUi)}) {
        c.stage = PassStage(stage);
        for (RenderPass* p : passes_[stage]) {
            if (std::find(done, done + n, p) != done + n) continue;
            if (n < 32) done[n++] = p;
            p->prepare(c);
        }
    }
}

void Renderer::runShadowPasses(const ShadowContext& sc) {
    for (RenderPass* p : passes_[int(PassStage::World)]) p->drawShadow(sc);
}

void Renderer::runPasses(PassStage stage, const PassContext& ctx) {
    if (passes_[int(stage)].empty()) return;
    PassContext c = ctx;
    c.stage = stage;
    for (RenderPass* p : passes_[int(stage)]) p->draw(c);
}

u32 Renderer::allocDescriptors(u32 count) {
    if (descriptorsUsed_ + count > descriptorCapacity_) return UINT32_MAX;
    const u32 first = descriptorsUsed_;
    descriptorsUsed_ += count;
    return first;
}

D3D12_CPU_DESCRIPTOR_HANDLE Renderer::cpuDescriptor(u32 index) const {
    return {srvHeap_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(index) * descriptorSize_};
}

D3D12_GPU_DESCRIPTOR_HANDLE Renderer::gpuDescriptor(u32 index) const {
    return {srvHeap_->GetGPUDescriptorHandleForHeapStart().ptr + u64(index) * descriptorSize_};
}

void Renderer::submitAndWait() {
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    waitIdle();
}

bool Renderer::uploadTexture(ID3D12Resource* texture, const std::vector<const u8*>& subresources) {
    const D3D12_RESOURCE_DESC td = texture->GetDesc();
    const u32 subs = u32(subresources.size());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(subs);
    std::vector<UINT> rows(subs);
    std::vector<UINT64> rowBytes(subs);
    UINT64 total = 0;
    device_->GetCopyableFootprints(&td, 0, subs, 0, fp.data(), rows.data(), rowBytes.data(), &total);
    ComPtr<ID3D12Resource> staging = createBuffer(total, D3D12_HEAP_TYPE_UPLOAD);
    if (!staging) return false;
    u8* mapped;
    if (FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
    for (u32 i = 0; i < subs; ++i)
        for (u32 y = 0; y < rows[i] * fp[i].Footprint.Depth; ++y)
            std::memcpy(mapped + fp[i].Offset + size_t(y) * fp[i].Footprint.RowPitch,
                        subresources[i] + size_t(y) * rowBytes[i], size_t(rowBytes[i]));
    staging->Unmap(0, nullptr);
    waitIdle();
    allocators_[0]->Reset();
    list_->Reset(allocators_[0].Get(), nullptr);
    for (u32 i = 0; i < subs; ++i) {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = i;
        src.pResource = staging.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp[i];
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    submitAndWait();
    printDebugMessages();
    return true;
}

bool Renderer::uploadBuffer(ID3D12Resource* buffer, const void* data, u64 size, D3D12_RESOURCE_STATES after) {
    ComPtr<ID3D12Resource> staging = createBuffer(size, D3D12_HEAP_TYPE_UPLOAD);
    if (!staging) return false;
    u8* mapped;
    if (FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
    std::memcpy(mapped, data, size_t(size));
    staging->Unmap(0, nullptr);
    waitIdle();
    allocators_[0]->Reset();
    list_->Reset(allocators_[0].Get(), nullptr);
    list_->CopyBufferRegion(buffer, 0, staging.Get(), 0, size);
    if (after != D3D12_RESOURCE_STATE_COPY_DEST) transition(buffer, D3D12_RESOURCE_STATE_COPY_DEST, after);
    submitAndWait();
    printDebugMessages();
    return true;
}
