#include "assets/asset_source.h"

#include <json/json.hpp>

#include <algorithm>
#include <mutex>

namespace {

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

// PackFormat: major.minor, compared in that order.
struct Format {
    int major = 0, minor = 0;
    bool operator<=(const Format& o) const { return major != o.major ? major < o.major : minor <= o.minor; }
};
Format gResourceFormat{97, 1}, gDataFormat{121, 0};  // 26.3 until the jar says otherwise

// PackFormat.BOTTOM_CODEC / TOP_CODEC: a major, or [major, minor...]; a missing minor is 0 (min) or any (max).
std::optional<Format> parseFormat(const nlohmann::json& j, bool top) {
    if (j.is_number_integer()) return Format{j.get<int>(), top ? INT32_MAX : 0};
    if (j.is_array() && !j.empty() && j[0].is_number_integer())
        return Format{j[0].get<int>(), j.size() > 1 && j[1].is_number_integer() ? j[1].get<int>() : top ? INT32_MAX : 0};
    return std::nullopt;
}

// OverlayEntry.isApplicable: min_format..max_format, or "formats" (majors: n, [min, max], {min_inclusive, max_inclusive}).
bool overlayApplies(const nlohmann::json& e, const Format& version) {
    std::optional<Format> lo, hi;
    if (e.contains("min_format")) lo = parseFormat(e["min_format"], false);
    if (e.contains("max_format")) hi = parseFormat(e["max_format"], true);
    if ((!lo || !hi) && e.contains("formats")) {
        const nlohmann::json& f = e["formats"];
        int a = -1, b = -1;
        if (f.is_number_integer()) a = b = f.get<int>();
        else if (f.is_array() && f.size() == 2 && f[0].is_number_integer() && f[1].is_number_integer()) a = f[0], b = f[1];
        else if (f.is_object() && f.contains("min_inclusive") && f.contains("max_inclusive")) a = f["min_inclusive"], b = f["max_inclusive"];
        if (a < 0) return false;
        lo = Format{a, 0}, hi = Format{b, INT32_MAX};
    }
    return lo && hi && *lo <= version && version <= *hi;
}

std::optional<std::regex> pattern(const nlohmann::json& o, const char* key) {
    if (!o.is_object() || !o.contains(key) || !o[key].is_string()) return std::nullopt;
    try {
        return std::regex(o[key].get<std::string>());
    } catch (const std::regex_error&) {
        return std::regex("$^");  // an invalid pattern matches nothing
    }
}

}  // namespace

void AssetPack::setFormats(int resourceMajor, int resourceMinor, int dataMajor, int dataMinor) {
    gResourceFormat = {resourceMajor, resourceMinor}, gDataFormat = {dataMajor, dataMinor};
}

// --- AssetPack ------------------------------------------------------------------------------------------------

std::unique_ptr<AssetPack> AssetPack::openZip(const fs::path& file, std::string id, std::string_view prefix,
                                              std::shared_ptr<ZipArchive> shared) {
    auto pack = std::make_unique<AssetPack>();
    pack->id_ = std::move(id);
    pack->path_ = file;
    pack->zip_ = shared;
    if (!pack->zip_) {
        pack->zip_ = std::make_shared<ZipArchive>();
        if (!pack->zip_->open(file)) return nullptr;
    }
    const auto& entries = pack->zip_->entries();
    pack->files_.reserve(prefix.empty() ? entries.size() : 64);
    for (size_t i = 0; i < entries.size(); ++i) {
        std::string_view n = entries[i].name;
        if (n.empty() || n.back() == '/' || !n.starts_with(prefix)) continue;
        n.remove_prefix(prefix.size());
        pack->files_.push_back({std::string(n), u32(i), {}});
    }
    pack->applyMeta();
    pack->finish();
    return pack;
}

std::unique_ptr<AssetPack> AssetPack::openFolder(const fs::path& dir, std::string id) {
    auto pack = std::make_unique<AssetPack>();
    pack->id_ = std::move(id);
    pack->path_ = dir;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string rel = utf8(fs::relative(it->path(), dir, ec));
        if (ec) break;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        pack->files_.push_back({std::move(rel), 0, utf8(it->path())});
    }
    pack->applyMeta();
    pack->finish();
    return pack;
}

std::unique_ptr<AssetPack> AssetPack::openIndex(const fs::path& minecraftDir, const fs::path& jar, std::string id) {
    std::vector<u8> bytes;
    fs::path versionJson = jar;
    versionJson.replace_extension(".json");
    if (!readFile(versionJson, bytes)) return nullptr;
    const nlohmann::json v = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    auto str = [](const nlohmann::json& o, const char* key) {  // never throws on unexpected types
        if (!o.is_object()) return std::string();
        const auto it = o.find(key);
        return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    std::string indexId = v.is_object() && v.contains("assetIndex") ? str(v["assetIndex"], "id") : "";
    if (indexId.empty()) indexId = str(v, "assets");
    if (indexId.empty() || !readFile(minecraftDir / "assets" / "indexes" / (indexId + ".json"), bytes)) return nullptr;
    auto pack = std::make_unique<AssetPack>();
    pack->id_ = std::move(id);
    pack->path_ = minecraftDir / "assets";
    const std::string objects = utf8(minecraftDir / "assets" / "objects") + "/";
    // A flat scan instead of a full JSON parse (the index has ~5000 objects and is read at every start):
    // "<key>": {"hash": "<sha1>", "size": n}. Keys and hashes contain no escapes.
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    size_t pos = text.find("\"objects\"");
    if (pos == std::string_view::npos) return nullptr;
    pos = text.find('{', pos);
    while (pos != std::string_view::npos) {
        const size_t k0 = text.find('"', pos + 1);
        if (k0 == std::string_view::npos) break;
        const size_t k1 = text.find('"', k0 + 1);
        const size_t open = text.find('{', k1);
        const size_t close = text.find('}', open);
        if (k1 == std::string_view::npos || open == std::string_view::npos || close == std::string_view::npos) break;
        const std::string_view body = text.substr(open, close - open);
        const size_t h = body.find("\"hash\"");
        const size_t h0 = h == std::string_view::npos ? h : body.find('"', h + 6);
        const size_t h1 = h0 == std::string_view::npos ? h0 : body.find('"', h0 + 1);
        if (h1 != std::string_view::npos && h1 - h0 > 3) {
            const std::string_view hash = body.substr(h0 + 1, h1 - h0 - 1);
            pack->files_.push_back({"assets/" + std::string(text.substr(k0 + 1, k1 - k0 - 1)), 0,
                                    objects + std::string(hash.substr(0, 2)) + "/" + std::string(hash)});
        }
        pos = close + 1;
        // Past the last object: "}" closes "objects".
        const size_t next = text.find_first_not_of(" \t\r\n,", pos);
        if (next == std::string_view::npos || text[next] == '}') break;
        pos = next - 1;
    }
    if (pack->files_.empty()) return nullptr;
    pack->finish();
    return pack;
}

// CompositePackResources: the overlay directories that apply to this version lie over the root (the last listed on
// top), assets/ by the resource pack format and data/ by the data pack format. ResourceFilterSection: the filter.
void AssetPack::applyMeta() {
    const auto meta = std::find_if(files_.begin(), files_.end(), [](const File& f) { return f.path == "pack.mcmeta"; });
    if (meta == files_.end()) return;
    std::vector<u8> bytes;
    if (zip_ ? !zip_->read(zip_->entries()[meta->entry], bytes) : !readFile(fromUtf8(meta->disk), bytes)) return;
    const nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (!j.is_object()) return;
    if (j.contains("filter") && j["filter"].is_object() && j["filter"].contains("block") && j["filter"]["block"].is_array())
        for (const nlohmann::json& b : j["filter"]["block"]) filter_.push_back({pattern(b, "namespace"), pattern(b, "path")});
    if (!j.contains("overlays") || !j["overlays"].is_object() || !j["overlays"].contains("entries") ||
        !j["overlays"]["entries"].is_array())
        return;
    std::vector<std::pair<std::string, int>> dirs;  // overlay directory -> 1 assets, 2 data, 3 both apply
    for (const nlohmann::json& e : j["overlays"]["entries"]) {
        if (!e.is_object() || !e.contains("directory") || !e["directory"].is_string()) continue;
        const int applies = (overlayApplies(e, gResourceFormat) ? 1 : 0) | (overlayApplies(e, gDataFormat) ? 2 : 0);
        if (applies) dirs.push_back({e["directory"].get<std::string>() + "/", applies});
    }
    if (dirs.empty()) return;
    std::unordered_map<std::string, size_t> at;  // path -> its file in files_ (the root's, then each overlay's)
    for (size_t i = 0; i < files_.size(); ++i) at.emplace(files_[i].path, i);
    std::vector<File> overlaid;
    for (const auto& [dir, applies] : dirs)
        for (const File& f : files_) {
            if (!f.path.starts_with(dir)) continue;
            std::string rest = f.path.substr(dir.size());
            if (!((applies & 1) && rest.starts_with("assets/")) && !((applies & 2) && rest.starts_with("data/"))) continue;
            File o = f;
            o.path = std::move(rest);
            overlaid.push_back(std::move(o));
        }
    // Later entries win: put them first, so finish()'s unique() keeps them over earlier ones and the root.
    std::reverse(overlaid.begin(), overlaid.end());
    overlaid.insert(overlaid.end(), files_.begin(), files_.end());
    files_ = std::move(overlaid);
}

bool AssetPack::filters(std::string_view path) const {
    std::string_view rest;
    if (path.starts_with("assets/")) rest = path.substr(7);
    else if (path.starts_with("data/")) rest = path.substr(5);
    else return false;
    const size_t slash = rest.find('/');
    if (slash == std::string_view::npos) return false;
    const std::string ns(rest.substr(0, slash)), p(rest.substr(slash + 1));
    for (const FilterEntry& e : filter_)
        if ((!e.ns || std::regex_match(ns, *e.ns)) && (!e.path || std::regex_match(p, *e.path))) return true;
    return false;
}

void AssetPack::finish() {
    std::stable_sort(files_.begin(), files_.end(), [](const File& a, const File& b) { return a.path < b.path; });
    files_.erase(std::unique(files_.begin(), files_.end(), [](const File& a, const File& b) { return a.path == b.path; }),
                 files_.end());
    index_.reserve(files_.size());
    for (u32 i = 0; i < files_.size(); ++i) index_.emplace(std::string_view(files_[i].path), i);
}

const AssetPack::File* AssetPack::lookup(std::string_view path) const {
    auto it = index_.find(path);
    return it == index_.end() ? nullptr : &files_[it->second];
}

bool AssetPack::read(std::string_view path, std::vector<u8>& out) const {
    const File* f = lookup(path);
    if (!f) return false;
    if (zip_) return zip_->read(zip_->entries()[f->entry], out);
    return readFile(fromUtf8(f->disk), out);
}

void AssetPack::list(std::string_view prefix, const std::function<void(const std::string&)>& fn) const {
    auto it = std::lower_bound(files_.begin(), files_.end(), prefix,
                               [](const File& f, std::string_view p) { return std::string_view(f.path) < p; });
    for (; it != files_.end() && std::string_view(it->path).starts_with(prefix); ++it) fn(it->path);
}

void AssetPack::children(std::string_view root, std::vector<std::string>& out) const {
    auto it = std::lower_bound(files_.begin(), files_.end(), root,
                               [](const File& f, std::string_view p) { return std::string_view(f.path) < p; });
    while (it != files_.end() && std::string_view(it->path).starts_with(root)) {
        const std::string_view rest = std::string_view(it->path).substr(root.size());
        const size_t slash = rest.find('/');
        if (slash == std::string_view::npos) {
            ++it;
            continue;
        }
        const std::string child(rest.substr(0, slash));
        if (std::find(out.begin(), out.end(), child) == out.end()) out.push_back(child);
        // Skip past every path of this child: "<root><child>/" sorts before "<root><child>0".
        const std::string next = std::string(root) + child + char('/' + 1);
        it = std::lower_bound(it, files_.end(), next,
                              [](const File& f, const std::string& p) { return f.path < p; });
    }
}

fs::path AssetPack::diskPath(std::string_view path) const {
    const File* f = lookup(path);
    return f && !f->disk.empty() ? fromUtf8(f->disk) : fs::path();
}

// --- AssetSource ----------------------------------------------------------------------------------------------

bool AssetSource::visible(size_t pack, std::string_view path) const {
    for (size_t j = pack + 1; j < packs_.size(); ++j)
        if (packs_[j]->hasFilter() && packs_[j]->filters(path)) return false;
    return true;
}

int AssetSource::find(std::string_view path) const {
    for (size_t i = packs_.size(); i-- > 0;)
        if (packs_[i]->has(path)) return visible(i, path) ? int(i) : -1;
    return -1;
}

bool AssetSource::read(std::string_view path, std::vector<u8>& out) const {
    const int i = find(path);
    return i >= 0 && packs_[size_t(i)]->read(path, out);
}

std::vector<std::string> AssetSource::list(std::string_view prefix, std::string_view suffix) const {
    std::vector<std::string> out;
    for (size_t i = 0; i < packs_.size(); ++i)
        packs_[i]->list(prefix, [&](const std::string& f) {
            if (std::string_view(f).ends_with(suffix) && visible(i, f)) out.push_back(f);
        });
    if (packs_.size() > 1) {
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
    return out;
}

void AssetSource::readAll(std::string_view path,
                          const std::function<void(size_t pack, const std::vector<u8>&)>& fn) const {
    std::vector<u8> bytes;
    for (size_t i = 0; i < packs_.size(); ++i)
        if (packs_[i]->has(path) && visible(i, path) && packs_[i]->read(path, bytes)) fn(i, bytes);
}

std::vector<std::string> AssetSource::namespaces(std::string_view root) const {
    std::vector<std::string> out;
    std::string r(root);
    if (!r.empty() && r.back() != '/') r += '/';
    for (const auto& p : packs_) p->children(r, out);
    return out;
}

void AssetSource::readLang(std::string_view code, std::unordered_map<std::string, std::string>& out) const {
    for (const std::string& ns : namespaces("assets"))
        readAll("assets/" + ns + "/lang/" + std::string(code) + ".json", [&](size_t, const std::vector<u8>& bytes) {
            const nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
            if (!j.is_object()) return;
            for (const auto& item : j.items())
                if (item.value().is_string()) out[item.key()] = item.value().get<std::string>();
        });
    // DeprecatedTranslationsInfo: the game's renamed keys (the jar's language files still use the old names).
    std::vector<u8> bytes;
    if (read("assets/minecraft/lang/deprecated.json", bytes)) {
        const nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (j.is_object() && j.contains("renamed") && j["renamed"].is_object())
            for (const auto& [from, to] : j["renamed"].items())
                if (auto it = out.find(from); it != out.end() && to.is_string()) out[to.get<std::string>()] = it->second;
    }
}

void AssetSource::addVanilla(AssetSource& src, const fs::path& jar, bool clientAssets) {
    if (auto pack = AssetPack::openZip(jar, "vanilla")) {
        std::vector<u8> bytes;  // version.json pack_version: the formats pack overlays are chosen by
        if (pack->read("version.json", bytes)) {
            const nlohmann::json v = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
            if (v.is_object() && v.contains("pack_version") && v["pack_version"].is_object()) {
                const nlohmann::json& p = v["pack_version"];
                auto num = [&](const char* k, int def) { return p.contains(k) && p[k].is_number_integer() ? p[k].get<int>() : def; };
                AssetPack::setFormats(num("resource_major", 97), num("resource_minor", 0), num("data_major", 121), num("data_minor", 0));
            }
        }
        src.add(std::move(pack));
    } else {
        LOG("error: cannot read %s", jar.string().c_str());
    }
    if (!clientAssets) return;
    // versions/<v>/<v>.jar -> .minecraft
    const fs::path minecraftDir = jar.parent_path().parent_path().parent_path();
    if (auto index = AssetPack::openIndex(minecraftDir, jar, "vanilla/objects")) src.add(std::move(index));
}

namespace {
std::mutex gStackMutex;
}

AssetSource& AssetSource::client(const fs::path& jar) {
    static AssetSource s;
    if (!jar.empty()) {
        std::lock_guard lock(gStackMutex);
        if (s.empty()) addVanilla(s, jar, true);
    }
    return s;
}

AssetSource& AssetSource::data(const fs::path& jar) {
    static AssetSource s;
    if (!jar.empty()) {
        std::lock_guard lock(gStackMutex);
        if (s.empty()) addVanilla(s, jar, false);
    }
    return s;
}
