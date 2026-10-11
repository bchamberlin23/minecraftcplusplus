// Shader pack GPU resources (main thread, beginFrame / resize): render targets, depth copies, shadow maps, the block
// atlas built from the renderer's texture array, default / noise / lightmap / custom textures, images, buffers and the
// per-texture-layer block id table (mc_Entity) from block.properties.
#include "render/packs/pack_programs.h"

#include "assets/asset_source.h"
#include "assets/assets.h"

#include <stb/stb_image.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>

using namespace packs;

namespace {

constexpr D3D12_RESOURCE_STATES kSrv = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

D3D12_CPU_DESCRIPTOR_HANDLE cpuAt(ID3D12DescriptorHeap* h, u32 stride, u32 i) {
    return {h->GetCPUDescriptorHandleForHeapStart().ptr + size_t(i) * stride};
}

DXGI_FORMAT uavCompatible(DXGI_FORMAT f) { return f; }

}  // namespace

bool Renderer::Packs::finalize(Pipe& p, std::string& error) {
    ID3D12Device* dev = r.device_.Get();
    PackDefinition& def = *p.def;
    auto newCpu = [&] { return p.cpuCount < p.cpuCap ? p.cpuCount++ : 0u; };
    {  // GPU timestamps between the pack's passes
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = Pipe::kMaxMarks * Renderer::kFrames;
        if (SUCCEEDED(dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&p.timeQueries))))
            p.timeReadback = r.createBuffer(u64(qh.Count) * 8, D3D12_HEAP_TYPE_READBACK);
        if (!p.timeReadback) p.timeQueries.Reset();
    }
    {  // PackSink quads (fan order, unlike the renderer's strip-order quad indices)
        std::vector<u32> idx(size_t(Pipe::kQuadIndexQuads) * 6);
        for (u32 q = 0; q < Pipe::kQuadIndexQuads; ++q) {
            const u32 pattern[6] = {0, 1, 2, 0, 2, 3};
            for (int i = 0; i < 6; ++i) idx[size_t(q) * 6 + i] = q * 4 + pattern[i];
        }
        p.quadIndex = r.createBuffer(idx.size() * 4, D3D12_HEAP_TYPE_DEFAULT);
        r.stage(p.quadIndex.Get(), 0, idx.data(), idx.size() * 4);
    }
    auto texture2D = [&](u32 w, u32 h, u16 mips, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                         u16 depth = 1, bool is3d = false) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = is3d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = std::max(w, 1u);
        d.Height = std::max(h, 1u);
        d.DepthOrArraySize = depth;
        d.MipLevels = mips;
        d.Format = f;
        d.SampleDesc.Count = 1;
        d.Flags = flags;
        ComPtr<ID3D12Resource> res;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&res));
        return res;
    };
    auto srv2D = [&](Texture& t, DXGI_FORMAT f, u32 mips) {
        if (!t.srv) t.srv = newCpu();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = f;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = mips;
        dev->CreateShaderResourceView(t.res.Get(), &sv, cpuAt(p.cpuHeap.Get(), p.srvStride, t.srv));
    };
    auto upload = [&](Texture& t, u32 w, u32 h, DXGI_FORMAT f, const void* pixels, u32 bpp) {
        t.res = texture2D(w, h, 1, f, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!t.res) return false;
        r.uploadTexture(t.res.Get(), {static_cast<const u8*>(pixels)});
        (void)bpp;
        srv2D(t, f, 1);
        return true;
    };

    // ---- default textures ----
    const u8 white[4] = {255, 255, 255, 255}, normal[4] = {128, 128, 255, 255}, specular[4] = {0, 0, 0, 0};
    const float far1 = 1.0f;
    if (!upload(p.white, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, white, 4) ||
        !upload(p.normals, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, normal, 4) ||
        !upload(p.specular, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, specular, 4) ||
        !upload(p.depthFar, 1, 1, DXGI_FORMAT_R32_FLOAT, &far1, 4))
        return error = "default textures", false;
    {  // noise (Iris: random bytes)
        const u32 n = u32(std::clamp(def.settings.noiseResolution, 1, 4096));
        std::vector<u8> px(size_t(n) * n * 4);
        std::mt19937 rng(0);
        for (size_t i = 0; i < px.size(); ++i) px[i] = u8(rng() & 255);
        upload(p.noise, n, n, DXGI_FORMAT_R8G8B8A8_UNORM, px.data(), 4);
    }
    for (int g = 0; g < 2; ++g) {  // enchantment glint (scrolling UVs wrap: not usable from an atlas)
        std::vector<u8> bytes;
        const char* path = g == 0 ? "assets/minecraft/textures/misc/enchanted_glint_armor.png"
                                  : "assets/minecraft/textures/misc/enchanted_glint_item.png";
        int w = 0, h = 0, n = 0;
        u8* img = AssetSource::client().read(path, bytes)
                      ? stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4)
                      : nullptr;
        if (!img || !upload(p.glint[g], u32(w), u32(h), DXGI_FORMAT_R8G8B8A8_UNORM, img, 4)) p.glint[g] = Texture{};
        if (img) stbi_image_free(img);
    }
    {  // lightmap: 16x16, rewritten every frame from an upload buffer
        p.lightmap.res = texture2D(16, 16, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, kSrv);
        srv2D(p.lightmap, DXGI_FORMAT_R8G8B8A8_UNORM, 1);
        p.lightmapUpload = r.createBuffer(256 * 16 * Renderer::kFrames, D3D12_HEAP_TYPE_UPLOAD);
        p.lightmapUpload->Map(0, nullptr, reinterpret_cast<void**>(&p.lightmapMapped));
    }

    // ---- block atlas from the texture array (one 16x16 tile per layer, 5 mips) ----
    if (r.texture_) {
        const D3D12_RESOURCE_DESC ad = r.texture_->GetDesc();
        const u32 layers = ad.DepthOrArraySize;
        p.atlasTilesPerRow = 64;
        const u32 rows = (layers + p.atlasTilesPerRow - 1) / p.atlasTilesPerRow;
        p.atlasW = p.atlasTilesPerRow * kTexSize, p.atlasH = std::max(1u, rows) * kTexSize;
        p.atlas.res = texture2D(p.atlasW, p.atlasH, kTexMips, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                D3D12_RESOURCE_STATE_COPY_DEST);
        if (!p.atlas.res) return error = "atlas", false;
        r.allocators_[0]->Reset();
        r.list_->Reset(r.allocators_[0].Get(), nullptr);
        r.transition(r.texture_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (u32 l = 0; l < layers; ++l)
            for (u32 m = 0; m < u32(kTexMips); ++m) {
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = p.atlas.res.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = m;
                src.pResource = r.texture_.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = m + l * u32(kTexMips);
                const u32 size = u32(kTexSize) >> m;
                r.list_->CopyTextureRegion(&dst, (l % p.atlasTilesPerRow) * size, (l / p.atlasTilesPerRow) * size, 0, &src, nullptr);
            }
        r.transition(r.texture_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        r.transition(p.atlas.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kSrv);
        r.submitAndWait();
        srv2D(p.atlas, DXGI_FORMAT_R8G8B8A8_UNORM, kTexMips);
    } else {
        p.atlas = p.white;
        p.atlasW = p.atlasH = 16;
    }

    // ---- custom textures ----
    p.custom.resize(def.textures.size());
    for (size_t i = 0; i < def.textures.size(); ++i) {
        const CustomTexture& c = def.textures[i];
        Texture& t = p.custom[i];
        if (c.path.find(':') != std::string::npos) {  // resource location
            if (c.path.find("atlas/blocks") != std::string::npos) t = p.atlas;
            else {
                t = p.white;
                p.warnings.push_back("resource texture not available: " + c.path);
            }
            continue;
        }
        std::vector<u8> bytes;
        if (!def.source.readBytes(c.path, bytes)) {
            t = p.white;
            p.warnings.push_back("missing texture " + c.path);
            continue;
        }
        if (c.raw) {
            const DXGI_FORMAT f = dxgiFormat(c.rawFormat);
            const bool is3d = c.rawDims == 3;
            t.res = texture2D(u32(std::max(c.rawW, 1)), u32(std::max(c.rawH, 1)), 1, f, D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST, u16(is3d ? std::max(c.rawD, 1) : 1), is3d);
            if (!t.res) continue;
            UINT64 total = 0;
            D3D12_RESOURCE_DESC d = t.res->GetDesc();
            dev->GetCopyableFootprints(&d, 0, 1, 0, nullptr, nullptr, nullptr, &total);
            bytes.resize(std::max<size_t>(bytes.size(), size_t(total)));
            r.uploadTexture(t.res.Get(), {bytes.data()});
            t.srv = newCpu();
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = f;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.ViewDimension = is3d ? D3D12_SRV_DIMENSION_TEXTURE3D : D3D12_SRV_DIMENSION_TEXTURE2D;
            if (is3d) sv.Texture3D.MipLevels = 1;
            else sv.Texture2D.MipLevels = 1;
            t.dim = sv.ViewDimension;
            dev->CreateShaderResourceView(t.res.Get(), &sv, cpuAt(p.cpuHeap.Get(), p.srvStride, t.srv));
            continue;
        }
        int w = 0, h = 0, n = 0;
        u8* img = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
        if (!img) {
            t = p.white;
            p.warnings.push_back("cannot decode " + c.path);
            continue;
        }
        upload(t, u32(w), u32(h), DXGI_FORMAT_R8G8B8A8_UNORM, img, 4);
        stbi_image_free(img);
        std::string meta;  // texture.png.mcmeta: {"texture": {"blur": true, "clamp": true}}
        if (def.source.read(c.path + ".mcmeta", meta)) {
            auto flag = [&](const char* key) {
                size_t a = meta.find(key);
                return a != std::string::npos && meta.find("true", a) < meta.find_first_of(",}", a);
            };
            const_cast<CustomTexture&>(c).blur = flag("\"blur\"");
            const_cast<CustomTexture&>(c).clamp = flag("\"clamp\"");
        }
    }

    // ---- buffers (bufferObject.N) ----
    for (u32 i = 0; i < kMaxBuffers; ++i) {
        p.bufferUav[i] = newCpu();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R32_TYPELESS;
        uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        auto it = def.bufferObjects.find(int(i));
        if (it == def.bufferObjects.end()) {
            uv.Buffer.NumElements = 1;
            dev->CreateUnorderedAccessView(nullptr, nullptr, &uv, cpuAt(p.cpuHeap.Get(), p.srvStride, p.bufferUav[i]));
            continue;
        }
        const u64 size = (std::max<u64>(it->second, 16) + 3) & ~u64(3);
        ComPtr<ID3D12Resource> b = r.createBuffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!b) return error = "buffer object " + std::to_string(i) + " (" + std::to_string(size) + " bytes)", false;
        uv.Buffer.NumElements = UINT(size / 4);
        dev->CreateUnorderedAccessView(b.Get(), nullptr, &uv, cpuAt(p.cpuHeap.Get(), p.srvStride, p.bufferUav[i]));
        p.buffers.push_back(b);
    }

    p.startTime = p.lastTime = nowSeconds();
    if (!createTargets(p)) return error = "render targets", false;
    p.layerTableBlocks = 0;
    updateLayerTable(p);
    return true;
}

bool Renderer::Packs::createTargets(Pipe& p) {
    ID3D12Device* dev = r.device_.Get();
    PackDefinition& def = *p.def;
    targetSize(p.width, p.height, p.scaled);
    p.outW = r.width_, p.outH = r.height_;
    auto newCpu = [&] { return p.cpuCount < p.cpuCap ? p.cpuCount++ : 0u; };
    auto res2D = [&](u32 w, u32 h, u16 mips, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                     const D3D12_CLEAR_VALUE* clear, u16 depth = 1, bool is3d = false) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = is3d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = std::max(w, 1u);
        d.Height = std::max(h, 1u);
        d.DepthOrArraySize = depth;
        d.MipLevels = mips;
        d.Format = f;
        d.SampleDesc.Count = 1;
        d.Flags = flags;
        ComPtr<ID3D12Resource> res;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, clear, IID_PPV_ARGS(&res))))
            LOG("error: shader pack: texture %ux%u format %d", w, h, int(f));
        return res;
    };

    // Which buffers exist, which need an alternate (written by a composite-style pass), which are mipmapped.
    bool used[16] = {}, written[16] = {}, mip[16] = {};
    for (const ProgramBin& b : p.programs) {
        auto scan = [&](const StageBin& st) {
            for (const ResRef& ref : st.textures)
                if (ref.kind == Res::Color) used[ref.index] = true;
            for (const ResRef& ref : st.images)
                if (ref.kind == Res::Color) used[ref.index] = true;
        };
        for (const StageBin& v : b.vs) scan(v);
        scan(b.ps);
        scan(b.cs);
        if (!b.shadow)
            for (int db : b.drawBuffers)
                if (db >= 0 && db < 16) {
                    used[db] = true;
                    if (!b.gbuffers && !b.final) written[db] = true;
                }
        for (int m : b.src->mipmapped) mip[m & 15] = true;
        for (const auto& [buf, f] : b.src->flip)
            if (f) written[buf & 15] = true;
    }
    used[0] = true;
    for (int i = 0; i < 16; ++i) {
        Target& t = p.color[i];
        const RenderTargetDesc& d = def.settings.targets[size_t(i)];
        if (!used[i]) {
            t.tex[0].Reset(), t.tex[1].Reset();
            continue;
        }
        t.format = dxgiFormat(d.format);
        t.isInt = formatIsInt(d.format);
        t.w = d.sizeRelative ? u32(std::max(1.0f, std::round(float(p.width) * d.size[0]))) : u32(std::max(1.0f, d.size[0]));
        t.h = d.sizeRelative ? u32(std::max(1.0f, std::round(float(p.height) * d.size[1]))) : u32(std::max(1.0f, d.size[1]));
        t.mips = mip[i] ? u32(std::floor(std::log2(float(std::max(t.w, t.h))))) + 1 : 1;
        t.alt = written[i];
        t.clearEnabled = d.clear;
        if (d.clearColor) t.clear = *d.clearColor;
        else if (i == 1) t.clear = {1, 1, 1, 1};
        else t.clear = {0, 0, 0, 0};  // colortex0: the fog colour, set per frame
        D3D12_CLEAR_VALUE cv{};
        cv.Format = t.format;
        std::copy(t.clear.begin(), t.clear.end(), cv.Color);
        for (int k = 0; k < (t.alt ? 2 : 1); ++k) {
            t.tex[k] = res2D(t.w, t.h, u16(t.mips), t.format,
                             D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_RENDER_TARGET, &cv);
            if (!t.tex[k]) return false;
            t.tex[k]->SetName((L"pack colortex" + std::to_wstring(i) + (k ? L" alt" : L" main")).c_str());
            t.state[k] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (!t.srv[k]) t.srv[k] = newCpu(), t.uav[k] = newCpu(), t.rtv[k] = p.rtvCount++;
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = t.format;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = t.mips;
            dev->CreateShaderResourceView(t.tex[k].Get(), &sv, cpuAt(p.cpuHeap.Get(), p.srvStride, t.srv[k]));
            D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
            uv.Format = t.format;
            uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            dev->CreateUnorderedAccessView(t.tex[k].Get(), nullptr, &uv, cpuAt(p.cpuHeap.Get(), p.srvStride, t.uav[k]));
            dev->CreateRenderTargetView(t.tex[k].Get(), nullptr, cpuAt(p.rtvHeap.Get(), p.rtvStride, t.rtv[k]));
            t.mipSrv[k].resize(t.mips > 1 ? t.mips : 0);
            t.mipUav[k].resize(t.mips > 1 ? t.mips : 0);
            for (u32 m = 0; m < t.mipSrv[k].size(); ++m) {
                if (!t.mipSrv[k][m]) t.mipSrv[k][m] = newCpu(), t.mipUav[k][m] = newCpu();
                D3D12_SHADER_RESOURCE_VIEW_DESC ms = sv;
                ms.Texture2D.MostDetailedMip = m;
                ms.Texture2D.MipLevels = 1;
                dev->CreateShaderResourceView(t.tex[k].Get(), &ms, cpuAt(p.cpuHeap.Get(), p.srvStride, t.mipSrv[k][m]));
                D3D12_UNORDERED_ACCESS_VIEW_DESC mu = uv;
                mu.Texture2D.MipSlice = m;
                dev->CreateUnorderedAccessView(t.tex[k].Get(), nullptr, &mu, cpuAt(p.cpuHeap.Get(), p.srvStride, t.mipUav[k][m]));
            }
        }
    }

    // GL depth copies.
    for (int i = 0; i < 3; ++i) {
        p.depthTex[i] = res2D(p.width, p.height, 1, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kSrv, nullptr);
        if (!p.depthTex[i]) return false;
        p.depthTex[i]->SetName((L"pack depthtex" + std::to_wstring(i)).c_str());
        if (!p.depthSrv[i]) p.depthSrv[i] = newCpu(), p.depthUav[i] = newCpu();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_FLOAT;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(p.depthTex[i].Get(), &sv, cpuAt(p.cpuHeap.Get(), p.srvStride, p.depthSrv[i]));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R32_FLOAT;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(p.depthTex[i].Get(), nullptr, &uv, cpuAt(p.cpuHeap.Get(), p.srvStride, p.depthUav[i]));
    }
    // Upscaling: block textures keep their detail at the lower render size (atlas samplers get a negative mip bias,
    // like the built-in pipeline's sampleBlock).
    if (const float bias = p.scaled ? std::log2(std::min(float(p.width) / float(p.outW), 1.0f)) : 0.0f; bias != p.atlasMipBias) {
        p.atlasMipBias = bias;
        for (size_t i = 0; i < p.samplerKind.size(); ++i) {
            if (p.samplerKind[i] != Samp::AtlasWrap) continue;
            D3D12_SAMPLER_DESC sd{};  // samplerDesc(Samp::AtlasWrap) in pack_pipeline.cpp, plus the bias
            sd.Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sd.MaxLOD = 4;
            sd.MaxAnisotropy = 1;
            sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            sd.MipLODBias = bias;
            dev->CreateSampler(&sd, cpuAt(p.samplerHeap.Get(), p.samplerStride, u32(i)));
        }
    }
    // Upscaling (Pipe::scaled): the pack's own scene depth at the render size, the upscaler's inputs and output.
    p.ownDepth.Reset(), p.upColor.Reset(), p.upDepth.Reset(), p.upMotion.Reset(), p.upOut.Reset();
    if (p.scaled) {
        D3D12_CLEAR_VALUE dc{};
        dc.Format = DXGI_FORMAT_D32_FLOAT;
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        p.ownDepth = res2D(p.width, p.height, 1, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                           D3D12_RESOURCE_STATE_DEPTH_WRITE, &dc);
        p.upDepth = res2D(p.width, p.height, 1, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                          D3D12_RESOURCE_STATE_DEPTH_READ | kSrv, &dc);
        D3D12_CLEAR_VALUE cc{};
        cc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        p.upColor = res2D(p.width, p.height, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrv, &cc);
        p.upMotion = res2D(p.width, p.height, 1, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kSrv, nullptr);
        p.upOut = res2D(p.outW, p.outH, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
        if (!p.ownDepth || !p.upDepth || !p.upColor || !p.upMotion || !p.upOut) return false;
        p.ownDepth->SetName(L"pack scene depth (scaled)");
        p.upDepth->SetName(L"pack upscaler depth"), p.upColor->SetName(L"pack upscaler color");
        p.upMotion->SetName(L"pack upscaler motion"), p.upOut->SetName(L"pack upscaler output");
        dev->CreateDepthStencilView(p.ownDepth.Get(), &dv, cpuAt(p.dsvHeap.Get(), p.dsvStride, Pipe::kOwnDsv));
        dev->CreateDepthStencilView(p.upDepth.Get(), &dv, cpuAt(p.dsvHeap.Get(), p.dsvStride, Pipe::kUpDepthDsv));
        if (!p.upColorSrv) p.upColorSrv = newCpu(), p.upDepthSrv = newCpu(), p.upMotionSrv = newCpu(), p.upMotionUav = newCpu(),
                           p.upOutSrv = newCpu(), p.upColorRtv = p.rtvCount++;
        auto srv = [&](ID3D12Resource* res, DXGI_FORMAT f, u32 slot) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = f;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(res, &sv, cpuAt(p.cpuHeap.Get(), p.srvStride, slot));
        };
        srv(p.upColor.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, p.upColorSrv);
        srv(p.upDepth.Get(), DXGI_FORMAT_R32_FLOAT, p.upDepthSrv);
        srv(p.upMotion.Get(), DXGI_FORMAT_R16G16_FLOAT, p.upMotionSrv);
        srv(p.upOut.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, p.upOutSrv);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R16G16_FLOAT;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(p.upMotion.Get(), nullptr, &uv, cpuAt(p.cpuHeap.Get(), p.srvStride, p.upMotionUav));
        dev->CreateRenderTargetView(p.upColor.Get(), nullptr, cpuAt(p.rtvHeap.Get(), p.rtvStride, p.upColorRtv));
    }
    {  // the scene depth (reversed-Z) as R32F: the renderer's depth buffer, or ownDepth when scaled
        if (!p.sceneDepthSrv) p.sceneDepthSrv = newCpu();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_FLOAT;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(p.sceneDepth(r), &sv, cpuAt(p.cpuHeap.Get(), p.srvStride, p.sceneDepthSrv));
    }
    {  // LDR layer for passes that do not use the pack
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        p.fallbackLayer = res2D(p.width, p.height, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                kSrv, &cv);
        if (p.fallbackLayer) p.fallbackLayer->SetName(L"pack fallback layer");
        p.fallbackState = kSrv;
        if (!p.fallbackSrv) p.fallbackSrv = newCpu(), p.fallbackRtv = p.rtvCount++;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(p.fallbackLayer.Get(), &sv, cpuAt(p.cpuHeap.Get(), p.srvStride, p.fallbackSrv));
        dev->CreateRenderTargetView(p.fallbackLayer.Get(), nullptr, cpuAt(p.rtvHeap.Get(), p.rtvStride, p.fallbackRtv));
    }

    {  // the scene depth the D3D way up, for passes drawing without the pack
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        p.flippedDepth = res2D(p.width, p.height, 1, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                               D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv);
        if (!p.flippedDepth) return false;
        p.flippedDepth->SetName(L"pack flipped depth");
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dev->CreateDepthStencilView(p.flippedDepth.Get(), &dv, cpuAt(p.dsvHeap.Get(), p.dsvStride, p.flippedDsv));
    }

    // Shadow maps (only with a shadow program).
    p.shadowEnabled = def.resolve(Gb::Shadow) || def.resolve(Gb::ShadowSolid) || def.resolve(Gb::ShadowCutout);
    if (p.shadowEnabled && !p.shadowDepth[0]) {
        p.shadowRes = u32(std::clamp(def.settings.shadow.resolution, 64, 16384));
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = 1.0f;
        p.shadowDepth[0] = res2D(p.shadowRes, p.shadowRes, 1, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                 kSrv, &cv);
        p.shadowDepth[1] = res2D(p.shadowRes, p.shadowRes, 1, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_NONE, kSrv, nullptr);
        if (!p.shadowDepth[0] || !p.shadowDepth[1]) return false;
        p.shadowDepth[0]->SetName(L"pack shadowtex0");
        p.shadowDepth[1]->SetName(L"pack shadowtex1");
        for (int i = 0; i < 2; ++i) {
            p.shadowSrv[i] = newCpu();
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_R32_FLOAT;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(p.shadowDepth[i].Get(), &sv, cpuAt(p.cpuHeap.Get(), p.srvStride, p.shadowSrv[i]));
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        p.shadowDsv = 0;
        dev->CreateDepthStencilView(p.shadowDepth[0].Get(), &dv, cpuAt(p.dsvHeap.Get(), p.dsvStride, p.shadowDsv));
        for (int i = 0; i < 2; ++i) {
            Target& t = p.shadowColor[i];
            t.format = dxgiFormat(def.settings.shadow.colorFormat[i]);
            t.isInt = formatIsInt(def.settings.shadow.colorFormat[i]);
            t.w = t.h = p.shadowRes;
            t.mips = def.settings.shadow.colorMipmap[i] ? u32(std::floor(std::log2(float(p.shadowRes)))) + 1 : 1;
            t.clear = def.settings.shadow.colorClearColor[i];
            t.clearEnabled = def.settings.shadow.colorClear[i];
            D3D12_CLEAR_VALUE cv{};
            cv.Format = t.format;
            std::copy(t.clear.begin(), t.clear.end(), cv.Color);
            t.tex[0] = res2D(t.w, t.h, u16(t.mips), t.format,
                             D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_RENDER_TARGET, &cv);
            if (!t.tex[0]) return false;
            t.tex[0]->SetName((L"pack shadowcolor" + std::to_wstring(i)).c_str());
            t.state[0] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            t.srv[0] = newCpu(), t.uav[0] = newCpu(), t.rtv[0] = p.rtvCount++;
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = t.format;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = t.mips;
            dev->CreateShaderResourceView(t.tex[0].Get(), &sv, cpuAt(p.cpuHeap.Get(), p.srvStride, t.srv[0]));
            D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
            uv.Format = t.format;
            uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            dev->CreateUnorderedAccessView(t.tex[0].Get(), nullptr, &uv, cpuAt(p.cpuHeap.Get(), p.srvStride, t.uav[0]));
            dev->CreateRenderTargetView(t.tex[0].Get(), nullptr, cpuAt(p.rtvHeap.Get(), p.rtvStride, t.rtv[0]));
            t.mipSrv[0].resize(t.mips > 1 ? t.mips : 0);
            t.mipUav[0].resize(t.mips > 1 ? t.mips : 0);
            for (u32 m = 0; m < t.mipSrv[0].size(); ++m) {
                t.mipSrv[0][m] = newCpu(), t.mipUav[0][m] = newCpu();
                D3D12_SHADER_RESOURCE_VIEW_DESC ms = sv;
                ms.Texture2D.MostDetailedMip = m;
                ms.Texture2D.MipLevels = 1;
                dev->CreateShaderResourceView(t.tex[0].Get(), &ms, cpuAt(p.cpuHeap.Get(), p.srvStride, t.mipSrv[0][m]));
                D3D12_UNORDERED_ACCESS_VIEW_DESC mu = uv;
                mu.Texture2D.MipSlice = m;
                dev->CreateUnorderedAccessView(t.tex[0].Get(), nullptr, &mu, cpuAt(p.cpuHeap.Get(), p.srvStride, t.mipUav[0][m]));
            }
        }
    }

    // Custom images (screen-relative ones follow the size).
    p.images.resize(def.images.size());
    for (size_t i = 0; i < def.images.size(); ++i) {
        const CustomImage& ci = def.images[i];
        Image& im = p.images[i];
        if (im.res && !ci.relative) continue;
        im.name = ci.name, im.sampler = ci.sampler, im.clear = ci.clear;
        im.format = dxgiFormat(ci.format);
        const bool is3d = ci.dims == 3;
        const u32 w = ci.relative ? u32(std::max(1.0f, float(p.width) * ci.size[0])) : u32(std::max(1.0f, ci.size[0]));
        const u32 h = ci.relative ? u32(std::max(1.0f, float(p.height) * ci.size[1])) : u32(std::max(1.0f, ci.size[1]));
        const u32 d = is3d ? u32(std::max(1.0f, ci.size[2])) : 1;
        im.res = res2D(w, ci.dims == 1 ? 1 : h, 1, im.format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, u16(d), is3d);
        if (!im.res) return false;
        im.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if (!im.srv) im.srv = newCpu(), im.uav = newCpu();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = im.format;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = is3d ? D3D12_SRV_DIMENSION_TEXTURE3D : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (is3d) sv.Texture3D.MipLevels = 1;
        else sv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(im.res.Get(), &sv, cpuAt(p.cpuHeap.Get(), p.srvStride, im.srv));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = im.format;
        uv.ViewDimension = is3d ? D3D12_UAV_DIMENSION_TEXTURE3D : D3D12_UAV_DIMENSION_TEXTURE2D;
        if (is3d) uv.Texture3D.WSize = d;
        dev->CreateUnorderedAccessView(im.res.Get(), nullptr, &uv, cpuAt(p.cpuHeap.Get(), p.srvStride, im.uav));
    }
    for (bool& f : p.flipped) f = false;
    p.havePrev = false;
    return true;
}

// Per texture layer: the block.properties id (mc_Entity.x) and light emission of the block states that use it.
// New block states appear while the world loads, so this runs incrementally.
void Renderer::Packs::updateLayerTable(Pipe& p) {
    if (!registry_ || !r.texture_) return;
    const size_t count = registry_->blockCount();
    if (count == p.layerTableBlocks) return;
    const u32 layers = r.texture_->GetDesc().DepthOrArraySize;
    std::vector<u32>& table = p.layerCpu;
    std::vector<u8>& exact = p.layerExact;
    if (p.layerTableBlocks == 0 || table.size() != size_t(layers) * 4) {
        table.assign(size_t(layers) * 4, 0);
        for (u32 l = 0; l < layers; ++l) table[l * 4] = 0xFFFFFFFFu;
        exact.assign(layers, 0);
        p.layerTableBlocks = 0;
        p.layerTable = r.createBuffer(table.size() * 4, D3D12_HEAP_TYPE_DEFAULT);
        p.layerTableSize = p.layerTable ? layers : 0;
    }
    // Index the id map by block name.
    std::unordered_map<std::string, std::vector<const IdMapEntry*>> byName;
    for (const IdMapEntry& e : p.blockIds) byName[e.name].push_back(&e);
    const BlockInfo* infos = registry_->infos();
    if (p.blockIdByState.size() < count) p.blockIdByState.resize(count, -1);
    for (size_t id = p.layerTableBlocks; id < count; ++id) {
        const std::string& state = registry_->name(u16(id));
        const size_t bracket = state.find('[');
        const std::string base = state.substr(0, bracket);
        int mapped = -1;
        if (auto it = byName.find(base); it != byName.end()) {
            for (const IdMapEntry* e : it->second) {
                bool ok = true;
                for (const auto& [prop, values] : e->states) {
                    std::string_view v = registry_->property(u16(id), prop);
                    if (std::find(values.begin(), values.end(), std::string(v)) == values.end()) ok = false;
                }
                if (ok) mapped = e->id;  // later entries win (Iris)
            }
        }
        p.blockIdByState[id] = mapped;
        const BlockInfo& b = infos[id];
        const std::string shortName = base.substr(base.find(':') + 1);
        auto assign = [&](u32 layer) {
            if (layer >= layers) return;
            const std::string& tn = r.textureNames_.size() > layer ? r.textureNames_[layer] : std::string();
            const bool isExact = tn.size() > 6 && tn.compare(6, std::string::npos, shortName) == 0;
            if (table[layer * 4] != 0xFFFFFFFFu && (exact[layer] || !isExact)) return;
            table[layer * 4] = u32(mapped);
            table[layer * 4 + 1] = b.emission;
            exact[layer] = isExact;
        };
        if (b.kind == KindCube)
            for (const CubeFace& f : b.faces) assign(f.tex);
        else if (b.kind == KindModel)
            for (u32 q = b.quadFirst; q < b.quadFirst + b.quadCount; ++q) assign(registry_->modelQuad(q).tex);
        if (base == "minecraft:water" || base == "minecraft:lava")  // fluid surfaces use the still / flow textures
            for (u32 l = 0; l < layers && l < r.textureNames_.size(); ++l) {
                const std::string& tn = r.textureNames_[l];
                if (tn == "block/" + shortName + "_still" || tn == "block/" + shortName + "_flow")
                    table[l * 4] = u32(mapped), table[l * 4 + 1] = b.emission, exact[l] = 1;
            }
    }
    p.layerTableBlocks = count;
    // The adapters look up the base layer (before the animation remap), so animated frames need no entries.
    // Staged copy: recorded at the start of the next frame (buffers decay to COMMON and promote on read).
    if (p.layerTable) r.stage(p.layerTable.Get(), 0, table.data(), table.size() * 4);
    if (std::getenv("MCPP_PACK_LAYERS")) {  // debug: texture layer -> mc_Entity id
        for (u32 l = 0; l < layers && l < r.textureNames_.size(); ++l)
            if (table[l * 4] != 0xFFFFFFFFu) LOG("pack layer %u %s -> %d", l, r.textureNames_[l].c_str(), int(table[l * 4]));
    }
}
