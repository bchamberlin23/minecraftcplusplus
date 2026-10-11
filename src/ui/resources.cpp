#include "ui/resources.h"

#include <stb/stb_image.h>

namespace ui {

bool decodeImage(const std::vector<u8>& bytes, Image& out) {
    int w, h, n;
    u8* img = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
    if (!img) return false;
    out.w = w, out.h = h;
    out.rgba.assign(img, img + size_t(w) * h * 4);
    stbi_image_free(img);
    return true;
}

bool Resources::open(const fs::path& jar, const fs::path& minecraftDir) {
    (void)minecraftDir;  // the asset index beside the jar is part of the client stack
    src_ = &AssetSource::client(jar);
    if (src_->empty()) {
        LOG("ui: cannot open %s", jar.string().c_str());
        return false;
    }
    fs::path unifont;
    if (objectPath("assets/minecraft/font/unifont.zip", unifont).empty())
        LOG("ui: no asset index found; unifont glyphs are unavailable");
    src_->readLang("en_us", lang_);
    return true;
}

const fs::path& Resources::objectPath(std::string_view path, fs::path& out) const {
    out.clear();
    const int pack = src_ ? src_->find(path) : -1;
    if (pack >= 0) out = src_->pack(size_t(pack)).diskPath(path);
    return out;
}

bool Resources::read(std::string_view path, std::vector<u8>& out) const { return src_ && src_->read(path, out); }

bool Resources::exists(std::string_view path) const { return src_ && src_->exists(path); }

bool Resources::image(std::string_view path, Image& out) const {
    std::vector<u8> bytes;
    return read(path, bytes) && decodeImage(bytes, out);
}

nlohmann::json Resources::json(std::string_view path) const {
    std::vector<u8> bytes;
    if (!read(path, bytes)) return nlohmann::json(nlohmann::json::value_t::discarded);
    return nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
}

std::vector<std::string> Resources::list(std::string_view prefix) const {
    return src_ ? src_->list(prefix) : std::vector<std::string>();
}

}  // namespace ui
