// Lines: world space (block outline) or screen space (crosshair). gDraw.sectionIndex = 1 for screen.
#include "common.hlsli"

struct LineVertex {
    float3 pos;  // camera-relative world position, or NDC xy
    uint color;  // 0xAABBGGRR
};
StructuredBuffer<LineVertex> gLines : register(t0);

struct LineOut {
    float4 pos : SV_Position;
    float4 color : COLOR0;
};

float4 unpackColor(uint c) {
    return float4(c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24) / 255.0;
}

LineOut LineVS(uint vid : SV_VertexID) {
    LineVertex v = gLines[gDraw.quadOffset + vid];
    LineOut o;
    o.pos = gDraw.sectionIndex == 0 ? mul(gViewProj, float4(v.pos, 1)) : float4(v.pos.xy, 0.5, 1);
    o.color = unpackColor(v.color);
    return o;
}

float4 LinePS(LineOut i) : SV_Target {
    return i.color;
}
