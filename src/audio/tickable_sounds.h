#pragma once
// The client's tickable sound instances and ambient sound handlers (client/resources/sounds): minecarts rolling,
// riding a minecart / happy ghast / nautilus, elytra wind, bees, the guardian beam charging, sniffer digging, the
// underwater loop with its additions (UnderwaterAmbientSoundHandler), entering / leaving water and bubble columns.
#include "audio/audio.h"

#include <unordered_set>
#include <vector>

class Game;
namespace ent {
class EntityWorld;
}

namespace audio {

class TickableSounds {
public:
    TickableSounds(AudioSystem& audio, ent::EntityWorld& entities, Game& game) : audio_(audio), entities_(entities), game_(game) {}
    // Main thread, every frame after AudioSystem::update. inWorld: a session with its player; gameMode as WorldInfo.
    void frame(double dt, bool inWorld, int gameMode);

private:
    enum class Kind : u8 { Minecart, RidingMinecart, Riding, Elytra, Bee, Guardian, Sniffer, UnderwaterLoop, UnderwaterSub };
    struct Instance {
        Kind kind;
        u32 handle = 0;
        int entity = -1;
        float soundVolume = 1;  // the sound's own volume (sounds.json)
        float volume = 0, pitch = 1;
        int time = 0;  // elytra: ticks; underwater loop: fade
        bool underwaterSound = false, aggressive = false;
        float volumeMin = 0, volumeMax = 1, amplifier = 1;
        bool stopped = false;
    };
    void tick(int gameMode);
    bool start(Instance& in, const char* event, Category category, const double pos[3], bool linear = true, bool relative = false,
               bool looping = true);
    void stopAll();

    AudioSystem& audio_;
    ent::EntityWorld& entities_;
    Game& game_;
    double clock_ = 0;
    std::vector<Instance> instances_;
    std::unordered_set<int> seen_;  // minecarts and bees whose sound was started (ClientPacketListener.postAddEntitySoundInstance)
    std::unordered_set<int> guardians_, sniffers_;  // attacking guardians / digging sniffers last tick
    int vehicle_ = -1;
    bool wasFallFlying_ = false, wasUnderwater_ = false, inBubbleColumn_ = false, firstTick_ = true;
    int underwaterDelay_ = 0;
    JavaRandom rng_{0x5EED};
};

}  // namespace audio
