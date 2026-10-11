/* minecraftCplusplus native mod API (C ABI, version 1).
 *
 * A native mod is a DLL dropped into the mods folder (%APPDATA%\minecraftCplusplus\mods) that exports
 *
 *     MCPP_EXPORT int mcpp_mod_init(const McppApi* api, McppMod* mod);
 *
 * The game calls it once at startup (after the world is shown). Fill in `mod` (name, version, the callbacks you
 * want) and return 0; any other value unloads the DLL again. Keep `api`: it stays valid until on_unload.
 *
 * Rules:
 *  - Everything happens on the game's main thread; callbacks must return quickly (they run inside a frame).
 *  - Strings are UTF-8. Strings passed to a callback are only valid during that call.
 *  - Versioning: both tables start with a version and a size. New functions / callbacks are only ever appended,
 *    so a mod built against an older header keeps working. Check MCPP_HAS(api, fn) before calling a function
 *    newer than the version you require. `mod` points at zero-filled memory larger than any McppMod.
 *  - A mod that crashes inside a callback is disabled (logged), the game keeps running.
 *  - DLLs are only loaded from the mods folder, and only when enabled in the Mods & Packs screen.
 */
#ifndef MCPP_MOD_H
#define MCPP_MOD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCPP_API_VERSION 1

#if defined(_WIN32)
#define MCPP_EXPORT __declspec(dllexport)
#else
#define MCPP_EXPORT __attribute__((visibility("default")))
#endif

/* True when the host's function table has `fn` (it is new enough). */
#define MCPP_HAS(api, fn) ((api)->size >= offsetof(McppApi, fn) + sizeof(void*) && (api)->fn != NULL)

typedef struct McppHud McppHud; /* drawing context, only valid inside McppMod.on_hud */

/* A chat command: the text after "/name " (may be empty). */
typedef void (*McppCommandFn)(void* user, const char* args);

typedef struct McppApi {
    uint32_t version; /* MCPP_API_VERSION of the game */
    uint32_t size;    /* sizeof(McppApi) in the game */

    /* Writes a line to the game's log (stdout). */
    void (*log)(const char* text);
    /* Shows a message in the chat of this player only. "&" codes are not interpreted; colour with argb. */
    void (*chat)(const char* text, uint32_t argb);
    /* Sends a chat message, or runs a server command when it starts with "/" (needs a server: --play/--connect). */
    void (*send_chat)(const char* text);
    /* Registers "/name": typed in chat it calls fn instead of going to the server. Returns 0 if the name is taken. */
    int (*register_command)(const char* name, const char* usage, McppCommandFn fn, void* user);

    /* World queries. get_block writes the block state ("minecraft:oak_log[axis=y]") and returns its length. */
    int (*get_block)(int x, int y, int z, char* out, int out_size);
    void (*player_position)(double out_xyz[3]); /* feet position */
    void (*player_rotation)(float out_yaw_pitch[2]); /* degrees, like the game's F3 */
    int (*is_online)(void); /* 1 when connected to a server (singleplayer through the local server counts) */
    double (*time_seconds)(void); /* seconds since the game started */

    /* HUD drawing (only inside on_hud). Coordinates are GUI pixels (scaled like the game's GUI), origin top left. */
    void (*hud_size)(McppHud* hud, float out_wh[2]);
    void (*hud_text)(McppHud* hud, float x, float y, uint32_t argb, const char* text); /* with shadow */
    int (*hud_text_width)(McppHud* hud, const char* text);
    void (*hud_rect)(McppHud* hud, float x0, float y0, float x1, float y1, uint32_t argb);
} McppApi;

typedef struct McppMod {
    uint32_t api_version; /* set to MCPP_API_VERSION (the header the mod was built with) */
    uint32_t size;        /* set to sizeof(McppMod) */
    const char* name;     /* shown in the Mods & Packs screen and the log (static string) */
    const char* version;

    void (*on_unload)(void);                                        /* game exit */
    void (*on_tick)(void);                                          /* 20 times per second */
    void (*on_block_break)(int x, int y, int z, const char* state); /* the player broke a block (its old state) */
    void (*on_block_place)(int x, int y, int z, const char* state); /* the player placed a block */
    void (*on_hud)(McppHud* hud);                                   /* every frame the HUD is drawn */
    void (*on_chat)(const char* text);                              /* every chat line received (plain text) */
} McppMod;

/* The one export a mod must have. */
typedef int (*McppModInitFn)(const McppApi* api, McppMod* mod);

#ifdef __cplusplus
}
#endif

#endif /* MCPP_MOD_H */
