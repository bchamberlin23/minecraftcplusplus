# glslang (GLSL -> SPIR-V) and SPIRV-Cross (SPIR-V -> HLSL) as static libraries for shader-pack support
# (src/render/packs). Sources come from tools/fetch_shader_tools.ps1; when they are missing nothing is defined and
# MCPP_HAVE_SHADER_TOOLS stays off (packs unavailable, everything else unchanged).
set(MCPP_SHADER_TOOLS OFF)
set(_gl ${CMAKE_CURRENT_LIST_DIR}/glslang)
set(_sc ${CMAKE_CURRENT_LIST_DIR}/spirv-cross)
if(EXISTS ${_gl}/glslang/MachineIndependent/ShaderLang.cpp AND EXISTS ${_sc}/spirv_hlsl.cpp)
  file(GLOB _gl_src CONFIGURE_DEPENDS
    ${_gl}/glslang/MachineIndependent/*.cpp ${_gl}/glslang/MachineIndependent/preprocessor/*.cpp
    ${_gl}/glslang/GenericCodeGen/*.cpp ${_gl}/glslang/ResourceLimits/ResourceLimits.cpp
    ${_gl}/glslang/OSDependent/Windows/ossource.cpp ${_gl}/SPIRV/*.cpp)
  list(FILTER _gl_src EXCLUDE REGEX "(CInterface|resource_limits_c|SpvTools)")
  add_library(mcpp_glslang STATIC ${_gl_src})
  target_include_directories(mcpp_glslang PUBLIC ${_gl} ${_gl}/include)
  target_compile_definitions(mcpp_glslang PUBLIC ENABLE_SPIRV ENABLE_OPT=0 PRIVATE GLSLANG_OSINCLUDE_WIN32
    _CRT_SECURE_NO_WARNINGS NOMINMAX)
  target_compile_options(mcpp_glslang PRIVATE /W0 /utf-8 /EHsc /bigobj $<$<CONFIG:Release>:/O2>)

  add_library(mcpp_spirv_cross STATIC ${_sc}/spirv_cross.cpp ${_sc}/spirv_parser.cpp ${_sc}/spirv_cross_parsed_ir.cpp
    ${_sc}/spirv_cfg.cpp ${_sc}/spirv_glsl.cpp ${_sc}/spirv_hlsl.cpp)
  target_include_directories(mcpp_spirv_cross PUBLIC ${_sc})
  target_compile_definitions(mcpp_spirv_cross PUBLIC SPIRV_CROSS_NAMESPACE_OVERRIDE=spirv_cross PRIVATE
    _CRT_SECURE_NO_WARNINGS NOMINMAX)
  target_compile_options(mcpp_spirv_cross PRIVATE /W0 /utf-8 /EHsc /bigobj $<$<CONFIG:Release>:/O2>)
  set(MCPP_SHADER_TOOLS ON)
endif()
