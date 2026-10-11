#pragma once
// Shader pack files (OptiFine / Iris format): a .zip or a folder whose "shaders" directory holds the programs. The
// shaders directory may sit one level down (GitHub archives: "Pack-main/shaders/"). Paths are relative to it, with
// forward slashes ("world0/gbuffers_terrain.vsh", "lib/common.glsl"). Read-only.
#include "core/common.h"

#include <memory>
#include <unordered_map>

class ZipArchive;

namespace packs {

class PackSource {
public:
    PackSource();
    ~PackSource();
    PackSource(const PackSource&) = delete;
    PackSource& operator=(const PackSource&) = delete;

    bool open(const fs::path& path, std::string& error);
    const fs::path& path() const { return path_; }
    const std::string& name() const { return name_; }  // file / folder name ("BSL_v10.1.8.zip")

    // Relative to the shaders directory; "..", "." and repeated slashes are resolved. Case-insensitive fallback.
    bool read(std::string_view rel, std::string& out) const;
    bool readBytes(std::string_view rel, std::vector<u8>& out) const;
    bool exists(std::string_view rel) const;
    // Files (relative paths) directly inside a directory of the shaders tree ("" = the shaders directory itself).
    std::vector<std::string> list(std::string_view dir) const;
    // Every file of the shaders tree.
    const std::vector<std::string>& files() const { return files_; }

    static std::string normalize(std::string_view path);  // resolves . / .. / backslashes; no leading slash
    // A file that is not in the pack (built-in fallback programs); the pack's own files win.
    void addVirtual(std::string_view rel, std::string text);

private:
    const std::string* find(std::string_view rel) const;  // canonical stored name
    fs::path path_;
    std::string name_;
    std::unique_ptr<ZipArchive> zip_;
    std::string zipPrefix_;  // entry prefix of the shaders directory inside the zip ("Pack-main/shaders/")
    fs::path folder_;        // the shaders directory of a folder pack
    std::vector<std::string> files_;
    std::unordered_map<std::string, size_t> index_;       // exact relative path -> files_ index
    std::unordered_map<std::string, size_t> lowerIndex_;  // lower-case relative path -> files_ index
    std::unordered_map<std::string, size_t> zipEntry_;    // relative path -> zip entry index
    std::unordered_map<std::string, std::string> virtual_;  // relative path -> text (addVirtual)
};

}  // namespace packs
