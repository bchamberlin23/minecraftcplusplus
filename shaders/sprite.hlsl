// Screen-space quads for the HUD, optionally textured from the block texture array.
#include "common.hlsli"

struct SpriteVertex {
    float2 pos;   // NDC
    float2 uv;
    uint layer;   // texture layer, 0xFFFFFFFF = solid colour
    uint color;   // 0xAABBGGRR
};
StructuredBuffer<SpriteVertex> gSprites : register(t0);

struct SpriteOut {
    float4 pos : SV_Position;
    float3 uvl : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation uint textured : TEXTURED;
};

SpriteOut SpriteVS(uint vid : SV_VertexID) {
    SpriteVertex v = gSprites[gDraw.quadOffset + vid];
    SpriteOut o;
    o.pos = float4(v.pos, 0, 1);
    o.uvl = float3(v.uv, float(v.layer & 0xFFFF));
    o.color = float4(v.color & 255, (v.color >> 8) & 255, (v.color >> 16) & 255, v.color >> 24) / 255.0;
    o.textured = v.layer != 0xFFFFFFFF;
    return o;
}

float4 SpritePS(SpriteOut i) : SV_Target {
    float4 c = i.color;
    if (i.textured) c *= gTextures.SampleLevel(gSampler, i.uvl, 0);
    clip(c.a - 0.01);
    return c;
}
