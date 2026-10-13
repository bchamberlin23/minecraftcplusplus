// The End poem and credits (WinScreen: after leaving the End, ClientboundGameEventPacket WIN_GAME) and Credits and
// Attribution (CreditsAndAttributionScreen, from Options: the credits alone, the attribution and licenses links).
#include "assets/asset_source.h"
#include "audio/audio.h"
#include "net/client.h"
#include "ui/frontend.h"
#include "ui/modules.h"
#include "ui/ui_state.h"

#include <json/json.hpp>
#include <shellapi.h>

namespace ui {

namespace {

void openUrl(const std::string& url) {
    std::wstring w(url.begin(), url.end());  // the links here are ASCII
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

constexpr int kHeaderH = 33, kFooterH = 33;
const std::string kObfuscate = "\xC2\xA7" "f\xC2\xA7" "k\xC2\xA7" "a\xC2\xA7" "b";  // OBFUSCATE_TOKEN: §f§k§a§b

struct Win {
    bool poem = false;
    std::vector<Text> lines;
    std::vector<bool> centered;
    float scroll = 0, base = 0.5f, speed = 0.5f;
    bool space = false, up = false;
    int modifiers = 0;
    int total = 0;
    bool finished = false;
    double lastClock = -1;
} g;

// RandomSource.createThreadLocalInstance (java.util.Random's generator).
struct JavaRandom {
    u64 seed;
    explicit JavaRandom(i64 s) : seed((u64(s) ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1)) {}
    int next(int bits) {
        seed = (seed * 0x5DEECE66DULL + 0xB) & ((1ULL << 48) - 1);
        return int(i64(seed) >> (48 - bits));
    }
    int nextInt(int bound) {  // a power of two here
        return int((i64(bound) * i64(next(31))) >> 31);
    }
};

std::string readText(const char* path) {
    std::vector<u8> bytes;
    if (!AssetSource::client().read(path, bytes)) return {};
    return std::string(bytes.begin(), bytes.end());
}

void add(UiState& s, const Text& t, bool centered) {
    if (centered) {
        g.lines.push_back(t), g.centered.push_back(true);
        return;
    }
    for (const Text& l : s.font.split(t, 256)) g.lines.push_back(l), g.centered.push_back(false);
}
void empty() { g.lines.push_back(Text()), g.centered.push_back(false); }

// WinScreen.addPoemFile
void addPoem(UiState& s, const char* path, const std::string& player) {
    const std::string text = readText(path);
    if (text.empty()) return;
    JavaRandom random(8124371);
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        start = end + 1;
        for (size_t at; (at = line.find("PLAYERNAME")) != std::string::npos;) line.replace(at, 10, player);
        for (size_t at; (at = line.find(kObfuscate)) != std::string::npos;)
            line = line.substr(0, at) + "\xC2\xA7" "f\xC2\xA7" "k" + std::string("XXXXXXXX").substr(0, size_t(random.nextInt(4) + 3)) +
                   line.substr(at + kObfuscate.size());
        add(s, Text(line), false);
        empty();
    }
    for (int i = 0; i < 8; ++i) empty();
}

// WinScreen.addCreditsFile
void addCredits(UiState& s) {
    const std::string text = readText("assets/minecraft/texts/credits.json");
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_array()) return;
    auto str = [](const nlohmann::json& o, const char* k) {
        return o.is_object() && o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string();
    };
    for (const nlohmann::json& section : j) {
        add(s, Text("============", colorStyle(0xFFFFFF)), true);
        add(s, Text(str(section, "section"), colorStyle(0xFFFF55)), true);
        add(s, Text("============", colorStyle(0xFFFFFF)), true);
        empty(), empty();
        if (!section.contains("disciplines") || !section["disciplines"].is_array()) continue;
        for (const nlohmann::json& d : section["disciplines"]) {
            const std::string discipline = str(d, "discipline");
            if (!discipline.empty()) {
                add(s, Text(discipline, colorStyle(0xFFFF55)), true);
                empty(), empty();
            }
            if (!d.contains("titles") || !d["titles"].is_array()) continue;
            for (const nlohmann::json& t : d["titles"]) {
                add(s, Text(str(t, "title"), colorStyle(0xAAAAAA)), false);
                if (t.contains("names") && t["names"].is_array())
                    for (const nlohmann::json& n : t["names"])
                        if (n.is_string()) add(s, Text("           " + n.get<std::string>(), colorStyle(0xFFFFFF)), false);
                empty(), empty();
            }
        }
    }
}

void updateSpeed() {  // WinScreen.calculateScrollSpeed
    g.speed = (g.space ? g.base * (5.0f + float(g.modifiers) * 15.0f) : g.base) * (g.up ? -1.0f : 1.0f);
}

void finish(UiState& s) {  // WinScreen.respawn
    if (g.finished) return;
    g.finished = true;
    if (g.poem) {
        if (s.client) {  // ServerboundClientCommandPacket.Action.PERFORM_RESPAWN
            net::Writer w;
            w.varInt(0);
            s.client->sendPlay("client_command", w);
        }
        s.closeScreen(false);
    } else {
        s.openScreen(Screen::CreditsAttribution);
    }
}

void build(UiState& s) { s.buttons.clear(); }

void tileTexture(Canvas& c, const Sprite* t, float x, float y, float w, float h, float size, float vOffset, u32 color) {
    if (!t) return;
    c.pushScissor(x, y, x + w, y + h);
    const float y0 = y - std::fmod(vOffset, size);
    for (float ty = y0; ty < y + h; ty += size)
        for (float tx = x; tx < x + w; tx += size) blitTexture(c, t, tx, ty, 0, 0, size, size, int(size), int(size), color);
    c.popScissor();
}

void draw(UiState& s, Canvas& c) {
    const int W = c.guiWidth(), H = c.guiHeight();
    // Background: the End (poem) or the menu background scrolling with the text (credits, over the world).
    if (g.poem) {
        c.fill(0, 0, float(W), float(H), 0xFF000000);
        // RenderPipelines.END_PORTAL over the screen: the End sky, then star layers drifting (blend 2: src * src + dst).
        tileTexture(c, s.sprites.texture("environment/end_sky"), 0, 0, float(W), float(H), 128, 0, 0xFF0E2A2E);  // * COLORS[0]
        static const u32 kLayers[6] = {0xFF357178, 0xFF276F6B, 0xFF3B7272, 0xFF4D777A, 0xFF5B7B70, 0xFF5B6A7E};  // sqrt(2 * EndPortal COLORS)
        c.setBlend(2);
        for (int i = 0; i < 6; ++i) {
            const float size = 64.0f + float(i) * 24.0f;
            const float drift = float(s.clock) * (3.0f + float(i) * 1.5f);
            tileTexture(c, s.sprites.texture("entity/end_portal"), 0, 0, float(W), float(H), size, drift, kLayers[i]);
        }
        c.setBlend(0);
    } else {
        c.gradient(0, 0, float(W), float(H), 0xC0101010, 0xD0101010);
        tileTexture(c, s.sprites.texture("gui/menu_background"), 0, 0, float(W), float(H), 32, g.scroll * 0.5f, 0xFFFFFFFF);
    }
    // The lines under the logo, scrolling up; the last one stops in the middle of the screen.
    const double dt = g.lastClock < 0 ? 0.0 : std::min(s.clock - g.lastClock, 0.25);
    g.lastClock = s.clock;
    g.scroll = std::max(0.0f, g.scroll + float(dt) * 20.0f * g.speed);  // scrollSpeed per tick
    const float logoX = float(W / 2 - 128), logoY = float(H + 50);
    float yOffs = -g.scroll;
    c.push();
    c.translate(0, yOffs);
    blitTexture(c, s.sprites.texture("gui/title/minecraft"), logoX, logoY, 0, 0, 256, 44, 256, 64);
    blitTexture(c, s.sprites.texture("gui/title/edition"), float(W / 2 - 64), logoY + 44 - 7, 0, 0, 128, 14, 128, 16);
    float yPos = logoY + 100;
    for (size_t i = 0; i < g.lines.size(); ++i) {
        if (i + 1 == g.lines.size()) {
            const float diff = yPos + yOffs - float(H / 2 - 6);
            if (diff < 0) c.translate(0, -diff), yOffs -= diff;
        }
        if (yPos + yOffs + 12 + 8 > 0 && yPos + yOffs < float(H)) {
            if (g.centered[i]) s.drawCentered(c, g.lines[i], logoX + 128, yPos, 0xFFFFFFFF);
            else s.drawText(c, g.lines[i], logoX, yPos, 0xFFFFFFFF, true);
        }
        yPos += 12;
    }
    c.pop();
    // RenderPipelines.VIGNETTE: dst * (1 - src).
    c.setBlend(3);
    blitTexture(c, s.sprites.texture("misc/credits_vignette"), 0, 0, 0, 0, float(W), float(H), W, H);  // stretched
    c.setBlend(0);
    if (g.scroll > float(g.total + H + H + 24)) finish(s);
}

void input(UiState& s, const InputEvent& e) {
    using E = InputEvent;
    if (e.type != E::Key) return;
    if (e.code == VK_ESCAPE && e.down) return finish(s);  // WinScreen.onClose
    if (e.code == VK_SPACE) g.space = e.down;
    else if (e.code == VK_CONTROL || e.code == VK_LCONTROL || e.code == VK_RCONTROL) g.modifiers = e.down ? 1 : 0;
    else if (e.code == VK_UP) g.up = e.down;
    else return;
    updateSpeed();
}

void closed(UiState&) {
    if (audio::AudioSystem* a = audio::instance()) a->setScreenMusic("");  // WinScreen.removed
}

const ScreenRegistrar kWin({Screen::Win, build, draw, input, closed});

// --- Credits and Attribution --------------------------------------------------------------------------------------

void confirmLink(UiState& s, const std::string& url) {  // ConfirmLinkScreen.confirmLink
    UiState* sp = &s;
    showConfirm(s, Text(s.res.translate("chat.link.confirm")), Text(url), s.res.translate("chat.link.open"), [sp, url] {
        openUrl(url);
        sp->openScreen(Screen::CreditsAttribution);
    }, Screen::CreditsAttribution);
}

void buildCredits(UiState& s) {
    s.buttons.clear();
    const int W = s.guiW(), H = s.guiH();
    const float top = float(kHeaderH + std::max(0, (H - kHeaderH - kFooterH - (3 * 20 + 2 * 8)) / 2));
    UiState* sp = &s;
    auto button = [&](float y, const char* key, std::function<void()> fn, float w) {
        Button b;
        b.x = float(W / 2) - w / 2, b.y = y, b.w = w;
        b.label = Text(s.res.translate(key));
        b.onPress = std::move(fn);
        s.buttons.push_back(std::move(b));
    };
    button(top, "credits_and_attribution.button.credits", [sp] { openWinScreen(*sp, false); }, 210);
    button(top + 28, "credits_and_attribution.button.attribution", [sp] { confirmLink(*sp, "https://aka.ms/MinecraftJavaAttribution"); }, 210);
    button(top + 56, "credits_and_attribution.button.licenses", [sp] { confirmLink(*sp, "https://aka.ms/MinecraftJavaLicenses"); }, 210);
    button(float(H - kFooterH + (kFooterH - 20) / 2), "gui.done", [sp] { sp->openScreen(sp->optionsParent); }, 200);
}

void drawCredits(UiState& s, Canvas& c) {
    const int W = c.guiWidth(), H = c.guiHeight();
    c.gradient(0, 0, float(W), float(H), 0xC0101010, 0xD0101010);
    tileTexture(c, s.sprites.texture("gui/header_separator"), 0, float(kHeaderH - 2), float(W), 2, 32, 0, 0xFFFFFFFF);
    tileTexture(c, s.sprites.texture("gui/footer_separator"), 0, float(H - kFooterH), float(W), 2, 32, 0, 0xFFFFFFFF);
    s.drawCentered(c, Text(s.res.translate("credits_and_attribution.screen.title")), float(W / 2), float((kHeaderH - 9) / 2), 0xFFFFFFFF);
    s.drawButtons(c);
}

void inputCredits(UiState& s, const InputEvent& e) {
    if (e.type == InputEvent::Key && e.down && e.code == VK_ESCAPE) return s.openScreen(s.optionsParent);
    s.buttonsInput(e);
}

const ScreenRegistrar kCredits({Screen::CreditsAttribution, buildCredits, drawCredits, inputCredits, nullptr});

// --- WIN_GAME -------------------------------------------------------------------------------------------------------

void handlers(UiState& s, net::Client& client) {
    UiState* sp = &s;
    client.onPlayPeek("game_event", [sp](net::Reader& r) {  // ClientPacketListener: WIN_GAME (4) rolls the poem
        const int event = r.u8v();
        r.f32();
        if (r.ok && event == 4) sp->post([sp] { openWinScreen(*sp, true); });
    });
}

const UiModuleRegistrar kModule({.name = "credits", .registerHandlers = handlers});

}  // namespace

void openWinScreen(UiState& s, bool poem) {
    g = Win{};
    g.poem = poem, g.base = g.speed = poem ? 0.5f : 0.75f;
    std::string player = s.fe ? s.fe->cfg.playerName : std::string();
    if (player.empty()) player = "Player";
    if (poem) addPoem(s, "assets/minecraft/texts/end.txt", player);
    addCredits(s);
    if (poem) addPoem(s, "assets/minecraft/texts/postcredits.txt", player);
    g.total = int(g.lines.size()) * 12;
    if (audio::AudioSystem* a = audio::instance()) a->setScreenMusic("music.credits");  // Musics.CREDITS
    s.openScreen(Screen::Win);
}

}  // namespace ui
