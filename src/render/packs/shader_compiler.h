#pragma once
// Shader pack stage compilation: transformed GLSL (glsl_transform.h) -> SPIR-V (glslang, Vulkan rules relaxed: loose
// uniforms become the default block "mcpp_Globals") -> HLSL (SPIRV-Cross, SM 6.0) + reflection; then HLSL + the
// adapter / wrapper entry points -> DXIL (dxcompiler.dll at run time). DXIL is cached on disk by content hash.
//
// Register layout (matches the pack root signatures in pack_pipeline.cpp):
//   b0 space0   the stage's default uniform block (one root CBV per stage, visibility-split)
//   t<i> s<i>   space0: the stage's textures in reflection order (one SRV + one sampler table per stage)
//   t0..2 s0..2 space2: gtexture / normals / specular (per-draw: atlas, entity texture, sun / moon ...)
//   u<i> space0 images (image2D / image3D), u<n> space1 shader storage buffers (binding n = bufferObject.n)
#include "core/common.h"
#include "render/packs/glsl_transform.h"

#include <memory>

namespace packs {

enum class BaseType : u8 { Float, Int, UInt, Bool, Struct, Other };

struct UniformMember {
    std::string name;   // GLSL name ("gbufferModelView"; struct members "mcpp_Fog.color")
    u32 offset = 0;     // bytes into the block
    BaseType type = BaseType::Float;
    u32 vecsize = 1, columns = 1;  // columns > 1: matrix (column-major, matrixStride per column)
    u32 arraySize = 0;  // 0: not an array
    u32 arrayStride = 0, matrixStride = 16;
};

enum class TexDim : u8 { Tex1D, Tex2D, Tex3D, Cube, Tex2DArray, Other };
struct TextureBinding {
    std::string name;  // GLSL sampler name
    TexDim dim = TexDim::Tex2D;
    bool shadow = false;            // sampler2DShadow (comparison)
    BaseType sampled = BaseType::Float;
    u32 space = 0, reg = 0;         // assigned register (t + s)
};
struct ImageBinding {
    std::string name;
    TexDim dim = TexDim::Tex2D;
    u32 reg = 0;  // u<reg> space0
};
struct BufferBinding {
    std::string name;
    u32 binding = 0;  // u<binding> space1
};

struct StageVar {
    std::string glslName;  // as declared in GLSL
    std::string hlslName;  // the SPIRV-Cross static it became
    std::string hlslType;  // "float2", "float3x3", "int"...
    u32 arraySize = 0;
    u32 location = 0;
    bool flat = false, noperspective = false, centroid = false;
};

struct StageReflection {
    std::vector<UniformMember> uniforms;
    u32 uniformBytes = 0;  // 0: no default block
    std::vector<TextureBinding> textures;
    std::vector<ImageBinding> images;
    std::vector<BufferBinding> buffers;
    std::vector<StageVar> inputs, outputs;
    bool writesDepth = false;  // gl_FragDepth
    bool usesFrontFacing = false, usesFragCoord = false;
    u32 localSize[3] = {1, 1, 1};
};

// GLSL -> HLSL. `perDrawTextures`: samplers bound per draw (space2 slots in this order: gtexture, normals, specular).
struct SpirvResult {
    std::string hlsl;
    StageReflection refl;
    std::string error;
};
bool glslToHlsl(const std::string& glsl, ShaderStage stage, const std::string& name, SpirvResult& out);

// HLSL -> DXIL with dxcompiler.dll (loaded on first use from the executable's folder). Thread-safe.
// `profile`: vs_6_0 / ps_6_0 / cs_6_0. Results are cached in `cacheDir` (empty: no disk cache).
struct DxilResult {
    std::vector<u8> code;
    std::string error;
    bool cached = false;
};
bool compileHlsl(const std::string& source, const char* entry, const char* profile, const fs::path& cacheDir,
                 DxilResult& out);
bool dxcAvailable(std::string* error = nullptr);

u64 hash64(std::string_view data, u64 seed = 0xcbf29ce484222325ull);

}  // namespace packs
