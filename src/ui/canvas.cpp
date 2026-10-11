#include "ui/canvas.h"

namespace ui {

void Atlas::init(int w, int h) {
    w_ = w, h_ = h;
    pixels_.assign(size_t(w) * h * 4, 0);
    reset();
    dirty0_ = 0, dirty1_ = h;
}

void Atlas::reset() {
    shelfX_ = shelfY_ = shelfH_ = 0;
    full_ = false;
}

bool Atlas::alloc(int w, int h, int& x, int& y) {
    const int W = w + 2, H = h + 2;
    if (W > w_) return false;
    if (shelfX_ + W > w_) {
        shelfY_ += shelfH_;
        shelfX_ = shelfH_ = 0;
    }
    if (shelfY_ + H > h_) {
        full_ = true;
        return false;
    }
    x = shelfX_ + 1, y = shelfY_ + 1;
    shelfX_ += W;
    shelfH_ = std::max(shelfH_, H);
    return true;
}

void Atlas::blit(int x, int y, int w, int h, const u8* rgba, int srcStride) {
    for (int r = 0; r < h; ++r) std::memcpy(pixel(x, y + r), rgba + size_t(r) * srcStride, size_t(w) * 4);
    markDirty(y, y + h);
}

void Atlas::markDirty(int y0, int y1) {
    y0 = std::max(0, y0 - 1), y1 = std::min(h_, y1 + 1);  // include the gutters
    if (dirty1_ <= dirty0_) dirty0_ = y0, dirty1_ = y1;
    else dirty0_ = std::min(dirty0_, y0), dirty1_ = std::max(dirty1_, y1);
}

bool Atlas::takeDirty(int& y0, int& y1, int maxRows) {
    if (dirty1_ <= dirty0_) return false;
    y0 = dirty0_;
    y1 = std::min(dirty1_, dirty0_ + maxRows);
    dirty0_ = y1;
    if (dirty0_ >= dirty1_) dirty0_ = dirty1_ = 0;
    return true;
}

void Canvas::begin(u32 widthPx, u32 heightPx, int guiScale) {
    pxW_ = widthPx, pxH_ = heightPx;
    scale_ = std::max(1, guiScale);
    guiW_ = int((widthPx + u32(scale_) - 1) / u32(scale_));
    guiH_ = int((heightPx + u32(scale_) - 1) / u32(scale_));
    pose_ = {};
    poses_.clear();
    scissors_.clear();
    verts_.clear();
    batches_.clear();
    blend_ = 0;
    newBatch();
}

void Canvas::push() { poses_.push_back(pose_); }
void Canvas::pop() {
    if (poses_.empty()) return;
    pose_ = poses_.back();
    poses_.pop_back();
}
void Canvas::translate(float x, float y) {
    pose_.tx += x * pose_.sx;
    pose_.ty += y * pose_.sy;
}
void Canvas::scaleBy(float s) { scaleBy(s, s); }
void Canvas::scaleBy(float sx, float sy) {
    pose_.sx *= sx;
    pose_.sy *= sy;
}

void Canvas::vertex(float x, float y, float u, float v, u32 abgr, u32 mode) {
    UiVertex vt;
    point(x, y, vt.x, vt.y);
    vt.u = u, vt.v = v, vt.color = abgr, vt.mode = mode;
    verts_.push_back(vt);
}

void Canvas::fill(float x0, float y0, float x1, float y1, u32 c) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    quad(x0, y0, x1, y1, 0, 0, 0, 0, c, ModeSolid);
}

void Canvas::gradient(float x0, float y0, float x1, float y1, u32 top, u32 bottom) {
    if (!alphaOf(top) && !alphaOf(bottom)) return;
    const u32 t = argbToAbgr(top), b = argbToAbgr(bottom);
    vertex(x0, y0, 0, 0, t, ModeSolid), vertex(x0, y1, 0, 0, b, ModeSolid), vertex(x1, y1, 0, 0, b, ModeSolid);
    vertex(x0, y0, 0, 0, t, ModeSolid), vertex(x1, y1, 0, 0, b, ModeSolid), vertex(x1, y0, 0, 0, t, ModeSolid);
    batches_.back().count += 6;
}

void Canvas::quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 c, TexMode mode) {
    if (!alphaOf(c)) return;
    const float xy[4][2] = {{x0, y0}, {x0, y1}, {x1, y1}, {x1, y0}};
    const float uv[4][2] = {{u0, v0}, {u0, v1}, {u1, v1}, {u1, v0}};
    quad4(xy, uv, c, mode);
}

void Canvas::quad4(const float xy[4][2], const float uv[4][2], u32 c, TexMode mode) {
    if (!alphaOf(c)) return;
    const u32 abgr = argbToAbgr(c);
    for (int k : {0, 1, 2, 0, 2, 3}) vertex(xy[k][0], xy[k][1], uv[k][0], uv[k][1], abgr, mode);
    batches_.back().count += 6;
}

void Canvas::newBatch() {
    Batch b;
    b.first = u32(verts_.size());
    b.blend = blend_;
    if (!scissors_.empty()) {
        b.clip = true;
        std::copy(scissors_.back().begin(), scissors_.back().end(), b.scissor);
    }
    if (!batches_.empty() && batches_.back().count == 0) batches_.back() = b;
    else batches_.push_back(b);
}

void Canvas::pushScissor(float x0, float y0, float x1, float y1) {
    float px0, py0, px1, py1;
    point(x0, y0, px0, py0);
    point(x1, y1, px1, py1);
    std::array<int, 4> r = {int(std::floor(px0)), int(std::floor(py0)), int(std::ceil(px1)), int(std::ceil(py1))};
    if (!scissors_.empty()) {
        const auto& o = scissors_.back();
        r = {std::max(r[0], o[0]), std::max(r[1], o[1]), std::min(r[2], o[2]), std::min(r[3], o[3])};
    }
    r[2] = std::max(r[2], r[0]), r[3] = std::max(r[3], r[1]);
    scissors_.push_back(r);
    newBatch();
}

void Canvas::setBlend(u8 mode) {
    if (mode == blend_) return;
    blend_ = mode;
    newBatch();
}

void Canvas::popScissor() {
    if (!scissors_.empty()) scissors_.pop_back();
    newBatch();
}

}  // namespace ui
