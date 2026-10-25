p = 'src/ui/icons.cpp'
s = open(p, encoding='utf-8').read()
def rep(a, b):
    global s
    assert a in s, a[:100]
    s = s.replace(a, b, 1)

rep('#include "ui/icons.h"\n', '#include "ui/icons.h"\n\n#include "ui/sprites.h"\n')
rep("""struct Tex {
    int w = 0, h = 0;
    std::vector<u8> rgba;
    bool translucent = false;
};""", """struct Tex {
    int w = 0, h = 0;
    std::vector<u8> rgba;  // the frame shown
    bool translucent = false;
    // Animated (.mcmeta): the strip and the frame (state) in rgba.
    bool animated = false;
    Sprites::Animation anim;
    Image strip;
    u64 shown = 0;
};""")
rep("""    std::unordered_map<u64, std::array<float, 4>> icons;  // key -> uv (u0 < 0: nothing)""",
"""    std::unordered_map<u64, std::array<float, 4>> icons;  // key -> uv (u0 < 0: nothing)
    // Icons drawn with animated textures: rasterized again into their atlas slot when a frame changes.
    struct AnimatedIcon {
        ItemStack stack;
        int x, y;
    };
    std::vector<AnimatedIcon> animatedIcons;
    bool touchedAnimated = false;  // set by texture() while an icon is rasterized""")
rep("""        auto it = texCache.find(key);
        if (it != texCache.end()) return it->second;
        Tex& t = texCache[key];
        Image img;
        if (res->image("assets/minecraft/textures/" + key + ".png", img) && img.w > 0) {
            t.w = img.w, t.h = std::min(img.h, img.w);  // animated strips: first frame
            t.rgba.assign(img.rgba.begin(), img.rgba.begin() + size_t(t.w) * t.h * 4);
            for (size_t i = 3; i < t.rgba.size(); i += 4) t.translucent |= t.rgba[i] > 25 && t.rgba[i] < 230;
        }
        return t;""", """        auto it = texCache.find(key);
        if (it != texCache.end()) {
            touchedAnimated |= it->second.animated;
            return it->second;
        }
        Tex& t = texCache[key];
        Image img;
        const std::string path = "assets/minecraft/textures/" + key + ".png";
        if (res->image(path, img) && img.w > 0) {
            if (res->exists(path + ".mcmeta") && Sprites::parseAnimation(img, res->json(path + ".mcmeta"), t.anim)) {
                t.animated = touchedAnimated = true;
                t.w = t.anim.fw, t.h = t.anim.fh;
                t.anim.frameAt(img, 0, t.rgba);
                t.shown = t.anim.stateAt(0);
                for (size_t i = 3; i < img.rgba.size(); i += 4) t.translucent |= img.rgba[i] > 25 && img.rgba[i] < 230;
                t.strip = std::move(img);
                return t;
            }
            t.w = img.w, t.h = std::min(img.h, img.w);  // strips without .mcmeta: first frame
            t.rgba.assign(img.rgba.begin(), img.rgba.begin() + size_t(t.w) * t.h * 4);
            for (size_t i = 3; i < t.rgba.size(); i += 4) t.translucent |= t.rgba[i] > 25 && t.rgba[i] < 230;
        }
        return t;""")
rep("""    bool render(const ItemStack& s, float uv[4]) {
        const std::string name = strip(items->name(s.item));
        const json& def = jsonAt("assets/minecraft/items/" + name + ".json");
        std::vector<Part> parts;
        if (def.is_object()) evaluate(def.value("model", json()), s, parts, 0);
        Raster r;
        r.init(size);
        bool any = false;""", """    bool rasterize(const ItemStack& s, Raster& r) {
        const std::string name = strip(items->name(s.item));
        const json& def = jsonAt("assets/minecraft/items/" + name + ".json");
        std::vector<Part> parts;
        if (def.is_object()) evaluate(def.value("model", json()), s, parts, 0);
        r.init(size);
        bool any = false;""")
rep("""            drawModel(r, *m, p.tints);
            any = true;
        }
        if (!any) return false;
        int x, y;
        if (!atlas->alloc(size, size, x, y)) {
            if (!atlas->full()) return false;
            atlas->reset();  // full: start over (icons re-render on demand)
            icons.clear();
            if (!atlas->alloc(size, size, x, y)) return false;
        }
        for (int py = 0; py < size; ++py)""", """            drawModel(r, *m, p.tints);
            any = true;
        }
        return any;
    }

    bool render(const ItemStack& s, float uv[4]) {
        Raster r;
        touchedAnimated = false;
        if (!rasterize(s, r)) return false;
        int x, y;
        if (!atlas->alloc(size, size, x, y)) {
            if (!atlas->full()) return false;
            atlas->reset();  // full: start over (icons re-render on demand)
            icons.clear(), animatedIcons.clear();
            if (!atlas->alloc(size, size, x, y)) return false;
        }
        if (touchedAnimated) animatedIcons.push_back({s, x, y});
        write(r, x, y);
        const float W = float(atlas->width()), H = float(atlas->height());
        uv[0] = float(x) / W, uv[1] = float(y) / H, uv[2] = float(x + size) / W, uv[3] = float(y + size) / H;
        return true;
    }

    void write(const Raster& r, int x, int y) {
        for (int py = 0; py < size; ++py)""")
rep("""        atlas->markDirty(y, y + size);
        const float W = float(atlas->width()), H = float(atlas->height());
        uv[0] = float(x) / W, uv[1] = float(y) / H, uv[2] = float(x + size) / W, uv[3] = float(y + size) / H;
        return true;
    }
};""", """        atlas->markDirty(y, y + size);
    }
};""")
rep("""void ItemIcons::setScale(int guiScale) {
    const int size = std::clamp(16 * guiScale, 16, 160);
    if (size == size_) return;
    size_ = impl_->size = size;
    impl_->icons.clear();
    impl_->atlas->reset();
}""", """void ItemIcons::setScale(int guiScale) {
    const int size = std::clamp(16 * guiScale, 16, 160);
    if (size == size_) return;
    size_ = impl_->size = size;
    impl_->icons.clear(), impl_->animatedIcons.clear();
    impl_->atlas->reset();
}

void ItemIcons::tick(u64 tick) {
    Impl& m = *impl_;
    bool changed = false;
    for (auto& [key, t] : m.texCache) {
        if (!t.animated) continue;
        const u64 state = t.anim.stateAt(tick);
        if (state == t.shown) continue;
        t.shown = state;
        t.anim.frameAt(t.strip, tick, t.rgba);
        changed = true;
    }
    if (!changed) return;
    m.size = size_;
    for (const Impl::AnimatedIcon& a : m.animatedIcons) {
        Impl::Raster r;
        if (m.rasterize(a.stack, r)) m.write(r, a.x, a.y);
    }
}""")
open(p, 'w', encoding='utf-8').write(s)

p = 'src/ui/icons.h'
s = open(p, encoding='utf-8').read()
rep("""    // Atlas UVs of the icon (u0 v0 u1 v1); false when the item draws nothing.
    bool icon(const ItemStack& s, float uv[4]);""", """    // Atlas UVs of the icon (u0 v0 u1 v1); false when the item draws nothing.
    bool icon(const ItemStack& s, float uv[4]);
    // Icons with animated textures (prismarine, magma, sea lantern...) are redrawn when a frame changes.
    void tick(u64 tick);""")
open(p, 'w', encoding='utf-8').write(s)

p = 'src/ui/ui.cpp'
s = open(p, encoding='utf-8').read()
rep("""    s.sprites.tick(u64(s.guiTicks));  // animated GUI sprites and textures""", """    s.sprites.tick(u64(s.guiTicks));  // animated GUI sprites and textures, item icons
    s.icons.tick(u64(s.guiTicks));""")
open(p, 'w', encoding='utf-8').write(s)
print('ok')
