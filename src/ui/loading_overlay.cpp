// LoadingOverlay: the Mojang Studios logo on the brand background (red, or black with Monochrome Logo) with the
// progress bar, over everything but the toasts. It covers the start-up (fading out to the title screen) and resource
// reloads (fading in over the screen first; the reload here runs once the overlay covers it).
#include "ui/loading_overlay.h"

#include "ui/frontend.h"
#include "ui/sprites.h"
#include "ui/ui_state.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

struct Overlay {
    bool active = false, fadeIn = false, reloaded = false;
    double fadeInStart = -1, fadeOutStart = -1;  // UiState clock seconds
    float progress = 0;                           // currentProgress
    std::function<bool()> reload;
} g;

u32 brand(UiState& s, float alpha) {  // LOGO_BACKGROUND_COLOR(_DARK) with an alpha
    const u32 rgb = s.settings.darkMojangStudiosBackground ? 0x000000u : 0xEF323Du;
    return (u32(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.999f) << 24) | rgb;  // Mth.ceil
}

}  // namespace

void startLoadingOverlay(bool fadeIn, std::function<bool()> reload) {
    g = {};
    g.active = true, g.fadeIn = fadeIn;
    g.reload = std::move(reload);
    g.reloaded = !g.reload;
}

bool loadingOverlayActive() { return g.active; }

void drawLoadingOverlay(UiState& s, Canvas& c) {
    if (!g.active) return;
    double now = s.clock;
    if (g.fadeIn && g.fadeInStart < 0) g.fadeInStart = now;
    if (!g.reloaded && (!g.fadeIn || now - g.fadeInStart >= 0.5)) {  // the screen is covered: reload
        g.reloaded = true;
        g.reload();
    }
    // tick: the reload done (and, fading in, a second shown) starts the fade out.
    if (g.fadeOutStart < 0 && g.reloaded && (!g.fadeIn || now - g.fadeInStart >= 1.0)) g.fadeOutStart = now;
    const float fadeOut = g.fadeOutStart >= 0 ? float(now - g.fadeOutStart) : -1.0f;
    const float fadeInAnim = g.fadeInStart >= 0 ? float(now - g.fadeInStart) / 0.5f : -1.0f;
    const float W = float(c.guiWidth()), H = float(c.guiHeight());
    float logoAlpha;
    if (fadeOut >= 1.0f) {  // the screen shows through
        c.fill(0, 0, W, H, brand(s, 1.0f - std::clamp(fadeOut - 1.0f, 0.0f, 1.0f)));
        logoAlpha = 1.0f - std::clamp(fadeOut - 1.0f, 0.0f, 1.0f);
    } else if (g.fadeIn) {
        c.fill(0, 0, W, H, brand(s, std::clamp(fadeInAnim, 0.15f, 1.0f)));
        logoAlpha = std::clamp(fadeInAnim, 0.0f, 1.0f);
    } else {  // the clear colour override: only the brand background
        c.fill(0, 0, W, H, brand(s, 1.0f));
        logoAlpha = 1.0f;
    }
    // The logo: the texture's top half on the left, its bottom half on the right (120 x 120 units, a 1/16 overlap).
    const float cx = std::floor(W * 0.5f), cy = std::floor(H * 0.5f);
    const float logoH = float(std::min(double(W) * 0.75, double(H)) * 0.25);
    const float halfH = std::floor(logoH * 0.5f), halfW = std::floor(logoH * 4.0f * 0.5f);
    if (const Sprite* t = s.sprites.texture("gui/title/mojangstudios")) {
        const float du = t->u1 - t->u0, dv = t->v1 - t->v0;
        auto u = [&](float x) { return t->u0 + du * x / 120.0f; };
        auto v = [&](float y) { return t->v0 + dv * y / 120.0f; };
        const u32 white = (u32(std::clamp(logoAlpha, 0.0f, 1.0f) * 255.0f) << 24) | 0xFFFFFFu;
        c.setBlend(5);  // RenderPipelines.MOJANG_LOGO: SRC_ALPHA, ONE
        c.quad(cx - halfW, cy - halfH, cx, cy - halfH + std::floor(logoH), u(-0.0625f), v(0), u(119.9375f), v(60), white, ModeGui);
        c.quad(cx, cy - halfH, cx + halfW, cy - halfH + std::floor(logoH), u(0.0625f), v(60), u(120.0625f), v(120), white, ModeGui);
        c.setBlend(0);
    }
    // The progress bar (extractProgressBar), fading with the first second of the fade out.
    g.progress = std::clamp(g.progress * 0.95f + (g.reloaded ? 1.0f : 0.0f) * 0.050000012f, 0.0f, 1.0f);
    if (fadeOut < 1.0f) {
        const float barY = std::floor(H * 0.8325f), x0 = std::floor(W / 2) - halfW, x1 = std::floor(W / 2) + halfW, y0 = barY - 5,
                    y1 = barY + 5;
        const u32 col = (u32(std::round((1.0f - std::clamp(fadeOut, 0.0f, 1.0f)) * 255.0f)) << 24) | 0xFFFFFFu;
        c.fill(x0 + 2, y0 + 2, x0 + std::ceil((x1 - x0 - 2) * g.progress), y1 - 2, col);
        c.fill(x0 + 1, y0, x1 - 1, y0 + 1, col);
        c.fill(x0 + 1, y1 - 1, x1 - 1, y1, col);
        c.fill(x0, y0, x0 + 1, y1, col);
        c.fill(x1 - 1, y0, x1, y1, col);
    }
    if (fadeOut >= 2.0f) g.active = false;
}

}  // namespace ui
