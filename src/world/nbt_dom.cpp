#include "world/nbt_dom.h"

namespace {

bool readPayload(NbtReader& r, u8 type, NbtValue& v, int depth) {
    if (depth > 512) return false;
    v.type = type;
    switch (type) {
        case TagByte: v.i = i8(r.u8v()); break;
        case TagShort: v.i = r.i16v(); break;
        case TagInt: v.i = r.i32v(); break;
        case TagLong: v.i = r.i64v(); break;
        case TagFloat: v.d = r.f32v(); break;
        case TagDouble: v.d = r.f64v(); break;
        case TagString: v.s = std::string(r.str()); break;
        case TagByteArray: {
            i32 n = r.i32v();
            if (n < 0 || !r.need(size_t(n))) return false;
            v.bytes.assign(r.p, r.p + n);
            r.p += n;
            break;
        }
        case TagIntArray: {
            i32 n = r.i32v();
            if (n < 0 || !r.need(size_t(n) * 4)) return false;
            v.ints.resize(size_t(n));
            for (auto& x : v.ints) x = r.i32v();
            break;
        }
        case TagLongArray: {
            i32 n = r.i32v();
            if (n < 0 || !r.need(size_t(n) * 8)) return false;
            v.longs.resize(size_t(n));
            for (auto& x : v.longs) x = r.i64v();
            break;
        }
        case TagList: {
            v.listType = r.u8v();
            i32 n = r.i32v();
            if (n < 0) n = 0;
            v.items.resize(size_t(n));
            for (auto& e : v.items)
                if (!readPayload(r, v.listType, e, depth + 1)) return false;
            break;
        }
        case TagCompound: {
            u8 t;
            std::string_view name;
            while (r.field(t, name)) {
                v.names.emplace_back(name);
                v.items.emplace_back();
                if (!readPayload(r, t, v.items.back(), depth + 1)) return false;
            }
            break;
        }
        default: return false;
    }
    return r.ok;
}

struct Writer {
    std::vector<u8>& out;
    void u8v(u8 v) { out.push_back(v); }
    void u16v(u16 v) { u8v(u8(v >> 8)), u8v(u8(v)); }
    void u32v(u32 v) { u16v(u16(v >> 16)), u16v(u16(v)); }
    void u64v(u64 v) { u32v(u32(v >> 32)), u32v(u32(v)); }
    void str(std::string_view s) {
        u16v(u16(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    }
    void payload(const NbtValue& v) {
        switch (v.type) {
            case TagByte: u8v(u8(v.i)); break;
            case TagShort: u16v(u16(v.i)); break;
            case TagInt: u32v(u32(v.i)); break;
            case TagLong: u64v(u64(v.i)); break;
            case TagFloat: {
                float f = float(v.d);
                u32 b;
                std::memcpy(&b, &f, 4);
                u32v(b);
                break;
            }
            case TagDouble: {
                u64 b;
                std::memcpy(&b, &v.d, 8);
                u64v(b);
                break;
            }
            case TagString: str(v.s); break;
            case TagByteArray:
                u32v(u32(v.bytes.size()));
                out.insert(out.end(), v.bytes.begin(), v.bytes.end());
                break;
            case TagIntArray:
                u32v(u32(v.ints.size()));
                for (i32 x : v.ints) u32v(u32(x));
                break;
            case TagLongArray:
                u32v(u32(v.longs.size()));
                for (i64 x : v.longs) u64v(u64(x));
                break;
            case TagList:
                u8v(v.items.empty() && v.listType == TagEnd ? TagEnd : v.listType);
                u32v(u32(v.items.size()));
                for (const auto& e : v.items) payload(e);
                break;
            case TagCompound:
                for (size_t k = 0; k < v.items.size(); ++k) {
                    u8v(v.items[k].type);
                    str(v.names[k]);
                    payload(v.items[k]);
                }
                u8v(TagEnd);
                break;
        }
    }
};

}  // namespace

bool nbtRead(const u8* data, size_t size, NbtValue& root, std::string* rootName) {
    NbtReader r(data, size);
    u8 t = r.u8v();
    if (t != TagCompound) return false;
    std::string_view name = r.str();
    if (rootName) *rootName = std::string(name);
    return readPayload(r, TagCompound, root, 0);
}

void nbtWrite(const NbtValue& root, std::vector<u8>& out, std::string_view rootName) {
    Writer w{out};
    w.u8v(TagCompound);
    w.str(rootName);
    w.payload(root);
}
