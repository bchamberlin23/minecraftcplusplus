#include "worldgen/datapack.h"

#include "assets/asset_source.h"

#include <algorithm>

namespace wg {

std::string DataPack::normalize(std::string_view id) {
    if (id.starts_with("minecraft:")) id.remove_prefix(10);
    if (!id.empty() && id[0] == '#') id.remove_prefix(1);
    if (id.starts_with("minecraft:")) id.remove_prefix(10);
    return std::string(id);
}

bool DataPack::open(const fs::path& jar) {
    // The game's data with the enabled data packs (AssetSource::data): worldgen files come from the topmost pack,
    // tags are merged over all packs bottom first like the game's TagLoader ("replace": true drops lower values).
    const AssetSource& src = AssetSource::data(jar);
    if (src.empty()) return false;
    jar_ = jar;
    std::vector<u8> bytes;
    for (const std::string& ns : src.namespaces("data")) {
        const std::string root = "data/" + ns + "/";
        for (const char* kind : {"worldgen/", "tags/", "dimension_type/", "dimension/"}) {
            for (const std::string& file : src.list(root + kind, ".json")) {
                std::string_view rel = file;
                rel.remove_prefix(root.size());
                rel.remove_suffix(5);
                // "worldgen/<registry>/<path>", "tags/<kind>/<path>" (kind "block" or "worldgen/<registry>")
                const size_t typeEnd = rel.find('/');
                // dimension_type/<path>, dimension/<path>: no registry folder level
                const bool flat = rel.starts_with("dimension");
                size_t regEnd = flat ? typeEnd : rel.find('/', typeEnd + 1);
                if (regEnd == std::string_view::npos) continue;
                if (rel.starts_with("tags/worldgen/")) {
                    regEnd = rel.find('/', regEnd + 1);
                    if (regEnd == std::string_view::npos) continue;
                }
                const std::string_view path = rel.substr(regEnd + 1);
                std::string key(rel.substr(0, regEnd + 1));
                if (ns != "minecraft") key += ns + ":";
                key += path;
                if (rel.starts_with("tags/")) {
                    nlohmann::json merged = {{"values", nlohmann::json::array()}};
                    src.readAll(file, [&](size_t, const std::vector<u8>& b) {
                        const nlohmann::json j = nlohmann::json::parse(b.begin(), b.end(), nullptr, false);
                        if (!j.is_object()) return;
                        // Data packs can be sloppy ("replace": "true", 1...): never let one tag file throw.
                        const auto replace = j.find("replace");
                        if (replace != j.end() && replace->is_boolean() && replace->get<bool>())
                            merged["values"] = nlohmann::json::array();
                        if (j.contains("values") && j["values"].is_array())
                            for (const auto& v : j["values"]) merged["values"].push_back(v);
                    });
                    files_.emplace(std::move(key), std::move(merged));
                    continue;
                }
                if (!src.read(file, bytes) || bytes.empty()) continue;
                nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
                if (!j.is_discarded()) files_.emplace(std::move(key), std::move(j));
            }
        }
    }
    return !files_.empty();
}

bool DataPack::readFile(std::string_view path, std::vector<u8>& out) const {
    return AssetSource::data(jar_).read(path, out);
}

const nlohmann::json* DataPack::file(std::string_view folder, std::string_view id) const {
    auto it = files_.find(std::string(folder) + "/" + normalize(id));
    return it == files_.end() ? nullptr : &it->second;
}

std::vector<std::string> DataPack::fileIds(std::string_view folder) const {
    const std::string prefix = std::string(folder) + "/";
    std::vector<std::string> out;
    for (const auto& [key, j] : files_)
        if (key.starts_with(prefix)) {
            const std::string rest = key.substr(prefix.size());
            out.push_back(rest.find(':') == std::string::npos ? "minecraft:" + rest : rest);
        }
    std::sort(out.begin(), out.end());
    return out;
}

const nlohmann::json* DataPack::worldgen(std::string_view registry, std::string_view id) const {
    std::string key = "worldgen/";
    key += registry;
    key += '/';
    key += normalize(id);
    auto it = files_.find(key);
    return it == files_.end() ? nullptr : &it->second;
}

std::vector<std::string> DataPack::ids(std::string_view registry) const {
    const std::string prefix = "worldgen/" + std::string(registry) + "/";
    std::vector<std::pair<std::string, std::string>> found;  // (path, namespace)
    for (const auto& [key, j] : files_) {
        if (!key.starts_with(prefix)) continue;
        std::string rest = key.substr(prefix.size()), ns = "minecraft";
        if (const size_t colon = rest.find(':'); colon != std::string::npos) ns = rest.substr(0, colon), rest = rest.substr(colon + 1);
        found.emplace_back(std::move(rest), std::move(ns));
    }
    std::sort(found.begin(), found.end());
    std::vector<std::string> out;
    for (const auto& [path, ns] : found) out.push_back(ns + ":" + path);
    return out;
}

std::vector<std::string> DataPack::tag(std::string_view kind, std::string_view id) const {
    std::vector<std::string> out;
    std::vector<std::string> pending{normalize(id)};
    std::vector<std::string> seen;
    while (!pending.empty()) {
        std::string t = pending.back();
        pending.pop_back();
        if (std::find(seen.begin(), seen.end(), t) != seen.end()) continue;
        seen.push_back(t);
        auto it = files_.find("tags/" + std::string(kind) + "/" + t);
        if (it == files_.end()) continue;
        const nlohmann::json& values = it->second.contains("values") ? it->second["values"] : it->second;
        for (const auto& v : values) {
            std::string s = v.is_string() ? v.get<std::string>()
                            : v.is_object() && v.contains("id") && v["id"].is_string() ? v["id"].get<std::string>()
                                                                                      : std::string();
            if (s.starts_with("#"))
                pending.push_back(normalize(s));
            else if (!s.empty())
                out.push_back(s.find(':') == std::string::npos ? "minecraft:" + s : s);
        }
    }
    return out;
}

const nlohmann::json* DataPack::tagValues(std::string_view kind, std::string_view id) const {
    auto it = files_.find("tags/" + std::string(kind) + "/" + normalize(id));
    if (it == files_.end()) return nullptr;
    return it->second.contains("values") ? &it->second["values"] : &it->second;
}

std::string blockStateKey(const nlohmann::json& state) {
    std::string name = state.is_string() ? state.get<std::string>()
                       : state.is_object() && state.contains("Name") && state["Name"].is_string()
                           ? state["Name"].get<std::string>()
                           : std::string("minecraft:air");
    if (name.find(':') == std::string::npos) name = "minecraft:" + name;
    std::vector<std::pair<std::string, std::string>> props;
    if (state.is_object())
        if (auto it = state.find("Properties"); it != state.end() && it->is_object())
            for (const auto& item : it->items())
                props.push_back({item.key(), item.value().is_string() ? item.value().get<std::string>() : item.value().dump()});
    std::sort(props.begin(), props.end());
    if (props.empty()) return name;
    name += '[';
    for (size_t i = 0; i < props.size(); ++i) {
        if (i) name += ',';
        name += props[i].first + "=" + props[i].second;
    }
    name += ']';
    return name;
}

}  // namespace wg
