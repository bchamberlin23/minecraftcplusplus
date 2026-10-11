// Shader pipeline, screen passes before TAA:
//   DeferredCS    lights the G-buffer (terrain), relights the forward layer (entities), turns the vanilla sky
//                 stage into HDR and applies aerial perspective. Writes the scene, its copy (refraction / SSR
//                 source for water) and view depth.
//   VolumetricCS  half (or quarter) resolution ray march through the shadow map: god rays / underwater shafts.
//   CompositeCS   applies volumetrics + underwater fog, then composites the late LDR layer (clouds, weather).
#include "common.hlsli"
#include "shading.hlsli"

float vanillaToHdrScale() {
    return luminance(gAmbient.rgb) + luminance(gLightColor.rgb) * 0.6 + gBlockColor.w + 0.02;
}

// Forward layer (World stage, see entity.hlsl): opaque pixels store alpha = (128 + code) / 255 with code =
// sky << 3 | block >> 1, 126 = full bright, 127 = a pass unaware of the pipeline (vanilla-lit colour, alpha 1);
// translucent pixels store their coverage * 0.49.
float3 shadeForward(float4 e, float3 rel, float2 pixel) {
    uint code = uint(round(e.a * 255.0)) - 128;
    float3 albedo = srgbToLinear(e.rgb);
    if (code >= 127) return albedo * vanillaToHdrScale();
    if (code == 126) return albedo * (1.0 + gShadeMisc.z * 0.5);
    Surface s;
    s.albedo = albedo;
    s.n = gLightDir.xyz;  // the albedo already carries the game's directional entity shading
    s.sky = float(code >> 3);
    s.block = float(code & 7) * (15.0 / 7.0);
    s.ao = 1;
    s.mat = kMatDefault;
    return shadeSurface(s, rel, pixel) * 0.8;
}

// ---- screen-space ambient occlusion (ultra): half resolution, on top of the baked per-vertex AO -------

[numthreads(8, 8, 1)]
void AoCS(uint2 px : SV_DispatchThreadID) {
    uint2 size = (uint2(gScreen.xy) + 1) >> 1;
    if (any(px >= size)) return;
    uint2 full = min(px * 2, uint2(gScreen.xy) - 1);
    float d = gTex[kTexDepth].Load(int3(full, 0)).r;
    if (d <= 0) {
        gRW[kRwAo][px] = 1;
        return;
    }
    float2 pixel = float2(full) + 0.5;
    float3 p = relPosition(pixel, d);
    float3 n = octDecode(gTex[kTexNormal].Load(int3(full, 0)).xy);
    float z = viewDepth(d);
    const float radius = 1.2;  // blocks
    float pixRadius = radius / (z * length(gRayY.xyz)) * gScreen.y * 0.5;
    if (pixRadius < 1.0) {
        gRW[kRwAo][px] = 1;
        return;
    }
    pixRadius = min(pixRadius, 96.0);
    float noise = ign(float2(px), gFlags.y) * 2 * kPi;
    float2x2 rot = float2x2(cos(noise), -sin(noise), sin(noise), cos(noise));
    float occ = 0;
    [unroll] for (uint i = 0; i < 8; ++i) {
        float2 o = mul(rot, kVogel16[i * 2]) * pixRadius;
        int2 q = clamp(int2(pixel + o), 0, int2(gScreen.xy) - 1);
        float qd = gTex[kTexDepth].Load(int3(q, 0)).r;
        if (qd <= 0) continue;
        float3 v = relPosition(float2(q) + 0.5, qd) - p;
        float len = length(v);
        float fall = saturate(1.0 - len / (radius * 2.0));
        occ += saturate(dot(v, n) / max(len, 1e-3) - 0.15) * fall;
    }
    gRW[kRwAo][px] = saturate(1.0 - occ / 8.0 * 1.6);
}

[numthreads(8, 8, 1)]
void DeferredCS(uint2 px : SV_DispatchThreadID) {
    if (any(px >= uint2(gScreen.xy))) return;
    float2 pixel = float2(px) + 0.5;
    float d = gTex[kTexDepth].Load(int3(px, 0)).r;
    float4 skyLdr = skyStage(int2(px));
    float3 skyHdr = skyToHdr(skyLdr.rgb);
    float3 dir = normalize(viewRay(pixelNdc(pixel)));
    float mu = dot(dir, gSunDir.xyz);
    // HDR sun disc (the vanilla sun texture's bright core) and a soft forward-scattering glow around it.
    float lum = luminance(skyLdr.rgb);
    float disc = mu > 0.94 ? pow(saturate(lum), 6.0) * smoothstep(0.94, 0.97, mu) : 0;
    float3 glow = gLightColor.rgb * gLightColor.w * (pow(saturate(mu), 24.0) * 0.025 + pow(saturate(mu), 300.0) * 0.4) *
                  (1.0 - gTime.y * 0.9);
    float3 sky = skyHdr * (1.0 + gSunDir.w * disc) + glow * (dir.y > -0.05 ? 1 : 0);

    float3 c;
    float z;
    if (d <= 0) {
        c = sky;
        z = 1e6;
    } else {
        float3 rel = relPosition(pixel, d);
        z = viewDepth(d);
        float4 e = gTex[kTexLdrFwd].Load(int3(px, 0));
        if (e.a >= 0.499) {
            c = shadeForward(e, rel, pixel);
        } else {
            float4 a = gTex[kTexAlbedo].Load(int3(px, 0));
            float4 nl = gTex[kTexNormal].Load(int3(px, 0));
            uint code = uint(round(a.a * 255.0));
            Surface s;
            s.albedo = a.rgb;
            s.n = octDecode(nl.xy);
            s.sky = nl.z * 15.0;
            s.block = nl.w * 15.0;
            s.ao = float(code >> 3) / 31.0;
            s.mat = code & 7;
            if ((gFlags.x & kFlagSsao) != 0) s.ao *= gTex[kTexAo].SampleLevel(gLinearClamp, pixel * gScreen.zw, 0).r;
            c = shadeSurface(s, rel, pixel);
            uint dbg = debugView();
            if (dbg == 2) c = s.albedo;
            else if (dbg == 3) c = s.n * 0.5 + 0.5;
            else if (dbg == 4) {
                float th;
                c = shadowVisibility(rel, s.n, pixel, th).xxx;
            } else if (dbg == 5) {
                s.albedo = 0.18;
                c = shadeSurface(s, rel, pixel);
            }
            if (dbg >= 2) {
                gRW[kRwScene][px] = float4(c, 1);
                gRW[kRwSceneCopy][px] = float4(c, 1);
                gRW[kRwLinDepth][px] = float4(viewDepth(d), 0, 0, 0);
                return;
            }
            if (e.a > 0) {  // translucent entity layer over the terrain (vanilla-lit colours)
                float cover = saturate(e.a / 0.49);
                float3 col = srgbToLinear(saturate(e.rgb / max(e.a, 1e-3))) * vanillaToHdrScale();
                c = lerp(c, col, cover);
            }
        }
        if (gWater.w < 0.5) c = applyFog(c, rel, sky);
    }
    gRW[kRwScene][px] = float4(c, 1);
    gRW[kRwSceneCopy][px] = float4(c, 1);
    gRW[kRwLinDepth][px] = float4(z, 0, 0, 0);
}

// ---- volumetric light ----------------------------------------------------------------------------------

float phaseHG(float mu, float g) {
    float g2 = g * g;
    return (1 - g2) / (4 * kPi * pow(abs(1 + g2 - 2 * g * mu), 1.5));
}

// gPassA.x = downscale shift (1 = half, 2 = quarter resolution).
[numthreads(8, 8, 1)]
void VolumetricCS(uint2 px : SV_DispatchThreadID) {
    uint shift = gPassA.x;
    uint2 size = (uint2(gScreen.xy) + (1u << shift) - 1) >> shift;
    if (any(px >= size)) return;
    uint2 full = min(px << shift, uint2(gScreen.xy) - 1);
    float2 pixel = float2(full) + 0.5;
    float d = gTex[kTexDepth].Load(int3(full, 0)).r;
    bool under = gWater.w > 0.5;
    float maxDist = under ? 48.0 : min(gLightDir.w, 160.0);
    float3 ray = viewRay(pixelNdc(pixel));
    float3 dir = normalize(ray);
    float dist = d > 0 ? viewDepth(d) * length(ray) : 1e6;
    float marchDist = min(dist, maxDist);
    uint steps = gOpts.w;
    float jitter = ign(float2(px), gFlags.y);
    float mu = dot(dir, gLightDir.xyz);
    float phase = lerp(1.0 / (4 * kPi), phaseHG(mu, 0.7), 0.6) * 4 * kPi;
    float3 light = gLightColor.rgb;
    float sigma = under ? gWaterScatter.w : gFog.x * gFog.w * (1.0 + gTime.y * 3.0);
    float3 scatter = 0;
    float trans = 1;
    float dt = marchDist / float(steps);
    [loop] for (uint i = 0; i < steps; ++i) {
        float t = (float(i) + jitter) * dt;
        float3 p = dir * t;
        float height = under ? 1.0 : exp(-gFog.y * max(gTime.w + p.y - gFog.z, -64.0));
        float s = sigma * height;
        float3 sp;
        int c = shadowCascade(p, sp);
        float vis = 1;
        if (c >= 0) vis = gShadowMap.SampleCmpLevelZero(gShadowCmp, float3(sp.xy * float2(0.5, -0.5) + 0.5, c), sp.z);
        float stepT = exp(-s * dt);
        scatter += trans * (1 - stepT) * vis;
        trans *= stepT;
    }
    float3 tint = under ? gWaterScatter.rgb : 1.0;
    float strength = under ? 1.0 : 1.0;
    gRW[kRwVol][px] = float4(scatter * light * phase * tint * strength, under ? 1 : trans);
}

// ---- composite ------------------------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void CompositeCS(uint2 px : SV_DispatchThreadID) {
    if (any(px >= uint2(gScreen.xy))) return;
    float2 pixel = float2(px) + 0.5;
    float4 c = gRW[kRwScene][px];
    float d = gTex[kTexDepth].Load(int3(px, 0)).r;
    float3 ray = viewRay(pixelNdc(pixel));
    float dist = d > 0 ? viewDepth(d) * length(ray) : 1e6;
    if (gWater.w > 0.5) {  // underwater: absorption and in-scatter over the whole view distance
        float3 t = exp(-gWater.rgb * 0.45 * min(dist, 512.0));
        c.rgb = c.rgb * t + gWaterScatter.rgb * (1 - t);
    }
    if ((gFlags.x & kFlagVolumetric) != 0) {
        // Depth-aware upsample of the low resolution march.
        uint shift = gPassA.x;
        float2 lp = pixel / float(1u << shift) - 0.5;
        int2 base = int2(floor(lp));
        float2 f = lp - base;
        float z = viewDepth(max(d, 1e-9));
        float4 sum = 0;
        float wsum = 0;
        uint2 lsize = (uint2(gScreen.xy) + (1u << shift) - 1) >> shift;
        [unroll] for (int k = 0; k < 4; ++k) {
            int2 o = int2(k & 1, k >> 1);
            int2 q = clamp(base + o, 0, int2(lsize) - 1);
            uint2 fq = min(uint2(q) << shift, uint2(gScreen.xy) - 1);
            float qd = gTex[kTexDepth].Load(int3(fq, 0)).r;
            float qz = viewDepth(max(qd, 1e-9));
            float w = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y) + 1e-4;
            w *= 1.0 / (1e-3 + abs(qz - z) / max(z, 1e-3));
            sum += gTex[kTexVol].Load(int3(q, 0)) * w;
            wsum += w;
        }
        float4 v = sum / wsum;
        c.rgb = c.rgb * v.a + v.rgb;
    }
    // Late layer (clouds, rain / snow): premultiplied over, converted from the vanilla LDR colours.
    float4 o = gTex[kTexLdrSky].Load(int3(px, 0));
    if (o.a > 0) {
        float3 col = srgbToLinear(saturate(o.rgb / o.a)) * gAmbient.w * 0.9;
        c.rgb = col * o.a + c.rgb * (1 - o.a);
    }
    gRW[kRwScene][px] = float4(max(c.rgb, 0), 1);
}
