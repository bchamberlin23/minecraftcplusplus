// Block entities (chests, signs, banners, heads, ...): quads baked per section into one arena, plus per-frame quads
// of animating ones. A quad's section is found by binary search over the frame's visible-section list.
// Self-contained like entity.hlsl: same light curve, two-light entity shading and fog.

cbuffer Frame : register(b0) {
    float4x4 gViewProj;  // camera-relative
    float4 gFogColor;
    float4 gFogParams;   // x = fog start, y = fog end, z = daylight, w = banner wave time (ticks mod 100)
    float4 gLight0;      // world-space diffuse light directions (the game's Lighting.setupLevel)
    float4 gLight1;
    float4 gLightmap[3]; // the game's lightmap inputs (lightmap.hlsli)
    float4x4 gPrevViewProj;  // shader pipeline: camera-relative position (this frame) -> previous clip, unjittered
    float4 gMotion;          // curUv = SV_Position.xy * gMotion.xy + gMotion.zw (PassContext::motionScale)
    float4 gMisc;            // x = 1: shader pipeline (pipeline-aware output, see entity.hlsl); y = last frame's wave time
    float4x4 gShadowMat[4];  // shadow cascades (camera-relative -> light clip)
};
#include "lightmap.hlsli"
cbuffer Draw : register(b1) {
    uint gListBase;   // static: first visible-section entry; dynamic: first quad
    uint gListCount;  // static: visible sections in the list
    uint gDynamic;
    uint gCascade;    // shadow caster draws
};

struct Quad {
    float3 pos[4];
    float2 uv[4];
    uint normal;  // snorm8 xyz
    uint color;   // 0xAABBGGRR
    uint light;   // sky << 4 | block; 256 full bright, 512 no diffuse shading, 1024 only within 16 blocks
    uint tex;     // 0xFFFFFFFF entity atlas, 0xFFFFFFFE font, 0xFFFFFFFD solid, else block texture layer
    float3 pivot; // banner flag wave pivot
    uint wave;    // 1 << 31 | phase offset << 16 | axis angle (u16 of a turn)
};
struct Batch {
    float3 origin;  // section origin - camera
    uint first;
    uint prefix;
    uint3 pad;
};
struct Corners {
    float3 p[4];  // last frame's corners of a dynamic quad (camera-relative, this frame's camera)
};

StructuredBuffer<Quad> gStatic : register(t0);
StructuredBuffer<Quad> gDyn : register(t1);
StructuredBuffer<Batch> gBatches : register(t2);
Texture2D<float4> gAtlas : register(t3);
Texture2D<float4> gFont : register(t4);
Texture2DArray<float4> gBlocks : register(t5);
StructuredBuffer<Corners> gDynPrev : register(t6);
SamplerState gPoint : register(s0);
SamplerState gBlockSampler : register(s1);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint tex : TEX;
    float4 tint : COLOR0;
    float3 light : COLOR1;
    float fog : FOG;
    float shade : SHADE;                        // directional shading alone (shader pipeline)
    nointerpolation uint lightBits : LIGHTBITS;
    float4 prevClip : PREVCLIP;                 // previous frame's clip position (motion vectors)
};

static const uint kCorner[6] = {0, 1, 2, 0, 2, 3};

float lightCurve(float level) {
    float f = level / 15.0;
    return f / (4.0 - 3.0 * f);
}
float3 lightColor(uint packed) { return vanillaLight(float((packed >> 4) & 15), float(packed & 15), gLightmap); }
float4 unpackColor(uint c) {
    return float4(c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24) / 255.0;
}
float3 unpackNormal(uint n) {
    int3 v = int3(int(n << 24) >> 24, int(n << 16) >> 24, int(n << 8) >> 24);
    return float3(v) / 127.0;
}
// Rodrigues rotation about a unit axis.
float3 rotateAxis(float3 v, float3 k, float a) {
    float c = cos(a), s = sin(a);
    return v * c + cross(k, v) * s + k * dot(k, v) * (1.0 - c);
}

// The quad of this vertex: static (section batch, origin = section - camera) or dynamic (camera-relative).
Quad fetch(uint qi, out float3 origin, out uint dynIndex) {
    origin = 0;
    dynIndex = 0xFFFFFFFF;
    if (gDynamic != 0) {
        dynIndex = gListBase + qi;
        return gDyn[dynIndex];
    }
    uint lo = 0, hi = gListCount - 1;
    while (lo < hi) {
        uint mid = (lo + hi + 1) >> 1;
        if (gBatches[gListBase + mid].prefix <= qi) lo = mid;
        else hi = mid - 1;
    }
    Batch b = gBatches[gListBase + lo];
    origin = b.origin;
    return gStatic[b.first + qi - b.prefix];
}
// BannerFlagModel: flag.xRot = (-0.0125 + 0.01 cos(2 pi phase)) pi, at wave time t.
float3 wave(Quad q, float3 p, float t, inout float3 n) {
    if (q.wave == 0) return p;
    float phase = frac((float((q.wave >> 16) & 127) + t) / 100.0);
    float a = (-0.0125 + 0.01 * cos(6.2831853 * phase)) * 3.14159265;
    float ang = float(q.wave & 0xFFFF) / 65536.0 * 6.2831853;
    float3 axis = float3(cos(ang), 0, sin(ang));
    n = rotateAxis(n, axis, a);
    return q.pivot + rotateAxis(p - q.pivot, axis, a);
}

VSOut BlockEntityVS(uint vid : SV_VertexID) {
    uint qi = vid / 6;
    uint corner = kCorner[vid % 6];
    float3 origin;
    uint dynIndex;
    Quad q = fetch(qi, origin, dynIndex);
    float3 n = unpackNormal(q.normal), nPrev = n;
    float3 rel = wave(q, q.pos[corner], gFogParams.w, n) + origin;
    float3 prevRel = dynIndex != 0xFFFFFFFF ? wave(q, gDynPrev[dynIndex].p[corner], gMisc.y, nPrev)
                                            : wave(q, q.pos[corner], gMisc.y, nPrev) + origin;
    VSOut o;
    o.pos = mul(gViewProj, float4(rel, 1));
    o.prevClip = mul(gPrevViewProj, float4(prevRel, 1));
    if (q.light & 1024) {  // glow ink outline: only within 16 blocks (decided per quad: all corners agree)
        float3 c = q.pos[0] + origin;
        if (dot(c, c) > 256.0) o.pos = float4(0, 0, 0, 1);  // degenerate: not rasterized
    }
    o.uv = q.uv[corner];
    o.tex = q.tex;
    o.tint = unpackColor(q.color);
    float3 l = 1.0;
    o.shade = 1.0;
    if (!(q.light & 256)) l = lightColor(q.light);
    if (!(q.light & 512)) {
        n = normalize(n + 1e-6);
        float d = saturate(dot(gLight0.xyz, n)) + saturate(dot(gLight1.xyz, n));
        o.shade = min(1.0, d * 0.6 + 0.4);
        l *= o.shade;
    }
    o.light = l;
    o.lightBits = q.light;
    o.fog = saturate((length(rel) - gFogParams.x) / max(gFogParams.y - gFogParams.x, 1e-3));
    return o;
}

float4 sampleTex(float2 uv, uint tex) {
    if (tex == 0xFFFFFFFF) return gAtlas.SampleLevel(gPoint, uv, 0);
    if (tex == 0xFFFFFFFE) return gFont.SampleLevel(gPoint, uv, 0);
    if (tex == 0xFFFFFFFD) return 1;
    return gBlocks.Sample(gBlockSampler, float3(uv, float(tex & 0xFFFF)));
}

float4 shade(VSOut i, float minAlpha, bool translucent) {
    float4 c = sampleTex(i.uv, i.tex) * i.tint;
    clip(c.a - minAlpha);
    if (gMisc.x > 0.5) {  // shader pipeline: the World stage layer it relights (encoding of entity.hlsl)
        bool bright = (i.lightBits & 256) != 0;
        if (translucent) return bright ? float4(c.rgb, min(c.a, 0.49)) : float4(c.rgb * i.light, c.a * 0.49);
        uint sky = (i.lightBits >> 4) & 15, block = (i.lightBits & 15) >> 1;
        uint code = bright ? 126 : (sky << 3) | (sky == 15 ? min(block, 5u) : block);
        return float4(c.rgb * i.shade, float(128 + code) / 255.0);
    }
    c.rgb *= i.light;
    c.rgb = lerp(c.rgb, gFogColor.rgb, i.fog);
    return c;
}

float4 BlockEntityPS(VSOut i) : SV_Target { return shade(i, 0.1, false); }
float4 BlockEntityTranslucentPS(VSOut i) : SV_Target { return shade(i, 0.004, true); }

// Shader pipeline with a motion target: previous UV - current UV (deferred.hlsl MotionCS convention).
struct MvOut {
    float4 color : SV_Target0;
    float2 motion : SV_Target1;
};
MvOut BlockEntityMvPS(VSOut i) {
    MvOut o;
    o.color = shade(i, 0.1, false);
    float2 cur = i.pos.xy * gMotion.xy + gMotion.zw;
    float2 prev = float2(i.prevClip.x, -i.prevClip.y) / i.prevClip.w * 0.5 + 0.5;
    o.motion = i.prevClip.w > 0 ? prev - cur : 0;
    return o;
}

// Shadow casters (shader pipeline): the same quads into a cascade, alpha-tested.
struct ShadowOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint tex : TEX;
    float alpha : ALPHA;
};
ShadowOut BlockEntityShadowVS(uint vid : SV_VertexID) {
    uint qi = vid / 6;
    uint corner = kCorner[vid % 6];
    float3 origin;
    uint dynIndex;
    Quad q = fetch(qi, origin, dynIndex);
    float3 n = 0;
    ShadowOut o;
    o.pos = mul(gShadowMat[gCascade], float4(wave(q, q.pos[corner], gFogParams.w, n) + origin, 1));
    o.uv = q.uv[corner];
    o.tex = q.tex;
    o.alpha = unpackColor(q.color).a;
    return o;
}
void BlockEntityShadowPS(ShadowOut i) {
    clip(sampleTex(i.uv, i.tex).a * i.alpha - 0.5);
}
