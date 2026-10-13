#pragma once
// Internal state shared by the UI's parts (ui.cpp: core, input, network; hud.cpp: HUD, chat, F3;
// screens.cpp: inventories, containers, creative, pause, options).
#include "game/game.h"
#include "mods/native_mods.h"
#include "net/client.h"
#include "net/session.h"
#include "platform/window.h"
#include "ui/canvas.h"
#include "ui/font.h"
#include "ui/icons.h"
#include "ui/items.h"
#include "ui/modules.h"
#include "ui/resources.h"
#include "ui/sprites.h"
#include "ui/text.h"
#include "ui/ui.h"
#include "ui/ui_pass.h"

#include <deque>
#include <map>
#include <functional>
#include <mutex>
#include <optional>

class World;
class BlockRegistry;
class Terrain;
class GameReports;

namespace ent {
class EntityWorld;
class ItemModels;
}

namespace packs {
class PackOptionsModel;  // render/packs/pack_options.h
}

namespace ui {

struct FrontEndState;  // frontend.h

// A server (or local) menu: the player inventory (container 0) or an open container.
struct Menu {
    int containerId = 0;
    int type = -1;  // minecraft:menu network id; -1: player inventory
    std::string typeName = "inventory";
    Text title;
    std::vector<ItemStack> slots;
    std::vector<int> data = std::vector<int>(16, 0);
    int stateId = 0;
};

// Where a menu's slots are on screen (relative to the background) and how it is drawn.
struct SlotPos {
    int x, y;
};
struct Layout {
    std::string background;  // texture below textures/ ("gui/container/inventory")
    int width = 176, height = 166;
    int texW = 256, texH = 256;
    std::vector<SlotPos> slots;
    int playerStart = 0;  // first slot of the player inventory (27 main + 9 hotbar); -1: none (lectern)
    int titleX = 8, titleY = 6, invLabelX = 8, invLabelY = 72;  // titleX -1: centred; Y <= -1000: not drawn
    bool chestRows = false;  // generic_9xN: background drawn in two parts
    int rows = 0;
};
Layout layoutFor(const Menu& m);

// What the client tells the server about its options (main hand, skin parts, server listing).
net::Client::Information clientInformation(const Settings& s);

// GuiMessageTag: the indicator left of a chat message (0: none) and its tooltip's translation key.
struct ChatTag {
    u32 color = 0;
    std::string key;
};
inline const ChatTag kTagSystemSinglePlayer{0xD0D0D0, "chat.tag.system_single_player"};
inline const ChatTag kTagSystem{0xD0D0D0, "chat.tag.system"};
inline const ChatTag kTagNotSecure{0xD0D0D0, "chat.tag.not_secure"};
inline const ChatTag kTagError{0xFF5555, "chat.tag.error"};
// A tooltip's image component (ClientTooltipComponent.extractImage, e.g. a bundle's contents), after the first line.
struct TooltipImage {
    int width = 0, height = 0;
    std::function<void(Canvas& c, float x, float y, int tooltipWidth)> draw;
};

struct ChatLine {
    Text text;
    int addedTick = 0;
    bool endOfEntry = true;
    ChatTag tag;
};
struct ChatMessage {  // GuiMessage
    Text text;
    int addedTick = 0;
    std::string signature;  // MessageSignature (256 bytes), "" none
    ChatTag tag;
    u8 source = 1;  // GuiMessageSource: 0 client system, 1 server system, 2 player (the Chat option filters them)
};

struct Button {
    float x = 0, y = 0, w = 200, h = 20;
    Text label;
    bool active = true;
    std::function<void()> onPress;
    // Sliders: value 0..1, label from the value; dragging updates it.
    bool slider = false;
    float value = 0;
    std::function<void(float)> onSlide;
    std::function<Text()> labelFn;
    std::function<Text()> tooltip;  // shown while hovered (Tooltip, split at 170 pixels); empty text: none
    std::string icon;  // SpriteIconButton.TextAndIcon: this sprite at the right, the label centred beside it
    float iconW = 0, iconH = 0;
    bool iconOnly = false;       // SpriteIconButton.CenteredIcon: only the sprite (the label is for narration / tooltip)
    std::function<int()> badge;  // FriendsButton: a notification count drawn at the top right (notification/1..5, more)
};

enum class Screen {
    None, Chat, Inventory, Container, Creative, Pause, Options, Death, Mods, Video, PackOptions,
    // Options sub-screens (options_screens.cpp; VideoExtra: video_screen.cpp)
    Online, SkinCustomization, Sounds, Controls, MouseSettings, KeyBinds, Accessibility, VideoExtra,
    ChatOptions, Language, FontOptions, AccessibilityOnboarding, ConfirmLink,  // options_screens.cpp, language_screen.cpp
    // Registered screens (modules.h ScreenDef): signs and books, advancements, statistics, game mode switcher, bed
    Sign, BookEdit, BookView, Advancements, Stats, GameModeSwitcher, InBed,
    Friends, AddFriend,  // friends_screen.cpp
    Dialog,              // dialogs.cpp: server dialogs and their confirmation / waiting screens
    ServerPackPrompt, CodeOfConduct, Reconfiguring,  // server_config.cpp
    Win, CreditsAttribution,  // credits.cpp
    WorldOptions, InWorldGameRules, Restrictions,  // world_options.cpp
    CommandBlockEdit, MinecartCommandEdit, JigsawEdit, StructureEdit, TestBlockEdit, TestInstanceEdit,  // block_editors.cpp
    SocialInteractions,  // social_screen.cpp
    DebugOptions,        // debug_screen.cpp
    MultiplayerWarning,  // servers.cpp: SafetyScreen
    // Front end (frontend.h)
    Title, Worlds, CreateWorld, EditWorld, Confirm, Servers, EditServer, DirectConnect, Loading, Message
};

struct Ui::State {
    Init init;
    bool loaded = false;
    Window* window = nullptr;
    Renderer* renderer = nullptr;
    Game* game = nullptr;
    World* world = nullptr;
    BlockRegistry* registry = nullptr;
    Terrain* terrain = nullptr;
    const GameReports* reports = nullptr;
    ent::EntityWorld* entities = nullptr;  // entity models / skins / the mount (Ui::setEntities); may be null
    ent::ItemModels* itemModels = nullptr; // equipment layers for the previews (Ui::setItemModels); may be null
    net::Client* client = nullptr;
    net::Session* session = nullptr;
    bool online() const { return client != nullptr && session != nullptr; }

    Resources res;
    Atlas guiAtlas, iconAtlas;
    Font font;
    Sprites sprites;
    Items items;
    ItemIcons icons;
    std::unique_ptr<UiPass> pass;
    Settings settings;
    Canvas* canvas = nullptr;
    std::function<std::string(int, int, int)> biomeLookup;
    int lastGuiW = 0, lastGuiH = 0;
    int guiW() const { return int((window->width() + u32(guiScale()) - 1) / u32(guiScale())); }
    int guiH() const { return int((window->height() + u32(guiScale()) - 1) / u32(guiScale())); }

    // Player state from the server (local mode: creative defaults).
    float health = 20, maxHealth = 20, saturation = 5, absorption = 0;
    int food = 20;
    float xpProgress = 0;
    int xpLevel = 0, xpTotal = 0;
    float armor = 0;
    int air = 300, maxAir = 300;
    int ticksFrozen = 0;  // our DATA_TICKS_FROZEN (powder snow: the frost overlay, fully frozen at 140)
    bool serverAir = false;  // air supply comes from the server (entity data)
    int entityId = -1;
    struct Effect {
        std::string name;  // "poison"
        int amplifier = 0, duration = 0;  // ticks, -1 infinite
        bool ambient = false, showIcon = true;
        float blend = 0, blendO = 0;  // MobEffectInstance.BlendState factor (this / previous tick)
    };
    std::map<int, Effect> effects;  // by mob_effect network id
    bool hasEffect(const char* name) const {
        for (const auto& [id, e] : effects)
            if (e.name == name) return true;
        return false;
    }
    const Effect* findEffect(const char* name) const {
        for (const auto& [id, e] : effects)
            if (e.name == name) return &e;
        return nullptr;
    }
    // View effects (view_effects.cpp): the local player's state behind the fog, lightmap, warp and screen overlays.
    int waterVisionTime = 0;                          // LocalPlayer.waterVisionTime
    bool eyeInWater = false;                          // this tick
    float portalIntensity = 0, portalIntensityO = 0;  // LocalPlayer.portalEffectIntensity (nether portal)
    float spinTime = 0, spinSpeed = 0;                // LocalPlayer.spinningEffectTime / spinningEffectSpeed
    bool onFire = false;                              // our shared flag 0 (Entity.isOnFire on the client)
    std::vector<std::string> postEffects;             // ClientboundPostEffectsPacket: the player's active post effects
    void tickViewEffects(bool eyeInWater);
    float effectBlend(const char* name) const;        // LivingEntity.getEffectBlendFactor at the partial tick
    bool hardcore = false;
    // The server's policy for this player (login, game_event, entity_event): F3 without positions, the death screen
    // (off: immediate respawn), limited crafting (stored; the recipe book does not use it yet); the level's difficulty.
    bool reducedDebugInfo = false, showDeathScreen = true, limitedCrafting = false;
    int difficulty = 2;  // 0 peaceful, 1 easy, 2 normal, 3 hard
    bool difficultyLocked = false;
    Text deathMessage;  // ClientboundPlayerCombatKillPacket
    int lastGameMode = -1;

    Menu inventory;                   // 46 slots, container 0
    std::unique_ptr<Menu> container;  // open server container
    ItemStack carried;
    int hotbarSynced[10] = {-2, -2, -2, -2, -2, -2, -2, -2, -2, -2};  // hotbar items, then the off hand

    // HUD
    bool hudHidden = false, debug = false;
    int guiTicks = 0;
    // Where the Options sub-screens (Sounds, Accessibility, Language) go back to: the Options screen, or the
    // accessibility onboarding screen that opened them.
    Screen optionsParent = Screen::Options;
    // Hud.tickAutosaveIndicator: the local server saving (MinecraftServer.isCurrentlySaving), eased in and out.
    bool serverSaving = false;
    int xpDisplayStart = INT_MIN;  // LocalPlayer.experienceDisplayStartTick (gui ticks): the progress last changed
    float autosaveIndicator = 0, lastAutosaveIndicator = 0;
    double tickClock = 0;
    float partialTick = 0;
    Text title, subtitle, actionbar;
    bool hasTitle = false, hasSubtitle = false;
    int titleTime = 0, titleFadeIn = 10, titleStay = 70, titleFadeOut = 20;
    int actionbarTime = 0;
    bool actionbarAnimated = false;  // Hud.setNowPlaying: the overlay message's colour cycles
    int spectatorEffectToggles = 0;
    // Engine numbers for F3 entries the UI cannot see ("particles", "entities", "sound_mood", "sound_cache",
    // "post_effects", "day_count"): their line, "" none (set by main).
    std::function<std::string(std::string_view what)> engineInfo;  // presses of Toggle Spectator Shader Effects (GameRenderer.toggleSpectatorPostEffect)
    int toolHighlightTimer = 0;
    ItemStack lastToolHighlight;
    int lastSelected = -1;
    double fps = 0, frameMs = 0;
    std::deque<float> frameTimes;
    int heartBlinkEnd = 0, lastHealthInt = 20, displayHealth = 20;
    double lastHealthTime = 0;
    float fovModifier = 1, oldFovModifier = 1;  // Camera.tickFov

    // Chat
    std::vector<ChatLine> chatLines;  // newest first, wrapped
    std::vector<ChatMessage> chatMessages;  // newest first (ChatComponent.allMessages)
    // ChatListener: messages waiting for the Chat Delay option (each shows itself; false: dropped), and when the last
    // one was shown (seconds, clock).
    std::deque<std::function<bool()>> chatQueue;
    double previousMessageTime = 0;
    std::vector<std::string> sentHistory;
    int historyPos = -1;
    std::string historyDraft;
    int chatScroll = 0;
    bool newMessageSinceScroll = false;
    // ChatComponent.Draft (Save Unsent Chats): the text left in the chat input, shown again (grey, italic) the next
    // time the chat opens; how the chat screen was left (ChatScreen.ExitReason: 0 interrupted, 1 Esc, 2 sent).
    std::string chatDraft;
    bool chatIsDraft = false;
    int chatExit = 0;
    void leaveChat();
    void openChat(bool command);  // Gui.openChatScreen(MESSAGE / COMMAND): the draft or the prefix

    // Text field (chat input, creative search).
    struct TextField {
        std::string value;
        int cursor = 0, selection = 0;  // byte offsets
        int maxLength = 256;
        double blinkStart = 0;
        int scroll = 0;  // first visible byte
    };
    TextField chatInput, search;
    // EditBox.setFormatter for the chat input (chat_ext.cpp: command colouring): the visible text from `offset`;
    // false keeps the plain text.
    std::function<bool(const std::string& visible, int offset, Text& out)> chatFormatter;
    // The same for one other text field (the command block editors' command: chat_ext.cpp).
    TextField* formattedField = nullptr;
    std::function<bool(const std::string& visible, int offset, Text& out)> fieldFormatter;

    // Screens
    Screen screen = Screen::None;
    std::vector<Button> buttons;
    int draggingSlider = -1;
    float mouseX = 0, mouseY = 0;  // GUI coordinates
    bool leftDown = false, rightDown = false;
    // Container interaction (AbstractContainerScreen).
    int hoveredSlot = -1;
    bool isQuickCrafting = false;
    int quickCraftButton = 0, quickCraftType = 0;
    std::vector<int> quickCraftSlots;
    int quickCraftRemaining = 0;
    bool skipNextRelease = false, doubleClick = false;
    int lastClickSlot = -2;
    double lastClickTime = 0;
    int lastClickButton = -1;
    ItemStack lastQuickMoved;

    // Network events decoded on the network thread, applied on the main thread.
    std::mutex mutex;
    std::vector<std::function<void()>> pending;
    void post(std::function<void()> fn) {
        std::lock_guard lock(mutex);
        pending.push_back(std::move(fn));
    }

    // Script / app control.
    struct ScriptStep {
        double time;
        std::string verb;
        std::vector<std::string> args;
    };
    std::vector<ScriptStep> script;
    size_t scriptPos = 0;
    double clock = 0;
    fs::path shotPath;
    bool shotPending = false, quit = false, scriptShiftHeld = false;

    // --- ui.cpp ---
    int gameMode() const;
    bool creative() const { return gameMode() == 1; }
    bool spectator() const { return gameMode() == 3; }
    bool survivalLike() const { return gameMode() == 0 || gameMode() == 2; }
    int guiScale() const;
    // --- keybinds.cpp ---
    Text keyLabel(int vk) const;                                   // a bound key as the game shows it
    bool isKey(Key k, const Window::InputEvent& e) const;         // a press of the key bound to k
    void openScreen(Screen s);
    void toggleFullscreen();
    void applyControls();          // Settings -> Game (keys, toggles, mouse options), every frame
    bool keyCaptureActive = false;  // Key Binds: the next key / button binds (no other key handling)
    bool paused = false;             // Minecraft.isPaused: singleplayer (not LAN) with a pause screen open
    double lastActiveTime = 0;       // Minecraft.pauseIfInactive: the window last had the focus (clock)
    void updatePause();
    void closeScreen(bool sendClose = true);
    void syncHotbarToGame();
    void addChat(const Text& message, int source = 0);  // a client (0) / server (1) system message
    void addChatMessage(ChatMessage m);                  // ChatComponent.addMessage (dropped when chat hides it)
    void refreshChatLines();                             // ChatComponent.refreshTrimmedMessages
    void handleChatMessage(std::function<bool()> show);  // ChatListener.handleMessage: now, or after the delay
    void tickChat();                                     // ChatListener.tick + ChatComponent.tick
    bool chatVisible(int source) const;                  // ChatAbilities.visibleMessagesFilter (Chat option)
    int chatLinesPerPage() const;
    void scrollChat(int lines);                          // ChatComponent.scrollChat
    // Options.getBackgroundColor: text backgrounds outside the chat use the Text Background Opacity option when the
    // Text Background option says Everywhere, else their own colour.
    u32 textBackground(u32 defaultColor) const {
        return settings.backgroundForChatOnly ? defaultColor : withAlpha(0, float(settings.textBackgroundOpacity));
    }
    u32 textBackground(float defaultOpacity) const {
        return withAlpha(0, settings.backgroundForChatOnly ? defaultOpacity : float(settings.textBackgroundOpacity));
    }
    void sendChat(const std::string& text);
    const Lang* lang() const { return &res.lang(); }
    void runScript(double dt);

    // --- hud.cpp ---
    void tickHud();
    void drawHud(Canvas& c);
    void drawSavingIndicator(Canvas& c);  // Hud.extractSavingIndicator
    bool rendersNowPlayingToast() const;
    void drawChat(Canvas& c, bool focused);
    // playerSlot: the player inventory menu slot the stack is (item model properties compare stack identity: the
    // selected stack, the stack in use); -1 none.
    void drawItem(Canvas& c, const ItemStack& s, float x, float y, bool decorations = true, const char* countOverride = nullptr,
                  int playerSlot = -1);
    void drawTooltip(Canvas& c, const std::vector<Text>& lines, float x, float y, const TooltipImage* image = nullptr);
    void drawText(Canvas& c, const Text& t, float x, float y, u32 color, bool shadow) { font.draw(c, t, x, y, color, shadow); }
    void drawCentered(Canvas& c, const Text& t, float cx, float y, u32 color, bool shadow = true) {
        font.draw(c, t, cx - float(font.width(t) / 2), y, color, shadow);
    }
    void drawTextField(Canvas& c, TextField& f, float x, float y, float w, u32 color, bool focused, bool shadow);
    bool textFieldKey(TextField& f, const Window::InputEvent& e);

    // --- screens.cpp ---
    void drawScreen(Canvas& c);
    void screenInput(const Window::InputEvent& e);
    void buildButtons();
    void drawButtons(Canvas& c);
    void drawButton(Canvas& c, size_t i, bool hoverable = true);
    // Clicks on buttons [first, last) (an options list takes the clicks inside it, the rest outside).
    bool buttonsInput(const Window::InputEvent& e, size_t first = 0, size_t last = SIZE_MAX);
    Menu& activeMenu() { return container ? *container : inventory; }
    void containerClick(int slot, int button, int input);  // simulates locally and sends container_click
    void creativeSetSlot(int menuSlot, const ItemStack& s);
    // The open container's player slots and the inventory menu are one Inventory (Java shares it): copy them across
    // (toContainer: inventory menu -> container).
    void mirrorPlayerSlots(bool toContainer = false);
    void dropSelected(bool all);
    void swapHands();
    int menuSlotForInventory(const Menu& m, int inventoryIndex) const;  // Inventory index (0-8 hotbar, 40 offhand)

    // --- video_screen.cpp: Video Settings, and Shaders & Upscaling (DLSS / FSR / XeSS, frame generation, latency) ---
    void applyVideo(VideoSettings v);
    // --- render/packs/pack_screen.cpp: a shader pack's option screens ---
    std::shared_ptr<packs::PackOptionsModel> packOptions;
    std::vector<std::string> packScreens;  // screen stack ("" = the pack's main screen)
    int packPage = 0;
    std::string packMessage;               // why the options could not be opened
    void openPackOptions();
    void closePackOptions();
    void buildPackButtons();
    void drawPackScreen(Canvas& c);
    void packInput(const Window::InputEvent& e);

    // --- mods_screen.cpp: Mods & Packs screen (mods/mod_manager.h), native mods' HUD ---
    int modsScroll = 0;  // first list row shown
    // Screen.onFilesDrop: files dropped on the window while this screen is open (set by its build, reset on a change).
    std::function<void(const std::vector<std::filesystem::path>&)> onFilesDrop;
    Screen modsReturn = Screen::None;  // opened from Options (Resource Packs...): back there, else pause / title
    void buildModsButtons();
    void drawModsScreen(Canvas& c);
    void modsInput(const Window::InputEvent& e);
    void modsBack();
    void drawModHud(Canvas& c);
    void initNativeMods();
    // --- title.cpp, worlds.cpp, servers.cpp: title screen, world / server lists, switching instances ---
    std::unique_ptr<FrontEndState> fe;
    bool frontEndScreen() const { return screen >= Screen::Title; }
    void buildFrontEndButtons();
    void drawFrontEnd(Canvas& c);
    void frontEndInput(const Window::InputEvent& e);
    void frontEndFrame();  // per frame: the next instance's progress, pings, disconnects
    // Last member: native mods unload first (their on_unload may still use the UI).
    mods::NativeMods nativeMods;
};

// language.cpp: what object text components draw (atlas sprites, player heads) and the right-to-left default.
void installFontHooks(Ui::State& s);

}  // namespace ui
