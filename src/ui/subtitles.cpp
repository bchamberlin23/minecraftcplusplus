// Subtitles (SubtitleOverlay): with the Show Subtitles option, every sound that has a subtitle (sounds.json) lists it
// at the bottom right while it is audible: newest at the bottom, greying from white over three seconds (the
// Notification Time option scales it), "<" / ">" for sounds out of view to the side.
#include "ui/subtitles.h"

#include "audio/audio.h"
#include "ui/modules.h"
#include "ui/ui_state.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ui {

namespace {

struct PlayedAt {
    double pos[3];
    double time;  // seconds
};
struct Subtitle {
    std::string key;  // translation key
    float range = 0;
    bool looping = false;
    std::vector<PlayedAt> played;
};
std::vector<Subtitle> g_subtitles;
bool g_listening = false;

double dist2(const double a[3], const double b[3]) {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

// Subtitle.getBestSubtitleCandidate: the latest play within range (the nearer one on a tie).
const PlayedAt* best(const Subtitle& s, const double cam[3]) {
    if (s.played.empty()) return nullptr;
    if (s.played.size() == 1) return &s.played.front();
    const PlayedAt* out = nullptr;
    for (const PlayedAt& p : s.played) {
        if (!(dist2(p.pos, cam) < double(s.range) * double(s.range))) continue;
        if (!out || p.time > out->time || (p.time == out->time && dist2(p.pos, cam) > dist2(out->pos, cam))) out = &p;
    }
    return out;
}

void onSound(const std::string& key, const double pos[3], float range, bool looping) {  // SubtitleOverlay.onPlaySound
    const double now = nowSeconds();
    for (Subtitle& s : g_subtitles)
        if (s.key == key) {  // refresh
            std::erase_if(s.played, [&](const PlayedAt& p) { return p.pos[0] == pos[0] && p.pos[1] == pos[1] && p.pos[2] == pos[2]; });
            s.played.push_back({{pos[0], pos[1], pos[2]}, now});
            return;
        }
    Subtitle s;
    s.key = key, s.range = range, s.looping = looping;
    s.played.push_back({{pos[0], pos[1], pos[2]}, now});
    g_subtitles.push_back(std::move(s));
}

void frame(UiState& s) {  // the listener follows the option (SoundManager.addListener / removeListener)
    audio::AudioSystem* a = audio::instance();
    if (!a) return;
    if (!g_listening && s.settings.showSubtitles) {
        a->onSound = onSound;
        g_listening = true;
    } else if (g_listening && !s.settings.showSubtitles) {
        a->onSound = nullptr;
        g_listening = false;
    }
}

void drawHudTop(UiState& s, Canvas& c) {
    if (s.screen == Screen::None || s.screen == Screen::Chat) drawSubtitles(s, c);
}

const UiModuleRegistrar kModule({.name = "subtitles", .drawHudTop = drawHudTop, .frame = frame});

}  // namespace

void drawSubtitles(UiState& s, Canvas& c) {
    if (!g_listening || !s.game) return;
    const CameraState cam = s.game->viewCamera();  // the sound listener (Camera)
    const double pos[3] = {cam.x, cam.y, cam.z};
    const double sy = std::sin(cam.yaw), cy = std::cos(cam.yaw), sp = std::sin(cam.pitch), cp = std::cos(cam.pitch);
    const double fwd[3] = {-sy * cp, -sp, cy * cp}, up[3] = {-sy * sp, cp, cy * sp};
    const double right[3] = {fwd[1] * up[2] - fwd[2] * up[1], fwd[2] * up[0] - fwd[0] * up[2], fwd[0] * up[1] - fwd[1] * up[0]};
    const double mult = s.settings.notificationDisplayTime, now = nowSeconds();
    std::vector<const Subtitle*> audible;
    int width = 0;
    for (Subtitle& sub : g_subtitles) {
        std::erase_if(sub.played, [&](const PlayedAt& p) { return now - p.time > 3.0 * mult; });  // purgeOldInstances
        if (sub.played.empty()) continue;
        if (!std::isinf(sub.range)) {  // isAudibleFrom
            const PlayedAt* p = best(sub, pos);
            if (!p || !(dist2(p->pos, pos) < double(sub.range) * double(sub.range))) continue;
        }
        audible.push_back(&sub);
        width = std::max(width, s.font.width(Text(s.res.translate(sub.key))));
    }
    if (audible.empty()) return;
    width += s.font.width(Text("<")) + s.font.width(Text(" ")) + s.font.width(Text(">")) + s.font.width(Text(" "));
    const int halfWidth = width / 2, halfHeight = 9 / 2;
    int row = 0;
    for (const Subtitle* sub : audible) {
        const PlayedAt* p = best(*sub, pos);
        if (!p) continue;
        double d[3] = {p->pos[0] - pos[0], p->pos[1] - pos[1], p->pos[2] - pos[2]};
        const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (double& v : d) v = len < 1e-4 ? 0.0 : v / len;  // Vec3.normalize
        const double rightness = right[0] * d[0] + right[1] * d[1] + right[2] * d[2];
        const double forwardness = fwd[0] * d[0] + fwd[1] * d[1] + fwd[2] * d[2];
        // getBrightness: 255 down to 75 over the display time (loops hold full brightness for half a second)
        const double hold = sub->looping ? 0.5 : 0.0;
        const double t = std::max(0.0, now - p->time - hold) / ((3.0 - hold) * mult);
        const int brightness = int(std::floor(255.0 + (75.0 - 255.0) * std::clamp(t, 0.0, 1.0)));
        const u32 color = 0xFF000000u | u32(brightness) << 16 | u32(brightness) << 8 | u32(brightness);
        const Text text(s.res.translate(sub->key));
        const int textWidth = s.font.width(text);
        c.push();
        c.translate(float(c.guiWidth()) - float(halfWidth) - 2.0f, float(c.guiHeight() - 35 - row * (9 + 1)));
        c.fill(float(-halfWidth - 1), float(-halfHeight - 1), float(halfWidth + 1), float(halfHeight + 1), s.textBackground(0.8f));
        if (forwardness <= 0.5) {  // out of view
            if (rightness > 0.0) s.drawText(c, Text(">"), float(halfWidth - s.font.width(Text(">"))), float(-halfHeight), color, true);
            else if (rightness < 0.0) s.drawText(c, Text("<"), float(-halfWidth), float(-halfHeight), color, true);
        }
        s.drawText(c, text, float(-textWidth / 2), float(-halfHeight), color, true);
        c.pop();
        ++row;
    }
}

}  // namespace ui
