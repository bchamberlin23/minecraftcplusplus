#include "ui/ui.h"

#include "ui/item_props.h"

#include "assets/assets.h"
#include "platform/launch.h"
#include "audio/audio.h"
#include "assets/game_reports.h"
#include "ui/frontend.h"
#include "ui/item_ui.h"
#include "ui/signs_books.h"
#include "entity/entities.h"
#include "ui/block_editors.h"
#include "ui/debug_screen.h"
#include "ui/dialogs.h"
#include "ui/gameplay_hud.h"
#include "ui/narrator.h"
#include "ui/options_list.h"
#include "ui/mp_hud.h"
#include "ui/ui_state.h"
#include "world/terrain.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

namespace ui {

namespace {
constexpr double kTick = 0.05;
constexpr float kPi = 3.14159265f;

// Vec3.LP_STREAM_CODEC (as entity/entities.cpp reads it).
void readLpVec3(net::Reader& r, double out[3]) {
    out[0] = out[1] = out[2] = 0;
    const u32 lowest = r.u8v();
    if (lowest == 0) return;
    const u32 middle = r.u8v();
    const u64 highest = r.u32v();
    const u64 buffer = (highest << 16) | (u64(middle) << 8) | lowest;
    u64 scale = lowest & 3;
    if (lowest & 4) scale |= (u64(u32(r.varInt())) & 0xFFFFFFFFull) << 2;
    auto unpack = [](u64 v) { return std::min(double(v & 32767), 32766.0) * 2.0 / 32766.0 - 1.0; };
    out[0] = unpack(buffer >> 3) * double(scale);
    out[1] = unpack(buffer >> 18) * double(scale);
    out[2] = unpack(buffer >> 33) * double(scale);
}

std::string shortName(const std::string& n) { return n.substr(n.find(':') + 1); }

int vkFromName(std::string n) {
    for (char& c : n) c = char(std::toupper(u8(c)));
    if (n.size() == 1) {
        const char c = n[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
        if (c == '/') return VK_OEM_2;
    }
    if (n.size() >= 2 && n[0] == 'F' && std::isdigit(u8(n[1]))) return VK_F1 + std::atoi(n.c_str() + 1) - 1;
    static const std::pair<const char*, int> names[] = {
        {"ESC", VK_ESCAPE}, {"ESCAPE", VK_ESCAPE}, {"ENTER", VK_RETURN}, {"RETURN", VK_RETURN}, {"BACKSPACE", VK_BACK},
        {"TAB", VK_TAB},    {"SHIFT", VK_SHIFT},   {"CTRL", VK_CONTROL}, {"CONTROL", VK_CONTROL}, {"SPACE", VK_SPACE},
        {"UP", VK_UP},      {"DOWN", VK_DOWN},     {"LEFT", VK_LEFT},    {"RIGHT", VK_RIGHT},     {"DELETE", VK_DELETE},
        {"HOME", VK_HOME},  {"END", VK_END},       {"SLASH", VK_OEM_2},  {"LSHIFT", VK_LSHIFT}, {"LCTRL", VK_LCONTROL}};
    for (auto [k, v] : names)
        if (n == k) return v;
    return 0;
}
}  // namespace

// --- settings ----------------------------------------------------------------------------------------------

const char* const kModelParts[7] = {"cape", "jacket", "left_sleeve", "right_sleeve", "left_pants_leg", "right_pants_leg", "hat"};

fs::path Settings::path() {
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    return fs::path(local ? local : L".") / "minecraftCplusplus" / "options.txt";
}

bool Settings::load() {
    std::ifstream f(path());
    if (!f) return false;
    tutorialStep = 5;  // none: options from before the tutorial are an experienced player's (a new file starts it)
    std::string line;
    while (std::getline(f, line)) {
        const size_t c = line.find(':');
        if (c == std::string::npos) continue;
        const std::string k = line.substr(0, c), v = line.substr(c + 1);
        if (loadExtra(k, v)) continue;
        if (k == "fov") fov = std::clamp(float(std::atof(v.c_str())), 30.f, 110.f);
        else if (k == "renderDistance") renderDistance = std::clamp(std::atoi(v.c_str()), 2, 64);
        else if (k == "guiScale") guiScale = std::clamp(std::atoi(v.c_str()), 0, 8);
        else if (k == "mouseSensitivity") sensitivity = std::clamp(float(std::atof(v.c_str())), 0.f, 1.f);
        else if (k == "shaders") {
            static const char* presets[] = {"off", "low", "medium", "high", "ultra", "pathtraced"};
            for (u8 i = 0; i < 6; ++i)
                if (v == presets[i]) video.shaders = i, videoSaved = true;
        } else if (k == "pathTracing") VideoSettings::parsePathTracing(v, video.pathTracing);
        else if (k == "upscaler") VideoSettings::parseUpscaler(v, video.upscaler);
        else if (k == "upscaleQuality") VideoSettings::parseQuality(v, video.quality);
        else if (k == "sharpness") video.sharpness = std::clamp(float(std::atof(v.c_str())), 0.f, 1.f);
        else if (k == "frameGeneration") video.parseFrameGen(v);
        else if (k == "lowLatency") VideoSettings::parseLowLatency(v, video.lowLatency);
        else if (k == "vsync") video.vsync = v == "true";
        else if (k == "maxFps") video.fpsLimit = u32(std::clamp(std::atoi(v.c_str()), 0, 1000));
        else if (k == "shaderPack") shaderPack = v;
        else if (k == "bobView") bobView = v == "true";
        else if (k == "attackIndicator") attackIndicator = ((std::atoi(v.c_str()) % 3) + 3) % 3;  // ByIdMap WRAP
        else if (k == "fullscreen") fullscreen = v == "true";
        else if (k == "fullscreenResolution") fullscreenResolution = v;
        else if (k == "toggleCrouch") toggleCrouch = v == "true";
        else if (k == "toggleSprint") toggleSprint = v == "true";
        else if (k == "autoJump") autoJump = v == "true";
        else if (k == "sprintWindow") sprintWindow = std::clamp(std::atoi(v.c_str()), 0, 10);
        else if (k == "invertXMouse") invertMouseX = v == "true";
        else if (k == "invertYMouse") invertMouseY = v == "true";
        else if (k == "discrete_mouse_scroll") discreteMouseScroll = v == "true";
        else if (k == "mouseWheelSensitivity") mouseWheelSensitivity = std::clamp(std::atof(v.c_str()), 0.01, 10.0);
        else if (k == "mainHand") mainHand = v == "left" ? 0 : 1;
        else if (k == "allowServerListing") allowServerListing = v == "true";
        else if (k == "pauseOnLostFocus") pauseOnLostFocus = v == "true";
        else if (k == "advancedItemTooltips") advancedItemTooltips = v == "true";
        else if (k.starts_with("modelPart_")) {
            for (int i = 0; i < 7; ++i)
                if (k.substr(10) == kModelParts[i]) modelParts = v == "true" ? modelParts | (1 << i) : modelParts & ~(1 << i);
        } else if (k.starts_with("key_")) {  // key_key.forward:key.keyboard.w (an unknown key unbinds, like the game)
            for (size_t i = 0; i < kKeyCount; ++i)
                if (k.substr(4) == keyDef(Key(i)).name) keys[i] = keyFromName(v);
        }
    }
    loaded = true;
    return true;
}

void Settings::save() const {
    std::error_code ec;
    fs::create_directories(path().parent_path(), ec);
    std::ofstream f(path());
    f << "fov:" << fov << "\nrenderDistance:" << renderDistance << "\nguiScale:" << guiScale
      << "\nmouseSensitivity:" << sensitivity << "\n";
    static const char* presets[] = {"off", "low", "medium", "high", "ultra", "pathtraced"};
    f << "shaders:" << presets[std::min<int>(video.shaders, 5)] << "\npathTracing:" << VideoSettings::pathTracingKey(video.pathTracing)
      << "\nupscaler:" << VideoSettings::upscalerKey(video.upscaler)
      << "\nupscaleQuality:" << VideoSettings::qualityKey(video.quality) << "\nsharpness:" << video.sharpness
      << "\nframeGeneration:" << video.frameGenKey() << "\nlowLatency:" << VideoSettings::lowLatencyKey(video.lowLatency)
      << "\nvsync:" << (video.vsync ? "true" : "false") << "\nmaxFps:" << video.fpsLimit << "\n";
    f << "shaderPack:" << shaderPack << "\n";
    f << "bobView:" << (bobView ? "true" : "false") << "\nattackIndicator:" << attackIndicator << "\n";
    auto b = [](bool v) { return v ? "true" : "false"; };
    f << "fullscreen:" << b(fullscreen) << "\ntoggleCrouch:" << b(toggleCrouch) << "\ntoggleSprint:" << b(toggleSprint)
      << "\nautoJump:" << b(autoJump) << "\nsprintWindow:" << sprintWindow << "\ninvertXMouse:" << b(invertMouseX) << "\ninvertYMouse:" << b(invertMouseY)
      << "\ndiscrete_mouse_scroll:" << b(discreteMouseScroll) << "\nmouseWheelSensitivity:" << mouseWheelSensitivity
      << "\nmainHand:" << (mainHand == 0 ? "left" : "right") << "\nallowServerListing:" << b(allowServerListing)
      << "\npauseOnLostFocus:" << b(pauseOnLostFocus) << "\nadvancedItemTooltips:" << b(advancedItemTooltips) << "\n";
    if (!fullscreenResolution.empty()) f << "fullscreenResolution:" << fullscreenResolution << "\n";
    for (int i = 0; i < 7; ++i) f << "modelPart_" << kModelParts[i] << ":" << b(modelParts & (1 << i)) << "\n";
    for (size_t i = 0; i < kKeyCount; ++i) f << "key_" << keyDef(Key(i)).name << ":" << keyName(keys[i]) << "\n";
    saveExtra(f);
}

net::Client::Information clientInformation(const Settings& s) {
    net::Client::Information info;
    info.modelParts = u8(s.modelParts & 0x7F);
    info.mainHand = s.mainHand;
    info.allowListing = s.allowServerListing;
    info.language = s.lang, info.chatVisibility = s.chatVisibility, info.chatColors = s.chatColors;  // Options.buildPlayerInformation
    info.particles = s.particles;
    return info;
}

float Settings::radiansPerPixel() const {
    const float d = sensitivity * 0.6f + 0.2f;
    return d * d * d * 8.0f * 0.15f * kPi / 180.0f;
}

// --- state helpers ------------------------------------------------------------------------------------------

void Ui::State::toggleFullscreen() {  // Window.toggleFullScreen, saved as the fullscreen option
    settings.fullscreen = !window->fullscreen();
    window->setFullscreen(settings.fullscreen);
    settings.save();
}

namespace {
// Screen.isPauseScreen: everything but chat, inventories / containers, the death screen, sign editing, the bed chat and
// the game mode switcher (and the front end's own screens, which only the title instance shows).
bool pausesGame(Screen s) {
    switch (s) {
        case Screen::None: case Screen::Chat: case Screen::Inventory: case Screen::Container: case Screen::Creative:
        case Screen::Death: case Screen::Sign: case Screen::InBed: case Screen::GameModeSwitcher: return false;
        case Screen::Dialog: return dialogPausesGame();  // dialogs.h
        default: return true;
    }
}
}  // namespace

void Ui::State::updatePause() {
    // Minecraft.pauseIfInactive: half a second without the focus opens the pause menu (scripted runs drive their own
    // input and keep playing).
    if (window->focused() || fe->cfg.menu || window->headless()) lastActiveTime = clock;  // headless: never focused
    else if (settings.pauseOnLostFocus && script.empty() && screen == Screen::None && clock - lastActiveTime > 0.5)
        openScreen(Screen::Pause);
    // Minecraft.runTick: a singleplayer world that is not open to LAN pauses with a pause screen; the integrated server
    // stops ticking (here: the local server's tick freeze) and so does the client.
    const bool pause = fe->cfg.singleplayer && online() && fe->lanOpenPort == 0 && pausesGame(screen);
    if (pause != paused) {
        paused = pause;
        if (fe->cfg.pauseServer) fe->cfg.pauseServer(pause);
        LOG("ui: game %s", pause ? "paused" : "resumed");
    }
    game->paused = paused;
}

void Ui::State::applyControls() {  // the key bindings and control options the game reads
    Game::Keys& k = game->keys;
    k.forward = settings.key(Key::Forward), k.left = settings.key(Key::Left), k.back = settings.key(Key::Back);
    k.right = settings.key(Key::Right), k.jump = settings.key(Key::Jump), k.sneak = settings.key(Key::Sneak);
    k.sprint = settings.key(Key::Sprint), k.attack = settings.key(Key::Attack), k.use = settings.key(Key::Use);
    k.pickItem = settings.key(Key::PickItem), k.perspective = settings.key(Key::TogglePerspective);
    for (int i = 0; i < 9; ++i) k.hotbar[i] = settings.key(Key(int(Key::Hotbar1) + i));
    k.debugModifier = settings.key(Key::DebugModifier);
    game->toggleCrouch = settings.toggleCrouch, game->toggleSprint = settings.toggleSprint;
    game->toggleAttack = settings.toggleAttack, game->toggleUse = settings.toggleUse;
    game->invertMouseX = settings.invertMouseX, game->invertMouseY = settings.invertMouseY;
    game->discreteScroll = settings.discreteMouseScroll, game->wheelSensitivity = settings.mouseWheelSensitivity;
    game->player().autoJump = settings.autoJump;
    game->player().sprintWindow = settings.sprintWindow;
}

int Ui::State::gameMode() const {
    if (session) return session->gameMode();
    return 1;  // local worlds play like creative
}

int Ui::State::guiScale() const {
    const u32 w = window->width(), h = window->height();
    int scale = 1;
    const int max = settings.guiScale;
    while (scale != max && u32(scale) < w && u32(scale) < h && w / u32(scale + 1) >= 320 && h / u32(scale + 1) >= 240) ++scale;
    if (settings.forceUnicodeFont && scale % 2 != 0) ++scale;  // Window.calculateScale(enforceUnicode)
    return scale;
}

void Ui::State::openScreen(Screen s) {
    const bool wasNone = screen == Screen::None;
    if (screen == Screen::Video && s != Screen::Video) window->changeFullscreenVideoMode();  // VideoSettingsScreen.onClose
    if (screen == Screen::Chat && s != Screen::Chat) leaveChat();
    if (screen != s)
        if (const ScreenDef* d = screenDef(screen); d && d->closed) d->closed(*this);
    screen = s;
    if (s == Screen::Options || s == Screen::Title || s == Screen::None) optionsParent = Screen::Options;
    buttons.clear();
    draggingSlider = -1;
    isQuickCrafting = false;
    quickCraftSlots.clear();
    hoveredSlot = -1;
    onFilesDrop = nullptr;
    if (s == Screen::Chat) {
        chatInput.blinkStart = clock;
        historyPos = -1;
        chatScroll = 0;
    }
    buildButtons();
    if (wasNone) {
        window->setMouseCaptured(false);
        window->centerCursor();
    }
    window->setClickCaptures(false);
}

void Ui::State::leaveChat() {  // ChatScreen.removed
    const bool blank = chatInput.value.find_first_not_of(' ') == std::string::npos;
    const bool discard = chatExit != 0 && (chatExit != 1 || !settings.saveChatDrafts);  // shouldDiscardDraft
    if (discard || blank) chatDraft.clear();
    else if (!chatIsDraft) chatDraft = chatInput.value;
    chatExit = 0;
    chatIsDraft = false;
    chatScroll = 0, newMessageSinceScroll = false;  // resetChatScroll
}

void Ui::State::openChat(bool command) {
    openScreen(Screen::Chat);
    if (!chatDraft.empty() && (!command || chatDraft[0] == '/')) {  // ChatMethod.isDraftRestorable
        chatInput.value = chatDraft;
        chatIsDraft = true;
    } else {
        chatInput.value = command ? "/" : "";
    }
    chatInput.cursor = chatInput.selection = int(chatInput.value.size());
}

void Ui::State::closeScreen(bool sendClose) {
    const Screen old = screen;
    if (old == Screen::Chat) leaveChat();
    if (const ScreenDef* d = screenDef(old); d && d->closed) d->closed(*this);
    if ((old == Screen::Inventory || old == Screen::Container) && online()) {
        // Carried items go back (the server does the same on close); tell it the menu closed.
        if (sendClose) {
            net::Writer w;
            w.varInt(activeMenu().containerId);
            client->sendPlay("container_close", w);
        }
    }
    if (old == Screen::Container) mirrorPlayerSlots(), container.reset();  // the server's inventory menu takes these
    if (old == Screen::Creative && !carried.empty()) {  // dropped into the world, like closing the creative menu
        if (online()) {
            net::Writer w;
            w.u16v(u16(i16(-1)));
            items.write(w, carried, true);
            client->sendPlay("set_creative_mode_slot", w);
        }
        carried = ItemStack{};
    }
    if (!online() && !carried.empty()) {  // local: back into the first free slot
        for (int i : {36, 37, 38, 39, 40, 41, 42, 43, 44})
            if (inventory.slots[size_t(i)].empty()) {
                inventory.slots[size_t(i)] = carried;
                carried = ItemStack{};
                break;
            }
        for (int i = 9; i < 36 && !carried.empty(); ++i)
            if (inventory.slots[size_t(i)].empty()) inventory.slots[size_t(i)] = carried, carried = ItemStack{};
        carried = ItemStack{};
    }
    screen = Screen::None;
    buttons.clear();
    chatInput = TextField{};
    window->setClickCaptures(true);
    window->setMouseCaptured(true);
}

void Ui::State::sendChat(const std::string& raw) {
    std::string text = raw;
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    if (text.empty()) return;
    if (sentHistory.empty() || sentHistory.back() != text) sentHistory.push_back(text);
    if (sentHistory.size() > 100) sentHistory.erase(sentHistory.begin());
    if (text[0] == '/' && nativeMods.command(text)) return;  // a native mod's command (mods/mcpp_mod.h)
    if (settings.chatVisibility == 2 || (settings.chatVisibility == 1 && text[0] != '/')) {  // ChatAbilities
        addChat(Text(res.translate("chat.disabled.options"), colorStyle(0xFF5555)));
        return;
    }
    if (!online()) {
        if (text[0] == '/') addChat(Text("Commands need a server", colorStyle(0xFF5555)));
        else addChat(translatable("chat.type.text", {Text("Player"), Text(text)}, lang()));
        return;
    }
    if (text[0] == '/') sendChatCommand(*this, text.substr(1));  // chat_net.cpp
    else sendChatMessage(*this, text);
}

void Ui::State::syncHotbarToGame() {
    for (int i = 0; i < 10; ++i) {  // the hotbar, then the off hand (45)
        const ItemStack& s = inventory.slots[size_t(i < 9 ? 36 + i : 45)];
        const int item = s.empty() ? -1 : s.item;
        if (item == hotbarSynced[i]) continue;
        hotbarSynced[i] = item;
        u16 block = 0;
        if (item >= 0) {
            std::string name = items.name(item);
            static const std::pair<const char*, const char*> kPlaced[] = {
                {"minecraft:wheat_seeds", "minecraft:wheat"},       {"minecraft:redstone", "minecraft:redstone_wire"},
                {"minecraft:string", "minecraft:tripwire"},         {"minecraft:carrot", "minecraft:carrots"},
                {"minecraft:potato", "minecraft:potatoes"},         {"minecraft:beetroot_seeds", "minecraft:beetroots"},
                {"minecraft:melon_seeds", "minecraft:melon_stem"},  {"minecraft:pumpkin_seeds", "minecraft:pumpkin_stem"},
                {"minecraft:sweet_berries", "minecraft:sweet_berry_bush"}, {"minecraft:cocoa_beans", "minecraft:cocoa"},
                {"minecraft:glow_berries", "minecraft:cave_vines"}, {"minecraft:torchflower_seeds", "minecraft:torchflower_crop"},
                {"minecraft:pitcher_pod", "minecraft:pitcher_crop"}};
            bool mapped = false;
            for (auto [from, to] : kPlaced)
                if (name == from) name = to, mapped = true;
            if (mapped || items.isBlockItem(item)) block = registry->idFor(name);
        }
        if (i < 9) game->setHotbarBlock(i, block);
        else game->setOffhandBlock(block);
    }
}

void Ui::State::dropSelected(bool all) {
    const int slot = 36 + game->selectedSlot();
    ItemStack& s = inventory.slots[size_t(slot)];
    if (s.empty()) return;
    if (online()) {
        client->sendPlayerAction(all ? 4 : 5, 0, 0, 0, 0, 0);  // DROP_ALL_ITEMS / DROP_ITEM
    }
    s = all ? ItemStack{} : s.withCount(s.count - 1);
}

void Ui::State::swapHands() {
    std::swap(inventory.slots[size_t(36 + game->selectedSlot())], inventory.slots[45]);
    if (online()) client->sendPlayerAction(7, 0, 0, 0, 0, 0);  // SWAP_ITEM_WITH_OFFHAND
}

int Ui::State::menuSlotForInventory(const Menu& m, int idx) const {
    if (m.type < 0) {
        if (idx >= 0 && idx < 9) return 36 + idx;
        if (idx >= 9 && idx < 36) return idx;
        if (idx >= 36 && idx < 40) return 8 - (idx - 36);
        if (idx == 40) return 45;
        return -1;
    }
    const int start = layoutFor(m).playerStart;
    if (start < 0) return -1;  // no player inventory (lectern)
    if (idx >= 0 && idx < 9) return start + 27 + idx;
    if (idx >= 9 && idx < 36) return start + idx - 9;
    return -1;
}

// --- Ui -----------------------------------------------------------------------------------------------------

Ui::Ui() : s_(std::make_unique<State>()) { s_->fe = std::make_unique<FrontEndState>(); }
Ui::~Ui() = default;

Settings& Ui::settings() { return s_->settings; }
bool Ui::quitRequested() const { return s_->quit; }
void Ui::setServerSaving(bool saving) { s_->serverSaving = saving; }

bool Ui::menuOpen() const { return s_->screen != Screen::None && s_->screen != Screen::Chat; }

float Ui::fovRadians() const {  // Camera.getFov: the option scaled by the dynamic FOV modifier
    const float m = s_->oldFovModifier + (s_->fovModifier - s_->oldFovModifier) * s_->partialTick;
    // modifyFovBasedOnDeathOrFluid: x 6/7 with the eyes in water or lava.
    float fluid = 1.0f;
    if (s_->game && s_->world && s_->registry) {
        const CameraState cam = s_->game->camera();
        const u16 eye = s_->world->block(int(std::floor(cam.x)), int(std::floor(cam.y)), int(std::floor(cam.z)));
        if (s_->eyeInWater || s_->registry->name(eye).starts_with("minecraft:lava")) fluid = 0.85714287f;
    }
    return s_->settings.fov * m * fluid * kPi / 180.0f;
}
bool Ui::hudHidden() const { return s_->hudHidden; }
bool Ui::inBed() const { return s_->screen == Screen::InBed; }
void Ui::setBiomeLookup(std::function<std::string(int, int, int)> fn) { s_->biomeLookup = std::move(fn); }

const fs::path* Ui::screenshotRequest() {
    if (!s_->shotPending) return nullptr;
    s_->shotPending = false;
    return &s_->shotPath;
}

bool Ui::load(const fs::path& jar, const fs::path& minecraftDir, const GameReports& reports) {
    try {
        return loadImpl(jar, minecraftDir, reports);
    } catch (const std::exception& e) {
        LOG("error: UI resources: %s", e.what());
        return false;
    }
}

bool Ui::loadImpl(const fs::path& jar, const fs::path& minecraftDir, const GameReports& reports) {
    State& s = *s_;
    s.reports = &reports;
    Timer t;
    s.settings.load();
    if (!s.res.open(jar, minecraftDir)) return false;
    if (s.settings.lang != "en_us") s.res.setLanguage(s.settings.lang);
    s.guiAtlas.init(2048, 4096);
    s.iconAtlas.init(2048, 2048);
    if (!s.font.load(s.res, s.guiAtlas, s.settings.forceUnicodeFont, s.settings.japaneseGlyphVariants) ||
        !s.sprites.load(s.res, s.guiAtlas))
        return false;
    installFontHooks(s);
    if (!s.items.init(reports, s.res)) LOG("warning: item registry unavailable; inventories will be empty");
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA")) {  // item_data.cpp: tooltip data, default components
        const fs::path cache = fs::path(local) / "minecraftCplusplus" / "reports";
        const std::string version = jar.stem().string();
        s.items.loadGameData(minecraftDir, version, cache, cache / version / "reports" / "minecraft" / "components" / "item");
    }
    for (const char* tex : {"gui/container/inventory", "gui/container/generic_54", "gui/container/crafting_table",
                            "gui/container/furnace", "gui/container/creative_inventory/tab_item_search",
                            "gui/container/creative_inventory/tab_items", "gui/container/dispenser",
                            "gui/container/hopper", "gui/container/shulker_box", "gui/container/smoker",
                            "gui/container/blast_furnace"})
        s.sprites.texture(tex);
    s.icons.init(s.res, s.items, s.iconAtlas);
    itemWorld().keyDown = [st = &s](std::string_view name) {  // IsKeybindDown: KeyMapping.isDown by name
        for (size_t k = 0; k < kKeyCount; ++k)
            if (name == keyDef(Key(k)).name) {
                const int vk = st->settings.key(Key(k));
                return vk != 0 && st->window && st->window->keyDown(vk);
            }
        return false;
    };
    s.loaded = true;
    LOG("ui      %8.1f ms  fonts, GUI sprites and %d items from the game", t.ms(), s.items.count());
    return true;
}

bool Ui::init(const Init& in) {
    State& s = *s_;
    if (!s.loaded) return false;
    s.init = in;
    s.window = in.window, s.renderer = in.renderer, s.game = in.game, s.world = in.world;
    s.registry = in.registry, s.terrain = in.terrain;
    s.pass = std::make_unique<UiPass>(s.guiAtlas, s.iconAtlas);
    if (!in.renderer->addPass(s.pass.get(), PassStage::Hud)) return false;
    in.renderer->addPass(s.pass.get(), PassStage::AfterUi);  // the inverting crosshair, against the final image

    s.inventory.slots.assign(46, ItemStack{});
    s.inventory.title = Text(s.res.translate("container.crafting"));
    // Local worlds: start from the game's hotbar.
    for (int i = 0; i < 9; ++i) {
        const u16 b = in.game->hotbarBlock(i);
        if (!b) continue;
        std::string name = in.registry->name(b);
        name = name.substr(0, name.find('['));
        int id = s.items.id(name);
        if (id < 0)
            if (size_t w = name.find("_wall_"); w != std::string::npos) id = s.items.id(name.erase(w, 5));
        if (id >= 0) s.inventory.slots[size_t(36 + i)] = ItemStack{id, 1};
    }
    in.game->drawHud = false;
    in.game->handleEscape = false;
    in.game->onPickBlock = [this](int x, int y, int z) {
        State& st = *s_;
        const u16 block = st.world->block(x, y, z);
        if (st.online()) {
            net::Writer w;
            w.blockPos(x, y, z);
            w.boolean(false);  // include data
            st.client->sendPlay("pick_item_from_block", w);
            return;
        }
        std::string name = st.registry->name(block);
        name = name.substr(0, name.find('['));
        int id = st.items.id(name);
        if (id < 0)
            if (size_t w = name.find("_wall_"); w != std::string::npos) id = st.items.id(name.erase(w, 5));
        if (id < 0) return;
        for (int i = 0; i < 9; ++i)
            if (st.inventory.slots[size_t(36 + i)].item == id) {
                st.game->setSelectedSlot(i);
                return;
            }
        st.inventory.slots[size_t(36 + st.game->selectedSlot())] = ItemStack{id, 1};
    };
    in.game->extraLines = [this](std::vector<LineVertex>& lines, const CameraState& cam, float w, float h) {
        operatorBoxes(*s_, lines, cam, w, h);  // block_editors.cpp
        debugLines(*s_, lines, cam, w, h);     // debug_screen.cpp: chunk borders, hitboxes
    };
    in.game->onInteract = [this](int x, int y, int z) {
        State& st = *s_;
        if (!st.online()) return false;
        if (openBlockEditor(st, x, y, z)) return true;  // block_editors.cpp: command, structure, jigsaw, test blocks
        std::string n = st.registry->name(st.world->block(x, y, z));
        n = n.substr(n.find(':') + 1, n.find('[') == std::string::npos ? std::string::npos : n.find('[') - n.find(':') - 1);
        static const char* exact[] = {"barrel", "crafting_table", "crafter",
                                      "furnace", "blast_furnace", "smoker", "dispenser", "dropper", "hopper",
                                      "enchanting_table", "brewing_stand", "loom", "stonecutter", "grindstone",
                                      "smithing_table", "cartography_table", "beacon", "lectern", "lever", "repeater",
                                      "comparator", "daylight_detector", "cake", "respawn_anchor", "jukebox", "note_block",
                                      "anvil", "chipped_anvil", "damaged_anvil", "bell", "decorated_pot", "chiseled_bookshelf"};
        for (const char* e : exact)
            if (n == e) return true;
        const bool iron = n.starts_with("iron_");
        // chest, trapped / ender / copper chests; shelves; signs (the server opens the sign editor)
        return n.ends_with("chest") || n.ends_with("_shelf") || n.ends_with("_sign") ||
               n.ends_with("shulker_box") || n.ends_with("_bed") || n.ends_with("_button") || n.ends_with("_fence_gate") ||
               (!iron && (n.ends_with("_door") || n.ends_with("_trapdoor")));
    };
    // The launch detail radius sized the geometry arena: the option goes up to it.
    // (kept as saved: a launch with a smaller --detail must not overwrite the player's choice)
    if (!s.settings.loaded) s.settings.renderDistance = in.detailChunks;
    in.terrain->setDetailRadius(float(std::min(s.settings.renderDistance, in.detailChunks)) * 16.0f);
    // Minecraft: the override size for the window, the exclusive fullscreen mode, then options.txt fullscreen.
    if (s.settings.overrideWidth > 0 && s.settings.overrideHeight > 0 && !launch::placement())
        s.window->setClientSize(s.settings.overrideWidth, s.settings.overrideHeight);
    s.window->setExclusiveFullscreen(s.settings.exclusiveFullscreen);
    {
        Window::VideoMode mode;
        if (parseVideoMode(s.settings.fullscreenResolution, mode)) s.window->setPreferredVideoMode(&mode);
    }
    if (s.settings.fullscreen) s.window->setFullscreen(true);  // options.txt fullscreen
    s.initNativeMods();  // mods_screen.cpp
    frontEndInit(s);     // title.cpp: the title screen in menu mode
    return true;
}

static void resetConnection(Ui::State& s);

void Ui::registerHandlers(net::Client& client) {
    State* s = s_.get();
    resetConnection(*s);
    client.setInformation(clientInformation(s->settings));  // the login sends it
    const Lang* lang = &s->res.lang();
    const Items* items = &s->items;
    attachItemWorld(client);  // item model properties: dimension, spawn, last death (item_props.h)

    client.onPlay("set_health", [s](net::Reader& r) {
        const float health = r.f32();
        const int food = r.varInt();
        const float sat = r.f32();
        if (std::getenv("MCPP_UI_VERBOSE")) LOG("ui: set_health %.1f food %d sat %.1f", health, food, sat);
        s->post([s, health, food, sat] {
            s->health = health, s->food = food, s->saturation = sat;
            s->game->player().canSprint = food > 6 || s->creative() || s->spectator();  // hasEnoughFoodToSprint
            if (health <= 0 && s->showDeathScreen && s->screen != Screen::Death) s->openScreen(Screen::Death);
        });
    });
    client.onPlay("player_combat_kill", [s, lang](net::Reader& r) {  // DeathScreen with the cause of death
        const int id = r.varInt();
        Text message = parseComponent(r.nbt(), lang);
        s->post([s, id, message] {
            if (s->session && id != s->session->entityId()) return;
            if (!s->showDeathScreen) {  // handlePlayerCombatKill: LocalPlayer.respawn at once (doImmediateRespawn)
                if (s->client) {
                    net::Writer w;
                    w.varInt(0);  // ServerboundClientCommandPacket.Action.PERFORM_RESPAWN
                    s->client->sendPlay("client_command", w);
                }
                return;
            }
            s->deathMessage = message;
            if (s->screen != Screen::Death) s->openScreen(Screen::Death);
        });
    });
    // Knockback: the server sets the local player's motion (ClientPacketListener.handleSetEntityMotion) and adds an
    // explosion's push (handleExplosion).
    client.onPlay("set_entity_motion", [s](net::Reader& r) {
        const int id = r.varInt();
        double v[3];
        readLpVec3(r, v);
        if (!r.ok) return;
        s->post([s, id, v0 = v[0], v1 = v[1], v2 = v[2]] {
            if (!s->session || id != s->session->entityId()) return;
            Player& p = s->game->player();
            p.vel[0] = v0, p.vel[1] = v1, p.vel[2] = v2;
        });
    });
    client.onPlay("explode", [s](net::Reader& r) {
        for (int i = 0; i < 3; ++i) r.f64();  // centre
        r.f32();                              // radius
        r.i32v();                             // block count
        if (!r.boolean()) return;
        double k[3];
        for (double& c : k) c = r.f64();
        if (!r.ok) return;
        s->post([s, k0 = k[0], k1 = k[1], k2 = k[2]] {
            Player& p = s->game->player();
            p.vel[0] += k0, p.vel[1] += k1, p.vel[2] += k2;
        });
    });
    client.onPlay("set_experience", [s](net::Reader& r) {
        const float progress = r.f32();
        const int level = r.varInt(), total = r.varInt();
        s->post([s, progress, level, total] {
            if (progress != s->xpProgress)  // setExperienceDisplayStartTickToTickCount (not for the first values)
                s->xpDisplayStart = s->xpDisplayStart == INT_MIN ? INT_MIN + 1 : s->guiTicks;
            s->xpProgress = progress, s->xpLevel = level, s->xpTotal = total;
        });
    });
    client.onPlayPeek("set_entity_data", [s](net::Reader& r) {  // our shared flags (0), air supply (1), ticks frozen (7)
        const int entity = r.varInt();
        int air = -1, frozen = -1, flags = -1;
        for (int guard = 0; guard < 8 && r.ok; ++guard) {
            const int index = r.u8v();
            if (index == 0xFF) break;
            const int type = r.varInt();
            if (type == 0) {  // byte
                const int v = r.u8v();
                if (index == 0) flags = v;
            } else if (type == 1) {
                const int v = r.varInt();
                if (index == 1) air = v;
                if (index == 7) frozen = v;
            } else if (type == 3) r.f32();
            else if (type == 8) r.boolean();
            else if (type == 20) r.varInt();  // pose
            else break;
        }
        if (air >= 0 || frozen >= 0 || flags >= 0) s->post([s, entity, air, frozen, flags] {
            if (!s->session || entity != s->session->entityId()) return;
            if (air >= 0) s->air = air, s->serverAir = true;
            if (frozen >= 0) s->ticksFrozen = frozen;
            if (flags >= 0) s->onFire = (flags & 1) != 0;
        });
    });
    // Registry ids resolved once (the handlers run for every entity on the network thread).
    auto registryNames = [s](const char* key) {
        std::vector<std::string> names;
        const nlohmann::json& reg = s->reports->registries();
        if (reg.contains(key))
            for (const auto& e : reg[key]["entries"].items()) {
                const int id = e.value().value("protocol_id", -1);
                if (id < 0) continue;
                if (size_t(id) >= names.size()) names.resize(size_t(id) + 1);
                names[size_t(id)] = shortName(e.key());
            }
        return names;
    };
    const std::vector<std::string> attributes = registryNames("minecraft:attribute");
    const std::vector<std::string> mobEffects = registryNames("minecraft:mob_effect");
    client.onPlayPeek("update_attributes", [s, attributes](net::Reader& r) {  // also handled by the entities
        const int entity = r.varInt();
        const i32 n = r.varInt();
        float armor = -1, maxHealth = -1;
        for (i32 i = 0; i < n && r.ok; ++i) {
            const int id = r.varInt();
            const double base = r.f64();
            double add = 0, mulBase = 0, mulTotal = 1;
            const i32 mods = r.varInt();
            for (i32 m = 0; m < mods && r.ok; ++m) {
                r.str();
                const double amount = r.f64();
                const int op = r.varInt();
                if (op == 0) add += amount;
                else if (op == 1) mulBase += amount;
                else mulTotal *= 1 + amount;
            }
            const double v = base + add, value = (v + v * mulBase) * mulTotal;
            const std::string& name = id >= 0 && size_t(id) < attributes.size() ? attributes[size_t(id)] : std::string();
            if (name == "armor") armor = float(std::clamp(value, 0.0, 30.0));
            if (name == "max_health") maxHealth = float(value);
        }
        if (armor < 0 && maxHealth < 0) return;
        s->post([s, entity, armor, maxHealth] {
            if (s->session && entity != s->session->entityId()) return;
            if (armor >= 0) s->armor = armor;
            if (maxHealth >= 0) s->maxHealth = maxHealth;
        });
    });
    // ClientboundLoginPacket: hardcore, then after the level list, max players and distances, the player's flags.
    client.onPlayPeek("login", [s](net::Reader& r) {
        r.i32v();
        const bool hardcore = r.boolean();
        const i32 levels = r.varInt();
        for (i32 i = 0; i < levels && r.ok; ++i) r.str();
        r.varInt(), r.varInt(), r.varInt();  // max players, chunk radius, simulation distance
        const bool reduced = r.boolean(), deathScreen = r.boolean(), limited = r.boolean();
        if (r.ok) s->post([s, hardcore, reduced, deathScreen, limited] {
            s->hardcore = hardcore, s->reducedDebugInfo = reduced, s->showDeathScreen = deathScreen, s->limitedCrafting = limited;
            s->difficulty = 2, s->difficultyLocked = false;  // ClientLevelData starts at NORMAL; the server sends its own
        });
    });
    client.onPlayPeek("change_difficulty", [s](net::Reader& r) {  // ClientboundChangeDifficultyPacket
        const int difficulty = r.varInt();
        const bool locked = r.boolean();
        if (r.ok) s->post([s, difficulty, locked] { s->difficulty = difficulty, s->difficultyLocked = locked; });
    });
    client.onPlayPeek("game_event", [s](net::Reader& r) {  // IMMEDIATE_RESPAWN (11), LIMITED_CRAFTING (12)
        const int event = r.u8v();
        const float param = r.f32();
        if (!r.ok || (event != 11 && event != 12)) return;
        s->post([s, event, param] {
            if (event == 11) s->showDeathScreen = param == 0.0f;
            else s->limitedCrafting = param == 1.0f;
        });
    });
    client.onPlayPeek("entity_event", [s](net::Reader& r) {  // EntityEvent.REDUCED_DEBUG_INFO (22) / FULL_DEBUG_INFO (23)
        const int id = r.i32v(), event = r.u8v();
        if (r.ok && (event == 22 || event == 23)) s->post([s, id, event] {
            if (s->session && id == s->session->entityId()) s->reducedDebugInfo = event == 22;
        });
    });
    client.onPlayPeek("respawn", [s](net::Reader&) {  // a new player entity: no effects, no open screens
        s->post([s] {
            s->effects.clear();
            s->onFire = false;
            s->air = s->maxAir;
            if (s->screen == Screen::Death || s->screen == Screen::Container || s->screen == Screen::Inventory)
                s->closeScreen(false);
        });
    });
    client.onPlayPeek("update_mob_effect", [s, mobEffects](net::Reader& r) {
        const int entity = r.varInt(), id = r.varInt(), amp = r.varInt(), dur = r.varInt(), flags = r.u8v();
        const std::string name = id >= 0 && size_t(id) < mobEffects.size() ? mobEffects[size_t(id)] : std::string();
        s->post([s, entity, id, amp, dur, flags, name] {
            if (!s->session || entity != s->session->entityId()) return;
            State::Effect e{name, amp, dur, (flags & 1) != 0, (flags & 4) != 0};
            if (auto it = s->effects.find(id); it != s->effects.end()) e.blend = it->second.blend, e.blendO = it->second.blendO;
            if (!(flags & 8)) e.blend = e.blendO = -1;  // no FLAG_BLEND: skipBlending (set on the next tick)
            s->effects[id] = e;
        });
    });
    // ClientboundPostEffectsPacket: LocalPlayer.setActivePostEffects (drawn by render/post_effects.cpp).
    client.onPlay("post_effects", [s](net::Reader& r) {
        std::vector<std::string> ids;
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok && i < 64; ++i) ids.emplace_back(r.str());
        if (r.ok) s->post([s, ids] { s->postEffects = ids; });
    });
    client.onPlayPeek("remove_mob_effect", [s](net::Reader& r) {
        const int entity = r.varInt(), id = r.varInt();
        s->post([s, entity, id] {
            if (s->session && entity == s->session->entityId()) s->effects.erase(id);
        });
    });
    client.onPlay("container_set_content", [s, items](net::Reader& r) {
        const int id = r.varInt(), state = r.varInt();
        const i32 n = r.varInt();
        std::vector<ItemStack> slots;
        bool ok = true;
        for (i32 i = 0; i < n && ok; ++i) {
            ItemStack st;
            ok = items->read(r, st);
            slots.push_back(std::move(st));
        }
        ItemStack carried;
        if (ok) items->read(r, carried);
        s->post([s, id, state, slots = std::move(slots), carried, ok] {
            Menu* m = id == 0 ? &s->inventory : s->container && s->container->containerId == id ? s->container.get() : nullptr;
            if (!m) return;
            m->stateId = state;
            if (m->slots.size() < slots.size()) m->slots.resize(slots.size());
            for (size_t i = 0; i < slots.size(); ++i) m->slots[i] = slots[i];
            if (ok) s->carried = carried;
            s->mirrorPlayerSlots(id == 0);  // one Inventory behind both menus
        });
    });
    client.onPlay("container_set_slot", [s, items](net::Reader& r) {
        const int id = r.varInt(), state = r.varInt(), slot = r.i16v();
        ItemStack st;
        items->read(r, st);
        if (std::getenv("MCPP_UI_VERBOSE")) LOG("ui: container_set_slot %d #%d = %d x %d", id, slot, st.count, st.item);
        s->post([s, id, state, slot, st] {
            Menu* m = id == 0 ? &s->inventory : s->container && s->container->containerId == id ? s->container.get() : nullptr;
            if (!m || slot < 0) return;
            m->stateId = state;
            if (size_t(slot) >= m->slots.size()) m->slots.resize(size_t(slot) + 1);
            m->slots[size_t(slot)] = st;
            s->mirrorPlayerSlots(id == 0);  // one Inventory behind both menus
        });
    });
    client.onPlay("container_set_data", [s](net::Reader& r) {
        const int id = r.varInt(), prop = r.i16v(), value = r.i16v();
        s->post([s, id, prop, value] {
            if (s->container && s->container->containerId == id && prop >= 0 && prop < 16) s->container->data[size_t(prop)] = value;
        });
    });
    client.onPlay("container_close", [s](net::Reader& r) {
        const int id = r.varInt();
        s->post([s, id] {
            if ((s->screen == Screen::Container && s->container && s->container->containerId == id) ||
                (s->screen == Screen::Inventory && id == 0))
                s->closeScreen(false);
        });
    });
    client.onPlay("open_screen", [s, lang](net::Reader& r) {
        auto m = std::make_shared<Menu>();
        m->containerId = r.varInt();
        m->type = r.varInt();
        m->title = parseComponent(r.nbt(), lang);
        const nlohmann::json& reg = s->reports->registries();
        if (reg.contains("minecraft:menu"))
            for (const auto& e : reg["minecraft:menu"]["entries"].items())
                if (e.value().value("protocol_id", -1) == m->type) m->typeName = shortName(e.key());
        s->post([s, m] {
            if (s->screen != Screen::None && s->screen != Screen::Container) s->closeScreen(false);
            s->container = std::make_unique<Menu>(*m);
            const Layout l = layoutFor(*s->container);
            s->container->slots.assign(l.slots.size(), ItemStack{});
            s->openScreen(Screen::Container);
            if (const StationDef* d = stationFor(s->container->typeName); d && d->opened) d->opened(*s, *s->container);
        });
    });
    client.onPlay("set_cursor_item", [s, items](net::Reader& r) {
        ItemStack st;
        items->read(r, st);
        s->post([s, st] { s->carried = st; });
    });
    client.onPlay("set_player_inventory", [s, items](net::Reader& r) {
        const int slot = r.varInt();
        ItemStack st;
        items->read(r, st);
        if (std::getenv("MCPP_UI_VERBOSE")) LOG("ui: set_player_inventory #%d = %d x %d", slot, st.count, st.item);
        s->post([s, slot, st] {
            const int ms = s->menuSlotForInventory(s->inventory, slot);
            if (ms >= 0) s->inventory.slots[size_t(ms)] = st;
            if (s->container) {
                const int cs = s->menuSlotForInventory(*s->container, slot);
                if (cs >= 0 && size_t(cs) < s->container->slots.size()) s->container->slots[size_t(cs)] = st;
            }
        });
    });
    client.onPlay("set_held_slot", [s](net::Reader& r) {
        const int slot = r.varInt();
        s->post([s, slot] {
            if (slot >= 0 && slot < 9) s->game->setSelectedSlot(slot);
        });
    });
    // Chat packets: chat_net.cpp.
    client.onPlay("set_title_text", [s, lang](net::Reader& r) {
        Text t = parseComponent(r.nbt(), lang);
        s->post([s, t] {
            s->title = t, s->hasTitle = true;
            s->titleTime = s->titleFadeIn + s->titleStay + s->titleFadeOut;
        });
    });
    client.onPlay("set_subtitle_text", [s, lang](net::Reader& r) {
        Text t = parseComponent(r.nbt(), lang);
        s->post([s, t] { s->subtitle = t, s->hasSubtitle = true; });
    });
    client.onPlay("set_action_bar_text", [s, lang](net::Reader& r) {
        Text t = parseComponent(r.nbt(), lang);
        s->post([s, t] { s->actionbar = t, s->actionbarTime = 60, s->actionbarAnimated = false; });
    });
    client.onPlayPeek("level_event", [s](net::Reader& r) {  // 1010: Hud.setNowPlaying(the song's description)
        const int type = r.i32v();
        r.u64v();
        const int data = r.i32v();
        if (!r.ok || type != 1010) return;
        s->post([s, data] {
            const auto& songs = s->items.registry("minecraft:jukebox_song");
            if (data < 0 || size_t(data) >= songs.size()) return;
            s->actionbar = translatable("record.nowPlaying", {songs[size_t(data)].description}, s->lang());
            s->actionbarTime = 60, s->actionbarAnimated = true;
        });
    });
    client.onPlay("set_titles_animation", [s](net::Reader& r) {
        const int in = r.i32v(), stay = r.i32v(), out = r.i32v();
        s->post([s, in, stay, out] {
            if (in >= 0) s->titleFadeIn = in;
            if (stay >= 0) s->titleStay = stay;
            if (out >= 0) s->titleFadeOut = out;
            if (s->titleTime > 0) s->titleTime = s->titleFadeIn + s->titleStay + s->titleFadeOut;
        });
    });
    client.onPlay("clear_titles", [s](net::Reader& r) {
        const bool reset = r.boolean();
        s->post([s, reset] {
            s->hasTitle = s->hasSubtitle = false;
            s->titleTime = 0;
            if (reset) s->titleFadeIn = 10, s->titleStay = 70, s->titleFadeOut = 20;
        });
    });
    for (const UiModule& m : uiModules())  // modules.h
        if (m.registerHandlers) m.registerHandlers(*s, client);
}

// Per-connection state starts over: when a new client's handlers are registered (before any of its packets is
// applied: the join completes on a worker and its first packets can be applied before attachSession) and when the
// session goes away.
static void resetConnection(Ui::State& s) {
    s.inventory.slots.assign(46, ItemStack{});
    s.effects.clear();
    std::fill(std::begin(s.hotbarSynced), std::end(s.hotbarSynced), -2);
    s.carried = ItemStack{};
    s.container.reset();
    for (const UiModule& m : uiModules())
        if (m.attached) m.attached(s);
}

void Ui::attachSession(net::Client* client, net::Session* session) {
    State& s = *s_;
    s.client = client;
    s.session = session;
    if (session) session->creativeHotbarSync = false;  // the inventory is server-driven now
    if (s.screen != Screen::None && s.screen != Screen::Chat) s.closeScreen(false);
    if (!session) return resetConnection(s);
    // Minecraft.startUseItem reached this hand's item use (Game::startUseItem decides the hands' turns from the
    // gameplay layer's predicted results): ServerboundUseItemPacket; a book and quill opens client side.
    s.game->onUseItem = [&s, session](int hand) {
        openWritableBook(s, hand);  // signs_books.cpp
        session->useItem(hand);
    };
}

void Ui::setEntities(ent::EntityWorld* entities) {
    s_->entities = entities;
    if (!entities) return;
    State* s = s_.get();
    entities->onUse = [s](const ent::Entity& e) {  // MinecartCommandBlock.interact: the editor on the client
        if (!e.info || e.info->name != "minecraft:command_block_minecart") return;
        openMinecartCommandEditor(*s, e.id, e.minecartCommand, parseComponent(e.minecartLastOutput, s->lang()));
    };
}
void Ui::setItemModels(ent::ItemModels* models) {
    s_->itemModels = models;
    s_->icons.setItemModels(models);
}

int Ui::ticksFrozen() const { return s_->ticksFrozen; }
int Ui::spectatorEffectToggles() const { return s_->spectatorEffectToggles; }
std::vector<std::string> Ui::postEffects() const { return s_->postEffects; }
void Ui::setEngineInfo(std::function<std::string(std::string_view)> fn) { s_->engineInfo = std::move(fn); }

const Items& Ui::items() const { return s_->items; }

const ItemStack* Ui::playerSlot(int slot) const {
    const auto& slots = s_->inventory.slots;
    if (slot < 0 || size_t(slot) >= slots.size() || slots[size_t(slot)].empty()) return nullptr;
    return &slots[size_t(slot)];
}

bool Ui::loadScript(const fs::path& file) {
    std::ifstream f(file);
    if (!f) {
        LOG("ui: cannot read script %s", file.string().c_str());
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        State::ScriptStep st;
        in >> st.time >> st.verb;
        std::string rest;
        std::getline(in, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        if (st.verb == "type" || st.verb == "chat") {
            st.args.push_back(rest);
        } else {
            std::istringstream a(rest);
            std::string w;
            while (a >> w) st.args.push_back(w);
        }
        s_->script.push_back(std::move(st));
    }
    std::stable_sort(s_->script.begin(), s_->script.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    LOG("ui: script %s, %zu steps", file.string().c_str(), s_->script.size());
    return true;
}

// Scripted input for automated tests. Times are seconds since the first frame.
void Ui::State::runScript(double) {
    using E = Window::InputEvent;
    if (scriptShiftHeld) {
        window->inject({E::Key, VK_SHIFT, false});
        scriptShiftHeld = false;
    }
    auto toPixels = [&](float gx, float gy, int& px, int& py) {
        const int sc = guiScale();
        px = int(gx * float(sc) + float(sc) / 2), py = int(gy * float(sc) + float(sc) / 2);
    };
    while (scriptPos < script.size() && script[scriptPos].time <= clock) {
        const ScriptStep& st = script[scriptPos++];
        const auto& a = st.args;
        auto arg = [&](size_t i) { return i < a.size() ? a[i] : std::string(); };
        if (st.verb == "key") {  // key <name> [down|up|press]
            const int vk = vkFromName(arg(0));
            const std::string mode = a.size() > 1 ? arg(1) : "press";
            if (mode != "up") window->inject({E::Key, vk, true});
            if (mode != "down") window->inject({E::Key, vk, false});
        } else if (st.verb == "type") {
            for (size_t i = 0; i < arg(0).size();) window->inject({E::Char, int(nextCodepoint(arg(0), i)), true});
        } else if (st.verb == "chat") {  // opens chat, types, sends
            openScreen(Screen::Chat);
            chatInput.value = arg(0);
            sendChat(chatInput.value);
            closeScreen();
        } else if (st.verb == "move" || st.verb == "click" || st.verb == "release") {  // GUI coordinates
            int px, py;
            toPixels(float(std::atof(arg(0).c_str())), float(std::atof(arg(1).c_str())), px, py);
            window->setScriptedCursor(px, py);
            const std::string b = arg(2) == "down" ? arg(3) : arg(2);  // move x y down [right|middle]: hold
            const int button = b == "right" ? 1 : b == "middle" ? 2 : 0;
            if (st.verb == "click") window->inject({E::Button, button, true}), window->inject({E::Button, button, false});
            if (st.verb == "release") window->inject({E::Button, button, false});
            if (st.verb == "move" && arg(2) == "down") window->inject({E::Button, button, true});
        } else if (st.verb == "slot") {  // slot <index> [left|right] [shift]: click a slot of the open menu
            const Menu& m = activeMenu();
            const Layout l = layoutFor(m);
            const int idx = std::atoi(arg(0).c_str());
            if (idx >= 0 && size_t(idx) < l.slots.size()) {
                const float left = float(recipeBookLeft(*this, m, l.width)), top = float((guiH() - l.height) / 2);
                int px, py;
                toPixels(left + float(l.slots[size_t(idx)].x) + 8, top + float(l.slots[size_t(idx)].y) + 8, px, py);
                window->setScriptedCursor(px, py);
                const bool shift = arg(1) == "shift" || arg(2) == "shift";
                const int button = arg(1) == "right" ? 1 : 0;
                if (shift) window->inject({E::Key, VK_SHIFT, true}), scriptShiftHeld = true;  // released next frame
                window->inject({E::Button, button, true});
                window->inject({E::Button, button, false});
            }
        } else if (st.verb == "wheel") {
            window->inject({E::Wheel, std::atoi(arg(0).c_str()), false});
        } else if (st.verb == "look") {  // look <yaw> <pitch> (degrees)
            game->player().yaw = float(std::atof(arg(0).c_str())) * kPi / 180.0f;
            game->player().pitch = float(std::atof(arg(1).c_str())) * kPi / 180.0f;
        } else if (st.verb == "select") {
            game->setSelectedSlot(std::clamp(std::atoi(arg(0).c_str()), 0, 8));
        } else if (st.verb == "use") {  // place (or break) at the crosshair
            game->retarget();
            game->useTarget(arg(0) != "break");
        } else if (st.verb == "useentity") {  // right click on the entity under the crosshair
            if (game->onEntityClick) game->onEntityClick(false, true, -1.0);
        } else if (st.verb == "useitem") {  // right click into the air: use the held item
            game->startUseItem(false);
        } else if (st.verb == "attack") {  // left click on the entity under the crosshair
            if (game->gameMode == 3 && game->onSpectatorAttack) game->onSpectatorAttack(-1.0);
            else if (game->onEntityClick) game->onEntityClick(true, false, -1.0);
        } else if (st.verb == "shot") {
            shotPath = arg(0);
            shotPending = true;
        } else if (st.verb == "log") {
            std::string msg;
            for (const auto& w : a) msg += w + " ";
            const Player& p = game->player();
            LOG("script: %s| pos %.3f %.3f %.3f ground %d health %.1f food %d sneak %d slot %d", msg.c_str(), p.pos[0], p.pos[1],
                p.pos[2], int(p.onGround), health, food, int(p.lastInput.sneak), game->selectedSlot());
        } else if (st.verb == "block") {  // block x y z: logs the block state there
            const int x = std::atoi(arg(0).c_str()), y = std::atoi(arg(1).c_str()), z = std::atoi(arg(2).c_str());
            LOG("script: block %d %d %d = %s", x, y, z, registry->name(world->block(x, y, z)).c_str());
        } else if (st.verb == "trace") {  // trace on|off: the player's state every tick
            game->traceTicks = arg(0) != "off";
        } else if (st.verb == "dump") {  // logs the open menu's slots
            const Menu& m = activeMenu();
            for (size_t i = 0; i < m.slots.size(); ++i)
                if (!m.slots[i].empty())
                    LOG("script: slot %zu = %d x %s%s%s ench %d", i, m.slots[i].count, items.name(m.slots[i].item).c_str(),
                        m.slots[i].hasCustomName ? " named " : "", m.slots[i].hasCustomName ? m.slots[i].customName.plain().c_str() : "",
                        m.slots[i].enchantments);
            if (!carried.empty()) LOG("script: carried = %d x %s", carried.count, items.name(carried.item).c_str());
            LOG("script: xp level %d, menu %s data %d %d %d %d", xpLevel, m.typeName.c_str(), m.data[0], m.data[1], m.data[2], m.data[3]);
        } else if (st.verb == "selfstuck") {  // selfstuck <arrows> <stingers> <left parrot> <right parrot>: our avatar's layers
            if (entities) {
                entities->self.arrowCount = std::atoi(arg(0).c_str()), entities->self.stingerCount = std::atoi(arg(1).c_str());
                entities->self.shoulderParrot[0] = a.size() > 2 ? std::atoi(arg(2).c_str()) : -1;
                entities->self.shoulderParrot[1] = a.size() > 3 ? std::atoi(arg(3).c_str()) : -1;
            }
        } else if (st.verb == "dragondeath") {  // dragondeath <ticks>: client-side dragon death time (render tests)
            if (entities)
                for (auto& [id, e] : entities->entitiesMutable())
                    if (e.info && e.info->name == "minecraft:ender_dragon") e.sp.t[1] = std::atoi(arg(0).c_str()), e.health = 0;
        } else if (st.verb == "mobappearance") {  // the elder guardian curse's appearance, as from game event 10
            if (entities) entities->mobAppearances.push_back(nowSeconds());
        } else if (st.verb == "drop") {  // drop <path>...: files dropped on the window
            for (const auto& w : a) window->injectDrop(std::filesystem::path(w).wstring());
        } else if (st.verb == "screen") {  // screen <name>: opens a menu screen directly
            static const std::pair<const char*, Screen> kScreens[] = {
                {"pause", Screen::Pause},         {"options", Screen::Options},     {"online", Screen::Online},
                {"skin", Screen::SkinCustomization}, {"sounds", Screen::Sounds},    {"video", Screen::Video},
                {"videoextra", Screen::VideoExtra}, {"social", Screen::SocialInteractions}, {"controls", Screen::Controls}, {"mouse", Screen::MouseSettings},
                {"keybinds", Screen::KeyBinds},   {"accessibility", Screen::Accessibility}, {"none", Screen::None},
                {"chatoptions", Screen::ChatOptions}, {"language", Screen::Language}, {"font", Screen::FontOptions},
                {"onboarding", Screen::AccessibilityOnboarding}, {"chat", Screen::Chat}, {"credits", Screen::CreditsAttribution},
                {"worldoptions", Screen::WorldOptions}, {"gamerules", Screen::InWorldGameRules}, {"restrictions", Screen::Restrictions}, {"mods", Screen::Mods}, {"mpwarning", Screen::MultiplayerWarning}};
            for (auto [n, sc] : kScreens)
                if (arg(0) == n) sc == Screen::None ? closeScreen() : openScreen(sc);
        } else if (st.verb == "option") {  // option <options.txt key> <value>: sets it as the options screens would
            if (settings.loadExtra(arg(0), arg(1))) {
                settings.save();
                if (arg(0) == "forceUnicodeFont" || arg(0) == "japaneseGlyphVariants") reloadFonts(*this);
                if (arg(0).starts_with("chat") || arg(0) == "textBackgroundOpacity") chatRescale(*this);
                if (arg(0) == "lang") setLanguage(*this, arg(1));
                if (screen != Screen::None) buildButtons();
            }
            LOG("script: option %s = %s", arg(0).c_str(), arg(1).c_str());
        } else if (st.verb == "fullscreen") {
            toggleFullscreen();
        } else if (st.verb == "quit") {
            quit = true;
        }
    }
}

void Ui::beginFrame(double dt) {
    State& s = *s_;
    s.clock += dt;
    itemWorld().shiftDown = s.window && s.window->keyDown(VK_SHIFT);
    {
        std::vector<std::function<void()>> pending;
        {
            std::lock_guard lock(s.mutex);
            pending.swap(s.pending);
        }
        for (auto& fn : pending) fn();
    }
    s.frontEndFrame();  // title.cpp
    for (const UiModule& m : uiModules())  // modules.h
        if (m.frame) m.frame(s);
    if (s.game) s.game->bobView = s.settings.bobView;  // the View Bobbing option
    if (s.game) s.game->damageTilt = float(s.settings.damageTiltStrength), s.game->highContrastOutline = s.settings.highContrastBlockOutline;
    if (s.window) s.window->setQuitShortcuts(s.settings.quitShortcuts);
    if (audio::AudioSystem* a = audio::instance()) a->setMusicFrequency(s.settings.musicFrequency);
    // The server's data-driven registries (enchantment / painting / instrument names, creative contents).
    if (s.client && s.client->inPlay() && !s.items.registriesSet()) s.items.setRegistries(*s.client);
    if (s.session && !s.session->connected() && !s.quit) {
        // Connection lost: nothing more will arrive; keep the UI usable for quitting.
    }
    if (!s.script.empty()) s.runScript(dt);

    // HUD ticks at 20 Hz like the game's GUI.
    s.tickClock += std::min(dt, 0.25);
    while (s.tickClock >= kTick) {
        s.tickClock -= kTick;
        s.tickHud();
    }
    s.partialTick = float(s.tickClock / kTick);
    s.nativeMods.frame(dt);  // native mods' on_tick at 20 Hz
    s.frameMs = dt * 1000.0;
    s.frameTimes.push_back(float(dt));
    if (s.frameTimes.size() > 120) s.frameTimes.pop_front();
    {
        float sum = 0;
        for (float f : s.frameTimes) sum += f;
        s.fps = sum > 0 ? double(s.frameTimes.size()) / double(sum) : 0;
    }

    // Input.
    const int scale = s.guiScale();
    int cx, cy;
    s.window->cursorPos(cx, cy);
    s.mouseX = float(cx) / float(scale), s.mouseY = float(cy) / float(scale);
    bool skipChar = false;
    // MouseHandler.onDrop: files dropped on the window go to the open screen (Screen.onFilesDrop).
    if (std::vector<std::wstring> dropped = s.window->takeDroppedFiles(); !dropped.empty() && s.screen != Screen::None && s.onFilesDrop) {
        std::vector<std::filesystem::path> files(dropped.begin(), dropped.end());
        s.onFilesDrop(files);
    }
    for (const Window::InputEvent& e : s.window->takeInput()) {
        if (e.type == Window::InputEvent::Button || e.type == Window::InputEvent::Wheel)
            s.mouseX = float(e.x) / float(scale), s.mouseY = float(e.y) / float(scale);
        if (e.type == Window::InputEvent::Button) {
            if (e.code == 0) s.leftDown = e.down;
            if (e.code == 1) s.rightDown = e.down;
        }
        if (e.type == Window::InputEvent::Char && skipChar) {
            skipChar = false;
            continue;
        }
        if (std::getenv("MCPP_UI_VERBOSE") && e.type == Window::InputEvent::Key && e.down)
            LOG("ui: key %d, screen %d", e.code, int(s.screen));
        // KeyboardHandler: the fullscreen key works on every screen (not while a key is being bound).
        if (s.isKey(Key::Fullscreen, e) && !e.repeat && !s.keyCaptureActive) {
            s.toggleFullscreen();
            continue;
        }
        // Ctrl+B: the narrator hotkey (not while typing in a text field or binding a key).
        if (!s.keyCaptureActive && s.screen != Screen::Chat && s.screen != Screen::Language && narrator::hotkey(s, e)) continue;
        // Minecraft.handleGlobalKeyPress: the friends key (not with F3 held or while a text field has focus).
        if (s.isKey(Key::Friends, e) && !e.repeat && !s.keyCaptureActive && s.fe->focus < 0 && s.screen != Screen::Chat &&
            !(s.settings.key(Key::DebugModifier) && s.window->keyDown(s.settings.key(Key::DebugModifier))) && toggleFriendsScreen(s))
            continue;
        if (s.screen != Screen::None) {
            s.screenInput(e);
            continue;
        }
        bool consumed = false;  // modules.h: keys of registered features (player list, advancements...)
        for (const UiModule& m : uiModules())
            if (!consumed && m.worldKey) consumed = m.worldKey(s, e);
        if (consumed) continue;
        if ((e.type != Window::InputEvent::Key && e.type != Window::InputEvent::Button) || !e.down || e.repeat) continue;
        // Minecraft.handleKeybinds / KeyboardHandler with the bound keys (Escape always pauses).
        if (e.type == Window::InputEvent::Key && e.code == VK_ESCAPE) {
            s.openScreen(Screen::Pause);
        } else if (s.isKey(Key::ToggleGui, e)) {
            s.hudHidden = !s.hudHidden;
        } else if (s.isKey(Key::Screenshot, e)) {  // %APPDATA%\minecraftCplusplus\screenshots\<date>.png (game: .minecraft)
            const wchar_t* appdata = _wgetenv(L"APPDATA");
            const fs::path dir = fs::path(appdata ? appdata : L".") / L"minecraftCplusplus" / L"screenshots";
            std::error_code ec;
            fs::create_directories(dir, ec);
            SYSTEMTIME t;
            GetLocalTime(&t);
            char name[64];
            std::snprintf(name, sizeof(name), "%04u-%02u-%02u_%02u.%02u.%02u.png", t.wYear, t.wMonth, t.wDay, t.wHour,
                          t.wMinute, t.wSecond);
            s.shotPath = dir / name;
            s.shotPending = true;
            Style link;
            link.underlined = 1;
            s.addChat(translatable("screenshot.success", {Text(name, link)}, s.lang()));
        } else if (s.isKey(Key::Inventory, e)) {
            s.openScreen(s.creative() ? Screen::Creative : Screen::Inventory);
        } else if (s.isKey(Key::Chat, e) || s.isKey(Key::Command, e)) {
            s.openChat(!s.isKey(Key::Chat, e));
            skipChar = e.type == Window::InputEvent::Key;
        } else if (s.isKey(Key::Drop, e)) {
            if (std::getenv("MCPP_UI_VERBOSE")) LOG("ui: drop slot %d", s.game->selectedSlot());
            s.dropSelected(s.window->keyDown(VK_CONTROL));
        } else if (s.isKey(Key::SwapOffhand, e)) {
            s.swapHands();
        } else if (s.isKey(Key::SmoothCamera, e)) {  // Options.smoothCamera (not saved)
            s.game->smoothCamera = !s.game->smoothCamera;
        }
    }
    s.game->inputSuspended = s.screen != Screen::None;
    s.game->sensitivity = s.settings.radiansPerPixel();
    s.applyControls();
    s.updatePause();

    // The open container's player slots are the player's inventory (one Inventory in the game).
    if (s.container) {
        const int start = layoutFor(*s.container).playerStart;
        for (int i = 0; start >= 0 && i < 36 && size_t(start + i) < s.container->slots.size(); ++i)
            s.inventory.slots[size_t(i < 27 ? 9 + i : 36 + i - 27)] = s.container->slots[size_t(start + i)];
    }
    s.syncHotbarToGame();
}

void Ui::endFrame() {
    State& s = *s_;
    Canvas& c = s.pass->canvas();
    s.canvas = &c;
    const int scale = s.guiScale();
    s.icons.setScale(scale);
    c.begin(s.renderer->width(), s.renderer->height(), scale);
    if ((c.guiWidth() != s.lastGuiW || c.guiHeight() != s.lastGuiH) && !s.buttons.empty()) s.buildButtons();  // resized
    s.lastGuiW = c.guiWidth(), s.lastGuiH = c.guiHeight();
    if (const Sprite* g = s.sprites.texture("misc/enchanted_glint_item")) {  // ItemRenderer glint scrolling
        const double ms = s.clock * 1000.0 * 8.0 * s.settings.glintSpeed;  // TextureTransform.setupGlintTexturing
        s.pass->glint[0] = g->u0, s.pass->glint[1] = g->v0, s.pass->glint[2] = g->u1 - g->u0, s.pass->glint[3] = g->v1 - g->v0;
        s.pass->glint[4] = -float(std::fmod(ms, 110000.0) / 110000.0), s.pass->glint[5] = float(std::fmod(ms, 30000.0) / 30000.0);
        s.pass->glint[6] = float(16 * scale) * 2.0f;
    }
    s.sprites.tick(u64(s.guiTicks));  // animated GUI sprites and textures, item icons
    s.icons.tick(u64(s.guiTicks));
    if (!s.fe->cfg.menu) drawScreenEffects(s, c);  // view_effects.cpp: in the 3D view, with or without the HUD
    const bool hud = !s.hudHidden && !s.fe->cfg.menu;  // the title screen has no HUD
    if (hud) s.drawHud(c);
    else if (!s.fe->cfg.menu) drawSleepOverlay(s, c);  // Hud.extractSleepOverlay: also with the HUD hidden
    if (hud) s.drawModHud(c);  // native mods
    drawDebugScreen(s, c);  // debug_screen.cpp (always-on entries show without F3)
    if (s.screen != Screen::None) s.drawScreen(c);
    if (!s.fe->cfg.menu) s.drawSavingIndicator(c);
    narrator::frame(s);  // the widget under the mouse, when the narrator narrates the system
    for (const UiModule& m : uiModules())  // modules.h: toasts
        if (m.drawTop) m.drawTop(s, c);
}

}  // namespace ui
