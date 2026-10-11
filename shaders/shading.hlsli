// Shader pipeline ("shaders on"): constants, bindings and the lighting model shared by the G-buffer, deferred,
// forward-translucent and post passes. Include after common.hlsli. Layout must match ShadeConstants in
// src/render/shading.cpp; texture / UAV slots match the Tex / Rw enums there.
#ifndef SHADING_HLSLI
#define SHADING_HLSLI

cbuffer Shade : register(b2) {
    float4x4 gShadowMat[4];      // camera-relative position -> shadow clip (xy -1..1, z 0..1 from the light)
    float4x4 gPrevViewProj;      // camera-relative position (this frame) -> previous frame clip, unjittered
    float4x4 gViewProjNoJitter;  // camera-relative position -> clip, unjittered
    float4 gRayX, gRayY, gRayZ;  // camera-relative position = viewZ * (gRayZ + ndc.x * gRayX + ndc.y * gRayY)
    float4 gCascade[4];          // x = texel size (blocks), y = depth range (blocks), z = filter radius (texels), w = 1 / map size
    float4 gLightDir;            // towards the shadow-casting body; w = shadow distance (fade end, blocks)
    float4 gLightColor;          // HDR light of that body (0 below the horizon); w = 1 sun, 0 moon
    float4 gSunDir;              // towards the sun; w = sun disc boost
    float4 gAmbient;             // sky ambient (HDR); w = vanilla sky colour -> HDR scale
    float4 gBlockColor;          // block light colour (HDR); w = minimum (cave) light
    float4 gFog;                 // x = density at sea level (1/block), y = 1/scale height, z = sea level, w = volumetric strength
    float4 gWater;               // rgb = absorption (1/block); w = 1 when the camera is underwater
    float4 gWaterScatter;        // rgb = in-scatter colour (HDR); w = water fog density
    float4 gScreen;              // render (internal) resolution: xy = size, zw = 1 / size
    float4 gJitter;              // xy = jitter (NDC); zw = camera world x, z modulo 2048
    float4 gTime;                // x = seconds (wrapped), y = rain, z = thunder, w = camera world y
    float4 gSkyZenith;           // linear sky colour (HDR)
    float4 gSkyHorizon;          // linear fog colour (HDR); w = vanilla fog start
    float4 gShadeMisc;                // x = exposure compensation, y = bloom strength, z = emissive strength, w = vanilla fog end
    float4 gMoonDir;             // towards the moon; w = night factor (0 day .. 1 night)
    float4 gOutput;              // output (swap chain) resolution: xy = size, zw = 1 / size
    uint4 gOpts;                 // x = cascades, y = shadow taps, z = ssr steps, w = volumetric steps
    uint4 gFlags;                // x = flags (kFlag*), y = frame index, z = previous history slot, w = this history slot
    float4 gCave;                // minimum light: the dimension's ambient light colour (linear, HDR)
};
// Debug views (MCPP_SHADERS_DEBUG): 1 raw HDR (exposure 1, no tonemap), 2 albedo, 3 normal, 4 shadow, 5 lighting only,
// 6 volumetric in-scatter x4, 7 motion vectors.
uint debugView() { return (gFlags.x >> 8) & 15; }
uint tonemapMode() { return (gFlags.x >> 12) & 3; }
static const uint kFlagWaving = 1, kFlagPcss = 2, kFlagSsao = 4, kFlagVolumetric = 8, kFlagSsr = 16, kFlagTaa = 32;

cbuffer PassConstants : register(b3) {
    uint4 gPassA;
    uint4 gPassB;
};

// Texture slots (Tex in shading.cpp).
static const uint kTexDepth = 0, kTexAlbedo = 1, kTexNormal = 2, kTexLdrSky = 3, kTexLdrFwd = 4, kTexScene = 5,
                  kTexSceneCopy = 6, kTexLinDepth = 7, kTexVol = 8, kTexHist0 = 9, kTexAo = 11, kTexBloom0 = 12,
                  kTexMotion = 18, kTexExposure = 19, kTexUi = 20;
static const uint kRwScene = 0, kRwSceneCopy = 1, kRwLinDepth = 2, kRwVol = 3, kRwHist0 = 4, kRwAo = 6, kRwBloom0 = 7,
                  kRwMotion = 13, kRwExposure = 14;
// The content of a render pixel was sampled at its centre + this offset (render pixels, +y down): the jitter.
float2 jitterPixels() { return float2(-gJitter.x, gJitter.y) * 0.5 * gScreen.xy; }

Texture2D<float4> gTex[24] : register(t0, space1);
Texture2DArray<float> gShadowMap : register(t0, space2);
StructuredBuffer<uint> gMaterial : register(t0, space3);  // per texture layer, see kMat*
RWTexture2D<float4> gRW[24] : register(u0, space1);
RWStructuredBuffer<float4> gState : register(u0, space2);  // [0].x = exposure
SamplerComparisonState gShadowCmp : register(s1);
SamplerState gLinearClamp : register(s2);
SamplerState gPointClamp : register(s3);

// Material classes (low 4 bits of gMaterial; the G-buffer keeps 3 bits).
static const uint kMatDefault = 0, kMatLeaves = 1, kMatPlant = 2, kMatEmissive = 3, kMatGlow = 4, kMatWater = 5,
                  kMatGlass = 6, kMatIce = 7,
                  kMatEntity = 8;  // forward layer (entities, block entities): no stored normal
// Waving (bits 4..5): 0 none, 1 plant (top waves), 2 upper half of a double plant, 3 leaves (everything).
uint matClass(uint m) { return m & 15; }
uint matWave(uint m) { return (m >> 4) & 3; }

static const float kPi = 3.14159265;

float3 srgbToLinear(float3 c) {
    return select(c <= 0.04045, c / 12.92, pow(abs((c + 0.055) / 1.055), 2.4));
}
float3 linearToSrgb(float3 c) {
    c = max(c, 0);
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}
float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float2 octEncode(float3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 e = n.z >= 0 ? n.xy : (1.0 - abs(n.yx)) * select(n.xy >= 0, 1.0, -1.0);
    return e * 0.5 + 0.5;
}
float3 octDecode(float2 e) {
    e = e * 2 - 1;
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.xy += select(n.xy >= 0, -t, t);
    return normalize(n);
}

// Interleaved gradient noise, animated by the frame index (resolved by TAA).
float ign(float2 pixel, uint frame) {
    pixel += float(frame % 64) * 5.588238;
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// ---- positions ----------------------------------------------------------------------------------------

float2 pixelNdc(float2 pixel) {
    float2 uv = pixel * gScreen.zw;
    return float2(uv.x * 2 - 1, 1 - uv.y * 2);
}
float viewDepth(float d) { return 0.05 / max(d, 1e-12); }  // reversed-Z infinite projection, near 0.05
float3 viewRay(float2 ndc) { return gRayZ.xyz + ndc.x * gRayX.xyz + ndc.y * gRayY.xyz; }  // z component 1 along the view
float3 relPosition(float2 pixel, float d) { return viewDepth(d) * viewRay(pixelNdc(pixel)); }

// ---- light ---------------------------------------------------------------------------------------------

float skyCurve(float level) {  // 0..15 -> sky light visibility
    float f = saturate(level / 15.0);
    return f * f * (0.35 + 0.65 * f);
}
float blockCurve(float level) {  // 0..15 -> block light intensity
    float f = saturate(level / 15.0);
    return f * f * f * 1.6 + f * 0.12;
}

// Optical depth of exponential height fog between the camera and rel.
float fogDepth(float3 rel) {
    float dist = length(rel);
    float y0 = gTime.w - gFog.z;
    float k = gFog.y * rel.y;
    float base = gFog.x * exp(-gFog.y * y0);
    float f = abs(k) > 1e-4 ? (1.0 - exp(-k)) / k : 1.0;
    return base * dist * f;
}

// Vanilla-style sky for a direction (reflections, aerial perspective where the sky stage is not visible).
float3 skyColor(float3 dir) {
    float up = saturate(dir.y);
    float3 c = lerp(gSkyHorizon.rgb, gSkyZenith.rgb, saturate(pow(up, 0.6) * 1.2));
    if (dir.y < 0) c = lerp(gSkyHorizon.rgb, gSkyHorizon.rgb * 0.35, saturate(-dir.y * 3));
    float mu = dot(dir, gSunDir.xyz);
    float glow = pow(saturate(mu), 8.0) * 0.6 + pow(saturate(mu), 64.0) * 2.0;
    c += gLightColor.w * gLightColor.rgb * glow * 0.08 * (1.0 - gTime.y * 0.8);
    return c;
}

// ---- shadows -------------------------------------------------------------------------------------------

static const float2 kVogel16[16] = {
    float2(0.177, 0.000), float2(-0.225, 0.206), float2(0.032, -0.393), float2(0.276, 0.367),
    float2(-0.505, -0.084), float2(0.495, -0.282), float2(-0.161, 0.590), float2(-0.307, -0.574),
    float2(0.647, 0.239), float2(-0.693, 0.268), float2(0.355, -0.672), float2(0.231, 0.774),
    float2(-0.674, -0.506), float2(0.865, -0.147), float2(-0.563, 0.726), float2(-0.070, -0.935)};

int shadowCascade(float3 rel, out float3 sp) {
    [loop] for (uint c = 0; c < gOpts.x; ++c) {
        float4 p = mul(gShadowMat[c], float4(rel, 1));
        if (all(abs(p.xy) < 0.97)) {
            sp = p.xyz;
            return int(c);
        }
    }
    sp = 0;
    return -1;
}

// Shadow visibility (0..1) at a camera-relative position with surface normal n. thickness: blocks of caster
// in front of the point along the light (for translucency), 0 if lit.
float shadowVisibility(float3 rel, float3 n, float2 pixel, out float thickness) {
    thickness = 0;
    float dist = length(rel);
    if (gOpts.x == 0 || dist > gLightDir.w) return 1;
    float3 sp;
    int c = shadowCascade(rel, sp);
    if (c < 0) return 1;
    float texel = gCascade[c].x;
    // Normal offset: a texel and a half along the normal (towards the light for lit faces).
    float3 p = rel + n * (texel * 1.5 + 0.004 * dist / 64.0);
    sp = mul(gShadowMat[c], float4(p, 1)).xyz;
    float2 uv = sp.xy * float2(0.5, -0.5) + 0.5;
    float ref = sp.z - 0.01 / gCascade[c].y;
    float noise = ign(pixel, gFlags.y) * 2 * kPi;
    float2x2 rot = float2x2(cos(noise), -sin(noise), sin(noise), cos(noise));
    float invSize = gCascade[c].w;
    float radius = gCascade[c].z;
    float occluder = gShadowMap.SampleLevel(gPointClamp, float3(uv, c), 0);
    if ((gFlags.x & kFlagPcss) != 0) {  // blocker search, penumbra from the blocker distance
        float sum = 0, count = 0;
        [unroll] for (uint i = 0; i < 8; ++i) {
            float2 o = mul(rot, kVogel16[i * 2]) * (radius * 3.0) * invSize;
            float4 d = gShadowMap.GatherRed(gPointClamp, float3(uv + o, c));
            float4 m = d < ref;
            sum += dot(d, m);
            count += dot(m, 1);
        }
        if (count == 0) return 1;
        float blocker = sum / count;
        float penumbraBlocks = (ref - blocker) * gCascade[c].y * 0.035;
        radius = clamp(penumbraBlocks / texel, 1.0, radius * 3.0);
    }
    float vis = 0;
    uint taps = gOpts.y;
    [loop] for (uint i = 0; i < taps; ++i) {
        float2 o = mul(rot, kVogel16[i * 16 / taps]) * radius * invSize;
        vis += gShadowMap.SampleCmpLevelZero(gShadowCmp, float3(uv + o, c), ref);
    }
    vis /= float(taps);
    thickness = max(0, (sp.z - occluder) * gCascade[c].y);
    // Fade into "lit" over the last 15% of the shadow distance.
    float fade = saturate((dist - gLightDir.w * 0.85) / (gLightDir.w * 0.15));
    return lerp(vis, 1, fade);
}

// ---- surface lighting ----------------------------------------------------------------------------------

struct Surface {
    float3 albedo;  // linear
    float3 n;
    float sky, block;  // light levels 0..15
    float ao;
    uint mat;
};

// HDR radiance leaving a surface towards the camera (no fog).
float3 shadeSurface(Surface s, float3 rel, float2 pixel) {
    float3 v = -normalize(rel);
    float3 l = gLightDir.xyz;
    float ndl = dot(s.n, l);
    bool foliage = s.mat == kMatLeaves || s.mat == kMatPlant;
    float thickness;
    float shadow = 1;
    float lightVisible = dot(gLightColor.rgb, 1) > 0 ? 1 : 0;
    if (lightVisible > 0 && (ndl > 0 || foliage)) shadow = shadowVisibility(rel, s.n * (ndl >= 0 ? 1 : -1), pixel, thickness);
    else thickness = 4;
    // Entities have no stored normal (n = light direction, the albedo carries the game's directional shading), so
    // their far side lies in their own shadow: keep some of the direct light there instead of going black.
    if (s.mat == kMatEntity) shadow = lerp(0.4, 1.0, shadow);
    // Sky gate: no direct light deep inside (beyond the shadow distance the sky light level is all we know).
    float gateFar = pow(saturate(s.sky / 15.0), 6.0);
    float gateNear = saturate(s.sky / 4.0);
    float dist = length(rel);
    float gate = lerp(gateNear, gateFar, saturate((dist - gLightDir.w * 0.85) / (gLightDir.w * 0.15)));
    float3 direct = gLightColor.rgb * gate;
    float3 diffuse = direct * saturate(ndl) * shadow;
    if (foliage) {  // wrap + transmission through thin foliage
        float wrap = saturate((ndl + 0.5) / 1.5);
        float trans = exp(-thickness * 1.2) * (0.35 + 0.65 * pow(saturate(dot(-v, l)), 4.0) * 2.0);
        diffuse = direct * (wrap * shadow * 0.6 + trans * 0.3);
    }
    float hemi = s.mat == kMatEntity ? 1.0 : 0.65 + 0.35 * s.n.y;
    float3 ambient = gAmbient.rgb * skyCurve(s.sky) * hemi;
    float3 block = gBlockColor.rgb * blockCurve(s.block);
    float3 cave = gCave.rgb * hemi;
    float3 light = diffuse + (ambient + cave) * s.ao + block * lerp(1.0, s.ao, 0.7);
    float3 c = s.albedo * light;
    if (s.mat == kMatGlow) c += s.albedo * gShadeMisc.z;
    else if (s.mat == kMatEmissive) c += s.albedo * gShadeMisc.z * smoothstep(0.45, 0.85, max(max(s.albedo.r, s.albedo.g), s.albedo.b));
    return c;
}

// Aerial perspective: blends towards the sky behind (linear HDR), plus the vanilla end fog so the far
// terrain meets the sky exactly where the world ends.
float3 applyFog(float3 c, float3 rel, float3 skyBehind) {
    float t = exp(-fogDepth(rel));
    c = lerp(skyBehind, c, t);
    float dist = length(rel);
    float vanilla = saturate((dist - gSkyHorizon.w) / max(gShadeMisc.w - gSkyHorizon.w, 1e-3));
    return lerp(c, skyBehind, vanilla);
}

// Vanilla LDR (sRGB) sky stage colour -> linear HDR.
float3 skyToHdr(float3 srgb) { return srgbToLinear(srgb) * gAmbient.w; }
// The sky stage layer (cleared to zero: untouched pixels show the fog colour, like the vanilla clear).
float4 skyStage(int2 px) {
    float4 s = gTex[kTexLdrSky].Load(int3(px, 0));
    return s.a > 0 ? s : float4(gFogColor.rgb, 1);
}

#endif
