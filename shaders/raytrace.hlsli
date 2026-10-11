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
StructuredBuffer<uint4> gRtCube : register(t0);  // CubeQuad (mesher.h)
StructuredBuffer<uint> gRtModel : register(t7);  // model quads, 12 words each

static const uint kKindOpaque = 0, kKindCutout = 1, kKindTranslucent = 2, kKindModel = 3;
static const float3 kRtNormal[6] = {
    float3(0, -1, 0), float3(0, 1, 0), float3(0, 0, -1), float3(0, 0, 1), float3(-1, 0, 0), float3(1, 0, 0)};

uint rtKind(uint map, uint geometry) { return (map >> (geometry * 2)) & 3; }

// Camera-relative position of a section's min corner.
float3 rtSectionRel(uint slot) { return float3(gSections[slot].origin.xyz - gCamBlock.xyz) - gCamFrac.xyz; }

uint rtCubeIndex(uint slot, uint kind, uint quad) { return gSections[slot].ranges[kind * 6].x + quad; }
uint rtModelBase(uint slot, uint quad) { return (gSections[slot].ranges[18].x + quad) * 12; }

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
    return p;
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

RtSurface rtSurface(uint slot, uint kind, uint prim, float2 bary, float3 origin, float3 dir, float t) {
    RtSurface s;
    s.pos = origin + dir * t;
    s.kind = kind;
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
        s.sky = dot(sky, 0.25);
        s.block = dot(blk, 0.25);
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
                  q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(), origin, dir, q.CommittedRayT());
    return true;
}

#endif
