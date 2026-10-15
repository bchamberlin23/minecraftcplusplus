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
    float4 gFade;        // x = fade clock (seconds), y = 1 / fade duration, z = frame index, w = temporal AA on
    float4 gShade[2];    // CardinalLighting by face: down up north south, west east
    float4 gFogRender;   // the render distance fog (cylindrical): x = start, y = end (gFogParams.xy: environmental)
    uint4 gHzbInfo;      // occlusion culling: xy = scene depth size, z = pyramid levels (0: off)
};
#include "lightmap.hlsli"
#include "fog.hlsli"

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
// gMisc.z: the Texture Filtering option (bits 0-1: 0 none, 1 RGSS, 2 anisotropic), Mipmap Levels (bits 2-4: the
// highest mip sampled) and Anisotropic Filtering (bits 5-6: 1..3 = 2x / 4x / 8x, samplers s4..s6).
SamplerState gAniso2 : register(s4);
SamplerState gAniso4 : register(s5);
SamplerState gAniso8 : register(s6);
// SampleBias with the Mipmap Levels cap: SampleBias's clamp argument is a minimum LOD, so the cap scales the
// derivatives instead (keeps anisotropic filtering): lod' = min(lod + bias, maxMip).
float4 sampleCapped(SamplerState s, float3 uvl, float bias, float maxMip) {
    float lod = gTextures.CalculateLevelOfDetailUnclamped(s, uvl.xy) + bias;
    float k = exp2(bias + min(maxMip - lod, 0.0));
    return gTextures.SampleGrad(s, uvl, ddx(uvl.xy) * k, ddy(uvl.xy) * k);
}
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
    float4 nearest = sampleCapped(gSampler, uvl, bias, maxMip);
    return lerp(nearest, rgss, blend);
}
float4 sampleBlock(float3 uvl) {
    uint opts = gMisc.z;
    float bias = asfloat(gMisc.y), maxMip = float((opts >> 2) & 7);
    uint mode = opts & 3;
    if (mode == 1) return sampleRgss(uvl, bias, maxMip);
    if (mode == 2) {
        uint a = (opts >> 5) & 3;
        if (a <= 1) return sampleCapped(gAniso2, uvl, bias, maxMip);
        if (a == 2) return sampleCapped(gAniso4, uvl, bias, maxMip);
        return sampleCapped(gAniso8, uvl, bias, maxMip);
    }
    return sampleCapped(gSampler, uvl, bias, maxMip);
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
    return vanillaFog(rel, gFogParams.xy, gFogRender.xy);
}

float4 shadePixel(VSOut i) {
    float4 c = sampleTerrain(i.uvl);
    c.rgb = lerp(c.rgb * i.color, gFogColor.rgb, i.fog);
    return c;
}

// ---- Fluid quads (geo bit 30; FluidRenderer, see mesher.h) -------------------------------------------------
// One block face: the top's corner heights (sky word bytes by a + 2b, 1/240 block) or a side's two top heights (bytes
// 0 / 1 by a), the block's light (block word: sky << 4 | block), the top's flow angle (tex bits 12..19, 255: still).
bool isFluidQuad(uint4 q) { return ((q.x >> 30) & 1u) != 0; }
// FluidRenderer's backward faces (fluid quads with bit 31): facing `face`, placed and textured as face ^ 1.
uint fluidGeoFace(uint4 q, uint face) { return isFluidQuad(q) && (q.x >> 31) != 0 ? face ^ 1u : face; }
float fluidHeightByte(uint4 q, uint i) { return float((q.z >> (i * 8)) & 0xFF) / 240.0; }
// Corner (a, b) of a fluid face (geometric, after the front-facing swap): its height and texture coordinates.
void fluidCorner(uint4 q, uint face, float a, float b, inout float3 p, out float2 st) {
    float cellY = float((q.x >> 4) & 15);
    if (face == 1) {  // the top: four heights; still (u, v) = (a, b) or the flow texture turned to the flow
        p.y = cellY + fluidHeightByte(q, uint(a) + 2u * uint(b)) - 0.001;
        uint ang = (q.y >> 12) & 0xFF;
        if (ang == 255u) {
            st = float2(a, b);
        } else {
            float angle = float(ang) / 255.0 * 6.28318530718;
            float s = sin(angle) * 0.25, c = cos(angle) * 0.25;
            st = float2(0.5 + (a > 0.5 ? c : -c) + (b > 0.5 ? s : -s), 0.5 + (b > 0.5 ? c : -c) + (a > 0.5 ? -s : s));
        }
    } else if (face == 0) {
        p.y += 0.001;
        st = float2(a, b);
    } else {  // sides: half the flowing texture, down from (1 - height) / 2
        float h = fluidHeightByte(q, uint(a));
        if (b > 0.5) p.y = cellY + h;
        if (face == 2) p.z += 0.001; else if (face == 3) p.z -= 0.001; else if (face == 4) p.x += 0.001; else p.x -= 0.001;
        bool flip = face == 3 || face == 4;
        st = float2((flip ? 1.0 - a : a) * 0.5, b > 0.5 ? (1.0 - h) * 0.5 : 0.5);
    }
}
float fluidSky(uint4 q) { return float((q.w >> 4) & 15); }
float fluidBlock(uint4 q) { return float(q.w & 15); }
