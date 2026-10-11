#include "ui/ui.h"

#include "assets/assets.h"
#include "assets/game_reports.h"
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

std::string shortName(const std::string& n) { return n.substr(n.find(':') + 1); }

// Vanilla chat types in registry order (the server sends minecraft:chat_type sorted by id).
struct ChatType {
    const char* key;
    int params;  // 2: sender, content; 3: target, sender, content; -2: target, content
    bool grayItalic;
};
const ChatType kChatTypes[] = {{"chat.type.text", 2, false},
                               {"chat.type.emote", 2, false},
                               {"commands.message.display.incoming", 2, true},
                               {"commands.message.display.outgoing", -2, true},
                               {"chat.type.announcement", 2, false},
                               {"chat.type.team.text", 3, false},
                               {"chat.type.team.sent", 3, false}};

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
        {"HOME", VK_HOME},  {"END", VK_END},       {"SLASH", VK_OEM_2}};
    for (auto [k, v] : names)
        if (n == k) return v;
    return 0;
}
}  // namespace

// --- settings ----------------------------------------------------------------------------------------------

fs::path Settings::path() {
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    return fs::path(local ? local : L".") / "minecraftCplusplus" / "options.txt";
}

bool Settings::load() {
    std::ifstream f(path());
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        const size_t c = line.find(':');
        if (c == std::string::npos) continue;
        const std::string k = line.substr(0, c), v = line.substr(c + 1);
        if (k == "fov") fov = std::clamp(float(std::atof(v.c_str())), 30.f, 110.f);
        else if (k == "renderDistance") renderDistance = std::clamp(std::atoi(v.c_str()), 2, 64);
        else if (k == "guiScale") guiScale = std::clamp(std::atoi(v.c_str()), 0, 8);
        else if (k == "mouseSensitivity") sensitivity = std::clamp(float(std::atof(v.c_str())), 0.f, 1.f);
        else if (k == "shaders") {
            static const char* presets[] = {"off", "low", "medium", "high", "ultra"};
            for (u8 i = 0; i < 5; ++i)
                if (v == presets[i]) video.shaders = i, videoSaved = true;
        } else if (k == "upscaler") VideoSettings::parseUpscaler(v, video.upscaler);
        else if (k == "upscaleQuality") VideoSettings::parseQuality(v, video.quality);
        else if (k == "sharpness") video.sharpness = std::clamp(float(std::atof(v.c_str())), 0.f, 1.f);
        else if (k == "frameGeneration") video.parseFrameGen(v);
        else if (k == "lowLatency") VideoSettings::parseLowLatency(v, video.lowLatency);
        else if (k == "vsync") video.vsync = v == "true";
        else if (k == "maxFps") video.fpsLimit = u32(std::clamp(std::atoi(v.c_str()), 0, 1000));
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
    static const char* presets[] = {"off", "low", "medium", "high", "ultra"};
    f << "shaders:" << presets[std::min<int>(video.shaders, 4)] << "\nupscaler:" << VideoSettings::upscalerKey(video.upscaler)
      << "\nupscaleQuality:" << VideoSettings::qualityKey(video.quality) << "\nsharpness:" << video.sharpness
      << "\nframeGeneration:" << video.frameGenKey() << "\nlowLatency:" << VideoSettings::lowLatencyKey(video.lowLatency)
      << "\nvsync:" << (video.vsync ? "true" : "false") << "\nmaxFps:" << video.fpsLimit << "\n";
}

float Settings::radiansPerPixel() const {
    const float d = sensitivity * 0.6f + 0.2f;
    return d * d * d * 8.0f * 0.15f * kPi / 180.0f;
}

// --- state helpers ------------------------------------------------------------------------------------------

int Ui::State::gameMode() const {
    if (session) return session->gameMode();
    return 1;  // local worlds play like creative
}

int Ui::State::guiScale() const {
    const u32 w = window->width(), h = window->height();
    int scale = 1;
    const int max = settings.guiScale;
    while (scale != max && u32(scale) < w && u32(scale) < h && w / u32(scale + 1) >= 320 && h / u32(scale + 1) >= 240) ++scale;
    return scale;
}

void Ui::State::openScreen(Screen s) {
    const bool wasNone = screen == Screen::None;
    screen = s;
    buttons.clear();
    draggingSlider = -1;
    isQuickCrafting = false;
    quickCraftSlots.clear();
    hoveredSlot = -1;
    if (s == Screen::Chat) {
        chatInput.blinkStart = clock;
        historyPos = -1;
        chatScroll = 0;
    }
    if (s == Screen::Creative) updateCreativeFilter();
    buildButtons();
    if (wasNone) {
        window->setMouseCaptured(false);
        window->centerCursor();
    }
    window->setClickCaptures(false);
}

void Ui::State::closeScreen(bool sendClose) {
    const Screen old = screen;
    if ((old == Screen::Inventory || old == Screen::Container) && online()) {
        // Carried items go back (the server does the same on close); tell it the menu closed.
        if (sendClose) {
            net::Writer w;
            w.varInt(activeMenu().containerId);
            client->sendPlay("container_close", w);
        }
    }
    if (old == Screen::Container) container.reset();
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

void Ui::State::addChat(const Text& message) {
    nativeMods.chatReceived(message.plain());
    chatMessages.insert(chatMessages.begin(), {message, guiTicks});
    if (chatMessages.size() > 100) chatMessages.pop_back();
    const std::vector<Text> lines = font.split(message, 320);  // ChatComponent.getWidth() at default options
    for (size_t i = 0; i < lines.size(); ++i) {
        if (screen == Screen::Chat && chatScroll > 0) ++chatScroll;
        chatLines.insert(chatLines.begin(), ChatLine{lines[i], guiTicks, i == lines.size() - 1});
    }
    if (chatLines.size() > 100) chatLines.resize(100);
}

void Ui::State::sendChat(const std::string& raw) {
    std::string text = raw;
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    if (text.empty()) return;
    if (sentHistory.empty() || sentHistory.back() != text) sentHistory.push_back(text);
    if (sentHistory.size() > 100) sentHistory.erase(sentHistory.begin());
    if (text[0] == '/' && nativeMods.command(text)) return;  // a native mod's command (mods/mcpp_mod.h)
    if (!online()) {
        if (text[0] == '/') addChat(Text("Commands need a server", colorStyle(0xFF5555)));
        else addChat(translatable("chat.type.text", {Text("Player"), Text(text)}, lang()));
        return;
    }
    if (text[0] == '/') {
        net::Writer w;
        w.str(text.substr(1));
        client->sendPlay("chat_command", w);
        LOG("[chat] sent command %s", text.c_str());
        return;
    }
    // ServerboundChatPacket: unsigned (offline mode), with the message acknowledgements the server tracks.
    static std::mt19937_64 rng(std::random_device{}());
    net::Writer w;
    w.str(text);
    w.u64v(u64(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
    w.u64v(rng());
    w.boolean(false);  // no signature
    w.varInt(lastSeenOffset);
    lastSeenOffset = 0;
    u8 bits[3] = {};
    for (int i = 0; i < 20; ++i) {
        const int index = (lastSeenTail + i) % 20;
        if (!lastSeen[size_t(index)]) continue;
        bits[i / 8] |= u8(1 << (i % 8));
        lastSeen[size_t(index)]->acknowledged = true;
    }
    w.bytes(bits, 3);
    w.u8v(0);  // checksum: ignored
    client->sendPlay("chat", w);
    LOG("[chat] sent message %s", text.c_str());
}

void Ui::State::syncHotbarToGame() {
    for (int i = 0; i < 9; ++i) {
        const ItemStack& s = inventory.slots[size_t(36 + i)];
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
        game->setHotbarBlock(i, block);
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
    if (idx >= 0 && idx < 9) return start + 27 + idx;
    if (idx >= 9 && idx < 36) return start + idx - 9;
    return -1;
}

// --- Ui -----------------------------------------------------------------------------------------------------

Ui::Ui() : s_(std::make_unique<State>()) {}
Ui::~Ui() = default;

Settings& Ui::settings() { return s_->settings; }
bool Ui::quitRequested() const { return s_->quit; }
bool Ui::menuOpen() const { return s_->screen != Screen::None && s_->screen != Screen::Chat; }

float Ui::fovRadians() const { return s_->settings.fov * kPi / 180.0f; }
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
    s.guiAtlas.init(2048, 4096);
    s.iconAtlas.init(2048, 2048);
    if (!s.font.load(s.res, s.guiAtlas) || !s.sprites.load(s.res, s.guiAtlas)) return false;
    if (!s.items.init(reports, s.res)) LOG("warning: item registry unavailable; inventories will be empty");
    for (const char* tex : {"gui/container/inventory", "gui/container/generic_54", "gui/container/crafting_table",
                            "gui/container/furnace", "gui/container/creative_inventory/tab_item_search",
                            "gui/container/creative_inventory/tab_items", "gui/container/dispenser",
                            "gui/container/hopper", "gui/container/shulker_box", "gui/container/smoker",
                            "gui/container/blast_furnace"})
        s.sprites.texture(tex);
    s.icons.init(s.res, s.items, s.iconAtlas);
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

    s.inventory.slots.assign(46, ItemStack{});
    s.inventory.title = Text(s.res.translate("container.crafting"));
    for (int i = 0; i < s.items.count(); ++i)
        if (s.items.name(i) != "minecraft:air") {
            ItemStack st;
            st.item = i, st.count = 1;
            s.creativeItems.push_back(st);
        }
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
    in.game->onInteract = [this](int x, int y, int z) {
        State& st = *s_;
        if (!st.online()) return false;
        std::string n = st.registry->name(st.world->block(x, y, z));
        n = n.substr(n.find(':') + 1, n.find('[') == std::string::npos ? std::string::npos : n.find('[') - n.find(':') - 1);
        static const char* exact[] = {"chest", "trapped_chest", "ender_chest", "barrel", "crafting_table", "crafter",
                                      "furnace", "blast_furnace", "smoker", "dispenser", "dropper", "hopper",
                                      "enchanting_table", "brewing_stand", "loom", "stonecutter", "grindstone",
                                      "smithing_table", "cartography_table", "beacon", "lectern", "lever", "repeater",
                                      "comparator", "daylight_detector", "cake", "respawn_anchor", "jukebox", "note_block",
                                      "anvil", "chipped_anvil", "damaged_anvil", "bell", "decorated_pot", "chiseled_bookshelf"};
        for (const char* e : exact)
            if (n == e) return true;
        const bool iron = n.starts_with("iron_");
        return n.ends_with("shulker_box") || n.ends_with("_bed") || n.ends_with("_button") || n.ends_with("_fence_gate") ||
               (!iron && (n.ends_with("_door") || n.ends_with("_trapdoor")));
    };
    // The launch detail radius sized the geometry arena: the option goes up to it.
    // (kept as saved: a launch with a smaller --detail must not overwrite the player's choice)
    if (!s.settings.loaded) s.settings.renderDistance = in.detailChunks;
    in.terrain->setDetailRadius(float(std::min(s.settings.renderDistance, in.detailChunks)) * 16.0f);
    s.initNativeMods();  // mods_screen.cpp
    return true;
}

void Ui::registerHandlers(net::Client& client) {
    State* s = s_.get();
    const Lang* lang = &s->res.lang();
    const Items* items = &s->items;

    client.onPlay("set_health", [s](net::Reader& r) {
        const float health = r.f32();
        const int food = r.varInt();
        const float sat = r.f32();
        if (std::getenv("MCPP_UI_VERBOSE")) LOG("ui: set_health %.1f food %d sat %.1f", health, food, sat);
        s->post([s, health, food, sat] {
            s->health = health, s->food = food, s->saturation = sat;
            if (health <= 0 && s->screen != Screen::Death) s->openScreen(Screen::Death);
        });
    });
    client.onPlay("set_experience", [s](net::Reader& r) {
        const float progress = r.f32();
        const int level = r.varInt(), total = r.varInt();
        s->post([s, progress, level, total] { s->xpProgress = progress, s->xpLevel = level, s->xpTotal = total; });
    });
    client.onPlayPeek("set_entity_data", [s](net::Reader& r) {  // our air supply (entity data index 1)
        const int entity = r.varInt();
        int air = -1;
        for (int guard = 0; guard < 4 && r.ok; ++guard) {
            const int index = r.u8v();
            if (index == 0xFF) break;
            const int type = r.varInt();
            if (type == 0) r.u8v();  // byte (shared flags)
            else if (type == 1) {
                const int v = r.varInt();
                if (index == 1) air = v;
            } else break;
        }
        if (air >= 0) s->post([s, entity, air] {
            if (s->session && entity == s->session->entityId()) s->air = air, s->serverAir = true;
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
    client.onPlayPeek("respawn", [s](net::Reader&) {  // a new player entity: no effects, no open screens
        s->post([s] {
            s->effects.clear();
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
            s->effects[id] = State::Effect{name, amp, dur, (flags & 1) != 0, (flags & 4) != 0};
        });
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
    auto chatMessage = [s](Text t) {
        LOG("[chat] %s", t.plain().c_str());
        s->post([s, t = std::move(t)] { s->addChat(t); });
    };
    client.onPlay("system_chat", [s, lang, chatMessage](net::Reader& r) {
        Text t = parseComponent(r.nbt(), lang);
        if (r.boolean()) {
            s->post([s, t] {
                s->actionbar = t;
                s->actionbarTime = 60;
            });
            return;
        }
        chatMessage(std::move(t));
    });
    auto decorate = [lang](int chatType, const Text& sender, const Text* target, const Text& content) {
        if (chatType < 0 || chatType >= int(std::size(kChatTypes))) return translatable("chat.type.text", {sender, content}, lang);
        const ChatType& ct = kChatTypes[chatType];
        Style st;
        if (ct.grayItalic) st.color = 0xAAAAAA, st.italic = 1;
        const Text tgt = target ? *target : Text();
        if (ct.params == 3) return translatable(ct.key, {tgt, sender, content}, lang, st);
        if (ct.params == -2) return translatable(ct.key, {tgt, content}, lang, st);
        return translatable(ct.key, {sender, content}, lang, st);
    };
    client.onPlay("player_chat", [s, lang, chatMessage, decorate](net::Reader& r) {
        r.varInt();    // global index
        r.skip(16);    // sender
        r.varInt();    // index
        std::string signature;
        if (r.boolean()) signature = std::string(r.bytes(256));
        Text content(std::string(r.str()));
        r.i64v(), r.i64v();  // timestamp, salt
        const i32 seen = r.varInt();
        for (i32 i = 0; i < seen && r.ok; ++i)
            if (r.varInt() == 0) r.skip(256);
        if (r.boolean()) content = parseComponent(r.nbt(), lang);
        const int filter = r.varInt();
        if (filter == 2) {
            const i32 longs = r.varInt();
            r.skip(size_t(std::max(0, longs)) * 8);
        }
        const int type = r.varInt() - 1;
        if (type < 0) return;  // inline chat type: not supported
        const Text sender = parseComponent(r.nbt(), lang);
        Text target;
        const bool hasTarget = r.boolean();
        if (hasTarget) target = parseComponent(r.nbt(), lang);
        if (!signature.empty())
            s->post([s, signature] {  // LastSeenMessagesTracker.addPending
                if (signature == s->lastSeenSignature) return;
                s->lastSeenSignature = signature;
                s->lastSeen[size_t(s->lastSeenTail)] = State::Seen{signature, false};
                s->lastSeenTail = (s->lastSeenTail + 1) % 20;
                if (++s->lastSeenOffset > 64 && s->online()) {
                    net::Writer w;
                    w.varInt(s->lastSeenOffset);
                    s->client->sendPlay("chat_ack", w);
                    s->lastSeenOffset = 0;
                }
            });
        if (filter == 1) return;
        chatMessage(decorate(type, sender, hasTarget ? &target : nullptr, content));
    });
    client.onPlay("disguised_chat", [lang, chatMessage, decorate](net::Reader& r) {
        const Text content = parseComponent(r.nbt(), lang);
        const int type = r.varInt() - 1;
        const Text sender = type >= 0 ? parseComponent(r.nbt(), lang) : Text("?");
        Text target;
        const bool hasTarget = type >= 0 && r.boolean();
        if (hasTarget) target = parseComponent(r.nbt(), lang);
        chatMessage(decorate(type, sender, hasTarget ? &target : nullptr, content));
    });
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
        s->post([s, t] { s->actionbar = t, s->actionbarTime = 60; });
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
}

void Ui::attachSession(net::Client* client, net::Session* session) {
    State& s = *s_;
    s.client = client;
    s.session = session;
    if (session) session->creativeHotbarSync = false;  // the inventory is server-driven now
    s.inventory.slots.assign(46, ItemStack{});
    s.effects.clear();
    std::fill(std::begin(s.hotbarSynced), std::end(s.hotbarSynced), -2);
    s.carried = ItemStack{};
    s.container.reset();
    if (s.screen != Screen::None && s.screen != Screen::Chat) s.closeScreen(false);
}

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
            const std::string b = arg(2);
            const int button = b == "right" ? 1 : b == "middle" ? 2 : 0;
            if (st.verb == "click") window->inject({E::Button, button, true}), window->inject({E::Button, button, false});
            if (st.verb == "release") window->inject({E::Button, button, false});
            if (st.verb == "move" && b == "down") window->inject({E::Button, button, true});
        } else if (st.verb == "slot") {  // slot <index> [left|right] [shift]: click a slot of the open menu
            const Menu& m = activeMenu();
            const Layout l = layoutFor(m);
            const int idx = std::atoi(arg(0).c_str());
            if (idx >= 0 && size_t(idx) < l.slots.size()) {
                const float left = float((guiW() - l.width) / 2), top = float((guiH() - l.height) / 2);
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
        } else if (st.verb == "shot") {
            shotPath = arg(0);
            shotPending = true;
        } else if (st.verb == "log") {
            std::string msg;
            for (const auto& w : a) msg += w + " ";
            LOG("script: %s", msg.c_str());
        } else if (st.verb == "dump") {  // logs the open menu's slots
            const Menu& m = activeMenu();
            for (size_t i = 0; i < m.slots.size(); ++i)
                if (!m.slots[i].empty())
                    LOG("script: slot %zu = %d x %s", i, m.slots[i].count, items.name(m.slots[i].item).c_str());
            if (!carried.empty()) LOG("script: carried = %d x %s", carried.count, items.name(carried.item).c_str());
        } else if (st.verb == "quit") {
            quit = true;
        }
    }
}

void Ui::beginFrame(double dt) {
    State& s = *s_;
    s.clock += dt;
    {
        std::vector<std::function<void()>> pending;
        {
            std::lock_guard lock(s.mutex);
            pending.swap(s.pending);
        }
        for (auto& fn : pending) fn();
    }
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
        if (s.screen != Screen::None) {
            s.screenInput(e);
            continue;
        }
        if (e.type != Window::InputEvent::Key || !e.down) continue;
        switch (e.code) {
            case VK_ESCAPE: s.openScreen(Screen::Pause); break;
            case VK_F1: s.hudHidden = !s.hudHidden; break;
            case VK_F3: s.debug = !s.debug; break;
            case 'E':
                s.openScreen(s.creative() ? Screen::Creative : Screen::Inventory);
                break;
            case 'T':
            case VK_OEM_2:
                s.openScreen(Screen::Chat);
                if (e.code == VK_OEM_2) {
                    s.chatInput.value = "/";
                    s.chatInput.cursor = s.chatInput.selection = 1;
                }
                skipChar = true;
                break;
            case 'Q':
                if (std::getenv("MCPP_UI_VERBOSE")) LOG("ui: drop slot %d", s.game->selectedSlot());
                s.dropSelected(s.window->keyDown(VK_CONTROL));
                break;
            case 'F':
                s.swapHands();
                break;
        }
    }
    s.game->inputSuspended = s.screen != Screen::None;
    s.game->sensitivity = s.settings.radiansPerPixel();

    // The open container's player slots are the player's inventory (one Inventory in the game).
    if (s.container) {
        const int start = layoutFor(*s.container).playerStart;
        for (int i = 0; i < 36 && size_t(start + i) < s.container->slots.size(); ++i)
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
        const double ms = s.clock * 1000.0 * 8.0;
        s.pass->glint[0] = g->u0, s.pass->glint[1] = g->v0, s.pass->glint[2] = g->u1 - g->u0, s.pass->glint[3] = g->v1 - g->v0;
        s.pass->glint[4] = -float(std::fmod(ms, 110000.0) / 110000.0), s.pass->glint[5] = float(std::fmod(ms, 30000.0) / 30000.0);
        s.pass->glint[6] = float(16 * scale) * 2.0f;
    }
    if (!s.hudHidden) s.drawHud(c);
    if (!s.hudHidden) s.drawModHud(c);  // native mods
    if (s.debug) s.drawDebug(c);
    if (s.screen != Screen::None) s.drawScreen(c);
}

}  // namespace ui
