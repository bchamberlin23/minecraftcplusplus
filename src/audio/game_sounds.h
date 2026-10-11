#pragma once
// Sound data that lives in the game's code rather than its data files: every block's SoundType
// (break/step/place/hit/fall events, volume, pitch), plus the block tags and the audio environment
// attributes (background music, ambient sounds) of dimension types and biomes. Produced once per
// version by a small Java program run against the installed client jar with the launcher's Java
// runtime (single-file source launch), and cached as JSON.
#include "audio/sound_bank.h"

namespace audio {

struct BlockSoundType {
    int breakSound = -1, step = -1, place = -1, hit = -1, fall = -1;  // SoundBank event ids
    float volume = 1, pitch = 1;
};

struct MusicDef {
    int event = -1;
    int minDelay = 12000, maxDelay = 24000;  // ticks
    bool replace = false;
};
struct BackgroundMusic {
    MusicDef normal, creative, underwater;  // event < 0 when absent
    // BackgroundMusic.select
    const MusicDef* select(bool isCreative, bool isUnderwater) const {
        if (isUnderwater && underwater.event >= 0) return &underwater;
        if (isCreative && creative.event >= 0) return &creative;
        return normal.event >= 0 ? &normal : nullptr;
    }
};
struct AmbientSounds {
    int loop = -1;
    struct Addition {
        int event;
        double chance;
    };
    std::vector<Addition> additions;
    bool hasMood = false;
    int moodEvent = -1, moodTickDelay = 6000, moodExtent = 8;
    double moodOffset = 2.0;
};
// Audio attributes of one dimension type or biome (a biome's value overrides its dimension's).
struct AudioEnvironment {
    bool hasMusic = false, hasAmbient = false, hasMusicVolume = false;
    BackgroundMusic music;
    AmbientSounds ambient;
    float musicVolume = 1.0f;
};

enum BlockTag : u8 { TagInsideStep = 1, TagCombinationStep = 2, TagCrystal = 4, TagClimbable = 8 };

class GameSounds {
public:
    bool load(const fs::path& minecraftDir, const std::string& version, const fs::path& cacheDir, const SoundBank& bank);

    // Sound type index for a full block state key ("minecraft:oak_log[axis=y]"); 0 if unknown.
    int typeFor(std::string_view stateKey) const;
    const BlockSoundType& type(int i) const { return types_[i]; }
    u8 tags(std::string_view blockName) const;  // BlockTag bits
    const AudioEnvironment* dimension(std::string_view name) const;
    const AudioEnvironment* biome(std::string_view name) const;

private:
    bool generate(const fs::path& minecraftDir, const std::string& version, const fs::path& out);

    std::vector<BlockSoundType> types_;
    StringMap<int> blocks_, states_;
    StringMap<u8> tags_;
    StringMap<AudioEnvironment> dimensions_, biomes_;
};

}  // namespace audio
