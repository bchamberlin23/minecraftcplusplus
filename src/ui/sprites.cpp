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

bool Sprites::parseAnimation(const Image& img, const nlohmann::json& meta, Animation& out) {
    if (!meta.is_object() || !meta.contains("animation") || !meta["animation"].is_object()) return false;
    const nlohmann::json& a = meta["animation"];
    // Frame size: "width" / "height", else square frames of the strip's smaller side.
    int fw = a.value("width", -1), fh = a.value("height", -1);
    if (fw <= 0 && fh <= 0) fw = fh = std::min(img.w, img.h);
    else if (fw <= 0) fw = fh;
    else if (fh <= 0) fh = fw;
    fw = std::min(fw, img.w), fh = std::min(fh, img.h);
    if (fw <= 0 || fh <= 0) return false;
    out.fw = fw, out.fh = fh, out.cols = std::max(1, img.w / fw);
    const int count = out.cols * std::max(1, img.h / fh);
    const int frametime = std::max(1, a.value("frametime", 1));
    out.interpolate = a.value("interpolate", false);
    out.sequence.clear();
    if (a.contains("frames") && a["frames"].is_array() && !a["frames"].empty()) {
        for (const nlohmann::json& f : a["frames"]) {
            const int index = f.is_object() ? f.value("index", 0) : f.is_number() ? f.get<int>() : 0;
            const int time = f.is_object() ? std::max(1, f.value("time", frametime)) : frametime;
            out.sequence.push_back({std::clamp(index, 0, count - 1), time});
        }
    } else {
        for (int k = 0; k < count; ++k) out.sequence.push_back({k, frametime});
    }
    out.totalTicks = 0;
    for (const auto& [f, t] : out.sequence) out.totalTicks += t;
    return out.sequence.size() > 1;
}

u64 Sprites::Animation::stateAt(u64 tick) const {
    u64 t = tick % u64(std::max(1, totalTicks));
    for (size_t i = 0; i < sequence.size(); ++i) {
        const u64 time = u64(sequence[i].second);
        if (t < time) return interpolate ? (u64(i) << 32 | t) : u64(sequence[i].first);
        t -= time;
    }
    return 0;
}

void Sprites::Animation::frameAt(const Image& strip, u64 tick, std::vector<u8>& out) const {
    u64 t = tick % u64(std::max(1, totalTicks));
    size_t i = 0;
    for (; i + 1 < sequence.size() && t >= u64(sequence[i].second); ++i) t -= u64(sequence[i].second);
    out.resize(size_t(fw) * fh * 4);
    auto row = [&](int frame, int y) { return strip.at((frame % cols) * fw, (frame / cols) * fh + y); };
    const int cur = sequence[i].first;
    if (!interpolate) {
        for (int y = 0; y < fh; ++y) std::memcpy(&out[size_t(y) * fw * 4], row(cur, y), size_t(fw) * 4);
        return;
    }
    // animate_sprite_interpolate.fsh: mix(current, next, subFrame / time).
    const int next = sequence[(i + 1) % sequence.size()].first;
    const float p = float(t) / float(sequence[i].second);
    for (int y = 0; y < fh; ++y) {
        const u8 *a = row(cur, y), *b = row(next, y);
        u8* o = &out[size_t(y) * fw * 4];
        for (int k = 0; k < fw * 4; ++k) o[k] = u8(float(a[k]) + (float(b[k]) - float(a[k])) * p + 0.5f);
    }
}

bool Sprites::place(const Image& img, const nlohmann::json& meta, Sprite& s) {
    Animation anim;
    const bool animated = parseAnimation(img, meta, anim);
    const int w = animated ? anim.fw : img.w, h = animated ? anim.fh : img.h;
    int x, y;
    if (!atlas_->alloc(w, h, x, y)) return false;
    if (animated) {
        Animated a{std::move(anim), img, x, y};
        std::vector<u8> first;
        a.anim.frameAt(a.strip, 0, first);
        atlas_->blit(x, y, w, h, first.data(), w * 4);
        a.shown = a.anim.stateAt(0);
        animated_.push_back(std::move(a));
    } else {
        atlas_->blit(x, y, w, h, img.rgba.data(), w * 4);
    }
    s.w = s.sw = w, s.h = s.sh = h;
    const float W = float(atlas_->width()), H = float(atlas_->height());
    s.u0 = float(x) / W, s.v0 = float(y) / H, s.u1 = float(x + w) / W, s.v1 = float(y + h) / H;
    return true;
}

void Sprites::tick(u64 tick) {
    std::vector<u8> pixels;
    for (Animated& a : animated_) {
        const u64 state = a.anim.stateAt(tick);
        if (state == a.shown) continue;
        a.shown = state;
        a.anim.frameAt(a.strip, tick, pixels);
        atlas_->blit(a.x, a.y, a.anim.fw, a.anim.fh, pixels.data(), a.anim.fw * 4);
    }
}

bool Sprites::add(const std::string& key, const Image& img, const nlohmann::json& meta) {
    Sprite s;
    if (!place(img, meta, s)) {
        LOG("ui: GUI atlas full at %s", key.c_str());
        return false;
    }
    if (meta.is_object() && meta.contains("gui") && meta["gui"].contains("scaling")) {
        const nlohmann::json& sc = meta["gui"]["scaling"];
        const std::string type = sc.value("type", "stretch");
        s.sw = sc.value("width", s.w), s.sh = sc.value("height", s.h);
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
    const std::string path = "assets/minecraft/textures/" + name + ".png";
    const nlohmann::json meta = res_->exists(path + ".mcmeta") ? res_->json(path + ".mcmeta") : nlohmann::json();
    if (!res_->image(path, img) || !place(img, meta, s)) {
        LOG("ui: texture %s unavailable", name.c_str());
        return nullptr;
    }
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
