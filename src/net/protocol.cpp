#include "net/protocol.h"

#include "assets/assets.h"
#include "assets/game_reports.h"
#include "world/nbt.h"

#include <json/json.hpp>

#include <algorithm>

namespace net {

namespace {
using Lang = std::unordered_map<std::string, std::string>;

u64 loadBE64(const u8* p) {
    u64 v;
    std::memcpy(&v, p, 8);
    return _byteswap_uint64(v);
}

// Java-style format string with %s, %d, %n$s and %%.
std::string format(const std::string& fmt, const std::vector<std::string>& args) {
    std::string out;
    size_t next = 0;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%' || i + 1 >= fmt.size()) {
            out += fmt[i];
            continue;
        }
        if (fmt[i + 1] == '%') {
            out += '%';
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9') ++j;
        size_t arg = next;
        if (j < fmt.size() && fmt[j] == '$' && j > i + 1) arg = size_t(std::atoi(fmt.c_str() + i + 1) - 1), ++j;
        else if (j != i + 1) j = fmt.size();
        if (j < fmt.size() && (fmt[j] == 's' || fmt[j] == 'd')) {
            if (arg == next) ++next;
            if (arg < args.size()) out += args[arg];
            i = j;
            continue;
        }
        out += fmt[i];
    }
    return out;
}

std::string translate(const std::string& key, const std::string& fallback, const std::vector<std::string>& args,
                      const Lang* lang) {
    if (lang)
        if (auto it = lang->find(key); it != lang->end()) return format(it->second, args);
    if (!fallback.empty()) return format(fallback, args);
    std::string out = key;
    for (const std::string& a : args) out += " " + a;
    return out;
}

std::string nbtComponent(NbtReader& r, u8 tag, const Lang* lang) {
    switch (tag) {
        case TagString: return std::string(r.str());
        case TagByte: return std::to_string(i8(r.u8v()));
        case TagShort: return std::to_string(r.i16v());
        case TagInt: return std::to_string(r.i32v());
        case TagLong: return std::to_string(r.i64v());
        case TagFloat: return std::to_string(r.f32v());
        case TagDouble: return std::to_string(r.f64v());
        case TagList: {
            const u8 et = r.u8v();
            const i32 n = r.i32v();
            std::string s;
            for (i32 i = 0; i < n && r.ok; ++i) s += nbtComponent(r, et, lang);
            return s;
        }
        case TagCompound: {
            std::string text, key, fallback, extra;
            std::vector<std::string> with;
            u8 t;
            std::string_view name;
            while (r.field(t, name)) {
                if ((name == "text" || name.empty()) && t != TagCompound && t != TagList) {
                    text = nbtComponent(r, t, lang);
                } else if (name == "translate" && t == TagString) {
                    key = r.str();
                } else if (name == "fallback" && t == TagString) {
                    fallback = r.str();
                } else if ((name == "with" || name == "extra") && t == TagList) {
                    const u8 et = r.u8v();
                    const i32 n = r.i32v();
                    for (i32 i = 0; i < n && r.ok; ++i) {
                        std::string part = nbtComponent(r, et, lang);
                        if (name == "with") with.push_back(std::move(part));
                        else extra += part;
                    }
                } else {
                    r.skip(t);
                }
            }
            return (key.empty() ? text : translate(key, fallback, with, lang)) + extra;
        }
        default: r.skip(tag); return {};
    }
}

std::string jsonComponent(const nlohmann::json& j, const Lang* lang) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_number() || j.is_boolean()) return j.dump();
    std::string s;
    if (j.is_array()) {
        for (const auto& e : j) s += jsonComponent(e, lang);
        return s;
    }
    if (!j.is_object()) return s;
    if (j.contains("translate")) {
        std::vector<std::string> with;
        if (j.contains("with"))
            for (const auto& a : j["with"]) with.push_back(jsonComponent(a, lang));
        s = translate(j.value("translate", ""), j.value("fallback", ""), with, lang);
    } else if (j.contains("text")) {
        s = jsonComponent(j["text"], lang);
    }
    if (j.contains("extra"))
        for (const auto& e : j["extra"]) s += jsonComponent(e, lang);
    return s;
}
}  // namespace

u16 Reader::u16v() {
    if (!need(2)) return 0;
    const u16 v = u16((p[0] << 8) | p[1]);
    p += 2;
    return v;
}
u32 Reader::u32v() {
    if (!need(4)) return 0;
    u32 v;
    std::memcpy(&v, p, 4);
    p += 4;
    return _byteswap_ulong(v);
}
u64 Reader::u64v() {
    if (!need(8)) return 0;
    const u64 v = loadBE64(p);
    p += 8;
    return v;
}
float Reader::f32() {
    const u32 v = u32v();
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
double Reader::f64() {
    const u64 v = u64v();
    double d;
    std::memcpy(&d, &v, 8);
    return d;
}
i32 Reader::varInt() {
    u32 v = 0;
    for (int i = 0; i < 5; ++i) {
        const u8 b = u8v();
        v |= u32(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) return i32(v);
    }
    ok = false;
    p = end;
    return 0;
}
i64 Reader::varLong() {
    u64 v = 0;
    for (int i = 0; i < 10; ++i) {
        const u8 b = u8v();
        v |= u64(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) return i64(v);
    }
    ok = false;
    p = end;
    return 0;
}
std::string_view Reader::bytes(size_t n) {
    if (!need(n)) return {};
    std::string_view s(reinterpret_cast<const char*>(p), n);
    p += n;
    return s;
}
void Reader::blockPos(int& x, int& y, int& z) {
    const i64 v = i64v();
    x = int(v >> 38);
    y = int((v << 52) >> 52);
    z = int((v << 26) >> 38);
}
std::string_view Reader::nbt() {
    const u8* start = p;
    const u8 type = u8v();
    if (type == TagEnd) return {};
    NbtReader nr(p, left());
    nr.skip(type);
    if (!nr.ok) {
        ok = false;
        p = end;
        return {};
    }
    p = nr.p;
    return std::string_view(reinterpret_cast<const char*>(start), size_t(p - start));
}

void Writer::u16v(u16 v) {
    buf.push_back(u8(v >> 8));
    buf.push_back(u8(v));
}
void Writer::u32v(u32 v) {
    for (int s = 24; s >= 0; s -= 8) buf.push_back(u8(v >> s));
}
void Writer::u64v(u64 v) {
    for (int s = 56; s >= 0; s -= 8) buf.push_back(u8(v >> s));
}
void Writer::f32(float v) {
    u32 u;
    std::memcpy(&u, &v, 4);
    u32v(u);
}
void Writer::f64(double v) {
    u64 u;
    std::memcpy(&u, &v, 8);
    u64v(u);
}
void Writer::varInt(i32 value) {
    u32 v = u32(value);
    while (v >= 0x80) {
        buf.push_back(u8(v | 0x80));
        v >>= 7;
    }
    buf.push_back(u8(v));
}
void Writer::varLong(i64 value) {
    u64 v = u64(value);
    while (v >= 0x80) {
        buf.push_back(u8(v | 0x80));
        v >>= 7;
    }
    buf.push_back(u8(v));
}
void Writer::bytes(const void* data, size_t n) {
    const u8* b = static_cast<const u8*>(data);
    buf.insert(buf.end(), b, b + n);
}
void Writer::str(std::string_view s) {
    varInt(i32(s.size()));
    bytes(s.data(), s.size());
}

std::string componentText(std::string_view nbt, const Lang* lang) {
    if (nbt.empty()) return {};
    NbtReader r(reinterpret_cast<const u8*>(nbt.data()) + 1, nbt.size() - 1);
    return nbtComponent(r, u8(nbt[0]), lang);
}

std::string jsonComponentText(std::string_view json, const Lang* lang) {
    const nlohmann::json j = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    return j.is_discarded() ? std::string(json) : jsonComponent(j, lang);
}

int ceilLog2(u32 n) {
    int bits = 0;
    while ((u64(1) << bits) < n) ++bits;
    return bits;
}

void BlockIds::init(const GameReports& reports, BlockRegistry& registry) {
    registry_ = &registry;
    int maxId = -1;
    for (const auto& [key, id] : reports.stateIds()) maxId = std::max(maxId, id);
    names_.assign(size_t(maxId + 1), {});
    for (const auto& [key, id] : reports.stateIds())
        if (id >= 0) names_[size_t(id)] = key;
    map_.assign(names_.size(), kUnresolved);
    globalBits_ = std::max(1, ceilLog2(u32(names_.size())));
}

void BlockIds::initTable(std::vector<std::string> names, BlockRegistry& registry) {
    registry_ = &registry;
    names_ = std::move(names);
    map_.assign(names_.size(), kUnresolved);
    globalBits_ = std::max(1, ceilLog2(u32(names_.size())));
}

u16 BlockIds::resolve(u32 globalId) {
    const std::string& name = names_[globalId];
    return map_[globalId] = name.empty() ? u16(0) : registry_->idFor(name);
}

bool decodeSections(Reader& r, int count, BlockIds& ids, int biomeBits, std::vector<World::PackedSection>& out,
                    const std::vector<u8>* biomeIds) {
    thread_local std::vector<u32> seen(65536, 0);
    thread_local std::vector<u16> slot(65536, 0);
    thread_local u32 stamp = 0;
    out.clear();
    out.resize(size_t(count) + 2);
    for (int i = 0; i < count && r.ok; ++i) {
        World::PackedSection& s = out[size_t(i) + 1];
        const i16 nonEmpty = r.i16v();
        r.i16v();  // fluid count
        const u8 bits = r.u8v();
        if (bits == 0) {
            s.palette.assign(1, ids.local(u32(r.varInt())));
        } else {
            // Same storage choice as the game: linear/hash palettes up to 8 bits, direct ids above.
            const int mem = bits <= 4 ? 4 : bits <= 8 ? bits : ids.globalBits();
            u16 pal[256];
            int n = 0;
            if (bits <= 8) {
                n = r.varInt();
                if (n <= 0 || n > 256) return false;
                for (int k = 0; k < n; ++k) pal[k] = ids.local(u32(r.varInt()));
            }
            const u32 perWord = 64 / u32(mem), words = (4096 + perWord - 1) / perWord;
            if (!r.need(size_t(words) * 8)) return false;
            if (bits <= 8 && n == 1) {
                s.palette.assign(1, pal[0]);
            } else if (bits <= 8) {  // our storage uses the same packing: copy the words
                s.palette.assign(pal, pal + n);
                s.bits = u8(mem);
                s.words.resize(words);
                for (u32 w = 0; w < words; ++w) s.words[w] = loadBE64(r.p + size_t(w) * 8);
            } else {  // direct global ids: build a palette and repack
                if (++stamp == 0) std::fill(seen.begin(), seen.end(), 0), stamp = 1;
                u16 index[4096];
                const u64 mask = (u64(1) << mem) - 1;
                u32 idx = 0;
                for (u32 w = 0; w < words && idx < 4096; ++w) {
                    u64 v = loadBE64(r.p + size_t(w) * 8);
                    for (u32 k = 0; k < perWord && idx < 4096; ++k, v >>= mem) {
                        const u16 id = ids.local(u32(v & mask));
                        if (seen[id] != stamp) {
                            seen[id] = stamp;
                            slot[id] = u16(s.palette.size());
                            s.palette.push_back(id);
                        }
                        index[idx++] = slot[id];
                    }
                }
                if (s.palette.size() > 1) {
                    const u32 b = u32(std::max(4, ceilLog2(u32(s.palette.size()))));
                    const u32 per = 64 / b;
                    s.bits = u8(b);
                    s.words.assign((4096 + per - 1) / per, 0);
                    for (u32 k = 0; k < 4096; ++k) s.words[k / per] |= u64(index[k]) << ((k % per) * b);
                }
            }
            r.p += size_t(words) * 8;
        }
        if (nonEmpty == 0) {  // only air
            s.palette.clear();
            s.words.clear();
            s.bits = 0;
        }
        // Biomes (4x4x4, index (y * 4 + z) * 4 + x): keep the lowest layer for biome colours.
        auto worldBiome = [&](i32 id) {
            return biomeIds && id >= 0 && size_t(id) < biomeIds->size() ? (*biomeIds)[size_t(id)] : World::kUnknownBiome;
        };
        const u8 bb = r.u8v();
        if (bb == 0) {
            std::fill(std::begin(s.biomes), std::end(s.biomes), worldBiome(r.varInt()));
        } else {
            const int mem = bb <= 3 ? bb : biomeBits;
            i32 palette[8] = {};
            i32 n = 0;
            if (bb <= 3) {
                n = r.varInt();
                for (i32 k = 0; k < n && r.ok; ++k) {
                    const i32 v = r.varInt();
                    if (k < 8) palette[k] = v;
                }
            }
            const u32 bitsPer = u32(std::clamp(mem, 1, 32)), perWord = 64 / bitsPer;
            const size_t words = (64 + perWord - 1) / perWord;
            if (r.need(words * 8)) {
                for (int q = 0; q < 16; ++q) {  // the first 16 entries live in the first words
                    u64 v = 0;
                    for (int b = 0; b < 8; ++b) v = (v << 8) | r.p[size_t(q / perWord) * 8 + size_t(b)];
                    const i32 e = i32((v >> ((q % perWord) * bitsPer)) & ((u64(1) << bitsPer) - 1));
                    s.biomes[q] = worldBiome(bb <= 3 ? (e < n && e < 8 ? palette[e] : -1) : e);
                }
            }
            r.skip(words * 8);
        }
        s.hasBiomes = biomeIds != nullptr;
    }
    return r.ok;
}

bool readLight(Reader& r, int minSy, int lightSections, std::vector<LightSection>& out) {
    // Masks are BitSets sent as byte arrays (bit i = byte i / 8, bit i % 8).
    const std::string_view masks[4] = {r.byteArray(), r.byteArray(), r.byteArray(), r.byteArray()};
    auto bit = [](std::string_view m, int i) { return size_t(i >> 3) < m.size() && ((u8(m[size_t(i >> 3)]) >> (i & 7)) & 1); };
    for (int layer = 0; layer < 2 && r.ok; ++layer) {  // sky arrays, then block arrays
        const i32 n = r.varInt();
        int next = 0;
        for (i32 k = 0; k < n && r.ok; ++k) {
            const std::string_view data = r.byteArray();
            while (next < lightSections && !bit(masks[layer], next)) ++next;
            if (next >= lightSections || data.size() != 2048) continue;
            LightSection& s = out.emplace_back();
            s.sy = minSy - 1 + next++;
            s.sky = layer == 0;
            s.data.assign(data.begin(), data.end());
        }
        for (int i = 0; i < lightSections; ++i)
            if (bit(masks[2 + layer], i) && !bit(masks[layer], i)) out.push_back({minSy - 1 + i, layer == 0, {}});
    }
    return r.ok;
}

}  // namespace net
