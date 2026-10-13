#pragma once
// The game's data-driven "environment attributes" (26.x) for the visuals the sky needs: sky / fog / cloud
// colours, celestial angles, star brightness, moon phase and the lightmap's sky light. Values are layered
// like EnvironmentAttributeSystem does it: attribute default -> dimension_type "attributes" -> biome
// "attributes" (Gaussian-weighted around the camera) -> the dimension's timelines (keyframe tracks on a
// world clock) -> the built-in rain / thunder layers -> the lightning flash. All data comes from the jar.
#include "core/common.h"

#include <array>
#include <optional>
#include <unordered_map>

class AssetSource;

namespace sky {

enum class Attr : u8 {
    FogColor,
    SkyColor,
    SunriseSunsetColor,
    CloudColor,
    CloudHeight,
    SunAngle,
    MoonAngle,
    StarAngle,
    MoonPhase,
    StarBrightness,
    SkyLightColor,
    SkyLightFactor,
    AmbientLightColor,
    BlockLightTint,
    SkyFogEnd,
    CloudFogEnd,
    WaterFogColor,
    WaterFogStart,
    WaterFogEnd,
    NightVisionColor,
    SkyLightLevel,  // gameplay/sky_light_level
    FogStart,       // visual/fog_start_distance (AtmosphericFogEnvironment)
    FogEnd,         // visual/fog_end_distance
    Count
};
constexpr int kAttrCount = int(Attr::Count);

using Value = std::array<float, 4>;  // colours: r g b a (0..1); scalars and the moon phase index in [0]

enum class Op : u8 { Override, Add, Subtract, Multiply, Minimum, Maximum, AlphaBlend, BlendToGray };
struct Modifier {
    Op op = Op::Override;
    Value arg{};  // BlendToGray: brightness, factor; float AlphaBlend: value, alpha
};
struct AttributeMap {
    std::array<std::optional<Modifier>, kAttrCount> entries;
};

struct BiomeInfo {
    std::string name;
    AttributeMap attributes;
    float temperature = 0.5f;
    bool precipitation = true;
    bool frozen = false;  // temperature_modifier "frozen"
};

struct Track {
    Attr attr{};
    Op op = Op::Override;
    int ease = 0;  // 0 linear, 1 constant, 2 cubic bezier
    float bezier[4] = {};
    std::vector<std::pair<int, Value>> keys;
};
struct Timeline {
    std::string clock;
    int period = 0;  // 0: no period
    std::vector<Track> tracks;
};

enum class Skybox : u8 { Overworld, None, End };
struct DimensionInfo {
    std::string name;  // "minecraft:overworld"
    AttributeMap attributes;
    std::vector<const Timeline*> timelines;
    std::string clock;  // default_clock ("" = none)
    Skybox skybox = Skybox::Overworld;
    bool hasSkylight = true, hasCeiling = false;
    bool piglinsZombify = true;  // gameplay/piglins_zombify
    bool netherShade = false;    // cardinal_light "nether" (CardinalLighting.NETHER)
    int minY = -64;
    bool canHaveWeather() const { return hasSkylight && !hasCeiling && skybox != Skybox::End; }
};

class Environment {
public:
    bool load(const AssetSource& data);  // the game's data with the data packs

    const DimensionInfo* dimension(std::string_view name) const;  // null if unknown
    int biome(std::string_view name) const;                         // -1 if unknown
    const BiomeInfo& biomeInfo(int index) const { return biomes_[size_t(index)]; }

    struct Inputs {
        const DimensionInfo* dimension = nullptr;
        const std::vector<std::pair<int, double>>* biomeWeights = nullptr;  // biome index, weight
        const std::unordered_map<std::string, double>* clocks = nullptr;    // clock name -> total ticks
        float rain = 0, thunder = 0;  // thunder already multiplied by rain (Level.getThunderLevel)
        bool flash = false;           // lightning sky flash
    };
    void evaluate(const Inputs& in, Value out[kAttrCount]) const;

private:
    std::vector<BiomeInfo> biomes_;
    std::unordered_map<std::string, int> biomeIndex_;
    std::vector<DimensionInfo> dimensions_;
    std::vector<std::unique_ptr<Timeline>> timelines_;
};

const char* attrName(Attr a);

}  // namespace sky
