// Inputs prepared for the vendor upscalers (render/upscalers/upscalers.cpp). Own root signature:
// t0 = the World stage's LDR layer (alpha > 0 where entities / block entities drew), u0 = R8 reactive mask, b0 = consts.
Texture2D<float4> gLayer : register(t0);
RWTexture2D<float> gReactive : register(u0);
cbuffer Consts : register(b0) {
    uint2 gSize;
    float gScale;  // reactive value for fully covered pixels (FSR: <= 0.9, XeSS clamps at 0.8)
};

// Entities have no motion vectors (camera motion only): mark them so the upscaler trusts their history less.
[numthreads(8, 8, 1)]
void ReactiveCS(uint2 id : SV_DispatchThreadID) {
    if (any(id >= gSize)) return;
    gReactive[id] = saturate(gLayer[id].a) * gScale;
}
