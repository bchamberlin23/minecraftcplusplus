#include "entity/skin_cache.h"

#include <windows.h>
#include <urlmon.h>
#include <json/json.hpp>
#include <stb/stb_image.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "urlmon.lib")

namespace ent {
namespace {

using json = nlohmann::json;

std::string base64Decode(const std::string& in) {
    std::string out;
    int val = 0, bits = -8;
    for (char ch : in) {
        int d;
        if (ch >= 'A' && ch <= 'Z') d = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') d = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') d = ch - '0' + 52;
        else if (ch == '+' || ch == '-') d = 62;
        else if (ch == '/' || ch == '_') d = 63;
        else continue;
        val = (val << 6) | d, bits += 6;
        if (bits >= 0) out += char((val >> bits) & 0xFF), bits -= 8;
    }
    return out;
}

bool officialUrl(const std::string& url) {
    return url.starts_with("http://textures.minecraft.net/texture/") || url.starts_with("https://textures.minecraft.net/texture/");
}

struct Entry {
    std::atomic<int> state{0};
    fs::path file;
    bool loaded = false;
    std::vector<u8> rgba;
    int w = 0, h = 0;
};
std::unordered_map<std::string, std::unique_ptr<Entry>> g_entries;

// SkinTextureDownloader.processLegacySkin: 64 x 32 skins grow to 64 x 64 (the legacy limbs mirrored over to the left
// side), the base layers opaque, a fully opaque legacy hat layer see-through.
bool processLegacySkin(std::vector<u8>& rgba, int w, int& h) {
    if (w != 64 || (h != 32 && h != 64)) return false;
    const bool legacy = h == 32;
    if (legacy) {
        rgba.resize(64 * 64 * 4, 0);
        h = 64;
        auto copyRect = [&](int sx, int sy, int ox, int oy, int sw, int sh) {  // NativeImage.copyRect, swapX
            for (int y = 0; y < sh; ++y)
                for (int x = 0; x < sw; ++x)
                    std::memcpy(&rgba[(size_t(sy + oy + y) * 64 + size_t(sx + ox + sw - 1 - x)) * 4], &rgba[(size_t(sy + y) * 64 + size_t(sx + x)) * 4], 4);
        };
        copyRect(4, 16, 16, 32, 4, 4), copyRect(8, 16, 16, 32, 4, 4);
        copyRect(0, 20, 24, 32, 4, 12), copyRect(4, 20, 16, 32, 4, 12), copyRect(8, 20, 8, 32, 4, 12), copyRect(12, 20, 16, 32, 4, 12);
        copyRect(44, 16, -8, 32, 4, 4), copyRect(48, 16, -8, 32, 4, 4);
        copyRect(40, 20, 0, 32, 4, 12), copyRect(44, 20, -8, 32, 4, 12), copyRect(48, 20, -16, 32, 4, 12), copyRect(52, 20, -8, 32, 4, 12);
    }
    auto alpha = [&](int x, int y) -> u8& { return rgba[(size_t(y) * 64 + x) * 4 + 3]; };
    auto noAlpha = [&](int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) alpha(x, y) = 255;
    };
    noAlpha(0, 0, 32, 16);
    if (legacy) {  // doNotchTransparencyHack
        bool translucent = false;
        for (int y = 0; y < 32 && !translucent; ++y)
            for (int x = 32; x < 64 && !translucent; ++x) translucent = alpha(x, y) < 128;
        if (!translucent)
            for (int y = 0; y < 32; ++y)
                for (int x = 32; x < 64; ++x) alpha(x, y) = 0;
    }
    noAlpha(0, 16, 64, 32);
    noAlpha(16, 48, 48, 64);
    return true;
}

}  // namespace

bool parseProfileTextures(const std::string& property, ProfileTextures& out) {
    out = {};
    if (property.empty()) return false;
    const json j = json::parse(base64Decode(property), nullptr, false);
    if (!j.is_object() || !j.contains("textures") || !j["textures"].is_object()) return false;
    const json& t = j["textures"];
    auto url = [&](const char* key) -> std::string {
        if (!t.contains(key) || !t[key].is_object() || !t[key].contains("url") || !t[key]["url"].is_string()) return {};
        std::string u = t[key]["url"].get<std::string>();
        return officialUrl(u) ? u : std::string();
    };
    out.skinUrl = url("SKIN"), out.capeUrl = url("CAPE");
    if (t.contains("SKIN") && t["SKIN"].is_object() && t["SKIN"].contains("metadata") && t["SKIN"]["metadata"].is_object()) {
        const json& m = t["SKIN"]["metadata"];
        out.slim = m.contains("model") && m["model"].is_string() && m["model"].get<std::string>() == "slim";
    }
    return !out.skinUrl.empty() || !out.capeUrl.empty();
}

std::string profileTextureHash(const std::string& url) { return url.substr(url.rfind('/') + 1); }

int fetchProfileTexture(const std::string& url, bool skin, std::vector<u8>* rgba, int* w, int* h) {
    if (!officialUrl(url)) return 3;
    auto& entry = g_entries[url];
    if (!entry) {
        entry = std::make_unique<Entry>();
        const std::string hash = profileTextureHash(url);
        bool safe = !hash.empty() && hash.size() <= 128;
        for (char ch : hash) safe = safe && std::isalnum(u8(ch)) != 0;
        const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
        if (!safe || !local) {
            entry->state = 3;
        } else {
            const fs::path dir = fs::path(local) / L"minecraftCplusplus" / L"skins";
            entry->file = dir / (hash + ".png");
            std::error_code ec;
            if (fs::exists(entry->file, ec)) {
                entry->state = 2;
            } else {
                entry->state = 1;
                fs::create_directories(dir, ec);
                std::thread([e = entry.get(), url] {
                    const fs::path tmp = fs::path(e->file).concat(".download");
                    const bool ok = URLDownloadToFileW(nullptr, std::wstring(url.begin(), url.end()).c_str(), tmp.c_str(), 0, nullptr) == S_OK;
                    std::error_code ec2;
                    if (ok) fs::rename(tmp, e->file, ec2);
                    e->state = ok && !ec2 ? 2 : 3;
                }).detach();
            }
        }
    }
    if (entry->state == 2 && !entry->loaded) {
        entry->loaded = true;
        std::vector<u8> bytes;
        int iw = 0, ih = 0, comp = 0;
        u8* px = readFile(entry->file, bytes) ? stbi_load_from_memory(bytes.data(), int(bytes.size()), &iw, &ih, &comp, 4) : nullptr;
        if (px) {
            entry->rgba.assign(px, px + size_t(iw) * ih * 4);
            stbi_image_free(px);
            entry->w = iw, entry->h = ih;
            if (skin && !processLegacySkin(entry->rgba, entry->w, entry->h)) entry->state = 3;  // incorrectly sized: discarded
        } else {
            entry->state = 3;
        }
    }
    const int state = entry->state;
    if (state == 2) {
        if (rgba) *rgba = entry->rgba;
        if (w) *w = entry->w;
        if (h) *h = entry->h;
    }
    return state;
}

}  // namespace ent
