#include "render/packs/pack_options.h"

#include "render/packs/pack.h"
#include "render/packs/properties.h"

#include <algorithm>
#include <cctype>
#include <functional>

namespace packs {

PackOptionsModel::PackOptionsModel() = default;
PackOptionsModel::~PackOptionsModel() = default;

bool PackOptionsModel::open(const PackInfo& pack, std::string& error) {
    pack_ = pack;
    values_ = loadPackSettings(pack);
    changed_ = false;
    def_ = std::make_unique<PackDefinition>();
    LoadOptions opt;
    opt.optionValues = values_;
    opt.compileHlsl = false;
    if (!def_->load(pack.path, opt, error)) {
        def_.reset();
        return false;
    }
    // Keep only values the pack declares (stale entries from other versions are dropped on save).
    for (auto it = values_.begin(); it != values_.end();)
        it = def_->options.decls.count(it->first) ? std::next(it) : values_.erase(it);
    return true;
}

std::string PackOptionsModel::text(const std::string& key, const std::string& fallback) const {
    if (!def_) return fallback;
    auto it = def_->lang.find(key);
    if (it == def_->lang.end()) return fallback;
    std::string out;  // without formatting codes (section sign + one character)
    const std::string& s = it->second;
    for (size_t i = 0; i < s.size(); ++i) {
        if (u8(s[i]) == 0xC2 && i + 1 < s.size() && u8(s[i + 1]) == 0xA7) {
            i += 2;
            continue;
        }
        if (s[i] == '&' && i + 1 < s.size() && std::isalnum(u8(s[i + 1]))) {  // some packs write &x codes
            ++i;
            continue;
        }
        out += s[i];
    }
    return out;
}

std::vector<PackOptionsModel::Entry> PackOptionsModel::entries(const std::string& screen) const {
    std::vector<Entry> out;
    if (!def_) return out;
    const std::string* list = def_->props.get(screen.empty() ? "screen" : "screen." + screen);
    if (!list) {  // no screens: every option
        for (const auto& [name, d] : def_->options.decls)
            if (!d.isConst || !d.allowed.empty()) out.push_back({Entry::Option, name});
        return out;
    }
    for (const std::string& item : splitWhitespace(*list)) {
        if (item == "<empty>") out.push_back({Entry::Empty, {}});
        else if (item == "<profile>") out.push_back({Entry::Profile, {}});
        else if (item.size() > 2 && item.front() == '[' && item.back() == ']') out.push_back({Entry::Link, item.substr(1, item.size() - 2)});
        else if (item == "*") {  // every option not on a screen
            for (const auto& [name, d] : def_->options.decls) out.push_back({Entry::Option, name});
        } else if (def_->options.decls.count(item)) {
            out.push_back({Entry::Option, item});
        }
    }
    return out;
}

int PackOptionsModel::columns(const std::string& screen) const {
    if (!def_) return 2;
    const std::string v = def_->props.value(screen.empty() ? "screen.columns" : "screen." + screen + ".columns", "2");
    return std::clamp(std::atoi(v.c_str()), 1, 4);
}

std::string PackOptionsModel::screenTitle(const std::string& screen) const {
    if (screen.empty()) return pack_.name;
    return text("screen." + screen, screen);
}

std::string PackOptionsModel::value(const std::string& option) const {
    if (auto it = values_.find(option); it != values_.end()) return it->second;
    auto d = def_->options.decls.find(option);
    return d == def_->options.decls.end() ? std::string() : d->second.boolean ? (d->second.value == "false" ? "false" : "true")
                                                                                : d->second.value;
}

bool PackOptionsModel::isSlider(const std::string& option) const {
    if (!def_) return false;
    const std::string* s = def_->props.get("sliders");
    if (!s) return false;
    const auto names = splitWhitespace(*s);
    return std::find(names.begin(), names.end(), option) != names.end();
}

std::vector<std::string> PackOptionsModel::profiles() const {
    std::vector<std::string> out;
    if (!def_) return out;
    for (const auto& [k, v] : def_->props.entries)
        if (k.compare(0, 8, "profile.") == 0 && std::find(out.begin(), out.end(), k.substr(8)) == out.end()) out.push_back(k.substr(8));
    return out;
}

std::unordered_map<std::string, std::string> PackOptionsModel::profileValues(const std::string& name) const {
    std::unordered_map<std::string, std::string> out;
    std::function<void(const std::string&, int)> apply = [&](const std::string& profile, int depth) {
        const std::string* def = def_->props.get("profile." + profile);
        if (!def || depth > 8) return;
        for (const std::string& item : splitWhitespace(*def)) {
            if (item.compare(0, 8, "profile.") == 0) apply(item.substr(8), depth + 1);
            else if (item[0] == '!') out[item.substr(1)] = "false";
            else if (size_t eq = item.find('='); eq != std::string::npos) out[item.substr(0, eq)] = item.substr(eq + 1);
            else if (size_t col = item.find(':'); col != std::string::npos) out[item.substr(0, col)] = item.substr(col + 1);
            else out[item] = "true";
        }
    };
    apply(name, 0);
    return out;
}

std::string PackOptionsModel::currentProfile() const {
    for (const std::string& p : profiles()) {
        bool match = true;
        for (const auto& [k, v] : profileValues(p))
            if (def_->options.decls.count(k) && value(k) != v) match = false;
        if (match) return p;
    }
    return {};
}

std::string PackOptionsModel::label(const Entry& e) const {
    switch (e.kind) {
        case Entry::Empty: return {};
        case Entry::Link: return text("screen." + e.id, e.id) + "...";
        case Entry::Profile: {
            const std::string p = currentProfile();
            return "Profile: " + (p.empty() ? std::string("Custom") : text("profile." + p, p));
        }
        case Entry::Option: break;
    }
    const std::string v = value(e.id);
    const auto& d = def_->options.decls.at(e.id);
    std::string shown = d.boolean ? (v == "true" ? "ON" : "OFF") : text("value." + e.id + "." + v, v);
    // OptiFine's prefix / suffix lang keys around a value (prefix.NAME / suffix.NAME).
    shown = text("prefix." + e.id, "") + shown + text("suffix." + e.id, "");
    return text("option." + e.id, e.id) + ": " + shown;
}

void PackOptionsModel::cycle(const Entry& e, int dir) {
    if (!def_) return;
    auto set = [&](const std::string& name, const std::string& v) {
        auto d = def_->options.decls.find(name);
        if (d == def_->options.decls.end()) return;
        const std::string def = d->second.boolean ? (d->second.value == "false" ? "false" : "true") : d->second.value;
        if (v == def) values_.erase(name);
        else values_[name] = v;
        changed_ = true;
    };
    if (e.kind == Entry::Profile) {
        const auto names = profiles();
        if (names.empty()) return;
        const std::string cur = currentProfile();
        auto it = std::find(names.begin(), names.end(), cur);
        size_t i = it == names.end() ? 0 : size_t(it - names.begin());
        i = it == names.end() ? 0 : (i + names.size() + size_t(dir > 0 ? 1 : names.size() - 1)) % names.size();
        for (const auto& [k, v] : profileValues(names[i])) set(k, v);
        return;
    }
    if (e.kind != Entry::Option) return;
    const auto& d = def_->options.decls.at(e.id);
    if (d.boolean) return set(e.id, value(e.id) == "true" ? "false" : "true");
    std::vector<std::string> allowed = d.allowed;
    if (allowed.empty()) return;
    auto it = std::find(allowed.begin(), allowed.end(), value(e.id));
    size_t i = it == allowed.end() ? 0 : size_t(it - allowed.begin());
    i = (i + allowed.size() + (dir > 0 ? 1 : allowed.size() - 1)) % allowed.size();
    set(e.id, allowed[i]);
}

void PackOptionsModel::resetAll() {
    if (!values_.empty()) changed_ = true;
    values_.clear();
}

}  // namespace packs
