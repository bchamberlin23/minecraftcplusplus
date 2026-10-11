// Video Settings screen (Options -> Video Settings...): shader preset, temporal upscaler (TAA / NVIDIA DLSS / AMD FSR /
// Intel XeSS) with the vendor's quality modes, sharpness, frame generation (DLSS FG / Multi Frame Generation, FSR FG,
// XeSS-FG), low-latency mode (Reflex / Anti-Lag 2 / XeLL), VSync and the frame rate cap. Only what this GPU and
// driver support is offered (render/upscalers/upscalers.h). Every change applies immediately; options.txt keeps it.
#include "render/packs/pack_api.h"
#include "render/renderer.h"
#include "render/upscalers/upscalers.h"
#include "ui/ui_state.h"

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

}  // namespace

void Ui::State::applyVideo(VideoSettings v) {
    Upscalers& up = init.renderer->upscalers();
    v.renderScaleOverride = 0;
    settings.video = up.apply(v);
    settings.videoSaved = true;
    buildButtons();
}

void Ui::State::buildVideoButtons() {
    const int W = canvas ? guiW() : int(window->width()) / std::max(1, guiScale());
    const int H = canvas ? guiH() : int(window->height()) / std::max(1, guiScale());
    Upscalers& up = init.renderer->upscalers();
    const upscale::Caps& caps = up.caps();
    VideoSettings cur = up.settings();  // what is in effect (may include command-line overrides)
    cur.renderScaleOverride = 0;
    const bool shaders = cur.shaders != 0;
    const int x0 = W / 2 - 205, x1 = W / 2 + 5;
    int y = 32;
    auto button = [&](int x, int yy, std::string label, std::function<void()> fn, bool active = true) {
        Button b;
        b.x = float(x), b.y = float(yy), b.w = 200, b.label = Text(std::move(label)), b.onPress = std::move(fn), b.active = active;
        buttons.push_back(std::move(b));
    };
    auto slider = [&](int x, int yy, float value, std::function<Text()> label, std::function<void(float)> slide) {
        Button b;
        b.x = float(x), b.y = float(yy), b.w = 200, b.slider = true, b.value = value, b.labelFn = std::move(label);
        b.onSlide = std::move(slide);
        buttons.push_back(std::move(b));
    };

    button(x0, y, std::string("Shaders: ") + presetName(cur.shaders), [this, cur]() mutable {
        cur.shaders = u8((cur.shaders + 1) % 5);
        applyVideo(cur);
    });
    button(x1, y, std::string("Upscaler: ") + Upscalers::upscalerName(cur.upscaler),
           [this, cur, &caps]() mutable {
               int k = int(cur.upscaler);
               do k = (k + 1) % int(UpscalerKind::Count);
               while (!caps.upscaler[k]);
               cur.upscaler = UpscalerKind(k);
               applyVideo(cur);
           },
           shaders);
    y += 24;
    button(x0, y, std::string("Quality: ") + up.qualityName(cur.upscaler, cur.quality),
           [this, cur, &up]() mutable {
               const auto modes = up.qualityModes(cur.upscaler);
               auto it = std::find(modes.begin(), modes.end(), cur.quality);
               cur.quality = it == modes.end() || it + 1 == modes.end() ? modes.front() : *(it + 1);
               applyVideo(cur);
           },
           shaders);
    slider(x1, y, cur.sharpness,
           [this] { return Text("Sharpness: " + std::to_string(int(std::lround(init.renderer->upscalers().settings().sharpness * 100))) + "%"); },
           [this](float v) {
               VideoSettings s = init.renderer->upscalers().settings();
               s.sharpness = std::round(v * 20.0f) / 20.0f;
               if (s.sharpness != settings.video.sharpness) {
                   settings.video = init.renderer->upscalers().apply(s);
                   settings.videoSaved = true;
               }
           });
    y += 24;
    const auto fgChoices = frameGenChoices(caps);
    button(x0, y, "Frame Gen: " + frameGenLabel(cur),
           [this, cur, fgChoices]() mutable {
               size_t i = 0;
               for (; i < fgChoices.size(); ++i)
                   if (fgChoices[i].first == cur.frameGen && (cur.frameGen != FrameGenKind::Dlss || fgChoices[i].second == cur.frameGenMultiplier))
                       break;
               const auto& next = fgChoices[(i + 1) % fgChoices.size()];
               cur.frameGen = next.first, cur.frameGenMultiplier = next.second;
               applyVideo(cur);
           },
           shaders && fgChoices.size() > 1);
    {
        const bool any = caps.lowLatency[int(LowLatencyMode::On)];
        std::string name = caps.lowLatencyName[0] ? caps.lowLatencyName : "Low Latency";
        if (name.starts_with("NVIDIA ")) name = name.substr(7);
        if (name.starts_with("AMD ")) name = name.substr(4);
        const char* state = cur.lowLatency == LowLatencyMode::Boost ? "On+Boost" : cur.lowLatency == LowLatencyMode::On ? "On" : "Off";
        if (!any) name = "Low Latency", state = "n/a";
        if (cur.frameGen == FrameGenKind::Xess) name = "XeLL", state = "On (FG)";
        button(x1, y, name + ": " + state,
               [this, cur, &caps]() mutable {
                   int m = int(cur.lowLatency);
                   do m = (m + 1) % int(LowLatencyMode::Count);
                   while (!caps.lowLatency[m]);
                   cur.lowLatency = LowLatencyMode(m);
                   applyVideo(cur);
               },
               any && cur.frameGen != FrameGenKind::Xess);
    }
    y += 24;
    button(x0, y, std::string("VSync: ") + (cur.vsync ? "On" : "Off"), [this, cur]() mutable {
        cur.vsync = !cur.vsync;
        applyVideo(cur);
    });
    {
        int idx = int(std::size(kFpsSteps)) - 1;
        for (int i = 0; i < int(std::size(kFpsSteps)); ++i)
            if (kFpsSteps[i] == cur.fpsLimit) idx = i;
        slider(x1, y, float(idx) / float(std::size(kFpsSteps) - 1),
               [this] {
                   const u32 f = init.renderer->upscalers().settings().fpsLimit;
                   return Text("Max FPS: " + (f ? std::to_string(f) : std::string("Unlimited")));
               },
               [this](float v) {
                   const u32 f = kFpsSteps[size_t(std::lround(v * float(std::size(kFpsSteps) - 1)))];
                   VideoSettings s = init.renderer->upscalers().settings();
                   if (s.fpsLimit != f) {
                       s.fpsLimit = f;
                       settings.video = init.renderer->upscalers().apply(s);
                       settings.videoSaved = true;
                   }
               });
    }
    y += 24;
    {  // shader packs (render/packs): the built-in pipeline or an OptiFine / Iris pack, switched without a restart
        const auto packList = packs::listPacks();
        std::string shown = settings.shaderPack.empty() ? "Built-in" : settings.shaderPack;
        if (shown.size() > 22) shown = shown.substr(0, 20) + "..";
        button(x0, y, "Shader Pack: " + shown, [this, packList] {
            size_t i = 0;  // 0 = built-in, i = packList[i - 1]
            for (size_t k = 0; k < packList.size(); ++k)
                if (packList[k].name == settings.shaderPack) i = k + 1;
            i = (i + 1) % (packList.size() + 1);
            settings.shaderPack = i ? packList[i - 1].name : std::string();
            packs::selectPack(*init.renderer, settings.shaderPack);
            settings.save();
            buildButtons();
        });
        button(x1, y, "Shader Options...", [this] { openPackOptions(); }, !settings.shaderPack.empty());
    }
    y += 24;
    {  // AccessibilityOptionsScreen bobView, VideoSettingsScreen attackIndicator (options.generic_value captions)
        auto value = [this](const char* key, const std::string& v) {
            return translatable("options.generic_value", {Text(res.translate(key)), Text(v)}, &res.lang()).plain();
        };
        button(x0, y, value("options.viewBobbing", res.translate(settings.bobView ? "options.on" : "options.off")), [this] {
            settings.bobView = !settings.bobView;
            settings.save();
            buildButtons();
        });
        static const char* kIndicator[3] = {"options.off", "options.attack.crosshair", "options.attack.hotbar"};
        button(x1, y, value("options.attackIndicator", res.translate(kIndicator[std::clamp(settings.attackIndicator, 0, 2)])),
               [this] {
                   settings.attackIndicator = (settings.attackIndicator + 1) % 3;
                   settings.save();
                   buildButtons();
               });
    }
    Button done;
    done.x = float(W / 2 - 100), done.y = float(H - 30), done.w = 200, done.label = Text(res.translate("gui.done"));
    done.onPress = [this] {
        settings.save();
        openScreen(Screen::Options);
    };
    buttons.push_back(std::move(done));
}

void Ui::State::drawVideoScreen(Canvas& c) {
    const int W = c.guiWidth(), H = c.guiHeight();
    c.gradient(0, 0, float(W), float(H), 0xC0101010, 0xD0101010);
    drawCentered(c, Text("Video Settings"), float(W / 2), 8.f, 0xFFFFFFFF);
    drawCentered(c, Text("changes apply immediately"), float(W / 2), 19.f, 0xFF808080, false);
    drawButtons(c);
    Upscalers& up = init.renderer->upscalers();
    const upscale::Caps& caps = up.caps();
    float y = 32 + 6 * 24 + 6;
    auto line = [&](const std::string& s, u32 color) {
        if (y + 10 > float(H - 32)) return;  // keep clear of the Done button
        drawCentered(c, Text(s), float(W / 2), y, color, false);
        y += 10;
    };
    {  // shader pack state
        const auto st = packs::packStatus(*init.renderer);
        if (!st.loading.empty()) line("Shader pack: loading " + st.loading + "...", 0xFFFFD060);
        else if (!st.error.empty()) line("Shader pack error (built-in pipeline in use): " + st.error, 0xFFFF6060);
        // Packs render at the upscaler's render size (render/packs: Pipe::scaled); turn the pack's own TAA off with one.
        else if (!st.active.empty()) line("Shader pack: " + st.active + " (with DLSS / FSR / XeSS: turn the pack's TAA off)", 0xFFA0FFA0);
        if (!packMessage.empty()) line(packMessage, 0xFFFF6060);
    }
    {  // the status line, wrapped at its " | " separators
        const std::string st = up.status();
        std::string cur;
        for (size_t pos = 0; pos <= st.size();) {
            size_t end = st.find(" | ", pos);
            if (end == std::string::npos) end = st.size();
            const std::string tok = st.substr(pos, end - pos);
            const std::string joined = cur.empty() ? tok : cur + " | " + tok;
            if (!cur.empty() && font.width(Text(joined)) > W - 16) {
                line(cur, 0xFFE0E0E0);
                cur = tok;
            } else {
                cur = joined;
            }
            pos = end + 3;
        }
        if (!cur.empty()) line(cur, 0xFFE0E0E0);
    }
    if (up.frameGenActive() && up.lastFramesPresented() > 1)
        line("Frame generation: " + std::to_string(up.lastFramesPresented()) + " frames presented per rendered frame", 0xFFA0FFA0);
    for (int k = 1; k < int(UpscalerKind::Count); ++k) {
        const std::string name = Upscalers::upscalerName(UpscalerKind(k));
        if (caps.upscaler[k]) line(name + ": " + caps.version[k], 0xFFA0A0A0);
        else line(name + ": unavailable - " + caps.why[k], 0xFF808080);
    }
    if (up.settings().shaders == 0) line("Upscalers and frame generation need the shader pipeline (Shaders: Low or higher)", 0xFFFFD060);
}

void Ui::State::videoInput(const Window::InputEvent& e) {
    if (e.type == Window::InputEvent::Key && e.down && e.code == VK_ESCAPE) {
        settings.save();
        openScreen(Screen::Options);
        return;
    }
    buttonsInput(e);
}

}  // namespace ui
