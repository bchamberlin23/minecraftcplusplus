// LevelLoadingScreen for a respawn or a dimension change (ClientPacketListener.startWaitingForNewLevel): the portal the
// player came through, or the menu background, with "Loading terrain..." until the session's LevelLoadTracker reports
// the level ready (the local server runs in its own process, so there is no chunk status grid or progress bar).
#include "net/session.h"
#include "ui/frontend.h"
#include "ui/modules.h"
#include "ui/narrator.h"
#include "ui/ui_state.h"

namespace ui {

namespace {

bool g_shown = false;  // the screen was opened for the level now loading

void build(UiState& s) { s.buttons.clear(); }

void draw(UiState& s, Canvas& c) {
    const int W = c.guiWidth(), H = c.guiHeight();
    const int reason = s.session ? s.session->levelLoadingReason() : 0;
    if (reason == 1) {  // NETHER_PORTAL: the nether portal's particle sprite over the whole screen (its animation)
        if (const Sprite* t = s.sprites.texture("block/nether_portal"); t && t->w > 0 && t->h >= t->w) {
            const int frames = t->h / t->w, frame = int(s.clock * 20.0) % frames;  // one frame a tick
            const float dv = (t->v1 - t->v0) / float(frames);
            c.quad(0, 0, float(W), float(H), t->u0, t->v0 + dv * frame, t->u1, t->v0 + dv * (frame + 1), 0xFFFFFFFF, ModeGui);
        }
    } else if (reason == 2) {  // END_PORTAL: the end portal's starfield (approximated: the end sky, the portal layer over it)
        c.fill(0, 0, float(W), float(H), 0xFF000000);
        if (const Sprite* sky = s.sprites.texture("environment/end_sky"))
            for (int y = 0; y < H; y += 64)
                for (int x = 0; x < W; x += 64) blitTexture(c, sky, float(x), float(y), 0, 0, 64, 64, 64, 64, 0xFF3A5A5A);
        if (const Sprite* portal = s.sprites.texture("entity/end_portal"))
            for (int y = 0; y < H; y += 256)
                for (int x = 0; x < W; x += 256) blitTexture(c, portal, float(x), float(y), 0, 0, 256, 256, 256, 256, 0x9055C8B4);
    } else {  // OTHER: the panorama (this port's: the world), blurred, under the menu background
        menuBackground(s, c);
    }
    s.drawCentered(c, Text(s.res.translate("multiplayer.downloadingTerrain")), float(W / 2), float(H / 2 - 50), 0xFFFFFFFF);
}

void input(UiState&, const InputEvent&) {}  // shouldCloseOnEsc: false; nothing to click

// Opened once for a respawn's level; closed (with the narrator's "Ready to play") when the level is ready.
void frame(UiState& s) {
    const bool loading = s.session && s.session->levelLoading() && s.session->levelLoadingRespawn();
    if (loading && !g_shown) {
        g_shown = true;
        s.openScreen(Screen::LevelLoading);
    } else if (!loading && g_shown) {
        g_shown = false;
        if (s.screen == Screen::LevelLoading) {
            narrator::systemNow(s, Text(s.res.translate("narrator.ready_to_play")));
            s.closeScreen();
        }
    }
}

const ScreenRegistrar kScreen({Screen::LevelLoading, build, draw, input, nullptr});
const UiModuleRegistrar kModule({.name = "level_loading", .frame = frame});

}  // namespace

}  // namespace ui
