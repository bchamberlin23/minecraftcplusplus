// Video Settings (VideoSettingsScreen layout: Display, Quality & Performance, Preferences) with the options this
// client has, and its own page for the rendering features the game does not have, reached from Quality &
// Performance: Shaders & Upscaling (shader preset and packs, temporal upscaler TAA / NVIDIA DLSS / AMD FSR / Intel
// XeSS with the vendor's quality modes, sharpness, frame generation, low-latency mode). Only what this GPU and
// driver support is offered (render/upscalers/upscalers.h). Every change applies immediately; options.txt keeps it.
#include "render/packs/pack_api.h"
#include "render/renderer.h"
#include "render/upscalers/upscalers.h"
#include "ui/options_list.h"
#include "world/terrain.h"

#include <cmath>

namespace ui {

namespace {

constexpr u32 kFpsSteps[] = {30, 40, 45, 50, 60, 72, 75, 90, 100, 120, 144, 165, 180, 200, 240, 300, 360, 0};  // 0 = off

const char* presetName(u8 p) {
    static const char* n[] = {"Off", "Low", "Medium", "High", "Ultra"};
    return n[std::min<int>(p, 4)];
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

void videoBuild(UiState& s) {
    OptionsList l(s, Text(s.res.translate("options.videoTitle")), Screen::Options);
    UiState* p = &s;
    Upscalers& up = s.init.renderer->upscalers();
    l.header(Text(s.res.translate("options.video.display.header")));
    {  // Max Framerate (our steps include common refresh rates) and VSync
        const u32 cur = up.settings().fpsLimit;
        int idx = int(std::size(kFpsSteps)) - 1;
        for (int i = 0; i < int(std::size(kFpsSteps)); ++i)
            if (kFpsSteps[i] == cur) idx = i;
        Button fps = sliderButton(float(idx) / float(std::size(kFpsSteps) - 1),
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
                                  });
        Button vsync = cycleButton(
            [p] {
                return optionLabel(*p, "options.vsync",
                                   Text(p->res.translate(p->init.renderer->upscalers().settings().vsync ? "options.on" : "options.off")));
            },
            [p] {
                VideoSettings v = p->init.renderer->upscalers().settings();
                v.vsync = !v.vsync;
                p->applyVideo(v);
                p->settings.save();
            });
        l.half(std::move(fps), std::move(vsync));
    }
    {  // GUI Scale (Auto, then 1 .. the largest this window allows) and Fullscreen (borderless, this monitor)
        Button gui = cycleButton(
            [p] {
                return optionLabel(*p, "options.guiScale",
                                   Text(p->settings.guiScale == 0 ? p->res.translate("options.guiScale.auto") : std::to_string(p->settings.guiScale)));
            },
            [p] {
                int maxScale = 1;
                const u32 w = p->window->width(), h = p->window->height();
                while (w / u32(maxScale + 1) >= 320 && h / u32(maxScale + 1) >= 240) ++maxScale;
                p->settings.guiScale = p->settings.guiScale >= maxScale ? 0 : p->settings.guiScale + 1;
                p->settings.save();
            });
        Button full = cycleButton(
            [p] { return optionLabel(*p, "options.fullscreen", Text(p->res.translate(p->window->fullscreen() ? "options.on" : "options.off"))); },
            [p] { p->toggleFullscreen(); });
        l.half(std::move(gui), std::move(full));
    }
    l.header(Text(s.res.translate("options.video.quality.header")));
    {  // where the game has its graphics preset: this client's rendering features
        Button extra = screenButton(s, "", Screen::VideoExtra);
        extra.label = Text("Shaders & Upscaling...");
        extra.tooltip = [] { return Text("Shader preset and shader packs, DLSS / FSR / XeSS upscaling, frame generation and low latency"); };
        l.big(std::move(extra));
    }
    {  // Render Distance: up to the launch detail radius (it sized the geometry arena)
        const int maxRd = std::max(2, s.init.detailChunks);
        l.half(sliderButton(float(std::min(s.settings.renderDistance, maxRd) - 2) / float(std::max(1, maxRd - 2)),
                             [p, maxRd] {
                                 return optionLabel(*p, "options.renderDistance",
                                                    Text(formatJava(p->res.translate("options.chunks"),
                                                                    {std::to_string(std::min(p->settings.renderDistance, maxRd))})));
                             },
                             [p, maxRd](float v) {
                                 p->settings.renderDistance = 2 + int(std::lround(v * float(maxRd - 2)));
                                 p->terrain->setDetailRadius(float(p->settings.renderDistance) * 16.0f);
                             }));
    }
    l.header(Text(s.res.translate("options.video.preferences.header")));
    static const char* kIndicator[3] = {"options.off", "options.attack.crosshair", "options.attack.hotbar"};
    l.half(cycleButton(
        [p] { return optionLabel(*p, "options.attackIndicator", Text(p->res.translate(kIndicator[std::clamp(p->settings.attackIndicator, 0, 2)]))); },
        [p] {
            p->settings.attackIndicator = (p->settings.attackIndicator + 1) % 3;
            p->settings.save();
        }));
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
    {  // the shader pack's state under the title
        const auto st = packs::packStatus(*s.init.renderer);
        if (!st.loading.empty()) l.subtitle(Text("Loading shader pack " + st.loading + "..."), 0xFFFFD060);
        else if (!st.error.empty()) l.subtitle(Text("Shader pack error (built-in pipeline in use): " + st.error), 0xFFFF6060);
        else if (!s.packMessage.empty()) l.subtitle(Text(s.packMessage), 0xFFFF6060);
    }
    const std::function<Text()> needsShaders = [] { return Text("Needs the shader pipeline (Shaders: Low or higher)"); };

    l.header(Text("Shaders"));
    Button preset = cycleButton([p, cur] { return value(*p, "Shaders", presetName(cur.shaders)); },
                                [p, cur]() mutable {
                                    cur.shaders = u8((cur.shaders + 1) % 5);
                                    p->applyVideo(cur);
                                    p->settings.save();
                                },
                                [] { return Text("The built-in renderer's lighting and effects. Off is the plain look; upscaling and frame "
                                                 "generation need Low or higher."); });
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

    l.header(Text("Upscaling"));
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
                                      if (u.settings().shaders == 0) t += "Needs the shader pipeline (Shaders: Low or higher)\n";
                                      return Text(t + u.status());
                                  });
    upscaler.active = shaders;
    Button quality = cycleButton([p, cur, &up] { return value(*p, "Quality", up.qualityName(cur.upscaler, cur.quality)); },
                                 [p, cur, &up]() mutable {
                                     const auto modes = up.qualityModes(cur.upscaler);
                                     auto it = std::find(modes.begin(), modes.end(), cur.quality);
                                     cur.quality = it == modes.end() || it + 1 == modes.end() ? modes.front() : *(it + 1);
                                     p->applyVideo(cur);
                                     p->settings.save();
                                 },
                                 shaders ? std::function<Text()>() : needsShaders);
    quality.active = shaders;
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
    sharpness.active = shaders;
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
