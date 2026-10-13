#include "audio/audio.h"

#include "assets/assets.h"
#include "assets/game_reports.h"
#include "game/game.h"
#include "net/client.h"
#include "world/world.h"

#include <json/json.hpp>

#include <mmdeviceapi.h>

#include <algorithm>
#include <climits>
#include <limits>
#include <cmath>
#include <fstream>

namespace audio {

namespace {
AudioSystem* g_instance = nullptr;
constexpr double kTick = 0.05;
}  // namespace

AudioSystem* instance() { return g_instance; }

namespace {
constexpr PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};  // PKEY_Device_FriendlyName

// Active render endpoints: (id, friendly name). On a thread of its own (COM apartment of its own).
std::vector<std::pair<std::wstring, std::string>> endpoints() {
    std::vector<std::pair<std::wstring, std::string>> out;
    std::thread([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* en = nullptr;
        IMMDeviceCollection* list = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                       reinterpret_cast<void**>(&en))) &&
            SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list))) {
            UINT n = 0;
            list->GetCount(&n);
            for (UINT i = 0; i < n; ++i) {
                IMMDevice* dev = nullptr;
                IPropertyStore* props = nullptr;
                LPWSTR id = nullptr;
                if (SUCCEEDED(list->Item(i, &dev)) && SUCCEEDED(dev->GetId(&id)) && SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                    PROPVARIANT v;
                    PropVariantInit(&v);
                    if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) {
                        const int len = WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                        std::string name(size_t(std::max(len, 1)) - 1, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, name.data(), len, nullptr, nullptr);
                        out.push_back({id, name});
                    }
                    PropVariantClear(&v);
                }
                if (id) CoTaskMemFree(id);
                if (props) props->Release();
                if (dev) dev->Release();
            }
        }
        if (list) list->Release();
        if (en) en->Release();
        CoUninitialize();
    }).join();
    return out;
}
}  // namespace

std::vector<std::string> outputDevices() {
    std::vector<std::string> names;
    for (auto& [id, name] : endpoints()) names.push_back(name);
    return names;
}

void AudioSystem::setOutputDevice(const std::string& name, bool hrtf) {
    std::wstring id;  // by name like the game's option; a device that is gone falls back to the default
    if (!name.empty())
        for (auto& [eid, ename] : endpoints())
            if (ename == name) id = eid;
    if (!name.empty() && id.empty()) LOG("audio: output device \"%s\" not found; using the default", name.c_str());
    engine_.setDevice(id, hrtf);
}

void playUiClick() {
    if (g_instance) g_instance->playUi("minecraft:ui.button.click");
}

AudioSystem::~AudioSystem() {
    if (loader_.joinable()) loader_.join();
    if (g_instance == this) g_instance = nullptr;
    engine_.stop();
}

void AudioSystem::init(const Config& config) {
    config_ = config;
    for (float& v : volumes_) v = 1.0f;
    {  // settings: "<category> <volume>" lines
        std::ifstream f(config_.dataDir / "audio_settings.txt");
        std::string name;
        float v;
        while (f >> name >> v)
            for (int c = 0; c < kCategoryCount; ++c)
                if (name == categoryName(Category(c))) volumes_[c] = std::clamp(v, 0.0f, 1.0f);
    }
    rng_.setSeed(i64(nowSeconds() * 1e6));
    netQueue_.reserve(256);
    netWork_.reserve(256);
    finished_.reserve(512);
    entitySounds_.reserve(64);
    loops_.reserve(8);
    g_instance = this;
    if (const wchar_t* off = _wgetenv(L"MCPP_NO_AUDIO"); off && *off && *off != L'0') {
        LOG("audio: disabled (MCPP_NO_AUDIO)");
        return;
    }
    loader_ = std::thread([this] { load(); });
}

void AudioSystem::load() {
    struct Done {
        std::atomic<bool>& flag;
        ~Done() { flag.store(true); }
    } done{loadDone_};
    Timer t;
    const nlohmann::json v = [&] {
        std::vector<u8> bytes;
        if (!readFile(config_.minecraftDir / "versions" / config_.version / (config_.version + ".json"), bytes))
            return nlohmann::json();
        return nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    }();
    const std::string indexId =
        v.is_object() && v.contains("assetIndex") ? v["assetIndex"].value("id", "") : std::string();
    if (indexId.empty() || !bank_.load(config_.minecraftDir, indexId)) {
        LOG("audio: game sounds not found (asset index %s); sound disabled", indexId.c_str());
        return;
    }
    const double bankMs = t.ms();
    haveGameSounds_ = game_.load(config_.minecraftDir, config_.version, config_.dataDir / "sounds", bank_);
    if (!haveGameSounds_) LOG("audio: block sound types unavailable; block, step and music sounds disabled");
    if (config_.reports) {
        const nlohmann::json& reg = config_.reports->registries();
        if (reg.is_object() && reg.contains("minecraft:sound_event")) {
            const auto& entries = reg["minecraft:sound_event"]["entries"];
            protocolEvents_.assign(entries.size(), -1);
            for (const auto& e : entries.items()) {
                const int id = e.value().value("protocol_id", -1);
                if (id >= 0 && id < int(protocolEvents_.size())) protocolEvents_[id] = bank_.eventId(e.key());
            }
        }
    }
    if (config_.reports && haveGameSounds_)
        for (const auto& [key, id] : config_.reports->stateIds()) {
            if (id < 0 || id > 1 << 20) continue;
            if (size_t(id) >= stateSoundTypes_.size()) stateSoundTypes_.resize(size_t(id) + 1, -1);
            const std::string_view block = std::string_view(key).substr(0, key.find('['));
            const bool air = block == "minecraft:air" || block == "minecraft:cave_air" || block == "minecraft:void_air";
            stateSoundTypes_[id] = air ? -1 : game_.typeFor(key);
        }
    if (!engine_.start(bank_, config_.capture)) return;
    for (int c = 0; c < kCategoryCount; ++c)
        engine_.setCategoryGain(Category(c), config_.muted && Category(c) == Category::Master ? 0.0f : volumes_[c]);
    engine_.preload(bank_.preloadFiles());
    LOG("audio   %8.1f ms  %zu sound events, %zu files (index %.1f ms), %zu network sound ids, XAudio2%s", t.ms(),
        bank_.eventCount(), bank_.fileCount(), bankMs, protocolEvents_.size(),
        config_.capture.empty() ? "" : " + capture");
    ready_.store(true, std::memory_order_release);
}

bool AudioSystem::waitReady(double seconds) {
    const double end = nowSeconds() + seconds;
    while (!ready() && !loadDone_.load() && nowSeconds() < end) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return ready();
}

double AudioSystem::captureTime() const { return nowSeconds() - engine_.captureStartTime(); }

void AudioSystem::setVolume(Category c, float v) {
    volumes_[int(c)] = std::clamp(v, 0.0f, 1.0f);
    engine_.setCategoryGain(c, config_.muted && c == Category::Master ? 0.0f : volumes_[int(c)]);
    saveSettings();
    if ((c == Category::Music || c == Category::Master) && v > 0.0f && !toastShown_) {  // showNowPlayingToastIfNeeded
        ++nowPlayingShows_;
        toastShown_ = true;
    }
}

void AudioSystem::saveSettings() const {
    std::error_code ec;
    fs::create_directories(config_.dataDir, ec);
    std::ofstream f(config_.dataDir / "audio_settings.txt");
    for (int c = 0; c < kCategoryCount; ++c) f << categoryName(Category(c)) << ' ' << volumes_[c] << '\n';
}

// ---- playing ----------------------------------------------------------------------------------

u32 AudioSystem::playNetwork(int registryId, std::string_view name, Category category, const double pos[3], float volume,
                             float pitch) {
    if (!ready()) return 0;
    const int event = registryId >= 0 ? (registryId < int(protocolEvents_.size()) ? protocolEvents_[size_t(registryId)] : -1)
                                      : bank_.eventId(name);
    return play(event, category, pos, volume, pitch, rng_.nextLong());
}

u32 AudioSystem::play(int event, Category category, const double pos[3], float volume, float pitch, i64 seed, bool linear,
                      bool relative, bool looping, float* soundVolume) {
    if (!ready() || event < 0) return 0;
    JavaRandom random(seed);  // SimpleSoundInstance(..., RandomSource.create(seed), ...)
    ResolvedSound s;
    if (!bank_.resolve(event, random, s)) return 0;
    lastFile_ = s.file;
    const float instVolume = volume * s.volume;
    if (soundVolume) *soundVolume = s.volume;
    if (onSound) {  // SoundEngine.notifyListeners: before the silent check
        const std::string& subtitle = bank_.subtitle(event);
        if (!subtitle.empty())
            onSound(subtitle, pos, relative || !linear ? std::numeric_limits<float>::infinity() : std::max(instVolume, 1.0f) * float(s.attenuation),
                    looping);
    }
    const float clamped = std::clamp(instVolume, 0.0f, 1.0f);
    const float gain = clamped * volumes_[int(category)] * (category == Category::Master ? 1.0f : volumes_[0]);
    if (gain <= 0.0f && category != Category::Music) return 0;  // SoundEngine.play: skipped when silent
    PlayRequest r;
    r.handle = nextHandle_++;
    if (!r.handle) r.handle = nextHandle_++;
    r.event = event;
    r.file = s.file;
    r.category = category;
    r.stream = s.stream;
    r.looping = looping;
    r.relative = relative;
    r.linear = linear;
    r.volume = clamped;
    r.pitch = std::clamp(pitch * s.pitch, 0.5f, 2.0f);
    r.attenuation = std::max(instVolume, 1.0f) * float(s.attenuation);
    std::copy(pos, pos + 3, r.pos);
    r.requestTime = nowSeconds();
    engine_.play(r);
    ++played_;
    return r.handle;
}

u32 AudioSystem::play(std::string_view event, Category category, double x, double y, double z, float volume, float pitch) {
    if (!ready()) return 0;
    const double pos[3] = {x, y, z};
    return play(bank_.eventId(event), category, pos, volume, pitch, rng_.nextLong());
}

u32 AudioSystem::playUi(std::string_view event, float pitch, float volume) {
    if (!ready()) return 0;
    const double zero[3] = {};
    return play(bank_.eventId(event), Category::Ui, zero, volume, pitch, rng_.nextLong(), false, true);
}

void AudioSystem::stop(u32 handle) {
    if (handle) engine_.stopHandle(handle);
}

// ---- blocks -----------------------------------------------------------------------------------

const AudioSystem::BlockSound& AudioSystem::blockSound(u16 id) {
    if (blockCache_.size() <= id) {
        blockCache_.resize(65536);
        blockResolved_.assign(65536, 0);
    }
    if (!blockResolved_[id]) {
        BlockSound& b = blockCache_[id];
        const std::string& name = registry_->name(id);
        const std::string_view block = std::string_view(name).substr(0, name.find('['));
        b.type = game_.typeFor(name);
        b.tags = game_.tags(block);
        b.air = block == "minecraft:air" || block == "minecraft:cave_air" || block == "minecraft:void_air";
        b.water = block == "minecraft:water" || block == "minecraft:bubble_column" ||
                  name.find("waterlogged=true") != std::string::npos;
        blockResolved_[id] = 1;
    }
    return blockCache_[id];
}

AudioSystem::BlockSound AudioSystem::blockAt(int x, int y, int z, u16* idOut) {
    const u16 id = world_->block(x, y, z);
    if (idOut) *idOut = id;
    return blockSound(id);
}

void AudioSystem::onBlockChanged(bool place, int x, int y, int z, u16 state) {
    if (!ready() || !haveGameSounds_) return;
    const BlockSound& b = blockSound(state);
    if (b.air) return;
    const BlockSoundType& t = game_.type(b.type);
    // LevelEventHandler 2001 (break) / BlockItem.place: (volume + 1) / 2, pitch * 0.8, at the block centre.
    const double pos[3] = {x + 0.5, y + 0.5, z + 0.5};
    play(place ? t.place : t.breakSound, Category::Block, pos, (t.volume + 1.0f) / 2.0f, t.pitch * 0.8f, rng_.nextLong());
}

void AudioSystem::blockHit(int x, int y, int z) {
    if (!ready() || !haveGameSounds_ || !world_) return;
    const BlockSound& b = blockSound(world_->block(x, y, z));
    if (b.air) return;
    const BlockSoundType& t = game_.type(b.type);
    // SimpleSoundInstance(hit sound, BLOCKS, (volume + 1) / 8, pitch * 0.5, unseeded random, pos): at the centre.
    const double pos[3] = {x + 0.5, y + 0.5, z + 0.5};
    play(t.hit, Category::Block, pos, (t.volume + 1.0f) / 8.0f, t.pitch * 0.5f, rng_.nextLong());
}

void AudioSystem::attachGame(Game& game, World& world, BlockRegistry& registry) {
    gameRef_ = &game;
    world_ = &world;
    registry_ = &registry;
    game.onBlockChanged = [this, prev = std::move(game.onBlockChanged)](bool place, int x, int y, int z, u16 state) {
        if (prev) prev(place, x, y, z, state);  // hooks set earlier (native mods)
        onBlockChanged(place, x, y, z, state);
    };
    game.onPlayerTick = [this] { onPlayerTick(); };
    game.onBreakingEffects = [this, prev = std::move(game.onBreakingEffects)](int x, int y, int z, int face, bool sound) {
        if (prev) prev(x, y, z, face, sound);
        if (sound) blockHit(x, y, z);
    };
}

// ---- player movement sounds -----------------------------------------------------------------

void AudioSystem::playerSound(int event, float volume, float pitch) {
    // Player.playSound -> level.playSound(player, x, y, z, ...): at the feet, player category.
    const Player& p = gameRef_->player();
    play(event, Category::Player, p.pos, volume, pitch, rng_.nextLong());
}

void AudioSystem::stepSound(int x, int y, int z, const BlockSound& on) {
    const Player& p = gameRef_->player();
    const BlockSoundType& onType = game_.type(on.type);
    if (p.inWater) {  // Player.playStepSound: swimming + muffled step
        const double* v = p.vel;
        const float speed = std::min(1.0f, float(std::sqrt(v[0] * v[0] * 0.2f + v[1] * v[1] + v[2] * v[2] * 0.2f)) * 0.35f);
        const float f1 = rng_.nextFloat(), f2 = rng_.nextFloat();
        playerSound(bank_.eventId("minecraft:entity.player.swim"), speed, 1.0f + (f1 - f2) * 0.4f);
        playerSound(onType.step, onType.volume * 0.05f, onType.pitch * 0.8f);
    } else {
        const BlockSound above = blockAt(x, y + 1, z);
        if (above.tags & (TagInsideStep | TagCombinationStep)) {
            const BlockSoundType& a = game_.type(above.type);
            playerSound(a.step, a.volume * 0.15f, a.pitch);
            if (above.tags & TagCombinationStep) playerSound(onType.step, onType.volume * 0.05f, onType.pitch * 0.8f);
        } else {
            playerSound(onType.step, onType.volume * 0.15f, onType.pitch);
        }
    }
    if ((on.tags & TagCrystal) && playerTicks_ >= lastCrystalTick_ + 20) {  // Entity.playAmethystStepSound
        crystalIntensity_ *= float(std::pow(0.997, playerTicks_ - lastCrystalTick_));
        crystalIntensity_ = std::min(1.0f, crystalIntensity_ + 0.07f);
        const float pitch = 0.5f + crystalIntensity_ * rng_.nextFloat() * 1.2f;
        playerSound(bank_.eventId("minecraft:block.amethyst_block.chime"), 0.1f + crystalIntensity_ * 1.2f, pitch);
        lastCrystalTick_ = playerTicks_;
    }
}

void AudioSystem::onPlayerTick() {
    if (!ready() || !haveGameSounds_ || !gameRef_) return;
    const Player& p = gameRef_->player();
    ++playerTicks_;
    double d[3] = {p.pos[0] - p.prev[0], p.pos[1] - p.prev[1], p.pos[2] - p.prev[2]};
    if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 100.0) d[0] = d[1] = d[2] = 0;  // teleported
    const bool creative = info_.gameMode == 1 || info_.gameMode == 3;
    const bool inWater = p.inWater;

    // Entity.updateFluidInteraction: splash when entering water.
    if (inWater && !wasInWater_ && !firstTick_ && info_.gameMode != 3) {
        const float speed = std::min(1.0f, float(std::sqrt(d[0] * d[0] * 0.2f + d[1] * d[1] + d[2] * d[2] * 0.2f)) * 0.2f);
        const float f1 = rng_.nextFloat(), f2 = rng_.nextFloat();
        playerSound(bank_.eventId(speed < 0.25f ? "minecraft:entity.player.splash" : "minecraft:entity.player.splash.high_speed"),
                    speed, 1.0f + (f1 - f2) * 0.4f);
    }
    wasInWater_ = inWater;
    firstTick_ = false;

    // Supporting block (Entity.getOnPos): below the feet, else under a corner of the collision box.
    const int fx = int(std::floor(p.pos[0])), fz = int(std::floor(p.pos[2])), by = int(std::floor(p.pos[1] - 0.2));
    int sx = fx, sz = fz;
    BlockSound on = blockAt(sx, by, sz);
    if (on.air && p.onGround) {
        double best = 1e9;
        for (int c = 0; c < 4; ++c) {
            const int cx = int(std::floor(p.pos[0] + ((c & 1) ? 0.299 : -0.299)));
            const int cz = int(std::floor(p.pos[2] + ((c & 2) ? 0.299 : -0.299)));
            const BlockSound b = blockAt(cx, by, cz);
            const double dd = std::abs(cx + 0.5 - p.pos[0]) + std::abs(cz + 0.5 - p.pos[2]);
            if (!b.air && dd < best) best = dd, on = b, sx = cx, sz = cz;
        }
    }

    // Fall distance and fall sounds (Entity.checkFallDamage, LivingEntity.causeFallDamage).
    if (inWater || p.flying)
        fallDistance_ = 0;
    else if (d[1] < 0)
        fallDistance_ -= d[1];
    if (p.onGround) {
        if (fallDistance_ > 0 && !creative) {
            const int damage = int(std::floor(fallDistance_ + 1.0E-6 - 3.0));
            if (damage > 0) {
                playerSound(bank_.eventId(damage > 4 ? "minecraft:entity.player.big_fall" : "minecraft:entity.player.small_fall"),
                            1.0f, 1.0f);
                const BlockSound under = blockAt(fx, int(std::floor(p.pos[1] - 0.2f)), fz);  // playBlockFallSound
                if (!under.air) {
                    const BlockSoundType& t = game_.type(under.type);
                    playerSound(t.fall, t.volume * 0.5f, t.pitch * 0.75f);
                }
            }
        }
        fallDistance_ = 0;
    }

    // Entity.applyMovementEmissionAndPlaySound
    if (info_.gameMode == 3) return;  // spectators emit nothing
    const bool climbing = (on.tags & TagClimbable) != 0;
    const float moved = float(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * 0.6f);
    const float horizontal = float(std::sqrt(d[0] * d[0] + d[2] * d[2]) * 0.6f);
    moveDist_ += climbing ? moved : horizontal;
    if (moveDist_ > nextStep_ && !on.air) {
        const bool swimming = false;  // sprint-swimming is not simulated
        if ((p.onGround || climbing || (p.sneaking && d[1] == 0.0)) && !swimming) {
            stepSound(sx, by, sz, on);
            nextStep_ = float(int(moveDist_) + 1);
        } else if (inWater) {
            nextStep_ = float(int(moveDist_) + 1);
            const double* v = p.vel;
            const float speed = std::min(1.0f, float(std::sqrt(v[0] * v[0] * 0.2f + v[1] * v[1] + v[2] * v[2] * 0.2f)) * 0.35f);
            const float f1 = rng_.nextFloat(), f2 = rng_.nextFloat();
            playerSound(bank_.eventId("minecraft:entity.player.swim"), speed, 1.0f + (f1 - f2) * 0.4f);
        }
    }
}

bool AudioSystem::eyeInWater() {
    if (!gameRef_ || !world_) return false;
    const CameraState c = gameRef_->camera();
    const u16 id = world_->block(int(std::floor(c.x)), int(std::floor(c.y)), int(std::floor(c.z)));
    return blockSound(id).water;
}

// ---- network ----------------------------------------------------------------------------------

void AudioSystem::attachNet(net::Client& client) {
    // Holder<SoundEvent>: VarInt 0 = inline (identifier + optional fixed range), else registry id + 1.
    auto readEvent = [this](net::Reader& r) {
        const int holder = r.varInt();
        if (holder != 0) {
            const int id = holder - 1;
            return id >= 0 && id < int(protocolEvents_.size()) ? protocolEvents_[id] : -1;
        }
        const std::string_view name = r.str();
        if (r.boolean()) r.f32();
        return bank_.eventId(name);
    };
    client.onPlay("sound", [this, readEvent](net::Reader& r) {
        if (!ready()) return;
        NetSound s{NetSound::Sound};
        s.event = readEvent(r);
        s.category = r.varInt();
        for (double& c : s.pos) c = double(r.i32v()) / 8.0;
        s.volume = r.f32();
        s.pitch = r.f32();
        s.seed = r.i64v();
        if (!r.ok) return;
        std::lock_guard lock(netMutex_);
        netQueue_.push_back(s);
    });
    client.onPlay("sound_entity", [this, readEvent](net::Reader& r) {
        if (!ready()) return;
        NetSound s{NetSound::EntitySound};
        s.event = readEvent(r);
        s.category = r.varInt();
        s.entity = r.varInt();
        s.volume = r.f32();
        s.pitch = r.f32();
        s.seed = r.i64v();
        if (!r.ok) return;
        std::lock_guard lock(netMutex_);
        netQueue_.push_back(s);
    });
    // Another player's block break (LevelEventHandler 2001); the breaking player plays its own. 2020: the hit sound
    // of a block being broken (the server plays it every 4 ticks of a dig, for the digging player too).
    client.onPlay("level_event", [this](net::Reader& r) {
        if (!ready()) return;
        const int type = r.i32v();
        NetSound s{NetSound::BlockBreak};
        int x, y, z;
        r.blockPos(x, y, z);
        const int data = r.i32v();
        if (r.ok && type == 2020) {
            s.type = NetSound::BlockHit;
            s.pos[0] = x, s.pos[1] = y, s.pos[2] = z;
            std::lock_guard lock(netMutex_);
            netQueue_.push_back(s);
            return;
        }
        if (!r.ok || type != 2001 || data < 0 || data >= int(stateSoundTypes_.size()) || stateSoundTypes_[data] < 0) return;
        s.event = stateSoundTypes_[data];
        s.pos[0] = x + 0.5, s.pos[1] = y + 0.5, s.pos[2] = z + 0.5;
        std::lock_guard lock(netMutex_);
        netQueue_.push_back(s);
    });
    client.onPlay("stop_sound", [this](net::Reader& r) {
        if (!ready()) return;
        NetSound s{NetSound::Stop};
        const u8 flags = r.u8v();
        if (flags & 1) s.category = r.varInt();
        s.anyEvent = !(flags & 2);
        if (flags & 2) s.event = bank_.eventId(r.str());
        if (!r.ok) return;
        std::lock_guard lock(netMutex_);
        netQueue_.push_back(s);
    });
}

void AudioSystem::handleNet(const NetSound& s) {
    ++netSounds_;
    const Category category = s.category >= 0 && s.category < kCategoryCount ? Category(s.category) : Category::Master;
    switch (s.type) {
    case NetSound::Sound:  // ClientLevel.playSeededSound -> SimpleSoundInstance at the position
        play(s.event, category, s.pos, s.volume, s.pitch, s.seed);
        break;
    case NetSound::EntitySound: {  // EntityBoundSoundInstance: follows the entity while it plays
        double pos[3] = {listener_[0], listener_[1], listener_[2]};
        if (entityPosition_ && !entityPosition_(s.entity, pos)) break;  // unknown entity: the game skips it
        const u32 h = play(s.event, category, pos, s.volume, s.pitch, s.seed);
        if (h && entityPosition_ && entitySounds_.size() < 64) entitySounds_.push_back({h, s.entity});
        break;
    }
    case NetSound::BlockBreak: {
        const BlockSoundType& t = game_.type(s.event);
        play(t.breakSound, Category::Block, s.pos, (t.volume + 1.0f) / 2.0f, t.pitch * 0.8f, rng_.nextLong());
        break;
    }
    case NetSound::BlockHit:
        blockHit(int(s.pos[0]), int(s.pos[1]), int(s.pos[2]));
        break;
    case NetSound::Stop:  // SoundManager.stop(name, source)
        if (!s.anyEvent && s.event < 0) break;  // a sound this client does not know is not playing
        if (engine_.capturing()) engine_.mark("stop_sound");
        engine_.stopMatching(s.anyEvent ? -1 : s.event, s.category);
        break;
    }
}

// ---- per frame / per tick ---------------------------------------------------------------------

void AudioSystem::update(double dt, const CameraState& cam, const WorldInfo& info) {
    if (!ready()) return;
    Timer timer;
    info_.gameMode = info.gameMode;
    if (info.dimension != dimension_) dimension_ = info.dimension;
    info_.dimension = dimension_;
    camera_ = cam;

    // Listener at the camera (Camera.position / forwardVector / upVector).
    const float sy = std::sin(cam.yaw), cy = std::cos(cam.yaw), sp = std::sin(cam.pitch), cp = std::cos(cam.pitch);
    const float dir[6] = {-sy * cp, -sp, cy * cp, -sy * sp, cp, cy * sp};
    const double pos[3] = {cam.x, cam.y, cam.z};
    bool moved = false;
    for (int a = 0; a < 3; ++a) moved |= std::abs(pos[a] - listener_[a]) > 1e-4;
    for (int a = 0; a < 6; ++a) moved |= std::abs(dir[a] - listenerDir_[a]) > 1e-5f;
    if (moved) {
        std::copy(pos, pos + 3, listener_);
        std::copy(dir, dir + 6, listenerDir_);
        engine_.setListener(listener_, dir, dir + 3);
    }

    finished_.clear();
    engine_.takeFinished(finished_);
    for (u32 h : finished_) {
        if (h == music_) music_ = 0, musicSong_.clear();
        for (Loop& l : loops_)
            if (l.handle == h) l.handle = 0;
        std::erase_if(entitySounds_, [h](const EntityBound& e) { return e.handle == h; });
    }

    {
        std::lock_guard lock(netMutex_);
        netWork_.swap(netQueue_);
    }
    for (const NetSound& s : netWork_) handleNet(s);
    netWork_.clear();

    tickClock_ += std::min(dt, 0.25);
    while (tickClock_ >= kTick) {
        tickClock_ -= kTick;
        tick();
    }

    const double ms = timer.ms();
    slowUpdates_ += ms > 0.1;
    updateMsTotal_ += ms;
    updateMsMax_ = std::max(updateMsMax_, ms);
    ++updates_;
}

void AudioSystem::tick() {
    ++ticks_;
    // Entity-bound sounds follow their entity.
    for (size_t i = 0; i < entitySounds_.size();) {
        double pos[3];
        if (entityPosition_ && entityPosition_(entitySounds_[i].entity, pos)) {
            engine_.setPosition(entitySounds_[i].handle, pos);
            ++i;
        } else {
            engine_.stopHandle(entitySounds_[i].handle);  // removed entity
            entitySounds_[i] = entitySounds_.back();
            entitySounds_.pop_back();
        }
    }
    if (!haveGameSounds_ || !world_) return;

    // Environment attributes: the biome's value overrides its dimension's.
    if (biomeSource_ && ++biomeAge_ >= 20) {
        biomeAge_ = 0;
        const std::string_view b = biomeSource_(int(std::floor(camera_.x)), int(std::floor(camera_.y)), int(std::floor(camera_.z)));
        if (b != biome_) biome_ = b;
    }
    const bool underwater = eyeInWater();
    std::string_view biome = biome_;
    if (!biomeSource_) {  // no biome data: the most common biome of the dimension for biome-only sounds
        if (info_.dimension == "minecraft:the_nether") biome = "minecraft:nether_wastes";
        else if (info_.dimension == "minecraft:overworld" && underwater) biome = "minecraft:ocean";
    }
    const AudioEnvironment* dim = game_.dimension(info_.dimension);
    const AudioEnvironment* bio = biome.empty() ? nullptr : game_.biome(biome);
    const AudioEnvironment* musicEnv = bio && bio->hasMusic ? bio : dim && dim->hasMusic ? dim : nullptr;
    const AudioEnvironment* ambientEnv = bio && bio->hasAmbient ? bio : dim && dim->hasAmbient ? dim : nullptr;
    const float musicVolume = bio && bio->hasMusicVolume ? bio->musicVolume : dim && dim->hasMusicVolume ? dim->musicVolume : 1.0f;
    tickMusic(musicEnv, info_.gameMode == 1, underwater, musicVolume);
    tickAmbient(ambientEnv ? &ambientEnv->ambient : nullptr);
}

void AudioSystem::setScreenMusic(std::string_view event) {
    const int id = event.empty() || !ready() ? -1 : bank_.eventId(event);
    if (id < 0 && screenMusic_.event >= 0 && music_ && musicEvent_ == screenMusic_.event) {  // MusicManager.stopPlaying
        engine_.stopHandle(music_);
        music_ = 0, musicEvent_ = -1;
        musicSong_.clear();
        ++nowPlayingHides_;
        nextSongDelay_ = 100;
    }
    screenMusic_ = MusicDef{id, 0, 0, true};  // Musics.CREDITS: no delay, replaces what plays
}

// MusicManager.tick; MusicFrequency.getNextSongDelay (at most 20, 10 or 0 minutes; constant: 5 seconds).
void AudioSystem::tickMusic(const AudioEnvironment* env, bool creative, bool underwater, float volume) {
    const int maxFrequency = (musicFrequency_ == 0 ? 20 : musicFrequency_ == 1 ? 10 : 0) * 1200;
    auto nextDelay = [&](const MusicDef* m) {
        if (!m) return maxFrequency;
        if (musicFrequency_ == 2) return 100;
        return rng_.nextInt(std::min(m->minDelay, maxFrequency), std::min(m->maxDelay, maxFrequency));
    };
    if (music_ && musicGain_ != volume) {  // fadePlaying
        if (musicGain_ < volume) {
            musicGain_ += std::clamp(musicGain_, 5.0E-4f, 0.005f);
            if (musicGain_ > volume) musicGain_ = volume;
        } else {
            musicGain_ = 0.03f * volume + 0.97f * musicGain_;
            if (std::abs(musicGain_ - volume) < 1.0E-4f || musicGain_ < volume) musicGain_ = volume;
        }
        musicGain_ = std::clamp(musicGain_, 0.0f, 1.0f);
        if (musicGain_ <= 1.0E-4f) {
            engine_.stopHandle(music_);
            music_ = 0;
            musicSong_.clear();
            ++nowPlayingHides_;
            nextSongDelay_ = nextDelay(screenMusic_.event >= 0 ? &screenMusic_ : env ? env->music.select(creative, underwater) : nullptr) + 100;
            return;
        }
        engine_.setMusicFade(musicGain_);
    }
    const MusicDef* music = screenMusic_.event >= 0 ? &screenMusic_ : env ? env->music.select(creative, underwater) : nullptr;
    if (!music) {
        nextSongDelay_ = std::max(nextSongDelay_, 100);
        return;
    }
    if (music_ && music->replace && music->event != musicEvent_) {
        engine_.stopHandle(music_);
        music_ = 0;
        musicSong_.clear();
        nextSongDelay_ = rng_.nextInt(0, music->minDelay / 2);
    }
    if (!music_ && musicEvent_ >= 0) {  // the song ended
        musicEvent_ = -1;
        nextSongDelay_ = std::min(nextSongDelay_, nextDelay(music));
    }
    nextSongDelay_ = std::min(nextSongDelay_, music->maxDelay);
    if (!music_ && --nextSongDelay_ <= 0) {  // startPlaying: SimpleSoundInstance.forMusic
        const double zero[3] = {};
        musicGain_ = volume;
        engine_.setMusicFade(musicGain_);
        music_ = play(music->event, Category::Music, zero, 1.0f, 1.0f, rng_.nextLong(), false, true);
        musicEvent_ = music_ ? music->event : -1;
        if (music_ && lastFile_ >= 0) {  // Identifier.toShortLanguageKey with '/' as '.'
            musicSong_ = bank_.fileName(lastFile_);
            for (char& ch : musicSong_)
                if (ch == '/' || ch == ':') ch = '.';
            // SoundManager.play: STARTED shows the toast, STARTED_SILENTLY (no music volume) waits for the volume
            toastShown_ = volumes_[int(Category::Music)] * volumes_[0] > 0.0f;
            if (toastShown_) ++nowPlayingShows_;
        }
        nextSongDelay_ = INT_MAX;
        if (music_) LOG("audio: music %s", bank_.eventName(music->event).c_str());
    }
}

// BiomeAmbientSoundsHandler.tick
void AudioSystem::tickAmbient(const AmbientSounds* ambient) {
    for (Loop& l : loops_) {  // LoopSoundInstance.tick
        if (l.fade < 0) {
            engine_.stopHandle(l.handle);
            l.handle = 0;
        }
        l.fade += l.direction;
        if (l.handle) engine_.setVolume(l.handle, std::clamp(l.fade / 40.0f, 0.0f, 1.0f) * l.soundVolume);
    }
    std::erase_if(loops_, [](const Loop& l) { return l.handle == 0; });
    const int loop = ambient ? ambient->loop : -1;
    if (loop != loopEvent_) {
        loopEvent_ = loop;
        for (Loop& l : loops_) {
            l.fade = std::min(l.fade, 40);
            l.direction = -1;
        }
        if (loop >= 0) {
            auto it = std::find_if(loops_.begin(), loops_.end(), [&](const Loop& l) { return l.event == loop; });
            if (it == loops_.end()) {
                Loop l;
                l.event = loop;
                const double zero[3] = {};
                l.handle = play(loop, Category::Ambient, zero, 1.0f, 1.0f, rng_.nextLong(), true, true, true, &l.soundVolume);
                if (l.handle) {
                    engine_.setVolume(l.handle, 0.0f);
                    loops_.push_back(l);
                    it = loops_.end() - 1;
                }
            }
            if (it != loops_.end()) {
                it->fade = std::max(0, it->fade);
                it->direction = 1;
            }
        }
    }
    if (!ambient) return;
    for (const auto& a : ambient->additions)
        if (rng_.nextDouble() < a.chance) {
            const double zero[3] = {};
            play(a.event, Category::Ambient, zero, 1.0f, 1.0f, rng_.nextLong(), false, true);
        }
    if (!ambient->hasMood || !gameRef_) return;
    const Player& p = gameRef_->player();
    const double eyeY = p.pos[1] + p.eyeHeight();
    const int span = ambient->moodExtent * 2 + 1;
    const int dx = rng_.nextInt(span), dy = rng_.nextInt(span), dz = rng_.nextInt(span);
    const int bx = int(std::floor(p.pos[0] + dx - ambient->moodExtent));
    const int by = int(std::floor(eyeY + dy - ambient->moodExtent));
    const int bz = int(std::floor(p.pos[2] + dz - ambient->moodExtent));
    const World::LightView lv = world_->light(bx >> 4, by >> 4, bz >> 4);
    const int sky = info_.dimension == "minecraft:the_nether" ? 0 : lv.sky4(bx & 15, by & 15, bz & 15);
    if (sky > 0)
        moodiness_ -= float(sky) / 15.0f * 0.001f;
    else
        moodiness_ -= float(lv.block4(bx & 15, by & 15, bz & 15) - 1) / float(ambient->moodTickDelay);
    if (moodiness_ >= 1.0f) {
        const double sx = bx + 0.5, sy = by + 0.5, sz = bz + 0.5;
        const double ddx = sx - p.pos[0], ddy = sy - eyeY, ddz = sz - p.pos[2];
        const double dist = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
        const double r = dist + ambient->moodOffset;
        const double pos[3] = {p.pos[0] + ddx / dist * r, eyeY + ddy / dist * r, p.pos[2] + ddz / dist * r};
        play(ambient->moodEvent, Category::Ambient, pos, 1.0f, 1.0f, rng_.nextLong());  // forAmbientMood
        moodiness_ = 0.0f;
    } else {
        moodiness_ = std::max(moodiness_, 0.0f);
    }
}

void AudioSystem::report() const {
    if (!ready()) return;
    const Engine::Stats s = engine_.stats();
    LOG("audio: %llu sounds played (%llu from the server), %u decoded in %.1f ms, %u dropped, %u stolen | main-thread "
        "update avg %.4f ms, max %.3f ms, %llu of %llu frames over 0.1 ms | cave moodiness %.3f",
        (unsigned long long)played_, (unsigned long long)netSounds_, s.decoded, s.decodeMs, s.dropped, s.stolen,
        updates_ ? updateMsTotal_ / double(updates_) : 0.0, updateMsMax_, (unsigned long long)slowUpdates_,
        (unsigned long long)updates_, moodiness_);
}

}  // namespace audio
