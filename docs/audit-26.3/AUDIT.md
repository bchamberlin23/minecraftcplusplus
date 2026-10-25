# Minecraft 26.3 port audit

Audited October 11, 2026. Reference: `C:/Users/Admin/Desktop/Minecraft Split Screen/decompiled-26.3`. Reference `version.json` identifies **26.3, protocol 777, world data version 5023, resource pack 97.1, data pack 121.0, Java 25**. Repository base: `c69e6c3a74b18f4dfc077e5426a4e8bf75d8ab3f` plus the working tree captured in `inventory.json`.

The largest remaining work is client parity: options and input, network-driven UI/state, item component handling, specialized visuals, and client-side prediction. This is already a substantial engine, but importing assets, extracting models, or registering a packet does not establish that the corresponding behavior has been ported.

## Scope and how to read this audit

The audit inventories all 241 C++ source/header/include files, 18 shader files, and 7,301 Java reference files. It compares the reference's saved options, bindings, screen classes, protocol declarations, components, particles, renderer registrations, and worldgen registries. Detailed findings follow implementation paths through startup, handlers, UI modules, prediction, rendering, and persistence. This is a **source audit**, not an assertion that every line of either codebase was manually reviewed, nor a completed runtime parity certification.

The generated CSV files retain exact reference paths and line numbers. `source-files.csv` and `shader-files.csv` retain SHA-256 hashes to identify the audited implementation. Other work was editing rendering/terrain files during this audit; those changes were preserved, and the final inventory captures the resulting source snapshot. This audit adds documentation and a repeatable inventory tool; it does not change game behavior.

Finding statuses:

- **Missing**: no implementation path exists, or code explicitly rejects/omits it.
- **Partial**: a path exists but concrete reference behavior is omitted or simplified.
- **Verify**: implementation exists, but source inspection alone cannot establish parity.
- **Delegated**: supplied by the official local/remote Java server; absent native logic is not itself a missing user feature.
- **Different scope**: Java-specific infrastructure, OS-specific options, account services, or intentional engine extensions need a product decision rather than blind copying.

Priorities: **P1** blocks ordinary gameplay/compatibility or can make saved/native behavior incorrect; **P2** is visible functionality or a broadly useful option; **P3** is polish, advanced tooling, or service scope. Priority assumes an eventual broadly compatible 26.3 client.

There are **85 reviewed findings: 28 P1, 54 P2, and 3 P3**. Findings group related work; they are not counts of individual missing features. `backlog.csv` exports the same findings for sorting and planning.

## What the inventory actually establishes

| Reference surface | Result | Interpretation |
|---|---|---|
| Fixed saved options | 113 keys; 8 have equivalent native settings persistence | 105 lack that persistence path. Some have hardcoded defaults, alternate pack/settings systems, or Java/OS-specific scope. |
| Key mappings | 53 individually named declarations plus 9 hotbar bindings | Actions are hardcoded; there is no configurable bindings table or keybind screen. |
| Minecraft client screens | 128 classes, separately classified in `screens.csv` | Includes base classes and generic message screens; not 128 independent missing features. |
| Realms screens | 28 classes | Service/UI scope absent. |
| Play packets | 129 clientbound declarations; 85 have direct/adapter handler evidence | 44 lack a handler; several are synthetic/no-op/diagnostic. The actionable omissions are described below. |
| Play sends | 61 serverbound declarations; 37 have named send evidence | 24 absent named sends include alternative movement packet forms, not just missing actions. |
| Item components | 122 types; 32 fall through `Items::skipValue` to failure | Concrete compatibility limitation. Generic slash-name varint handling and inline holders require additional codec review. |
| Particles | 128 types; 53 have explicit kind mappings; 73 use generic behavior; 2 reject network arguments | Generic support is an approximation, not a missing particle registration. |
| Entity renderers | 150 reference registrations | 13 types rely on default name-based models/textures instead of species-specific selection. Ten resolve basic vanilla assets; three have absent default texture paths and fall back to boxes. Species-specific states/layers/animation remain incomplete. |
| Block entity renderers | 26 reference registrations | 10 special renderer families are absent after accounting for chest/sign families. Ordinary block meshes may still display their base blocks. |
| Native feature types | All 58 names have implementation/registration evidence | This does not establish exact block output or side effects. |
| Structure types | 13 of 16 selected natively | `end_city`, `fortress`, `nether_fossil` are absent. |
| Structure processors | All 11 implemented in the structure-template path | The separate template-feature path only handles a subset. |
| Trunk/foliage placers | All 10 trunk and 12 foliage types have implementation evidence | Exact RNG/placement parity still requires differential fixtures. |

Do not calculate an overall “percent ported” from these counts. Settings, renderer families, gameplay, and Java-only infrastructure have very different scope, and string matches are not behavior tests.

## Architecture: what is already provided by Java

`src/net/local_server.h:2` explicitly uses the official dedicated server for mobs, redstone, survival, and world rules. `src/net/local_server.cpp:432` configures offline mode; `src/main.cpp:338` launches it for `--play`. The frontend starts worlds through this path. Consequently AI, spawning, farming/growth, redstone simulation, loot/recipes, combat damage, hunger/XP, advancements/statistics authority, Nether/End generation, bosses, and most block/item interactions are **delegated**, not wholly missing from playable singleplayer.

The native viewer/edit path is different: `Game` defaults to creative, native generation writes blocks, and no native server simulation replaces Java. If the goal is a Java-free game, the above delegated systems remain major unported subsystems. If the goal is a native client with a Java server, focus on client omissions and the integration defects below. Native visual terrain outside server range also must not be confused with authoritative simulated chunks.

## Settings and input gaps

The complete per-key baseline is `settings.csv`; complete named bindings are in `keybindings.csv`. Native equivalents currently persisted are `fov`, `renderDistance`, `guiScale`, `mouseSensitivity`, `enableVsync` as `vsync`, `maxFps`, `bobView`, and `attackIndicator`. Custom shader, upscaler, sharpness, frame-generation, latency, and shader-pack settings are additional engine features.

| ID | Priority/status | Missing or incomplete behavior | Evidence and consequence |
|---|---|---|---|
| OPT-01 | P1 Missing | Remappable keyboard/mouse bindings and conflict/reset UI | Java `Options.java:660` and `KeyBindsScreen`; C++ `src/game/game.cpp:154` and `src/ui/ui.cpp:1020` hardcode keys. Rebinding or using alternative movement layouts is unavailable. |
| OPT-02 | P2 Missing | Auto jump; X/Y inversion; scroll sensitivity/discrete scrolling; crouch/sprint/attack/use toggles; sprint window | `Options.java`, `MouseSettingsScreen`, `ControlsScreen`; native settings in `src/ui/ui.h:38`, input in `src/game/game.cpp:154`. Only mouse sensitivity is exposed. |
| OPT-03 | P2 Missing | Fullscreen mode, exclusive/fullscreen resolution selection, F11 action | `Options.java`, `VideoSettingsScreen`; `src/platform/window.cpp` creates a normal window and has no fullscreen transition. Maximization is not the same behavior. |
| OPT-04 | P2 Partial | Simulation distance; entity distance/shadows; AO, biome blending, cloud mode/range, weather radius, leaf cutout, vignette, chunk fade/update priority, gamma/brightness, mipmap/filtering/anisotropy settings | `Options.java`, `VideoSettingsScreen`; `src/ui/video_screen.cpp` primarily exposes custom rendering features. AO/clouds/shadows exist through engine defaults/presets, but the reference's independent controls and semantics are not available. |
| OPT-05 | P1 Missing | Accessibility onboarding, narrator and hotkey, subtitles, high-contrast outline, effect/FOV/darkness/glint/damage-tilt controls | `AccessibilityOptionsScreen`, `AccessibilityOnboardingScreen`; C++ `Settings` lacks these controls. High Contrast resource pack can be selected through Mods & Packs, so the pack itself is already supported. |
| OPT-06 | P2 Partial | Sound options screen, category sliders, output device, directional audio option, music frequency and music toast | `SoundOptionsScreen`, `Options.java`; `src/audio/audio.cpp:117` already exposes and persists category gains in `audio_settings.txt`, but UI never calls `setVolume`. Audio volume is implemented underneath, inaccessible through the game's menus. |
| OPT-07 | P2 Missing | Chat visibility/colors/links/link confirmation/opacity/spacing/background/scale/width/heights/delay/draft persistence and secure-chat filter | `ChatOptionsScreen`, `Options.java`; `src/ui/hud.cpp`, `src/ui/chat_ext.cpp` use fixed behavior. Chat itself exists. |
| OPT-08 | P2 Missing | Language selection, font selection/Unicode/Japanese variants | `LanguageSelectScreen`, `FontOptionsScreen`; `src/ui/resources.cpp:28` loads `en_us`, `src/ui/font.cpp:69` uses fixed font filters. Unicode glyph availability does not equal language/font setting parity. |
| OPT-09 | P2 Missing | Main hand and player skin-part customization; online preferences/privacy settings | `SkinCustomizationScreen`, `OnlineOptionsScreen`; `src/net/client.cpp:233` sends fixed English, distance 12, visible chat, all skin parts, right hand, listing enabled, all particles. Saved options do not update this information. |
| OPT-10 | P2 Missing | Pause on lost focus; inactive FPS limiting; debug GUI scale/reduced-debug preference; advanced tooltips; notification/autosave display controls | `Options.java`; native `Settings` and UI loops omit these controls. Reduced-debug state requested by the server is also discarded. |
| OPT-11 | P3 Partial/different scope | Splash visibility, panorama speed, menu blur, startup/accessibility flags, last-server preference, tutorial progress, telemetry/presence/Realms notification preferences | `Options.java`; native frontend/settings expose no matching controls. Last-server behavior and pack enable/order persist elsewhere; Java transport/graphics-backend/debug/write-sync flags and macOS-specific options require native equivalents or deliberate exclusion. |

### All 105 fixed keys without native option persistence

This list excludes the 8 equivalents above. It is a persistence/control gap list, **not** a claim that every underlying feature is absent.

```text
ao, biomeBlendRadius, chunkSectionFadeInTime, cutoutLeaves, entityDistanceScaling,
entityShadows, forceUnicodeFont, japaneseGlyphVariants, fovEffectScale,
darknessEffectScale, glintSpeed, glintStrength, preferredGraphicsBackend,
graphicsPreset, prioritizeChunkUpdates, fullscreen, exclusiveFullscreen,
macFullscreenMenuVisibility, gamma, debugGuiScale, maxAnisotropyBit,
textureFiltering, improvedTransparency, inactivityFpsLimit, mipmapLevels,
narrator, particles, reducedDebugInfo, renderClouds, cloudRange,
simulationDistance, screenEffectScale, soundDevice, vignette, weatherRadius,
autoJump, rotateWithMinecart, operatorItemsTab, autoSuggestions, chatColors,
chatLinks, chatLinksPrompt, discrete_mouse_scroll, invertXMouse, invertYMouse,
realmsNotifications, showSubtitles, directionalAudio, toggleCrouch, toggleSprint,
toggleAttack, toggleUse, sprintWindow, darkMojangStudiosBackground,
hideLightningFlashes, hideSplashTexts, damageTiltStrength, highContrast,
highContrastBlockOutline, narratorHotkey, resourcePacks, incompatibleResourcePacks,
lastServer, lang, chatVisibility, chatOpacity, chatLineSpacing,
textBackgroundOpacity, backgroundForChatOnly, hideServerAddress,
advancedItemTooltips, pauseOnLostFocus, overrideWidth, overrideHeight,
chatHeightFocused, chatDelay, chatHeightUnfocused, chatScale, chatWidth,
notificationDisplayTime, useNativeTransport, mainHand, tutorialStep,
mouseWheelSensitivity, allowCursorChanges, quitShortcuts,
ctrlClickEmulatesRightClick, glDebugVerbosity, skipMultiplayerWarning,
hideMatchedNames, joinedFirstServer, syncChunkWrites, showAutosaveIndicator,
allowServerListing, inGameNotification, sharePresence, onlyShowSecureChat,
saveChatDrafts, panoramaScrollSpeed, telemetryOptInExtra, onboardAccessibility,
menuBackgroundBlurriness, startedCleanly, musicToast, musicFrequency
```

Dynamic families additionally absent from native `options.txt`: `key_*` and `modelPart_*`. `soundCategory_*` has the separate native category-gain system noted above. Resource/data pack enablement and ordering already persist through the mod manager; do not implement a second competing pack manager merely to copy Java option keys.

## Networking, multiplayer, and server-driven state

| ID | Priority/status | Gap | Evidence and consequence |
|---|---|---|---|
| NET-01 | P1 Missing | Authenticated online-mode login and encrypted connection | `ServerLoginPacketListenerImpl`, `ClientHandshakePacketListenerImpl`; `src/net/client.cpp:211` immediately rejects `hello`. Microsoft/Xbox/Minecraft session auth, RSA challenge response and AES/CFB8 are absent. Public online-mode servers cannot be joined. Reading installed ownership/profile information does not implement login. |
| NET-02 | P1 Missing | Server resource-pack push/pop lifecycle, required pack behavior | `ClientCommonPacketListenerImpl.java:192`; `src/net/client.cpp:266` always declines pushes; pop has no handler. Pack-required servers and server asset presentation cannot work correctly. Local resource-pack support does exist. |
| NET-03 | P1 Partial | Dynamic registry and tag consumption | `ClientConfigurationPacketListenerImpl`, `ClientCommonPacketListenerImpl`; `src/net/client.cpp:295` retains only enchantments, paintings, instruments, banners, plus special dimension/biome handling. `update_tags` has no handler. Server chat types, variants and other custom registries cannot reliably drive the native models/rules/UI. |
| NET-04 | P1 Missing | World border initialization/updates, collision/targeting, warning overlay | `ClientPacketListener.java:1885`; all six border packets lack handlers. Native player physics and renderer have no border model. Server correction/damage does not replace client collision and visualization. |
| NET-05 | P1 Missing | Item cooldown groups/timers and hotbar/model cooldown display | `ClientboundCooldownPacket`, `ItemCooldowns`; `cooldown` lacks a handler; no cooldown state in UI/item prediction. Repeated item use and visuals cannot match reference cooldown behavior. |
| NET-06 | P2 Missing | Server tick-rate, freeze, and step controls | `ClientPacketListener.java:696`; `ticking_state`/`ticking_step` absent. `src/game/game.cpp:195`, entity/audio/sky loops advance independently at 20 Hz. `/tick` behaviors are not synchronized. |
| NET-07 | P2 Missing | Waypoints and locator HUD | `ClientPacketListener.java:2509`, `ClientboundTrackedWaypointPacket`; `waypoint` absent. No native locator-bar implementation. |
| NET-08 | P2 Missing | Server dialogs and server links, response actions/clear lifecycle | `ClientCommonPacketListenerImpl.java:258`; `show_dialog`, `clear_dialog`, `server_links` absent; corresponding dialog screens absent. Server-provided interaction flows are unusable. |
| NET-09 | P2 Missing | Transfer packets and persistent server cookies | `ClientCommonPacketListenerImpl.java:228` and `:315`; `store_cookie`/`transfer` absent; cookie requests always return absent. Server transfer networks lose their expected lifecycle/state. |
| NET-10 | P1 Partial | Configuration acceptance/reset lifecycle | `ClientConfigurationPacketListenerImpl`, `CodeOfConductScreen`; `src/net/client.cpp:323` logs and automatically accepts code of conduct, without a screen. `update_enabled_features` and `reset_chat` absent. Registry/configuration vectors need reset/reconfiguration fixtures; current dimension definitions append. |
| NET-11 | P2 Missing | Chunk biome changes and cache/simulation state | `ClientPacketListener.java:864`; `chunks_biomes`, `set_chunk_cache_center`, `set_simulation_distance` absent; cache radius only logs. `/fillbiome` and other server biome changes do not update native tint/environment state. |
| NET-12 | P2 Missing | Server camera, look-at, and rotation changes | `ClientPacketListener.java:1876`, `ClientboundPlayerLookAtPacket`, `ClientboundPlayerRotationPacket`; `set_camera`, `player_look_at`, `player_rotation` absent. Spectating entities and forced view changes do not work as in Java. |
| NET-13 | P2 Missing | Server game rules/difficulty UI and client flags | `ClientPacketListener.java:2109`, `InWorldGameRulesScreen`; `game_rule_values`, `change_difficulty` absent; login discards reduced-debug/death-screen/limited-crafting flags. Survival UI/respawn and command-related state can diverge from server policy. |
| NET-14 | P2 Partial | Signed chat/session state, acknowledgments, deletion/filtering, chat types and custom completions | `ClientPacketListener.java:1079`, `ChatListener`; `src/ui/ui.cpp:242` sends unsigned chat with a dummy acknowledgment/checksum, `:728` rejects inline chat type; `delete_chat` and `custom_chat_completions` absent. Basic chat/commands/suggestions are present, but secure chat and server custom chat cannot match. |
| NET-15 | P2 Missing | Minecart track interpolation, leash/entity links, projectile acceleration, transient blocks | `ClientboundMoveMinecartPacket`, `ClientboundSetEntityLinkPacket`, `ClientboundProjectilePowerPacket`, `ClientPacketListener.java:1177`; named handlers absent. Generic movement exists but does not implement these specialized states. |
| NET-16 | P3 Missing | Debug subscriptions/samples/value packets, tag-query responses, test-block status and GameTest highlight, low-disk warning, custom crash details | Reference packet types and `ClientPacketListener`; no native handlers. Debug/game-test editing and server diagnostics are incomplete. |
| NET-17 | P2 Partial | Unknown packet/reader failure visibility | `src/net/client.cpp:343` dispatches registered callbacks, and the final fallback returns true without diagnosing ignored packets. Some component/metadata readers return early. Current packet “support” lists therefore conceal partial parsing and silently omitted behavior. Add coverage/remaining-bytes diagnostics in fixtures before declaring parity. |

`packets.csv` lists all protocol declarations and source handler/send evidence. Important exclusions: `bundle` is synthetic, `bundle_delimiter` need not produce gameplay state, `chunk_batch_start` is a boundary marker, combat enter/end packets can be inert without separate UI, and alternative player movement packet encodings need not all be emitted. Handshake/status/ping are encoded directly in `client.cpp`/`status.cpp`; missing *named* send evidence for those is not missing networking. A registered empty callback is still partial/no-op, not complete support.

## Items, inventories, recipes, and menus

| ID | Priority/status | Gap | Evidence and consequence |
|---|---|---|---|
| ITEM-01 | P1 Partial | Complete current item-component wire decoding | `DataComponents`, `src/ui/items.cpp:322` and `:414`. The 32 types listed below fall through to false. Patches are not length-delimited, so an unknown component prevents decoding subsequent stack/packet fields; inventory/equipment/container decoding can be truncated. Default item-report components do not fix server-sent patches. |
| ITEM-02 | P1 Partial | Inline registry holders and component predicates | `src/ui/items.cpp:333` rejects holder value 0; `:401` requires both component matcher counts to be 0. Inline instruments/trims/effects and component-based adventure predicates do not parse fully. Generic slash-name varint handling must be checked against each codec, not assumed valid. |
| ITEM-03 | P1 Partial | Component-aware predicted use/equipment rules | `ItemStack`, `Consumable`, `Equippable`; `src/game/gameplay.cpp:484` uses `rules_.use(itemName)` rather than the actual patched stack; equipment slot restrictions in `src/ui/screens.cpp:27` use suffix/name heuristics. Modified/custom consumables, weapons, equipment and interaction animations cannot reproduce Java stack semantics. |
| ITEM-04 | P2 Missing | Recipe book unlock/add/remove/settings, search/filter/categories, ghost recipes, placement | `RecipeBookComponent`, `ClientPacketListener.java:1753`; no handlers for recipe-book/ghost packets or sends for recipe placement/book settings. Crafting and furnace inventories work through the server, but the recipe-book interface is absent. |
| ITEM-05 | P2 Partial | Recipe-property sets and tagged stonecutter ingredients | `ClientboundUpdateRecipesPacket`, `SlotDisplay`; `src/ui/stations_a2.cpp:96` discards property sets and `:42` marks tag ingredients unsupported. Station input predicates remain hardcoded/approximate rather than using server recipe sets. |
| ITEM-06 | P2 Partial | Map decorations and in-world map presentation | `MapRenderer`, `MapDecoration`; `src/ui/stations_a2.cpp:117` consumes and discards all decoration entries. Cartography pixel preview exists; in-hand/item-frame map content, decoration icons/names and related presentation are not complete. |
| ITEM-07 | P2 Missing | Bundle selected-slot/scroll interaction and contents tooltip/model | `BundleTooltip`, `BundleSelectedItemSpecialRenderer`, `ServerboundSelectBundleItemPacket`; no `bundle_item_selected` send. `bundle_contents` can be skipped and the item exists, but selected-content interaction is absent. |
| ITEM-08 | P2 Partial | Complete tooltip composition and tooltip-hide semantics | `ItemStack` tooltip builders; `src/ui/items.cpp:247` displays name, limited effects/enchants/lore/durability. Attribute lines, broader component descriptions, bundle/container previews and advanced debug tooltip detail are incomplete. `:457` discards the tooltip-display hide-all boolean. |
| ITEM-09 | P2 Missing | Command block, command minecart, jigsaw, structure block, test block and test-instance editors | Corresponding Java screens and `set_*`/`jigsaw_generate` serverbound packets; absent native screen definitions and sends. These items/blocks appearing in creative does not make their editors functional. |
| ITEM-10 | P2 Partial | Creative inventory dynamic tab content and feature gating | `CreativeModeTabs`, `CreativeModeInventoryScreen`; `src/ui/creative.cpp:88` builds from `creative_tabs.inc` with selected expansions. New/modded enabled-feature items, registry-driven additions, permissions and operator-tab preference need parity; settings/feature-enable packets are missing. Saved hotbars and search do already exist. |
| ITEM-11 | P2 Verify | All station click/input/output predictions and mount menus | All 25 `MenuType` entries map to generic grids or station modules (generic_9xN explains absent literal-name matches); horse and nautilus inventories are present in `stations_b.cpp`. Compare slot acceptance, quick-move, shift-double-click, carried items, crafting remainders, anvil/enchant/smithing/trading and server resync. Do not label whole station families missing. |

### 32 component types rejected by the current decoder

```text
damage_type, attribute_modifiers, food, consumable, use_remainder, use_cooldown,
damage_resistant, weapon, attack_range, equippable, repairable, death_protection,
blocks_attacks, piercing_weapon, kinetic_weapon, attack_animation,
interact_animation, block_transformer, villager_food, provides_trim_material,
jukebox_playable, profile, bees, sulfur_cube_content, break_sound, compostable,
cooking_fuel, brewing_fuel, mob_visibility, provides_pottery_pattern,
sign_text_front, sign_text_back
```

`components.csv` distinguishes explicit decoding/skipping, generic slash-name handling, and rejection for every one of the 122 reference types. Supporting skip/decode is only the first step; prediction, rendering, tooltips and persistence must consume the actual components too.

## Entity and block-entity visuals

| ID | Priority/status | Gap | Evidence and consequence |
|---|---|---|---|
| VIS-01 | P1 Partial | Default rendering for 13 entity types; three wrong texture paths | `EntityRenderers`; `src/entity/entity_render.cpp:163` uses a generic name-based model/texture and Head animation for **allay, bat, blaze, breeze, creaking, cushion, endermite, nautilus, shulker, silverfish, sulfur_cube, tadpole, zombie_nautilus**. The installed 26.3 JAR and extracted model cache confirm ten have basic model/texture support. **Cushion, sulfur cube and zombie nautilus** lack the default texture path, causing Box fallback at `:322`; their textures require color/outer/variant selection. The ten supported base models still lack species-specific animation/layers/states, such as shulker color/opening, breeze effects, creaking eyes and nautilus equipment/baby selection. |
| VIS-02 | P1 Missing | Item frames, glow item frames and paintings | Java `ItemFrameRenderer`, `PaintingRenderer`; `src/entity/entity_render.cpp:310` explicitly sets their kind to None. Painting variants and frame metadata are already decoded, so missing rendering is an integration gap. |
| VIS-03 | P1 Missing | Block/item/text display entities | Java `DisplayRenderer`; `src/entity/entity_render.cpp:307` sets them to None. Displays are often central to server maps and datapack experiences; transforms, interpolation, text/billboards and content are absent. |
| VIS-04 | P2 Partial | Lightning bolt geometry and other effect-only entities | `LightningBoltRenderer`, `OminousItemSpawnerRenderer`; `src/entity/entity_render.cpp:307` hides them. `src/sky/sky.cpp:335` tracks lightning and flashes the sky, so the flash is present; the actual bolt is not drawn. Area-effect clouds need their own client particle emission; marker/interaction invisibility is correct. |
| VIS-05 | P2 Partial | Species-specific animation/render state | `src/entity/entity_anim.h:2` explicitly states other models only turn their head; enum implements twelve shared animation families. Many species use Head/None/generic quadruped animation: dragon/wither, aquatic mobs, flight, sitting/rolling/sniffing, camel transitions, wardens, attack animations and keyframe states are not fully ported. Compare per-species model `setupAnim`, not just extracted geometry. |
| VIS-06 | P2 Missing | Leash ropes, glowing outlines, cape/custom skin presentation, specialized combat/render layers | Java `EntityRenderer`, `CapeLayer`, `SkinManager`, outline rendering; native entity renderer lacks these paths. `set_entity_link` is also absent. Armor/elytra/glint/shadows/name tags and many variant textures already exist; these are not blanket missing systems. |
| VIS-07 | P2 Partial | Player skin resolution and metadata serializers | `SkinManager`, `EntityDataSerializers`; `src/entity/entities.cpp:494` skips player texture properties; `:690` selects default skins by UUID. `:345` stops on unknown/profile serializers; particle metadata only handles entity-effect color safely. Custom profiles/skins and later metadata in the same update can be lost. |
| VIS-08 | P2 Partial | Full item stacks preserved for entity equipment/held/dropped rendering | `LivingEntityRenderer`, `ItemEntityRenderer`; `src/entity/entities.cpp:81` reduces stacks to item id/count/dye/foil and `:802` stores those summaries. Per-stack item models, trims, profiles, custom model data and richer component-driven visuals cannot be reproduced from that summary. |
| VIS-09 | P2 Partial | Entity animation events and pickup animation | `ClientPacketListener`/entity event handlers; `src/entity/entities.cpp:545` animate callback is empty; `:823` only applies hurt/death entity events. Pickup removes/updates an item without the full reference pickup travel animation. Critical/enchantment/totem and other client effects require the omitted events. |
| VIS-10 | P1 Missing | Moving piston block-entity rendering | `PistonHeadRenderer`; no native piston Kind in `src/blockentity/block_entities.h:54`. Server redstone/piston logic exists, but client animation, carried block shape and moving collision/push behavior are not supplied by ordinary chunk meshes. |
| VIS-11 | P2 Missing | Spawner/trial spawner entity contents and special vault rendering | `SpawnerRenderer`, `TrialSpawnerRenderer`, `VaultRenderer`; native Kind/dispatch omit them. Their ordinary block textures can render, while rotating contained mobs/items and state-specific presentation are absent. |
| VIS-12 | P2 Missing | Beacon beams, End portal effect and End gateway beam/effect | `BeaconRenderer`, `TheEndPortalRenderer`, `TheEndGatewayRenderer`; native block-entity renderer has no matching Kind. Shader-pack role enums/material emissive tagging do not emit these geometries. Beacon menu and base block rendering exist. |
| VIS-13 | P3 Missing | Brushable-block item reveal, structure/test-instance bounding boxes and debug presentation | `BrushableBlockRenderer`, `BlockEntityWithBoundingBoxRenderer`, `TestInstanceRenderer`; no native Kind. Archaeology authority remains on the server, but item reveal/render and editor aids are absent. |
| VIS-14 | P2 Partial | Block-entity rendered item/profile fidelity | `CampfireRenderer`, `ShelfRenderer`, `SkullBlockRenderer`; `src/blockentity/block_entities.h:92` stores mostly names/profile summaries; item rendering uses simplified blocks/sprites. Compare components, special item models, transformations, custom head skins and dynamic animations. Chest/sign/banner/pot/bell/conduit/book/statue rendering is already substantial. |

The ten omitted special block-entity families are: mob spawner, piston, End portal, End gateway, beacon, structure block, test-instance block, brushable block, trial spawner, vault. No special renderer is required for every block entity (e.g. furnace authority is not a missing furnace renderer).

The VIS-01 asset check used `C:/Users/Admin/AppData/Roaming/.minecraft/versions/26.3/26.3.jar` and `C:/Users/Admin/AppData/Local/minecraftCplusplus/models/26.3/entities_v2.json`. All thirteen model layers are present in that cache. Only cushion, sulfur cube and zombie nautilus lack `textures/entity/<name>/<name>.png`. This confirms why a missing literal-name branch is not by itself evidence of a missing visible model.

## Item models, textures, particles, atmosphere, and audio

| ID | Priority/status | Gap | Evidence and consequence |
|---|---|---|---|
| FX-01 | P1 Partial | Item-model predicates/selects/ranges in GUI and world/hand paths | Java property registries indexed in `item-conditions.csv`, `item-ranges.csv`, `item-selects.csv`; `src/ui/icons.cpp:255` handles few properties, select picks fallback/first case; `src/entity/item_models.cpp:528` only handles using-item conditions and a few select/range properties. Compass/clock, fishing cast, custom-model data, component checks, bundle fullness/selection and other dynamic variants are incomplete. |
| FX-02 | P2 Partial | Item tint/special-renderer fidelity | `ItemTintSources`, `SpecialModelRenderers`; `icons.cpp` and `item_models.cpp` support different subsets. Firework/map/custom model tint sources, patterned shield/banner detail, player-head and context-dependent special models need individual parity checks. `bundle/selected_item` special model is absent; ordinary model geometry is already implemented. |
| FX-03 | P2 Partial | Animated item/entity/UI textures and terrain texture interpolation | `SpriteContents`; `src/ui/icons.cpp:111`, `src/entity/item_models.cpp:213`, `src/entity/entity_assets.cpp:329`, `src/ui/sprites.cpp:25` keep first frames. Terrain textures do have frame remapping via `src/assets/assets.cpp:566`, but no `.mcmeta` interpolation blending path. Do not report water/lava/fire terrain animations as wholly missing. |
| FX-04 | P2 Missing | Network item/vibration particle decoding | `ItemParticleOption`, `VibrationParticleOption`; `src/fx/particles.cpp:57` marks both unsupported and `:162` rejects packets. Local eating item shards exist (`itemShard`), but server item particles and vibration paths do not. |
| FX-05 | P2 Partial | 73 particle types use generic behavior | `ParticleEngine` provider registrations; `src/fx/particles.cpp:115` defaults to Generic. See `particles.csv`: sculk/sonic/gust/totem/firework/drips/sulfur/gas/firefly etc. have specialized lifecycles, geometry, emitters or movement in Java. Trail/float/extra parameters are often read and discarded; dust transition discards its second color. |
| FX-06 | P2 Partial | Client block animateTick/clientTick particles and level event coverage | `ClientLevel`, `LevelEventHandler`; native particle/audio level-event handlers only implement selected events, and native ambient particle sampling supports a subset. Port per-block effects and entity-emitted effects rather than expecting server particles to supply all client visuals. |
| FX-07 | P1 Missing | Reference effect-driven view/fog/overlays | `FogRenderer`, `GameRenderer`, `ScreenEffectRenderer`, `Hud`; native HUD supports spyglass/frost, camera bob/hurt and normal atmosphere, but no complete blindness/darkness/night-vision, nausea/portal warp, pumpkin, water/fire/in-block overlay and spectator post-effect paths. Server `post_effects` also has no handler. Effect icons are not effect rendering. |
| FX-08 | P2 Partial | Environment/dimension state and fog controls | `EnvironmentAttributeProbe`, dimension/biome registries; native sky/environment has broad timeline/weather support, but atmospheric values largely come from local asset data, not complete server registry attributes; fog uses fixed vanilla render-distance input (`src/sky/sky.cpp:25`), horizon assumes nonflat (`:620`). Compare custom dimensions/biomes, flat worlds, underground and fluid fog behavior. |
| FX-09 | P2 Partial | Subtitles, output device/directional option, music UI/settings | `SubtitleOverlay`, `SoundOptionsScreen`, `MusicManager`; no subtitle event/UI implementation, audio engine uses default output device and fixed spatial behavior. Category gain, seeded sound choice, streamed sounds, music/biome ambience and positional/entity-bound sound already exist. |
| FX-10 | P2 Partial | Client-only looping/event sounds | `MinecartSoundInstance`, `ElytraOnPlayerSoundInstance`, `UnderwaterAmbientSoundInstances`, entity/block client ticks; native audio handles player steps/falls/swim, biome music/ambience and server sound/stop packets, but full entity/client sound lifecycle and level-event sound coverage are not ported. |
| FX-11 | P2 Missing | Text/font descriptions, TTF provider, bidirectional shaping and rich object contents | Java `Font.java:47`, font providers, component/font descriptors; native Style has no font descriptor and provider loader handles bitmap/space/reference/unihex only (`src/ui/font.cpp:77`), no TTF or shaping. Arabic/RTL text, pack custom fonts and newer object components are incomplete. UTF-8/unihex and styled text are already present. |

The D3D12 pipeline, terrain LOD, shader packs, TAA/DLSS/FSR/XeSS and frame generation are native extensions. Java's OpenGL/backend implementation is not something to port verbatim. Audit their output against the vanilla effects above. Shader-pack geometry shaders, some resource/uniform forms and required Iris features have explicitly limited support; that is pack compatibility scope, not vanilla 26.3 feature completeness.

## Player movement, interactions, and mounts

| ID | Priority/status | Gap | Evidence and consequence |
|---|---|---|---|
| PLAY-01 | P1 Missing | Singleplayer pause/focus integration | `IntegratedServer.java:130`, `Minecraft.java:1361`; native pause screen only suspends input (`src/ui/ui.cpp:1062`), and `Game::frame` still ticks. No request suspends local-server simulation. Mobs/world time and player gravity continue while the menu is open. LAN worlds should retain their separate unpaused behavior. |
| PLAY-02 | P1 Partial | Server/attribute/item-driven interaction reach | Java player interaction range and `AttackRange`; `src/game/game.cpp:15` fixes block reach at 5; gameplay crosshair living check caps at 3 (`src/game/gameplay.cpp:417`), while actual entity clicks use fixed reach 5 (`src/main.cpp:707`, `src/entity/entities.cpp:1140`). Visual targeting and actual clicks use inconsistent limits; game-mode/attribute/weapon range semantics are not consumed. |
| PLAY-03 | P1 Partial | Server ability movement speeds | `ClientboundPlayerAbilitiesPacket`; `src/net/client.cpp:454` only keeps three flags and drops speed floats; `src/game/player.cpp:473` hardcodes flight speeds. Server-modified fly/walk ability speeds cannot be reproduced. |
| PLAY-04 | P1 Partial | Moving/block/entity collision responses | `Entity.move`, `SlimeBlock`, `BedBlock`, `HoneyBlock`, piston collision; `src/game/player.cpp:314` zeros collision velocity. No complete slime/bed bounce, honey wall slide, moving-piston displacement or entity collision/push implementation. Basic friction/soul-sand/honey speed factors, cobweb/bush slowdowns, ladders, powder snow/scaffolding context and step-up are present. |
| PLAY-05 | P1 Partial | Fluid-flow pushing and exact fluid body intersection | `Entity.updateFluidHeightAndDoFluidPushing`, `FluidState`; native `Physics::inFluid` uses cell occupancy and swimming/liquid code lacks full flow-vector/current sampling. Bubble column response exists; source/waterlogged/flow height and current boundary fixtures are needed. |
| PLAY-06 | P2 Partial | Riptide/spin attack and new weapon interaction/kinetic/piercing/animation semantics | Java `LivingEntity`, item component classes; native player pose set only has Standing/Crouching/Swimming/FallFlying, patched weapon/attack-range/kinetic/piercing codecs are rejected. Basic swing/bow/crossbow/trident-use animations do not implement the complete client state machine. Server damage/projectiles are still delegated. |
| PLAY-07 | P1 Partial | Nautilus/zombie nautilus and happy-ghast local control; camel dash/jump cooldown | `Nautilus`, `HappyGhast`, `Camel`; `src/game/riding.cpp:32`/`:94` only grants local control to boats, horse/camel types, steered pig/strider; no nautilus/happy-ghast controls. `:171` always returns jump cooldown 0. Mount menus exist, so fix riding independently from UI. |
| PLAY-08 | P2 Missing | Spectator HUD/actions, teleport menu, entity spectating and spectator outlines/shaders | `SpectatorGui`, `SpectatorMenu`, spectator screens/actions; native noclip and gamemode switching exist, but serverbound spectator/teleport actions and `set_camera` handling are absent. First-person body-head rendering and view-specific post effects are incomplete. |
| PLAY-09 | P2 Partial | Sprint double-tap/settings behavior, autojump, exact local input/use outcomes | `LocalPlayer`; native sprint is key-driven and settings are missing. `Game::startUseItem`/block placement uses limited name/orientation prediction and hardcoded inside/border hit flags (`src/net/client.cpp:532`). Compare hand fallback, failed/consumed outcomes, special placement, cooldown, and component-patched tools. |
| PLAY-10 | P2 Verify | Swimming/crawling/elytra, mining, use animation, boat/horse physics and pose/camera transitions | Implemented in `player.cpp`, `gameplay.cpp`, `riding.cpp`, `game.cpp`, `visuals.cpp`. Differential movement traces are needed for velocities, transitions, collision, jump timing, glider rocket boost, dismount/seat positioning and FOV. These features are not entirely unported. |

## World generation, saves, resource packs, and frontend

| ID | Priority/status | Gap | Evidence and consequence |
|---|---|---|---|
| WORLD-01 | P1 Missing in native path | Native Nether/End/custom/flat/preset generator selection and three structures | `NetherFortressStructure`, `EndCityStructure`, `NetherFossilStructure`, biome/chunk generator types; native `ChunkGenerator` uses `OverworldBiomeSource` (`src/worldgen/generator.cpp:35`), startup constructs default overworld settings (`src/main.cpp:521`), no selection from saved WorldGenSettings. Native structure factory lacks three names; server generates these correctly in play mode. |
| WORLD-02 | P1 Partial | Generation side effects: entities, block-entity NBT/loot/spawners, scheduled ticks, structure starts/references | `StructureTemplate`, `ChunkGenerator`, chunk serialization; `GenChunk` holds blocks/biomes/heightmaps only, `src/worldgen/structure_template.cpp:690` consumes loot seed RNG without persisting container data; new saved chunks have empty structure/block-entity/tick metadata (`src/world/save.cpp:345`) and no generated entity persistence path. Native block parity does not imply a semantically valid generated world. |
| WORLD-03 | P2 Partial | Template feature processor subset versus structure template processors | Java `TemplateFeature`, all 11 processor types; `src/worldgen/features_misc.cpp:2326` only handles block_rot/protected/rule/block_ignore/nop, treats nontrivial rule position predicates as true and ignores other processors. The complete structure-template path must not mask this second implementation's gaps. |
| WORLD-04 | P1 Partial | Old/new chunk blending and generation status transitions | Java `Blender` and chunk status pipeline; `src/worldgen/density.cpp:1971` substitutes blend alpha/offset context fallbacks; existing chunks become read-only neighbors. Native generated chunks lack the Java blending/upgrading/postprocess lifecycle, so mixed-version boundaries need differential validation. Java server conversion/backups are delegated. |
| WORLD-05 | P1 Partial | Region compression/external chunk support and dimension save loading | `RegionFile`, `RegionFileVersion`; `src/world/world.cpp:771` skips LZ4/external files; save loading chooses overworld region paths. Native viewer cannot read every valid reference save/dimension even though server play can load it. Native save session locks/atomic writes and many existing NBT fields are already preserved. |
| WORLD-06 | P1 Verify | Exact noise, surface, aquifer, carver, feature, structure RNG parity | All 58 features/13 structures/11 processors/trunk/foliage families have implementation evidence. Existing `gentest.cpp`/`gentest_structures.cpp` provide differential tools, but an index/register count is not an exact-output result. Run multi-seed, dimension/preset/datapack, old-boundary and side-effect fixtures before calling native worldgen complete. |
| WORLD-07 | P2 Missing | World creation presets, flat customization, single-biome customization, experiments, bonus chest, game-rule editor and creation pack flow | Corresponding `worldselection`/flat/buffet Java screens; native `src/ui/worlds.cpp:415` has name/seed/mode/difficulty/cheats/structures only. Official server creation works, but these configuration flows cannot be selected through the UI. |
| WORLD-08 | P2 Missing | World edit recreation/backup/export/optimization/recovery workflows | `EditWorldScreen`, `OptimizeWorldScreen`, `RecoverWorldDataScreen`; `src/ui/worlds.cpp:494` exposes rename/open-folder. Automatic upgrade backup exists in `LocalServer`; user-invoked world maintenance and recreation are separate missing flows. |
| WORLD-09 | P2 Partial | Resource-pack metadata filters/overlays and complete pack-provided asset semantics | `PackResources`, `PackMetadataSection`, `ResourceFilterSection`; `src/assets/asset_source.cpp` performs topmost-file/merged stack reads, without a complete pack overlay/filter implementation. Resource-pack order/enabling, data packs, namespace merging, high contrast/programmer art, and Fabric assets/data already exist. Texture/model/font limitations above further restrict pack compatibility. |
| WORLD-10 | P2 Missing/partial | Friends/social/reporting, credits/End poem, tutorial, toasts and server management/safety screens | Java screen inventory; native enum/modules have no corresponding flows. `src/ui/advancements.cpp:439` discards show-toasts flag. Advancements/statistics screens exist; unlocking/toast/tutorial/music/save/disk notifications and account-service screens do not. Realms/friends/reporting may be deliberately excluded, but should be explicit product scope. |
| WORLD-11 | P2 Partial/different scope | Fabric client logic and mod protocol expectations | Fabric server helper and registry/assets/data sync are implemented (`fabric_server.cpp`, `fabric_sync.cpp`), plus a native DLL API. Java Fabric client mixins/renderers/screens/custom channels are not executed natively, so “Fabric support” must not imply arbitrary client mod compatibility. Server gameplay can work while native modded blocks/items/entities render incorrectly or custom handshakes fail. |

## Subsystem coverage and existing work to preserve

| Area | Already present | Principal outstanding work |
|---|---|---|
| Assets/reports/shapes | Installed-JAR assets, registry reports, extracted collision/outline shapes, resource stacks | Complete context-sensitive shapes, server data, pack metadata and dynamic assets |
| Audio | XAudio2, seeded variant choice, categories, spatial/entity sounds, music, biome ambience, capture tool | UI/settings, subtitles, specialized client sound instances |
| Block entities | Chests/signs/hanging signs/banners/skulls/shulkers/bells/pots/conduits/books/campfires/shelves/statues | Ten special renderers and full contained item/profile data |
| Core/platform | Jobs, window/input, launcher/handoff, logs, crash dumps, ownership check | Remapping, fullscreen/focus pause, richer input/text workflows |
| Entities | Registry/model extraction, network transforms/data/equipment, shared animations, shadows/armor/elytra/glint | Missing species, species animations, displays/decorations, stack fidelity, profiles/layers |
| Game/player | Fixed-tick movement, shape collision, mining/prediction, effects/attributes, item use, riding | Listed movement/interaction omissions and authoritative option/state integration |
| Mesh/world/terrain | Chunk/region storage, greedy/detail meshes, light, biome colors, far LOD, edits and native saves | Complete save variants, special collision, dimensional generators, semantic generation metadata |
| Mods/packs | Resource/data packs, Fabric server support, native mods, restart-based changes | Pack filters/overlays, client mod logic scope, server pack lifecycle |
| Networking | Offline login/config/play, protocol/report lookup, chunk/light stream, local server, LAN discovery/publish | Authentication and omitted protocol/state/UI families |
| Rendering/upscalers/packs | D3D12, culling, shader pipeline, shadows/post, shader packs, TAA/vendor upscalers/FG | Vanilla effect/special geometry gaps; separate shader-pack compatibility verification |
| Sky/environment | Time/timelines, biomes, sun/moon/stars, clouds/weather, flashes and boss atmosphere | Server environment data, option coupling, special effect fog/view modes |
| UI/frontend | Title/world/server/create/edit/LAN/loading, HUD/chat, inventories/stations, creative, books/signs, stats/advancements | Settings submenus, recipe book, special editors, server dialogs, spectator/locator/toasts/services |
| Worldgen | Extensive native overworld noise/material/carver/features/structures with differential tools | Generator selection, three structures, side effects, blending and proved fixture parity |

## Implementation order

1. **Repair the client data boundary.** Decode every component codec and preserve full stacks; consume server tags/registries/flags; add protocol fixtures for parser completion. This underpins modded/custom items, models, recipes, variants and settings synchronization.
2. **Complete ordinary play compatibility.** World border, item cooldowns, local pause, reach/ability speed, moving collisions, missing entity/decorative/display renderers and block-entity specials. Add authenticated login/server resource packs for online-server scope.
3. **Finish settings and input.** Introduce a single option/binding model with persistent values, screens, runtime consumers and client-information updates; expose the audio gains that already exist. Start with controls, language, fullscreen, accessibility, sound and effect controls.
4. **Complete interaction/UI systems.** Recipe book/ghost placement, bundle interaction, game rules/difficulty, spectator, waypoints/dialogs/links, creation presets and maintenance flows.
5. **Finish client visual/state machines.** Per-species models/animations, all particle providers/events, item property/tint/special renderers, text/fonts, view effects and client sound loops.
6. **Prove native worldgen/save parity.** Add generator selection/side effects/blending/save variants; validate exact output and semantic metadata. Do not sacrifice delegated gameplay coverage merely to duplicate Java authority unless Java-free execution is the intended goal.

## Verification work still required

No worlds were modified and no authenticated/service actions were attempted. No fresh gameplay run or differential worldgen run was performed during this source audit; existing tooling is identified, not represented as newly passed tests. The inventory generator was run and its outputs checked against source declarations/branches. A fresh native build would not prove behavior parity and is unnecessary for documentation-only changes.

For completion criteria, require:

- **Settings:** each reference player-facing setting has a screen, persisted value, runtime consumer, and correct server/client-information effect where relevant; keys use a binding table. Test restart and world/title handoff.
- **Packets/components:** fixtures from protocol 777 exhaust each codec, including inline holders/nested patches and multiple stacks. Assert state change and complete parsing, not merely handler registration. Cover reconfiguration, transfer, resource-pack lifecycle and tags/custom registries.
- **Movement:** record Java/native per-tick position/velocity/pose for block/fluids/piston/entity contact, every mount, gliding/swimming/riptide, item use/cooldown and range/ability modifiers.
- **Visuals:** one scene per renderer/layer/animation/property/particle/block effect and potion/view mode, with matched snapshots plus state checks. Include modded/custom components and packs.
- **Worldgen:** compare biome/heightmap/block and structure starts/references, NBT/loot/entities/ticks, neighboring/chunk-boundary output across representative seeds/presets/dimensions/old chunks/data packs.
- **Saves:** read/write fixtures for each region compression/external-chunk format and dimension; round-trip unrecognized NBT safely; keep viewer/edit and server-play ownership distinct.

## Files and reproduction

Run `python tools/audit_26_3.py` from the repository. Pass `--reference <directory>` if the decompilation moves. It regenerates static inventories and hashes, not this manually reviewed narrative. Screen mappings and codec/particle classifications are documented source assessments; remaining literal-name evidence is explicitly not a pass/fail verdict.

- `inventory.json`: reference version, snapshot/base/working-tree context, counts and caveats.
- `backlog.csv`: every reviewed finding, priority, status, evidence and consequence from this report; suitable for sorting/planning.
- `settings.csv`, `keybindings.csv`: complete option/binding baselines.
- `packets.csv`: all protocol directions/phases, named handlers/sends and reference locations.
- `components.csv`, `particles.csv`: every registered type, source classification and evidence.
- `screens.csv`, `realms-screens.csv`: every reference screen and native/scope assessment.
- `entity-renderers.csv`, `block-entity-renderers.csv`, `item-special-renderers.csv`: registration baselines.
- `item-conditions.csv`, `item-ranges.csv`, `item-selects.csv`, `item-tints.csv`: model property/tint baselines.
- `features.csv`, `structures.csv`, `processors.csv`, `trunk-placers.csv`, `foliage-placers.csv`: generation baselines.
- `menus.csv`: reference menu registrations (generic_9xN support is via prefix).
- `source-files.csv`, `shader-files.csv`: audited native file/line/hash inventory.
