#pragma once
// Fabric server mods for singleplayer / LAN (--play): when the mods folder has Fabric mods, the local server is
// Fabric's dedicated server (the official launcher from meta.fabricmc.net for the installed game version, downloaded
// once and verified against the installer Fabric publishes with a SHA-1 on its maven) running those mods on top of
// the official server.jar. Server-side mods (performance, rules, commands, world generation) then work through
// this engine's vanilla-protocol client.
#include "core/common.h"

#include <vector>

namespace mods {

// serverRoot: %LOCALAPPDATA%\minecraftCplusplus\server. vanillaJar: the verified official server.jar.
// On success runDir is <serverRoot>\<version>-fabric (its mods\ holding exactly `mods` plus our helper mod, which
// writes the server's block table to mcpp-registries.json there, see net/fabric_sync.h) and launcherJar the
// launcher to run with `java -jar` (same arguments as the vanilla server).
bool prepareFabricServer(const fs::path& serverRoot, const fs::path& minecraftDir, const std::string& version,
                         const fs::path& vanillaJar, const std::vector<fs::path>& mods, fs::path& runDir,
                         fs::path& launcherJar);

}  // namespace mods
