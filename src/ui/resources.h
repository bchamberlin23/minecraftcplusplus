#pragma once
// Read-only view of the game's client resources: the installed client jar plus the asset index objects
// the launcher downloads next to it (unifont, sounds, other languages), with the enabled resource packs on top
// (AssetSource::client). Index objects take precedence over the jar, like the game's vanilla pack.
#include "assets/asset_source.h"
#include "core/common.h"

#include <json/json.hpp>

#include <map>
#include <unordered_map>

namespace ui {

struct Image {
    int w = 0, h = 0;
    std::vector<u8> rgba;  // w * h * 4
    const u8* at(int x, int y) const { return &rgba[(size_t(y) * w + x) * 4]; }
};

class Resources {
public:
    // jar: versions/<v>/<v>.jar; the asset index id comes from the version json beside it.
    bool open(const fs::path& jar, const fs::path& minecraftDir);

    // Full resource paths: "assets/minecraft/textures/gui/sprites/hud/hotbar.png".
    bool read(std::string_view path, std::vector<u8>& out) const;
    bool exists(std::string_view path) const;
    bool image(std::string_view path, Image& out) const;
    nlohmann::json json(std::string_view path) const;  // discarded value when missing / malformed
    // Files under a prefix in any pack.
    std::vector<std::string> list(std::string_view prefix) const;
    const fs::path& objectPath(std::string_view path, fs::path& out) const;  // the file on disk, or empty

    // The selected language over en_us (ClientLanguage.loadFrom): the same map object for the whole run.
    const std::unordered_map<std::string, std::string>& lang() const { return lang_; }
    std::string translate(const std::string& key) const {
        auto it = lang_.find(key);
        return it == lang_.end() ? key : it->second;
    }
    // Languages from the packs' pack.mcmeta "language" sections (LanguageManager.extractLanguages), by code.
    struct LanguageInfo {
        std::string name, region;
        bool bidirectional = false;
    };
    std::map<std::string, LanguageInfo> languages() const;
    // LanguageManager.onResourceManagerReload: en_us, then `code` over it (an unknown code keeps en_us). Callers
    // make sure nothing reads lang() meanwhile (the network thread: net::Client::pauseDispatch).
    void setLanguage(const std::string& code);
    const std::string& languageCode() const { return langCode_; }
    bool rightToLeft() const { return rtl_; }  // ClientLanguage.isDefaultRightToLeft

private:
    const AssetSource* src_ = nullptr;  // the game's assets with resource packs (assets/asset_source.h)
    std::unordered_map<std::string, std::string> lang_;
    std::string langCode_ = "en_us";
    bool rtl_ = false;
};

// Decodes a PNG (or anything stb_image reads) to RGBA8.
bool decodeImage(const std::vector<u8>& bytes, Image& out);

}  // namespace ui
