// Entities: model cubes (vertex-pulled from a static quad buffer, posed by per-part matrices) and
// free quads (items, falling blocks, sprites) built on the CPU each frame.
// Self-contained: the light curve and fog below follow common.hlsli.

cbuffer Frame : register(b0) {
    float4x4 gViewProj;  // camera-relative
    float4 gFogColor;
    float4 gFogParams;   // x = fog start, y = fog end, z = daylight, w = 1: shader pipeline on (see below)
    float4 gLight0;      // world-space diffuse light directions (the game's Lighting.setupLevel)
    float4 gLight1;
    float4 gGlintRect;   // atlas rect of misc/enchanted_glint_armor (u0 v0 du dv)
    float4 gGlint;       // x, y: scroll offset; z: strength
};
cbuffer Draw : register(b1) {
    uint gRefBase;
    uint gPad;
};

struct ModelQuad {
    float3 pos[4];  // model pixels, part space
    float2 uv[4];   // 0..1 in the entity texture
    float3 normal;
    uint part;
};
struct PartMatrix {
    float4 r0, r1, r2;  // 3x4 affine: part space (blocks) -> camera-relative world
};
struct Instance {
    uint matrixBase;
    uint color;    // 0xAABBGGRR tint
    uint light;    // sky << 4 | block; bit 8: full bright (eyes); bit 9: no diffuse shading
    uint overlay;  // red (hurt) amount in bits 0..7, white flash amount in bits 8..15
    float4 uvRect; // atlas offset (xy) and size (zw)
};
struct FreeQuad {
    float3 pos[4];  // camera-relative world
    float2 uv[4];
    float3 normal;
    uint tex;       // block texture array layer, or 0xFFFFFFFF: uv is in the entity atlas
    uint color;
    uint light;
    uint overlay;
    uint pad;
};

StructuredBuffer<ModelQuad> gQuads : register(t0);
StructuredBuffer<PartMatrix> gMatrices : register(t1);
StructuredBuffer<Instance> gInstances : register(t2);
StructuredBuffer<uint2> gRefs : register(t3);       // (quad, instance)
StructuredBuffer<FreeQuad> gFree : register(t4);
Texture2D<float4> gAtlas : register(t5);
Texture2DArray<float4> gBlocks : register(t6);
SamplerState gPoint : register(s0);
SamplerState gBlockSampler : register(s1);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint layer : LAYER;
    float4 tint : COLOR0;
    float3 light : COLOR1;     // light map * directional shading
    float4 overlay : COLOR2;   // rgb overlay colour, a = amount (hurt red / creeper white flash)
    float fog : FOG;
    float shade : SHADE;       // directional shading alone (shader pipeline)
    nointerpolation uint lightBits : LIGHTBITS;
    float2 rawUv : TEXCOORD1;  // texture coordinates within the entity texture (glint)
};

static const uint kCorner[6] = {0, 1, 2, 0, 2, 3};

float lightCurve(float level) {
    float f = level / 15.0;
    return f / (4.0 - 3.0 * f);
}
float3 lightColor(uint packed) {
    float sky = lightCurve(float((packed >> 4) & 15)) * gFogParams.z;
    float block = lightCurve(float(packed & 15));
    float3 c = saturate(max(float3(sky, sky, sky), block * float3(1.0, 0.9, 0.75)));
    float3 lifted = 1.0 - pow(saturate(1.0 - c), 4.0);
    return lerp(lerp(c, lifted, 0.5), 0.75, 0.04);
}

float4 unpackColor(uint c) {
    return float4(c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24) / 255.0;
}

VSOut finish(float3 rel, float3 n, float2 uv, uint layer, uint color, uint light, uint overlay) {
    VSOut o;
    o.pos = mul(gViewProj, float4(rel, 1));
    o.uv = uv;
    o.layer = layer;
    o.tint = unpackColor(color);
    float3 l = 1.0;
    o.shade = 1.0;
    if (!(light & 256)) {
        l = lightColor(light);
        if (!(light & 512)) {  // the game's two-light entity shading
            float d = saturate(dot(gLight0.xyz, n)) + saturate(dot(gLight1.xyz, n));
            o.shade = min(1.0, d * 0.6 + 0.4);
            l *= o.shade;
        }
    }
    o.light = l;
    o.lightBits = light;
    float red = float(overlay & 255) / 255.0, white = float((overlay >> 8) & 255) / 255.0;
    o.overlay = white > 0 ? float4(1, 1, 1, white) : float4(1, 0, 0, red);
    o.fog = saturate((length(rel) - gFogParams.x) / max(gFogParams.y - gFogParams.x, 1e-3));
    o.rawUv = 0;
    return o;
}

VSOut EntityModelVS(uint vid : SV_VertexID) {
    uint2 ref = gRefs[gRefBase + vid / 6];
    uint corner = kCorner[vid % 6];
    ModelQuad q = gQuads[ref.x];
    Instance inst = gInstances[ref.y];
    PartMatrix m = gMatrices[inst.matrixBase + q.part];
    float4 p = float4(q.pos[corner] / 16.0, 1);
    float3 rel = float3(dot(m.r0, p), dot(m.r1, p), dot(m.r2, p));
    float3 n = normalize(float3(dot(m.r0.xyz, q.normal), dot(m.r1.xyz, q.normal), dot(m.r2.xyz, q.normal)) + 1e-6);
    float2 uv = inst.uvRect.xy + q.uv[corner] * inst.uvRect.zw;
    VSOut o = finish(rel, n, uv, 0xFFFFFFFF, inst.color, inst.light, inst.overlay);
    o.rawUv = q.uv[corner];
    return o;
}

VSOut EntityFreeVS(uint vid : SV_VertexID) {
    FreeQuad q = gFree[gRefBase + vid / 6];
    uint corner = kCorner[vid % 6];
    return finish(q.pos[corner], q.normal, q.uv[corner], q.tex, q.color, q.light, q.overlay);
}

float4 shade(VSOut i, float minAlpha) {
    float4 t = i.layer == 0xFFFFFFFF ? gAtlas.SampleLevel(gPoint, i.uv, 0)
                                     : gBlocks.Sample(gBlockSampler, float3(i.uv, float(i.layer & 0xFFFF)));
    float4 c = t * i.tint;
    clip(c.a - minAlpha);
    c.rgb = lerp(c.rgb, i.overlay.rgb, i.overlay.a);  // OverlayTexture: mix(overlay, colour, overlay alpha)
    c.rgb *= i.light;
    c.rgb = lerp(c.rgb, gFogColor.rgb, i.fog);
    return c;
}

// Shader pipeline on (gFogParams.w = 1): the World stage draws into an LDR layer the pipeline relights
// (shaders/deferred.hlsl). Opaque pixels: albedo * directional shade, alpha = (128 + code) / 255 with
// code = sky << 3 | block >> 1 (sky 15: block code at most 5), 126 = full bright. Translucent pixels: the
// vanilla-lit colour with alpha * 0.49 (coverage below one half marks them). No fog (the pipeline applies it).
float4 shadePipeline(VSOut i, float minAlpha, bool translucent) {
    float4 t = i.layer == 0xFFFFFFFF ? gAtlas.SampleLevel(gPoint, i.uv, 0)
                                     : gBlocks.Sample(gBlockSampler, float3(i.uv, float(i.layer & 0xFFFF)));
    float4 c = t * i.tint;
    clip(c.a - minAlpha);
    c.rgb = lerp(c.rgb, i.overlay.rgb, i.overlay.a);
    bool bright = (i.lightBits & 256) != 0;
    if (translucent) return bright ? c : float4(c.rgb * i.light, c.a * 0.49);
    uint sky = (i.lightBits >> 4) & 15, block = (i.lightBits & 15) >> 1;
    uint code = bright ? 126 : (sky << 3) | (sky == 15 ? min(block, 5u) : block);
    return float4(c.rgb * i.shade, float(128 + code) / 255.0);
}

float4 EntityCutoutPS(VSOut i) : SV_Target { return gFogParams.w > 0.5 ? shadePipeline(i, 0.1, false) : shade(i, 0.1); }
float4 EntityTranslucentPS(VSOut i) : SV_Target {
    return gFogParams.w > 0.5 ? shadePipeline(i, 0.004, true) : shade(i, 0.004);
}

// Armour enchantment glint (RenderTypes.armorCutoutNoCullGlint): misc/enchanted_glint_armor scrolled over the
// piece's texture coordinates (TextureTransform.setupGlintTexturing, scale 0.16), added as src * src + dst.
float4 EntityGlintPS(VSOut i) : SV_Target {
    float4 base = gAtlas.SampleLevel(gPoint, i.uv, 0);
    clip(base.a * i.tint.a - 0.1);
    const float c = 0.98480775, s = 0.17364818;
    float2 g = i.rawUv * 0.16;
    g = float2(g.x * c - g.y * s, g.x * s + g.y * c) + gGlint.xy;
    float3 glint = gAtlas.SampleLevel(gPoint, gGlintRect.xy + frac(g) * gGlintRect.zw, 0).rgb * gGlint.z;
    return float4(glint * (1.0 - i.fog), 1);
}
