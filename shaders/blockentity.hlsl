// Block entities (chests, signs, banners, heads, ...): quads baked per section into one arena, plus per-frame quads
// of animating ones. A quad's section is found by binary search over the frame's visible-section list.
// Self-contained like entity.hlsl: same light curve, two-light entity shading and fog.

cbuffer Frame : register(b0) {
    float4x4 gViewProj;  // camera-relative
    float4 gFogColor;
    float4 gFogParams;   // x = fog start, y = fog end, z = daylight, w = banner wave time (ticks mod 100)
    float4 gLight0;      // world-space diffuse light directions (the game's Lighting.setupLevel)
    float4 gLight1;
};
cbuffer Draw : register(b1) {
    uint gListBase;   // static: first visible-section entry; dynamic: first quad
    uint gListCount;  // static: visible sections in the list
    uint gDynamic;
    uint gPad;
};

struct Quad {
    float3 pos[4];
    float2 uv[4];
    uint normal;  // snorm8 xyz
    uint color;   // 0xAABBGGRR
    uint light;   // sky << 4 | block; 256 full bright, 512 no diffuse shading, 1024 only within 16 blocks
    uint tex;     // 0xFFFFFFFF entity atlas, 0xFFFFFFFE font, 0xFFFFFFFD solid, else block texture layer
    float3 pivot; // banner flag wave pivot
    uint wave;    // 1 << 31 | phase offset << 16 | axis angle (u16 of a turn)
};
struct Batch {
    float3 origin;  // section origin - camera
    uint first;
    uint prefix;
    uint3 pad;
};

StructuredBuffer<Quad> gStatic : register(t0);
StructuredBuffer<Quad> gDyn : register(t1);
StructuredBuffer<Batch> gBatches : register(t2);
Texture2D<float4> gAtlas : register(t3);
Texture2D<float4> gFont : register(t4);
Texture2DArray<float4> gBlocks : register(t5);
SamplerState gPoint : register(s0);
SamplerState gBlockSampler : register(s1);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint tex : TEX;
    float4 tint : COLOR0;
    float3 light : COLOR1;
    float fog : FOG;
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
float3 unpackNormal(uint n) {
    int3 v = int3(int(n << 24) >> 24, int(n << 16) >> 24, int(n << 8) >> 24);
    return float3(v) / 127.0;
}
// Rodrigues rotation about a unit axis.
float3 rotateAxis(float3 v, float3 k, float a) {
    float c = cos(a), s = sin(a);
    return v * c + cross(k, v) * s + k * dot(k, v) * (1.0 - c);
}

VSOut BlockEntityVS(uint vid : SV_VertexID) {
    uint qi = vid / 6;
    uint corner = kCorner[vid % 6];
    Quad q;
    float3 origin = 0;
    if (gDynamic != 0) {
        q = gDyn[gListBase + qi];
    } else {
        uint lo = 0, hi = gListCount - 1;
        while (lo < hi) {
            uint mid = (lo + hi + 1) >> 1;
            if (gBatches[gListBase + mid].prefix <= qi) lo = mid;
            else hi = mid - 1;
        }
        Batch b = gBatches[gListBase + lo];
        q = gStatic[b.first + qi - b.prefix];
        origin = b.origin;
    }
    float3 p = q.pos[corner];
    float3 n = unpackNormal(q.normal);
    if (q.wave != 0) {  // BannerFlagModel: flag.xRot = (-0.0125 + 0.01 cos(2 pi phase)) pi
        float phase = frac((float((q.wave >> 16) & 127) + gFogParams.w) / 100.0);
        float a = (-0.0125 + 0.01 * cos(6.2831853 * phase)) * 3.14159265;
        float ang = float(q.wave & 0xFFFF) / 65536.0 * 6.2831853;
        float3 axis = float3(cos(ang), 0, sin(ang));
        p = q.pivot + rotateAxis(p - q.pivot, axis, a);
        n = rotateAxis(n, axis, a);
    }
    float3 rel = p + origin;
    VSOut o;
    o.pos = mul(gViewProj, float4(rel, 1));
    if (q.light & 1024) {  // glow ink outline: only within 16 blocks (decided per quad: all corners agree)
        float3 c = q.pos[0] + origin;
        if (dot(c, c) > 256.0) o.pos = float4(0, 0, 0, 1);  // degenerate: not rasterized
    }
    o.uv = q.uv[corner];
    o.tex = q.tex;
    o.tint = unpackColor(q.color);
    float3 l = 1.0;
    if (!(q.light & 256)) l = lightColor(q.light);
    if (!(q.light & 512)) {
        n = normalize(n + 1e-6);
        float d = saturate(dot(gLight0.xyz, n)) + saturate(dot(gLight1.xyz, n));
        l *= min(1.0, d * 0.6 + 0.4);
    }
    o.light = l;
    o.fog = saturate((length(rel) - gFogParams.x) / max(gFogParams.y - gFogParams.x, 1e-3));
    return o;
}

float4 shade(VSOut i, float minAlpha) {
    float4 t;
    if (i.tex == 0xFFFFFFFF) t = gAtlas.SampleLevel(gPoint, i.uv, 0);
    else if (i.tex == 0xFFFFFFFE) t = gFont.SampleLevel(gPoint, i.uv, 0);
    else if (i.tex == 0xFFFFFFFD) t = 1;
    else t = gBlocks.Sample(gBlockSampler, float3(i.uv, float(i.tex & 0xFFFF)));
    float4 c = t * i.tint;
    clip(c.a - minAlpha);
    c.rgb *= i.light;
    c.rgb = lerp(c.rgb, gFogColor.rgb, i.fog);
    return c;
}

float4 BlockEntityPS(VSOut i) : SV_Target { return shade(i, 0.1); }
float4 BlockEntityTranslucentPS(VSOut i) : SV_Target { return shade(i, 0.004); }
