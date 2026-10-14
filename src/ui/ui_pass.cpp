#include "ui/ui_pass.h"

#include "UiPS.h"
#include "UiVS.h"

namespace ui {

namespace {
bool ok(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    LOG("ui: %s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    return false;
}

ComPtr<ID3D12Resource> uploadBuffer(ID3D12Device* device, u64 size) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                    IID_PPV_ARGS(&r));
    return r;
}

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}
}  // namespace

bool UiPass::createTexture(Atlas& atlas, ComPtr<ID3D12Resource>& tex, u32 descriptor) {
    ID3D12Device* device = renderer_->device();
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = u64(atlas.width());
    td.Height = u32(atlas.height());
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    if (!ok(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&tex)),
            "atlas texture"))
        return false;
    if (!renderer_->uploadTexture(tex.Get(), {atlas.data()})) return false;
    int y0, y1;
    while (atlas.takeDirty(y0, y1, atlas.height())) {
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = td.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(tex.Get(), &srv, renderer_->cpuDescriptor(descriptor));
    return true;
}

bool UiPass::create(Renderer& renderer) {
    renderer_ = &renderer;
    ID3D12Device* device = renderer.device();
    descriptors_ = renderer.allocDescriptors(2);
    if (descriptors_ == UINT32_MAX) return ok(E_OUTOFMEMORY, "descriptor allocation");
    if (!createTexture(gui_, guiTex_, descriptors_) || !createTexture(icons_, iconTex_, descriptors_ + 1)) return false;

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 12;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 2;
    rd.pParameters = params;
    rd.NumStaticSamplers = 1;
    rd.pStaticSamplers = &sampler;
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, error;
    if (!ok(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), "UI root signature") ||
        !ok(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)),
            "UI root signature"))
        return false;

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"MODE", 0, DXGI_FORMAT_R32_UINT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_.Get();
    d.VS = {g_UiVS, sizeof(g_UiVS)};
    d.PS = {g_UiPS, sizeof(g_UiPS)};
    d.InputLayout = {layout, 4};
    auto& rt = d.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.DepthStencilState.DepthEnable = FALSE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    d.SampleDesc.Count = 1;
    if (!ok(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso_)), "UI pipeline")) return false;
    rt.SrcBlend = D3D12_BLEND_INV_DEST_COLOR;  // RenderPipelines.CROSSHAIR-style inverting blend
    rt.DestBlend = D3D12_BLEND_INV_SRC_COLOR;
    if (!ok(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&psoInvert_)), "UI pipeline")) return false;
    rt.SrcBlend = D3D12_BLEND_SRC_COLOR;  // RenderPipelines.GLINT: src * src + dst
    rt.DestBlend = D3D12_BLEND_ONE;
    rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
    rt.DestBlendAlpha = D3D12_BLEND_ONE;
    if (!ok(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&psoGlint_)), "UI pipeline")) return false;
    rt.SrcBlend = D3D12_BLEND_ZERO;  // RenderPipelines.VIGNETTE: dst * (1 - src colour), alpha kept
    rt.DestBlend = D3D12_BLEND_INV_SRC_COLOR;
    if (!ok(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&psoVignette_)), "UI pipeline")) return false;
    rt.SrcBlend = D3D12_BLEND_ONE;  // BlendFunction.ADDITIVE (GUI_NAUSEA_OVERLAY)
    rt.DestBlend = D3D12_BLEND_ONE;
    if (!ok(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&psoAdditive_)), "UI pipeline")) return false;
    rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;  // RenderPipelines.MOJANG_LOGO: SRC_ALPHA, ONE
    if (!ok(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&psoLogo_)), "UI pipeline")) return false;

    vertices_ = uploadBuffer(device, kVertexBytes * Renderer::kFrames);
    upload_ = uploadBuffer(device, kUploadBytes * Renderer::kFrames);
    if (!vertices_ || !upload_) return ok(E_OUTOFMEMORY, "UI buffers");
    vertices_->Map(0, nullptr, reinterpret_cast<void**>(&vertexMapped_));
    upload_->Map(0, nullptr, reinterpret_cast<void**>(&uploadMapped_));
    return true;
}

void UiPass::upload(const PassContext& ctx, Atlas& atlas, ID3D12Resource* tex, u64& used) {
    const u64 pitch = u64(atlas.width()) * 4;  // a multiple of 256 for the atlas widths used
    const int maxRows = int((kUploadBytes - used) / pitch);
    int y0, y1;
    if (maxRows <= 0 || !atlas.takeDirty(y0, y1, maxRows)) return;
    const u64 offset = u64(ctx.frame) * kUploadBytes + used;
    for (int y = y0; y < y1; ++y)
        std::memcpy(uploadMapped_ + offset + u64(y - y0) * pitch, atlas.data() + u64(y) * pitch, size_t(pitch));
    used += u64(y1 - y0) * pitch;
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upload_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = offset;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, u32(atlas.width()), u32(y1 - y0), 1, u32(pitch)};
    barrier(ctx.list, tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    ctx.list->CopyTextureRegion(&dst, 0, u32(y0), 0, &src, nullptr);
    barrier(ctx.list, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void UiPass::draw(const PassContext& ctx) {
    // With a separate UI layer the batches that read the scene (inverting crosshair, vignette, additive overlays)
    // draw onto the final image before the layer goes over it (BeforeUi); otherwise everything draws in the Hud stage.
    // A blur mark (a menu's background): what comes before it draws onto the final image, which is then blurred, and
    // the rest goes on as usual (GuiRenderer: blurBeforeThisStratum).
    const bool afterUi = ctx.stage == PassStage::BeforeUi;
    const int split = blur && ctx.uiBlur ? canvas_.blurBatch() : -1;
    if (ctx.stage == PassStage::AfterUi || (afterUi && !ctx.uiLayer)) return;
    u64 used = 0;
    if (!afterUi) {
        upload(ctx, gui_, guiTex_.Get(), used);
        upload(ctx, icons_, iconTex_.Get(), used);
    }

    const auto& verts = canvas_.vertices();
    if (verts.empty()) return;
    const u64 bytes = std::min<u64>(verts.size() * sizeof(UiVertex), kVertexBytes);
    const u64 offset = u64(ctx.frame) * kVertexBytes;
    std::memcpy(vertexMapped_ + offset, verts.data(), size_t(bytes));
    const u32 maxVerts = u32(bytes / sizeof(UiVertex));

    ID3D12GraphicsCommandList* list = ctx.list;
    ID3D12DescriptorHeap* heaps[] = {renderer_->descriptorHeap()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(root_.Get());
    list->SetPipelineState(pso_.Get());
    u8 blend = 0;
    const float consts[12] = {2.0f / float(ctx.width), 2.0f / float(ctx.height), glint[4], glint[5], glint[0], glint[1],
                              glint[2], glint[3], std::max(1.0f, glint[6]), glint[7], 0, 0};
    list->SetGraphicsRoot32BitConstants(0, 12, consts, 0);
    list->SetGraphicsRootDescriptorTable(1, renderer_->gpuDescriptor(descriptors_));
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv{vertices_->GetGPUVirtualAddress() + offset, u32(bytes), sizeof(UiVertex)};
    list->IASetVertexBuffers(0, 1, &vbv);
    const D3D12_RECT full{0, 0, LONG(ctx.width), LONG(ctx.height)};
    auto bind = [&] {
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(root_.Get());
        list->SetGraphicsRoot32BitConstants(0, 12, consts, 0);
        list->SetGraphicsRootDescriptorTable(1, renderer_->gpuDescriptor(descriptors_));
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &vbv);
        list->SetPipelineState(blend == 1   ? psoInvert_.Get()
                               : blend == 2 ? psoGlint_.Get()
                               : blend == 3 ? psoVignette_.Get()
                               : blend == 4 ? psoAdditive_.Get()
                               : blend == 5 ? psoLogo_.Get()
                                            : pso_.Get());
    };
    const bool blurHere = split >= 0 && (!ctx.uiLayer || afterUi);  // onto the final image
    const auto& batches = canvas_.batches();
    for (size_t bi = 0; bi < batches.size(); ++bi) {
        const Canvas::Batch& b = batches[bi];
        if (blurHere && int(bi) == split) {
            const D3D12_VIEWPORT vp{0, 0, float(ctx.width), float(ctx.height), 0, 1};
            blur(ctx);
            list->RSSetViewports(1, &vp);
            bind();
        }
        if (!b.count || b.first >= maxVerts) continue;
        const bool special = b.blend == 1 || b.blend == 3 || b.blend == 4;
        // with a layer: before the mark everything goes onto the final image (BeforeUi), after it the special ones
        if (ctx.uiLayer && ((split >= 0 && int(bi) < split) || special) != afterUi) continue;
        D3D12_RECT r = full;
        if (b.clip) {
            r = {LONG(b.scissor[0]), LONG(b.scissor[1]), LONG(b.scissor[2]), LONG(b.scissor[3])};
            r.left = std::clamp<LONG>(r.left, 0, full.right), r.right = std::clamp<LONG>(r.right, 0, full.right);
            r.top = std::clamp<LONG>(r.top, 0, full.bottom), r.bottom = std::clamp<LONG>(r.bottom, 0, full.bottom);
            if (r.right <= r.left || r.bottom <= r.top) continue;
        }
        if (b.blend != blend) {
            blend = b.blend;
            list->SetPipelineState(blend == 1   ? psoInvert_.Get()
                                   : blend == 2 ? psoGlint_.Get()
                                   : blend == 3 ? psoVignette_.Get()
                                   : blend == 4 ? psoAdditive_.Get()
                                   : blend == 5 ? psoLogo_.Get()
                                                : pso_.Get());
        }
        list->RSSetScissorRects(1, &r);
        list->DrawInstanced(std::min(b.count, maxVerts - b.first), 1, b.first, 0);
    }
    list->RSSetScissorRects(1, &full);
}

}  // namespace ui
