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
    uint4 gMisc;         // x = active slot count, y = texture LOD bias (float bits), z = texture filtering options
    float4 gLightmap[3]; // the game's lightmap inputs (lightmap.hlsli)
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
};
StructuredBuffer<GpuSection> gSections : register(t1);
StructuredBuffer<uint> gActive : register(t3);  // slots selected by the CPU LOD cut

Texture2DArray<float4> gTextures : register(t2);
StructuredBuffer<uint> gColors : register(t4);  // tint colour table (0xRRGGBB), see ColorTable in mesher.h
StructuredBuffer<uint> gTextureRemap : register(t5);  // texture layer -> current animation frame's layer
float3 tintColor(uint index) {
    uint c = gColors[index];
    return float3((c >> 16) & 255, (c >> 8) & 255, c & 255) / 255.0;
}
SamplerState gSampler : register(s0);
// Block textures with the texture LOD bias of the active upscaler (gMisc.y, float bits; 0 at native resolution).
// gMisc.z: the Texture Filtering option (bits 0-1: 0 none, 1 RGSS, 2 anisotropic), Mipmap Levels (bits 2-4: the
// highest mip sampled) and Anisotropic Filtering (bits 5-6: 1..3 = 2x / 4x / 8x, samplers s4..s6).
SamplerState gAniso2 : register(s4);
SamplerState gAniso4 : register(s5);
SamplerState gAniso8 : register(s6);
// texture_sampling.glsl sampleRGSS: four rotated-grid taps in the texel's footprint at the derivative mip, blended in
// as texels get smaller than a pixel.
float4 sampleRgss(float3 uvl, float bias, float maxMip) {
    const float pixel = 1.0 / 16.0;
    float2 du = ddx(uvl.xy), dv = ddy(uvl.xy);
    float2 texelScreen = sqrt(du * du + dv * dv);
    float blend = smoothstep(pixel, pixel * 2.0, max(texelScreen.x, texelScreen.y));
    float effective = sqrt(min(length(du), length(dv)) * max(length(du), length(dv)));
    float mip = clamp(log2(max(effective, 1e-8) / pixel) + bias, 0.0, maxMip);
    float lo = floor(mip), hi = min(lo + 1.0, maxMip), f = frac(mip);
    static const float2 kOffsets[4] = {float2(0.125, 0.375), float2(-0.125, -0.375), float2(0.375, -0.125), float2(-0.375, 0.125)};
    float4 a = 0, b = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        float3 p = float3(uvl.xy + kOffsets[k] * pixel, uvl.z);
        a += gTextures.SampleLevel(gSampler, p, lo);
        b += gTextures.SampleLevel(gSampler, p, hi);
    }
    float4 rgss = lerp(a, b, f) * 0.25;
    float4 nearest = gTextures.SampleBias(gSampler, uvl, bias, int2(0, 0), maxMip);
    return lerp(nearest, rgss, blend);
}
float4 sampleBlock(float3 uvl) {
    uint opts = gMisc.z;
    float bias = asfloat(gMisc.y), maxMip = float((opts >> 2) & 7);
    uint mode = opts & 3;
    if (mode == 1) return sampleRgss(uvl, bias, maxMip);
    if (mode == 2) {
        uint a = (opts >> 5) & 3;
        if (a <= 1) return gTextures.SampleBias(gAniso2, uvl, bias, int2(0, 0), maxMip);
        if (a == 2) return gTextures.SampleBias(gAniso4, uvl, bias, int2(0, 0), maxMip);
        return gTextures.SampleBias(gAniso8, uvl, bias, int2(0, 0), maxMip);
    }
    return gTextures.SampleBias(gSampler, uvl, bias, int2(0, 0), maxMip);
}

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
