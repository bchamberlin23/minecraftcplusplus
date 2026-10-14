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
audited and fixed. Spectating an entity: its eye height (pose, baby dimensions, scale) eases like ours, a living
entity's hurt tilt (direction 0) / death roll and a player's walk bob and FOV modifier (default abilities, speed with
its sprint flag, bow, spyglass) apply; residual: the experimental minecart camera branch (NewMinecartBehavior). Also audited and fixed: music (fades, underwater), tooltips, key
binds, the spectator menu (residual: teams listed alphabetically, Java's hash order), the title screen (the first
one fades in; deliberate: the live world panorama without the overlay, Mods & Packs in the Realms slot, the port in
the version line) and command suggestions. Then: name tags, advancements, boats (push rule), camels, elytra (the
server ends the glide), item use (crossbow 72000, the unclamped counter, horn durations), the attack's item dip.
Residuals noted by the audits (the lava first tick, the shield's block delay / 15 degree head limit, the hit boat /
minecart nudge, mount Jump Boost, the camel standing up on forward input are done): the hurtClient rules of frames /
paintings / cushions in adventure, the camel passenger's sprint (held key only, no double tap), horizontal wheel
scrolling (no such input).
Later: armor (black dye, wings on piglins / drowned), riding / dead walk animation, spectator bodies, map label
depth, the camera's fluid (near plane), weather (biome precipitation data, instant levels). Fog residuals: the
render-distance fog cylinder follows the far-terrain fog distance, not the chunk setting (deliberate for the LOD
terrain); a non-atmospheric environment can only shrink the sky fog end; decal armor trims use the normal pipeline.
Lightmap (flicker, End flash), outline gating, particles (box collision, limits), conduit frames, the shulker box
lid pushing the player done (other entities are the server's). The level loading screen for respawns / dimension
changes (LevelLoadTracker: event 13, the player's chunk, 30 s) and the 30 s read timeout are in; the end portal
background is approximated (no END_PORTAL pipeline in the GUI). Network residuals: select_known_packs claims nothing
(the server sends every registry in full), packets after a transfer in the same stream are dropped, the brand payload
is read in the configuration phase only. Pickups (a flying copy for orbs and partial stacks) and the living render
effects (upside-down xRot, creeper flash steps / ignited swelling, invisible hurt tint, skeleton shake) audited and
fixed; residual: remote players' eye height for the pickup target is the type's constant (no pose). Anvil /
grindstone / cartography, brewing / crafter / beacon / bundles and the death / win screens audited and fixed;
residuals: screens without a known title narrate only the usage hint on opening, the win screen's End
portal background is approximated, the death screen is not reopened (nor the player respawned) when another screen
closes while dead, no draft-report icon on the death screen's title button. Books / lectern and the operator block
editors audited and fixed; residuals: writable pages ignore text filtering (getPages(isTextFilteringEnabled)), the
test instance's error message is the one it had when opened. Merchant / enchanting and special-block movement
(bubble columns per cell, stuck order, straw beds) audited and fixed; residual: the player's block effects are
sampled at the box after the move, not along the swept path (Entity.checkInsideBlocks over the movement segments).
Fluid rendering audited: residuals are BlockState.isSolid and #blocks_fluid_flow approximated by the collision shape
(+ signs) in the mesher, heights in 1/240 steps and the flow angle in 255 steps, no 0.001 inset on the sides' edges,
no lava back faces. Loom / stonecutter / smithing / mount screens audited; residuals: the loom predicts no result and
draws its banner preview flat (no BannerFlagModel), mount slots use fixed type lists (not #can_equip_saddle / #can_wear_horse_armor). Boss bars / titles / action
bar and the beam renderers audited and fixed (the end crystal beam's black-to-white shading is per vertex:
MeshQuad kQuadGreyGradient). Added: DatapackLoadFailureScreen (a local
server stopping on its datapacks offers Safe Mode = --safeMode; tested with a broken dimension_type), the symlink
warnings (NoticeWithLinkScreen; allowed_symlinks.txt; untested end to end: creating symlinks needs admin here).
Screens still missing (by a sweep of client/gui/screens): TelemetryInfoScreen (the Telemetry button stays a disabled
stub: no telemetry here), FileFixerProgressScreen / FileFixerAbortedScreen (the server process does the file fixing;
the loading screen shows it), OutOfMemoryScreen, UnsupportedGraphicsWarningScreen (no GPU blocklist), the ban notices
and player reporting screens (Microsoft account). Coverage sweeps: every particle provider, entity renderer, block
entity renderer and special item model has a counterpart (0 of 161 entity types fall back to a box); thrown items /
fireballs / the ender eye / fireworks now draw their item models (potion tints, custom item models). Packet sweeps:
every clientbound packet is handled except the debug/* values (development builds only subscribe to them) and the
empty combat enter/end; every serverbound packet is sent except chat_session_update (profile keys: Microsoft).
Video settings audit: Improved Transparency is stored but the renderer has no Fabulous-style separate translucency
targets (its own sorting stays); the Entity Distance option reaches the local server only at launch (the integrated
server reads it live); no graphics backend choice (D3D12 only). Placement prediction audited (two passes over ~40
block families) and fixed; the client's redstone view (placement.cpp hasNeighborSignal) reads block-entity outputs
(jukebox songs, trapped chest viewers, comparator levels) as 0 and treats opaque cubes as the conductors. Block use
and item-on-block prediction audited and ported (doors, levers, repeaters, cake, candles, pots, anchors; axe / hoe /
shovel / honeycomb transforms, flint / fire charge fire, shears, campfire dousing); fire held only by flammable
neighbours (no solid floor) is left to the server (no flammability table on the client). Mob keyframe animations
(warden, sniffer, camel, armadillo, breeze) audited: the definitions come from the jar (animations_v1.json, cached by
version only), triggers and procedural poses match. Item model properties audited; residuals: a select with no case
and no fallback draws nothing (Java: the missing model), local_time ignores "locale" (English names) and is not
cached per second, select on "component" matches the named fields only (undecoded components never match a value).
F3 and world border audited; residuals: memory lines are the process's (no JVM heap), local difficulty / chunk
generation / spawn counts show nothing (integrated-server data), the border wall is rebuilt every frame (Java: per
camera section). Sound engine and level events audited; residuals: the voice pools are fixed at 247 + 8 (Java:
from the device's source count) and a full pool replaces its quietest voice (deliberate), streams buffer 0.5 s.
Entity interaction and attack audited; the client now predicts held-stack changes (placement, one-item uses, buckets,
milking, stew, feeding) and tells SUCCESS from CONSUME / SUCCESS_SERVER. Tame animals know their owner (DATA_OWNERUUID_ID);
max health is the synced attribute or the type's default (Gameplay::maxHealthOf, a table from createAttributes).
Item use predicts thrown items, bottle filling and armour swaps; horses / llamas / camels, fish buckets, iron golem
repair, furnace minecart fuel, tadpoles, ocelots and golden dandelions follow their mobInteract. Shears cut leashes and shear off saddles / harnesses / carpets
(Mob.attemptToShearEquipment), a tamed wolf's armour is repaired while sitting; durability changes are the
server's (ItemStack.hurtAndBreak does nothing on the client). Not predicted: villager trading or sleeping state. Block use covers composters, beehives, campfires, cauldrons (CauldronInteractions results), jukeboxes,
berry bushes, cave vines, vaults, shelves and lecterns; not modelled: the shelf's hit slot, chiseled bookshelf slots,
signs' waxed / applicator swing, the shovel's durability on a campfire. Client projectiles: arrows, throwables,
fireworks, TNT, eyes of ender, shulker bullets, llama spit and fishing hooks (flying / hooked / bobbing, the bite
dip) follow their own client tick. Container clicks audited (PICKUP / QUICK_CRAFT / PICKUP_ALL / SWAP / CLONE / THROW
match; a crafting remainder that does not fit swings the arm like Player.drop, the server drops it). Camera: a
spectated entity's hurt tilt, bob, eye height and FOV follow it (Game::entityCamera).
Audited and matching: maps, name tags, tab list, sidebar, boss bars, toasts, chat, advancements. Residuals: command
suggestions ask the server for every argument (Java lists entity / registry / position suggestions locally), no
suggestion narration; friend toasts have no face; no resource pack load-failure toast; the advancements screen
ignores the horizontal wheel. Boats: no client outOfControlTicks (the server ejects; the bubble column splash and
the rider's 105 degree clamp on mouse turns are done). Server list: a favicon that is not 64x64 is kept in
servers.dat (Java drops it), the MOTD is not sanitised (depth 16, player sprites rejected). Create World: a blank
name is stored as "New World" (Java stores it empty), no narration. The allay's give / take / duplicate is predicted
(its hand and the player's stack). Not ported:
filtered sign text (needs the account's profanity filter flag). The bell's raider particles (showBellParticles) are
invisible in 26.3 (ColorParticleOption.create(ENTITY_EFFECT, 0xFED839 + 5i) has alpha 0, which SpellParticle keeps),
so they are not drawn here either. Toasts follow ToastInstance now (the
server pack download toast shows its progress). Toast residuals: no world access failure toast
(world errors show screens) or regional periodic notifications; friend toasts have no face (this port's friends have no
game profile); the Online screen's Friends section has only In-Game Notification (the rest is Microsoft's). Tested against local mock status servers
(a Python script: modern, no-version, legacy-only, silent and closing servers).

## Protocol / networking


## Items / UI

- **HashedStack component hashes** (items.cpp Items::componentHash): computed like the game for damage, max damage,
  repair cost, max stack size, map id / colour, dyed colour, glint override, rarity, the unit components, names and
  lore, (stored) enchantments, custom data, potion contents with custom effects, writable / written book contents
  and bundle / charged projectile / container item lists, fireworks, banner patterns, base colours, trims, stew
  effects, pot decorations, block states, instruments, lodestone trackers, custom model data, tooltip display, note
  block sounds, bucket / entity / block entity data, bees, attribute modifiers, profiles, food, use cooldowns, weapons, painting
  variants, item models, tooltip styles, enchantability, dyes, waxing, minimum attack charge, potion duration scale, tools and
  equippables
  (verified: no resync after clicks); the other gameplay components set by commands (consumable, can_break, death_protection...; only an extra slot update, nothing visible)
  still send 0 and the server resyncs that slot.


## Visuals / effects

- **Improved Transparency** (video option) is stored but not read; this engine's translucency path (OIT under the
  shader pipeline) is its own.

- **Block rendering, approximations**: face occlusion shapes come from outline / collision boxes; model faces follow
  Block.shouldRenderFace with the face shapes of slabs, stairs, carpets, farmland and dirt paths (other partial
  blocks are treated as not occluding) and the pane / bars, slime / honey and mangrove roots skipRendering rules.
- **Keyboard focus**: Tab / arrows / Enter / Space navigate the buttons of the option lists (and their drop-downs),
  the pause, death, options, notice / confirmation and button-only front-end screens (the world / server lists: Tab
  to their buttons), with the focus highlight, slider editing and the focused widget's tooltip below it; containers,
  books, editors and the screens with search fields keep their own keys.
- **Audit leftovers (2026-10-13)**: remote entities are not pushed by client-side pistons (Entity.move(PISTON) and its applyPredictedMovement); riders are placed each tick (positionRider, babies' dimensions dumped) but a horse's seat uses standAnimO of our own animation; keyboard-focused widget tooltips (BelowOrAboveWidgetTooltipPositioner) and the widget tooltips of the other screens' own widgets (recipe book buttons, server / world list buttons) still use the mouse positioner; the chat's MODIFIED tag and RestrictionsScreen. (Minecarts under minecart_improvements: NO_OP interpolation, untested without an experimental world.)
- **VIS-06 residual**: glowing outlines under a shader pack (entities go through the pack's gbuffers there).
- Test instance blocks: optional tests' failure beam is red, not orange (the test registry is not sent to clients);
  the error markers and game test highlights are drawn in the UI layer (no depth test).

## Screens

- The friends feature is this port's own; Java's FriendToast (Mojang's friend service) needs the Microsoft login.
- Deliberate differences: a full static voice pool replaces its quietest voice with a louder new sound (Java drops the
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
- Station residuals: the loom's banner preview is a flat quad (Java: the 3D BannerFlagModel). GUI entity previews
  draw held and head items as their item models (UiPass binds the item / entity atlases and block textures); a
  worn skull there is its item model, not SkullBlockRenderer's head.
- Pack screens: Options' Resource Packs and Create World's Data Packs are this port's Mods & Packs manager (one list
  of switches) rather than PackSelectionScreen's Available / Selected lists.
- SystemToasts: chunk load / save failures and low disk space come from the local server's log, pack load failures
  from start-up; FILE_DROP_FAILURE has no trigger in the game either; no demo overlay or regional
  PeriodicNotificationManager.


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

Path tracing ("Shaders: Path Traced", DXR 1.1 inline queries; DLSS Ray Reconstruction denoises and upscales when
DLSS is the upscaler): hybrid, the raster G-buffer gives the primary surfaces, lighting is traced within 128..384
blocks (Low..Ultra; Ultra 3 bounces, 2 spp). Entity hits (mobs, block entities, displays) are textured, lit by their
light levels and alpha tested. RR's normal guide at entity pixels comes from the entity pass's third target
(TexEntNormal; block entities and held items still fall back to the view direction; MCPP_PT_DEBUG=9 shows the guide).
Residuals: water refraction is screen space; opaque blocks have no specular (vanilla has no specular maps); terminal bounce hits use
the game's light levels; the built-in denoiser assumes a static world (moving entities ghost without RR).

Performance (October 2026, RTX 5070 Ti, 1080p, Shaders Off + DLSS Quality): about 1130 fps, p99 frame 2.8 ms, with
CPU (~0.55 ms) and GPU (~0.68 ms) roughly balanced (`--profile` prints the main thread's phases at exit). Load times:
- Launch to the first frame takes ~2.5 s. ~2.1 s of it is the Streamline chain (slInit ~0.9 s on the preinit thread,
  then slSetD3DDevice ~1.15 s of serial NGX feature init in the driver); the rest of the game is ready by ~0.45 s.
- The local server is launched directly from the bundler's classpath with a JDK AOT class cache
  (`server/26.3/aot/`, trained once in the background), at above-normal priority until it is up.
- Entering a world: in the world at ~4.3 s (the server's own bootstrap is the critical path).
- Save and Quit to Title: the title instance starts while the world saves (handoff gate) and shows in ~3 s.
- JVM flags (ParallelGC, -UsePerfData, CICompilerCount, C1 only) measured no faster.
- Real terrain with the default far terrain (16384 blocks, ~170k nodes in the cut), 720p DLSS: Shaders Off ~880 fps,
  Shaders High ~400 fps. The translucent pass had been 2.6 ms of empty indirect draws (fixed slots per section: now an
  ordered count / scan / emit compaction); the outer shadow cascades update at capped rates; translucent sorting is a
  radix sort. `--profile` logs every 5 s and Shaders Off per-pass GPU times at exit. Open: LOD meshes carry no light
  (sky 15, no AO; giving them the smooth path's AO cost +38% geometry and ~7% fps for little visible change), so
  distant lighting still differs a little where full detail takes over.
- Frame generation with Shaders Off is untested in headless runs (DLSS-G presents no generated frames for a background
  window, with or without shaders): test it in a focused window.
