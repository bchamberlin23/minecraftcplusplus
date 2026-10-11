/* Example native mod for minecraftCplusplus (see src/mods/mcpp_mod.h).
 *
 * Build: it is part of the CMake project (target hello_mod) -> build\<dir>\hello_mod.dll.
 * Install: copy hello_mod.dll into %APPDATA%\minecraftCplusplus\mods (Mods & Packs screen -> Open Mods Folder).
 *
 * What it does:
 *  - a HUD panel (top right) with the player's position and the blocks broken / placed this session
 *  - /hello            replies in chat
 *  - /lookdown         tells you which block is under your feet
 */
#include "mcpp_mod.h"

#include <stdio.h>
#include <string.h>

static const McppApi* api;
static int broken, placed, ticks, chatLines;
static char lastBlock[128] = "-";

static void hello(void* user, const char* args) {
    (void)user;
    double p[3];
    char line[256];
    api->player_position(p);
    snprintf(line, sizeof line, "Hello from hello_mod%s%s! You are at %.1f %.1f %.1f, %d ticks in.", *args ? ", " : "",
             args, p[0], p[1], p[2], ticks);
    api->chat(line, 0xFF55FFFF);
}

static void lookDown(void* user, const char* args) {
    (void)user, (void)args;
    double p[3];
    char state[128], line[256];
    api->player_position(p);
    const int x = (int)(p[0] < 0 ? p[0] - 1 : p[0]), y = (int)(p[1] < 0 ? p[1] - 1 : p[1]) - 1,
              z = (int)(p[2] < 0 ? p[2] - 1 : p[2]);
    api->get_block(x, y, z, state, sizeof state);
    snprintf(line, sizeof line, "Under your feet (%d %d %d): %s", x, y, z, state);
    api->chat(line, 0xFFFFFF55);
}

static void onTick(void) { ++ticks; }

static void onBreak(int x, int y, int z, const char* state) {
    (void)x, (void)y, (void)z;
    ++broken;
    snprintf(lastBlock, sizeof lastBlock, "broke %s", state);
}

static void onPlace(int x, int y, int z, const char* state) {
    (void)x, (void)y, (void)z;
    ++placed;
    snprintf(lastBlock, sizeof lastBlock, "placed %s", state);
}

static void onChat(const char* text) {
    (void)text;
    ++chatLines;
}

static void onHud(McppHud* hud) {
    float size[2];
    double p[3];
    char lines[4][160];
    api->hud_size(hud, size);
    api->player_position(p);
    snprintf(lines[0], sizeof lines[0], "hello_mod (native)");
    snprintf(lines[1], sizeof lines[1], "pos %.1f %.1f %.1f", p[0], p[1], p[2]);
    snprintf(lines[2], sizeof lines[2], "broken %d  placed %d  chat %d", broken, placed, chatLines);
    snprintf(lines[3], sizeof lines[3], "%.60s", lastBlock);
    int w = 0;
    for (int i = 0; i < 4; ++i) {
        const int lw = api->hud_text_width(hud, lines[i]);
        if (lw > w) w = lw;
    }
    const float x1 = size[0] - 4, x0 = x1 - (float)w - 8, y0 = 4, y1 = y0 + 4 * 10 + 6;
    api->hud_rect(hud, x0, y0, x1, y1, 0x90000000);
    api->hud_rect(hud, x0, y0, x1, y0 + 1, 0xFF55FFFF);  /* accent line */
    for (int i = 0; i < 4; ++i) api->hud_text(hud, x0 + 4, y0 + 4 + 10.0f * (float)i, i ? 0xFFE0E0E0 : 0xFF55FFFF, lines[i]);
}

static void onUnload(void) { api->log("hello_mod: bye"); }

MCPP_EXPORT int mcpp_mod_init(const McppApi* host, McppMod* mod) {
    if (host->version < 1) return 1;
    api = host;
    mod->api_version = MCPP_API_VERSION;
    mod->size = sizeof(McppMod);
    mod->name = "hello_mod";
    mod->version = "1.0";
    mod->on_tick = onTick;
    mod->on_block_break = onBreak;
    mod->on_block_place = onPlace;
    mod->on_hud = onHud;
    mod->on_chat = onChat;
    mod->on_unload = onUnload;
    api->register_command("hello", "/hello [text]", hello, NULL);
    api->register_command("lookdown", "/lookdown", lookDown, NULL);
    api->log("hello_mod: loaded");
    return 0;
}
