#pragma once
// CPU side of UI drawing: texture atlases (RGBA8, shelf packed, uploaded in dirty row ranges) and the
// per-frame draw list in GUI coordinates (the game's GUI-scaled pixels), turned into screen pixels here.
#include "core/common.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ui {

class Atlas {
public:
    void init(int w, int h);
    void reset();  // forget all allocations (pixels stay until overwritten)
    // Reserves w x h pixels (1 px gutter around). false when full.
    bool alloc(int w, int h, int& x, int& y);
    u8* pixel(int x, int y) { return &pixels_[(size_t(y) * w_ + x) * 4]; }
    void blit(int x, int y, int w, int h, const u8* rgba, int srcStride);  // copies + marks dirty
    void markDirty(int y0, int y1);
    // Dirty row range to upload [y0, y1); clears it. false if nothing changed.
    bool takeDirty(int& y0, int& y1, int maxRows);
    int width() const { return w_; }
    int height() const { return h_; }
    const u8* data() const { return pixels_.data(); }
    bool full() const { return full_; }

private:
    int w_ = 0, h_ = 0;
    std::vector<u8> pixels_;
    int shelfX_ = 0, shelfY_ = 0, shelfH_ = 0;
    int dirty0_ = 0, dirty1_ = 0;
    bool full_ = false;
};

enum TexMode : u32 {
    ModeSolid = 0,  // vertex colour only
    ModeGui = 1,    // GUI atlas (sprites, fonts), point sampled
    ModeIcon = 2,   // item icon atlas, point sampled
    ModeGlint = 3,  // enchantment glint masked by an item icon (use with blend 2)
};

struct UiVertex {
    float x, y;  // screen pixels
    float u, v;
    u32 color;  // 0xAABBGGRR
    u32 mode;
};

// Draw list for one frame. Colours are the game's ARGB ints (0xAARRGGBB).
class Canvas {
public:
    struct Batch {
        u32 first = 0, count = 0;
        bool clip = false;
        u8 blend = 0;  // 0 alpha, 1 invert (one minus destination colour: crosshair), 2 glint (src * src + dst)
        int scissor[4] = {0, 0, 0, 0};  // screen pixels x0 y0 x1 y1 when clip
    };

    void begin(u32 widthPx, u32 heightPx, int guiScale);
    int scale() const { return scale_; }
    int guiWidth() const { return guiW_; }
    int guiHeight() const { return guiH_; }
    u32 pixelWidth() const { return pxW_; }
    u32 pixelHeight() const { return pxH_; }

    // Pose: GUI coordinates are mapped by an affine transform (in GUI units) before the GUI scale.
    void push();
    void pop();
    void translate(float x, float y);
    void scaleBy(float s);
    void scaleBy(float sx, float sy);
    void rotate(float radians);  // clockwise on screen (y points down), like the game's pose rotate

    void fill(float x0, float y0, float x1, float y1, u32 argb);
    void gradient(float x0, float y0, float x1, float y1, u32 top, u32 bottom);
    // Textured rectangle; uv in 0..1 of the atlas selected by mode.
    void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 argb, TexMode mode);
    // General quad (corners in order top-left, bottom-left, bottom-right, top-right).
    void quad4(const float xy[4][2], const float uv[4][2], u32 argb, TexMode mode);
    // Clip subsequent drawing to a GUI rectangle (intersected with the current one); stack.
    void pushScissor(float x0, float y0, float x1, float y1);
    void popScissor();
    void setInvert(bool on) { setBlend(on ? 1 : 0); }
    void setBlend(u8 mode);

    const std::vector<UiVertex>& vertices() const { return verts_; }
    const std::vector<Batch>& batches() const { return batches_; }

private:
    struct Pose {
        float m00 = 1, m01 = 0, m10 = 0, m11 = 1, tx = 0, ty = 0;  // [m00 m01; m10 m11] * (x, y) + t
    };
    void point(float x, float y, float& px, float& py) const {
        px = (pose_.m00 * x + pose_.m01 * y + pose_.tx) * float(scale_);
        py = (pose_.m10 * x + pose_.m11 * y + pose_.ty) * float(scale_);
    }
    void vertex(float x, float y, float u, float v, u32 abgr, u32 mode);
    void newBatch();

    u32 pxW_ = 0, pxH_ = 0;
    int scale_ = 1, guiW_ = 0, guiH_ = 0;
    Pose pose_;
    std::vector<Pose> poses_;
    std::vector<std::array<int, 4>> scissors_;
    std::vector<UiVertex> verts_;
    std::vector<Batch> batches_;
    u8 blend_ = 0;
};

inline u32 argbToAbgr(u32 c) { return (c & 0xFF00FF00) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF); }
inline u32 argb(int a, int r, int g, int b) { return (u32(a) << 24) | (u32(r) << 16) | (u32(g) << 8) | u32(b); }
inline u32 withAlpha(u32 rgb, float a) {
    const int ai = int(std::floor(std::clamp(a, 0.0f, 1.0f) * 255.0f));  // ARGB.as8BitChannel
    return (u32(ai) << 24) | (rgb & 0xFFFFFF);
}
inline u32 scaleRgb(u32 c, float f) {
    const u32 r = u32(float((c >> 16) & 255) * f), g = u32(float((c >> 8) & 255) * f), b = u32(float(c & 255) * f);
    return (c & 0xFF000000) | (r << 16) | (g << 8) | b;
}
inline u32 alphaOf(u32 c) { return c >> 24; }

}  // namespace ui
