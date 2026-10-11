#include "render/packs/pack_source.h"

#include "assets/zip.h"

#include <algorithm>

namespace packs {

namespace {
std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower(u8(c)));
    return r;
}
}  // namespace

PackSource::PackSource() = default;
PackSource::~PackSource() = default;

std::string PackSource::normalize(std::string_view path) {
    std::vector<std::string_view> parts;
    std::string tmp(path);
    std::replace(tmp.begin(), tmp.end(), '\\', '/');
    std::string_view s = tmp;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('/', i);
        if (j == std::string_view::npos) j = s.size();
        std::string_view p = s.substr(i, j - i);
        if (p == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!p.empty() && p != ".") {
            parts.push_back(p);
        }
        i = j + 1;
    }
    std::string out;
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k) out += '/';
        out += parts[k];
    }
    return out;
}

bool PackSource::open(const fs::path& path, std::string& error) {
    path_ = path;
    name_ = path.filename().string();
    files_.clear();
    index_.clear();
    lowerIndex_.clear();
    zipEntry_.clear();
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        // The shaders directory: path/shaders, or path/<one folder>/shaders.
        fs::path shaders = path / "shaders";
        if (!fs::is_directory(shaders, ec)) {
            shaders.clear();
            for (const auto& d : fs::directory_iterator(path, ec))
                if (d.is_directory() && fs::is_directory(d.path() / "shaders", ec)) {
                    shaders = d.path() / "shaders";
                    break;
                }
        }
        if (shaders.empty()) return error = "no shaders folder in " + name_, false;
        folder_ = shaders;
        for (auto it = fs::recursive_directory_iterator(shaders, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file()) continue;
            std::string rel = fs::relative(it->path(), shaders, ec).generic_string();
            if (rel.empty()) continue;
            index_[rel] = files_.size();
            lowerIndex_.emplace(lower(rel), files_.size());
            files_.push_back(rel);
        }
        return true;
    }
    zip_ = std::make_unique<ZipArchive>();
    if (!zip_->open(path)) return error = "cannot read " + name_ + " (not a zip?)", false;
    // Find the shortest "<prefix>shaders/" directory that holds files.
    std::string best;
    bool found = false;
    for (const auto& e : zip_->entries()) {
        std::string_view n = e.name;
        size_t at = n.find("shaders/");
        while (at != std::string_view::npos) {
            if (at == 0 || n[at - 1] == '/') {
                std::string prefix(n.substr(0, at + 8));
                if (std::count(prefix.begin(), prefix.end(), '/') <= 2 && (!found || prefix.size() < best.size()))
                    best = prefix, found = true;
                break;
            }
            at = n.find("shaders/", at + 1);
        }
    }
    if (!found) return error = "no shaders folder in " + name_, false;
    zipPrefix_ = best;
    const auto& entries = zip_->entries();
    for (size_t i = 0; i < entries.size(); ++i) {
        const std::string& n = entries[i].name;
        if (n.size() <= zipPrefix_.size() || n.compare(0, zipPrefix_.size(), zipPrefix_) != 0 || n.back() == '/') continue;
        std::string rel = normalize(std::string_view(n).substr(zipPrefix_.size()));
        if (rel.empty() || index_.count(rel)) continue;
        index_[rel] = files_.size();
        lowerIndex_.emplace(lower(rel), files_.size());
        zipEntry_[rel] = i;
        files_.push_back(rel);
    }
    return true;
}

const std::string* PackSource::find(std::string_view rel) const {
    const std::string n = normalize(rel);
    if (auto it = index_.find(n); it != index_.end()) return &files_[it->second];
    if (auto it = lowerIndex_.find(lower(n)); it != lowerIndex_.end()) return &files_[it->second];
    return nullptr;
}

bool PackSource::exists(std::string_view rel) const { return find(rel) != nullptr; }

bool PackSource::readBytes(std::string_view rel, std::vector<u8>& out) const {
    const std::string* name = find(rel);
    if (!name) return false;
    if (zip_) {
        auto it = zipEntry_.find(*name);
        return it != zipEntry_.end() && zip_->read(zip_->entries()[it->second], out);
    }
    return readFile(folder_ / fs::u8path(*name), out);
}

bool PackSource::read(std::string_view rel, std::string& out) const {
    std::vector<u8> bytes;
    if (!readBytes(rel, bytes)) return false;
    size_t skip = bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF ? 3 : 0;  // UTF-8 BOM
    out.assign(reinterpret_cast<const char*>(bytes.data()) + skip, bytes.size() - skip);
    return true;
}

std::vector<std::string> PackSource::list(std::string_view dir) const {
    std::string prefix = normalize(dir);
    if (!prefix.empty()) prefix += '/';
    std::vector<std::string> out;
    for (const std::string& f : files_)
        if (f.size() > prefix.size() && f.compare(0, prefix.size(), prefix) == 0 &&
            f.find('/', prefix.size()) == std::string::npos)
            out.push_back(f);
    return out;
}

}  // namespace packs
