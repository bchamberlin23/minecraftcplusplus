#include "world/world_backup.h"

#include <libdeflate.h>

#include <chrono>
#include <ctime>
#include <fstream>

namespace {

// A ZIP writer (deflated entries, Zip64 records once the archive passes 4 GB or 65535 entries).
class ZipWriter {
public:
    explicit ZipWriter(const fs::path& file) : out_(file, std::ios::binary | std::ios::trunc) {
        comp_ = libdeflate_alloc_compressor(6);
    }
    ~ZipWriter() { libdeflate_free_compressor(comp_); }
    bool ok() const { return bool(out_) && comp_; }

    bool add(const std::string& name, const std::vector<u8>& data, std::time_t modified) {
        Entry e;
        e.name = name;
        e.crc = libdeflate_crc32(0, data.data(), data.size());
        e.size = data.size();
        e.offset = written_;
        std::tm tm{};
        localtime_s(&tm, &modified);
        e.time = u16(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
        e.date = u16(std::max(tm.tm_year - 80, 0) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
        std::vector<u8> packed(libdeflate_deflate_compress_bound(comp_, data.size()));
        size_t n = data.empty() ? 0 : libdeflate_deflate_compress(comp_, data.data(), data.size(), packed.data(), packed.size());
        e.method = 8;
        if (!n || n >= data.size()) e.method = 0, n = data.size();  // stored when deflating does not help
        e.packed = n;
        const bool zip64 = e.size >= 0xFFFFFFFFull || e.packed >= 0xFFFFFFFFull;
        std::vector<u8> h;
        put32(h, 0x04034b50);
        put16(h, zip64 ? 45 : 20), put16(h, 0x0800), put16(h, e.method), put16(h, e.time), put16(h, e.date);
        put32(h, e.crc);
        put32(h, zip64 ? 0xFFFFFFFFu : u32(e.packed)), put32(h, zip64 ? 0xFFFFFFFFu : u32(e.size));
        put16(h, u16(name.size())), put16(h, zip64 ? 20 : 0);
        h.insert(h.end(), name.begin(), name.end());
        if (zip64) put16(h, 1), put16(h, 16), put64(h, e.size), put64(h, e.packed);
        write(h.data(), h.size());
        write(e.method ? packed.data() : data.data(), n);
        entries_.push_back(std::move(e));
        return bool(out_);
    }

    bool finish() {
        const u64 cdStart = written_;
        for (const Entry& e : entries_) {
            const bool big = e.size >= 0xFFFFFFFFull || e.packed >= 0xFFFFFFFFull || e.offset >= 0xFFFFFFFFull;
            std::vector<u8> extra;
            if (big) {
                put16(extra, 1), put16(extra, 24);
                put64(extra, e.size), put64(extra, e.packed), put64(extra, e.offset);
            }
            std::vector<u8> h;
            put32(h, 0x02014b50);
            put16(h, big ? 45 : 20), put16(h, big ? 45 : 20), put16(h, 0x0800), put16(h, e.method), put16(h, e.time),
                put16(h, e.date);
            put32(h, e.crc);
            put32(h, big ? 0xFFFFFFFFu : u32(e.packed)), put32(h, big ? 0xFFFFFFFFu : u32(e.size));
            put16(h, u16(e.name.size())), put16(h, u16(extra.size())), put16(h, 0), put16(h, 0), put16(h, 0), put32(h, 0);
            put32(h, big ? 0xFFFFFFFFu : u32(e.offset));
            h.insert(h.end(), e.name.begin(), e.name.end());
            h.insert(h.end(), extra.begin(), extra.end());
            write(h.data(), h.size());
        }
        const u64 cdSize = written_ - cdStart, count = entries_.size();
        const bool zip64 = cdStart >= 0xFFFFFFFFull || count >= 0xFFFF;
        std::vector<u8> t;
        if (zip64) {
            const u64 record = written_;
            put32(t, 0x06064b50), put64(t, 44), put16(t, 45), put16(t, 45), put32(t, 0), put32(t, 0);
            put64(t, count), put64(t, count), put64(t, cdSize), put64(t, cdStart);
            put32(t, 0x07064b50), put32(t, 0), put64(t, record), put32(t, 1);
        }
        put32(t, 0x06054b50), put16(t, 0), put16(t, 0);
        put16(t, zip64 ? 0xFFFF : u16(count)), put16(t, zip64 ? 0xFFFF : u16(count));
        put32(t, zip64 ? 0xFFFFFFFFu : u32(cdSize)), put32(t, zip64 ? 0xFFFFFFFFu : u32(cdStart)), put16(t, 0);
        write(t.data(), t.size());
        out_.flush();
        return bool(out_);
    }
    u64 size() const { return written_; }

private:
    struct Entry {
        std::string name;
        u32 crc = 0;
        u64 size = 0, packed = 0, offset = 0;
        u16 method = 8, time = 0, date = 0;
    };
    static void put16(std::vector<u8>& v, u32 x) { v.push_back(u8(x)), v.push_back(u8(x >> 8)); }
    static void put32(std::vector<u8>& v, u32 x) { put16(v, x & 0xFFFF), put16(v, x >> 16); }
    static void put64(std::vector<u8>& v, u64 x) { put32(v, u32(x)), put32(v, u32(x >> 32)); }
    void write(const u8* p, size_t n) {
        out_.write(reinterpret_cast<const char*>(p), std::streamsize(n));
        written_ += n;
    }

    std::ofstream out_;
    libdeflate_compressor* comp_ = nullptr;
    std::vector<Entry> entries_;
    u64 written_ = 0;
};

std::string utf8Of(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

}  // namespace

std::optional<u64> makeWorldBackup(const fs::path& world, const fs::path& backups, fs::path* zip) {
    std::error_code ec;
    fs::create_directories(backups, ec);
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm);
    const std::string levelId = utf8Of(world.filename());
    // FileUtil.findAvailableName: "<name>.zip", else "<name> (1).zip"...
    const std::wstring base = std::wstring(stamp, stamp + std::strlen(stamp)) + L"_" + world.filename().wstring();
    fs::path file = backups / (base + L".zip");
    for (int i = 1; fs::exists(file, ec); ++i) file = backups / (base + L" (" + std::to_wstring(i) + L").zip");
    {
        ZipWriter z(file);
        if (!z.ok()) return std::nullopt;
        for (auto it = fs::recursive_directory_iterator(world, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code fe;
            if (!it->is_regular_file(fe) || it->path().filename() == "session.lock") continue;
            std::vector<u8> data;
            if (!readFile(it->path(), data)) {  // a file another program holds: skipped like an unreadable one
                LOG("backup: could not read %s", utf8Of(it->path()).c_str());
                continue;
            }
            const auto mtime = fs::last_write_time(it->path(), fe);
            const std::time_t t = fe ? now
                                     : std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(mtime));
            if (!z.add(levelId + "/" + utf8Of(fs::relative(it->path(), world, fe)), data, t)) {
                ec = std::make_error_code(std::errc::io_error);
                break;
            }
        }
        if (ec || !z.finish()) {
            fs::remove(file, ec);
            return std::nullopt;
        }
    }
    if (zip) *zip = file;
    return fs::file_size(file, ec);
}
