#include "audio/tickable_sounds.h"

#include "entity/entities.h"
#include "game/game.h"
#include "assets/assets.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <shared_mutex>

namespace audio {

namespace {

constexpr double kTick = 0.05;

float clampedLerp(float f, float lo, float hi) { return f < 0 ? lo : f > 1 ? hi : lo + f * (hi - lo); }  // Mth.clampedLerp

}  // namespace

bool TickableSounds::start(Instance& in, const char* event, Category category, const double pos[3], bool linear, bool relative,
                           bool looping) {
    const int id = audio_.eventId(event);
    if (id < 0) return false;
    // SoundInstance.canStartSilent: BeeSoundInstance, MinecartSoundInstance, RidingEntitySoundInstance
    const std::string_view e(event);
    const bool silentOk = e.starts_with("minecraft:entity.bee.loop") || e == "minecraft:entity.minecart.riding" ||
                          e.starts_with("minecraft:entity.minecart.inside") || e == "minecraft:entity.nautilus.riding" ||
                          e == "minecraft:entity.happy_ghast.riding";
    in.handle = audio_.play(id, category, pos, 1.0f, 1.0f, audio_.nextSeed(), linear, relative, looping, &in.soundVolume, silentOk);
    if (!in.handle) return false;
    audio_.setVolume(in.handle, in.volume * in.soundVolume);  // the instance's starting volume (0: started silently)
    if (in.pitch != 1.0f) audio_.setPitch(in.handle, in.pitch);
    instances_.push_back(in);
    return true;
}

// AbstractMinecart.isOnRails on the client (NewMinecartBehavior.tick): a rail at getCurrentBlockPosOrRailBelow.
bool TickableSounds::onRails(const ent::Entity& e) const {
    std::shared_lock lock(entities_.world().mutex());
    const int x = int(std::floor(e.pos[0])), y = int(std::floor(e.pos[1])), z = int(std::floor(e.pos[2]));
    auto rail = [&](int by) {
        const std::string& n = entities_.registry().name(entities_.world().block(x, by, z));
        const std::string_view b = std::string_view(n).substr(0, n.find('['));
        return b == "minecraft:rail" || b == "minecraft:powered_rail" || b == "minecraft:detector_rail" || b == "minecraft:activator_rail";
    };
    return rail(y - 1) || rail(y);
}

void TickableSounds::stopAll() {
    for (Instance& in : instances_) audio_.stop(in.handle);
    instances_.clear();
    seen_.clear(), guardians_.clear(), sniffers_.clear();
    vehicle_ = -1;
    wasFallFlying_ = wasUnderwater_ = inBubbleColumn_ = false;
    firstTick_ = true;
}

void TickableSounds::frame(double dt, bool inWorld, int gameMode) {
    if (!inWorld || !audio_.ready()) {
        if (!instances_.empty() || !firstTick_) stopAll();
        clock_ = 0;
        return;
    }
    for (Instance& in : instances_)  // a sound that ended on its own (or was stolen) is gone
        if (audio_.finished(in.handle)) in.handle = 0, in.stopped = true;
    std::erase_if(instances_, [](const Instance& in) { return in.stopped; });
    clock_ += std::min(dt, 0.25);
    while (clock_ >= kTick) {
        clock_ -= kTick;
        tick(gameMode);
    }
}

void TickableSounds::tick(int gameMode) {
    const Player& p = game_.player();
    const double playerPos[3] = {p.pos[0], p.pos[1], p.pos[2]};
    const double zero[3] = {};
    const bool underwater = p.eyeInWater;  // LocalPlayer.isUnderWater: wasUnderwater, the eye in water
    // The motion of an entity (getDeltaMovement): the server's motion packets, or the vehicle we drive's last move.
    auto motion = [&](const ent::Entity& e, double out[3]) {
        for (int a = 0; a < 3; ++a) out[a] = entities_.localControl() == e.id ? e.pos[a] - e.prevPos[a] : e.vel[a];
    };
    auto entityUnderWater = [&](const ent::Entity& e) {
        const double eye = e.pos[1] + (e.info ? e.info->eye : 0.0);
        std::shared_lock lock(entities_.world().mutex());
        const std::string& n = entities_.registry().name(entities_.world().block(int(std::floor(e.pos[0])), int(std::floor(eye)), int(std::floor(e.pos[2]))));
        return n.starts_with("minecraft:water") || n.starts_with("minecraft:bubble_column") || n.find("waterlogged=true") != std::string::npos;
    };

    // ClientPacketListener.postAddEntitySoundInstance: minecarts roll, bees buzz (started near the listener: the
    // sounds are silent beyond their attenuation distance anyway).
    for (const auto& [id, e] : entities_.entities()) {
        if (!e.info || seen_.contains(id)) continue;
        const bool minecart = e.info->name.ends_with("minecart"), bee = e.info->name == "minecraft:bee";
        if (!minecart && !bee) continue;
        const double dx = e.pos[0] - p.pos[0], dy = e.pos[1] - p.pos[1], dz = e.pos[2] - p.pos[2];
        if (dx * dx + dy * dy + dz * dz > 24.0 * 24.0) continue;
        seen_.insert(id);
        Instance in{minecart ? Kind::Minecart : Kind::Bee};
        in.entity = id, in.volume = 0.0f;
        in.aggressive = bee && e.angry;
        start(in, minecart ? "minecraft:entity.minecart.riding" : in.aggressive ? "minecraft:entity.bee.loop_aggressive" : "minecraft:entity.bee.loop",
              Category::Neutral, e.pos);
    }
    for (auto it = seen_.begin(); it != seen_.end();) {  // out of range or gone: may start again
        const ent::Entity* e = entities_.entity(*it);
        const bool gone = !e || (e->pos[0] - p.pos[0]) * (e->pos[0] - p.pos[0]) + (e->pos[1] - p.pos[1]) * (e->pos[1] - p.pos[1]) +
                                       (e->pos[2] - p.pos[2]) * (e->pos[2] - p.pos[2]) > 32.0 * 32.0;
        if (!gone) {
            ++it;
            continue;
        }
        const int id = *it;
        for (Instance& in : instances_)
            if ((in.kind == Kind::Minecart || in.kind == Kind::Bee) && in.entity == id) audio_.stop(in.handle), in.stopped = true;
        it = seen_.erase(it);
    }
    // Guardian entity event 21 (the beam starts with its attack target) and sniffer event 63 (digging starts).
    std::unordered_set<int> guardians, sniffers;
    for (const auto& [id, e] : entities_.entities()) {
        if (!e.info) continue;
        if (e.attackTarget != 0 && (e.info->name == "minecraft:guardian" || e.info->name == "minecraft:elder_guardian")) {
            guardians.insert(id);
            const bool playing = std::any_of(instances_.begin(), instances_.end(), [&](const Instance& in) {
                return in.kind == Kind::Guardian && in.entity == id && !in.stopped;
            });
            if (!guardians_.contains(id) && !playing) {
                Instance in{Kind::Guardian};
                in.entity = id, in.volume = 1.0f;
                start(in, "minecraft:entity.guardian.attack", Category::Hostile, e.pos, false);
            }
        }
        if (e.snifferState == 5 && e.info->name == "minecraft:sniffer") {
            sniffers.insert(id);
            if (!sniffers_.contains(id)) {
                Instance in{Kind::Sniffer};
                in.entity = id, in.volume = 1.0f;
                start(in, "minecraft:entity.sniffer.digging", Category::Neutral, e.pos, true, false, false);
            }
        }
    }
    guardians_.swap(guardians), sniffers_.swap(sniffers);

    // LocalPlayer.startRiding: the riding sounds of the new vehicle.
    if (game_.vehicleId != vehicle_) {
        vehicle_ = game_.vehicleId;
        if (const ent::Entity* v = vehicle_ >= 0 ? entities_.entity(vehicle_) : nullptr; v && v->info) {
            const std::string& n = v->info->name;
            if (n.ends_with("minecart")) {
                Instance in{Kind::RidingMinecart};
                in.entity = vehicle_, in.volumeMin = 0.0f, in.volumeMax = 0.75f, in.amplifier = 1.0f, in.volume = 0.0f;
                in.underwaterSound = true;
                start(in, "minecraft:entity.minecart.inside.underwater", Category::Neutral, v->pos, false);
                in.underwaterSound = false;
                start(in, "minecraft:entity.minecart.inside", Category::Neutral, v->pos, false);
            } else if (n == "minecraft:happy_ghast" || n == "minecraft:nautilus" || n == "minecraft:zombie_nautilus") {
                Instance in{Kind::Riding};
                in.entity = vehicle_, in.volumeMin = 0.0f, in.volumeMax = 1.0f, in.amplifier = 5.0f, in.volume = 0.0f;
                in.underwaterSound = n != "minecraft:happy_ghast";
                start(in, in.underwaterSound ? "minecraft:entity.nautilus.riding" : "minecraft:entity.happy_ghast.riding", Category::Neutral, v->pos,
                      false);
            }
        }
    }
    // LocalPlayer.onSyncedDataUpdated: gliding starts.
    if (p.fallFlying && !wasFallFlying_) {
        Instance in{Kind::Elytra};
        in.volume = 0.1f;
        start(in, "minecraft:item.elytra.flying", Category::Player, playerPos);
    }
    wasFallFlying_ = p.fallFlying;
    // LocalPlayer.updateIsUnderwater: the enter / exit sounds and the underwater loop (none for a spectator).
    if (underwater != wasUnderwater_ && gameMode == 3) {
        wasUnderwater_ = underwater;
    } else if (underwater != wasUnderwater_) {
        audio_.play(underwater ? "minecraft:ambient.underwater.enter" : "minecraft:ambient.underwater.exit", Category::Ambient, playerPos[0],
                    playerPos[1], playerPos[2], 1.0f, 1.0f);
        if (underwater) {
            Instance in{Kind::UnderwaterLoop};
            in.volume = 1.0f;
            start(in, "minecraft:ambient.underwater.loop", Category::Ambient, zero, true, true);
        }
        wasUnderwater_ = underwater;
    }

    std::vector<int> switched;  // bees whose loop changes
    for (Instance& in : instances_) {
        if (in.stopped) continue;
        const ent::Entity* e = in.entity >= 0 ? entities_.entity(in.entity) : nullptr;
        bool stop = false;
        double pos[3] = {};
        if (e) std::copy(e->pos, e->pos + 3, pos);
        switch (in.kind) {
            case Kind::Minecart: {  // MinecartSoundInstance (its pitch ramp writes a shadowing field: the pitch stays 1)
                if (!e || e->silent) {
                    stop = true;
                    break;
                }
                double v[3];
                motion(*e, v);
                const float speed = float(std::sqrt(v[0] * v[0] + v[2] * v[2]));
                const bool offRail = e->newMinecart && !onRails(*e);  // the experimental behaviour off its rails: quiet
                in.volume = speed >= 0.01f && entities_.tickRate.runs && !offRail ? std::clamp(speed, 0.0f, 0.5f) * 0.7f : 0.0f;
                break;
            }
            case Kind::RidingMinecart:
            case Kind::Riding: {  // RidingMinecartSoundInstance / RidingEntitySoundInstance
                if (!e || e->silent || game_.vehicleId != in.entity) {
                    stop = true;
                    break;
                }
                const bool wrongMedium = in.underwaterSound != (in.kind == Kind::RidingMinecart ? underwater : entityUnderWater(*e));
                if (wrongMedium) {
                    in.volume = in.volumeMin;
                    break;
                }
                double v[3];
                motion(*e, v);
                const float speed = float(in.kind == Kind::RidingMinecart ? std::sqrt(v[0] * v[0] + v[2] * v[2])
                                                                           : std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
                // shoudlPlaySound: a minecart on its rails (or with the old behaviour)
                const bool plays = in.kind != Kind::RidingMinecart || !e->newMinecart || onRails(*e);
                in.volume = speed >= 0.01f && plays ? in.amplifier * clampedLerp(speed, in.volumeMin, in.volumeMax) : in.volumeMin;
                break;
            }
            case Kind::Elytra: {  // ElytraOnPlayerSoundInstance
                ++in.time;
                if (!(in.time <= 20 || p.fallFlying)) {
                    stop = true;
                    break;
                }
                std::copy(playerPos, playerPos + 3, pos);
                const float speed = float(p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1] + p.vel[2] * p.vel[2]);
                in.volume = speed >= 1.0e-7f ? std::clamp(speed / 4.0f, 0.0f, 1.0f) : 0.0f;
                if (in.time < 20) in.volume = 0.0f;
                else if (in.time < 40) in.volume *= float(in.time - 20) / 20.0f;
                in.pitch = in.volume > 0.8f ? 1.0f + (in.volume - 0.8f) : 1.0f;
                break;
            }
            case Kind::Bee: {  // BeeFlyingSoundInstance / BeeAggressiveSoundInstance
                if (!e || e->silent) {
                    stop = true;
                    break;
                }
                if (e->angry != in.aggressive) {  // shouldSwitchSounds: the other loop takes over
                    stop = true;
                    switched.push_back(in.entity);
                    break;
                }
                double v[3];
                motion(*e, v);
                const float speed = float(std::sqrt(v[0] * v[0] + v[2] * v[2]));
                const float minPitch = e->baby ? 1.1f : 0.7f, maxPitch = e->baby ? 1.5f : 1.1f;
                if (speed >= 0.01f) {  // lerp(clamp(speed, min, max), min, max)
                    in.pitch = minPitch + std::clamp(speed, minPitch, maxPitch) * (maxPitch - minPitch);
                    in.volume = std::clamp(speed, 0.0f, 0.5f) * 1.2f;
                } else {
                    in.pitch = 0.0f, in.volume = 0.0f;
                }
                break;
            }
            case Kind::Guardian: {  // GuardianAttackSoundInstance
                if (!e || e->silent) {
                    stop = true;
                    break;
                }
                const float scale = float(e->attackTime) / (e->info && e->info->name == "minecraft:elder_guardian" ? 60.0f : 80.0f);
                in.volume = scale * scale;
                in.pitch = 0.7f + 0.5f * scale;
                break;
            }
            case Kind::Sniffer:  // SnifferSoundInstance
                stop = !e || e->silent || (e->snifferState != 5 && e->snifferState != 4);
                break;
            case Kind::UnderwaterLoop:  // UnderLiquidAmbientSoundInstance
                if (in.time < 0) {
                    stop = true;
                    break;
                }
                in.time += underwater ? 1 : -2;
                in.time = std::min(in.time, 40);
                in.volume = std::clamp(float(in.time) / 40.0f, 0.0f, 1.0f);
                break;
            case Kind::UnderwaterSub:  // UnderLiquidSubSound
                stop = !underwater;
                break;
        }
        if (stop) {
            audio_.stop(in.handle);
            in.stopped = true;
            continue;
        }
        audio_.setVolume(in.handle, in.volume * in.soundVolume);
        audio_.setPitch(in.handle, in.pitch);
        if (in.kind != Kind::UnderwaterLoop && in.kind != Kind::UnderwaterSub) audio_.setPosition(in.handle, pos);
    }
    std::erase_if(instances_, [](const Instance& in) { return in.stopped; });
    for (int id : switched)
        if (const ent::Entity* e = entities_.entity(id)) {
            Instance in{Kind::Bee};
            in.entity = id, in.volume = 0.0f, in.aggressive = e->angry;
            start(in, in.aggressive ? "minecraft:entity.bee.loop_aggressive" : "minecraft:entity.bee.loop", Category::Neutral, e->pos);
        }

    // UnderwaterAmbientSoundHandler: the additions while under water.
    if (--underwaterDelay_ <= 0 && underwater) {
        const float r = rng_.nextFloat();
        const char* addition = r < 1.0e-4f ? "minecraft:ambient.underwater.loop.additions.ultra_rare"
                               : r < 0.001f ? "minecraft:ambient.underwater.loop.additions.rare"
                               : r < 0.01f  ? "minecraft:ambient.underwater.loop.additions"
                                            : nullptr;
        if (addition) {
            underwaterDelay_ = 0;
            Instance in{Kind::UnderwaterSub};
            in.volume = 1.0f;
            start(in, addition, Category::Ambient, zero, true, true, false);
        }
    }
    // BubbleColumnAmbientSoundHandler: entering a bubble column (the middle of the body in it).
    {
        const AABB b = p.box();
        const double x0 = b.min[0] + 1e-6, y0 = b.min[1] + 0.4 + 1e-6, z0 = b.min[2] + 1e-6;
        const double x1 = b.max[0] - 1e-6, y1 = b.max[1] - 0.4 - 1e-6, z1 = b.max[2] - 1e-6;
        int column = 0;  // 1 upwards, 2 whirlpool
        {
            std::shared_lock lock(entities_.world().mutex());
            for (int y = int(std::floor(y0)); y <= int(std::floor(y1)) && !column; ++y)
                for (int z = int(std::floor(z0)); z <= int(std::floor(z1)) && !column; ++z)
                    for (int x = int(std::floor(x0)); x <= int(std::floor(x1)) && !column; ++x) {
                        const std::string& n = entities_.registry().name(entities_.world().block(x, y, z));
                        if (n.starts_with("minecraft:bubble_column")) column = n.find("drag=true") != std::string::npos ? 2 : 1;
                    }
        }
        if (column && !inBubbleColumn_ && !firstTick_ && gameMode != 3)
            audio_.play(column == 2 ? "minecraft:block.bubble_column.whirlpool_inside" : "minecraft:block.bubble_column.upwards_inside", Category::Player,
                        playerPos[0], playerPos[1], playerPos[2], 1.0f, 1.0f);
        inBubbleColumn_ = column != 0;
        firstTick_ = false;
    }
}

}  // namespace audio
