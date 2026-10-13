#pragma once
// Game-facing sound system: plays the game's sound events through the XAudio2 engine with the
// game's rules (seeded variant choice, category volumes, linear attenuation), and produces the
// client-side sounds of the game: block break/place by the player, footsteps, falls, swimming and
// splashes, background music (MusicManager) and biome ambience (BiomeAmbientSoundsHandler).
// Server sounds arrive through net::Client packet handlers. Everything here runs on the main thread
// except the packet handlers, which only queue.
#include "audio/audio_engine.h"
#include "audio/game_sounds.h"
#include "render/renderer.h"

#include <functional>

class Game;
class World;
class BlockRegistry;
class GameReports;
namespace net {
class Client;
}

namespace audio {

struct WorldInfo {
    int gameMode = 1;  // 0 survival, 1 creative, 2 adventure, 3 spectator
    std::string_view dimension = "minecraft:overworld";
};

class AudioSystem {
public:
    struct Config {
        fs::path minecraftDir;
        std::string version;
        fs::path dataDir;  // %LOCALAPPDATA%\minecraftCplusplus: settings and the block sound cache
        const GameReports* reports = nullptr;
        fs::path capture;  // debug: WAV of the final mix (+ .json log of every sound started)
        bool muted = false;  // automated runs: master gain 0 (the saved volumes stay as they are)
    };

    AudioSystem() = default;
    ~AudioSystem();
    // Starts loading in the background; play() calls before it is ready are ignored.
    void init(const Config& config);
    bool ready() const { return ready_.load(std::memory_order_acquire); }
    bool waitReady(double seconds);

    // Hooks. attachNet must be called before the client is started; it may be called again for a
    // new client (each registers its own handlers).
    void attachGame(Game& game, World& world, BlockRegistry& registry);
    void attachNet(net::Client& client);
    // Optional: biome name at a block position (music / ambience follow biomes when given).
    void setBiomeSource(std::function<std::string_view(int x, int y, int z)> source) { biomeSource_ = std::move(source); }
    // Entities hook: the current position of a server entity; false = unknown (sound skipped).
    // Until it is set, entity sounds play at the listener.
    void setEntityPositionSource(std::function<bool(int entityId, double pos[3])> source) {
        entityPosition_ = std::move(source);
    }

    // Once per frame on the main thread.
    void update(double dt, const CameraState& camera, const WorldInfo& info);

    // A sound event at a position (ClientLevel.playSound); seed picks the variant. Returns a handle
    // (0 if nothing was played).
    u32 play(std::string_view event, Category category, double x, double y, double z, float volume, float pitch);
    u32 play(int event, Category category, const double pos[3], float volume, float pitch, i64 seed, bool linear = true,
             bool relative = false, bool looping = false, float* soundVolume = nullptr);
    // SimpleSoundInstance.forUI: not positioned, UI category.
    u32 playUi(std::string_view event, float pitch = 1.0f, float volume = 0.25f);
    // Screen.getBackgroundMusic: a screen's own music (WinScreen: music.credits) over the situational one; "" ends
    // it (stopping it if it plays).
    void setScreenMusic(std::string_view event);
    // BossHealthOverlay.shouldPlayMusic: in the End a boss bar asking for music plays Musics.END_BOSS (music.dragon).
    void setBossMusic(bool on) { bossMusic_ = on; }
    void blockHitSound(u16 state, Category category, const double pos[3], float volume, float pitch);  // the state's hit sound as is
    // MusicManager: the Music Frequency option (0 default, 1 frequent, 2 constant); the song playing as a translation
    // key (getCurrentMusicTranslationKey: "music.game.sweden", "" none); counters of the now playing toast's show
    // requests (a song started audibly, or the music turned up while one plays silently) and hide requests.
    void setMusicFrequency(int frequency) { musicFrequency_ = frequency; }
    const std::string& currentSongKey() const { return musicSong_; }
    int nowPlayingShows() const { return nowPlayingShows_; }
    int nowPlayingHides() const { return nowPlayingHides_; }
    // A sound the server named by Holder<SoundEvent>: registry id (>= 0), else the inline name.
    u32 playNetwork(int registryId, std::string_view name, Category category, const double pos[3], float volume, float pitch);
    void stop(u32 handle);
    // Tickable sound instances (tickable_sounds.cpp): a playing sound's instance volume (times the sound's own volume,
    // clamped to [0, 1]), pitch and position; whether it is still playing.
    void setVolume(u32 handle, float volume) { engine_.setVolume(handle, volume); }
    void setPitch(u32 handle, float pitch) { engine_.setPitch(handle, pitch); }
    void setPosition(u32 handle, const double pos[3]) { engine_.setPosition(handle, pos); }
    bool finished(u32 handle) const { return std::find(finished_.begin(), finished_.end(), handle) != finished_.end(); }
    int eventId(std::string_view name) const { return bank_.eventId(name); }
    i64 nextSeed() { return rng_.nextLong(); }

    // SoundEventListener (the subtitles): every sound that starts with its subtitle key (sounds.json), position,
    // audible range (infinite for sounds at the listener) and whether it loops. Main thread.
    std::function<void(const std::string& subtitle, const double pos[3], float range, bool looping)> onSound;
    // The Device and Directional Audio options: an output device by name ("" the system default), HRTF on / off.
    void setOutputDevice(const std::string& name, bool hrtf);
    bool directionalActive() const { return engine_.hrtfActive(); }

    // Category volumes 0..1 (the game's sound options), saved to the settings file.
    float volume(Category c) const { return volumes_[int(c)]; }
    void setVolume(Category c, float v);

    // F3: SoundEngine.getChannelDebugString with the ambient mood (DebugEntrySoundMood), and the sound cache.
    std::string channelDebug() const;
    std::string cacheDebug() const;
    void mark(const char* label) { engine_.mark(label); }  // debug capture log
    double captureTime() const;                            // seconds since the capture started
    const GameSounds& gameSounds() const { return game_; }
    const SoundBank& bank() const { return bank_; }
    void report() const;

private:
    struct BlockSound {
        int type = 0;
        u8 tags = 0;
        bool air = false, water = false;
    };
    struct NetSound {
        enum Type : u8 { Sound, EntitySound, Stop, BlockBreak, BlockHit, JukeboxPlay, JukeboxStop } type;
        int event = -1, category = -1, entity = 0;  // BlockBreak: event = sound type
        double pos[3] = {};
        float volume = 1, pitch = 1;
        i64 seed = 0;
        bool anyEvent = false;  // stop: no name given
    };
    struct Loop {  // BiomeAmbientSoundsHandler.LoopSoundInstance
        int event = -1;
        u32 handle = 0;
        float soundVolume = 1;
        int fade = 0, direction = 0;
    };
    struct EntityBound {
        u32 handle;
        int entity;
    };

    void load();
    const BlockSound& blockSound(u16 id);
    BlockSound blockAt(int x, int y, int z, u16* idOut = nullptr);
    void playerSound(int event, float volume, float pitch);
    void onBlockChanged(bool place, int x, int y, int z, u16 state);
    void blockHit(int x, int y, int z);  // ClientLevel.playBreakingSound
    void onPlayerTick();
    void stepSound(int x, int y, int z, const BlockSound& on);
    void tick();
    void tickMusic(const AudioEnvironment* env, bool creative, bool underwater, float musicVolume, const MusicDef* situational = nullptr);
    void tickAmbient(const AmbientSounds* ambient);
    void handleNet(const NetSound& s);
    bool eyeInWater();
    void saveSettings() const;

    Config config_;
    SoundBank bank_;
    GameSounds game_;
    Engine engine_;
    std::thread loader_;
    std::atomic<bool> ready_{false}, loadDone_{false};
    bool haveGameSounds_ = false;
    std::vector<int> protocolEvents_;  // network sound_event registry id -> bank event id
    std::vector<int> stateSoundTypes_;  // network block state id -> sound type (-1: air)

    Game* gameRef_ = nullptr;
    World* world_ = nullptr;
    BlockRegistry* registry_ = nullptr;
    std::vector<BlockSound> blockCache_;
    std::vector<u8> blockResolved_;
    std::function<std::string_view(int, int, int)> biomeSource_;
    std::function<bool(int, double[3])> entityPosition_;

    float volumes_[kCategoryCount];
    JavaRandom rng_;
    u32 nextHandle_ = 1;
    CameraState camera_;
    double listener_[3] = {};
    float listenerDir_[6] = {};
    WorldInfo info_;
    std::string dimension_;
    double tickClock_ = 0;
    int ticks_ = 0;

    // Player movement sounds (Entity.applyMovementEmissionAndPlaySound and friends).
    float moveDist_ = 0, nextStep_ = 1;
    double fallDistance_ = 0;
    bool wasInWater_ = false, firstTick_ = true;
    int playerTicks_ = 0, lastCrystalTick_ = -1000;
    float crystalIntensity_ = 0;

    // LevelEventHandler.playJukeboxSong / stopJukeboxSong: the song playing at each jukebox (block position key)
    std::unordered_map<u64, u32> jukeboxes_;
    net::Client* client_ = nullptr;
    // MusicManager
    u32 music_ = 0;
    int musicEvent_ = -1;
    MusicDef screenMusic_;  // setScreenMusic (event < 0: none)
    MusicDef menuMusic_, bossMusicDef_;  // Musics.MENU / END_BOSS (resolved on first use)
    bool bossMusic_ = false;
    int nextSongDelay_ = 100;
    float musicGain_ = 1.0f;
    int musicFrequency_ = 0;
    std::string musicSong_;
    int nowPlayingShows_ = 0, nowPlayingHides_ = 0;
    bool toastShown_ = false;
    int lastFile_ = -1;  // the file the last play() picked
    // BiomeAmbientSoundsHandler
    std::vector<Loop> loops_;
    int loopEvent_ = -1;
    float moodiness_ = 0;
    std::string biome_;
    int biomeAge_ = 1000;

    std::vector<EntityBound> entitySounds_;
    std::vector<u32> finished_;
    std::mutex netMutex_;
    std::vector<NetSound> netQueue_, netWork_;

    double updateMsTotal_ = 0, updateMsMax_ = 0;
    u64 updates_ = 0, slowUpdates_ = 0, played_ = 0, netSounds_ = 0;
};

AudioSystem* instance();  // the running system (nullptr before init)
std::vector<std::string> outputDevices();  // active render endpoints by friendly name (MMDevice API)
void playUiClick();       // ui.button.click, as the game's buttons play it (pitch 1, volume 0.25)

}  // namespace audio
