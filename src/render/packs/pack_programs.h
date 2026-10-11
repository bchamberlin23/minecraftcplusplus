#pragma once
// Program helpers of the shader pack pipeline (pack_programs.cpp): sampler / image name resolution, sampler kinds,
// and the generated HLSL entry points (vertex adapters -> pack vertex shader -> varyings, varyings -> pack fragment
// shader -> render targets).
#include "render/packs/pack_pipe.h"

namespace packs {

ResRef resolveTexture(const PackDefinition& def, const std::string& program, const std::string& name,
                      const std::vector<TextureBinding>& all, std::string* warning);
ResRef resolveImage(const PackDefinition& def, const std::string& name);
Samp samplerFor(const TextureBinding& t, const ResRef& ref, const PackDefinition& def, const Program* prog);

// depthMode: adapter.hlsli mcppClip modes. rtTypes: "float4" / "int4" / "uint4" per render target.
std::string vertexWrapper(const StageReflection& vs, const StageReflection& fs, int depthMode, bool flipY);
std::string pixelWrapper(const StageReflection& fs, int depthMode, bool final, const std::vector<std::string>& rtTypes);
std::string adapterSource(Adapter a);   // #defines + shaders/packs/adapter.hlsli
BlendMode vanillaBlend(u8 packBlend);  // the GL blend of a PackBlend (render/pack_sink.h)

}  // namespace packs
