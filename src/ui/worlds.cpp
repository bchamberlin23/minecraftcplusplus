// Front end: the world list (SelectWorldScreen / WorldSelectionList), Create New World, Edit World and deleting a
// world (to the Recycle Bin). Worlds are played through the local official server (--play), created by it, or opened
// in the fast local viewer (--world, read only).
#include "ui/frontend.h"
#include "assets/assets.h"
#include "net/local_server.h"
#include "ui/toasts.h"
#include "world/nbt_dom.h"
#include "world/world_backup.h"

#include <libdeflate.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <future>

namespace ui {

constexpr int kHeader = 49, kFooter = 60, kRowH = 36, kRowW = 270;

bool readNbtGz(const fs::path& p, NbtValue& root, std::string* rootName) {
    std::vector<u8> gz;
    if (!readFile(p, gz) || gz.size() < 18) return false;
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    std::vector<u8> raw(gz.size() * 8 + 4096);
    size_t actual = 0;
    libdeflate_result r;
    while ((r = libdeflate_gzip_decompress(d, gz.data(), gz.size(), raw.data(), raw.size(), &actual)) ==
               LIBDEFLATE_INSUFFICIENT_SPACE &&
           raw.size() < (size_t(64) << 20))
        raw.resize(raw.size() * 2);
    libdeflate_free_decompressor(d);
    return r == LIBDEFLATE_SUCCESS && nbtRead(raw.data(), actual, root, rootName);
}

bool writeNbtGz(const fs::path& p, const NbtValue& root, std::string_view rootName) {
    std::vector<u8> encoded;
    nbtWrite(root, encoded, rootName);
    libdeflate_compressor* c = libdeflate_alloc_compressor(6);
    std::vector<u8> packed(libdeflate_gzip_compress_bound(c, encoded.size()));
    const size_t n = libdeflate_gzip_compress(c, encoded.data(), encoded.size(), packed.data(), packed.size());
    libdeflate_free_compressor(c);
    if (!n) return false;
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(packed.data()), std::streamsize(n));
        if (!f) return false;
    }
    std::error_code ec;
    fs::copy_file(p, p.string() + "_old", fs::copy_options::overwrite_existing, ec);  // like the game's level.dat_old
    fs::rename(tmp, p, ec);
    return !ec;
}

namespace {

// WorldSelectionList.DATE_FORMAT: the short localized date and time.
std::string formatDate(i64 ms) {
    ULARGE_INTEGER u;
    u.QuadPart = u64(ms) * 10000ull + 116444736000000000ull;
    FILETIME ft{u.LowPart, u.HighPart};
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return {};
    wchar_t date[64] = {}, time[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, time, 64);
    return utf8(fs::path(std::wstring(date) + L", " + time));
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(u8(c)));
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}


WorldEntry* selected(State& s) {
    FrontEndState& f = *s.fe;
    return f.worldSel >= 0 && f.worldSel < int(f.worlds.size()) ? &f.worlds[size_t(f.worldSel)] : nullptr;
}

void filterWorlds(State& s) {
    FrontEndState& f = *s.fe;
    const std::string q = lower(f.worldSearch.text.value);  // toLowerCase(Locale.ROOT), untrimmed
    f.worldView.clear();
    for (size_t i = 0; i < f.worlds.size(); ++i)
        if (q.empty() || lower(f.worlds[i].name).find(q) != std::string::npos ||
            lower(f.worlds[i].folder).find(q) != std::string::npos)
            f.worldView.push_back(int(i));
    if (std::find(f.worldView.begin(), f.worldView.end(), f.worldSel) == f.worldView.end()) f.worldSel = -1;
}

int listRows(State& s) { return std::max(1, (s.guiH() - kHeader - kFooter - 4) / kRowH); }
float maxScroll(State& s, size_t rows) {
    return std::max(0.0f, float(int(rows) * kRowH + 4) - float(s.guiH() - kHeader - kFooter));
}

std::vector<std::wstring> worldArgs(const WorldEntry& w, bool play) {
    std::vector<std::wstring> a = launch::stickyArguments();
    if (play) a.push_back(L"--play");
    a.insert(a.end(), {L"--world", w.dir.wstring()});
    if (!play) a.push_back(L"--readonly");  // the viewer never writes to a world the server did not simulate
    return a;
}

void openRecover(State& s);

// LevelSummary for this game (26.3, a stable release of the "main" series).
int currentDataVersion(State& s) {
    static int v = gameDataVersion(s.fe->cfg.jar);
    return v;
}
bool compatible(const WorldEntry& w) { return w.series == "main"; }
bool manualConversion(const WorldEntry& w) { return !w.broken && w.storageVersion != 19133; }
bool fileFixing(const WorldEntry& w) {  // FileFixerUpper.requiresFileFixing: below the last file fix (4899)
    return !w.broken && (w.dataVersion < 4772 ? 0 : w.dataVersion) < 4899;
}
enum class Backup { None, Downgrade, FileFixing };  // BackupStatus (no UPGRADE_TO_SNAPSHOT in a release)
Backup backupStatus(State& s, const WorldEntry& w) {
    if (w.broken) return Backup::None;
    if (fileFixing(w)) return Backup::FileFixing;
    const int current = currentDataVersion(s);
    return current && w.versionId > current ? Backup::Downgrade : Backup::None;
}
bool disabled(const WorldEntry& w) { return !w.broken && (w.locked || manualConversion(w) || !compatible(w)); }
bool canEdit(const WorldEntry& w) { return !w.broken && !disabled(w) && !fileFixing(w); }
Text versionName(State& s, const WorldEntry& w) {
    return w.versionName.empty() ? tr(s, "selectWorld.versionUnknown") : Text(w.versionName);
}

// WorldOpenFlows.openWorld: incompatible -> AlertScreen; a backup question for file fixing / downgrades and for
// experimental worlds (BackupConfirmScreen); low disk space -> ConfirmScreen; then the world.
void continuePlay(State& s, int step);

void playWorld(State& s, bool play) {
    WorldEntry* w = selected(s);
    if (!w) return;
    if (w->broken) return openRecover(s);  // CorruptedLevelSummary: the primary action recovers
    if (disabled(*w)) return;
    if (!play) {  // this port's read-only viewer (Shift)
        s.fe->failTitle = Text("Could not open the world");
        return startChild(s, worldArgs(*w, false), Text("Viewing \"" + w->name + "\""), Screen::Worlds, false);
    }
    continuePlay(s, 0);
}

// The pages of the Edit World screen: Edit World itself, a BackupConfirmScreen (Optimize World's, or opening a world
// that wants a backup), and Recover World (RecoverWorldDataScreen, for a world whose level.dat cannot be read).
enum class EditPage { Edit, Backup, Recover };
EditPage editPage = EditPage::Edit;
bool eraseCache = false;  // BackupConfirmScreen's "Erase cached data"
struct BackupPrompt {
    Text title, description, confirm;
    bool eraseCachePrompt = false;
    std::function<void(bool backup)> proceed;
    std::function<void()> cancel;
};
BackupPrompt prompt;

void openEditWorld(State& s) {
    WorldEntry* w = selected(s);
    if (!w) return;
    editPage = EditPage::Edit;
    s.fe->editName.text = TextField{};
    s.fe->editName.text.value = w->name;
    s.fe->editName.text.cursor = s.fe->editName.text.selection = int(w->name.size());
    s.openScreen(Screen::EditWorld);
}

void saveEditWorld(State& s, bool blankToo = false) {
    WorldEntry* w = selected(s);
    if (!w) return openWorlds(s);
    const std::string name = trim(s.fe->editName.text.value);  // LevelStorageAccess.renameLevel: trimmed
    if ((!name.empty() || blankToo) && name != w->name && !worldLocked(w->dir)) {
        NbtValue root;
        std::string rootName;
        if (readNbtGz(w->dir / "level.dat", root, &rootName))
            if (NbtValue* data = root.find("Data")) {
                NbtValue v = NbtValue::make(TagString);
                v.s = name;
                data->set("LevelName", std::move(v));
                if (writeNbtGz(w->dir / "level.dat", root, rootName)) LOG("ui: renamed world %s to \"%s\"", w->folder.c_str(), name.c_str());
            }
    }
    openWorlds(s);
}

bool recycle(const fs::path& dir) {
    std::wstring from = dir.wstring();
    from.push_back(L'\0');  // double-null terminated list
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

void deleteWorld(State& s) {
    WorldEntry* w = selected(s);
    if (!w) return;
    const fs::path dir = w->dir;
    const std::string name = w->name;
    State* p = &s;
    showConfirm(s, tr(s, "selectWorld.deleteQuestion"),  // canDelete: always (a locked world's delete fails)
                Text(formatJava(s.res.translate("selectWorld.deleteWarning"), {name}) + "\n(It goes to the Recycle Bin.)"),
                s.res.translate("selectWorld.deleteButton"),
                [p, dir] {
                    if (recycle(dir)) {
                        LOG("ui: moved world %s to the Recycle Bin", utf8(dir).c_str());
                        p->fe->worldSel = -1;
                        openWorlds(*p);
                    } else {
                        LOG("ui: could not delete %s", utf8(dir).c_str());
                        // SystemToast.onWorldDeleteFailure (WORLD_ACCESS_FAILURE), then the reloaded list.
                        showToast(tr(*p, "selectWorld.delete_failure"), Text(utf8(dir.filename())), "world_access_failure");
                        openWorlds(*p);
                    }
                },
                Screen::Worlds);
}

}  // namespace

// --- saves --------------------------------------------------------------------------------------------------------

bool worldLocked(const fs::path& dir) {
    const HANDLE h = CreateFileW((dir / "session.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    OVERLAPPED ov{};
    const bool locked = !LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov);
    if (!locked) UnlockFileEx(h, 0, 1, 0, &ov);
    CloseHandle(h);
    return locked;
}

bool readWorld(const fs::path& dir, WorldEntry& out) {
    NbtValue root;
    if (!readNbtGz(dir / "level.dat", root)) return false;
    NbtValue* data = root.find("Data");
    if (!data) return false;
    out.dir = dir;
    out.folder = utf8(dir.filename());
    auto num = [&](const char* key, i64 fallback) {
        const NbtValue* v = data->find(key);
        return v ? v->i : fallback;
    };
    const NbtValue* name = data->find("LevelName");
    out.name = name && !name->s.empty() ? name->s : out.folder;
    out.lastPlayed = num("LastPlayed", -1);
    out.gameMode = int(num("GameType", 0));
    out.hardcore = num("hardcore", 0) != 0;
    if (NbtValue* ds = data->find("difficulty_settings"))  // 26.x
        if (NbtValue* h = ds->find("hardcore")) out.hardcore = h->i != 0;
    out.cheats = num("allowCommands", 0) != 0;
    out.dataVersion = int(num("DataVersion", 0));
    out.storageVersion = int(num("version", 0));
    if (NbtValue* v = data->find("Version")) {  // LevelVersion.parse (missing: the current version's)
        if (NbtValue* n = v->find("Name")) out.versionName = n->s;
        if (NbtValue* n = v->find("Id")) out.versionId = int(n->i);
        if (NbtValue* n = v->find("Series")) out.series = n->s;
    } else {
        out.series = "main";
    }
    if (NbtValue* features = data->find("enabled_features"))
        for (const NbtValue& e : features->items) out.experimental |= e.s != "minecraft:vanilla";
    out.locked = worldLocked(dir);
    std::vector<u8> png;
    if (readFile(dir / "icon.png", png)) decodeImage(png, out.icon);
    return true;
}

std::vector<WorldEntry> scanWorlds(const fs::path& saves) {
    std::vector<WorldEntry> out;
    std::error_code ec;
    for (const auto& d : fs::directory_iterator(saves, ec)) {
        if (!d.is_directory(ec)) continue;
        WorldEntry w;
        if (readWorld(d.path(), w)) {
            out.push_back(std::move(w));
        } else if (fs::exists(d.path() / "level.dat", ec) || fs::exists(d.path() / "level.dat_old", ec)) {
            // A save whose level.dat cannot be read (LevelStorageSource: CorruptedLevelSummary).
            w.dir = d.path(), w.folder = w.name = utf8(d.path().filename()), w.broken = true;
            const auto t = fs::last_write_time(d.path() / "level.dat", ec);
            if (!ec)
                w.lastPlayed = i64(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::clock_cast<std::chrono::system_clock>(t).time_since_epoch()).count());
            w.locked = worldLocked(d.path());
            out.push_back(std::move(w));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const WorldEntry& a, const WorldEntry& b) {
        if (a.lastPlayed != b.lastPlayed) return a.lastPlayed > b.lastPlayed;
        return a.folder < b.folder;  // LevelSummary.compareTo: then the level id
    });
    return out;
}

std::string worldFolderFor(const fs::path& saves, const std::string& name) {
    // FileUtil.findAvailableName: illegal characters and dots become '_', Windows device names get underscores.
    std::string base;
    for (char c : name) base += std::strchr("/\n\r\t\f`?*\\<>|\":.", c) && c ? '_' : c;
    if (base.empty()) base = "World";
    static const char* reserved[] = {"CON", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "PRN",
                                     "AUX", "NUL", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    std::string upper = base;
    for (char& c : upper) c = char(std::toupper(u8(c)));
    for (const char* r : reserved)
        if (upper == r) base = "_" + base + "_";
    std::string folder = base;
    std::error_code ec;
    for (int i = 1; fs::exists(saves / widen(folder), ec); ++i) folder = base + " (" + std::to_string(i) + ")";
    return folder;
}

// --- world list ---------------------------------------------------------------------------------------------------

void openWorlds(State& s) {
    FrontEndState& f = *s.fe;
    fs::path sel;
    if (const WorldEntry* w = selected(s)) sel = w->dir;
    f.worlds = scanWorlds(f.cfg.savesDir);
    f.worldSel = -1;
    for (size_t i = 0; i < f.worlds.size(); ++i)
        if (f.worlds[i].dir == sel) f.worldSel = int(i);
    f.worldSearch.hint = s.res.translate("gui.selectWorld.search");
    filterWorlds(s);
    f.worldScroll = std::clamp(f.worldScroll, 0.0f, maxScroll(s, f.worldView.size()));
    LOG("ui: %zu worlds in %s", f.worlds.size(), utf8(f.cfg.savesDir).c_str());
    // WorldSelectionList.handleNewLevels: no worlds at all -> CreateWorldScreen.openFresh (closing to the title)
    if (f.worlds.empty()) return openCreateWorld(s, true);
    s.openScreen(Screen::Worlds);
}

void buildWorlds(State& s) {
    FrontEndState& f = *s.fe;
    const int W = s.guiW(), H = s.guiH();
    State* p = &s;
    f.worldSearch.x = float(W / 2 - 100), f.worldSearch.y = 22, f.worldSearch.w = 200;
    setFields(s, {&f.worldSearch}, 0);  // setInitialFocus(searchBox) (a rebuild keeps the focus)
    const WorldEntry* w = selected(s);
    const bool active = w && (w->broken || !disabled(*w));  // primaryActionActive
    const float x0 = float(W / 2 - 154), y0 = float(H - 52), y1 = float(H - 28);
    // Shift: the world in the fast local viewer instead (read only, no server): this port's own.
    const bool view = s.window->keyDown(VK_SHIFT) && active && !w->broken;
    Text play = w && w->broken ? tr(s, "recover_world.button") : view ? Text("View Only")
              : w && fileFixing(*w) ? tr(s, "selectWorld.upgrade_and_play") : tr(s, "selectWorld.select");
    Button& playButton = addButton(s, x0, y0, 150, play, [p, view] { playWorld(*p, !view); }, active);
    addButton(s, x0 + 158, y0, 150, tr(s, "selectWorld.create"), [p] { openCreateWorld(*p); });
    Button& edit = addButton(s, x0, y1, 71, tr(s, "selectWorld.edit"), [p] { openEditWorld(*p); }, w && canEdit(*w));
    addButton(s, x0 + 79, y1, 71, tr(s, "selectWorld.delete"), [p] { deleteWorld(*p); }, w != nullptr);
    Button& recreate = addButton(s, x0 + 158, y1, 71, tr(s, "selectWorld.recreate"), [p] {
        if (const WorldEntry* we = selected(*p); we && !recreateWorld(*p, we->dir))
            showMessage(*p, tr(*p, "selectWorld.recreate.error.title"), tr(*p, "selectWorld.recreate.error.text"),
                        p->res.translate("gui.back"), [p] { openWorlds(*p); }, true);
    }, w && canEdit(*w));
    if (w && fileFixing(*w)) {  // SelectWorldScreen.updateButtonStatus: why they are off
        playButton.tooltip = [p] { return tr(*p, "selectWorld.requiresFileFixingTooltip.play"); };
        edit.tooltip = [p] { return tr(*p, "selectWorld.requiresFileFixingTooltip.edit"); };
        recreate.tooltip = [p] { return tr(*p, "selectWorld.requiresFileFixingTooltip.recreate"); };
    }
    addButton(s, x0 + 237, y1, 71, tr(s, "gui.back"), [p] { p->openScreen(Screen::Title); });
    f.onBack = [p] { p->openScreen(Screen::Title); };
    f.onEnter = [p] {  // WorldListEntry.keyPressed: the list entry has the focus (not the search box)
        if (p->fe->focus < 0) {
            const WorldEntry* we = selected(*p);
            if (we && (we->broken || !disabled(*we))) playWorld(*p, true);
        }
    };
}

void drawWorlds(State& s, Canvas& c) {
    FrontEndState& f = *s.fe;
    const int W = c.guiWidth(), H = c.guiHeight();
    menuBackground(s, c);
    s.drawCentered(c, tr(s, "selectWorld.title"), float(W / 2), 8, 0xFFFFFFFF);
    drawFields(s, c);
    const float top = float(kHeader), bottom = float(H - kFooter);
    listBackground(s, c, top, bottom);
    const float rowLeft = float(W / 2 - kRowW / 2 + 2);
    f.worldScroll = std::clamp(f.worldScroll, 0.0f, maxScroll(s, f.worldView.size()));
    c.pushScissor(0, top, float(W), bottom);
    std::vector<Text> tooltip;
    for (size_t i = 0; i < f.worldView.size(); ++i) {
        const WorldEntry& w = f.worlds[size_t(f.worldView[i])];
        const float y = top + 4 + float(i) * kRowH - f.worldScroll;
        if (y + kRowH < top || y > bottom) continue;
        const bool hovered = s.mouseX >= rowLeft - 2 && s.mouseX < rowLeft + kRowW - 2 && s.mouseY >= y - 2 &&
                             s.mouseY < y + kRowH - 2 && s.mouseY >= top && s.mouseY < bottom;
        if (f.worldView[i] == f.worldSel) {  // AbstractSelectionList.renderSelection
            c.fill(rowLeft - 2, y - 2, rowLeft + kRowW - 2, y + kRowH - 2, 0xFFFFFFFF);
            c.fill(rowLeft - 1, y - 1, rowLeft + kRowW - 3, y + kRowH - 3, 0xFF000000);
        }
        const Sprite* icon = w.icon.w ? iconSprite(s, "world:" + utf8(w.dir), w.icon, std::to_string(w.lastPlayed)) : nullptr;
        if (icon) blitSprite(c, icon, rowLeft, y, 32, 32);
        else blitTexture(c, s.sprites.texture("misc/unknown_server"), rowLeft, y, 0, 0, 32, 32, 32, 32);
        // StringWidgets of at most 270 - 37 - 2 wide (clipped with "..."), the full text as their tooltip
        const int textW = kRowW - 37 - 2;
        auto line = [&](const Text& t, float ty, u32 color) {
            const bool clipped = s.font.width(t) > textW;
            s.drawText(c, clipped ? Text(fitText(s.font, t.plain(), textW), t.spans.empty() ? Style{} : t.spans[0].style) : t,
                       rowLeft + 35, ty, color, true);
            if (clipped && s.mouseX >= rowLeft + 35 && s.mouseX < rowLeft + 35 + float(textW) && s.mouseY >= ty && s.mouseY < ty + 9)
                tooltip = s.font.split(t, 170);
        };
        line(Text(w.name), y + 1, 0xFFFFFFFF);
        std::string line2 = w.folder;
        if (w.lastPlayed != -1) line2 += " (" + formatDate(w.lastPlayed) + ")";
        line(Text(line2), y + 12, 0xFF808080);
        // LevelSummary.getInfo (merged over gray)
        const Backup backup = backupStatus(s, w);
        Text info;
        if (w.broken) {
            info = Text(s.res.translate("recover_world.warning"), colorStyle(0xFF0000));
        } else if (w.locked) {
            info = Text(s.res.translate("selectWorld.locked"), colorStyle(0xFF5555));
        } else if (manualConversion(w)) {
            info = Text(s.res.translate("selectWorld.conversion"), colorStyle(0xFF5555));
        } else if (!compatible(w)) {
            info = trf(s, "selectWorld.incompatible.info", {versionName(s, w)});
            for (Span& sp : info.spans) sp.style = sp.style.over(colorStyle(0xFF5555));
        } else {
            if (w.hardcore) info.append(s.res.translate("gameMode.hardcore"), colorStyle(0xFF0000));
            else info.append(s.res.translate(w.gameMode == 1 ? "gameMode.creative" : w.gameMode == 2 ? "gameMode.adventure"
                                             : w.gameMode == 3 ? "gameMode.spectator" : "gameMode.survival"));
            if (w.cheats) info.append(", " + s.res.translate("selectWorld.commands"));
            if (w.experimental) info.append(", "), info.append(s.res.translate("selectWorld.experimental"), colorStyle(0xFFFF55));
            info.append(", " + s.res.translate("selectWorld.version") + " ");
            Text version = versionName(s, w);
            if (backup != Backup::None) {  // shouldBackup: italic, a downgrade red
                Style st;
                if (backup == Backup::Downgrade) st = colorStyle(0xFF5555);
                else st.italic = 1;
                for (Span& sp : version.spans) sp.style = sp.style.over(st);
            }
            info.append(version);
        }
        line(info, y + 21, 0xFF808080);
        if (hovered) {
            c.fill(rowLeft, y, rowLeft + 32, y + 32, 0xA0909090);
            const bool overIcon = s.mouseX >= rowLeft && s.mouseX < rowLeft + 32 && s.mouseY >= y && s.mouseY < y + 32;
            auto sprite = [&](const char* name) {
                blitSprite(c, s.sprites.sprite(std::string("world_list/") + name + (overIcon ? "_highlighted" : "")), rowLeft, y, 32, 32);
            };
            auto tip = [&](const char* key) {
                if (overIcon) tooltip = s.font.split(Text(s.res.translate(key), colorStyle(0xFF5555)), 175);
            };
            if (w.broken) {
                sprite("error"), sprite("marked_join");
            } else if (w.locked) {
                sprite("error"), tip("selectWorld.locked");
            } else if (manualConversion(w)) {
                sprite("error"), tip("selectWorld.conversion.tooltip");
            } else if (!compatible(w)) {
                sprite("error"), tip("selectWorld.incompatible.tooltip");
            } else if (backup != Backup::None) {
                sprite("marked_join");
                if (backup == Backup::Downgrade) {
                    sprite("error");
                    if (overIcon)
                        tooltip = {Text(s.res.translate("selectWorld.tooltip.fromNewerVersion1"), colorStyle(0xFF5555)),
                                   Text(s.res.translate("selectWorld.tooltip.fromNewerVersion2"), colorStyle(0xFF5555))};
                }
            } else {
                sprite("join");
            }
        }
    }
    c.popScissor();
    if (const float ms = maxScroll(s, f.worldView.size()); ms > 0) {  // scroll bar right of the rows
        const float x = rowLeft + kRowW + 4, h = bottom - top;
        const float bar = std::max(32.0f, h * h / (h + ms)), by = top + (h - bar) * f.worldScroll / ms;
        blitSprite(c, s.sprites.sprite("widget/scroller_background"), x, top, 6, h);
        blitSprite(c, s.sprites.sprite("widget/scroller"), x, by, 6, bar);
        s.scrollbarCursor(x, top, h);
    }
    s.drawButtons(c);
    if (!tooltip.empty()) s.drawTooltip(c, tooltip, s.mouseX, s.mouseY);
    for (const Button& b : s.buttons)  // the file fixing tooltips (MenuTooltipPositioner)
        if (b.tooltip && s.mouseX >= b.x && s.mouseY >= b.y && s.mouseX < b.x + b.w && s.mouseY < b.y + b.h)
            s.drawTooltip(c, s.font.split(b.tooltip(), 170), s.mouseX, s.mouseY, nullptr, false, {}, std::array<float, 4>{b.x, b.y, b.w, b.h}.data());
}

void worldsInput(State& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    FrontEndState& f = *s.fe;
    const int W = s.guiW(), H = s.guiH();
    const float top = float(kHeader), bottom = float(H - kFooter), rowLeft = float(W / 2 - kRowW / 2 + 2);
    if (e.type == E::Wheel) {
        f.worldScroll = std::clamp(f.worldScroll - float(e.code) * kRowH / 2, 0.0f, maxScroll(s, f.worldView.size()));
        return;
    }
    if (e.type == E::Key && e.code == VK_SHIFT && !e.repeat) s.buildButtons();  // Play / View Only
    if (e.type == E::Key && e.down && (e.code == VK_UP || e.code == VK_DOWN) && !f.worldView.empty()) {
        auto it = std::find(f.worldView.begin(), f.worldView.end(), f.worldSel);
        int i = it == f.worldView.end() ? -1 : int(it - f.worldView.begin());
        if (i < 0) i = e.code == VK_DOWN ? 0 : int(f.worldView.size()) - 1;  // nothing selected: the first / last
        else i = std::clamp(i + (e.code == VK_DOWN ? 1 : -1), 0, int(f.worldView.size()) - 1);
        f.worldSel = f.worldView[size_t(i)];
        const float y = 4 + float(i) * kRowH;  // keep it in view
        f.worldScroll = std::clamp(f.worldScroll, y + kRowH - (bottom - top), y);
        s.buildButtons();
        return;
    }
    if (e.type == E::Button && e.down && e.code == 0 && s.mouseY >= top && s.mouseY < bottom && s.mouseX >= rowLeft - 2 &&
        s.mouseX < rowLeft + kRowW - 2) {
        const int i = int(std::floor((s.mouseY - top - 4 + 2 + f.worldScroll) / kRowH));
        if (i >= 0 && i < int(f.worldView.size())) {
            f.focus = -1;
            f.worldSel = f.worldView[size_t(i)];
            s.buildButtons();
            const float y = top + 4 + float(i) * kRowH - f.worldScroll;
            const bool overIcon = s.mouseX < rowLeft + 32 && s.mouseY >= y && s.mouseY < y + 32;
            const WorldEntry& we = f.worlds[size_t(f.worldSel)];
            if ((doubleClicked(s, i) || overIcon) && (we.broken || !disabled(we))) playWorld(s, true);  // canInteract
        }
        return;
    }
    const std::string before = f.worldSearch.text.value;
    if (fieldsInput(s, e)) {
        if (f.worldSearch.text.value != before) {
            filterWorlds(s);
            f.worldScroll = 0;
            s.buildButtons();
        }
        return;
    }
    s.buttonsInput(e);
}

// --- edit world (EditWorldScreen), Optimize World (BackupConfirmScreen, OptimizeWorldScreen), Recover World ----------

namespace {

// A backup / optimization running in the background (main thread state).
struct WorldTask {
    enum class After { List, Optimize, Restore, Play } after = After::List;
    int nextStep = 0;  // After::Play: where continuePlay goes on
    fs::path dir;
    std::string folder, name;
    bool eraseCache = false;
    std::future<std::optional<u64>> backup;
    std::unique_ptr<LocalServer> server;
    std::future<void> stopping;
    bool cancelled = false;
};
std::unique_ptr<WorldTask> task;

fs::path backupsDir(State& s) {  // LevelStorageSource.getBackupPath: backups next to the saves folder
    return s.fe->cfg.savesDir.parent_path() / "backups";
}

void openFolder(const fs::path& dir) { ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void showWaiting(State& s, Text title, std::string status, float progress, std::function<void()> cancel) {
    FrontEndState& f = *s.fe;
    f.loadingTitle = std::move(title);
    f.loadingStatus = std::move(status);
    f.progress = progress;
    f.loadingStart = s.clock;
    f.loadingCancel = bool(cancel);
    f.loadingCancelFn = std::move(cancel);
    s.openScreen(Screen::Loading);
}

// EditWorldScreen.makeBackupAndShowToast, then what the backup was for.
void startTask(State& s, const WorldEntry& w, WorldTask::After after, bool backupFirst) {
    task = std::make_unique<WorldTask>();
    task->after = after, task->dir = w.dir, task->folder = w.folder, task->name = w.name, task->eraseCache = eraseCache;
    if (backupFirst) {
        const fs::path world = w.dir, backups = backupsDir(s);
        task->backup = std::async(std::launch::async, [world, backups] { return makeWorldBackup(world, backups); });
        showWaiting(s, tr(s, after == WorldTask::After::Restore ? "recover_world.restoring" : "selectWorld.waitingForBackup.title"),
                    s.res.translate("selectWorld.waitingForBackup.message"), -1, nullptr);
    }
}

void startOptimize(State& s) {
    WorldTask& t = *task;
    const FrontEndState& f = *s.fe;
    LocalServer::Config cfg;
    cfg.minecraftDir = f.cfg.minecraftDir, cfg.version = f.cfg.version, cfg.world = t.dir;
    if (!f.cfg.playerName.empty()) cfg.player = f.cfg.playerName;
    cfg.forceUpgrade = true, cfg.eraseCache = t.eraseCache;
    t.server = std::make_unique<LocalServer>();
    LOG("ui: optimizing world %s%s", t.folder.c_str(), t.eraseCache ? " (erasing cached data)" : "");
    if (!t.server->start(cfg)) {
        task.reset();
        return showMessage(s, tr(s, "optimizeWorld.stage.failed"), Text("The local server could not start; see the log."),
                           s.res.translate("gui.back"), [&s] { openWorlds(s); });
    }
    State* p = &s;
    showWaiting(s, trf(s, "optimizeWorld.title", {Text(t.name)}), s.res.translate("optimizeWorld.stage.counting"), 0,
                [p] {
                    if (task) task->cancelled = true;  // OptimizeWorldScreen's Cancel: stop where it is
                });
}

void restoreLevelData(State& s, const fs::path& dir) {  // LevelStorageAccess.restoreLevelDataFromOld
    std::error_code ec;
    const fs::path dat = dir / "level.dat", old = dir / "level.dat_old";
    fs::copy_file(dat, dir / "level.dat_corrupted", fs::copy_options::overwrite_existing, ec);
    ec.clear();
    fs::copy_file(old, dat, fs::copy_options::overwrite_existing, ec);
    WorldEntry check;
    const bool ok = !ec && readWorld(dir, check);
    LOG("ui: restoring %s from level.dat_old %s", utf8(dir).c_str(), ok ? "worked" : "failed");
    // RecoverWorldDataScreen: success -> ConfirmScreen (Continue opens the world, Back), failure -> AlertScreen
    if (ok)
        showConfirm(s, tr(s, "recover_world.done.title"), tr(s, "recover_world.done.success"), s.res.translate("gui.continue"),
                    [&s] {
                        openWorlds(s);
                        playWorld(s, true);
                    },
                    Screen::Worlds, s.res.translate("gui.back"), [&s] { openWorlds(s); });
    else
        showMessage(s, tr(s, "recover_world.done.title"), tr(s, "recover_world.done.failed"), s.res.translate("gui.back"),
                    [&s] { openWorlds(s); }, true);
}

void continuePlay(State& s, int step) {
    WorldEntry* w = selected(s);
    if (!w) return openWorlds(s);
    State* p = &s;
    auto ask = [&](Text question, Text warning, int next) {
        prompt = {std::move(question), std::move(warning), tr(s, "selectWorld.backupJoinConfirmButton"), false,
                  [p, next](bool backup) {
                      editPage = EditPage::Edit;
                      const WorldEntry* we = selected(*p);
                      if (!we) return openWorlds(*p);
                      if (!backup) return continuePlay(*p, next);
                      startTask(*p, *we, WorldTask::After::Play, true);  // EditWorldScreen.makeBackupAndShowToast
                      task->nextStep = next;
                  },
                  [p] {
                      editPage = EditPage::Edit;
                      openWorlds(*p);
                  }};
        editPage = EditPage::Backup;
        s.openScreen(Screen::EditWorld);
    };
    if (step <= 0) {
        if (!compatible(*w)) {
            return showMessage(s, Text(s.res.translate("selectWorld.incompatible.title"), colorStyle(0xFF0000)),
                               trf(s, "selectWorld.incompatible.description", {versionName(s, *w)}), s.res.translate("gui.back"),
                               [p] { openWorlds(*p); }, true);
        }
        if (const Backup b = backupStatus(s, *w); b != Backup::None) {
            const std::string key = b == Backup::Downgrade ? "downgrade" : "file_fixing_required";
            Text question = tr(s, ("selectWorld.backupQuestion." + key).c_str());
            if (b == Backup::Downgrade)  // severe
                for (Span& sp : question.spans) sp.style = sp.style.over(colorStyle(0xDF5050));
            return ask(std::move(question),
                       trf(s, ("selectWorld.backupWarning." + key).c_str(), {versionName(s, *w), Text(s.fe->cfg.version)}), 1);
        }
    }
    if (step <= 1 && w->experimental)  // openWorldCheckWorldStemCompatibility: an unstable (experimental) world
        return ask(tr(s, "selectWorld.backupQuestion.experimental"), tr(s, "selectWorld.backupWarning.experimental"), 2);
    if (step <= 2) {  // openWorldCheckDiskSpace: under 64 MiB free
        ULARGE_INTEGER free{};
        if (GetDiskFreeSpaceExW(w->dir.c_str(), &free, nullptr, nullptr) && free.QuadPart < 67108864ull) {
            return showConfirm(s, Text(s.res.translate("selectWorld.warning.lowDiskSpace.title"), colorStyle(0xFF5555)),
                               tr(s, "selectWorld.warning.lowDiskSpace.description"), s.res.translate("gui.continue"),
                               [p] { continuePlay(*p, 3); }, Screen::Worlds, s.res.translate("gui.back"), [p] { openWorlds(*p); });
        }
    }
    s.fe->failTitle = Text("Could not open the world");
    startChild(s, worldArgs(*w, true), Text("Loading \"" + w->name + "\""), Screen::Worlds, false);
}

}  // namespace

void worldsFrame(State& s) {
    if (!task) return;
    WorldTask& t = *task;
    if (t.backup.valid()) {
        if (t.backup.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        const std::optional<u64> size = t.backup.get();
        if (size) {
            showToast(trf(s, "selectWorld.edit.backupCreated", {Text(t.folder)}),
                      trf(s, "selectWorld.edit.backupSize", {Text(std::to_string((*size + 1048575) / 1048576))}));
        } else {
            showToast(tr(s, "selectWorld.edit.backupFailed"));
        }
        LOG("ui: backup of %s %s", t.folder.c_str(), size ? "written" : "failed");
        if (t.after == WorldTask::After::Optimize) return startOptimize(s);
        const fs::path dir = t.dir;
        const WorldTask::After after = t.after;
        const int next = t.nextStep;
        task.reset();
        if (after == WorldTask::After::Restore) return restoreLevelData(s, dir);
        if (after == WorldTask::After::Play) return continuePlay(s, next);
        return openWorlds(s);
    }
    if (t.stopping.valid()) {
        if (t.stopping.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        const bool cancelled = t.cancelled;
        task.reset();
        if (!cancelled) showToast(tr(s, "optimizeWorld.stage.finished"));
        return openWorlds(s);
    }
    if (t.server) {
        FrontEndState& f = *s.fe;
        if (const int pct = t.server->upgradePercent(); pct >= 0) {
            f.progress = float(pct) / 100.0f;
            f.loadingStatus = t.server->upgradeStatus();
        }
        // The upgrade is done once the server goes on to start (or it stopped): save and stop it off this thread.
        if (t.cancelled || t.server->ready() || !t.server->running()) {
            f.loadingStatus = s.res.translate(t.cancelled ? "optimizeWorld.stage.canceled" : "optimizeWorld.stage.finished");
            f.loadingCancel = false, f.loadingCancelFn = nullptr;
            s.buildButtons();
            LocalServer* server = t.server.get();
            t.stopping = std::async(std::launch::async, [server] { server->stop(); });
        }
    }
}

void buildEditWorld(State& s) {
    FrontEndState& f = *s.fe;
    const int W = s.guiW(), H = s.guiH();
    State* p = &s;
    const WorldEntry* w = selected(s);
    f.onBack = [p] {
        editPage = EditPage::Edit;
        openWorlds(*p);
    };
    if (editPage == EditPage::Backup) {  // BackupConfirmScreen
        const int lines = int(s.font.split(prompt.description, W - 50).size());
        const float textSize = float((lines + 1) * 9);
        addButton(s, float(W / 2 - 155), 100 + textSize, 150, prompt.confirm, [] { prompt.proceed(true); });
        addButton(s, float(W / 2 + 5), 100 + textSize, 150, tr(s, "selectWorld.backupJoinSkipButton"), [] { prompt.proceed(false); });
        addButton(s, float(W / 2 - 75), 124 + textSize, 150, tr(s, "gui.cancel"), [] { prompt.cancel(); });
        f.onBack = nullptr;  // shouldCloseOnEsc: false
        return;
    }
    if (editPage == EditPage::Recover) {  // RecoverWorldDataScreen
        std::error_code ec;
        WorldEntry fallback;
        const bool hasOld = w && fs::exists(w->dir / "level.dat_old", ec);
        const float top = float(H / 2 - 40);
        addButton(s, float(W / 2 - 61), top + 52, 120, tr(s, "recover_world.restore"), [p] {
            const WorldEntry* we = selected(*p);
            editPage = EditPage::Edit;
            if (we) startTask(*p, *we, WorldTask::After::Restore, true);
        }, hasOld && w && !w->locked);
        addButton(s, float(W / 2 - 61), top + 80, 120, tr(s, "gui.back"), [p] {
            editPage = EditPage::Edit;
            openWorlds(*p);
        });
        return;
    }
    // LinearLayout(spacing 5), centred: spacer, label, name, Reset Icon, Open World Folder, Make Backup, Open Backups
    // Folder, Optimize World, spacer, Save | Cancel.
    const float x = float(W / 2 - 100), top = float(std::max(0, (H - 234) / 2));
    f.editName.x = x, f.editName.y = top + 39, f.editName.w = 200;
    setFields(s, {&f.editName}, 0);
    std::error_code ec;
    const bool hasIcon = w && fs::is_regular_file(w->dir / "icon.png", ec);
    const bool can = w && !w->locked;
    addButton(s, x, top + 64, 200, tr(s, "selectWorld.edit.resetIcon"), [p] {
        if (WorldEntry* we = selected(*p)) {
            std::error_code e;
            fs::remove(we->dir / "icon.png", e);
            we->icon = Image{};
            p->buildButtons();
        }
    }, hasIcon);
    addButton(s, x, top + 89, 200, tr(s, "selectWorld.edit.openFolder"), [p] {
        if (const WorldEntry* we = selected(*p)) openFolder(we->dir);
    });
    addButton(s, x, top + 114, 200, tr(s, "selectWorld.edit.backup"), [p] {
        if (const WorldEntry* we = selected(*p)) startTask(*p, *we, WorldTask::After::List, true);
    }, w != nullptr);
    addButton(s, x, top + 139, 200, tr(s, "selectWorld.edit.backupFolder"), [p] {
        std::error_code e;
        fs::create_directories(backupsDir(*p), e);
        openFolder(backupsDir(*p));
    });
    addButton(s, x, top + 164, 200, tr(s, "selectWorld.edit.optimize"), [p] {
        prompt = {tr(*p, "optimizeWorld.confirm.title"), tr(*p, "optimizeWorld.confirm.description"),
                  tr(*p, "optimizeWorld.confirm.proceed"), true,
                  [p](bool backup) {
                      const WorldEntry* we = selected(*p);
                      editPage = EditPage::Edit;
                      if (!we) return openWorlds(*p);
                      startTask(*p, *we, WorldTask::After::Optimize, backup);
                      if (!backup) startOptimize(*p);
                  },
                  [p] {
                      editPage = EditPage::Edit;
                      p->buildButtons();
                  }};
        editPage = EditPage::Backup, eraseCache = false;
        p->buildButtons();
    });
    addButton(s, x, top + 214, 98, tr(s, "selectWorld.edit.save"), [p] { saveEditWorld(*p); },
              !trim(f.editName.text.value).empty());
    addButton(s, x + 102, top + 214, 98, tr(s, "gui.cancel"), [p] { openWorlds(*p); });
    f.onEnter = [p] {  // keyPressed: Enter in the name field renames (even blank) and closes
        if (p->fe->focus == 0) saveEditWorld(*p, true);
    };
}

void drawEditWorld(State& s, Canvas& c) {
    const int W = c.guiWidth(), H = c.guiHeight();
    menuBackground(s, c);
    if (editPage == EditPage::Backup) {
        s.drawCentered(c, prompt.title, float(W / 2), 50, 0xFFFFFFFF);
        const std::vector<Text> lines = s.font.split(prompt.description, W - 50);
        for (size_t i = 0; i < lines.size(); ++i) s.drawCentered(c, lines[i], float(W / 2), 70 + float(i) * 9, 0xFFFFFFFF);
        if (!prompt.eraseCachePrompt) {
            s.drawButtons(c);
            return;
        }
        // Checkbox "Erase cached data" (17 x 17 box, label 4 to the right, the label colour -2039584).
        const float bx = float(W / 2 - 155 + 80), by = 76 + float((lines.size() + 1) * 9);
        const bool over = s.mouseX >= bx && s.mouseX < bx + 17 + 4 + s.font.width(tr(s, "selectWorld.backupEraseCache")) &&
                          s.mouseY >= by && s.mouseY < by + 17;
        const char* sprite = eraseCache ? (over ? "widget/checkbox_selected_highlighted" : "widget/checkbox_selected")
                                        : (over ? "widget/checkbox_highlighted" : "widget/checkbox");
        blitSprite(c, s.sprites.sprite(sprite), bx, by, 17, 17);
        s.drawText(c, tr(s, "selectWorld.backupEraseCache"), bx + 21, by + 4, 0xFFE0E0E0, true);
        s.drawButtons(c);
        return;
    }
    if (editPage == EditPage::Recover) {
        const WorldEntry* w = selected(s);
        const float top = float(H / 2 - 40);
        s.drawCentered(c, tr(s, "recover_world.title"), float(W / 2), top - 40, 0xFFFFFFFF);
        const Text msg = trf(s, "recover_world.message", {Text(w ? w->folder : std::string(), colorStyle(0xAAAAAA))});
        const std::vector<Text> lines = s.font.split(msg, W - 50);
        for (size_t i = 0; i < lines.size(); ++i) s.drawCentered(c, lines[i], float(W / 2), top - 26 + float(i) * 9, 0xFFFFFFFF);
        // The state of level.dat and its fallback (recover_world.state_entry / issue.*).
        if (w) {
            std::error_code ec;
            float y = top + 10;
            for (const char* file : {"level.dat", "level.dat_old"}) {
                const fs::path path = w->dir / file;
                NbtValue root;
                Text issue = !fs::exists(path, ec) ? Text(s.res.translate("recover_world.issue.missing_file"), colorStyle(0xFF5555))
                             : readNbtGz(path, root) ? Text(s.res.translate("recover_world.issue.none"), colorStyle(0x55FF55))
                                                     : Text("Failed to read the file", colorStyle(0xFF5555));
                Text line(std::string(file) + ": ");
                line.append(issue);  // Text& append(const Text&)
                s.drawCentered(c, line, float(W / 2), y, 0xFFFFFFFF);
                y += 12;
            }
        }
        s.drawButtons(c);
        return;
    }
    const float x = float(W / 2 - 100), top = float(std::max(0, (H - 234) / 2));
    s.drawCentered(c, tr(s, "selectWorld.edit.title"), float(W / 2), 15, 0xFFFFFFFF);
    s.drawText(c, tr(s, "selectWorld.enterName"), x, top + 25, 0xFFA0A0A0, false);
    drawFields(s, c);
    s.drawButtons(c);
}

void editWorldInput(State& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    if (editPage == EditPage::Backup && prompt.eraseCachePrompt && e.type == E::Button && e.down && e.code == 0) {
        const int lines = int(s.font.split(prompt.description, s.guiW() - 50).size());
        const float bx = float(s.guiW() / 2 - 155 + 80), by = 76 + float((lines + 1) * 9);
        if (s.mouseX >= bx && s.mouseX < bx + 21 + s.font.width(tr(s, "selectWorld.backupEraseCache")) && s.mouseY >= by &&
            s.mouseY < by + 17) {
            eraseCache = !eraseCache;
            return;
        }
    }
    if (editPage == EditPage::Edit) {
        const std::string before = s.fe->editName.text.value;
        if (fieldsInput(s, e)) {
            if (trim(before).empty() != trim(s.fe->editName.text.value).empty()) s.buildButtons();  // Save needs a name
            return;
        }
    }
    s.buttonsInput(e);
}

namespace {

void openRecover(State& s) {
    editPage = EditPage::Recover;
    s.openScreen(Screen::EditWorld);
}

}  // namespace

}  // namespace ui

namespace ui {

bool writeTitleWorld(const fs::path& dir, i64 seed, int dataVersion, const double pos[3], float yawDeg, float pitchDeg) {
    auto tag = [](u8 type) { return NbtValue::make(type); };
    NbtValue data = tag(TagCompound);
    NbtValue v = tag(TagInt);
    v.i = dataVersion;
    data.set("DataVersion", v);
    v = tag(TagString);
    v.s = "Title Screen";
    data.set("LevelName", v);
    NbtValue settings = tag(TagCompound);
    v = tag(TagLong);
    v.i = seed;
    settings.set("seed", v);
    data.set("WorldGenSettings", std::move(settings));
    NbtValue player = tag(TagCompound), p = tag(TagList), r = tag(TagList);
    p.listType = TagDouble, r.listType = TagFloat;
    for (int i = 0; i < 3; ++i) {
        v = tag(TagDouble);
        v.d = pos[i];
        p.items.push_back(v);
    }
    for (float f : {yawDeg, pitchDeg}) {
        v = tag(TagFloat);
        v.d = f;
        r.items.push_back(v);
    }
    player.set("Pos", std::move(p));
    player.set("Rotation", std::move(r));
    data.set("Player", std::move(player));
    NbtValue root = tag(TagCompound);
    root.set("Data", std::move(data));
    std::error_code ec;
    fs::create_directories(dir / "dimensions" / "minecraft" / "overworld" / "region", ec);
    return writeNbtGz(dir / "level.dat", root, "");
}

}  // namespace ui
