#pragma once
#include "core/common.h"

// Minimal read-only zip reader (enough for Minecraft's client jar). Reads are thread-safe.
// The file is memory-mapped: opening only parses the central directory, entries are read on demand.
class ZipArchive {
public:
    struct Entry {
        std::string name;
        u32 method = 0, compSize = 0, size = 0, localOffset = 0, crc = 0;
    };

    ZipArchive() = default;
    ~ZipArchive();
    ZipArchive(const ZipArchive&) = delete;
    ZipArchive& operator=(const ZipArchive&) = delete;

    bool open(const fs::path& path);
    const std::vector<Entry>& entries() const { return entries_; }
    bool read(const Entry& e, std::vector<u8>& out) const;

private:
    void close();
    const u8* data_ = nullptr;
    size_t size_ = 0;
    void* file_ = nullptr;  // HANDLEs of the mapping
    void* mapping_ = nullptr;
    std::vector<Entry> entries_;
};
