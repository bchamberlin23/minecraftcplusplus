#pragma once
// The game's fonts from its own resources (FontManager / FontSet): every font a style can name ("minecraft:default",
// "minecraft:alt", pack fonts), built from the providers of its font JSON: bitmap sheets, space advances, GNU Unifont
// (unihex) and TrueType (stb_truetype), with the provider filters of the font options (Force Unicode Font, Japanese
// Glyph Variants). Glyph metrics, shadows, bold, italic, underline, strikethrough and obfuscated text as the game
// draws them; text components' "object" contents (atlas sprites, player heads) as 8x8 glyphs; bidirectional text
// (ui/bidi.h) in visual order with Arabic shaping.
#include "ui/canvas.h"
#include "ui/resources.h"
#include "ui/text.h"

#include <functional>
#include <future>
#include <map>
#include <memory>
#include <random>
#include <unordered_map>

namespace ui {

struct Sprite;

struct Glyph {
    float advance = 0;
    float boldOffset = 1, shadowOffset = 1;
    bool visible = false;               // has a bitmap
    float left = 0, right = 0, top = 0, bottom = 0;  // relative to the pen position (GUI units)
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;            // GUI atlas
    bool overlay = false;                            // a second quad over the first (a player head's hat layer)
    float ou0 = 0, ov0 = 0, ou1 = 0, ov1 = 0;
    float advanceFor(bool bold) const { return advance + (bold ? boldOffset : 0); }
};

class Font {
public:
    static constexpr int kLineHeight = 9;

    ~Font();
    // FontOption filters: uniform (Force Unicode Font), jp (Japanese Glyph Variants).
    bool load(const Resources& res, Atlas& atlas, bool uniform = false, bool jp = false);
    // Options or resource packs changed: the fonts are rebuilt (sheets and glyphs already in the atlas are reused).
    void reload(bool uniform, bool jp);
    void setRightToLeft(bool rtl) { rtl_ = rtl; }  // the language's default paragraph direction
    // Sprites for "object" glyphs (atlas sprites, player skins): set by the UI.
    std::function<const Sprite*(const FontDesc& d)> objectSprite;

    const Glyph& glyph(u32 cp);                     // the default font
    const Glyph& glyph(u32 cp, const Style& style);  // the style's font
    // Calls fn(position, style, codepoint) for every visible code point, applying legacy codes.
    void iterate(const Text& t, const std::function<bool(int, const Style&, u32)>& fn) const;

    float widthF(const Text& t);
    int width(const Text& t) { return int(std::ceil(widthF(t))); }
    int width(std::string_view s) { return width(Text(std::string(s))); }
    int width(const std::string& s) { return width(std::string_view(s)); }
    int width(const char* s) { return width(std::string_view(s)); }

    // Draws `t` with its top-left at (x, y); returns the pen x after the text.
    // color: ARGB for runs without a colour; shadow like GuiGraphics.text(..., dropShadow).
    // logical: keep the logical order (EditBox text), else bidirectional text is drawn in visual order.
    float draw(Canvas& c, const Text& t, float x, float y, u32 color, bool shadow, bool logical = false);
    float draw(Canvas& c, std::string_view s, float x, float y, u32 color, bool shadow, bool logical = false) {
        return draw(c, Text(std::string(s)), x, y, color, shadow, logical);
    }
    float draw(Canvas& c, const std::string& s, float x, float y, u32 color, bool shadow, bool logical = false) {
        return draw(c, std::string_view(s), x, y, color, shadow, logical);
    }
    float draw(Canvas& c, const char* s, float x, float y, u32 color, bool shadow, bool logical = false) {
        return draw(c, std::string_view(s), x, y, color, shadow, logical);
    }
    // Word wrap like StringSplitter.splitLines (spaces, newlines, overlong words).
    std::vector<Text> split(const Text& t, int maxWidth);
    // Longest prefix / suffix of plain `s` that fits in `width`.
    std::string headByWidth(std::string_view s, int width);
    std::string tailByWidth(std::string_view s, int width);

private:
    struct Provider;
    struct FontSet {
        std::vector<Provider*> providers;
        std::unordered_map<u32, Glyph> cache;
        std::unordered_map<int, std::vector<u32>> byWidth;  // bitmap glyphs by advance (obfuscated text)
    };
    FontSet& set(int fontIndex);  // -1: minecraft:default
    void build(FontSet& fs, const std::string& fontId, int depth);
    Provider* provider(const std::string& key, const std::function<std::unique_ptr<Provider>()>& make);
    std::unique_ptr<Provider> loadBitmap(const nlohmann::json& p);
    std::unique_ptr<Provider> loadUnihex(const nlohmann::json& p);
    std::unique_ptr<Provider> loadTrueType(const nlohmann::json& p);
    const Glyph* providerGlyph(Provider& p, u32 cp);
    const Glyph& setGlyph(FontSet& fs, u32 cp);
    const Glyph& objectGlyph(int fontIndex);
    const Glyph& randomGlyph(FontSet& fs, int width);

    const Resources* res_ = nullptr;
    Atlas* atlas_ = nullptr;
    bool uniform_ = false, jp_ = false, rtl_ = false;
    std::map<std::string, std::shared_ptr<Provider>> providers_;  // by definition, kept over reloads
    std::unordered_map<std::string, FontSet> sets_;                // by font id
    std::unordered_map<int, Glyph> objects_;                       // object glyphs by font index
    FontSet* default_ = nullptr;
    Glyph missing_;
    std::mt19937 random_{12345};
};

}  // namespace ui
