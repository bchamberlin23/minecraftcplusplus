// The spectated entities' post effects (post_effect/creeper.json, spider.json, invert.json): the game's post shaders
// with its texture coordinates (y up), sampled in D3D's (y down).
cbuffer Post : register(b0) {
    float2 gInSize;   // input texels
    float2 gUvScale;  // used size / texture size (the targets only grow)
    float4 gP0, gP1, gP2, gP3;  // the pass's uniforms
};
Texture2D<float4> gIn : register(t0);
Texture2D<float4> gBlur : register(t1);
SamplerState gPoint : register(s0);
SamplerState gLinear : register(s1);

struct PostV {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;      // texCoord
    float2 scaled : TEXCOORD1;  // post/rotscale: scaledCoord
};
float4 tap(SamplerState s, Texture2D<float4> t, float2 uv) { return t.SampleLevel(s, float2(uv.x, 1.0 - uv.y) * gUvScale, 0); }

PostV PostVS(uint vid : SV_VertexID) {  // core/screenquad
    PostV o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * 2.0 - 1.0, 0, 1);
    o.scaled = 0;
    return o;
}
PostV RotScaleVS(uint vid : SV_VertexID) {  // post/rotscale: InScale gP0.xy, InOffset gP0.zw, InRotation gP1.x (degrees)
    PostV o = PostVS(vid);
    const float r = gP1.x * 0.0174532925, c = cos(r), s = sin(r);
    o.scaled = float2(o.uv.x * c - o.uv.y * s, o.uv.y * c + o.uv.x * s) * gP0.xy + gP0.zw;
    return o;
}

float4 ColorConvolvePS(PostV i) : SV_Target {  // RedMatrix gP0, GreenMatrix gP1, BlueMatrix gP2; saturation 1.8
    float3 t = tap(gPoint, gIn, i.uv).rgb;
    float3 c = float3(dot(t, gP0.xyz), dot(t, gP1.xyz), dot(t, gP2.xyz));
    const float luma = dot(c, float3(0.3, 0.59, 0.11));
    return float4((c - luma) * 1.8 + luma, 1);
}

float4 BitsPS(PostV i) : SV_Target {  // Resolution gP0.x, MosaicSize gP0.y
    const float2 mosaicInSize = gInSize / gP0.y;
    const float2 fractPix = frac(i.uv * mosaicInSize) / mosaicInSize;
    float4 base = tap(gPoint, gIn, i.uv - fractPix);
    const float3 f = base.rgb - frac(base.rgb * gP0.x) / gP0.x;
    const float luma = dot(f, float3(0.3, 0.59, 0.11));
    return float4(luma + (f - luma) * 1.5, 1);
}

float4 BoxBlurPS(PostV i) : SV_Target {  // BlurDir gP0.xy, Radius gP0.z (bilinear, two texels a tap)
    const float2 step = gP0.xy / gInSize;
    const float radius = round(gP0.z);
    float4 blurred = 0;
    for (float a = -radius + 0.5; a <= radius; a += 2.0) blurred += tap(gLinear, gIn, i.uv + step * a);
    blurred += tap(gLinear, gIn, i.uv + step * radius) / 2.0;
    return blurred / (radius + 0.5);
}

float4 SpiderClipPS(PostV i) : SV_Target {  // Scissor gP2, Vignette gP3
    const float2 sc = i.scaled;
    const float4 blur = tap(gPoint, gBlur, i.uv);
    float4 o = tap(gPoint, gIn, sc);
    const float4 scissor = gP2, vignette = gP3;
    if (sc.x < scissor.x) o = blur;
    if (sc.y < scissor.y) o = blur;
    if (sc.x > scissor.z) o = blur;
    if (sc.y > scissor.w) o = blur;
    if (sc.x < vignette.x) o = lerp(blur, o, (scissor.x - sc.x) / (scissor.x - vignette.x));
    if (sc.y < vignette.y) o = lerp(blur, o, (scissor.y - sc.y) / (scissor.y - vignette.y));
    if (sc.x > vignette.z) o = lerp(blur, o, (scissor.z - sc.x) / (scissor.z - vignette.z));
    if (sc.y > vignette.w) o = lerp(blur, o, (scissor.w - sc.y) / (scissor.w - vignette.w));
    return float4(o.rgb, 1);
}

float4 InvertPS(PostV i) : SV_Target {  // InverseAmount gP0.x
    const float4 d = tap(gPoint, gIn, i.uv);
    return float4(lerp(d, 1.0 - d, gP0.x).rgb, 1);
}

float4 PostBlitPS(PostV i) : SV_Target {  // ColorModulate gP0 (opaque: it replaces the scene)
    return float4((tap(gPoint, gIn, i.uv) * gP0).rgb, 1);
}
