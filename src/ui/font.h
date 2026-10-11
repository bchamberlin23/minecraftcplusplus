#pragma once
// The game's default font from its own resources: bitmap sheets, space advances and GNU Unifont (unihex)
// for everything else, with the game's glyph metrics, shadows, bold, italic, underline, strikethrough
// and obfuscated text.
#include "ui/canvas.h"
#include "ui/resources.h"
#include "ui/text.h"

#include <functional>
#include <future>
#include <random>
#include <unordered_map>

namespace ui {

struct Glyph {
    float advance = 0;
    float boldOffset = 1, shadowOffset = 1;
    bool visible = false;               // has a bitmap
    float left = 0, right = 0, top = 0, bottom = 0;  // relative to the pen position (GUI units)
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;            // GUI atlas
    float advanceFor(bool bold) const { return advance + (bold ? boldOffset : 0); }
};

class Font {
public:
    static constexpr int kLineHeight = 9;

    ~Font();
    bool load(const Resources& res, Atlas& atlas, const std::string& fontId = "minecraft:default");  // "minecraft:alt"

    const Glyph& glyph(u32 cp);
    // Calls fn(position, style, codepoint) for every visible code point, applying legacy codes.
    void iterate(const Text& t, const std::function<bool(int, const Style&, u32)>& fn) const;

    float widthF(const Text& t);
    int width(const Text& t) { return int(std::ceil(widthF(t))); }
    int width(std::string_view s) { return width(Text(std::string(s))); }
    int width(const std::string& s) { return width(std::string_view(s)); }
    int width(const char* s) { return width(std::string_view(s)); }

    // Draws `t` with its top-left at (x, y); returns the pen x after the text.
    // color: ARGB for runs without a colour; shadow like GuiGraphics.text(..., dropShadow).
    float draw(Canvas& c, const Text& t, float x, float y, u32 color, bool shadow);
    float draw(Canvas& c, std::string_view s, float x, float y, u32 color, bool shadow) {
        return draw(c, Text(std::string(s)), x, y, color, shadow);
    }
    float draw(Canvas& c, const std::string& s, float x, float y, u32 color, bool shadow) {
        return draw(c, std::string_view(s), x, y, color, shadow);
    }
    float draw(Canvas& c, const char* s, float x, float y, u32 color, bool shadow) {
        return draw(c, std::string_view(s), x, y, color, shadow);
    }
    // Word wrap like StringSplitter.splitLines (spaces, newlines, overlong words).
    std::vector<Text> split(const Text& t, int maxWidth);
    // Longest prefix / suffix of plain `s` that fits in `width`.
    std::string headByWidth(std::string_view s, int width);
    std::string tailByWidth(std::string_view s, int width);

private:
    struct Unihex {
        u32 rows[16];
        u8 left, right;  // inclusive bit columns (from the left of a 32-bit row)
    };
    struct Provider {
        enum Type { Bitmap, Space } type;
        std::unordered_map<u32, Glyph> glyphs;
    };
    bool loadProviders(const Resources& res, const std::string& fontId, int depth);
    void loadUnihex(const Resources& res, const nlohmann::json& p);
    const Unihex* unihex(u32 cp);
    Glyph bakeUnihex(const Unihex& u);
    const Glyph& randomGlyph(int width);

    Atlas* atlas_ = nullptr;
    std::vector<Provider> providers_;
    std::unordered_map<u32, Glyph> cache_;
    Glyph missing_;
    // Unifont loads on a background thread; first use waits for it.
    std::future<void> unihexLoad_;
    std::unordered_map<u32, Unihex> unihex_;
    bool unihexReady_ = false;
    std::unordered_map<int, std::vector<u32>> byWidth_;
    std::mt19937 random_{12345};
};

}  // namespace ui
