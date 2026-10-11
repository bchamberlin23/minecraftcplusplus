#pragma once
// Sky, clouds and weather (vanilla 26.3 look): day/night cycle from the world clock, sun / moon / stars,
// sunrise glow, sky and fog colours from the game's environment attributes, fancy 3D clouds and rain /
// snow around the camera. Drives Renderer::setAtmosphere (clear / fog colour and the sky light factor).
//
// Time and weather come from the save (local mode) or from set_time / game_event packets (connected);
// --time / --weather / --dimension / --biome force them for screenshots.
#include "core/common.h"
#include "sky/environment.h"

#include <functional>
#include <memory>
#include <mutex>

class GameReports;
class Renderer;
class World;
class BlockRegistry;
struct CameraState;
namespace net {
class Client;
}

namespace sky {

struct DebugOptions {
    double time = -1;       // --time <ticks>: fixed time of day (frozen)
    int weather = -1;       // --weather clear|rain|thunder
    std::string dimension;  // --dimension overworld|nether|end (visuals only)
    std::string biome;      // --biome <name>: biome used everywhere (sky colour, rain or snow)
};
bool isOption(std::wstring_view arg);  // one of the flags above (each takes a value)
void parseOption(DebugOptions& o, std::wstring_view arg, std::wstring_view value);

class SkyGpu;

class Sky {
public:
    // Noise biome name at quart coordinates (block >> 2); main thread only.
    using BiomeLookup = std::function<std::string(int qx, int qy, int qz)>;

    Sky();
    ~Sky();
    // Reads the environment data and textures from the jar and registers the render passes.
    bool init(Renderer& renderer, World& world, BlockRegistry& registry, const fs::path& jar,
              const DebugOptions& debug);
    // Time of day and weather of a save (local mode; replaced by the server's once connected).
    void loadSave(const fs::path& worldDir);
    // Registers the time / weather / dimension / lightning packet observers. Call after creating the client,
    // before start(). `reports` (optional) resolves the lightning bolt's entity type id for sky flashes.
    void attach(net::Client& client, const GameReports* reports = nullptr);
    void setBiomeLookup(BiomeLookup lookup);
    // Once per frame before Renderer::render: advances time, evaluates the attributes, sets the atmosphere.
    void update(const CameraState& cam, double dt);
    // The boss bars' flags (BossHealthOverlay.shouldDarkenScreen / shouldCreateWorldFog), before update():
    // the world darkening ramps in (GameRenderer.bossOverlayWorldDarkening) and darkens the lightmap and fog colour;
    // the world fog closes the fog in to 10..96 blocks and hides the sky.
    void setBossOverlay(bool darkenScreen, bool worldFog) { bossDarken_ = darkenScreen, bossFog_ = worldFog; }

    // Shared with the render passes.
    struct Frame;
    const Frame& frame() const { return *frame_; }

private:
    struct NetState;
    struct Column;
    void applyNet();
    int biomeAtQuart(int qx, int qy, int qz);
    float temperature(int biome, int x, int y, int z) const;
    int motionBlockingHeight(int x, int z);
    void buildCelestial(const Value* attr, float rainLevel, bool darkDisc);
    void buildWeather(const CameraState& cam, const Value* attr, float rain);
    void buildClouds(const CameraState& cam, const Value* attr);

    Environment env_;
    Renderer* renderer_ = nullptr;
    World* world_ = nullptr;
    BlockRegistry* registry_ = nullptr;
    DebugOptions debug_;
    BiomeLookup biomeLookup_;
    std::unique_ptr<NetState> net_;
    std::unique_ptr<Frame> frame_;
    std::unique_ptr<SkyGpu> gpu_;
    struct Noise;
    std::unique_ptr<Noise> noise_;

    const DimensionInfo* dimension_ = nullptr;
    std::unordered_map<std::string, double> clocks_;  // clock name -> total ticks (fractional)
    std::unordered_map<std::string, float> clockRates_;
    double gameTime_ = 0;  // drives cloud drift and rain animation
    float rain_ = 0, thunder_ = 0;              // displayed levels (thunder not multiplied by rain)
    float rainTarget_ = 0, thunderTarget_ = 0;  // latest values from the server / save
    bool connected_ = false, loggedTime_ = false;
    struct Bolt {  // LightningBolt's client-side flash pattern
        int life = 2, flashes = 1;
    };
    std::vector<Bolt> bolts_;
    int flashTime_ = 0;  // ClientLevel.skyFlashTime
    bool bossDarken_ = false, bossFog_ = false;
    float darken_ = 0, darkenO_ = 0;  // GameRenderer.bossOverlayWorldDarkening (per tick)
    double tickAccumulator_ = 0;

    std::unordered_map<u64, int> biomeCache_;  // quart key -> biome index (-1 unknown)
    std::vector<std::pair<int, double>> biomeWeights_;
    struct Height {
        int y;
        double stamp;
    };
    std::unordered_map<u64, Height> heights_;
    double now_ = 0;

    // Cloud mesh state (CloudRenderer.prepare): rebuilt when the cloud cell or camera band changes.
    int cloudCellX_ = INT_MIN, cloudCellZ_ = INT_MIN, cloudBand_ = -1;
    std::vector<u32> cloudCells_;  // per texel: 0 empty, else 1 | edge-empty flags << 1 (N E S W)
    int cloudW_ = 0, cloudH_ = 0;
    std::vector<u32> cloudFaces_;  // x (12 bits) | z (12 bits) << 12 | direction and flags << 24
    u64 cloudVersion_ = 0;

    double cpuMs_ = 0;
    u64 frames_ = 0;
};

// What the passes draw this frame (filled by Sky::update).
struct Sky::Frame {
    Skybox skybox = Skybox::Overworld;
    bool renderSky = true;  // false under a boss bar's world fog (LevelRenderer.render shouldRenderSky)
    float fogColor[4] = {};
    float skyColor[3] = {};
    float skyEnd = 512;
    bool darkDisc = false;
    // Geometry for the sky stage (camera-relative positions, drawn without depth).
    struct Vertex {
        float pos[3];
        float uv[2];
        u32 color;  // 0xAABBGGRR
        u32 layer;  // texture: 0 sun, 1..8 moon phases, 100 end sky, 0xFFFF none
        u32 pad;
    };
    std::vector<Vertex> fan;       // sunrise / sunset fan (alpha blended)
    std::vector<Vertex> additive;  // sun and moon (additive)
    std::vector<Vertex> endSky;    // end skybox (alpha blended)
    float starRot[9] = {};         // row-major rotation of the static star field
    float starBrightness = 0;
    // Clouds.
    bool clouds = false;
    float cloudColor[4] = {};
    float cloudOffset[3] = {};
    float cloudFogEnd = 2048;
    const std::vector<u32>* cloudFaces = nullptr;
    u64 cloudVersion = 0;
    // Weather columns (rain then snow), camera-relative.
    std::vector<Vertex> weather;
    u32 rainVertices = 0;
};

}  // namespace sky
