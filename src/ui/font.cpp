#include "ui/font.h"

#include "assets/zip.h"

#include <cmath>

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
}  // namespace

Font::~Font() {
    if (unihexLoad_.valid()) unihexLoad_.wait();
}

bool Font::load(const Resources& res, Atlas& atlas) {
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
    if (!loadProviders(res, "minecraft:default", 0)) return false;
    for (const Provider& p : providers_)
        for (const auto& [cp, g] : p.glyphs)
            if (p.type == Provider::Bitmap && g.visible) byWidth_[int(std::ceil(g.advance))].push_back(cp);
    for (auto& [w, v] : byWidth_) std::sort(v.begin(), v.end());
    return true;
}

bool Font::loadProviders(const Resources& res, const std::string& fontId, int depth) {
    if (depth > 8) return false;
    std::string id = fontId.substr(fontId.find(':') + 1);
    const nlohmann::json j = res.json("assets/minecraft/font/" + id + ".json");
    if (!j.is_object() || !j.contains("providers")) {
        LOG("ui: font %s not found", fontId.c_str());
        return false;
    }
    for (const auto& p : j["providers"]) {
        // Filters: the default options are uniform = false, jp = false.
        if (p.contains("filter") && p["filter"].is_object()) {
            bool active = true;
            for (const auto& f : p["filter"].items()) active &= f.value().is_boolean() && !f.value().get<bool>();
            if (!active) continue;
        }
        const std::string type = p.value("type", "");
        if (type == "reference") {
            loadProviders(res, p.value("id", ""), depth + 1);
        } else if (type == "space") {
            Provider prov{Provider::Space, {}};
            if (p.contains("advances"))
                for (const auto& a : p["advances"].items()) {
                    std::string key = a.key();
                    size_t i = 0;
                    Glyph g;
                    g.advance = a.value().is_number() ? a.value().get<float>() : 0;
                    prov.glyphs[nextCodepoint(key, i)] = g;
                }
            providers_.push_back(std::move(prov));
        } else if (type == "bitmap") {
            std::string file = p.value("file", "");
            file = file.substr(file.find(':') + 1);
            Image img;
            if (!res.image("assets/minecraft/textures/" + file, img) || !p.contains("chars")) continue;
            std::vector<std::vector<u32>> grid;
            for (const auto& row : p["chars"]) grid.push_back(codepoints(row.get<std::string>()));
            if (grid.empty() || grid[0].empty()) continue;
            int ax, ay;
            if (!atlas_->alloc(img.w, img.h, ax, ay)) {
                LOG("ui: GUI atlas full (font %s)", file.c_str());
                continue;
            }
            atlas_->blit(ax, ay, img.w, img.h, img.rgba.data(), img.w * 4);
            const int gw = img.w / int(grid[0].size()), gh = img.h / int(grid.size());
            const int height = p.value("height", 8), ascent = p.value("ascent", 0);
            const float scale = float(height) / float(gh);
            Provider prov{Provider::Bitmap, {}};
            for (int sy = 0; sy < int(grid.size()); ++sy)
                for (int sx = 0; sx < int(grid[sy].size()); ++sx) {
                    const u32 cp = grid[sy][sx];
                    if (!cp || prov.glyphs.count(cp)) continue;
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
                    prov.glyphs[cp] = g;
                }
            providers_.push_back(std::move(prov));
        } else if (type == "unihex") {
            loadUnihex(res, p);
        }
    }
    return true;
}

void Font::loadUnihex(const Resources& res, const nlohmann::json& p) {
    std::string file = p.value("hex_file", "");
    file = "assets/minecraft/" + file.substr(file.find(':') + 1);
    struct Override {
        u32 from, to;
        int left, right;
    };
    std::vector<Override> overrides;
    if (p.contains("size_overrides"))
        for (const auto& o : p["size_overrides"])
            overrides.push_back({jsonCodepoint(o["from"]), jsonCodepoint(o["to"]), o.value("left", 0), o.value("right", 0)});
    fs::path path;
    res.objectPath(file, path);
    if (path.empty()) return;
    unihexLoad_ = std::async(std::launch::async, [this, path, overrides] {
        Timer t;
        ZipArchive zip;
        std::vector<u8> hex;
        if (!zip.open(path)) return;
        for (const auto& e : zip.entries())
            if (e.name.ends_with(".hex") && zip.read(e, hex)) break;
        std::unordered_map<u32, Unihex> out;
        out.reserve(60000);
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
                out[cp] = u;
            }
            i = eol + 1;
        }
        unihex_ = std::move(out);
        LOG("ui: unifont %zu glyphs in %.0f ms", unihex_.size(), t.ms());
    });
}

const Font::Unihex* Font::unihex(u32 cp) {
    if (!unihexReady_) {
        if (!unihexLoad_.valid()) return nullptr;
        unihexLoad_.wait();
        unihexReady_ = true;
    }
    auto it = unihex_.find(cp);
    return it == unihex_.end() ? nullptr : &it->second;
}

Glyph Font::bakeUnihex(const Unihex& u) {
    const int w = int(u.right) - int(u.left) + 1;
    Glyph g;
    g.advance = float(w / 2 + 1);
    g.shadowOffset = g.boldOffset = 0.5f;
    int x, y;
    if (w <= 0 || !atlas_->alloc(w, 16, x, y)) return g;
    for (int row = 0; row < 16; ++row)
        for (int c = 0; c < w; ++c) {
            const int bit = 31 - (int(u.left) + c);
            const bool set = bit >= 0 && bit < 32 && ((u.rows[row] >> bit) & 1);
            u8* p = atlas_->pixel(x + c, y + row);
            p[0] = p[1] = p[2] = 255, p[3] = set ? 255 : 0;
        }
    atlas_->markDirty(y, y + 16);
    g.visible = true;
    g.left = 0, g.right = float(w) / 2.0f, g.top = 0, g.bottom = 8;
    const float W = float(atlas_->width()), H = float(atlas_->height());
    g.u0 = (float(x) + 0.01f) / W, g.u1 = (float(x + w) - 0.01f) / W;
    g.v0 = (float(y) + 0.01f) / H, g.v1 = (float(y + 16) - 0.01f) / H;
    return g;
}

const Glyph& Font::glyph(u32 cp) {
    if (auto it = cache_.find(cp); it != cache_.end()) return it->second;
    for (const Provider& p : providers_)
        if (auto it = p.glyphs.find(cp); it != p.glyphs.end()) return cache_[cp] = it->second;
    if (const Unihex* u = unihex(cp)) return cache_[cp] = bakeUnihex(*u);
    return cache_[cp] = missing_;
}

const Glyph& Font::randomGlyph(int width) {
    auto it = byWidth_.find(width);
    if (it == byWidth_.end() || it->second.empty()) return missing_;
    return glyph(it->second[random_() % it->second.size()]);
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
        w += glyph(cp).advanceFor(st.isBold());
        return true;
    });
    return w;
}

float Font::draw(Canvas& c, const Text& t, float x0, float y, u32 color, bool shadow) {
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
    glyphs.clear(), effects.clear();
    float x = x0;
    iterate(t, [&](int pos, const Style& st, u32 cp) {
        const Glyph* g = &glyph(cp);
        if (st.isObfuscated() && cp != ' ') g = &randomGlyph(int(std::ceil(g->advance)));
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
        const float ex0 = pos == 0 ? x - 1.0f : x;
        if (st.isStrikethrough()) effects.push_back({ex0, y + 4.5f - 1.0f, x + adv, y + 4.5f, text, sh, g->shadowOffset});
        if (st.isUnderlined()) effects.push_back({ex0, y + 8.0f, x + adv, y + 9.0f, text, sh, g->shadowOffset});
        x += adv;
        return true;
    });
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
        chars.push_back({cp, st, glyph(cp).advanceFor(st.isBold())});
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
