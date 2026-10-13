#pragma once
// Mods and packs: one folder (%APPDATA%\minecraftCplusplus\mods, or --mods-dir / MCPP_MODS_DIR) where the player
// drops files; each one's kind is detected from its contents:
//  - resource pack: zip or folder with pack.mcmeta and assets/  -> layered over the game's assets (AssetSource::client)
//  - data pack:     zip or folder with pack.mcmeta and data/    -> layered over the game's data for the world generator,
//                   and copied into the world's datapacks/ when it is played (--play), so the server uses it too
//  - Fabric mod:    jar with fabric.mod.json                   -> --play runs the Fabric server with these mods
//  - native mod:    DLL exporting mcpp_mod_init (mods/mcpp_mod.h)
// The player's vanilla .minecraft\resourcepacks are listed too, enabled as selected in .minecraft\options.txt
// (only read, never written). On/off and order are kept in mods.json beside the mods folder.
#include "core/common.h"

#include <string>
#include <vector>

namespace mods {

enum class Kind { ResourcePack, DataPack, ResourceAndDataPack, FabricMod, NativeMod, Unsupported };
const char* kindName(Kind k);

struct Entry {
    std::string id;           // "mods/<file>" or "resourcepacks/<file>" (stable key in mods.json)
    std::string name;         // display name
    std::string description;  // pack description or mod summary (plain text)
    std::string source;       // "mods folder", ".minecraft resourcepacks"
    fs::path path;
    std::string prefix;       // archive entries below this folder (a pack zipped inside a folder)
    Kind kind = Kind::Unsupported;
    bool enabled = false;
    bool hasAssets = false, hasData = false;
    // Fabric
    std::string modId, version, environment;
    bool clientContent = false;  // registers blocks / items / entities with client assets
    std::vector<std::string> warnings;
    std::string compat;  // pack format note ("made for an older version"), empty when it matches
    std::vector<std::string> features;  // pack.mcmeta features.enabled: the feature flags the pack requests
    bool detected = true;  // false: a vanilla resource pack not looked into yet (only when shown or enabled)

    bool isResourcePack() const { return kind == Kind::ResourcePack || kind == Kind::ResourceAndDataPack; }
    bool isDataPack() const { return kind == Kind::DataPack || kind == Kind::ResourceAndDataPack; }
};

class Manager {
public:
    struct Config {
        fs::path minecraftDir;  // .minecraft
        std::string version;    // "26.3"
        fs::path jar;           // client jar
        fs::path world;         // the world being opened (may be empty when joining a server)
        fs::path modsDir;       // empty: MCPP_MODS_DIR, else %APPDATA%\minecraftCplusplus\mods
        bool play = false;      // the world is played through the local server (data packs are installed into it)
    };

    static Manager& get();
    // Scans the mods folder and the vanilla resource packs, then fills AssetSource::client() and ::data().
    void init(const Config& config);
    bool initialized() const { return initialized_; }

    const std::vector<Entry>& entries() const { return entries_; }
    const fs::path& modsDir() const { return modsDir_; }
    // UI edits (saved to mods.json immediately; take effect after a restart).
    void setEnabled(size_t index, bool on);
    void move(size_t index, int delta);  // within the entries of the same group (packs / mods)
    bool restartNeeded() const;
    // Starts this program again with the same command line once it has exited (the local server stopped first).
    void requestRestart() { restart_ = true; }
    ~Manager();
    void openModsFolder() const;
    void rescan();  // picks up files dropped in while running (listed, but applied after a restart)
    void detectPending();  // looks into the vanilla resource packs skipped at startup (for the list)

    // Enabled Fabric mod jars (for the local server) and native mod DLLs.
    std::vector<fs::path> fabricMods() const;
    std::vector<fs::path> nativeMods() const;
    // True when enabled Fabric mods add blocks / items the vanilla protocol cannot show.
    bool fabricContent() const;
    // Data packs of the world as the server will load them (after install), bottom first: names for the log / UI.
    const std::vector<std::string>& worldDataPacks() const { return worldPacks_; }

private:
    void scan();
    void loadConfig();
    void saveConfig() const;
    void buildClientStack();
    void buildDataStack();
    void installWorldDataPacks();
    std::string stateKey() const;
    void detectEntry(Entry& e);

    Config config_;
    fs::path modsDir_, configFile_;
    std::vector<Entry> entries_;
    std::vector<std::string> worldPacks_;
    std::string startState_;
    bool initialized_ = false, restart_ = false;
    size_t vanillaPacks_ = 0;  // bottom packs of the client stack (jar, asset index)
    int resourceFormat_ = 0, dataFormat_ = 0;
};

}  // namespace mods
