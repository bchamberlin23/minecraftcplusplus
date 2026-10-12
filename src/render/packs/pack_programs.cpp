// Shader pack programs: resource names -> pipeline resources, generated entry points around the translated stages,
// DXIL variants per geometry adapter, sampler tables and pipeline state objects.
#include "render/packs/pack_programs.h"

#include <algorithm>
#include <atomic>
#include <thread>

#include "pack_adapter_hlsli.h"

namespace packs {

namespace {

// HLSL type string -> (scalar base, components); "float3x3" -> components 0 (matrix).
void parseHlslType(const std::string& t, std::string& base, int& comps) {
    size_t i = 0;
    while (i < t.size() && std::isalpha(u8(t[i]))) ++i;
    base = t.substr(0, i);
    if (base == "min16float" || base == "half") base = "float";
    if (i == t.size()) comps = 1;
    else if (t.find('x', i) != std::string::npos) comps = 0;
    else comps = t[i] - '0';
}

// float4 expression -> value of HLSL type `t`.
std::string convertFrom4(const std::string& src4, const std::string& t) {
    std::string base;
    int comps;
    parseHlslType(t, base, comps);
    if (comps == 0) return "(" + t + ")0";
    static const char* kSw[] = {"", ".x", ".xy", ".xyz", ".xyzw"};
    return t + "((" + src4 + ")" + kSw[std::clamp(comps, 1, 4)] + ")";
}

const char* stageGroup(const std::string& program) {
    static const char* kGroups[] = {"gbuffers", "shadowcomp", "shadow", "deferred", "composite", "prepare", "begin", "setup"};
    if (program == "final") return "composite";
    for (const char* g : kGroups)
        if (program.compare(0, std::strlen(g), g) == 0) return g;
    return "";
}

bool hasTileInput(const StageReflection& fs) {
    for (const StageVar& v : fs.inputs)
        if (v.glslName == "mcpp_tile") return true;
    return false;
}

std::string varyingsStruct(const StageReflection& fs) {
    std::string s = "struct McppVaryings {\n    float4 pos : SV_Position;\n";
    for (size_t i = 0; i < fs.inputs.size(); ++i) {
        const StageVar& v = fs.inputs[i];
        if (v.glslName == "mcpp_tile") continue;
        std::string mod = v.flat ? "nointerpolation " : v.noperspective ? "noperspective " : v.centroid ? "centroid " : "";
        s += "    " + mod + v.hlslType + " v" + std::to_string(i) +
             (v.arraySize ? "[" + std::to_string(v.arraySize) + "]" : "") + " : MCPPV" + std::to_string(i) + "X;\n";
    }
    if (hasTileInput(fs)) s += "    nointerpolation float4 tile : MCPPTILE;\n";
    s += "    nointerpolation float fade : MCPPFADE;\n";
    s += "};\n";
    return s;
}

}  // namespace

ResRef resolveTexture(const PackDefinition& def, const std::string& program, const std::string& name,
                      const std::vector<TextureBinding>& all, std::string* warning) {
    // Stage-specific custom textures replace anything (texture.<stage>.<sampler>).
    const char* group = stageGroup(program);
    for (size_t i = 0; i < def.textures.size(); ++i) {
        const CustomTexture& c = def.textures[i];
        if (!c.stage.empty() && c.stage == group && c.sampler == name) return {Res::Custom, u16(i)};
    }
    static const std::pair<const char*, int> kAliases[] = {{"gcolor", 0}, {"gdepth", 1}, {"gnormal", 2}, {"composite", 3},
                                                           {"gaux1", 4},  {"gaux2", 5},  {"gaux3", 6},   {"gaux4", 7}};
    for (const auto& [a, i] : kAliases)
        if (name == a) return {Res::Color, u16(i)};
    if (name.compare(0, 8, "colortex") == 0) {
        int i = std::atoi(name.c_str() + 8);
        if (name.size() > 8 && i >= 0 && i < 16) return {Res::Color, u16(i)};
    }
    if (name == "depthtex0" || name == "gdepthtex") return {Res::Depth0, 0};
    if (name == "depthtex1") return {Res::Depth1, 0};
    if (name == "depthtex2") return {Res::Depth2, 0};
    bool watershadow = false;
    for (const TextureBinding& t : all)
        if (t.name == "watershadow") watershadow = true;
    if (name == "shadowtex0" || name == "watershadow") return {Res::Shadow0, 0};
    if (name == "shadow") return {watershadow ? Res::Shadow1 : Res::Shadow0, 0};
    if (name == "shadowtex1") return {Res::Shadow1, 0};
    if (name == "shadowcolor" || name == "shadowcolor0") return {Res::ShadowColor, 0};
    if (name == "shadowcolor1") return {Res::ShadowColor, 1};
    if (name == "noisetex") {
        for (size_t i = 0; i < def.textures.size(); ++i)
            if (def.textures[i].sampler == "noisetex" && def.textures[i].stage.empty()) return {Res::Custom, u16(i)};
        return {Res::Noise, 0};
    }
    if (name == "lightmap") return {Res::Lightmap, 0};
    if (name == "normals") return {Res::Normals, 0};
    if (name == "specular") return {Res::Specular, 0};
    for (size_t i = 0; i < def.images.size(); ++i)
        if (def.images[i].sampler == name) return {Res::Image, u16(i)};
    for (size_t i = 0; i < def.textures.size(); ++i)
        if (def.textures[i].sampler == name && def.textures[i].stage.empty()) return {Res::Custom, u16(i)};
    if (name.compare(0, 10, "dhDepthTex") == 0 || name.compare(0, 10, "vxDepthTex") == 0) return {Res::DepthFar, 0};
    if (warning) *warning = program + ": unknown sampler '" + name + "' (bound to a white texture)";
    return {Res::White, 0};
}

ResRef resolveImage(const PackDefinition& def, const std::string& name) {
    if (name.compare(0, 8, "colorimg") == 0) return {Res::Color, u16(std::atoi(name.c_str() + 8) & 15)};
    if (name.compare(0, 14, "shadowcolorimg") == 0) return {Res::ShadowColor, u16(std::atoi(name.c_str() + 14) & 1)};
    for (size_t i = 0; i < def.images.size(); ++i)
        if (def.images[i].name == name) return {Res::Image, u16(i)};
    return {Res::None, 0};
}

Samp samplerFor(const TextureBinding& t, const ResRef& ref, const PackDefinition& def, const Program* prog) {
    if (t.sampled != BaseType::Float) return Samp::PointClamp;
    switch (ref.kind) {
        case Res::Color:
            if (prog && prog->mipmapped.count(ref.index)) return Samp::LinearMipClamp;
            return Samp::LinearClamp;
        case Res::Depth0: case Res::Depth1: case Res::Depth2: case Res::DepthFar: return Samp::PointClamp;
        case Res::Shadow0: case Res::Shadow1: {
            const int i = ref.kind == Res::Shadow0 ? 0 : 1;
            if (t.shadow) return def.settings.shadow.hardwareFiltering[i] ? Samp::ShadowCmpLinear : Samp::ShadowCmpPoint;
            return def.settings.shadow.nearest[i] ? Samp::PointClamp : Samp::LinearClamp;
        }
        case Res::ShadowColor: {
            const int i = std::min<int>(ref.index, 1);
            if (def.settings.shadow.colorMipmap[i]) return Samp::LinearMipClamp;
            return def.settings.shadow.colorNearest[i] ? Samp::PointClamp : Samp::LinearClamp;
        }
        case Res::Noise: return Samp::LinearWrap;
        case Res::Lightmap: return Samp::LinearClamp;
        case Res::Normals: case Res::Specular: case Res::Atlas: return Samp::AtlasWrap;
        case Res::Custom: {
            const CustomTexture& c = def.textures[ref.index];
            if (c.path.find("atlas/blocks") != std::string::npos) return Samp::AtlasWrap;
            if (c.blur) return c.clamp ? Samp::LinearClamp : Samp::LinearWrap;
            return c.clamp ? Samp::PointClamp : Samp::LinearWrap;
        }
        case Res::Image: return Samp::LinearClamp;
        default: return Samp::PointClamp;
    }
}

std::string vertexWrapper(const StageReflection& vs, const StageReflection& fs, int depthMode, bool flipY) {
    std::string s = varyingsStruct(fs);
    s += "McppVaryings mcpp_vs(uint mcppVid : SV_VertexID) {\n    McppVertex v = mcppPull(mcppVid);\n";
    for (const StageVar& in : vs.inputs) {
        const std::string& n = in.glslName;
        std::string src;
        if (n == "mcpp_Vertex" || n == "vaPosition") src = "v.vertex";
        else if (n == "mcpp_Normal" || n == "vaNormal") src = "float4(v.normal, 0)";
        else if (n == "mcpp_Color" || n == "vaColor") src = "v.color";
        else if (n == "mcpp_MultiTexCoord0" || n == "vaUV0") src = "v.uv0";
        else if (n == "mcpp_MultiTexCoord1" || n == "vaUV2") src = "v.uv1";
        else if (n == "mcpp_MultiTexCoord2" || n == "vaUV1") src = "v.uv2";
        else if (n == "mc_Entity") src = "v.entity";
        else if (n == "mc_midTexCoord") src = "v.midTex";
        else if (n == "at_tangent") src = "v.tangent";
        else if (n == "at_midBlock") src = "v.midBlock";
        else if (n == "mc_chunkFade") src = "float4(1, 1, 1, 1)";
        else if (n == "gl_VertexIndex" || n == "gl_VertexID") src = "float4(v.vertexId, 0, 0, 0)";
        else src = "float4(0, 0, 0, 0)";
        if (in.arraySize) continue;
        s += "    " + in.hlslName + " = " + convertFrom4(src, in.hlslType) + ";\n";
    }
    if (vs.usesFragCoord) {}  // (vertex shaders have no gl_FragCoord)
    s += "    mcpp_spirv_entry_inner();\n    McppVaryings o;\n";
    s += "    o.pos = mcppClip(gl_Position, " + std::to_string(depthMode) + ", " + (flipY ? "true" : "false") + ");\n";
    for (size_t i = 0; i < fs.inputs.size(); ++i) {
        const StageVar& f = fs.inputs[i];
        if (f.glslName == "mcpp_tile") continue;
        const StageVar* src = nullptr;
        for (const StageVar& out : vs.outputs)
            if (out.glslName == f.glslName) src = &out;
        const std::string dst = "o.v" + std::to_string(i);
        if (!src) {
            if (f.arraySize)
                for (u32 k = 0; k < f.arraySize; ++k) s += "    " + dst + "[" + std::to_string(k) + "] = (" + f.hlslType + ")0;\n";
            else s += "    " + dst + " = (" + f.hlslType + ")0;\n";
            continue;
        }
        if (f.arraySize || src->arraySize) {
            for (u32 k = 0; k < std::max(f.arraySize, 1u); ++k) {
                std::string d = f.arraySize ? dst + "[" + std::to_string(k) + "]" : dst;
                if (k < std::max(src->arraySize, 1u))
                    s += "    " + d + " = (" + f.hlslType + ")" + src->hlslName +
                         (src->arraySize ? "[" + std::to_string(k) + "]" : "") + ";\n";
                else s += "    " + d + " = (" + f.hlslType + ")0;\n";
            }
            continue;
        }
        if (src->hlslType == f.hlslType) s += "    " + dst + " = " + src->hlslName + ";\n";
        else s += "    " + dst + " = (" + f.hlslType + ")" + src->hlslName + ";\n";
    }
    if (hasTileInput(fs)) s += "    o.tile = v.tile;\n";
    s += "    o.fade = v.fade;\n";
    s += "    return o;\n}\n";
    return s;
}

std::string pixelWrapper(const StageReflection& fs, int depthMode, bool final, const std::vector<std::string>& rtTypes) {
    std::string s = varyingsStruct(fs);
    s += "cbuffer McppPass : register(b3) { float mcppAlphaRef; uint mcppAlphaFunc; uint2 mcppPassPad; };\n";
    s += "struct McppTargets {\n";
    for (size_t k = 0; k < rtTypes.size(); ++k)
        s += "    " + rtTypes[k] + " o" + std::to_string(k) + " : SV_Target" + std::to_string(k) + ";\n";
    if (fs.writesDepth) s += "    float depth : SV_Depth;\n";
    if (rtTypes.empty() && !fs.writesDepth) s += "    float4 unused : SV_Target7;\n";
    s += "};\n";
    s += "McppTargets mcpp_ps(McppVaryings i, bool mcppFront : SV_IsFrontFace) {\n";
    if (fs.usesFragCoord)
        s += "    gl_FragCoord = float4(i.pos.x, " + std::string(final ? "mcppDepth.w - i.pos.y" : "i.pos.y") +
             ", mcppGlDepth(i.pos.z, " + std::to_string(depthMode) + "), 1.0 / i.pos.w);\n";
    if (fs.usesFrontFacing) s += "    gl_FrontFacing = mcppFront;\n";
    for (size_t k = 0; k < fs.inputs.size(); ++k) {
        const StageVar& v = fs.inputs[k];
        if (v.glslName == "mcpp_tile") s += "    " + v.hlslName + " = i.tile;\n";
        else s += "    " + v.hlslName + " = i.v" + std::to_string(k) + ";\n";
    }
    s += "    if (mcppFadedOut(i.pos.xy, i.fade)) discard;\n";
    s += "    mcpp_spirv_entry_inner();\n    McppTargets o = (McppTargets)0;\n";
    auto outAt = [&](u32 loc, std::string& expr, std::string& type) -> bool {
        for (const StageVar& v : fs.outputs) {
            if (v.arraySize) {
                if (loc >= v.location && loc < v.location + v.arraySize) {
                    expr = v.hlslName + "[" + std::to_string(loc - v.location) + "]";
                    type = v.hlslType;
                    return true;
                }
            } else if (v.location == loc) {
                expr = v.hlslName;
                type = v.hlslType;
                return true;
            }
        }
        return false;
    };
    std::string e0, t0;
    if (outAt(0, e0, t0)) {
        std::string base;
        int comps;
        parseHlslType(t0, base, comps);
        if (comps == 4 && base == "float")
            s += "    if (mcppAlphaFunc != 0) {\n        float a = " + e0 + ".a, r = mcppAlphaRef;\n"
                 "        bool pass = mcppAlphaFunc == 2 ? a < r : mcppAlphaFunc == 3 ? a == r : mcppAlphaFunc == 4 ? a <= r :\n"
                 "                    mcppAlphaFunc == 5 ? a > r : mcppAlphaFunc == 6 ? a != r : mcppAlphaFunc == 7 ? a >= r :\n"
                 "                    mcppAlphaFunc == 8;\n        if (!pass) discard;\n    }\n";
    }
    for (size_t k = 0; k < rtTypes.size(); ++k) {
        std::string e, t;
        if (!outAt(u32(k), e, t)) continue;
        std::string base;
        int comps;
        parseHlslType(t, base, comps);
        const std::string ks = std::to_string(k);
        if (rtTypes[k] == "float4") {
            std::string f = base == "float" ? e : "float" + (comps > 1 ? std::to_string(comps) : std::string()) + "(" + e + ")";
            std::string v4 = comps == 4 ? f : comps == 3 ? "float4(" + f + ", 1)" : comps == 2 ? "float4(" + f + ", 0, 1)"
                                                                                               : "float4(" + f + ", 0, 0, 1)";
            s += "    o.o" + ks + " = " + v4 + ";\n";
        } else {
            std::string iv = comps == 4 ? e : comps == 3 ? e + ".xyzz" : comps == 2 ? e + ".xyyy" : e + ".xxxx";
            s += "    o.o" + ks + " = " + rtTypes[k] + "(" + iv + ");\n";
        }
    }
    if (fs.writesDepth) s += "    o.depth = mcppOurDepth(gl_FragDepth, " + std::to_string(depthMode) + ");\n";
    s += "    return o;\n}\n";
    return s;
}

// Vanilla blend of a geometry kind (PackBlend), GL factors.
BlendMode vanillaBlend(u8 key) {
    BlendMode b;
    switch (PackBlend(key)) {
        case PackBlend::Opaque: b.enabled = false; break;
        case PackBlend::Translucent:
            b = {true, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendFactor::One, BlendFactor::OneMinusSrcAlpha};
            break;
        case PackBlend::Additive: b = {true, BlendFactor::One, BlendFactor::One, BlendFactor::One, BlendFactor::One}; break;
        case PackBlend::AdditiveAlpha:
            b = {true, BlendFactor::SrcAlpha, BlendFactor::One, BlendFactor::One, BlendFactor::Zero};
            break;
        case PackBlend::Glint:
            b = {true, BlendFactor::SrcColor, BlendFactor::One, BlendFactor::Zero, BlendFactor::One};
            break;
        case PackBlend::Crumbling:
            b = {true, BlendFactor::DstColor, BlendFactor::SrcColor, BlendFactor::One, BlendFactor::Zero};
            break;
    }
    return b;
}

std::string adapterSource(Adapter a) {
    std::string s;
    switch (a) {
        case Adapter::Cube: s = "#define MCPP_ADAPTER_TERRAIN\n#define MCPP_ADAPTER_CUBE\n"; break;
        case Adapter::Model: s = "#define MCPP_ADAPTER_TERRAIN\n#define MCPP_ADAPTER_MODEL\n"; break;
        case Adapter::Generic: s = "#define MCPP_ADAPTER_GENERIC\n"; break;
        default: s = "#define MCPP_ADAPTER_FULLSCREEN\n"; break;
    }
    return s + g_packAdapterHlsli;
}

}  // namespace packs
