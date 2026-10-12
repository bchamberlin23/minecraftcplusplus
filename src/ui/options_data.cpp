// The options.txt keys of the game's Options that Settings keeps beyond the core ones in ui.cpp: video quality and
// preferences, accessibility, chat, language and fonts, music & sounds, and the startup / tutorial / service flags.
// Same keys and encodings as Options.processOptions (booleans "true" / "false", enums by id or serialized name).
#include "ui/ui.h"

#include <ostream>
#include <variant>

namespace ui {

namespace {

struct EnumNames {
    std::vector<const char*> names;  // serialized name by index
};

struct Field {
    const char* key;
    std::variant<bool Settings::*, int Settings::*, double Settings::*, std::string Settings::*> member;
    double lo = 0, hi = 0;           // numeric range (lo == hi: unclamped)
    const EnumNames* names = nullptr;  // int enums stored by name
};

const EnumNames kPreset{{"fast", "fancy", "fabulous", "custom"}};
const EnumNames kClouds{{"false", "fast", "true"}};
const EnumNames kInactivity{{"minimized", "afk"}};
const EnumNames kMusicFrequency{{"DEFAULT", "FREQUENT", "CONSTANT"}};
const EnumNames kMusicToast{{"never", "pause", "pause_and_toast"}};
const EnumNames kTutorial{{"movement", "find_tree", "punch_tree", "open_inventory", "craft_planks", "none"}};

using S = Settings;
const Field kFields[] = {
    {"graphicsPreset", &S::graphicsPreset, 0, 3, &kPreset},
    {"simulationDistance", &S::simulationDistance, 5, 32},
    {"entityDistanceScaling", &S::entityDistanceScaling, 0.5, 5.0},
    {"entityShadows", &S::entityShadows},
    {"ao", &S::ao},
    {"cutoutLeaves", &S::cutoutLeaves},
    {"vignette", &S::vignette},
    {"improvedTransparency", &S::improvedTransparency},
    {"biomeBlendRadius", &S::biomeBlendRadius, 0, 7},
    {"renderClouds", &S::cloudStatus, 0, 2, &kClouds},
    {"cloudRange", &S::cloudRange, 2, 128},
    {"weatherRadius", &S::weatherRadius, 3, 10},
    {"chunkSectionFadeInTime", &S::chunkFade, 0.0, 2.0},
    {"prioritizeChunkUpdates", &S::prioritizeChunkUpdates, 0, 2},
    {"gamma", &S::gamma, 0.0, 1.0},
    {"mipmapLevels", &S::mipmapLevels, 0, 4},
    {"textureFiltering", &S::textureFiltering, 0, 2},
    {"maxAnisotropyBit", &S::maxAnisotropyBit, 1, 3},
    {"particles", &S::particles, 0, 2},
    {"inactivityFpsLimit", &S::inactivityFpsLimit, 0, 1, &kInactivity},
    {"showAutosaveIndicator", &S::showAutosaveIndicator},
    {"narrator", &S::narrator, 0, 3},
    {"narratorHotkey", &S::narratorHotkey},
    {"showSubtitles", &S::showSubtitles},
    {"highContrast", &S::highContrast},
    {"highContrastBlockOutline", &S::highContrastBlockOutline},
    {"hideLightningFlashes", &S::hideLightningFlash},
    {"hideSplashTexts", &S::hideSplashTexts},
    {"darkMojangStudiosBackground", &S::darkMojangStudiosBackground},
    {"rotateWithMinecart", &S::rotateWithMinecart},
    {"menuBackgroundBlurriness", &S::menuBackgroundBlurriness, 0, 10},
    {"textBackgroundOpacity", &S::textBackgroundOpacity, 0.0, 1.0},
    {"panoramaScrollSpeed", &S::panoramaSpeed, 0.0, 1.0},
    {"notificationDisplayTime", &S::notificationDisplayTime, 0.5, 10.0},
    {"screenEffectScale", &S::screenEffectScale, 0.0, 1.0},
    {"fovEffectScale", &S::fovEffectScale, 0.0, 1.0},
    {"darknessEffectScale", &S::darknessEffectScale, 0.0, 1.0},
    {"glintSpeed", &S::glintSpeed, 0.0, 1.0},
    {"glintStrength", &S::glintStrength, 0.0, 1.0},
    {"damageTiltStrength", &S::damageTiltStrength, 0.0, 1.0},
    {"backgroundForChatOnly", &S::backgroundForChatOnly},
    {"chatVisibility", &S::chatVisibility, 0, 2},
    {"chatColors", &S::chatColors},
    {"chatLinks", &S::chatLinks},
    {"chatLinksPrompt", &S::chatLinksPrompt},
    {"autoSuggestions", &S::autoSuggestions},
    {"hideMatchedNames", &S::hideMatchedNames},
    {"reducedDebugInfo", &S::reducedDebugInfo},
    {"onlyShowSecureChat", &S::onlyShowSecureChat},
    {"saveChatDrafts", &S::saveChatDrafts},
    {"chatOpacity", &S::chatOpacity, 0.0, 1.0},
    {"chatLineSpacing", &S::chatLineSpacing, 0.0, 1.0},
    {"chatScale", &S::chatScale, 0.0, 1.0},
    {"chatWidth", &S::chatWidth, 0.0, 1.0},
    {"chatHeightFocused", &S::chatHeightFocused, 0.0, 1.0},
    {"chatHeightUnfocused", &S::chatHeightUnfocused, 0.0, 1.0},
    {"chatDelay", &S::chatDelay, 0.0, 6.0},
    {"lang", &S::lang},
    {"forceUnicodeFont", &S::forceUnicodeFont},
    {"japaneseGlyphVariants", &S::japaneseGlyphVariants},
    {"soundDevice", &S::soundDevice},
    {"directionalAudio", &S::directionalAudio},
    {"musicFrequency", &S::musicFrequency, 0, 2, &kMusicFrequency},
    {"musicToast", &S::musicToast, 0, 2, &kMusicToast},
    {"lastServer", &S::lastServer},
    {"tutorialStep", &S::tutorialStep, 0, 5, &kTutorial},
    {"joinedFirstServer", &S::joinedFirstServer},
    {"skipMultiplayerWarning", &S::skipMultiplayerWarning},
    {"hideServerAddress", &S::hideServerAddress},
    {"realmsNotifications", &S::realmsNotifications},
    {"telemetryOptInExtra", &S::telemetryOptInExtra},
    {"onboardAccessibility", &S::onboardAccessibility},
    {"startedCleanly", &S::startedCleanly},
};

// Options writes strings of OptionInstance<String> as JSON ("soundDevice":"\"\""); plain fields as they are.
std::string unquote(const std::string& v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        std::string out;
        for (size_t i = 1; i + 1 < v.size(); ++i) {
            if (v[i] == '\\' && i + 2 < v.size()) ++i;
            out += v[i];
        }
        return out;
    }
    return v;
}

}  // namespace

bool Settings::loadExtra(const std::string& key, const std::string& value) {
    for (const Field& f : kFields) {
        if (key != f.key) continue;
        std::visit(
            [&](auto m) {
                using T = std::remove_reference_t<decltype(this->*m)>;
                if constexpr (std::is_same_v<T, bool>) {
                    this->*m = value == "true";
                } else if constexpr (std::is_same_v<T, std::string>) {
                    this->*m = unquote(value);
                } else if constexpr (std::is_same_v<T, int>) {
                    int v = std::atoi(value.c_str());
                    if (f.names) {
                        v = -1;
                        for (size_t i = 0; i < f.names->names.size(); ++i)
                            if (value == f.names->names[i]) v = int(i);
                        if (v < 0) return;  // unknown name: the default stays
                    }
                    this->*m = f.lo < f.hi ? std::clamp(v, int(f.lo), int(f.hi)) : v;
                } else {
                    const double v = std::atof(value.c_str());
                    this->*m = f.lo < f.hi ? std::clamp(v, f.lo, f.hi) : v;
                }
            },
            f.member);
        return true;
    }
    return false;
}

void Settings::saveExtra(std::ostream& out) const {
    for (const Field& f : kFields) {
        out << f.key << ':';
        std::visit(
            [&](auto m) {
                using T = std::remove_cvref_t<decltype(this->*m)>;
                if constexpr (std::is_same_v<T, bool>) out << (this->*m ? "true" : "false");
                else if constexpr (std::is_same_v<T, std::string>) out << (std::string_view(f.key) == "soundDevice" ? "\"" + this->*m + "\"" : this->*m);
                else if constexpr (std::is_same_v<T, int>) {
                    if (f.names) out << f.names->names[size_t(std::clamp(this->*m, 0, int(f.names->names.size()) - 1))];
                    else out << this->*m;
                } else out << this->*m;
            },
            f.member);
        out << '\n';
    }
}

}  // namespace ui
