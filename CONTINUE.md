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

Then (2026-10-13, later): client block placement prediction (game/placement.cpp: BlockItem.place's
getStateForPlacement for the common families, stacking, scaffolding columns, walls, redstone wire, vines / multiface,
dripstone, the neighbours' updateShape and the other halves of doors / plants / beds); the step-up of Entity.collide;
the scale attribute for every living entity and the local player; client movement of items, XP orbs and falling blocks
(entity/entity_physics.cpp); interpolated texture animations; inline painting variants and unsent synched data
defaults (paintings, primed TNT, villagers, interactions with their size); block variants for displays / pistons;
particle fixes from a full provider audit; widget tooltip positioning (MenuTooltipPositioner); chat trust levels and
the Restrictions screen from chat; smooth lighting's second ring and fluid face occlusion; container drag / double
click rules; riding input and camel / horse rules; the world border depth state. The Oct 11 backlog in
docs/audit-26.3 was re-checked: most of it is done, its remaining items are folded into the sections below.

Then (2026-10-13, last): the advancements screen's init / tab / hover rules; toasts as ToastManager (slot heights,
sound caps, SystemToast wrapping, TutorialToast lines, 10 s toasts, the unsecure-server toast, hidden with the HUD);
the server list as ServerSelectionList / ServerStatusPinger (address validation, hidden direct-join entries, lazy
pings on five threads, the legacy ping fallback, the pinging / unreachable / incompatible / old / unknown states,
LAN entries, Shift+arrow moves, F5) and ConnectScreen while joining; the world list as LevelSummary / WorldOpenFlows
(incompatible / conversion / file fixing / downgrade / experimental states and overlays, the backup questions, low
disk space, an empty list opening Create World), OptimizeWorldScreen, the Message / Confirm screens as
DisconnectedScreen / AlertScreen / ConfirmScreen, and tab list heads; note block instruments predicted (a table generated
from Blocks.java); Create World (Allow Commands, debug states, flat preset parsing, the flat list, game rule styles);
Statistics (loading tab, selection, sorting, Ctrl+Tab); the menu background blur (Canvas.markBlur + PostEffects.blur
mid-UI, both with and without the UI layer; the pause menu and the menus use it, block editors the gradient); lily
pads / frogspawn and mushrooms predicted; the death screen's title confirmation and score; game mode switching
permissions and the previous mode; credits details; options sliders and Ctrl+wheel GUI scale; repeater locking; the
death message's hover / links; books (lengths, truncation, lectern click events, silent key page turns) and the sign
preview in the world; the recipe book (narrow windows, overlay, resets, id searches, ghosts, toast list); station
screens (tooltips while carrying, stonecutter / loom clicks, the merchant's active offer, mount slots); movement
fixes (block speed while flying, underwater, crouch timing); component hashes for container clicks; movement sounds
(emission gating, fall sounds by block, honey slides). Fall sounds do not model the pointed dripstone's fallOn.
Social interactions (row order, tooltip delay, hidden list per launch) and keys (global F2 / F11, F3 over screens,
spectator drop / swap). Not ported: F3+L client profiling (no metrics recorder); the Ctrl+B narrator hotkey is only
kept out of Chat and Language (no general "an EditBox has focus" query); the social screen's report button stays
disabled (reporting needs a Microsoft account). HUD order, creative inventory, tab list and chat (tags, restrictions,
history) audited and fixed. Not ported: the chat screen's own narration (title, usage, "Message to send"); tab heads
show in singleplayer though the local server is offline (deliberate); subtitles draw before the toasts, not after.
Container clicks (drag on mouse moves, remainder count, highlightable slots), block breaking (redstone ore on the
client, invisible blocks without particles), view bobbing, entity use (the hand loop) and picking, and the camera
audited and fixed. Camera residuals while spectating an entity: its eye height is the type's constant (no pose /
scale / easing), its hurt tilt and walk bob are not applied, and the FOV modifiers stay 1 (the game uses a spectated
player's flying / speed / bow); the minecart camera branch is not implemented. Toasts follow ToastInstance now (the
server pack download toast shows its progress). Toast residuals: no toasts
for world access / chunk load or save /
file drop / pack load failures or periodic notifications; friend toasts have no face (this port's friends have no
game profile); the Online screen's Friends section has only In-Game Notification (the rest is Microsoft's). Tested against local mock status servers
(a Python script: modern, no-version, legacy-only, silent and closing servers).

## Protocol / networking


## Items / UI

- **HashedStack component hashes** (items.cpp Items::componentHash): computed like the game for damage, max damage,
  repair cost, max stack size, map id / colour, dyed colour, glint override, rarity, the unit components, names and
  lore, (stored) enchantments and custom data (verified: no resync after clicks); every other component still sends 0
  and the server resyncs that slot.


## Visuals / effects

- **Improved Transparency** (video option) is stored but not read; this engine's translucency path (OIT under the
  shader pipeline) is its own.

- **Block rendering, approximations**: face occlusion shapes come from collision boxes (fluid faces against partial
  blocks; model faces against partial neighbours still cull only on full cubes, not Block.shouldRenderFace's shapes).
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
- Server list residuals: Java pings each list entry (two entries of one address ping twice; here one ping per
  address), and a ping shows "Pinging..." from the start rather than an empty MOTD until the socket connects. A legacy-only
  server shows its incompatible icon at once (Java keeps the last pinging frame until the list is rebuilt). The
  disconnect screen's "Open Report Directory" / "Report to Server" buttons come with client packet-error reports;
  this client logs malformed packets and goes on instead of disconnecting.
- World list residuals: Delete moves the world to the Recycle Bin (Java deletes it) and Shift turns Play into this
  port's View Only; the old customized-world question (Re-Create, pre-1.13 worlds) is missing; after file fixing the
  server upgrades on start, without Java's "upgradeWorld.done / joinNow" confirmation; Optimize World shows one bar and
  the total from the server's log (no per-dimension bars or converted / skipped counts); a failed backup's toast has
  no reason line.
- Station residuals: the loom's banner preview is a flat quad (Java: the 3D BannerFlagModel); the smithing armor
  stand shows no item model on its head for head items without an equippable asset; selecting a trade does not
  pre-fill the payment slots before the server's sync (MerchantMenu.tryMoveItems).
- Pack screens: Options' Resource Packs and Create World's Data Packs are this port's Mods & Packs manager (one list
  of switches) rather than PackSelectionScreen's Available / Selected lists.
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
  fix them a moment later; rails are straight along the look as the Java client places them;
  repeaters' locked state is predicted, power changes are the server's as in Java): BlockItem.canPlace's canSurvive beyond the covered families
  (vegetation and wither roses by their tags, crops / stems on farmland, carpets, rails, diodes, pressure plates, door
  bottoms, torches, signs, banners, lanterns, attached blocks, ladders, vines, dripstone, wire, mushrooms, lily pads and
  frogspawn): tall seagrass and the like (dry vegetation, nether plants, wart, bamboo, cactus, sugar cane,
  kelp / seagrass, the growing vines and cocoa are in). The face sturdiness tests use collision boxes (Java: the
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
