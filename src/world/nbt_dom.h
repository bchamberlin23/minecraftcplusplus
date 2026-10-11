#pragma once
#include "world/nbt.h"

// Small NBT document model for rewriting saved data (chunks, level.dat) without losing
// anything this engine does not understand. Loading uses the streaming NbtReader.
struct NbtValue {
    u8 type = TagEnd;
    i64 i = 0;  // byte / short / int / long
    double d = 0;
    std::string s;
    std::vector<u8> bytes;
    std::vector<i32> ints;
    std::vector<i64> longs;
    u8 listType = TagEnd;
    std::vector<NbtValue> items;      // list elements or compound values
    std::vector<std::string> names;   // compound keys, parallel to items

    static NbtValue make(u8 t) {
        NbtValue v;
        v.type = t;
        return v;
    }
    NbtValue* find(std::string_view key) {
        for (size_t k = 0; k < names.size(); ++k)
            if (names[k] == key) return &items[k];
        return nullptr;
    }
    NbtValue& set(std::string_view key, NbtValue v) {
        if (NbtValue* e = find(key)) return *e = std::move(v);
        names.emplace_back(key);
        items.push_back(std::move(v));
        return items.back();
    }
    void erase(std::string_view key) {
        for (size_t k = 0; k < names.size(); ++k)
            if (names[k] == key) {
                names.erase(names.begin() + k);
                items.erase(items.begin() + k);
                return;
            }
    }
};

bool nbtRead(const u8* data, size_t size, NbtValue& root, std::string* rootName = nullptr);
void nbtWrite(const NbtValue& root, std::vector<u8>& out, std::string_view rootName = "");
// Network NBT (NbtIo.writeAnyTag without a name): the type byte, then the payload; any tag type.
bool nbtReadNetwork(const u8* data, size_t size, NbtValue& root);
void nbtWriteNetwork(const NbtValue& root, std::vector<u8>& out);
