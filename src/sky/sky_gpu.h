#pragma once
// GPU side of the sky: the Sky stage (sky disc, sunrise fan, sun / moon / stars, dark disc, end sky) and the
// AfterTranslucent stage (clouds, rain / snow). One root signature, a few PSOs, vertex pulling throughout.
#include "render/renderer.h"
#include "sky/sky.h"

namespace sky {

struct Image {
    int w = 0, h = 0;
    std::vector<u8> rgba;
};
struct SkyImages {
    Image celestial[9];  // sun, then the 8 moon phases (MoonPhase order); all the same size
    Image endSky, rain, snow;
};

class SkyGpu {
public:
    explicit SkyGpu(const Sky::Frame& frame) : frame_(frame), skyStage_(*this, false), lateStage_(*this, true) {}
    bool init(Renderer& renderer, SkyImages images, std::vector<Sky::Frame::Vertex> stars);
    float gpuMs() const { return gpuMs_; }  // sky + late passes, average after start-up
    float skyMs() const { return timedFrames_ > 30 ? float(skyMs_ / double(timedFrames_ - 30)) : 0.0f; }
    float lateMs() const { return timedFrames_ > 30 ? float(lateMs_ / double(timedFrames_ - 30)) : 0.0f; }

private:
    struct Stage : RenderPass {
        SkyGpu& gpu;
        bool late;
        Stage(SkyGpu& g, bool l) : gpu(g), late(l) {}
        bool create(Renderer&) override { return true; }
        void draw(const PassContext& ctx) override { late ? gpu.drawLate(ctx) : gpu.drawSky(ctx); }
    };
    static constexpr u64 kSlotBytes = u64(512) << 10;  // per frame: constants + dynamic vertices
    static constexpr u32 kMaxDynamicVertices = 12000;
    static constexpr u32 kMaxCloudFaces = 240000;

    void drawSky(const PassContext& ctx);
    void drawLate(const PassContext& ctx);
    // A shader pack is active (ctx.pack): vanilla geometry for its gbuffers_skybasic / skytextured / clouds / weather.
    void drawPack(const PassContext& ctx, bool late);
    void writeConstants(const PassContext& ctx);
    u32 putVertices(const std::vector<Sky::Frame::Vertex>& v);  // into this frame's slot; returns first index
    void bindCommon(ID3D12GraphicsCommandList* list);
    bool createTexture(const Image* layers, u32 count, u32 descriptor, ComPtr<ID3D12Resource>& out);

    const Sky::Frame& frame_;
    Stage skyStage_, lateStage_;
    Renderer* renderer_ = nullptr;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> psoSky_, psoDarkDisc_, psoBlend_, psoAdd_, psoClouds_, psoWeather_;
    ComPtr<ID3D12Resource> celestial_, endSky_, rain_, snow_, stars_, ring_, cloudFaces_, cloudStaging_;
    ComPtr<ID3D12Resource> celestial2d_[9];        // shader packs: sun / moon phases as single textures
    std::vector<Sky::Frame::Vertex> starsCpu_;
    ComPtr<ID3D12QueryHeap> queries_;
    ComPtr<ID3D12Resource> queryReadback_;
    u8* ringMapped_ = nullptr;
    u8* cloudStagingMapped_ = nullptr;
    u32 descriptors_ = 0;
    u32 starVertices_ = 0;
    u32 frameIndex_ = 0;  // ctx.frame of the current frame
    u32 ringVertices_ = 0;
    D3D12_GPU_VIRTUAL_ADDRESS constantsVa_ = 0;
    u64 cloudVersion_ = 0;
    u32 cloudFaceCount_ = 0;
    bool queried_[Renderer::kFrames] = {};
    u64 timestampFreq_ = 1;
    float gpuMs_ = 0;
    double skyMs_ = 0, lateMs_ = 0;
    u64 timedFrames_ = 0;
};

}  // namespace sky
