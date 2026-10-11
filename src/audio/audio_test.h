#pragma once
// --audio-test: scripted real-time gameplay (settle, break, place, walk, two positioned test
// sounds) whose timeline is written into the --audio-capture log for automated checks.
#include "core/common.h"

class Game;
class World;
class BlockRegistry;

namespace audio {

class AudioSystem;

class SelfTest {
public:
    void frame(Game& game, World& world, BlockRegistry& registry, AudioSystem& audio, double dt);

private:
    double t_ = -1, acc_ = 0;
    int step_ = 0;
    double walkStart_[3] = {};
};

}  // namespace audio
