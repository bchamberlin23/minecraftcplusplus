// Lines: world space (block outline) or screen space (crosshair). gDraw.sectionIndex = 1 for screen.
#include "common.hlsli"

struct LineVertex {
    float3 pos;    // camera-relative world position, or NDC xy
    uint color;    // 0xAABBGGRR
    float2 width;  // world lines: line width / screen size
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

// rendertype_lines.vsh: each line is a screen-space quad LineWidth pixels wide (two triangles, six vertices), the
// sides offset perpendicular to the projected line. Ends behind the near plane are clipped first.
LineOut WideLineVS(uint vid : SV_VertexID) {
    static const uint kCorner[6] = {0, 1, 2, 2, 1, 3};
    const uint corner = kCorner[vid % 6];
    const LineVertex a = gLines[gDraw.quadOffset + vid / 6 * 2], b = gLines[gDraw.quadOffset + vid / 6 * 2 + 1];
    float4 p0 = mul(gViewProj, float4(a.pos, 1)), p1 = mul(gViewProj, float4(b.pos, 1));
    const float kNear = 1e-3;
    if (p0.w < kNear && p1.w >= kNear) p0 = lerp(p0, p1, (kNear - p0.w) / (p1.w - p0.w));
    else if (p1.w < kNear && p0.w >= kNear) p1 = lerp(p1, p0, (kNear - p1.w) / (p0.w - p1.w));
    LineOut o;
    o.color = unpackColor(a.color);
    if (p0.w < kNear) {  // wholly behind the camera
        o.pos = float4(0, 0, 0, -1);
        return o;
    }
    const float3 n0 = p0.xyz / p0.w, n1 = p1.xyz / p1.w;
    const float2 d = (n1.xy - n0.xy) / a.width;
    const float2 dir = dot(d, d) > 0 ? normalize(d) : float2(1, 0);
    float2 offset = float2(-dir.y, dir.x) * a.width;
    if (offset.x < 0) offset = -offset;
    const float4 p = corner >= 2 ? p1 : p0;
    const float3 n = corner >= 2 ? n1 : n0;
    o.pos = float4((n + float3((corner & 1) ? -offset : offset, 0)) * p.w, p.w);
    return o;
}

float4 LinePS(LineOut i) : SV_Target {
    return i.color;
}
