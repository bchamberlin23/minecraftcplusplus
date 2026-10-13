// Language and fonts: the Language screen (LanguageSelectScreen: a search box, the packs' languages in an 18 pixel
// selection list, the accuracy warning, Font Settings... and Done), switching the language while running
// (LanguageManager + Language.inject: the UI's and the network client's tables, the right-to-left default, the client
// information the server gets), the font options, and what "object" text components draw: atlas sprites (the
// atlas's sources say which texture) and player heads (the profile's skin: a texture patch, the textures property's
// skin downloaded from its URL, or the default skin of the profile's UUID).
#include "entity/entities.h"
#include "ui/frontend.h"
#include "ui/options_list.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <thread>

namespace ui {

namespace {

constexpr int kHeaderH = 36, kFooterH = 53, kRowH = 18, kRowW = 270;

struct LangScreen {
    std::vector<std::pair<std::string, Resources::LanguageInfo>> all, shown;
    std::string selected;
    UiState::TextField search;
    bool searchFocused = true, listFocused = false;
    float scroll = 0;
    double lastClick = -1;
    std::string lastClickCode;
};
LangScreen g;

float listTop() { return float(kHeaderH); }
float listBottom(UiState& s) { return float(s.guiH() - kFooterH); }
float contentH() { return float(g.shown.size() * kRowH + 4); }
float maxScroll(UiState& s) { return std::max(0.0f, contentH() - (listBottom(s) - listTop())); }
float rowY(size_t i) { return listTop() + 2 - g.scroll + float(i * kRowH); }

Text languageText(const Resources::LanguageInfo& l) {  // LanguageInfo.toComponent: "name (region)"
    return Text(l.name + " (" + l.region + ")");
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(u8(c)));
    return s;
}

void filter(UiState& s) {  // LanguageSelectionList.filterEntries
    const std::string f = lower(g.search.value);
    g.shown.clear();
    for (const auto& e : g.all)
        if (f.empty() || lower(e.second.name).find(f) != std::string::npos || lower(e.second.region).find(f) != std::string::npos)
            g.shown.push_back(e);
    g.scroll = std::clamp(g.scroll, 0.0f, maxScroll(s));
}

void onDone(UiState& s) {
    if (!g.selected.empty() && g.selected != s.res.languageCode()) setLanguage(s, g.selected);
    s.openScreen(s.optionsParent);
}

void build(UiState& s) {
    s.buttons.clear();
    if (g.all.empty()) {  // opened: the languages, the current one selected and centred
        for (auto& e : s.res.languages()) g.all.push_back(e);
        g.selected = s.res.languageCode();
        g.search = {};
        g.searchFocused = true, g.listFocused = false;
        filter(s);
        for (size_t i = 0; i < g.shown.size(); ++i)
            if (g.shown[i].first == g.selected)  // centerScrollOn
                g.scroll = std::clamp(float(i * kRowH + kRowH / 2) - (listBottom(s) - listTop()) / 2, 0.0f, maxScroll(s));
    }
    UiState* p = &s;
    const float y = float(s.guiH() - kFooterH + (kFooterH - 37) / 2 + 9 + 8);
    Button font;
    font.label = Text(s.res.translate("options.font"));
    font.x = float(s.guiW() / 2 - 154), font.y = y, font.w = 150;
    font.onPress = [p] { p->openScreen(Screen::FontOptions); };
    s.buttons.push_back(std::move(font));
    Button done;
    done.label = Text(s.res.translate("gui.done"));
    done.x = float(s.guiW() / 2 + 4), done.y = y, done.w = 150;
    done.onPress = [p] { onDone(*p); };
    s.buttons.push_back(std::move(done));
}

void draw(UiState& s, Canvas& c) {
    const int W = c.guiWidth();
    menuBackground(s, c);
    const float top = listTop(), bottom = listBottom(s);
    listBackground(s, c, top, bottom);
    // Header: the title, then the search box (200 x 15), 4 apart, centred in the 36 pixel header.
    const float titleY = float((kHeaderH - 28) / 2);
    s.drawCentered(c, Text(s.res.translate("options.language.title")), float(W / 2), titleY, 0xFFFFFFFF);
    const float bx = float(W / 2 - 100), by = titleY + 13;
    blitSprite(c, s.sprites.sprite(g.searchFocused ? "widget/text_field_highlighted" : "widget/text_field"), bx, by, 200, 15);
    if (g.search.value.empty() && !g.searchFocused) {
        Style hint = colorStyle(0xAAAAAA);
        hint.italic = 1;
        s.drawText(c, Text(s.res.translate("gui.language.search"), hint), bx + 4, by + 4, 0xFFFFFFFF, true);
    } else {
        s.drawTextField(c, g.search, bx + 4, by + 4, 192, 0xFFE0E0E0, g.searchFocused, true);
    }
    // The list.
    c.pushScissor(0, top, float(W), bottom);
    const float left = float(W / 2 - kRowW / 2);
    for (size_t i = 0; i < g.shown.size(); ++i) {
        const float y = rowY(i);
        if (y + kRowH < top || y > bottom) continue;
        if (g.shown[i].first == g.selected) {  // AbstractSelectionList.extractSelection
            c.fill(left, y, left + kRowW, y + kRowH, g.listFocused ? 0xFFFFFFFF : 0xFF808080);
            c.fill(left + 1, y + 1, left + kRowW - 1, y + kRowH - 1, 0xFF000000);
        }
        s.drawCentered(c, languageText(g.shown[i].second), float(W / 2), y + 2 + 7 - 4, 0xFFFFFFFF);
    }
    c.popScissor();
    if (maxScroll(s) > 0) {
        const float h = bottom - top, bar = std::clamp(float(int(h * h / contentH())), 32.0f, h - 8);
        const float sx = left + kRowW + 6 + 2;
        const float y = std::max(top, float(int(g.scroll) * int(h - bar) / int(maxScroll(s))) + top);
        blitSprite(c, s.sprites.sprite("widget/scroller_background"), sx, top, 6, h);
        blitSprite(c, s.sprites.sprite("widget/scroller"), sx, y, 6, bar);
    }
    // Footer: the warning, then Font Settings... and Done.
    s.drawCentered(c, Text(s.res.translate("options.languageAccuracyWarning")), float(W / 2),
                   float(s.guiH() - kFooterH + (kFooterH - 37) / 2), 0xFFBABABA);
    s.drawButtons(c);
}

void input(UiState& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) return s.openScreen(s.optionsParent);
    if (e.type == E::Key && e.down && e.code == VK_RETURN) return onDone(s);
    if (e.type == E::Wheel && s.mouseY >= listTop() && s.mouseY < listBottom(s)) {
        g.scroll = std::clamp(g.scroll - float(e.code) * float(kRowH / 2), 0.0f, maxScroll(s));
        return;
    }
    if (g.searchFocused && (e.type == E::Key || e.type == E::Char)) {
        const std::string before = g.search.value;
        s.textFieldKey(g.search, e);
        if (g.search.value != before) filter(s);
        return;
    }
    if (e.type == E::Key && e.down && g.listFocused && (e.code == VK_UP || e.code == VK_DOWN) && !g.shown.empty()) {
        size_t i = 0;
        for (; i < g.shown.size() && g.shown[i].first != g.selected; ++i) {}
        i = i >= g.shown.size() ? 0 : e.code == VK_UP ? (i ? i - 1 : 0) : std::min(i + 1, g.shown.size() - 1);
        g.selected = g.shown[i].first;
        return;
    }
    if (e.type == E::Button && e.down && e.code == 0) {
        const float bx = float(s.guiW() / 2 - 100), by = float((kHeaderH - 28) / 2 + 13);
        if (s.mouseX >= bx && s.mouseX < bx + 200 && s.mouseY >= by && s.mouseY < by + 15) {
            g.searchFocused = true, g.listFocused = false;
            g.search.blinkStart = s.clock;
            return;
        }
        const float left = float(s.guiW() / 2 - kRowW / 2);
        if (s.mouseY >= listTop() && s.mouseY < listBottom(s) && s.mouseX >= left && s.mouseX < left + kRowW) {
            for (size_t i = 0; i < g.shown.size(); ++i) {
                const float y = rowY(i);
                if (s.mouseY < y || s.mouseY >= y + kRowH) continue;
                g.searchFocused = false, g.listFocused = true;
                const bool doubleClick = g.lastClickCode == g.shown[i].first && s.clock - g.lastClick < 0.25;
                g.selected = g.lastClickCode = g.shown[i].first;
                g.lastClick = s.clock;
                if (doubleClick) onDone(s);
                return;
            }
        }
        g.searchFocused = false;
    }
    s.buttonsInput(e);
}

void closed(UiState&) { g.all.clear(); }  // the next opening starts over

const ScreenRegistrar kLanguage({Screen::Language, build, draw, input, closed});

// --- object glyphs ---------------------------------------------------------------------------------------------

std::string base64Decode(const std::string& in) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (char ch : in) {
        const size_t d = chars.find(ch);
        if (d == std::string::npos) continue;
        val = (val << 6) + int(d);
        bits += 6;
        if (bits >= 0) {
            out += char((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

bool httpGet(const std::string& url, std::vector<u8>& out) {
    const std::wstring w(url.begin(), url.end());
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[1024] = {};
    uc.lpszHostName = host, uc.dwHostNameLength = 256, uc.lpszUrlPath = path, uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) return false;
    HINTERNET session = WinHttpOpen(L"minecraftCplusplus", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
    bool ok = req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    DWORD status = 0, size = sizeof(status);
    if (ok) ok = WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                     &size, WINHTTP_NO_HEADER_INDEX) && status == 200;
    while (ok) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        const size_t at = out.size();
        out.resize(at + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, out.data() + at, avail, &read)) ok = false;
        out.resize(at + read);
        if (out.size() > (4u << 20)) ok = false;
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return ok && !out.empty();
}

// A skin URL from the profile's textures property, downloaded once (0: not yet, 1: in the atlas, -1: failed).
std::unordered_map<std::string, int> gSkins;

const Sprite* skinFromUrl(UiState& s, const std::string& url) {
    const std::string key = "skin:" + url;
    auto it = gSkins.find(url);
    if (it == gSkins.end()) {
        gSkins[url] = 0;
        UiState* p = &s;
        std::thread([p, url, key] {
            std::vector<u8> bytes;
            Image img;
            const bool ok = httpGet(url, bytes) && decodeImage(bytes, img) && img.w == 64 && (img.h == 64 || img.h == 32);
            if (!ok) LOG("ui: skin %s unavailable", url.c_str());
            p->post([p, url, key, ok, img = std::move(img)] { gSkins[url] = ok && p->sprites.addImage(key, img) ? 1 : -1; });
        }).detach();
        return nullptr;
    }
    return it->second == 1 ? s.sprites.textureAt(key) : nullptr;
}

bool parseUuid(const std::string& t, u8 out[16]) {
    std::string hex;
    for (char c : t)
        if (std::isxdigit(u8(c))) hex += c;
    if (hex.size() != 32) return false;
    for (int i = 0; i < 16; ++i) out[i] = u8(std::stoi(hex.substr(size_t(i) * 2, 2), nullptr, 16));
    return true;
}

void offlineUuid(const std::string& name, u8 out[16]) {  // UUIDUtil.createOfflinePlayerUUID
    const std::string s = "OfflinePlayer:" + name;
    BCryptHash(BCRYPT_MD5_ALG_HANDLE, nullptr, 0, PUCHAR(s.data()), ULONG(s.size()), out, 16);
    out[6] = u8((out[6] & 0x0F) | 0x30);
    out[8] = u8((out[8] & 0x3F) | 0x80);
}

std::string textureFile(const std::string& id) {  // "ns:path" -> assets/ns/textures/path.png
    const size_t c = id.find(':');
    return "assets/" + (c == std::string::npos ? std::string("minecraft") : id.substr(0, c)) + "/textures/" +
           (c == std::string::npos ? id : id.substr(c + 1)) + ".png";
}

// The texture of an atlas sprite: the atlas's sources (atlases/<atlas>.json: directory with prefix, single).
const Sprite* atlasSprite(UiState& s, const std::string& atlas, const std::string& sprite) {
    std::string atlasId = atlas.find(':') == std::string::npos ? "minecraft:" + atlas : atlas;
    std::string spriteId = sprite.find(':') == std::string::npos ? "minecraft:" + sprite : sprite;
    if (atlasId == "minecraft:gui") return s.sprites.sprite(spriteId.substr(10));
    static std::unordered_map<std::string, std::string> files;  // atlas|sprite -> texture file ("" none)
    const std::string key = atlasId + "|" + spriteId;
    auto it = files.find(key);
    if (it == files.end()) {
        const size_t c = atlasId.find(':');
        const nlohmann::json j = s.res.json("assets/" + atlasId.substr(0, c) + "/atlases/" + atlasId.substr(c + 1) + ".json");
        const size_t sc = spriteId.find(':');
        const std::string ns = spriteId.substr(0, sc), path = spriteId.substr(sc + 1);
        std::string file;
        if (j.is_object() && j.contains("sources") && j["sources"].is_array())
            for (const auto& src : j["sources"]) {
                const std::string type = src.value("type", "");
                if (type == "minecraft:directory" || type == "directory") {
                    const std::string prefix = src.value("prefix", "");
                    if (path.starts_with(prefix))
                        if (const std::string f = "assets/" + ns + "/textures/" + src.value("source", "") + "/" + path.substr(prefix.size()) + ".png";
                            s.res.exists(f))
                            file = f;
                } else if (type == "minecraft:single" || type == "single") {
                    const std::string res = src.value("resource", "");
                    std::string name = src.value("sprite", res);
                    if (name.find(':') == std::string::npos) name = "minecraft:" + name;
                    if (name == spriteId) file = textureFile(res);
                }
            }
        if (file.empty() && s.res.exists(textureFile(spriteId))) file = textureFile(spriteId);  // blocks / items
        it = files.emplace(key, file).first;
    }
    return it->second.empty() ? nullptr : s.sprites.textureAt(it->second);
}

const Sprite* playerSkin(UiState& s, const FontDesc& d) {
    static const char* kSkins[9] = {"alex", "ari", "efe", "kai", "makena", "noor", "steve", "sunny", "zuri"};
    if (!d.texture.empty()) {
        const bool identifier = std::all_of(d.texture.begin(), d.texture.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/' || c == ':';
        });
        if (identifier) {  // a skin patch: a texture of the game or a pack
            if (const Sprite* sp = s.sprites.textureAt(textureFile(d.texture))) return sp;
        } else {  // the textures property: {"textures": {"SKIN": {"url": ...}}}
            const nlohmann::json j = nlohmann::json::parse(base64Decode(d.texture), nullptr, false);
            if (j.is_object() && j.contains("textures") && j["textures"].contains("SKIN")) {
                const std::string url = j["textures"]["SKIN"].value("url", "");
                if (url.starts_with("http://textures.minecraft.net/") || url.starts_with("https://textures.minecraft.net/"))
                    if (const Sprite* sp = skinFromUrl(s, url)) return sp;
            }
        }
    }
    u8 uuid[16];  // DefaultPlayerSkin.get(uuid): the profile's id, else the offline id of its name
    if (!parseUuid(d.uuid, uuid)) {
        if (d.name.empty()) return s.sprites.texture("entity/player/wide/steve");
        offlineUuid(d.name, uuid);
    }
    const int skin = ent::defaultSkinIndex(uuid);
    return s.sprites.texture(std::string("entity/player/") + (skin < 9 ? "slim/" : "wide/") + kSkins[skin % 9]);
}

}  // namespace

void installFontHooks(UiState& s) {
    UiState* p = &s;
    s.font.setRightToLeft(s.res.rightToLeft());
    s.font.objectSprite = [p](const FontDesc& d) -> const Sprite* {
        return d.kind == FontDesc::Atlas ? atlasSprite(*p, d.id, d.sprite) : playerSkin(*p, d);
    };
}

void reloadFonts(UiState& s) {
    s.font.reload(s.settings.forceUnicodeFont, s.settings.japaneseGlyphVariants);
    s.font.setRightToLeft(s.res.rightToLeft());
    chatRescale(s);
}

void setLanguage(UiState& s, const std::string& code) {
    // LanguageManager.setSelected + reloadResourcePacks: the tables are replaced while no packet is being handled
    // (handlers translate on the network thread).
    if (s.client) s.client->pauseDispatch([&] { s.res.setLanguage(code); }, code);
    else s.res.setLanguage(code);
    s.settings.lang = s.res.languageCode();
    s.settings.save();
    s.font.setRightToLeft(s.res.rightToLeft());
    if (s.client) s.client->setInformation(clientInformation(s.settings));  // the server hears the language
    s.inventory.title = Text(s.res.translate("container.crafting"));
    chatRescale(s);
}

}  // namespace ui
