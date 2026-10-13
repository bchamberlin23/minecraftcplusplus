#pragma once
// Singleplayer: the game's rules (mobs, redstone, survival...) come from the official dedicated
// server, run locally on the player's world in the background while this engine renders it.
//  - server.jar is the official one for the installed version (downloaded once, SHA-1 checked).
//  - A world from an older version is backed up before the server upgrades it, like the game's
//    "Backup and load".
//  - The singleplayer host's player data (inventory, position...) is handed to the local offline
//    player before the server starts and written back after it stops, so the world keeps working
//    in the real game.
#include "core/common.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

#include <windows.h>

class LocalServer {
public:
    struct Config {
        fs::path minecraftDir;  // .minecraft
        std::string version;    // installed version id ("26.3")
        fs::path world;         // the save to play
        std::string player = "Player";
        u16 port = 0;           // 0: any free port
        bool lan = false;       // listen on all interfaces (friends can join) instead of loopback only
        int viewDistance = 12;  // chunks the server sends; farther terrain is this engine's own LOD
        // A new world (Create New World): the server creates `world` with these settings; once it stops, level.dat
        // gets the singleplayer setup the game expects (LevelName, cheats, the owner's player data).
        struct Create {
            std::string levelName;           // shown in the world list
            std::string seed;                // empty: random; text seeds are hashed like the game does
            std::string gameMode = "survival";  // survival / creative / adventure / spectator
            std::string difficulty = "normal";  // peaceful / easy / normal / hard
            bool hardcore = false, cheats = false, structures = true;
            // Create World's World / More tabs (ui/world_create.cpp):
            std::string levelType;          // world preset ("minecraft:flat", "minecraft:large_biomes"...); empty: normal
            std::string generatorSettings;  // Superflat: FlatLevelGeneratorSettings JSON (server.properties)
            std::string singleBiome;        // Single Biome: its biome (a preset override pack while the world is made)
            bool bonusChest = false;        // the server's --bonusChest
            std::vector<std::pair<std::string, std::string>> gameRules;  // set from the console once it is made
            std::vector<std::string> enabledPacks, disabledPacks;        // initial-enabled-packs (experiments, packs)
        };
        std::optional<Create> create;
        std::string ownerUuid;  // the Java profile id: whose singleplayer player data the save keeps (new worlds)
        // Optimize World (EditWorldScreen): the server upgrades every chunk first (--forceUpgrade [--eraseCache]).
        bool forceUpgrade = false, eraseCache = false;
    };

    ~LocalServer();
    bool start(const Config& config);  // prepares files and launches; returns without waiting
    bool ready() const { return ready_; }      // accepting connections
    bool upgrading() const { return upgrading_; }  // the world is being converted to this version first
    bool creating() const { return creating_; }    // the server is creating the world (Config::create)
    // --forceUpgrade: the upgrader's last status line and percentage (-1 until it reports one).
    std::string upgradeStatus() const;
    int upgradePercent() const { return upgradePercent_; }
    bool running() const;
    bool saving() const { return saving_; }  // MinecraftServer.isCurrentlySaving (autosave / save-all)
    u16 port() const { return port_; }
    // Fabric server: the block table its helper mod wrote (Client::Config::serverRegistries), else empty.
    fs::path stateTable() const {
        std::error_code ec;
        const fs::path p = runDir_ / "mcpp-registries.json";
        return fs::exists(p, ec) ? p : fs::path();
    }
    void command(const std::string& line);     // server console
    // Open to LAN with cheats: every player who joins from now on is made an operator (vanilla's LAN worlds allow
    // commands for later joiners too; "op @a" only reached those online at the time).
    void setOpJoiners(bool on) { opJoiners_ = on; }
    void stop(double timeoutSeconds = 60.0);   // saves, exits and syncs the player data back

private:
    bool ensureServerJar(fs::path& jar);
    bool backupIfUpgrading();
    bool exportHostPlayer();   // host -> offline player file
    void importHostPlayer();   // offline player file -> host
    void finishNewWorld();     // level.dat of a created world: name, cheats (the dedicated server leaves them out)
    void readOutput();

    Config config_;
    fs::path runDir_;
    std::string offlineUuid_, hostUuid_;
    HANDLE process_ = nullptr, job_ = nullptr, stdinWrite_ = nullptr, stdoutRead_ = nullptr;
    std::thread reader_;
    std::atomic<bool> ready_{false}, failed_{false}, opJoiners_{false}, saving_{false};
    std::atomic<int> upgradePercent_{-1};
    mutable std::mutex statusMutex_;
    std::string upgradeStatus_;
    std::mutex stdinMutex_;
    u16 port_ = 0;
    bool stopped_ = false, upgrading_ = false, creating_ = false;
};
