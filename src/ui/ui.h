#pragma once
// The game's user interface: HUD (hotbar, hearts, hunger, armor, air, experience, titles, action bar), F3 debug
// screen, chat, inventories and containers (server driven), creative inventory, pause and options menus.
//
// Per frame: beginFrame() before Game::frame (routes input: while a screen is open the game gets none),
// endFrame() after it (builds the canvas the UI render pass draws over the frame).
#include "core/common.h"

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

struct Settings {
    float fov = 70;           // degrees (vertical), 30..110
    int renderDistance = 32;  // full-detail radius in chunks
    int guiScale = 0;         // 0 = auto
    float sensitivity = 0.5f;  // 0..1 like the game's option
    bool loaded = false;

    static fs::path path();
    bool load();
    void save() const;
    // Mouse look in radians per pixel (the game's sensitivity curve).
    float radiansPerPixel() const;
};

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
    const fs::path* screenshotRequest();  // scripted screenshots: pass to Renderer::render
    bool loadScript(const fs::path& file);
    float fovRadians() const;
    // F3 biome name at a block position (local worlds with the generator); unset: not shown.
    void setBiomeLookup(std::function<std::string(int x, int y, int z)> fn);
    Settings& settings();

    struct State;  // ui_state.h

private:
    bool loadImpl(const fs::path& jar, const fs::path& minecraftDir, const GameReports& reports);
    std::unique_ptr<State> s_;
};

}  // namespace ui
