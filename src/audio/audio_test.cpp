#include "audio/audio_test.h"

#include "assets/assets.h"
#include "audio/audio.h"
#include "game/game.h"
#include "world/world.h"

#include <cmath>

namespace audio {

void SelfTest::frame(Game& game, World& world, BlockRegistry& registry, AudioSystem& audio, double dt) {
    Player& p = game.player();
    if (t_ < 0) {
        if (!audio.ready()) return;
        t_ = 0;
        p.flying = false;
        audio.mark("start");
    } else {
        t_ += std::min(dt, 0.25);
    }
    char label[256];
    auto at = [&](double when) { return step_ < 100 && t_ >= when; };
    if (step_ == 0 && at(1.5)) {
        step_ = 1;
        for (float pitch : {1.0f, 1.3f, 1.55f, 0.75f}) {  // the floor about a block ahead
            p.pitch = pitch;
            game.retarget();
            if (game.target().hit) break;
        }
        const RayHit& t = game.target();
        if (t.hit) {
            std::snprintf(label, sizeof(label), "break %s at %d %d %d", registry.name(world.block(t.x, t.y, t.z)).c_str(), t.x,
                          t.y, t.z);
            audio.mark(label);
            game.useTarget(false);
        } else {
            audio.mark("break: nothing targeted");
        }
    }
    if (step_ == 1 && at(2.5)) {
        step_ = 2;
        game.retarget();
        game.setHeld(registry.idFor("minecraft:glowstone"));
        audio.mark(game.useTarget(true) ? "place minecraft:glowstone" : "place refused");
    }
    if (step_ == 2 && at(3.5)) {
        step_ = 3;
        p.pitch = 0;
        std::copy(p.pos, p.pos + 3, walkStart_);
        audio.mark("walk start");
    }
    if (step_ == 3 && at(6.5)) {
        step_ = 4;
        std::snprintf(label, sizeof(label), "walk end: %.2f blocks", std::hypot(p.pos[0] - walkStart_[0], p.pos[2] - walkStart_[2]));
        audio.mark(label);
    }
    // Positioned test sounds: 8 blocks to the listener's right, then 4 to its left.
    const CameraState cam = game.camera();
    const double right[3] = {-std::cos(cam.yaw), 0, -std::sin(cam.yaw)};
    if ((step_ == 4 && at(7.0)) || (step_ == 5 && at(8.0))) {
        const double d = step_ == 4 ? 8.0 : -4.0;
        audio.mark(step_ == 4 ? "test sound right 8" : "test sound left 4");
        audio.play("minecraft:block.note_block.harp", Category::Block, cam.x + right[0] * d, cam.y, cam.z + right[2] * d, 1.0f,
                   1.0f);
        ++step_;
    }

    // Real-time 20 Hz ticks: walking only between "walk start" and "walk end".
    acc_ += std::min(dt, 0.25);
    PlayerInput in;
    if (step_ == 3) in.forward = 1;
    while (acc_ >= 0.05) {
        acc_ -= 0.05;
        game.simulate(in, 1);
    }
    game.buildOverlay();
}

}  // namespace audio
