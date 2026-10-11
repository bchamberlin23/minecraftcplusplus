#pragma once
// XAudio2 output on its own thread. The main thread only queues small commands (no allocation in
// steady state); the audio thread decodes Ogg Vorbis (cached for short sounds, streamed for
// `stream: true` entries), pools source voices like the game's channel pools (247 static, 8
// streaming) and positions every voice from the listener: the game's linear attenuation (silent at
// the sound's attenuation distance) and constant-power stereo panning of mono sounds.
#include "audio/sound_bank.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace audio {

struct PlayRequest {
    u32 handle = 0;
    int event = -1, file = -1;
    Category category = Category::Master;
    bool stream = false, looping = false, relative = false, linear = true;
    float volume = 1, pitch = 1;  // volume clamped to [0, 1], pitch to [0.5, 2]
    float attenuation = 16;       // blocks to silence (linear attenuation only)
    double pos[3] = {};
    double requestTime = 0;  // nowSeconds() when the game asked for it
};

class Engine {
public:
    static constexpr int kStaticVoices = 247, kStreamingVoices = 8;

    Engine() = default;
    ~Engine() { stop(); }
    // capture: optional WAV file receiving everything the mastering voice outputs (debug/verification).
    bool start(const SoundBank& bank, const fs::path& capture);
    void stop();
    bool running() const { return running_; }

    // Main thread (or any thread): queued for the audio thread.
    void play(const PlayRequest& r);
    void stopHandle(u32 handle);
    void stopMatching(int event, int category);  // -1 = any
    void setPosition(u32 handle, const double pos[3]);
    void setVolume(u32 handle, float volume);
    void setListener(const double pos[3], const float forward[3], const float up[3]);
    void setCategoryGain(Category c, float gain) { gains_[int(c)].store(gain, std::memory_order_relaxed); }
    void setMusicFade(float gain) { musicFade_.store(gain, std::memory_order_relaxed); }
    void takeFinished(std::vector<u32>& out);  // handles that stopped playing since the last call
    void preload(const std::vector<int>& files);

    // Debug capture.
    bool capturing() const { return !capturePath_.empty(); }
    double captureStartTime() const { return captureStart_.load(); }  // nowSeconds() of the first captured frame
    void mark(const char* label);                                    // timestamped note in the capture log

    struct Stats {
        u32 voices = 0, streams = 0, dropped = 0, stolen = 0, decoded = 0;
        double decodeMs = 0;
    };
    Stats stats() const;

private:
    struct Command {
        enum Type : u8 { Play, Stop, StopMatching, Position, Volume, Listener, Preload } type;
        PlayRequest req;  // Play; handle/event/category/pos/volume reused by the others
        float dir[6] = {};  // Listener: forward, up
    };
    struct Impl;
    void run();

    const SoundBank* bank_ = nullptr;
    fs::path capturePath_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Command> commands_;  // reserved; swapped with the audio thread's list
    std::vector<u32> finished_;
    std::vector<int> preloadFiles_;
    bool quit_ = false;
    std::atomic<bool> running_{false};
    std::atomic<float> gains_[kCategoryCount];
    std::atomic<float> musicFade_{1.0f};
    std::atomic<double> captureStart_{0};
    Impl* impl_ = nullptr;
};

}  // namespace audio
