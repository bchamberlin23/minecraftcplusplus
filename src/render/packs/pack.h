#pragma once
// A loaded shader pack (OptiFine / Iris format) for one dimension: options, shaders.properties semantics, the
// program set with Iris's fallback chain, render-target / shadow directives, and every program translated to HLSL
// (glsl_pre -> glsl_transform -> shader_compiler). No D3D objects: the pipeline (pack_pipeline.h) builds those.
#include "core/common.h"
#include "render/packs/glsl_pre.h"
#include "render/packs/glsl_transform.h"
#include "render/packs/pack_source.h"
#include "render/packs/properties.h"
#include "render/packs/shader_compiler.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace packs {

// Iris gbuffers / shadow programs (ProgramId) in fallback order.
enum class Gb : u8 {
    Basic, Line, Textured, TexturedLit, SkyBasic, SkyTextured, Clouds, Terrain, TerrainSolid, TerrainCutout,
    DamagedBlock, Block, BlockTrans, BeaconBeam, Item, Entities, EntitiesTrans, EntitiesGlowing, ArmorGlint,
    SpiderEyes, Lightning, Hand, HandWater, Weather, Water, Particles, ParticlesTrans,
    Shadow, ShadowSolid, ShadowCutout, ShadowWater, ShadowEntities, ShadowLightning, ShadowBlock,
    Count
};
const char* gbName(Gb g);  // "gbuffers_terrain", "shadow_cutout", ...
Gb gbFallback(Gb g);        // Gb::Count: none

// Render stage values (Iris WorldRenderingPhase; the MC_RENDER_STAGE_* macros).
enum class RenderStage : u8 {
    None, Sky, Sunset, CustomSky, Sun, Moon, Stars, Void, TerrainSolid, TerrainCutoutMipped, TerrainCutout,
    Entities, BlockEntities, Destroy, Outline, Debug, HandSolid, TerrainTranslucent, Tripwire, Particles, Clouds,
    RainSnow, WorldBorder, HandTranslucent
};

enum class BlendFactor : u8 { Zero, One, SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor, SrcAlpha,
                              OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha, SrcAlphaSaturate };
struct BlendMode {
    bool enabled = false;
    BlendFactor src = BlendFactor::SrcAlpha, dst = BlendFactor::OneMinusSrcAlpha;
    BlendFactor srcA = BlendFactor::One, dstA = BlendFactor::OneMinusSrcAlpha;
};
enum class AlphaFunc : u8 { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
struct AlphaTest {
    bool enabled = false;
    AlphaFunc func = AlphaFunc::Greater;
    float ref = 0.1f;
};

// Render target formats (OptiFine names; GL internal formats).
enum class TexFormat : u8 {
    RGBA8, R8, RG8, RGB8, RGBA16, R16, RG16, RGB16, RGBA16F, R16F, RG16F, RGB16F, RGBA32F, R32F, RG32F, RGB32F,
    R11F_G11F_B10F, RGB10_A2, RGB5_A1, RGBA8_SNORM, R8_SNORM, RG8_SNORM, RGBA16_SNORM, R16_SNORM, RG16_SNORM,
    R8I, RG8I, RGBA8I, R8UI, RG8UI, RGBA8UI, R16I, RG16I, RGBA16I, R16UI, RG16UI, RGBA16UI, R32I, RG32I, RGBA32I,
    R32UI, RG32UI, RGBA32UI, RGB9_E5, RGB8_SNORM, RGB16_SNORM, RGB8I, RGB8UI, RGB16I, RGB16UI, RGB32I, RGB32UI,
    Count
};
bool parseTexFormat(std::string_view name, TexFormat& out);
const char* texFormatName(TexFormat f);

struct RenderTargetDesc {
    TexFormat format = TexFormat::RGBA8;
    bool clear = true;
    std::optional<std::array<float, 4>> clearColor;  // default: colortex0 fog colour, colortex1 white, others 0
    bool sizeRelative = true;
    float size[2] = {1, 1};  // relative scale or absolute pixels
};

struct ShadowDesc {
    int resolution = 1024;
    float distance = 160.0f;
    float distanceRenderMul = -1.0f;
    float intervalSize = 2.0f;
    bool hardwareFiltering[2] = {false, false};  // shadowtex0 / shadowtex1 (shadowHardwareFiltering[0|1])
    bool nearest[2] = {false, false};            // shadowtexNNearest
    bool colorNearest[2] = {false, false};
    bool colorMipmap[2] = {false, false};
    bool depthMipmap[2] = {false, false};
    TexFormat colorFormat[2] = {TexFormat::RGBA8, TexFormat::RGBA8};
    bool colorClear[2] = {true, true};
    std::array<float, 4> colorClearColor[2] = {{1, 1, 1, 1}, {1, 1, 1, 1}};
    // properties
    bool terrain = true, translucent = true, entities = true, player = false, blockEntities = true;
    std::string culling = "true";
};

struct CustomTexture {
    std::string sampler;  // GLSL sampler name it is bound to
    std::string stage;    // "gbuffers", "shadow", "deferred", "composite", ... or "" (customTexture.*: everywhere)
    std::string path;     // pack-relative file, or a resource id ("minecraft:textures/...")
    bool raw = false;     // raw binary texture (texture.x = path TEXTURE_2D ...)
    int rawDims = 2, rawW = 0, rawH = 0, rawD = 0;
    TexFormat rawFormat = TexFormat::RGBA8;
    std::string rawPixelFormat, rawPixelType;
    bool blur = false, clamp = false;  // .mcmeta
};

struct CustomImage {  // image.<name> = sampler format internalFormat pixelType clear relative w h [d]
    std::string name, sampler;
    TexFormat format = TexFormat::RGBA8;
    bool clear = true, relative = false;
    float size[3] = {1, 1, 1};
    int dims = 2;
};

struct CustomUniform {  // uniform.<type>.<name> = expr / variable.<type>.<name> = expr
    std::string type, name, expr;
    bool isVariable = false;
};

struct Stage {
    bool present = false;
    std::string file;           // pack-relative source file
    std::string glsl;           // translated GLSL (diagnostics)
    GlslTransformResult transform;
    SpirvResult hlsl;
    std::string error;
};

struct Program {
    std::string name;  // "gbuffers_terrain", "composite3", "composite3_a" (compute)
    Stage vs, fs, cs;
    bool enabled = true;
    std::vector<int> drawBuffers;          // colortex indices (DRAWBUFFERS / RENDERTARGETS), fragment location order
    std::set<int> mipmapped;               // colortexNMipmapEnabled in this program
    std::map<int, bool> flip;              // flip.<program>.<buffer>
    std::optional<BlendMode> blend;        // blend.<program>
    std::map<int, BlendMode> bufferBlend;  // blend.<program>.<buffer>
    std::optional<AlphaTest> alphaTest;    // alphaTest.<program>
    float scale[3] = {1, 0, 0};            // scale.<program> = s [x y]
    // Compute: dispatch size (workGroups = groups, workGroupsRender = fraction of the screen in threads).
    bool hasWorkGroups = false, hasWorkGroupsRender = false;
    int workGroups[3] = {1, 1, 1};
    float workGroupsRender[2] = {1, 1};
    bool ok() const { return enabled && ((vs.present && fs.present && vs.error.empty() && fs.error.empty()) ||
                                         (cs.present && cs.error.empty())); }
};

struct OptionState {
    std::map<std::string, OptionDecl> decls;               // discovered options
    std::unordered_map<std::string, std::string> values;   // non-default values in effect
};

// Pack-level settings the pipeline and uniforms need.
struct PackSettings {
    std::array<RenderTargetDesc, 16> targets;
    ShadowDesc shadow;
    float sunPathRotation = 0;
    float ambientOcclusionLevel = 1;
    float eyeBrightnessHalflife = 10, centerDepthHalflife = 1, wetnessHalflife = 600, drynessHalflife = 200;
    int noiseResolution = 256;
    bool separateAo = false, oldLighting = false, oldHandLight = true, underwaterOverlay = true, vignette = true;
    bool sun = true, moon = true, stars = true, rainDepth = false, beaconBeamDepth = false, prepareBeforeShadow = false;
    std::string clouds = "fancy", particlesOrdering = "before";
    bool frustumCulling = true;
};

struct LoadOptions {
    std::string dimension = "minecraft:overworld";  // dimension id (world0 / world-1 / world1 folders)
    std::unordered_map<std::string, std::string> optionValues;  // user settings (option -> value)
    std::string profile;                                         // profile name to apply first (optional)
    std::string gpuVendor = "NVIDIA";
    bool compileHlsl = true;  // translate every program to HLSL (false: options / properties only)
    bool failOnErrors = true;  // a program that does not translate fails the load (Iris); false: report and go on
    unsigned threads = 0;     // compile threads (0 = hardware)
};

class PackDefinition {
public:
    bool load(const fs::path& path, const LoadOptions& opt, std::string& error);

    PackSource source;
    Properties props;     // preprocessed shaders.properties
    OptionState options;
    PackSettings settings;
    std::string dimensionDir;  // "world0/" or ""
    MacroTable macros;         // standard + option macros (the environment programs were preprocessed with)

    std::array<std::optional<Program>, size_t(Gb::Count)> gbuffers;  // present programs (no fallback applied)
    // Composite-style passes in order (setup, begin, shadowcomp, prepare, deferred, composite, final).
    struct PassGroup {
        std::string prefix;              // "deferred"
        std::vector<Program> passes;     // each: fragment pass and / or compute programs (in `computes`)
    };
    std::vector<Program> setup, begin, shadowcomp, prepare, deferred, composite;
    std::optional<Program> final_;
    std::map<std::string, std::vector<Program>> computes;  // "composite3" -> composite3.csh, composite3_a.csh, ...

    std::vector<CustomTexture> textures;
    std::vector<CustomImage> images;
    std::map<int, u64> bufferObjects;  // bufferObject.N = size
    std::vector<CustomUniform> uniforms;
    std::map<std::string, std::string> lang;  // en_us.lang (option names, values, screens)
    std::vector<std::string> warnings;
    double loadMs = 0;
    u32 stagesCompiled = 0, stagesFailed = 0;

    // The program used for a gbuffers role after the fallback chain (nullptr: none, i.e. vanilla rendering skipped).
    const Program* resolve(Gb g) const;

private:
    std::string predefines_;  // standard macros as #define lines (glslang sees them)
    bool loadProgram(Program& p, const std::string& base, bool compute, std::vector<std::function<void()>>& jobs);
    void parseProperties();
    void scanDirectives(Program& p, const std::string& activeText, ShaderStage stage, const MacroTable& macros);
};

// Iris id maps: block.properties / item.properties / entity.properties ("block.<id> = names").
struct IdMapEntry {
    int id = 0;
    std::string name;                                   // "minecraft:oak_leaves" (namespace added)
    std::vector<std::pair<std::string, std::vector<std::string>>> states;  // property = allowed values
};
bool loadIdMap(const PackSource& src, const std::string& file, const MacroTable& macros, const std::string& prefix,
               std::vector<IdMapEntry>& out);

}  // namespace packs
