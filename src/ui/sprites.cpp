#include "ui/sprites.h"

namespace ui {

bool Sprites::load(const Resources& res, Atlas& atlas) {
    res_ = &res;
    atlas_ = &atlas;
    const std::string prefix = "assets/minecraft/textures/gui/sprites/";
    std::vector<std::string> files = res.list(prefix);
    std::sort(files.begin(), files.end());
    int count = 0;
    for (const std::string& f : files) {
        if (!f.ends_with(".png")) continue;
        Image img;
        if (!res.image(f, img)) continue;
        const nlohmann::json meta = res.exists(f + ".mcmeta") ? res.json(f + ".mcmeta") : nlohmann::json();
        count += add(f.substr(prefix.size(), f.size() - prefix.size() - 4), img, meta);
    }
    LOG("ui: %d GUI sprites", count);
    return count > 0;
}

bool Sprites::add(const std::string& key, const Image& img0, const nlohmann::json& meta) {
    Image img = img0;
    if (meta.is_object() && meta.contains("animation") && img.h > img.w) {  // animated strip: first frame
        const nlohmann::json& a = meta["animation"];
        const int fw = a.value("width", img.w), fh = a.value("height", fw);
        Image f;
        f.w = std::min(fw, img.w), f.h = std::min(fh, img.h);
        f.rgba.resize(size_t(f.w) * f.h * 4);
        for (int y = 0; y < f.h; ++y) std::memcpy(&f.rgba[size_t(y) * f.w * 4], img.at(0, y), size_t(f.w) * 4);
        img = std::move(f);
    }
    int x, y;
    if (!atlas_->alloc(img.w, img.h, x, y)) {
        LOG("ui: GUI atlas full at %s", key.c_str());
        return false;
    }
    atlas_->blit(x, y, img.w, img.h, img.rgba.data(), img.w * 4);
    Sprite s;
    s.w = img.w, s.h = img.h;
    s.sw = img.w, s.sh = img.h;
    const float W = float(atlas_->width()), H = float(atlas_->height());
    s.u0 = float(x) / W, s.v0 = float(y) / H, s.u1 = float(x + img.w) / W, s.v1 = float(y + img.h) / H;
    if (meta.is_object() && meta.contains("gui") && meta["gui"].contains("scaling")) {
        const nlohmann::json& sc = meta["gui"]["scaling"];
        const std::string type = sc.value("type", "stretch");
        s.sw = sc.value("width", img.w), s.sh = sc.value("height", img.h);
        if (type == "tile") s.scaling = Sprite::Tile;
        if (type == "nine_slice") {
            s.scaling = Sprite::NineSlice;
            s.stretchInner = sc.value("stretch_inner", false);
            const nlohmann::json& b = sc["border"];
            if (b.is_number()) {
                std::fill(s.border, s.border + 4, b.get<int>());
            } else if (b.is_object()) {
                s.border[0] = b.value("left", 0), s.border[1] = b.value("top", 0);
                s.border[2] = b.value("right", 0), s.border[3] = b.value("bottom", 0);
            }
        }
    }
    sprites_[key] = s;
    return true;
}

const Sprite* Sprites::sprite(std::string_view name) const {
    auto it = sprites_.find(std::string(name));
    return it == sprites_.end() ? nullptr : &it->second;
}

const Sprite* Sprites::texture(const std::string& name) {
    if (auto it = textures_.find(name); it != textures_.end()) return it->second.w ? &it->second : nullptr;
    Sprite& s = textures_[name];
    Image img;
    int x, y;
    if (!res_->image("assets/minecraft/textures/" + name + ".png", img) || !atlas_->alloc(img.w, img.h, x, y)) {
        LOG("ui: texture %s unavailable", name.c_str());
        return nullptr;
    }
    atlas_->blit(x, y, img.w, img.h, img.rgba.data(), img.w * 4);
    s.w = s.sw = img.w, s.h = s.sh = img.h;
    const float W = float(atlas_->width()), H = float(atlas_->height());
    s.u0 = float(x) / W, s.v0 = float(y) / H, s.u1 = float(x + img.w) / W, s.v1 = float(y + img.h) / H;
    return &s;
}

namespace {
// Region (fu0..fu1, fv0..fv1 as fractions of the sprite) into a rectangle.
void part(Canvas& c, const Sprite* s, float x0, float y0, float x1, float y1, float fu0, float fv0, float fu1, float fv1,
          u32 color) {
    if (x1 <= x0 || y1 <= y0) return;
    const float du = s->u1 - s->u0, dv = s->v1 - s->v0;
    c.quad(x0, y0, x1, y1, s->u0 + du * fu0, s->v0 + dv * fv0, s->u0 + du * fu1, s->v0 + dv * fv1, color, ModeGui);
}

// Tiles the sprite region (in logical pixels tx, ty, tw, th of a sw x sh sprite) over a rectangle.
void tiled(Canvas& c, const Sprite* s, float x, float y, float w, float h, int tx, int ty, int tw, int th, u32 color) {
    if (w <= 0 || h <= 0 || tw <= 0 || th <= 0) return;
    for (float py = 0; py < h; py += float(th)) {
        const float ph = std::min(float(th), h - py);
        for (float px = 0; px < w; px += float(tw)) {
            const float pw = std::min(float(tw), w - px);
            part(c, s, x + px, y + py, x + px + pw, y + py + ph, float(tx) / s->sw, float(ty) / s->sh,
                 (float(tx) + pw) / s->sw, (float(ty) + ph) / s->sh, color);
        }
    }
}

void inner(Canvas& c, const Sprite* s, float x, float y, float w, float h, int tx, int ty, int tw, int th, u32 color) {
    if (s->stretchInner)
        part(c, s, x, y, x + w, y + h, float(tx) / s->sw, float(ty) / s->sh, float(tx + tw) / s->sw, float(ty + th) / s->sh,
             color);
    else
        tiled(c, s, x, y, w, h, tx, ty, tw, th, color);
}
}  // namespace

void blitSprite(Canvas& c, const Sprite* s, float x, float y, float w, float h, u32 color) {
    if (!s || w <= 0 || h <= 0) return;
    if (s->scaling == Sprite::Stretch) {
        part(c, s, x, y, x + w, y + h, 0, 0, 1, 1, color);
    } else if (s->scaling == Sprite::Tile) {
        tiled(c, s, x, y, w, h, 0, 0, s->sw, s->sh, color);
    } else {
        const int W = s->sw, H = s->sh;
        if (int(w) == W && int(h) == H) return part(c, s, x, y, x + w, y + h, 0, 0, 1, 1, color);
        const int l = std::min(s->border[0], int(w) / 2), r = std::min(s->border[2], int(w) / 2);
        const int t = std::min(s->border[1], int(h) / 2), b = std::min(s->border[3], int(h) / 2);
        const float xs[4] = {x, x + l, x + w - r, x + w}, ys[4] = {y, y + t, y + h - b, y + h};
        const int us[4] = {0, l, W - r, W}, vs[4] = {0, t, H - b, H};
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i) {
                const float dw = xs[i + 1] - xs[i], dh = ys[j + 1] - ys[j];
                const int sw = us[i + 1] - us[i], sh = vs[j + 1] - vs[j];
                if (i != 1 && j != 1) part(c, s, xs[i], ys[j], xs[i + 1], ys[j + 1], float(us[i]) / W, float(vs[j]) / H,
                                           float(us[i + 1]) / W, float(vs[j + 1]) / H, color);
                else inner(c, s, xs[i], ys[j], dw, dh, us[i], vs[j], sw, sh, color);
            }
    }
}

void blitSpriteRegion(Canvas& c, const Sprite* s, int texW, int texH, int srcX, int srcY, float x, float y, float w,
                      float h, u32 color) {
    if (!s || w <= 0 || h <= 0) return;
    part(c, s, x, y, x + w, y + h, float(srcX) / texW, float(srcY) / texH, (float(srcX) + w) / texW,
         (float(srcY) + h) / texH, color);
}

void blitTexture(Canvas& c, const Sprite* s, float x, float y, float u, float v, float w, float h, int texW, int texH,
                 u32 color) {
    if (!s) return;
    part(c, s, x, y, x + w, y + h, u / texW, v / texH, (u + w) / texW, (v + h) / texH, color);
}

}  // namespace ui
