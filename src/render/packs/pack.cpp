#include "render/packs/pack.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

namespace packs {

namespace {

const char* kGbNames[] = {
    "gbuffers_basic", "gbuffers_line", "gbuffers_textured", "gbuffers_textured_lit", "gbuffers_skybasic",
    "gbuffers_skytextured", "gbuffers_clouds", "gbuffers_terrain", "gbuffers_terrain_solid", "gbuffers_terrain_cutout",
    "gbuffers_damagedblock", "gbuffers_block", "gbuffers_block_translucent", "gbuffers_beaconbeam", "gbuffers_item",
    "gbuffers_entities", "gbuffers_entities_translucent", "gbuffers_entities_glowing", "gbuffers_armor_glint",
    "gbuffers_spidereyes", "gbuffers_lightning", "gbuffers_hand", "gbuffers_hand_water", "gbuffers_weather",
    "gbuffers_water", "gbuffers_particles", "gbuffers_particles_translucent",
    "shadow", "shadow_solid", "shadow_cutout", "shadow_water", "shadow_entities", "shadow_lightning", "shadow_block"};
static_assert(std::size(kGbNames) == size_t(Gb::Count));

const char* kFormatNames[] = {
    "RGBA8", "R8", "RG8", "RGB8", "RGBA16", "R16", "RG16", "RGB16", "RGBA16F", "R16F", "RG16F", "RGB16F", "RGBA32F",
    "R32F", "RG32F", "RGB32F", "R11F_G11F_B10F", "RGB10_A2", "RGB5_A1", "RGBA8_SNORM", "R8_SNORM", "RG8_SNORM",
    "RGBA16_SNORM", "R16_SNORM", "RG16_SNORM", "R8I", "RG8I", "RGBA8I", "R8UI", "RG8UI", "RGBA8UI", "R16I", "RG16I",
    "RGBA16I", "R16UI", "RG16UI", "RGBA16UI", "R32I", "RG32I", "RGBA32I", "R32UI", "RG32UI", "RGBA32UI", "RGB9_E5",
    "RGB8_SNORM", "RGB16_SNORM", "RGB8I", "RGB8UI", "RGB16I", "RGB16UI", "RGB32I", "RGB32UI"};
static_assert(std::size(kFormatNames) == size_t(TexFormat::Count));

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::toupper(u8(c)));
    return r;
}
std::string lowerStr(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower(u8(c)));
    return r;
}

// Buffer name -> colortex index (colortexN, OptiFine aliases).
int bufferIndex(std::string_view name) {
    static const std::pair<const char*, int> kAliases[] = {{"gcolor", 0}, {"gdepth", 1}, {"gnormal", 2}, {"composite", 3},
                                                           {"gaux1", 4},  {"gaux2", 5},  {"gaux3", 6},   {"gaux4", 7}};
    for (const auto& [n, i] : kAliases)
        if (name == n) return i;
    if (name.substr(0, 8) == "colortex") {
        int i = std::atoi(std::string(name.substr(8)).c_str());
        if (name.size() > 8 && i >= 0 && i < 16) return i;
    }
    return -1;
}

bool parseBlendFactor(std::string_view s, BlendFactor& f) {
    static const std::pair<const char*, BlendFactor> k[] = {
        {"ZERO", BlendFactor::Zero}, {"ONE", BlendFactor::One}, {"SRC_COLOR", BlendFactor::SrcColor},
        {"ONE_MINUS_SRC_COLOR", BlendFactor::OneMinusSrcColor}, {"DST_COLOR", BlendFactor::DstColor},
        {"ONE_MINUS_DST_COLOR", BlendFactor::OneMinusDstColor}, {"SRC_ALPHA", BlendFactor::SrcAlpha},
        {"ONE_MINUS_SRC_ALPHA", BlendFactor::OneMinusSrcAlpha}, {"DST_ALPHA", BlendFactor::DstAlpha},
        {"ONE_MINUS_DST_ALPHA", BlendFactor::OneMinusDstAlpha}, {"SRC_ALPHA_SATURATE", BlendFactor::SrcAlphaSaturate}};
    std::string u = upper(s);
    for (const auto& [n, v] : k)
        if (u == n) return f = v, true;
    return false;
}

bool parseBlend(const std::string& v, BlendMode& out) {
    auto parts = splitWhitespace(v);
    if (parts.size() == 1 && lowerStr(parts[0]) == "off") return out.enabled = false, true;
    if (parts.size() != 4) return false;
    out.enabled = true;
    return parseBlendFactor(parts[0], out.src) && parseBlendFactor(parts[1], out.dst) && parseBlendFactor(parts[2], out.srcA) &&
           parseBlendFactor(parts[3], out.dstA);
}

bool parseAlphaTest(const std::string& v, AlphaTest& out) {
    auto parts = splitWhitespace(v);
    if (parts.size() == 1 && lowerStr(parts[0]) == "off") return out.enabled = false, true;
    if (parts.size() != 2) return false;
    static const std::pair<const char*, AlphaFunc> k[] = {
        {"NEVER", AlphaFunc::Never}, {"LESS", AlphaFunc::Less}, {"EQUAL", AlphaFunc::Equal}, {"LEQUAL", AlphaFunc::LessEqual},
        {"GREATER", AlphaFunc::Greater}, {"NOTEQUAL", AlphaFunc::NotEqual}, {"GEQUAL", AlphaFunc::GreaterEqual},
        {"ALWAYS", AlphaFunc::Always}};
    std::string u = upper(parts[0]);
    for (const auto& [n, f] : k)
        if (u == n) {
            out.enabled = true;
            out.func = f;
            out.ref = float(std::atof(parts[1].c_str()));
            return true;
        }
    return false;
}

// Numeric value of a directive (macros expanded, simple arithmetic).
std::optional<double> number(const std::string& text, const MacroTable& macros) {
    std::string t = trim(text);
    // GLSL float suffixes
    if (!t.empty() && (t.back() == 'f' || t.back() == 'F')) t.pop_back();
    auto v = evalPp(t, macros);
    if (!v) return std::nullopt;
    return v->num();
}

bool truthy(const std::string& text, const MacroTable& macros) {
    std::string t = trim(text);
    if (t == "true") return true;
    if (t == "false") return false;
    auto v = number(t, macros);
    return v && *v != 0;
}

// vec4(a, b, c, d) / vec4(a) -> 4 floats.
std::optional<std::array<float, 4>> vec4Value(const std::string& text, const MacroTable& macros) {
    std::string t = expandMacros(text, macros);
    size_t a = t.find('('), b = t.rfind(')');
    if (a == std::string::npos || b == std::string::npos) return std::nullopt;
    std::vector<float> v;
    std::string inner = t.substr(a + 1, b - a - 1);
    size_t i = 0;
    while (i <= inner.size()) {
        size_t j = inner.find(',', i);
        if (j == std::string::npos) j = inner.size();
        auto n = number(inner.substr(i, j - i), macros);
        if (!n) return std::nullopt;
        v.push_back(float(*n));
        i = j + 1;
    }
    if (v.size() == 1) return std::array<float, 4>{v[0], v[0], v[0], v[0]};
    if (v.size() != 4) return std::nullopt;
    return std::array<float, 4>{v[0], v[1], v[2], v[3]};
}

void defineMacro(MacroTable& m, const std::string& name, const std::string& body = "") { m[name] = Macro{false, {}, body}; }

void standardMacros(MacroTable& m, const std::string& vendor) {
    defineMacro(m, "MC_VERSION", "260300");
    defineMacro(m, "MC_GL_VERSION", "460");
    defineMacro(m, "MC_GLSL_VERSION", "460");
    defineMacro(m, "MC_OS_WINDOWS");
    if (vendor == "NVIDIA") defineMacro(m, "MC_GL_VENDOR_NVIDIA"), defineMacro(m, "MC_GL_RENDERER_GEFORCE");
    else if (vendor == "AMD") defineMacro(m, "MC_GL_VENDOR_ATI"), defineMacro(m, "MC_GL_VENDOR_AMD"), defineMacro(m, "MC_GL_RENDERER_RADEON");
    else if (vendor == "Intel") defineMacro(m, "MC_GL_VENDOR_INTEL"), defineMacro(m, "MC_GL_RENDERER_INTEL");
    defineMacro(m, "MC_RENDER_QUALITY", "1.0");
    defineMacro(m, "MC_SHADOW_QUALITY", "1.0");
    defineMacro(m, "MC_HAND_DEPTH", "0.125");
    defineMacro(m, "MC_NORMAL_MAP");
    defineMacro(m, "MC_SPECULAR_MAP");
    defineMacro(m, "IS_IRIS");
    defineMacro(m, "IRIS_VERSION", "11200");
    defineMacro(m, "IRIS_TAG_SUPPORT", "2");
    for (const char* f : {"CUSTOM_IMAGES", "SSBO", "COMPUTE_SHADERS", "SEPARATE_HARDWARE_SAMPLERS", "HIGHER_SHADOWCOLOR",
                          "PER_BUFFER_BLENDING", "ENTITY_TRANSLUCENT", "BLOCK_EMISSION_ATTRIBUTE", "CUSTOM_TEXTURE_NAME"})
        defineMacro(m, std::string("IRIS_FEATURE_") + f);
    for (const char* e : {"ARB_shader_texture_lod", "EXT_gpu_shader4", "ARB_gpu_shader5", "ARB_texture_gather",
                          "ARB_shader_image_load_store", "ARB_shading_language_packing", "ARB_compute_shader",
                          "ARB_shader_storage_buffer_object", "ARB_explicit_attrib_location", "EXT_texture_array",
                          "ARB_texture_rectangle", "ARB_derivative_control", "ARB_conservative_depth"})
        defineMacro(m, std::string("MC_GL_") + e);
    static const char* kStages[] = {"NONE", "SKY", "SUNSET", "CUSTOM_SKY", "SUN", "MOON", "STARS", "VOID", "TERRAIN_SOLID",
                                    "TERRAIN_CUTOUT_MIPPED", "TERRAIN_CUTOUT", "ENTITIES", "BLOCK_ENTITIES", "DESTROY",
                                    "OUTLINE", "DEBUG", "HAND_SOLID", "TERRAIN_TRANSLUCENT", "TRIPWIRE", "PARTICLES",
                                    "CLOUDS", "RAIN_SNOW", "WORLD_BORDER", "HAND_TRANSLUCENT"};
    for (size_t i = 0; i < std::size(kStages); ++i) defineMacro(m, std::string("MC_RENDER_STAGE_") + kStages[i], std::to_string(i));
    for (size_t i = 0; i < size_t(TexFormat::Count); ++i)
        defineMacro(m, std::string("MC_TEXTURE_FORMAT_") + kFormatNames[i], std::to_string(i));
}

bool isProgramFile(const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    return ext == ".vsh" || ext == ".fsh" || ext == ".csh" || ext == ".gsh";
}

}  // namespace

const char* gbName(Gb g) { return kGbNames[size_t(g)]; }

Gb gbFallback(Gb g) {
    switch (g) {
        case Gb::Basic: return Gb::Count;
        case Gb::Line: return Gb::Basic;
        case Gb::Textured: return Gb::Basic;
        case Gb::TexturedLit: return Gb::Textured;
        case Gb::SkyBasic: return Gb::Basic;
        case Gb::SkyTextured: return Gb::Textured;
        case Gb::Clouds: return Gb::Textured;
        case Gb::Terrain: return Gb::TexturedLit;
        case Gb::TerrainSolid: return Gb::Terrain;
        case Gb::TerrainCutout: return Gb::Terrain;
        case Gb::DamagedBlock: return Gb::Terrain;
        case Gb::Block: return Gb::Terrain;
        case Gb::BlockTrans: return Gb::Block;
        case Gb::BeaconBeam: return Gb::Textured;
        case Gb::Item: return Gb::TexturedLit;
        case Gb::Entities: return Gb::TexturedLit;
        case Gb::EntitiesTrans: return Gb::Entities;
        case Gb::EntitiesGlowing: return Gb::Entities;
        case Gb::ArmorGlint: return Gb::Textured;
        case Gb::SpiderEyes: return Gb::Textured;
        case Gb::Lightning: return Gb::Entities;
        case Gb::Hand: return Gb::TexturedLit;
        case Gb::HandWater: return Gb::Hand;
        case Gb::Weather: return Gb::TexturedLit;
        case Gb::Water: return Gb::Terrain;
        case Gb::Particles: return Gb::TexturedLit;
        case Gb::ParticlesTrans: return Gb::Particles;
        case Gb::Shadow: return Gb::Count;
        case Gb::ShadowSolid: return Gb::Shadow;
        case Gb::ShadowCutout: return Gb::Shadow;
        case Gb::ShadowWater: return Gb::Shadow;
        case Gb::ShadowEntities: return Gb::Shadow;
        case Gb::ShadowLightning: return Gb::ShadowEntities;
        case Gb::ShadowBlock: return Gb::Shadow;
        default: return Gb::Count;
    }
}

bool parseTexFormat(std::string_view name, TexFormat& out) {
    std::string u = upper(trim(name));
    for (size_t i = 0; i < size_t(TexFormat::Count); ++i)
        if (u == kFormatNames[i]) return out = TexFormat(i), true;
    if (u == "RGBA") return out = TexFormat::RGBA8, true;
    if (u == "RGB") return out = TexFormat::RGB8, true;
    if (u == "RG") return out = TexFormat::RG8, true;
    if (u == "R") return out = TexFormat::R8, true;
    return false;
}

const char* texFormatName(TexFormat f) { return kFormatNames[size_t(f)]; }

const Program* PackDefinition::resolve(Gb g) const {
    for (Gb c = g; c != Gb::Count; c = gbFallback(c))
        if (gbuffers[size_t(c)] && gbuffers[size_t(c)]->ok()) return &*gbuffers[size_t(c)];
    return nullptr;
}

// ---- directives -----------------------------------------------------------------------------------------------

void PackDefinition::scanDirectives(Program& p, const std::string& text, ShaderStage stage, const MacroTable& macros) {
    PackSettings& s = settings;
    // Comment directives: the last DRAWBUFFERS / RENDERTARGETS wins.
    if (stage == ShaderStage::Fragment) {
        size_t at = 0;
        size_t lastDraw = std::string::npos, lastTargets = std::string::npos;
        while ((at = text.find("DRAWBUFFERS:", at)) != std::string::npos) lastDraw = at, at += 12;
        at = 0;
        while ((at = text.find("RENDERTARGETS:", at)) != std::string::npos) lastTargets = at, at += 14;
        if (lastTargets != std::string::npos && (lastDraw == std::string::npos || lastTargets > lastDraw)) {
            p.drawBuffers.clear();
            size_t i = lastTargets + 14;
            for (;;) {
                while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
                size_t j = i;
                while (j < text.size() && std::isdigit(u8(text[j]))) ++j;
                if (j == i) break;
                p.drawBuffers.push_back(std::atoi(text.c_str() + i));
                while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) ++j;
                if (j >= text.size() || text[j] != ',') break;
                i = j + 1;
            }
        } else if (lastDraw != std::string::npos) {
            p.drawBuffers.clear();
            for (size_t i = lastDraw + 12; i < text.size() && std::isxdigit(u8(text[i])); ++i) {
                char c = text[i];
                int v = std::isdigit(u8(c)) ? c - '0' : std::toupper(u8(c)) - 'A' + 10;
                p.drawBuffers.push_back(v);
            }
        }
    }
    // const directives (in code or in comments).
    size_t i = 0;
    while ((i = text.find("const ", i)) != std::string::npos) {
        if (i > 0 && (std::isalnum(u8(text[i - 1])) || text[i - 1] == '_')) {
            i += 6;
            continue;
        }
        size_t semi = text.find(';', i);
        size_t eol = text.find('\n', i);
        if (semi == std::string::npos || (eol != std::string::npos && semi > eol)) {
            i += 6;
            continue;
        }
        std::string decl = text.substr(i + 6, semi - i - 6);
        i = semi;
        auto words = splitWhitespace(decl.substr(0, decl.find('=')));
        size_t eq = decl.find('=');
        if (words.size() != 2 || eq == std::string::npos) continue;
        const std::string& type = words[0];
        const std::string& name = words[1];
        std::string value = trim(decl.substr(eq + 1));
        auto num = [&] { return number(value, macros); };
        auto flag = [&] { return truthy(value, macros); };
        auto bufferDirective = [&](const std::string& prefix, int index) {
            std::string rest = name.substr(prefix.size());
            RenderTargetDesc& t = s.targets[size_t(index)];
            if (rest == "Format") {
                TexFormat f;
                if (parseTexFormat(value, f)) t.format = f;
                else warnings.push_back("unknown format " + value + " for " + name);
            } else if (rest == "Clear") {
                t.clear = flag();
            } else if (rest == "ClearColor") {
                if (auto v = vec4Value(value, macros)) t.clearColor = *v;
            } else if (rest == "MipmapEnabled") {
                if (flag()) p.mipmapped.insert(index);
            }
        };
        bool handled = false;
        for (int k = 15; k >= 0 && !handled; --k) {
            std::string pre = "colortex" + std::to_string(k);
            if (name.compare(0, pre.size(), pre) == 0 && name.size() > pre.size() && !std::isdigit(u8(name[pre.size()])))
                bufferDirective(pre, k), handled = true;
        }
        static const char* kAliases[] = {"gcolor", "gdepth", "gnormal", "composite", "gaux1", "gaux2", "gaux3", "gaux4"};
        for (int k = 0; k < 8 && !handled; ++k) {
            std::string pre = kAliases[k];
            if (name.compare(0, pre.size(), pre) == 0 && name.size() > pre.size() && std::isupper(u8(name[pre.size()])))
                bufferDirective(pre, k), handled = true;
        }
        if (handled) continue;
        if (name == "shadowMapResolution") { if (auto v = num()) s.shadow.resolution = int(*v); }
        else if (name == "shadowDistance") { if (auto v = num()) s.shadow.distance = float(*v); }
        else if (name == "shadowDistanceRenderMul") { if (auto v = num()) s.shadow.distanceRenderMul = float(*v); }
        else if (name == "shadowIntervalSize") { if (auto v = num()) s.shadow.intervalSize = float(*v); }
        else if (name == "shadowHardwareFiltering") s.shadow.hardwareFiltering[0] = s.shadow.hardwareFiltering[1] = flag();
        else if (name == "shadowHardwareFiltering0") s.shadow.hardwareFiltering[0] = flag();
        else if (name == "shadowHardwareFiltering1") s.shadow.hardwareFiltering[1] = flag();
        else if (name == "shadowtexNearest" || name == "shadowtex0Nearest" || name == "shadow0MinMagNearest") s.shadow.nearest[0] = flag();
        else if (name == "shadowtex1Nearest" || name == "shadow1MinMagNearest") s.shadow.nearest[1] = flag();
        else if (name == "shadowcolor0Nearest" || name == "shadowColor0Nearest" || name == "shadowColor0MinMagNearest") s.shadow.colorNearest[0] = flag();
        else if (name == "shadowcolor1Nearest" || name == "shadowColor1Nearest" || name == "shadowColor1MinMagNearest") s.shadow.colorNearest[1] = flag();
        else if (name == "shadowcolor0Format" || name == "shadowcolorFormat") { TexFormat f; if (parseTexFormat(value, f)) s.shadow.colorFormat[0] = f; }
        else if (name == "shadowcolor1Format") { TexFormat f; if (parseTexFormat(value, f)) s.shadow.colorFormat[1] = f; }
        else if (name == "shadowcolor0Clear") s.shadow.colorClear[0] = flag();
        else if (name == "shadowcolor1Clear") s.shadow.colorClear[1] = flag();
        else if (name == "shadowcolor0ClearColor") { if (auto v = vec4Value(value, macros)) s.shadow.colorClearColor[0] = *v; }
        else if (name == "shadowcolor1ClearColor") { if (auto v = vec4Value(value, macros)) s.shadow.colorClearColor[1] = *v; }
        else if (name == "generateShadowMipmap" || name == "shadowtex0Mipmap" || name == "shadowtexMipmap") s.shadow.depthMipmap[0] = flag();
        else if (name == "shadowtex1Mipmap") s.shadow.depthMipmap[1] = flag();
        else if (name == "generateShadowColorMipmap" || name == "shadowcolor0Mipmap" || name == "shadowColor0Mipmap") s.shadow.colorMipmap[0] = flag();
        else if (name == "shadowcolor1Mipmap" || name == "shadowColor1Mipmap") s.shadow.colorMipmap[1] = flag();
        else if (name == "sunPathRotation") { if (auto v = num()) s.sunPathRotation = float(*v); }
        else if (name == "ambientOcclusionLevel") { if (auto v = num()) s.ambientOcclusionLevel = float(*v); }
        else if (name == "eyeBrightnessHalflife") { if (auto v = num()) s.eyeBrightnessHalflife = float(*v); }
        else if (name == "centerDepthHalflife") { if (auto v = num()) s.centerDepthHalflife = float(*v); }
        else if (name == "wetnessHalflife") { if (auto v = num()) s.wetnessHalflife = float(*v); }
        else if (name == "drynessHalflife") { if (auto v = num()) s.drynessHalflife = float(*v); }
        else if (name == "noiseTextureResolution") { if (auto v = num()) s.noiseResolution = int(*v); }
        else if (name == "workGroups" && type == "ivec3") {
            std::string t = expandMacros(value, macros);
            size_t a = t.find('(');
            if (a != std::string::npos) {
                std::vector<int> v;
                std::string inner = t.substr(a + 1, t.rfind(')') - a - 1);
                size_t k = 0;
                while (k <= inner.size()) {
                    size_t j = inner.find(',', k);
                    if (j == std::string::npos) j = inner.size();
                    auto n = number(inner.substr(k, j - k), macros);
                    v.push_back(n ? int(*n) : 1);
                    k = j + 1;
                }
                for (size_t c = 0; c < 3; ++c) p.workGroups[c] = c < v.size() ? v[c] : (v.size() == 1 ? v[0] : 1);
                p.hasWorkGroups = true;
            }
        } else if (name == "workGroupsRender" && type == "vec2") {
            if (auto v = vec4Value(value, macros)) {
                p.workGroupsRender[0] = (*v)[0], p.workGroupsRender[1] = (*v)[1];
            } else {
                std::string t = expandMacros(value, macros);
                size_t a = t.find('('), c = t.find(',', a);
                if (a != std::string::npos && c != std::string::npos) {
                    auto x = number(t.substr(a + 1, c - a - 1), macros), y = number(t.substr(c + 1, t.rfind(')') - c - 1), macros);
                    if (x && y) p.workGroupsRender[0] = float(*x), p.workGroupsRender[1] = float(*y);
                }
            }
            p.hasWorkGroupsRender = true;
        }
    }
    // Old OptiFine comment directives.
    auto commentValue = [&](const char* key) -> std::optional<std::string> {
        size_t a = text.rfind(key);
        if (a == std::string::npos) return std::nullopt;
        size_t b = a + std::strlen(key), e = b;
        while (e < text.size() && !std::isspace(u8(text[e])) && text[e] != '*') ++e;
        return text.substr(b, e - b);
    };
    if (auto v = commentValue("SHADOWRES:")) s.shadow.resolution = std::atoi(v->c_str());
    if (auto v = commentValue("SHADOWHPL:")) s.shadow.distance = float(std::atof(v->c_str()));
    static const char* kFmt[] = {"GCOLORFORMAT:", "GDEPTHFORMAT:", "GNORMALFORMAT:", "COMPOSITEFORMAT:",
                                 "GAUX1FORMAT:",  "GAUX2FORMAT:",  "GAUX3FORMAT:",   "GAUX4FORMAT:"};
    for (int k = 0; k < 8; ++k)
        if (auto v = commentValue(kFmt[k])) {
            TexFormat f;
            if (parseTexFormat(*v, f)) s.targets[size_t(k)].format = f;
        }
}

// ---- properties ------------------------------------------------------------------------------------------------

void PackDefinition::parseProperties() {
    PackSettings& s = settings;
    auto b = [&](const char* key, bool& dst) { dst = props.boolean(key, dst); };
    b("separateAo", s.separateAo);
    b("oldLighting", s.oldLighting);
    b("oldHandLight", s.oldHandLight);
    b("underwaterOverlay", s.underwaterOverlay);
    b("vignette", s.vignette);
    b("sun", s.sun);
    b("moon", s.moon);
    b("stars", s.stars);
    b("rain.depth", s.rainDepth);
    b("beacon.beam.depth", s.beaconBeamDepth);
    b("prepareBeforeShadow", s.prepareBeforeShadow);
    b("frustum.culling", s.frustumCulling);
    b("shadowTerrain", s.shadow.terrain);
    b("shadowTranslucent", s.shadow.translucent);
    b("shadowEntities", s.shadow.entities);
    b("shadowPlayer", s.shadow.player);
    b("shadowBlockEntities", s.shadow.blockEntities);
    s.shadow.culling = props.value("shadow.culling", s.shadow.culling);
    s.clouds = props.value("clouds", s.clouds);
    s.particlesOrdering = props.value("particles.ordering", s.particlesOrdering);

    for (const auto& [key, value] : props.entries) {
        if (key.compare(0, 12, "size.buffer.") == 0) {
            int idx = bufferIndex(key.substr(12));
            auto parts = splitWhitespace(value);
            if (idx < 0 || parts.size() < 2) continue;
            // Iris: values with a decimal point are relative scales, integers absolute pixels.
            bool rel = parts[0].find('.') != std::string::npos || parts[1].find('.') != std::string::npos;
            auto n0 = number(parts[0], macros), n1 = number(parts[1], macros);
            if (!n0 || !n1) continue;
            RenderTargetDesc& t = s.targets[size_t(idx)];
            t.sizeRelative = rel;
            t.size[0] = float(*n0), t.size[1] = float(*n1);
        } else if (key.compare(0, 8, "texture.") == 0) {
            CustomTexture ct;
            std::string rest = key.substr(8);
            if (rest == "noise") {
                ct.sampler = "noisetex";
            } else {
                size_t dot = rest.find('.');
                if (dot == std::string::npos) continue;
                ct.stage = rest.substr(0, dot);
                ct.sampler = rest.substr(dot + 1);
                int bi = bufferIndex(ct.sampler);
                if (bi >= 0) ct.sampler = "colortex" + std::to_string(bi);
            }
            auto parts = splitWhitespace(value);
            if (parts.empty()) continue;
            ct.path = parts[0];
            if (parts.size() >= 2 && parts[1].compare(0, 8, "TEXTURE_") == 0) {
                ct.raw = true;
                ct.rawDims = parts[1] == "TEXTURE_3D" ? 3 : parts[1] == "TEXTURE_1D" ? 1 : 2;
                if (parts.size() >= 3) parseTexFormat(parts[2], ct.rawFormat);
                size_t k = 3;
                if (parts.size() > k) ct.rawW = std::atoi(parts[k++].c_str());
                if (ct.rawDims >= 2 && parts.size() > k) ct.rawH = std::atoi(parts[k++].c_str());
                if (ct.rawDims >= 3 && parts.size() > k) ct.rawD = std::atoi(parts[k++].c_str());
                if (parts.size() > k) ct.rawPixelFormat = parts[k++];
                if (parts.size() > k) ct.rawPixelType = parts[k++];
            }
            textures.push_back(std::move(ct));
        } else if (key.compare(0, 14, "customTexture.") == 0) {
            CustomTexture ct;
            ct.sampler = key.substr(14);
            auto parts = splitWhitespace(value);
            if (parts.empty()) continue;
            ct.path = parts[0];
            textures.push_back(std::move(ct));
        } else if (key.compare(0, 6, "image.") == 0) {
            auto parts = splitWhitespace(value);
            if (parts.size() < 8) {
                warnings.push_back("malformed " + key);
                continue;
            }
            CustomImage im;
            im.name = key.substr(6);
            im.sampler = parts[0];
            if (!parseTexFormat(parts[2], im.format)) warnings.push_back("unknown image format " + parts[2]);
            im.clear = parts[4] == "true";
            im.relative = parts[5] == "true";
            im.dims = int(parts.size()) - 6;
            for (int k = 0; k < im.dims && k < 3; ++k) {
                auto n = number(parts[size_t(6 + k)], macros);
                im.size[k] = n ? float(*n) : 1.0f;
            }
            if (im.relative) im.dims = 2;
            // Later definitions replace earlier ones (shaders.properties redefines under #if).
            auto it = std::find_if(images.begin(), images.end(), [&](const CustomImage& o) { return o.name == im.name; });
            if (it != images.end()) *it = im;
            else images.push_back(im);
        } else if (key.compare(0, 13, "bufferObject.") == 0) {
            auto parts = splitWhitespace(value);
            auto n = parts.empty() ? std::nullopt : number(parts[0], macros);
            if (n) bufferObjects[std::atoi(key.c_str() + 13)] = u64(*n);
        } else if (key.compare(0, 8, "uniform.") == 0 || key.compare(0, 9, "variable.") == 0) {
            const bool var = key[0] == 'v';
            std::string rest = key.substr(var ? 9 : 8);
            size_t dot = rest.find('.');
            if (dot == std::string::npos) continue;
            CustomUniform u;
            u.type = rest.substr(0, dot);
            u.name = rest.substr(dot + 1);
            u.expr = value;
            u.isVariable = var;
            uniforms.push_back(std::move(u));
        }
    }
    // iris.features.required
    if (const std::string* req = props.get("iris.features.required")) {
        static const std::set<std::string> kSupported = {"CUSTOM_IMAGES", "SSBO", "COMPUTE_SHADERS",
                                                         "SEPARATE_HARDWARE_SAMPLERS", "HIGHER_SHADOWCOLOR",
                                                         "PER_BUFFER_BLENDING", "ENTITY_TRANSLUCENT",
                                                         "BLOCK_EMISSION_ATTRIBUTE", "CUSTOM_TEXTURE_NAME"};
        for (const std::string& f : splitWhitespace(*req))
            if (!kSupported.count(upper(f))) warnings.push_back("required Iris feature not supported: " + f);
    }
}

// ---- programs ---------------------------------------------------------------------------------------------------

bool PackDefinition::loadProgram(Program& p, const std::string& base, bool compute,
                                 std::vector<std::function<void()>>& jobs) {
    const bool gbuffers = p.name.compare(0, 9, "gbuffers_") == 0 ||
                          (p.name.compare(0, 6, "shadow") == 0 && p.name.compare(0, 10, "shadowcomp") != 0);
    auto stage = [&](Stage& st, const std::string& ext, ShaderStage kind) {
        std::string file = base + ext;
        if (!source.exists(file)) return;
        st.present = true;
        st.file = file;
        jobs.push_back([this, &st, &p, kind, gbuffers] {
            if (!p.enabled) return;  // disabled by shaders.properties: not translated
            Included inc;
            if (!expandIncludes(source, st.file, inc)) {
                st.error = inc.error;
                return;
            }
            applyOptions(inc.lines, options.decls, options.values);
            MacroTable m = macros;
            Preprocessed pp;
            if (!preprocess(inc.lines, m, pp)) {
                st.error = pp.error;
                return;
            }
            {
                static std::mutex mu;  // directives write shared settings
                std::lock_guard lock(mu);
                scanDirectives(p, pp.text, kind, m);
            }
            GlslTransformOptions to;
            to.predefines = predefines_;
            to.stage = kind;
            to.atlasWrap = gbuffers && kind == ShaderStage::Fragment;
            to.forceLod0 = kind == ShaderStage::Compute || (!gbuffers && kind == ShaderStage::Fragment);
            if (!transformGlsl(pp.text, to, st.transform)) {
                st.error = st.transform.error;
                return;
            }
            st.glsl = st.transform.glsl;
            if (!glslToHlsl(st.glsl, kind, st.file, st.hlsl)) {
                // Map "file:line" of glslang errors (ERROR: <name>:<line>: ...) back to the pack's files.
                std::string err = st.hlsl.error, mapped;
                size_t i = 0;
                while (i < err.size()) {
                    size_t e = err.find('\n', i);
                    if (e == std::string::npos) e = err.size();
                    std::string line = err.substr(i, e - i);
                    size_t at = line.find(st.file + ":");
                    if (at != std::string::npos) {
                        size_t numStart = at + st.file.size() + 1;
                        u32 ln = u32(std::atoi(line.c_str() + numStart));
                        size_t numEnd = line.find(':', numStart);
                        if (ln > 0 && numEnd != std::string::npos)
                            line = line.substr(0, at) + describeLine(inc, pp, ln - 1) + line.substr(numEnd);
                    }
                    mapped += line + "\n";
                    i = e + 1;
                }
                st.error = mapped;
            }
        });
    };
    if (compute) {
        stage(p.cs, ".csh", ShaderStage::Compute);
    } else {
        stage(p.vs, ".vsh", ShaderStage::Vertex);
        stage(p.fs, ".fsh", ShaderStage::Fragment);
        if (source.exists(base + ".gsh")) warnings.push_back(p.name + ": geometry shaders are not supported (ignored)");
    }
    return p.vs.present || p.fs.present || p.cs.present;
}

bool PackDefinition::load(const fs::path& path, const LoadOptions& opt, std::string& error) {
    Timer timer;
    if (!source.open(path, error)) return false;

    // Dimension folder.
    {
        std::string folder = opt.dimension == "minecraft:the_nether" ? "world-1"
                             : opt.dimension == "minecraft:the_end" ? "world1"
                                                                     : "world0";
        std::string dimText;
        if (source.read("dimension.properties", dimText)) {
            Properties dp;
            dp.parse(dimText);
            std::string chosen, wildcard;
            for (const auto& [k, v] : dp.entries) {
                if (k.compare(0, 10, "dimension.") != 0) continue;
                for (const std::string& id : splitWhitespace(v)) {
                    if (id == "*" && wildcard.empty()) wildcard = k.substr(10);
                    if (id == opt.dimension && chosen.empty()) chosen = k.substr(10);
                }
            }
            if (!chosen.empty()) folder = chosen;
            else if (!wildcard.empty()) folder = wildcard;
        }
        bool any = false;
        for (const std::string& f : source.list(folder))
            if (isProgramFile(f)) any = true;
        dimensionDir = any ? folder + "/" : "";
    }

    // Options: scan every program file of the active folder (with includes).
    standardMacros(macros, opt.gpuVendor);
    for (const auto& [name, m] : macros) predefines_ += "#define " + name + (m.body.empty() ? "" : " " + m.body) + "\n";
    {
        std::vector<SourceLine> all;
        std::unordered_map<std::string, int> referenced;
        for (const std::string& f : source.list(dimensionDir.empty() ? "" : dimensionDir.substr(0, dimensionDir.size() - 1))) {
            if (!isProgramFile(f)) continue;
            Included inc;
            if (!expandIncludes(source, f, inc)) continue;
            collectConditionNames(inc.lines, referenced);
            all.insert(all.end(), std::make_move_iterator(inc.lines.begin()), std::make_move_iterator(inc.lines.end()));
        }
        scanOptions(all, options.decls);
        for (auto it = options.decls.begin(); it != options.decls.end();) {
            if (it->second.boolean && !referenced.count(it->first)) it = options.decls.erase(it);
            else ++it;
        }
    }

    auto optionMacros = [&](MacroTable& m) {
        for (const auto& [name, d] : options.decls) {
            auto v = options.values.find(name);
            const std::string& value = v != options.values.end() ? v->second : d.value;
            if (d.boolean) {
                if (value == "true") defineMacro(m, name);
                else m.erase(name);
            } else {
                defineMacro(m, name, value);
            }
        }
    };
    std::string propsText;
    const bool haveProps = source.read("shaders.properties", propsText);
    auto readProperties = [&] {
        props = {};
        if (!haveProps) return;
        MacroTable m = macros;
        optionMacros(m);
        Included inc;
        expandIncludesText(source, "shaders.properties", propsText, inc);
        Preprocessed pp;
        if (preprocess(inc.lines, m, pp)) props.parse(pp.text);
        else warnings.push_back("shaders.properties: " + pp.error), props.parse(propsText);
    };
    readProperties();
    // Profile, then the user's values.
    auto setOption = [&](const std::string& name, const std::string& value) {
        auto d = options.decls.find(name);
        if (d == options.decls.end()) return;
        if (value == d->second.value) options.values.erase(name);
        else options.values[name] = value;
    };
    std::function<void(const std::string&, int)> applyProfile = [&](const std::string& profile, int depth) {
        const std::string* def = props.get("profile." + profile);
        if (!def || depth > 8) return;
        for (const std::string& item : splitWhitespace(*def)) {
            if (item.compare(0, 8, "profile.") == 0) applyProfile(item.substr(8), depth + 1);
            else if (item[0] == '!') setOption(item.substr(1), "false");
            else if (size_t eq = item.find('='); eq != std::string::npos) setOption(item.substr(0, eq), item.substr(eq + 1));
            else if (size_t col = item.find(':'); col != std::string::npos) setOption(item.substr(0, col), item.substr(col + 1));
            else setOption(item, "true");
        }
    };
    if (!opt.profile.empty()) applyProfile(opt.profile, 0);
    for (const auto& [k, v] : opt.optionValues) setOption(k, v);
    if (!options.values.empty()) readProperties();  // conditions may depend on the options
    {  // property values may name options (size.buffer.colortex1 = REFLECTION_RES REFLECTION_RES)
        MacroTable programMacros = macros;
        optionMacros(macros);
        parseProperties();
        macros.swap(programMacros);
    }

    // Lang file (option labels for the UI).
    for (const char* lf : {"lang/en_us.lang", "lang/en_US.lang"}) {
        std::string text;
        if (!source.read(lf, text)) continue;
        Properties lp;
        lp.parse(text);
        for (auto& [k, v] : lp.entries) lang[k] = v;
        break;
    }

    // Programs. Iris draws geometry whose fallback chain ends without a program with its internal shaders (the
    // vanilla look into colortex0): the same here as built-in gbuffers_basic / gbuffers_textured.
    {
        static const char* kVsh = R"(#version 120
varying vec2 texcoord;
varying vec2 lmcoord;
varying vec4 glcolor;
void main() {
    gl_Position = ftransform();
    texcoord = (gl_TextureMatrix[0] * gl_MultiTexCoord0).xy;
    lmcoord = (gl_TextureMatrix[1] * gl_MultiTexCoord1).xy;
    glcolor = gl_Color;
}
)";
        static const char* kBasicFsh = R"(#version 120
uniform sampler2D lightmap;
varying vec2 lmcoord;
varying vec4 glcolor;
void main() {
    vec4 color = glcolor * texture2D(lightmap, lmcoord);
/* DRAWBUFFERS:0 */
    gl_FragData[0] = color;
}
)";
        static const char* kTexturedFsh = R"(#version 120
uniform sampler2D gtexture;
uniform sampler2D lightmap;
varying vec2 texcoord;
varying vec2 lmcoord;
varying vec4 glcolor;
void main() {
    vec4 color = texture2D(gtexture, texcoord) * glcolor * texture2D(lightmap, lmcoord);
    if (color.a < 0.1) discard;
/* DRAWBUFFERS:0 */
    gl_FragData[0] = color;
}
)";
        auto has = [&](const char* name) {
            return source.exists(dimensionDir + name + ".vsh") || source.exists(dimensionDir + name + ".fsh");
        };
        const bool basic = has("gbuffers_basic"), textured = has("gbuffers_textured");
        if (!basic) {
            source.addVirtual(dimensionDir + "gbuffers_basic.vsh", kVsh);
            source.addVirtual(dimensionDir + "gbuffers_basic.fsh", kBasicFsh);
        }
        if (!basic && !textured) {
            source.addVirtual(dimensionDir + "gbuffers_textured.vsh", kVsh);
            source.addVirtual(dimensionDir + "gbuffers_textured.fsh", kTexturedFsh);
        }
    }
    std::vector<std::function<void()>> jobs;
    for (size_t g = 0; g < size_t(Gb::Count); ++g) {
        const std::string base = dimensionDir + kGbNames[g];
        if (!source.exists(base + ".vsh") && !source.exists(base + ".fsh")) continue;
        gbuffers[g].emplace();
        gbuffers[g]->name = kGbNames[g];
        loadProgram(*gbuffers[g], base, false, jobs);  // jobs keep references: the optionals do not move
    }
    // Composite families: prefix, prefix1..99 (fragment) and prefix[N].csh, prefix[N]_a..z.csh (compute).
    std::vector<std::function<void()>> famJobs;
    auto family = [&](const std::string& prefix, std::vector<Program>& out, bool computeOnly) {
        for (int idx = 0; idx < 100; ++idx) {
            std::string name = idx == 0 ? prefix : prefix + std::to_string(idx);
            const std::string base = dimensionDir + name;
            bool frag = !computeOnly && (source.exists(base + ".fsh"));
            std::vector<std::string> cs;
            if (source.exists(base + ".csh")) cs.push_back(name);
            for (char c = 'a'; c <= 'z'; ++c)
                if (source.exists(base + "_" + c + ".csh")) cs.push_back(name + "_" + c);
            if (!frag && cs.empty()) continue;
            Program p;
            p.name = name;
            if (!frag) p.enabled = true;
            out.push_back(std::move(p));
            if (!cs.empty()) {
                auto& list = computes[name];
                for (const std::string& c : cs) {
                    Program cp;
                    cp.name = c;
                    list.push_back(std::move(cp));
                }
            }
        }
    };
    family("setup", setup, true);
    family("begin", begin, false);
    family("shadowcomp", shadowcomp, false);
    family("prepare", prepare, false);
    family("deferred", deferred, false);
    family("composite", composite, false);
    if (source.exists(dimensionDir + "final.fsh")) {
        final_.emplace();
        final_->name = "final";
    }
    // Now that the containers are final, queue their stages.
    for (auto* list : {&setup, &begin, &shadowcomp, &prepare, &deferred, &composite})
        for (Program& p : *list)
            if (source.exists(dimensionDir + p.name + ".fsh")) loadProgram(p, dimensionDir + p.name, false, jobs);
    if (final_) loadProgram(*final_, dimensionDir + "final", false, jobs);
    for (auto& [name, list] : computes)
        for (Program& cp : list) loadProgram(cp, dimensionDir + cp.name, true, jobs);

    // program.<dir>/<name>.enabled / program.<name>.enabled
    // program.*.enabled: boolean expressions over options (booleans are 1 / 0, value options their values).
    MacroTable conditionMacros = macros;
    for (const auto& [name, d] : options.decls) {
        auto v = options.values.find(name);
        const std::string& value = v != options.values.end() ? v->second : d.value;
        defineMacro(conditionMacros, name, d.boolean ? (value == "true" ? "1" : "0") : value);
    }
    auto enabled = [&](Program& p) {
        for (const std::string& key : {"program." + dimensionDir + p.name + ".enabled", "program." + p.name + ".enabled"})
            if (const std::string* v = props.get(key)) {
                p.enabled = truthy(*v, conditionMacros);
                return;
            }
    };
    for (auto& g : gbuffers)
        if (g) enabled(*g);
    for (auto* list : {&setup, &begin, &shadowcomp, &prepare, &deferred, &composite})
        for (Program& p : *list) enabled(p);
    if (final_) enabled(*final_);
    for (auto& [name, list] : computes)
        for (Program& cp : list) enabled(cp);
    // blend / alphaTest / flip / scale
    auto programProps = [&](Program& p) {
        if (const std::string* v = props.get("blend." + p.name)) {
            BlendMode bm;
            if (parseBlend(*v, bm)) p.blend = bm;
        }
        if (const std::string* v = props.get("alphaTest." + p.name)) {
            AlphaTest at;
            if (parseAlphaTest(*v, at)) p.alphaTest = at;
        }
        if (const std::string* v = props.get("scale." + p.name)) {
            auto parts = splitWhitespace(*v);
            for (size_t k = 0; k < parts.size() && k < 3; ++k) p.scale[k] = float(std::atof(parts[k].c_str()));
        }
        const std::string blendPrefix = "blend." + p.name + ".", flipPrefix = "flip." + p.name + ".";
        for (const auto& [k, v] : props.entries) {
            if (k.compare(0, blendPrefix.size(), blendPrefix) == 0) {
                int idx = bufferIndex(k.substr(blendPrefix.size()));
                BlendMode bm;
                if (idx >= 0 && parseBlend(v, bm)) p.bufferBlend[idx] = bm;
            } else if (k.compare(0, flipPrefix.size(), flipPrefix) == 0) {
                int idx = bufferIndex(k.substr(flipPrefix.size()));
                if (idx >= 0) p.flip[idx] = v == "true";
            }
        }
    };
    for (auto& g : gbuffers)
        if (g) programProps(*g);
    for (auto* list : {&begin, &shadowcomp, &prepare, &deferred, &composite})
        for (Program& p : *list) programProps(p);
    if (final_) programProps(*final_);

    if (opt.compileHlsl) {
        std::atomic<size_t> next{0};
        unsigned n = opt.threads ? opt.threads : std::max(1u, std::thread::hardware_concurrency());
        n = std::min<unsigned>(n, unsigned(jobs.size()));
        std::vector<std::thread> workers;
        for (unsigned t = 0; t < n; ++t)
            workers.emplace_back([&] {
                for (size_t i; (i = next++) < jobs.size();) jobs[i]();
            });
        for (auto& w : workers) w.join();
    }
    std::string firstError;
    auto count = [&](const Program& p) {
        if (!p.enabled) return;
        for (const Stage* st : {&p.vs, &p.fs, &p.cs}) {
            if (!st->present) continue;
            if (st->error.empty()) {
                ++stagesCompiled;
            } else {
                ++stagesFailed;
                if (firstError.empty())
                    firstError = p.name + (st == &p.vs ? ".vsh" : st == &p.fs ? ".fsh" : ".csh") + ": " +
                                 st->error.substr(0, st->error.find('\n'));
            }
        }
    };
    for (auto& g : gbuffers)
        if (g) count(*g);
    for (auto* list : {&setup, &begin, &shadowcomp, &prepare, &deferred, &composite})
        for (Program& p : *list) count(p);
    if (final_) count(*final_);
    for (auto& [name, list] : computes)
        for (Program& cp : list) count(cp);
    loadMs = timer.ms();
    if (opt.compileHlsl && opt.failOnErrors && stagesFailed) {  // like Iris: a program that does not compile fails the pack
        error = std::to_string(stagesFailed) + " shader stage(s) failed; " + firstError;
        return false;
    }
    return true;
}

// ---- id maps -----------------------------------------------------------------------------------------------------

bool loadIdMap(const PackSource& src, const std::string& file, const MacroTable& macros, const std::string& prefix,
               std::vector<IdMapEntry>& out) {
    std::string text;
    if (!src.read(file, text)) return false;
    Included inc;
    expandIncludesText(src, file, text, inc);
    MacroTable m = macros;
    Preprocessed pp;
    if (!preprocess(inc.lines, m, pp)) pp.text = text;
    Properties p;
    p.parse(pp.text);
    for (const auto& [key, value] : p.entries) {
        if (key.compare(0, prefix.size(), prefix) != 0) continue;
        int id = std::atoi(key.c_str() + prefix.size());
        for (const std::string& item : splitWhitespace(value)) {
            IdMapEntry e;
            e.id = id;
            // [namespace:]name[:prop=v1,v2[:prop2=...]]
            std::vector<std::string> parts;
            size_t i = 0;
            while (i <= item.size()) {
                size_t j = item.find(':', i);
                if (j == std::string::npos) j = item.size();
                parts.push_back(item.substr(i, j - i));
                i = j + 1;
            }
            size_t k = 0;
            if (parts.size() >= 2 && parts[1].find('=') == std::string::npos) {
                e.name = parts[0] + ":" + parts[1];
                k = 2;
            } else {
                e.name = "minecraft:" + parts[0];
                k = 1;
            }
            for (; k < parts.size(); ++k) {
                size_t eq = parts[k].find('=');
                if (eq == std::string::npos) continue;
                std::vector<std::string> vals;
                std::string vs = parts[k].substr(eq + 1);
                size_t a = 0;
                while (a <= vs.size()) {
                    size_t b = vs.find(',', a);
                    if (b == std::string::npos) b = vs.size();
                    vals.push_back(vs.substr(a, b - a));
                    a = b + 1;
                }
                e.states.emplace_back(parts[k].substr(0, eq), std::move(vals));
            }
            out.push_back(std::move(e));
        }
    }
    return true;
}

}  // namespace packs
