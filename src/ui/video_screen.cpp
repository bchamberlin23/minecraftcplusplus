// Video Settings (VideoSettingsScreen: Display, Quality & Performance with the graphics presets, Preferences) and
// this client's own page for the rendering features the game does not have, reached from Quality & Performance:
// Shaders & Upscaling (shader preset and packs, temporal upscaler TAA / NVIDIA DLSS / AMD FSR / Intel XeSS with the
// vendor's quality modes, sharpness, frame generation, low-latency mode). Only what this GPU and driver support is
// offered (render/upscalers/upscalers.h). Every change applies immediately (the renderer, terrain, sky and HUD read
// the options each frame: ui/graphics_options.cpp); options.txt keeps it.
#include "render/packs/pack_api.h"
#include "render/renderer.h"
#include "render/upscalers/upscalers.h"
#include "ui/frontend.h"
#include "ui/options_list.h"
#include "world/terrain.h"

#include <cmath>
#include <json/json.hpp>
#include <memory>

namespace ui {

namespace {

constexpr u32 kFpsSteps[] = {30, 40, 45, 50, 60, 72, 75, 90, 100, 120, 144, 165, 180, 200, 240, 300, 360, 0};  // 0 = off

const char* presetName(u8 p) {
    static const char* n[] = {"Off", "Low", "Medium", "High", "Ultra", "Path Traced"};
    return n[std::min<int>(p, 5)];
}

std::string frameGenLabel(const VideoSettings& v) {
    switch (v.frameGen) {
        case FrameGenKind::Dlss: return "DLSS " + std::to_string(v.frameGenMultiplier) + "x";
        case FrameGenKind::Fsr: return "FSR 2x";
        case FrameGenKind::Xess: return "XeSS 2x";
        default: return "Off";
    }
}

// Frame generation choices this machine offers, in menu order.
std::vector<std::pair<FrameGenKind, u32>> frameGenChoices(const upscale::Caps& caps) {
    std::vector<std::pair<FrameGenKind, u32>> v{{FrameGenKind::Off, 2}};
    if (caps.frameGen[int(FrameGenKind::Dlss)])
        for (u32 m = 2; m <= caps.maxMultiplier[int(FrameGenKind::Dlss)]; ++m) v.push_back({FrameGenKind::Dlss, m});
    if (caps.frameGen[int(FrameGenKind::Fsr)]) v.push_back({FrameGenKind::Fsr, 2});
    if (caps.frameGen[int(FrameGenKind::Xess)]) v.push_back({FrameGenKind::Xess, 2});
    return v;
}

Text value(UiState& s, const std::string& caption, const std::string& v) {
    return translatable("options.generic_value", {Text(caption), Text(v)}, s.lang());
}

// --- Video Settings ----------------------------------------------------------------------------------------------

// Options.setGraphicsPresetToCustom: any quality option changed by hand.
void custom(UiState& s) { s.settings.graphicsPreset = 3; }

// GraphicsPreset.apply: the Quality & Performance options of Fast / Fancy / Fabulous (Custom changes nothing).
void applyPreset(UiState& s, int preset) {
    Settings& o = s.settings;
    const int maxRd = std::max(2, s.init.detailChunks);
    auto rd = [&](int d) {
        o.renderDistance = std::min(d, maxRd);
        s.terrain->setDetailRadius(float(o.renderDistance) * 16.0f);
    };
    switch (preset) {
        case 0:
            o.biomeBlendRadius = 1, rd(8), o.prioritizeChunkUpdates = 0, o.simulationDistance = 6, o.ao = false;
            o.cloudStatus = 1, o.particles = 1, o.mipmapLevels = 2, o.entityShadows = false, o.entityDistanceScaling = 0.75;
            o.menuBackgroundBlurriness = 2, o.cloudRange = 32, o.cutoutLeaves = false, o.improvedTransparency = false;
            o.weatherRadius = 5, o.maxAnisotropyBit = 1, o.textureFiltering = 0;
            break;
        case 1:
            o.biomeBlendRadius = 2, rd(16), o.prioritizeChunkUpdates = 1, o.simulationDistance = 12, o.ao = true;
            o.cloudStatus = 2, o.particles = 0, o.mipmapLevels = 4, o.entityShadows = true, o.entityDistanceScaling = 1.0;
            o.menuBackgroundBlurriness = 5, o.cloudRange = 64, o.cutoutLeaves = true, o.improvedTransparency = false;
            o.weatherRadius = 10, o.maxAnisotropyBit = 1, o.textureFiltering = 1;
            break;
        case 2:
            o.biomeBlendRadius = 2, rd(32), o.prioritizeChunkUpdates = 1, o.simulationDistance = 12, o.ao = true;
            o.cloudStatus = 2, o.particles = 0, o.mipmapLevels = 4, o.entityShadows = true, o.entityDistanceScaling = 1.25;
            o.menuBackgroundBlurriness = 5, o.cloudRange = 128, o.cutoutLeaves = true, o.improvedTransparency = true;
            o.weatherRadius = 10, o.maxAnisotropyBit = 2, o.textureFiltering = 2;
            break;
        default: break;
    }
    o.graphicsPreset = preset;
}

// A boolean quality option (CycleButton.onOffBuilder): a change makes the preset Custom.
Button qualityOnOff(UiState& s, const char* key, bool* v, const char* tooltip = nullptr) {
    UiState* p = &s;
    return onOffButton(s, key, v, [p] { custom(*p); }, tooltip);
}

}  // namespace

bool parseVideoMode(const std::string& json, Window::VideoMode& out) {
    if (json.empty()) return false;
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_object()) return false;
    try {
        out.width = j.at("width").get<int>(), out.height = j.at("height").get<int>();
        out.redBits = j.at("red_bits").get<int>(), out.greenBits = j.at("green_bits").get<int>(), out.blueBits = j.at("blue_bits").get<int>();
        out.refreshRate = j.at("refresh_rate").get<float>();
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

std::string writeVideoMode(const Window::VideoMode& m) {  // Gson: floats with a fraction (60.0)
    char rate[32];
    if (m.refreshRate == std::rint(m.refreshRate)) std::snprintf(rate, sizeof(rate), "%d.0", int(m.refreshRate));
    else std::snprintf(rate, sizeof(rate), "%g", double(m.refreshRate));
    return "{\"width\":" + std::to_string(m.width) + ",\"height\":" + std::to_string(m.height) + ",\"red_bits\":" +
           std::to_string(m.redBits) + ",\"green_bits\":" + std::to_string(m.greenBits) + ",\"blue_bits\":" +
           std::to_string(m.blueBits) + ",\"refresh_rate\":" + rate + "}";
}

namespace {

void videoBuild(UiState& s) {
    OptionsList l(s, Text(s.res.translate("options.videoTitle")), Screen::Options);
    UiState* p = &s;
    Settings& o = s.settings;
    l.header(Text(s.res.translate("options.video.display.header")));
    {  // Exclusive Fullscreen Mode: IntRange(-1, modes - 1) over the monitor's video modes, -1 its current one
        const std::vector<Window::VideoMode> modes = s.window->videoModes();
        int initial = -1;
        Window::VideoMode pref;
        if (parseVideoMode(o.fullscreenResolution, pref))
            if (auto it = std::find(modes.begin(), modes.end(), pref); it != modes.end()) initial = int(it - modes.begin());
        const int n = int(modes.size());
        auto label = [p, modes](int value) {
            if (modes.empty()) return Text(p->res.translate("options.fullscreen.unavailable"));
            if (value < 0) return optionLabel(*p, "options.fullscreen.exclusive.mode", Text(p->res.translate("options.fullscreen.current")));
            const Window::VideoMode& m = modes[size_t(value)];
            char rate[32];  // VideoMode.refreshRateLabel
            if (m.refreshRate == std::rint(m.refreshRate)) std::snprintf(rate, sizeof(rate), "%d", int(m.refreshRate));
            else std::snprintf(rate, sizeof(rate), "%.2f", double(m.refreshRate));
            return optionLabel(*p, "options.fullscreen.exclusive.mode",
                               translatable("options.fullscreen.entry", {Text(std::to_string(m.width)), Text(std::to_string(m.height)), Text(rate),
                                                                         Text(std::to_string(m.redBits + m.greenBits + m.blueBits))},
                                            p->lang()));
        };
        auto current = std::make_shared<int>(initial);
        Button mode = sliderButton(n ? float(initial + 1) / float(n) : 0.0f, [label, current] { return label(*current); },
                                   [p, modes, n, current](float v) {
                                       const int value = n ? int(std::lround(v * float(n))) - 1 : -1;
                                       if (value == *current) return;
                                       *current = value;
                                       p->settings.fullscreenResolution = value < 0 ? std::string() : writeVideoMode(modes[size_t(value)]);
                                       p->window->setPreferredVideoMode(value < 0 ? nullptr : &modes[size_t(value)]);
                                   });
        mode.active = n > 0;
        mode.tooltip = [p] { return Text(p->res.translate("options.fullscreen.exclusive.mode.tooltip")); };
        l.big(std::move(mode));
    }
    std::vector<Button> display;
    {  // Max Framerate (our steps include common refresh rates) and VSync
        const u32 cur = s.init.renderer->upscalers().settings().fpsLimit;
        int idx = int(std::size(kFpsSteps)) - 1;
        for (int i = 0; i < int(std::size(kFpsSteps)); ++i)
            if (kFpsSteps[i] == cur) idx = i;
        display.push_back(sliderButton(float(idx) / float(std::size(kFpsSteps) - 1),
                                       [p] {
                                           const u32 f = p->init.renderer->upscalers().settings().fpsLimit;
                                           const Text v = f ? Text(formatJava(p->res.translate("options.framerate"), {std::to_string(f)}))
                                                            : Text(p->res.translate("options.framerateLimit.max"));
                                           return optionLabel(*p, "options.framerateLimit", v);
                                       },
                                       [p](float v) {
                                           const u32 f = kFpsSteps[size_t(std::lround(v * float(std::size(kFpsSteps) - 1)))];
                                           VideoSettings vs = p->init.renderer->upscalers().settings();
                                           if (vs.fpsLimit != f) {
                                               vs.fpsLimit = f;
                                               p->settings.video = p->init.renderer->upscalers().apply(vs);
                                               p->settings.videoSaved = true;
                                           }
                                       }));
        display.push_back(cycleButton(
            [p] {
                return optionLabel(*p, "options.vsync",
                                   Text(p->res.translate(p->init.renderer->upscalers().settings().vsync ? "options.on" : "options.off")));
            },
            [p] {
                VideoSettings v = p->init.renderer->upscalers().settings();
                v.vsync = !v.vsync;
                p->applyVideo(v);
                p->settings.save();
            }));
    }
    display.push_back(enumButton(s, "options.inactivityFpsLimit", &o.inactivityFpsLimit,
                                 {"options.inactivityFpsLimit.minimized", "options.inactivityFpsLimit.afk"}, {}, [p] {
                                     return Text(p->res.translate(p->settings.inactivityFpsLimit == 0 ? "options.inactivityFpsLimit.minimized.tooltip"
                                                                                                      : "options.inactivityFpsLimit.afk.tooltip"));
                                 }));
    // GUI Scale (Auto, then 1 .. the largest this window allows) and Fullscreen (borderless, this monitor)
    display.push_back(cycleButton(
        [p] {
            return optionLabel(*p, "options.guiScale",
                               Text(p->settings.guiScale == 0 ? p->res.translate("options.guiScale.auto") : std::to_string(p->settings.guiScale)));
        },
        [p] {
            int maxScale = 1;
            const u32 w = p->window->width(), h = p->window->height();
            while (w / u32(maxScale + 1) >= 320 && h / u32(maxScale + 1) >= 240) ++maxScale;
            if (p->settings.forceUnicodeFont && maxScale % 2 != 0) ++maxScale;  // Window.calculateScale(0, unicode): even
            p->settings.guiScale = p->settings.guiScale >= maxScale ? 0 : p->settings.guiScale + 1;
            p->settings.save();
        }));
    display.push_back(cycleButton(
        [p] { return optionLabel(*p, "options.fullscreen", Text(p->res.translate(p->window->fullscreen() ? "options.on" : "options.off"))); },
        [p] { p->toggleFullscreen(); }));
    {
        Button exclusive = onOffButton(s, "options.exclusiveFullscreen", &o.exclusiveFullscreen,
                                       [p] { p->window->setExclusiveFullscreen(p->settings.exclusiveFullscreen); });
        exclusive.tooltip = [p] {
            return Text(p->res.translate(p->settings.exclusiveFullscreen ? "options.exclusiveFullscreen.on.tooltip" : "options.exclusiveFullscreen.off.tooltip"));
        };
        display.push_back(std::move(exclusive));
    }
    display.push_back(unitSlider(s, &o.gamma, [p] {  // Brightness: Moody .. Default .. Bright
        const int v = int(p->settings.gamma * 100.0);
        if (v == 0) return optionLabel(*p, "options.gamma", Text(p->res.translate("options.gamma.min")));
        if (v == 50) return optionLabel(*p, "options.gamma", Text(p->res.translate("options.gamma.default")));
        if (v == 100) return optionLabel(*p, "options.gamma", Text(p->res.translate("options.gamma.max")));
        return optionLabel(*p, "options.gamma", Text(std::to_string(v)));
    }));
    {  // Graphics API: this client renders with Direct3D 12 only
        Button api;
        api.label = optionLabel(s, "options.graphicsApi", Text("Direct3D 12"));
        api.active = false;
        api.tooltip = tooltipKey(s, "options.graphicsApi.tooltip");
        display.push_back(std::move(api));
    }
    l.halves(std::move(display));

    l.header(Text(s.res.translate("options.video.quality.header")));
    {  // Preset: a slider over Fast, Fancy, Fabulous!, Custom
        static const char* const kPresets[4] = {"options.graphics.fast", "options.graphics.fancy", "options.graphics.fabulous",
                                                "options.graphics.custom"};
        Button preset = sliderButton(float(std::clamp(o.graphicsPreset, 0, 3)) / 3.0f,
                                     [p] { return optionLabel(*p, "options.graphics.preset", Text(p->res.translate(kPresets[std::clamp(p->settings.graphicsPreset, 0, 3)]))); },
                                     [p](float v) {
                                         const int i = std::clamp(int(std::floor((v >= 1.0f ? 0.99999f : v) * 4.0f)), 0, 3);
                                         if (i == p->settings.graphicsPreset) return;
                                         applyPreset(*p, i);
                                         p->settings.save();
                                         const int dragging = p->draggingSlider;
                                         p->buildButtons();  // the other widgets show the preset's values
                                         p->draggingSlider = dragging;
                                     });
        preset.tooltip = tooltipKey(s, "options.graphics.preset.tooltip");
        l.big(std::move(preset));
    }
    std::vector<Button> q;
    q.push_back(rangeSlider(s, 0, 7, [p] { return p->settings.biomeBlendRadius; },
                            [p](int v) { p->settings.biomeBlendRadius = v, custom(*p); },
                            [p] {
                                return optionLabel(*p, "options.biomeBlendRadius",
                                                   Text(p->res.translate("options.biomeBlendRadius." + std::to_string(p->settings.biomeBlendRadius * 2 + 1))));
                            }));
    {  // Render Distance: up to the launch detail radius (it sized the geometry arena)
        const int maxRd = std::max(2, s.init.detailChunks);
        q.push_back(rangeSlider(s, 2, maxRd, [p] { return p->settings.renderDistance; },
                                [p](int v) {
                                    p->settings.renderDistance = v, custom(*p);
                                    p->terrain->setDetailRadius(float(v) * 16.0f);
                                },
                                [p, maxRd] {
                                    return optionLabel(*p, "options.renderDistance",
                                                       Text(formatJava(p->res.translate("options.chunks"),
                                                                       {std::to_string(std::min(p->settings.renderDistance, maxRd))})));
                                }));
    }
    {
        static const char* const kTips[3] = {"options.prioritizeChunkUpdates.none.tooltip", "options.prioritizeChunkUpdates.byPlayer.tooltip",
                                             "options.prioritizeChunkUpdates.nearby.tooltip"};
        q.push_back(enumButton(s, "options.prioritizeChunkUpdates", &o.prioritizeChunkUpdates,
                               {"options.prioritizeChunkUpdates.none", "options.prioritizeChunkUpdates.byPlayer", "options.prioritizeChunkUpdates.nearby"},
                               [p] { custom(*p); }, [p] { return Text(p->res.translate(kTips[std::clamp(p->settings.prioritizeChunkUpdates, 0, 2)])); }));
    }
    {  // Simulation Distance: the local server's (it takes the value when the world starts)
        Button sim = rangeSlider(s, 5, 32, [p] { return p->settings.simulationDistance; },
                                 [p](int v) { p->settings.simulationDistance = v, custom(*p); },
                                 [p] {
                                     return optionLabel(*p, "options.simulationDistance",
                                                        Text(formatJava(p->res.translate("options.chunks"), {std::to_string(p->settings.simulationDistance)})));
                                 });
        if (s.online()) sim.tooltip = [] { return Text("Applies the next time this world is opened"); };
        q.push_back(std::move(sim));
    }
    q.push_back(qualityOnOff(s, "options.ao", &o.ao));
    q.push_back(enumButton(s, "options.renderClouds", &o.cloudStatus, {"options.off", "options.clouds.fast", "options.clouds.fancy"},
                           [p] { custom(*p); }));
    q.push_back(enumButton(s, "options.particles", &o.particles, {"options.particles.all", "options.particles.decreased", "options.particles.minimal"},
                           [p] { custom(*p); }));
    q.push_back(rangeSlider(s, 0, 4, [p] { return p->settings.mipmapLevels; }, [p](int v) { p->settings.mipmapLevels = v, custom(*p); },
                            [p] {
                                const int v = p->settings.mipmapLevels;
                                return optionLabel(*p, "options.mipmapLevels", Text(v == 0 ? p->res.translate("options.off") : std::to_string(v)));
                            }));
    q.push_back(qualityOnOff(s, "options.entityShadows", &o.entityShadows));
    q.push_back(rangeSlider(s, 2, 20, [p] { return int(p->settings.entityDistanceScaling * 4.0); },
                            [p](int v) { p->settings.entityDistanceScaling = v / 4.0, custom(*p); },
                            [p] { return percentLabel(*p, "options.entityDistanceScaling", p->settings.entityDistanceScaling); }));
    {
        Button blur = rangeSlider(s, 0, 10, [p] { return p->settings.menuBackgroundBlurriness; },
                                  [p](int v) { p->settings.menuBackgroundBlurriness = v, custom(*p); },
                                  [p] {
                                      const int b = p->settings.menuBackgroundBlurriness;
                                      return optionLabel(*p, "options.accessibility.menu_background_blurriness",
                                                         Text(b == 0 ? p->res.translate("options.off") : std::to_string(b)));
                                  });
        blur.tooltip = tooltipKey(s, "options.accessibility.menu_background_blurriness.tooltip");
        q.push_back(std::move(blur));
    }
    q.push_back(rangeSlider(s, 2, 128, [p] { return p->settings.cloudRange; }, [p](int v) { p->settings.cloudRange = v, custom(*p); },
                            [p] {
                                return optionLabel(*p, "options.renderCloudsDistance",
                                                   Text(formatJava(p->res.translate("options.chunks"), {std::to_string(p->settings.cloudRange)})));
                            }));
    q.push_back(qualityOnOff(s, "options.cutoutLeaves", &o.cutoutLeaves, "options.cutoutLeaves.tooltip"));
    q.push_back(qualityOnOff(s, "options.improvedTransparency", &o.improvedTransparency, "options.improvedTransparency.oit.tooltip"));
    {
        static const char* const kTips[3] = {"options.textureFiltering.none.tooltip", "options.textureFiltering.rgss.tooltip",
                                             "options.textureFiltering.anisotropic.tooltip"};
        q.push_back(enumButton(s, "options.textureFiltering", &o.textureFiltering,
                               {"options.textureFiltering.none", "options.textureFiltering.rgss", "options.textureFiltering.anisotropic"},
                               [p] {
                                   custom(*p);
                                   p->buildButtons();  // Anisotropic Filtering is only active with Anisotropic
                               },
                               [p] { return Text(p->res.translate(kTips[std::clamp(p->settings.textureFiltering, 0, 2)])); }));
    }
    {
        Button aniso = rangeSlider(s, 1, 3, [p] { return p->settings.maxAnisotropyBit; }, [p](int v) { p->settings.maxAnisotropyBit = v, custom(*p); },
                                   [p] {
                                       return optionLabel(*p, "options.maxAnisotropy",
                                                          Text(formatJava(p->res.translate("options.multiplier"),
                                                                          {std::to_string(1 << p->settings.maxAnisotropyBit)})));
                                   });
        aniso.tooltip = tooltipKey(s, "options.maxAnisotropy.tooltip");
        aniso.active = o.textureFiltering == 2;
        q.push_back(std::move(aniso));
    }
    {
        Button weather = rangeSlider(s, 3, 10, [p] { return p->settings.weatherRadius; }, [p](int v) { p->settings.weatherRadius = v, custom(*p); },
                                     [p] {
                                         return optionLabel(*p, "options.weatherRadius",
                                                            Text(formatJava(p->res.translate("options.blocks"), {std::to_string(p->settings.weatherRadius)})));
                                     });
        weather.tooltip = tooltipKey(s, "options.weatherRadius.tooltip");
        q.push_back(std::move(weather));
    }
    l.halves(std::move(q));
    {  // this client's own rendering features, after the game's quality options
        Button extra = screenButton(s, "", Screen::VideoExtra);
        extra.label = Text("Shaders & Upscaling...");
        extra.tooltip = [] { return Text("Shader preset and shader packs, DLSS / FSR / XeSS upscaling, frame generation and low latency"); };
        l.big(std::move(extra));
    }

    l.header(Text(s.res.translate("options.video.preferences.header")));
    std::vector<Button> prefs;
    prefs.push_back(onOffButton(s, "options.autosaveIndicator", &o.showAutosaveIndicator));
    prefs.push_back(onOffButton(s, "options.vignette", &o.vignette, {}, "options.vignette.tooltip"));
    prefs.push_back(enumButton(s, "options.attackIndicator", &o.attackIndicator, {"options.off", "options.attack.crosshair", "options.attack.hotbar"}));
    {
        Button fade = rangeSlider(s, 0, 40, [p] { return int(p->settings.chunkFade * 20.0); }, [p](int v) { p->settings.chunkFade = v / 20.0; },
                                  [p] {
                                      if (p->settings.chunkFade <= 0.0) return Text(p->res.translate("options.chunkFade.none"));
                                      char buf[16];
                                      std::snprintf(buf, sizeof(buf), "%.2f", p->settings.chunkFade);
                                      return Text(formatJava(p->res.translate("options.chunkFade.seconds"), {buf}));
                                  });
        fade.tooltip = tooltipKey(s, "options.chunkFade.tooltip");
        prefs.push_back(std::move(fade));
    }
    l.halves(std::move(prefs));
    l.footer();
}

// --- Shaders & Upscaling --------------------------------------------------------------------------------------------

void extraBuild(UiState& s) {
    OptionsList l(s, Text("Shaders & Upscaling"), Screen::Video);
    UiState* p = &s;
    Upscalers& up = s.init.renderer->upscalers();
    const upscale::Caps& caps = up.caps();
    VideoSettings cur = up.settings();  // what is in effect (may include command-line overrides)
    cur.renderScaleOverride = 0;
    const bool shaders = cur.shaders != 0;
    const bool upscaling = shaders || cur.plainAntialiasing;  // Shaders Off upscales with anti-aliasing on
    {  // the shader pack's state under the title
        const auto st = packs::packStatus(*s.init.renderer);
        if (!st.loading.empty()) l.subtitle(Text("Loading shader pack " + st.loading + "..."), 0xFFFFD060);
        else if (!st.error.empty()) l.subtitle(Text("Shader pack error (built-in pipeline in use): " + st.error), 0xFFFF6060);
        else if (!s.packMessage.empty()) l.subtitle(Text(s.packMessage), 0xFFFF6060);
    }
    const std::function<Text()> needsShaders = [] { return Text("Needs the shader pipeline (Shaders: Low or higher)"); };

    l.header(Text("Shaders"));
    const bool ptOk = s.init.renderer->pathTracingSupported();
    Button preset = cycleButton([p, cur] { return value(*p, "Shaders", presetName(cur.shaders)); },
                                [p, cur, ptOk]() mutable {
                                    cur.shaders = u8((cur.shaders + 1) % (ptOk ? 6 : 5));
                                    p->applyVideo(cur);
                                    p->settings.save();
                                },
                                [p] {
                                    std::string why, t = "The built-in renderer's lighting and effects. Off is the plain look (anti-aliasing "
                                                         "and upscaling still apply); frame generation needs Low or higher.";
                                    if (p->init.renderer->pathTracingSupported(&why))
                                        t += "\nPath Traced: ray traced light, shadows and reflections near you (DirectX Raytracing).";
                                    else
                                        t += "\nPath Traced " + why + ".";
                                    return Text(t);
                                });
    const auto packList = packs::listPacks();
    Button pack = cycleButton(
        [p] {
            std::string shown = p->settings.shaderPack.empty() ? "Built-in" : p->settings.shaderPack;
            if (shown.size() > 18) shown = shown.substr(0, 16) + "..";
            return value(*p, "Shader Pack", shown);
        },
        [p, packList] {
            size_t i = 0;  // 0 = built-in, i = packList[i - 1]
            for (size_t k = 0; k < packList.size(); ++k)
                if (packList[k].name == p->settings.shaderPack) i = k + 1;
            i = (i + 1) % (packList.size() + 1);
            p->settings.shaderPack = i ? packList[i - 1].name : std::string();
            packs::selectPack(*p->init.renderer, p->settings.shaderPack);
            p->settings.save();
            p->buildButtons();
        },
        [p] {
            const auto st = packs::packStatus(*p->init.renderer);
            std::string t = "OptiFine / Iris shader packs from the shaderpacks folder, switched without a restart.";
            if (!st.active.empty()) t += "\nIn use: " + st.active + ". With DLSS / FSR / XeSS, turn the pack's own TAA off.";
            return Text(t);
        });
    l.half(std::move(preset), std::move(pack));
    {
        Button options = screenButton(s, "", Screen::None, !s.settings.shaderPack.empty());
        options.label = Text("Shader Pack Options...");
        options.onPress = [p] { p->openPackOptions(); };
        l.half(std::move(options));
    }

    if (cur.shaders == u8(ShaderPreset::PathTraced)) {  // path tracing quality and its denoiser
        static const char* kQuality[] = {"Low", "Medium", "High", "Ultra"};
        Button quality = cycleButton(
            [p, cur] { return value(*p, "Path Tracing", kQuality[std::min<int>(cur.pathTracing, 3)]); },
            [p, cur]() mutable {
                cur.pathTracing = u8((cur.pathTracing + 1) % 4);
                p->applyVideo(cur);
                p->settings.save();
            },
            [p, cur] {
                ShaderSettings q;
                q.setPathTraceQuality(PathTraceQuality(cur.pathTracing));
                char b[200];
                std::snprintf(b, sizeof(b), "%u bounce%s, %u sample%s per pixel, traced within %.0f chunks; beyond that the world "
                              "is lit like Ultra.\n", q.ptBounces, q.ptBounces == 1 ? "" : "s", q.ptSamples,
                              q.ptSamples == 1 ? "" : "s", q.ptRadius / 16.0f);
                return Text(b + p->init.renderer->pathTracingStatus());
            });
        Upscalers& u = up;
        const bool rr = u.rayReconstruction() != nullptr;
        Button denoiser = cycleButton(
            [p, rr] { return value(*p, "Denoiser", rr ? "DLSS RR" : "Built-in"); },
            [] {},
            [p, rr, &u] {
                if (rr) return Text("DLSS Ray Reconstruction denoises and upscales the path traced image (Upscaler: DLSS).");
                std::string t = "Built-in temporal denoiser. ";
                t += u.rrWhy().empty() ? "Select the DLSS upscaler for DLSS Ray Reconstruction." : "DLSS Ray Reconstruction: " + u.rrWhy();
                return Text(t);
            });
        denoiser.active = false;  // follows the upscaler
        l.half(std::move(quality), std::move(denoiser));
    }

    l.header(Text("Anti-Aliasing & Upscaling"));
    Button aa = cycleButton([p, cur, shaders] {
                                return value(*p, "Anti-Aliasing", shaders || cur.plainAntialiasing ? "Temporal" : "Off");
                            },
                            [p, cur]() mutable {
                                cur.plainAntialiasing = !cur.plainAntialiasing;
                                p->applyVideo(cur);
                                p->settings.save();
                            },
                            [shaders] {
                                return Text(shaders ? "The shader pipeline always anti-aliases with the upscaler below."
                                                    : "Shaders Off: temporal anti-aliasing against shimmering edges and textures, "
                                                      "with the upscaler below (TAA, DLSS / DLAA, FSR, XeSS). Off: no anti-aliasing.");
                            });
    aa.active = !shaders;
    l.half(std::move(aa));
    Button upscaler = cycleButton([p, cur] { return value(*p, "Upscaler", Upscalers::upscalerName(cur.upscaler)); },
                                  [p, cur, &caps]() mutable {
                                      int k = int(cur.upscaler);
                                      do k = (k + 1) % int(UpscalerKind::Count);
                                      while (!caps.upscaler[k]);
                                      cur.upscaler = UpscalerKind(k);
                                      p->applyVideo(cur);
                                      p->settings.save();
                                  },
                                  [p, &caps] {  // availability of each vendor's upscaler and what is running
                                      Upscalers& u = p->init.renderer->upscalers();
                                      std::string t;
                                      for (int k = 1; k < int(UpscalerKind::Count); ++k) {
                                          const std::string name = Upscalers::upscalerName(UpscalerKind(k));
                                          t += name + (caps.upscaler[k] ? ": " + std::string(caps.version[k]) : ": unavailable - " + std::string(caps.why[k])) + "\n";
                                      }
                                      if (u.settings().shaders == 0 && !u.settings().plainAntialiasing)
                                          t += "Needs Anti-Aliasing (or Shaders: Low or higher)\n";
                                      return Text(t + u.status());
                                  });
    upscaler.active = upscaling;
    Button quality = cycleButton([p, cur, &up] { return value(*p, "Quality", up.qualityName(cur.upscaler, cur.quality)); },
                                 [p, cur, &up]() mutable {
                                     const auto modes = up.qualityModes(cur.upscaler);
                                     auto it = std::find(modes.begin(), modes.end(), cur.quality);
                                     cur.quality = it == modes.end() || it + 1 == modes.end() ? modes.front() : *(it + 1);
                                     p->applyVideo(cur);
                                     p->settings.save();
                                 },
                                 upscaling ? std::function<Text()>() : needsShaders);
    quality.active = upscaling;
    l.half(std::move(upscaler), std::move(quality));
    Button sharpness = sliderButton(cur.sharpness,
                                    [p] {
                                        const int pct = int(std::lround(p->init.renderer->upscalers().settings().sharpness * 100));
                                        return translatable("options.percent_value", {Text("Sharpness"), Text(std::to_string(pct))}, p->lang());
                                    },
                                    [p](float v) {
                                        VideoSettings vs = p->init.renderer->upscalers().settings();
                                        vs.sharpness = std::round(v * 20.0f) / 20.0f;
                                        if (vs.sharpness != p->settings.video.sharpness) {
                                            p->settings.video = p->init.renderer->upscalers().apply(vs);
                                            p->settings.videoSaved = true;
                                        }
                                    });
    sharpness.active = upscaling;
    l.half(std::move(sharpness));

    l.header(Text("Frame Generation & Latency"));
    const auto fgChoices = frameGenChoices(caps);
    Button fg = cycleButton([p, cur] { return value(*p, "Frame Generation", frameGenLabel(cur)); },
                            [p, cur, fgChoices]() mutable {
                                size_t i = 0;
                                for (; i < fgChoices.size(); ++i)
                                    if (fgChoices[i].first == cur.frameGen &&
                                        (cur.frameGen != FrameGenKind::Dlss || fgChoices[i].second == cur.frameGenMultiplier))
                                        break;
                                const auto& next = fgChoices[(i + 1) % fgChoices.size()];
                                cur.frameGen = next.first, cur.frameGenMultiplier = next.second;
                                p->applyVideo(cur);
                                p->settings.save();
                            },
                            [p, shaders] {
                                Upscalers& u = p->init.renderer->upscalers();
                                if (!shaders) return Text("Needs the shader pipeline (Shaders: Low or higher)");
                                if (u.frameGenActive() && u.lastFramesPresented() > 1)
                                    return Text(std::to_string(u.lastFramesPresented()) + " frames presented per rendered frame");
                                return Text("Generated frames between rendered ones (DLSS Multi Frame Generation, FSR, XeSS-FG)");
                            });
    fg.active = shaders && fgChoices.size() > 1;
    Button latency;
    {
        const bool any = caps.lowLatency[int(LowLatencyMode::On)];
        std::string name = caps.lowLatencyName[0] ? caps.lowLatencyName : "Low Latency";
        if (name.starts_with("NVIDIA ")) name = name.substr(7);
        if (name.starts_with("AMD ")) name = name.substr(4);
        std::string state = cur.lowLatency == LowLatencyMode::Boost ? "On+Boost" : cur.lowLatency == LowLatencyMode::On ? "On" : "Off";
        if (!any) name = "Low Latency", state = "n/a";
        if (cur.frameGen == FrameGenKind::Xess) name = "XeLL", state = "On (FG)";
        latency = cycleButton([p, name, state] { return value(*p, name, state); },
                              [p, cur, &caps]() mutable {
                                  int m = int(cur.lowLatency);
                                  do m = (m + 1) % int(LowLatencyMode::Count);
                                  while (!caps.lowLatency[m]);
                                  cur.lowLatency = LowLatencyMode(m);
                                  p->applyVideo(cur);
                                  p->settings.save();
                              });
        latency.active = any && cur.frameGen != FrameGenKind::Xess;
    }
    l.half(std::move(fg), std::move(latency));
    l.footer();
}

void listDraw(UiState& s, Canvas& c) { drawOptionsList(s, c); }

const ScreenRegistrar kVideo({Screen::Video, videoBuild, listDraw, optionsListInput, nullptr});
const ScreenRegistrar kExtra({Screen::VideoExtra, extraBuild, listDraw, optionsListInput, nullptr});

}  // namespace

void Ui::State::applyVideo(VideoSettings v) {
    Upscalers& up = init.renderer->upscalers();
    v.renderScaleOverride = 0;
    settings.video = up.apply(v);
    settings.videoSaved = true;
    buildButtons();
}

}  // namespace ui
