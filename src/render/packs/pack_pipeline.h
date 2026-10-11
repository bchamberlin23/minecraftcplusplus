#pragma once
// Shader pack pipeline (OptiFine / Iris model) on D3D12, replacing the built-in pipeline while a pack is active:
//
//   clear colortex (clear flags / colours)            begin passes
//   shadow pass: GPU cull (pack_util.hlsl) against the pack's shadow box, shadow / shadow_solid / shadow_cutout
//     programs over our terrain (shadowtex0, shadowcolor0/1), shadowtex1 copied before translucent, shadow_water
//   shadowcomp, prepare passes
//   gbuffers (opaque): skybasic / skytextured (Sky stage), terrain solid / cutout / models (renderer's GPU cull),
//     entities, block entities (World stage through the PackSink)
//   depthtex2 / depthtex1 (GL depth copies), hand (solid), deferred passes
//   gbuffers (translucent): water, clouds / weather / particles (AfterTranslucent stage), translucent hand
//   depthtex0, composite passes, final -> back buffer; flipped persistent buffers copied back
//
// Pack programs are translated at load (pack.h), compiled to DXIL per geometry adapter (cube quads, model quads,
// generic vertices, full screen) and bound with one root signature (see kRoot* in pack_pipeline.cpp). The scene depth
// stays reversed-Z (the renderer's depth buffer); the adapters convert gl_Position, gl_FragCoord.z and gl_FragDepth,
// and depthtex0..2 are GL-depth copies.
#include "render/pack_sink.h"
#include "render/packs/pack.h"
#include "render/packs/uniforms.h"
#include "render/renderer.h"

#include <array>
#include <atomic>
#include <mutex>
#include <thread>

class BlockRegistry;

namespace packs {
struct PackGameState;
struct PackPlayerState;
}

struct Renderer::Packs : PackSink {
    explicit Packs(Renderer& renderer);
    ~Packs() override;

    Renderer& r;

    // ---- lifecycle (main thread) ----
    // Starts loading a pack in the background (translation + DXIL); the switch happens at a frame boundary.
    void request(const fs::path& path, const packs::LoadOptions& options);
    void disable();                     // back to the built-in pipeline (next frame)
    bool active() const { return pipe_ != nullptr; }
    void beginFrame();                  // applies a finished load (waits for the GPU) before recording
    void resize();
    struct Status {
        std::string pack;     // active pack ("" = none)
        std::string loading;  // pack being loaded
        std::string error;    // last load error (the built-in pipeline stays active)
        std::vector<std::string> warnings;
        double loadMs = 0;
    };
    Status status() const;
    void setBlockRegistry(const BlockRegistry* registry) { registry_ = registry; }
    void setGameState(const packs::PackGameState& s);
    void setPlayerState(const packs::PackPlayerState& s);

    // ---- per frame ----
    struct FrameInput {
        const CameraState* cam;
        float viewDistance;
        D3D12_GPU_VIRTUAL_ADDRESS activeVa;
        u32 activeCount;
        bool haveWorld;
        u32 frameIndex;
    };
    void record(ID3D12GraphicsCommandList* list, const FrameInput& in, PassContext& pc, ID3D12Resource* backbuffer,
                D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv);
    // Frame generation with a pack (needs the upscaling path: its upright depth and camera motion vectors). After record:
    // the overlays and Hud go into the built-in pipeline's UI layer (frameGenUi), and Upscalers::endFrame tags these.
    bool frameGenUi() const;
    u32 renderSize(bool height) const;  // the pack's render size (below the output size when upscaling)
    bool frameGenInputs(ID3D12Resource*& depth, ID3D12Resource*& motion) const;  // states: DEPTH_READ|SRV, SRV

    // PackSink (extension passes, during record).
    void draw(const PackDraw& d) override;
    bool atlasUv(u32 layer, float u, float v, float out[2]) const override;
    int entityId(std::string_view name) const override;
    int itemId(std::string_view name) const override;
    int blockId(u16 blockState) const override;
    bool vanillaClouds() const override;
    ID3D12Resource* glintTexture(bool item) const override;

    struct Pipe;   // a loaded pack's GPU state (pack_pipe.h)
    struct Frame;  // per-frame recording (pack_frame.cpp)

private:
    void loadThread(fs::path path, packs::LoadOptions options, u64 generation);
    // Background: root signatures, heaps, DXIL, PSOs. Main thread (beginFrame): textures and targets.
    bool build(std::unique_ptr<packs::PackDefinition> def, std::unique_ptr<Pipe>& out, std::string& error);
    bool finalize(Pipe& p, std::string& error);
    bool createTargets(Pipe& p);
    void targetSize(u32& w, u32& h, bool& scaled) const;  // the pack's render size (upscaler render size when scaled)
    struct ScaleCache {  // the SDKs' optimal render scale (queried when the upscaler, quality or output size change)
        int kind = -1, quality = -1;
        u32 w = 0, h = 0;
        float scale = 1;
    };
    mutable ScaleCache scaleCache_;
    void updateLayerTable(Pipe& p);

    std::unique_ptr<Pipe> pipe_;
    const BlockRegistry* registry_ = nullptr;
    std::unique_ptr<packs::PackGameState> game_;
    std::unique_ptr<packs::PackPlayerState> player_;

    mutable std::mutex mutex_;
    std::thread loader_;
    std::atomic<u64> generation_{0};
    std::unique_ptr<Pipe> pendingPipe_;  // built in the background, finalized by beginFrame
    Frame* frame_ = nullptr;             // while recording (PackSink draws)
    struct Batch {
        PackDraw draw;  // the key (vertices unused)
        struct Range {
            D3D12_GPU_VIRTUAL_ADDRESS va;  // vertices in this frame's upload slice
            u32 count;
            bool quads;
        };
        std::vector<Range> ranges;
    };
    std::vector<Batch> batches_;  // sink draws of the current extension stage
    // Shadow casters: the last frame's entity / block entity batches (their vertices are still in that frame's upload
    // slice), drawn into the next shadow map moved by the camera's motion; one frame of animation lag.
    std::vector<Batch> casters_;
    double casterCam_[3] = {};
    const void* casterPipe_ = nullptr;
    bool casterStage_ = false;
    std::vector<Batch> hand_;  // gbuffers_hand / hand_water draws of this frame
    void drawHand(bool translucent);
    void flushBatches();
    void logTimings(const Pipe& p);
    void drawRanges(const PackDraw& key, const Batch::Range* ranges, size_t count);
    bool inStage_ = false;               // inside an extension stage (restore the renderer's heap after draws)
    bool disableRequested_ = false;
    fs::path requestPath_;          // the pack asked for (empty: built-in), reloaded when the dimension changes
    packs::LoadOptions requestOptions_;
    Status status_;
};
