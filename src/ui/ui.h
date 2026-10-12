#pragma once
// The game's user interface: HUD (hotbar, hearts, hunger, armor, air, experience, titles, action bar), F3 debug
// screen, chat, inventories and containers (server driven), creative inventory, pause and options menus.
//
// Per frame: beginFrame() before Game::frame (routes input: while a screen is open the game gets none),
// endFrame() after it (builds the canvas the UI render pass draws over the frame).
#include "core/common.h"
#include "render/upscalers/video_settings.h"

#include <array>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string_view>

class Window;
class Renderer;
class Game;
class World;
class BlockRegistry;
class Terrain;
class GameReports;
namespace net {
class Client;
class Session;
}  // namespace net
namespace ent {
class EntityWorld;
class ItemModels;
}

struct CameraState;
struct ViewEffects;

namespace ui {

struct ItemStack;
class Items;

// Key mappings (Options.keyMappings, then the hotbar slots), bound to Windows virtual keys; mouse buttons are
// VK_LBUTTON / VK_RBUTTON / VK_MBUTTON / VK_XBUTTON1 / VK_XBUTTON2, 0 is unbound (keybinds.cpp).
enum class Key : u8 {
    Attack, Use, Forward, Left, Back, Right, Jump, Sneak, Sprint, Drop, Inventory, Chat, PlayerList, PickItem, Command,
    Friends, SocialInteractions, ToggleGui, ToggleSpectatorShaderEffects, Screenshot, TogglePerspective, SmoothCamera,
    Fullscreen, SpectatorOutlines, SpectatorHotbar, SwapOffhand, SaveToolbarActivator, LoadToolbarActivator,
    Advancements, QuickActions, DebugOverlay, DebugModifier,
    Hotbar1, Hotbar2, Hotbar3, Hotbar4, Hotbar5, Hotbar6, Hotbar7, Hotbar8, Hotbar9,
    DebugReloadChunk, DebugShowHitboxes, DebugClearChat, DebugCrash, DebugShowChunkBorders, DebugShowAdvancedTooltips,
    DebugCopyRecreateCommand, DebugSpectate, DebugSwitchGameMode, DebugDebugOptions, DebugFocusPause,
    DebugDumpDynamicTextures, DebugReloadResourcePacks, DebugProfiling, DebugCopyLocation, DebugDumpVersion,
    DebugProfilingChart, DebugFpsCharts, DebugNetworkCharts, DebugLightmapTexture, DebugImprovedTransparency,
    Count
};
constexpr size_t kKeyCount = size_t(Key::Count);
struct KeyDef {
    const char* name;      // "key.forward" (options.txt key_<name>, the translation key)
    const char* category;  // "movement" (key.category.minecraft.<category>; KeyMapping.Category order)
    int defaultKey;        // virtual key
    int order = 0;         // within the category (KeyMapping.order), then by translated name
};
const KeyDef& keyDef(Key k);
std::string keyName(int vk);              // InputConstants name: "key.keyboard.w", "key.mouse.left"
int keyFromName(std::string_view name);   // 0: unknown / unbound
extern const char* const kModelParts[7];  // PlayerModelPart ids in mask bit order (options.txt modelPart_<id>)

struct Settings {
    float fov = 70;           // degrees (vertical), 30..110
    int renderDistance = 32;  // full-detail radius in chunks
    int guiScale = 0;         // 0 = auto
    float sensitivity = 0.5f;  // 0..1 like the game's option
    VideoSettings video;       // Video Settings screen (shaders, upscaler, frame generation, latency, VSync, FPS limit)
    bool videoSaved = false;   // options.txt had video settings
    std::string shaderPack;    // shader pack in use (render/packs; "" = the built-in pipeline)
    bool bobView = true;       // "View Bobbing" (options.txt bobView)
    int attackIndicator = 1;   // AttackIndicatorStatus id: 0 off, 1 crosshair, 2 hotbar (options.txt attackIndicator)
    std::array<int, kKeyCount> keys = defaultKeys();  // key_<name>
    bool fullscreen = false;
    // Controls / Mouse Settings
    bool toggleCrouch = false, toggleSprint = false, autoJump = false;
    int sprintWindow = 7;  // ticks a double-tapped forward key starts sprinting within (0: off)
    bool invertMouseX = false, invertMouseY = false, discreteMouseScroll = false;
    double mouseWheelSensitivity = 1.0;  // 10^(-2..1) like Options.logMouse
    // Skin Customization (client_information): main hand 0 left 1 right, PlayerModelPart bits (cape 0 .. hat 6)
    int mainHand = 1;
    int modelParts = 0x7F;
    bool allowServerListing = true;
    bool pauseOnLostFocus = true, advancedItemTooltips = false;
    // The rest of Options (options_data.cpp keeps them in options.txt under the game's keys, with its defaults).
    // Video Settings: quality and preferences
    int graphicsPreset = 1;                // GraphicsPreset: 0 fast, 1 fancy, 2 fabulous, 3 custom
    int simulationDistance = 12;           // chunks (the local server's simulation distance)
    double entityDistanceScaling = 1.0;    // 0.5..5 (entity render distance)
    bool entityShadows = true, ao = true, cutoutLeaves = true, vignette = true, improvedTransparency = false;
    int biomeBlendRadius = 2;              // 0..7 (blend width 2r + 1)
    int cloudStatus = 2;                   // CloudStatus: 0 off, 1 fast, 2 fancy (options.txt renderClouds)
    int cloudRange = 128;                  // chunks, 2..128
    int weatherRadius = 10;                // blocks, 3..10
    double chunkFade = 0.75;               // seconds, 0..2 (chunkSectionFadeInTime)
    int prioritizeChunkUpdates = 0;        // PrioritizeChunkUpdates: 0 none, 1 player affected, 2 nearby
    double gamma = 0.5;                    // Brightness
    int mipmapLevels = 4;                  // 0..4
    int textureFiltering = 0;              // TextureFilteringMethod: 0 none, 1 RGSS, 2 anisotropic
    int maxAnisotropyBit = 2;              // 1..3 (2x, 4x, 8x)
    int particles = 0;                     // ParticleStatus: 0 all, 1 decreased, 2 minimal
    int inactivityFpsLimit = 1;            // InactivityFpsLimit: 0 minimized, 1 afk
    bool showAutosaveIndicator = true;
    // Accessibility
    int narrator = 0;                      // NarratorStatus: 0 off, 1 all, 2 chat, 3 system
    bool narratorHotkey = true, showSubtitles = false, highContrast = false, highContrastBlockOutline = false;
    bool hideLightningFlash = false, hideSplashTexts = false, darkMojangStudiosBackground = false, rotateWithMinecart = false;
    int menuBackgroundBlurriness = 5;      // 0..10
    double textBackgroundOpacity = 0.5, panoramaSpeed = 1.0, notificationDisplayTime = 1.0;
    double screenEffectScale = 1.0, fovEffectScale = 1.0, darknessEffectScale = 1.0;
    double glintSpeed = 0.5, glintStrength = 0.75, damageTiltStrength = 1.0;
    bool backgroundForChatOnly = true;
    // Chat Settings
    int chatVisibility = 0;                // ChatVisiblity: 0 full, 1 system (commands only), 2 hidden
    bool chatColors = true, chatLinks = true, chatLinksPrompt = true, autoSuggestions = true, hideMatchedNames = true;
    bool reducedDebugInfo = false, onlyShowSecureChat = false, saveChatDrafts = false;
    double chatOpacity = 1.0, chatLineSpacing = 0.0, chatScale = 1.0, chatWidth = 1.0;
    double chatHeightFocused = 1.0, chatHeightUnfocused = 70.0 / 160.0, chatDelay = 0.0;
    // Language and fonts
    std::string lang = "en_us";
    bool forceUnicodeFont = false, japaneseGlyphVariants = false;
    // Music & Sounds
    std::string soundDevice;               // "" = the system default
    bool directionalAudio = false;
    int musicFrequency = 0;                // MusicManager.MusicFrequency: 0 default, 1 frequent, 2 constant
    int musicToast = 0;                    // MusicToastDisplayState: 0 never, 1 pause, 2 pause and toast
    // Flags and preferences
    std::string lastServer;                // lastServer (Direct Connect's address)
    int tutorialStep = 0;                  // TutorialSteps: 0 movement, 1 find_tree, 2 punch_tree, 3 open_inventory, 4 craft_planks, 5 none
    bool joinedFirstServer = false, skipMultiplayerWarning = false, hideServerAddress = false;
    bool realmsNotifications = true, telemetryOptInExtra = false, onboardAccessibility = true, startedCleanly = true;
    bool loaded = false;

    int key(Key k) const { return keys[size_t(k)]; }
    static std::array<int, kKeyCount> defaultKeys();

    static fs::path path();
    bool load();
    void save() const;
    bool loadExtra(const std::string& key, const std::string& value);  // options_data.cpp: the keys above
    void saveExtra(std::ostream& out) const;
    // Mouse look in radians per pixel (the game's sensitivity curve).
    float radiansPerPixel() const;
};

// Title screen, world / server lists and switching between them (title.cpp, worlds.cpp, servers.cpp). Each world
// and the title screen run in their own instance of the program (platform/launch.h).
struct FrontEnd {
    bool menu = false;          // this instance is the title screen (no world)
    fs::path minecraftDir, savesDir, jar;
    fs::path serversFile;       // our server list (servers.json); servers.dat is imported from it once
    fs::path serversDat;        // the game's server list (read only)
    std::string version;        // installed game version ("26.3")
    std::string playerName;     // the Java profile (multiplayer name)
    std::string openScreen;     // --menu worlds|servers: open that list first
    std::wstring childScript;   // tests: --script for the next instance
    // In a world:
    fs::path worldDir;          // the save (singleplayer / viewer)
    std::string worldName;      // its name when level.dat does not have it yet (a world being created)
    bool singleplayer = false;  // through the local server: Open to LAN is offered
    u16 lanPort = 0;            // already open to LAN on this port (--lan)
    // Singleplayer pause (IntegratedServer pausing while a pause screen is open): freezes / resumes the local server.
    std::function<void(bool paused)> pauseServer;
    // Open to LAN: publishes the local server (game mode for others, the host's cheats, port); error on failure.
    std::function<bool(const std::string& motd, const std::string& gameMode, bool cheats, u16 port, std::string& error)>
        openToLan;
    // Multiplayer: the server list entry this connection belongs to (its address; --connect, or the first server of a
    // transfer), for its "Server Resource Packs" setting and accepted Code of Conduct.
    std::string serverAddress;
    // Server resource packs changed the client asset stack: reload what main.cpp owns (block textures); false: failed.
    std::function<bool()> reloadResources;
};

// Map data from ClientboundMapItemDataPacket (main thread): the 128 x 128 packed map colours of a map id, nullptr if
// none arrived; `version` changes with every update. mapColorRgba: MapColor.getColorFromPackedId as RGBA bytes.
const std::vector<u8>* mapColors(int id, u32& version);
u32 mapColorRgba(u8 packed);
// The map's decorations (MapDecoration, replaced with each update that carries them): map_decoration_type id, position
// in half map pixels from the centre (-128..127), rotation in 16ths of a turn, name (component NBT, "" none).
struct MapDecor {
    int type = 0, x = 0, y = 0, rot = 0;
    std::string name;
};
const std::vector<MapDecor>* mapDecorations(int id);
// MapDecorationType: the sprite (texture below textures/: "map/decorations/player"; nullptr for an unknown type) and
// showOnItemFrame (also what GUI maps draw).
const char* mapDecorationSprite(int type, bool* onFrame = nullptr);

// The title screen's panorama world (a cache, generated once): level.dat with the seed and the camera spot.
bool writeTitleWorld(const fs::path& dir, i64 seed, int dataVersion, const double pos[3], float yawDeg, float pitchDeg);

class Ui {
public:
    struct Init {
        BlockRegistry* registry = nullptr;
        Renderer* renderer = nullptr;
        Window* window = nullptr;
        Game* game = nullptr;
        World* world = nullptr;
        Terrain* terrain = nullptr;
        int detailChunks = 32;  // launch detail radius (upper bound of the render distance option)
    };

    Ui();
    ~Ui();
    // Game resources (fonts, sprites, items); before any client is created.
    bool load(const fs::path& jar, const fs::path& minecraftDir, const GameReports& reports);
    // Once the renderer and the game exist: registers the render pass and the game hooks.
    bool init(const Init& init);
    // Registers the play-packet handlers on a client (before Client::start).
    void registerHandlers(net::Client& client);
    // The session for the client above (may appear mid-run; switches the HUD to server-driven state).
    void attachSession(net::Client* client, net::Session* session);

    void beginFrame(double dt);
    void endFrame();

    bool quitRequested() const;
    // Front end (title.cpp): before init(). The title screen opens with init() in menu mode.
    void setFrontEnd(const FrontEnd& fe);
    bool titleRequested() const;   // "Save and Quit to Title" / "Disconnect" / "Back to Title Screen" (quit follows)
    bool switching() const;        // the next instance is loading: background work can pause
    // Leaving a world for the title screen: a progress screen while the world saves, then the title instance starts
    // and quitRequested() turns true once it shows its window.
    void showSaving();
    void startTitle();
    bool menuOpen() const;  // a full-screen menu / inventory is open (frame generation pauses, vendor guidance)
    const fs::path* screenshotRequest();  // scripted screenshots: pass to Renderer::render
    bool loadScript(const fs::path& file);
    float fovRadians() const;
    bool hudHidden() const;  // F1
    bool inBed() const;      // sleeping (the bed screen is up)
    // F3 biome name at a block position (local worlds with the generator); unset: not shown.
    void setBiomeLookup(std::function<std::string(int x, int y, int z)> fn);
    Settings& settings();
    // Entity models, skins and entities (inventory previews, mounts); after init().
    void setEntities(ent::EntityWorld* entities);
    // Item models (equipment layers worn in the entity previews); set by the entity visuals.
    void setItemModels(ent::ItemModels* models);
    // Player inventory menu slot (5-8 armour, 36-44 hotbar, 45 offhand); nullptr when empty.
    const ItemStack* playerSlot(int slot) const;
    const Items& items() const;
    // LivingEntity.getEquipmentSlotForItem / isEquippableInSlot for the player (the stack's equippable component, set
    // by the gameplay layer): the EquipmentSlot id (0 main hand, 1 feet .. 4 head, 5 off hand...), allowed: the
    // equippable allows players. Unset: armour by name.
    int ticksFrozen() const;  // our DATA_TICKS_FROZEN (the third-person avatar shakes when fully frozen)
    // The local player's mob effects as the view shows them (fog, lightmap: view_effects.cpp), and the nausea / nether
    // portal warp of the world camera (GameRenderer.renderLevel).
    ViewEffects viewEffects() const;
    void warpCamera(CameraState& cam) const;

    struct State;  // ui_state.h

private:
    bool loadImpl(const fs::path& jar, const fs::path& minecraftDir, const GameReports& reports);
    std::unique_ptr<State> s_;
};

}  // namespace ui
