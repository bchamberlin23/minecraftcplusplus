#include "render/packs/shader_compiler.h"

#include <windows.h>
// WIN32_LEAN_AND_MEAN leaves out COM / OLE types that dxcapi.h needs.
#include <oleauto.h>
#include <unknwn.h>

#include <dxcapi.h>
#include <wrl/client.h>

#include <atomic>
#include <fstream>
#include <mutex>

#if MCPP_HAVE_SHADER_TOOLS
#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv_hlsl.hpp>
#endif

namespace packs {

u64 hash64(std::string_view data, u64 seed) {
    u64 h = seed;
    for (char c : data) {
        h ^= u8(c);
        h *= 0x100000001b3ull;
    }
    h ^= h >> 29;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 32;
    return h;
}

#if MCPP_HAVE_SHADER_TOOLS

namespace {

std::once_flag g_glslangInit;

BaseType baseOf(const spirv_cross::SPIRType& t) {
    using T = spirv_cross::SPIRType;
    switch (t.basetype) {
        case T::Float: return BaseType::Float;
        case T::Int: return BaseType::Int;
        case T::UInt: return BaseType::UInt;
        case T::Boolean: return BaseType::Bool;
        case T::Struct: return BaseType::Struct;
        default: return BaseType::Other;
    }
}

TexDim dimOf(const spirv_cross::SPIRType& t) {
    switch (t.image.dim) {
        case spv::Dim1D: return TexDim::Tex1D;
        case spv::Dim2D: return t.image.arrayed ? TexDim::Tex2DArray : TexDim::Tex2D;
        case spv::Dim3D: return TexDim::Tex3D;
        case spv::DimCube: return TexDim::Cube;
        default: return TexDim::Other;
    }
}

void reflectStruct(const spirv_cross::Compiler& comp, const spirv_cross::SPIRType& type, u32 base, const std::string& prefix,
                   std::vector<UniformMember>& out) {
    for (u32 i = 0; i < u32(type.member_types.size()); ++i) {
        const spirv_cross::SPIRType& mt = comp.get_type(type.member_types[i]);
        UniformMember m;
        m.name = prefix + comp.get_member_name(type.self, i);
        m.offset = base + comp.type_struct_member_offset(type, i);
        m.type = baseOf(mt);
        m.vecsize = mt.vecsize;
        m.columns = mt.columns;
        m.arraySize = mt.array.empty() ? 0 : mt.array[0];
        if (!mt.array.empty()) m.arrayStride = comp.type_struct_member_array_stride(type, i);
        if (mt.columns > 1) m.matrixStride = comp.type_struct_member_matrix_stride(type, i);
        if (m.type == BaseType::Struct) {
            if (m.arraySize == 0) reflectStruct(comp, mt, m.offset, m.name + ".", out);
            continue;
        }
        out.push_back(std::move(m));
    }
}

// "static <type> <name>[...];" in SPIRV-Cross output -> type and array size.
bool findStatic(const std::string& hlsl, const std::string& name, std::string& type, u32& arraySize) {
    size_t at = 0;
    for (;;) {
        at = hlsl.find("static ", at);
        if (at == std::string::npos) return false;
        if (at > 0 && hlsl[at - 1] != '\n') {
            at += 7;
            continue;
        }
        size_t eol = hlsl.find('\n', at);
        std::string line = hlsl.substr(at + 7, eol == std::string::npos ? std::string::npos : eol - at - 7);
        at += 7;
        // line: "<type> <name>[N];" or "<type> <name>;" (type may contain "const" for constants: skip those)
        if (line.compare(0, 6, "const ") == 0) continue;
        size_t semi = line.find(';');
        if (semi == std::string::npos) continue;
        std::string decl = line.substr(0, semi);
        size_t eq = decl.find('=');
        if (eq != std::string::npos) decl = decl.substr(0, eq);
        while (!decl.empty() && decl.back() == ' ') decl.pop_back();
        u32 arr = 0;
        if (!decl.empty() && decl.back() == ']') {
            size_t lb = decl.rfind('[');
            arr = u32(std::atoi(decl.c_str() + lb + 1));
            decl = decl.substr(0, lb);
        }
        size_t sp = decl.rfind(' ');
        if (sp == std::string::npos) continue;
        if (decl.substr(sp + 1) != name) continue;
        type = decl.substr(0, sp);
        arraySize = arr;
        return true;
    }
}

}  // namespace

bool glslToHlsl(const std::string& glsl, ShaderStage stage, const std::string& name, SpirvResult& out) {
    out = {};
    std::call_once(g_glslangInit, [] { glslang::InitializeProcess(); });
    EShLanguage lang = stage == ShaderStage::Vertex     ? EShLangVertex
                       : stage == ShaderStage::Fragment ? EShLangFragment
                       : stage == ShaderStage::Compute  ? EShLangCompute
                                                        : EShLangGeometry;
    std::vector<u32> spirv;
    {
        glslang::TShader shader(lang);
        const char* strs[] = {glsl.c_str()};
        const int lens[] = {int(glsl.size())};
        const char* names[] = {name.c_str()};
        shader.setStringsWithLengthsAndNames(strs, lens, names, 1);
        shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
        shader.setEnvInputVulkanRulesRelaxed();
        shader.setGlobalUniformBlockName("mcpp_Globals");
        shader.setGlobalUniformSet(0);
        shader.setGlobalUniformBinding(0);
        shader.setAutoMapBindings(true);
        shader.setAutoMapLocations(true);
        const EShMessages msgs = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
        if (!shader.parse(GetDefaultResources(), 460, false, msgs)) {
            out.error = shader.getInfoLog();
            return false;
        }
        glslang::TProgram program;
        program.addShader(&shader);
        if (!program.link(msgs) || !program.mapIO()) {
            out.error = std::string(program.getInfoLog()) + shader.getInfoLog();
            return false;
        }
        glslang::SpvOptions so;
        so.disableOptimizer = true;
        so.validate = false;
        spv::SpvBuildLogger logger;
        glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &logger, &so);
        if (spirv.empty()) {
            out.error = "SPIR-V generation failed: " + logger.getAllMessages();
            return false;
        }
    }
    try {
        spirv_cross::CompilerHLSL comp(std::move(spirv));
        spirv_cross::CompilerHLSL::Options ho;
        ho.shader_model = 60;
        ho.point_size_compat = true;
        ho.point_coord_compat = true;
        ho.force_storage_buffer_as_uav = true;
        ho.use_entry_point_name = true;
        comp.set_hlsl_options(ho);
        spirv_cross::CompilerGLSL::Options co = comp.get_common_options();
        co.vertex.flip_vert_y = false;
        co.vertex.fixup_clipspace = false;
        comp.set_common_options(co);
        const spv::ExecutionModel model = stage == ShaderStage::Vertex     ? spv::ExecutionModelVertex
                                          : stage == ShaderStage::Fragment ? spv::ExecutionModelFragment
                                          : stage == ShaderStage::Compute  ? spv::ExecutionModelGLCompute
                                                                           : spv::ExecutionModelGeometry;
        const char* entry = stage == ShaderStage::Compute ? "mcpp_cs" : "mcpp_spirv_entry";
        comp.rename_entry_point("main", entry, model);
        spirv_cross::ShaderResources res = comp.get_shader_resources();
        StageReflection& refl = out.refl;

        auto bind = [&](u32 id, u32 set, u32 binding) {
            comp.set_decoration(id, spv::DecorationDescriptorSet, set);
            comp.set_decoration(id, spv::DecorationBinding, binding);
        };
        for (const auto& r : res.uniform_buffers) {
            const spirv_cross::SPIRType& type = comp.get_type(r.base_type_id);
            std::string block = comp.get_name(r.base_type_id);
            if (block != "mcpp_Globals" && r.name != "mcpp_Globals") {
                out.error = "uniform blocks are not supported (" + block + ")";
                return false;
            }
            bind(r.id, 0, 0);
            spirv_cross::HLSLResourceBinding b;
            b.stage = model, b.desc_set = 0, b.binding = 0;
            b.cbv = {0, 0};
            comp.add_hlsl_resource_binding(b);
            reflectStruct(comp, type, 0, "", refl.uniforms);
            refl.uniformBytes = u32(comp.get_declared_struct_size(type));
        }
        u32 texReg = 0;
        for (const auto& r : res.sampled_images) {
            const spirv_cross::SPIRType& type = comp.get_type(r.type_id);
            TextureBinding t;
            t.name = r.name;
            t.dim = dimOf(type);
            t.shadow = type.image.depth;
            t.sampled = baseOf(comp.get_type(type.image.type));
            if (!type.array.empty()) {
                out.error = "sampler arrays are not supported (" + t.name + ")";
                return false;
            }
            if (t.name == "gtexture" || t.name == "tex" || t.name == "texture") t.space = 2, t.reg = 0;
            else if (t.name == "normals") t.space = 2, t.reg = 1;
            else if (t.name == "specular") t.space = 2, t.reg = 2;
            else t.space = 0, t.reg = texReg++;
            const u32 binding = u32(refl.textures.size());
            bind(r.id, 1, binding);
            spirv_cross::HLSLResourceBinding b;
            b.stage = model, b.desc_set = 1, b.binding = binding;
            b.srv = {t.space, t.reg};
            b.sampler = {t.space, t.reg};
            comp.add_hlsl_resource_binding(b);
            refl.textures.push_back(std::move(t));
        }
        if (!res.separate_images.empty() || !res.separate_samplers.empty()) {
            out.error = "separate images / samplers are not supported";
            return false;
        }
        for (const auto& r : res.storage_images) {
            const spirv_cross::SPIRType& type = comp.get_type(r.type_id);
            ImageBinding im;
            im.name = r.name;
            im.dim = dimOf(type);
            im.reg = u32(refl.images.size());
            bind(r.id, 2, im.reg);
            spirv_cross::HLSLResourceBinding b;
            b.stage = model, b.desc_set = 2, b.binding = im.reg;
            b.uav = {0, im.reg};
            b.srv = {0, im.reg};
            comp.add_hlsl_resource_binding(b);
            refl.images.push_back(std::move(im));
        }
        for (const auto& r : res.storage_buffers) {
            BufferBinding bb;
            bb.name = r.name;
            bb.binding = comp.get_decoration(r.id, spv::DecorationBinding);
            bind(r.id, 3, bb.binding);
            spirv_cross::HLSLResourceBinding b;
            b.stage = model, b.desc_set = 3, b.binding = bb.binding;
            b.uav = {1, bb.binding};
            b.srv = {1, bb.binding};
            comp.add_hlsl_resource_binding(b);
            refl.buffers.push_back(std::move(bb));
        }
        if (stage == ShaderStage::Compute)
            for (int k = 0; k < 3; ++k) refl.localSize[k] = std::max(1u, comp.get_execution_mode_argument(spv::ExecutionModeLocalSize, k));

        out.hlsl = comp.compile();

        auto stageVars = [&](const spirv_cross::SmallVector<spirv_cross::Resource>& list, std::vector<StageVar>& dst) {
            for (const auto& r : list) {
                StageVar v;
                v.glslName = r.name;
                v.hlslName = comp.get_name(r.id);
                if (v.hlslName.empty()) v.hlslName = r.name;
                v.location = comp.get_decoration(r.id, spv::DecorationLocation);
                v.flat = comp.has_decoration(r.id, spv::DecorationFlat);
                v.noperspective = comp.has_decoration(r.id, spv::DecorationNoPerspective);
                v.centroid = comp.has_decoration(r.id, spv::DecorationCentroid);
                if (!findStatic(out.hlsl, v.hlslName, v.hlslType, v.arraySize)) continue;  // not emitted (unused)
                const spirv_cross::SPIRType& t = comp.get_type(r.type_id);
                if (t.basetype == spirv_cross::SPIRType::Int || t.basetype == spirv_cross::SPIRType::UInt) v.flat = true;
                dst.push_back(std::move(v));
            }
        };
        stageVars(res.stage_inputs, refl.inputs);
        stageVars(res.stage_outputs, refl.outputs);
        std::string t;
        u32 a;
        refl.writesDepth = findStatic(out.hlsl, "gl_FragDepth", t, a);
        refl.usesFrontFacing = findStatic(out.hlsl, "gl_FrontFacing", t, a);
        refl.usesFragCoord = findStatic(out.hlsl, "gl_FragCoord", t, a);
    } catch (const std::exception& e) {
        out.error = std::string("SPIRV-Cross: ") + e.what();
        return false;
    }
    return true;
}

#else

bool glslToHlsl(const std::string&, ShaderStage, const std::string&, SpirvResult& out) {
    out = {};
    out.error = "shader pack support was built without glslang / SPIRV-Cross (run tools/fetch_shader_tools.ps1)";
    return false;
}

#endif

// ---- DXC -----------------------------------------------------------------------------------------------------

namespace {
using Microsoft::WRL::ComPtr;
std::once_flag g_dxcOnce;
DxcCreateInstanceProc g_dxcCreate = nullptr;
std::string g_dxcError;

void loadDxc() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    fs::path dir = fs::path(exe).parent_path();
    HMODULE m = LoadLibraryW((dir / L"dxcompiler.dll").c_str());
    if (!m) m = LoadLibraryW(L"dxcompiler.dll");
    if (!m) {
        g_dxcError = "dxcompiler.dll not found next to the executable";
        return;
    }
    g_dxcCreate = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(m, "DxcCreateInstance"));
    if (!g_dxcCreate) g_dxcError = "dxcompiler.dll has no DxcCreateInstance";
}

struct DxcThread {
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcUtils> utils;
};
DxcThread& dxcThread() {
    thread_local DxcThread t;
    if (!t.compiler && g_dxcCreate) {
        g_dxcCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&t.compiler));
        g_dxcCreate(CLSID_DxcUtils, IID_PPV_ARGS(&t.utils));
    }
    return t;
}

std::atomic<u32> g_tmpCounter{0};
}  // namespace

bool dxcAvailable(std::string* error) {
    std::call_once(g_dxcOnce, loadDxc);
    if (error) *error = g_dxcError;
    return g_dxcCreate != nullptr;
}

bool compileHlsl(const std::string& source, const char* entry, const char* profile, const fs::path& cacheDir,
                 DxilResult& out) {
    out = {};
    std::call_once(g_dxcOnce, loadDxc);
    // Cache key: source, entry, profile and a format tag (bump when the argument list changes).
    const std::string tag = std::string("dxil-v1|") + entry + "|" + profile + "|";
    const u64 key = hash64(source, hash64(tag));
    char keyName[32];
    std::snprintf(keyName, sizeof(keyName), "%016llx.dxil", static_cast<unsigned long long>(key));
    if (!cacheDir.empty() && readFile(cacheDir / keyName, out.code) && out.code.size() > 16) {
        out.cached = true;
        return true;
    }
    if (!g_dxcCreate) {
        out.error = g_dxcError;
        return false;
    }
    DxcThread& t = dxcThread();
    if (!t.compiler) {
        out.error = "DxcCreateInstance failed";
        return false;
    }
    std::wstring wentry(entry, entry + std::strlen(entry)), wprofile(profile, profile + std::strlen(profile));
    const wchar_t* args[] = {L"-E", wentry.c_str(), L"-T", wprofile.c_str(), L"-O3", L"-HV", L"2018",
                             L"-Qstrip_debug", L"-Qstrip_reflect", L"-Wno-ignored-attributes"};
    DxcBuffer buf{source.data(), source.size(), DXC_CP_UTF8};
    ComPtr<IDxcResult> result;
    if (FAILED(t.compiler->Compile(&buf, args, UINT32(std::size(args)), nullptr, IID_PPV_ARGS(&result))) || !result) {
        out.error = "DXC Compile call failed";
        return false;
    }
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (FAILED(status)) {
        out.error = errors && errors->GetStringLength() ? std::string(errors->GetStringPointer(), errors->GetStringLength())
                                                        : "DXC compilation failed";
        return false;
    }
    ComPtr<IDxcBlob> code;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&code), nullptr);
    if (!code || !code->GetBufferSize()) {
        out.error = "DXC produced no code";
        return false;
    }
    out.code.assign(static_cast<const u8*>(code->GetBufferPointer()),
                    static_cast<const u8*>(code->GetBufferPointer()) + code->GetBufferSize());
    if (!cacheDir.empty()) {
        std::error_code ec;
        fs::create_directories(cacheDir, ec);
        const fs::path tmp = cacheDir / (std::string(keyName) + ".tmp" + std::to_string(GetCurrentThreadId()) + "_" +
                                         std::to_string(g_tmpCounter++));
        {
            std::ofstream f(tmp, std::ios::binary);
            f.write(reinterpret_cast<const char*>(out.code.data()), std::streamsize(out.code.size()));
        }
        fs::rename(tmp, cacheDir / keyName, ec);
        if (ec) fs::remove(tmp, ec);
    }
    return true;
}

}  // namespace packs
