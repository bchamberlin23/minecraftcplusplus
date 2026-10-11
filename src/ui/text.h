#pragma once
// Styled text like the game's components: a flat list of literal runs with resolved styles. Legacy
// formatting codes (section sign + code) inside a run are applied at layout time, like StringDecomposer.
#include "core/common.h"

#include <json/json.hpp>

#include <unordered_map>

namespace ui {

using Lang = std::unordered_map<std::string, std::string>;

struct Style {
    i32 color = -1;   // 0xRRGGBB, -1: not set
    i64 shadow = -1;  // 0xAARRGGBB shadow colour override, -1: not set
    i8 bold = -1, italic = -1, underlined = -1, strikethrough = -1, obfuscated = -1;  // -1: not set

    bool isBold() const { return bold == 1; }
    bool isItalic() const { return italic == 1; }
    bool isUnderlined() const { return underlined == 1; }
    bool isStrikethrough() const { return strikethrough == 1; }
    bool isObfuscated() const { return obfuscated == 1; }
    // This style's set fields over `parent`.
    Style over(const Style& parent) const;
    // Style.applyLegacyFormat: code is the character after the section sign. false if unknown.
    bool applyLegacy(char code, const Style& reset);
    bool operator==(const Style&) const = default;
};

struct Span {
    std::string text;  // UTF-8, may contain legacy codes
    Style style;
};

struct Text {
    std::vector<Span> spans;

    Text() = default;
    Text(std::string s, Style st = {}) { spans.push_back({std::move(s), st}); }  // NOLINT: implicit literal
    bool empty() const;
    std::string plain() const;  // without legacy codes
    Text& append(const Text& t, const Style* parent = nullptr);
    Text& append(std::string s, Style st = {}) {
        spans.push_back({std::move(s), st});
        return *this;
    }
    Text withStyle(const Style& base) const;  // `base` under every run's own style
    bool operator==(const Text&) const;
};

// The 16 named colours (ChatFormatting) by legacy code index 0..15.
extern const u32 kLegacyColors[16];
i32 colorByName(std::string_view name);  // "red", "#FF0000"; -1 if unknown
Style colorStyle(u32 rgb);

// Chat components from the network (NBT: type byte + payload) and JSON.
Text parseComponent(std::string_view nbt, const Lang* lang);
Text parseJsonComponent(const nlohmann::json& j, const Lang* lang);
// Translatable text with plain arguments.
Text translatable(const std::string& key, const std::vector<Text>& args, const Lang* lang, Style style = {});
std::string formatJava(const std::string& fmt, const std::vector<std::string>& args);

// Decodes one UTF-8 code point at s[i], advancing i (invalid bytes yield U+FFFD).
u32 nextCodepoint(std::string_view s, size_t& i);
void appendUtf8(std::string& s, u32 cp);

}  // namespace ui
