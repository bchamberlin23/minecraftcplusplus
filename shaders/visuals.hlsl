// Held items, item frames, paintings, name tags, the first-person hand (src/entity/visuals.cpp) and particles.
// Meshes: quads in a static store (item models) or a per-frame buffer (text, arms), placed by instances; a quad's
// instance is found by binary search over the draw's instance list (prefix sums). Particles are camera-facing
// quads expanded here. Self-contained like entity.hlsl: same light curve, two-light shading and fog.

cbuffer Frame : register(b0) {
    float4x4 gViewProj;  // camera-relative world -> clip
    float4 gFogColor;
    float4 gFogParams;   // x = fog start, y = fog end, z = daylight, w = 1: World stage with the shader pipeline
    float4 gLight0;      // world-space diffuse light directions (the game's Lighting.setupLevel)
    float4 gLight1;
    float4 gCamRight;    // world-space camera right (particles)
    float4 gCamUp;       // world-space camera up
    float4 gGlintRect;   // item atlas rect of misc/enchanted_glint_item (u0 v0 du dv)
    float4 gGlint;       // x, y = scroll offset, z = strength, w = 1: no fog (first-person hand)
    float4 gLightmap[3]; // the game's lightmap inputs (lightmap.hlsli)
    float4x4 gPrevViewProj;  // shader pipeline: camera-relative position (this frame) -> previous clip, unjittered
    float4 gMotion;          // curUv = SV_Position.xy * gMotion.xy + gMotion.zw (PassContext::motionScale)
    float4x4 gShadowMat[4];  // shadow cascades (camera-relative -> light clip)
};
#include "lightmap.hlsli"
cbuffer Draw : register(b1) {
    uint gBase;   // first instance (meshes) / particle
    uint gCount;  // instances in the list
    uint gMode;
    uint gCascade;  // shadow caster draws
};

struct Quad {
    float3 pos[4];
    float2 uv[4];
    uint normal;  // snorm8 xyz
    uint color;   // 0xAABBGGRR
    uint tex;     // 0xFFFFFFFF item atlas, 0xFFFFFFFE font, 0xFFFFFFFD solid, 0xFFFFFFFC entity atlas, else block layer
    uint flags;   // 1: no diffuse shading
};
struct Instance {
    float4 r0, r1, r2;  // mesh -> camera-relative world
    uint color;         // tint multiplied with the quad colour
    uint light;         // sky << 4 | block; 256: full bright; 512: no diffuse shading
    uint overlay;       // hurt red in bits 0..7
    uint flags;         // 1: quads from the per-frame buffer
    uint first;         // first quad
    uint count;
    uint prefix;        // quads of the earlier instances in this list
    uint pad;
    float4 p0, p1, p2;  // last frame's transform (motion vectors)
};
struct Particle {
    float3 pos;    // camera-relative
    float size;    // half extent
    float4 uv;     // u0 v0 u1 v1
    uint color;    // 0xAABBGGRR
    uint light;
    uint tex;      // like Quad.tex
    float roll;
    float3 motion; // moved since the last frame (blocks)
    uint orient;   // bit 31: turned by rotationY(a).rotateX(b).rotateY(c) (10 bits each), not facing the camera
};

StructuredBuffer<Quad> gStatic : register(t0);
StructuredBuffer<Quad> gDynamic : register(t1);
StructuredBuffer<Instance> gInstances : register(t2);
StructuredBuffer<Particle> gParticles : register(t3);
Texture2D<float4> gItems : register(t4);
Texture2D<float4> gFont : register(t5);
Texture2D<float4> gEntity : register(t6);
Texture2DArray<float4> gBlocks : register(t7);
SamplerState gPoint : register(s0);
SamplerState gBlockSampler : register(s1);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float2 glintUv : TEXCOORD1;
    nointerpolation uint tex : TEX;
    float4 tint : COLOR0;
    float3 light : COLOR1;
    float4 overlay : COLOR2;
    float fog : FOG;
    float shade : SHADE;
    nointerpolation uint lightBits : LIGHTBITS;
    float4 prevClip : PREVCLIP;  // previous frame's clip position (motion vectors)
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

VSOut finish(float3 rel, float3 n, float2 uv, uint tex, float4 tint, uint light, uint overlay, bool shadeOn) {
    VSOut o;
    o.pos = mul(gViewProj, float4(rel, 1));
    o.uv = uv;
    o.glintUv = 0;
    o.tex = tex;
    o.tint = tint;
    float3 l = 1.0;
    o.shade = 1.0;
    if (!(light & 256)) l = lightColor(light);
    if (shadeOn && !(light & 512)) {
        n = normalize(n + 1e-6);
        float d = saturate(dot(gLight0.xyz, n)) + saturate(dot(gLight1.xyz, n));
        o.shade = min(1.0, d * 0.6 + 0.4);
        l *= o.shade;
    }
    o.light = l;
    o.lightBits = light;
    o.overlay = float4(1, 0, 0, float(overlay & 255) / 255.0);
    o.fog = gGlint.w > 0.5 ? 0.0 : saturate((length(rel) - gFogParams.x) / max(gFogParams.y - gFogParams.x, 1e-3));
    o.prevClip = 0;
    return o;
}

VSOut VisualsVS(uint vid : SV_VertexID) {
    uint qi = vid / 6;
    uint corner = kCorner[vid % 6];
    uint lo = 0, hi = gCount - 1;
    while (lo < hi) {
        uint mid = (lo + hi + 1) >> 1;
        if (gInstances[gBase + mid].prefix <= qi) lo = mid;
        else hi = mid - 1;
    }
    Instance inst = gInstances[gBase + lo];
    uint index = inst.first + qi - inst.prefix;
    Quad q;
    if (inst.flags & 1) q = gDynamic[index];
    else q = gStatic[index];
    float4 p = float4(q.pos[corner], 1);
    float3 rel = float3(dot(inst.r0, p), dot(inst.r1, p), dot(inst.r2, p));
    float3 nl = unpackNormal(q.normal);
    float3 n = float3(dot(inst.r0.xyz, nl), dot(inst.r1.xyz, nl), dot(inst.r2.xyz, nl));
    float4 tint = unpackColor(q.color) * unpackColor(inst.color);
    VSOut o = finish(rel, n, q.uv[corner], q.tex, tint, inst.light, inst.overlay, (q.flags & 1) == 0);
    // ItemRenderer glint: texture scrolled over the item in its own space (rotated 10 degrees).
    float2 g = q.pos[corner].xy * 0.5 + q.pos[corner].z * 0.25;
    const float c = 0.98480775, s = 0.17364818;
    o.glintUv = float2(g.x * c - g.y * s, g.x * s + g.y * c) + gGlint.xy;
    o.prevClip = mul(gPrevViewProj, float4(dot(inst.p0, p), dot(inst.p1, p), dot(inst.p2, p), 1));
    return o;
}

VSOut ParticleVS(uint vid : SV_VertexID) {
    Particle pt = gParticles[gBase + vid / 6];
    uint corner = kCorner[vid % 6];
    // QuadParticleRenderState: corners (1,-1) (1,1) (-1,1) (-1,-1) rotated by the camera and the roll.
    static const float2 kXY[4] = {float2(1, -1), float2(1, 1), float2(-1, 1), float2(-1, -1)};
    float2 xy = kXY[corner];
    float cr = cos(pt.roll), sr = sin(pt.roll);
    float2 r = float2(xy.x * cr - xy.y * sr, xy.x * sr + xy.y * cr) * pt.size;
    float3 rel = pt.pos + gCamRight.xyz * r.x + gCamUp.xyz * r.y;
    if (pt.orient & 0x80000000u) {  // QuadParticleRenderState with a rotation of its own (VibrationSignalParticle)
        float3 ang = float3(pt.orient & 1023u, (pt.orient >> 10) & 1023u, (pt.orient >> 20) & 1023u) * (6.28318531 / 1024.0);
        float3 v = float3(xy * pt.size, 0);
        v = float3(v.x * cos(ang.z), v.y, -v.x * sin(ang.z));                         // rotateY(c)
        v = float3(v.x, v.y * cos(ang.y) - v.z * sin(ang.y), v.y * sin(ang.y) + v.z * cos(ang.y));  // rotateX(b)
        v = float3(v.x * cos(ang.x) + v.z * sin(ang.x), v.y, -v.x * sin(ang.x) + v.z * cos(ang.x));  // rotationY(a)
        rel = pt.pos + v;
    }
    float2 uv = float2(xy.x > 0 ? pt.uv.z : pt.uv.x, xy.y > 0 ? pt.uv.y : pt.uv.w);
    VSOut o = finish(rel, float3(0, 1, 0), uv, pt.tex, unpackColor(pt.color), pt.light | 512, 0, false);
    o.prevClip = mul(gPrevViewProj, float4(rel - pt.motion, 1));
    return o;
}

float4 sampleTex(VSOut i) {
    if (i.tex == 0xFFFFFFFF) return gItems.SampleLevel(gPoint, i.uv, 0);
    if (i.tex == 0xFFFFFFFE) return gFont.SampleLevel(gPoint, i.uv, 0);
    if (i.tex == 0xFFFFFFFD) return 1;
    if (i.tex == 0xFFFFFFFC) return gEntity.SampleLevel(gPoint, i.uv, 0);
    return gBlocks.Sample(gBlockSampler, float3(i.uv, float(i.tex & 0xFFFF)));
}

float4 shade(VSOut i, float minAlpha, bool translucent) {
    float4 c = sampleTex(i) * i.tint;
    clip(c.a - minAlpha);
    c.rgb = lerp(c.rgb, i.overlay.rgb, i.overlay.a);
    if (gFogParams.w > 0.5) {  // World stage, shader pipeline on: same encoding as entity.hlsl
        bool bright = (i.lightBits & 256) != 0;
        if (translucent) return bright ? float4(c.rgb, min(c.a, 0.49)) : float4(c.rgb * i.light, c.a * 0.49);
        uint sky = (i.lightBits >> 4) & 15, block = (i.lightBits & 15) >> 1;
        uint code = bright ? 126 : (sky << 3) | (sky == 15 ? min(block, 5u) : block);
        return float4(c.rgb * i.shade, float(128 + code) / 255.0);
    }
    c.rgb *= i.light;
    c.rgb = lerp(c.rgb, gFogColor.rgb, i.fog);
    if (gGlint.w > 0.5 && !translucent) c.a = 1;  // HUD stage: the pipeline's UI layer is premultiplied
    return c;
}

float4 VisualsCutoutPS(VSOut i) : SV_Target { return shade(i, 0.1, false); }

// The entity outline target (OutlineBufferSource): the silhouette in the outline colour (gMode: 0xRRGGBB).
float4 VisualsOutlinePS(VSOut i) : SV_Target {
    clip(sampleTex(i).a * i.tint.a - 0.1);
    return float4(float((gMode >> 16) & 255u), float((gMode >> 8) & 255u), float(gMode & 255u), 255.0) / 255.0;
}

// Shader pipeline with a motion target: previous UV - current UV (deferred.hlsl MotionCS convention).
struct MvOut {
    float4 color : SV_Target0;
    float2 motion : SV_Target1;
};
float2 motionVector(float4 svPos, float4 prevClip) {
    float2 cur = svPos.xy * gMotion.xy + gMotion.zw;
    float2 prev = float2(prevClip.x, -prevClip.y) / prevClip.w * 0.5 + 0.5;
    return prevClip.w > 0 ? prev - cur : 0;
}
MvOut VisualsCutoutMvPS(VSOut i) {
    MvOut o;
    o.color = shade(i, 0.1, false);
    o.motion = motionVector(i.pos, i.prevClip);
    return o;
}
float4 VisualsTranslucentPS(VSOut i) : SV_Target { return shade(i, 0.004, true); }

// Text (name tags): the font atlas or solid backgrounds, alpha blended, light without diffuse shading.
float4 VisualsTextPS(VSOut i) : SV_Target {
    float4 c = sampleTex(i) * i.tint;
    clip(c.a - 0.004);
    c.rgb *= i.light;
    return c;
}

// Enchantment glint (RenderTypes glint: additive src * src), drawn over the item with the same geometry.
float4 VisualsGlintPS(VSOut i) : SV_Target {
    float4 base = sampleTex(i);
    clip(base.a - 0.1);
    float2 uv = gGlintRect.xy + frac(i.glintUv) * gGlintRect.zw;
    float3 g = gItems.SampleLevel(gPoint, uv, 0).rgb * gGlint.z;
    return float4(g * (1.0 - i.fog), 1);
}

// Block cracks (RenderPipelines.CRUMBLING): the destroy stage texture multiplied in twice (blend dst * src +
// src * dst), colour only; fog fades it to the neutral 0.5. Shader pipeline: the terrain is lit later from the
// G-buffer, so the OIT stage texture goes into the entity layer alpha blended, its alpha as the cover the deferred
// pass lerps the lit terrain by (black at 1/2 = the multiply's halving). The light texels (a slight brightening with
// the multiply) are left out: a lerp towards white would overexpose shaded faces.
float4 VisualsCrumblingPS(VSOut i) : SV_Target {
    float4 c = sampleTex(i) * i.tint;
    if (gFogParams.w > 0.5) {
        clip(c.a - 0.004);
        clip(0.5 - c.r);
        return float4(c.rgb, c.a * 0.49);
    }
    clip(c.a - 0.1);
    return float4(lerp(c.rgb, 0.5, i.fog), 1);
}

float4 ParticleCutoutPS(VSOut i) : SV_Target { return shade(i, 0.1, false); }
MvOut ParticleCutoutMvPS(VSOut i) {
    MvOut o;
    o.color = shade(i, 0.1, false);
    o.motion = motionVector(i.pos, i.prevClip);
    return o;
}
float4 ParticleTranslucentPS(VSOut i) : SV_Target {
    float4 c = sampleTex(i) * i.tint;
    clip(c.a - 0.004);
    c.rgb *= i.light;
    c.rgb = lerp(c.rgb, gFogColor.rgb, i.fog);
    return c;
}

// Shadow casters (shader pipeline): instance quads into a cascade, alpha-tested.
struct ShadowOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint tex : TEX;
    float alpha : ALPHA;
};
ShadowOut VisualsShadowVS(uint vid : SV_VertexID) {
    uint qi = vid / 6;
    uint corner = kCorner[vid % 6];
    uint lo = 0, hi = gCount - 1;
    while (lo < hi) {
        uint mid = (lo + hi + 1) >> 1;
        if (gInstances[gBase + mid].prefix <= qi) lo = mid;
        else hi = mid - 1;
    }
    Instance inst = gInstances[gBase + lo];
    uint index = inst.first + qi - inst.prefix;
    Quad q;
    if (inst.flags & 1) q = gDynamic[index];
    else q = gStatic[index];
    float4 p = float4(q.pos[corner], 1);
    ShadowOut o;
    o.pos = mul(gShadowMat[gCascade], float4(dot(inst.r0, p), dot(inst.r1, p), dot(inst.r2, p), 1));
    o.uv = q.uv[corner];
    o.tex = q.tex;
    o.alpha = unpackColor(q.color).a * unpackColor(inst.color).a;
    return o;
}
void VisualsShadowPS(ShadowOut i) {
    VSOut v = (VSOut)0;
    v.uv = i.uv;
    v.tex = i.tex;
    clip(sampleTex(v).a * i.alpha - 0.5);
}
