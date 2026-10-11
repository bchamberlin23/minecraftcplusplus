#pragma once
// Native mods: DLLs in the mods folder implementing the C ABI in mods/mcpp_mod.h. Loaded once at startup (only
// from the mods folder, only when enabled), called on the main thread; a mod that crashes in a callback is
// disabled and the game keeps running. The UI owns the instance and provides the host side (chat, world, HUD).
#include "core/common.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct McppMod;

namespace mods {

class NativeMods {
public:
    struct Host {
        std::function<void(const std::string& text, u32 argb)> chat;  // local chat line
        std::function<void(const std::string& text)> sendChat;        // to the server (message or /command)
        std::function<std::string(int x, int y, int z)> block;        // block state key
        std::function<void(double out[3])> position;
        std::function<void(float out[2])> rotation;  // degrees
        std::function<bool()> online;
    };
    // HUD drawing during on_hud (GUI pixels).
    struct Hud {
        float width = 0, height = 0;
        std::function<void(float x, float y, u32 argb, const std::string& text)> text;
        std::function<int(const std::string& text)> textWidth;
        std::function<void(float x0, float y0, float x1, float y1, u32 argb)> rect;
    };
    struct Status {
        fs::path file;
        std::string name, version, state;  // state: "loaded", "crashed in on_tick (disabled)", ...
        bool active = false;
    };

    NativeMods();
    ~NativeMods();
    NativeMods(const NativeMods&) = delete;
    NativeMods& operator=(const NativeMods&) = delete;

    void load(const std::vector<fs::path>& dlls, const fs::path& modsDir, Host host);
    void frame(double dt);  // on_tick at 20 Hz
    void blockChanged(bool place, int x, int y, int z, const std::string& state);
    bool command(const std::string& line);  // "/name args": true when a mod's command handled it
    void chatReceived(const std::string& text);
    void drawHud(Hud& hud);
    const std::vector<Status>& status() const { return status_; }
    size_t activeCount() const;

    struct Mod;  // native_mods.cpp
    struct Command {
        size_t mod;
        void (*fn)(void*, const char*);
        void* user;
        std::string usage;
    };

private:
    friend struct Api;
    void crashed(size_t mod, const char* where);

    Host host_;
    std::vector<std::unique_ptr<Mod>> mods_;
    std::vector<Status> status_;
    std::map<std::string, Command> commands_;
    double tickClock_ = 0;
    size_t current_ = SIZE_MAX;  // mod being called (for log lines / command registration)
};

}  // namespace mods
