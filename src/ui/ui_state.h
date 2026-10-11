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

namespace ui {

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
    int playerStart = 0;  // first slot of the player inventory (27 main + 9 hotbar)
    int titleX = 8, titleY = 6, invLabelX = 8, invLabelY = 72;
    bool chestRows = false;  // generic_9xN: background drawn in two parts
    int rows = 0;
};
Layout layoutFor(const Menu& m);

struct ChatLine {
    Text text;
    int addedTick = 0;
    bool endOfEntry = true;
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
};

enum class Screen { None, Chat, Inventory, Container, Creative, Pause, Options, Death, Mods, Video };

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
    bool serverAir = false;  // air supply comes from the server (entity data)
    int entityId = -1;
    struct Effect {
        std::string name;  // "poison"
        int amplifier = 0, duration = 0;  // ticks, -1 infinite
        bool ambient = false, showIcon = true;
    };
    std::map<int, Effect> effects;  // by mob_effect network id
    bool hasEffect(const char* name) const {
        for (const auto& [id, e] : effects)
            if (e.name == name) return true;
        return false;
    }
    bool hardcore = false;
    int lastGameMode = -1;

    Menu inventory;                   // 46 slots, container 0
    std::unique_ptr<Menu> container;  // open server container
    ItemStack carried;
    std::vector<ItemStack> creativeItems;  // all items in registry order
    int hotbarSynced[9] = {-2, -2, -2, -2, -2, -2, -2, -2, -2};

    // HUD
    bool hudHidden = false, debug = false;
    int guiTicks = 0;
    double tickClock = 0;
    float partialTick = 0;
    Text title, subtitle, actionbar;
    bool hasTitle = false, hasSubtitle = false;
    int titleTime = 0, titleFadeIn = 10, titleStay = 70, titleFadeOut = 20;
    int actionbarTime = 0;
    int toolHighlightTimer = 0;
    ItemStack lastToolHighlight;
    int lastSelected = -1;
    double fps = 0, frameMs = 0;
    std::deque<float> frameTimes;
    int heartBlinkEnd = 0, lastHealthInt = 20, displayHealth = 20;
    double lastHealthTime = 0;

    // Chat
    std::vector<ChatLine> chatLines;  // newest first, wrapped
    std::vector<std::pair<Text, int>> chatMessages;  // newest first: message, added tick
    std::vector<std::string> sentHistory;
    int historyPos = -1;
    std::string historyDraft;
    int chatScroll = 0;
    // Message acknowledgements (LastSeenMessagesTracker): signatures of signed messages.
    struct Seen {
        std::string signature;
        bool acknowledged = false;
    };
    std::vector<std::optional<Seen>> lastSeen = std::vector<std::optional<Seen>>(20);
    int lastSeenTail = 0, lastSeenOffset = 0;
    std::string lastSeenSignature;

    // Text field (chat input, creative search).
    struct TextField {
        std::string value;
        int cursor = 0, selection = 0;  // byte offsets
        int maxLength = 256;
        double blinkStart = 0;
        int scroll = 0;  // first visible byte
    };
    TextField chatInput, search;

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
    // Creative screen.
    float creativeScroll = 0;  // 0..1
    std::vector<int> creativeFiltered;
    bool scrolling = false;

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
    void openScreen(Screen s);
    void closeScreen(bool sendClose = true);
    void syncHotbarToGame();
    void addChat(const Text& message);
    void sendChat(const std::string& text);
    const Lang* lang() const { return &res.lang(); }
    void runScript(double dt);

    // --- hud.cpp ---
    void tickHud();
    void drawHud(Canvas& c);
    void drawChat(Canvas& c, bool focused);
    void drawDebug(Canvas& c);
    void drawItem(Canvas& c, const ItemStack& s, float x, float y, bool decorations = true, const char* countOverride = nullptr);
    void drawTooltip(Canvas& c, const std::vector<Text>& lines, float x, float y);
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
    bool buttonsInput(const Window::InputEvent& e);
    Menu& activeMenu() { return container ? *container : inventory; }
    void containerClick(int slot, int button, int input);  // simulates locally and sends container_click
    void creativeSetSlot(int menuSlot, const ItemStack& s);
    void updateCreativeFilter();
    void dropSelected(bool all);
    void swapHands();
    int menuSlotForInventory(const Menu& m, int inventoryIndex) const;  // Inventory index (0-8 hotbar, 40 offhand)

    // --- video_screen.cpp: Video Settings (shaders, DLSS / FSR / XeSS, frame generation, latency, VSync, FPS cap) ---
    void buildVideoButtons();
    void drawVideoScreen(Canvas& c);
    void videoInput(const Window::InputEvent& e);
    void applyVideo(VideoSettings v);

    // --- mods_screen.cpp: Mods & Packs screen (mods/mod_manager.h), native mods' HUD ---
    int modsScroll = 0;  // first list row shown
    void buildModsButtons();
    void drawModsScreen(Canvas& c);
    void modsInput(const Window::InputEvent& e);
    void drawModHud(Canvas& c);
    void initNativeMods();
    // Last member: native mods unload first (their on_unload may still use the UI).
    mods::NativeMods nativeMods;
};

}  // namespace ui
