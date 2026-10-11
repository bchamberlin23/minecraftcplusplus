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

private:
    bool add(const std::string& key, const Image& img, const nlohmann::json& meta);
    const Resources* res_ = nullptr;
    Atlas* atlas_ = nullptr;
    std::unordered_map<std::string, Sprite> sprites_, textures_;
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
