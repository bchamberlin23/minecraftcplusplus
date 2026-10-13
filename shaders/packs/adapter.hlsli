// Shader pack adapters (src/render/packs): appended to every translated pack stage (SPIRV-Cross HLSL) together with a
// generated entry point. The vertex adapters decode our geometry - vertex-pulled greedy cube quads (16 bytes), block
// model quads (48 bytes), generic vertices from the extension passes, a full-screen triangle - into the attributes a
// pack's vertex shader expects (gl_Vertex, gl_Normal, gl_Color, gl_MultiTexCoord0/1, mc_Entity, mc_midTexCoord,
// at_tangent, at_midBlock, vaPosition...). Positions are camera-relative world positions: gl_ModelViewMatrix is
// gbufferModelView, so gbufferModelViewInverse * gl_ModelViewMatrix * gl_Vertex is the player-space position packs
// expect. The pack's GL clip position is converted to our conventions here (y flip, reversed-Z depth).
// Layouts must match pack_pipeline.cpp (McppFrameConstants, PackVertex) and the renderer (GpuSection, quads).
#ifndef MCPP_PACK_ADAPTER_HLSLI
#define MCPP_PACK_ADAPTER_HLSLI

cbuffer McppFrame : register(b2) {
    int4 mcppCamBlock;     // floor(camera position)
    float4 mcppCamFrac;    // camera position - mcppCamBlock
    float4 mcppDepth;      // x = near, y = (F - n) / F, z = F / (F - n), w = output height (final pass flip)
    float4 mcppAtlas;      // xy = one tile in atlas UV, z = tiles per row, w = 1 when gl_Color carries AO in alpha
    float4 mcppScreen;     // xy = viewport size, zw = 1 / size
    uint4 mcppMisc;        // x = layer table size, yz = gbuffers jitter (asfloat, GL NDC)
    float4 mcppFade;       // the renderer's section cross-fade: x = clock (s), y = 1 / duration, z = frame index
};

// Sections entering / leaving the LOD cut cross-fade with complementary dither (common.hlsli fadedOut; packs
// accumulate temporally, so the noise moves every frame).
bool mcppFadedOut(float2 pixel, float fade) {
    if (fade == 0 || mcppFade.y == 0) return false;
    float t = saturate((mcppFade.x - abs(fade)) * mcppFade.y);
    uint h = uint(pixel.x) * 1973u + uint(pixel.y) * 9277u + uint(mcppFade.z) * 26699u;
    h = (h ^ 61u) ^ (h >> 16), h *= 9u, h ^= h >> 4, h *= 0x27d4eb2du, h ^= h >> 15;
    float d = float(h & 0xFFFFFFu) / 16777216.0;
    return fade > 0 ? d >= t : d < t;
}
struct McppDrawConstants {
    uint quadOffset;
    uint sectionIndex;
};
ConstantBuffer<McppDrawConstants> mcppDraw : register(b1);

struct McppVertex {
    float4 vertex;     // gl_Vertex (camera-relative world position, w = 1)
    float3 normal;     // gl_Normal (world space)
    float4 color;      // gl_Color
    float4 uv0;        // gl_MultiTexCoord0 (atlas / texture coordinates)
    float4 uv1;        // gl_MultiTexCoord1 (lightmap: block, sky in 0..240)
    float4 uv2;        // gl_MultiTexCoord2 (overlay)
    float4 entity;     // mc_Entity
    float4 midTex;     // mc_midTexCoord
    float4 tangent;    // at_tangent
    float4 midBlock;   // at_midBlock (xyz: block centre - vertex, 1/64 block units; w = light emission)
    float4 tile;       // atlas tile for coordinate wrapping in the fragment shader (z = 0: no wrapping)
    float fade;        // the section's cross-fade (mcppFadedOut), 0 none
    uint vertexId;
};

McppVertex mcppDefaultVertex() {
    McppVertex v;
    v.vertex = float4(0, 0, 0, 1);
    v.normal = float3(0, 1, 0);
    v.color = 1;
    v.uv0 = float4(0, 0, 0, 1);
    v.uv1 = float4(0, 240, 0, 1);
    v.uv2 = float4(0, 10, 0, 1);
    v.entity = float4(-1, 0, 0, 0);
    v.midTex = float4(0, 0, 0, 1);
    v.tangent = float4(1, 0, 0, 1);
    v.midBlock = 0;
    v.tile = 0;
    v.fade = 0;
    v.vertexId = 0;
    return v;
}

// Atlas tile of a texture layer (animated layers already remapped).
float4 mcppTile(uint layer) {
    float col = float(layer % uint(mcppAtlas.z)), row = float(layer / uint(mcppAtlas.z));
    return float4(col * mcppAtlas.x, row * mcppAtlas.y, mcppAtlas.x, mcppAtlas.y);
}

#ifdef MCPP_ADAPTER_TERRAIN
struct McppSection {
    int4 origin;  // xyz = block coordinates of the min corner, w = LOD level
    uint2 ranges[19];
    float fade, pad;  // the renderer's cross-fade (not used by packs)
};
StructuredBuffer<McppSection> mcppSections : register(t1, space1);
StructuredBuffer<uint> mcppColors : register(t2, space1);
StructuredBuffer<uint> mcppRemap : register(t3, space1);
// Per texture layer: x = mc_Entity id (int bits, -1 none), y = light emission, z = flags, w = unused.
StructuredBuffer<uint4> mcppLayers : register(t4, space1);

float3 mcppTint(uint index) {
    uint c = mcppColors[index];
    return float3((c >> 16) & 255, (c >> 8) & 255, c & 255) / 255.0;
}
uint4 mcppLayerInfo(uint layer) {
    return layer < mcppMisc.x ? mcppLayers[layer] : uint4(0xFFFFFFFFu, 0, 0, 0);
}
#endif

#ifdef MCPP_ADAPTER_CUBE
StructuredBuffer<uint4> mcppQuads : register(t0, space1);
static const float3 kMcppNormal[6] = {
    float3(0, -1, 0), float3(0, 1, 0), float3(0, 0, -1), float3(0, 0, 1), float3(-1, 0, 0), float3(1, 0, 0)};

// Fluid quads (geo bit 30; see common.hlsli fluidCorner, duplicated here for the pack compiler).
bool mcppFluidQuad(uint4 q) { return ((q.x >> 30) & 1u) != 0; }
float mcppFluidHeight(uint4 q, uint i) { return float((q.z >> (i * 8)) & 0xFF) / 240.0; }
void mcppFluidCorner(uint4 q, uint face, float a, float b, inout float3 p, out float2 st) {
    float cellY = float((q.x >> 4) & 15);
    if (face == 1) {
        p.y = cellY + mcppFluidHeight(q, uint(a) + 2u * uint(b)) - 0.001;
        uint ang = (q.y >> 12) & 0xFF;
        if (ang == 255u) {
            st = float2(a, b);
        } else {
            float angle = float(ang) / 255.0 * 6.28318530718;
            float s = sin(angle) * 0.25, c = cos(angle) * 0.25;
            st = float2(0.5 + (a > 0.5 ? c : -c) + (b > 0.5 ? s : -s), 0.5 + (b > 0.5 ? c : -c) + (a > 0.5 ? -s : s));
        }
    } else if (face == 0) {
        p.y += 0.001;
        st = float2(a, b);
    } else {
        float h = mcppFluidHeight(q, uint(a));
        if (b > 0.5) p.y = cellY + h;
        if (face == 2) p.z += 0.001; else if (face == 3) p.z -= 0.001; else if (face == 4) p.x += 0.001; else p.x -= 0.001;
        bool flip = face == 3 || face == 4;
        st = float2((flip ? 1.0 - a : a) * 0.5, b > 0.5 ? (1.0 - h) * 0.5 : 0.5);
    }
}

// Section-local position (blocks) and block-unit UV of corner c (same convention as terrain.hlsl pullCube).
void mcppCubeCorner(uint4 q, uint corner, int level, out float3 p, out float2 st) {
    uint face = (q.x >> 20) & 7;
    float a = float(corner & 1), b = float(corner >> 1);
    float w = float(((q.x >> 12) & 15) + 1), h = float(((q.x >> 16) & 15) + 1);
    float3 axisA = face < 4 ? float3(1, 0, 0) : float3(0, 0, 1);
    float3 axisB = face < 2 ? float3(0, 0, 1) : float3(0, 1, 0);
    p = float3(q.x & 15, (q.x >> 4) & 15, (q.x >> 8) & 15);
    p += max(kMcppNormal[face], 0) + axisA * (a * w) + axisB * (b * h);
    if ((q.x >> 29) & 1u) {
        if (face == 1 || (face >= 2 && (corner >> 1) == 1)) p.y -= 0.125;
    }
    p.xz *= float(1u << level);
    float2 uv;
    switch (face) {
        case 0: uv = float2(p.x, -p.z); break;
        case 1: uv = float2(p.x, p.z); break;
        case 2: uv = float2(-p.x, -p.y); break;
        case 3: uv = float2(p.x, -p.y); break;
        case 4: uv = float2(p.z, -p.y); break;
        default: uv = float2(-p.z, -p.y); break;
    }
    uint rot = (q.x >> 23) & 3;
    st = rot == 0 ? uv : rot == 1 ? float2(uv.y, -uv.x) : rot == 2 ? -uv : float2(-uv.y, uv.x);
    if (mcppFluidQuad(q)) mcppFluidCorner(q, face, a, b, p, st);
}

McppVertex mcppPull(uint vid) {
    McppVertex v = mcppDefaultVertex();
    v.vertexId = vid;
    uint4 q = mcppQuads[mcppDraw.quadOffset + (vid >> 2)];
    uint corner = vid & 3;
    uint face = (q.x >> 20) & 7;
    if (((0x19u >> face) & 1u) != 0 && (corner == 1 || corner == 2)) corner ^= 3;
    int4 origin = mcppSections[mcppDraw.sectionIndex].origin;
    v.fade = mcppSections[mcppDraw.sectionIndex].fade;
    float3 p, p1, p2;
    float2 st, st1, st2;
    mcppCubeCorner(q, corner, origin.w, p, st);
    // Two more corners for the UV range and the tangent frame.
    mcppCubeCorner(q, corner ^ 1, origin.w, p1, st1);
    mcppCubeCorner(q, corner ^ 2, origin.w, p2, st2);
    float2 stMin = min(st, min(st1, st2)), stMax = max(st, max(st1, st2));
    float2 base = floor(stMin + 1e-4);
    float2 local = st - base;  // tile units, 0..1 for a single block, up to the merged size otherwise

    float3 rel = float3(origin.xyz - mcppCamBlock.xyz) - mcppCamFrac.xyz + p;
    v.vertex = float4(rel, 1);
    v.normal = kMcppNormal[face];

    uint layer = q.y & 0xFFF;
    uint frame = mcppRemap[layer];
    float4 tile = mcppTile(frame);
    v.tile = tile;
    v.uv0 = float4(tile.xy + local * tile.zw, 0, 1);
    v.midTex = float4(tile.xy + 0.5 * tile.zw, 0, 1);

    uint colorIndex = ((q.y >> 20) << 4) | ((q.x >> 25) & 15);
    float ao = mcppFluidQuad(q) ? 1.0 : 1.0 - 0.2 * float((q.y >> (12 + corner * 2)) & 3);
    float3 tint = mcppTint(colorIndex);
    v.color = mcppAtlas.w > 0.5 ? float4(tint, ao) : float4(tint * ao, 1);
    float sky = float((q.z >> (corner * 8)) & 0xFF), block = float((q.w >> (corner * 8)) & 0xFF);
    if (mcppFluidQuad(q)) sky = float((q.w >> 4) & 15) * 16.0, block = float(q.w & 15) * 16.0;
    v.uv1 = float4(block, sky, 0, 1);

    uint4 info = mcppLayerInfo(layer);
    v.entity = float4(float(asint(info.x)), 0, 0, 0);
    // Tangent: direction of +u in world space (handedness from +v).
    float3 e1 = p1 - p, e2 = p2 - p;
    float2 d1 = st1 - st, d2 = st2 - st;
    float det = d1.x * d2.y - d2.x * d1.y;
    float3 t = (e1 * d2.y - e2 * d1.y) * (det != 0 ? 1.0 / det : 0);
    float3 bt = (e2 * d1.x - e1 * d2.x) * (det != 0 ? 1.0 / det : 0);
    t = normalize(t + 1e-9);
    float hand = dot(cross(v.normal, t), bt) < 0 ? -1 : 1;
    v.tangent = float4(t, hand);
    // at_midBlock: centre of the block this corner belongs to (towards the quad's inside), minus the vertex.
    float3 inward = normalize((p1 + p2) * 0.5 - p + 1e-6) * 0.25;
    float3 cell = floor(p - v.normal * 0.5 + inward) + 0.5;
    v.midBlock = float4((cell - p) * 64.0, float(info.y));
    return v;
}
#endif

#ifdef MCPP_ADAPTER_MODEL
StructuredBuffer<uint> mcppModel : register(t0, space1);
uint mcppRead16(uint base, uint index) {
    uint w = mcppModel[base + (index >> 1)];
    return (index & 1) ? (w >> 16) : (w & 0xFFFF);
}
McppVertex mcppPull(uint vid) {
    McppVertex v = mcppDefaultVertex();
    v.vertexId = vid;
    uint base = (mcppDraw.quadOffset + (vid >> 2)) * 12;
    uint c = vid & 3;
    float3 p = float3(mcppRead16(base, c * 3), mcppRead16(base, c * 3 + 1), mcppRead16(base, c * 3 + 2)) / 2048.0 - 8.0;
    float2 uv = float2(mcppRead16(base, 12 + c * 2), mcppRead16(base, 13 + c * 2)) / 65535.0;
    uint meta = mcppModel[base + 10], lit = mcppModel[base + 11];
    // Other corners: normal and tangent frame.
    uint c1 = c ^ 1, c2 = c ^ 2;
    float3 p1 = float3(mcppRead16(base, c1 * 3), mcppRead16(base, c1 * 3 + 1), mcppRead16(base, c1 * 3 + 2)) / 2048.0 - 8.0;
    float3 p2 = float3(mcppRead16(base, c2 * 3), mcppRead16(base, c2 * 3 + 1), mcppRead16(base, c2 * 3 + 2)) / 2048.0 - 8.0;
    float2 uv1 = float2(mcppRead16(base, 12 + c1 * 2), mcppRead16(base, 13 + c1 * 2)) / 65535.0;
    float2 uv2 = float2(mcppRead16(base, 12 + c2 * 2), mcppRead16(base, 13 + c2 * 2)) / 65535.0;
    int3 origin = mcppSections[mcppDraw.sectionIndex].origin.xyz;
    v.fade = mcppSections[mcppDraw.sectionIndex].fade;
    float3 rel = float3(origin - mcppCamBlock.xyz) - mcppCamFrac.xyz + p;
    v.vertex = float4(rel, 1);
    // Corners are in "Z" order (0,1,2 / 2,1,3) with the renderer's front-facing winding, for which the outward normal
    // is -(p1 - p0) x (p2 - p0) (as for the cube faces); corners 1 and 2 see the edges in the other order.
    float3 n = -cross(p1 - p, p2 - p);
    if (c == 1 || c == 2) n = -n;
    v.normal = normalize(n + 1e-9);

    uint layer = meta & 0xFFFF;
    uint frame = mcppRemap[layer];
    float4 tile = mcppTile(frame);
    v.uv0 = float4(tile.xy + uv * tile.zw, 0, 1);
    v.midTex = float4(tile.xy + 0.5 * tile.zw, 0, 1);
    v.tile = float4(tile.xy, 0, 0);  // model UVs stay inside their tile: no wrapping

    float3 tint = mcppTint(lit >> 8);
    v.color = float4(tint, 1);
    uint light = lit & 0xFF;
    v.uv1 = float4(float((light & 15) << 4), float((light >> 4) << 4), 0, 1);
    uint4 info = mcppLayerInfo(layer);
    v.entity = float4(float(asint(info.x)), 0, 0, 0);
    float2 d1 = uv1 - uv, d2 = uv2 - uv;
    float3 e1 = p1 - p, e2 = p2 - p;
    float det = d1.x * d2.y - d2.x * d1.y;
    float3 t = normalize((e1 * d2.y - e2 * d1.y) * (det != 0 ? 1.0 / det : 0) + 1e-9);
    float3 bt = (e2 * d1.x - e1 * d2.x);
    v.tangent = float4(t, dot(cross(v.normal, t), bt) < 0 ? -1 : 1);
    float3 cell = floor(p + 1e-3 - v.normal * 1e-3) + 0.5;
    v.midBlock = float4((cell - p) * 64.0, float(info.y));
    return v;
}
#endif

#ifdef MCPP_ADAPTER_GENERIC
// Vertices from the extension passes (PackVertex in render/pack_sink.h), camera-relative.
struct McppGenericVertex {
    float3 pos;
    uint color;     // 0xAABBGGRR
    float2 uv;
    uint light;     // block | sky << 16 (0..240)
    uint overlay;   // u | v << 16
    uint normal;    // snorm8 x, y, z
    int entity;     // mc_Entity.x
    float2 midUv;
};
StructuredBuffer<McppGenericVertex> mcppVertices : register(t0, space1);
McppVertex mcppPull(uint vid) {
    McppVertex v = mcppDefaultVertex();
    v.vertexId = vid;
    McppGenericVertex g = mcppVertices[mcppDraw.quadOffset + vid];
    v.vertex = float4(g.pos, 1);
    v.color = float4(g.color & 255, (g.color >> 8) & 255, (g.color >> 16) & 255, g.color >> 24) / 255.0;
    v.uv0 = float4(g.uv, 0, 1);
    v.uv1 = float4(float(g.light & 0xFFFF), float(g.light >> 16), 0, 1);
    v.uv2 = float4(float(g.overlay & 0xFFFF), float(g.overlay >> 16), 0, 1);
    int3 ni = int3(int(g.normal << 24) >> 24, int(g.normal << 16) >> 24, int(g.normal << 8) >> 24);
    v.normal = normalize(float3(ni) / 127.0 + 1e-9);
    v.entity = float4(float(g.entity), 0, 0, 0);
    v.midTex = float4(g.midUv, 0, 1);
    float3 up = abs(v.normal.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    v.tangent = float4(normalize(cross(up, v.normal)), 1);
    return v;
}
#endif

#ifdef MCPP_ADAPTER_FULLSCREEN
McppVertex mcppPull(uint vid) {
    McppVertex v = mcppDefaultVertex();
    v.vertexId = vid;
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));  // (0,0) (2,0) (0,2): covers the unit square
    v.vertex = float4(p, 0, 1);
    v.uv0 = float4(p, 0, 1);
    v.normal = float3(0, 0, 1);
    return v;
}
#endif

// Converts the pack's GL clip position to ours. mode: 0 world (reversed-Z, exact d = near / w), 1 generic GL depth ->
// reversed (hand: keeps the pack's depth range), 2 GL [-1, 1] -> D3D [0, 1] (shadow maps, no reversal), 3 no depth.
// flipY: GL window y up -> render target rows (memory layout matches GL: row 0 = GL bottom row).
float4 mcppClip(float4 c, int mode, bool flipY) {
    if (mode <= 1) c.xy += asfloat(mcppMisc.yz) * c.w;  // temporal upscaling: this frame's jitter (GL NDC; 0 = none)
    if (flipY) c.y = -c.y;
    if (mode == 0) c.z = mcppDepth.x;
    else if (mode == 1) c.z = c.w - (0.5 * c.z + 0.5 * c.w) * mcppDepth.y;
    else if (mode == 2) c.z = 0.5 * (c.z + c.w);
    else c.z = 0;
    return c;
}
// Our depth (SV_Position.z) -> the GL depth gl_FragCoord.z packs expect, and back for gl_FragDepth.
float mcppGlDepth(float d, int mode) {
    if (mode == 2) return d;
    return min((1.0 - d) * mcppDepth.z, 1.0);
}
float mcppOurDepth(float d, int mode) {
    if (mode == 2) return d;
    return 1.0 - d * mcppDepth.y;
}

#endif
