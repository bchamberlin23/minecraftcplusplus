#include "assets/game_reports.h"

#include <windows.h>

#include <algorithm>
#include <fstream>

namespace {

nlohmann::json readJson(const fs::path& p) {
    std::vector<u8> bytes;
    if (!readFile(p, bytes)) return nullptr;
    return nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
}

// The Java runtime the launcher installed for this version (component name from the version JSON).
fs::path findJava(const fs::path& minecraftDir, const std::string& component) {
    std::vector<fs::path> roots = {minecraftDir / "runtime"};
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"))
        roots.push_back(fs::path(local) / "Packages" / "Microsoft.4297127D64EC6_8wekyb3d8bbwe" / "LocalCache" / "Local" /
                        "runtime");
    if (const wchar_t* pf = _wgetenv(L"ProgramFiles(x86)")) roots.push_back(fs::path(pf) / "Minecraft Launcher" / "runtime");
    std::error_code ec;
    for (const auto& root : roots) {
        const fs::path java = root / component / "windows-x64" / component / "bin" / "java.exe";
        if (fs::exists(java, ec)) return java;
    }
    return {};
}

bool runHidden(const std::wstring& commandLine, const fs::path& workDir, DWORD timeoutMs) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = commandLine;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, workDir.c_str(), &si,
                        &pi))
        return false;
    const bool finished = WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0;
    DWORD code = 1;
    if (finished) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return finished && code == 0;
}

}  // namespace

fs::path findGameJava(const fs::path& minecraftDir, const std::string& version) {
    const nlohmann::json v = readJson(minecraftDir / "versions" / version / (version + ".json"));
    if (!v.is_object()) return {};
    return findJava(minecraftDir, v.contains("javaVersion") ? v["javaVersion"].value("component", "") : "");
}

namespace {
// The client jar plus every library the version needs on Windows.
std::wstring gameClassPath(const fs::path& minecraftDir, const std::string& version, const nlohmann::json& v) {
    const fs::path versionDir = minecraftDir / "versions" / version;
    std::wstring cp = (versionDir / (version + ".jar")).wstring();
    for (const auto& lib : v.value("libraries", nlohmann::json::array())) {
        bool allowed = true;
        if (lib.contains("rules")) {
            allowed = false;
            for (const auto& rule : lib["rules"]) {
                const bool matches = !rule.contains("os") || rule["os"].value("name", "") == "windows";
                if (matches) allowed = rule.value("action", "") == "allow";
            }
        }
        if (!allowed || !lib.contains("downloads") || !lib["downloads"].contains("artifact")) continue;
        cp += L";" + (minecraftDir / "libraries" / lib["downloads"]["artifact"].value("path", "")).wstring();
    }
    return cp;
}
}  // namespace

bool runGameProgram(const fs::path& minecraftDir, const std::string& version, const std::string& className,
                    const std::string& source, const std::vector<std::wstring>& args, const fs::path& workDir,
                    unsigned timeoutMs) {
    const nlohmann::json v = readJson(minecraftDir / "versions" / version / (version + ".json"));
    if (!v.is_object()) return false;
    const fs::path java = findJava(minecraftDir, v.contains("javaVersion") ? v["javaVersion"].value("component", "") : "");
    if (java.empty()) return false;
    std::error_code ec;
    fs::create_directories(workDir, ec);
    const fs::path file = workDir / (className + ".java");
    {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f << source;
        if (!f) return false;
    }
    std::wstring cmd = L"\"" + java.wstring() + L"\" -cp \"" + gameClassPath(minecraftDir, version, v) + L"\" \"" +
                       file.wstring() + L"\"";
    for (const auto& a : args) cmd += L" \"" + a + L"\"";
    return runHidden(cmd, workDir, timeoutMs);
}

bool GameReports::generate(const fs::path& minecraftDir, const std::string& version, const fs::path& out) {
    const fs::path versionDir = minecraftDir / "versions" / version;
    const nlohmann::json v = readJson(versionDir / (version + ".json"));
    if (!v.is_object()) return false;
    const std::string component = v.contains("javaVersion") ? v["javaVersion"].value("component", "") : "";
    const fs::path java = findJava(minecraftDir, component);
    if (java.empty()) {
        LOG("error: the game's Java runtime (%s) was not found; launch the game once from the launcher", component.c_str());
        return false;
    }
    // Classpath: the client jar plus every library the version needs on Windows.
    std::wstring cp = (versionDir / (version + ".jar")).wstring();
    for (const auto& lib : v.value("libraries", nlohmann::json::array())) {
        bool allowed = true;
        if (lib.contains("rules")) {
            allowed = false;
            for (const auto& rule : lib["rules"]) {
                const bool matches = !rule.contains("os") || rule["os"].value("name", "") == "windows";
                if (matches) allowed = rule.value("action", "") == "allow";
            }
        }
        if (!allowed || !lib.contains("downloads") || !lib["downloads"].contains("artifact")) continue;
        cp += L";" + (minecraftDir / "libraries" / lib["downloads"]["artifact"].value("path", "")).wstring();
    }
    std::error_code ec;
    fs::create_directories(out, ec);
    LOG("generating block definitions with the game's data generator (one time, ~10 s)...");
    const std::wstring cmd = L"\"" + java.wstring() + L"\" -cp \"" + cp + L"\" net.minecraft.data.Main --reports --output \"" +
                             out.wstring() + L"\"";
    return runHidden(cmd, out, 180000);
}

bool GameReports::load(const fs::path& minecraftDir, const std::string& version, const fs::path& cacheDir) {
    const fs::path out = cacheDir / version;
    const fs::path blocksFile = out / "reports" / "blocks.json";
    std::error_code ec;
    if (!fs::exists(blocksFile, ec) && !generate(minecraftDir, version, out)) return false;

    const nlohmann::json blocks = readJson(blocksFile);
    if (!blocks.is_object()) return false;
    for (const auto& item : blocks.items()) {
        Block b;
        for (const auto& state : item.value().value("states", nlohmann::json::array())) {
            std::vector<std::pair<std::string, std::string>> props;
            if (state.contains("properties"))
                for (const auto& p : state["properties"].items()) props.push_back({p.key(), p.value().get<std::string>()});
            std::sort(props.begin(), props.end());
            std::string key = item.key();
            if (!props.empty()) {
                key += '[';
                for (size_t i = 0; i < props.size(); ++i) key += (i ? "," : "") + props[i].first + "=" + props[i].second;
                key += ']';
            }
            stateIds_[key] = state.value("id", -1);
            if (state.value("default", false)) b.defaults = props;
        }
        blocks_.emplace(item.key(), std::move(b));
    }
    registries_ = readJson(out / "reports" / "registries.json");
    packets_ = readJson(out / "reports" / "packets.json");
    return !blocks_.empty();
}

std::string GameReports::complete(std::string_view key) const {
    const size_t b = key.find('[');
    const std::string name(key.substr(0, b));
    auto it = blocks_.find(name);
    if (it == blocks_.end() || it->second.defaults.empty()) return std::string(key);
    std::vector<std::pair<std::string, std::string>> props = it->second.defaults;
    if (b != std::string_view::npos) {
        std::string_view rest = key.substr(b + 1, key.size() - b - 2);
        while (!rest.empty()) {
            const size_t comma = rest.find(',');
            const std::string_view kv = rest.substr(0, comma);
            const size_t eq = kv.find('=');
            if (eq != std::string_view::npos) {
                const std::string k(kv.substr(0, eq)), v(kv.substr(eq + 1));
                auto p = std::find_if(props.begin(), props.end(), [&](const auto& e) { return e.first == k; });
                if (p != props.end())
                    p->second = v;
                else
                    props.push_back({k, v});  // a property this version does not know: keep it
            }
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
        std::sort(props.begin(), props.end());
    }
    std::string out = name + "[";
    for (size_t i = 0; i < props.size(); ++i) out += (i ? "," : "") + props[i].first + "=" + props[i].second;
    return out + "]";
}

int GameReports::stateId(std::string_view key) const {
    auto it = stateIds_.find(std::string(key));
    return it == stateIds_.end() ? -1 : it->second;
}
