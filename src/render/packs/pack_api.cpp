#include "render/packs/pack_api.h"

#include "render/packs/pack_pipeline.h"

#include <algorithm>
#include <fstream>

Renderer::Packs& Renderer::packs() {
    if (!packs_) packs_ = std::make_unique<Packs>(*this);
    return *packs_;
}

namespace packs {

namespace {
fs::path appData() {
    const wchar_t* a = _wgetenv(L"APPDATA");
    return a ? fs::path(a) : fs::path(L".");
}
bool isPack(const fs::directory_entry& e) {
    std::error_code ec;
    if (e.is_directory(ec)) {
        if (fs::is_directory(e.path() / "shaders", ec)) return true;
        for (const auto& sub : fs::directory_iterator(e.path(), ec))
            if (sub.is_directory(ec) && fs::is_directory(sub.path() / "shaders", ec)) return true;
        return false;
    }
    std::string ext = e.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(std::tolower(u8(c))); });
    return ext == ".zip";
}
std::unordered_map<std::string, std::string> readSettings(const fs::path& file) {
    std::unordered_map<std::string, std::string> out;
    std::vector<u8> bytes;
    if (!readFile(file, bytes)) return out;
    std::string text(bytes.begin(), bytes.end());
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i);
        i = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}
}  // namespace

fs::path packDirectory() {
    fs::path d = appData() / "minecraftCplusplus" / "shaderpacks";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

fs::path externalPackDirectory() { return appData() / ".minecraft" / "shaderpacks"; }

std::vector<PackInfo> listPacks() {
    std::vector<PackInfo> out;
    std::error_code ec;
    for (int pass = 0; pass < 2; ++pass) {
        const fs::path dir = pass == 0 ? packDirectory() : externalPackDirectory();
        std::vector<PackInfo> found;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!isPack(e)) continue;
            PackInfo p;
            p.name = e.path().filename().string();
            p.path = e.path();
            p.external = pass == 1;
            if (std::any_of(out.begin(), out.end(), [&](const PackInfo& o) { return o.name == p.name; })) continue;
            found.push_back(std::move(p));
        }
        std::sort(found.begin(), found.end(), [](const PackInfo& a, const PackInfo& b) { return a.name < b.name; });
        out.insert(out.end(), found.begin(), found.end());
    }
    return out;
}

bool findPack(const std::string& nameOrPath, PackInfo& out) {
    std::error_code ec;
    const fs::path asPath = fs::u8path(nameOrPath);
    if (fs::exists(asPath, ec) && (asPath.has_parent_path())) {
        out.name = asPath.filename().string();
        out.path = asPath;
        out.external = false;
        return true;
    }
    std::string want = nameOrPath;
    std::transform(want.begin(), want.end(), want.begin(), [](char c) { return char(std::tolower(u8(c))); });
    const auto packs = listPacks();
    for (int exact = 1; exact >= 0; --exact)
        for (const PackInfo& p : packs) {
            std::string n = p.name;
            std::transform(n.begin(), n.end(), n.begin(), [](char c) { return char(std::tolower(u8(c))); });
            if (exact ? n == want : n.find(want) != std::string::npos) {
                out = p;
                return true;
            }
        }
    return false;
}

std::unordered_map<std::string, std::string> loadPackSettings(const PackInfo& pack) {
    const fs::path ours = packDirectory() / (pack.name + ".txt");
    std::error_code ec;
    if (fs::exists(ours, ec)) return readSettings(ours);
    // The launcher's settings (Iris / OptiFine keep "<pack>.txt" next to the pack): read, never written.
    return readSettings(pack.path.parent_path() / (pack.name + ".txt"));
}

void savePackSettings(const PackInfo& pack, const std::unordered_map<std::string, std::string>& values) {
    const fs::path ours = packDirectory() / (pack.name + ".txt");
    std::error_code ec;
    fs::create_directories(ours.parent_path(), ec);
    std::vector<std::pair<std::string, std::string>> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    std::ofstream f(ours, std::ios::binary);
    f << "#minecraftCplusplus shader pack settings (Iris format)\n";
    for (const auto& [k, v] : sorted) f << k << "=" << v << "\n";
}

void setShaderPack(Renderer& renderer, const fs::path& pack, const std::unordered_map<std::string, std::string>& options) {
    Renderer::Packs& p = renderer.packs();
    if (pack.empty()) {
        p.disable();
        return;
    }
    LoadOptions opt;
    opt.optionValues = options;
    p.request(pack, opt);
}

void setBlockRegistry(Renderer& renderer, const BlockRegistry* registry) { renderer.packs().setBlockRegistry(registry); }

void setGameState(Renderer& renderer, const PackGameState& state) { renderer.packs().setGameState(state); }

PackStatus packStatus(Renderer& renderer) {
    const auto s = renderer.packs().status();
    PackStatus out;
    out.active = s.pack, out.loading = s.loading, out.error = s.error, out.warnings = s.warnings, out.loadMs = s.loadMs;
    return out;
}

namespace {
std::string g_cliPack;
std::unordered_map<std::string, std::string> g_cliOptions;
}  // namespace

void commandLine(const std::wstring& flag, const std::wstring& value) {
    const std::u8string u = fs::path(value).u8string();
    const std::string v(u.begin(), u.end());
    if (flag == L"--shaderpack") g_cliPack = v;
    else if (size_t eq = v.find('='); eq != std::string::npos) g_cliOptions[v.substr(0, eq)] = v.substr(eq + 1);
}

void applyCommandLine(Renderer& renderer, const BlockRegistry* registry) {
    setBlockRegistry(renderer, registry);
    if (g_cliPack.empty() || g_cliPack == "off") return;
    PackInfo info;
    if (!findPack(g_cliPack, info)) {
        LOG("warning: shader pack '%s' not found (folders: %s, %s)", g_cliPack.c_str(), packDirectory().string().c_str(),
            externalPackDirectory().string().c_str());
        return;
    }
    auto options = loadPackSettings(info);
    for (const auto& [k, v] : g_cliOptions) options[k] = v;
    LOG("shader pack: loading %s (%zu option values)", info.path.string().c_str(), options.size());
    setShaderPack(renderer, info.path, options);
}

bool commandLinePack() { return !g_cliPack.empty(); }

bool selectPack(Renderer& renderer, const std::string& name) {
    if (name.empty()) {
        setShaderPack(renderer, {});
        return true;
    }
    PackInfo info;
    if (!findPack(name, info)) {
        LOG("warning: shader pack '%s' not found", name.c_str());
        return false;
    }
    const auto options = loadPackSettings(info);
    LOG("shader pack: loading %s (%zu option values)", info.path.string().c_str(), options.size());
    setShaderPack(renderer, info.path, options);
    return true;
}

}  // namespace packs
