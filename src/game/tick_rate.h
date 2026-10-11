#pragma once
// The level's TickRateManager on the client (ticking_state / ticking_step): the server's tick rate, whether it is
// frozen, and the steps it runs while frozen. Ticked once per client tick (Minecraft.tick); while it does not run
// normally the level's time, world border and non-player entities stand still.
#include <algorithm>

struct TickRate {
    float rate = 20.0f;  // ticks per second
    bool frozen = false;
    int stepTicks = 0;   // frozenTicksToRun
    bool runs = true;    // runsNormally (runGameElements of the last tick)

    void tick() {
        runs = !frozen || stepTicks > 0;
        if (stepTicks > 0) --stepTicks;
    }
    // Minecraft.getTickTargetMillis: the client slows down with a slower server rate while the level runs normally.
    double tickSeconds() const { return runs ? std::max(0.05, 1.0 / double(rate)) : 0.05; }
};
