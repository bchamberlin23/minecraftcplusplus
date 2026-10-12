#pragma once
// Styled text like the game's components: a flat list of literal runs with resolved styles. Legacy
// formatting codes (section sign + code) inside a run are applied at layout time, like StringDecomposer.
#include "core/common.h"

#include <json/json.hpp>

#include <memory>
#include <unordered_map>

namespace ui {

struct TextEvents;  // click / hover events of a component (below)

using Lang = std::unordered_map<std::string, std::string>;

// Style.font (FontDescription): a font resource ("minecraft:alt"), or what an "object" component shows in the text
// (an atlas sprite, a player's head). Interned: Style keeps the index (fontIndex / fontDesc, any thread).
struct FontDesc {
    enum Kind : u8 { Resource, Atlas, Player } kind = Resource;
    std::string id;             // Resource: the font ("minecraft:default"); Atlas: the atlas ("minecraft:blocks")
    std::string sprite;         // Atlas: the sprite ("block/stone")
    std::string name, uuid;     // Player: the profile (name and / or "xxxxxxxx-xxxx-..." id)
    std::string texture;        // Player: the profile's skin texture override ("entity/player/wide/alex"), or the
                                // "textures" property value (base64 JSON with the skin URL)
    bool hat = true;            // Player: the hat layer too
    bool operator==(const FontDesc&) const = default;
};
int fontIndex(const FontDesc& d);  // >= 0
FontDesc fontDesc(int index);

struct Style {
    i32 color = -1;   // 0xRRGGBB, -1: not set
    i64 shadow = -1;  // 0xAARRGGBB shadow colour override, -1: not set
    i8 bold = -1, italic = -1, underlined = -1, strikethrough = -1, obfuscated = -1;  // -1: not set
    i32 font = -1;    // fontIndex(FontDesc), -1: not set (the default font)
    std::shared_ptr<const TextEvents> events;  // click_event / hover_event / insertion (inherited); null: none

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

// Style click_event / hover_event / insertion (ClickEvent, HoverEvent).
struct TextEvents {
    std::string click;       // "open_url", "run_command", "suggest_command", "copy_to_clipboard", "change_page"...; "" none
    std::string clickValue;  // url / command / value / page number / custom id
    std::string clickNbt;    // show_dialog dialog (id string or inline compound) / custom payload: network NBT
    std::string hover;       // "show_text", "show_item", "show_entity"; "" none
    Text hoverText;          // show_text value; show_entity name
    std::string hoverId;     // show_item item id / show_entity type id
    int hoverCount = 1;      // show_item count
    std::string hoverUuid;   // show_entity
    bool hasInsertion = false;
    std::string insertion;   // shift-click inserts into the chat input
};

// The 16 named colours (ChatFormatting) by legacy code index 0..15.
extern const u32 kLegacyColors[16];
i32 colorByName(std::string_view name);  // "red", "#FF0000"; -1 if unknown
Style colorStyle(u32 rgb);

// Chat components from the network (NBT: type byte + payload) and JSON.
Text parseComponent(std::string_view nbt, const Lang* lang);
Text parseJsonComponent(const nlohmann::json& j, const Lang* lang);
// Style.Serializer as network NBT (a component's style fields: colour, flags, events).
Style parseStyle(std::string_view nbt, const Lang* lang);
// Translatable text with plain arguments.
Text translatable(const std::string& key, const std::vector<Text>& args, const Lang* lang, Style style = {});
std::string formatJava(const std::string& fmt, const std::vector<std::string>& args);

// Decodes one UTF-8 code point at s[i], advancing i (invalid bytes yield U+FFFD).
u32 nextCodepoint(std::string_view s, size_t& i);
void appendUtf8(std::string& s, u32 cp);

}  // namespace ui
