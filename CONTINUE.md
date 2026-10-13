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
jukebox songs (FX-06), the tickable sounds (FX-10: minecarts, riding, elytra, bees, guardian, sniffer, underwater,
bubble columns), terrain textures with shaders off (a min-LOD clamp bug), the 26.3 title screen layout, the spectator
menu / HUD / spectate click (PLAY-08), glowing entity outlines and capes (VIS-06), profile
skins for players / mannequins / the player list (VIS-07, NET-17), the Social Interactions screen, trail / sulfur / geyser
particles and the potent sulfur client effects (FX-05), command suggestions and colours in the command block
editors, the spectated entities' post effects (FX-07), experimental minecart movement (NET-15), name / id
only profiles through Mojang's profile service and the skin patch's cape / elytra, the 26.3 F3 debug screen (entries,
Debug Options, key combinations, renderers, charts with the TPS debug subscription: DEBUG-01, NET-16), server post
effects (/posteffect), saved creative hotbars, the smooth camera and spyglass sensitivity, glowing outlines of dropped
items / frames / paintings, minecart contents and the TNT fuse flash, the sleep fade, the friends key, the test instance
beam and error markers, files dropped on the window (packs into the mods folder), F3 + C (debug crash) and F3 + S
(texture dump), the multiplayer safety warning, the experimental features confirmation, cobwebs with Weaving, every
block's animateTick with its sounds (ClientLevel.animateTick, biome ambient particles, the creative markers), rain
sounds, the menu and dragon music, the client-played sounds (pickups, totem, lightning, dragon, portal trigger, air
bubbles, arrow hits, pufferfish, elder guardian curse with its appearance, armor stand hits), the client side of mob
ticks (blazes, endermen with the stare sound, guardians, ravagers, slimes, spellcasters, wardens, sniffers with the dig
sound, breezes, pandas sneezing and eating, the ender dragon's phases and death explosions, feeding baby horses /
llamas / camels), the dragon's death dissolve and rays, the player layers (stuck arrows and bee stingers, shoulder
parrots, deadmau5's ears), the totem activation animation, break cracks on block entities, the End's sky flashes
with their sound, hover tooltips in books, the dolphin / composter / elder guardian particles, the user's own item
sounds (brushing with its dust, spyglass, goat horns, flint and steel), and the video options that did nothing
(Particles, Vignette, Distortion Effects with the nausea overlay, Darkness Pulsing, Glint Speed / Strength, Texture
Filtering / Mipmaps / Anisotropy, Simulation Distance, the rain particles' Weather Radius) and the other options
that were only stored (splash texts, auto suggestions, hidden LAN addresses, hide matched names, lastServer, the
first-server Social Interactions toast, rotate with minecarts, startedCleanly), and the Mojang Studios loading
overlay (start-up, F3 + T, server packs), block entity client ticks (spawner flames, trial spawner / vault
particles and ambience, note block notes), team-formatted name tags with the below-name score, screenshot names /
links, pick block on entities (Ctrl: with data), the fuller F3 + B hitboxes, the happy ghast harness goggles, F3 + Esc
(pause without the menu), the window icon, the quick-craft drag preview, mouse editing in text fields and the
container key / mouse bindings, and the sounds the server leaves to the acting player (doors, trapdoors, fence gates,
buttons, comparators, buckets, bottles, block transformers, shears, dye, allays, milking, igniting creepers), the
sound engine's pause / distance delay / canStartSilent / music volume, the camera_distance attribute and FOV (death
zoom, hand at 70), burning entity flames, the guardian beam and invisible entities (armour shown, the body for
spectators / teammates), the end crystal beams (healing the dragon, beam targets), the renderers' block light
overrides (blazes, vexes, fireballs, XP orbs, glow squids), the shulker bullet halo, XP orbs (icon by value, colour
cycle, translucent), block-attached entities kept on their block (frames / paintings / leash knots, with real pick
boxes), item frame name tags, the renderers' name tag rules (holograms, team name tag visibility), no block outline
behind an entity, the text background opacity on name tags, the dropped item copy seeds, the boat water patch and
bubble-column rocking, display entity shadows and hidden falling blocks inside their block, wolf armour cracks,
piglin / hoglin conversion shaking, beacon beams to the render distance, particle fixes (clouds, snowflakes, end
rods, white smoke, light levels), the effects list beside the inventories and Hud.java details (frozen hearts, air
row, off hand side, Now Playing, the light-level vignette), cursor shapes (requestCursor, Allow Cursor Changes), armour
trims on worn armour (26.3 paletted trim textures) and the smithing table's armour stand preview, Quick Charge and
enchantment value effects, use-animation arm poses, first-person hand details, sprint-hit sprint ending, bubble
columns and gliding rules, own hurt / death sounds, hitting fireballs / shulker bullets / wind charges and the ender
dragon's parts (Entity.isPickable, getPickRadius), predicted entity interactions passing to item use,
attack / use timing details, carrot / fungus on a stick boosts, vehicle fluid rules, block tints exactly as BlockColors (per tint layer, redstone power, stem
age, leaf litter's dry foliage colormap), weighted model variants picked per position (grass / stone / sand rotations,
multipart lists), random block offsets (models, outlines, picking, collision), flat full-emission lighting of light sources
(emissiveRendering, element light_emission), the dimension's cardinal shade (Nether: up / down 0.9; model faces
shaded by their nearest direction), smooth lighting of block models (BlockModelLighter: per corner light and ambient
occlusion with partial-face weights; flat lighting by the culled / boundary face's cell) and fluid shapes (FluidRenderer:
averaged corner heights, flow-turned flowing textures, side faces with the water overlay; full detail only, LOD keeps
cubes); then audits of weather, block breaking, music / ambient sound, sky / clouds, camera, HUD overlays and status bars, entity layers (shadows, fire, name tags, leashes, player layers, carried items, wool / saddles / armour), projectiles, signs, maps, spawners / vaults, block entity animations, first-person hands, chat, GUI item decorations / tooltips and entity interpolation, with their mismatches fixed (fog: FogRenderer's environmental + render distance fogs). ITEM-08 was already complete; FX-02 is obsolete (26.3 removed `map_color`). Untested on real hardware: the exclusive fullscreen display-mode
switch (automated runs never change the display mode) and the LAN publish from World Options (it would open a
listening socket).

## Protocol / networking


## Items / UI

- **HashedStack component hashes**: container clicks send hash 0 for every component of a changed / carried stack
  (Java hashes each component through its codec with HashOps), so the server resyncs such slots after each click.
  Porting needs a canonical per-component encoding (DataComponents codecs) feeding CRC32C like HashOps.


## Visuals / effects

- **Improved Transparency** (video option) is stored but not read; this engine's translucency path (OIT under the
  shader pipeline) is its own. The nausea overlay and vignette draw after the UI layer (they also tint the HUD).

- **Block rendering audit, still open**: smooth lighting (cubes and models, one BlockModelLighter port) clamps its
  lookups to the section's one-cell border (Java reads up to two cells beyond a face for light permeability). Fluids skip self-occlusion of waterlogged partial blocks (faces hidden inside them).
- **Audit leftovers (2026-10-13)**: remote entities are not pushed by client-side pistons (Entity.move(PISTON) and its applyPredictedMovement), passengers are placed at render time; keyboard-focused widget tooltips (BelowOrAboveWidgetTooltipPositioner) and the widget tooltips of the other screens' own widgets (recipe book buttons, server / world list buttons) still use the mouse positioner; the chat's MODIFIED tag and RestrictionsScreen. (Minecarts under minecart_improvements: NO_OP interpolation, untested without an experimental world.)
- **VIS-06 residual**: glowing outlines under a shader pack (entities go through the pack's gbuffers there).
- Test instance blocks: optional tests' failure beam is red, not orange (the test registry is not sent to clients);
  the error markers and game test highlights are drawn in the UI layer (no depth test).

## Screens

- The friends feature is this port's own; Java's FriendToast (Mojang's friend service) needs the Microsoft login.
- Deliberate differences: mouse look stops at +-1.55 rad pitch (Java: 90 degrees; the camera bases here come from a
  cross product with up); a full static voice pool replaces its quietest voice with a louder new sound (Java drops the
  new one); the window title (an FPS / GPU line in game, not "Minecraft* 26.3 - Singleplayer"); the
  level loading screen follows the server process's log, without LevelLoadingScreen's chunk map; no OutOfMemoryScreen
  or crash-report text (crashes write a minidump to %LOCALAPPDATA%\minecraftCplusplus\crashes).
- Video options of the engine's own: Max Framerate steps follow display refresh rates (30 .. 360, Unlimited) instead
  of 10 .. 260; Render Distance reaches the detail distance (beyond 32); Graphics API is a fixed Direct3D 12 label;
  Simulation Distance has a tooltip about applying on the next world load (the server is a separate process).
- SystemToasts without a trigger here: PACK_LOAD_FAILURE, FILE_DROP_FAILURE, CHUNK_LOAD/SAVE_FAILURE (the server
  runs out of process), the demo overlay and the regional PeriodicNotificationManager.


## Debug

- **DEBUG-01 residual**: the profiler pie (F3 + 1) and F3 + L profiling (no profiler of the game's kind), the sky
  light sections renderer, the chunk culling / octree renderers (the game's section graph), labels of the projected
  renderers not scaled with distance.

## Gameplay / world

- **Test commands in 26.3**: block states in NBT are strings (`{block_state:"minecraft:stone"}`); the old
  `{Name:...}` compound is silently ignored (an air display).
- **Placement prediction (game/placement.cpp) residual**: not predicted client-side (the server's block updates
  fix them a moment later): rails' shapes, tripwire, vines / glow lichen /
  multiface blocks, chorus / mushroom stems, big dripleaf, pointed dripstone, scaffolding distance, note block
  instruments, repeater / comparator neighbours' power. The face sturdiness tests use collision boxes (Java: the
  support shape), which differ for leaves, scaffolding and a few others.
- **WORLD-04**: Blender port (blending old pre-1.18 chunks with new generation). Low priority: it only affects
  upgraded old worlds.
- **WORLD-06 residual**: 0.3–0.8% feature-position mismatches (coal ore, sculk, moss, dark oak). The cause is open;
  see `docs/worldgen-parity.md`.

## How to build and test (rules for whoever continues)

- **Build:** `powershell -NoProfile -File build.ps1 -Dir Lead2` (test build). `build\Lead` is the owner's.
- **One instance at a time:** take the lock with `mkdir %TEMP%\mcpp-game.lock` and remove it after.
- **Headless test runs:** use `--script` / `MCPP_HEADLESS=1` with `--shaders off` and a scratch `--saves-dir`.
- **Never write to the real `%APPDATA%\.minecraft`.**
- **Local data:** back up and restore `%LOCALAPPDATA%\minecraftCplusplus\options.txt`, `audio_settings.txt` and
  `command_history.txt` (scripted `/commands` are persisted there; delete it after a run if it did not exist before)
  around runs. Never touch its `friends\` folder.
- **No perf numbers** while other game instances run.
- **Processes:** only kill PIDs you started.
- **Subagents:** code is written by the main agent; Haiku/Sonnet subagents only explore. At most ~2 at a time.
