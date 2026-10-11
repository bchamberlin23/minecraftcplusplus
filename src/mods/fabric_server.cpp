#include "mods/fabric_server.h"

#include "assets/game_reports.h"
#include "assets/zip.h"

#include <json/json.hpp>

#include <windows.h>
#include <bcrypt.h>
#include <urlmon.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

#pragma comment(lib, "urlmon.lib")

namespace mods {

namespace {

std::string sha1Hex(const std::vector<u8>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return {};
    u8 digest[20];
    const bool ok = BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(data.data()), ULONG(data.size()), digest, 20) == 0;
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};
    static const char* hexDigits = "0123456789abcdef";
    std::string s;
    for (u8 b : digest) s += hexDigits[b >> 4], s += hexDigits[b & 15];
    return s;
}

bool download(const std::string& url, const fs::path& to) {
    std::error_code ec;
    fs::remove(to, ec);
    return URLDownloadToFileW(nullptr, std::wstring(url.begin(), url.end()).c_str(), to.c_str(), 0, nullptr) == S_OK;
}

nlohmann::json downloadJson(const std::string& url, const fs::path& tmp) {
    std::vector<u8> bytes;
    if (!download(url, tmp) || !readFile(tmp, bytes)) return nlohmann::json();
    std::error_code ec;
    fs::remove(tmp, ec);
    return nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
}

// The meta server builds the launcher from the installer jar plus install.properties: every other entry must be
// the installer's own (same CRC-32 and size), and the installer must match the SHA-1 published on Fabric's maven.
bool verifyLauncher(const fs::path& launcher, const fs::path& installer, const std::string& loader,
                    const std::string& game) {
    ZipArchive l, i;
    if (!l.open(launcher) || !i.open(installer)) return false;
    std::map<std::string, std::pair<u32, u32>> known;
    for (const auto& e : i.entries()) known[e.name] = {e.crc, e.size};
    bool props = false, manifest = false;
    std::vector<u8> bytes;
    for (const auto& e : l.entries()) {
        if (e.name == "install.properties") {
            if (!l.read(e, bytes)) return false;
            const std::string text(bytes.begin(), bytes.end());
            props = text.find("fabric-loader-version=" + loader) != std::string::npos &&
                    text.find("game-version=" + game) != std::string::npos;
        } else if (e.name == "META-INF/MANIFEST.MF") {
            if (!l.read(e, bytes)) return false;
            manifest = std::string(bytes.begin(), bytes.end()).find("Main-Class: net.fabricmc.installer.ServerLauncher") !=
                       std::string::npos;
        } else if (e.name.back() != '/') {
            const auto it = known.find(e.name);
            if (it == known.end() || it->second != std::make_pair(e.crc, e.size)) {
                LOG("fabric: the server launcher's %s does not match the published installer", e.name.c_str());
                return false;
            }
        }
    }
    return props && manifest;
}

// Fabric's server launcher for this game version, downloaded and verified once, then reused offline.
fs::path fabricLauncher(const fs::path& cacheDir, const std::string& game) {
    std::error_code ec;
    fs::create_directories(cacheDir, ec);
    const std::string prefix = "fabric-server-mc." + game + "-loader.";
    fs::path cached;
    fs::file_time_type newest{};
    for (const auto& d : fs::directory_iterator(cacheDir, ec)) {
        const std::string n = d.path().filename().string();
        if (n.starts_with(prefix) && n.ends_with(".jar") && (cached.empty() || d.last_write_time(ec) > newest))
            cached = d.path(), newest = d.last_write_time(ec);
    }
    if (!cached.empty()) return cached;

    LOG("fabric: downloading Fabric's server launcher for %s (one time)...", game.c_str());
    const fs::path tmp = cacheDir / "meta.json.download";
    const nlohmann::json loaders = downloadJson("https://meta.fabricmc.net/v2/versions/loader/" + game, tmp);
    const nlohmann::json installers = downloadJson("https://meta.fabricmc.net/v2/versions/installer", tmp);
    std::string loader, installer, installerUrl;
    if (loaders.is_array())
        for (const auto& l : loaders)
            if (l.contains("loader") && l["loader"].value("stable", false)) {
                loader = l["loader"].value("version", "");
                break;
            }
    if (installers.is_array())
        for (const auto& i : installers)
            if (i.value("stable", false)) {
                installer = i.value("version", ""), installerUrl = i.value("url", "");
                break;
            }
    if (loader.empty() || installer.empty() || !installerUrl.starts_with("https://maven.fabricmc.net/")) {
        LOG("error: Fabric has no loader for Minecraft %s yet (meta.fabricmc.net)", game.c_str());
        return {};
    }
    const fs::path installerJar = cacheDir / ("fabric-installer-" + installer + ".jar.download");
    const fs::path sha1File = cacheDir / "installer.sha1.download";
    std::vector<u8> jarBytes, sha1Bytes;
    if (!download(installerUrl, installerJar) || !download(installerUrl + ".sha1", sha1File) ||
        !readFile(installerJar, jarBytes) || !readFile(sha1File, sha1Bytes)) {
        LOG("error: could not download the Fabric installer %s", installerUrl.c_str());
        return {};
    }
    std::string published(sha1Bytes.begin(), sha1Bytes.end());
    published = published.substr(0, published.find_first_of(" \r\n"));
    fs::remove(sha1File, ec);
    if (sha1Hex(jarBytes) != published) {
        LOG("error: the Fabric installer does not match its published SHA-1");
        fs::remove(installerJar, ec);
        return {};
    }
    const std::string name = prefix + loader + "-launcher." + installer + ".jar";
    const fs::path launcherTmp = cacheDir / (name + ".download");
    const std::string url =
        "https://meta.fabricmc.net/v2/versions/loader/" + game + "/" + loader + "/" + installer + "/server/jar";
    const bool ok = download(url, launcherTmp) && verifyLauncher(launcherTmp, installerJar, loader, game);
    fs::remove(installerJar, ec);
    if (!ok) {
        LOG("error: the Fabric server launcher could not be downloaded or verified (%s)", url.c_str());
        fs::remove(launcherTmp, ec);
        return {};
    }
    fs::rename(launcherTmp, cacheDir / name, ec);
    LOG("fabric: Fabric loader %s, launcher %s verified", loader.c_str(), installer.c_str());
    return ec ? fs::path() : cacheDir / name;
}

// The helper mod: at server start (after every mod registered its content) it writes each block's states in the
// server's raw id order and Block.BLOCK_STATE_REGISTRY, so modded blocks in chunks map to state keys. Pure
// reflection on the unobfuscated game: compiled once by the game's own Java runtime, no other dependency.
constexpr const char* kHelperVersion = "1";
constexpr const char* kHelperSource = R"java(package mcpp;

import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

/** minecraftCplusplus: writes the server's block state table (mcpp-registries.json) for its client. */
public final class RegistryDump {
    private RegistryDump() {}

    private static String q(String s) {
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            if (c == '"' || c == '\\') b.append('\\');
            if (c >= ' ') b.append(c);
        }
        return b.append('"').toString();
    }

    public static void dump() {
        try {
            ClassLoader cl = RegistryDump.class.getClassLoader();
            Class<?> idMap = Class.forName("net.minecraft.core.IdMap", true, cl);
            Method byId = idMap.getMethod("byId", int.class), size = idMap.getMethod("size");
            Method getKey = Class.forName("net.minecraft.core.Registry", true, cl).getMethod("getKey", Object.class);
            Object blocks = Class.forName("net.minecraft.core.registries.BuiltInRegistries", true, cl).getField("BLOCK").get(null);
            StringBuilder sb = new StringBuilder(1 << 21).append("{\"blocks\":[");
            int n = (Integer) size.invoke(blocks);
            for (int i = 0; i < n; i++) {
                Object b = byId.invoke(blocks, i);
                Object def = b.getClass().getMethod("getStateDefinition").invoke(b);
                List<?> states = (List<?>) def.getClass().getMethod("getPossibleStates").invoke(def);
                sb.append(i > 0 ? "," : "").append("{\"id\":").append(q(String.valueOf(getKey.invoke(blocks, b)))).append(",\"states\":[");
                for (int k = 0; k < states.size(); k++) sb.append(k > 0 ? "," : "").append(q(String.valueOf(states.get(k))));
                sb.append("]}");
            }
            Object table = Class.forName("net.minecraft.world.level.block.Block", true, cl).getField("BLOCK_STATE_REGISTRY").get(null);
            sb.append("],\"table\":[");
            int m = (Integer) size.invoke(table);
            for (int i = 0; i < m; i++) sb.append(i > 0 ? "," : "").append(q(String.valueOf(byId.invoke(table, i))));
            sb.append("]}");
            Files.writeString(Path.of("mcpp-registries.json"), sb, StandardCharsets.UTF_8);
            System.out.println("[minecraftCplusplus] block table written: " + n + " blocks, " + m + " states");
        } catch (Throwable t) {
            System.out.println("[minecraftCplusplus] block table failed: " + t);
        }
    }
}
)java";
constexpr const char* kHelperModJson =
    R"({"schemaVersion":1,"id":"mcpp_registry_dump","version":"1.0.0","name":"minecraftCplusplus block table",)"
    R"("description":"Writes the server's block state ids for the minecraftCplusplus client.","environment":"server",)"
    R"("entrypoints":{"server":["mcpp.RegistryDump::dump"]}})";
// Single-file program (java Build.java <dir> <jar>): compiles the helper with the runtime's compiler and packs it.
constexpr const char* kHelperBuild = R"java(import java.nio.file.*;
import java.util.jar.*;
import javax.tools.*;

public class McppHelperBuild {
    public static void main(String[] a) throws Exception {
        Path dir = Path.of(a[0]), classes = dir.resolve("classes");
        Files.createDirectories(classes);
        JavaCompiler c = ToolProvider.getSystemJavaCompiler();
        if (c == null || c.run(null, null, null, "-d", classes.toString(), dir.resolve("RegistryDump.java").toString()) != 0)
            System.exit(2);
        Manifest mf = new Manifest();
        mf.getMainAttributes().put(Attributes.Name.MANIFEST_VERSION, "1.0");
        try (JarOutputStream j = new JarOutputStream(Files.newOutputStream(Path.of(a[1])), mf)) {
            j.putNextEntry(new JarEntry("fabric.mod.json"));
            j.write(Files.readAllBytes(dir.resolve("fabric.mod.json")));
            j.closeEntry();
            j.putNextEntry(new JarEntry("mcpp/RegistryDump.class"));
            j.write(Files.readAllBytes(classes.resolve("mcpp/RegistryDump.class")));
            j.closeEntry();
        }
    }
}
)java";

fs::path helperMod(const fs::path& cacheDir, const fs::path& minecraftDir, const std::string& version) {
    const fs::path jar = cacheDir / (std::string("mcpp-registry-dump-") + kHelperVersion + ".jar");
    std::error_code ec;
    if (fs::exists(jar, ec)) return jar;
    const fs::path work = cacheDir / "helper-build";
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    {
        std::ofstream(work / "RegistryDump.java", std::ios::binary) << kHelperSource;
        std::ofstream(work / "fabric.mod.json", std::ios::binary) << kHelperModJson;
    }
    const fs::path tmp = cacheDir / "mcpp-registry-dump.jar.tmp";
    const bool ok = runGameProgram(minecraftDir, version, "McppHelperBuild", kHelperBuild, {work.wstring(), tmp.wstring()},
                                   work, 120000);
    fs::remove_all(work, ec);
    if (!ok || !fs::exists(tmp, ec)) {
        LOG("warning: could not build the helper for Fabric content mods; their blocks will show as air");
        return {};
    }
    fs::rename(tmp, jar, ec);
    return ec ? fs::path() : jar;
}

bool sameFile(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::exists(b, ec) && fs::file_size(a, ec) == fs::file_size(b, ec) &&
           fs::last_write_time(a, ec) == fs::last_write_time(b, ec);
}

}  // namespace

bool prepareFabricServer(const fs::path& serverRoot, const fs::path& minecraftDir, const std::string& version,
                         const fs::path& vanillaJar, const std::vector<fs::path>& modJars, fs::path& runDir,
                         fs::path& launcherJar) {
    const fs::path launcher = fabricLauncher(serverRoot / "fabric", version);
    if (launcher.empty()) return false;
    const fs::path serverJar = vanillaJar;
    runDir = serverRoot / (version + "-fabric");
    std::error_code ec;
    fs::create_directories(runDir / "mods", ec);
    fs::remove(runDir / "mcpp-registries.json", ec);  // written again by the helper at this start
    std::vector<fs::path> mods = modJars;
    if (const fs::path helper = helperMod(serverRoot / "fabric", minecraftDir, version); !helper.empty())
        mods.push_back(helper);
    // Exactly the enabled mods (the run directory is ours; the player's files are only read).
    std::set<std::wstring> wanted;
    for (const fs::path& m : mods) {
        const fs::path dst = runDir / "mods" / m.filename();
        wanted.insert(m.filename().wstring());
        if (sameFile(m, dst)) continue;
        fs::copy_file(m, dst, fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::last_write_time(dst, fs::last_write_time(m, ec), ec);
        if (ec) {
            LOG("error: could not copy %s for the Fabric server: %s", m.filename().string().c_str(), ec.message().c_str());
            return false;
        }
    }
    for (const auto& d : fs::directory_iterator(runDir / "mods", ec))
        if (d.is_regular_file(ec) && d.path().extension() == ".jar" && !wanted.count(d.path().filename().wstring()))
            fs::remove(d.path(), ec);
    // The launcher runs the official server.jar we already verified instead of downloading its own.
    {
        std::ofstream p(runDir / "fabric-server-launcher.properties", std::ios::trunc);
        p << "# Written by minecraftCplusplus\nserverJar=" << serverJar.generic_string() << "\n";
    }
    launcherJar = launcher;
    LOG("fabric: running Fabric's server with %zu mod%s (%s)", modJars.size(), modJars.size() == 1 ? "" : "s",
        runDir.string().c_str());
    return true;
}

}  // namespace mods
