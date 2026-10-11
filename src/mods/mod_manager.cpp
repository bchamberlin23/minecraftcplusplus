#include "mods/mod_manager.h"

#include "assets/asset_source.h"
#include "assets/zip.h"
#include "world/nbt_dom.h"

#include <json/json.hpp>
#include <libdeflate.h>

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

using json = nlohmann::json;

namespace mods {

const char* kindName(Kind k) {
    switch (k) {
    case Kind::ResourcePack: return "Resource pack";
    case Kind::DataPack: return "Data pack";
    case Kind::ResourceAndDataPack: return "Resource + data pack";
    case Kind::FabricMod: return "Fabric mod";
    case Kind::NativeMod: return "Native mod";
    default: return "Not supported";
    }
}

namespace {

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(u8(c)));
    return s;
}

// Lenient JSON: mod metadata often has raw newlines / tabs inside strings, which strict JSON rejects.
json parseLenient(const std::vector<u8>& bytes) {
    json j = json::parse(bytes.begin(), bytes.end(), nullptr, false, true);
    if (!j.is_discarded()) return j;
    std::string s(bytes.begin(), bytes.end());
    for (char& c : s)
        if (u8(c) < 0x20) c = ' ';
    return json::parse(s, nullptr, false, true);
}

// Text component (string, {"text": ...}, [ ... ]) -> plain text.
std::string plainText(const json& j) {
    if (j.is_string()) return j.get<std::string>();
    std::string out;
    if (j.is_array())
        for (const json& part : j) out += plainText(part);
    else if (j.is_object()) {
        if (j.contains("text")) out += plainText(j["text"]);
        else if (j.contains("translate")) out += j.value("fallback", j["translate"].is_string() ? j["translate"].get<std::string>() : "");
        if (j.contains("extra")) out += plainText(j["extra"]);
    }
    return out;
}

std::string oneLine(std::string s) {
    for (char& c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    // The game's formatting codes (section sign + code) are dropped for the list.
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (u8(s[i]) == 0xC2 && i + 2 < s.size() && u8(s[i + 1]) == 0xA7) {
            i += 2;
            continue;
        }
        out += s[i];
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

int formatMajor(const json& v) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_array() && !v.empty() && v[0].is_number_integer()) return v[0].get<int>();
    return -1;
}

// Pack format range of a pack.mcmeta, compared with the game's: a short note when it does not match.
std::string compatNote(const json& meta, int current) {
    if (!meta.is_object() || !meta.contains("pack") || current <= 0) return {};
    const json& p = meta["pack"];
    int lo = -1, hi = -1;
    if (p.contains("min_format")) lo = formatMajor(p["min_format"]);
    if (p.contains("max_format")) hi = formatMajor(p["max_format"]);
    if (lo < 0 && p.contains("supported_formats")) {
        const json& s = p["supported_formats"];
        if (s.is_array() && s.size() == 2) lo = formatMajor(s[0]), hi = formatMajor(s[1]);
        else if (s.is_object()) lo = s.value("min_inclusive", -1), hi = s.value("max_inclusive", -1);
        else if (s.is_number_integer()) lo = hi = s.get<int>();
    }
    if (lo < 0 && p.contains("pack_format")) lo = hi = formatMajor(p["pack_format"]);
    if (lo < 0) return {};
    if (hi < lo) hi = lo;
    if (current < lo) return "made for a newer version";
    if (current > hi) return "made for an older version";
    return {};
}

bool readNbtGz(const fs::path& p, NbtValue& root) {
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
    return r == LIBDEFLATE_SUCCESS && nbtRead(raw.data(), actual, root);
}

struct PackVersion {
    int resource = 0, data = 0;
};

PackVersion packVersion(const AssetSource& src) {
    std::vector<u8> bytes;
    PackVersion v;
    if (!src.read("version.json", bytes)) return v;
    const json j = json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (!j.is_object() || !j.contains("pack_version")) return v;
    const json& pv = j["pack_version"];
    if (pv.is_object()) v.resource = pv.value("resource_major", 0), v.data = pv.value("data_major", 0);
    else if (pv.is_number_integer()) v.resource = v.data = pv.get<int>();
    return v;
}

// Fills kind / names / flags of an entry from its file or folder.
void detect(Entry& e, const PackVersion& pv) {
    std::error_code ec;
    const std::string ext = lower(e.path.extension().string());
    if (fs::is_directory(e.path, ec)) {
        if (!fs::exists(e.path / "pack.mcmeta", ec)) {
            e.warnings.push_back("folder without pack.mcmeta: not a pack");
            return;
        }
        e.hasAssets = fs::is_directory(e.path / "assets", ec);
        e.hasData = fs::is_directory(e.path / "data", ec);
        std::vector<u8> bytes;
        const json meta = readFile(e.path / "pack.mcmeta", bytes) ? parseLenient(bytes) : json();
        if (meta.is_object() && meta.contains("pack") && meta["pack"].contains("description"))
            e.description = oneLine(plainText(meta["pack"]["description"]));
        e.kind = e.hasAssets && e.hasData ? Kind::ResourceAndDataPack : e.hasData ? Kind::DataPack : Kind::ResourcePack;
        e.compat = compatNote(meta, e.hasAssets ? pv.resource : pv.data);
        return;
    }
    if (ext == ".dll") {
        e.kind = Kind::NativeMod;
        e.description = "native mod (DLL)";
        return;
    }
    if (ext != ".zip" && ext != ".jar") {
        e.warnings.push_back("unknown file type (drop .zip / .jar / .dll files or pack folders here)");
        return;
    }
    ZipArchive zip;
    if (!zip.open(e.path)) {
        e.warnings.push_back("not a readable zip archive");
        return;
    }
    const ZipArchive::Entry* fabric = nullptr;
    const ZipArchive::Entry* mcmeta = nullptr;
    bool forge = false, quilt = false;
    std::set<std::string> nestedRoots;
    for (const auto& z : zip.entries()) {
        const std::string_view n = z.name;
        if (n == "fabric.mod.json") fabric = &z;
        else if (n == "pack.mcmeta") mcmeta = &z;
        else if (n == "quilt.mod.json") quilt = true;
        else if (n == "META-INF/mods.toml" || n == "META-INF/neoforge.mods.toml") forge = true;
        else if (n.ends_with("/pack.mcmeta") && std::count(n.begin(), n.end(), '/') == 1)
            nestedRoots.insert(std::string(n.substr(0, n.find('/') + 1)));
    }
    if (!mcmeta && !fabric && nestedRoots.size() == 1) {  // "MyPack/pack.mcmeta": zipped with its folder
        e.prefix = *nestedRoots.begin();
        for (const auto& z : zip.entries())
            if (z.name == e.prefix + "pack.mcmeta") mcmeta = &z;
    }
    std::vector<u8> bytes;
    if (fabric) {
        e.kind = Kind::FabricMod;
        const json j = zip.read(*fabric, bytes) ? parseLenient(bytes) : json();
        if (!j.is_object()) {
            e.warnings.push_back("fabric.mod.json is unreadable");
            e.kind = Kind::Unsupported;
            return;
        }
        e.modId = j.value("id", "");
        e.version = j.contains("version") && j["version"].is_string() ? j["version"].get<std::string>() : "";
        if (j.contains("name") && j["name"].is_string()) e.name = j["name"].get<std::string>() + (e.version.empty() ? "" : " " + e.version);
        if (j.contains("description") && j["description"].is_string()) e.description = oneLine(j["description"].get<std::string>());
        e.environment = j.contains("environment") && j["environment"].is_string() ? j["environment"].get<std::string>() : "*";
        // Content a vanilla client cannot show: the mod ships block or item definitions in its own namespace.
        for (const auto& z : zip.entries()) {
            const std::string_view n = z.name;
            if (!n.starts_with("assets/") || n.starts_with("assets/minecraft/")) continue;
            const size_t slash = n.find('/', 7);
            if (slash == std::string_view::npos) continue;
            const std::string_view rest = n.substr(slash + 1);
            if (rest.starts_with("blockstates/") || rest.starts_with("items/")) {
                e.clientContent = true;
                break;
            }
        }
        for (const auto& z : zip.entries()) {
            if (z.name.starts_with("assets/")) e.hasAssets = true;
            if (z.name.starts_with("data/")) e.hasData = true;
        }
        if (e.environment == "client")
            e.warnings.push_back("client-only mod: it changes the Java client, which this engine replaces; not used");
        else if (e.clientContent)
            e.warnings.push_back("adds blocks / items: they are drawn from the mod's own models (best effort); "
                                 "custom renderers, GUIs and entities of the mod are not shown");
        return;
    }
    if (mcmeta) {
        for (const auto& z : zip.entries()) {
            if (!z.name.starts_with(e.prefix)) continue;
            const std::string_view n = std::string_view(z.name).substr(e.prefix.size());
            if (n.starts_with("assets/")) e.hasAssets = true;
            if (n.starts_with("data/")) e.hasData = true;
        }
        const json meta = zip.read(*mcmeta, bytes) ? parseLenient(bytes) : json();
        if (meta.is_object() && meta.contains("pack") && meta["pack"].contains("description"))
            e.description = oneLine(plainText(meta["pack"]["description"]));
        if (!e.hasAssets && !e.hasData) {
            e.warnings.push_back("pack.mcmeta but no assets/ or data/ folder");
            return;
        }
        e.kind = e.hasAssets && e.hasData ? Kind::ResourceAndDataPack : e.hasData ? Kind::DataPack : Kind::ResourcePack;
        e.compat = compatNote(meta, e.hasAssets ? pv.resource : pv.data);
        return;
    }
    if (forge) e.warnings.push_back("Forge / NeoForge mod: only Fabric mods are supported");
    else if (quilt) e.warnings.push_back("Quilt-only mod: only Fabric mods are supported");
    else e.warnings.push_back("no pack.mcmeta or fabric.mod.json inside");
}

bool hiddenFile(const fs::path& p) {
    const std::string n = lower(p.filename().string());
    return n.empty() || n[0] == '.' || n.ends_with(".tmp") || n.ends_with(".part") || n.ends_with(".crdownload") ||
           n == "desktop.ini" || n == "thumbs.db";
}

int group(const Entry& e) {  // list order: mods first, then the packs (in priority order), then the rest
    if (e.kind == Kind::FabricMod) return 0;
    if (e.kind == Kind::NativeMod) return 1;
    if (e.isResourcePack() || e.isDataPack()) return 2;
    return 3;
}

void copyTree(const fs::path& from, const fs::path& to, std::error_code& ec) {
    if (fs::is_directory(from, ec)) {
        fs::create_directories(to, ec);
        fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    } else {
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    }
}

}  // namespace

Manager& Manager::get() {
    static Manager m;
    return m;
}

// The static instance is destroyed after wmain returned: the world is saved and the local server has stopped.
Manager::~Manager() {
    if (!restart_) return;
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    // Same arguments, minus a UI test script (it would replay into the new process).
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            if (std::wstring_view(argv[i]) == L"--script") {
                ++i;
                continue;
            }
            cmd += L" \"" + std::wstring(argv[i]) + L"\"";
        }
        LocalFree(argv);
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void Manager::init(const Config& config) {
    Timer t;
    config_ = config;
    modsDir_ = config.modsDir;
    if (modsDir_.empty())
        if (const wchar_t* env = _wgetenv(L"MCPP_MODS_DIR"); env && *env) modsDir_ = env;
    if (modsDir_.empty()) {
        const wchar_t* appdata = _wgetenv(L"APPDATA");
        modsDir_ = fs::path(appdata ? appdata : L".") / "minecraftCplusplus" / "mods";
    }
    std::error_code ec;
    modsDir_ = fs::absolute(modsDir_, ec);
    fs::create_directories(modsDir_, ec);
    configFile_ = modsDir_.parent_path() / (modsDir_.filename().wstring() + L".json");  // ...\mods.json

    // The vanilla jar first: pack detection reads the game's pack format from it.
    AssetSource& client = AssetSource::client();
    client.clear();
    AssetSource::addVanilla(client, config.jar, true);
    vanillaPacks_ = client.packCount();
    const PackVersion pv = packVersion(client);
    resourceFormat_ = pv.resource, dataFormat_ = pv.data;
    scan();
    loadConfig();
    if (config.play && !config.world.empty()) installWorldDataPacks();
    buildClientStack();
    buildDataStack();
    startState_ = stateKey();
    initialized_ = true;

    int rp = 0, dp = 0, fm = 0, nm = 0;
    for (const Entry& e : entries_) {
        if (!e.enabled) continue;
        rp += e.isResourcePack(), dp += e.isDataPack(), fm += e.kind == Kind::FabricMod, nm += e.kind == Kind::NativeMod;
    }
    LOG("mods    %8.1f ms  %d resource packs, %d data packs, %d Fabric mods, %d native mods enabled (%s)", t.ms(), rp, dp,
        fm, nm, utf8(modsDir_).c_str());
    for (const Entry& e : entries_) {
        if (!e.enabled && e.kind != Kind::Unsupported) continue;
        LOG("mods      %-3s %-21s %s%s%s", e.enabled ? "on" : "--", kindName(e.kind), e.name.c_str(),
            e.compat.empty() ? "" : (" (" + e.compat + ")").c_str(), e.source == "mods folder" ? "" : (" [" + e.source + "]").c_str());
        for (const std::string& w : e.warnings) LOG("mods          note: %s", w.c_str());
    }
    if (!worldPacks_.empty()) {
        std::string list;
        for (const std::string& p : worldPacks_) list += (list.empty() ? "" : ", ") + p;
        LOG("mods      world data packs: %s", list.c_str());
    }
}

void Manager::detectEntry(Entry& e) {
    const Kind before = e.kind;
    e.kind = Kind::Unsupported, e.hasAssets = e.hasData = false, e.warnings.clear();
    detect(e, PackVersion{resourceFormat_, dataFormat_});
    e.detected = true;
    if (before == Kind::ResourcePack && e.source != "mods folder") {  // only resource packs live there
        if (!e.isResourcePack()) e.enabled = false, e.kind = Kind::Unsupported;
        else e.kind = Kind::ResourcePack;
    }
}

void Manager::detectPending() {
    for (Entry& e : entries_)
        if (!e.detected) detectEntry(e);
}

void Manager::scan() {
    entries_.clear();
    const PackVersion pv{resourceFormat_, dataFormat_};
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& d : fs::directory_iterator(modsDir_, ec))
        if (!hiddenFile(d.path())) files.push_back(d.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        Entry e;
        e.path = f;
        e.id = "mods/" + utf8(f.filename());
        e.name = utf8(f.filename());
        e.source = "mods folder";
        detect(e, pv);
        entries_.push_back(std::move(e));
    }
    // The vanilla launcher's resource packs (read only).
    files.clear();
    for (const auto& d : fs::directory_iterator(config_.minecraftDir / "resourcepacks", ec))
        if (!hiddenFile(d.path())) files.push_back(d.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        Entry e;
        e.path = f;
        e.id = "resourcepacks/" + utf8(f.filename());
        e.name = utf8(f.filename());
        e.source = ".minecraft resourcepacks";
        // Opened only when enabled or shown (they can be large): until then assumed to be a resource pack.
        const std::string ext = lower(f.extension().string());
        if (ext != ".zip" && !fs::is_directory(f, ec)) continue;
        e.kind = Kind::ResourcePack, e.hasAssets = true, e.detected = false;
        entries_.push_back(std::move(e));
    }
    // The game's built-in optional packs.
    {
        const AssetSource& client = AssetSource::client();
        for (size_t i = 0; i < client.packCount(); ++i) {
            const fs::path zip = client.pack(i).diskPath("assets/minecraft/resourcepacks/programmer_art.zip");
            if (zip.empty()) continue;
            Entry e;
            e.path = zip, e.id = "builtin/programmer_art", e.name = "Programmer Art", e.source = "built-in";
            e.description = "The classic look of Minecraft";
            e.kind = Kind::ResourcePack, e.hasAssets = true;
            entries_.push_back(std::move(e));
        }
        if (client.packCount() && client.pack(0).has("resourcepacks/high_contrast/pack.mcmeta")) {
            Entry e;
            e.path = config_.jar, e.prefix = "resourcepacks/high_contrast/", e.id = "builtin/high_contrast";
            e.name = "High Contrast", e.source = "built-in", e.description = "Enhances the contrast of UI elements";
            e.kind = Kind::ResourcePack, e.hasAssets = true;
            entries_.push_back(std::move(e));
        }
    }
    // Fabric dependencies that are missing from the folder.
    std::set<std::string> have = {"minecraft", "java", "fabricloader", "fabric", "mixinextras"};  // the loader's own
    bool fabricApi = false;
    for (const Entry& e : entries_)
        if (e.kind == Kind::FabricMod) {
            have.insert(e.modId);
            fabricApi |= e.modId == "fabric-api";
        }
    for (Entry& e : entries_) {
        if (e.kind != Kind::FabricMod) continue;
        ZipArchive zip;
        std::vector<u8> bytes;
        if (!zip.open(e.path)) continue;
        for (const auto& z : zip.entries())
            if (z.name == "fabric.mod.json" && zip.read(z, bytes)) {
                const json j = parseLenient(bytes);
                if (!j.is_object() || !j.contains("depends") || !j["depends"].is_object()) break;
                for (const auto& [dep, range] : j["depends"].items()) {
                    if (have.count(dep) || (fabricApi && dep.starts_with("fabric-"))) continue;
                    e.warnings.push_back("needs the mod '" + dep + "', which is not in the mods folder");
                }
                break;
            }
    }
}

void Manager::loadConfig() {
    std::vector<u8> bytes;
    json cfg = readFile(configFile_, bytes) ? json::parse(bytes.begin(), bytes.end(), nullptr, false) : json();
    std::map<std::string, std::pair<int, bool>> known;  // id -> (position, enabled)
    if (cfg.is_object() && cfg.contains("entries") && cfg["entries"].is_array()) {
        int pos = 0;
        for (const json& item : cfg["entries"])
            if (item.is_object() && item.contains("id") && item["id"].is_string())
                known[item["id"].get<std::string>()] = {pos++, item.value("enabled", true)};
    }
    // Vanilla selection (options.txt "resourcePacks", bottom first) for packs this config has not seen yet.
    std::vector<std::string> vanillaSelected;
    {
        std::ifstream f(config_.minecraftDir / "options.txt");
        std::string line;
        while (std::getline(f, line))
            if (line.starts_with("resourcePacks:")) {
                const json list = json::parse(line.substr(14), nullptr, false);
                if (list.is_array())
                    for (const json& id : list)
                        if (id.is_string()) vanillaSelected.push_back(id.get<std::string>());
            }
    }
    auto vanillaRank = [&](const Entry& e) -> int {  // higher = closer to the top; -1 not selected
        std::string vid;
        if (e.id.starts_with("resourcepacks/")) vid = "file/" + e.id.substr(14);
        else if (e.id.starts_with("builtin/")) vid = e.id.substr(8);
        for (size_t i = 0; i < vanillaSelected.size(); ++i)
            if (vanillaSelected[i] == vid) return int(i);
        return -1;
    };
    // Display order (top = highest priority): known entries in saved order; new mods-folder packs on top of their
    // group, new vanilla packs below them (selected ones by their vanilla priority, then unselected).
    struct Key {
        int group, band, rank;
        std::string name;
    };
    std::vector<std::pair<Key, size_t>> order;
    for (size_t i = 0; i < entries_.size(); ++i) {
        Entry& e = entries_[i];
        Key k{group(e), 0, 0, lower(e.name)};
        if (auto it = known.find(e.id); it != known.end()) {
            e.enabled = it->second.second;
            k.band = 1, k.rank = it->second.first;
        } else if (e.source == "mods folder") {
            e.enabled = e.kind != Kind::Unsupported;
            k.band = 0;
        } else {
            const int r = vanillaRank(e);
            e.enabled = r >= 0;
            k.band = 2, k.rank = r >= 0 ? -r : 1000;
        }
        if (e.kind == Kind::Unsupported) e.enabled = false;
        order.push_back({k, i});
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        const Key &x = a.first, &y = b.first;
        if (x.group != y.group) return x.group < y.group;
        if (x.band != y.band) return x.band < y.band;
        if (x.rank != y.rank) return x.rank < y.rank;
        return x.name < y.name;
    });
    std::vector<Entry> sorted;
    for (const auto& [k, i] : order) sorted.push_back(std::move(entries_[i]));
    entries_ = std::move(sorted);
    for (Entry& e : entries_)
        if (e.enabled && !e.detected) detectEntry(e);
    if (!known.empty() || !entries_.empty()) saveConfig();
}

void Manager::saveConfig() const {
    json cfg = json::object();
    std::vector<u8> bytes;
    if (readFile(configFile_, bytes)) {  // keep other sections (installed world copies)
        json old = json::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (old.is_object()) cfg = old;
    }
    cfg["version"] = 1;
    json list = json::array();
    for (const Entry& e : entries_) list.push_back({{"id", e.id}, {"enabled", e.enabled}});
    cfg["entries"] = list;
    std::error_code ec;
    fs::create_directories(configFile_.parent_path(), ec);
    const fs::path tmp = configFile_.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << cfg.dump(2) << "\n";
        if (!f) return;
    }
    fs::rename(tmp, configFile_, ec);
}

void Manager::buildClientStack() {
    AssetSource& client = AssetSource::client();
    client.truncate(vanillaPacks_);
    const std::shared_ptr<ZipArchive> jar = client.packCount() ? client.pack(0).archive() : nullptr;
    // Fabric mods' own assets (blocks / items they add, their lang) sit right above the game's.
    for (const Entry& e : entries_)
        if (e.enabled && e.kind == Kind::FabricMod && e.hasAssets && e.environment != "client")
            if (auto p = AssetPack::openZip(e.path, e.id)) client.add(std::move(p));
    // Resource packs, bottom (end of the list) to top (start).
    for (size_t i = entries_.size(); i-- > 0;) {
        const Entry& e = entries_[i];
        if (!e.enabled || !e.isResourcePack()) continue;
        std::unique_ptr<AssetPack> p;
        if (e.id == "builtin/high_contrast") p = AssetPack::openZip(e.path, e.id, e.prefix, jar);
        else if (fs::is_directory(e.path)) p = AssetPack::openFolder(e.path, e.id);
        else p = AssetPack::openZip(e.path, e.id, e.prefix);
        if (p) client.add(std::move(p));
        else LOG("mods: cannot read %s", e.name.c_str());
    }
}

void Manager::buildDataStack() {
    AssetSource& data = AssetSource::data();
    data.clear();
    worldPacks_.clear();
    if (AssetSource::client().packCount()) data.add(AssetSource::client().packPtr(0));  // the jar, shared
    const std::shared_ptr<ZipArchive> jar = data.packCount() ? data.pack(0).archive() : nullptr;
    if (!jar) return;
    // Mods' data (Fabric loads it right above the game's).
    for (const Entry& e : entries_)
        if (e.enabled && e.kind == Kind::FabricMod && e.hasData && e.environment != "client")
            if (auto p = AssetPack::openZip(e.path, e.id)) data.add(std::move(p));

    // The world's packs in the order the server applies them: level.dat's enabled list, then packs it has not
    // seen yet (enabled automatically, sorted by id) unless listed as disabled.
    std::vector<std::string> enabled, disabled;
    NbtValue root;
    if (!config_.world.empty() && readNbtGz(config_.world / "level.dat", root))
        if (NbtValue* d = root.find("Data"))
            if (NbtValue* packs = d->find("DataPacks")) {
                auto names = [](NbtValue* list, std::vector<std::string>& out) {
                    if (list)
                        for (const NbtValue& v : list->items) out.push_back(v.s);
                };
                names(packs->find("Enabled"), enabled);
                names(packs->find("Disabled"), disabled);
            }
    const fs::path dir = config_.world.empty() ? fs::path() : config_.world / "datapacks";
    std::set<std::string> worldFiles;
    std::error_code ec;
    std::vector<std::string> fresh;
    if (!dir.empty())
        for (const auto& d : fs::directory_iterator(dir, ec)) {
            const std::string name = utf8(d.path().filename());
            worldFiles.insert(name);
            const std::string id = "file/" + name;
            if (std::find(enabled.begin(), enabled.end(), id) == enabled.end() &&
                std::find(disabled.begin(), disabled.end(), id) == disabled.end())
                fresh.push_back(id);
        }
    std::sort(fresh.begin(), fresh.end());
    std::vector<std::string> order = enabled;
    order.insert(order.end(), fresh.begin(), fresh.end());
    const PackVersion pv = packVersion(data);
    for (const std::string& id : order) {
        if (id == "vanilla" || id == "fabric" || id.starts_with("fabric-")) continue;
        std::unique_ptr<AssetPack> p;
        if (id.starts_with("file/")) {
            const fs::path f = dir / fs::path(std::u8string(id.begin() + 5, id.end()));
            Entry probe;
            probe.path = f;
            detect(probe, pv);
            if (!probe.isDataPack()) continue;  // the server skips what is not a data pack too
            p = fs::is_directory(f, ec) ? AssetPack::openFolder(f, id) : AssetPack::openZip(f, id, probe.prefix);
        } else if (jar) {  // the game's feature packs (data/minecraft/datapacks/<id>/)
            const std::string prefix = "data/minecraft/datapacks/" + id + "/";
            if (!data.pack(0).has(prefix + "pack.mcmeta")) continue;  // a mod's pack: covered above
            p = AssetPack::openZip(config_.jar, id, prefix, jar);
        }
        if (!p) continue;
        worldPacks_.push_back(id);
        data.add(std::move(p));
    }
    // Global data packs not installed in this world (viewing without --play): on top.
    for (size_t i = entries_.size(); i-- > 0;) {
        const Entry& e = entries_[i];
        if (!e.enabled || !e.isDataPack() || worldFiles.count(utf8(e.path.filename()))) continue;
        auto p = fs::is_directory(e.path, ec) ? AssetPack::openFolder(e.path, e.id) : AssetPack::openZip(e.path, e.id, e.prefix);
        if (!p) continue;
        worldPacks_.push_back(e.id);
        data.add(std::move(p));
    }
}

// --play: enabled global data packs are copied into the world (the server only loads the world's own packs);
// copies made earlier for packs that are now off or gone are removed again.
void Manager::installWorldDataPacks() {
    std::vector<u8> bytes;
    json cfg = readFile(configFile_, bytes) ? json::parse(bytes.begin(), bytes.end(), nullptr, false) : json::object();
    if (!cfg.is_object()) cfg = json::object();
    std::error_code ec;
    const std::string worldKey = utf8(fs::weakly_canonical(config_.world, ec));
    json& installed = cfg["installed"];
    if (!installed.is_object()) installed = json::object();
    std::set<std::string> copies;
    if (installed.contains(worldKey) && installed[worldKey].is_array())
        for (const json& n : installed[worldKey])
            if (n.is_string()) copies.insert(n.get<std::string>());
    const fs::path dir = config_.world / "datapacks";
    fs::create_directories(dir, ec);
    std::set<std::string> wanted;
    for (const Entry& e : entries_) {
        if (!e.enabled || !e.isDataPack() || e.source != "mods folder") continue;
        const std::string name = utf8(e.path.filename());
        wanted.insert(name);
        if (fs::exists(dir / e.path.filename(), ec)) continue;
        copyTree(e.path, dir / e.path.filename(), ec);
        if (ec) {
            LOG("mods: could not install the data pack %s into the world: %s", name.c_str(), ec.message().c_str());
            continue;
        }
        copies.insert(name);
        LOG("mods: data pack %s installed into %s", name.c_str(), utf8(config_.world.filename()).c_str());
    }
    for (auto it = copies.begin(); it != copies.end();) {
        if (wanted.count(*it)) {
            ++it;
            continue;
        }
        fs::remove_all(dir / fs::path(std::u8string(it->begin(), it->end())), ec);
        LOG("mods: data pack %s removed from %s (turned off)", it->c_str(), utf8(config_.world.filename()).c_str());
        it = copies.erase(it);
    }
    installed[worldKey] = json(std::vector<std::string>(copies.begin(), copies.end()));
    if (installed[worldKey].empty()) installed.erase(worldKey);
    const fs::path tmp = configFile_.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << cfg.dump(2) << "\n";
        if (!f) return;
    }
    fs::rename(tmp, configFile_, ec);
}

std::string Manager::stateKey() const {
    std::string s;
    for (const Entry& e : entries_)
        if (e.enabled) s += e.id + "\n";
    return s;
}

void Manager::setEnabled(size_t index, bool on) {
    if (index >= entries_.size() || entries_[index].kind == Kind::Unsupported) return;
    entries_[index].enabled = on;
    saveConfig();
}

void Manager::move(size_t index, int delta) {
    if (index >= entries_.size() || delta == 0) return;
    const int g = group(entries_[index]);
    const long long target = (long long)index + delta;
    if (target < 0 || target >= (long long)entries_.size() || group(entries_[size_t(target)]) != g) return;
    std::swap(entries_[index], entries_[size_t(target)]);
    saveConfig();
}

bool Manager::restartNeeded() const { return initialized_ && stateKey() != startState_; }

void Manager::openModsFolder() const {
    std::error_code ec;
    fs::create_directories(modsDir_, ec);
    ShellExecuteW(nullptr, L"open", modsDir_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Manager::rescan() {
    saveConfig();
    scan();
    loadConfig();
    detectPending();
}

std::vector<fs::path> Manager::fabricMods() const {
    std::vector<fs::path> out;
    for (const Entry& e : entries_)
        if (e.enabled && e.kind == Kind::FabricMod && e.environment != "client") out.push_back(e.path);
    return out;
}

std::vector<fs::path> Manager::nativeMods() const {
    std::vector<fs::path> out;
    for (const Entry& e : entries_)
        if (e.enabled && e.kind == Kind::NativeMod) out.push_back(e.path);
    return out;
}

bool Manager::fabricContent() const {
    for (const Entry& e : entries_)
        if (e.enabled && e.kind == Kind::FabricMod && e.clientContent && e.environment != "client") return true;
    return false;
}

}  // namespace mods
