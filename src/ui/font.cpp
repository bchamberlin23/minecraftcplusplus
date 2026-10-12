#include "ui/font.h"

#include "assets/zip.h"
#include "ui/bidi.h"
#include "ui/sprites.h"

#include <stb/stb_truetype.h>

#include <cmath>
#include <unordered_set>

namespace ui {

namespace {
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::vector<u32> codepoints(const std::string& s) {
    std::vector<u32> out;
    for (size_t i = 0; i < s.size();) out.push_back(nextCodepoint(s, i));
    return out;
}

u32 jsonCodepoint(const nlohmann::json& j) {
    if (!j.is_string()) return 0;
    const std::string s = j.get<std::string>();
    size_t i = 0;
    return s.empty() ? 0 : nextCodepoint(s, i);
}

// "minecraft:font/ascii.png" -> assets/minecraft/textures/font/ascii.png (prefix "textures/")
std::string resourcePath(const std::string& id, const char* prefix) {
    const size_t c = id.find(':');
    const std::string ns = c == std::string::npos ? "minecraft" : id.substr(0, c);
    return "assets/" + ns + "/" + prefix + (c == std::string::npos ? id : id.substr(c + 1));
}

struct Unihex {
    u32 rows[16];
    u8 left, right;  // inclusive bit columns (from the left of a 32-bit row)
};
}  // namespace

struct Font::Provider {
    enum Type { Bitmap, Space, Unifont, TrueType } type = Bitmap;
    std::unordered_map<u32, Glyph> glyphs;  // bitmap / space glyphs; baked unihex / TrueType glyphs
    // unihex: the font loads on a background thread; first use waits for it.
    std::shared_future<std::shared_ptr<std::unordered_map<u32, Unihex>>> unihex;
    std::unordered_set<u32> missing;  // code points this provider has no glyph for (unihex / TrueType)
    // TrueType
    std::vector<u8> ttf;
    stbtt_fontinfo info{};
    float scale = 1, oversample = 1, shiftX = 0, shiftY = 0;
    std::unordered_set<u32> skip;
};

Font::~Font() {
    for (auto& [k, p] : providers_)
        if (p->unihex.valid()) p->unihex.wait();
}

bool Font::load(const Resources& res, Atlas& atlas, bool uniform, bool jp) {
    res_ = &res;
    atlas_ = &atlas;
    // SpecialGlyphs.MISSING: a 5x8 outline box, advance 6.
    int x, y;
    if (atlas.alloc(5, 8, x, y)) {
        for (int py = 0; py < 8; ++py)
            for (int px = 0; px < 5; ++px) {
                u8* p = atlas.pixel(x + px, y + py);
                const bool edge = px == 0 || px == 4 || py == 0 || py == 7;
                p[0] = p[1] = p[2] = 255, p[3] = edge ? 255 : 0;
            }
        atlas.markDirty(y, y + 8);
        missing_.visible = true;
        missing_.advance = 6;
        missing_.right = 5, missing_.bottom = 8;
        missing_.u0 = float(x) / atlas.width(), missing_.v0 = float(y) / atlas.height();
        missing_.u1 = float(x + 5) / atlas.width(), missing_.v1 = float(y + 8) / atlas.height();
    }
    reload(uniform, jp);
    return !default_->providers.empty();
}

void Font::reload(bool uniform, bool jp) {
    uniform_ = uniform, jp_ = jp;
    sets_.clear();
    objects_.clear();
    default_ = &set(-1);
}

Font::FontSet& Font::set(int fontIndex) {
    const FontDesc d = fontIndex < 0 ? FontDesc{FontDesc::Resource, "minecraft:default"} : fontDesc(fontIndex);
    std::string id = d.kind == FontDesc::Resource ? d.id : "minecraft:default";
    if (id.find(':') == std::string::npos) id = "minecraft:" + id;
    auto it = sets_.find(id);
    if (it != sets_.end()) return it->second;
    FontSet& fs = sets_[id];
    build(fs, id, 0);
    if (fs.providers.empty() && id != "minecraft:default") LOG("ui: font %s not found", id.c_str());
    for (Provider* p : fs.providers)
        if (p->type == Provider::Bitmap)
            for (const auto& [cp, g] : p->glyphs)
                if (g.visible) fs.byWidth[int(std::ceil(g.advance))].push_back(cp);
    for (auto& [w, v] : fs.byWidth) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    return fs;
}

Font::Provider* Font::provider(const std::string& key, const std::function<std::unique_ptr<Provider>()>& make) {
    auto it = providers_.find(key);
    if (it != providers_.end()) return it->second.get();
    std::unique_ptr<Provider> p = make();
    Provider* raw = p.get();
    providers_[key] = std::shared_ptr<Provider>(std::move(p));  // a failed load is remembered as null
    return raw;
}

void Font::build(FontSet& fs, const std::string& fontId, int depth) {
    if (depth > 8) return;
    const nlohmann::json j = res_->json(resourcePath(fontId, "font/") + ".json");
    if (!j.is_object() || !j.contains("providers") || !j["providers"].is_array()) return;
    for (const auto& p : j["providers"]) {
        if (!p.is_object()) continue;
        // FontOption.Filter: every listed option must be in that state (uniform: Force Unicode Font, jp: Japanese
        // Glyph Variants).
        if (p.contains("filter") && p["filter"].is_object()) {
            bool active = true;
            for (const auto& f : p["filter"].items()) {
                const bool want = f.value().is_boolean() && f.value().get<bool>();
                const bool have = f.key() == "uniform" ? uniform_ : f.key() == "jp" ? jp_ : false;
                active &= want == have;
            }
            if (!active) continue;
        }
        const std::string type = p.value("type", "");
        const std::string key = type + "|" + p.dump();
        Provider* prov = nullptr;
        if (type == "reference") {
            build(fs, p.value("id", ""), depth + 1);
            continue;
        } else if (type == "space") {
            prov = provider(key, [&] {
                auto sp = std::make_unique<Provider>();
                sp->type = Provider::Space;
                if (p.contains("advances") && p["advances"].is_object())
                    for (const auto& a : p["advances"].items()) {
                        std::string k = a.key();
                        size_t i = 0;
                        Glyph g;
                        g.advance = a.value().is_number() ? a.value().get<float>() : 0;
                        sp->glyphs[nextCodepoint(k, i)] = g;
                    }
                return sp;
            });
        } else if (type == "bitmap") {
            prov = provider(key, [&] { return loadBitmap(p); });
        } else if (type == "unihex") {
            prov = provider(key, [&] { return loadUnihex(p); });
        } else if (type == "ttf") {
            prov = provider(key, [&] { return loadTrueType(p); });
        }
        if (prov) fs.providers.push_back(prov);
    }
}

std::unique_ptr<Font::Provider> Font::loadBitmap(const nlohmann::json& p) {
    Image img;
    if (!res_->image(resourcePath(p.value("file", ""), "textures/"), img) || !p.contains("chars") || !p["chars"].is_array()) return nullptr;
    std::vector<std::vector<u32>> grid;
    for (const auto& row : p["chars"])
        if (row.is_string()) grid.push_back(codepoints(row.get<std::string>()));
    if (grid.empty() || grid[0].empty()) return nullptr;
    int ax, ay;
    if (!atlas_->alloc(img.w, img.h, ax, ay)) {
        LOG("ui: GUI atlas full (font %s)", p.value("file", "").c_str());
        return nullptr;
    }
    atlas_->blit(ax, ay, img.w, img.h, img.rgba.data(), img.w * 4);
    const int gw = img.w / int(grid[0].size()), gh = img.h / int(grid.size());
    const int height = p.value("height", 8), ascent = p.value("ascent", 0);
    const float scale = float(height) / float(gh);
    auto prov = std::make_unique<Provider>();
    prov->type = Provider::Bitmap;
    for (int sy = 0; sy < int(grid.size()); ++sy)
        for (int sx = 0; sx < int(grid[sy].size()); ++sx) {
            const u32 cp = grid[sy][sx];
            if (!cp || prov->glyphs.count(cp)) continue;
            int actual = gw - 1;
            for (; actual >= 0; --actual) {
                bool any = false;
                for (int py = 0; py < gh && !any; ++py) any = img.at(sx * gw + actual, sy * gh + py)[3] != 0;
                if (any) break;
            }
            Glyph g;
            g.advance = float(int(0.5 + double(actual + 1) * scale) + 1);
            g.visible = true;
            g.left = 0, g.right = float(gw) * scale;
            g.top = 7.0f - float(ascent), g.bottom = g.top + float(gh) * scale;
            const float W = float(atlas_->width()), H = float(atlas_->height());
            g.u0 = (float(ax + sx * gw) + 0.01f) / W, g.u1 = (float(ax + sx * gw + gw) - 0.01f) / W;
            g.v0 = (float(ay + sy * gh) + 0.01f) / H, g.v1 = (float(ay + sy * gh + gh) - 0.01f) / H;
            prov->glyphs[cp] = g;
        }
    return prov;
}

std::unique_ptr<Font::Provider> Font::loadUnihex(const nlohmann::json& p) {
    const std::string file = resourcePath(p.value("hex_file", ""), "");
    struct Override {
        u32 from, to;
        int left, right;
    };
    std::vector<Override> overrides;
    if (p.contains("size_overrides") && p["size_overrides"].is_array())
        for (const auto& o : p["size_overrides"])
            overrides.push_back({jsonCodepoint(o["from"]), jsonCodepoint(o["to"]), o.value("left", 0), o.value("right", 0)});
    fs::path path;
    res_->objectPath(file, path);
    if (path.empty()) {  // only on disk (the asset index, folder packs)
        LOG("ui: %s unavailable", file.c_str());
        return nullptr;
    }
    auto prov = std::make_unique<Provider>();
    prov->type = Provider::Unifont;
    prov->unihex = std::async(std::launch::async, [path, overrides, file] {
        Timer t;
        auto out = std::make_shared<std::unordered_map<u32, Unihex>>();
        ZipArchive zip;
        std::vector<u8> hex;
        if (!zip.open(path)) return out;
        for (const auto& e : zip.entries())
            if (e.name.ends_with(".hex") && zip.read(e, hex)) break;
        out->reserve(60000);
        size_t i = 0;
        while (i < hex.size()) {
            size_t colon = i;
            while (colon < hex.size() && hex[colon] != ':' && hex[colon] != '\n') ++colon;
            size_t eol = colon;
            while (eol < hex.size() && hex[eol] != '\n') ++eol;
            if (colon >= hex.size() || hex[colon] != ':') {
                i = eol + 1;
                continue;
            }
            u32 cp = 0;
            for (size_t k = i; k < colon; ++k) cp = (cp << 4) | u32(std::max(0, hexDigit(char(hex[k]))));
            size_t len = eol - colon - 1;
            if (len && hex[colon + len] == '\r') --len;
            const int bitWidth = len == 32 ? 8 : len == 64 ? 16 : len == 96 ? 24 : len == 128 ? 32 : 0;
            if (bitWidth) {
                Unihex u{};
                const size_t digits = size_t(bitWidth / 4);
                u32 mask = 0;
                for (int row = 0; row < 16; ++row) {
                    u32 v = 0;
                    for (size_t d = 0; d < digits; ++d) v = (v << 4) | u32(std::max(0, hexDigit(char(hex[colon + 1 + row * digits + d]))));
                    u.rows[row] = bitWidth == 32 ? v : v << (32 - bitWidth);
                    mask |= u.rows[row];
                }
                int left = 0, right = bitWidth;
                if (mask) {
                    unsigned long lz, tz;
                    _BitScanReverse(&lz, mask);
                    _BitScanForward(&tz, mask);
                    left = 31 - int(lz);
                    right = 31 - int(tz);
                }
                for (const Override& o : overrides)
                    if (cp >= o.from && cp <= o.to) left = o.left, right = o.right;
                u.left = u8(left), u.right = u8(right);
                (*out)[cp] = u;
            }
            i = eol + 1;
        }
        LOG("ui: %s: %zu glyphs in %.0f ms", file.c_str(), out->size(), t.ms());
        return out;
    }).share();
    return prov;
}

std::unique_ptr<Font::Provider> Font::loadTrueType(const nlohmann::json& p) {
    auto prov = std::make_unique<Provider>();
    prov->type = Provider::TrueType;
    const std::string file = resourcePath(p.value("file", ""), "font/");
    if (!res_->read(file, prov->ttf) || !stbtt_InitFont(&prov->info, prov->ttf.data(), stbtt_GetFontOffsetForIndex(prov->ttf.data(), 0))) {
        LOG("ui: TrueType font %s unavailable", file.c_str());
        return nullptr;
    }
    const float size = p.value("size", 11.0f);
    prov->oversample = std::max(0.01f, p.value("oversample", 1.0f));
    // FT_Set_Pixel_Sizes(round(size * oversample)): pixels per em.
    prov->scale = stbtt_ScaleForMappingEmToPixels(&prov->info, std::round(size * prov->oversample));
    if (p.contains("shift") && p["shift"].is_array() && p["shift"].size() == 2)
        prov->shiftX = p["shift"][0].get<float>(), prov->shiftY = p["shift"][1].get<float>();
    if (p.contains("skip")) {  // a string, or a list of strings joined
        std::string skip;
        if (p["skip"].is_string()) skip = p["skip"].get<std::string>();
        else if (p["skip"].is_array())
            for (const auto& s : p["skip"])
                if (s.is_string()) skip += s.get<std::string>();
        for (u32 cp : codepoints(skip)) prov->skip.insert(cp);
    }
    LOG("ui: TrueType font %s (size %.1f, oversample %.1f)", file.c_str(), size, prov->oversample);
    return prov;
}

// A provider's glyph for cp (baking unihex / TrueType glyphs into the atlas on first use); nullptr if it has none.
const Glyph* Font::providerGlyph(Provider& p, u32 cp) {
    if (auto it = p.glyphs.find(cp); it != p.glyphs.end()) return &it->second;
    if (p.type == Provider::Bitmap || p.type == Provider::Space || p.missing.count(cp)) return nullptr;
    if (p.type == Provider::Unifont) {
        const auto map = p.unihex.get();
        const auto it = map->find(cp);
        if (it == map->end()) {
            p.missing.insert(cp);
            return nullptr;
        }
        const Unihex& u = it->second;
        const int w = int(u.right) - int(u.left) + 1;
        Glyph g;
        g.advance = float(w / 2 + 1);
        g.shadowOffset = g.boldOffset = 0.5f;
        int x, y;
        if (w > 0 && atlas_->alloc(w, 16, x, y)) {
            for (int row = 0; row < 16; ++row)
                for (int c = 0; c < w; ++c) {
                    const int bit = 31 - (int(u.left) + c);
                    const bool set = bit >= 0 && bit < 32 && ((u.rows[row] >> bit) & 1);
                    u8* px = atlas_->pixel(x + c, y + row);
                    px[0] = px[1] = px[2] = 255, px[3] = set ? 255 : 0;
                }
            atlas_->markDirty(y, y + 16);
            g.visible = true;
            g.left = 0, g.right = float(w) / 2.0f, g.top = 0, g.bottom = 8;
            const float W = float(atlas_->width()), H = float(atlas_->height());
            g.u0 = (float(x) + 0.01f) / W, g.u1 = (float(x + w) - 0.01f) / W;
            g.v0 = (float(y) + 0.01f) / H, g.v1 = (float(y + 16) - 0.01f) / H;
        }
        return &(p.glyphs[cp] = g);
    }
    // TrueType (TrueTypeGlyphProvider): the outline at size * oversample pixels per em, shifted, as an alpha mask.
    const int index = p.skip.count(cp) ? 0 : stbtt_FindGlyphIndex(&p.info, int(cp));
    if (!index) {
        p.missing.insert(cp);
        return nullptr;
    }
    int advance, lsb;
    stbtt_GetGlyphHMetrics(&p.info, index, &advance, &lsb);
    Glyph g;
    g.advance = std::round(float(advance) * p.scale) / p.oversample;  // FreeType's hinted advance is whole pixels
    const float sx = p.shiftX * p.oversample, sy = p.shiftY * p.oversample;
    const float fx = sx - std::floor(sx), fy = sy - std::floor(sy);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBoxSubpixel(&p.info, index, p.scale, p.scale, fx, fy, &x0, &y0, &x1, &y1);
    const int w = x1 - x0, h = y1 - y0;
    int ax, ay;
    if (w > 0 && h > 0 && atlas_->alloc(w, h, ax, ay)) {
        std::vector<u8> mask(size_t(w) * size_t(h));
        stbtt_MakeGlyphBitmapSubpixel(&p.info, mask.data(), w, h, w, p.scale, p.scale, fx, fy, index);
        for (int row = 0; row < h; ++row)
            for (int c = 0; c < w; ++c) {
                u8* px = atlas_->pixel(ax + c, ay + row);
                px[0] = px[1] = px[2] = 255, px[3] = mask[size_t(row) * size_t(w) + size_t(c)];
            }
        atlas_->markDirty(ay, ay + h);
        g.visible = true;
        // GlyphBitmap: left = bearing left, top = 7 - bearing top (bitmap_top = -y0 in stb's y-down boxes)
        g.left = (float(x0) + std::floor(sx)) / p.oversample;
        g.right = g.left + float(w) / p.oversample;
        g.top = 7.0f + (float(y0) + std::floor(sy)) / p.oversample;
        g.bottom = g.top + float(h) / p.oversample;
        const float W = float(atlas_->width()), H = float(atlas_->height());
        g.u0 = (float(ax) + 0.01f) / W, g.u1 = (float(ax + w) - 0.01f) / W;
        g.v0 = (float(ay) + 0.01f) / H, g.v1 = (float(ay + h) - 0.01f) / H;
    }
    return &(p.glyphs[cp] = g);
}

const Glyph& Font::setGlyph(FontSet& fs, u32 cp) {
    if (auto it = fs.cache.find(cp); it != fs.cache.end()) return it->second;
    for (Provider* p : fs.providers)
        if (const Glyph* g = providerGlyph(*p, cp)) return fs.cache[cp] = *g;
    return fs.cache[cp] = missing_;
}

// AtlasGlyphProvider / PlayerGlyphProvider: an 8x8 sprite (a player's face and hat), advance 8, top at 7 - 8.
const Glyph& Font::objectGlyph(int fontIndex) {
    Glyph& g = objects_[fontIndex];
    const FontDesc d = fontDesc(fontIndex);
    const Sprite* s = objectSprite ? objectSprite(d) : nullptr;
    g = Glyph{};
    g.advance = 8;
    if (!s) return g = missing_;
    g.visible = true;
    g.left = 0, g.right = 8, g.top = -1, g.bottom = 7;
    if (d.kind == FontDesc::Player) {  // face (8, 8) and hat (40, 8) of the 64x64 skin
        const float du = (s->u1 - s->u0) / float(std::max(1, s->w)), dv = (s->v1 - s->v0) / float(std::max(1, s->h));
        g.u0 = s->u0 + du * 8, g.u1 = s->u0 + du * 16, g.v0 = s->v0 + dv * 8, g.v1 = s->v0 + dv * 16;
        if (d.hat) g.overlay = true, g.ou0 = s->u0 + du * 40, g.ou1 = s->u0 + du * 48, g.ov0 = g.v0, g.ov1 = g.v1;
    } else {
        g.u0 = s->u0, g.u1 = s->u1, g.v0 = s->v0, g.v1 = s->v1;
    }
    return g;
}

const Glyph& Font::glyph(u32 cp) { return setGlyph(*default_, cp); }

const Glyph& Font::glyph(u32 cp, const Style& style) {
    if (style.font < 0) return setGlyph(*default_, cp);
    const FontDesc d = fontDesc(style.font);
    if (d.kind != FontDesc::Resource) return objectGlyph(style.font);
    return setGlyph(set(style.font), cp);
}

const Glyph& Font::randomGlyph(FontSet& fs, int width) {
    auto it = fs.byWidth.find(width);
    if (it == fs.byWidth.end() || it->second.empty()) return missing_;
    return setGlyph(fs, it->second[random_() % it->second.size()]);
}

void Font::iterate(const Text& t, const std::function<bool(int, const Style&, u32)>& fn) const {
    for (const Span& span : t.spans) {
        Style style = span.style;
        const std::string& s = span.text;
        size_t i = 0;
        while (i < s.size()) {
            const int pos = int(i);
            const u32 cp = nextCodepoint(s, i);
            if (cp == 0xA7) {  // section sign: formatting code
                if (i >= s.size()) break;
                const u32 code = nextCodepoint(s, i);
                if (code < 128) style.applyLegacy(char(code), span.style);
                continue;
            }
            if (!fn(pos, style, cp)) return;
        }
    }
}

float Font::widthF(const Text& t) {
    float w = 0;
    iterate(t, [&](int, const Style& st, u32 cp) {
        w += glyph(cp, st).advanceFor(st.isBold());
        return true;
    });
    return w;
}

float Font::draw(Canvas& c, const Text& t, float x0, float y, u32 color, bool shadow, bool logical) {
    if (!alphaOf(color)) return x0;
    struct Inst {
        const Glyph* g;
        float x;
        u32 color, shadow;
        bool bold, italic;
    };
    struct Effect {
        float x0, y0, x1, y1;
        u32 color, shadow;
        float offset;
    };
    thread_local std::vector<Inst> glyphs;
    thread_local std::vector<Effect> effects;
    thread_local std::vector<Style> styles;
    thread_local std::vector<bidi::Char> chars;
    glyphs.clear(), effects.clear(), styles.clear(), chars.clear();
    // Logical order first (with the style of each code point), then the visual order (ClientLanguage.getVisualOrder).
    bool special = rtl_;
    iterate(t, [&](int, const Style& st, u32 cp) {
        if (styles.empty() || !(styles.back() == st)) styles.push_back(st);
        chars.push_back({cp, u32(styles.size() - 1)});
        special |= bidi::needsBidi(cp);
        return true;
    });
    if (special && !logical) bidi::reorder(chars, rtl_);
    float x = x0;
    for (size_t i = 0; i < chars.size(); ++i) {
        const Style& st = styles[chars[i].tag];
        const u32 cp = chars[i].cp;
        const Glyph* g = &glyph(cp, st);
        if (st.isObfuscated() && cp != ' ') g = &randomGlyph(st.font < 0 ? *default_ : set(st.font), int(std::ceil(g->advance)));
        const u32 text = st.color >= 0 ? (color & 0xFF000000) | u32(st.color) : color;
        u32 sh = 0;
        if (st.shadow >= 0) {
            const u32 s = u32(st.shadow);
            const float ta = float(alphaOf(text)) / 255.0f;
            sh = ta != 1.0f ? withAlpha(s, ta * float(alphaOf(s)) / 255.0f) : s;
        } else if (shadow) {
            sh = scaleRgb(text, 0.25f);
        }
        const bool bold = st.isBold();
        const float adv = g->advanceFor(bold);
        if (g->visible) glyphs.push_back({g, x, text, sh, bold, st.isItalic()});
        const float ex0 = i == 0 ? x - 1.0f : x;
        if (st.isStrikethrough()) effects.push_back({ex0, y + 4.5f - 1.0f, x + adv, y + 4.5f, text, sh, g->shadowOffset});
        if (st.isUnderlined()) effects.push_back({ex0, y + 8.0f, x + adv, y + 9.0f, text, sh, g->shadowOffset});
        x += adv;
    }
    auto put = [&](const Inst& i, float gx, float gy, u32 col) {
        const Glyph& g = *i.g;
        const float extra = i.bold ? 0.1f : 0.0f;
        const float shTop = i.italic ? 1.0f - 0.25f * g.top : 0.0f, shBottom = i.italic ? 1.0f - 0.25f * g.bottom : 0.0f;
        const float xa = gx + g.left, xb = gx + g.right, ya = gy + g.top, yb = gy + g.bottom;
        const float xy[4][2] = {{xa + shTop - extra, ya - extra},
                                {xa + shBottom - extra, yb + extra},
                                {xb + shBottom + extra, yb + extra},
                                {xb + shTop + extra, ya - extra}};
        const float uv[4][2] = {{g.u0, g.v0}, {g.u0, g.v1}, {g.u1, g.v1}, {g.u1, g.v0}};
        c.quad4(xy, uv, col, ModeGui);
        if (g.overlay) {
            const float ov[4][2] = {{g.ou0, g.ov0}, {g.ou0, g.ov1}, {g.ou1, g.ov1}, {g.ou1, g.ov0}};
            c.quad4(xy, ov, col, ModeGui);
        }
    };
    for (const Inst& i : glyphs) {
        if (!alphaOf(i.shadow)) continue;
        const float o = i.g->shadowOffset;
        put(i, i.x + o, y + o, i.shadow);
        if (i.bold) put(i, i.x + i.g->boldOffset + o, y + o, i.shadow);
    }
    for (const Inst& i : glyphs) {
        put(i, i.x, y, i.color);
        if (i.bold) put(i, i.x + i.g->boldOffset, y, i.color);
    }
    for (const Effect& e : effects) {
        if (alphaOf(e.shadow)) c.fill(e.x0 + e.offset, e.y0 + e.offset, e.x1 + e.offset, e.y1 + e.offset, e.shadow);
        c.fill(e.x0, e.y0, e.x1, e.y1, e.color);
    }
    return x;
}

std::vector<Text> Font::split(const Text& t, int maxWidth) {
    struct Ch {
        u32 cp;
        Style style;
        float adv;
    };
    std::vector<Ch> chars;
    iterate(t, [&](int, const Style& st, u32 cp) {
        chars.push_back({cp, st, glyph(cp, st).advanceFor(st.isBold())});
        return true;
    });
    std::vector<Text> lines;
    auto emit = [&](size_t a, size_t b) {
        Text line;
        for (size_t k = a; k < b; ++k) {
            std::string s;
            appendUtf8(s, chars[k].cp);
            if (!line.spans.empty() && line.spans.back().style == chars[k].style) line.spans.back().text += s;
            else line.spans.push_back({s, chars[k].style});
        }
        lines.push_back(std::move(line));
    };
    size_t start = 0;
    while (start < chars.size()) {
        float w = 0;
        size_t lastSpace = SIZE_MAX, k = start;
        bool newline = false;
        for (; k < chars.size(); ++k) {
            if (chars[k].cp == '\n') {
                newline = true;
                break;
            }
            if (chars[k].cp == ' ') lastSpace = k;
            w += chars[k].adv;
            if (w > float(maxWidth)) break;
        }
        if (newline) {
            emit(start, k);
            start = k + 1;
            if (start == chars.size()) lines.emplace_back();
            continue;
        }
        if (k >= chars.size()) {
            emit(start, k);
            break;
        }
        if (lastSpace != SIZE_MAX && lastSpace > start) {
            emit(start, lastSpace);
            start = lastSpace + 1;
        } else {
            const size_t end = std::max(k, start + 1);
            emit(start, end);
            start = end;
        }
    }
    if (lines.empty()) lines.emplace_back();
    return lines;
}

std::string Font::headByWidth(std::string_view s, int width) {
    float w = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = i;
        const u32 cp = nextCodepoint(s, j);
        w += glyph(cp).advance;
        if (w > float(width)) break;
        i = j;
    }
    return std::string(s.substr(0, i));
}

std::string Font::tailByWidth(std::string_view s, int width) {
    std::vector<size_t> starts;
    for (size_t i = 0; i < s.size();) {
        starts.push_back(i);
        nextCodepoint(s, i);
    }
    float w = 0;
    size_t from = s.size();
    for (size_t k = starts.size(); k-- > 0;) {
        size_t j = starts[k];
        w += glyph(nextCodepoint(s, j)).advance;
        if (w > float(width)) break;
        from = starts[k];
    }
    return std::string(s.substr(from));
}

}  // namespace ui
