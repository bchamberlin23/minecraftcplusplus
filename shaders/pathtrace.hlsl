// Path tracer ("Shaders: Path Traced", src/render/pathtrace.cpp):
//   BlasGenCS     triangles for the bottom-level acceleration structures: the quads of the sections being built,
//                 expanded to positions (section-local, R32G32B32_FLOAT) in vertex pulling order.
//   PathTraceCS   G-buffer surfaces within the traced radius: sun / moon with soft shadows, sky light and diffuse
//                 bounces by path tracing (the game's light levels stand in for what lies past the last bounce).
//                 Writes the noisy lighting (radiance / albedo) for the denoiser.
//   PtAccumCS     built-in denoiser (no DLSS Ray Reconstruction): temporal accumulation of that lighting with
//                 reprojection and geometry tests, then albedo, emission and fog -> scene, blended into the
//                 rasterized lighting over the last blocks of the radius.
//   PtComposeCS   with DLSS Ray Reconstruction: the noisy lighting straight into the scene + RR's guide buffers.
#include "common.hlsli"
#include "shading.hlsli"
#include "raytrace.hlsli"

cbuffer PathTrace : register(b4) {
    float4 gPtParams;  // x = traced radius (blocks), y = blend band (blocks), z = history valid (0 / 1), w = max history
    uint4 gPtOpts;     // x = debug view (MCPP_PT_DEBUG), y = bounces, z = samples per pixel, w = frame
    uint4 gPtJobs;     // x = build jobs, y = first job (element), z = quads to expand, w = vertex buffer offset (bytes)
    uint4 gPtSlots;    // x = previous history slot, y = this frame's history slot
    float4 gPtMove;    // camera position - previous camera position (blocks)
};

struct BuildJob {
    uint slot, firstQuad, vertexBase, ptIndex;
    uint4 counts;  // quads per kind: opaque, cutout, translucent, model
};
StructuredBuffer<BuildJob> gJobs : register(t0, space4);
RWByteAddressBuffer gVertices : register(u0);  // root UAV (the renderer's counts slot)

// Path tracer targets (render resolution): PathTracer::Tex / Rw.
Texture2D<float4> gPtTex[8] : register(t8, space4);
RWTexture2D<float4> gPtRw[8] : register(u8, space4);
static const uint kPtLight = 0, kPtHist0 = 1, kPtGeom0 = 3, kPtNormalRough = 5, kPtDiffAlbedo = 6, kPtSpecAlbedo = 7;

// ---- BLAS triangles ---------------------------------------------------------------------------------------------

[numthreads(64, 1, 1)]
void BlasGenCS(uint id : SV_DispatchThreadID) {
    if (id >= gPtJobs.z) return;
    uint lo = 0, hi = gPtJobs.x - 1;
    while (lo < hi) {
        uint mid = (lo + hi + 1) >> 1;
        if (gJobs[gPtJobs.y + mid].firstQuad <= id) lo = mid;
        else hi = mid - 1;
    }
    BuildJob j = gJobs[gPtJobs.y + lo];
    uint k = id - j.firstQuad;
    float3 v[4];
    uint cubes = j.counts.x + j.counts.y + j.counts.z;
    if (k < cubes) {
        uint kind = k < j.counts.x ? 0u : k < j.counts.x + j.counts.y ? 1u : 2u;
        uint local = kind == 0 ? k : kind == 1 ? k - j.counts.x : k - j.counts.x - j.counts.y;
        uint4 q = gRtCube[rtCubeIndex(j.slot, kind, local)];
        [unroll] for (uint c = 0; c < 4; ++c) v[c] = rtCubeCorner(q, c);
    } else {
        uint base = rtModelBase(j.slot, k - cubes);
        [unroll] for (uint c = 0; c < 4; ++c) v[c] = rtModelCorner(base, c);
    }
    uint addr = gPtJobs.w + (j.vertexBase + k * 4) * 12;
    [unroll] for (uint c = 0; c < 4; ++c) gVertices.Store3(addr + c * 12, asuint(v[c]));
}

// ---- random numbers ---------------------------------------------------------------------------------------------

uint pcgHash(uint v) {
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
struct Rng {
    uint s;
    float next() {
        s = s * 747796405u + 2891336453u;
        uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
        return float((w >> 22u) ^ w) * (1.0 / 4294967296.0);
    }
};
Rng makeRng(uint2 px, uint frame, uint salt) {
    Rng r;
    r.s = pcgHash(px.x + pcgHash(px.y + pcgHash(frame * 9781u + salt)));
    return r;
}

float3 cosineDir(float3 n, float2 u) {
    float a = 2 * kPi * u.x, r = sqrt(u.y);
    float3 t = abs(n.y) < 0.99 ? normalize(cross(float3(0, 1, 0), n)) : float3(1, 0, 0);
    float3 b = cross(n, t);
    return normalize(t * (r * cos(a)) + b * (r * sin(a)) + n * sqrt(max(0, 1 - u.y)));
}
// A direction within `cosMax` of d (uniform over the cone): soft shadows of the sun / moon disc.
float3 coneDir(float3 d, float cosMax, float2 u) {
    float c = 1 - u.x * (1 - cosMax), s = sqrt(max(0, 1 - c * c)), a = 2 * kPi * u.y;
    float3 t = abs(d.y) < 0.99 ? normalize(cross(float3(0, 1, 0), d)) : float3(1, 0, 0);
    float3 b = cross(d, t);
    return normalize(t * (s * cos(a)) + b * (s * sin(a)) + d * c);
}

// ---- ray queries with translucent surfaces ----------------------------------------------------------------------

// Light through a translucent surface (water, glass, ice) crossed at distance t (and t blocks of water before it when
// the ray starts below the surface).
float3 rtTransmit(uint slot, uint prim, float2 bary, float3 pos, float t) {
    uint baseLayer;
    float3 uvl = rtUvLayer(slot, kKindTranslucent, prim, bary, pos, baseLayer);
    uint m = matClass(gMaterial[baseLayer]);
    if (m == kMatWater) return exp(-gWater.rgb * min(t, 32.0)) * 0.95;
    float4 tex = gTextures.SampleLevel(gSampler, uvl, 0);
    if (m == kMatIce) return 0.85;
    return lerp(1.0, srgbToLinear(tex.rgb) * 0.95, tex.a);  // glass, stained glass, slime, honey...
}

// Visibility (transmittance) along a ray: 0 behind anything opaque.
float3 rtVisibility(float3 origin, float3 dir, float tmax) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0;
    ray.TMax = tmax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, ray);
    float3 trans = 1;
    while (q.Proceed()) {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint kind = rtKind(q.CandidateInstanceContributionToHitGroupIndex(), q.CandidateGeometryIndex());
        float t = q.CandidateTriangleRayT();
        if (kind == kKindTranslucent)
            trans *= rtTransmit(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(),
                                origin + dir * t, t);
        else if (rtAlphaTest(q.CandidateInstanceID(), kind, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(),
                             origin + dir * t))
            q.CommitNonOpaqueTriangleHit();
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0 : trans;
}

// Closest opaque / alpha-tested hit; translucent surfaces in front of it attenuate (`trans`).
bool rtClosestThrough(float3 origin, float3 dir, float tmax, out RtSurface s, out float3 trans) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0;
    ray.TMax = tmax;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, ray);
    float tt[2] = {1e30, 1e30};
    float3 tc[2] = {float3(1, 1, 1), float3(1, 1, 1)};
    uint n = 0;
    while (q.Proceed()) {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint kind = rtKind(q.CandidateInstanceContributionToHitGroupIndex(), q.CandidateGeometryIndex());
        float t = q.CandidateTriangleRayT();
        if (kind == kKindTranslucent) {
            if (n < 2) {
                tt[n] = t;
                tc[n] = rtTransmit(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(),
                                   origin + dir * t, t);
                ++n;
            }
        } else if (rtAlphaTest(q.CandidateInstanceID(), kind, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(),
                               origin + dir * t)) {
            q.CommitNonOpaqueTriangleHit();
        }
    }
    bool hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    float tHit = hit ? q.CommittedRayT() : 1e30;
    trans = 1;
    [unroll] for (uint i = 0; i < 2; ++i)
        if (tt[i] < tHit) trans *= tc[i];
    if (!hit) {
        s = (RtSurface)0;
        return false;
    }
    s = rtSurface(q.CommittedInstanceID(), rtKind(q.CommittedInstanceContributionToHitGroupIndex(), q.CommittedGeometryIndex()),
                  q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(), origin, dir, q.CommittedRayT());
    return true;
}

// ---- light ------------------------------------------------------------------------------------------------------

static const float kSunCos = 0.99993;  // ~0.7 degrees: the sun / moon disc as seen through the shadows

float rayEps(float dist) { return 0.003 + dist * 0.0004; }

// Sun / moon light arriving at p (normal n), times the cosine (radiance per albedo, like shadeSurface's diffuse).
float3 sunLight(float3 p, float3 n, float dist, bool foliage, inout Rng rng) {
    if (dot(gLightColor.rgb, 1) <= 0) return 0;
    float3 l = coneDir(gLightDir.xyz, kSunCos, float2(rng.next(), rng.next()));
    float ndl = dot(n, l);
    float cosTerm = foliage ? abs(ndl) * 0.7 + 0.15 : ndl;
    if (cosTerm <= 0) return 0;
    float3 origin = p + n * (ndl >= 0 || !foliage ? rayEps(dist) : -rayEps(dist));
    return gLightColor.rgb * cosTerm * rtVisibility(origin, l, 1e4);
}

// What the game's light levels say about light at a surface (beyond the last traced bounce).
float3 cachedLight(float sky, float block, float3 n) {
    float hemi = 0.65 + 0.35 * n.y;
    return gAmbient.rgb * skyCurve(sky) * hemi + gCave.rgb * hemi + gBlockColor.rgb * blockCurve(block);
}

float3 emission(float3 albedo, uint mat) {
    if (mat == kMatGlow) return albedo * gShadeMisc.z;
    if (mat == kMatEmissive) return albedo * gShadeMisc.z * smoothstep(0.45, 0.85, max(max(albedo.r, albedo.g), albedo.b));
    return 0;
}

// Radiance leaving a surface hit by a bounce ray (towards the ray), with `bounces` more bounces after it.
float3 bounceRadiance(RtSurface s, float dist, uint bounces, bool emissive, inout Rng rng) {
    float4 tex = gTextures.SampleLevel(gSampler, float3(s.uv, s.layer), 0);
    float3 albedo = srgbToLinear(tex.rgb * s.tint);
    uint mat = matClass(s.mat);
    bool foliage = mat == kMatLeaves || mat == kMatPlant;
    float3 light = sunLight(s.pos, s.n, dist, foliage, rng);
    if (bounces > 0) {
        float3 d = cosineDir(s.n, float2(rng.next(), rng.next()));
        RtSurface h;
        float3 trans;
        if (rtClosestThrough(s.pos + s.n * rayEps(dist), d, gPtParams.x * 1.5, h, trans)) {
            float4 t2 = gTextures.SampleLevel(gSampler, float3(h.uv, h.layer), 0);
            float3 a2 = srgbToLinear(t2.rgb * h.tint);
            light += trans * (a2 * (sunLight(h.pos, h.n, dist, false, rng) + cachedLight(h.sky, h.block, h.n)) +
                              emission(a2, matClass(h.mat)));
        } else {
            light += trans * skyColor(d);
        }
    } else {
        light += cachedLight(s.sky, s.block, s.n);
    }
    return albedo * light + (emissive ? emission(albedo, mat) : 0);
}

// The sky / fog colour behind a pixel (DeferredCS).
float3 skyBehind(uint2 px, float3 dir) {
    float4 skyLdr = skyStage(int2(px));
    float3 skyHdr = skyToHdr(skyLdr.rgb);
    float mu = dot(dir, gSunDir.xyz);
    float lum = luminance(skyLdr.rgb);
    float disc = mu > 0.94 ? pow(saturate(lum), 6.0) * smoothstep(0.94, 0.97, mu) : 0;
    float3 glow = gLightColor.rgb * gLightColor.w * (pow(saturate(mu), 24.0) * 0.025 + pow(saturate(mu), 300.0) * 0.4) *
                  (1.0 - gTime.y * 0.9);
    return skyHdr * (1.0 + gSunDir.w * disc) + glow * (dir.y > -0.05 ? 1 : 0);
}

struct PtPixel {
    bool traced;      // a G-buffer surface within the radius
    float weight;     // blend into the rasterized lighting (1 inside, 0 at the radius)
    float3 rel, n, albedo;
    uint mat;
    float sky, block, dist;
};
PtPixel ptPixel(uint2 px) {
    PtPixel p = (PtPixel)0;
    float d = gTex[kTexDepth].Load(int3(px, 0)).r;
    if (d <= 0) return p;
    if (gTex[kTexLdrFwd].Load(int3(px, 0)).a >= 0.499) return p;  // entities (forward layer): rasterized lighting
    float2 pixel = float2(px) + 0.5;
    p.rel = relPosition(pixel, d);
    p.dist = length(p.rel);
    p.weight = saturate((gPtParams.x - p.dist) / gPtParams.y);
    if (p.weight <= 0) return p;
    float4 a = gTex[kTexAlbedo].Load(int3(px, 0));
    float4 nl = gTex[kTexNormal].Load(int3(px, 0));
    p.traced = true;
    p.albedo = srgbToLinear(a.rgb);
    p.mat = uint(round(a.a * 255.0)) & 7;
    p.n = octDecode(nl.xy);
    p.sky = nl.z * 15.0;
    p.block = nl.w * 15.0;
    return p;
}

// ---- path tracing -----------------------------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void PathTraceCS(uint2 px : SV_DispatchThreadID) {
    if (any(px >= uint2(gScreen.xy))) return;
    float2 pixel = float2(px) + 0.5;
    uint dbg = gPtOpts.x;
    if (dbg == 1 || dbg == 2) {  // camera rays straight into the acceleration structure: normals / albedo
        float3 dir = normalize(viewRay(pixelNdc(pixel)));
        RtSurface s;
        float3 c = float3(0.03, 0.03, 0.08);
        if (rtClosest(0, dir, gPtParams.x * 2.0, s)) {
            if (dbg == 1) c = s.n * 0.5 + 0.5;
            else c = srgbToLinear(gTextures.SampleLevel(gSampler, float3(s.uv, s.layer), 0).rgb * s.tint);
        }
        gRW[kRwScene][px] = float4(c, 1);
        gRW[kRwSceneCopy][px] = float4(c, 1);
        gPtRw[kPtLight][px] = 0;
        return;
    }
    PtPixel p = ptPixel(px);
    if (!p.traced) {
        gPtRw[kPtLight][px] = 0;
        return;
    }
    bool foliage = p.mat == kMatLeaves || p.mat == kMatPlant;
    float3 light = 0;
    uint spp = max(gPtOpts.z, 1u);
    for (uint i = 0; i < spp; ++i) {
        Rng rng = makeRng(px, gPtOpts.w, i);
        float3 l = sunLight(p.rel, p.n, p.dist, foliage, rng);
        // Sky and bounce light: one cosine-weighted path.
        float3 d = cosineDir(p.n, float2(rng.next(), rng.next()));
        RtSurface s;
        float3 trans;
        float3 origin = p.rel + p.n * rayEps(p.dist);
        if (rtClosestThrough(origin, d, gPtParams.x * 1.5, s, trans))
            l += trans * bounceRadiance(s, p.dist, gPtOpts.y > 1 ? 1 : 0, true, rng);
        else
            l += trans * skyColor(d);
        // Block light (torches, lava...) from the game's light levels until emissive surfaces are sampled.
        l += gBlockColor.rgb * blockCurve(p.block);
        light += l;
    }
    light /= float(spp);
    // Fireflies: a bounce off a sunlit or glowing surface seen through one pixel.
    float lum = luminance(light);
    if (lum > 24.0) light *= 24.0 / lum;
    gPtRw[kPtLight][px] = float4(light, 1);
}

// ---- built-in denoiser: temporal accumulation ---------------------------------------------------------------------

float4 packGeom(float dist, float3 n) {
    float2 o = octEncode(n);
    return float4(dist, o, 1);
}

[numthreads(8, 8, 1)]
void PtAccumCS(uint2 px : SV_DispatchThreadID) {
    if (any(px >= uint2(gScreen.xy))) return;
    uint prev = gPtSlots.x, cur = gPtSlots.y;
    PtPixel p = ptPixel(px);
    if (!p.traced) {
        gPtRw[kPtHist0 + cur][px] = 0;
        gPtRw[kPtGeom0 + cur][px] = 0;
        return;
    }
    float3 noisy = gPtRw[kPtLight][px].rgb;
    // Reprojection (the world is static: camera motion only) with bilinear taps that pass the geometry test.
    float4 hist = 0;
    if (gPtParams.z > 0) {
        float4 pc = mul(gPrevViewProj, float4(p.rel, 1));
        float prevDist = length(p.rel + gPtMove.xyz);
        if (pc.w > 0) {
            float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
            float2 pp = puv * gScreen.xy - 0.5;
            int2 base = int2(floor(pp));
            float2 f = pp - base;
            float wsum = 0;
            float4 sum = 0;
            [unroll] for (int k = 0; k < 4; ++k) {
                int2 o = int2(k & 1, k >> 1);
                int2 q = base + o;
                if (any(q < 0) || any(q >= int2(gScreen.xy))) continue;
                float4 g = gPtRw[kPtGeom0 + prev][q];
                if (g.w <= 0) continue;
                float3 gn = octDecode(g.yz);
                // Same surface: its distance from the previous camera and its normal.
                if (abs(g.x - prevDist) > 0.03 * prevDist + 0.2 || dot(gn, p.n) < 0.9) continue;
                float w = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y);
                sum += gPtRw[kPtHist0 + prev][q] * w;
                wsum += w;
            }
            if (wsum > 0.05) hist = sum / wsum;
        }
    }
    float n = min(hist.a, gPtParams.w);
    float3 acc = lerp(hist.rgb, noisy, 1.0 / (n + 1.0));
    gPtRw[kPtHist0 + cur][px] = float4(acc, n + 1.0);
    gPtRw[kPtGeom0 + cur][px] = packGeom(p.dist, p.n);

    float3 c = p.albedo * acc + emission(p.albedo, p.mat);
    float3 dir = p.rel / max(p.dist, 1e-4);
    if (gWater.w < 0.5) c = applyFog(c, p.rel, skyBehind(px, dir));
    if (gPtOpts.x == 3) c = acc;  // lighting only
    float4 raster = gRW[kRwScene][px];
    c = lerp(raster.rgb, c, p.weight);
    gRW[kRwScene][px] = float4(c, 1);
    gRW[kRwSceneCopy][px] = float4(c, 1);
}
