p = 'src/ui/sprites.h'
s = open(p, encoding='utf-8').read()
a = """    // Any texture by resource path below textures/: "gui/container/inventory", loaded on first use.
    const Sprite* texture(const std::string& name);

private:
    bool add(const std::string& key, const Image& img, const nlohmann::json& meta);
    const Resources* res_ = nullptr;
    Atlas* atlas_ = nullptr;
    std::unordered_map<std::string, Sprite> sprites_, textures_;
};"""
b = """    // Any texture by resource path below textures/: "gui/container/inventory", loaded on first use.
    const Sprite* texture(const std::string& name);
    // Animated sprites and textures (.mcmeta "animation", SpriteContents.AnimatedTexture) show their frame for this
    // client tick (blended with the next one when interpolated); call once per frame.
    void tick(u64 tick);

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
};"""
assert a in s
s = s.replace(a, b)
open(p, 'w', encoding='utf-8').write(s)

p = 'src/ui/sprites.cpp'
s = open(p, encoding='utf-8').read()
a = s[s.index("bool Sprites::add("):s.index("    if (meta.is_object() && meta.contains(\"gui\")")]
b = '''bool Sprites::parseAnimation(const Image& img, const nlohmann::json& meta, Animation& out) {
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
'''
s = s.replace(a, b)
a = '''        s.sw = sc.value("width", img.w), s.sh = sc.value("height", img.h);'''
assert a in s
s = s.replace(a, '''        s.sw = sc.value("width", s.w), s.sh = sc.value("height", s.h);''')
a = '''    Sprite& s = textures_[name];
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
    return &s;'''
b = '''    Sprite& s = textures_[name];
    Image img;
    const std::string path = "assets/minecraft/textures/" + name + ".png";
    const nlohmann::json meta = res_->exists(path + ".mcmeta") ? res_->json(path + ".mcmeta") : nlohmann::json();
    if (!res_->image(path, img) || !place(img, meta, s)) {
        LOG("ui: texture %s unavailable", name.c_str());
        return nullptr;
    }
    return &s;'''
assert a in s
s = s.replace(a, b)
open(p, 'w', encoding='utf-8').write(s)
print("ok")
