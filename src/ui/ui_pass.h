#pragma once
// GPU side of the UI: draws a Canvas over the frame (Renderer HUD stage) with two atlas textures that are
// created from their CPU Atlas at create() and then updated in dirty row ranges.
#include "render/renderer.h"
#include "ui/canvas.h"

namespace ui {

class UiPass : public RenderPass {
public:
    UiPass(Atlas& gui, Atlas& icons) : gui_(gui), icons_(icons) {}
    bool create(Renderer& renderer) override;
    void draw(const PassContext& ctx) override;
    // The canvas drawn by the next frames (swapped in by the UI each frame, before Renderer::render).
    Canvas& canvas() { return canvas_; }
    // Enchantment glint: texture rect in the GUI atlas (u0 v0 du dv), scroll offset, screen pixels per repeat.
    float glint[8] = {};

private:
    static constexpr u64 kVertexBytes = u64(8) << 20;  // per frame in flight
    static constexpr u64 kUploadBytes = u64(8) << 20;  // atlas rows per frame in flight

    bool createTexture(Atlas& atlas, ComPtr<ID3D12Resource>& tex, u32 descriptor);
    void upload(const PassContext& ctx, Atlas& atlas, ID3D12Resource* tex, u64& used);

    Atlas& gui_;
    Atlas& icons_;
    Canvas canvas_;
    Renderer* renderer_ = nullptr;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pso_, psoInvert_, psoGlint_;
    ComPtr<ID3D12Resource> guiTex_, iconTex_, vertices_, upload_;
    u8* vertexMapped_ = nullptr;
    u8* uploadMapped_ = nullptr;
    u32 descriptors_ = 0;
};

}  // namespace ui
