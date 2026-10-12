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
//   PtClearCS / LightGatherCS: emissive surfaces as lights. BlasGenCS records each section's emissive quads (colour
//                 from the texture's glowing texels, strength from the light level the game gives the block); every
//                 frame the lights of the sections near the camera are gathered into one list that PathTraceCS
//                 samples with ReSTIR DI (RIS over random candidates + temporal reuse of the reservoirs).
#include "common.hlsli"
#include "shading.hlsli"
#include "raytrace.hlsli"

struct BuildJob {
    uint slot, firstQuad, vertexBase, ptIndex;
    uint4 counts;  // quads per kind: opaque, cutout, translucent, model
};
StructuredBuffer<BuildJob> gJobs : register(t0, space4);
RWByteAddressBuffer gVertices : register(u0);  // root UAV (the renderer's counts slot)

// Emissive lights: per section (pt index) up to kSectionLights records; this frame's list of light ids.
static const uint kSectionLights = 256;
RWStructuredBuffer<uint2> gLights : register(u1, space4);  // kind << 30 | quad, Le (RGB9E5)
RWStructuredBuffer<uint> gLightSlot : register(u18, space4);  // per pt index: its mesh slot
RWStructuredBuffer<uint> gLightCount : register(u2, space4);
RWStructuredBuffer<uint> gFrameLights : register(u3, space4);  // frame section << 8 | index in the section
RWStructuredBuffer<uint> gFrameLightCount : register(u4, space4);
RWStructuredBuffer<uint> gLightPower : register(u5, space4);   // per pt index: sum of luminance(Le) * area, x 16
RWStructuredBuffer<uint4> gFrameSecs : register(u6, space4);   // per frame section: pt index, first light, lights, weight
RWStructuredBuffer<float> gFrameSecCdf : register(u7, space4);  // inclusive prefix sum of the weights
StructuredBuffer<uint2> gLightSections : register(t5, space4);  // (pt index, distance bits) of the sections near the camera
// Light ids in reservoirs are stable across frames: pt index << 8 | index in the section.
RWTexture2D<uint4> gPtRes[2] : register(u16, space4);  // ReSTIR reservoirs: light id, sample uv, W, M


// Shared-exponent HDR colour in 32 bits (DXGI_FORMAT_R9G9B9E5_SHAREDEXP layout).
uint packRgb9e5(float3 c) {
    c = clamp(c, 0, 65408.0);
    float m = max(c.r, max(c.g, c.b));
    int e = max(-16, int(floor(log2(max(m, 1e-9))))) + 1;
    float scale = exp2(float(9 - e));
    if (uint(floor(m * scale + 0.5)) == 512u) e += 1, scale *= 0.5;
    uint3 v = uint3(floor(c * scale + 0.5));
    return v.r | v.g << 9 | v.b << 18 | uint(e + 15) << 27;
}
float3 unpackRgb9e5(uint v) {
    return float3(v & 511, (v >> 9) & 511, (v >> 18) & 511) * exp2(float(int(v >> 27) - 15 - 9));
}

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

    // Emissive quads become lights.
    uint kind = k < j.counts.x ? 0u : k < j.counts.x + j.counts.y ? 1u : k < cubes ? 2u : 3u;
    uint local = kind == 0 ? k : kind == 1 ? k - j.counts.x : kind == 2 ? k - j.counts.x - j.counts.y : k - cubes;
    uint layer, packedLight;
    float level;
    if (kind != kKindModel) {
        uint4 q = gRtCube[rtCubeIndex(j.slot, kind, local)];
        layer = q.y & 0xFFF;
        // The cells in front of an emitting face are lit one level below its emission.
        level = max(max(q.w & 0xFF, (q.w >> 8) & 0xFF), max((q.w >> 16) & 0xFF, q.w >> 24)) / 16.0 + 1.0;
    } else {
        uint base = rtModelBase(j.slot, local);
        layer = gRtModel[base + 10] & 0xFFFF;
        level = float(gRtModel[base + 11] & 15);
    }
    uint m = matClass(gMaterial[layer]);
    if (m != kMatGlow && m != kMatEmissive) return;
    // Colour and share of the texture that glows (emissive textures glow in their bright texels, like the raster path).
    uint tw, th, te, mips;
    gTextures.GetDimensions(0, tw, th, te, mips);
    uint mip = firstbithigh(max(tw, 16u)) - 4;
    float3 sum = 0;
    float wsum = 0, opaque = 0;
    for (uint t = 0; t < 256; ++t) {
        float4 c = gTextures.Load(int4(t & 15, t >> 4, layer, mip));
        if (c.a < 0.5) continue;
        opaque += 1;
        float w = m == kMatGlow ? 1.0 : smoothstep(0.45, 0.85, max(c.r, max(c.g, c.b)));
        sum += srgbToLinear(c.rgb) * w;
        wsum += w;
    }
    if (wsum < 0.5) return;
    float3 color = sum / wsum;
    color /= max(max(color.r, color.g), max(color.b, 1e-3));
    color = pow(color, 1.6);  // flames: warmer than their texels' average
    float3 le = color * (wsum / opaque) * (kind == kKindModel ? 300.0 : 8.0) * pow(saturate(level / 15.0), 2.0);
    float area = length(cross(v[1] - v[0], v[2] - v[0]));
    InterlockedAdd(gLightPower[j.ptIndex], uint(min(luminance(le) * area * 16.0, 1e6)));
    uint i;
    InterlockedAdd(gLightCount[j.ptIndex], 1, i);
    if (i >= kSectionLights) return;
    gLights[j.ptIndex * kSectionLights + i] = uint2(kind << 30 | local, packRgb9e5(le));
}

// Entities: this frame's posed model quads and free quads (entity.hlsl layouts) -> camera-relative triangles.
ByteAddressBuffer gEntQuads : register(t6, space4);
ByteAddressBuffer gEntFrame : register(t7, space4);

[numthreads(64, 1, 1)]
void EntityGenCS(uint id : SV_DispatchThreadID) {
    uint refs = gPtEntity.w & 0xFFFFF, frees = gPtEntity2.x;
    if (id >= refs + frees) return;
    float3 c[4];
    if (id < refs) {
        uint2 ref = gEntFrame.Load2(gPtEntity.z + id * 8);
        uint q = ref.x * 96;
        uint part = gEntQuads.Load(q + 92);
        uint m = gPtEntity.x + (gEntFrame.Load(gPtEntity.y + ref.y * 32) + part) * 48;
        float4 r0 = asfloat(gEntFrame.Load4(m)), r1 = asfloat(gEntFrame.Load4(m + 16)), r2 = asfloat(gEntFrame.Load4(m + 32));
        [unroll] for (uint k = 0; k < 4; ++k) {
            float4 p = float4(asfloat(gEntQuads.Load3(q + k * 12)) / 16.0, 1);
            c[k] = float3(dot(r0, p), dot(r1, p), dot(r2, p));
        }
    } else {
        uint f = gPtEntity2.y + (id - refs) * 128;
        [unroll] for (uint k = 0; k < 4; ++k) c[k] = asfloat(gEntFrame.Load3(f + k * 12));
    }
    // The entity draws use triangles (0,1,2) (0,2,3); the shared index buffer (v0,v1,v2) (v2,v1,v3).
    uint addr = id * 48;
    gVertices.Store3(addr, asuint(c[1]));
    gVertices.Store3(addr + 12, asuint(c[2]));
    gVertices.Store3(addr + 24, asuint(c[0]));
    gVertices.Store3(addr + 36, asuint(c[3]));
}

// Light counts of the sections being built start at zero.
[numthreads(64, 1, 1)]
void PtClearCS(uint id : SV_DispatchThreadID) {
    if (id >= gPtJobs.x) return;
    BuildJob j = gJobs[gPtJobs.y + id];
    gLightCount[j.ptIndex] = 0;
    gLightPower[j.ptIndex] = 0;
    gLightSlot[j.ptIndex] = j.slot;
}

// This frame's light list: the lights of the sections near the camera.
[numthreads(64, 1, 1)]
void LightGatherCS(uint id : SV_DispatchThreadID) {
    if (id >= gPtLights.x) return;
    uint2 ls = gLightSections[gPtLights.y + id];
    uint e = ls.x;
    float d = asfloat(ls.y);
    uint n = min(gLightCount[e], kSectionLights), base = 0;
    if (n > 0) InterlockedAdd(gFrameLightCount[0], n, base);
    n = base < gPtLights.z ? min(n, gPtLights.z - base) : 0;
    // Sections are picked by power over distance (from the camera) squared.
    float w = n > 0 ? float(gLightPower[e]) / 16.0 / max(d * d, 64.0) : 0;
    gFrameSecs[id] = uint4(e, base, n, asuint(w));
    for (uint i = 0; i < n; ++i) gFrameLights[base + i] = id << 8 | i;
}

groupshared float gsScan[1024];
// Prefix sum of the section weights (one group).
[numthreads(1024, 1, 1)]
void LightCdfCS(uint tid : SV_GroupIndex) {
    uint n = gPtLights.x, per = (n + 1023) / 1024, b = tid * per, e = min(b + per, n);
    float s = 0;
    for (uint i = b; i < e; ++i) s += asfloat(gFrameSecs[i].w);
    gsScan[tid] = s;
    GroupMemoryBarrierWithGroupSync();
    for (uint off = 1; off < 1024; off <<= 1) {
        float v = tid >= off ? gsScan[tid - off] : 0;
        GroupMemoryBarrierWithGroupSync();
        gsScan[tid] += v;
        GroupMemoryBarrierWithGroupSync();
    }
    float run = tid ? gsScan[tid - 1] : 0;
    for (uint k = b; k < e; ++k) {
        run += asfloat(gFrameSecs[k].w);
        gFrameSecCdf[k] = run;
    }
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

// ---- light ------------------------------------------------------------------------------------------------------

static const float kSunCos = 0.99993;  // ~0.7 degrees: the sun / moon disc as seen through the shadows



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


// Radiance leaving a surface hit by a bounce ray (towards the ray), with `bounces` more bounces after it.
float3 bounceRadiance(RtSurface s, float dist, uint bounces, bool emissive, inout Rng rng) {
    float3 albedo = rtAlbedo(s);
    uint mat = matClass(s.mat);
    bool foliage = mat == kMatLeaves || mat == kMatPlant;
    float3 light = sunLight(s.pos, s.n, dist, foliage, rng);
    if (bounces > 0) {
        float3 d = cosineDir(s.n, float2(rng.next(), rng.next()));
        RtSurface h;
        float3 trans;
        if (rtClosestThrough(s.pos + s.n * rayEps(dist), d, gPtParams.x * 1.5, h, trans)) {
            float3 a2 = rtAlbedo(h);
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

// ---- emissive lights (ReSTIR DI) --------------------------------------------------------------------------------

struct LightPoint {
    float3 pos, n, le;
    float area;
    bool twoSided;
};

// The point at (u, v) on light `id` (a quad: corner 0 + u * edge 0->1 + v * edge 0->2).
bool lightPoint(uint id, float2 uv, out LightPoint l) {
    l = (LightPoint)0;
    uint e = id >> 8, i = id & 255;
    if (i >= min(gLightCount[e], kSectionLights)) return false;
    uint2 rec = gLights[e * kSectionLights + i];
    uint slot = gLightSlot[e], kind = rec.x >> 30, quad = rec.x & 0x3FFFFFFF;
    float3 c0, c1, c2;
    if (kind != kKindModel) {
        uint4 q = gRtCube[rtCubeIndex(slot, kind, quad)];
        c0 = rtCubeCorner(q, 0), c1 = rtCubeCorner(q, 1), c2 = rtCubeCorner(q, 2);
        l.n = kRtNormal[(q.x >> 20) & 7];
    } else {
        uint base = rtModelBase(slot, quad);
        c0 = rtModelCorner(base, 0), c1 = rtModelCorner(base, 1), c2 = rtModelCorner(base, 2);
        l.twoSided = true;
    }
    float3 cr = cross(c1 - c0, c2 - c0);
    l.area = length(cr);
    if (kind == kKindModel) l.n = cr / max(l.area, 1e-6);
    l.pos = rtSectionRel(slot) + c0 + (c1 - c0) * uv.x + (c2 - c0) * uv.y;
    l.le = unpackRgb9e5(rec.y);
    return true;
}

// Unshadowed light from l reaching p (normal n), per albedo; dir / dist towards the light point.
float3 lightContrib(LightPoint l, float3 p, float3 n, bool foliage, out float3 dir, out float dist) {
    float3 d = l.pos - p;
    float d2 = dot(d, d);
    dist = sqrt(d2);
    dir = d / max(dist, 1e-5);
    float cs = dot(n, dir);
    if (foliage) cs = abs(cs);
    float cl = -dot(l.n, dir);
    if (l.twoSided) cl = abs(cl);
    if (cs <= 0 || cl <= 0) return 0;
    // A window past ~48 blocks: block light in the game reaches 15 blocks; the largest emitters (lava lakes, lava
    // falls) still light their surroundings without colouring whole valleys.
    float win = saturate(1.0 - d2 * d2 / 5308416.0);  // 48^4
    return l.le * (cs * cl * l.area / (kPi * max(d2, 0.25)) * win * win);
}

uint packUv(float2 uv) { return uint(saturate(uv.x) * 65535.0 + 0.5) | uint(saturate(uv.y) * 65535.0 + 0.5) << 16; }
float2 unpackUv(uint v) { return float2(v & 0xFFFF, v >> 16) / 65535.0; }

struct Reservoir {
    uint id;
    float2 uv;
    float wsum, M, pHat;
    void add(uint cid, float2 cuv, float w, float m, float p, inout Rng rng) {
        wsum += w;
        M += m;
        if (w > 0 && rng.next() * wsum < w) id = cid, uv = cuv, pHat = p;
    }
};

// Light from emissive surfaces at a G-buffer surface: RIS over random lights, temporal reuse, one shadow ray.
float3 emissiveLight(uint2 px, float3 p, float3 n, bool foliage, float dist, bool reuse, uint2 prevPx, inout Rng rng) {
    uint count = min(gFrameLightCount[0], gPtLights.z);
    Reservoir r = (Reservoir)0;
    float3 dir;
    float d;
    LightPoint l;
    if (count > 0) {
        // Candidates alternate between two strategies: any light (uniform) and lights of sections picked by power
        // over distance from the camera; RIS weighs each by the pdf of the mixture.
        uint secs = gPtLights.x;
        float total = gFrameSecCdf[secs - 1];
        for (uint k = 0; k < 8; ++k) {
            uint sec, local;
            if ((k & 1) == 0 || total <= 0) {
                uint f = gFrameLights[min(uint(rng.next() * count), count - 1)];
                sec = f >> 8, local = f & 255;
            } else {
                float x = rng.next() * total;
                uint lo = 0, hi = secs - 1;
                while (lo < hi) {
                    uint mid = (lo + hi) >> 1;
                    if (gFrameSecCdf[mid] > x) hi = mid;
                    else lo = mid + 1;
                }
                sec = lo;
                local = min(uint(rng.next() * gFrameSecs[sec].z), max(gFrameSecs[sec].z, 1u) - 1);
            }
            uint4 fs = gFrameSecs[sec];
            if (fs.z == 0) continue;
            float pdf = total > 0 ? 0.5 / float(count) + 0.5 * asfloat(fs.w) / total / float(fs.z) : 1.0 / float(count);
            uint id = fs.x << 8 | local;
            float2 uv = float2(rng.next(), rng.next());
            float ph = lightPoint(id, uv, l) ? luminance(lightContrib(l, p, n, foliage, dir, d)) : 0;
            r.add(id, uv, ph / pdf, 1, ph, rng);
        }
    }
    if (reuse) {
        uint4 q = gPtRes[gPtSlots.x][prevPx];
        float qW = asfloat(q.z), qM = min(asfloat(q.w), 20.0 * 8.0);
        if (qM > 0 && qW > 0) {
            float2 uv = unpackUv(q.y);
            float ph = lightPoint(q.x, uv, l) ? luminance(lightContrib(l, p, n, foliage, dir, d)) : 0;
            r.add(q.x, uv, ph * qW * qM, qM, ph, rng);
        }
    }
    float W = r.pHat > 0 ? r.wsum / (r.M * r.pHat) : 0;
    float3 c = 0;
    if (W > 0 && lightPoint(r.id, r.uv, l)) {
        float3 f = lightContrib(l, p, n, foliage, dir, d);
        float3 vis = rtVisibility(p + n * (dot(n, dir) >= 0 ? rayEps(dist) : -rayEps(dist)), dir, max(d - 0.02, 0.0));
        c = f * W * vis;
        if (dot(vis, 1) <= 0) W = 0;  // an occluded sample is not worth reusing
    }
    gPtRes[gPtSlots.y][px] = uint4(r.id, packUv(r.uv), asuint(W), asuint(r.M));
    return c;
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
        gPtRes[gPtSlots.y][px] = 0;
        return;
    }
    if (dbg == 5) {  // light list: frame lights (red, /64), lights of the first light section (green, /16)
        uint e = gLightSections[gPtLights.y].x;
        gPtRw[kPtLight][px] = float4(gFrameLightCount[0] / 64.0, gPtLights.x ? gLightCount[e] / 16.0 : 0, gPtLights.x / 64.0, 1);
        return;
    }
    bool foliage = p.mat == kMatLeaves || p.mat == kMatPlant;
    float3 light = 0, emissive = 0;
    // Emissive lights: the previous reservoir at the reprojected pixel when it shows the same surface.
    {
        bool reuse = false;
        uint2 prevPx = 0;
        if (gPtParams.z > 0) {
            float4 pc = mul(gPrevViewProj, float4(p.rel, 1));
            if (pc.w > 0) {
                float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
                int2 q = int2(floor(puv * gScreen.xy));
                if (all(q >= 0) && all(q < int2(gScreen.xy))) {
                    float4 g = gPtRw[kPtGeom0 + gPtSlots.x][q];
                    float prevDist = length(p.rel + gPtMove.xyz);
                    reuse = g.w > 0 && abs(g.x - prevDist) < 0.03 * prevDist + 0.2 && dot(octDecode(g.yz), p.n) > 0.9;
                    prevPx = uint2(q);
                }
            }
        }
        Rng rng = makeRng(px, gPtOpts.w, 77);
        emissive = emissiveLight(px, p.rel, p.n, foliage, p.dist, reuse, prevPx, rng);
    }
    uint spp = max(gPtOpts.z, 1u);
    for (uint i = 0; i < spp; ++i) {
        Rng rng = makeRng(px, gPtOpts.w, i);
        float3 l = sunLight(p.rel, p.n, p.dist, foliage, rng);
        // Sky and bounce light: one cosine-weighted path.
        float3 d = cosineDir(p.n, float2(rng.next(), rng.next()));
        RtSurface s;
        float3 trans;
        float3 origin = p.rel + p.n * rayEps(p.dist);
        // (Emissive surfaces this path hits first are left out: the light sampling above counts them.)
        if (rtClosestThrough(origin, d, gPtParams.x * 1.5, s, trans))
            l += trans * bounceRadiance(s, p.dist, gPtOpts.y > 1 ? 1 : 0, false, rng);
        else
            l += trans * skyColor(d);
        light += l;
    }
    light = light / float(spp) + emissive;
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
    float n = gPtOpts.x == 6 ? 0.0 : min(hist.a, gPtParams.w);  // 6: no accumulation (raw samples)
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

// ---- DLSS Ray Reconstruction: noisy radiance + guide buffers ------------------------------------------------------

[numthreads(8, 8, 1)]
void PtComposeCS(uint2 px : SV_DispatchThreadID) {
    if (any(px >= uint2(gScreen.xy))) return;
    uint cur = gPtSlots.y;
    float2 pixel = float2(px) + 0.5;
    float3 view = normalize(viewRay(pixelNdc(pixel)));
    // Guides for every pixel: the G-buffer surface (blocks: rough dielectric), the forward layer's colour (entities),
    // nothing for the sky.
    float d = gTex[kTexDepth].Load(int3(px, 0)).r;
    float3 albedo = 0, n = -view, spec = 0;
    float rough = 1;
    if (d > 0) {
        float4 e = gTex[kTexLdrFwd].Load(int3(px, 0));
        if (e.a >= 0.499) {
            albedo = srgbToLinear(e.rgb);
        } else {
            albedo = srgbToLinear(gTex[kTexAlbedo].Load(int3(px, 0)).rgb);
            n = octDecode(gTex[kTexNormal].Load(int3(px, 0)).xy);
        }
        spec = envBrdfApprox2(0.04, rough * rough, dot(n, -view));
    }
    gPtRw[kPtDiffAlbedo][px] = float4(albedo, 1);
    gPtRw[kPtSpecAlbedo][px] = float4(spec, 1);
    gPtRw[kPtNormalRough][px] = float4(n, rough);
    gPtHitDist[px] = 0;  // the water pass writes its reflections' distance

    PtPixel p = ptPixel(px);
    if (!p.traced) {
        gPtRw[kPtGeom0 + cur][px] = 0;
        return;
    }
    gPtRw[kPtGeom0 + cur][px] = packGeom(p.dist, p.n);
    float3 light = gPtRw[kPtLight][px].rgb;
    float3 c = p.albedo * light + emission(p.albedo, p.mat);
    if (gWater.w < 0.5) c = applyFog(c, p.rel, skyBehind(px, p.rel / max(p.dist, 1e-4)));
    if (gPtOpts.x == 3) c = light;
    c = lerp(gRW[kRwScene][px].rgb, c, p.weight);
    gRW[kRwScene][px] = float4(c, 1);
    gRW[kRwSceneCopy][px] = float4(c, 1);
}
