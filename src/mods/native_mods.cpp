#include "mods/native_mods.h"

#include "mods/mcpp_mod.h"

#include <windows.h>

#include <algorithm>

namespace mods {

namespace {

// Calls into mod code under structured exception handling: an access violation (or a C++ exception escaping the
// mod) disables that mod instead of ending the game. Plain functions: __try cannot share a frame with objects that
// need unwinding.
bool sehInit(McppModInitFn fn, const McppApi* api, McppMod* mod, int* result) {
    __try {
        *result = fn(api, mod);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool sehVoid(void (*fn)(void)) {
    __try {
        fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool sehBlock(void (*fn)(int, int, int, const char*), int x, int y, int z, const char* state) {
    __try {
        fn(x, y, z, state);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool sehText(void (*fn)(const char*), const char* text) {
    __try {
        fn(text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool sehHud(void (*fn)(McppHud*), McppHud* hud) {
    __try {
        fn(hud);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool sehCommand(McppCommandFn fn, void* user, const char* args) {
    __try {
        fn(user, args);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

NativeMods* gMods = nullptr;  // the C callbacks have no context argument
double gStart = nowSeconds();

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(u8(c)));
    return s;
}

}  // namespace

struct NativeMods::Mod {
    HMODULE module = nullptr;
    std::unique_ptr<u8[]> table;  // zero-filled, larger than any McppMod
    McppMod callbacks{};          // the fields the mod's McppMod.size covers
    std::string name;
    bool active = false;
};

// The host side of McppApi.
struct Api {
    static const char* modName() {
        return gMods && gMods->current_ < gMods->mods_.size() ? gMods->mods_[gMods->current_]->name.c_str() : "?";
    }
    static void log(const char* text) { LOG("[%s] %s", modName(), text ? text : ""); }
    static void chat(const char* text, uint32_t argb) {
        if (gMods && gMods->host_.chat && text) gMods->host_.chat(text, argb);
    }
    static void sendChat(const char* text) {
        if (gMods && gMods->host_.sendChat && text) gMods->host_.sendChat(text);
    }
    static int registerCommand(const char* name, const char* usage, McppCommandFn fn, void* user) {
        if (!gMods || !name || !*name || !fn || gMods->current_ >= gMods->mods_.size()) return 0;
        std::string n = lower(name[0] == '/' ? name + 1 : name);
        if (gMods->commands_.count(n)) return 0;
        gMods->commands_[n] = {gMods->current_, fn, user, usage ? usage : ""};
        LOG("[%s] registered /%s", modName(), n.c_str());
        return 1;
    }
    static int getBlock(int x, int y, int z, char* out, int size) {
        if (!gMods || !gMods->host_.block) return 0;
        const std::string s = gMods->host_.block(x, y, z);
        if (out && size > 0) {
            const size_t n = std::min(s.size(), size_t(size - 1));
            std::memcpy(out, s.data(), n);
            out[n] = 0;
        }
        return int(s.size());
    }
    static void position(double out[3]) {
        out[0] = out[1] = out[2] = 0;
        if (gMods && gMods->host_.position) gMods->host_.position(out);
    }
    static void rotation(float out[2]) {
        out[0] = out[1] = 0;
        if (gMods && gMods->host_.rotation) gMods->host_.rotation(out);
    }
    static int online() { return gMods && gMods->host_.online && gMods->host_.online() ? 1 : 0; }
    static double time() { return nowSeconds() - gStart; }
    static NativeMods::Hud& hud(McppHud* h) { return *reinterpret_cast<NativeMods::Hud*>(h); }
    static void hudSize(McppHud* h, float out[2]) { out[0] = hud(h).width, out[1] = hud(h).height; }
    static void hudText(McppHud* h, float x, float y, uint32_t argb, const char* text) {
        if (text && hud(h).text) hud(h).text(x, y, argb, text);
    }
    static int hudTextWidth(McppHud* h, const char* text) { return text && hud(h).textWidth ? hud(h).textWidth(text) : 0; }
    static void hudRect(McppHud* h, float x0, float y0, float x1, float y1, uint32_t argb) {
        if (hud(h).rect) hud(h).rect(x0, y0, x1, y1, argb);
    }

    static const McppApi* table() {
        static const McppApi api = {MCPP_API_VERSION, sizeof(McppApi), &log, &chat, &sendChat, &registerCommand,
                                    &getBlock, &position, &rotation, &online, &time, &hudSize, &hudText,
                                    &hudTextWidth, &hudRect};
        return &api;
    }
};

NativeMods::NativeMods() = default;

NativeMods::~NativeMods() {
    for (size_t i = 0; i < mods_.size(); ++i) {
        Mod& m = *mods_[i];
        current_ = i;
        if (m.active && m.callbacks.on_unload) sehVoid(m.callbacks.on_unload);
        if (m.active && m.module) FreeLibrary(m.module);  // a crashed mod's code may still be referenced: kept
    }
    if (gMods == this) gMods = nullptr;
}

void NativeMods::load(const std::vector<fs::path>& dlls, const fs::path& modsDir, Host host) {
    host_ = std::move(host);
    gMods = this;
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(modsDir, ec);
    for (const fs::path& dll : dlls) {
        Status st;
        st.file = dll;
        st.name = dll.filename().string();
        const fs::path full = fs::weakly_canonical(dll, ec);
        // Only DLLs directly in the mods folder.
        if (ec || full.parent_path() != root) {
            st.state = "not in the mods folder: not loaded";
            LOG("mods: %s is not in the mods folder; not loaded", dll.string().c_str());
            status_.push_back(st);
            continue;
        }
        Timer t;
        // Dependencies resolve from the mod's own folder and the system, never the working directory.
        HMODULE module = LoadLibraryExW(full.c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) {
            st.state = "could not be loaded (error " + std::to_string(GetLastError()) + ")";
            LOG("mods: %s %s", st.name.c_str(), st.state.c_str());
            status_.push_back(st);
            continue;
        }
        const auto init = reinterpret_cast<McppModInitFn>(GetProcAddress(module, "mcpp_mod_init"));
        if (!init) {
            FreeLibrary(module);
            st.state = "no mcpp_mod_init export: not a native mod";
            LOG("mods: %s has no mcpp_mod_init export; unloaded", st.name.c_str());
            status_.push_back(st);
            continue;
        }
        auto m = std::make_unique<Mod>();
        m->module = module;
        m->table = std::make_unique<u8[]>(4096);  // zero-filled
        m->name = st.name;
        McppMod* table = reinterpret_cast<McppMod*>(m->table.get());
        current_ = mods_.size();
        mods_.push_back(std::move(m));
        Mod& mod = *mods_.back();
        int result = -1;
        if (!sehInit(init, Api::table(), table, &result)) {
            st.state = "crashed in mcpp_mod_init (disabled)";
        } else if (result != 0) {
            st.state = "mcpp_mod_init returned " + std::to_string(result) + " (unloaded)";
            FreeLibrary(module), mod.module = nullptr;
        } else if (table->api_version == 0 || table->api_version > MCPP_API_VERSION) {
            st.state = "built for API version " + std::to_string(table->api_version) + ", this game has " +
                       std::to_string(MCPP_API_VERSION) + " (unloaded)";
            FreeLibrary(module), mod.module = nullptr;
        } else {
            // Only the callbacks the mod's own McppMod declares.
            std::memcpy(&mod.callbacks, table, std::min<size_t>(sizeof(McppMod), std::clamp<size_t>(table->size, 0, 4096)));
            const size_t n = strnlen(table->name ? table->name : "", 128);
            if (n) mod.name = std::string(table->name, n);
            st.name = mod.name;
            st.version = table->version ? std::string(table->version, strnlen(table->version, 64)) : "";
            st.state = "loaded";
            st.active = mod.active = true;
        }
        current_ = SIZE_MAX;
        LOG("mods: native mod %s %s: %s (%.1f ms)", st.name.c_str(), st.version.c_str(), st.state.c_str(), t.ms());
        status_.push_back(st);
    }
}

void NativeMods::crashed(size_t i, const char* where) {
    Mod& m = *mods_[i];
    m.active = false;
    for (Status& s : status_)
        if (s.name == m.name) {
            s.active = false;
            s.state = std::string("crashed in ") + where + " (disabled)";
        }
    for (auto it = commands_.begin(); it != commands_.end();) it = it->second.mod == i ? commands_.erase(it) : std::next(it);
    LOG("mods: native mod %s crashed in %s and was disabled", m.name.c_str(), where);
    if (host_.chat) host_.chat("Native mod " + m.name + " crashed and was disabled", 0xFFFF5555);
}

void NativeMods::frame(double dt) {
    if (mods_.empty()) return;
    tickClock_ += std::min(dt, 0.25);
    int ticks = 0;
    while (tickClock_ >= 0.05 && ticks < 5) {
        tickClock_ -= 0.05, ++ticks;
        for (size_t i = 0; i < mods_.size(); ++i) {
            Mod& m = *mods_[i];
            if (!m.active || !m.callbacks.on_tick) continue;
            current_ = i;
            if (!sehVoid(m.callbacks.on_tick)) crashed(i, "on_tick");
        }
    }
    current_ = SIZE_MAX;
}

void NativeMods::blockChanged(bool place, int x, int y, int z, const std::string& state) {
    for (size_t i = 0; i < mods_.size(); ++i) {
        Mod& m = *mods_[i];
        auto fn = place ? m.callbacks.on_block_place : m.callbacks.on_block_break;
        if (!m.active || !fn) continue;
        current_ = i;
        if (!sehBlock(fn, x, y, z, state.c_str())) crashed(i, place ? "on_block_place" : "on_block_break");
    }
    current_ = SIZE_MAX;
}

bool NativeMods::command(const std::string& line) {
    if (line.size() < 2 || line[0] != '/') return false;
    const size_t space = line.find(' ');
    const auto it = commands_.find(lower(line.substr(1, space == std::string::npos ? std::string::npos : space - 1)));
    if (it == commands_.end()) return false;
    const Command cmd = it->second;
    const std::string args = space == std::string::npos ? std::string() : line.substr(space + 1);
    current_ = cmd.mod;
    if (!sehCommand(cmd.fn, cmd.user, args.c_str())) crashed(cmd.mod, ("/" + it->first).c_str());
    current_ = SIZE_MAX;
    return true;
}

void NativeMods::chatReceived(const std::string& text) {
    for (size_t i = 0; i < mods_.size(); ++i) {
        Mod& m = *mods_[i];
        if (!m.active || !m.callbacks.on_chat) continue;
        current_ = i;
        if (!sehText(m.callbacks.on_chat, text.c_str())) crashed(i, "on_chat");
    }
    current_ = SIZE_MAX;
}

void NativeMods::drawHud(Hud& hud) {
    for (size_t i = 0; i < mods_.size(); ++i) {
        Mod& m = *mods_[i];
        if (!m.active || !m.callbacks.on_hud) continue;
        current_ = i;
        if (!sehHud(m.callbacks.on_hud, reinterpret_cast<McppHud*>(&hud))) crashed(i, "on_hud");
    }
    current_ = SIZE_MAX;
}

size_t NativeMods::activeCount() const {
    size_t n = 0;
    for (const auto& m : mods_) n += m->active;
    return n;
}

}  // namespace mods
