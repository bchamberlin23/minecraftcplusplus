#pragma once
// A shader pack's option screens (shaders.properties screen / screen.<NAME> / sliders, lang/en_us.lang) as data for
// the Video Settings UI: which entries a screen shows, their labels, and the user's values (Iris's settings format,
// saved by pack_api.h savePackSettings). No UI or D3D dependencies.
#include "render/packs/pack_api.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace packs {

class PackDefinition;

class PackOptionsModel {
public:
    PackOptionsModel();
    ~PackOptionsModel();
    // Reads the pack's options and screens (no shader compilation) and the user's saved values.
    bool open(const PackInfo& pack, std::string& error);

    struct Entry {
        enum Kind { Empty, Option, Link, Profile } kind = Empty;
        std::string id;  // option or screen name
    };
    std::vector<Entry> entries(const std::string& screen) const;  // "" = the main screen
    int columns(const std::string& screen) const;
    std::string screenTitle(const std::string& screen) const;
    std::string label(const Entry& e) const;  // "Shadow Quality: High", "Lighting...", "Profile: High"
    bool isSlider(const std::string& option) const;
    void cycle(const Entry& e, int dir);  // next / previous value (options and the profile)
    void resetAll();

    const PackInfo& pack() const { return pack_; }
    const std::unordered_map<std::string, std::string>& values() const { return values_; }  // non-default values
    bool changed() const { return changed_; }

private:
    std::string value(const std::string& option) const;  // in effect (default when not set)
    std::string text(const std::string& key, const std::string& fallback) const;  // lang lookup, formatting removed
    std::vector<std::string> profiles() const;
    std::string currentProfile() const;  // the profile whose values match, else ""
    std::unordered_map<std::string, std::string> profileValues(const std::string& name) const;

    PackInfo pack_;
    std::unique_ptr<PackDefinition> def_;
    std::unordered_map<std::string, std::string> values_;
    bool changed_ = false;
};

}  // namespace packs
