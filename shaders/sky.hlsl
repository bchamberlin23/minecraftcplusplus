// Sky (vanilla SkyRenderer), clouds (CloudRenderer, fancy) and weather (WeatherEffectRenderer).
// Self-contained: the sky passes have their own root signature (see src/sky/sky_gpu.cpp).
cbuffer SkyFrame : register(b0) {
    float4x4 gViewProj;  // camera-relative, same as the terrain's
    float4 gRayX, gRayY, gRayZ;  // view ray for an NDC position: gRayZ + x * gRayX + y * gRayY
    float4 gFogColor;
    float4 gSkyColor;     // w = sky fog end (blocks)
    float4 gDisc;         // x: sky disc, y: dark disc
    float4 gCloudColor;
    float4 gCloudOffset;  // xyz: camera-relative corner of cloud cell (0, 0), w: cloud fog end
    float4 gStarRot[3];   // rows
    float4 gStarBrightness;
};
cbuffer SkyDraw : register(b1) {
    uint gFirst;  // first vertex
    uint gMode;   // 1: rotate by gStarRot
};

struct SkyVertex {
    float3 pos;
    float2 uv;
    uint color;  // 0xAABBGGRR
    uint layer;  // 0 sun, 1..8 moon, 100 end sky, 0xFFFF untextured; weather: 0 rain, 1 snow
    uint pad;
};
StructuredBuffer<SkyVertex> gVerts : register(t0);
StructuredBuffer<uint> gFaces : register(t1);
Texture2DArray gCelestial : register(t2);
Texture2D gEndSky : register(t3);
Texture2D gRain : register(t4);
Texture2D gSnow : register(t5);
Texture2D gForcefield : register(t6);
SamplerState gWrap : register(s0);   // point, wrap
SamplerState gClamp : register(s1);  // point, clamp

static const float kPi = 3.14159265;

float4 unpackColor(uint c) {
    return float4(c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24) / 255.0;
}

// ---- full screen: sky disc and dark disc ------------------------------------------------------

struct ScreenOut {
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};

ScreenOut SkyFullscreenVS(uint vid : SV_VertexID) {
    ScreenOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.ndc = uv * float2(2, -2) + float2(-1, 1);
    o.pos = float4(o.ndc, 0, 1);
    return o;
}

float3 viewRay(float2 ndc) {
    return normalize(gRayZ.xyz + ndc.x * gRayX.xyz + ndc.y * gRayY.xyz);
}

// The game's sky discs: 8-segment fans of radius 512 whose fog distance (16 at the centre, ~512 at the rim)
// is interpolated across each triangle. p: point on the disc plane relative to its centre.
// Returns the fog factor, or -1 outside the disc.
float discFog(float2 p) {
    float r = length(p);
    float theta = atan2(p.y, p.x);
    float seg = kPi / 4;
    float mid = (floor(theta / seg) + 0.5) * seg;
    float rim = 512.0 * cos(seg * 0.5) / cos(theta - mid);
    if (r >= rim) return -1;
    float dist = 16.0 + (512.25 - 16.0) * (r / rim);
    return saturate(dist / max(gSkyColor.w, 1e-3));
}

float4 SkyDiscPS(ScreenOut i) : SV_Target {
    float3 d = viewRay(i.ndc);
    float3 c = gFogColor.rgb;
    if (gDisc.x > 0 && d.y > 1e-5) {
        float f = discFog(d.xz * (16.0 / d.y));
        if (f >= 0) c = lerp(gSkyColor.rgb, gFogColor.rgb, f);
    }
    return float4(c, 1);
}

float4 SkyDarkDiscPS(ScreenOut i) : SV_Target {
    float3 d = viewRay(i.ndc);
    if (d.y > -1e-5) discard;
    float f = discFog(d.xz * (-4.0 / d.y));  // the disc at y = -16 moved up by 12
    if (f < 0) discard;
    return float4(gFogColor.rgb * f, 1);
}

// ---- geometry: sunrise fan, sun, moon, stars, end sky, weather --------------------------------

struct GeoOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation uint layer : LAYER;
};

GeoOut SkyGeoVS(uint vid : SV_VertexID) {
    SkyVertex v = gVerts[gFirst + vid];
    float3 p = v.pos;
    float4 color = unpackColor(v.color);
    if (gMode == 1) {
        p = float3(dot(gStarRot[0].xyz, p), dot(gStarRot[1].xyz, p), dot(gStarRot[2].xyz, p));
        color = gStarBrightness.xxxx;
    }
    GeoOut o;
    o.pos = mul(gViewProj, float4(p, 1));
    o.uv = v.uv;
    o.color = color;
    o.layer = v.layer;
    return o;
}

float4 SkyGeoPS(GeoOut i) : SV_Target {
    float4 c = i.color;
    if (i.layer < 16) {
        float4 t = gCelestial.Sample(gClamp, float3(i.uv, i.layer));
        if (t.a == 0) discard;
        c *= t;
    } else if (i.layer == 100) {
        c *= gEndSky.Sample(gWrap, i.uv);
    }
    if (c.a == 0) discard;
    return c;
}

[earlydepthstencil]  // no depth writes: terrain in front rejects the (large, mostly empty) columns early
float4 SkyWeatherPS(GeoOut i) : SV_Target {
    float4 t = i.layer == 0 ? gRain.SampleLevel(gWrap, i.uv, 0) : gSnow.SampleLevel(gWrap, i.uv, 0);
    float4 c = t * i.color;
    if (c.a < 0.1) discard;
    return c;
}

// World border wall (core/world_border.fsh): the forcefield texture times the status tint and fade.
float4 SkyBorderPS(GeoOut i) : SV_Target {
    float4 t = gForcefield.Sample(gWrap, i.uv);
    if (t.a == 0) discard;
    return t * i.color;
}

// ---- clouds ------------------------------------------------------------------------------------

struct CloudOut {
    float4 pos : SV_Position;
    float4 color : COLOR0;
    float dist : FOG;
};

static const float kCloudShade[6] = {0.7, 1.0, 0.8, 0.8, 0.9, 0.9};  // down up north south west east

CloudOut SkyCloudVS(uint vid : SV_VertexID) {
    static const uint kCorner[6] = {0, 1, 2, 2, 3, 0};
    uint face = gFaces[vid / 6];
    uint corner = kCorner[vid % 6];
    int cx = int(face << 20) >> 20;
    int cz = int(face << 8) >> 20;
    uint flags = face >> 24;
    uint dir = flags & 7;
    bool inside = (flags & 16) != 0;
    uint index = dir * 4 + (inside ? 3 - corner : corner);
    float3 v = float3((0xF03CC3u >> index) & 1, (0x6666F0u >> index) & 1, (0xC3F066u >> index) & 1);
    float3 cell = float3(12, 4, 12);
    float3 p = v * cell + float3(cx, 0, cz) * cell + gCloudOffset.xyz;
    CloudOut o;
    o.pos = mul(gViewProj, float4(p, 1));
    o.dist = length(p);
    o.color = float4(kCloudShade[dir].xxx, 1) * gCloudColor;
    return o;
}

float4 SkyCloudPS(CloudOut i) : SV_Target {
    float4 c = i.color;
    c.a *= 1.0 - saturate(i.dist / max(gCloudOffset.w, 1e-3));
    return c;
}
