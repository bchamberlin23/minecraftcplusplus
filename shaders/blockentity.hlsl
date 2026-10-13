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
    float4 gMisc;            // x = 1: shader pipeline (pipeline-aware output, see entity.hlsl); y = last frame's wave time;
                             // z = the game's GameTime uniform (day time fraction, end portal layers)
    float4x4 gShadowMat[4];  // shadow cascades (camera-relative -> light clip)
    float4 gEndSky;          // atlas rects (u0, v0, du, dv) of environment/end_sky and entity/end_portal/end_portal
    float4 gEndPortal;
    float4 gFogRender;       // the render distance fog (cylindrical): x = start, y = end
};
#include "fog.hlsli"
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
    uint light;   // sky << 4 | block; 256 full bright, 512 no diffuse shading, 1024 only within 16 blocks,
                  // 2048 casts no shadow
    uint tex;     // 0xFFFFFFFF entity atlas, 0xFFFFFFFE font, 0xFFFFFFFD solid, 0xFFFFFFFC entity atlas repeating
                  // (rect in pivot + normal), 0xFFFFFFFB / 0xFFFFFFFA end portal / gateway, else block texture layer
    float3 pivot; // banner flag wave pivot; repeating atlas texture: u0, v0, du (dv: asfloat(normal))
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
    nointerpolation float4 rect : RECT;         // repeating atlas texture: its rect
    float4 clip : CLIP;                         // end portal: the clip position (texProj0)
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
    o.rect = float4(q.pivot, asfloat(q.normal));
    o.clip = o.pos;
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
    o.fog = vanillaFog(rel, gFogParams.xy, gFogRender.xy);
    return o;
}

// An entity atlas texture repeating like a texture of its own (beacon beams, the end portal layers).
float4 sampleRepeat(float2 uv, float4 rect) {
    return gAtlas.SampleLevel(gPoint, rect.xy + float2(frac(uv.x), frac(uv.y)) * rect.zw, 0);
}
float4 sampleTex(float2 uv, uint tex, float4 rect) {
    if (tex == 0xFFFFFFFF) return gAtlas.SampleLevel(gPoint, uv, 0);
    if (tex == 0xFFFFFFFE) return gFont.SampleLevel(gPoint, uv, 0);
    if (tex == 0xFFFFFFFD) return 1;
    if (tex == 0xFFFFFFFC) return sampleRepeat(float2(min(uv.x, 0.9999), uv.y), rect);
    return gBlocks.Sample(gBlockSampler, float3(uv, float(tex & 0xFFFF)));
}

// rendertype_end_portal.fsh: the end sky plus 15 (end portal) or 16 (end gateway) layers of end_portal.png in
// screen space, each scaled, rotated and scrolling with GameTime.
static const float3 kEndColors[16] = {
    float3(0.022087, 0.098399, 0.110818), float3(0.011892, 0.095924, 0.089485), float3(0.027636, 0.101689, 0.100326),
    float3(0.046564, 0.109883, 0.114838), float3(0.064901, 0.117696, 0.097189), float3(0.063761, 0.086895, 0.123646),
    float3(0.084817, 0.111994, 0.166380), float3(0.097489, 0.154120, 0.091064), float3(0.106152, 0.131144, 0.195191),
    float3(0.097721, 0.110188, 0.187229), float3(0.133516, 0.138278, 0.148582), float3(0.070006, 0.243332, 0.235792),
    float3(0.196766, 0.142899, 0.214696), float3(0.047281, 0.315338, 0.321970), float3(0.204675, 0.390010, 0.302066),
    float3(0.080955, 0.314821, 0.661491)};
float3 endPortal(float4 clip, uint layers) {
    float2 s = clip.xy / clip.w * 0.5 + 0.5;  // projection_from_position, divided
    float3 color = sampleRepeat(s, gEndSky).rgb * kEndColors[0];
    for (uint i = 0; i < layers; ++i) {
        float layer = float(i + 1);
        // texProj0 * (scale * rotate) * translate * SCALE_TRANSLATE (GLSL row vector times matrices)
        float a = radians((layer * layer * 4321.0 + layer * 9.0) * 2.0), c = cos(a), sn = sin(a);
        float k = (4.5 - layer / 4.0) * 2.0;
        float2 p = k * float2(s.x * c - s.y * sn, s.x * sn + s.y * c);
        p += float2(17.0 / layer, (2.0 + layer / 1.5) * (gMisc.z * 1.5));
        color += sampleRepeat(p * 0.5 + 0.25, gEndPortal).rgb * kEndColors[i];
    }
    return color;
}

float4 shade(VSOut i, float minAlpha, bool translucent) {
    if (i.tex == 0xFFFFFFFB || i.tex == 0xFFFFFFFA) {  // end portal / gateway: unlit, opaque
        float3 e = endPortal(i.clip, i.tex == 0xFFFFFFFB ? 15 : 16);
        if (gMisc.x > 0.5) return float4(e, 1);  // code 127: a final (vanilla) colour, scaled like vanilla-lit ones
        return float4(lerp(e, gFogColor.rgb, i.fog), 1);
    }
    float4 c = sampleTex(i.uv, i.tex, i.rect) * i.tint;
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
    nointerpolation float4 rect : RECT;
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
    if (q.light & 2048) o.pos = float4(0, 0, 0, 1);  // casts no shadow: degenerate
    o.uv = q.uv[corner];
    o.tex = q.tex;
    o.alpha = unpackColor(q.color).a;
    o.rect = float4(q.pivot, asfloat(q.normal));
    return o;
}
void BlockEntityShadowPS(ShadowOut i) {
    clip(sampleTex(i.uv, i.tex, i.rect).a * i.alpha - 0.5);
}
