// Inline ray tracing against the path tracer's acceleration structure (src/render/pathtrace.cpp). Include after
// common.hlsli and shading.hlsli.
//   TLAS instance  = one full-detail section: InstanceID = the renderer's mesh slot (gSections), transform = translation
//                    to its camera-relative position (camera at the origin, like everything else in the pipeline).
//   BLAS geometry  = its opaque / cutout / translucent cube quads and its model quads, two triangles per quad in the
//                    vertex pulling order (vertices 4i..4i+3, triangles (0,1,2) (2,1,3)); geometries without quads
//                    are left out, so InstanceContributionToHitGroupIndex maps geometry index -> kind (2 bits each).
// Opaque cube geometry is flagged opaque; the others come back as candidates: cutout and model quads are alpha
// tested against the block texture, translucent ones (water, glass, ice) are handled by the caller.
#ifndef RAYTRACE_HLSLI
#define RAYTRACE_HLSLI

RaytracingAccelerationStructure gScene : register(t6);
#ifndef RT_CUBE_QUADS  // terrain.hlsl has them as gQuads already
StructuredBuffer<uint4> gRtCube : register(t0);  // CubeQuad (mesher.h)
#endif
StructuredBuffer<uint> gRtModel : register(t7);  // model quads, 12 words each

cbuffer PathTrace : register(b4) {
    float4 gPtParams;  // x = traced radius (blocks), y = blend band (blocks), z = history valid (0 / 1), w = max history
    uint4 gPtOpts;     // x = debug view (MCPP_PT_DEBUG), y = bounces, z = samples per pixel, w = frame
    uint4 gPtJobs;     // x = build jobs, y = first job (element), z = quads to expand, w = vertex buffer offset (bytes)
    uint4 gPtSlots;    // x = previous history slot, y = this frame's history slot
    float4 gPtMove;    // camera position - previous camera position (blocks)
    uint4 gPtLights;   // x = light sections this frame, y = their first element, z = frame light capacity
    uint4 gPtEntity;   // entities (RtEntities, byte offsets): x = matrices, y = instances, z = refs, w = ref count
    uint4 gPtEntity2;  // x = free quads, y = their offset
};

// Path tracer targets (render resolution): PathTracer::Tex / Rw.
Texture2D<float4> gPtTex[8] : register(t8, space4);
RWTexture2D<float4> gPtRw[8] : register(u8, space4);
static const uint kPtLight = 0, kPtHist0 = 1, kPtGeom0 = 3, kPtNormalRough = 5, kPtDiffAlbedo = 6, kPtSpecAlbedo = 7;
RWTexture2D<float> gPtHitDist : register(u19, space4);  // specular hit distance (RR guide)

static const uint kKindOpaque = 0, kKindCutout = 1, kKindTranslucent = 2, kKindModel = 3;
static const float3 kRtNormal[6] = {
    float3(0, -1, 0), float3(0, 1, 0), float3(0, 0, -1), float3(0, 0, 1), float3(-1, 0, 0), float3(1, 0, 0)};

uint rtKind(uint map, uint geometry) { return (map >> (geometry * 2)) & 3; }

// Camera-relative position of a section's min corner.
float3 rtSectionRel(uint slot) { return float3(gSections[slot].origin.xyz - gCamBlock.xyz) - gCamFrac.xyz; }

uint rtCubeIndex(uint slot, uint kind, uint quad) { return gSections[slot].ranges[kind * 6].x + quad; }
uint rtModelBase(uint slot, uint quad) { return (gSections[slot].ranges[18].x + quad) * 15; }

uint rtRead16(uint base, uint index) {
    uint w = gRtModel[base + (index >> 1)];
    return (index & 1) ? (w >> 16) : (w & 0xFFFF);
}
float3 rtModelCorner(uint base, uint c) {
    return float3(rtRead16(base, c * 3), rtRead16(base, c * 3 + 1), rtRead16(base, c * 3 + 2)) / 2048.0 - 8.0;
}
float2 rtModelUv(uint base, uint c) { return float2(rtRead16(base, 12 + c * 2), rtRead16(base, 13 + c * 2)) / 65535.0; }

// Canonical face UVs of a cube quad at section-local position p (TerrainVS / pullCube, full detail).
float2 rtCubeUv(uint4 q, float3 p) {
    uint face = (q.x >> 20) & 7;
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
    return rot == 0 ? uv : rot == 1 ? float2(uv.y, -uv.x) : rot == 2 ? -uv : float2(-uv.y, uv.x);
}

// Section-local position of corner `corner` (vertex pulling order) of a cube quad (TerrainVS pullCube, full detail).
float3 rtCubeCorner(uint4 q, uint corner) {
    uint face = (q.x >> 20) & 7;
    if (((0x19u >> face) & 1u) != 0 && (corner == 1 || corner == 2)) corner ^= 3;
    float a = float(corner & 1), b = float(corner >> 1);
    float w = float(((q.x >> 12) & 15) + 1), h = float(((q.x >> 16) & 15) + 1);
    float3 axisA = face < 4 ? float3(1, 0, 0) : float3(0, 0, 1);
    float3 axisB = face < 2 ? float3(0, 0, 1) : float3(0, 1, 0);
    float3 p = float3(q.x & 15, (q.x >> 4) & 15, (q.x >> 8) & 15);
    p += max(kRtNormal[face], 0) + axisA * (a * w) + axisB * (b * h);
    if ((q.x >> 29) & 1u) {
        if (face == 1 || (face >= 2 && (corner >> 1) == 1)) p.y -= 0.125;
    }
    if (isFluidQuad(q)) {
        float2 st;
        fluidCorner(q, face, a, b, p, st);
    }
    return p;
}

// Texture coordinates of corner `corner` of a fluid quad (fluidCorner).
float2 rtFluidUv(uint4 q, uint corner) {
    uint face = (q.x >> 20) & 7;
    if (((0x19u >> face) & 1u) != 0 && (corner == 1 || corner == 2)) corner ^= 3;
    float3 p = 0;
    float2 st;
    fluidCorner(q, face, float(corner & 1), float(corner >> 1), p, st);
    return st;
}

struct RtSurface {
    float3 pos;    // camera-relative
    float3 n;      // geometric normal, against the ray
    float2 uv;
    uint layer;    // texture layer (animation frame applied)
    uint mat;      // gMaterial entry of the base layer
    float3 tint;   // linear 0..1 (sRGB tint values)
    float sky, block;  // light levels 0..15 at the surface (the game's light engine)
    uint kind;
};

// Texture coordinates + layer only (alpha tests of candidates).
float3 rtUvLayer(uint slot, uint kind, uint prim, float2 bary, float3 pos, out uint baseLayer) {
    uint quad = prim >> 1;
    if (kind != kKindModel) {
        uint4 q = gRtCube[rtCubeIndex(slot, kind, quad)];
        baseLayer = q.y & 0xFFF;
        if (isFluidQuad(q)) {  // the corners' fluid texture coordinates across the triangle
            uint3 c = (prim & 1) ? uint3(2, 1, 3) : uint3(0, 1, 2);
            float2 uv = rtFluidUv(q, c.x) * (1 - bary.x - bary.y) + rtFluidUv(q, c.y) * bary.x + rtFluidUv(q, c.z) * bary.y;
            return float3(uv, float(gTextureRemap[baseLayer]));
        }
        return float3(rtCubeUv(q, pos - rtSectionRel(slot)), float(gTextureRemap[baseLayer]));
    }
    uint base = rtModelBase(slot, quad);
    uint3 c = (prim & 1) ? uint3(2, 1, 3) : uint3(0, 1, 2);
    float2 uv = rtModelUv(base, c.x) * (1 - bary.x - bary.y) + rtModelUv(base, c.y) * bary.x + rtModelUv(base, c.z) * bary.y;
    baseLayer = gRtModel[base + 10] & 0xFFFF;
    return float3(uv, float(gTextureRemap[baseLayer]));
}

bool rtAlphaTest(uint slot, uint kind, uint prim, float2 bary, float3 pos) {
    uint baseLayer;
    float3 uvl = rtUvLayer(slot, kind, prim, bary, pos, baseLayer);
    return gTextures.SampleLevel(gSampler, uvl, 0).a >= 0.5;
}

static const uint kRtEntities = 0xFFFFFF;  // InstanceID of the entities' instance (posed triangles)
static const uint kKindEntity = 4;

// The entities' own data (EntityRenderer, entity.hlsl layouts): model quads (96 bytes: pos[4], uv[4], normal, part)
// and this frame's upload (part matrices, instances, cutout refs, free quads of 128 bytes), and the entity atlas.
ByteAddressBuffer gEntQuads : register(t6, space4);
ByteAddressBuffer gEntFrame : register(t7, space4);
Texture2D<float4> gEntAtlas : register(t1, space4);

float3 rtAlbedo(RtSurface s) {
    if (s.kind == kKindEntity) {  // the entity atlas (point sampled, like EntityPS) or a block texture layer
        float4 t;
        if (s.layer == 0xFFFFFFFF) {
            uint w, h;
            gEntAtlas.GetDimensions(w, h);
            t = gEntAtlas.Load(int3(min(uint2(frac(s.uv) * float2(w, h)), uint2(w - 1, h - 1)), 0));
        } else {
            t = gTextures.SampleLevel(gSampler, float3(s.uv, s.layer), 0);
        }
        return srgbToLinear(t.rgb * s.tint);
    }
    return srgbToLinear(gTextures.SampleLevel(gSampler, float3(s.uv, s.layer), 0).rgb * s.tint);
}

// An entity triangle: quad id = geometry * 65536 + prim / 2 (EntityGenCS order: cutout refs, then free quads; its
// vertices c1 c2 c0 c3, triangles (c1, c2, c0) and (c0, c2, c3)).
RtSurface rtEntitySurface(uint geometry, uint prim, float2 bary, float3 pos, float3 dir) {
    RtSurface s = (RtSurface)0;
    s.pos = pos;
    s.kind = kKindEntity;
    uint id = geometry * 65536 + (prim >> 1);
    uint3 c = (prim & 1) ? uint3(0, 2, 3) : uint3(1, 2, 0);
    float3 w = float3(1 - bary.x - bary.y, bary.x, bary.y);
    uint refs = gPtEntity.w & 0xFFFFF;
    uint color, light;
    float3 n;
    if (id < refs) {
        uint2 ref = gEntFrame.Load2(gPtEntity.z + id * 8);  // (model quad, instance)
        uint q = ref.x * 96, inst = gPtEntity.y + ref.y * 32;
        float2 uv = asfloat(gEntQuads.Load2(q + 48 + c.x * 8)) * w.x + asfloat(gEntQuads.Load2(q + 48 + c.y * 8)) * w.y +
                    asfloat(gEntQuads.Load2(q + 48 + c.z * 8)) * w.z;
        float4 rect = asfloat(gEntFrame.Load4(inst + 16));  // the texture's place in the atlas
        s.uv = rect.xy + uv * rect.zw;
        s.layer = 0xFFFFFFFF;
        color = gEntFrame.Load(inst + 4), light = gEntFrame.Load(inst + 8);
        uint m = gPtEntity.x + (gEntFrame.Load(inst) + gEntQuads.Load(q + 92)) * 48;  // the part's matrix
        float3 pn = asfloat(gEntQuads.Load3(q + 80));
        n = float3(dot(asfloat(gEntFrame.Load3(m)), pn), dot(asfloat(gEntFrame.Load3(m + 16)), pn), dot(asfloat(gEntFrame.Load3(m + 32)), pn));
    } else {
        uint f = gPtEntity2.y + (id - refs) * 128;
        s.uv = asfloat(gEntFrame.Load2(f + 48 + c.x * 8)) * w.x + asfloat(gEntFrame.Load2(f + 48 + c.y * 8)) * w.y +
               asfloat(gEntFrame.Load2(f + 48 + c.z * 8)) * w.z;
        uint tex = gEntFrame.Load(f + 92);
        s.layer = tex == 0xFFFFFFFF ? tex : (tex & 0xFFFF);
        n = asfloat(gEntFrame.Load3(f + 80));
        color = gEntFrame.Load(f + 96), light = gEntFrame.Load(f + 100);
    }
    s.n = dot(n, n) > 1e-8 ? normalize(n) : -dir;
    if (dot(s.n, dir) > 0) s.n = -s.n;
    s.tint = float3(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF) / 255.0;  // 0xAABBGGRR
    s.sky = float((light >> 4) & 15);
    s.block = float(light & 15);
    s.mat = (light & 256) ? kMatGlow : kMatDefault;  // full bright parts (eyes) glow
    return s;
}

RtSurface rtSurface(uint slot, uint kind, uint prim, float2 bary, float3 origin, float3 dir, float t, uint geometry) {
    RtSurface s;
    s.pos = origin + dir * t;
    s.kind = kind;
    if (slot == kRtEntities) return rtEntitySurface(geometry, prim, bary, s.pos, dir);
    uint quad = prim >> 1;
    uint baseLayer;
    float3 uvl = rtUvLayer(slot, kind, prim, bary, s.pos, baseLayer);
    s.uv = uvl.xy;
    s.layer = uint(uvl.z);
    s.mat = gMaterial[baseLayer];
    if (kind != kKindModel) {
        uint4 q = gRtCube[rtCubeIndex(slot, kind, quad)];
        s.n = kRtNormal[(q.x >> 20) & 7];
        s.tint = tintColor(((q.y >> 20) << 4) | ((q.x >> 25) & 15));
        float4 sky = float4(q.z & 0xFF, (q.z >> 8) & 0xFF, (q.z >> 16) & 0xFF, q.z >> 24) / 16.0;
        float4 blk = float4(q.w & 0xFF, (q.w >> 8) & 0xFF, (q.w >> 16) & 0xFF, q.w >> 24) / 16.0;
        s.sky = isFluidQuad(q) ? fluidSky(q) : dot(sky, 0.25);
        s.block = isFluidQuad(q) ? fluidBlock(q) : dot(blk, 0.25);
    } else {
        uint base = rtModelBase(slot, quad);
        uint3 c = (prim & 1) ? uint3(2, 1, 3) : uint3(0, 1, 2);
        float3 a = rtModelCorner(base, c.x), b = rtModelCorner(base, c.y), d = rtModelCorner(base, c.z);
        s.n = normalize(cross(b - a, d - a));
        uint lit = gRtModel[base + 11];
        s.tint = tintColor(lit >> 8);
        s.sky = float((lit >> 4) & 15);
        s.block = float(lit & 15);
    }
    if (dot(s.n, dir) > 0) s.n = -s.n;
    return s;
}

// Closest hit for camera / bounce rays (translucent surfaces count as hits). False: nothing within tmax.
bool rtClosest(float3 origin, float3 dir, float tmax, out RtSurface s) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0;
    ray.TMax = tmax;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint kind = rtKind(q.CandidateInstanceContributionToHitGroupIndex(), q.CandidateGeometryIndex());
        if (kind == kKindTranslucent ||
            rtAlphaTest(q.CandidateInstanceID(), kind, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(),
                        origin + dir * q.CandidateTriangleRayT()))
            q.CommitNonOpaqueTriangleHit();
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        s = (RtSurface)0;
        return false;
    }
    s = rtSurface(q.CommittedInstanceID(), rtKind(q.CommittedInstanceContributionToHitGroupIndex(), q.CommittedGeometryIndex()),
                  q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(), origin, dir, q.CommittedRayT(),
                  q.CommittedGeometryIndex());
    return true;
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
                  q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(), origin, dir, q.CommittedRayT(),
                  q.CommittedGeometryIndex());
    return true;
}

// ---- light ----------------------------------------------------------------------------------------------------

float rayEps(float dist) { return 0.003 + dist * 0.0004; }

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

// Specular albedo for RR's guide (DLSS-RR integration guide 4.2.1, Ray Tracing Gems ch. 32).
float3 envBrdfApprox2(float3 specularColor, float alpha, float NoV) {
    NoV = abs(NoV);
    float4 X = float4(1.0, NoV, NoV * NoV, NoV * NoV * NoV);
    float4 Y = float4(1.0, alpha, alpha * alpha, alpha * alpha * alpha);
    float2x2 M1 = float2x2(0.99044, -1.28514, 1.29678, -0.755907);
    float3x3 M2 = float3x3(1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592, 121.563, 626.13, 316.627);
    float2x2 M3 = float2x2(0.0365463, 3.32707, 9.0632, -9.04756);
    float3x3 M4 = float3x3(1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949, 5.56589, 19.7886, -20.2123);
    float bias = dot(mul(M1, X.xy), Y.xy) * rcp(dot(mul(M2, X.xyw), Y.xyw));
    float scale = dot(mul(M3, X.xy), Y.xy) * rcp(dot(mul(M4, X.xzw), Y.xyw));
    bias *= saturate(specularColor.g * 50);
    return mad(specularColor, max(0, scale), max(0, bias));
}

// Radiance leaving a surface hit by a reflection ray: hard sun / moon shadow, the light levels for the rest.
float3 rtHitRadiance(RtSurface h, float dist) {
    float3 albedo = rtAlbedo(h);
    float3 light = cachedLight(h.sky, h.block, h.n);
    float ndl = dot(h.n, gLightDir.xyz);
    if (ndl > 0 && dot(gLightColor.rgb, 1) > 0)
        light += gLightColor.rgb * ndl * rtVisibility(h.pos + h.n * rayEps(dist), gLightDir.xyz, 1e4);
    return albedo * light + emission(albedo, matClass(h.mat));
}

#endif
