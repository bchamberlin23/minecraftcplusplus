#pragma once
#include "core/common.h"

#include <cstdlib>

enum : u8 {
    TagEnd = 0,
    TagByte,
    TagShort,
    TagInt,
    TagLong,
    TagFloat,
    TagDouble,
    TagByteArray,
    TagString,
    TagList,
    TagCompound,
    TagIntArray,
    TagLongArray,
};

// Streaming big-endian NBT reader. No DOM: callers walk compounds with field() and
// skip() whatever they do not need. Any overrun sets ok = false and parks at the end.
struct NbtReader {
    const u8* p;
    const u8* end;
    bool ok = true;

    NbtReader(const u8* data, size_t size) : p(data), end(data + size) {}

    bool need(size_t n) {
        if (!ok || size_t(end - p) < n) {
            ok = false;
            p = end;
            return false;
        }
        return true;
    }
    void advance(size_t n) {
        if (need(n)) p += n;
    }
    u8 u8v() { return need(1) ? *p++ : 0; }
    i16 i16v() {
        if (!need(2)) return 0;
        u16 v;
        std::memcpy(&v, p, 2);
        p += 2;
        return i16(_byteswap_ushort(v));
    }
    i32 i32v() {
        if (!need(4)) return 0;
        u32 v;
        std::memcpy(&v, p, 4);
        p += 4;
        return i32(_byteswap_ulong(v));
    }
    i64 i64v() {
        if (!need(8)) return 0;
        u64 v;
        std::memcpy(&v, p, 8);
        p += 8;
        return i64(_byteswap_uint64(v));
    }
    float f32v() {
        u32 v = u32(i32v());
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    double f64v() {
        u64 v = u64(i64v());
        double d;
        std::memcpy(&d, &v, 8);
        return d;
    }
    std::string_view str() {
        u16 n = u16(i16v());
        if (!need(n)) return {};
        std::string_view s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
    // Reads the next named tag of a compound. Returns false at TagEnd or on error.
    bool field(u8& tag, std::string_view& name) {
        tag = u8v();
        if (!ok || tag == TagEnd) return false;
        name = str();
        return ok;
    }
    static size_t fixedSize(u8 tag) {
        switch (tag) {
            case TagByte: return 1;
            case TagShort: return 2;
            case TagInt:
            case TagFloat: return 4;
            case TagLong:
            case TagDouble: return 8;
            default: return 0;
        }
    }
    void skip(u8 tag) {
        if (size_t fs = fixedSize(tag)) return advance(fs);
        switch (tag) {
            case TagByteArray: return advance(size_t(u32(i32v())));
            case TagString: return advance(u16(i16v()));
            case TagIntArray: return advance(size_t(u32(i32v())) * 4);
            case TagLongArray: return advance(size_t(u32(i32v())) * 8);
            case TagList: {
                u8 et = u8v();
                i32 n = i32v();
                if (n <= 0) return;
                if (size_t fs = fixedSize(et)) return advance(fs * size_t(n));
                for (i32 i = 0; i < n && ok; ++i) skip(et);
                return;
            }
            case TagCompound: {
                u8 t;
                std::string_view name;
                while (field(t, name)) skip(t);
                return;
            }
            default: ok = false; p = end;
        }
    }
};
