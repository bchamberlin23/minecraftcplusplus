// The accessibility onboarding screen (AccessibilityOnboardingScreen): shown before the title screen until it is
// finished once (Options.onboardAccessibility). The logo over the panorama, the welcome text in a framed box, the
// Narrator / Music & Sounds / Accessibility / Language buttons and Continue; the widgets fade in over two seconds and
// out over one, and the narrator reads the welcome after 40 frames.
#include "ui/frontend.h"
#include "ui/narrator.h"
#include "ui/options_list.h"

#include <array>
#include <algorithm>

namespace ui {

namespace {

constexpr int kHeaderH = 90, kFooterH = 33, kTextW = 374;  // HeaderAndFooterLayout(this, 90, 33), TEXT_WIDGET_WIDTH

struct Onboarding {
    double fadeInStart = 0, fadeOutStart = 0;
    bool fadingIn = true, narrated = false;
    int frames = 0;
};
Onboarding g;

// Mth.clampedMap(fade, 0.5, 1, 0, 1)
float widgetAlpha(float fade) { return std::clamp((std::clamp(fade, 0.0f, 1.0f) - 0.5f) / 0.5f, 0.0f, 1.0f); }

std::vector<Text> titleLines(UiState& s) {
    return s.font.split(Text(s.res.translate("accessibility.onboarding.screen.title")), kTextW - 8);
}

// The contents: the text box (cell padding 8), then the 2 x 2 grid of 150 wide buttons (cells padded 4), in a column
// whose cells are padded 4; at the header + 30, or higher when it would run into the footer.
float contentTop(UiState& s) {
    const int textCell = int(titleLines(s).size()) * 9 + 8 + 16;
    const int gridCell = 2 * 28 + 8;
    return float(std::min(kHeaderH + 30, s.guiH() - kFooterH - (textCell + gridCell)));
}

void openSub(UiState& s, Screen target) {  // closeAndSetScreen: back here afterwards, not finished
    s.optionsParent = Screen::AccessibilityOnboarding;
    s.openScreen(target);
}

void build(UiState& s) {
    s.buttons.clear();
    UiState* p = &s;
    const float gridTop = contentTop(s) + float(int(titleLines(s).size()) * 9 + 8 + 16) + 4;
    const float left = float(s.guiW() / 2 - 158);
    auto place = [&](Button b, int i) {
        b.x = left + float(i % 2) * 158 + 4, b.y = gridTop + float(i / 2) * 28 + 4, b.w = 150, b.h = 20;
        s.buttons.push_back(std::move(b));
    };
    place(narratorButton(s), 0);
    Button sounds;
    sounds.label = Text(s.res.translate("options.sounds"));
    sounds.icon = "icon/music_notes", sounds.iconW = sounds.iconH = 16;
    sounds.onPress = [p] { openSub(*p, Screen::Sounds); };
    place(std::move(sounds), 1);
    Button access;  // CommonButtons.accessibility
    access.label = Text(s.res.translate("accessibility.onboarding.accessibility.button"));
    access.icon = "icon/accessibility", access.iconW = access.iconH = 15;
    access.onPress = [p] { openSub(*p, Screen::Accessibility); };
    access.tooltip = [p] { return Text(p->res.translate("options.accessibility.tooltip")); };
    place(std::move(access), 2);
    Button lang;  // CommonButtons.language
    lang.label = Text(s.res.translate("options.language"));
    lang.icon = "icon/language", lang.iconW = lang.iconH = 15;
    lang.onPress = [p] { openSub(*p, Screen::Language); };
    lang.tooltip = [p] { return Text(p->res.translate("options.language.tooltip")); };
    place(std::move(lang), 3);
    Button cont;
    cont.label = Text(s.res.translate("gui.continue"));
    cont.x = float(s.guiW() / 2 - 75), cont.y = float(s.guiH() - kFooterH + (kFooterH - 20) / 2), cont.w = 150;
    cont.onPress = [] {  // onClose: fade out first
        if (g.fadeOutStart == 0) g.fadeOutStart = nowSeconds();
    };
    s.buttons.push_back(std::move(cont));
}

void finish(UiState& s) {  // close(true, onClose): Options.onboardingAccessibilityFinished, then the title screen
    s.settings.onboardAccessibility = false;
    s.settings.save();
    g = Onboarding{};
    s.openScreen(Screen::Title);
}

void draw(UiState& s, Canvas& c) {
    const int W = c.guiWidth();
    menuBackground(s, c);
    // handleInitialNarrationDelay: 40 frames, then the welcome (with the window active)
    if (!g.narrated && narrator::available()) {
        if (g.frames < 40) ++g.frames;
        else if (s.window->focused()) {
            narrator::say(s.res.translate("accessibility.onboarding.screen.narrator"), true);
            g.narrated = true;
        }
    }
    const double now = nowSeconds();
    if (g.fadeInStart == 0 && g.fadingIn) g.fadeInStart = now;
    float alpha = 1.0f;
    if (g.fadeInStart > 0) {
        const float fade = float((now - g.fadeInStart) / 2.0);
        if (fade >= 1.0f) g.fadingIn = false, g.fadeInStart = 0;
        else alpha = widgetAlpha(fade);
    }
    if (g.fadeOutStart > 0) {
        const float fade = 1.0f - float((now - g.fadeOutStart) / 1.0);
        if (fade <= 0.0f) return finish(s);
        alpha = widgetAlpha(fade);
    }
    c.setAlpha(alpha);
    // FocusableTextWidget: black box, grey border (white while focused: no narrator puts the focus on it), centred lines
    const std::vector<Text> lines = titleLines(s);
    const float bx = float(W / 2 - kTextW / 2), by = contentTop(s) + 8, bh = float(lines.size() * 9 + 8);
    c.fill(bx + 1, by, bx + kTextW, by + bh, 0xFF000000);
    const u32 border = narrator::available() ? 0xFFA0A0A0 : 0xFFFFFFFF;
    c.fill(bx, by, bx + kTextW, by + 1, border);
    c.fill(bx, by + bh - 1, bx + kTextW, by + bh, border);
    c.fill(bx, by + 1, bx + 1, by + bh - 1, border);
    c.fill(bx + kTextW - 1, by + 1, bx + kTextW, by + bh - 1, border);
    for (size_t i = 0; i < lines.size(); ++i) s.drawCentered(c, lines[i], float(W / 2), by + 4 + float(i * 9), 0xFFFFFFFF);
    s.drawButtons(c);
    c.setAlpha(1.0f);
    // LogoRenderer (keepLogoThroughFade): 256x44 of the logo, the edition 7 pixels into it.
    blitTexture(c, s.sprites.texture("gui/title/minecraft"), float(W / 2 - 128), 30, 0, 0, 256, 44, 256, 64);
    blitTexture(c, s.sprites.texture("gui/title/edition"), float(W / 2 - 64), 30 + 44 - 7, 0, 0, 128, 14, 128, 16);
    for (const Button& b : s.buttons)
        if (b.tooltip && s.mouseX >= b.x && s.mouseY >= b.y && s.mouseX < b.x + b.w && s.mouseY < b.y + b.h)
            s.drawTooltip(c, s.font.split(b.tooltip(), 170), s.mouseX, s.mouseY, nullptr, false, {},
                          std::array<float, 4>{b.x, b.y, b.w, b.h}.data());
}

void input(UiState& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) {  // Screen.onClose: the same fade out
        if (g.fadeOutStart == 0) g.fadeOutStart = nowSeconds();
        return;
    }
    s.buttonsInput(e);
}

const ScreenRegistrar kOnboarding({Screen::AccessibilityOnboarding, build, draw, input, nullptr});

}  // namespace

}  // namespace ui
