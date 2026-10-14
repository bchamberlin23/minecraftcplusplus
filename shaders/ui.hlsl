// User interface: screen-space quads, solid or textured from the GUI atlas / item icon atlas.
cbuffer UiConstants : register(b0) {
    float2 gInvScreen;  // 2 / screen size in pixels
    float2 gGlintOffset;  // enchantment glint scroll (texture units)
    float4 gGlintRect;    // glint texture in the GUI atlas: u0 v0 du dv
    float gGlintScale;    // screen pixels per glint repeat
    float gGlintAlpha;    // the Glint Strength option (glint.fsh GlintAlpha)
    float2 gPad;
};
Texture2D gGui : register(t0);
Texture2D gIcons : register(t1);
Texture2D gItems : register(t2);   // the item model atlas (entity previews' held items)
Texture2D gEntity : register(t3);  // the entity atlas (their special item models)
Texture2DArray gBlocks : register(t4);  // the block textures (block item models; mode 6 + layer << 8)
SamplerState gPoint : register(s0);

struct UiIn {
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    uint mode : MODE;
};
struct UiOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation uint mode : MODE;
};

UiOut UiVS(UiIn i) {
    UiOut o;
    o.pos = float4(i.pos.x * gInvScreen.x - 1.0, 1.0 - i.pos.y * gInvScreen.y, 0, 1);
    o.uv = i.uv;
    o.color = i.color;
    o.mode = i.mode;
    return o;
}

float4 UiPS(UiOut i) : SV_Target {
    float4 c = i.color;
    const uint mode = i.mode & 255;
    if (mode == 1) c *= gGui.SampleLevel(gPoint, i.uv, 0);
    else if (mode == 2) c *= gIcons.SampleLevel(gPoint, i.uv, 0);
    else if (mode >= 4 && mode <= 6) {  // item model quads: cutout
        c *= mode == 4   ? gItems.SampleLevel(gPoint, i.uv, 0)
             : mode == 5 ? gEntity.SampleLevel(gPoint, i.uv, 0)
                         : gBlocks.SampleLevel(gPoint, float3(i.uv, float(i.mode >> 8)), 0);
        if (c.a < 0.1) discard;
    }
    else if (mode == 3) {  // enchantment glint over an item icon (additive, masked by the icon)
        if (gIcons.SampleLevel(gPoint, i.uv, 0).a < 0.1) discard;
        float2 p = i.pos.xy / gGlintScale;
        const float a = 0.17453292;  // rotated 10 degrees like the game's glint texturing
        float2 r = float2(p.x * cos(a) - p.y * sin(a), p.x * sin(a) + p.y * cos(a)) + gGlintOffset;
        float4 g = gGui.SampleLevel(gPoint, gGlintRect.xy + frac(r) * gGlintRect.zw, 0);
        c = float4(g.rgb * c.a * gGlintAlpha, 1);
    }
    if (c.a < 0.004) discard;
    return c;
}
