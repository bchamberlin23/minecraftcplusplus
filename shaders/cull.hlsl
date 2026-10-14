// GPU culling: one thread per section. Frustum + distance test, then per-face-direction
// back-face rejection, appending ExecuteIndirect commands for each visible draw range.
#include "common.hlsli"
#include "shading.hlsli"

RWByteAddressBuffer gCounts : register(u0);  // one uint per pass
RWByteAddressBuffer gArgs : register(u1);    // 28-byte commands: root constants (2) + DrawIndexed (5)

static const uint kMaxQuadsPerDraw = 65536;  // size of the shared quad index buffer
static const uint kCommandBytes = 28;

void emit(uint pass, uint quadOffset, uint quadCount, uint sectionIndex) {
    while (quadCount > 0) {
        uint n = min(quadCount, kMaxQuadsPerDraw);
        uint slot;
        gCounts.InterlockedAdd(pass * 4, 1, slot);
        if (slot < gSegCap[pass]) {
            uint addr = (gSegBase[pass] + slot) * kCommandBytes;
            gArgs.Store4(addr, uint4(quadOffset, sectionIndex, n * 6, 1));
            gArgs.Store3(addr + 16, uint3(0, 0, 0));
        }
        quadOffset += n;
        quadCount -= n;
    }
}

// The section's box relative to the camera; false if outside the frustum or the fog.
bool sectionVisible(uint id, out float3 mn, out float3 mx) {
    int4 origin = gSections[id].origin;
    mn = float3(origin.xyz - gCamBlock.xyz) - gCamFrac.xyz;
    mx = mn + float3(float(16u << origin.w), 16.0, float(16u << origin.w));  // LOD nodes stay 16 tall

    [unroll] for (uint i = 0; i < 5; ++i) {
        float4 pl = gFrustum[i];
        float3 v = float3(pl.x >= 0 ? mx.x : mn.x, pl.y >= 0 ? mx.y : mn.y, pl.z >= 0 ? mx.z : mn.z);
        if (dot(pl.xyz, v) + pl.w < 0) return false;
    }
    if (gFogParams.z > 0) {
        float3 c = clamp(0, mn, mx);
        if (dot(c, c) > gFogParams.z * gFogParams.z) return false;
    }
    return true;
}

[numthreads(64, 1, 1)]
void CullCS(uint tid : SV_DispatchThreadID) {
    if (tid >= gMisc.x) return;
    uint id = gActive[tid];
    float3 mn, mx;
    if (!sectionVisible(id, mn, mx)) return;
    // Opaque and cutout: one draw for all six face ranges of the layer (they are contiguous); the rasterizer's
    // back-face culling replaces the per-direction test. Fewer, larger draws measured faster at every LOD
    // level (sptest 1440p: 74k -> 33k draws, 1.45 -> 1.05 ms). A section cross-fading draws its opaque faces with
    // the cutout pipeline, whose pixel shader discards (fadedOut); the opaque one stays discard-free.
    bool fading = fadeProgress(gSections[id].fade) < 1.0;
    for (uint l = 0; l < 2; ++l) {
        uint first = gSections[id].ranges[l * 6].x, count = 0;
        [unroll] for (uint f = 0; f < 6; ++f) count += gSections[id].ranges[l * 6 + f].y;
        if (count != 0) emit(fading ? 1 : l, first, count, id);
    }
    uint2 m = gSections[id].ranges[18];
    if (m.y != 0) emit(3, m.x, m.y, id);
}

// Translucent: gMisc.w sections after the active list, sorted far to near by the CPU. Blending needs that order
// kept, so their commands are not appended but placed by an ordered prefix sum (count, scan, emit: one group per 1024
// sections). Fixed slots per section left empty draws, which cost the command processor as much as small ones:
// distant oceans made tens of thousands per frame (2.6 ms). The group totals and offsets live after the last segment.
uint scratchAddr(uint group) { return (gSegBase[3] + gSegCap[3]) * kCommandBytes + group * 4; }

// The face directions of translucent section s that have quads and can be visible, as a bit mask.
uint translucentFaces(uint s, out uint id) {
    id = 0;
    if (s >= gMisc.w) return 0;
    id = gActive[gMisc.x + s];
    float3 mn, mx;
    if (!sectionVisible(id, mn, mx)) return 0;
    // A face direction can only be visible if the camera is on its front side of some plane (drawn unculled).
    bool vis[6] = {mx.y > 0, mn.y < 0, mx.z > 0, mn.z < 0, mx.x > 0, mn.x < 0};
    uint mask = 0;
    [unroll] for (uint f = 0; f < 6; ++f)
        if (vis[f] && gSections[id].ranges[12 + f].y != 0) mask |= 1u << f;
    return mask;
}

groupshared uint gsWaveSums[256];

// Exclusive prefix sum of v over the 1024-thread group, and the group's total.
uint groupPrefix(uint i, uint v, out uint total) {
    const uint lanes = WaveGetLaneCount(), wave = i / lanes, waves = (1024 + lanes - 1) / lanes;
    uint before = WavePrefixSum(v), sum = WaveActiveSum(v);
    if (WaveIsFirstLane()) gsWaveSums[wave] = sum;
    GroupMemoryBarrierWithGroupSync();
    total = 0;
    for (uint w = 0; w < waves; ++w) {
        if (w < wave) before += gsWaveSums[w];
        total += gsWaveSums[w];
    }
    return before;
}

[numthreads(1024, 1, 1)]
void TranslucentCountCS(uint i : SV_GroupIndex, uint g : SV_GroupID) {
    uint id, total;
    groupPrefix(i, countbits(translucentFaces(g * 1024 + i, id)), total);
    if (i == 0) gArgs.Store(scratchAddr(g), total);
}

[numthreads(1024, 1, 1)]
void TranslucentScanCS(uint i : SV_GroupIndex) {  // one group: at most 1024 groups of sections (kMaxActive)
    const uint groups = (gMisc.w + 1023) / 1024;
    uint v = i < groups ? gArgs.Load(scratchAddr(i)) : 0, total;
    uint before = groupPrefix(i, v, total);
    if (i < groups) gArgs.Store(scratchAddr(i), before);
    if (i == 0) gCounts.Store(2 * 4, total);
}

[numthreads(1024, 1, 1)]
void TranslucentEmitCS(uint i : SV_GroupIndex, uint g : SV_GroupID) {
    uint id, total;
    uint mask = translucentFaces(g * 1024 + i, id);
    uint slot = gArgs.Load(scratchAddr(g)) + groupPrefix(i, countbits(mask), total);
    for (uint f = 0; f < 6; ++f) {
        if ((mask & (1u << f)) == 0) continue;
        uint2 r = gSections[id].ranges[12 + f];
        uint addr = (gSegBase[2] + slot++) * kCommandBytes;
        gArgs.Store4(addr, uint4(r.x, id, r.y * 6, 1));
        gArgs.Store3(addr + 16, uint3(0, 0, 0));
    }
}

// ---- shadow cascades ---------------------------------------------------------------------------------------
// One thread per section: for every cascade updated this frame (gPassA.y bit mask), tests the section's box
// against the cascade's light-space box (no near plane: casters between the light and the box are clamped onto
// it by the rasterizer's depth clamp) and emits one draw per layer (opaque cubes, cutout cubes, models), the
// rasterizer's culling doing the per-face work. Commands go to segment (cascade * 3 + kind) of the shadow
// argument buffer (bound as gArgs, its counts as gCounts; gPassA.z = commands per segment).
void emitShadow(uint seg, uint quadOffset, uint quadCount, uint sectionIndex) {
    while (quadCount > 0) {
        uint n = min(quadCount, kMaxQuadsPerDraw);
        uint slot;
        gCounts.InterlockedAdd(seg * 4, 1, slot);
        if (slot < gPassA.z) {
            uint addr = (seg * gPassA.z + slot) * kCommandBytes;
            gArgs.Store4(addr, uint4(quadOffset, sectionIndex, n * 6, 1));
            gArgs.Store3(addr + 16, uint3(0, 0, 0));
        }
        quadOffset += n;
        quadCount -= n;
    }
}

[numthreads(64, 1, 1)]
void ShadowCullCS(uint tid : SV_DispatchThreadID) {
    if (tid >= gMisc.x) return;
    uint id = gActive[tid];
    int4 origin = gSections[id].origin;
    float3 size = float3(float(16u << origin.w), 16.0, float(16u << origin.w));
    float3 center = float3(origin.xyz - gCamBlock.xyz) - gCamFrac.xyz + size * 0.5;
    float3 extent = size * 0.5 + 1.0;  // a block of margin for waving foliage

    uint opaqueFirst = gSections[id].ranges[0].x, opaqueCount = 0, cutoutFirst = gSections[id].ranges[6].x, cutoutCount = 0;
    [unroll] for (uint f = 0; f < 6; ++f) {
        opaqueCount += gSections[id].ranges[f].y;
        cutoutCount += gSections[id].ranges[6 + f].y;
    }
    uint2 model = gSections[id].ranges[18];

    for (uint c = 0; c < gOpts.x; ++c) {
        // Coarse nodes only reach the outer cascades.
        if ((gPassA.y & (1u << c)) == 0 || origin.w > max(int(c) - 1, 0)) continue;
        float4x4 m = gShadowMat[c];
        float3 lc = mul(m, float4(center, 1)).xyz;
        float3 le = float3(dot(abs(m[0].xyz), extent), dot(abs(m[1].xyz), extent), dot(abs(m[2].xyz), extent));
        if (abs(lc.x) - le.x > 1 || abs(lc.y) - le.y > 1 || lc.z - le.z > 1) continue;
        if (opaqueCount) emitShadow(c * 3, opaqueFirst, opaqueCount, id);
        if (cutoutCount) emitShadow(c * 3 + 1, cutoutFirst, cutoutCount, id);
        if (model.y) emitShadow(c * 3 + 2, model.x, model.y, id);
    }
}
