#pragma once
// Layered game files, like the game's pack repository: the installed client jar (plus the launcher's asset
// index objects) at the bottom, enabled packs (zips or folders) above it. A file comes from the topmost pack that
// has it; merged files (sounds.json, lang, tags, atlases) are read from every pack, bottom first.
//
// Two process-wide stacks exist: client() for assets/ (resource packs, Fabric mods' client assets) and data()
// for data/ (data packs of the world, global data packs, Fabric mods' data). The mod manager (mods/mod_manager.h)
// fills both at startup; until then each holds just the jar it was first asked for.
#include "assets/zip.h"
#include "core/common.h"

#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <unordered_map>

class AssetPack {
public:
    AssetPack() = default;
    AssetPack(const AssetPack&) = delete;
    AssetPack& operator=(const AssetPack&) = delete;
    // A zip or jar, optionally only its entries under `prefix` (a pack nested in an archive, e.g. the jar's
    // resourcepacks/high_contrast/). `shared` reuses an archive another pack has open.
    static std::unique_ptr<AssetPack> openZip(const fs::path& file, std::string id, std::string_view prefix = {},
                                              std::shared_ptr<ZipArchive> shared = {});
    static std::unique_ptr<AssetPack> openFolder(const fs::path& dir, std::string id);
    // The launcher's asset index for this client jar (versions/<v>/<v>.json -> assets/indexes/<id>.json):
    // "assets/minecraft/..." paths served from assets/objects/<hash>.
    static std::unique_ptr<AssetPack> openIndex(const fs::path& minecraftDir, const fs::path& jar, std::string id);

    const std::string& id() const { return id_; }
    const fs::path& path() const { return path_; }
    const std::shared_ptr<ZipArchive>& archive() const { return zip_; }
    size_t fileCount() const { return files_.size(); }

    bool has(std::string_view path) const { return lookup(path) != nullptr; }
    bool read(std::string_view path, std::vector<u8>& out) const;
    // Pack-relative paths starting with `prefix`, sorted.
    void list(std::string_view prefix, const std::function<void(const std::string&)>& fn) const;
    // First path segments below `root` ("assets/" -> namespaces).
    void children(std::string_view root, std::vector<std::string>& out) const;
    // The file on disk for folder and index packs (empty for zips).
    fs::path diskPath(std::string_view path) const;
    // pack.mcmeta "filter" (ResourceFilterSection): whether it hides `path` ("assets/<ns>/..." or "data/<ns>/...") in
    // the packs below this one.
    bool filters(std::string_view path) const;
    bool hasFilter() const { return !filter_.empty(); }
    // The game's pack formats (version.json pack_version), for pack.mcmeta "overlays": resource (assets/) and data
    // (data/) formats as major, minor. Set once the jar is known (AssetSource::addVanilla).
    static void setFormats(int resourceMajor, int resourceMinor, int dataMajor, int dataMinor);

private:
    struct File {
        std::string path;
        u32 entry = 0;     // zip entry index
        std::string disk;  // folder / index packs: file on disk (UTF-8)
    };
    const File* lookup(std::string_view path) const;
    void finish();  // sorts files_ and builds the index
    void applyMeta();  // pack.mcmeta: overlay directories over the root, the filter

    std::string id_;
    fs::path path_;
    std::shared_ptr<ZipArchive> zip_;
    std::vector<File> files_;  // sorted by path
    std::unordered_map<std::string_view, u32> index_;
    struct FilterEntry {  // ResourceFilterSection block entry: a missing pattern matches everything
        std::optional<std::regex> ns, path;
    };
    std::vector<FilterEntry> filter_;
};

class AssetSource {
public:
    void clear() { packs_.clear(); }
    void add(std::shared_ptr<AssetPack> pack) { packs_.push_back(std::move(pack)); }  // on top
    void truncate(size_t count) { packs_.resize(std::min(count, packs_.size())); }    // keeps the bottom packs
    size_t packCount() const { return packs_.size(); }
    const AssetPack& pack(size_t i) const { return *packs_[i]; }
    const std::shared_ptr<AssetPack>& packPtr(size_t i) const { return packs_[i]; }  // to share with another stack
    bool empty() const { return packs_.empty(); }

    bool read(std::string_view path, std::vector<u8>& out) const;  // topmost pack with the file (filters apply)
    bool exists(std::string_view path) const { return find(path) >= 0; }
    int find(std::string_view path) const;                          // topmost pack index, -1 if none
    // Every pack's files under `prefix` ending with `suffix`, unique and sorted.
    std::vector<std::string> list(std::string_view prefix, std::string_view suffix = {}) const;
    // Every pack that has `path`, bottom first (for files the game merges across packs).
    void readAll(std::string_view path, const std::function<void(size_t pack, const std::vector<u8>&)>& fn) const;
    // Namespaces under "assets/" or "data/" across all packs.
    std::vector<std::string> namespaces(std::string_view root) const;
    // A language ("en_us") merged like the game does: every namespace's lang file in every pack, later wins.
    void readLang(std::string_view code, std::unordered_map<std::string, std::string>& out) const;

    // Process-wide stacks. With a jar given, an empty stack first gets that jar (and, for client(), the
    // launcher's asset index beside it) as its only pack.
    static AssetSource& client(const fs::path& jar = {});
    static AssetSource& data(const fs::path& jar = {});
    // Bottom packs of a stack: the jar (+ the asset index for client assets).
    static void addVanilla(AssetSource& src, const fs::path& jar, bool clientAssets);

private:
    bool visible(size_t pack, std::string_view path) const;  // not filtered out by a pack above it
    std::vector<std::shared_ptr<AssetPack>> packs_;
};
