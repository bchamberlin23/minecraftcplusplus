#pragma once
// GUI sprites (textures/gui/sprites/**, with their .mcmeta GUI scaling) and whole GUI textures
// (container backgrounds and the like), packed into the GUI atlas, and the game's ways to draw them.
#include "ui/canvas.h"
#include "ui/resources.h"

#include <unordered_map>

namespace ui {

struct Sprite {
    enum Scaling { Stretch, Tile, NineSlice } scaling = Stretch;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    int w = 0, h = 0;          // source pixels
    int sw = 0, sh = 0;        // GUI size of the scaling (tile / nine slice width and height)
    int border[4] = {};        // left top right bottom
    bool stretchInner = false;
};

class Sprites {
public:
    bool load(const Resources& res, Atlas& atlas);
    // "hud/hotbar" (textures/gui/sprites/hud/hotbar.png). nullptr if missing.
    const Sprite* sprite(std::string_view name) const;
    // Any texture by resource path below textures/: "gui/container/inventory", loaded on first use.
    const Sprite* texture(const std::string& name);
    // Any image by full resource path ("assets/ns/textures/block/stone.png"), loaded on first use.
    const Sprite* textureAt(const std::string& path);
    // A decoded image (a downloaded skin) under `key`, replacing nothing already there.
    const Sprite* addImage(const std::string& key, const Image& img);
    // Animated sprites and textures (.mcmeta "animation", SpriteContents.AnimatedTexture) show their frame for this
    // client tick (blended with the next one when interpolated); call once per frame.
    void tick(u64 tick);
    // Server resource packs: re-reads everything loaded so far from the asset stack (in place where sizes match).
    void reload();

    // A .mcmeta animation: frames of fw x fh in the strip, the (frame, ticks) sequence.
    struct Animation {
        int fw = 0, fh = 0, cols = 1;
        std::vector<std::pair<int, int>> sequence;
        int totalTicks = 0;
        bool interpolate = false;
        // The pixels shown at `tick` (fw x fh RGBA).
        void frameAt(const Image& strip, u64 tick, std::vector<u8>& out) const;
        // Identifies what is shown at `tick`: changes whenever the pixels do.
        u64 stateAt(u64 tick) const;
    };
    // False when the image is not animated (no "animation" section or a single frame).
    static bool parseAnimation(const Image& img, const nlohmann::json& meta, Animation& out);

private:
    bool add(const std::string& key, const Image& img, const nlohmann::json& meta);
    static void scaling(const nlohmann::json& meta, Sprite& s);  // the .mcmeta "gui" scaling
    // Atlas space for img (its first frame when animated, registering the animation). False when the atlas is full.
    bool place(const Image& img, const nlohmann::json& meta, Sprite& s);
    const Resources* res_ = nullptr;
    Atlas* atlas_ = nullptr;
    std::unordered_map<std::string, Sprite> sprites_, textures_;
    struct Animated {
        Animation anim;
        Image strip;
        int x = 0, y = 0;
        u64 shown = ~u64(0);
    };
    std::vector<Animated> animated_;
};

// GuiGraphics.blitSprite: the whole sprite scaled into the rectangle per its GUI scaling.
void blitSprite(Canvas& c, const Sprite* s, float x, float y, float w, float h, u32 color = 0xFFFFFFFF);
// Part of a sprite whose logical size is texW x texH: the (srcX, srcY, w, h) region drawn 1:1 at (x, y).
void blitSpriteRegion(Canvas& c, const Sprite* s, int texW, int texH, int srcX, int srcY, float x, float y, float w,
                      float h, u32 color = 0xFFFFFFFF);
// GuiGraphics.blit of a plain texture: region (u, v, w, h) of a texW x texH texture at (x, y).
void blitTexture(Canvas& c, const Sprite* s, float x, float y, float u, float v, float w, float h, int texW, int texH,
                 u32 color = 0xFFFFFFFF);

}  // namespace ui
