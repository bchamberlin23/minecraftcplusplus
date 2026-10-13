// Toasts (ToastManager + SystemToast, AdvancementToast, TutorialToast): queued, at most as many as fit in five
// 32-pixel slots at once.
#include "ui/toasts.h"

#include "audio/audio.h"
#include "ui/items.h"
#include "ui/modules.h"
#include "ui/ui_state.h"

#include <deque>

namespace ui {

namespace {

constexpr double kDisplay = 5.0, kSlide = 0.6;  // display time, ToastInstance slide animation
constexpr int kSlots = 5;

enum class Kind { System, Advancement, Tutorial, Custom };

struct Toast {
    Kind kind = Kind::System;
    Text title, message;
    std::string id;
    int type = 0;           // advancement type
    ItemStack icon;         // advancement icon
    std::string sprite;     // tutorial icon sprite
    bool progressable = false;
    float progress = 0, smoothed = 0;
    double timeToDisplay = 0;  // tutorial: seconds until it hides itself (0: until the tutorial hides it)
    int handle = 0;
    bool hideRequested = false;
    std::string custom;  // Custom: its kind, width and drawing
    int width = 160;
    std::function<void(Canvas&, float, float, double)> draw;
    double displayTime = kDisplay;  // SystemToastId.displayTime (seconds, before the Notification Time option)
    double lastSmoothing = 0;       // TutorialToast.lastSmoothingTime (seconds fully visible)
    bool changed = false;           // SystemToast / RecipeToast: updated (its time runs from lastChanged)
    double lastChanged = 0;
    int slot = 0, slots = 1;
    // ToastManager.ToastInstance (seconds of the UI clock)
    double animationStart = -1, becameFullyVisibleAt = -1, fullyVisibleFor = 0;
    bool show = false, finished = false;  // visibility SHOW / hasFinishedRendering
    float portion = 0;
};

std::deque<Toast> g_queue;
std::vector<Toast> g_visible;
int g_nextHandle = 1;
double g_displayScale = 1;  // the Notification Time option

void play(const char* event) {
    if (audio::AudioSystem* a = audio::instance()) a->playUi(event, 1.0f, 1.0f);
}

u32 alpha(u32 rgb, float a) { return (u32(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24) | (rgb & 0xFFFFFF); }

// SystemToast: lines split at 200 px, 12 apart; height 20 + (title lines - 1) * 12 + max(message lines, 1) * 12
std::vector<Text> systemLines(UiState& s, const Text& t) { return t.empty() ? std::vector<Text>{} : s.font.split(t, 200); }
int systemWidth(UiState& s, const Toast& t) {  // recalculateWidth
    int widest = -1;
    for (const Text& l : systemLines(s, t.title)) widest = std::max(widest, s.font.width(l));
    for (const Text& l : systemLines(s, t.message)) widest = std::max(widest, s.font.width(l));
    return std::max(160, widest < 0 ? 200 : widest) + 30;
}
// TutorialToast: the title lines (purple) and the message lines at 126 px
std::vector<Text> tutorialLines(UiState& s, const Toast& t) {
    Style purple;
    purple.color = 0x500050;
    std::vector<Text> lines;
    for (const Text& l : s.font.split(t.title, 126)) {
        Text c = l;
        for (Span& sp : c.spans) sp.style = sp.style.over(purple);
        lines.push_back(std::move(c));
    }
    if (!t.message.empty())
        for (const Text& l : s.font.split(t.message, 126)) lines.push_back(l);
    return lines;
}
int toastHeight(UiState& s, const Toast& t) {
    if (t.kind == Kind::System)
        return 20 + (std::max(1, int(systemLines(s, t.title).size())) - 1) * 12 + std::max(int(systemLines(s, t.message).size()), 1) * 12;
    if (t.kind == Kind::Tutorial) return 7 + std::max(int(tutorialLines(s, t).size()), 2) * 11 + 3;
    return 32;
}

void drawSystem(UiState& s, Canvas& c, const Toast& t, float x, float y, int width) {
    blitSprite(c, s.sprites.sprite("toast/system"), x, y, float(width), float(toastHeight(s, t)));
    const std::vector<Text> title = systemLines(s, t.title), message = systemLines(s, t.message);
    auto lines = [&](const std::vector<Text>& v, int top, u32 color) {
        for (size_t i = 0; i < v.size(); ++i) s.drawText(c, v[i], x + 18, y + float(top + int(i) * 12), color, false);
    };
    if (message.empty()) {
        lines(title, 12, 0xFFFFFF00);
    } else {
        lines(title, 7, 0xFFFFFF00);
        lines(message, 7 + int(title.size()) * 12, 0xFFFFFFFF);
    }
}

// AdvancementToast.extractRenderState
void drawAdvancement(UiState& s, Canvas& c, const Toast& t, float x, float y) {
    blitSprite(c, s.sprites.sprite("toast/advancement"), x, y, 160, 32);
    const std::vector<Text> lines = s.font.split(t.title, 125);
    const u32 titleColor = t.type == 1 ? 0xFF88FF : 0xFFFF00;
    const char* key = t.type == 1 ? "advancements.toast.challenge" : t.type == 2 ? "advancements.toast.goal" : "advancements.toast.task";
    const Text label(s.res.translate(key));
    if (lines.size() == 1) {
        s.drawText(c, label, x + 30, y + 7, 0xFF000000 | titleColor, false);
        s.drawText(c, lines[0], x + 30, y + 18, 0xFFFFFFFF, false);
    } else {
        const double ms = t.fullyVisibleFor * 1000.0;
        if (ms < 1500.0) {
            s.drawText(c, label, x + 30, y + 11, alpha(titleColor, float((1500.0 - ms) / 300.0)), false);
        } else {
            const float a = float((ms - 1500.0) / 300.0) * 252.0f / 255.0f;
            float ly = y + float(16 - int(lines.size()) * 9 / 2);  // height / 2 - lines * 9 / 2 (integers)
            for (const Text& l : lines) s.drawText(c, l, x + 30, ly, alpha(0xFFFFFF, a), false), ly += 9;
        }
    }
    s.drawItem(c, t.icon, x + 8, y + 8, false);
}

// TutorialToast.extractRenderState
void drawTutorial(UiState& s, Canvas& c, Toast& t, float x, float y) {
    const int height = toastHeight(s, t);
    blitSprite(c, s.sprites.sprite("toast/tutorial"), x, y, 160, float(height));
    blitSprite(c, s.sprites.sprite("toast/" + t.sprite), x + 6, y + 6, 20, 20);
    const std::vector<Text> lines = tutorialLines(s, t);
    const int content = std::max(int(lines.size()), 2) * 11, top = 7 + (content - int(lines.size()) * 11) / 2;
    for (size_t i = 0; i < lines.size(); ++i) s.drawText(c, lines[i], x + 30, y + float(top + int(i) * 11), 0xFF000000, false);
    if (t.progressable) {
        const float barY = y + float(height - 4);
        c.fill(x + 3, barY, x + 157, barY + 1, 0xFFFFFFFF);
        const u32 col = t.progress >= t.smoothed ? 0xFF005500 : 0xFF550000;
        c.fill(x + 3, barY, x + float(int(3.0f + 154.0f * t.smoothed)), barY + 1, col);
    }
}

// --- NowPlayingToast: its own ToastInstance at the top left (MusicManager's song, the Music Toast option) ---------
struct NowPlaying {
    double animationStart = -1, fullyVisibleAt = -1;  // ToastInstance (seconds of the UI clock)
    bool visible = false, wanted = false, updating = false, finished = true;
    float portion = 0;
    int shows = 0, hides = 0, mode = -1;  // the audio system's counters / the option, as last seen
};
NowPlaying g_np;
int g_noteTick = 0;
double g_noteChange = 0;
u32 g_noteColor = 0xFFFFFFFF;

// ColorLerper.getLerpedColor(MUSIC_NOTE): 30 steps a colour through MUSIC_NOTE_COLORS (texture colours x 1.25).
u32 musicNoteColor(int tick) {
    static const u32 kColors[12] = {0xF9FFFE, 0x9D9D97, 0x3AB3DA, 0x3C44AA, 0x169C9C, 0x5E7C16,
                                    0x80C71F, 0xFED83D, 0xF9801D, 0xF38BAA, 0xB02E26, 0xC74EBD};
    auto modified = [](int i) -> u32 {
        if (i == 0) return 0xFFE6E6E6;  // white: -1644826
        u32 c = 0xFF000000;
        for (int k = 0; k < 24; k += 8) c |= u32(std::clamp(int(std::floor(float((kColors[i] >> k) & 255) * 1.25f)), 0, 255)) << k;
        return c;
    };
    const int value = tick / 30, c1 = value % 12, c2 = (value + 1) % 12;
    const float sub = float(tick % 30) / 30.0f;
    const u32 a = modified(c1), b = modified(c2);
    u32 out = 0;
    for (int k = 0; k < 32; k += 8) {  // ARGB.srgbLerp: Mth.lerpInt per channel
        const int from = int((a >> k) & 255), to = int((b >> k) & 255);
        out |= u32((from + int(std::floor(sub * float(to - from)))) & 255) << k;
    }
    return out;
}

Text nowPlayingText(UiState& s, const std::string& key) { return Text(s.res.translate(key)); }

void drawToasts(UiState& s, Canvas& c) {
    const double now = s.clock;
    if (audio::AudioSystem* a = audio::instance()) {  // ToastManager: showNowPlayingToast / hideNowPlayingToast
        const int mode = s.settings.musicToast;
        if (mode != g_np.mode) {  // setMusicToastDisplayState: a new instance, shown at once with music audible
            const bool wasNone = g_np.mode < 0;
            g_np = NowPlaying{};
            g_np.mode = mode, g_np.shows = a->nowPlayingShows(), g_np.hides = a->nowPlayingHides();
            if (!wasNone && mode == 2 && a->volume(audio::Category::Music) * a->volume(audio::Category::Master) > 0.0f)
                g_np.updating = g_np.wanted = true;
        }
        if (a->nowPlayingShows() != g_np.shows) {
            g_np.shows = a->nowPlayingShows();
            if (mode != 0) {
                g_np = NowPlaying{-1, -1, false, true, true, true, 0, g_np.shows, g_np.hides, mode};  // resetToast + showToast
            }
        }
        if (a->nowPlayingHides() != g_np.hides) g_np.hides = a->nowPlayingHides(), g_np.wanted = false;
        if (mode != 0) {  // ToastInstance.update
            NowPlaying& n = g_np;
            if (n.animationStart < 0) n.animationStart = now, n.visible = true;
            if (n.visible && now - n.animationStart <= kSlide) n.fullyVisibleAt = now;
            const double fullyVisibleFor = now - n.fullyVisibleAt;
            float p = float(std::clamp((now - n.animationStart) / kSlide, 0.0, 1.0));
            p *= p;
            n.portion = n.visible ? p : 1.0f - p;
            if (n.updating) {  // NowPlayingToast.update
                n.wanted = fullyVisibleFor < kDisplay * s.settings.notificationDisplayTime;
                tickNowPlayingNotes(s);
            }
            if (n.wanted != n.visible) {
                n.animationStart = now - (1.0f - n.portion) * kSlide;
                n.visible = n.wanted;
            }
            const bool wasFinished = n.finished;
            n.finished = !n.visible && now - n.animationStart > kSlide;
            if (n.finished && !wasFinished) n.updating = false;  // onFinishedRendering
            if (mode == 2 && !n.finished && s.screen != Screen::Pause && !s.hudHidden) {
                const std::string& key = a->currentSongKey();
                if (!key.empty()) {
                    const float w = float(30 + s.font.width(nowPlayingText(s, key)) + 7);
                    drawNowPlaying(s, c, w * n.portion - w, 0);
                }
            }
        }
    }
    g_displayScale = s.settings.notificationDisplayTime;
    // ToastManager.update: the visible toasts first (one visibility sound per update), then the queued ones take the
    // first run of free slots their height needs (32 px each), one sound per toast kind at most.
    bool visibilitySound = false;
    for (Toast& t : g_visible) {
        const bool before = t.show;
        // ToastInstance.update
        if (t.animationStart < 0) t.animationStart = now, t.show = true;
        if (t.show && now - t.animationStart <= kSlide) t.becameFullyVisibleAt = now;
        t.fullyVisibleFor = now - t.becameFullyVisibleAt;
        float f = float(std::clamp((now - t.animationStart) / kSlide, 0.0, 1.0));
        f *= f;
        t.portion = t.show ? f : 1.0f - f;
        // Toast.update: the visibility each kind wants
        const double vis = t.fullyVisibleFor;
        bool wanted = true;
        switch (t.kind) {
            case Kind::System:
            case Kind::Custom:  // SystemToast / RecipeToast: from the last change
                if (t.changed) t.lastChanged = vis, t.changed = false;
                wanted = !t.hideRequested && vis - t.lastChanged < (t.kind == Kind::System ? t.displayTime : kDisplay) * g_displayScale;
                break;
            case Kind::Advancement: wanted = vis < kDisplay * g_displayScale; break;
            case Kind::Tutorial:
                if (t.timeToDisplay > 0) {
                    t.progress = t.smoothed = float(std::min(vis / t.timeToDisplay, 1.0));
                    t.lastSmoothing = vis;
                    if (vis > t.timeToDisplay) t.hideRequested = true;
                } else if (t.progressable) {  // Mth.clampedLerp((visible - last) / 100 ms, ...)
                    const float k = std::clamp(float((vis - t.lastSmoothing) * 1000.0 / 100.0), 0.0f, 1.0f);
                    t.smoothed += (t.progress - t.smoothed) * k;
                    t.lastSmoothing = vis;
                }
                wanted = !t.hideRequested;
                break;
        }
        if (wanted != t.show) {
            t.animationStart = now - (1.0f - t.portion) * kSlide;
            t.show = wanted;
        }
        t.finished = !t.show && now - t.animationStart > kSlide;
        if (t.show != before && !visibilitySound) visibilitySound = true, play(t.show ? "ui.toast.in" : "ui.toast.out");
    }
    std::erase_if(g_visible, [](const Toast& t) { return t.finished; });
    bool challengeSound = false;
    bool used[kSlots] = {};
    for (const Toast& t : g_visible)
        for (int i = t.slot; i < t.slot + t.slots && i < kSlots; ++i) used[i] = true;
    for (auto it = g_queue.begin(); it != g_queue.end();) {
        const int need = (toastHeight(s, *it) + 31) / 32;
        int at = -1;
        for (int i = 0, run = 0; i < kSlots && at < 0; ++i) {
            run = used[i] ? 0 : run + 1;
            if (run == need) at = i + 1 - run;
        }
        if (at < 0) {
            ++it;
            continue;
        }
        Toast next = std::move(*it);
        it = g_queue.erase(it);
        next.slot = at, next.slots = need;
        for (int i = at; i < at + need; ++i) used[i] = true;
        if (next.kind == Kind::Advancement && next.type == 1 && !challengeSound) challengeSound = true, play("ui.toast.challenge_complete");
        g_visible.push_back(std::move(next));  // its first update is the next one
    }
    // ToastManager.extractRenderState: nothing with the HUD hidden; x from the visible portion, y = slot * height (yPos)
    if (s.hudHidden) return;
    const int W = c.guiWidth();
    for (Toast& t : g_visible) {
        if (t.animationStart < 0 || t.finished) continue;
        const int width = t.kind == Kind::System ? systemWidth(s, t) : t.width;
        const float x = float(W) - float(width) * t.portion, y = float(t.slot * toastHeight(s, t));
        switch (t.kind) {
            case Kind::System: drawSystem(s, c, t, x, y, width); break;
            case Kind::Advancement: drawAdvancement(s, c, t, x, y); break;
            case Kind::Tutorial: drawTutorial(s, c, t, x, y); break;
            case Kind::Custom: t.draw(c, x, y, t.fullyVisibleFor); break;
        }
    }
}

const UiModuleRegistrar g_module({.name = "toasts", .drawTop = drawToasts});

Toast* byHandle(int handle) {
    for (Toast& t : g_visible)
        if (t.handle == handle) return &t;
    for (Toast& t : g_queue)
        if (t.handle == handle) return &t;
    return nullptr;
}

}  // namespace

void showToast(const Text& title, const Text& message, const std::string& id, double displaySeconds) {
    if (!id.empty()) {  // SystemToast.addOrUpdate: getToast (also one sliding out), then reset
        for (Toast& t : g_visible)
            if (t.id == id && t.kind == Kind::System) {
                t.title = title, t.message = message, t.changed = true;
                return;
            }
        for (Toast& t : g_queue)
            if (t.id == id && t.kind == Kind::System) {
                t.title = title, t.message = message, t.changed = true;
                return;
            }
    }
    Toast t;
    t.title = title, t.message = message, t.id = id, t.displayTime = displaySeconds;
    g_queue.push_back(std::move(t));
}

void showAdvancementToast(const Text& title, int type, const ItemStack& icon) {
    Toast t;
    t.kind = Kind::Advancement, t.title = title, t.type = type, t.icon = icon;
    g_queue.push_back(std::move(t));
}

int showTutorialToast(const std::string& icon, const Text& title, const Text& message, bool progressable, double seconds) {
    Toast t;
    t.kind = Kind::Tutorial, t.sprite = icon, t.title = title, t.message = message, t.progressable = progressable;
    t.timeToDisplay = seconds;
    t.handle = g_nextHandle++;
    g_queue.push_back(t);
    return t.handle;
}

bool customToastActive(const char* kind) {
    for (const Toast& t : g_queue)
        if (t.kind == Kind::Custom && t.custom == kind) return true;
    for (const Toast& t : g_visible)
        if (t.kind == Kind::Custom && t.custom == kind) return true;
    return false;
}

void forceHideToast(const std::string& id) {  // SystemToast.forceHide
    for (Toast& t : g_visible)
        if (t.id == id && t.kind == Kind::System) t.hideRequested = true;
    for (Toast& t : g_queue)
        if (t.id == id && t.kind == Kind::System) t.hideRequested = true;
}

void clearToasts() {  // ToastManager.clear (Hud.onDisconnected)
    g_visible.clear();
    g_queue.clear();
}

void showCustomToast(const char* kind, int width, std::function<void(Canvas& c, float x, float y, double shownFor)> draw) {
    for (Toast& t : g_queue)
        if (t.kind == Kind::Custom && t.custom == kind) {
            t.draw = std::move(draw);
            return;
        }
    for (Toast& t : g_visible)
        if (t.kind == Kind::Custom && t.custom == kind) {  // RecipeToast.addItem: changed (slides back in when going)
            t.draw = std::move(draw);
            t.changed = true;
            return;
        }
    Toast t;
    t.kind = Kind::Custom, t.custom = kind, t.width = width, t.draw = std::move(draw);
    g_queue.push_back(std::move(t));
}

void tickNowPlayingNotes(UiState&) {  // NowPlayingToast.tickMusicNotes: the notes change colour every 25 ms
    audio::AudioSystem* a = audio::instance();
    if (!a || a->currentSongKey().empty()) return;
    const double now = nowSeconds();
    if (now > g_noteChange + 0.025) {
        ++g_noteTick;
        g_noteChange = now;
        g_noteColor = musicNoteColor(g_noteTick);
    }
}

void drawNowPlaying(UiState& s, Canvas& c, float x, float y) {  // NowPlayingToast.extractToast
    audio::AudioSystem* a = audio::instance();
    if (!a || a->currentSongKey().empty()) return;
    const Text text = nowPlayingText(s, a->currentSongKey());
    blitSprite(c, s.sprites.sprite("toast/now_playing"), x, y, float(30 + s.font.width(text) + 7), 30);
    blitSprite(c, s.sprites.sprite("icon/music_notes"), x + 7, y + 7, 16, 16, g_noteColor);
    s.drawText(c, text, x + 30, y + 15 - 9 / 2, 0xFFD3D3D3, true);
}

void setTutorialToastProgress(int handle, float progress) {
    if (Toast* t = byHandle(handle)) t->progress = progress;
}

void hideTutorialToast(int handle) {
    if (Toast* t = byHandle(handle)) t->hideRequested = true;
    std::erase_if(g_queue, [&](const Toast& t) { return t.handle == handle; });  // never shown: just dropped
}

}  // namespace ui
