// Greedy cube quads, expanded from 16 bytes each by vertex pulling (no vertex buffers).
// TerrainVS / TerrainPS*: the vanilla look ("shaders off"). TerrainGbuf*, TerrainShadow*, TerrainFwd*: the shader
// pipeline (G-buffer, shadow maps, lit translucent surfaces incl. water), see src/render/shading.cpp.
#include "common.hlsli"
#include "shading.hlsli"

StructuredBuffer<uint4> gQuads : register(t0);  // CubeQuad: geo, tex | ao, sky corners, block corners

static const float kShade[6] = {0.5, 1.0, 0.8, 0.8, 0.6, 0.6};
static const float3 kNormal[6] = {
    float3(0, -1, 0), float3(0, 1, 0), float3(0, 0, -1), float3(0, 0, 1), float3(-1, 0, 0), float3(1, 0, 0)};

struct CubeVertex {
    uint4 q;
    uint corner, face;
    int4 origin;
    float3 p;    // section-local position (blocks)
    float3 rel;  // camera-relative position
    float2 st;   // texture coordinates
};

CubeVertex pullCube(uint vid) {
    CubeVertex v;
    uint4 q = gQuads[gDraw.quadOffset + (vid >> 2)];
    uint corner = vid & 3;
    uint face = (q.x >> 20) & 7;
    // Faces whose (A x B) points along the normal swap corners 1/2 to stay front-facing.
    if (((0x19u >> face) & 1u) != 0 && (corner == 1 || corner == 2)) corner ^= 3;

    float a = float(corner & 1), b = float(corner >> 1);
    float w = float(((q.x >> 12) & 15) + 1), h = float(((q.x >> 16) & 15) + 1);
    float3 axisA = face < 4 ? float3(1, 0, 0) : float3(0, 0, 1);
    float3 axisB = face < 2 ? float3(0, 0, 1) : float3(0, 1, 0);
    int4 origin = gSections[gDraw.sectionIndex].origin;
    float3 p = float3(q.x & 15, (q.x >> 4) & 15, (q.x >> 8) & 15);
    p += max(kNormal[face], 0) + axisA * (a * w) + axisB * (b * h);
    p.xz *= float(1u << origin.w);  // LOD columns span 2^level blocks (heights stay exact); UVs stay in block units

    // Canonical face UVs (same convention as the game's automatic UVs), then rotation.
    float2 uv;
    switch (face) {
        case 0: uv = float2(p.x, -p.z); break;
        case 1: uv = float2(p.x, p.z); break;
        case 2: uv = float2(-p.x, -p.y); break;
        case 3: uv = float2(p.x, -p.y); break;
        case 4: uv = float2(p.z, -p.y); break;
        default: uv = float2(-p.z, -p.y); break;
    }
    uint rot = (q.x >> 23) & 3;
    v.st = rot == 0 ? uv : rot == 1 ? float2(uv.y, -uv.x) : rot == 2 ? -uv : float2(-uv.y, uv.x);
    v.q = q;
    v.corner = corner;
    v.face = face;
    v.origin = origin;
    v.p = p;
    v.rel = float3(origin.xyz - gCamBlock.xyz) - gCamFrac.xyz + p;
    return v;
}

float cornerSky(CubeVertex v) { return float((v.q.z >> (v.corner * 8)) & 0xFF) / 16.0; }
float cornerBlock(CubeVertex v) { return float((v.q.w >> (v.corner * 8)) & 0xFF) / 16.0; }
float cornerAo(CubeVertex v) { return 1.0 - 0.2 * float((v.q.y >> (12 + v.corner * 2)) & 3); }
uint colorIndex(CubeVertex v) { return ((v.q.y >> 20) << 4) | ((v.q.x >> 25) & 15); }

// ---- vanilla ---------------------------------------------------------------------------------------------

VSOut TerrainVS(uint vid : SV_VertexID) {
    CubeVertex v = pullCube(vid);
    VSOut o;
    o.pos = mul(gViewProj, float4(v.rel, 1));
    o.uvl = float3(v.st, float(v.q.y & 0xFFF));
    // Smooth lighting: this corner's light (4.4 fixed point) and ambient occlusion (1 - 0.2 per solid cell).
    o.color = tintColor(colorIndex(v)) * (kShade[v.face] * cornerAo(v)) * lightColorLevels(cornerSky(v), cornerBlock(v));
    o.fog = fogFactor(v.rel);
    return o;
}

float4 TerrainPSOpaque(VSOut i) : SV_Target {
    return float4(shadePixel(i).rgb, 1);
}

float4 TerrainPSCutout(VSOut i) : SV_Target {
    float4 c = shadePixel(i);
    clip(c.a - 0.5);
    return float4(c.rgb, 1);
}

float4 TerrainPSTranslucent(VSOut i) : SV_Target {
    return shadePixel(i);
}

// ---- shader pipeline -------------------------------------------------------------------------------------

// Waving foliage: a world-anchored gust field (same in the shadow pass so shadows follow).
float3 waveOffset(float3 rel, uint mode, float weight) {
    if (mode == 0 || (gFlags.x & kFlagWaving) == 0) return 0;
    float3 w = rel + float3(gJitter.z, gTime.w, gJitter.w);
    float t = gTime.x;
    float gust = 0.6 + 0.4 * sin(t * 0.35 + w.x * 0.02 + w.z * 0.015);
    float a = sin(t * 1.9 + dot(w.xz, float2(0.62, 0.79)) * 1.2 + w.y * 0.4);
    float b = sin(t * 2.7 + dot(w.xz, float2(-0.71, 0.48)) * 1.9 + w.y * 0.3) * 0.5;
    float c = cos(t * 1.4 + dot(w.xz, float2(0.83, -0.31)) * 1.1);
    float amp = (mode == 3 ? 0.035 : 0.07) * gust * weight * (1.0 + gTime.y * 1.5);
    return float3(a + b, (mode == 3 ? c * 0.3 : 0), c + b) * amp;
}

struct GbufVSOut {
    float4 pos : SV_Position;
    float3 uvl : TEXCOORD0;
    float3 tint : COLOR0;
    float3 light : TEXCOORD1;  // sky level, block level, ao
    nointerpolation uint info : INFO;  // face | material class << 4
};

GbufVSOut TerrainGbufVS(uint vid : SV_VertexID) {
    CubeVertex v = pullCube(vid);
    uint layer = v.q.y & 0xFFF;
    uint m = gMaterial[layer];
    float3 rel = v.rel + waveOffset(v.rel, matWave(m) == 3 ? 3 : 0, 1);
    GbufVSOut o;
    o.pos = mul(gViewProj, float4(rel, 1));
    o.uvl = float3(v.st, float(layer));
    o.tint = tintColor(colorIndex(v));
    o.light = float3(cornerSky(v), cornerBlock(v), cornerAo(v));
    o.info = v.face | (matClass(m) << 4);
    return o;
}

struct GbufTargets {
    float4 albedo : SV_Target0;  // sRGB albedo, a = ao (5 bits) << 3 | material (3 bits)
    float4 normal : SV_Target1;  // octahedral normal, sky / 15, block / 15
};

GbufTargets gbufWrite(float3 albedo, float3 n, float3 light, uint mat) {
    GbufTargets o;
    uint ao = uint(saturate(light.z) * 31.0 + 0.5);
    o.albedo = float4(srgbToLinear(albedo), float((ao << 3) | (mat & 7)) / 255.0);  // the target encodes sRGB
    o.normal = float4(octEncode(n), light.x / 15.0, light.y / 15.0);
    return o;
}

GbufTargets TerrainGbufPS(GbufVSOut i) {
    float4 c = gTextures.Sample(gSampler, i.uvl);
    return gbufWrite(c.rgb * i.tint, kNormal[i.info & 7], i.light, i.info >> 4);
}

GbufTargets TerrainGbufPSCutout(GbufVSOut i) {
    float4 c = gTextures.Sample(gSampler, i.uvl);
    clip(c.a - 0.5);
    return gbufWrite(c.rgb * i.tint, kNormal[i.info & 7], i.light, i.info >> 4);
}

struct ShadowVSOut {
    float4 pos : SV_Position;
    float3 uvl : TEXCOORD0;
};

ShadowVSOut TerrainShadowVS(uint vid : SV_VertexID) {
    CubeVertex v = pullCube(vid);
    uint layer = v.q.y & 0xFFF;
    uint m = gMaterial[layer];
    float3 rel = v.rel + waveOffset(v.rel, matWave(m) == 3 ? 3 : 0, 1);
    ShadowVSOut o;
    o.pos = mul(gShadowMat[gPassA.x], float4(rel, 1));
    o.uvl = float3(v.st, float(layer));
    return o;
}

void TerrainShadowPSCutout(ShadowVSOut i) {
    clip(gTextures.Sample(gSampler, i.uvl).a - 0.5);
}

// ---- translucent surfaces (water, glass, ice) ------------------------------------------------------------

struct FwdVSOut {
    float4 pos : SV_Position;
    float3 uvl : TEXCOORD0;
    float3 tint : COLOR0;
    float3 light : TEXCOORD1;
    float3 rel : TEXCOORD2;
    nointerpolation uint info : INFO;
};

FwdVSOut TerrainFwdVS(uint vid : SV_VertexID) {
    CubeVertex v = pullCube(vid);
    uint layer = v.q.y & 0xFFF;
    FwdVSOut o;
    o.pos = mul(gViewProj, float4(v.rel, 1));
    o.uvl = float3(v.st, float(layer));
    o.tint = tintColor(colorIndex(v));
    o.light = float3(cornerSky(v), cornerBlock(v), cornerAo(v));
    o.rel = v.rel;
    o.info = v.face | (matClass(gMaterial[layer]) << 4);
    return o;
}

// Sum of directional waves: gradient of the height field (blocks).
float2 waveGradient(float2 p, float t) {
    static const float4 kWaves[5] = {
        float4(0.83, 0.55, 0.55, 1.1), float4(-0.45, 0.89, 0.95, 1.5), float4(0.25, -0.97, 1.7, 2.1),
        float4(-0.92, -0.38, 2.9, 2.7), float4(0.6, -0.8, 4.3, 3.4)};
    static const float kAmp[5] = {0.035, 0.022, 0.012, 0.007, 0.004};
    float2 g = 0;
    [unroll] for (uint k = 0; k < 5; ++k) {
        float2 d = kWaves[k].xy;
        float f = kWaves[k].z;
        g += d * (kAmp[k] * f * cos(dot(d, p) * f + t * kWaves[k].w));
    }
    return g;
}

float2 projectUv(float3 rel, out float viewZ) {
    float4 c = mul(gViewProj, float4(rel, 1));
    viewZ = c.w;
    return float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
}

// Screen-space reflection against the opaque scene; returns rgb + confidence.
float4 traceReflection(float3 rel, float3 r, float2 pixel) {
    uint steps = gOpts.z;
    if (steps == 0) return 0;
    float jitter = ign(pixel, gFlags.y);
    float startZ;
    projectUv(rel, startZ);
    float stepLen = max(0.25, startZ * 0.02);
    float t = stepLen * (0.5 + jitter);
    float prevT = 0;
    [loop] for (uint i = 0; i < steps; ++i) {
        float3 q = rel + r * t;
        float qz;
        float2 uv = projectUv(q, qz);
        if (qz <= 0.05 || any(uv < 0) || any(uv > 1)) break;
        float sceneZ = gTex[kTexLinDepth].SampleLevel(gPointClamp, uv, 0).r;
        float diff = qz - sceneZ;
        if (diff > 0 && diff < max(1.0, t * 0.25)) {
            // Refine between the last two samples.
            float lo = prevT, hi = t;
            [unroll] for (uint k = 0; k < 4; ++k) {
                float mid = (lo + hi) * 0.5;
                float mz;
                float2 muv = projectUv(rel + r * mid, mz);
                if (mz > gTex[kTexLinDepth].SampleLevel(gPointClamp, muv, 0).r) hi = mid;
                else lo = mid;
            }
            float hz;
            float2 huv = projectUv(rel + r * hi, hz);
            float2 edge = saturate(min(huv, 1 - huv) * 12.0);
            float conf = edge.x * edge.y * (1.0 - float(i) / float(steps));
            if (gTex[kTexLinDepth].SampleLevel(gPointClamp, huv, 0).r > 1e5) conf = 0;  // sky: use the sky model
            return float4(gTex[kTexSceneCopy].SampleLevel(gLinearClamp, huv, 0).rgb, conf);
        }
        prevT = t;
        t += stepLen;
        stepLen *= 1.18;
    }
    return 0;
}

float4 TerrainFwdPS(FwdVSOut i) : SV_Target {
    uint face = i.info & 7, mat = i.info >> 4;
    float2 pixel = i.pos.xy;
    float3 v = -normalize(i.rel);
    float dist = length(i.rel);
    float3 skyBehind = skyToHdr(skyStage(int2(pixel)).rgb);
    bool under = gWater.w > 0.5;

    if (mat == kMatWater) {
        float3 n = kNormal[face];
        float2 wp = i.rel.xz + gJitter.zw;
        float2 g = waveGradient(wp, gTime.x) * (1.0 + gTime.y * 0.6);
        if (face == 1) n = normalize(float3(-g.x, 1, -g.y));
        else n = normalize(n + float3(-g.x, 0, -g.y) * 0.5);
        if (dot(n, v) < 0 && face != 1) n = -n;
        float3 nv = face == 1 && under ? -n : n;
        float waterZ = mul(gViewProj, float4(i.rel, 1)).w;
        float2 uv = pixel * gScreen.zw;
        float sceneZ = gTex[kTexLinDepth].Load(int3(pixel, 0)).r;
        float thickness = max(0, sceneZ - waterZ);
        // Refraction: offset by the wave normal, only onto things behind the surface.
        float2 ruv = uv + nv.xz * 0.04 * saturate(thickness * 0.5) / max(1.0, waterZ * 0.05);
        float rz = gTex[kTexLinDepth].SampleLevel(gPointClamp, ruv, 0).r;
        if (rz < waterZ) ruv = uv, rz = sceneZ;
        float3 refracted = gTex[kTexSceneCopy].SampleLevel(gLinearClamp, ruv, 0).rgb;
        float3 tintLin = srgbToLinear(i.tint);
        float depthThrough = max(0, rz - waterZ) * (length(i.rel) / max(waterZ, 1e-3));
        if (rz > 1e5) depthThrough = 64;
        float3 absorb = gWater.rgb * (0.4 + 0.6 * (1.0 - tintLin));
        float3 trans = exp(-absorb * depthThrough);
        float lightLevel = skyCurve(i.light.x);
        float3 lit = gAmbient.rgb * lightLevel + gLightColor.rgb * saturate(gLightDir.y) * 0.5 * lightLevel +
                     gBlockColor.rgb * blockCurve(i.light.y);
        float3 scatter = tintLin * lit * 0.12;
        float3 body = refracted * trans + scatter * (1 - trans);
        if (under) {
            // From below: Snell's window (refracted world above), total internal reflection outside it.
            float cosI = abs(dot(v, n));
            float sinT2 = 1.777 * (1.0 - cosI * cosI);  // (n_water / n_air)^2 * sin^2
            float tir = smoothstep(0.85, 1.0, sinT2);
            float3 inside = gWaterScatter.rgb * 0.6;
            return float4(lerp(refracted * 0.9, inside, tir), 1);
        }
        // Reflection.
        float3 r = reflect(-v, n);
        r.y = abs(r.y);
        float ndv = saturate(dot(n, v));
        float fresnel = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);
        // Sky in the reflected direction: the rendered sky (sun, clouds) where that direction is on screen.
        float3 skyRefl = skyColor(r);
        float4 rc = mul(gViewProj, float4(r, 0));
        if (rc.w > 0) {
            float2 suv = float2(rc.x / rc.w * 0.5 + 0.5, 0.5 - rc.y / rc.w * 0.5);
            float2 edge = saturate(min(suv, 1 - suv) * 8.0);
            if (all(suv > 0) && all(suv < 1) && gTex[kTexLinDepth].SampleLevel(gPointClamp, suv, 0).r > 1e5)
                skyRefl = lerp(skyRefl, gTex[kTexSceneCopy].SampleLevel(gLinearClamp, suv, 0).rgb, edge.x * edge.y);
        }
        skyRefl = skyRefl * skyCurve(i.light.x) + gBlockColor.w;
        float3 refl = skyRefl;
        if ((gFlags.x & kFlagSsr) != 0) {
            float4 ssr = traceReflection(i.rel, r, pixel);
            refl = lerp(skyRefl, ssr.rgb, ssr.a);
        }
        float shadowThick;
        float shadow = dot(gLightColor.rgb, 1) > 0 ? shadowVisibility(i.rel, float3(0, 1, 0), pixel, shadowThick) : 0;
        float3 h = normalize(gLightDir.xyz + v);
        float ndh = saturate(dot(n, h));
        float a2 = 0.02 * 0.02;
        float dd = ndh * ndh * (a2 - 1) + 1;
        float spec = a2 / (kPi * dd * dd) * 0.25 * fresnel;
        float3 c = lerp(body, refl, fresnel) + gLightColor.rgb * spec * shadow * saturate(i.light.x / 15.0 * 2 - 1);
        return float4(applyFog(c, i.rel, skyBehind), 1);
    }

    // Glass, ice, other translucent blocks: lit like the G-buffer surfaces, with a little sky reflection.
    float4 t = gTextures.Sample(gSampler, i.uvl);
    Surface s;
    s.albedo = srgbToLinear(t.rgb * i.tint);
    s.n = kNormal[face];
    if (dot(s.n, v) < 0) s.n = -s.n;
    s.sky = i.light.x;
    s.block = i.light.y;
    s.ao = i.light.z;
    s.mat = mat == kMatGlow ? kMatGlow : kMatDefault;
    float3 c = shadeSurface(s, i.rel, pixel);
    float fresnel = 0.04 + 0.96 * pow(1.0 - saturate(dot(s.n, v)), 5.0);
    float3 refl = skyColor(reflect(-v, s.n)) * skyCurve(s.sky);
    float alpha = t.a;
    if (mat == kMatGlass || mat == kMatIce) {
        c = lerp(c, refl, fresnel * 0.6);
        alpha = lerp(alpha, 1, fresnel * 0.6);
    }
    return float4(applyFog(c, i.rel, skyBehind), alpha);
}
