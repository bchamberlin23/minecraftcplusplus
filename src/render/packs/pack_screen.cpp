// Shader pack option screens (Video Settings -> Shader Options...): the pack's own screens from shaders.properties
// with its lang names (render/packs/pack_options.h). Done saves the values (pack_api.h savePackSettings, Iris format)
// and reloads the pack when it is the one in use; nothing restarts.
#include "render/packs/pack_options.h"
#include "render/renderer.h"
#include "ui/ui_state.h"

namespace ui {

void Ui::State::openPackOptions() {
    packs::PackInfo info;
    if (settings.shaderPack.empty() || !packs::findPack(settings.shaderPack, info)) return;
    auto model = std::make_shared<packs::PackOptionsModel>();
    std::string error;
    if (!model->open(info, error)) {
        packMessage = "Cannot read " + info.name + ": " + error;
        return;
    }
    packOptions = std::move(model);
    packScreens.assign(1, std::string());
    packPage = 0;
    openScreen(Screen::PackOptions);
}

void Ui::State::closePackOptions() {
    if (packOptions && packOptions->changed()) {
        packs::savePackSettings(packOptions->pack(), packOptions->values());
        if (settings.shaderPack == packOptions->pack().name)
            packs::setShaderPack(*init.renderer, packOptions->pack().path, packOptions->values());
    }
    packOptions.reset();
    openScreen(Screen::Video);
}

void Ui::State::buildPackButtons() {
    if (!packOptions) return openScreen(Screen::Video);
    const int W = canvas ? guiW() : int(window->width()) / std::max(1, guiScale());
    const int H = canvas ? guiH() : int(window->height()) / std::max(1, guiScale());
    const std::string screenName = packScreens.empty() ? std::string() : packScreens.back();
    const auto entries = packOptions->entries(screenName);
    const int cols = W < 330 ? 1 : std::min(packOptions->columns(screenName), W >= 640 ? 3 : 2);
    const int bw = cols == 1 ? 200 : cols == 2 ? 150 : 130, gap = 4;
    const int rows = std::max(1, (H - 32 - 34) / 22);
    const int perPage = rows * cols;
    const int pages = std::max(1, (int(entries.size()) + perPage - 1) / perPage);
    packPage = std::clamp(packPage, 0, pages - 1);
    const int x0 = W / 2 - (cols * bw + (cols - 1) * gap) / 2;
    for (int i = packPage * perPage; i < std::min(int(entries.size()), (packPage + 1) * perPage); ++i) {
        const auto& e = entries[size_t(i)];
        if (e.kind == packs::PackOptionsModel::Entry::Empty) continue;
        const int k = i - packPage * perPage;
        Button b;
        b.x = float(x0 + (k % cols) * (bw + gap)), b.y = float(32 + (k / cols) * 22), b.w = float(bw);
        b.label = Text(packOptions->label(e));
        if (e.kind == packs::PackOptionsModel::Entry::Link) {
            b.onPress = [this, id = e.id] {
                packScreens.push_back(id);
                packPage = 0;
                buildButtons();
            };
        } else {
            b.onPress = [this, e] {
                packOptions->cycle(e, 1);
                buildButtons();
            };
        }
        buttons.push_back(std::move(b));
    }
    auto button = [&](int x, int w, std::string label, std::function<void()> fn, bool active = true) {
        Button b;
        b.x = float(x), b.y = float(H - 28), b.w = float(w), b.label = Text(std::move(label)), b.onPress = std::move(fn);
        b.active = active;
        buttons.push_back(std::move(b));
    };
    button(W / 2 - 205, 40, "<", [this] { --packPage, buildButtons(); }, packPage > 0);
    button(W / 2 - 161, 120, "Reset to Defaults", [this] { packOptions->resetAll(), buildButtons(); });
    button(W / 2 - 37, 120, packScreens.size() > 1 ? "Back" : "Done", [this] {
        if (packScreens.size() > 1) {
            packScreens.pop_back();
            packPage = 0;
            buildButtons();
        } else {
            closePackOptions();
        }
    });
    button(W / 2 + 87, 40, ">", [this] { ++packPage, buildButtons(); }, packPage + 1 < pages);
    button(W / 2 + 131, 74, "Apply", [this] {
        if (!packOptions->changed()) return;
        packs::savePackSettings(packOptions->pack(), packOptions->values());
        packs::setShaderPack(*init.renderer, packOptions->pack().path, packOptions->values());
    });
}

void Ui::State::drawPackScreen(Canvas& c) {
    const int W = c.guiWidth();
    const int H = c.guiHeight();
    c.gradient(0, 0, float(W), float(H), 0xC0101010, 0xD0101010);
    if (!packOptions) return;
    const std::string screenName = packScreens.empty() ? std::string() : packScreens.back();
    drawCentered(c, Text(packOptions->screenTitle(screenName)), float(W / 2), 8.f, 0xFFFFFFFF);
    const auto st = packs::packStatus(*init.renderer);
    std::string line = !st.loading.empty() ? "loading " + st.loading + "..."
                       : !st.error.empty() ? "error: " + st.error
                       : packOptions->changed() ? "changes apply on Done / Apply"
                                                : "";
    if (!line.empty()) drawCentered(c, Text(line), float(W / 2), 19.f, st.error.empty() ? 0xFF808080 : 0xFFFF6060, false);
    drawButtons(c);
}

void Ui::State::packInput(const Window::InputEvent& e) {
    if (e.type == Window::InputEvent::Key && e.down && e.code == VK_ESCAPE) {
        if (packScreens.size() > 1) {
            packScreens.pop_back();
            packPage = 0;
            buildButtons();
        } else {
            closePackOptions();
        }
        return;
    }
    buttonsInput(e);
}

}  // namespace ui
