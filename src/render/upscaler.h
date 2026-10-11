#pragma once
// Temporal upscaler interface of the shader pipeline (render/shading.cpp). The scene renders at the internal
// resolution (ShaderSettings::renderScale x output size); an Upscaler turns it into the output-resolution HDR image
// that bloom, eye adaptation and tonemapping then work on. The built-in implementation is the pipeline's TAA(U);
// SDK upscalers (DLSS / FSR / XeSS) implement the same interface and are installed with Renderer::setUpscaler.
//
// Frame order around the upscaler (shaders on):
//   scene (render res) -> composite (volumetrics, clouds/weather) -> motion vectors -> [Upscaler::evaluate]
//   -> eye adaptation + bloom (output res) -> tonemap into the swap chain back buffer ("hudless" image)
//   -> overlays (block outline) + Hud stage into the UI layer (separate target) -> UI composite onto the back buffer
//   -> Present (Renderer::present).
#include "core/common.h"

#include <d3d12.h>

class Renderer;

struct UpscaleInputs {
    ID3D12GraphicsCommandList* list = nullptr;
    // Render-resolution inputs. Every resource is in the state given here on entry and must be on exit.
    ID3D12Resource* color = nullptr;     // R16G16B16A16_FLOAT, linear HDR radiance, exposure NOT applied (pre-exposure 1);
                                         // includes the late layer (clouds, rain). ALL_SHADER_RESOURCE.
    ID3D12Resource* depth = nullptr;     // R32_TYPELESS (read as R32_FLOAT): reversed Z, infinite far plane, near = nearZ
                                         // (depth 0 = sky). DEPTH_READ | ALL_SHADER_RESOURCE.
    ID3D12Resource* motion = nullptr;    // R16G16_FLOAT: previous UV - current UV, unjittered, UV units (x right, y down).
                                         // Multiply by renderSize for pixels. Camera motion only (the terrain is static;
                                         // entities / waving foliage are not included). ALL_SHADER_RESOURCE.
    ID3D12Resource* reactive = nullptr;  // R8G8B8A8_UNORM, alpha > 0 where the World stage drew (entities, block entities):
                                         // a reactive / "less history" hint. ALL_SHADER_RESOURCE.
    ID3D12Resource* exposure = nullptr;  // R32_FLOAT 1x1: exposure multiplier the tonemapper applies (eye adaptation of the
                                         // previous frame). ALL_SHADER_RESOURCE.
    // Output-resolution result: linear HDR, exposure NOT applied. UNORDERED_ACCESS (and created with ALLOW_UNORDERED_ACCESS).
    ID3D12Resource* output = nullptr;    // R16G16B16A16_FLOAT
    float jitter[2] = {};                // sample offset from the pixel centre in render pixels (+x right, +y down)
    u32 renderSize[2] = {}, outputSize[2] = {};
    float fovY = 1.2f, aspect = 1.0f;    // vertical field of view (radians), width / height
    float nearZ = 0.05f, farZ = 1e30f;   // infinite projection: farZ is "infinity"
    float frameTimeMs = 16.6f;
    bool reset = false;                  // history invalid (first frame, teleport, resize, settings change)
    // Ray reconstruction guides (path tracing, render resolution, all in `guideState`): `color` is then the noisy
    // ray traced radiance. diffuseAlbedo / specularAlbedo: linear RGB; normalRoughness: world-space normal + linear
    // roughness in alpha. worldToView: row-major, column vectors (camera-relative world -> view, rotation only).
    ID3D12Resource* diffuseAlbedo = nullptr;
    ID3D12Resource* specularAlbedo = nullptr;
    ID3D12Resource* normalRoughness = nullptr;
    D3D12_RESOURCE_STATES guideState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};

class Upscaler {
public:
    virtual ~Upscaler() = default;
    virtual const char* name() const = 0;
    // Records the upscale into inputs.list. Descriptor heaps / root signature are not preserved across the call.
    virtual void evaluate(const UpscaleInputs& inputs) = 0;
};
