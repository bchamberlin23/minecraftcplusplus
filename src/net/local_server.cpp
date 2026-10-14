#include "net/local_server.h"

#include "assets/game_reports.h"
#include "assets/zip.h"
#include "mods/fabric_server.h"
#include "mods/mod_manager.h"
#include "world/nbt_dom.h"

#include <libdeflate.h>
#include <json/json.hpp>

#include <winsock2.h>
#include <bcrypt.h>
#include <urlmon.h>

#include <chrono>
#include <ctime>
#include <fstream>

#pragma comment(lib, "urlmon.lib")

namespace {

nlohmann::json readJson(const fs::path& p) {
    std::vector<u8> bytes;
    if (!readFile(p, bytes)) return nullptr;
    return nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
}

bool hash(const wchar_t* algorithm, const void* data, size_t size, std::vector<u8>& out) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, algorithm, nullptr, 0) != 0) return false;
    DWORD len = 0, got = 0;
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&len), sizeof(len), &got, 0);
    out.resize(len);
    const bool ok = BCryptHash(alg, nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(data)), ULONG(size), out.data(),
                               len) == 0;
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string hex(const std::vector<u8>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (u8 b : bytes) s += digits[b >> 4], s += digits[b & 15];
    return s;
}

std::string sha1File(const fs::path& p) {
    std::vector<u8> bytes, digest;
    if (!readFile(p, bytes) || !hash(BCRYPT_SHA1_ALGORITHM, bytes.data(), bytes.size(), digest)) return {};
    return hex(digest);
}

// UUID.nameUUIDFromBytes(("OfflinePlayer:" + name).getBytes(UTF_8)): the id an offline-mode server gives a name.
std::vector<u8> offlineUuid(const std::string& name) {
    const std::string s = "OfflinePlayer:" + name;
    std::vector<u8> md5;
    hash(BCRYPT_MD5_ALGORITHM, s.data(), s.size(), md5);
    md5[6] = u8((md5[6] & 0x0F) | 0x30);
    md5[8] = u8((md5[8] & 0x3F) | 0x80);
    return md5;
}

std::string uuidString(const std::vector<u8>& b) {
    const std::string h = hex(b);
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20);
}

// NBT stores UUIDs as four big-endian ints.
std::string uuidFromInts(const std::vector<i32>& v) {
    if (v.size() != 4) return {};
    std::vector<u8> b;
    for (i32 i : v)
        for (int k = 3; k >= 0; --k) b.push_back(u8(u32(i) >> (k * 8)));
    return uuidString(b);
}

std::vector<i32> intsFromUuid(const std::string& s) {
    std::string h;
    for (char c : s)
        if (c != '-') h += c;
    std::vector<i32> v;
    for (int i = 0; i < 4 && h.size() == 32; ++i) v.push_back(i32(u32(std::stoul(h.substr(size_t(i) * 8, 8), nullptr, 16))));
    return v;
}

bool readNbtGz(const fs::path& p, NbtValue& root, std::string* rootName = nullptr) {
    std::vector<u8> gz;
    if (!readFile(p, gz) || gz.size() < 18) return false;
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    std::vector<u8> raw(gz.size() * 8 + 4096);
    size_t actual = 0;
    libdeflate_result r;
    while ((r = libdeflate_gzip_decompress(d, gz.data(), gz.size(), raw.data(), raw.size(), &actual)) ==
               LIBDEFLATE_INSUFFICIENT_SPACE &&
           raw.size() < (size_t(256) << 20))
        raw.resize(raw.size() * 2);
    libdeflate_free_decompressor(d);
    return r == LIBDEFLATE_SUCCESS && nbtRead(raw.data(), actual, root, rootName);
}

bool writeNbtGz(const fs::path& p, const NbtValue& root, std::string_view rootName = "") {
    std::vector<u8> encoded;
    nbtWrite(root, encoded, rootName);
    libdeflate_compressor* c = libdeflate_alloc_compressor(6);
    std::vector<u8> packed(libdeflate_gzip_compress_bound(c, encoded.size()));
    const size_t n = libdeflate_gzip_compress(c, encoded.data(), encoded.size(), packed.data(), packed.size());
    libdeflate_free_compressor(c);
    if (!n) return false;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(packed.data()), std::streamsize(n));
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

u16 freePort() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    u16 port = 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(a);
    if (s != INVALID_SOCKET && bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 &&
        getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0)
        port = ntohs(a.sin_port);
    if (s != INVALID_SOCKET) closesocket(s);
    WSACleanup();
    return port ? port : 25565;
}

// The bundler's extracted classpath (META-INF/classpath-joined, relative to the bundler repo) and main class, for a
// direct launch; empty when the bundler has not unpacked everything yet (its own first run does).
std::wstring directClasspath(const fs::path& jar, const fs::path& repo, std::wstring& mainClass) {
    ZipArchive zip;
    if (!zip.open(jar)) return {};
    std::string joined, main;
    for (const ZipArchive::Entry& e : zip.entries()) {
        std::vector<u8> bytes;
        if (e.name == "META-INF/classpath-joined" && zip.read(e, bytes)) joined.assign(bytes.begin(), bytes.end());
        if (e.name == "META-INF/main-class" && zip.read(e, bytes)) main.assign(bytes.begin(), bytes.end());
    }
    while (!main.empty() && std::isspace(u8(main.back()))) main.pop_back();
    if (joined.empty() || main.empty()) return {};
    std::wstring cp;
    std::error_code ec;
    for (size_t start = 0; start <= joined.size();) {
        size_t end = joined.find(';', start);
        if (end == std::string::npos) end = joined.size();
        std::string rel = joined.substr(start, end - start);
        while (!rel.empty() && std::isspace(u8(rel.back()))) rel.pop_back();
        start = end + 1;
        if (rel.empty()) continue;
        const fs::path p = repo / fs::path(rel);
        if (!fs::exists(p, ec)) return {};
        if (!cp.empty()) cp += L';';
        cp += p.wstring();
    }
    mainClass.assign(main.begin(), main.end());
    return cp;
}

// The world format version this game version writes ("world_version" in the jar's version.json).
int gameWorldVersion(const fs::path& jar) {
    ZipArchive zip;
    if (!zip.open(jar)) return 0;
    for (const auto& e : zip.entries())
        if (e.name == "version.json") {
            std::vector<u8> bytes;
            if (!zip.read(e, bytes)) return 0;
            const auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
            return j.is_object() ? j.value("world_version", 0) : 0;
        }
    return 0;
}

// The data pack format of the installed game (version.json pack_version.data_major).
int gameDataFormat(const fs::path& jar) {
    ZipArchive zip;
    if (!zip.open(jar)) return 0;
    for (const auto& e : zip.entries())
        if (e.name == "version.json") {
            std::vector<u8> bytes;
            if (!zip.read(e, bytes)) return 0;
            const auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
            return j.is_object() && j.contains("pack_version") ? j["pack_version"].value("data_major", 0) : 0;
        }
    return 0;
}

// Single Biome (CreateBuffetWorldScreen): a dedicated server only knows the preset's own biome, so a data pack in the
// new world overrides the single_biome_surface preset while the world is made (removed again in finishNewWorld; the
// world keeps its dimensions in its generation settings).
constexpr const char* kBiomePack = "mcpp_single_biome";

bool writeSingleBiomePack(const fs::path& world, const fs::path& jar, const std::string& biome) {
    const fs::path dir = world / "datapacks" / kBiomePack;
    std::error_code ec;
    fs::create_directories(dir / "data" / "minecraft" / "worldgen" / "world_preset", ec);
    const int format = gameDataFormat(jar);
    nlohmann::json meta = {{"pack", {{"description", "Single Biome world creation (minecraftCplusplus)"},
                                     {"min_format", format}, {"max_format", format}}}};
    auto noise = [](nlohmann::json source, const char* settings) {
        return nlohmann::json{{"type", "minecraft:noise"}, {"biome_source", std::move(source)}, {"settings", settings}};
    };
    nlohmann::json preset = {
        {"dimensions",
         {{"minecraft:overworld", {{"type", "minecraft:overworld"},
                                   {"generator", noise({{"type", "minecraft:fixed"}, {"biome", biome}}, "minecraft:overworld")}}},
          {"minecraft:the_nether", {{"type", "minecraft:the_nether"},
                                    {"generator", noise({{"type", "minecraft:multi_noise"}, {"preset", "minecraft:nether"}},
                                                        "minecraft:nether")}}},
          {"minecraft:the_end",
           {{"type", "minecraft:the_end"}, {"generator", noise({{"type", "minecraft:the_end"}}, "minecraft:end")}}}}}};
    std::ofstream m(dir / "pack.mcmeta", std::ios::trunc), p(dir / "data" / "minecraft" / "worldgen" / "world_preset" /
                                                                 "single_biome_surface.json", std::ios::trunc);
    m << meta.dump(2) << "\n";
    p << preset.dump(2) << "\n";
    return m && p;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : ",") + x;
    return s;
}

const char* gameModeName(i64 m) {
    switch (m) {
    case 1: return "creative";
    case 2: return "adventure";
    case 3: return "spectator";
    default: return "survival";
    }
}

const char* difficultyName(i64 d) {
    switch (d) {
    case 0: return "peaceful";
    case 1: return "easy";
    case 3: return "hard";
    default: return "normal";
    }
}

// A server.properties value (java.util.Properties escaping, non-ASCII as \uXXXX).
std::string propertyValue(const std::string& v) {
    std::string out;
    for (size_t i = 0; i < v.size();) {
        u32 cp = u8(v[i]);
        const int n = cp < 0x80 ? 1 : cp < 0xE0 ? 2 : cp < 0xF0 ? 3 : 4;
        if (n > 1) {
            cp &= 0xFFu >> (n + 1);
            for (int k = 1; k < n && i + size_t(k) < v.size(); ++k) cp = (cp << 6) | (u8(v[i + size_t(k)]) & 0x3F);
        }
        i += size_t(n);
        if (cp == '\\' || cp == ':' || cp == '=' || cp == '#' || cp == '!') {
            out += '\\';
            out += char(cp);
        } else if (cp >= 0x20 && cp < 0x7F) {
            out += char(cp);
        } else {
            auto unit = [&](u32 u) {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04X", unsigned(u & 0xFFFF));
                out += b;
            };
            if (cp >= 0x10000) unit(0xD800 + ((cp - 0x10000) >> 10)), unit(0xDC00 + ((cp - 0x10000) & 0x3FF));
            else unit(cp);
        }
    }
    return out;
}

}  // namespace

LocalServer::~LocalServer() {
    stop();
    if (trainer_.joinable()) {  // an unfinished training run ends with the game (the next launch starts over)
        trainCancel_ = true;
        if (trainProcess_) TerminateProcess(trainProcess_, 1);
        trainer_.join();
    }
    if (trainJob_) CloseHandle(trainJob_);
}

// A server run on a throwaway world with -XX:AOTCacheOutput: the JDK records the classes it loads and links, and on
// exit assembles the ahead-of-time cache (JEP 483 / 514; about 40 s at below-normal priority). Later launches start
// with it (-XX:AOTCache), loading and linking those classes from the cache. One trainer at a time across instances.
void LocalServer::trainAot() {
    const Timer timer;
    std::error_code ec;
    const fs::path dir = aotCache_.parent_path(), work = dir / "train", tmp = aotCache_.wstring() + L".tmp";
    fs::create_directories(dir, ec);
    HANDLE lock = CreateFileW((dir / "train.lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return;  // another instance is training
    fs::remove_all(work, ec);
    fs::create_directories(work / "universe", ec);
    fs::remove(tmp, ec);
    std::ofstream(work / "eula.txt") << "eula=true\n";
    const u16 port = freePort();
    std::ofstream(work / "server.properties") << "online-mode=false\nserver-ip=127.0.0.1\nserver-port=" << port
                                              << "\nlevel-name=train\nspawn-protection=0\nmax-players=1\n";
    const fs::path java = findGameJava(config_.minecraftDir, config_.version);
    std::wstring cmd = L"\"" + java.wstring() + L"\" -Xms1G -Xmx4G -XX:AOTCacheOutput=\"" + tmp.wstring() + L"\" -cp \"" +
                       aotClasspath_ + L"\" " + aotMain_ + L" --nogui --universe \"" + (work / "universe").wstring() +
                       L"\" --world train --port " + std::to_wstring(port);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0)) return CloseHandle(lock), void();
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = si.hStdError = outWrite;
    PROCESS_INFORMATION pi{};
    const bool ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr, work.c_str(), &si, &pi);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (!ok) {
        CloseHandle(inWrite), CloseHandle(outRead), CloseHandle(lock);
        return;
    }
    trainJob_ = CreateJobObjectW(nullptr, nullptr);  // dies with the game (its cache assembler child too)
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(trainJob_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    AssignProcessToJobObject(trainJob_, pi.hProcess);
    trainProcess_ = pi.hProcess;
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    LOG("server: training the ahead-of-time class cache in the background (one time)");
    std::string pending;
    char buf[4096];
    DWORD got = 0;
    bool stopped = false;
    while (ReadFile(outRead, buf, sizeof(buf), &got, nullptr) && got) {
        pending.append(buf, got);
        if (!stopped && pending.find("Done (") != std::string::npos) {
            const char stop[] = "stop\n";
            DWORD put = 0;
            WriteFile(inWrite, stop, DWORD(sizeof(stop) - 1), &put, nullptr);
            stopped = true;
        }
        if (pending.size() > 8192) pending.erase(0, pending.size() - 64);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(inWrite), CloseHandle(outRead);
    if (!trainCancel_ && code == 0 && fs::exists(tmp, ec)) {
        fs::rename(tmp, aotCache_, ec);
        LOG("server: ahead-of-time class cache ready in %.1f s (%.0f MB): the next local servers start faster", timer.ms() / 1000.0,
            double(fs::file_size(aotCache_, ec)) / 1048576.0);
    } else {
        fs::remove(tmp, ec);
    }
    fs::remove_all(work, ec);
    CloseHandle(lock);
}

bool LocalServer::running() const { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }

bool LocalServer::ensureServerJar(fs::path& jar) {
    const nlohmann::json v = readJson(config_.minecraftDir / "versions" / config_.version / (config_.version + ".json"));
    if (!v.is_object() || !v.contains("downloads") || !v["downloads"].contains("server")) {
        LOG("error: the installed version has no server download listed");
        return false;
    }
    const std::string sha1 = v["downloads"]["server"].value("sha1", ""), url = v["downloads"]["server"].value("url", "");
    jar = runDir_ / "server.jar";
    std::error_code ec;
    if (fs::exists(jar, ec) && sha1File(jar) == sha1) return true;
    LOG("downloading the official server for %s (one time, %.0f MB)...", config_.version.c_str(),
        double(v["downloads"]["server"].value("size", 0)) / (1 << 20));
    const fs::path tmp = runDir_ / "server.jar.download";
    if (URLDownloadToFileW(nullptr, std::wstring(url.begin(), url.end()).c_str(), tmp.c_str(), 0, nullptr) != S_OK) {
        LOG("error: could not download %s", url.c_str());
        return false;
    }
    if (sha1File(tmp) != sha1) {
        LOG("error: the downloaded server does not match its published SHA-1");
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, jar, ec);
    return !ec;
}

bool LocalServer::backupIfUpgrading() {
    NbtValue root;
    if (!readNbtGz(config_.world / "level.dat", root)) {
        LOG("error: cannot read %s", (config_.world / "level.dat").string().c_str());
        return false;
    }
    NbtValue* data = root.find("Data");
    NbtValue* dv = data ? data->find("DataVersion") : nullptr;
    const int worldVersion = dv ? int(dv->i) : 0;
    const int gameVersion = gameWorldVersion(config_.minecraftDir / "versions" / config_.version / (config_.version + ".jar"));
    if (gameVersion && worldVersion > gameVersion) {
        LOG("error: this world was saved by a newer version of the game");
        return false;
    }
    if (!gameVersion || worldVersion >= gameVersion) return true;
    upgrading_ = true;
    // Like the game's "Backup and load": keep a copy of the world as it was before the upgrade.
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm);
    const fs::path dst = config_.minecraftDir / "backups" /
                         (config_.world.filename().string() + "_" + stamp + "_before_" + config_.version);
    Timer t;
    std::error_code ec;
    fs::create_directories(dst, ec);
    for (auto it = fs::recursive_directory_iterator(config_.world, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const fs::path rel = fs::relative(it->path(), config_.world, ec);
        if (rel == "session.lock") continue;
        if (it->is_directory(ec))
            fs::create_directories(dst / rel, ec);
        else
            fs::copy_file(it->path(), dst / rel, fs::copy_options::overwrite_existing, ec);
        if (ec) break;
    }
    if (ec) {
        LOG("error: backing up the world before upgrading it failed (%s); not starting", ec.message().c_str());
        return false;
    }
    LOG("backup  %8.1f ms  world (data version %d) copied to %s before the upgrade to %d", t.ms(), worldVersion,
        dst.string().c_str(), gameVersion);
    return true;
}

bool LocalServer::exportHostPlayer() {
    NbtValue root;
    if (!readNbtGz(config_.world / "level.dat", root)) return false;
    NbtValue* data = root.find("Data");
    if (!data) return false;
    NbtValue player;
    bool have = false;
    if (NbtValue* id = data->find("singleplayer_uuid"); id && id->type == TagIntArray) {
        hostUuid_ = uuidFromInts(id->ints);
        have = readNbtGz(config_.world / "players" / "data" / (hostUuid_ + ".dat"), player);
    } else if (NbtValue* p = data->find("Player")) {
        player = *p;
        if (NbtValue* id = p->find("UUID"); id && id->type == TagIntArray) hostUuid_ = uuidFromInts(id->ints);
        have = true;
    }
    if (!have) return true;  // nobody played this world in singleplayer: the server spawns a new player
    player.erase("UUID");    // the player keeps the offline id the server gives the name
    const bool modern = fs::exists(config_.world / "players");
    const fs::path file = modern ? config_.world / "players" / "data" / (offlineUuid_ + ".dat")
                                 : config_.world / "playerdata" / (offlineUuid_ + ".dat");
    if (!writeNbtGz(file, player)) {
        LOG("warning: could not hand the singleplayer inventory to the local server");
        return false;
    }
    return true;
}

void LocalServer::importHostPlayer() {
    if (hostUuid_.empty()) return;
    NbtValue player;
    if (!readNbtGz(config_.world / "players" / "data" / (offlineUuid_ + ".dat"), player) &&
        !readNbtGz(config_.world / "playerdata" / (offlineUuid_ + ".dat"), player))
        return;
    player.set("UUID", [&] {
        NbtValue v = NbtValue::make(TagIntArray);
        v.ints = intsFromUuid(hostUuid_);
        return v;
    }());
    const fs::path file = config_.world / "players" / "data" / (hostUuid_ + ".dat");
    std::error_code ec;
    if (fs::exists(file, ec)) fs::copy_file(file, file.string() + "_old", fs::copy_options::overwrite_existing, ec);
    if (!writeNbtGz(file, player)) {
        LOG("warning: could not write the singleplayer player data back");
        return;
    }
    // A dedicated server drops the singleplayer owner from level.dat; the game needs it to find the data.
    NbtValue root;
    std::string rootName;
    if (!readNbtGz(config_.world / "level.dat", root, &rootName)) return;
    if (NbtValue* data = root.find("Data")) {
        NbtValue id = NbtValue::make(TagIntArray);
        id.ints = intsFromUuid(hostUuid_);
        data->set("singleplayer_uuid", std::move(id));
        writeNbtGz(config_.world / "level.dat", root, rootName);
    }
}

void LocalServer::finishNewWorld() {
    NbtValue root;
    std::string rootName;
    if (!readNbtGz(config_.world / "level.dat", root, &rootName)) return;
    NbtValue* data = root.find("Data");
    if (!data) return;
    NbtValue name = NbtValue::make(TagString);
    name.s = config_.create->named ? config_.create->levelName : config_.world.filename().string();
    const std::string levelName = name.s;
    data->set("LevelName", std::move(name));
    NbtValue cheats = NbtValue::make(TagByte);
    cheats.i = config_.create->cheats ? 1 : 0;
    data->set("allowCommands", std::move(cheats));
    NbtValue mode = NbtValue::make(TagInt);  // the world's game mode (the server only gives it to players)
    const std::string& m = config_.create->gameMode;
    mode.i = m == "creative" ? 1 : m == "adventure" ? 2 : m == "spectator" ? 3 : 0;
    data->set("GameType", std::move(mode));
    if (!config_.create->singleBiome.empty()) {  // the preset override pack is not part of the world
        std::error_code ec;
        fs::remove_all(config_.world / "datapacks" / kBiomePack, ec);
        if (NbtValue* packs = data->find("DataPacks"))
            if (NbtValue* enabled = packs->find("Enabled"))
                std::erase_if(enabled->items, [](const NbtValue& v) { return v.s == std::string("file/") + kBiomePack; });
    }
    if (writeNbtGz(config_.world / "level.dat", root, rootName))
        LOG("server  new world \"%s\" set up for singleplayer (cheats %s)", levelName.c_str(),
            config_.create->cheats ? "on" : "off");
}

std::string LocalServer::upgradeStatus() const {
    std::lock_guard lock(statusMutex_);
    return upgradeStatus_;
}

bool LocalServer::start(const Config& config) {
    config_ = config;
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    runDir_ = fs::path(local ? local : L".") / "minecraftCplusplus" / "server" / config.version;
    std::error_code ec;
    fs::create_directories(runDir_, ec);
    const fs::path java = findGameJava(config.minecraftDir, config.version);
    fs::path jar;
    if (java.empty()) {
        LOG("error: the game's Java runtime was not found; launch the game once from the launcher");
        return false;
    }
    std::error_code existsEc;
    creating_ = config.create && !fs::exists(config.world / "level.dat", existsEc);
    if (!ensureServerJar(jar) || (!creating_ && !backupIfUpgrading())) return false;
    // Each world's server runs in its own folder (server.properties, ops.json, logs): two servers starting at once
    // (another world, a test) used to rewrite each other's settings in the shared one. The server jar unpacks its
    // libraries into the shared folder (bundlerRepoDir). Fabric servers keep their own folder (mods, cache).
    fs::path work = runDir_;
    std::wstring jvmExtra = L" -DbundlerRepoDir=\"" + runDir_.wstring() + L"\"";
    {
        std::error_code abs;
        const std::wstring key = fs::absolute(config.world, abs).wstring();
        char hash[17];
        std::snprintf(hash, sizeof(hash), "%08x", u32(std::hash<std::wstring>{}(key)));
        std::string name = config.world.filename().string();
        for (char& c : name)
            if (!std::isalnum(u8(c)) && c != '-' && c != '_') c = '_';
        work = runDir_ / "worlds" / (name.substr(0, 32) + "-" + hash);
        fs::create_directories(work, abs);
    }
    {  // The server's own logging (console + logs/latest.log), plus MinecraftServer's debug lines: "Autosave started" /
       // "Autosave finished" tell when it saves (MinecraftServer.isCurrentlySaving, the HUD's autosave indicator).
        std::ofstream log(work / "mcpp-log4j2.xml", std::ios::trunc);
        log << R"(<?xml version="1.0" encoding="UTF-8"?>
<Configuration status="WARN">
  <Appenders>
    <Console name="SysOut" target="SYSTEM_OUT"><PatternLayout pattern="[%d{HH:mm:ss}] [%t/%level]: %msg{nolookups}%n"/></Console>
    <RollingRandomAccessFile name="File" fileName="logs/latest.log" filePattern="logs/%d{yyyy-MM-dd}-%i.log.gz">
      <PatternLayout pattern="[%d{HH:mm:ss}] [%t/%level]: %msg{nolookups}%n"/>
      <Policies><TimeBasedTriggeringPolicy/><OnStartupTriggeringPolicy/></Policies>
      <DefaultRolloverStrategy max="1000"/>
    </RollingRandomAccessFile>
  </Appenders>
  <Loggers>
    <Logger name="net.minecraft.server.MinecraftServer" level="debug"/>
    <Root level="info">
      <filters><MarkerFilter marker="NETWORK_PACKETS" onMatch="DENY" onMismatch="NEUTRAL"/></filters>
      <AppenderRef ref="SysOut"/><AppenderRef ref="File"/>
    </Root>
  </Loggers>
</Configuration>
)";
        jvmExtra += L" -Dlog4j.configurationFile=\"" + (work / "mcpp-log4j2.xml").wstring() + L"\"";
    }
    // Fabric mods in the mods folder: Fabric's server runs them on top of this server.jar (mods/fabric_server.h).
    if (const auto fabric = mods::Manager::get().fabricMods(); !fabric.empty()) {
        if (!mods::prepareFabricServer(runDir_.parent_path(), config.minecraftDir, config.version, jar, fabric, runDir_, jar))
            return false;
        work = runDir_, jvmExtra.clear();
        if (mods::Manager::get().fabricContent())
            LOG("warning: some Fabric mods add blocks / items; they are drawn from the mods' own models where possible "
                "(see the Mods & Packs screen)");
    }
    offlineUuid_ = uuidString(offlineUuid(config.player));
    if (creating_) {
        // The owner's singleplayer player data is written once the server stops (importHostPlayer).
        std::string id;
        for (char c : config.ownerUuid)
            if (c != '-') id += char(std::tolower(u8(c)));
        if (id.size() == 32)
            hostUuid_ = id.substr(0, 8) + "-" + id.substr(8, 4) + "-" + id.substr(12, 4) + "-" + id.substr(16, 4) + "-" +
                        id.substr(20);
        fs::create_directories(config.world, existsEc);
        if (!config.create->singleBiome.empty() &&
            writeSingleBiomePack(config.world, config.minecraftDir / "versions" / config.version / (config.version + ".jar"),
                                 config.create->singleBiome)) {
            auto& packs = config_.create->enabledPacks;
            if (packs.empty()) packs.push_back("vanilla");
            packs.push_back(std::string("file/") + kBiomePack);
        }
    } else {
        exportHostPlayer();
    }

    NbtValue level;
    readNbtGz(config.world / "level.dat", level);
    NbtValue* data = level.find("Data");
    auto num = [&](const char* key, i64 fallback) {
        NbtValue* v = data ? data->find(key) : nullptr;
        return v ? v->i : fallback;
    };
    // 26.x keeps difficulty and hardcore in difficulty_settings; older saves at the top of Data.
    std::string difficulty = difficultyName(num("Difficulty", 2));
    bool hardcore = num("hardcore", 0) != 0;
    if (NbtValue* ds = data ? data->find("difficulty_settings") : nullptr) {
        if (NbtValue* d = ds->find("difficulty"); d && d->type == TagString && !d->s.empty()) difficulty = d->s;
        if (NbtValue* h = ds->find("hardcore")) hardcore = h->i != 0;
    }
    port_ = config.port ? config.port : freePort();
    {
        std::ofstream eula(work / "eula.txt", std::ios::trunc);
        eula << "# Accepted by the game's owner: https://aka.ms/MinecraftEULA\neula=true\n";
        std::ofstream p(work / "server.properties", std::ios::trunc);
        p << "# Written by minecraftCplusplus for singleplayer; rewritten on every launch.\n"
          << "online-mode=false\n"
          << "white-list=false\n"  // on by default since 26.1
          << "enforce-whitelist=false\n"
          << "enforce-secure-profile=false\n"
          << "server-ip=" << (config.lan ? "" : "127.0.0.1") << "\n"
          << "server-port=" << port_ << "\n"
          << "view-distance=" << std::clamp(config.viewDistance, 2, 32) << "\n"
          << "simulation-distance="
          << (config.simulationDistance > 0 ? std::clamp(config.simulationDistance, 2, 32) : std::clamp(config.viewDistance, 2, 10)) << "\n"
          << "entity-broadcast-range-percentage=" << std::clamp(int(std::lround(config.entityDistanceScaling * 100.0)), 10, 1000) << "\n"
          << "network-compression-threshold=" << (config.lan ? 256 : -1) << "\n"
          << "sync-chunk-writes=false\n"
          << "spawn-protection=0\n"
          << "allow-flight=true\n"
          << "enable-command-block=true\n"
          << "broadcast-console-to-ops=false\n"  // our console commands (pause: tick freeze) stay out of the chat
          << "max-players=8\n"
          << "gamemode=" << (creating_ ? config.create->gameMode : gameModeName(num("GameType", 0))) << "\n"
          << "difficulty=" << (creating_ ? config.create->difficulty : difficulty) << "\n"
          << "hardcore=" << ((creating_ ? config.create->hardcore : hardcore) ? "true" : "false") << "\n";
        if (creating_) {
            const Config::Create& c = *config_.create;
            p << "level-seed=" << propertyValue(c.seed) << "\n"
              << "generate-structures=" << (c.structures ? "true" : "false") << "\n";
            if (!c.levelType.empty()) p << "level-type=" << propertyValue(c.levelType) << "\n";
            if (!c.generatorSettings.empty()) p << "generator-settings=" << propertyValue(c.generatorSettings) << "\n";
            if (!c.enabledPacks.empty()) p << "initial-enabled-packs=" << propertyValue(joined(c.enabledPacks)) << "\n";
            if (!c.disabledPacks.empty()) p << "initial-disabled-packs=" << propertyValue(joined(c.disabledPacks)) << "\n";
        }
        p << "motd=" << config.world.filename().string() << " (minecraftCplusplus)\n";
        // Cheats on in the save: the player may run commands, as in singleplayer.
        std::ofstream ops(work / "ops.json", std::ios::trunc);
        if (num("allowCommands", 0) || (creating_ && config.create->cheats))
            ops << "[{\"uuid\":\"" << offlineUuid_ << "\",\"name\":\"" << config.player
                << "\",\"level\":4,\"bypassesPlayerLimit\":false}]\n";
        else
            ops << "[]\n";
    }

    // The server dies with this process (job object) even if we crash; normally stop() saves first.
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE stdinRead = nullptr, stdoutWrite = nullptr;
    if (!CreatePipe(&stdinRead, &stdinWrite_, &sa, 0) || !CreatePipe(&stdoutRead_, &stdoutWrite, &sa, 0)) return false;
    SetHandleInformation(stdinWrite_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdoutRead_, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdinRead;
    si.hStdOutput = si.hStdError = stdoutWrite;
    PROCESS_INFORMATION pi{};
    // Direct launch: the bundler's unpacked classpath and main class instead of -jar, so the JVM can use an
    // ahead-of-time class cache (classes behind the bundler's own class loader cannot be cached). The cache belongs to
    // this classpath and Java runtime; without one, a background run trains it once the server is up (trainAot).
    std::wstring launch = L" -jar \"" + jar.wstring() + L"\"";
    if (mods::Manager::get().fabricMods().empty()) {
        std::wstring mainClass;
        const std::wstring cp = directClasspath(jar, runDir_, mainClass);
        if (!cp.empty()) {
            launch = L" -cp \"" + cp + L"\" " + mainClass;
            std::error_code jec;
            const auto jsize = fs::file_size(java, jec);
            const auto jtime = fs::last_write_time(java, jec).time_since_epoch().count();
            char key[17];
            std::snprintf(key, sizeof(key), "%016llx",
                          static_cast<unsigned long long>(std::hash<std::wstring>{}(cp + java.wstring() + std::to_wstring(jsize) +
                                                                                     std::to_wstring(jtime))));
            aotCache_ = runDir_ / "aot" / ("server-" + config.version + "-" + key + ".aot");
            if (fs::exists(aotCache_, jec)) jvmExtra += L" -XX:AOTCache=\"" + aotCache_.wstring() + L"\"";
            else aotTrain_ = true, aotClasspath_ = cp, aotMain_ = mainClass;
        }
    }
    std::wstring cmd = L"\"" + java.wstring() + L"\" -Xms1G -Xmx4G" + jvmExtra + launch +
                       L" --nogui --universe \"" + config.world.parent_path().wstring() + L"\" --world \"" +
                       config.world.filename().wstring() + L"\" --port " + std::to_wstring(port_);
    if (creating_ && config.create->bonusChest) cmd += L" --bonusChest";
    if (config.forceUpgrade) cmd += config.eraseCache ? L" --forceUpgrade --eraseCache" : L" --forceUpgrade";
    if (config.safeMode) cmd += L" --safeMode";
    // Above normal priority while it starts (it competes with the game's own loading), normal once it is up.
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_SUSPENDED | ABOVE_NORMAL_PRIORITY_CLASS,
                                   nullptr, work.c_str(), &si, &pi);
    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);
    if (!ok) {
        LOG("error: could not start the local server");
        return false;
    }
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    reader_ = std::thread([this] { readOutput(); });
    LOG("server  starting the local server for %s on port %u%s", config.world.filename().string().c_str(), port_,
        config.lan ? " (open to the network)" : "");
    return true;
}

std::vector<LocalServer::StorageEvent> LocalServer::takeStorageEvents() {
    std::lock_guard lock(storageMutex_);
    return std::exchange(storageEvents_, {});
}

void LocalServer::readOutput() {
    std::string pending;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(stdoutRead_, buf, sizeof(buf), &got, nullptr) && got) {
        pending.append(buf, got);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // MinecraftServer.saveEverything: autosaves (debug lines) and save-all.
            if (line.find("]: Autosave started") != std::string::npos || line.find("]: Saving the game") != std::string::npos)
                saving_ = true;
            else if (line.find("]: Autosave finished") != std::string::npos || line.find("]: Saved the game") != std::string::npos)
                saving_ = false;
            for (const auto& [text, kind] : {std::pair{"]: Failed to load chunk ", StorageEvent::LoadFailure},
                                             std::pair{"]: Failed to save chunk ", StorageEvent::SaveFailure}}) {
                const size_t at = line.find(text);
                int x = 0, z = 0;
                if (at != std::string::npos && std::sscanf(line.c_str() + at + std::strlen(text), "%d,%d", &x, &z) == 2) {
                    std::lock_guard lock(storageMutex_);
                    storageEvents_.push_back({kind, x, z});
                }
            }
            if (line.find("]: Low disk space! Might not be able to save the world.") != std::string::npos) {
                std::lock_guard lock(storageMutex_);
                storageEvents_.push_back({StorageEvent::LowDiskSpace});
            }
            if (line.find("Failed to load datapacks") != std::string::npos ||  // Main: the server gives up
                line.find("Failed to load level data or datapacks") != std::string::npos) {
                datapackFailed_ = true;
                LOG("server: %s", line.c_str());
            }
            if (line.find("Done (") != std::string::npos) {
                if (creating_)  // Create World's game rules, before anyone joins (the console runs them next tick)
                    for (const auto& [rule, value] : config_.create->gameRules) command("gamerule " + rule + " " + value);
                ready_ = true;
                SetPriorityClass(process_, NORMAL_PRIORITY_CLASS);
                LOG("server: %s", line.c_str());
                if (aotTrain_.exchange(false)) trainer_ = std::thread([this] { trainAot(); });
            } else if (config_.forceUpgrade && upgradePercent_ < 100 &&
                       (line.find("Forcing world upgrade") != std::string::npos || line.find("% completed (") != std::string::npos ||
                        line.find("[main/INFO]") != std::string::npos || line.find("[Server Main/INFO]") != std::string::npos)) {
                // Optimize World: WorldUpgrader's status lines and "<n>% completed (<done> / <total> chunks)...".
                const size_t s = line.find("]: ");
                const std::string msg = s == std::string::npos ? line : line.substr(s + 3);
                if (const size_t pc = msg.find("% completed ("); pc != std::string::npos) {
                    upgradePercent_ = std::atoi(msg.c_str());
                    int done = 0, total = 0;  // "(<done> / <total> chunks)"
                    if (std::sscanf(msg.c_str() + pc, "%% completed (%d / %d", &done, &total) == 2) upgradeDone_ = done, upgradeTotal_ = total;
                } else if (msg.starts_with("Starting minecraft server") || msg.starts_with("Loading properties")) {
                    if (upgradePercent_ >= 0 || msg.starts_with("Starting")) upgradePercent_ = 100;
                } else if (!msg.starts_with("Forcing")) {
                    std::lock_guard lock(statusMutex_);
                    upgradeStatus_ = msg;
                }
                LOG("server: %s", line.c_str());
            } else if (const size_t j = line.find(" joined the game"); j != std::string::npos) {
                const size_t s = line.rfind("]: ", j);
                const std::string name = s == std::string::npos ? std::string() : line.substr(s + 3, j - s - 3);
                if (opJoiners_ && !name.empty() && name.find(' ') == std::string::npos) command("op " + name);
                if (forceGameMode_ && !name.empty() && name.find(' ') == std::string::npos && name != config_.player) {
                    std::lock_guard lock(forceMutex_);
                    command("gamemode " + forcedMode_ + " " + name);  // force-gamemode for the joining guest
                }
                LOG("server: %s", line.c_str());
            } else if (line.find("The game is frozen") != std::string::npos ||
                       line.find("The game is running normally") != std::string::npos) {
                LOG("server: %s", line.c_str());  // singleplayer pause (tick freeze / unfreeze)
            } else if (line.find("moved too quickly") != std::string::npos || line.find("moved wrongly") != std::string::npos) {
                LOG("server: %s", line.c_str());  // movement corrections (prediction mismatches)
            } else if ((line.find("ERROR") != std::string::npos || line.find("Exception") != std::string::npos ||
                        line.find("FAILED") != std::string::npos) &&
                       line.find("OFFLINE") == std::string::npos) {
                LOG("server: %s", line.c_str());
                if (line.find("FAILED TO BIND") != std::string::npos) failed_ = true;
            } else if (line.find("Preparing level") != std::string::npos || line.find("Upgrading") != std::string::npos ||
                       line.find("Preparing spawn area") != std::string::npos ||
                       line.find("Stopping server") != std::string::npos ||
                       (line.find("Loading ") != std::string::npos && line.find(" mods:") != std::string::npos) ||
                       line.starts_with("\t- ")) {  // Fabric's mod list
                LOG("server: %s", line.c_str());
            }
        }
    }
}

void LocalServer::setAllowCommands(bool on) {
    allowCommands_ = on ? 1 : 0;
    command((on ? "op " : "deop ") + config_.player);
}

void LocalServer::setForceGameMode(bool on, const std::string& mode) {
    std::lock_guard lock(forceMutex_);
    forceGameMode_ = on, forcedMode_ = mode;
}

void LocalServer::writeWorldOptions() {  // level.dat: allowCommands / DifficultyLocked changed in World Options
    if (allowCommands_ < 0 && !lockDifficulty_) return;
    NbtValue root;
    std::string rootName;
    if (!readNbtGz(config_.world / "level.dat", root, &rootName)) return;
    NbtValue* data = root.find("Data");
    if (!data) return;
    if (allowCommands_ >= 0) {
        NbtValue cheats = NbtValue::make(TagByte);
        cheats.i = allowCommands_;
        data->set("allowCommands", std::move(cheats));
    }
    if (lockDifficulty_) {
        NbtValue* ds = data->find("difficulty_settings");
        NbtValue locked = NbtValue::make(TagByte);
        locked.i = 1;
        if (ds) ds->set("locked", std::move(locked));
        else data->set("DifficultyLocked", std::move(locked));
    }
    writeNbtGz(config_.world / "level.dat", root, rootName);
}

void LocalServer::command(const std::string& line) {
    std::lock_guard lock(stdinMutex_);
    if (!stdinWrite_) return;
    const std::string s = line + "\n";
    DWORD written = 0;
    WriteFile(stdinWrite_, s.data(), DWORD(s.size()), &written, nullptr);
}

void LocalServer::stop(double timeoutSeconds) {
    if (stopped_ || !process_) return;
    stopped_ = true;
    Timer t;
    if (running()) {
        command("stop");
        if (WaitForSingleObject(process_, DWORD(timeoutSeconds * 1000)) != WAIT_OBJECT_0) {
            LOG("warning: the local server did not stop in time; terminating it");
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 5000);
        }
    }
    {
        std::lock_guard lock(stdinMutex_);
        CloseHandle(stdinWrite_);
        stdinWrite_ = nullptr;
    }
    if (reader_.joinable()) reader_.join();
    CloseHandle(stdoutRead_);
    CloseHandle(process_);
    if (job_) CloseHandle(job_);
    process_ = job_ = stdoutRead_ = nullptr;
    importHostPlayer();
    if (creating_) finishNewWorld();
    writeWorldOptions();
    LOG("server  %8.1f ms  local server saved and stopped", t.ms());
}
