#pragma once
// GameRenderer's post effects over the finished scene, before the UI: the ones the server asks for
// (ClientboundPostEffectsPacket) and the spectated entity's (checkEntityPostEffect: creeper, spider, invert), applied
// in order. A Hud-stage pass, registered before the UI's.
#include "render/renderer.h"

#include <string_view>
#include <utility>
#include <vector>

class PostEffects : public RenderPass {
public:
    enum Effect { None, Creeper, Spider, Invert, Blur };
    void setEffects(std::vector<Effect> effects, float blurRadius) { effects_ = std::move(effects), blurRadius_ = blurRadius; }
    static Effect byName(std::string_view id);  // "minecraft:creeper" -> Creeper; None if unknown
    bool create(Renderer& renderer) override;
    void draw(const PassContext& ctx) override;

private:
    Renderer* r_ = nullptr;
    std::vector<Effect> effects_;
    float blurRadius_ = 5;  // MenuBlurRadius (the menu background blurriness) for blur.json's radius 0
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
