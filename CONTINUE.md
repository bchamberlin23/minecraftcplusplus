# Continue: what's left (as of 2026-10-11)

Remaining work from the 26.3 audit (`docs/audit-26.3/`). Microsoft login is permanently out of scope, along with
everything that depends on it: chat signing (NET-14) and player reporting (WORLD-10).

Last pushed commit: `e431537` (entity blob shadows). The owner's `build\Lead` has not been rebuilt since; rebuild it
only when the game isn't running (`tasklist | findstr minecraftCplusplus`).

## Done since 2026-10-11

All options in the old table now apply (sky options, panorama speed, damage tilt, high contrast outline with wide
lines, toggle attack / use, quit shortcuts, operator tab, inactivity FPS limit, autosave indicator, music frequency
and toast, subtitles, onboarding screen, chunk update priority, exclusive fullscreen and the override size). Also:
leash ropes and fishing lines, hurting projectile motion (`projectile_power`), the 26.3 pause menu, World Options,
in-world game rules (NET-13) and Restrictions, waypoints and the locator bar (NET-07), `server_data`,
`add_transient_block`, camera overlays from the equippable component (ITEM-03), the operator block editors
(ITEM-09) with structure / test boxes (VIS-13), area effect cloud particles (VIS-04), all level events and
jukebox songs (FX-06). ITEM-08 was already complete; FX-02 is obsolete (26.3 removed `map_color`). Untested on real hardware: the exclusive fullscreen display-mode
switch (automated runs never change the display mode) and the LAN publish from World Options (it would open a
listening socket).

## Protocol / networking

- **NET-15**: `move_minecart_along_track` (experimental minecarts).
- **NET-16**: debug subscriptions.
- **NET-17**: metadata profile serializer (`src/entity/entities.cpp` ~384).

## Items / UI


## Visuals / effects

- **VIS-06**: glowing outlines, capes.
- **VIS-07 / VIS-14**: profile skins and custom player heads.
- **FX-05**: trail particle params, sulfur/gas particles.
- **FX-07**: post-effect chains (creeper/spider/enderman spectator views etc.).
- **FX-10**: minecart rolling, elytra wind and underwater ambient sounds.

## Screens

- Command suggestions in the command block editor (CommandSuggestions; the chat has them).
- SocialInteractionsScreen (hide players' chat; the pause menu's Player Reporting icon is disabled meanwhile).
- Compare TitleScreen with 26.3 (its icon buttons).

## Gameplay / world

- **PLAY-08**: spectator HUD and menu (hotbar teleport menu).
- **WORLD-04**: Blender port (blending old pre-1.18 chunks with new generation). Low priority: it only affects
  upgraded old worlds.
- **WORLD-06 residual**: 0.3–0.8% feature-position mismatches (coal ore, sculk, moss, dark oak). The cause is open;
  see `docs/worldgen-parity.md`.

## How to build and test (rules for whoever continues)

- **Build:** `powershell -NoProfile -File build.ps1 -Dir Lead2` (test build). `build\Lead` is the owner's.
- **One instance at a time:** take the lock with `mkdir %TEMP%\mcpp-game.lock` and remove it after.
- **Headless test runs:** use `--script` / `MCPP_HEADLESS=1` with `--shaders off` and a scratch `--saves-dir`.
- **Never write to the real `%APPDATA%\.minecraft`.**
- **Local data:** back up and restore `%LOCALAPPDATA%\minecraftCplusplus\options.txt` and `audio_settings.txt`
  around runs. Never touch its `friends\` folder.
- **No perf numbers** while other game instances run.
- **Processes:** only kill PIDs you started.
- **Subagents:** code is written by the main agent; Haiku/Sonnet subagents only explore. At most ~2 at a time.
