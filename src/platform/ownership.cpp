#include "platform/ownership.h"

#include <json/json.hpp>

Ownership checkOwnership(const fs::path& minecraftDir) {
    Ownership o;
    bool sawAccounts = false;
    for (const char* file : {"launcher_accounts_microsoft_store.json", "launcher_accounts.json"}) {
        std::vector<u8> bytes;
        if (!readFile(minecraftDir / file, bytes)) continue;
        const auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (!j.is_object() || !j.contains("accounts") || !j["accounts"].is_object()) continue;
        sawAccounts = true;
        const std::string active = j.value("activeAccountLocalId", "");
        for (const auto& [id, account] : j["accounts"].items()) {
            if (!active.empty() && id != active) continue;
            const auto profile = account.value("minecraftProfile", nlohmann::json::object());
            const bool licensed = account.contains("licenseProductIds") && account["licenseProductIds"].is_array() &&
                                  !account["licenseProductIds"].empty();
            if (profile.is_object() && !profile.value("name", "").empty() && licensed) {
                o.owned = true;
                o.playerName = profile.value("name", "");
                o.playerId = profile.value("id", "");
                return o;
            }
        }
    }
    o.reason = sawAccounts ? "the Minecraft Launcher's active account has no Minecraft: Java Edition profile"
                           : "the official Minecraft Launcher is not installed or not signed in";
    return o;
}
