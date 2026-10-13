#ifndef MCPP_FOG_HLSLI
#define MCPP_FOG_HLSLI
// fog.glsl total_fog_value: the larger of the environmental fog over the spherical distance and the render distance
// fog over the cylindrical one (max of horizontal distance and height).
float vanillaFog(float3 rel, float2 environmental, float2 renderDistance) {
    float spherical = length(rel), cylindrical = max(length(rel.xz), abs(rel.y));
    float env = saturate((spherical - environmental.x) / max(environmental.y - environmental.x, 1e-3));
    float rd = saturate((cylindrical - renderDistance.x) / max(renderDistance.y - renderDistance.x, 1e-3));
    return max(env, rd);
}
#endif
