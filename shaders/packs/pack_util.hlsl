// Utility passes of the shader pack pipeline (src/render/packs/pack_frame.cpp): GL depth copies of the reversed-Z
// scene depth (depthtex0..2), GPU culling of terrain sections against a pack's shadow volume, mip chains of
// render targets, and the merge of passes that drew without the pack into colortex0. Root signature: utilRoot in
// pack_pipeline.cpp.
cbuffer UtilConstants : register(b0) {
    float4x4 uShadowMat;  // camera-relative position -> shadow clip (GL, before the pack's distortion)
    int4 uCamBlock;
    float4 uCamFrac;
    float4 uDepth;        // x = F / (F - n) (reversed -> GL depth), y = width, z = height
    uint4 uMisc;          // x = active sections, y = commands per segment, z = shadow distance (blocks, 0: none)
};
Texture2D<float4> uSrc : register(t0);
RWTexture2D<float4> uDst : register(u0);

[numthreads(8, 8, 1)]
void PackDepthToGlCS(uint2 id : SV_DispatchThreadID) {
    if (id.x >= uint(uDepth.y) || id.y >= uint(uDepth.z)) return;
    float d = uSrc[id].r;
    uDst[id] = min((1.0 - d) * uDepth.x, 1.0);
}

// 2x2 box down-sample of one mip level (uDepth.yz = destination size).
[numthreads(8, 8, 1)]
void PackMipCS(uint2 id : SV_DispatchThreadID) {
    if (id.x >= uint(uDepth.y) || id.y >= uint(uDepth.z)) return;
    uint2 s = id * 2;
    uDst[id] = (uSrc[s] + uSrc[s + uint2(1, 0)] + uSrc[s + uint2(0, 1)] + uSrc[s + uint2(1, 1)]) * 0.25;
}

// ---- shadow culling: one thread per active section, 4 segments (opaque, cutout, translucent cubes, models) ----
struct Section {
    int4 origin;
    uint2 ranges[19];
};
StructuredBuffer<Section> uSections : register(t1);
StructuredBuffer<uint> uActive : register(t2);
RWByteAddressBuffer uCounts : register(u1);
RWByteAddressBuffer uArgs : register(u2);

void emitShadow(uint seg, uint offset, uint count, uint section) {
    while (count > 0) {
        uint n = min(count, 65536u);
        uint slot;
        uCounts.InterlockedAdd(seg * 4, 1, slot);
        if (slot < uMisc.y) {
            uint addr = (seg * uMisc.y + slot) * 28;
            uArgs.Store4(addr, uint4(offset, section, n * 6, 1));
            uArgs.Store3(addr + 16, uint3(0, 0, 0));
        }
        offset += n;
        count -= n;
    }
}

[numthreads(64, 1, 1)]
void PackShadowCullCS(uint tid : SV_DispatchThreadID) {
    if (tid >= uMisc.x) return;
    uint id = uActive[tid];
    int4 origin = uSections[id].origin;
    float3 size = float3(float(16u << origin.w), 16.0, float(16u << origin.w));
    float3 mn = float3(origin.xyz - uCamBlock.xyz) - uCamFrac.xyz;
    float3 center = mn + size * 0.5;
    float3 extent = size * 0.5 + 1.0;  // margin for waving geometry
    if (uMisc.z > 0) {
        float3 c = clamp(0, center - extent, center + extent);
        float d = float(uMisc.z);
        if (dot(c.xz, c.xz) > d * d) return;
    }
    float4x4 m = uShadowMat;
    float3 lc = mul(m, float4(center, 1)).xyz;
    float3 le = float3(dot(abs(m[0].xyz), extent), dot(abs(m[1].xyz), extent), dot(abs(m[2].xyz), extent));
    if (any(abs(lc) - le > 1.0)) return;
    [unroll] for (uint layer = 0; layer < 3; ++layer) {
        uint first = uSections[id].ranges[layer * 6].x, count = 0;
        [unroll] for (uint f = 0; f < 6; ++f) count += uSections[id].ranges[layer * 6 + f].y;
        if (count) emitShadow(layer, first, count, id);
    }
    uint2 model = uSections[id].ranges[18];
    if (model.y) emitShadow(3, model.x, model.y, id);
}

// ---- merge: passes that drew without the pack (LDR layer, straight alpha) over colortex0 ----
struct MergeOut {
    float4 pos : SV_Position;
};
MergeOut PackMergeVS(uint vid : SV_VertexID) {
    MergeOut o;
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    o.pos = float4(p * 2 - 1, 0, 1);
    return o;
}
float4 PackMergePS(MergeOut i) : SV_Target {
    // The layer was drawn the D3D way up (row 0 = top); colortex rows follow GL (row 0 = bottom).
    uint2 p = uint2(i.pos.xy);
    p.y = uint(uDepth.z) - 1 - p.y;
    return uSrc[p];
}

// ---- the scene depth turned the D3D way up, for passes that draw without the pack (they test against it) ----
float PackDepthFlipPS(MergeOut i) : SV_Depth {
    uint2 p = uint2(i.pos.xy);
    p.y = uint(uDepth.z) - 1 - p.y;
    return uSrc[p].r;
}
