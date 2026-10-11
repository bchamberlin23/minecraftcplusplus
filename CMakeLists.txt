cmake_minimum_required(VERSION 3.28)
project(minecraftCplusplus LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- third party -------------------------------------------------------------
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_COMPRESSION_SUPPORT ON CACHE BOOL "" FORCE)  # needed to write region files
add_subdirectory(third_party/libdeflate EXCLUDE_FROM_ALL)

# Friends (src/social): secp256k1 keys (BIP340 Schnorr, ECDH), ICE (libjuice), UPnP (miniupnpc), KCP. Vendored sources,
# see each folder's VERSION.txt and license.
add_library(mcpp_secp256k1 STATIC third_party/secp256k1/src/secp256k1.c third_party/secp256k1/src/precomputed_ecmult.c
  third_party/secp256k1/src/precomputed_ecmult_gen.c)
target_include_directories(mcpp_secp256k1 PUBLIC third_party/secp256k1/include)
target_compile_definitions(mcpp_secp256k1 PRIVATE ENABLE_MODULE_ECDH=1 ENABLE_MODULE_EXTRAKEYS=1 ENABLE_MODULE_SCHNORRSIG=1
  PUBLIC SECP256K1_STATIC)
file(GLOB JUICE_SOURCES CONFIGURE_DEPENDS third_party/libjuice/src/*.c)
add_library(mcpp_juice STATIC ${JUICE_SOURCES})
target_include_directories(mcpp_juice PUBLIC third_party/libjuice/include PRIVATE third_party/libjuice/include/juice)
target_compile_definitions(mcpp_juice PUBLIC JUICE_STATIC PRIVATE JUICE_EXPORTS USE_NETTLE=0 RELEASE=1 WIN32_LEAN_AND_MEAN
  NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(mcpp_juice PRIVATE ws2_32 bcrypt)
file(GLOB MINIUPNPC_SOURCES CONFIGURE_DEPENDS third_party/miniupnpc/src/*.c)
add_library(mcpp_miniupnpc STATIC ${MINIUPNPC_SOURCES})
target_include_directories(mcpp_miniupnpc PUBLIC third_party/miniupnpc/include PRIVATE third_party/miniupnpc/src)
target_compile_definitions(mcpp_miniupnpc PUBLIC MINIUPNP_STATICLIB PRIVATE _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS
  _WINSOCK_DEPRECATED_NO_WARNINGS)
target_link_libraries(mcpp_miniupnpc PRIVATE ws2_32 iphlpapi)
add_library(mcpp_kcp STATIC third_party/kcp/ikcp.c)
target_include_directories(mcpp_kcp PUBLIC third_party/kcp)
foreach(lib mcpp_secp256k1 mcpp_juice mcpp_miniupnpc mcpp_kcp)
  target_compile_options(${lib} PRIVATE /W0 /utf-8)
endforeach()

# --- shaders: compiled by DXC at build time and embedded as byte arrays -------
find_program(DXC dxc REQUIRED HINTS "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64")
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(SHADER_HEADERS "")
file(GLOB SHADER_INCLUDES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/*.hlsli)
function(add_shader src entry profile)
  set(out ${SHADER_OUT}/${entry}.h)
  add_custom_command(OUTPUT ${out}
    COMMAND ${DXC} -nologo -T ${profile} -E ${entry} -O3 -Qstrip_debug -Qstrip_reflect
            -Fh ${out} -Vn g_${entry} ${CMAKE_SOURCE_DIR}/shaders/${src}
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/${src} ${SHADER_INCLUDES}
    VERBATIM)
  set(SHADER_HEADERS ${SHADER_HEADERS} ${out} PARENT_SCOPE)
endfunction()
add_shader(terrain.hlsl TerrainVS vs_6_0)
add_shader(terrain.hlsl TerrainPSOpaque ps_6_0)
add_shader(terrain.hlsl TerrainPSCutout ps_6_0)
add_shader(terrain.hlsl TerrainPSTranslucent ps_6_0)
add_shader(detail.hlsl DetailVS vs_6_0)
add_shader(detail.hlsl DetailPS ps_6_0)
add_shader(cull.hlsl CullCS cs_6_0)
add_shader(overlay.hlsl LineVS vs_6_0)
add_shader(overlay.hlsl LinePS ps_6_0)
add_shader(sprite.hlsl SpriteVS vs_6_0)
add_shader(sprite.hlsl SpritePS ps_6_0)
add_shader(entity.hlsl EntityModelVS vs_6_0)
add_shader(entity.hlsl EntityFreeVS vs_6_0)
add_shader(entity.hlsl EntityCutoutPS ps_6_0)
add_shader(entity.hlsl EntityTranslucentPS ps_6_0)
add_shader(entity.hlsl EntityGlintPS ps_6_0)
add_shader(entity.hlsl EntityCutoutMvPS ps_6_0)
add_shader(entity.hlsl EntityShadowVS vs_6_0)
add_shader(entity.hlsl EntityShadowFreeVS vs_6_0)
add_shader(entity.hlsl EntityShadowPS ps_6_0)
add_shader(visuals.hlsl VisualsVS vs_6_0)
add_shader(visuals.hlsl ParticleVS vs_6_0)
add_shader(visuals.hlsl VisualsCutoutPS ps_6_0)
add_shader(visuals.hlsl VisualsTranslucentPS ps_6_0)
add_shader(visuals.hlsl VisualsTextPS ps_6_0)
add_shader(visuals.hlsl VisualsGlintPS ps_6_0)
add_shader(visuals.hlsl VisualsCrumblingPS ps_6_0)
add_shader(visuals.hlsl ParticleCutoutPS ps_6_0)
add_shader(visuals.hlsl ParticleTranslucentPS ps_6_0)
add_shader(visuals.hlsl VisualsCutoutMvPS ps_6_0)
add_shader(visuals.hlsl ParticleCutoutMvPS ps_6_0)
add_shader(visuals.hlsl VisualsShadowVS vs_6_0)
add_shader(visuals.hlsl VisualsShadowPS ps_6_0)
add_shader(blockentity.hlsl BlockEntityVS vs_6_0)
add_shader(blockentity.hlsl BlockEntityPS ps_6_0)
add_shader(blockentity.hlsl BlockEntityTranslucentPS ps_6_0)
add_shader(blockentity.hlsl BlockEntityMvPS ps_6_0)
add_shader(blockentity.hlsl BlockEntityShadowVS vs_6_0)
add_shader(blockentity.hlsl BlockEntityShadowPS ps_6_0)
add_shader(ui.hlsl UiVS vs_6_0)
add_shader(ui.hlsl UiPS ps_6_0)
add_shader(sky.hlsl SkyFullscreenVS vs_6_0)
add_shader(sky.hlsl SkyDiscPS ps_6_0)
add_shader(sky.hlsl SkyDarkDiscPS ps_6_0)
add_shader(sky.hlsl SkyGeoVS vs_6_0)
add_shader(sky.hlsl SkyGeoPS ps_6_0)
add_shader(sky.hlsl SkyWeatherPS ps_6_0)
add_shader(sky.hlsl SkyCloudVS vs_6_0)
add_shader(sky.hlsl SkyCloudPS ps_6_0)
# Built-in shader pipeline (src/render/shading.cpp).
add_shader(terrain.hlsl TerrainGbufVS vs_6_0)
add_shader(terrain.hlsl TerrainGbufPS ps_6_0)
add_shader(terrain.hlsl TerrainGbufPSCutout ps_6_0)
add_shader(terrain.hlsl TerrainShadowVS vs_6_0)
add_shader(terrain.hlsl TerrainShadowPSCutout ps_6_0)
add_shader(terrain.hlsl TerrainFwdVS vs_6_0)
add_shader(terrain.hlsl TerrainFwdPS ps_6_0)
add_shader(detail.hlsl DetailGbufVS vs_6_0)
add_shader(detail.hlsl DetailGbufPS ps_6_0)
add_shader(detail.hlsl DetailShadowVS vs_6_0)
add_shader(detail.hlsl DetailShadowPS ps_6_0)
add_shader(cull.hlsl ShadowCullCS cs_6_0)
add_shader(deferred.hlsl DeferredCS cs_6_0)
add_shader(deferred.hlsl VolumetricCS cs_6_0)
add_shader(deferred.hlsl CompositeCS cs_6_0)
add_shader(deferred.hlsl AoCS cs_6_0)
add_shader(deferred.hlsl MotionCS cs_6_0)
add_shader(post.hlsl DepthUpsamplePS ps_6_0)
add_shader(post.hlsl UiCompositePS ps_6_0)
add_shader(post.hlsl TaaCS cs_6_0)
add_shader(post.hlsl BloomDownCS cs_6_0)
add_shader(post.hlsl BloomUpCS cs_6_0)
add_shader(post.hlsl ExposureCS cs_6_0)
add_shader(post.hlsl TonemapVS vs_6_0)
add_shader(post.hlsl TonemapPS ps_6_0)
# Vendor upscaler inputs (src/render/upscalers).
add_shader(upscale_prep.hlsl ReactiveCS cs_6_0)
# OptiFine / Iris shader packs (src/render/packs): utility passes, and the vertex adapters embedded as text (compiled
# at run time together with each pack's translated programs).
add_shader(packs/pack_util.hlsl PackDepthToGlCS cs_6_0)
add_shader(packs/pack_util.hlsl PackMipCS cs_6_0)
add_shader(packs/pack_util.hlsl PackShadowCullCS cs_6_0)
add_shader(packs/pack_util.hlsl PackMergeVS vs_6_0)
add_shader(packs/pack_util.hlsl PackMergePS ps_6_0)
add_shader(packs/pack_util.hlsl PackDepthFlipPS ps_6_0)
add_shader(packs/pack_util.hlsl PackMotionCS cs_6_0)
add_shader(packs/pack_util.hlsl PackUpCopyPS ps_6_0)
add_shader(packs/pack_util.hlsl PackDepthUpPS ps_6_0)
add_custom_command(OUTPUT ${SHADER_OUT}/pack_adapter_hlsli.h
  COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/shaders/packs/adapter.hlsli -DOUT=${SHADER_OUT}/pack_adapter_hlsli.h
          -DNAME=g_packAdapterHlsli -P ${CMAKE_SOURCE_DIR}/tools/embed_text.cmake
  DEPENDS ${CMAKE_SOURCE_DIR}/shaders/packs/adapter.hlsli ${CMAKE_SOURCE_DIR}/tools/embed_text.cmake VERBATIM)
list(APPEND SHADER_HEADERS ${SHADER_OUT}/pack_adapter_hlsli.h)

# --- executable ---------------------------------------------------------------
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
# World generation must reproduce the game's Java float/double math exactly.
file(GLOB WORLDGEN_SOURCES CONFIGURE_DEPENDS src/worldgen/*.cpp)
set_source_files_properties(${WORLDGEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/fp:precise")
add_executable(minecraftCplusplus ${SOURCES} ${SHADER_HEADERS})
target_include_directories(minecraftCplusplus PRIVATE src third_party ${SHADER_OUT}
  third_party/libdeflate)
target_link_libraries(minecraftCplusplus PRIVATE libdeflate_static d3d12 dxgi dxguid ws2_32 bcrypt
  mcpp_secp256k1 mcpp_juice mcpp_miniupnpc mcpp_kcp winhttp iphlpapi)

# --- vendor upscalers / frame generation (src/render/upscalers, SDKs from tools/fetch_sdks.ps1) ----------------
# Headers live in third_party; the signed DLLs are loaded at runtime (each SDK optional). XeSS links its import
# libraries delay-loaded, Streamline and the FidelityFX loader are resolved with GetProcAddress.
target_include_directories(minecraftCplusplus PRIVATE third_party/streamline/include third_party/fidelityfx
  third_party/xess/inc)
if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/xess/lib/libxess.lib)
  target_link_libraries(minecraftCplusplus PRIVATE ${CMAKE_SOURCE_DIR}/third_party/xess/lib/libxess.lib
    ${CMAKE_SOURCE_DIR}/third_party/xess/lib/libxess_fg.lib ${CMAKE_SOURCE_DIR}/third_party/xess/lib/libxell.lib
    delayimp wintrust crypt32)
  target_link_options(minecraftCplusplus PRIVATE /DELAYLOAD:libxess.dll /DELAYLOAD:libxess_fg.dll /DELAYLOAD:libxell.dll)
  target_compile_definitions(minecraftCplusplus PRIVATE MCPP_HAVE_XESS=1)
else()
  target_link_libraries(minecraftCplusplus PRIVATE wintrust crypt32)
endif()
# Copy the SDK DLLs (and their licenses) next to the executable.
file(GLOB VENDOR_DLLS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/third_party/streamline/bin/*.dll
  ${CMAKE_SOURCE_DIR}/third_party/fidelityfx/bin/*.dll ${CMAKE_SOURCE_DIR}/third_party/xess/bin/*.dll)
foreach(dll ${VENDOR_DLLS})
  get_filename_component(name ${dll} NAME)
  add_custom_command(TARGET minecraftCplusplus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dll} $<TARGET_FILE_DIR:minecraftCplusplus>/${name} VERBATIM)
endforeach()
add_custom_command(TARGET minecraftCplusplus POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:minecraftCplusplus>/licenses VERBATIM)
foreach(lic streamline/license.txt streamline/nvngx_dlss.license.txt streamline/reflex.license.txt
            fidelityfx/license.md xess/LICENSE.txt secp256k1/COPYING libjuice/LICENSE miniupnpc/LICENSE kcp/LICENSE)
  if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/${lic})
    string(REPLACE "/" "_" licName ${lic})
    add_custom_command(TARGET minecraftCplusplus POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/third_party/${lic}
        $<TARGET_FILE_DIR:minecraftCplusplus>/licenses/${licName} VERBATIM)
  endif()
endforeach()

# --- OptiFine / Iris shader packs (src/render/packs): glslang + SPIRV-Cross from tools/fetch_shader_tools.ps1,
# DXC (dxcompiler.dll + dxil.dll from the Windows SDK) at run time. Without the sources packs are unavailable.
include(third_party/shader_tools.cmake)
if(MCPP_SHADER_TOOLS)
  target_link_libraries(minecraftCplusplus PRIVATE mcpp_glslang mcpp_spirv_cross)
  target_compile_definitions(minecraftCplusplus PRIVATE MCPP_HAVE_SHADER_TOOLS=1)
  add_custom_command(TARGET minecraftCplusplus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/third_party/glslang/LICENSE.txt
      $<TARGET_FILE_DIR:minecraftCplusplus>/licenses/glslang_LICENSE.txt
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/third_party/spirv-cross/LICENSE
      $<TARGET_FILE_DIR:minecraftCplusplus>/licenses/spirv-cross_LICENSE.txt VERBATIM)
endif()
get_filename_component(DXC_DIR ${DXC} DIRECTORY)
foreach(dll dxcompiler.dll dxil.dll)
  if(EXISTS ${DXC_DIR}/${dll})
    add_custom_command(TARGET minecraftCplusplus POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DXC_DIR}/${dll} $<TARGET_FILE_DIR:minecraftCplusplus>/${dll} VERBATIM)
  endif()
endforeach()

target_compile_definitions(minecraftCplusplus PRIVATE
  WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(minecraftCplusplus PRIVATE /W3 /permissive- /Zc:__cplusplus /utf-8 /EHsc
  $<$<CONFIG:Release>:/O2 /Oi /GL /arch:AVX2 /fp:fast /Zi>)
target_link_options(minecraftCplusplus PRIVATE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF /DEBUG /MAP>)

# --- example native mod (src/mods/mcpp_mod.h): a DLL to drop into the mods folder ---------------------------
add_library(hello_mod SHARED examples/hello_mod/hello_mod.c)
target_include_directories(hello_mod PRIVATE src/mods)
target_compile_definitions(hello_mod PRIVATE _CRT_SECURE_NO_WARNINGS)

# --- minecraftCplusplusW.exe: starts the game without a console window (shortcuts, tools/make_shortcut.ps1) -----
add_executable(minecraftCplusplusW WIN32 tools/launcher/launcher.cpp)
target_compile_definitions(minecraftCplusplusW PRIVATE UNICODE _UNICODE)
