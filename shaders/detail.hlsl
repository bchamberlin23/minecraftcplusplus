// Non-cube block model quads (plants, stairs, fences...): 48 bytes per quad, vertex pulled.
// DetailVS / DetailPS: vanilla look. DetailGbuf* / DetailShadow*: the shader pipeline (see terrain.hlsl).
#include "common.hlsli"
#include "shading.hlsli"

StructuredBuffer<uint> gModel : register(t0);

uint read16(uint base, uint index) {
    uint w = gModel[base + (index >> 1)];
    return (index & 1) ? (w >> 16) : (w & 0xFFFF);
}

struct ModelVertex {
    float3 rel;
    float2 uv;
    uint meta, lit;
};

ModelVertex pullModel(uint vid) {
    ModelVertex v;
    uint base = (gDraw.quadOffset + (vid >> 2)) * 12;
    uint c = vid & 3;
    float3 p = float3(read16(base, c * 3), read16(base, c * 3 + 1), read16(base, c * 3 + 2)) / 2048.0 - 8.0;
    v.uv = float2(read16(base, 12 + c * 2), read16(base, 13 + c * 2)) / 65535.0;
    v.meta = gModel[base + 10];
    v.lit = gModel[base + 11];
    int3 origin = gSections[gDraw.sectionIndex].origin.xyz;
    v.rel = float3(origin - gCamBlock.xyz) - gCamFrac.xyz + p;
    return v;
}

VSOut DetailVS(uint vid : SV_VertexID) {
    ModelVertex v = pullModel(vid);
    VSOut o;
    o.pos = mul(gViewProj, float4(v.rel, 1));
    o.uvl = float3(v.uv, float(gTextureRemap[v.meta & 0xFFFF]));
    o.color = tintColor(v.lit >> 8) * (float((v.meta >> 19) & 255) / 255.0) * lightColor(v.lit & 0xFF);
    o.fog = fogFactor(v.rel);
    return o;
}

float4 DetailPS(VSOut i) : SV_Target {
    float4 c = shadePixel(i);
    clip(c.a - 0.5);
    return float4(c.rgb, 1);
}

// ---- shader pipeline -------------------------------------------------------------------------------------

float3 modelWave(ModelVertex v, uint m) {
    if ((gFlags.x & kFlagWaving) == 0) return 0;
    uint mode = matWave(m);
    if (mode == 0) return 0;
    float weight = mode == 1 ? 1.0 - v.uv.y : mode == 2 ? 2.0 - v.uv.y : 0.6;
    float3 w = v.rel + float3(gJitter.z, gTime.w, gJitter.w);
    float t = gTime.x;
    float gust = 0.6 + 0.4 * sin(t * 0.35 + w.x * 0.02 + w.z * 0.015);
    float a = sin(t * 1.9 + dot(w.xz, float2(0.62, 0.79)) * 1.2);
    float b = sin(t * 2.7 + dot(w.xz, float2(-0.71, 0.48)) * 1.9) * 0.5;
    float c = cos(t * 1.4 + dot(w.xz, float2(0.83, -0.31)) * 1.1);
    float amp = 0.07 * gust * weight * (1.0 + gTime.y * 1.5);
    return float3(a + b, 0, c + b) * amp;
}

struct DetailGbufVSOut {
    float4 pos : SV_Position;
    float3 uvl : TEXCOORD0;
    float3 tint : COLOR0;
    float3 light : TEXCOORD1;  // sky, block, ao (1)
    float3 rel : TEXCOORD2;
    nointerpolation uint mat : MATERIAL;
};

DetailGbufVSOut DetailGbufVS(uint vid : SV_VertexID) {
    ModelVertex v = pullModel(vid);
    uint layer = v.meta & 0xFFFF;
    uint m = gMaterial[layer];
    float3 rel = v.rel + modelWave(v, m);
    DetailGbufVSOut o;
    o.pos = mul(gViewProj, float4(rel, 1));
    o.uvl = float3(v.uv, float(gTextureRemap[layer]));
    o.tint = tintColor(v.lit >> 8);
    o.light = float3(float((v.lit >> 4) & 15), float(v.lit & 15), 1);
    o.rel = rel;
    o.mat = matClass(m);
    return o;
}

struct GbufTargets {
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
};

GbufTargets DetailGbufPS(DetailGbufVSOut i) {
    float4 c = sampleBlock(i.uvl);
    clip(c.a - 0.5);
    float3 n = normalize(cross(ddy(i.rel), ddx(i.rel)));
    if (dot(n, i.rel) > 0) n = -n;
    uint mat = i.mat > 7 ? 0 : i.mat;
    if (mat == kMatPlant) n = float3(0, 1, 0);
    GbufTargets o;
    o.albedo = float4(c.rgb * i.tint, float((31u << 3) | mat) / 255.0);  // sRGB values (decoded by the deferred pass)
    o.normal = float4(octEncode(n), i.light.x / 15.0, i.light.y / 15.0);
    return o;
}

struct DetailShadowVSOut {
    float4 pos : SV_Position;
    float3 uvl : TEXCOORD0;
};

DetailShadowVSOut DetailShadowVS(uint vid : SV_VertexID) {
    ModelVertex v = pullModel(vid);
    uint layer = v.meta & 0xFFFF;
    DetailShadowVSOut o;
    o.pos = mul(gShadowMat[gPassA.x], float4(v.rel + modelWave(v, gMaterial[layer]), 1));
    o.uvl = float3(v.uv, float(gTextureRemap[layer]));
    return o;
}

void DetailShadowPS(DetailShadowVSOut i) {
    clip(gTextures.Sample(gSampler, i.uvl).a - 0.5);
}
