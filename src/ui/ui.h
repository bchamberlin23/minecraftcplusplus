#pragma once
// The game's user interface: HUD (hotbar, hearts, hunger, armor, air, experience, titles, action bar), F3 debug
// screen, chat, inventories and containers (server driven), creative inventory, pause and options menus.
//
// Per frame: beginFrame() before Game::frame (routes input: while a screen is open the game gets none),
// endFrame() after it (builds the canvas the UI render pass draws over the frame).
#include "core/common.h"
#include "render/upscalers/video_settings.h"

#include <functional>
#include <memory>

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

namespace ui {

struct ItemStack;

struct Settings {
    float fov = 70;           // degrees (vertical), 30..110
    int renderDistance = 32;  // full-detail radius in chunks
    int guiScale = 0;         // 0 = auto
    float sensitivity = 0.5f;  // 0..1 like the game's option
    VideoSettings video;       // Video Settings screen (shaders, upscaler, frame generation, latency, VSync, FPS limit)
    bool videoSaved = false;   // options.txt had video settings
    bool loaded = false;

    static fs::path path();
    bool load();
    void save() const;
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
    // Open to LAN: publishes the local server (game mode for others, the host's cheats, port); error on failure.
    std::function<bool(const std::string& motd, const std::string& gameMode, bool cheats, u16 port, std::string& error)>
        openToLan;
};

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
    // F3 biome name at a block position (local worlds with the generator); unset: not shown.
    void setBiomeLookup(std::function<std::string(int x, int y, int z)> fn);
    Settings& settings();
    // Player inventory menu slot (5-8 armour, 36-44 hotbar, 45 offhand); nullptr when empty.
    const ItemStack* playerSlot(int slot) const;

    struct State;  // ui_state.h

private:
    bool loadImpl(const fs::path& jar, const fs::path& minecraftDir, const GameReports& reports);
    std::unique_ptr<State> s_;
};

}  // namespace ui
