// Server-driven configuration, answered on the main thread (net::Client::pollCommon):
//  - server resource packs: the pack prompt (ClientCommonPacketListenerImpl.PackConfirmScreen) or the server entry's
//    "Server Resource Packs" setting, ServerPackManager's lifecycle with the game's status responses, downloads checked
//    against their SHA-1 into a cache (DownloadedPackSource / DownloadQueue), applied on top of the client asset stack
//    with a reload (FrontEnd::reloadResources: block textures; here: GUI sprites, item icons, language);
//  - the Code of Conduct screen (CodeOfConductScreen, remembered per server entry like ServerData);
//  - reset_chat, reconfiguration (ServerReconfigScreen) and transfers to another server with the cookies
//    (ClientCommonPacketListenerImpl.handleTransfer: the next instance connects with the transfer intent).
#include "assets/asset_source.h"
#include "assets/zip.h"
#include "net/client.h"
#include "net/session.h"
#include "ui/frontend.h"
#include "ui/loading_overlay.h"
#include "ui/modules.h"
#include "ui/toasts.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <atomic>
#include <fstream>
#include <thread>

namespace ui {

namespace {

// ServerboundResourcePackPacket.Action
enum Action { kLoaded = 0, kDeclined, kFailedDownload, kAccepted, kDownloaded, kInvalidUrl, kFailedReload, kDiscarded };
constexpr int kNoResult = -1, kSilent = -2;  // RemovalReason: none yet / SERVER_REMOVED, SERVER_REPLACED (no response)
enum class PromptStatus { Pending, Allowed, Declined };  // ServerPackManager.PackPromptStatus
enum EntryPackStatus { kPrompt = 0, kEnabled = 1, kDisabled = 2 };  // ServerData.ServerPackStatus

struct Pack {  // ServerPackManager.ServerPackData
    std::string id, url, hash;  // id: 16 bytes; hash: lower-case SHA-1 hex or "" (not checked)
    enum class Download { Requested, Pending, Done } download = Download::Requested;
    enum class Activation { Inactive, Pending, Active } activation = Activation::Inactive;
    int removal = kNoResult;  // the final result reported once inactive (kSilent: none)
    bool accepted = false;
    fs::path path;
    bool removed() const { return removal != kNoResult; }
    void remove(int reason) {
        if (removal == kNoResult) removal = reason;
    }
};

// One download batch (DownloadQueue.downloadBatch) on a worker thread.
struct Batch {
    struct Item {
        std::string id, url, hash;
        fs::path path;
        bool ok = false;
    };
    std::vector<Item> items;
    std::atomic<bool> done{false};
};

struct Request {  // PackConfirmScreen.PendingRequest
    std::string id, url, hash;
};

struct ServerConfig {
    net::Client* client = nullptr;
    bool started = false;  // configureForServerControl for this connection
    PromptStatus prompt = PromptStatus::Pending;
    std::vector<Pack> packs;
    std::shared_ptr<Batch> batch;
    size_t basePacks = SIZE_MAX;  // the client asset stack without server packs
    int serial = 0;               // DownloadedPackSource.packIdSerialNumber
    // Pack prompt
    std::vector<Request> requests;
    bool required = false;
    Text promptMessage;
    Screen promptBack = Screen::None;
    // Code of conduct
    std::string coc;
    bool cocDontShow = false;
    float cocScroll = 0;
    Screen cocBack = Screen::None;
    // Reconfiguration
    double reconfigStart = 0;
    bool reconfiguring = false;
} g;

const char* const kStatusNames[] = {"prompt", "enabled", "disabled"};

std::string hex(const u8* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) s += d[p[i] >> 4], s += d[p[i] & 15];
    return s;
}

std::string uuidText(const std::string& id) {  // 8-4-4-4-12
    const std::string h = hex(reinterpret_cast<const u8*>(id.data()), id.size());
    return h.size() < 32 ? h : h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20);
}

std::string sha1Of(const fs::path& file) {
    std::vector<u8> bytes;
    if (!readFile(file, bytes)) return {};
    u8 digest[20];
    if (BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, bytes.data(), ULONG(bytes.size()), digest, 20) != 0) return {};
    return hex(digest, 20);
}

// String.hashCode of the text (ServerData.acceptedCodeOfConduct): over UTF-16 code units.
i32 javaHash(const std::string& utf8) {
    std::wstring w(utf8.size() + 1, L'\0');
    w.resize(size_t(MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), w.data(), int(w.size()))));
    u32 h = 0;
    for (wchar_t c : w) h = 31 * h + u16(c);
    return i32(h);
}

// --- the server list entry of this connection (ServerData) ---------------------------------------------------------

template <class Fn>
bool withEntry(State& s, Fn&& fn, bool save) {
    FrontEndState& f = *s.fe;
    if (f.cfg.serverAddress.empty() || f.cfg.serversFile.empty()) return false;
    std::vector<ServerEntry> list = loadServers(f.cfg.serversFile, f.cfg.serversDat);
    for (ServerEntry& e : list)
        if (e.ip == f.cfg.serverAddress) {
            fn(e);
            if (save) saveServers(f.cfg.serversFile, list);  // ServerList.saveSingleServer
            return true;
        }
    return false;
}

int entryPackStatus(State& s) {
    if (s.fe->cfg.singleplayer) return kEnabled;  // configureForLocalWorld: a world's own pack needs no prompt
    int status = kPrompt;
    withEntry(s, [&](ServerEntry& e) { status = e.packStatus; }, false);
    return status;
}

void setEntryPackStatus(State& s, int status) {
    withEntry(s, [&](ServerEntry& e) { e.packStatus = status; }, true);
}

// --- ServerPackManager ------------------------------------------------------------------------------------------

void report(const Pack& p, int action) {
    LOG("packs: %s -> %d", uuidText(p.id).c_str(), action);
    if (g.client) g.client->sendPackStatus(p.id, action);
}

void accept(Pack& p) {
    report(p, kAccepted);
    p.accepted = true;
}

void pushPack(const std::string& id, const std::string& url, const std::string& hash) {
    if (g.prompt == PromptStatus::Declined) {
        Pack p;
        p.id = id;
        report(p, kDeclined);
        return;
    }
    for (Pack& p : g.packs)
        if (p.id == id) p.remove(kSilent);  // SERVER_REPLACED
    Pack p;
    p.id = id, p.url = url, p.hash = hash;
    g.packs.push_back(std::move(p));
    if (g.prompt == PromptStatus::Allowed) accept(g.packs.back());
}

void allowServerPacks() {
    g.prompt = PromptStatus::Allowed;
    for (Pack& p : g.packs)
        if (!p.accepted && !p.removed()) accept(p);
}

void rejectServerPacks() {
    g.prompt = PromptStatus::Declined;
    for (Pack& p : g.packs)
        if (!p.accepted) p.remove(kDeclined);
}

void popPack(const std::string& id) {
    for (Pack& p : g.packs)
        if (!p.removed() && (id.empty() || p.id == id)) {
            p.remove(kSilent);  // SERVER_REMOVED
            if (!id.empty()) break;
        }
}

// DownloadedPackSource.createDownloadHeaders
std::wstring downloadHeaders(State& s) {
    FrontEndState& f = *s.fe;
    const std::string name = f.cfg.playerName;
    const std::string off = "OfflinePlayer:" + name;
    u8 uuid[16];
    BCryptHash(BCRYPT_MD5_ALG_HANDLE, nullptr, 0, PUCHAR(off.data()), ULONG(off.size()), uuid, 16);
    uuid[6] = u8((uuid[6] & 0x0F) | 0x30), uuid[8] = u8((uuid[8] & 0x3F) | 0x80);
    std::string packFormat;
    ZipArchive zip;
    std::vector<u8> bytes;
    if (zip.open(f.cfg.jar))
        for (const auto& e : zip.entries())
            if (e.name == "version.json" && zip.read(e, bytes)) {
                const auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
                if (j.is_object() && j.contains("pack_version") && j["pack_version"].is_object())
                    packFormat = std::to_string(j["pack_version"].value("resource_major", 0)) + "." +
                                 std::to_string(j["pack_version"].value("resource_minor", 0));
            }
    const std::string h = "X-Minecraft-Username: " + name + "\r\nX-Minecraft-UUID: " + hex(uuid, 16) +
                          "\r\nX-Minecraft-Version: " + f.cfg.version + "\r\nX-Minecraft-Version-ID: " + f.cfg.version +
                          "\r\nX-Minecraft-Pack-Format: " + packFormat + "\r\nUser-Agent: Minecraft Java/" + f.cfg.version;
    return widen(h);
}

// HTTP(S) GET into `to` (at most 250 MB, like the game's MAX_PACK_SIZE_BYTES).
bool download(const std::string& url, const std::wstring& headers, const fs::path& to) {
    const std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host, uc.dwHostNameLength = DWORD(std::size(host));
    uc.lpszUrlPath = path, uc.dwUrlPathLength = DWORD(std::size(path));
    wchar_t extra[2048] = {};
    uc.lpszExtraInfo = extra, uc.dwExtraInfoLength = DWORD(std::size(extra));
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
    const std::wstring object = std::wstring(path) + extra;
    HINTERNET session = WinHttpOpen(L"minecraftCplusplus", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET conn = session ? WinHttpConnect(session, host, uc.nPort, 0) : nullptr;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
    bool ok = req && WinHttpSendRequest(req, headers.c_str(), DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    DWORD status = 0, size = sizeof(status);
    if (ok)
        ok = WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                 &status, &size, WINHTTP_NO_HEADER_INDEX) &&
             status == 200;
    if (ok) {
        std::ofstream out(to, std::ios::binary | std::ios::trunc);
        std::vector<char> buf(1 << 16);
        u64 total = 0;
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(req, buf.data(), DWORD(buf.size()), &got)) {
                ok = false;
                break;
            }
            if (!got) break;
            total += got;
            if (total > 262144000ull) {
                ok = false;
                break;
            }
            out.write(buf.data(), got);
        }
        ok = ok && bool(out);
    }
    if (!ok) LOG("packs: download of %s failed (HTTP %lu)", url.c_str(), status);
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

void startDownloads(State& s, std::vector<Pack*> todo) {
    auto batch = std::make_shared<Batch>();
    for (Pack* p : todo) batch->items.push_back({p->id, p->url, p->hash});
    g.batch = batch;
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    const fs::path cache = fs::path(local ? local : L".") / "minecraftCplusplus" / "downloads";
    const int count = int(todo.size());
    showToast(translatable("download.pack.title", {Text("1"), Text(std::to_string(count))}, s.lang()));
    std::thread([batch, cache, headers = downloadHeaders(s)] {
        for (Batch::Item& it : batch->items) {
            // DownloadQueue: <cache>/<pack id>/<sha1>; a cached file with the expected hash is not downloaded again.
            const fs::path dir = cache / uuidText(it.id);
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (!it.hash.empty() && fs::exists(dir / it.hash, ec) && sha1Of(dir / it.hash) == it.hash) {
                it.path = dir / it.hash, it.ok = true;
                LOG("packs: %s from the cache", it.url.c_str());
                continue;
            }
            const fs::path tmp = dir / "download.tmp";
            if (!download(it.url, headers, tmp)) continue;
            const std::string sha1 = sha1Of(tmp);
            if (!it.hash.empty() && sha1 != it.hash) {
                LOG("packs: %s has SHA-1 %s, expected %s", it.url.c_str(), sha1.c_str(), it.hash.c_str());
                fs::remove(tmp, ec);
                continue;
            }
            fs::remove(dir / sha1, ec);
            fs::rename(tmp, dir / sha1, ec);
            it.path = dir / sha1, it.ok = !ec;
            LOG("packs: downloaded %s (SHA-1 %s)", it.url.c_str(), sha1.c_str());
        }
        batch->done = true;
    }).detach();
}

// ServerPackManager.onDownload
void finishDownloads(State& s) {
    std::shared_ptr<Batch> batch = std::move(g.batch);
    int failed = 0;
    for (const Batch::Item& it : batch->items) failed += !it.ok;
    if (failed) {
        for (Pack& p : g.packs) {
            if (p.activation == Pack::Activation::Active) continue;
            bool thisFailed = false;
            for (const Batch::Item& it : batch->items) thisFailed |= it.id == p.id && !it.ok;
            p.remove(thisFailed ? kFailedDownload : kDiscarded);
        }
        showToast(translatable("download.pack.failed", {Text(std::to_string(failed)), Text(std::to_string(batch->items.size()))},
                               s.lang()));
    }
    for (const Batch::Item& it : batch->items) {
        if (!it.ok) continue;
        for (Pack& p : g.packs)
            if (p.id == it.id && p.download == Pack::Download::Pending) {
                p.download = Pack::Download::Done, p.path = it.path;
                if (!p.removed()) report(p, kDownloaded);
            }
    }
}

// DownloadedPackSource.startReload + Minecraft.reloadResourcePacks: the packs over the asset stack (the first pushed
// on top), then everything that reads it again. False: a pack is not a resource pack (no pack.mcmeta).
bool applyPacks(State& s, const std::vector<Pack*>& load) {
    std::vector<std::shared_ptr<AssetPack>> opened;
    for (auto it = load.rbegin(); it != load.rend(); ++it) {
        char name[64];
        std::snprintf(name, sizeof(name), "server/%08X/", g.serial++);
        std::shared_ptr<AssetPack> pack = AssetPack::openZip((*it)->path, name + uuidText((*it)->id));
        if (!pack || !pack->has("pack.mcmeta")) {
            LOG("packs: invalid pack metadata in %s, ignoring all", utf8((*it)->path).c_str());
            return false;
        }
        opened.push_back(std::move(pack));
    }
    AssetSource& src = AssetSource::client();
    if (g.basePacks == SIZE_MAX) g.basePacks = src.packCount();
    src.truncate(g.basePacks);
    for (auto& p : opened) src.add(std::move(p));
    LOG("packs: %zu server pack(s) over the game's assets; reloading", opened.size());
    startLoadingOverlay(true);  // Minecraft.reloadResourcePacks (the reload itself runs right here)
    s.res.open(s.fe->cfg.jar, s.fe->cfg.minecraftDir);  // language
    s.sprites.reload();
    s.icons.reload();
    if (auto& fn = s.fe->cfg.reloadResources; fn && !fn()) return false;  // block textures (main.cpp)
    return true;
}

// ServerPackManager.triggerReloadIfNeeded (the reload finishes right away here)
void reloadIfNeeded(State& s) {
    bool needsReload = false;
    std::vector<Pack*> load, unload;
    for (Pack& p : g.packs) {
        const bool shouldBeActive = p.accepted && p.download == Pack::Download::Done && !p.removed();
        if (shouldBeActive && p.activation == Pack::Activation::Inactive) load.push_back(&p), needsReload = true;
        if (p.activation == Pack::Activation::Active) {
            if (!shouldBeActive) needsReload = true, unload.push_back(&p);
            else load.push_back(&p);
        }
    }
    if (!needsReload) return;
    if (applyPacks(s, load)) {
        for (Pack* p : load) {
            const bool wasActive = p->activation == Pack::Activation::Active;
            p->activation = Pack::Activation::Active;
            if (!wasActive && !p->removed()) report(*p, kLoaded);
        }
        for (Pack* p : unload) p->activation = Pack::Activation::Inactive;
        return;
    }
    // onFailure(false): new packs fail, active ones are loaded again (the recovery).
    std::vector<Pack*> recovery;
    for (Pack& p : g.packs) {
        if (p.activation == Pack::Activation::Active) {
            recovery.push_back(&p);
        } else if (std::find(load.begin(), load.end(), &p) != load.end()) {
            p.remove(kFailedReload);
        } else {
            p.remove(kDiscarded);
        }
    }
    showToast(tr(s, "multiplayer.texturePrompt.failure.line1"), tr(s, "multiplayer.texturePrompt.failure.line2"));
    if (!applyPacks(s, recovery)) LOG("packs: double failure in loading server packs");
}

// ServerPackManager.tick
void tickPacks(State& s) {
    if (g.batch && g.batch->done) finishDownloads(s);
    std::vector<Pack*> todo;
    bool downloading = false;
    for (Pack& p : g.packs) {
        if (p.removed() || !p.accepted) continue;
        downloading |= p.download != Pack::Download::Done;
        if (p.download == Pack::Download::Requested && !g.batch) p.download = Pack::Download::Pending, todo.push_back(&p);
    }
    if (!todo.empty()) startDownloads(s, todo);
    if (!downloading) reloadIfNeeded(s);
    std::erase_if(g.packs, [](const Pack& p) {  // cleanupRemovedPacks
        if (p.activation != Pack::Activation::Inactive || !p.removed()) return false;
        if (p.removal >= 0) report(p, p.removal);
        return true;
    });
}

std::string lowerHash(const std::string& h) {  // tryParseSha1Hash: 40 hex digits, else not checked
    if (h.size() != 40) return {};
    std::string out;
    for (char c : h) {
        if (!std::isxdigit(u8(c))) return {};
        out += char(std::tolower(u8(c)));
    }
    return out;
}

// --- screens -------------------------------------------------------------------------------------------------------

void reopen(State& s, Screen back) {
    if (back == Screen::None) s.closeScreen(false);
    else s.openScreen(back);
}

Screen currentBack(State& s) {
    return s.screen == Screen::ServerPackPrompt || s.screen == Screen::CodeOfConduct ? Screen::None : s.screen;
}

void disconnect(State& s, const char* key) {
    if (g.client) g.client->disconnect(s.res.translate(key));
}

void packAnswer(State& s, bool yes) {
    const std::vector<Request> requests = std::move(g.requests);
    const bool required = g.required;
    g.requests.clear();
    reopen(s, g.promptBack);
    if (yes) {
        setEntryPackStatus(s, kEnabled);
        allowServerPacks();
    } else {
        rejectServerPacks();
        if (required) disconnect(s, "multiplayer.requiredTexturePrompt.disconnect");
        else setEntryPackStatus(s, kDisabled);
    }
    for (const Request& r : requests) pushPack(r.id, r.url, r.hash);
}

int promptTop(State& s, int lines) { return (s.guiH() - (9 + 8 + lines * 9 + 8 + 16 + 20)) / 2; }

std::vector<Text> promptLines(State& s) { return s.font.split(g.promptMessage, s.guiW() - 50); }

void promptBuild(State& s) {
    const int lines = std::min(15, int(promptLines(s).size()));
    const float y = float(promptTop(s, lines) + 9 + 8 + lines * 9 + 8 + 16);
    State* p = &s;
    addButton(s, float(s.guiW() / 2 - 152), y, 150, tr(s, g.required ? "gui.proceed" : "gui.yes"), [p] { packAnswer(*p, true); });
    addButton(s, float(s.guiW() / 2 + 2), y, 150, tr(s, g.required ? "menu.disconnect" : "gui.no"), [p] { packAnswer(*p, false); });
}

void promptDraw(State& s, Canvas& c) {
    menuBackground(s, c);
    std::vector<Text> lines = promptLines(s);
    if (lines.size() > 15) lines.resize(15);
    const int top = promptTop(s, int(lines.size()));
    const float cx = float(c.guiWidth() / 2);
    s.drawCentered(c, tr(s, g.required ? "multiplayer.requiredTexturePrompt.line1" : "multiplayer.texturePrompt.line1"), cx,
                   float(top), 0xFFFFFFFF);
    for (size_t i = 0; i < lines.size(); ++i) s.drawCentered(c, lines[i], cx, float(top + 17 + int(i) * 9), 0xFFFFFFFF);
    s.drawButtons(c);
}

void promptInput(State& s, const InputEvent& e) {
    if (e.type == InputEvent::Key && e.down && e.code == VK_ESCAPE) return packAnswer(s, false);
    s.buttonsInput(e);
}

const ScreenRegistrar kPromptScreen({Screen::ServerPackPrompt, promptBuild, promptDraw, promptInput, nullptr});

void showPrompt(State& s, const net::PackPushEvent& e) {
    // addOrUpdatePackPrompt: a prompt already open takes the request too (and this push's wording).
    if (s.screen != Screen::ServerPackPrompt) g.promptBack = currentBack(s), g.requests.clear();
    g.requests.push_back({e.id, e.url, lowerHash(e.hash)});
    g.required = e.required;
    Style st;
    if (e.required) st.color = 0xFFFF55, st.bold = 1;
    Text header = tr(s, e.required ? "multiplayer.requiredTexturePrompt.line2" : "multiplayer.texturePrompt.line2");
    if (e.required) header = Text(header.plain(), st);
    g.promptMessage = e.prompt.empty() ? header
                                       : translatable("multiplayer.texturePrompt.serverPrompt",
                                                      {header, parseComponent(e.prompt, s.lang())}, s.lang());
    s.openScreen(Screen::ServerPackPrompt);
}

// CodeOfConductScreen (WarningScreen): title, the text in a scrollable box, "do not notify again", two buttons.
struct CocLayout {
    int top, boxX, boxY, boxW, boxH, checkY, buttonsY;
    std::vector<Text> lines;
};
CocLayout cocLayout(State& s) {
    CocLayout l;
    const int W = s.guiW(), H = s.guiH();
    l.boxW = W - 100;
    l.lines = s.font.split(Text(g.coc), l.boxW - 8);
    l.boxH = std::min(int(l.lines.size()) * 9 + 8, H - 100);
    const int total = 9 + 8 + 24 + l.boxH + 8 + 17 + 8 + 20;
    l.top = (H - total) / 2;
    l.boxX = 50, l.boxY = l.top + 9 + 8 + 12;
    l.checkY = l.boxY + l.boxH + 12 + 8;
    l.buttonsY = l.checkY + 17 + 8;
    return l;
}

void cocAnswer(State& s, bool accepted) {
    // CodeOfConductScreen.onResult: remembered when accepted with the box ticked, forgotten otherwise.
    const std::string text = g.coc;
    const bool remember = accepted && g.cocDontShow;
    withEntry(s, [&](ServerEntry& e) { e.acceptedCodeOfConduct = remember ? javaHash(text) : 0; }, true);
    if (accepted) {
        if (g.client) g.client->acceptCodeOfConduct();
        reopen(s, g.cocBack);
    } else {
        disconnect(s, "multiplayer.disconnect.code_of_conduct");
    }
}

int checkWidth(State& s) { return 17 + 4 + s.font.width(tr(s, "multiplayer.codeOfConduct.check")); }

void cocBuild(State& s) {
    const CocLayout l = cocLayout(s);
    State* p = &s;
    addButton(s, float(s.guiW() / 2 - 154), float(l.buttonsY), 150, tr(s, "gui.acknowledge"), [p] { cocAnswer(*p, true); });
    addButton(s, float(s.guiW() / 2 + 4), float(l.buttonsY), 150, tr(s, "menu.disconnect"), [p] { cocAnswer(*p, false); });
}

void cocDraw(State& s, Canvas& c) {
    menuBackground(s, c);
    const CocLayout l = cocLayout(s);
    const float cx = float(c.guiWidth() / 2);
    Style bold;
    bold.bold = 1;
    s.drawCentered(c, Text(s.res.translate("multiplayer.codeOfConduct.title"), bold), cx, float(l.top), 0xFFFFFFFF);
    blitSprite(c, s.sprites.sprite("widget/text_field"), float(l.boxX), float(l.boxY), float(l.boxW), float(l.boxH));
    const float maxScroll = std::max(0.0f, float(l.lines.size()) * 9 + 8 - float(l.boxH));
    g.cocScroll = std::clamp(g.cocScroll, 0.0f, maxScroll);
    c.pushScissor(float(l.boxX + 1), float(l.boxY + 1), float(l.boxX + l.boxW - 1), float(l.boxY + l.boxH - 1));
    for (size_t i = 0; i < l.lines.size(); ++i) {
        const float y = float(l.boxY + 4 + int(i) * 9) - g.cocScroll;
        if (y + 9 < float(l.boxY) || y > float(l.boxY + l.boxH)) continue;
        s.drawText(c, l.lines[i], float(l.boxX + 4), y, 0xFFFFFFFF, true);
    }
    c.popScissor();
    const int cw = checkWidth(s);
    const float x = cx - float(cw / 2);
    blitSprite(c, s.sprites.sprite(g.cocDontShow ? "widget/checkbox_selected" : "widget/checkbox"), x, float(l.checkY), 17, 17);
    s.drawText(c, tr(s, "multiplayer.codeOfConduct.check"), x + 21, float(l.checkY + 4), 0xFFFFFFFF, true);
    s.drawButtons(c);
}

void cocInput(State& s, const InputEvent& e) {
    if (e.type == InputEvent::Key) return;  // shouldCloseOnEsc: false
    if (e.type == InputEvent::Wheel) {
        g.cocScroll -= float(e.code) * 9 * 2;
        return;
    }
    if (e.type == InputEvent::Button && e.down && e.code == 0) {
        const CocLayout l = cocLayout(s);
        const float x = float(s.guiW() / 2 - checkWidth(s) / 2);
        if (s.mouseX >= x && s.mouseX < x + float(checkWidth(s)) && s.mouseY >= float(l.checkY) && s.mouseY < float(l.checkY + 17)) {
            g.cocDontShow = !g.cocDontShow;
            return;
        }
    }
    s.buttonsInput(e);
}

const ScreenRegistrar kCocScreen({Screen::CodeOfConduct, cocBuild, cocDraw, cocInput, nullptr});

// ServerReconfigScreen: the title and a Disconnect button that turns active after 30 seconds.
void reconfigBuild(State& s) {
    State* p = &s;
    Button& b = addButton(s, float(s.guiW() / 2 - 75), float(s.guiH() / 2 - 10 + 10), 150, tr(s, "menu.disconnect"),
                          [p] { disconnect(*p, "connect.aborted"); });
    b.active = s.clock - g.reconfigStart >= 30.0;
}

void reconfigDraw(State& s, Canvas& c) {
    menuBackground(s, c);
    for (Button& b : s.buttons) b.active = s.clock - g.reconfigStart >= 30.0;  // DISCONNECT_TIME: 600 ticks
    s.drawCentered(c, tr(s, "connect.reconfiguring"), float(c.guiWidth() / 2), float(c.guiHeight() / 2 - 10 - 19), 0xFFFFFFFF);
    s.drawButtons(c);
}

void reconfigInput(State& s, const InputEvent& e) {
    if (e.type != InputEvent::Key) s.buttonsInput(e);
}

const ScreenRegistrar kReconfigScreen({Screen::Reconfiguring, reconfigBuild, reconfigDraw, reconfigInput, nullptr});

// --- events --------------------------------------------------------------------------------------------------------

bool validUrl(const std::string& url) {  // parseResourcePackUrl: http or https
    const std::wstring w = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = uc.dwUrlPathLength = DWORD(-1);
    return WinHttpCrackUrl(w.c_str(), 0, 0, &uc) && (uc.nScheme == INTERNET_SCHEME_HTTP || uc.nScheme == INTERNET_SCHEME_HTTPS);
}


void startConnection(State& s) {
    // configureForServerControl with the server entry's setting.
    g.started = true;
    const int status = entryPackStatus(s);
    g.prompt = status == kEnabled ? PromptStatus::Allowed : status == kDisabled ? PromptStatus::Declined : PromptStatus::Pending;
    LOG("packs: server resource packs: %s", kStatusNames[status]);
}

void transfer(State& s, const net::TransferEvent& e) {
    FrontEndState& f = *s.fe;
    if (f.cfg.singleplayer || !g.client) {  // "Cannot transfer to server from singleplayer"
        LOG("net: transfer from singleplayer ignored");
        return;
    }
    // TransferState: the cookies (and this server entry) go with the next connection.
    nlohmann::json j;
    j["entry"] = f.cfg.serverAddress;
    j["cookies"] = nlohmann::json::object();
    for (const auto& [key, value] : g.client->cookies())
        j["cookies"][key] = hex(reinterpret_cast<const u8*>(value.data()), value.size());
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    const fs::path file = fs::path(local ? local : L".") / "minecraftCplusplus" /
                          ("transfer-" + std::to_string(GetCurrentProcessId()) + ".json");
    std::ofstream(file, std::ios::trunc) << j.dump() << "\n";
    const std::string address = e.host + ":" + std::to_string(e.port);
    std::vector<std::wstring> args = launch::stickyArguments();
    args.insert(args.end(), {L"--connect", widen(address), L"--name", widen(f.cfg.playerName), L"--transfer", file.wstring()});
    LOG("ui: transferring to %s", address.c_str());
    f.failTitle = tr(s, "connect.failed.transfer");
    f.failAction = [p = &s] { requestTitle(*p, "servers"); };
    startChild(s, args, tr(s, "connect.transferring"), Screen::Servers, true);
}

void handle(State& s, net::CommonEvent& ev) {
    if (!g.started) startConnection(s);
    if (auto* e = std::get_if<net::PackPushEvent>(&ev)) {
        // ClientCommonPacketListenerImpl.handleResourcePackPush
        if (!validUrl(e->url)) {
            Pack p;
            p.id = e->id;
            report(p, kInvalidUrl);
            return;
        }
        LOG("packs: server pushes %s %s (%s%s)", uuidText(e->id).c_str(), e->url.c_str(), e->required ? "required" : "optional",
            e->hash.empty() ? ", no hash" : "");
        const int status = entryPackStatus(s);
        if (status != kPrompt && (!e->required || status != kDisabled)) pushPack(e->id, e->url, lowerHash(e->hash));
        else showPrompt(s, *e);
    } else if (auto* pop = std::get_if<net::PackPopEvent>(&ev)) {
        LOG("packs: server pops %s", pop->id.empty() ? "every pack" : uuidText(pop->id).c_str());
        popPack(pop->id);
    } else if (auto* coc = std::get_if<net::CodeOfConductEvent>(&ev)) {
        // ClientConfigurationPacketListenerImpl.handleCodeOfConduct
        bool known = false;
        withEntry(s, [&](ServerEntry& e) { known = e.acceptedCodeOfConduct != 0 && e.acceptedCodeOfConduct == javaHash(coc->text); }, false);
        LOG("net: server code of conduct (%zu characters)%s", coc->text.size(), known ? ": accepted before" : "");
        if (known) {
            g.client->acceptCodeOfConduct();
            return;
        }
        g.coc = coc->text, g.cocDontShow = false, g.cocScroll = 0;
        g.cocBack = currentBack(s);
        s.openScreen(Screen::CodeOfConduct);
    } else if (std::holds_alternative<net::ResetChatEvent>(ev)) {
        // The chat (messages and sent history) does not come back after this configuration.
        s.chatLines.clear(), s.chatMessages.clear(), s.sentHistory.clear();
    } else if (auto* t = std::get_if<net::TransferEvent>(&ev)) {
        transfer(s, *t);
    } else if (auto* c = std::get_if<net::ConfigurationEvent>(&ev)) {
        if (c->started) {  // clearClientLevel(new ServerReconfigScreen)
            g.reconfiguring = true, g.reconfigStart = s.clock;
            s.openScreen(Screen::Reconfiguring);
        } else {
            if (g.reconfiguring && s.screen == Screen::Reconfiguring) s.closeScreen(false);
            g.reconfiguring = false;
            if (g.client) s.items.setRegistries(*g.client);  // the registries of this configuration
        }
    }
}

void frame(State& s) {
    if (!g.client) return;
    std::vector<net::CommonEvent> events;
    g.client->pollCommon(events);
    for (net::CommonEvent& e : events) handle(s, e);
    if (g.started) tickPacks(s);
    FrontEndState& f = *s.fe;
    if (!s.session && !f.cfg.menu && !f.cfg.singleplayer && s.screen == Screen::None && !f.child) {
        // --connect still configuring in the window (main.cpp): ConnectScreen until the join
        f.loadingTitle = tr(s, "connect.joining");
        f.loadingStatus.clear();
        f.progress = -1, f.loadingCancel = false, f.loadingStart = s.clock;
        s.openScreen(Screen::Loading);
    }
}

void handlers(State&, net::Client& client) { g = ServerConfig{}, g.client = &client; }

const UiModuleRegistrar kModule({.name = "server_config", .registerHandlers = handlers, .frame = frame});

}  // namespace

}  // namespace ui
