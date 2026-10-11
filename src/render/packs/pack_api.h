#pragma once
// Shader pack API for the game and the UI: available packs (our folder and the vanilla launcher's), per-pack option
// files (Iris "<pack>.txt" format), the game state packs read as uniforms, and switching packs on a Renderer.
#include "core/common.h"

#include <map>
#include <string>
#include <unordered_map>

class Renderer;
class BlockRegistry;

namespace packs {

struct PackInfo {
    std::string name;  // file / folder name
    fs::path path;
    bool external = false;  // from .minecraft\shaderpacks (read-only; its settings file is read, ours is written)
};
fs::path packDirectory();                 // %APPDATA%\minecraftCplusplus\shaderpacks (created)
fs::path externalPackDirectory();         // %APPDATA%\.minecraft\shaderpacks
std::vector<PackInfo> listPacks();        // ours first, then .minecraft's (names not already listed)
bool findPack(const std::string& nameOrPath, PackInfo& out);

// Option values for a pack: ours (<packDirectory>/<name>.txt), else the vanilla launcher's next to the pack.
std::unordered_map<std::string, std::string> loadPackSettings(const PackInfo& pack);
void savePackSettings(const PackInfo& pack, const std::unordered_map<std::string, std::string>& values);

// What the game knows that packs read (Iris uniforms); the renderer derives what it can when this is not set.
struct PackGameState {
    bool valid = false;
    std::string dimension = "minecraft:overworld";
    i64 worldTime = -1;       // full world day time in ticks (-1: from the sky state); worldTime = % 24000
    int moonPhase = 0;
    int eyeBrightness[2] = {0, 240};  // block, sky light at the eye (x16)
    int isEyeInWater = -1;            // 0 air, 1 water, 2 lava, 3 powder snow (-1: from the sky state)
    float blindness = 0, nightVision = 0, darknessFactor = 0, darknessLightFactor = 0, playerMood = 0;
    std::string heldItem, heldItem2;  // "minecraft:torch"
    int heldBlockLight = 0, heldBlockLight2 = 0;
    std::string biome = "minecraft:plains";
    float temperature = 0.8f, rainfall = 0.4f;
    int precipitation = 1;  // 0 none, 1 rain, 2 snow
    int biomeCategory = 5;
    bool rightHanded = true, sneaking = false, sprinting = false, hurt = false, invisible = false, burning = false,
         onGround = true, spectator = false, hideGui = false;
    float health = 20, maxHealth = 20, food = 20, air = 300, maxAir = 300;
    float screenBrightness = 0.5f;
    std::map<std::string, float> extra;  // any other float uniform by name
};

// Renderer integration (the renderer creates its pack pipeline on first use).
void setShaderPack(Renderer& renderer, const fs::path& pack,
                   const std::unordered_map<std::string, std::string>& options = {});  // empty path: built-in
void setBlockRegistry(Renderer& renderer, const BlockRegistry* registry);
void setGameState(Renderer& renderer, const PackGameState& state);
struct PackStatus {
    std::string active, loading, error;
    std::vector<std::string> warnings;
    double loadMs = 0;
};
PackStatus packStatus(Renderer& renderer);

// Command line: --shaderpack <name | path | off> and --pack-option NAME=VALUE (main.cpp forwards them), applied once
// the renderer and the block registry exist.
void commandLine(const std::wstring& flag, const std::wstring& value);
void applyCommandLine(Renderer& renderer, const BlockRegistry* registry);
bool commandLinePack();  // --shaderpack was given (it wins over the saved choice)

// The Video Settings choice (options.txt shaderPack): a pack by name with its saved option values, "" = built-in.
bool selectPack(Renderer& renderer, const std::string& name);

}  // namespace packs
