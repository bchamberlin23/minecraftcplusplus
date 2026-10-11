// Front end: the world list (SelectWorldScreen / WorldSelectionList), Create New World, Edit World and deleting a
// world (to the Recycle Bin). Worlds are played through the local official server (--play), created by it, or opened
// in the fast local viewer (--world, read only).
#include "ui/frontend.h"
#include "world/nbt_dom.h"

#include <libdeflate.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <fstream>

namespace ui {

namespace {

constexpr int kHeader = 49, kFooter = 60, kRowH = 36, kRowW = 270;

bool readNbtGz(const fs::path& p, NbtValue& root, std::string* rootName = nullptr) {
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

const char* kModes[] = {"survival", "hardcore", "creative"};
const char* kDifficulties[] = {"peaceful", "easy", "normal", "hard"};

WorldEntry* selected(State& s) {
    FrontEndState& f = *s.fe;
    return f.worldSel >= 0 && f.worldSel < int(f.worlds.size()) ? &f.worlds[size_t(f.worldSel)] : nullptr;
}

void filterWorlds(State& s) {
    FrontEndState& f = *s.fe;
    const std::string q = lower(trim(f.worldSearch.text.value));
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

void playWorld(State& s, bool play) {
    WorldEntry* w = selected(s);
    if (!w || w->locked) return;
    s.fe->failTitle = Text("Could not open the world");
    startChild(s, worldArgs(*w, play), Text((play ? "Loading \"" : "Viewing \"") + w->name + "\""), Screen::Worlds, false);
}

void createWorld(State& s) {
    FrontEndState& f = *s.fe;
    std::string name = trim(f.createName.text.value);
    if (name.empty()) name = s.res.translate("selectWorld.newWorld");
    const std::string folder = worldFolderFor(f.cfg.savesDir, name);
    std::vector<std::wstring> a = launch::stickyArguments();
    a.insert(a.end(), {L"--play", L"--world", (f.cfg.savesDir / widen(folder)).wstring(), L"--create-name", widen(name),
                       L"--create-seed", widen(trim(f.createSeed.text.value)), L"--create-mode", widen(kModes[f.createMode]),
                       L"--create-difficulty", widen(kDifficulties[f.createMode == 1 ? 3 : f.createDifficulty]),
                       L"--create-cheats", f.createCheats && f.createMode != 1 ? L"1" : L"0", L"--create-structures",
                       f.createStructures ? L"1" : L"0"});
    LOG("ui: creating world \"%s\" in %s", name.c_str(), folder.c_str());
    f.failTitle = Text("Could not create the world");
    startChild(s, a, Text("Creating \"" + name + "\""), Screen::Worlds, false);
}

void openEditWorld(State& s) {
    WorldEntry* w = selected(s);
    if (!w) return;
    s.fe->editName.text = TextField{};
    s.fe->editName.text.value = w->name;
    s.fe->editName.text.cursor = s.fe->editName.text.selection = int(w->name.size());
    s.openScreen(Screen::EditWorld);
}

void saveEditWorld(State& s) {
    WorldEntry* w = selected(s);
    if (!w) return openWorlds(s);
    const std::string name = trim(s.fe->editName.text.value);
    if (!name.empty() && name != w->name && !worldLocked(w->dir)) {
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
    if (!w || w->locked) return;
    const fs::path dir = w->dir;
    const std::string name = w->name;
    State* p = &s;
    showConfirm(s, tr(s, "selectWorld.deleteQuestion"),
                Text(formatJava(s.res.translate("selectWorld.deleteWarning"), {name}) + "\n(It goes to the Recycle Bin.)"),
                s.res.translate("selectWorld.deleteButton"),
                [p, dir] {
                    if (recycle(dir)) {
                        LOG("ui: moved world %s to the Recycle Bin", utf8(dir).c_str());
                        p->fe->worldSel = -1;
                        openWorlds(*p);
                    } else {
                        LOG("ui: could not delete %s", utf8(dir).c_str());
                        showMessage(*p, tr(*p, "selectWorld.delete_failure"), Text(utf8(dir)), p->res.translate("gui.back"),
                                    [p] { openWorlds(*p); });
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
    if (NbtValue* v = data->find("Version"))
        if (NbtValue* n = v->find("Name")) out.versionName = n->s;
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
        if (readWorld(d.path(), w)) out.push_back(std::move(w));
    }
    std::stable_sort(out.begin(), out.end(), [](const WorldEntry& a, const WorldEntry& b) {
        if (a.lastPlayed != b.lastPlayed) return a.lastPlayed > b.lastPlayed;
        return a.name < b.name;
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
    s.openScreen(Screen::Worlds);
}

void buildWorlds(State& s) {
    FrontEndState& f = *s.fe;
    const int W = s.guiW(), H = s.guiH();
    State* p = &s;
    f.worldSearch.x = float(W / 2 - 100), f.worldSearch.y = 22, f.worldSearch.w = 200;
    setFields(s, {&f.worldSearch}, -1);
    const WorldEntry* w = selected(s);
    const bool can = w && !w->locked;
    const float x0 = float(W / 2 - 154), y0 = float(H - 52), y1 = float(H - 28);
    addButton(s, x0, y0, 150, tr(s, "selectWorld.select"), [p] { playWorld(*p, true); }, can);
    addButton(s, x0 + 158, y0, 150, tr(s, "selectWorld.create"), [p] { openCreateWorld(*p); });
    addButton(s, x0, y1, 71, tr(s, "selectWorld.edit"), [p] { openEditWorld(*p); }, w != nullptr);
    addButton(s, x0 + 79, y1, 71, tr(s, "selectWorld.delete"), [p] { deleteWorld(*p); }, can);
    addButton(s, x0 + 158, y1, 71, Text("View Only"), [p] { playWorld(*p, false); }, can);
    addButton(s, x0 + 237, y1, 71, tr(s, "gui.back"), [p] { p->openScreen(Screen::Title); });
    f.onBack = [p] { p->openScreen(Screen::Title); };
    f.onEnter = [p] { playWorld(*p, true); };
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
        const int textW = kRowW - 35 - 6;
        s.drawText(c, Text(fitText(s.font, w.name, textW)), rowLeft + 35, y + 1, 0xFFFFFFFF, true);
        std::string line2 = w.folder;
        if (w.lastPlayed > 0) line2 += " (" + formatDate(w.lastPlayed) + ")";
        s.drawText(c, Text(fitText(s.font, line2, textW)), rowLeft + 35, y + 12, 0xFF808080, true);
        // LevelSummary.getInfo: game mode, commands, version.
        Text info;
        if (w.locked) {
            info = Text(fitText(s.font, s.res.translate("selectWorld.locked"), textW), colorStyle(0xFF5555));
        } else {
            if (w.hardcore) info.append(s.res.translate("gameMode.hardcore"), colorStyle(0xFF0000));
            else info.append(s.res.translate(w.gameMode == 1 ? "gameMode.creative" : w.gameMode == 2 ? "gameMode.adventure"
                                             : w.gameMode == 3 ? "gameMode.spectator" : "gameMode.survival"));
            if (w.cheats) info.append(", " + s.res.translate("selectWorld.commands"));
            info.append(", " + s.res.translate("selectWorld.version") + " " +
                        (w.versionName.empty() ? s.res.translate("selectWorld.versionUnknown") : w.versionName));
        }
        s.drawText(c, info, rowLeft + 35, y + 21, 0xFF808080, true);
        if (hovered) {
            c.fill(rowLeft, y, rowLeft + 32, y + 32, 0xA0909090);
            const bool overIcon = s.mouseX < rowLeft + 32;
            if (w.locked) {
                blitSprite(c, s.sprites.sprite(overIcon ? "world_list/error_highlighted" : "world_list/error"), rowLeft, y, 32, 32);
                if (overIcon) tooltip = s.font.split(Text(s.res.translate("selectWorld.locked"), colorStyle(0xFF5555)), 175);
            } else {
                blitSprite(c, s.sprites.sprite(overIcon ? "world_list/join_highlighted" : "world_list/join"), rowLeft, y, 32, 32);
            }
        }
    }
    if (f.worldView.empty())
        s.drawCentered(c, Text(f.worlds.empty() ? "No worlds yet: Create New World makes one" : "No worlds match the search"),
                       float(W / 2), top + 16, 0xFFA0A0A0);
    c.popScissor();
    if (const float ms = maxScroll(s, f.worldView.size()); ms > 0) {  // scroll bar right of the rows
        const float x = rowLeft + kRowW + 4, h = bottom - top;
        const float bar = std::max(32.0f, h * h / (h + ms)), by = top + (h - bar) * f.worldScroll / ms;
        blitSprite(c, s.sprites.sprite("widget/scroller_background"), x, top, 6, h);
        blitSprite(c, s.sprites.sprite("widget/scroller"), x, by, 6, bar);
    }
    s.drawButtons(c);
    if (!tooltip.empty()) s.drawTooltip(c, tooltip, s.mouseX, s.mouseY);
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
    if (e.type == E::Key && e.down && (e.code == VK_UP || e.code == VK_DOWN) && !f.worldView.empty()) {
        auto it = std::find(f.worldView.begin(), f.worldView.end(), f.worldSel);
        int i = it == f.worldView.end() ? -1 : int(it - f.worldView.begin());
        i = std::clamp(i + (e.code == VK_DOWN ? 1 : -1), 0, int(f.worldView.size()) - 1);
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
            if (doubleClicked(s, i) || overIcon) playWorld(s, true);
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

// --- create world (CreateWorldScreen, one page) -------------------------------------------------------------------

void openCreateWorld(State& s) {
    FrontEndState& f = *s.fe;
    f.createName.text = TextField{};
    f.createName.text.value = s.res.translate("selectWorld.newWorld");
    f.createName.text.cursor = f.createName.text.selection = int(f.createName.text.value.size());
    f.createName.text.maxLength = 32;
    f.createSeed.text = TextField{};
    f.createSeed.text.maxLength = 32;
    f.createSeed.hint = s.res.translate("selectWorld.seedInfo");
    f.createMode = 0, f.createDifficulty = 2, f.createCheats = false, f.createStructures = true;
    s.openScreen(Screen::CreateWorld);
}

void buildCreateWorld(State& s) {
    FrontEndState& f = *s.fe;
    const int W = s.guiW(), H = s.guiH();
    State* p = &s;
    const float x = float(W / 2 - 100);
    f.createName.x = x, f.createName.y = 39, f.createName.w = 200;
    f.createSeed.x = x, f.createSeed.y = 157, f.createSeed.w = 200;
    setFields(s, {&f.createName, &f.createSeed}, 0);
    Button& mode = addButton(s, x, 76, 200, Text(), [p] {
        FrontEndState& fe = *p->fe;
        fe.createMode = (fe.createMode + 1) % 3;
        fe.createCheats = fe.createMode == 2;  // creative worlds start with commands on, like the game's default
        p->buildButtons();
    });
    mode.labelFn = [p] {
        return trf(*p, "options.generic_value",
                   {tr(*p, "selectWorld.gameMode"), Text(p->res.translate(std::string("selectWorld.gameMode.") + kModes[p->fe->createMode]))});
    };
    const bool hardcore = f.createMode == 1;
    Button& diff = addButton(s, x, 120, 98, Text(), [p] { p->fe->createDifficulty = (p->fe->createDifficulty + 1) % 4; }, !hardcore);
    diff.labelFn = [p] {
        const int d = p->fe->createMode == 1 ? 3 : p->fe->createDifficulty;
        return trf(*p, "options.generic_value",
                   {tr(*p, "options.difficulty"), Text(p->res.translate(std::string("options.difficulty.") + kDifficulties[d]))});
    };
    Button& cheats = addButton(s, x + 102, 120, 98, Text(), [p] { p->fe->createCheats = !p->fe->createCheats; }, !hardcore);
    cheats.labelFn = [p] {
        const bool on = p->fe->createCheats && p->fe->createMode != 1;
        return trf(*p, "options.generic_value", {Text("Commands"), tr(*p, on ? "options.on" : "options.off")});
    };
    Button& structures = addButton(s, x, 182, 200, Text(), [p] { p->fe->createStructures = !p->fe->createStructures; });
    structures.labelFn = [p] {
        return trf(*p, "options.generic_value",
                   {tr(*p, "selectWorld.mapFeatures"), tr(*p, p->fe->createStructures ? "options.on" : "options.off")});
    };
    addButton(s, float(W / 2 - 155), float(H - 28), 150, tr(s, "selectWorld.create"), [p] { createWorld(*p); });
    addButton(s, float(W / 2 + 5), float(H - 28), 150, tr(s, "gui.cancel"), [p] { openWorlds(*p); });
    f.onBack = [p] { openWorlds(*p); };
    f.onEnter = [p] { createWorld(*p); };
}

void drawCreateWorld(State& s, Canvas& c) {
    FrontEndState& f = *s.fe;
    const int W = c.guiWidth();
    const float x = float(W / 2 - 100);
    menuBackground(s, c);
    s.drawCentered(c, tr(s, "selectWorld.create"), float(W / 2), 12, 0xFFFFFFFF);
    s.drawText(c, tr(s, "selectWorld.enterName"), x, 28, 0xFFA0A0A0, true);
    std::string name = trim(f.createName.text.value);
    if (name.empty()) name = s.res.translate("selectWorld.newWorld");
    s.drawText(c, Text(fitText(s.font, s.res.translate("selectWorld.resultFolder") + " " + worldFolderFor(f.cfg.savesDir, name), 200)),
               x, 62, 0xFFA0A0A0, true);
    const std::string mode = std::string("selectWorld.gameMode.") + kModes[f.createMode];
    s.drawText(c, Text(s.res.translate(mode + ".line1")), x, 99, 0xFFA0A0A0, true);
    s.drawText(c, Text(s.res.translate(mode + ".line2")), x, 108, 0xFFA0A0A0, true);
    s.drawText(c, tr(s, "selectWorld.enterSeed"), x, 146, 0xFFA0A0A0, true);
    drawFields(s, c);
    s.drawButtons(c);
}

// --- edit world (EditWorldScreen: rename, open folder) ------------------------------------------------------------

void buildEditWorld(State& s) {
    FrontEndState& f = *s.fe;
    const int W = s.guiW();
    State* p = &s;
    const float x = float(W / 2 - 100);
    f.editName.x = x, f.editName.y = 52, f.editName.w = 200;
    setFields(s, {&f.editName}, 0);
    const WorldEntry* w = selected(s);
    addButton(s, x, 84, 200, tr(s, "selectWorld.edit.openFolder"), [p] {
        if (const WorldEntry* we = selected(*p)) ShellExecuteW(nullptr, L"open", we->dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
    addButton(s, x, 120, 98, tr(s, "selectWorld.edit.save"), [p] { saveEditWorld(*p); }, w && !w->locked);
    addButton(s, x + 102, 120, 98, tr(s, "gui.cancel"), [p] { openWorlds(*p); });
    f.onBack = [p] { openWorlds(*p); };
    f.onEnter = [p] { saveEditWorld(*p); };
}

void drawEditWorld(State& s, Canvas& c) {
    const int W = c.guiWidth();
    const float x = float(W / 2 - 100);
    menuBackground(s, c);
    s.drawCentered(c, tr(s, "selectWorld.edit.title"), float(W / 2), 15, 0xFFFFFFFF);
    s.drawText(c, tr(s, "selectWorld.enterName"), x, 40, 0xFFA0A0A0, true);
    if (const WorldEntry* w = selected(s))
        s.drawCentered(c, Text(fitText(s.font, formatJava(s.res.translate("selectWorld.targetFolder"), {w->folder}), 300)),
                       float(W / 2), 148, 0xFF808080);
    drawFields(s, c);
    s.drawButtons(c);
}

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
