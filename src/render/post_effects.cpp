#include "render/post_effects.h"

#include "BitsPS.h"
#include "BoxBlurPS.h"
#include "ColorConvolvePS.h"
#include "InvertPS.h"
#include "PostBlitPS.h"
#include "PostVS.h"
#include "RotScaleVS.h"
#include "SpiderClipPS.h"

#include <algorithm>

namespace {

bool check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    LOG("post effects: %s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    return false;
}

enum Shader { ShConvolve, ShBits, ShBlur, ShSpiderClip, ShInvert, ShBlit };  // (Sh: PostEffects::Invert is the effect)
enum Target { Main, Swap, Temp, LargeBlur, SmallBlur, Screen = -1 };
struct Pass {
    Shader shader;
    Target in, blur, out;
    float p[16];
};
// post_effect/creeper.json
const Pass kCreeper[] = {
    {ShConvolve, Main, Main, Swap, {0, 0, 0, 0, 0.3f, 0.59f, 0.11f, 0, 0, 0, 0, 0}},
    {ShBits, Swap, Swap, Screen, {16, 4}},
};
// post_effect/spider.json
const Pass kSpider[] = {
    {ShBlur, Main, Main, Temp, {1, 0, 15}},
    {ShBlur, Temp, Temp, LargeBlur, {0, 1, 15}},
    {ShBlur, Main, Main, Temp, {1, 0, 7}},
    {ShBlur, Temp, Temp, SmallBlur, {0, 1, 7}},
    {ShSpiderClip, Main, LargeBlur, Temp, {1.25f, 2.0f, -0.125f, -0.1f, 0, 0, 0, 0, 0, 0, 1, 1, 0.1f, 0.1f, 0.9f, 0.9f}},
    {ShSpiderClip, SmallBlur, Temp, Swap, {2.35f, 4.2f, -1.1f, -1.5f, -45, 0, 0, 0, 0.21f, 0, 0.79f, 1, 0.31f, 0.1f, 0.69f, 0.9f}},
    {ShSpiderClip, SmallBlur, Swap, Temp, {2.35f, 4.2f, 0.45f, -4.45f, 45, 0, 0, 0, 0.21f, 0, 0.79f, 1, 0.31f, 0.1f, 0.69f, 0.9f}},
    {ShSpiderClip, SmallBlur, Temp, Swap, {2.35f, 2.35f, -0.385f, -1.29f, 0, 0, 0, 0, 0, 0, 1, 1, 0.31f, 0.1f, 0.69f, 0.9f}},
    {ShSpiderClip, SmallBlur, Swap, Temp, {2.35f, 2.35f, -0.965f, -1.29f, 0, 0, 0, 0, 0, 0, 1, 1, 0.31f, 0.1f, 0.69f, 0.9f}},
    {ShBlit, Temp, Temp, Screen, {1.0f, 0.8f, 0.8f, 1.0f}},
};
// post_effect/invert.json
const Pass kInvert[] = {
    {ShInvert, Main, Main, Swap, {0.8f}},
    {ShBlit, Swap, Swap, Screen, {1, 1, 1, 1}},
};
// post_effect/blur.json: three horizontal / vertical box blur pairs of radius 0 (the menu blur radius)
const Pass kBlur[] = {
    {ShBlur, Main, Main, Swap, {1, 0, 0}}, {ShBlur, Swap, Swap, Main, {0, 1, 0}}, {ShBlur, Main, Main, Swap, {1, 0, 0}},
    {ShBlur, Swap, Swap, Main, {0, 1, 0}}, {ShBlur, Main, Main, Swap, {1, 0, 0}}, {ShBlur, Swap, Swap, Screen, {0, 1, 0}},
};

}  // namespace

PostEffects::Effect PostEffects::byName(std::string_view id) {
    if (id.starts_with("minecraft:")) id.remove_prefix(10);
    return id == "creeper" ? Creeper : id == "spider" ? Spider : id == "invert" ? Invert : id == "blur" ? Blur : None;
}

bool PostEffects::create(Renderer& r) {
    r_ = &r;
    ID3D12Device* device = r.device();
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0}};
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 20;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (int i = 0; i < 2; ++i) {
        params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_STATIC_SAMPLER_DESC samp[2]{};
    for (int i = 0; i < 2; ++i) {
        samp[i].Filter = i ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp[i].AddressU = samp[i].AddressV = samp[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp[i].MaxLOD = D3D12_FLOAT32_MAX;
        samp[i].ShaderRegister = UINT(i);
        samp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rd{3, params, 2, samp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        !check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&sig_)), "CreateRootSignature"))
        return false;
    auto pso = [&](const void* vs, size_t vsSize, const void* ps, size_t psSize, ComPtr<ID3D12PipelineState>& out) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = sig_.Get();
        d.VS = {vs, vsSize};
        d.PS = {ps, psSize};
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        return check(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)), "CreateGraphicsPipelineState");
    };
#define SH(n) g_##n, sizeof(g_##n)
    if (!pso(SH(PostVS), SH(ColorConvolvePS), pso_[ShConvolve]) || !pso(SH(PostVS), SH(BitsPS), pso_[ShBits]) ||
        !pso(SH(PostVS), SH(BoxBlurPS), pso_[ShBlur]) || !pso(SH(RotScaleVS), SH(SpiderClipPS), pso_[ShSpiderClip]) ||
        !pso(SH(PostVS), SH(InvertPS), pso_[ShInvert]) || !pso(SH(PostVS), SH(PostBlitPS), pso_[ShBlit]))
        return false;
#undef SH
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kTargets, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    return check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs_)), "CreateDescriptorHeap");
}

void PostEffects::draw(const PassContext& ctx) {
    ++frame_;
    std::erase_if(retired_, [&](const auto& r) { return frame_ > r.first + Renderer::kFrames + 1; });
    std::erase(effects_, None);
    if (effects_.empty() || !sig_ || !ctx.sceneColor || !ctx.colorRtv.ptr || !ctx.width || !ctx.height) return;
    ID3D12Device* device = r_->device();
    ID3D12GraphicsCommandList* list = ctx.list;
    const u32 w = ctx.width, h = ctx.height;
    const UINT rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtvOf = [&](int i) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvs_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += size_t(i) * rtvStride;
        return rtv;
    };
    if (w > w_ || h > h_) {  // the targets only grow (the old ones live until the GPU is past them)
        const u32 nw = std::max(w, w_), nh = std::max(h, h_);
        const u32 srv = r_->allocDescriptors(kTargets);
        if (srv == UINT32_MAX) return;
        for (int i = 0; i < kTargets; ++i) {
            if (tex_[i]) retired_.push_back({frame_, tex_[i]});
            tex_[i].Reset();
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = nw, td.Height = nh, td.DepthOrArraySize = 1, td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (!check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                       IID_PPV_ARGS(&tex_[i])),
                       "CreateCommittedResource")) {
                w_ = h_ = 0;
                return;
            }
            state_[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(tex_[i].Get(), &sd, r_->cpuDescriptor(srv + u32(i)));
            device->CreateRenderTargetView(tex_[i].Get(), nullptr, rtvOf(i));
        }
        srv_ = srv, w_ = nw, h_ = nh;
    }
    auto barrier = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        if (from == to) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from, b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
    };
    auto toState = [&](int i, D3D12_RESOURCE_STATES s) {
        barrier(tex_[i].Get(), state_[i], s);
        state_[i] = s;
    };
    // minecraft:main: a copy of the finished scene.
    barrier(ctx.sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    toState(Main, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{tex_[Main].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_TEXTURE_COPY_LOCATION src{ctx.sceneColor, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    const D3D12_BOX box{0, 0, 0, w, h, 1};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    barrier(ctx.sceneColor, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    const D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {r_->descriptorHeap()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(sig_.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (size_t e = 0; e < effects_.size(); ++e) {
    const Effect effect = effects_[e];
    const bool last = e + 1 == effects_.size();  // earlier chains write back into minecraft:main
    const Pass* passes = effect == Creeper ? kCreeper : effect == Spider ? kSpider : effect == Blur ? kBlur : kInvert;
    const size_t count = effect == Creeper ? std::size(kCreeper) : effect == Spider ? std::size(kSpider) : effect == Blur ? std::size(kBlur) : std::size(kInvert);
    for (size_t i = 0; i < count; ++i) {
        Pass p = passes[i];
        if (p.out == Screen && !last) p.out = Main;
        if (effect == Blur && p.shader == ShBlur) p.p[2] = blurRadius_;
        toState(p.in, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        toState(p.blur, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        D3D12_CPU_DESCRIPTOR_HANDLE target = ctx.colorRtv;  // the last pass replaces the scene
        if (p.out != Screen) toState(p.out, D3D12_RESOURCE_STATE_RENDER_TARGET), target = rtvOf(p.out);
        list->OMSetRenderTargets(1, &target, FALSE, nullptr);
        float k[20] = {float(w), float(h), float(w) / float(w_), float(h) / float(h_)};
        std::copy(p.p, p.p + 16, k + 4);
        list->SetGraphicsRoot32BitConstants(0, 20, k, 0);
        list->SetGraphicsRootDescriptorTable(1, r_->gpuDescriptor(srv_ + u32(p.in)));
        list->SetGraphicsRootDescriptorTable(2, r_->gpuDescriptor(srv_ + u32(p.blur)));
        list->SetPipelineState(pso_[p.shader].Get());
        list->DrawInstanced(3, 1, 0, 0);
    }
    }
    list->OMSetRenderTargets(1, &ctx.colorRtv, FALSE, ctx.dsv.ptr ? &ctx.dsv : nullptr);
}
