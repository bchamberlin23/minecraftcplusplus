#pragma once
// The spectated entity's post effect (GameRenderer.checkEntityPostEffect): creeper (green mosaic), spider (eight
// blurred eyes) and invert (enderman) over the finished scene, before the UI. A Hud-stage pass, registered before
// the UI's.
#include "render/renderer.h"

#include <utility>
#include <vector>

class PostEffects : public RenderPass {
public:
    enum Effect { None, Creeper, Spider, Invert };
    void setEffect(Effect e) { effect_ = e; }
    bool create(Renderer& renderer) override;
    void draw(const PassContext& ctx) override;

private:
    Renderer* r_ = nullptr;
    Effect effect_ = None;
    ComPtr<ID3D12RootSignature> sig_;
    ComPtr<ID3D12PipelineState> pso_[7];  // colour convolve, bits, box blur, spider clip, invert, blit
    ComPtr<ID3D12DescriptorHeap> rtvs_;
    static constexpr int kTargets = 5;  // main (the scene's copy), swap, temp, large_blur, small_blur
    ComPtr<ID3D12Resource> tex_[kTargets];
    D3D12_RESOURCE_STATES state_[kTargets] = {};
    u32 srv_ = UINT32_MAX, w_ = 0, h_ = 0;
    std::vector<std::pair<u64, ComPtr<ID3D12Resource>>> retired_;
    u64 frame_ = 0;
};
