#pragma once
// Sound definitions from the installed game's asset store: the launcher's asset index maps
// "minecraft/sounds.json" and "minecraft/sounds/<name>.ogg" to hashed object files. Events are
// resolved to files with the game's weighted choice (SoundManager / WeighedSoundEvents).
#include "core/common.h"

#include <functional>
#include <unordered_map>

namespace audio {

enum class Category : u8 { Master, Music, Record, Weather, Block, Hostile, Neutral, Player, Ambient, Voice, Ui, Count };
constexpr int kCategoryCount = int(Category::Count);
const char* categoryName(Category c);  // the game's option names: "master", "music", "record", ...

// java.util.Random / the game's LegacyRandomSource (RandomSource.create(seed)).
struct JavaRandom {
    u64 seed = 0;
    explicit JavaRandom(i64 s = 0) { setSeed(s); }
    void setSeed(i64 s) { seed = (u64(s) ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1); }
    int next(int bits) {
        seed = (seed * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
        return int(i64(seed) >> (48 - bits));
    }
    int nextInt(int bound) {
        if ((bound & (bound - 1)) == 0) return int((i64(bound) * i64(next(31))) >> 31);
        int bits, val;
        do {
            bits = next(31);
            val = bits % bound;
        } while (bits - val + (bound - 1) < 0);
        return val;
    }
    int nextInt(int lo, int hi) { return lo >= hi ? lo : nextInt(hi - lo + 1) + lo; }  // Mth.nextInt (inclusive)
    float nextFloat() { return float(next(24)) * 5.9604645E-8f; }
    i64 nextLong() {
        const i64 upper = next(32);
        const i64 lower = next(32);
        return i64(u64(upper) << 32) + lower;
    }
    double nextDouble() {
        const i64 upper = next(26);
        const i64 lower = next(27);
        return double((upper << 27) + lower) * double(1.110223E-16f);
    }
};

struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};
template <class V>
using StringMap = std::unordered_map<std::string, V, StringHash, std::equal_to<>>;

// A sound picked for one play: the file and the multipliers of the entries that led to it.
struct ResolvedSound {
    int file = -1;
    float volume = 1, pitch = 1;
    bool stream = false;
    int attenuation = 16;  // blocks, scaled by max(volume, 1) when played
};

class SoundBank {
public:
    // assetIndexId: "assetIndex"."id" of the version JSON. Definitions come from the game's assets with the enabled
    // resource packs (AssetSource::client): every namespace's sounds.json merged bottom first ("replace" honoured).
    bool load(const fs::path& minecraftDir, const std::string& assetIndexId);

    int eventId(std::string_view name) const;  // "minecraft:block.stone.break" or "block.stone.break"; -1 unknown
    const std::string& eventName(int id) const { return events_[id].name; }
    size_t eventCount() const { return events_.size(); }
    // WeighedSoundEvents.getSound: false when the event has nothing playable.
    bool resolve(int event, JavaRandom& rng, ResolvedSound& out) const;

    size_t fileCount() const { return files_.size(); }
    const std::string& fileName(int file) const { return files_[file].name; }  // "dig/stone1"
    const fs::path& filePath(int file) const { return files_[file].path; }  // empty for files inside a pack zip
    bool readBytes(int file, std::vector<u8>& out) const;  // the encoded .ogg
    const std::vector<int>& preloadFiles() const { return preload_; }

private:
    struct Entry {
        int target = -1;  // file index, or event index when isEvent
        bool isEvent = false, stream = false, preload = false;
        float volume = 1, pitch = 1;
        int weight = 1, attenuation = 16;
    };
    struct Event {
        std::string name;  // "minecraft:..."
        std::vector<Entry> entries;
    };
    struct File {
        std::string name;
        fs::path path;
        int pack = -1;      // AssetSource::client() pack holding it
        std::string asset;  // "assets/<ns>/sounds/<name>.ogg"
    };
    int weight(int event, int depth) const;
    bool pick(int event, JavaRandom& rng, ResolvedSound& out, int depth) const;

    std::vector<Event> events_;
    StringMap<int> eventIds_;  // without the "minecraft:" prefix
    std::vector<File> files_;
    std::vector<int> preload_;
};

}  // namespace audio
