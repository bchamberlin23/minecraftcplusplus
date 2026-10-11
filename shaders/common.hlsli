// Shared by all shaders. Layout must match FrameConstants in renderer.cpp.
cbuffer Frame : register(b0) {
    float4x4 gViewProj;  // camera-relative (world origin at the camera)
    float4 gFrustum[5];  // left, right, bottom, top, near (camera-relative)
    int4 gCamBlock;      // floor(camera position)
    float4 gCamFrac;     // camera position - gCamBlock
    float4 gFogColor;
    float4 gFogParams;   // x = fog start, y = fog end, z = max draw distance (0 = unlimited), w = daylight
    float4 gTints[8];
    uint4 gSegBase;      // first indirect command of each pass
    uint4 gSegCap;       // command capacity of each pass
    uint4 gMisc;         // x = active slot count, y = texture LOD bias (float bits)
    float4 gLightmap[3]; // the game's lightmap inputs (lightmap.hlsli)
    float4 gFade;        // x = fade clock (seconds), y = 1 / fade duration, z = frame index, w = temporal AA on
};
#include "lightmap.hlsli"

struct DrawConstants {
    uint quadOffset;
    uint sectionIndex;
};
ConstantBuffer<DrawConstants> gDraw : register(b1);

struct GpuSection {
    int4 origin;         // xyz = block coordinates of the min corner, w = LOD level (cell = 2^w blocks)
    uint2 ranges[19];    // (first quad, quad count) per draw range
    float fade;          // fade clock time a cross-fade started: > 0 fading in, < 0 fading out, 0 none
    float pad;
};
StructuredBuffer<GpuSection> gSections : register(t1);
StructuredBuffer<uint> gActive : register(t3);  // slots selected by the CPU LOD cut

// Cross-fade of a section entering or leaving the LOD cut: fading in it shows the pixels whose dither value is
// below its progress, fading out the others, so the old and new section together cover each pixel once.
float fadeProgress(float fade) { return fade == 0 ? 1.0 : saturate((gFade.x - abs(fade)) * gFade.y); }
bool fadedOut(float2 pixel) {
    float fade = gSections[gDraw.sectionIndex].fade;
    if (fade == 0) return false;
    float t = fadeProgress(fade);
    float d;
    if (gFade.w > 0) {  // white noise, new each frame: an even dissolve the temporal accumulation smooths
        uint h = uint(pixel.x) * 1973u + uint(pixel.y) * 9277u + uint(gFade.z) * 26699u;
        h = (h ^ 61u) ^ (h >> 16), h *= 9u, h ^= h >> 4, h *= 0x27d4eb2du, h ^= h >> 15;  // Wang hash
        d = float(h & 0xFFFFFFu) / 16777216.0;
    } else {  // no temporal accumulation: an even 4x4 ordered screen door
        static const float kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
        uint2 q = uint2(pixel) & 3;
        d = (kBayer[q.y * 4 + q.x] + 0.5) / 16.0;
    }
    return fade > 0 ? d >= t : d < t;
}

Texture2DArray<float4> gTextures : register(t2);
StructuredBuffer<uint> gColors : register(t4);  // tint colour table (0xRRGGBB), see ColorTable in mesher.h
StructuredBuffer<uint> gTextureRemap : register(t5);  // texture layer -> current animation frame's layer
float3 tintColor(uint index) {
    uint c = gColors[index];
    return float3((c >> 16) & 255, (c >> 8) & 255, c & 255) / 255.0;
}
SamplerState gSampler : register(s0);
// Block textures with the texture LOD bias of the active upscaler (gMisc.y, float bits; 0 at native resolution).
float4 sampleBlock(float3 uvl) { return gTextures.SampleBias(gSampler, uvl, asfloat(gMisc.y)); }

// Coarse LOD faces (the vertex shader stores their layer as -(layer + 1)): like Distant Horizons, the texture's
// average colour (its 1x1 mip: alpha-weighted, so leaves keep their green) with no holes, plus a little per-block
// noise in place of texture detail, faded out where blocks get smaller than a pixel.
float lodRand(float2 p) { return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453); }
float4 sampleTerrain(float3 uvl) {
    if (uvl.z >= 0) return sampleBlock(uvl);
    float3 c = gTextures.SampleLevel(gSampler, float3(uvl.xy, -uvl.z - 1.0), 4.0).rgb;
    float lum = (c.r + c.g + c.b) / 3.0;
    float amp = 0.06 * (1.0 - (lum * 2.0 - 1.0) * (lum * 2.0 - 1.0));
    float fade = saturate(1.5 - 1.5 * max(fwidth(uvl.x), fwidth(uvl.y)));  // blocks per pixel
    float r = (lodRand(floor(uvl.xy)) * 2.0 - 1.0) * amp * fade;
    c = saturate(c + (r > 0 ? (1.0 - c) * r : c * r));
    return float4(c, 1);
}

struct VSOut {
    float4 pos : SV_Position;
    float3 uvl : TEXCOORD0;  // uv + texture layer
    float3 color : COLOR0;   // tint * directional shade
    float fog : FOG;
};

// Light levels 0..15 -> brightness, following the shape of the game's lightmap
// (hyperbolic falloff, warm block light, default 50% brightness lift).
float lightCurve(float level) {
    float f = level / 15.0;
    return f / (4.0 - 3.0 * f);
}
float3 lightColorLevels(float skyLevel, float blockLevel) {  // fractional levels 0..15
    return vanillaLight(skyLevel, blockLevel, gLightmap);
}
float3 lightColor(uint packed) { return lightColorLevels(float(packed >> 4), float(packed & 15)); }

float fogFactor(float3 rel) {
    return saturate((length(rel) - gFogParams.x) / max(gFogParams.y - gFogParams.x, 1e-3));
}

float4 shadePixel(VSOut i) {
    float4 c = sampleTerrain(i.uvl);
    c.rgb = lerp(c.rgb * i.color, gFogColor.rgb, i.fog);
    return c;
}
