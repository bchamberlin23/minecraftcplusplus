// Toasts (ToastManager + SystemToast): queued, at most as many as fit in five 32-pixel slots at once.
#include "ui/toasts.h"

#include "audio/audio.h"
#include "ui/modules.h"
#include "ui/ui_state.h"

#include <deque>

namespace ui {

namespace {

constexpr double kDisplay = 5.0, kSlide = 0.6;  // SystemToast display time, ToastInstance slide animation
constexpr int kSlots = 5;

struct Toast {
    Text title, message;
    std::string id;
    double shownAt = -1;  // clock when it started sliding in (-1: queued)
    int slot = 0, slots = 1;
    bool hiding = false, soundOut = false;
    double hideAt = 0;
};

std::deque<Toast> g_queue;
std::vector<Toast> g_visible;
double g_now = 0;
double g_displayScale = 1;  // the Notification Time option

void play(const char* event) {
    if (audio::AudioSystem* a = audio::instance()) a->playUi(event, 1.0f, 1.0f);
}

void drawToasts(UiState& s, Canvas& c) {
    const double now = s.clock;
    g_now = now, g_displayScale = s.settings.notificationDisplayTime;
    // Free slots fill from the queue (ToastManager.update).
    while (!g_queue.empty()) {
        bool used[kSlots] = {};
        for (const Toast& t : g_visible)
            for (int i = t.slot; i < t.slot + t.slots && i < kSlots; ++i) used[i] = true;
        Toast& next = g_queue.front();
        const int need = 1;  // 32 pixels: a title and one message line
        int at = -1;
        for (int i = 0; i + need <= kSlots && at < 0; ++i) {
            bool free = true;
            for (int k = i; k < i + need; ++k) free = free && !used[k];
            if (free) at = i;
        }
        if (at < 0) break;
        next.slot = at, next.slots = need, next.shownAt = now;
        g_visible.push_back(std::move(next));
        g_queue.pop_front();
        play("ui.toast.in");
    }
    const int W = c.guiWidth();
    for (Toast& t : g_visible) {
        if (!t.hiding && now - t.shownAt >= kSlide + kDisplay * g_displayScale) {
            t.hiding = true, t.hideAt = now;
            play("ui.toast.out");
        }
        float f = float(std::clamp((now - (t.hiding ? t.hideAt : t.shownAt)) / kSlide, 0.0, 1.0));
        f *= f;
        const float visible = t.hiding ? 1.0f - f : f;
        const int width = std::max(160, 30 + std::max(s.font.width(t.title), t.message.empty() ? 0 : s.font.width(t.message)));
        const int height = 32;
        const float x = float(W) - float(width) * visible, y = float(t.slot * 32);
        blitSprite(c, s.sprites.sprite("toast/system"), x, y, float(width), float(height));
        if (t.message.empty()) {
            s.drawText(c, t.title, x + 18, y + 12, 0xFFFFFF00, false);
        } else {
            s.drawText(c, t.title, x + 18, y + 7, 0xFFFFFF00, false);
            s.drawText(c, t.message, x + 18, y + 18, 0xFFFFFFFF, false);
        }
    }
    std::erase_if(g_visible, [&](const Toast& t) { return t.hiding && now - t.hideAt >= kSlide; });
}

const UiModuleRegistrar g_module({.name = "toasts", .drawTop = drawToasts});

}  // namespace

void showToast(const Text& title, const Text& message, const std::string& id) {
    if (!id.empty()) {
        for (Toast& t : g_visible)
            if (t.id == id && !t.hiding) {
                t.title = title, t.message = message;
                t.shownAt = g_now - kSlide;  // fully in, its display time starts over
                return;
            }
        for (Toast& t : g_queue)
            if (t.id == id) {
                t.title = title, t.message = message;
                return;
            }
    }
    // The same toast twice in a row (a friend flickering online) is shown once.
    for (const Toast& t : g_queue)
        if (t.title.plain() == title.plain() && t.message.plain() == message.plain()) return;
    if (g_queue.size() >= 16) return;
    Toast t;
    t.title = title, t.message = message, t.id = id;
    g_queue.push_back(std::move(t));
}

}  // namespace ui
