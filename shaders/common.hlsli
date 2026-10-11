// Shared by all shaders. Layout must match FrameConstants in renderer.cpp.
cbuffer Frame : register(b0) {
    float4x4 gViewProj;  // camera-relative (world origin at the camera)
    float4 gFrustum[5];  // left, right, bottom, top, near (camera-relative)
    int4 gCamBlock;      // floor(camera position)
    float4 gCamFrac;     // camera position - gCamBlock
    float4 gFogColor;
    float4 gFogParams;   // x = fog start, y = fog end, z = max draw distance (0 = unlimited), w = daylight
    float4 gTints[8];
    uint4 gSegBase;      // first indirect command of each pass
    uint4 gSegCap;       // command capacity of each pass
    uint4 gMisc;         // x = active slot count, y = texture LOD bias (float bits)
};

struct DrawConstants {
    uint quadOffset;
    uint sectionIndex;
};
ConstantBuffer<DrawConstants> gDraw : register(b1);

struct GpuSection {
    int4 origin;         // xyz = block coordinates of the min corner, w = LOD level (cell = 2^w blocks)
    uint2 ranges[19];    // (first quad, quad count) per draw range
};
StructuredBuffer<GpuSection> gSections : register(t1);
StructuredBuffer<uint> gActive : register(t3);  // slots selected by the CPU LOD cut

Texture2DArray<float4> gTextures : register(t2);
StructuredBuffer<uint> gColors : register(t4);  // tint colour table (0xRRGGBB), see ColorTable in mesher.h
StructuredBuffer<uint> gTextureRemap : register(t5);  // texture layer -> current animation frame's layer
float3 tintColor(uint index) {
    uint c = gColors[index];
    return float3((c >> 16) & 255, (c >> 8) & 255, c & 255) / 255.0;
}
SamplerState gSampler : register(s0);
// Block textures with the texture LOD bias of the active upscaler (gMisc.y, float bits; 0 at native resolution).
float4 sampleBlock(float3 uvl) { return gTextures.SampleBias(gSampler, uvl, asfloat(gMisc.y)); }

struct VSOut {
    float4 pos : SV_Position;
    float3 uvl : TEXCOORD0;  // uv + texture layer
    float3 color : COLOR0;   // tint * directional shade
    float fog : FOG;
};

// Light levels 0..15 -> brightness, following the shape of the game's lightmap
// (hyperbolic falloff, warm block light, default 50% brightness lift).
float lightCurve(float level) {
    float f = level / 15.0;
    return f / (4.0 - 3.0 * f);
}
float3 lightColorLevels(float skyLevel, float blockLevel) {  // fractional levels 0..15
    float sky = lightCurve(skyLevel) * gFogParams.w;
    float block = lightCurve(blockLevel);
    float3 c = saturate(max(float3(sky, sky, sky), block * float3(1.0, 0.9, 0.75)));
    float3 lifted = 1.0 - pow(saturate(1.0 - c), 4.0);  // pow of a negative base is NaN
    return lerp(lerp(c, lifted, 0.5), 0.75, 0.04);
}
float3 lightColor(uint packed) { return lightColorLevels(float(packed >> 4), float(packed & 15)); }

float fogFactor(float3 rel) {
    return saturate((length(rel) - gFogParams.x) / max(gFogParams.y - gFogParams.x, 1e-3));
}

float4 shadePixel(VSOut i) {
    float4 c = sampleBlock(i.uvl);
    c.rgb = lerp(c.rgb * i.color, gFogColor.rgb, i.fog);
    return c;
}
