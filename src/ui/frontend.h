#pragma once
// The front end's internal state and helpers: title screen, loading / message / confirm screens and switching
// instances (title.cpp), world list, creation and editing (worlds.cpp), server list (servers.cpp).
#include "net/status.h"
#include "platform/launch.h"
#include "ui/ui_state.h"
#include "world/nbt_dom.h"

#include <memory>
#include <unordered_map>

namespace ui {

using State = Ui::State;
using TextField = Ui::State::TextField;

// --- worlds.cpp ---------------------------------------------------------------------------------------------------

struct WorldEntry {
    fs::path dir;
    std::string folder, name, versionName;
    i64 lastPlayed = -1;  // ms since 1970
    int gameMode = 0, dataVersion = 0;
    bool hardcore = false, cheats = false, locked = false;
    Image icon;  // icon.png (64x64), empty when the world has none
};
bool readWorld(const fs::path& dir, WorldEntry& out);       // level.dat summary; false: not a world
std::vector<WorldEntry> scanWorlds(const fs::path& saves);  // most recently played first
bool worldLocked(const fs::path& dir);                      // session.lock held by a running game / server
std::string worldFolderFor(const fs::path& saves, const std::string& name);  // FileUtil.findAvailableName
// Gzipped NBT files of a save (level.dat...); writing keeps the old file as <name>_old like the game.
bool readNbtGz(const fs::path& p, NbtValue& root, std::string* rootName = nullptr);
bool writeNbtGz(const fs::path& p, const NbtValue& root, std::string_view rootName);

// --- servers.cpp --------------------------------------------------------------------------------------------------

struct ServerEntry {
    std::string name, ip;
    std::string icon;  // base64 PNG (from servers.dat or the last status)
};
// Our list; on first use it is imported from the game's servers.dat (which is only read).
std::vector<ServerEntry> loadServers(const fs::path& json, const fs::path& dat);
void saveServers(const fs::path& json, const std::vector<ServerEntry>& servers);

// Status pings run on worker threads; their results land here (shared: a ping may outlive the screen).
struct PingBoard {
    struct Result {
        bool done = false;
        net::ServerStatus status;
    };
    std::mutex mutex;
    std::unordered_map<std::string, Result> results;  // by address
};

// --- shared -------------------------------------------------------------------------------------------------------

struct Field {  // a text box of a front-end screen (EditBox)
    TextField text;
    float x = 0, y = 0, w = 200, h = 20;
    std::string hint;  // shown gray while empty
};

struct FrontEndState {
    FrontEnd cfg;
    bool titleRequested = false;  // leave this world for the title screen
    bool leaving = false;         // saving / starting the title instance (no more disconnect screens)
    bool handedOff = false;       // the next instance shows its window: this one exits
    std::string titleMenu;        // the title instance opens this list ("servers" after a multiplayer disconnect)
    std::string splash;
    int protocol = 0;
    std::string gameVersion;  // "26.3"

    // Text boxes of the open screen and the focused one; Esc / Enter of the open screen (set by its build function).
    std::vector<Field*> fields;
    int focus = -1;
    std::function<void()> onBack, onEnter;
    // Dynamic images (world / server icons) in the GUI atlas, by key.
    std::unordered_map<std::string, Sprite> icons;
    std::unordered_map<std::string, std::string> iconSource;  // key -> what is uploaded (skips re-uploads)
    double lastClickTime = 0;
    int lastClickRow = -1;

    // World list
    std::vector<WorldEntry> worlds;
    std::vector<int> worldView;  // shown (search filter) -> worlds index
    int worldSel = -1;           // worlds index
    float worldScroll = 0;
    Field worldSearch;
    // Create / edit world
    Field createName, createSeed;
    int createMode = 0;  // 0 survival, 1 hardcore, 2 creative
    int createDifficulty = 2;
    bool createCheats = false, createStructures = true;
    Field editName;
    // Confirmation
    Text confirmTitle, confirmText;
    std::string confirmYes;
    std::function<void()> confirmAction;
    Screen confirmBack = Screen::Worlds;

    // Server list
    std::vector<ServerEntry> servers;
    std::shared_ptr<PingBoard> pings;
    int serverSel = -1;  // servers index, then LAN worlds (servers.size() + i)
    float serverScroll = 0;
    net::LanListener lan;
    std::vector<net::LanListener::Entry> lanWorlds;
    Field serverName, serverIp, directIp;
    int editServer = -1;  // -1: adding
    bool serversDirty = false;

    // Loading the next instance / message
    std::unique_ptr<launch::Child> child;
    Text loadingTitle;
    std::string loadingStatus;
    float progress = 0;
    double loadingStart = 0;
    bool loadingCancel = false;
    Screen failBack = Screen::Title;
    Text failTitle;
    Text messageTitle, messageText;
    std::string messageButton;
    std::function<void()> messageAction;

    // Open to LAN
    int lanMode = 0;  // survival, creative, adventure, spectator
    bool lanCheats = false;
    Field lanPort;
    std::string lanError;
    u16 lanOpenPort = 0;  // published on this port
};

// Layout and drawing helpers (title.cpp).
void menuBackground(State& s, Canvas& c);  // the menu texture over the panorama / world
void listBackground(State& s, Canvas& c, float top, float bottom);  // list area with header / footer separators
void setFields(State& s, std::vector<Field*> fields, int focus = 0);
void drawFields(State& s, Canvas& c);
bool fieldsInput(State& s, const Window::InputEvent& e);  // focus clicks, typing, Tab; true when used
Button& addButton(State& s, float x, float y, float w, Text label, std::function<void()> fn, bool active = true);
const Sprite* iconSprite(State& s, const std::string& key, const Image& img, const std::string& source);
std::string fitText(Font& f, const std::string& text, int maxWidth);
bool doubleClicked(State& s, int row);  // a click on `row` right after one on the same row
Text tr(State& s, const char* key);
Text trf(State& s, const char* key, const std::vector<Text>& args);

// Switching instances / messages (title.cpp).
void startChild(State& s, std::vector<std::wstring> args, Text title, Screen back, bool cancellable);
void showMessage(State& s, Text title, Text text, std::string button, std::function<void()> action);
void showConfirm(State& s, Text title, Text text, std::string yes, std::function<void()> action, Screen back);
void requestTitle(State& s, const std::string& menu);  // leave this world ("" title, "servers", "worlds")
std::wstring widen(const std::string& utf8);
std::string utf8(const fs::path& p);  // paths for display / keys (fs::path::string throws on some names)

// Screens: build (buttons and fields) / draw / input.
void buildTitle(State& s);
void drawTitle(State& s, Canvas& c);
void buildWorlds(State& s);
void drawWorlds(State& s, Canvas& c);
void worldsInput(State& s, const Window::InputEvent& e);
void openWorlds(State& s);  // rescans the saves
void buildCreateWorld(State& s);
void drawCreateWorld(State& s, Canvas& c);
void openCreateWorld(State& s);                         // world_create.cpp (tabs, Superflat, Single Biome, rules...)
void createWorldInput(State& s, const Window::InputEvent& e);
bool recreateWorld(State& s, const fs::path& dir);      // Re-Create: Create New World filled in from a world
void buildEditWorld(State& s);
void drawEditWorld(State& s, Canvas& c);
void buildServers(State& s);
void drawServers(State& s, Canvas& c);
void serversInput(State& s, const Window::InputEvent& e);
void openServers(State& s);  // loads the list, pings, starts LAN discovery
void buildEditServer(State& s);
void drawEditServer(State& s, Canvas& c);
void buildDirectConnect(State& s);
void drawDirectConnect(State& s, Canvas& c);
void serversFrame(State& s);  // ping results, LAN worlds
void frontEndInit(State& s);
void openToLanScreen(State& s);  // pause menu: Open to LAN  // Ui::init: the title screen (menu mode) or the in-world parts
// friends_screen.cpp: the Friends screen, its button label ("Friends (2)" with requests), --menu friend-join:<code>.
void openFriendsScreen(State& s);
Text friendsButtonLabel();
bool friendsMenuCommand(State& s, const std::string& command);

}  // namespace ui
