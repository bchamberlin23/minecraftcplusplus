#include "sky/sky_gpu.h"

#include "render/pack_sink.h"

#include "SkyCloudPS.h"
#include "SkyBorderPS.h"
#include "SkyCloudVS.h"
#include "SkyDarkDiscPS.h"
#include "SkyDiscPS.h"
#include "SkyFullscreenVS.h"
#include "SkyGeoPS.h"
#include "SkyGeoVS.h"
#include "SkyWeatherPS.h"

#include <algorithm>
#include <cmath>

namespace sky {

namespace {

// Must match cbuffer SkyFrame in sky.hlsl.
struct Constants {
    float viewProj[16];
    float rayX[4], rayY[4], rayZ[4];
    float fogColor[4];
    float skyColor[4];
    float disc[4];
    float cloudColor[4];
    float cloudOffset[4];
    float starRot[12];
    float starBrightness[4];
};
static_assert(sizeof(Constants) <= 256);
static_assert(sizeof(Sky::Frame::Vertex) == 32);

bool ok(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    LOG("sky: %s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    return false;
}

ComPtr<ID3D12Resource> buffer(ID3D12Device* device, u64 size, D3D12_HEAP_TYPE heap) {
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
    const D3D12_RESOURCE_STATES state = heap == D3D12_HEAP_TYPE_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
                                        : heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                                           : D3D12_RESOURCE_STATE_COMMON;
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
    return r;
}

enum class Blend { None, Alpha, Add };

}  // namespace

bool SkyGpu::createTexture(const Image* layers, u32 count, u32 descriptor, ComPtr<ID3D12Resource>& out, u32 mips) {
    ID3D12Device* device = renderer_->device();
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = u64(layers[0].w);
    td.Height = u32(layers[0].h);
    td.DepthOrArraySize = u16(count);
    if (count != 1) mips = 1;
    while (mips > 1 && ((layers[0].w >> (mips - 1)) == 0 || (layers[0].h >> (mips - 1)) == 0)) --mips;
    td.MipLevels = u16(mips);
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    if (!ok(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&out)),
            "texture"))
        return false;
    std::vector<const u8*> subs;
    for (u32 i = 0; i < count; ++i) subs.push_back(layers[i].rgba.data());
    std::vector<std::vector<u8>> chain;  // MipmappedTexture: box-filtered levels (single textures only)
    for (u32 level = 1; level < mips; ++level) {
        const int pw = layers[0].w >> (level - 1), ph = layers[0].h >> (level - 1), w = pw / 2, h = ph / 2;
        const u8* src = level == 1 ? layers[0].rgba.data() : chain.back().data();
        std::vector<u8> dst(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c) {
                    int sum = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) sum += src[(size_t(y * 2 + dy) * pw + (x * 2 + dx)) * 4 + c];
                    dst[(size_t(y) * w + x) * 4 + c] = u8((sum + 2) / 4);
                }
        chain.push_back(std::move(dst));
    }
    for (const std::vector<u8>& level : chain) subs.push_back(level.data());
    if (!renderer_->uploadTexture(out.Get(), subs)) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = td.Format;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (count > 1) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray.MipLevels = 1;
        srv.Texture2DArray.ArraySize = count;
    } else {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = mips;
    }
    device->CreateShaderResourceView(out.Get(), &srv, renderer_->cpuDescriptor(descriptor));
    return true;
}

bool SkyGpu::init(Renderer& renderer, SkyImages images, std::vector<Sky::Frame::Vertex> stars) {
    renderer_ = &renderer;
    ID3D12Device* device = renderer.device();
    descriptors_ = renderer.allocDescriptors(5);
    if (descriptors_ == UINT32_MAX) return ok(E_OUTOFMEMORY, "descriptor allocation");
    for (u32 i = 0; i < 10; ++i) {  // shader packs bind single textures (the srv slot is overwritten below)
        if (!createTexture(&images.celestial[i], 1, descriptors_, celestial2d_[i])) return false;
    }
    starsCpu_ = stars;
    if (!createTexture(images.celestial, 10, descriptors_, celestial_) ||
        !createTexture(&images.endSky, 1, descriptors_ + 1, endSky_) ||
        !createTexture(&images.rain, 1, descriptors_ + 2, rain_) || !createTexture(&images.snow, 1, descriptors_ + 3, snow_) ||
        !createTexture(&images.forcefield, 1, descriptors_ + 4, forcefield_, 5))  // WorldBorderRenderer: mipmapped (4)
        return false;

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 5;
    range.BaseShaderRegister = 2;
    D3D12_ROOT_PARAMETER params[5]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.Num32BitValues = 2;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 1;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges = &range;
    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for (int i = 0; i < 2; ++i) {
        samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        const auto mode = i == 0 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = mode;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = u32(i);
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 5;
    rd.pParameters = params;
    rd.NumStaticSamplers = 2;
    rd.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, error;
    if (!ok(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), "root signature") ||
        !ok(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)),
            "root signature"))
        return false;

    float depthBias = 0;  // DepthStencilState's bias (scale factor and constant alike)
    auto pso = [&](const void* vs, size_t vsSize, const void* ps, size_t psSize, Blend blend, bool depthTest,
                   bool depthWrite, bool cull, ComPtr<ID3D12PipelineState>& out) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root_.Get();
        d.VS = {vs, vsSize};
        d.PS = {ps, psSize};
        auto& rt = d.BlendState.RenderTarget[0];
        rt.BlendEnable = blend != Blend::None;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = blend == Blend::Add ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = blend == Blend::Add ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = cull ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
        d.RasterizerState.FrontCounterClockwise = TRUE;  // the game's quads are counter-clockwise from the front here
        d.RasterizerState.DepthClipEnable = TRUE;
        d.RasterizerState.DepthBias = INT(depthBias);
        d.RasterizerState.SlopeScaledDepthBias = depthBias;
        d.DepthStencilState.DepthEnable = depthTest;
        d.DepthStencilState.DepthWriteMask = depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reversed Z
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        d.SampleDesc.Count = 1;
        return ok(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)), "pipeline");
    };
#define SHADER(name) g_##name, sizeof(g_##name)
    if (!pso(SHADER(SkyFullscreenVS), SHADER(SkyDiscPS), Blend::None, false, false, false, psoSky_) ||
        !pso(SHADER(SkyFullscreenVS), SHADER(SkyDarkDiscPS), Blend::None, false, false, false, psoDarkDisc_) ||
        !pso(SHADER(SkyGeoVS), SHADER(SkyGeoPS), Blend::Alpha, false, false, false, psoBlend_) ||
        !pso(SHADER(SkyGeoVS), SHADER(SkyGeoPS), Blend::Add, false, false, false, psoAdd_) ||
        !pso(SHADER(SkyCloudVS), SHADER(SkyCloudPS), Blend::Alpha, true, true, true, psoClouds_) ||
        !pso(SHADER(SkyGeoVS), SHADER(SkyWeatherPS), Blend::Alpha, true, false, false, psoWeather_))
        return false;
    depthBias = 3.0f;  // RenderPipelines.WORLD_BORDER: DepthStencilState(GREATER_THAN_OR_EQUAL, write, 3, 3)
    if (!pso(SHADER(SkyGeoVS), SHADER(SkyBorderPS), Blend::Add, true, true, false, psoBorder_)) return false;
#undef SHADER

    starVertices_ = u32(stars.size());
    stars_ = buffer(device, stars.size() * sizeof(Sky::Frame::Vertex), D3D12_HEAP_TYPE_DEFAULT);
    if (!stars_ || !renderer.uploadBuffer(stars_.Get(), stars.data(), stars.size() * sizeof(Sky::Frame::Vertex),
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
        return ok(E_OUTOFMEMORY, "star buffer");
    ring_ = buffer(device, kSlotBytes * Renderer::kFrames, D3D12_HEAP_TYPE_UPLOAD);
    cloudStaging_ = buffer(device, u64(kMaxCloudFaces) * 4 * Renderer::kFrames, D3D12_HEAP_TYPE_UPLOAD);
    cloudFaces_ = buffer(device, u64(kMaxCloudFaces) * 4, D3D12_HEAP_TYPE_DEFAULT);
    queryReadback_ = buffer(device, 32 * Renderer::kFrames, D3D12_HEAP_TYPE_READBACK);
    if (!ring_ || !cloudStaging_ || !cloudFaces_ || !queryReadback_) return ok(E_OUTOFMEMORY, "buffers");
    ring_->Map(0, nullptr, reinterpret_cast<void**>(&ringMapped_));
    cloudStaging_->Map(0, nullptr, reinterpret_cast<void**>(&cloudStagingMapped_));
    D3D12_QUERY_HEAP_DESC qh{};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = 4 * Renderer::kFrames;
    device->CreateQueryHeap(&qh, IID_PPV_ARGS(&queries_));
    {  // timestamp frequency of a direct queue (the renderer's queue runs on the same engine)
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> q;
        if (SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) q->GetTimestampFrequency(&timestampFreq_);
    }
    return renderer.addPass(&skyStage_, PassStage::Sky) && renderer.addPass(&lateStage_, PassStage::AfterTranslucent);
}

void SkyGpu::bindCommon(ID3D12GraphicsCommandList* list) {
    ID3D12DescriptorHeap* heaps[] = {renderer_->descriptorHeap()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(root_.Get());
    list->SetGraphicsRootConstantBufferView(0, constantsVa_);
    list->SetGraphicsRootDescriptorTable(4, renderer_->gpuDescriptor(descriptors_));
    list->SetGraphicsRootShaderResourceView(3, cloudFaces_->GetGPUVirtualAddress());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

u32 SkyGpu::putVertices(const std::vector<Sky::Frame::Vertex>& v) {
    const u32 first = ringVertices_;
    const u32 n = u32(std::min<size_t>(v.size(), kMaxDynamicVertices - first));
    if (n) std::memcpy(ringMapped_ + u64(frameIndex_) * kSlotBytes + 256 + u64(first) * 32, v.data(), size_t(n) * 32);
    ringVertices_ += n;
    return first;
}

void SkyGpu::writeConstants(const PassContext& ctx) {
    const Sky::Frame& f = frame_;
    Constants c{};
    std::memcpy(c.viewProj, ctx.viewProj, sizeof(c.viewProj));
    // Rows of the camera-relative view-projection (element (r, c) at m[c * 4 + r]): row 0 = (f / aspect) * right,
    // row 1 = f * up, row 3 = forward. A view ray is forward + x * right * aspect / f + y * up / f.
    auto row = [&](int r, float out[4]) {
        for (int k = 0; k < 4; ++k) out[k] = ctx.viewProj[k * 4 + r];
    };
    float r0[4], r1[4], r3[4];
    row(0, r0), row(1, r1), row(3, r3);
    const float l0 = r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2], l1 = r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2];
    for (int k = 0; k < 3; ++k) c.rayX[k] = r0[k] / l0, c.rayY[k] = r1[k] / l1, c.rayZ[k] = r3[k];
    std::copy(f.fogColor, f.fogColor + 4, c.fogColor);
    std::copy(f.skyColor, f.skyColor + 3, c.skyColor);
    c.skyColor[3] = f.skyEnd;
    c.disc[0] = f.skybox == Skybox::Overworld ? 1.0f : 0.0f;
    c.disc[1] = f.skybox == Skybox::Overworld && f.darkDisc ? 1.0f : 0.0f;
    std::copy(f.cloudColor, f.cloudColor + 4, c.cloudColor);
    std::copy(f.cloudOffset, f.cloudOffset + 3, c.cloudOffset);
    c.cloudOffset[3] = f.cloudFogEnd;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) c.starRot[r * 4 + k] = f.starRot[r * 3 + k];
    c.starBrightness[0] = f.starBrightness;
    std::memcpy(ringMapped_ + u64(frameIndex_) * kSlotBytes, &c, sizeof(c));
    constantsVa_ = ring_->GetGPUVirtualAddress() + u64(frameIndex_) * kSlotBytes;
}

void SkyGpu::drawSky(const PassContext& ctx) {
    if (ctx.pack) return drawPack(ctx, false);
    frameIndex_ = ctx.frame;
    ringVertices_ = 0;
    if (queried_[ctx.frame]) {  // this slot's previous frame has completed
        D3D12_RANGE r{size_t(ctx.frame) * 32, size_t(ctx.frame) * 32 + 32}, none{0, 0};
        u8* p;
        if (SUCCEEDED(queryReadback_->Map(0, &r, reinterpret_cast<void**>(&p)))) {
            u64 t[4];
            std::memcpy(t, p + r.Begin, 32);
            queryReadback_->Unmap(0, &none);
            if (t[1] >= t[0] && t[3] >= t[2] && ++timedFrames_ > 30) {  // skip start-up frames
                const double k = 1000.0 / double(timestampFreq_);
                skyMs_ += double(t[1] - t[0]) * k, lateMs_ += double(t[3] - t[2]) * k;
                gpuMs_ = float((skyMs_ + lateMs_) / double(timedFrames_ - 30));
            }
        }
    }
    writeConstants(ctx);
    ID3D12GraphicsCommandList* list = ctx.list;
    list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, ctx.frame * 4);
    bindCommon(list);
    const D3D12_GPU_VIRTUAL_ADDRESS verts = ring_->GetGPUVirtualAddress() + u64(frameIndex_) * kSlotBytes + 256;
    auto draw = [&](ID3D12PipelineState* pso, const std::vector<Sky::Frame::Vertex>& v, u32 mode) {
        if (v.empty()) return;
        const u32 consts[2] = {putVertices(v), mode};
        list->SetPipelineState(pso);
        list->SetGraphicsRootShaderResourceView(2, verts);
        list->SetGraphicsRoot32BitConstants(1, 2, consts, 0);
        list->DrawInstanced(u32(v.size()), 1, 0, 0);
    };
    const Sky::Frame& f = frame_;
    if (!f.renderSky) {  // boss world fog: the fog colour only
    } else if (f.skybox == Skybox::Overworld) {
        list->SetPipelineState(psoSky_.Get());
        list->DrawInstanced(3, 1, 0, 0);
        draw(psoBlend_.Get(), f.fan, 0);
        draw(psoAdd_.Get(), f.additive, 0);
        if (f.starBrightness > 0.0f) {
            const u32 consts[2] = {0, 1};
            list->SetPipelineState(psoAdd_.Get());
            list->SetGraphicsRootShaderResourceView(2, stars_->GetGPUVirtualAddress());
            list->SetGraphicsRoot32BitConstants(1, 2, consts, 0);
            list->DrawInstanced(starVertices_, 1, 0, 0);
        }
        if (f.darkDisc) {
            list->SetPipelineState(psoDarkDisc_.Get());
            list->DrawInstanced(3, 1, 0, 0);
        }
    } else if (f.skybox == Skybox::End) {
        draw(psoBlend_.Get(), f.endSky, 0);
        draw(psoAdd_.Get(), f.additive, 0);  // the end flash
    }
    list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, ctx.frame * 4 + 1);
}

void SkyGpu::drawLate(const PassContext& ctx) {
    if (ctx.pack) return drawPack(ctx, true);
    ID3D12GraphicsCommandList* list = ctx.list;
    list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, ctx.frame * 4 + 2);
    const Sky::Frame& f = frame_;
    if (f.clouds && f.cloudFaces && f.cloudVersion != cloudVersion_) {  // new cloud mesh: copy it to the GPU buffer
        cloudVersion_ = f.cloudVersion;
        cloudFaceCount_ = u32(std::min<size_t>(f.cloudFaces->size(), kMaxCloudFaces));
        const u64 offset = u64(ctx.frame) * kMaxCloudFaces * 4;
        if (cloudFaceCount_) {
            std::memcpy(cloudStagingMapped_ + offset, f.cloudFaces->data(), size_t(cloudFaceCount_) * 4);
            list->CopyBufferRegion(cloudFaces_.Get(), 0, cloudStaging_.Get(), offset, u64(cloudFaceCount_) * 4);
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = cloudFaces_.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            list->ResourceBarrier(1, &b);
        }
    }
    bindCommon(list);
    if (f.clouds && cloudFaceCount_) {
        list->SetPipelineState(psoClouds_.Get());
        list->DrawInstanced(cloudFaceCount_ * 6, 1, 0, 0);
    }
    if (!f.weather.empty()) {
        const u32 consts[2] = {putVertices(f.weather), 0};
        list->SetPipelineState(psoWeather_.Get());
        list->SetGraphicsRootShaderResourceView(2, ring_->GetGPUVirtualAddress() + u64(frameIndex_) * kSlotBytes + 256);
        list->SetGraphicsRoot32BitConstants(1, 2, consts, 0);
        list->DrawInstanced(u32(std::min<size_t>(f.weather.size(), kMaxDynamicVertices - consts[0])), 1, 0, 0);
    }
    if (!f.border.empty()) {  // RenderPipelines.WORLD_BORDER: additive (BlendFunction.OVERLAY), no culling
        const u32 consts[2] = {putVertices(f.border), 0};
        list->SetPipelineState(psoBorder_.Get());
        list->SetGraphicsRootShaderResourceView(2, ring_->GetGPUVirtualAddress() + u64(frameIndex_) * kSlotBytes + 256);
        list->SetGraphicsRoot32BitConstants(1, 2, consts, 0);
        list->DrawInstanced(u32(std::min<size_t>(f.border.size(), kMaxDynamicVertices - consts[0])), 1, 0, 0);
    }
    list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, ctx.frame * 4 + 3);
    list->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, ctx.frame * 4, 4, queryReadback_.Get(),
                           u64(ctx.frame) * 32);
    queried_[ctx.frame] = true;
}


// ---- shader packs: the vanilla sky geometry handed to the pack's programs (PackSink) ----

void SkyGpu::drawPack(const PassContext& ctx, bool late) {
    PackSink& sink = *ctx.pack;
    const Sky::Frame& f = frame_;
    std::vector<PackVertex> v;
    auto convert = [&](const Sky::Frame::Vertex& s, u32 color) {
        PackVertex p{};
        std::copy(s.pos, s.pos + 3, p.pos);
        p.color = color;
        p.uv[0] = s.uv[0], p.uv[1] = s.uv[1];
        p.light = 240u << 16;
        p.overlay = 10u << 16;
        p.normal = packNormal(0, 1, 0);
        p.entity = -1;
        p.midUv[0] = p.midUv[1] = 0.5f;
        return p;
    };
    auto submit = [&](PackRole role, PackBlend blend, u8 stage, ID3D12Resource* tex, bool depth) {
        if (v.empty()) return;
        PackDraw d;
        d.role = role, d.blend = blend, d.renderStage = stage, d.texture = tex;
        d.depthTest = depth, d.depthWrite = false, d.cull = false, d.quads = false;
        d.vertices = v.data(), d.vertexCount = u32(v.size());
        sink.draw(d);
        v.clear();
    };
    if (!late) {
        if (f.skybox != Skybox::Overworld || !f.renderSky) return;
        for (const auto& s : f.fan) v.push_back(convert(s, s.color));
        submit(PackRole::SkyBasic, PackBlend::Translucent, 2 /* SUNSET */, nullptr, false);
        if (f.starBrightness > 0.0f) {  // the static star field, rotated with the sky
            const u8 b = u8(std::clamp(f.starBrightness, 0.0f, 1.0f) * 255.0f);
            const u32 color = u32(b) | u32(b) << 8 | u32(b) << 16 | u32(b) << 24;
            for (const auto& s : starsCpu_) {
                PackVertex p = convert(s, color);
                for (int r = 0; r < 3; ++r)
                    p.pos[r] = f.starRot[r * 3] * s.pos[0] + f.starRot[r * 3 + 1] * s.pos[1] + f.starRot[r * 3 + 2] * s.pos[2];
                v.push_back(p);
            }
            submit(PackRole::SkyBasic, PackBlend::AdditiveAlpha, 6 /* STARS */, nullptr, false);
        }
        // Sun and moon: one draw per texture (layer 0 sun, 1..8 moon phases).
        for (u32 layer = 0; layer < 9; ++layer) {
            for (const auto& s : f.additive)
                if (s.layer == layer) v.push_back(convert(s, s.color));
            submit(PackRole::SkyTextured, PackBlend::AdditiveAlpha, layer == 0 ? 4 /* SUN */ : 5 /* MOON */,
                   celestial2d_[layer].Get(), false);
        }
        return;
    }
    if (f.clouds && f.cloudFaces && sink.vanillaClouds()) {
        const u32 n = u32(std::min<size_t>(f.cloudFaces->size(), kMaxCloudFaces));
        static const float kShade[6] = {0.7f, 1.0f, 0.8f, 0.8f, 0.9f, 0.9f};
        static const u32 kCorner[6] = {0, 1, 2, 2, 3, 0};
        static const float kNormal[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
        v.reserve(size_t(n) * 6);
        const u32 alpha = u32(std::clamp(f.cloudColor[3], 0.0f, 1.0f) * 255.0f) << 24;
        for (u32 i = 0; i < n; ++i) {
            const u32 face = (*f.cloudFaces)[i];
            const int cx = int(face << 20) >> 20, cz = int(face << 8) >> 20;
            const u32 flags = face >> 24, dir = std::min(flags & 7, 5u);
            const bool inside = (flags & 16) != 0;
            const float shade = kShade[dir];
            auto chan = [&](float c) { return u32(std::clamp(c * shade, 0.0f, 1.0f) * 255.0f); };
            const u32 color = alpha | chan(f.cloudColor[0]) | chan(f.cloudColor[1]) << 8 | chan(f.cloudColor[2]) << 16;
            for (u32 c : kCorner) {
                const u32 index = dir * 4 + (inside ? 3 - c : c);
                PackVertex p{};
                p.pos[0] = float((0xF03CC3u >> index) & 1) * 12.0f + float(cx) * 12.0f + f.cloudOffset[0];
                p.pos[1] = float((0x6666F0u >> index) & 1) * 4.0f + f.cloudOffset[1];
                p.pos[2] = float((0xC3F066u >> index) & 1) * 12.0f + float(cz) * 12.0f + f.cloudOffset[2];
                p.color = color;
                p.light = 240u << 16;
                p.overlay = 10u << 16;
                p.normal = packNormal(kNormal[dir][0], kNormal[dir][1], kNormal[dir][2]);
                p.entity = -1;
                v.push_back(p);
            }
        }
        PackDraw d;
        d.role = PackRole::Clouds, d.blend = PackBlend::Translucent, d.renderStage = 20 /* CLOUDS */;
        d.depthTest = true, d.depthWrite = true, d.cull = false, d.quads = false;
        d.vertices = v.data(), d.vertexCount = u32(v.size());
        sink.draw(d);
        v.clear();
    }
    if (!f.weather.empty()) {
        for (size_t i = 0; i < f.weather.size(); ++i) {
            if (i == f.rainVertices) submit(PackRole::Weather, PackBlend::Translucent, 21 /* RAIN_SNOW */, rain_.Get(), true);
            v.push_back(convert(f.weather[i], f.weather[i].color));
        }
        submit(PackRole::Weather, PackBlend::Translucent, 21, f.rainVertices >= f.weather.size() ? rain_.Get() : snow_.Get(),
               true);
    }
}

}  // namespace sky
