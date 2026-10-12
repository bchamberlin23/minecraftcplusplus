# Continue: what's left (as of 2026-10-11)

Remaining work from the 26.3 audit (`docs/audit-26.3/`). Microsoft login is permanently out of scope, along with
everything that depends on it: chat signing (NET-14) and player reporting (WORLD-10).

Last pushed commit: `e431537` (entity blob shadows). The owner's `build\Lead` has not been rebuilt since; rebuild it
only when the game isn't running (`tasklist | findstr minecraftCplusplus`).

## Options that are saved but never read

Each of these exists in `Settings` (`src/ui/ui.h`) and on its options screen, but nothing in the game uses it yet.

| Audit | Option | Where to wire it |
|---|---|---|
| OPT-11 | `panoramaSpeed` | `src/main.cpp:1059`: `cam.yaw += dt * 0.035f`. Multiply by the setting. |
| OPT-05 | `damageTiltStrength` | No hurt camera tilt exists yet. Add GameRenderer.bobHurt (hurt_animation yaw, `hurtTime`) to the camera, scaled by the setting. |
| OPT-05 | `hideLightningFlash` | Settings field is unused. Sky reads `options_.hideFlashes` (`src/sky/sky.cpp:590`); check it is fed from this setting. |
| OPT-05 | `highContrastBlockOutline` | Block outline colour: vanilla draws a black/white outline when this is on. |
| OPT-05 / FX-09 | `showSubtitles` | Needs a subtitle overlay (SubtitleOverlay): audio `onSound` callback, then a HUD list with direction arrows. |
| OPT-05 | `onboardAccessibility` | First-launch accessibility onboarding screen. |
| OPT-06 | `musicFrequency`, `musicToast` | Audio music manager: delay between tracks; the "now playing" toast and pause-menu display. |
| OPT-10 | `inactivityFpsLimit` | Main loop: cap fps when minimized (0) or AFK (1). |
| OPT-10 | `showAutosaveIndicator` | HUD autosave spinner while the world saves. |
| OPT-04 | `prioritizeChunkUpdates` | Mesh scheduling: rebuild player-affected / nearby sections synchronously. |
| OPT-02 | `toggleAttack`, `toggleUse` | Not in `Settings` at all. Add them like `toggleCrouch` (`src/game/game.cpp:160`). |
| OPT-03 | `overrideWidth/Height`, `fullscreenResolution` | Not present. Window sizing plus exclusive fullscreen mode selection. |

## Protocol / networking

- **NET-07**: waypoints and the locator bar (`waypoint` packet plus the HUD bar).
- **NET-13**: the `game_rule_values` packet.
- **NET-15**: `move_minecart_along_track`, `set_entity_link` (leashes), `projectile_power`.
- **NET-16**: debug subscriptions.
- **NET-17**: metadata profile serializer (`src/entity/entities.cpp` ~384).

## Items / UI

- **ITEM-03**: equippable component (slot, sounds, model, camera overlay).
- **ITEM-08**: attribute modifier tooltip lines.
- **ITEM-09**: editors for command blocks, jigsaws, structure blocks and test blocks.
- **ITEM-10**: the operator items creative tab.

## Visuals / effects

- **VIS-04**: area-effect-cloud particles.
- **VIS-06**: leash ropes (needs NET-15 `set_entity_link`), glowing outlines, capes.
- **VIS-07 / VIS-14**: profile skins and custom player heads.
- **VIS-13**: structure and test block bounding boxes.
- **FX-02**: `map_color` tint.
- **FX-05**: trail particle params, sulfur/gas particles.
- **FX-06**: remaining `level_event` types.
- **FX-07**: post-effect chains (creeper/spider/enderman spectator views etc.).
- **FX-10**: minecart rolling, elytra wind and underwater ambient sounds.

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
