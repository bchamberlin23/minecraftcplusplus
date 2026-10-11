#include "assets/zip.h"

#include <libdeflate.h>

#include <windows.h>

namespace {
u16 rd16(const u8* p) { return u16(p[0] | (p[1] << 8)); }
u32 rd32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }

struct Inflater {
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    ~Inflater() { libdeflate_free_decompressor(d); }
};
}  // namespace

ZipArchive::~ZipArchive() { close(); }

void ZipArchive::close() {
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(mapping_);
    if (file_ && file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    data_ = nullptr, mapping_ = file_ = nullptr, size_ = 0;
    entries_.clear();
}

bool ZipArchive::open(const fs::path& path) {
    close();
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        file_ = nullptr;
        return false;
    }
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(file_, &fileSize) || fileSize.QuadPart < 22) return close(), false;
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping_) return close(), false;
    data_ = static_cast<const u8*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
    if (!data_) return close(), false;
    size_ = size_t(fileSize.QuadPart);
    size_t minPos = size_ > 65557 ? size_ - 65557 : 0;
    size_t eocd = SIZE_MAX;
    for (size_t i = size_ - 22 + 1; i-- > minPos;) {
        if (rd32(&data_[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd == SIZE_MAX) return close(), false;
    u16 count = rd16(&data_[eocd + 10]);
    size_t p = rd32(&data_[eocd + 16]);
    entries_.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        if (p + 46 > size_ || rd32(&data_[p]) != 0x02014b50) return close(), false;
        Entry e;
        e.method = rd16(&data_[p + 10]);
        e.crc = rd32(&data_[p + 16]);
        e.compSize = rd32(&data_[p + 20]);
        e.size = rd32(&data_[p + 24]);
        u16 nameLen = rd16(&data_[p + 28]), extraLen = rd16(&data_[p + 30]), commentLen = rd16(&data_[p + 32]);
        e.localOffset = rd32(&data_[p + 42]);
        if (p + 46 + nameLen > size_) return close(), false;
        e.name.assign(reinterpret_cast<const char*>(&data_[p + 46]), nameLen);
        entries_.push_back(std::move(e));
        p += 46 + size_t(nameLen) + extraLen + commentLen;
    }
    return true;
}

bool ZipArchive::read(const Entry& e, std::vector<u8>& out) const {
    size_t p = e.localOffset;
    if (!data_ || p + 30 > size_ || rd32(&data_[p]) != 0x04034b50) return false;
    size_t start = p + 30 + rd16(&data_[p + 26]) + rd16(&data_[p + 28]);
    if (start + e.compSize > size_) return false;
    out.resize(e.size);
    if (e.size == 0) return true;
    if (e.method == 0) {
        if (e.compSize != e.size) return false;
        std::memcpy(out.data(), &data_[start], e.size);
        return true;
    }
    if (e.method != 8) return false;
    thread_local Inflater inflater;
    size_t actual = 0;
    return libdeflate_deflate_decompress(inflater.d, &data_[start], e.compSize, out.data(), out.size(), &actual) ==
               LIBDEFLATE_SUCCESS &&
           actual == e.size;
}
