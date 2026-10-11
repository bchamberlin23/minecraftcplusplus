#include "audio/sound_bank.h"

#include "assets/asset_source.h"

#include <json/json.hpp>

namespace audio {

namespace {
constexpr std::string_view kNs = "minecraft:";
std::string_view stripNs(std::string_view s) { return s.starts_with(kNs) ? s.substr(kNs.size()) : s; }
}  // namespace

const char* categoryName(Category c) {
    static const char* names[kCategoryCount] = {"master", "music",  "record", "weather", "block", "hostile",
                                                "neutral", "player", "ambient", "voice",  "ui"};
    return names[int(c)];
}

bool SoundBank::load(const fs::path& mc, const std::string& indexId) {
    (void)mc;
    const AssetSource& src = AssetSource::client();
    if (src.empty() || !src.exists("assets/minecraft/sounds.json")) {
        LOG("audio: asset index %s not found (launch the game once from the launcher)", indexId.c_str());
        return false;
    }
    // Every namespace's sounds.json, merged over the packs bottom first like SoundManager.prepare: a definition
    // with "replace": true drops the sounds defined below it.
    nlohmann::json sounds = nlohmann::json::object();
    for (const std::string& ns : src.namespaces("assets"))
        src.readAll("assets/" + ns + "/sounds.json", [&](size_t, const std::vector<u8>& bytes) {
            const nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
            if (!j.is_object()) return;
            for (const auto& item : j.items()) {
                if (!item.value().is_object()) continue;
                const std::string key = ns == "minecraft" ? item.key() : ns + ":" + item.key();
                nlohmann::json& ev = sounds[key];
                if (!ev.is_object() || item.value().value("replace", false)) ev = {{"sounds", nlohmann::json::array()}};
                if (item.value().contains("sounds") && item.value()["sounds"].is_array())
                    for (const auto& snd : item.value()["sounds"]) ev["sounds"].push_back(snd);
            }
        });
    StringMap<int> fileIds;  // "dig/stone1" / "mod:x" -> file index
    // A file reference ("dig/stone1", "mod:x"): the topmost pack with assets/<ns>/sounds/<path>.ogg, or -1.
    auto fileFor = [&](std::string_view name) -> int {
        const size_t colon = name.find(':');
        const std::string ns = colon == std::string_view::npos ? "minecraft" : std::string(name.substr(0, colon));
        const std::string path(colon == std::string_view::npos ? name : name.substr(colon + 1));
        const std::string key = ns == "minecraft" ? path : ns + ":" + path;
        if (auto it = fileIds.find(key); it != fileIds.end()) return it->second;
        const std::string asset = "assets/" + ns + "/sounds/" + path + ".ogg";
        const int pack = src.find(asset);
        if (pack < 0) return -1;
        File f{key, src.pack(size_t(pack)).diskPath(asset), pack, asset};
        fileIds.emplace(key, int(files_.size()));
        files_.push_back(std::move(f));
        return int(files_.size()) - 1;
    };

    // Pass 1: event ids (entries may reference events defined later).
    for (const auto& item : sounds.items()) {
        eventIds_.emplace(item.key(), int(events_.size()));
        events_.push_back({item.key().find(':') == std::string::npos ? std::string(kNs) + item.key() : item.key(), {}});
    }
    for (const auto& item : sounds.items()) {
        Event& ev = events_[eventIds_.find(item.key())->second];
        if (!item.value().contains("sounds")) continue;
        for (const auto& s : item.value()["sounds"]) {
            Entry e;
            std::string name;
            bool isEvent = false;
            if (s.is_string()) {
                name = s.get<std::string>();
            } else if (s.is_object()) {
                name = s.value("name", "");
                isEvent = s.value("type", "file") == "event";
                e.volume = s.value("volume", 1.0f);
                e.pitch = s.value("pitch", 1.0f);
                e.weight = s.value("weight", 1);
                e.stream = s.value("stream", false);
                e.preload = s.value("preload", false);
                e.attenuation = s.value("attenuation_distance", 16);
            } else {
                continue;
            }
            const std::string_view n = stripNs(name);
            if (isEvent) {
                auto it = eventIds_.find(n);
                if (it == eventIds_.end()) continue;
                e.isEvent = true;
                e.target = it->second;
            } else {
                e.target = fileFor(name);
                if (e.target < 0) continue;  // the game skips files that do not exist
                if (e.preload) preload_.push_back(e.target);
            }
            ev.entries.push_back(e);
        }
    }
    return !events_.empty();
}

bool SoundBank::readBytes(int file, std::vector<u8>& out) const {
    if (file < 0 || file >= int(files_.size())) return false;
    const File& f = files_[size_t(file)];
    if (!f.path.empty()) return readFile(f.path, out);
    const AssetSource& src = AssetSource::client();
    return f.pack >= 0 && size_t(f.pack) < src.packCount() && src.pack(size_t(f.pack)).read(f.asset, out);
}

int SoundBank::eventId(std::string_view name) const {
    auto it = eventIds_.find(stripNs(name));
    return it == eventIds_.end() ? -1 : it->second;
}

int SoundBank::weight(int event, int depth) const {
    if (depth > 8) return 0;
    int sum = 0;
    for (const Entry& e : events_[event].entries) sum += e.isEvent ? weight(e.target, depth + 1) : e.weight;
    return sum;
}

bool SoundBank::pick(int event, JavaRandom& rng, ResolvedSound& out, int depth) const {
    if (depth > 8) return false;
    const int total = weight(event, depth);
    if (events_[event].entries.empty() || total == 0) return false;
    int index = rng.nextInt(total);
    for (const Entry& e : events_[event].entries) {
        index -= e.isEvent ? weight(e.target, depth + 1) : e.weight;
        if (index >= 0) continue;
        if (!e.isEvent) {
            out = {e.target, e.volume, e.pitch, e.stream, e.attenuation};
            return true;
        }
        ResolvedSound inner;
        if (!pick(e.target, rng, inner, depth + 1)) return false;
        out = {inner.file, inner.volume * e.volume, inner.pitch * e.pitch, inner.stream || e.stream, inner.attenuation};
        return true;
    }
    return false;
}

bool SoundBank::resolve(int event, JavaRandom& rng, ResolvedSound& out) const {
    return event >= 0 && event < int(events_.size()) && pick(event, rng, out, 0);
}

}  // namespace audio
