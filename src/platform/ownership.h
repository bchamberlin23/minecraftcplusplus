#pragma once
// Ownership check: the game only runs for someone who owns Minecraft: Java Edition. It requires the
// official launcher on this machine to be signed in with an account that has a Java Edition profile
// (profiles exist only for owners) and licences, plus the game's own client files (every asset is read
// from them). This is a local check against the launcher's data, not an online verification.
#include "core/common.h"

struct Ownership {
    bool owned = false;
    std::string playerName, playerId;  // the Java Edition profile
    std::string reason;                // why not, for the user
};

Ownership checkOwnership(const fs::path& minecraftDir);
