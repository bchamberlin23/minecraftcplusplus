#pragma once
// Rewrites a shader pack's (preprocessed) GLSL - #version 110..460, compatibility or core profile - into Vulkan-style
// GLSL 460 that glslang compiles to SPIR-V with relaxed rules (loose uniforms go into a default uniform block):
//   attribute / varying            -> in / out
//   gl_Vertex, gl_Normal, gl_Color, gl_MultiTexCoordN (VS)       -> mcpp_* inputs fed by the vertex adapters
//   gl_ModelViewMatrix, gl_ProjectionMatrix, gl_NormalMatrix, gl_TextureMatrix[], gl_Fog, ... -> mcpp_* uniforms
//   gl_FrontColor / gl_TexCoord[] / gl_FogFragCoord (VS out, FS in) -> mcpp_* varyings
//   gl_FragData[n] / gl_FragColor  -> layout(location = n) outputs (the draw buffer slot)
//   texture2D / texture2DLod / shadow2D / ftransform() ...        -> core equivalents
//   identifiers that are keywords or built-ins in GLSL 460 but not in the source's version -> renamed
//   uniform sampler2D texture (GLSL <= 120 name of the atlas)     -> gtexture
// Fragment shaders of terrain programs (atlasWrap) sample the block atlas through mcpp_atlas_* helpers that wrap the
// coordinates into the quad's tile (flat varying mcpp_tile): our greedy / LOD quads repeat their texture.
#include "core/common.h"

#include <map>
#include <set>

namespace packs {

enum class ShaderStage : u8 { Vertex, Fragment, Compute, Geometry };

struct GlslTransformOptions {
    ShaderStage stage = ShaderStage::Vertex;
    bool atlasWrap = false;
    bool forceLod0 = false;  // implicit-LOD texture() -> textureLod(..., 0) (compute, full-screen passes)
    std::string predefines;  // "#define X y" lines placed after #version (Iris standard macros)
    std::set<std::string> atlasSamplers = {"gtexture", "tex", "texture", "normals", "specular"};
};

struct GlslTransformResult {
    std::string glsl;
    int version = 0;
    bool compatibility = false;
    // Fragment outputs: GLSL name -> location (= draw buffer slot); gl_FragData with a dynamic index becomes the
    // array mcpp_FragData[8] (fragDataArray).
    std::map<std::string, int> outputs;
    bool fragDataArray = false;
    int texCoordCount = 0;                             // gl_TexCoord[] size (0: unused)
    std::map<std::string, std::string> uniformDefaults;  // initializers removed from uniform declarations
    std::set<std::string> builtinsUsed;               // mcpp_* names referenced
    std::string error;
};

bool transformGlsl(const std::string& source, const GlslTransformOptions& opt, GlslTransformResult& out);

}  // namespace packs
