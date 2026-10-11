// Shader pipeline, screen passes after the scene is complete:
//   TaaCS          temporal anti-aliasing (jittered scene + reprojected history -> new history)
//   BloomDownCS / BloomUpCS / ExposureCS   HDR bloom chain and eye adaptation
//   TonemapVS / TonemapPS                  exposure, bloom, sharpening, AgX tonemap, sRGB + dither -> swap chain
#include "common.hlsli"
#include "shading.hlsli"

float3 rgbToYcocg(float3 c) {
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}
float3 ycocgToRgb(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
// Reversible tonemap for filtering HDR values (Karis).
float3 tmWeight(float3 c) { return c / (1.0 + luminance(c)); }
float3 tmUnweight(float3 c) { return c / max(1.0 - luminance(c), 1e-4); }

// Catmull-Rom history sample from 5 bilinear taps.
float3 sampleHistory(uint tex, float2 uv, float2 size) {
    float2 pos = uv * size;
    float2 center = floor(pos - 0.5) + 0.5;
    float2 f = pos - center;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 tc12 = (center + w2 / w12) / size;
    float2 tc0 = (center - 1.0) / size;
    float2 tc3 = (center + 2.0) / size;
    float3 c = gTex[tex].SampleLevel(gLinearClamp, float2(tc12.x, tc0.y), 0).rgb * (w12.x * w0.y) +
               gTex[tex].SampleLevel(gLinearClamp, float2(tc0.x, tc12.y), 0).rgb * (w0.x * w12.y) +
               gTex[tex].SampleLevel(gLinearClamp, float2(tc12.x, tc12.y), 0).rgb * (w12.x * w12.y) +
               gTex[tex].SampleLevel(gLinearClamp, float2(tc3.x, tc12.y), 0).rgb * (w3.x * w12.y) +
               gTex[tex].SampleLevel(gLinearClamp, float2(tc12.x, tc3.y), 0).rgb * (w12.x * w3.y);
    float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(c / wsum, 0);
}

// Temporal anti-aliasing and upscaling (render -> output resolution). The current frame is reconstructed at the
// output pixel from the 3x3 render pixels around it, each weighted by the distance of its jittered sample to the
// pixel (Gaussian); the history is reprojected with the motion vector of the nearest surface and clipped to the
// neighbourhood's variance box. gPassA.x = history valid (0 after a reset or resize).
[numthreads(8, 8, 1)]
void TaaCS(uint2 px : SV_DispatchThreadID) {
    if (any(px >= uint2(gOutput.xy))) return;
    int2 maxp = int2(gScreen.xy) - 1;
    float2 uvOut = (float2(px) + 0.5) * gOutput.zw;
    float2 target = uvOut * gScreen.xy;  // render pixel space
    float2 jit = jitterPixels();
    int2 base = int2(floor(target - jit));
    float3 sum = 0, m1 = 0, m2 = 0, mn = 1e9, mx = -1e9;
    float wsum = 0, wmax = 0, bestD = 0;
    int2 bestP = clamp(base, 0, maxp);
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            int2 q = clamp(base + int2(x, y), 0, maxp);
            float3 s = rgbToYcocg(tmWeight(gTex[kTexScene].Load(int3(q, 0)).rgb));
            float2 d = float2(q) + 0.5 + jit - target;
            float w = exp(-2.9 * dot(d, d));
            sum += s * w;
            wsum += w;
            wmax = max(wmax, w);
            m1 += s;
            m2 += s * s;
            mn = min(mn, s);
            mx = max(mx, s);
            float dz = gTex[kTexDepth].Load(int3(q, 0)).r;
            if (dz > bestD) bestD = dz, bestP = q;
        }
    float3 cur = sum / max(wsum, 1e-5);
    float2 prevUv = uvOut + gTex[kTexMotion].Load(int3(bestP, 0)).xy;
    float3 result = cur;
    bool valid = gPassA.x != 0 && all(prevUv > 0) && all(prevUv < 1);
    if (valid) {
        float3 hist = rgbToYcocg(tmWeight(sampleHistory(kTexHist0 + gFlags.z, prevUv, gOutput.xy)));
        float3 mean = m1 / 9.0;
        float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0));
        bool dynamic = gTex[kTexLdrFwd].Load(int3(clamp(int2(target), 0, maxp), 0)).a > 0;  // entities move on their own
        float gamma = dynamic ? 0.75 : 1.25;
        float3 lo = max(mn, mean - gamma * sigma), hi = min(mx, mean + gamma * sigma);
        float3 center = (lo + hi) * 0.5, ext = max((hi - lo) * 0.5, 1e-5);
        float3 off = hist - center;
        float3 ts = abs(off / ext);
        float tmax = max(ts.x, max(ts.y, ts.z));
        if (tmax > 1) hist = center + off / tmax;
        // Less history under fast motion; when upscaling, a render sample far from this pixel counts for less.
        float motionPx = length((prevUv - uvOut) * gOutput.xy);
        float alpha = lerp(0.08, 0.18, saturate(motionPx * 0.25)) * saturate(wmax * 1.6);
        if (dynamic) alpha = max(alpha, 0.25);
        result = lerp(hist, cur, alpha);
    }
    gRW[kRwHist0 + gFlags.w][px] = float4(tmUnweight(ycocgToRgb(result)), 1);
}

// ---- bloom ------------------------------------------------------------------------------------------------

// gPassA.x = source texture slot (first level: the TAA history SRV, sampled; others: bloom UAVs, loaded),
// gPassA.y = destination UAV slot, gPassA.z = 1 for the first level, gPassB.xy = destination size.
[numthreads(8, 8, 1)]
void BloomDownCS(uint2 px : SV_DispatchThreadID) {
    if (any(px >= gPassB.xy)) return;
    float3 c = 0;
    if (gPassA.z) {
        // 2x2 bilinear taps of the full resolution history, Karis-weighted against fireflies.
        float2 uv = (float2(px) * 2.0 + 1.0) * gOutput.zw;
        float wsum = 0;
        [unroll] for (int k = 0; k < 4; ++k) {
            float2 o = float2(k & 1 ? 1 : -1, k & 2 ? 1 : -1) * gOutput.zw;
            float3 s = gTex[gPassA.x].SampleLevel(gLinearClamp, uv + o, 0).rgb;
            float w = 1.0 / (1.0 + luminance(s));
            c += s * w;
            wsum += w;
        }
        c /= wsum;
        // Soft-knee threshold on the exposed luminance: only highlights (sun, emissive blocks, specular) bloom.
        float e = max(gState[0].x, 1e-3);
        float l = luminance(c) * e;
        float knee = 0.5, threshold = 1.0;
        float soft = clamp(l - threshold + knee, 0, 2 * knee);
        soft = soft * soft / (4 * knee + 1e-4);
        c *= max(soft, l - threshold) / max(l, 1e-4);
    } else {
        // 4x4 tent ([1 3 3 1] squared) of the previous level.
        uint2 srcSize = gPassB.zw;
        static const float kW[4] = {1, 3, 3, 1};
        [unroll] for (int y = 0; y < 4; ++y)
            [unroll] for (int x = 0; x < 4; ++x) {
                int2 q = clamp(int2(px) * 2 + int2(x, y) - 1, 0, int2(srcSize) - 1);
                c += gRW[gPassA.x][q].rgb * (kW[x] * kW[y]);
            }
        c /= 64.0;
    }
    gRW[gPassA.y][px] = float4(c, 1);
}

// Destination level += tent upsample of the smaller level. gPassA.x = small level UAV, gPassA.y = destination
// UAV, gPassB.xy = destination size, gPassB.zw = small level size, gPassA.z = weight of the small level.
[numthreads(8, 8, 1)]
void BloomUpCS(uint2 px : SV_DispatchThreadID) {
    if (any(px >= gPassB.xy)) return;
    float2 p = (float2(px) + 0.5) * 0.5 - 0.5;
    int2 b = int2(floor(p));
    float2 f = p - b;
    float3 c = 0;
    // Bilinear of a 3x3 tent on the small level: tent weights at the 4 bilinear corners' neighbourhoods.
    [unroll] for (int y = -1; y <= 2; ++y)
        [unroll] for (int x = -1; x <= 2; ++x) {
            float wx = x == -1 ? (1 - f.x) * 0.25 : x == 0 ? (1 - f.x) * 0.5 + f.x * 0.25 : x == 1 ? (1 - f.x) * 0.25 + f.x * 0.5 : f.x * 0.25;
            float wy = y == -1 ? (1 - f.y) * 0.25 : y == 0 ? (1 - f.y) * 0.5 + f.y * 0.25 : y == 1 ? (1 - f.y) * 0.25 + f.y * 0.5 : f.y * 0.25;
            int2 q = clamp(b + int2(x, y), 0, int2(gPassB.zw) - 1);
            c += gRW[gPassA.x][q].rgb * (wx * wy);
        }
    float w = asfloat(gPassA.z);
    gRW[gPassA.y][px] = float4(gRW[gPassA.y][px].rgb + c * w, 1);
}

groupshared float gsLum[256];
groupshared float gsWeight[256];

// One group: average log luminance of the image (64 x 64 bilinear samples, centre weighted), adapted over
// time. gPassA.x = history texture slot, gPassA.y = asuint(frame seconds), gPassA.z = reset.
[numthreads(16, 16, 1)]
void ExposureCS(uint index : SV_GroupIndex) {
    float sum = 0, wsum = 0;
    uint2 size = uint2(64, 64);
    for (uint i = index; i < size.x * size.y; i += 256) {
        uint2 q = uint2(i % size.x, i / size.x);
        float2 uv = (float2(q) + 0.5) / float2(size);
        float2 c = uv - 0.5;
        float w = exp(-dot(c, c) * 4.0);
        float l = luminance(gTex[gPassA.x].SampleLevel(gLinearClamp, uv, 0).rgb);
        sum += log2(max(l, 1e-5)) * w;
        wsum += w;
    }
    gsLum[index] = sum;
    gsWeight[index] = wsum;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128; s > 0; s >>= 1) {
        if (index < s) {
            gsLum[index] += gsLum[index + s];
            gsWeight[index] += gsWeight[index + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) {
        float avgEv = gsLum[0] / max(gsWeight[0], 1e-6);
        float avg = exp2(avgEv);
        // Partial adaptation: dark scenes stay darker (night stays night), bright ones brighter.
        float ev = lerp(log2(0.2), avgEv, 0.7);
        float key = 0.13 * gShadeMisc.x;
        float target = clamp(key / exp2(ev), 0.08, 2.2);
        float dt = asfloat(gPassA.y);
        float prev = gState[0].x;
        float speed = target > prev ? 1.6 : 2.4;
        float e = gPassA.z || !(prev > 0) ? target : exp(lerp(log(target), log(prev), exp(-dt * speed)));
        gState[0] = float4(e, avg, target, 0);
        gRW[kRwExposure][uint2(0, 0)] = e;  // 1x1 texture for external upscalers
    }
}

// ---- tonemap ----------------------------------------------------------------------------------------------

struct ScreenOut {
    float4 pos : SV_Position;
};

ScreenOut TonemapVS(uint vid : SV_VertexID) {
    ScreenOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// AgX (Sobotka) with a punchy look.
float3 agxContrast(float3 x) {
    float3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
float3 agx(float3 c) {
    const float3x3 inset = float3x3(0.842479062253094, 0.0784335999999992, 0.0792237451477643,
                                    0.0423282422610123, 0.878468636469772, 0.0791661274605434,
                                    0.0423756549057051, 0.0784336, 0.879142973793104);
    const float3x3 outset = float3x3(1.19687900512017, -0.0980208811401368, -0.0990297440797205,
                                     -0.0528968517574562, 1.15190312990417, -0.0989611768448433,
                                     -0.0529716355144438, -0.0980434501171241, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    c = mul(inset, c);
    c = clamp(log2(max(c, 1e-10)), minEv, maxEv);
    c = (c - minEv) / (maxEv - minEv);
    c = agxContrast(c);
    // Look: slope / power / saturation ("punchy").
    c = pow(max(c, 0), 1.3);
    float l = luminance(c);
    c = l + 1.25 * (c - l);
    c = mul(outset, c);
    return pow(max(c, 0), 2.2);  // AgX's output is display-encoded; back to linear for the sRGB encode below
}

// ACES filmic fit (Hill), sRGB primaries.
float3 acesFit(float3 c) {
    const float3x3 i = float3x3(0.59719, 0.35458, 0.04823, 0.07600, 0.90834, 0.01566, 0.02840, 0.13383, 0.83777);
    const float3x3 o = float3x3(1.60475, -0.53108, -0.07367, -0.10208, 1.10813, -0.00605, -0.00327, -0.07276, 1.07602);
    c = mul(i, c * 1.6);
    c = (c * (c + 0.0245786) - 0.000090537) / (c * (0.983729 * c + 0.4329510) + 0.238081);
    return saturate(mul(o, c));
}
// Hable-style filmic with gentle desaturation of highlights (no LUT).
float3 tonyMcMapface(float3 c) {
    float l = luminance(c);
    float3 t = c / (1.0 + c);
    float3 tl = l / (1.0 + l);
    float3 r = lerp(t, tl * c / max(l, 1e-5), 0.35);
    return pow(saturate(r * 1.12), 1.08);
}

// gPassA.x = history slot to show, gPassA.y = sharpening (asfloat).
float4 TonemapPS(ScreenOut i) : SV_Target {
    int2 px = int2(i.pos.xy);
    uint tex = kTexHist0 + gPassA.x;
    float3 c = gTex[tex].Load(int3(px, 0)).rgb;
    // Contrast-adaptive sharpening (cross neighbourhood) to counter the TAA blur.
    float sharp = asfloat(gPassA.y);
    if (sharp > 0) {
        int2 maxp = int2(gOutput.xy) - 1;
        float3 n = gTex[tex].Load(int3(clamp(px + int2(0, -1), 0, maxp), 0)).rgb;
        float3 s = gTex[tex].Load(int3(clamp(px + int2(0, 1), 0, maxp), 0)).rgb;
        float3 w = gTex[tex].Load(int3(clamp(px + int2(-1, 0), 0, maxp), 0)).rgb;
        float3 e = gTex[tex].Load(int3(clamp(px + int2(1, 0), 0, maxp), 0)).rgb;
        float3 tc = tmWeight(c), tn = tmWeight(n), ts = tmWeight(s), tw = tmWeight(w), te = tmWeight(e);
        float3 mn = min(tc, min(min(tn, ts), min(tw, te))), mx = max(tc, max(max(tn, ts), max(tw, te)));
        float3 amp = sqrt(saturate(min(mn, 1 - mx) / max(mx, 1e-4)));
        float3 wgt = -amp * sharp * 0.2;
        float3 r = (tc + (tn + ts + tw + te) * wgt) / (1 + 4 * wgt);
        c = tmUnweight(saturate(r));
    }
    float exposure = gState[0].x;
    // Night vision (Purkinje): dim scenes lose colour towards blue.
    float scotopic = saturate(1.0 - log2(max(gState[0].y, 1e-5) / 0.002) / 4.0);
    float lum = luminance(c);
    c = lerp(c, lum * float3(0.62, 0.78, 1.15), scotopic * 0.65 * saturate(1.0 - lum * exposure * 4.0));
    if (debugView() == 7) {  // motion vectors: render pixels per frame (x red, y green; grey = still)
        uint2 q = min(uint2(i.pos.xy * gOutput.zw * gScreen.xy), uint2(gScreen.xy) - 1);
        float2 m = gTex[kTexMotion].Load(int3(q, 0)).xy * gScreen.xy;
        return float4(saturate(0.5 + m * 0.04), 0.5, 1);
    }
    if (debugView() == 1) return float4(linearToSrgb(saturate(c)), 1);
    if (debugView() >= 2) return float4(linearToSrgb(saturate(c)), 1);
    float2 uv = i.pos.xy * gOutput.zw;
    float3 bloom = gTex[kTexBloom0].SampleLevel(gLinearClamp, uv, 0).rgb;
    c = (c + bloom * gShadeMisc.y) * exposure;
    // Gentle vignette.
    float2 v = uv - 0.5;
    c *= 1.0 - dot(v, v) * 0.35;
    if (tonemapMode() == 0) c = acesFit(c);
    else if (tonemapMode() == 1) c = agx(c);
    else c = tonyMcMapface(c);
    float3 srgb = linearToSrgb(saturate(c));
    // Triangular dither against banding in the 8-bit output.
    float n1 = ign(i.pos.xy, gFlags.y), n2 = ign(i.pos.xy + 17.3, gFlags.y + 7);
    srgb += (n1 + n2 - 1.0) / 255.0;
    return float4(srgb, 1);
}

// ---- after tonemapping ------------------------------------------------------------------------------------

// Output-resolution depth for the overlays (block outline) when rendering below the output resolution.
float DepthUpsamplePS(ScreenOut i) : SV_Depth {
    float2 uv = i.pos.xy * gOutput.zw;
    return gTex[kTexDepth].Load(int3(min(uint2(uv * gScreen.xy), uint2(gScreen.xy) - 1), 0)).r;
}

// The UI layer (overlays + Hud stage, premultiplied) over the tonemapped image; blend ONE, INV_SRC_ALPHA.
float4 UiCompositePS(ScreenOut i) : SV_Target {
    return gTex[kTexUi].Load(int3(i.pos.xy, 0));
}
