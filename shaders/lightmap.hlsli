#ifndef LIGHTMAP_HLSLI
#define LIGHTMAP_HLSLI
// The game's lightmap (assets/minecraft/shaders/core/lightmap.fsh, 26.3) evaluated directly for fractional
// (smooth-lit) levels 0..15: ambient colour + sky light + tinted block light, then the brightness option.
//   lm[0] = ambient colour (dimension attribute), w = brightness option (0..1)
//   lm[1] = sky light colour, w = sky light factor (day / night, 0 in the Nether and the End)
//   lm[2] = block light tint, w = block factor (1.4 + flicker)
float lightmapCurve(float level) {
    float f = saturate(level / 15.0);
    return f / (4.0 - 3.0 * f);
}
float3 vanillaLight(float skyLevel, float blockLevel, float4 lm[3]) {
    float b = saturate(blockLevel / 15.0);
    float3 c = lm[0].rgb + lm[1].rgb * (lightmapCurve(skyLevel) * lm[1].w);
    float mixF = 0.9 * (2.0 * b - 1.0) * (2.0 * b - 1.0);
    c += lerp(lm[2].rgb, 1.0, mixF) * (lightmapCurve(blockLevel) * lm[2].w);
    c = saturate(c);
    float m = max(max(c.r, c.g), c.b);
    float inv = 1.0 - m;
    float3 notGamma = m > 0 ? c * ((1.0 - inv * inv * inv * inv) / m) : c;
    return lerp(c, notGamma, lm[0].w);
}
#endif
