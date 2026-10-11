// GPU side of the block entity renderer: one arena of baked section quads (DEFAULT heap, filled by copies from the
// per-frame upload slice), per-frame dynamic quads and visible-section lists, and six draws (cutout, text and
// translucent; static and dynamic each). The vertex shader finds a quad's section by binary search over the
// visible list, so culling costs one small upload instead of a draw call per section.
#include "blockentity/block_entities.h"

#include "entity/entity_render.h"
#include "render/pack_sink.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <shared_mutex>

#include "BlockEntityPS.h"
#include "BlockEntityTranslucentPS.h"
#include "BlockEntityVS.h"

namespace be {
namespace {

constexpr u64 kArenaQuads = u64(1) << 18;                   // 29 MB of baked section quads
constexpr u32 kMaxVis = 8192;                               // visible sections per pass
constexpr u32 kMaxDyn = 1 << 15;                            // per-frame quads of animating block entities
constexpr u64 kStageBytes = u64(8) << 20;                   // section quads + font rows uploaded per frame
constexpr u64 kVisOffset = 256;
constexpr u64 kDynOffset = kVisOffset + u64(kMaxVis) * 3 * 32;
constexpr u64 kStageOffset = (kDynOffset + u64(kMaxDyn) * sizeof(BeQuad) + 511) & ~u64(511);
constexpr u64 kFrameBytes = (kStageOffset + kStageBytes + 65535) & ~u64(65535);
constexpr float kViewDistance = 64.0f;

struct VisBatch {
    float origin[3];  // section origin - camera
    u32 first;        // first quad in the arena
    u32 prefix;       // quads of the earlier batches in this list
    u32 pad[3];
};
static_assert(sizeof(VisBatch) == 32);

bool check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    LOG("block entities: %s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    return false;
}

}  // namespace

bool BlockEntityRenderer::create(Renderer& r) {
    renderer_ = &r;
    ID3D12Device* device = r.device();
    if (entityRenderer_.atlasDescriptor() == UINT32_MAX) {
        LOG("block entities: the entity atlas is unavailable; block entities will not be drawn");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    for (int i = 0; i < 3; ++i) {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = UINT(3 + i);  // t3 atlas, t4 font, t5 block textures
    }
    D3D12_ROOT_PARAMETER params[8]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b0
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b1
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.Num32BitValues = 4;
    for (int i = 0; i < 3; ++i) {  // t0 arena, t1 dynamic quads, t2 visible sections
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2 + i].Descriptor.ShaderRegister = UINT(i);
        params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    for (int i = 0; i < 3; ++i) {
        params[5 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[5 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[5 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[5 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_STATIC_SAMPLER_DESC samp[2]{};
    samp[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp[0].AddressU = samp[0].AddressV = samp[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp[0].MaxLOD = D3D12_FLOAT32_MAX;
    samp[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samp[1] = samp[0];
    samp[1].Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    samp[1].AddressU = samp[1].AddressV = samp[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp[1].ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 8;
    rd.pParameters = params;
    rd.NumStaticSamplers = 2;
    rd.pStaticSamplers = samp;
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        LOG("block entities: root signature: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    if (!check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig_)),
               "CreateRootSignature"))
        return false;

    auto pso = [&](const void* ps, size_t psSize, bool blend, D3D12_CULL_MODE cull, ComPtr<ID3D12PipelineState>& out) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = rootSig_.Get();
        d.VS = {g_BlockEntityVS, sizeof(g_BlockEntityVS)};
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
        d.RasterizerState.FrontCounterClockwise = TRUE;  // the game's (OpenGL) counter-clockwise winding
        d.RasterizerState.DepthClipEnable = TRUE;
        d.DepthStencilState.DepthEnable = TRUE;
        d.DepthStencilState.DepthWriteMask = blend ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reversed Z; coplanar layers draw in order
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        d.SampleDesc.Count = 1;
        return check(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)), "CreateGraphicsPipelineState");
    };
    if (!pso(g_BlockEntityPS, sizeof(g_BlockEntityPS), false, D3D12_CULL_MODE_BACK, psoCutout_) ||
        !pso(g_BlockEntityPS, sizeof(g_BlockEntityPS), false, D3D12_CULL_MODE_NONE, psoText_) ||
        !pso(g_BlockEntityTranslucentPS, sizeof(g_BlockEntityTranslucentPS), true, D3D12_CULL_MODE_BACK, psoTranslucent_))
        return false;

    auto buffer = [&](u64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp{heap};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = size;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(&out)),
                     "CreateCommittedResource");
    };
    if (!buffer(kArenaQuads * sizeof(BeQuad), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arena_) ||
        !buffer(kFrameBytes * Renderer::kFrames, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, upload_))
        return false;
    D3D12_RANGE none{0, 0};
    if (!check(upload_->Map(0, &none, reinterpret_cast<void**>(&mapped_)), "Map")) return false;
    alloc_.reset(kArenaQuads);

    // Font atlas: created with the glyphs baked so far; later glyphs arrive as dirty rows.
    if (font_) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = u32(fontAtlas_.width());
        td.Height = u32(fontAtlas_.height());
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        if (!check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&fontTex_)),
                   "CreateCommittedResource(font)") ||
            !r.uploadTexture(fontTex_.Get(), {fontAtlas_.data()}))
            return false;
        int y0, y1;
        while (fontAtlas_.takeDirty(y0, y1, 1 << 16)) {}
        fontUploaded_ = true;
    }
    fontDescriptor_ = r.allocDescriptors(1);
    if (fontDescriptor_ == UINT32_MAX) {
        LOG("block entities: no free descriptor for the font atlas");
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(fontTex_.Get(), &sd, r.cpuDescriptor(fontDescriptor_));  // null view without a font
    world_.trackBlockChanges(true);  // edits from now on reach updateTracking
    return true;
}

// Rebuilds dirty sections into the arena (within the frame's upload budget) and font rows into the font texture.
void BlockEntityRenderer::uploadSections(const PassContext& ctx) {
    u8* stage = mapped_ + u64(ctx.frame) * kFrameBytes + kStageOffset;
    const u64 stageVa = u64(ctx.frame) * kFrameBytes + kStageOffset;
    u64 used = 0;
    struct Copy {
        u64 dst, src, size;
    };
    std::vector<Copy> copies;
    for (auto& [key, s] : sections_) {
        if (!s.dirty) continue;
        for (auto& v : scratch_) v.clear();
        buildSection(s, scratch_);
        const u64 total = scratch_[0].size() + scratch_[1].size() + scratch_[2].size();
        const u64 bytes = total * sizeof(BeQuad);
        if (used + bytes > kStageBytes) {
            if (used) break;  // next frame
            if (!arenaFull_) LOG("block entities: a section needs %llu quads, more than one upload holds", (unsigned long long)total);
            arenaFull_ = true;
            s.dirty = false;
            continue;
        }
        u64 offset = 0;
        if (total) {
            offset = alloc_.alloc(total);
            if (offset == RangeAllocator::kFail) {
                if (!arenaFull_) LOG("block entities: quad arena full (%llu quads); some block entities are not drawn",
                                     (unsigned long long)kArenaQuads);
                arenaFull_ = true;
                s.dirty = false;
                continue;
            }
            u64 at = used;
            for (const auto& v : scratch_) {
                std::memcpy(stage + at, v.data(), v.size() * sizeof(BeQuad));
                at += v.size() * sizeof(BeQuad);
            }
            copies.push_back({offset * sizeof(BeQuad), stageVa + used, bytes});
            used += bytes;
        }
        if (s.allocated) frees_.push_back({s.offset, s.allocated, frame_});
        s.offset = offset, s.allocated = total;
        for (int i = 0; i < 3; ++i) s.counts[i] = u32(scratch_[i].size());
        s.dirty = false;
    }
    // Font rows (glyphs baked since the last upload).
    struct Rows {
        int y0, y1;
        u64 src;
    };
    std::vector<Rows> rows;
    if (fontTex_) {
        const u64 pitch = u64(fontAtlas_.width()) * 4;
        int y0, y1;
        used = (used + 511) & ~u64(511);
        while (used + pitch < kStageBytes && fontAtlas_.takeDirty(y0, y1, int((kStageBytes - used) / pitch))) {
            for (int y = y0; y < y1; ++y) std::memcpy(stage + used + u64(y - y0) * pitch, fontAtlas_.data() + u64(y) * pitch, pitch);
            rows.push_back({y0, y1, stageVa + used});
            used = (used + u64(y1 - y0) * pitch + 511) & ~u64(511);
        }
    }
    if (copies.empty() && rows.empty()) return;
    ID3D12GraphicsCommandList* list = ctx.list;
    auto barrier = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
    };
    if (!copies.empty()) {
        barrier(arena_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const Copy& c : copies) list->CopyBufferRegion(arena_.Get(), c.dst, upload_.Get(), c.src, c.size);
        barrier(arena_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (!rows.empty()) {
        barrier(fontTex_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const Rows& r : rows) {
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = fontTex_.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = upload_.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = r.src;
            src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, u32(fontAtlas_.width()), u32(r.y1 - r.y0), 1,
                                             u32(fontAtlas_.width()) * 4};
            list->CopyTextureRegion(&dst, 0, u32(r.y0), 0, &src, nullptr);
        }
        barrier(fontTex_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}

void BlockEntityRenderer::draw(const PassContext& ctx) {
    Timer timer;
    if (!mapped_) return;
    ++frame_;
    // Ranges freed by edits are reused once the frames that drew them are done.
    for (size_t i = 0; i < frees_.size();) {
        if (frees_[i].frame + Renderer::kFrames < frame_) {
            alloc_.free(frees_[i].offset, frees_[i].count);
            frees_[i] = frees_.back();
            frees_.pop_back();
        } else {
            ++i;
        }
    }
    cam_[0] = ctx.camX, cam_[1] = ctx.camY, cam_[2] = ctx.camZ;
    const double now = nowSeconds();
    if (lastTime_ == 0) lastTime_ = now;
    tickClock_ += std::clamp(now - lastTime_, 0.0, 0.5);
    lastTime_ = now;

    u8* frameBase = mapped_ + u64(ctx.frame) * kFrameBytes;
    auto vp = [&](int r, int c) { return ctx.viewProj[c * 4 + r]; };
    float planes[5][4];
    for (int c = 0; c < 4; ++c) {
        planes[0][c] = vp(3, c) + vp(0, c);
        planes[1][c] = vp(3, c) - vp(0, c);
        planes[2][c] = vp(3, c) + vp(1, c);
        planes[3][c] = vp(3, c) - vp(1, c);
        planes[4][c] = vp(3, c) - vp(2, c);
    }
    Emit dyn{{&dyn_[0], &dyn_[1], &dyn_[2]}, {ctx.camX, ctx.camY, ctx.camZ}, {}, {}, {}};
    for (int r = 0; r < 3; ++r) {  // camera right, up, forward (world space)
        const int row = r == 2 ? 3 : r;
        float l = 0;
        for (int c = 0; c < 3; ++c) l += vp(row, c) * vp(row, c);
        l = std::sqrt(std::max(l, 1e-12f));
        float* out = r == 0 ? dyn.camRight : r == 1 ? dyn.camUp : dyn.camFwd;
        for (int c = 0; c < 3; ++c) out[c] = vp(row, c) / l;
    }
    VisBatch* vis = reinterpret_cast<VisBatch*>(frameBase + kVisOffset);
    u32 visCount[3] = {}, quadTotal[3] = {};
    for (auto& v : dyn_) v.clear();
    stats_ = {};
    {
        std::shared_lock lock(world_.mutex());
        applyNetwork();
        int ticks = 0;
        while (tickClock_ >= 0.05 && ticks < 10) tickClock_ -= 0.05, ++ticks, tick();
        if (tickClock_ >= 0.05) tickClock_ = 0;
        partial_ = float(std::clamp(tickClock_ / 0.05, 0.0, 1.0));
        updateTracking(ctx.camX, ctx.camY, ctx.camZ);
        refreshLight();
        Timer build;
        uploadSections(ctx);
        if (build.ms() > 4.0) LOG("block entities: rebuilt sections in %.2f ms", build.ms());

        for (auto& [key, s] : sections_) {
            stats_.tracked += u32(s.list.size());
            ++stats_.sections;
            stats_.staticQuads += s.counts[0] + s.counts[1] + s.counts[2];
            if (s.list.empty()) continue;
            const double ox = double(s.cx) * 16 - ctx.camX, oy = double(s.sy) * 16 - ctx.camY, oz = double(s.cz) * 16 - ctx.camZ;
            const float lo[3] = {float(ox) - 1, float(oy) - 1, float(oz) - 1};
            const float hi[3] = {float(ox) + 17, float(oy) + 19, float(oz) + 17};
            float d2 = 0;
            for (int a = 0; a < 3; ++a) {
                const float v = std::max({lo[a], -hi[a], 0.0f});
                d2 += v * v;
            }
            if (d2 > kViewDistance * kViewDistance) continue;
            bool inside = true;
            for (const auto& pl : planes) {
                const float px = pl[0] >= 0 ? hi[0] : lo[0], py = pl[1] >= 0 ? hi[1] : lo[1], pz = pl[2] >= 0 ? hi[2] : lo[2];
                if (pl[0] * px + pl[1] * py + pl[2] * pz + pl[3] < 0) {
                    inside = false;
                    break;
                }
            }
            if (!inside) continue;
            ++stats_.visibleSections;
            if (ctx.pack) {  // shader pack: all instances as vertices (no GPU section batches)
                for (const Instance& in : s.list) {
                    const double dx = in.x + 0.5 - ctx.camX, dy = in.y + 0.5 - ctx.camY, dz = in.z + 0.5 - ctx.camZ;
                    if (dx * dx + dy * dy + dz * dz > double(kViewDistance) * kViewDistance) continue;
                    packInstance(*ctx.pack, dyn, in, in.data >= 0 ? &s.data[size_t(in.data)] : nullptr,
                                 isDynamic(in, def(in.block)) ? partial_ : 0.0f);
                }
                continue;
            }
            u32 first = u32(s.offset);
            for (int p = 0; p < 3; ++p) {
                if (s.counts[p] && visCount[p] < kMaxVis) {
                    VisBatch& b = vis[p * kMaxVis + visCount[p]++];
                    b.origin[0] = float(ox), b.origin[1] = float(oy), b.origin[2] = float(oz);
                    b.first = first, b.prefix = quadTotal[p];
                    quadTotal[p] += s.counts[p];
                }
                first += s.counts[p];
            }
            // Animating block entities: rebuilt every frame, camera-relative.
            for (u32 i : s.dynamic) {
                if (i >= s.list.size()) continue;
                const Instance& in = s.list[i];
                const double dx = in.x + 0.5 - ctx.camX, dy = in.y + 0.5 - ctx.camY, dz = in.z + 0.5 - ctx.camZ;
                if (dx * dx + dy * dy + dz * dz > double(kViewDistance) * kViewDistance) continue;
                if (dyn_[0].size() + dyn_[1].size() + dyn_[2].size() > kMaxDyn - 4096) break;
                emitInstance(dyn, in, in.data >= 0 ? &s.data[size_t(in.data)] : nullptr, partial_);
                ++stats_.dynamic;
            }
        }
    }
    if (ctx.pack) {  // drawn by the shader pack (packInstance)
        stats_.cpuMs = timer.ms();
        cpuTotal_ += stats_.cpuMs;
        historyMs_[frames_ % kHistory] = float(stats_.cpuMs);
        ++frames_;
        return;
    }
    // Dynamic quads: [cutout][text][translucent].
    u32 dynFirst[3], dynCount[3], at = 0;
    for (int p = 0; p < 3; ++p) {
        dynFirst[p] = at;
        dynCount[p] = u32(std::min<size_t>(dyn_[p].size(), kMaxDyn - at));
        std::memcpy(frameBase + kDynOffset + u64(at) * sizeof(BeQuad), dyn_[p].data(), u64(dynCount[p]) * sizeof(BeQuad));
        at += dynCount[p];
    }
    stats_.dynamicQuads = at;

    struct Constants {
        float viewProj[16];
        float fogColor[4];
        float fogParams[4];  // fog start, fog end, daylight, banner wave time (ticks mod 100)
        float light0[4], light1[4];
        float lightmap[3][4];
    } k{};
    std::memcpy(k.lightmap, ctx.lightmap, sizeof(k.lightmap));
    std::memcpy(k.viewProj, ctx.viewProj, sizeof(k.viewProj));
    std::memcpy(k.fogColor, ctx.fogColor, sizeof(k.fogColor));
    k.fogParams[0] = ctx.fogStart, k.fogParams[1] = ctx.fogEnd, k.fogParams[2] = ctx.daylight;
    k.fogParams[3] = float(gameTicks_ % 100) + partial_;
    const float l0[3] = {0.2f, 1.0f, -0.7f}, l1[3] = {-0.2f, 1.0f, 0.7f};
    const float n0 = 1.0f / std::sqrt(0.04f + 1.0f + 0.49f);
    for (int a = 0; a < 3; ++a) k.light0[a] = l0[a] * n0, k.light1[a] = l1[a] * n0;
    std::memcpy(frameBase, &k, sizeof(k));

    stats_.cpuMs = timer.ms();
    cpuTotal_ += stats_.cpuMs;
    historyMs_[frames_ % kHistory] = float(stats_.cpuMs);
    ++frames_;
    if (!quadTotal[0] && !quadTotal[1] && !quadTotal[2] && !at) return;

    ID3D12GraphicsCommandList* list = ctx.list;
    const D3D12_GPU_VIRTUAL_ADDRESS va = upload_->GetGPUVirtualAddress() + u64(ctx.frame) * kFrameBytes;
    ID3D12DescriptorHeap* heaps[] = {renderer_->descriptorHeap()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(rootSig_.Get());
    list->SetGraphicsRootConstantBufferView(0, va);
    list->SetGraphicsRootShaderResourceView(2, arena_->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(3, va + kDynOffset);
    list->SetGraphicsRootShaderResourceView(4, va + kVisOffset);
    list->SetGraphicsRootDescriptorTable(5, renderer_->gpuDescriptor(entityRenderer_.atlasDescriptor()));
    list->SetGraphicsRootDescriptorTable(6, renderer_->gpuDescriptor(fontDescriptor_));
    list->SetGraphicsRootDescriptorTable(7, ctx.blockTextures);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12PipelineState* psos[3] = {psoCutout_.Get(), psoText_.Get(), psoTranslucent_.Get()};
    for (int p = 0; p < 3; ++p) {
        if (!quadTotal[p] && !dynCount[p]) continue;
        list->SetPipelineState(psos[p]);
        if (quadTotal[p]) {
            const u32 c[4] = {u32(p) * kMaxVis, visCount[p], 0, 0};
            list->SetGraphicsRoot32BitConstants(1, 4, c, 0);
            list->DrawInstanced(quadTotal[p] * 6, 1, 0, 0);
        }
        if (dynCount[p]) {
            const u32 c[4] = {dynFirst[p], 0, 1, 0};
            list->SetGraphicsRoot32BitConstants(1, 4, c, 0);
            list->DrawInstanced(dynCount[p] * 6, 1, 0, 0);
        }
    }
}

// ---- shader packs ----

void BlockEntityRenderer::packInstance(PackSink& pack, const Emit& base, const Instance& in, const Data* data, float pt) {
    Emit e = base;  // camera-relative quads
    for (int p = 0; p < 3; ++p) packQuads_[p].clear(), e.out[p] = &packQuads_[p];
    emitInstance(e, in, data, pt);
    const int id = pack.blockId(in.block);
    // [pass][texture]: textures 0 entity atlas, 1 font, 2 solid (white), 3 block texture (block atlas)
    static thread_local std::vector<PackVertex> verts[3][4];
    for (int p = 0; p < 3; ++p) {
        for (const BeQuad& q : packQuads_[p]) {
            const int t = q.tex == kTexAtlas ? 0 : q.tex == kTexFont ? 1 : q.tex == kTexSolid ? 2 : 3;
            const bool bright = (q.light & (256 | 1024)) != 0;
            const u32 sky = bright ? 15 : (q.light >> 4) & 15, block = bright ? 15 : q.light & 15;
            PackVertex v{};
            v.color = q.color;
            v.light = (block << 4) | ((sky << 4) << 16);
            v.overlay = 10u << 16;
            const float n[3] = {float(i8(q.normal & 255)) / 127.0f, float(i8((q.normal >> 8) & 255)) / 127.0f,
                                float(i8((q.normal >> 16) & 255)) / 127.0f};
            v.normal = packNormal(n[0], n[1], n[2]);
            v.entity = -1;
            float uv[4][2];
            for (int c = 0; c < 4; ++c) {
                uv[c][0] = q.uv[c][0], uv[c][1] = q.uv[c][1];
                if (t == 3 && !pack.atlasUv(q.tex, q.uv[c][0], q.uv[c][1], uv[c])) uv[c][0] = uv[c][1] = 0;
            }
            v.midUv[0] = (uv[0][0] + uv[2][0]) * 0.5f, v.midUv[1] = (uv[0][1] + uv[2][1]) * 0.5f;
            for (int c = 0; c < 4; ++c) {
                std::copy(q.pos[c], q.pos[c] + 3, v.pos);
                v.uv[0] = uv[c][0], v.uv[1] = uv[c][1];
                verts[p][t].push_back(v);
            }
        }
    }
    for (int p = 0; p < 3; ++p) {
        for (int t = 0; t < 4; ++t) {
            std::vector<PackVertex>& v = verts[p][t];
            if (v.empty()) continue;
            PackDraw d;
            d.role = p == 2 ? PackRole::BlockTranslucent : PackRole::Block;
            d.blend = p == 2 ? PackBlend::Translucent : PackBlend::Opaque;
            d.cull = false;
            d.blockAtlas = t == 3;
            d.texture = t == 0 ? entityRenderer_.atlasTexture() : t == 1 ? fontTex_.Get() : nullptr;
            d.vertices = v.data(), d.vertexCount = u32(v.size());
            d.blockEntityId = id;
            pack.draw(d);
            v.clear();
        }
    }
}

}  // namespace be
