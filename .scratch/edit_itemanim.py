p = 'src/entity/item_models.cpp'
s = open(p, encoding='utf-8').read()
def rep(a, b):
    global s
    assert a in s, a[:100]
    s = s.replace(a, b, 1)

rep('#include "ui/resources.h"\n', '#include "ui/resources.h"\n#include "ui/sprites.h"\n')
rep("""    struct Tex {
        int rect = -1;
        ui::Image img;  // first frame
    };""", """    struct Tex {
        int rect = -1;
        ui::Image img;  // first frame
    };
    struct Animated {  // .mcmeta animations (SpriteContents.AnimatedTexture): the current frame is copied in each tick
        ui::Sprites::Animation anim;
        ui::Image strip;
        int x = 0, y = 0;
        u64 shown = 0;
    };
    std::vector<Animated> animated;""")
rep("""        const int w = img.w, h = (img.h > img.w && img.h % img.w == 0) ? img.w : img.h;  // animated strips: first frame
        t.img.w = w, t.img.h = h;
        t.img.rgba.assign(img.rgba.begin(), img.rgba.begin() + size_t(w) * h * 4);
        int x, y;
        if (!self->atlas_.alloc(w, h, x, y)) {
            LOG("visuals: item atlas full (%s not loaded)", key.c_str());
            return nullptr;
        }
        self->atlas_.blit(x, y, w, h, t.img.rgba.data(), w * 4);""", """        const std::string meta = "assets/minecraft/textures/" + key + ".png.mcmeta";
        ui::Sprites::Animation anim;
        const bool animatedTex = res->exists(meta) && ui::Sprites::parseAnimation(img, res->json(meta), anim);
        int w = img.w, h = (img.h > img.w && img.h % img.w == 0) ? img.w : img.h;  // strips without .mcmeta: first frame
        if (animatedTex) {
            w = anim.fw, h = anim.fh;
            anim.frameAt(img, 0, t.img.rgba);
            t.img.w = w, t.img.h = h;
        } else {
            t.img.w = w, t.img.h = h;
            t.img.rgba.assign(img.rgba.begin(), img.rgba.begin() + size_t(w) * h * 4);
        }
        int x, y;
        if (!self->atlas_.alloc(w, h, x, y)) {
            LOG("visuals: item atlas full (%s not loaded)", key.c_str());
            return nullptr;
        }
        self->atlas_.blit(x, y, w, h, t.img.rgba.data(), w * 4);
        if (animatedTex) {
            const u64 shown = anim.stateAt(0);
            animated.push_back({std::move(anim), std::move(img), x, y, shown});
        }""")
open(p, 'w', encoding='utf-8').write(s)
print('ok')
