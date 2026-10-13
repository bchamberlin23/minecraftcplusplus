// Front end: the server list (JoinMultiplayerScreen / ServerSelectionList) with live status pings and LAN worlds,
// Add / Edit Server, Direct Connection. Our list is %LOCALAPPDATA%\minecraftCplusplus\servers.json; the game's
// servers.dat is imported into it once and never written.
#include "social/crypto.h"
#include "ui/frontend.h"
#include "ui/modules.h"
#include "world/nbt_dom.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

namespace ui {

namespace {

constexpr int kHeader = 33, kFooter = 60, kRowH = 36, kRowW = 305;

bool isServerScreen(Screen s) {
    return s == Screen::Servers || s == Screen::EditServer || s == Screen::DirectConnect || s == Screen::Confirm ||
           s == Screen::Loading;
}

std::string lastDirect(const fs::path& json) {
    std::vector<u8> bytes;
    if (!readFile(json, bytes)) return {};
    const auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    return j.is_object() ? j.value("lastDirect", "") : "";
}

void saveAll(State& s, const std::string* direct = nullptr) {
    FrontEndState& f = *s.fe;
    const std::string keep = direct ? *direct : lastDirect(f.cfg.serversFile);
    saveServers(f.cfg.serversFile, f.servers);
    // lastDirect rides along in the same file.
    std::vector<u8> bytes;
    if (!readFile(f.cfg.serversFile, bytes)) return;
    auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (!j.is_object()) return;
    j["lastDirect"] = keep;
    std::ofstream(f.cfg.serversFile, std::ios::trunc) << j.dump(2) << "\n";
}

void refreshPings(State& s) { s.fe->pings = std::make_shared<PingBoard>(); }

// ServerSelectionList.THREAD_POOL: five pinger threads taking the pings in order. Detached: a slow server never holds
// up the menu (the board outlives the screen).
void submitPing(std::function<void()> job) {
    static std::mutex mutex;
    static std::deque<std::function<void()>> queue;
    static int running = 0;
    std::lock_guard lock(mutex);
    queue.push_back(std::move(job));
    if (running >= 5) return;
    ++running;
    std::thread([] {
        for (;;) {
            std::function<void()> next;
            {
                std::lock_guard l(mutex);
                if (queue.empty()) {
                    --running;
                    return;
                }
                next = std::move(queue.front());
                queue.pop_front();
            }
            next();
        }
    }).detach();
}

// OnlineServerEntry.extractContent: an entry starts its ping the first time it is drawn.
void startPing(State& s, const std::string& ip) {
    FrontEndState& f = *s.fe;
    if (!f.pings) return;
    {
        std::lock_guard lock(f.pings->mutex);
        if (f.pings->results.count(ip)) return;
        f.pings->results[ip] = {};
    }
    {
        submitPing([board = f.pings, ip, protocol = f.protocol] {
            net::ServerStatus st = net::pingServer(ip, protocol);
            LOG("ping: %s -> %s", ip.c_str(),
                st.ok ? (std::to_string(st.pingMs) + " ms, " + std::to_string(st.online) + "/" + std::to_string(st.max) +
                         " players, " + st.versionName).c_str()
                      : st.error.c_str());
            std::lock_guard lock(board->mutex);
            board->results[ip].status = std::move(st);
            board->results[ip].done = true;
        });
    }
}

void connect(State& s, const std::string& address) {
    FrontEndState& f = *s.fe;
    std::vector<std::wstring> a = launch::stickyArguments();
    a.insert(a.end(), {L"--connect", widen(address), L"--name", widen(f.cfg.playerName)});
    LOG("ui: joining %s", address.c_str());
    f.failTitle = tr(s, "connect.failed");
    startChild(s, a, tr(s, "connect.connecting"), Screen::Servers, true);
}

int rowCount(State& s) { return int(s.fe->servers.size()) + 1 + int(s.fe->lanWorlds.size()); }
float maxScroll(State& s) { return std::max(0.0f, float(rowCount(s) * kRowH + 4) - float(s.guiH() - kHeader - kFooter)); }

void joinSelected(State& s) {
    FrontEndState& f = *s.fe;
    const int n = int(f.servers.size());
    if (f.serverSel >= 0 && f.serverSel < n) return connect(s, f.servers[size_t(f.serverSel)].ip);
    const int lan = f.serverSel - n - 1;
    if (lan >= 0 && lan < int(f.lanWorlds.size())) connect(s, f.lanWorlds[size_t(lan)].address);
}

void openEditServer(State& s, int index) {
    FrontEndState& f = *s.fe;
    f.editServer = index;
    f.serverName.text = TextField{};
    f.serverIp.text = TextField{};
    f.editPackStatus = 0;
    if (index >= 0 && index < int(f.servers.size())) {
        f.serverName.text.value = f.servers[size_t(index)].name;
        f.serverIp.text.value = f.servers[size_t(index)].ip;
        f.editPackStatus = f.servers[size_t(index)].packStatus;
    } else {
        f.serverName.text.value = s.res.translate("selectServer.defaultName");
    }
    for (Field* fl : {&f.serverName, &f.serverIp})
        fl->text.cursor = fl->text.selection = int(fl->text.value.size());
    s.openScreen(Screen::EditServer);
}

// ServerAddress.isValidAddress: Guava HostAndPort.fromString (a bracketed host, or host:port with one colon, the port
// 0..65535 in ASCII digits) with a non-empty host that IDN.toASCII takes (no empty label, ASCII labels of at most 63).
bool validAddress(const std::string& in) {
    std::string host, port;
    bool hasPort = false;
    if (!in.empty() && in[0] == '[') {
        const size_t colon = in.find(':'), close = in.rfind(']');
        if (colon == std::string::npos || close == std::string::npos || close < colon) return false;
        host = in.substr(1, close - 1);
        if (close + 1 < in.size()) {
            if (in[close + 1] != ':') return false;
            hasPort = true, port = in.substr(close + 2);
        }
    } else {
        const size_t colon = in.find(':');
        if (colon != std::string::npos && in.find(':', colon + 1) == std::string::npos)
            host = in.substr(0, colon), hasPort = true, port = in.substr(colon + 1);
        else host = in;
    }
    if (hasPort) {
        if (port.empty() || port.size() > 10) return false;
        for (char ch : port)
            if (ch < '0' || ch > '9') return false;
        if (std::stoll(port) > 65535) return false;
    }
    if (host.empty()) return false;
    if (host == ".") return true;  // IDN: the root label
    for (size_t at = 0; at < host.size();) {
        size_t dot = host.find('.', at);
        if (dot == std::string::npos) dot = host.size();
        const std::string label = host.substr(at, dot - at);
        if (label.empty()) return false;
        bool ascii = true;
        for (unsigned char ch : label) ascii &= ch < 0x80;
        if (ascii && label.size() > 63) return false;
        at = dot + 1;
    }
    return true;
}

void saveEditServer(State& s) {
    FrontEndState& f = *s.fe;
    std::string ip = f.serverIp.text.value, name = f.serverName.text.value;  // ManageServerScreen.onAdd
    if (!validAddress(ip)) return;
    if (name.empty()) name = s.res.translate("selectServer.defaultName");
    if (f.editServer >= 0 && f.editServer < int(f.servers.size())) {
        ServerEntry& e = f.servers[size_t(f.editServer)];
        if (e.ip != ip) e.icon.clear();
        e.name = name, e.ip = ip, e.packStatus = f.editPackStatus;
    } else {
        f.servers.push_back({name, ip, {}, f.editPackStatus});
        f.serverSel = int(f.servers.size()) - 1;
    }
    saveAll(s);
    openServers(s);
}

void deleteServer(State& s) {
    FrontEndState& f = *s.fe;
    if (f.serverSel < 0 || f.serverSel >= int(f.servers.size())) return;
    const int index = f.serverSel;
    State* p = &s;
    showConfirm(s, tr(s, "selectServer.deleteQuestion"),
                Text(formatJava(s.res.translate("selectServer.deleteWarning"), {f.servers[size_t(index)].name})),
                s.res.translate("selectServer.deleteButton"),
                [p, index] {
                    FrontEndState& fe = *p->fe;
                    if (index < int(fe.servers.size())) fe.servers.erase(fe.servers.begin() + index);
                    fe.serverSel = -1;
                    saveAll(*p);
                    openServers(*p);
                },
                Screen::Servers);
}

void moveServer(State& s, int index, int delta) {
    FrontEndState& f = *s.fe;
    const int to = index + delta;
    if (index < 0 || to < 0 || index >= int(f.servers.size()) || to >= int(f.servers.size())) return;
    std::swap(f.servers[size_t(index)], f.servers[size_t(to)]);
    f.serverSel = to;
    saveAll(s);
    s.buildButtons();
}

const char* pingSprite(int ms) {
    return ms < 150 ? "server_list/ping_5" : ms < 300 ? "server_list/ping_4" : ms < 600 ? "server_list/ping_3"
         : ms < 1000 ? "server_list/ping_2" : "server_list/ping_1";
}

}  // namespace

// --- servers.json / servers.dat -----------------------------------------------------------------------------------

std::vector<ServerEntry> loadServers(const fs::path& json, const fs::path& dat) {
    std::vector<ServerEntry> out;
    std::vector<u8> bytes;
    if (readFile(json, bytes)) {
        const auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (j.is_object() && j.contains("servers") && j["servers"].is_array())
            for (const auto& e : j["servers"])
                if (e.is_object()) {
                    ServerEntry s{e.value("name", ""), e.value("ip", ""), e.value("icon", "")};
                    const std::string packs = e.value("resourcePack", "prompt");
                    s.packStatus = packs == "enabled" ? 1 : packs == "disabled" ? 2 : 0;
                    s.acceptedCodeOfConduct = e.value("acceptedCodeOfConduct", 0);
                    out.push_back(std::move(s));
                }
        return out;
    }
    // First run: the game's list (uncompressed NBT: servers = [{name, ip, icon, hidden}]).
    NbtValue root;
    if (!dat.empty() && readFile(dat, bytes) && nbtRead(bytes.data(), bytes.size(), root))
        if (NbtValue* list = root.find("servers"))
            for (NbtValue& e : list->items) {
                const NbtValue* hidden = e.find("hidden");
                if (hidden && hidden->i) continue;
                ServerEntry s;
                if (const NbtValue* v = e.find("name")) s.name = v->s;
                if (const NbtValue* v = e.find("ip")) s.ip = v->s;
                if (const NbtValue* v = e.find("icon")) s.icon = v->s;
                if (const NbtValue* v = e.find("acceptTextures")) s.packStatus = v->i ? 1 : 2;
                if (const NbtValue* v = e.find("acceptedCodeOfConduct")) s.acceptedCodeOfConduct = i32(v->i);
                if (!s.ip.empty()) out.push_back(std::move(s));
            }
    LOG("ui: imported %zu servers from %s", out.size(), utf8(dat).c_str());
    saveServers(json, out);
    return out;
}

void saveServers(const fs::path& json, const std::vector<ServerEntry>& servers) {
    nlohmann::json j;
    j["servers"] = nlohmann::json::array();
    for (const ServerEntry& e : servers) {
        nlohmann::json o{{"name", e.name}, {"ip", e.ip}};
        if (!e.icon.empty()) o["icon"] = e.icon;
        if (e.packStatus) o["resourcePack"] = e.packStatus == 1 ? "enabled" : "disabled";
        if (e.acceptedCodeOfConduct) o["acceptedCodeOfConduct"] = e.acceptedCodeOfConduct;
        j["servers"].push_back(std::move(o));
    }
    std::error_code ec;
    fs::create_directories(json.parent_path(), ec);
    const fs::path tmp = json.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << j.dump(2) << "\n";
        if (!f) return;
    }
    fs::rename(tmp, json, ec);
}

// --- server list --------------------------------------------------------------------------------------------------

void openServers(State& s) {
    FrontEndState& f = *s.fe;
    if (!f.protocol) f.protocol = net::gameProtocol(f.cfg.jar, &f.gameVersion);
    f.servers = loadServers(f.cfg.serversFile, f.cfg.serversDat);
    if (f.serverSel >= int(f.servers.size())) f.serverSel = -1;
    if (!f.lan.running() && !f.lan.start()) LOG("ui: LAN discovery unavailable");
    refreshPings(s);
    s.openScreen(Screen::Servers);
}

void serversFrame(State& s) {
    FrontEndState& f = *s.fe;
    if (f.lan.running() && !isServerScreen(s.screen)) {
        f.lan.stop();
        f.lanWorlds.clear();
    }
    if (f.lan.running()) {
        const size_t before = f.lanWorlds.size();
        f.lanWorlds = f.lan.entries();
        if (f.lanWorlds.size() != before && s.screen == Screen::Servers) s.buildButtons();
    }
    if (!f.pings) return;
    // Favicons are kept in the list, like the game does in servers.dat.
    std::lock_guard lock(f.pings->mutex);
    for (ServerEntry& e : f.servers) {
        auto it = f.pings->results.find(e.ip);
        if (it == f.pings->results.end() || !it->second.done || it->second.status.faviconBase64.empty()) continue;
        if (e.icon != it->second.status.faviconBase64) {
            e.icon = it->second.status.faviconBase64;
            f.serversDirty = true;
        }
    }
    if (f.serversDirty) {
        f.serversDirty = false;
        saveServers(f.cfg.serversFile, f.servers);
    }
}

void buildServers(State& s) {
    FrontEndState& f = *s.fe;
    const int W = s.guiW(), H = s.guiH();
    State* p = &s;
    const int n = int(f.servers.size());
    const bool server = f.serverSel >= 0 && f.serverSel < n;
    const bool any = server || (f.serverSel > n && f.serverSel - n - 1 < int(f.lanWorlds.size()));
    const float x0 = float(W / 2 - 154), y0 = float(H - 52), y1 = float(H - 28);
    addButton(s, x0, y0, 100, tr(s, "selectServer.select"), [p] { joinSelected(*p); }, any);
    addButton(s, x0 + 104, y0, 100, tr(s, "selectServer.direct"), [p] {
        FrontEndState& fe = *p->fe;
        fe.directIp.text = TextField{};
        // DirectJoinServerScreen: options.lastMpIp (the last direct address), else this port's own record of it
        fe.directIp.text.value = !p->settings.lastServer.empty() ? p->settings.lastServer : lastDirect(fe.cfg.serversFile);
        fe.directIp.text.cursor = fe.directIp.text.selection = int(fe.directIp.text.value.size());
        p->openScreen(Screen::DirectConnect);
    });
    addButton(s, x0 + 208, y0, 100, tr(s, "selectServer.add"), [p] { openEditServer(*p, -1); });
    addButton(s, x0, y1, 74, tr(s, "selectServer.edit"), [p] { openEditServer(*p, p->fe->serverSel); }, server);
    addButton(s, x0 + 78, y1, 74, tr(s, "selectServer.delete"), [p] { deleteServer(*p); }, server);
    addButton(s, x0 + 156, y1, 74, tr(s, "selectServer.refresh"), [p] { openServers(*p); });
    addButton(s, x0 + 234, y1, 74, tr(s, "gui.back"), [p] { p->openScreen(Screen::Title); });
    f.onBack = [p] { p->openScreen(Screen::Title); };
    f.onEnter = [p] { joinSelected(*p); };
}

void drawServers(State& s, Canvas& c) {
    FrontEndState& f = *s.fe;
    const int W = c.guiWidth(), H = c.guiHeight();
    menuBackground(s, c);
    s.drawCentered(c, tr(s, "multiplayer.title"), float(W / 2), 12, 0xFFFFFFFF);
    const float top = float(kHeader), bottom = float(H - kFooter);
    listBackground(s, c, top, bottom);
    // the entries' content box: 2 in from the 305-wide row (getContentX / getContentRight)
    const float rowLeft = float(W / 2 - kRowW / 2 + 2), right = rowLeft + kRowW - 4;
    f.serverScroll = std::clamp(f.serverScroll, 0.0f, maxScroll(s));
    std::vector<Text> tooltip;
    std::unordered_map<std::string, PingBoard::Result> results;
    if (f.pings) {
        std::lock_guard lock(f.pings->mutex);
        results = f.pings->results;
    }
    c.pushScissor(0, top, float(W), bottom);
    const int n = int(f.servers.size());
    for (int row = 0; row < rowCount(s); ++row) {
        const float y = top + 4 + float(row) * kRowH - f.serverScroll;
        if (y + kRowH < top || y > bottom) continue;
        const bool hovered = s.mouseX >= rowLeft - 2 && s.mouseX < rowLeft + kRowW - 2 && s.mouseY >= y - 2 &&
                             s.mouseY < y + kRowH - 2 && s.mouseY >= top && s.mouseY < bottom;
        if (row == f.serverSel) {
            c.fill(rowLeft - 2, y - 2, rowLeft + kRowW - 2, y + kRowH - 2, 0xFFFFFFFF);
            c.fill(rowLeft - 1, y - 1, rowLeft + kRowW - 3, y + kRowH - 3, 0xFF000000);
        }
        if (row == n) {  // LANHeader: a LoadingDotsWidget (28 high) at the content's top, frames of 300 ms
            static const char* dots[] = {"O o o", "o O o", "o o O", "o O o"};
            const Text label = tr(s, "lanServer.scanning");
            const int middle = int(rowLeft) + (kRowW - 4) / 2, wx = middle - s.font.width(label) / 2, cx = wx + s.font.width(label) / 2;
            s.drawText(c, label, float(cx - s.font.width(label) / 2), y + 2, 0xFFFFFFFF, true);
            const Text d(dots[i64(s.clock * 1000.0) / 300 % 4]);
            s.drawText(c, d, float(cx - s.font.width(d) / 2), y + 28 - 9 - 2, 0xFF808080, true);
            continue;
        }
        if (row > n) {  // NetworkServerEntry
            // NetworkServerEntry: no icon or join overlay; the motd unclipped, "Address Hidden" with Hide Server Address on
            const net::LanListener::Entry& lan = f.lanWorlds[size_t(row - n - 1)];
            s.drawText(c, tr(s, "lanServer.title"), rowLeft + 35, y + 1, 0xFFFFFFFF, true);
            s.drawText(c, Text(lan.motd), rowLeft + 35, y + 12, 0xFF808080, true);
            s.drawText(c, s.settings.hideServerAddress ? tr(s, "selectServer.hiddenAddress") : Text(lan.address), rowLeft + 35, y + 23, 0xFF808080,
                       true);
            continue;
        }
        const ServerEntry& e = f.servers[size_t(row)];
        startPing(s, e.ip);
        const auto it = results.find(e.ip);
        const bool done = it != results.end() && it->second.done;
        const net::ServerStatus* st = done ? &it->second.status : nullptr;
        // Icon: the status favicon, else the one kept in the list.
        const Sprite* icon = nullptr;
        if (!e.icon.empty()) {
            const std::string key = "server:" + e.ip;
            icon = s.fe->icons.count(key) && s.fe->iconSource[key] == e.icon ? &s.fe->icons[key] : nullptr;
            if (!icon) {
                std::vector<u8> png;
                if (st && !st->favicon.empty() && st->faviconBase64 == e.icon) png = st->favicon;
                else {  // decode the kept base64 once
                    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    u32 acc = 0;
                    int bits = 0;
                    for (char ch : e.icon) {
                        const size_t v = chars.find(ch);
                        if (v == std::string::npos) continue;
                        acc = (acc << 6) | u32(v), bits += 6;
                        if (bits >= 8) bits -= 8, png.push_back(u8(acc >> bits));
                    }
                }
                // FaviconTexture.upload: 64 x 64 only (anything else is dropped from the list)
                Image img;
                if (decodeImage(png, img) && img.w == 64 && img.h == 64) icon = iconSprite(s, key, img, e.icon);
            }
        }
        if (icon) blitSprite(c, icon, rowLeft, y, 32, 32);
        else blitTexture(c, s.sprites.texture("misc/unknown_server"), rowLeft, y, 0, 0, 32, 32, 32, 32);
        s.drawText(c, Text(e.name), rowLeft + 35, y + 1, 0xFFFFFFFF, true);
        // MOTD (two lines), status icon and player count / version.
        const char* statusSprite = nullptr;
        Text status, motd;
        Text statusTip;
        std::vector<Text> playersTip;
        // ServerData.State.PINGING: the animated icon (100 ms frames, two apart per row); it stays so when the
        // connection opened without a status (onPingFailed: "can't connect") or the pong never came
        const bool pinging = !done || (!st->ok && st->connected) || (st->ok && !st->legacy && st->pingMs < 0);
        if (pinging) {
            const int frame = int(i64(s.clock * 1000.0) / 100 + row * 2) & 7;
            const int k = frame > 4 ? 8 - frame : frame;
            static const char* frames[] = {"server_list/pinging_1", "server_list/pinging_2", "server_list/pinging_3",
                                           "server_list/pinging_4", "server_list/pinging_5"};
            statusSprite = frames[k];
            statusTip = tr(s, "multiplayer.status.pinging");
        }
        if (!done) {
            motd = Text(s.res.translate("multiplayer.status.pinging"), colorStyle(0x808080));
        } else if (!st->ok && st->connected) {
            motd = Text(s.res.translate("multiplayer.status.cannot_connect"), colorStyle(0xFF0000));
        } else if (!st->ok) {
            statusSprite = "server_list/unreachable";
            motd = Text(s.res.translate(st->error == "unknown host" ? "multiplayer.status.cannot_resolve"
                                                                   : "multiplayer.status.cannot_connect"),
                        colorStyle(0xFF0000));
            statusTip = tr(s, "multiplayer.status.no_connection");
        } else {
            motd = st->description.is_null() ? Text() : parseJsonComponent(st->description, s.lang());
            // no version: "(old)" and protocol 0; the legacy ping always shows incompatible
            const Text version = st->hasVersion ? Text(st->versionName) : tr(s, "multiplayer.status.old");
            const bool incompatible = st->legacy || (f.protocol && (st->hasVersion ? st->protocol : 0) != f.protocol);
            if (st->hasPlayers) {  // ServerStatusPinger.formatPlayerCount: the numbers gray in dark gray
                status = trf(s, "multiplayer.status.player_count",
                             {Text(std::to_string(st->online), colorStyle(0xAAAAAA)), Text(std::to_string(st->max), colorStyle(0xAAAAAA))});
                for (Span& sp : status.spans) sp.style = sp.style.over(colorStyle(0x555555));
            } else {
                status = Text(s.res.translate("multiplayer.status.unknown"), colorStyle(0x555555));
            }
            if (incompatible) {
                Text red = version;
                for (Span& sp : red.spans) sp.style = sp.style.over(colorStyle(0xFF5555));
                status = red;
            }
            if (!pinging) {  // refreshStatus
                statusSprite = incompatible ? "server_list/incompatible" : pingSprite(st->pingMs);
                statusTip = incompatible ? tr(s, "multiplayer.status.incompatible")
                                         : trf(s, "multiplayer.status.ping", {Text(std::to_string(st->pingMs))});
                for (size_t i = 0; i < st->sample.size(); ++i)  // the anonymous profile is translated
                    playersTip.push_back(i < st->sampleAnonymous.size() && st->sampleAnonymous[i] ? tr(s, "multiplayer.status.anonymous_player")
                                                                                               : Text(st->sample[i]));
                if (!st->sample.empty() && int(st->sample.size()) < st->online)
                    playersTip.push_back(trf(s, "multiplayer.status.and_more", {Text(std::to_string(st->online - int(st->sample.size())))}));
            }
        }
        const std::vector<Text> lines = s.font.split(motd, kRowW - 4 - 32 - 2);  // getContentWidth() - 34
        for (size_t i = 0; i < std::min<size_t>(lines.size(), 2); ++i)
            s.drawText(c, lines[i], rowLeft + 35, y + 12 + 9 * float(i), 0xFF808080, true);
        const float iconX = right - 10 - 5;
        if (statusSprite) blitSprite(c, s.sprites.sprite(statusSprite), iconX, y, 10, 8);
        const int statusW = s.font.width(status);
        const float statusX = iconX - float(statusW) - 5;
        if (!status.empty()) s.drawText(c, status, statusX, y + 1, 0xFF808080, true);
        if (s.mouseX >= iconX && s.mouseX <= iconX + 10 && s.mouseY >= y && s.mouseY <= y + 8 && !statusTip.empty())
            tooltip = {statusTip};
        else if (!playersTip.empty() && s.mouseX >= statusX && s.mouseX <= statusX + float(statusW) && s.mouseY >= y && s.mouseY <= y + 8)
            tooltip = playersTip;
        if (hovered) {  // join (right half), move up / down (left quarters)
            c.fill(rowLeft, y, rowLeft + 32, y + 32, 0xA0909090);
            const float rx = s.mouseX - rowLeft, ry = s.mouseY - y;
            const bool overRight = rx >= 16 && rx < 32 && ry >= 0 && ry < 32;
            blitSprite(c, s.sprites.sprite(overRight ? "server_list/join_highlighted" : "server_list/join"), rowLeft, y, 32, 32);
            if (row > 0)
                blitSprite(c, s.sprites.sprite(rx < 16 && ry < 16 ? "server_list/move_up_highlighted" : "server_list/move_up"),
                           rowLeft, y, 32, 32);
            if (row < n - 1)
                blitSprite(c, s.sprites.sprite(rx < 16 && ry >= 16 && ry < 32 ? "server_list/move_down_highlighted" : "server_list/move_down"),
                           rowLeft, y, 32, 32);
        }
    }
    c.popScissor();
    if (const float ms = maxScroll(s); ms > 0) {
        const float x = rowLeft + kRowW + 4, h = bottom - top;
        const float bar = std::max(32.0f, h * h / (h + ms)), by = top + (h - bar) * f.serverScroll / ms;
        blitSprite(c, s.sprites.sprite("widget/scroller_background"), x, top, 6, h);
        blitSprite(c, s.sprites.sprite("widget/scroller"), x, by, 6, bar);
        s.scrollbarCursor(x, top, h);
    }
    s.drawButtons(c);
    if (!tooltip.empty()) s.drawTooltip(c, tooltip, s.mouseX, s.mouseY);
}

void serversInput(State& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    FrontEndState& f = *s.fe;
    const int W = s.guiW(), H = s.guiH();
    const float top = float(kHeader), bottom = float(H - kFooter), rowLeft = float(W / 2 - kRowW / 2 + 2);
    const int n = int(f.servers.size());
    auto selectable = [&](int row) { return row >= 0 && row < rowCount(s) && row != n; };
    if (e.type == E::Wheel) {
        f.serverScroll = std::clamp(f.serverScroll - float(e.code) * kRowH / 2, 0.0f, maxScroll(s));
        return;
    }
    if (e.type == E::Key && e.down && e.code == VK_F5) return openServers(s);  // JoinMultiplayerScreen: refresh
    // OnlineServerEntry.keyPressed: Shift+Up / Down moves the selected server
    if (e.type == E::Key && e.down && (e.code == VK_UP || e.code == VK_DOWN) && s.window->keyDown(VK_SHIFT) &&
        f.serverSel >= 0 && f.serverSel < n) {
        const int to = f.serverSel + (e.code == VK_DOWN ? 1 : -1);
        if (to >= 0 && to < n) moveServer(s, f.serverSel, e.code == VK_DOWN ? 1 : -1);
        return;
    }
    if (e.type == E::Key && e.down && (e.code == VK_UP || e.code == VK_DOWN)) {
        int row = f.serverSel;
        do row += e.code == VK_DOWN ? 1 : -1;
        while (row >= 0 && row < rowCount(s) && !selectable(row));
        if (selectable(row)) {
            f.serverSel = row;
            const float y = 4 + float(row) * kRowH;
            f.serverScroll = std::clamp(f.serverScroll, y + kRowH - (bottom - top), y);
            s.buildButtons();
        }
        return;
    }
    if (e.type == E::Button && e.down && e.code == 0 && s.mouseY >= top && s.mouseY < bottom && s.mouseX >= rowLeft - 2 &&
        s.mouseX < rowLeft + kRowW - 2) {
        const int row = int(std::floor((s.mouseY - top - 4 + 2 + f.serverScroll) / kRowH));
        if (!selectable(row)) return;
        f.serverSel = row;
        s.buildButtons();
        const float y = top + 4 + float(row) * kRowH - f.serverScroll;
        const float rx = s.mouseX - rowLeft, ry = s.mouseY - y;
        const bool dbl = doubleClicked(s, row);
        if (row < n && rx >= 0 && rx < 16 && ry >= 0 && ry < 16 && row > 0) return moveServer(s, row, -1);
        if (row < n && rx >= 0 && rx < 16 && ry >= 16 && ry < 32 && row < n - 1) return moveServer(s, row, 1);
        // a LAN entry joins on a double click only
        if (dbl || (row < n && rx >= 16 && rx < 32 && ry >= 0 && ry < 32)) joinSelected(s);
        return;
    }
    s.buttonsInput(e);
}

// --- Add / Edit Server (ManageServerScreen) -----------------------------------------------------------------------

void buildEditServer(State& s) {
    FrontEndState& f = *s.fe;
    const int W = s.guiW(), H = s.guiH();
    State* p = &s;
    const float x = float(W / 2 - 100);
    f.serverName.x = x, f.serverName.y = 66, f.serverName.w = 200;
    f.serverIp.x = x, f.serverIp.y = 106, f.serverIp.w = 200;
    f.serverIp.text.maxLength = 128;
    f.serverName.hint = s.res.translate("selectServer.defaultName");  // nameEdit.setHint
    setFields(s, {&f.serverName, &f.serverIp}, 0);
    // CycleButton "Server Resource Packs: Prompt / Enabled / Disabled" (ServerData.ServerPackStatus)
    static const char* const kPackStatus[] = {"manageServer.resourcePack.prompt", "manageServer.resourcePack.enabled",
                                              "manageServer.resourcePack.disabled"};
    addButton(s, x, float(H / 4 + 72), 200, Text(), [p] {
        int& st = p->fe->editPackStatus;  // the enum's order: ENABLED, DISABLED, PROMPT
        st = st == 1 ? 2 : st == 2 ? 0 : 1;
    }).labelFn = [p] {
        return trf(*p, "options.generic_value", {tr(*p, "manageServer.resourcePack"), tr(*p, kPackStatus[p->fe->editPackStatus])});
    };
    Button& done = addButton(s, x, float(H / 4 + 96 + 18), 200, tr(s, "addServer.add"), [p] { saveEditServer(*p); });
    done.labelFn = [p] { return tr(*p, "addServer.add"); };
    addButton(s, x, float(H / 4 + 120 + 18), 200, tr(s, "gui.cancel"), [p] { openServers(*p); });
    f.onBack = [p] { openServers(*p); };
    f.onEnter = nullptr;  // ManageServerScreen has no Enter shortcut
}

void drawEditServer(State& s, Canvas& c) {
    FrontEndState& f = *s.fe;
    const int W = c.guiWidth();
    const float x = float(W / 2 - 100);
    menuBackground(s, c);
    s.drawCentered(c, tr(s, f.editServer >= 0 ? "manageServer.edit.title" : "manageServer.add.title"), float(W / 2), 17,
                   0xFFFFFFFF);
    s.drawText(c, tr(s, "manageServer.enterName"), x + 1, 53, 0xFFA0A0A0, true);
    s.drawText(c, tr(s, "manageServer.enterIp"), x + 1, 94, 0xFFA0A0A0, true);
    // Done needs a valid address (ManageServerScreen.updateAddButtonStatus).
    for (Button& b : s.buttons)
        if (b.labelFn && b.y > float(s.guiH() / 4 + 72)) b.active = validAddress(f.serverIp.text.value);
    drawFields(s, c);
    s.drawButtons(c);
}

// --- Direct Connection (DirectJoinServerScreen) -------------------------------------------------------------------

void buildDirectConnect(State& s) {
    FrontEndState& f = *s.fe;
    const int W = s.guiW(), H = s.guiH();
    State* p = &s;
    const float x = float(W / 2 - 100);
    f.directIp.x = x, f.directIp.y = 116, f.directIp.w = 200;
    f.directIp.text.maxLength = 128;
    setFields(s, {&f.directIp}, 0);
    // DirectJoinServerScreen.removed: the address typed is kept as lastMpIp however the screen closes
    auto keep = [p] {
        const std::string ip = p->fe->directIp.text.value;
        saveAll(*p, &ip);
        p->settings.lastServer = ip, p->settings.save();
    };
    auto join = [p, keep] {
        const std::string ip = p->fe->directIp.text.value;
        if (!validAddress(ip)) return;
        keep();
        connect(*p, ip);
    };
    addButton(s, x, float(H / 4 + 96 + 12), 200, tr(s, "selectServer.select"), join).labelFn = [p] {
        return tr(*p, "selectServer.select");
    };
    auto cancel = [p, keep] {
        keep();
        openServers(*p);
    };
    addButton(s, x, float(H / 4 + 120 + 12), 200, tr(s, "gui.cancel"), cancel);
    f.onBack = cancel;
    f.onEnter = join;  // keyPressed: confirmation with the address valid
}

void drawDirectConnect(State& s, Canvas& c) {
    const int W = c.guiWidth();
    menuBackground(s, c);
    s.drawCentered(c, tr(s, "selectServer.direct"), float(W / 2), 20, 0xFFFFFFFF);
    s.drawText(c, tr(s, "manageServer.enterIp"), float(W / 2 - 100 + 1), 100, 0xFFA0A0A0, true);
    for (Button& b : s.buttons)  // updateSelectButtonStatus
        if (b.labelFn) b.active = validAddress(s.fe->directIp.text.value);
    drawFields(s, c);
    s.drawButtons(c);
}

namespace {

// ClientboundServerDataPacket: the server list entry of this connection takes the server's icon (ServerData.validateIcon:
// a 64x64 PNG) and is saved (ServerList.saveSingleServer); the motd is not kept in the list.
void registerServerData(UiState& s, net::Client& client) {
    UiState* sp = &s;
    client.onPlay("server_data", [sp](net::Reader& r) {
        r.nbt();  // motd
        std::vector<u8> icon;
        if (r.boolean()) {
            const int n = r.varInt();
            for (int i = 0; i < n && r.ok; ++i) icon.push_back(r.u8v());
        }
        if (!r.ok || icon.empty()) return;
        static const u8 kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        auto be32 = [&](size_t at) { return u32(icon[at]) << 24 | u32(icon[at + 1]) << 16 | u32(icon[at + 2]) << 8 | u32(icon[at + 3]); };
        if (icon.size() < 24 || !std::equal(kPng, kPng + 8, icon.begin()) || be32(16) != 64 || be32(20) != 64) return;
        sp->post([sp, icon] {
            const std::string& address = sp->fe->cfg.serverAddress;
            if (address.empty()) return;
            std::vector<ServerEntry> list = loadServers(sp->fe->cfg.serversFile, sp->fe->cfg.serversDat);
            for (ServerEntry& e : list)
                if (e.ip == address) {
                    e.icon = social::base64(icon.data(), icon.size());
                    saveServers(sp->fe->cfg.serversFile, list);
                    return;
                }
        });
    });
}

const UiModuleRegistrar kServerData({.name = "server_data", .registerHandlers = registerServerData});

// SafetyScreen (WarningScreen): the bold title, the message in a text area (width - 100, at most height - 100 high,
// padded 12), the "do not show again" box and Proceed / Back; spaced 8 and centred.
struct Safety {
    bool stopShowing = false;
    float scroll = 0;
} safety;

struct SafetyLayout {
    int top, boxX, boxY, boxW, boxH, checkY, buttonsY, checkW;
    std::vector<Text> lines;
};
SafetyLayout safetyLayout(State& s) {
    SafetyLayout l;
    const int W = s.guiW(), H = s.guiH();
    l.boxW = W - 100;
    l.lines = s.font.split(tr(s, "multiplayerWarning.message"), l.boxW - 8);
    l.boxH = std::min(int(l.lines.size()) * 9 + 8, H - 100);
    l.checkW = 17 + 4 + s.font.width(tr(s, "multiplayerWarning.check"));
    const int total = 9 + 8 + 24 + l.boxH + 8 + 17 + 8 + 20;
    l.top = (H - total) / 2;
    l.boxX = 50, l.boxY = l.top + 9 + 8 + 12;
    l.checkY = l.boxY + l.boxH + 12 + 8;
    l.buttonsY = l.checkY + 17 + 8;
    return l;
}

void safetyBuild(State& s) {
    const SafetyLayout l = safetyLayout(s);
    State* p = &s;
    addButton(s, float(s.guiW() / 2 - 154), float(l.buttonsY), 150, tr(s, "gui.proceed"), [p] {
        if (safety.stopShowing) {
            p->settings.skipMultiplayerWarning = true;
            p->settings.save();
        }
        openServers(*p);
    });
    addButton(s, float(s.guiW() / 2 + 4), float(l.buttonsY), 150, tr(s, "gui.back"), [p] { p->openScreen(Screen::Title); });
    s.fe->onBack = [p] { p->openScreen(Screen::Title); };
}

void safetyDraw(State& s, Canvas& c) {
    menuBackground(s, c);
    const SafetyLayout l = safetyLayout(s);
    const float cx = float(c.guiWidth() / 2);
    Style bold;
    bold.bold = 1;
    s.drawCentered(c, Text(s.res.translate("multiplayerWarning.header"), bold), cx, float(l.top), 0xFFFFFFFF);
    blitSprite(c, s.sprites.sprite("widget/text_field"), float(l.boxX), float(l.boxY), float(l.boxW), float(l.boxH));
    const float maxScroll = std::max(0.0f, float(l.lines.size()) * 9 + 8 - float(l.boxH));
    safety.scroll = std::clamp(safety.scroll, 0.0f, maxScroll);
    c.pushScissor(float(l.boxX + 1), float(l.boxY + 1), float(l.boxX + l.boxW - 1), float(l.boxY + l.boxH - 1));
    for (size_t i = 0; i < l.lines.size(); ++i) {
        const float y = float(l.boxY + 4 + int(i) * 9) - safety.scroll;
        if (y + 9 < float(l.boxY) || y > float(l.boxY + l.boxH)) continue;
        s.drawText(c, l.lines[i], float(l.boxX + 4), y, 0xFFFFFFFF, true);
    }
    c.popScissor();
    const float x = cx - float(l.checkW / 2);
    blitSprite(c, s.sprites.sprite(safety.stopShowing ? "widget/checkbox_selected" : "widget/checkbox"), x, float(l.checkY), 17, 17);
    s.drawText(c, tr(s, "multiplayerWarning.check"), x + 21, float(l.checkY + 4), 0xFFE0E0E0, true);
    s.drawButtons(c);
}

void safetyInput(State& s, const InputEvent& e) {
    if (e.type == InputEvent::Wheel) {
        safety.scroll -= float(e.code) * 9 * 2;
        return;
    }
    if (e.type == InputEvent::Button && e.down && e.code == 0) {
        const SafetyLayout l = safetyLayout(s);
        const float x = float(s.guiW() / 2 - l.checkW / 2);
        if (s.mouseX >= x && s.mouseX < x + float(l.checkW) && s.mouseY >= float(l.checkY) && s.mouseY < float(l.checkY + 17)) {
            safety.stopShowing = !safety.stopShowing;
            return;
        }
    }
    s.buttonsInput(e);
}

void safetyClosed(State&) { safety = Safety{}; }

const ScreenRegistrar kSafetyScreen({Screen::MultiplayerWarning, safetyBuild, safetyDraw, safetyInput, safetyClosed});

}  // namespace

}  // namespace ui
