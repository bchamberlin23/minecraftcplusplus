// The glowing entities' outline (post_effect/entity_outline.json): entity_sobel, two entity_outline_box_blur passes,
// then blended over the frame (RenderPipelines.ENTITY_OUTLINE_BLIT: src alpha, 1 - src alpha).
cbuffer Post : register(b0) {
    float2 gInSize;    // input texels in use
    float2 gUvScale;   // used size / texture size (the targets only grow)
    float2 gBlurDir;
    float2 gPad;
};
Texture2D<float4> gIn : register(t0);
SamplerState gPointClamp : register(s0);
SamplerState gLinearClamp : register(s1);

struct PostOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;  // 0..1 over the used area
};
PostOut OutlineVS(uint vid : SV_VertexID) {  // core/screenquad: one triangle over the target
    PostOut o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 tap(SamplerState s, float2 uv) { return gIn.SampleLevel(s, uv * gUvScale, 0); }

float4 SobelPS(PostOut i) : SV_Target {  // post/entity_sobel
    float2 t = 1.0 / gInSize;
    float4 center = tap(gPointClamp, i.uv), left = tap(gPointClamp, i.uv - float2(t.x, 0)), right = tap(gPointClamp, i.uv + float2(t.x, 0));
    float4 up = tap(gPointClamp, i.uv - float2(0, t.y)), down = tap(gPointClamp, i.uv + float2(0, t.y));
    float total = saturate(abs(center.a - left.a) + abs(center.a - right.a) + abs(center.a - up.a) + abs(center.a - down.a));
    float3 c = center.rgb * center.a + left.rgb * left.a + right.rgb * right.a + up.rgb * up.a + down.rgb * down.a;
    return float4(c * 0.2, total);
}

float4 BlurPS(PostOut i) : SV_Target {  // post/entity_outline_box_blur (radius 2, bilinear)
    float2 step = gBlurDir / gInSize;
    const float radius = 2.0;
    float4 blurred = 0;
    for (float a = -radius + 0.5; a <= radius; a += 2.0) blurred += tap(gLinearClamp, i.uv + step * a);
    blurred += tap(gLinearClamp, i.uv + step * radius) / 2.0;
    return float4((blurred / (radius + 0.5)).rgb, blurred.a);
}

float4 BlitPS(PostOut i) : SV_Target { return tap(gLinearClamp, i.uv); }  // core/blit_screen
