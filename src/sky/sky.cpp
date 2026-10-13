#include "sky/sky.h"

#include "ui/item_props.h"

#include "assets/assets.h"
#include "audio/audio.h"
#include "assets/game_reports.h"
#include "assets/asset_source.h"
#include "game/world_border.h"
#include "net/client.h"
#include "render/packs/pack_api.h"
#include "render/renderer.h"
#include "sky/sky_gpu.h"
#include "ui/item_props.h"
#include "world/nbt_dom.h"
#include "world/world.h"
#include "worldgen/rng.h"

#include <libdeflate.h>
#include <stb/stb_image.h>

#include <algorithm>
#include <cmath>

namespace sky {

namespace {

constexpr float kPi = 3.14159265358979f;

u32 packColor(float r, float g, float b, float a) {
    auto c = [](float v) { return u32(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

// 3x3 rotation, row-major, applied to column vectors.
struct Mat3 {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    void apply(const float in[3], float out[3]) const {
        for (int r = 0; r < 3; ++r) out[r] = m[r * 3] * in[0] + m[r * 3 + 1] * in[1] + m[r * 3 + 2] * in[2];
    }
};
Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
    return r;
}
Mat3 rotX(float a) {  // Axis.XP
    const float c = std::cos(a), s = std::sin(a);
    return {{1, 0, 0, 0, c, -s, 0, s, c}};
}
Mat3 rotY(float a) {  // Axis.YP
    const float c = std::cos(a), s = std::sin(a);
    return {{c, 0, s, 0, 1, 0, -s, 0, c}};
}
Mat3 rotZ(float a) {  // Axis.ZP
    const float c = std::cos(a), s = std::sin(a);
    return {{c, -s, 0, s, c, 0, 0, 0, 1}};
}

bool gunzipNbt(const fs::path& file, NbtValue& root) {
    std::vector<u8> gz;
    if (!readFile(file, gz) || gz.size() < 18) return false;
    const u32 isize = u32(gz[gz.size() - 4]) | (u32(gz[gz.size() - 3]) << 8) | (u32(gz[gz.size() - 2]) << 16) |
                      (u32(gz[gz.size() - 1]) << 24);
    std::vector<u8> raw(std::max<u32>(isize, 1));
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    size_t actual = 0;
    const bool ok =
        libdeflate_gzip_decompress(d, gz.data(), gz.size(), raw.data(), raw.size(), &actual) == LIBDEFLATE_SUCCESS;
    libdeflate_free_decompressor(d);
    return ok && nbtRead(raw.data(), actual, root);
}

bool loadImage(const AssetSource& src, const std::string& name, Image& out) {
    if (std::vector<u8> bytes; src.read(name, bytes)) {
        int w, h, n;
        u8* img = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
        if (!img) return false;
        out.w = w, out.h = h;
        out.rgba.assign(img, img + size_t(w) * h * 4);
        stbi_image_free(img);
        return true;
    }
    LOG("sky: %s missing from the game jar", name.c_str());
    return false;
}

// SkyRenderer.buildStars: 1500 candidates from the game's seeded random, as camera-centred quads.
std::vector<Sky::Frame::Vertex> buildStars() {
    std::vector<Sky::Frame::Vertex> v;
    wg::LegacyRandom random(10842);
    for (int i = 0; i < 1500; ++i) {
        const float x = random.nextFloat() * 2.0f - 1.0f, y = random.nextFloat() * 2.0f - 1.0f,
                    z = random.nextFloat() * 2.0f - 1.0f;
        const float size = 0.15f + random.nextFloat() * 0.1f;
        const float lengthSq = x * x + y * y + z * z;
        if (lengthSq <= 0.010000001f || lengthSq >= 1.0f) continue;
        const float inv = 1.0f / std::sqrt(lengthSq) * 100.0f;
        const float c[3] = {x * inv, y * inv, z * inv};
        const float zRot = float(random.nextDouble() * double(float(kPi)) * 2.0);
        // Matrix3f().rotateTowards(-centre, up): columns left, up, dir; then rotateZ(-zRot).
        const float dl = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        const float d[3] = {-c[0] / dl, -c[1] / dl, -c[2] / dl};
        float l[3] = {1 * d[2] - 0 * d[1], 0 * d[0] - 0 * d[2], 0 * d[1] - 1 * d[0]};  // up x dir, up = +y
        const float ll = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
        for (float& e : l) e /= ll;
        const float u[3] = {d[1] * l[2] - d[2] * l[1], d[2] * l[0] - d[0] * l[2], d[0] * l[1] - d[1] * l[0]};
        const float cr = std::cos(-zRot), sr = std::sin(-zRot);
        auto corner = [&](float sx, float sy) {
            const float rx = cr * sx - sr * sy, ry = sr * sx + cr * sy;
            Sky::Frame::Vertex vert{};
            for (int k = 0; k < 3; ++k) vert.pos[k] = l[k] * rx + u[k] * ry + c[k];
            vert.color = 0xFFFFFFFF;
            vert.layer = 0xFFFF;
            return vert;
        };
        const Sky::Frame::Vertex q[4] = {corner(size, -size), corner(size, size), corner(-size, size),
                                         corner(-size, -size)};
        for (int k : {0, 1, 2, 2, 3, 0}) v.push_back(q[k]);
    }
    return v;
}

Sky::Frame::Vertex vertex(const float p[3], float u, float v, u32 color, u32 layer) {
    Sky::Frame::Vertex r{};
    std::copy(p, p + 3, r.pos);
    r.uv[0] = u, r.uv[1] = v;
    r.color = color;
    r.layer = layer;
    return r;
}

// Vanilla lightmap.fsh for one (block, sky) light pair, with the default brightness option (0.5).
void lightmap(int block, int skyLevel, const Value* a, float out[3]) {
    auto brightness = [](float level) { return level / (4.0f - 3.0f * level); };
    const float bl = float(block) / 15.0f, sl = float(skyLevel) / 15.0f;
    const float blockB = brightness(bl) * 1.4f;
    const float skyB = brightness(sl) * a[int(Attr::SkyLightFactor)][0];
    const float mixF = 0.9f * (2.0f * bl - 1.0f) * (2.0f * bl - 1.0f);
    float c[3];
    float maxC = 0;
    for (int k = 0; k < 3; ++k) {
        const float tint = a[int(Attr::BlockLightTint)][k];
        c[k] = a[int(Attr::AmbientLightColor)][k] + a[int(Attr::SkyLightColor)][k] * skyB +
               (tint + (1.0f - tint) * mixF) * blockB;
        c[k] = std::clamp(c[k], 0.0f, 1.0f);
        maxC = std::max(maxC, c[k]);
    }
    const float inv = 1.0f - maxC, scaled = 1.0f - inv * inv * inv * inv;
    for (int k = 0; k < 3; ++k) out[k] = maxC > 0 ? c[k] + (c[k] * (scaled / maxC) - c[k]) * 0.5f : c[k];
}

}  // namespace

// ---- debug options -------------------------------------------------------------------------------

bool isOption(std::wstring_view arg) {
    return arg == L"--time" || arg == L"--weather" || arg == L"--dimension" || arg == L"--biome";
}

void parseOption(DebugOptions& o, std::wstring_view arg, std::wstring_view value) {
    const std::string v = fs::path(value).string();
    if (arg == L"--time") o.time = std::atof(v.c_str());
    else if (arg == L"--weather") o.weather = v == "thunder" ? 2 : v == "rain" ? 1 : 0;
    else if (arg == L"--dimension") o.dimension = v.find(':') == std::string::npos ? "minecraft:" + (v == "nether" ? "the_nether" : v == "end" ? "the_end" : v) : v;
    else if (arg == L"--biome") o.biome = v.find(':') == std::string::npos ? "minecraft:" + v : v;
}

// ---- state ---------------------------------------------------------------------------------------

struct Sky::NetState {
    std::mutex mutex;
    bool haveTime = false;
    i64 gameTime = 0;
    struct Clock {
        int id;
        i64 ticks;
        float partial, rate;
    };
    std::vector<Clock> clocks;
    std::vector<std::pair<int, float>> events;  // game_event type, parameter
    std::string level;                          // dimension of the last login / respawn
    bool joined = false;
    bool flat = false;                          // CommonPlayerSpawnInfo.isFlat (ClientLevelData.isFlat)
    int bolts = 0;  // lightning bolts added since the last frame
    bool frozen = false;  // ClientboundTickingStatePacket: the server's ticking is frozen (/tick freeze, pause)
};

// SimplexNoise (2D) as used by Biome's static temperature noises.
struct Sky::Noise {
    struct Simplex {
        double ox = 0, oy = 0;
        u8 perm[256] = {};
        void init(wg::RandomSource& r) {
            ox = r.nextDouble() * 0.0;
            oy = r.nextDouble() * 0.0;
            r.nextDouble();
            for (int i = 0; i < 256; ++i) perm[i] = u8(i);
            for (int i = 0; i < 256; ++i) std::swap(perm[i], perm[r.nextInt(256 - i) + i]);
        }
        int p(int x) const { return perm[x & 0xFF]; }
        static double corner(int g, double x, double y) {
            static constexpr int kGrad[12][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}, {1, 0}, {-1, 0},
                                                 {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0, 1}, {0, -1}};
            double t = 0.5 - x * x - y * y;
            if (t < 0.0) return 0.0;
            t *= t;
            return t * t * (kGrad[g][0] * x + kGrad[g][1] * y);
        }
        float get(double xin, double yin) const {
            static const double kF2 = 0.5 * (std::sqrt(3.0) - 1.0), kG2 = (3.0 - std::sqrt(3.0)) / 6.0;
            xin += ox, yin += oy;
            const double s = (xin + yin) * kF2;
            const int i = wg::jfloor(xin + s), j = wg::jfloor(yin + s);
            const double t = double(i + j) * kG2;
            const double x0 = xin - (i - t), y0 = yin - (j - t);
            const int i1 = x0 > y0 ? 1 : 0, j1 = x0 > y0 ? 0 : 1;
            const double x1 = x0 - i1 + kG2, y1 = y0 - j1 + kG2, x2 = x0 - 1.0 + 2.0 * kG2, y2 = y0 - 1.0 + 2.0 * kG2;
            const int ii = i & 0xFF, jj = j & 0xFF;
            return float(70.0 * (corner(p(ii + p(jj)) % 12, x0, y0) + corner(p(ii + i1 + p(jj + j1)) % 12, x1, y1) +
                                 corner(p(ii + 1 + p(jj + 1)) % 12, x2, y2)));
        }
    };
    Simplex temperature, frozen[3], info;
    Noise() {
        wg::LegacyRandom t(1234), f(3456), b(2345);
        temperature.init(t);
        for (Simplex& s : frozen) s.init(f);
        info.init(b);
    }
};

Sky::Sky() : net_(std::make_unique<NetState>()), frame_(std::make_unique<Frame>()), noise_(std::make_unique<Noise>()) {}
Sky::~Sky() {
    if (frames_)
        LOG("sky     cpu %.3f ms/frame avg | gpu %.3f ms avg (sky %.3f, clouds + weather %.3f)", cpuMs_ / double(frames_),
            gpu_ ? gpu_->gpuMs() : 0.0f, gpu_ ? gpu_->skyMs() : 0.0f, gpu_ ? gpu_->lateMs() : 0.0f);
}

bool Sky::init(Renderer& renderer, World& world, BlockRegistry& registry, const fs::path& jar,
               const DebugOptions& debug) {
    renderer_ = &renderer, world_ = &world, registry_ = &registry;
    debug_ = debug;
    Timer t;
    // Textures from the game's assets with the resource packs; dimension / biome data with the data packs.
    const AssetSource& zip = AssetSource::client(jar);
    if (zip.empty() || !env_.load(AssetSource::data(jar))) {
        LOG("sky: game data unavailable; sky disabled");
        return false;
    }
    dimension_ = env_.dimension(debug_.dimension.empty() ? "minecraft:overworld" : debug_.dimension);
    if (!dimension_) dimension_ = env_.dimension("minecraft:overworld");
    clocks_["minecraft:overworld"] = clocks_["minecraft:the_end"] = 0;
    if (debug_.time >= 0) clocks_["minecraft:overworld"] = debug_.time;
    if (debug_.weather >= 0) rain_ = rainTarget_ = debug_.weather ? 1.0f : 0.0f, thunder_ = thunderTarget_ = debug_.weather == 2 ? 1.0f : 0.0f;

    SkyImages images;
    const std::string env = "assets/minecraft/textures/environment/";
    bool ok = loadImage(zip, env + "celestial/sun.png", images.celestial[0]);
    static const char* const kPhases[8] = {"full_moon", "waning_gibbous",  "third_quarter", "waning_crescent",
                                           "new_moon",  "waxing_crescent", "first_quarter", "waxing_gibbous"};
    for (int i = 0; i < 8; ++i) ok &= loadImage(zip, env + "celestial/moon/" + kPhases[i] + ".png", images.celestial[i + 1]);
    ok &= loadImage(zip, env + "celestial/end_flash.png", images.celestial[9]);
    // One texture array: every image scaled (nearest) to the largest (the end flash is 64 x 64, the sun 32 x 32).
    int cw = 0, ch = 0;
    for (const Image& im : images.celestial) cw = std::max(cw, im.w), ch = std::max(ch, im.h);
    for (Image& im : images.celestial) {
        if (!ok || (im.w == cw && im.h == ch) || im.w <= 0 || im.h <= 0) continue;
        Image out;
        out.w = cw, out.h = ch, out.rgba.resize(size_t(cw) * ch * 4);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
                std::memcpy(&out.rgba[(size_t(y) * cw + x) * 4], &im.rgba[(size_t(y * im.h / ch) * im.w + x * im.w / cw) * 4], 4);
        im = std::move(out);
    }
    ok &= loadImage(zip, env + "end_sky.png", images.endSky) && loadImage(zip, env + "rain.png", images.rain) &&
          loadImage(zip, env + "snow.png", images.snow) &&
          loadImage(zip, "assets/minecraft/textures/misc/forcefield.png", images.forcefield);
    Image clouds;
    ok &= loadImage(zip, env + "clouds.png", clouds);
    if (!ok) return false;

    // CloudRenderer.prepare(ResourceManager): per texel, filled flag and which neighbours are empty.
    cloudW_ = clouds.w, cloudH_ = clouds.h;
    cloudCells_.assign(size_t(cloudW_) * cloudH_, 0);
    auto empty = [&](int x, int y) {
        x = ((x % cloudW_) + cloudW_) % cloudW_, y = ((y % cloudH_) + cloudH_) % cloudH_;
        return clouds.rgba[(size_t(y) * cloudW_ + x) * 4 + 3] < 10;
    };
    for (int y = 0; y < cloudH_; ++y)
        for (int x = 0; x < cloudW_; ++x)
            if (!empty(x, y))
                cloudCells_[size_t(y) * cloudW_ + x] =
                    1u | (empty(x, y - 1) ? 2u : 0u) | (empty(x + 1, y) ? 4u : 0u) | (empty(x, y + 1) ? 8u : 0u) |
                    (empty(x - 1, y) ? 16u : 0u);

    gpu_ = std::make_unique<SkyGpu>(*frame_);
    if (!gpu_->init(renderer, std::move(images), buildStars())) {
        gpu_.reset();
        return false;
    }
    LOG("sky     %8.1f ms  sky, clouds and weather ready", t.ms());
    return true;
}

void Sky::loadSave(const fs::path& worldDir) {
    NbtValue root;
    i64 dayTime = -1;
    if (gunzipNbt(worldDir / "level.dat", root))
        if (NbtValue* data = root.find("Data")) {
            if (NbtValue* v = data->find("Time")) gameTime_ = double(v->i);
            if (NbtValue* v = data->find("DayTime")) dayTime = v->i;  // before 26.x clocks
            if (NbtValue* v = data->find("raining")) rainTarget_ = v->i ? 1.0f : 0.0f;
            if (NbtValue* v = data->find("thundering")) thunderTarget_ = v->i ? 1.0f : 0.0f;
        }
    if (dayTime >= 0) clocks_["minecraft:overworld"] = double(dayTime);
    root = {};
    if (gunzipNbt(worldDir / "data" / "minecraft" / "world_clocks.dat", root))
        if (NbtValue* data = root.find("data"))
            for (size_t i = 0; i < data->names.size(); ++i) {
                NbtValue& c = data->items[i];
                if (NbtValue* t = c.find("total_ticks")) clocks_[data->names[i]] = double(t->i);
                const NbtValue* rate = c.find("rate");
                const NbtValue* paused = c.find("paused");
                clockRates_[data->names[i]] = paused && paused->i ? 0.0f : rate ? float(rate->d) : 1.0f;
            }
    root = {};
    if (gunzipNbt(worldDir / "data" / "minecraft" / "weather.dat", root))
        if (NbtValue* data = root.find("data")) {
            if (NbtValue* v = data->find("raining")) rainTarget_ = v->i ? 1.0f : 0.0f;
            if (NbtValue* v = data->find("thundering")) thunderTarget_ = v->i ? 1.0f : 0.0f;
        }
    if (debug_.time >= 0) clocks_["minecraft:overworld"] = debug_.time;
    if (debug_.weather >= 0) rainTarget_ = debug_.weather ? 1.0f : 0.0f, thunderTarget_ = debug_.weather == 2 ? 1.0f : 0.0f;
    rain_ = rainTarget_, thunder_ = thunderTarget_;
    LOG("sky: day time %.0f, game time %.0f, %s", clocks_["minecraft:overworld"], gameTime_,
        thunder_ > 0 ? "thunder" : rain_ > 0 ? "rain" : "clear");
}

void Sky::attach(net::Client& client, const GameReports* reports) {
    NetState* n = net_.get();
    int boltType = -1;
    if (reports && reports->registries().is_object()) {
        const auto& reg = reports->registries();
        if (reg.contains("minecraft:entity_type") && reg["minecraft:entity_type"].contains("entries")) {
            const auto& e = reg["minecraft:entity_type"]["entries"];
            if (e.contains("minecraft:lightning_bolt")) boltType = e["minecraft:lightning_bolt"].value("protocol_id", -1);
        }
    }
    if (boltType >= 0)
        client.onPlayPeek("add_entity", [n, boltType](net::Reader& r) {
            r.varInt();
            r.skip(16);  // uuid
            if (r.varInt() == boltType && r.ok) {
                std::lock_guard lock(n->mutex);
                ++n->bolts;
            }
        });
    client.onPlayPeek("set_time", [n](net::Reader& r) {
        const i64 gameTime = r.i64v();
        std::vector<NetState::Clock> clocks;
        const i32 count = r.varInt();
        for (i32 i = 0; i < count && r.ok; ++i) {
            NetState::Clock c;
            c.id = r.varInt();
            c.ticks = r.varLong();
            c.partial = r.f32();
            c.rate = r.f32();
            clocks.push_back(c);
        }
        if (!r.ok) return;
        std::lock_guard lock(n->mutex);
        n->haveTime = true;
        n->gameTime = gameTime;
        for (const NetState::Clock& c : clocks) n->clocks.push_back(c);
    });
    client.onPlayPeek("ticking_state", [n](net::Reader& r) {  // TickRateManager: a frozen level's clocks stand still
        r.f32();  // tick rate
        const bool frozen = r.boolean();
        if (!r.ok) return;
        std::lock_guard lock(n->mutex);
        n->frozen = frozen;
    });
    client.onPlayPeek("game_event", [n](net::Reader& r) {
        const int type = r.u8v();
        const float param = r.f32();
        if (r.ok && (type == 1 || type == 2 || type == 7 || type == 8)) {
            std::lock_guard lock(n->mutex);
            n->events.push_back({type, param});
        }
    });
    auto level = [n](net::Reader& r, bool login) {
        if (login) {
            r.i32v();
            r.boolean();
            const i32 levels = r.varInt();
            for (i32 i = 0; i < levels && r.ok; ++i) r.str();
            r.varInt(), r.varInt(), r.varInt();
            r.boolean(), r.boolean(), r.boolean();
        }
        r.varInt();  // dimension type
        const std::string name(r.str());
        r.i64v(), r.varInt(), r.u8v(), r.boolean();  // hashed seed, game mode, previous game mode, debug
        const bool flat = r.boolean();
        if (!r.ok) return;
        std::lock_guard lock(n->mutex);
        n->level = name;
        n->flat = flat;
        n->joined = true;
    };
    client.onPlayPeek("login", [level](net::Reader& r) { level(r, true); });
    client.onPlayPeek("respawn", [level](net::Reader& r) { level(r, false); });
}

void Sky::setBiomeLookup(BiomeLookup lookup) {
    biomeLookup_ = std::move(lookup);
    biomeCache_.clear();
}

void Sky::applyNet() {
    std::lock_guard lock(net_->mutex);
    NetState& n = *net_;
    if (n.joined) {
        n.joined = false;
        connected_ = true;
        if (debug_.dimension.empty())
            if (const DimensionInfo* d = env_.dimension(n.level)) dimension_ = d;
        flat_ = n.flat;
        LOG("sky: dimension %s", dimension_->name.c_str());
        rain_ = rainTarget_ = thunder_ = thunderTarget_ = 0;  // the server resends the weather
    }
    if (n.haveTime) {
        n.haveTime = false;
        gameTime_ = double(n.gameTime);
        // world_clock registry ids in data pack order: overworld, the_end.
        for (const NetState::Clock& c : n.clocks) {
            const char* name = c.id == 0 ? "minecraft:overworld" : c.id == 1 ? "minecraft:the_end" : nullptr;
            if (!name || (debug_.time >= 0 && c.id == 0)) continue;
            clocks_[name] = double(c.ticks) + double(c.partial);
            clockRates_[name] = c.rate;
            if (!loggedTime_ && c.id == 0) {
                loggedTime_ = true;
                LOG("sky: server time: game %lld, day clock %lld (rate %.2f)", (long long)n.gameTime, (long long)c.ticks, c.rate);
            }
        }
        n.clocks.clear();
    }
    for (const auto& [type, param] : n.events) {  // ClientPacketListener.handleGameEvent
        if (debug_.weather >= 0) break;
        if (type != 7 && type != 8) LOG("sky: server weather: %s", type == 1 ? "start raining" : "stop raining");
        if (type == 1) rainTarget_ = 0, rain_ = 0;           // start raining: ramps up from 0
        else if (type == 2) rainTarget_ = 1, rain_ = 1;      // stop raining: ramps down from 1
        else if (type == 7) rainTarget_ = std::clamp(param, 0.0f, 1.0f);
        else if (type == 8) thunderTarget_ = std::clamp(param, 0.0f, 1.0f);
    }
    n.events.clear();
    static wg::LegacyRandom boltRandom(i64(nowSeconds() * 1000.0));
    for (; n.bolts > 0; --n.bolts) bolts_.push_back({2, boltRandom.nextInt(3) + 1});
}

// ---- biomes ------------------------------------------------------------------------------------

int Sky::biomeAtQuart(int qx, int qy, int qz) {
    if (!debug_.biome.empty()) return env_.biome(debug_.biome);
    if (!biomeLookup_ || dimension_->name != "minecraft:overworld") {
        // No biome source: the dimension's typical biome.
        const char* fallback = dimension_->name == "minecraft:the_nether" ? "minecraft:nether_wastes"
                               : dimension_->name == "minecraft:the_end"  ? "minecraft:the_end"
                                                                          : "minecraft:plains";
        return env_.biome(fallback);
    }
    const u64 key = (u64(u32(qx) & 0x1FFFFF) << 42) | (u64(u32(qy) & 0x1FFFFF) << 21) | u64(u32(qz) & 0x1FFFFF);
    if (auto it = biomeCache_.find(key); it != biomeCache_.end()) return it->second;
    if (biomeCache_.size() > 65536) biomeCache_.clear();
    const int b = env_.biome(biomeLookup_(qx, qy, qz));
    biomeCache_[key] = b;
    return b;
}

int Sky::precipitationAt(int x, int y, int z) {
    if (!world_ || !world_->section(x >> 4, world_->minSectionY(), z >> 4, nullptr)) return 0;  // chunk not loaded
    const int biome = biomeAtQuart(x >> 2, y >> 2, z >> 2);
    if (biome < 0) return 0;
    if (!env_.biomeInfo(biome).precipitation) return 0;
    return temperature(biome, x, y, z) < 0.15f ? 2 : 1;  // Biome.coldEnoughToSnow
}

float Sky::temperature(int biome, int x, int y, int z) const {  // Biome.getHeightAdjustedTemperature
    const BiomeInfo& b = env_.biomeInfo(biome);
    float t = b.temperature;
    if (b.frozen) {
        float large = 0.14285715f * noise_->frozen[0].get(x * 0.05, z * 0.05) +
                      0.2857143f * noise_->frozen[1].get(x * 0.05 * 0.5, z * 0.05 * 0.5) +
                      0.5714286f * noise_->frozen[2].get(x * 0.05 * 0.25, z * 0.05 * 0.25);
        const double ice = double(large * 7.0f) + double(noise_->info.get(x * 0.2, z * 0.2));
        if (ice < 0.3 && double(noise_->info.get(x * 0.09, z * 0.09)) < 0.8) t = 0.2f;
    }
    constexpr int kSnowLevel = 63 + 17;
    if (y > kSnowLevel) {
        const float v = noise_->temperature.get(double(float(x) / 8.0f), double(float(z) / 8.0f)) * 8.0f;
        return t - (v + float(y) - float(kSnowLevel)) * 0.05f / 40.0f;
    }
    return t;
}

// Heightmap.Types.MOTION_BLOCKING: one above the highest block with collision or fluid.
int Sky::motionBlockingHeight(int x, int z) {
    const u64 key = World::chunkKey(x, z);
    if (auto it = heights_.find(key); it != heights_.end() && now_ - it->second.stamp < 1.0) return it->second.y;
    if (heights_.size() > 8192) heights_.clear();
    const int minY = dimension_->minY;
    int top = world_->minSectionY() * 16;
    const BlockInfo* infos = registry_->infos();
    for (int sy = world_->maxSectionY(); sy >= world_->minSectionY() && top == world_->minSectionY() * 16; --sy) {
        const World::Region* r = nullptr;
        const World::Section* s = world_->section(x >> 4, sy, z >> 4, &r);
        if (!s) continue;
        for (int ly = 15; ly >= 0; --ly) {
            const BlockInfo& b = infos[World::blockAt(*r, *s, u32(ly * 256 + (z & 15) * 16 + (x & 15)))];
            if (b.kind != KindAir && (b.collisionCount > 0 || b.fluid || b.waterlogged)) {
                top = sy * 16 + ly + 1;
                break;
            }
        }
    }
    top = std::max(top, minY);
    heights_[key] = {top, now_};
    return top;
}

// ---- per frame ---------------------------------------------------------------------------------

void Sky::update(const CameraState& cam, double dt) {
    if (!gpu_) return;
    Timer timer;
    applyNet();
    now_ += dt;
    dt = std::min(dt, 0.25);
    bool frozen;
    {
        std::lock_guard lock(net_->mutex);
        frozen = net_->frozen;
    }
    if (frozen) dt = 0;  // ClientLevel.tickTime does not run while the tick rate manager is frozen
    if (debug_.time < 0)
        for (auto& [name, ticks] : clocks_) {
            auto rate = clockRates_.find(name);
            ticks += dt * tickRate_ * (rate == clockRates_.end() ? 1.0 : rate->second);
        }
    gameTime_ += dt * tickRate_;
    // Lightning: ClientLevel.tick (skyFlashTime countdown) then LightningBolt.tick, at 20 ticks per second.
    tickAccumulator_ = std::min(tickAccumulator_ + dt * 20.0, 10.0);
    for (; tickAccumulator_ >= 1.0; tickAccumulator_ -= 1.0) {
        static wg::LegacyRandom random(12345);
        darkenO_ = darken_;  // GameRenderer.tick
        if (bossDarken_) darken_ = std::min(darken_ + 0.05f, 1.0f);
        else if (darken_ > 0.0f) darken_ -= 0.0125f;
        if (flashTime_ > 0) --flashTime_;
        for (size_t i = 0; i < bolts_.size();) {
            Bolt& b = bolts_[i];
            --b.life;
            if (b.life < 0) {
                if (b.flashes == 0) {
                    bolts_.erase(bolts_.begin() + std::ptrdiff_t(i));
                    continue;
                }
                if (b.life < -random.nextInt(10)) --b.flashes, b.life = 1;
            }
            if (b.life >= 0) flashTime_ = 2;
            ++i;
        }
    }
    // Weather levels follow the server's per-tick values (or the save's state) at the game's ramp speed.
    auto approach = [&](float& v, float target) {
        const float step = float(dt) * (connected_ ? 1.0f : 0.2f);
        v = v < target ? std::min(target, v + step) : std::max(target, v - step);
    };
    approach(rain_, rainTarget_);
    approach(thunder_, thunderTarget_);
    const bool weather = dimension_->canHaveWeather();
    const float rain = weather ? rain_ : 0.0f, thunder = weather ? thunder_ * rain_ : 0.0f;

    // EnvironmentAttributeProbe: biomes around the camera through the Gaussian kernel (quart resolution).
    biomeWeights_.clear();
    {
        static const double kKernel[7] = {0.0, 1.0, 4.0, 6.0, 4.0, 1.0, 0.0};
        const double px = cam.x * 0.25 - 0.5, py = cam.y * 0.25 - 0.5, pz = cam.z * 0.25 - 0.5;
        const int ix = int(std::floor(px)), iy = int(std::floor(py)), iz = int(std::floor(pz));
        const double rx = px - ix, ry = py - iy, rz = pz - iz;
        for (int z = 0; z < 6; ++z) {
            const double wz = kKernel[z + 1] + rz * (kKernel[z] - kKernel[z + 1]);
            for (int x = 0; x < 6; ++x) {
                const double wx = kKernel[x + 1] + rx * (kKernel[x] - kKernel[x + 1]);
                for (int y = 0; y < 6; ++y) {
                    const double wy = kKernel[y + 1] + ry * (kKernel[y] - kKernel[y + 1]);
                    const double w = wx * wy * wz;
                    if (w <= 0) continue;
                    const int b = biomeAtQuart(ix - 2 + x, iy - 2 + y, iz - 2 + z);
                    if (b < 0) continue;
                    auto it = std::find_if(biomeWeights_.begin(), biomeWeights_.end(),
                                           [b](const auto& e) { return e.first == b; });
                    if (it == biomeWeights_.end()) biomeWeights_.push_back({b, w});
                    else it->second += w;
                }
            }
        }
    }
    Value a[kAttrCount];
    Environment::Inputs in;
    in.dimension = dimension_;
    in.biomeWeights = &biomeWeights_;
    in.clocks = &clocks_;
    in.rain = rain, in.thunder = thunder;
    in.flash = flashTime_ > 0 && !options_.hideFlashes;  // Hide Sky Flashes
    env_.evaluate(in, a);

    // FogRenderer: the fluid at the camera picks the fog type (Camera.getFluidInCamera).
    const u16 eye = world_->block(int(std::floor(cam.x)), int(std::floor(cam.y)), int(std::floor(cam.z)));
    const std::string& eyeName = registry_->name(eye);
    const bool eyeWater = registry_->infos()[eye].waterlogged || eyeName.rfind("minecraft:water", 0) == 0 ||
                          eyeName.rfind("minecraft:bubble_column", 0) == 0;
    enum class FogType { Atmospheric, Water, Lava, PowderSnow };
    const FogType fogType = eyeWater                                         ? FogType::Water
                            : eyeName.rfind("minecraft:lava", 0) == 0        ? FogType::Lava
                            : eyeName.rfind("minecraft:powder_snow", 0) == 0 ? FogType::PowderSnow
                                                                             : FogType::Atmospheric;
    // computeFogColor: the first environment providing a colour (lava, powder snow, water, the atmosphere).
    const float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    const float forward[3] = {-sy * cp, -sp, cy * cp};
    float skyDark[3] = {a[int(Attr::SkyColor)][0], a[int(Attr::SkyColor)][1], a[int(Attr::SkyColor)][2]};
    if (rain > 0.0f) {
        const float m = 1.0f - rain * 0.5f, mb = 1.0f - rain * 0.4f;
        skyDark[0] = std::clamp(skyDark[0] * m, 0.0f, 1.0f), skyDark[1] = std::clamp(skyDark[1] * m, 0.0f, 1.0f);
        skyDark[2] = std::clamp(skyDark[2] * mb, 0.0f, 1.0f);
    }
    if (thunder > 0.0f)
        for (float& c : skyDark) c = std::clamp(c * (1.0f - thunder * 0.5f), 0.0f, 1.0f);
    float fog[3];
    if (fogType == FogType::Lava) {  // LavaFogEnvironment #991900
        fog[0] = 0x99 / 255.0f, fog[1] = 0x19 / 255.0f, fog[2] = 0;
    } else if (fogType == FogType::PowderSnow) {  // PowderedSnowFogEnvironment #9FBBCC
        fog[0] = 0x9F / 255.0f, fog[1] = 0xBB / 255.0f, fog[2] = 0xCC / 255.0f;
    } else if (fogType == FogType::Water) {  // WaterFogEnvironment: the biome's water fog colour
        for (int k = 0; k < 3; ++k) fog[k] = a[int(Attr::WaterFogColor)][k];
    } else {  // AtmosphericFogEnvironment.getBaseColor
        for (int k = 0; k < 3; ++k) fog[k] = a[int(Attr::FogColor)][k];
        const float sunAngle = a[int(Attr::SunAngle)][0] * kPi / 180.0f;
        const Value& sunrise = a[int(Attr::SunriseSunsetColor)];
        const float lookingAtSun = forward[0] * (std::sin(sunAngle) > 0.0f ? -1.0f : 1.0f);
        if (lookingAtSun > 0.0f && sunrise[3] > 0.0f)
            for (int k = 0; k < 3; ++k) fog[k] += (sunrise[k] - fog[k]) * lookingAtSun * sunrise[3];
        const float skyFogEnd = std::min(a[int(Attr::SkyFogEnd)][0] / 16.0f, float(renderDistance_));
        const float mix = 1.0f - std::pow(0.25f + 0.75f * std::clamp(skyFogEnd / 32.0f, 0.0f, 1.0f), 0.25f);
        for (int k = 0; k < 3; ++k) fog[k] += (skyDark[k] - fog[k]) * mix;
    }
    // The void darkness (ClientLevelData.voidDarknessOnsetRange: 1 block in a flat world, else 32), raised by the
    // first mob effect environment that modifies it (blindness, then darkness).
    const float onset = flat_ ? 1.0f : 32.0f;
    float voidDark = std::clamp((onset + float(dimension_->minY) - float(cam.y)) / onset, 0.0f, 1.0f);
    if (fx_.blindFog >= 0.0f) voidDark = std::max(fx_.blindDarkness, voidDark);
    else if (fx_.darkness) voidDark = std::max(fx_.darknessBlend, voidDark);
    if (voidDark > 0.0f && fogType != FogType::Lava && fogType != FogType::PowderSnow)
        for (float& c : fog) c *= (1.0f - voidDark) * (1.0f - voidDark);
    // FogRenderer.computeFogColor: the boss overlay's world darkening.
    const float darken = std::clamp(darkenO_ + (darken_ - darkenO_) * float(tickAccumulator_), 0.0f, 1.0f);
    static constexpr float kDarkenRgb[3] = {0.7f, 0.6f, 0.6f};
    if (darken > 0.0f)
        for (int k = 0; k < 3; ++k) fog[k] += (fog[k] * kDarkenRgb[k] - fog[k]) * darken;
    // Brightened towards full brightness by the water vision underwater, else by night vision.
    const float brighten = fogType == FogType::Water ? fx_.waterVision : fx_.fogBrighten;
    if (brighten > 0.0f && fog[0] != 0.0f && fog[1] != 0.0f && fog[2] != 0.0f) {
        const float scale = 1.0f / std::max(fog[0], std::max(fog[1], fog[2]));
        for (float& c : fog) c += (c * scale - c) * brighten;
    }

    Frame& f = *frame_;
    f.skybox = dimension_->skybox;
    std::copy(fog, fog + 3, f.fogColor);
    f.fogColor[3] = 1.0f;
    std::copy(a[int(Attr::SkyColor)].begin(), a[int(Attr::SkyColor)].begin() + 3, f.skyColor);
    f.skyEnd = std::min(float(renderDistance_ * 16), a[int(Attr::SkyFogEnd)][0]);
    // FogRenderer.setupFog: the first applicable environment sets the environmental (spherical) fog distances; the
    // renderer adds the render distance's own (cylindrical) fog.
    const float renderDistance = float(renderDistance_ * 16);
    float envStart = 1e30f, envEnd = 1e30f, skyEnd = 1e30f;
    // LevelRenderer.addSkyPass: no sky in lava or powder snow, or with blindness / darkness.
    bool hideSky = fogType == FogType::Lava || fogType == FogType::PowderSnow || fx_.blindFog >= 0.0f || fx_.darkness;
    if (fogType == FogType::Lava) {
        if (fx_.spectator) envStart = -8.0f, envEnd = renderDistance * 0.5f;
        else if (fx_.fireResistance) envStart = 0.0f, envEnd = 5.0f;
        else envStart = 0.25f, envEnd = 1.0f;
        skyEnd = envEnd;
    } else if (fogType == FogType::PowderSnow) {
        if (fx_.spectator) envStart = -8.0f, envEnd = renderDistance * 0.5f;
        else envStart = 0.0f, envEnd = 2.0f;
        skyEnd = envEnd;
    } else if (fx_.blindFog >= 0.0f) {  // BlindnessFogEnvironment
        const float d = renderDistance + (5.0f - renderDistance) * fx_.blindFog;
        envStart = d * 0.25f, envEnd = d, skyEnd = d * 0.8f;
    } else if (fx_.darkness) {  // DarknessFogEnvironment
        const float d = renderDistance + (15.0f - renderDistance) * fx_.darknessBlend;
        envStart = d * 0.75f, envEnd = d, skyEnd = d;
    } else if (fogType == FogType::Water) {  // WaterFogEnvironment
        envStart = a[int(Attr::WaterFogStart)][0];
        envEnd = a[int(Attr::WaterFogEnd)][0] * std::max(0.25f, fx_.waterVision);
        skyEnd = envEnd;
    } else {  // AtmosphericFogEnvironment: the dimension / biome distances, closed in by rain
        {  // updateRainFogState: rain at the camera under open sky (half where the biome has no precipitation)
            const int bx = int(std::floor(cam.x)), by = int(std::floor(cam.y)), bz = int(std::floor(cam.z));
            float skyLight = 15.0f;
            if (world_ && by >= world_->minSectionY() * 16 && by < (world_->maxSectionY() + 1) * 16)
                skyLight = float(world_->light(bx >> 4, by >> 4, bz >> 4).sky4(bx & 15, by & 15, bz & 15));
            const int biome = biomeAtQuart(bx >> 2, by >> 2, bz >> 2);
            const bool rains = biome < 0 || env_.biomeInfo(biome).precipitation;
            const float target = rain * std::clamp((skyLight - 8.0f) / 7.0f, 0.0f, 1.0f) * (rains ? 1.0f : 0.5f);
            rainFog_ += (target - rainFog_) * std::min(float(dt * tickRate_) * 0.2f, 1.0f);
        }
        envStart = a[int(Attr::FogStart)][0] - 160.0f * rainFog_;
        envEnd = std::max(std::min(96.0f, a[int(Attr::FogEnd)][0]), a[int(Attr::FogEnd)][0] - 256.0f * rainFog_);
        if (bossFog_) {  // a boss bar's world fog: start <= 10, end <= 96, the sky and clouds end with it
            envStart = std::min(envStart, 10.0f), envEnd = std::min(envEnd, 96.0f), skyEnd = envEnd;
        }
    }
    f.skyEnd = std::min(f.skyEnd, skyEnd);
    cloudFogLimit_ = skyEnd;  // FogData.cloudEnd follows the sky end in every environment but the plain atmosphere
    f.renderSky = !hideSky;
    renderer_->setFogLimit(envStart, envEnd);
    // ClientLevelData.getHorizonHeight: the bottom of the world when it is flat, else 63; not drawn underwater.
    f.darkDisc = cam.y - (flat_ ? double(dimension_->minY) : 63.0) < 0.0 && fogType != FogType::Water;  // not under water
    renderer_->setAtmosphere(f.fogColor, a[int(Attr::SkyLightFactor)][0]);
    skyDarken_ = int(15.0f - a[int(Attr::SkyLightLevel)][0]);
    {  // the game's lightmap inputs (LightmapRenderStateExtractor)
        Lightmap lm;
        for (int c = 0; c < 3; ++c) {
            lm.ambient[c] = a[int(Attr::AmbientLightColor)][c];
            lm.skyColor[c] = a[int(Attr::SkyLightColor)][c];
            lm.blockTint[c] = a[int(Attr::BlockLightTint)][c];
        }
        lm.skyFactor = a[int(Attr::SkyLightFactor)][0];
        // lightmap.fsh: night vision lifts the ambient colour to the night vision colour (max per channel).
        if (fx_.lightmapNightVision > 0.0f)
            for (int c = 0; c < 3; ++c)
                lm.ambient[c] = std::max(lm.ambient[c], a[int(Attr::NightVisionColor)][c] * fx_.lightmapNightVision);
        if (darken > 0.0f) {
            // lightmap.fsh: color = mix(color, color * (0.7, 0.6, 0.6), BossOverlayWorldDarkeningFactor) before the
            // clamp: ambient and sky light scale exactly; the block term's white part scales by the mean factor.
            float m[3], mean = 0;
            for (int c = 0; c < 3; ++c) m[c] = 1.0f - darken + darken * kDarkenRgb[c], mean += m[c] / 3.0f;
            for (int c = 0; c < 3; ++c) lm.ambient[c] *= m[c], lm.skyColor[c] *= m[c], lm.blockTint[c] *= m[c] / mean;
            lm.blockFactor *= mean;
        }
        // The darkness effect: color - DarknessScale before the clamp (the sum's constant part, so the ambient term),
        // and the brightness option lowered by its blend factor.
        for (float& c : lm.ambient) c -= fx_.darknessScale;
        lm.brightness = std::max(0.0f, options_.brightness - fx_.darknessBlend * fx_.darknessEffectScale);  // the Brightness option
        renderer_->setLightmap(lm);
    }
    {  // the renderer's shader pipeline lights the world from the same sun / moon and weather
        SkyLighting sl;
        sl.sunAngle = a[int(Attr::SunAngle)][0] * kPi / 180.0f;
        sl.moonAngle = a[int(Attr::MoonAngle)][0] * kPi / 180.0f;
        std::copy(skyDark, skyDark + 3, sl.skyColor);
        sl.rain = rain, sl.thunder = thunder;
        sl.overworld = f.skybox == Skybox::Overworld;
        sl.cameraInWater = fogType == FogType::Water;
        sl.skyHidden = hideSky;
        sl.dim = 1.0f - std::min(1.0f, fx_.darknessScale * 2.0f);
        renderer_->setSkyLighting(sl);

        // Shader packs (render/packs): the game state behind their uniforms.
        packs::PackGameState g;
        g.valid = true;
        g.dimension = dimension_->name;
        auto clock = clocks_.find(dimension_->clock.empty() ? std::string("minecraft:overworld") : dimension_->clock);
        if (clock != clocks_.end()) g.worldTime = i64(std::floor(clock->second));
        g.moonPhase = std::clamp(int(a[int(Attr::MoonPhase)][0]), 0, 7);
        // Item model properties (clock needles: minecraft:time daytime / moon_phase; wobble per level tick).
        ui::ItemWorld& iw = ui::itemWorld();
        iw.sunAngle = a[int(Attr::SunAngle)][0], iw.moonPhase = g.moonPhase;
        iw.gameTime = i64(std::floor(gameTime_)), iw.ticking = tickRate_ > 0;
        const int ex = int(std::floor(cam.x)), ey = int(std::floor(cam.y)), ez = int(std::floor(cam.z));
        const World::LightView lv = world_->light(ex >> 4, ey >> 4, ez >> 4);
        g.eyeBrightness[0] = lv.block4(ex & 15, ey & 15, ez & 15) * 16;
        g.eyeBrightness[1] = dimension_->hasSkylight ? lv.sky4(ex & 15, ey & 15, ez & 15) * 16 : 0;
        g.isEyeInWater = sl.cameraInWater ? 1 : fogType == FogType::Lava ? 2 : fogType == FogType::PowderSnow ? 3 : 0;
        int dominant = -1;
        double best = 0;
        for (const auto& [b, w] : biomeWeights_)
            if (w > best) best = w, dominant = b;
        if (dominant >= 0) {
            const BiomeInfo& bi = env_.biomeInfo(dominant);
            g.biome = bi.name;
            g.temperature = temperature(dominant, ex, ey, ez);
            g.precipitation = !bi.precipitation ? 0 : g.temperature < 0.15f ? 2 : 1;
        }
        packs::setGameState(*renderer_, g);
    }

    buildCelestial(a, rain, f.darkDisc);
    buildClouds(cam, a);
    buildWeather(cam, a, rain);
    buildBorder(cam);

    cpuMs_ += timer.ms();
    ++frames_;
}

// WorldBorderRenderer.extract / prepare / render: the four walls as quads from y -depthFar to depthFar, those
// closer than the render distance drawn with the forcefield texture scrolling diagonally, tinted by the border's
// status, faded in by (1 - distance / render distance)^4.
void Sky::buildBorder(const CameraState& cam) {
    Frame& f = *frame_;
    f.border.clear();
    if (!border_) return;
    const WorldBorder& b = *border_;
    const double rd = borderRange_;
    const double minX = b.minX(borderPartial_), maxX = b.maxX(borderPartial_);
    const double minZ = b.minZ(borderPartial_), maxZ = b.maxZ(borderPartial_);
    const double x = cam.x, z = cam.z;
    const bool nearEdge = !(x < maxX - rd) || !(x > minX + rd) || !(z < maxZ - rd) || !(z > minZ + rd);
    const bool farOutside = x < minX - rd || x > maxX + rd || z < minZ - rd || z > maxZ + rd;
    if (!nearEdge || farOutside) return;
    const double alpha = std::clamp(std::pow(1.0 - b.distanceToBorder(x, z) / rd, 4.0), 0.0, 1.0);
    if (alpha <= 0.0) return;
    const u32 tint = b.color();
    const u32 color = (u32(alpha * 255.0 + 0.5) << 24) | ((tint & 0xFF) << 16) | (tint & 0xFF00) | ((tint >> 16) & 0xFF);
    // rebuildWorldBorderBuffer: the wall spans the render distance around the camera along each side.
    const double bMinX = minX + 0.01, bMaxX = maxX - 0.01, bMinZ = minZ + 0.01, bMaxZ = maxZ - 0.01;
    const double z0 = std::max(std::floor(z - rd), bMinZ), z1 = std::min(std::ceil(z + rd), bMaxZ);
    const double x0 = std::max(std::floor(x - rd), bMinX), x1 = std::min(std::ceil(x + rd), bMaxX);
    const float u0z = float(i64(std::floor(z0)) & 1) * 0.5f, u1z = float(z1 - z0) / 2.0f;
    const float u0x = float(i64(std::floor(x0)) & 1) * 0.5f, u1x = float(x1 - x0) / 2.0f;
    const float h = float(std::max(rd * 4.0, 2048.0));  // Camera depthFar (the cloud range's 2048 at least)
    const float v0 = -float(cam.y * 0.5 - std::floor(cam.y * 0.5)), v1 = v0 + h;
    // TextureMat: the texture slides by (t, t) every 3 seconds.
    const float slide = float(std::fmod(nowSeconds() * 1000.0, 3000.0) / 3000.0);
    auto quad = [&](const double (&p)[4][2], const float (&u)[4]) {  // p: world x, z; corners low, low, high, high
        static constexpr int kOrder[6] = {0, 1, 2, 2, 3, 0};
        for (int k : kOrder) {
            const bool top = k >= 2;
            Frame::Vertex v{};
            v.pos[0] = float(p[k][0] - x), v.pos[1] = float((top ? h : -h) - cam.y), v.pos[2] = float(p[k][1] - z);
            v.uv[0] = u[k] + slide, v.uv[1] = (top ? v0 : v1) + slide;
            v.color = color;
            f.border.push_back(v);
        }
    };
    // WorldBorderRenderState.closestBorder: nearest first; Direction.get2DDataValue picks the quad.
    struct Side {
        int index;
        double distance;
    } sides[4] = {{2, z - minZ}, {0, maxZ - z}, {1, x - minX}, {3, maxX - x}};
    std::stable_sort(std::begin(sides), std::end(sides), [](const Side& a, const Side& c) { return a.distance < c.distance; });
    for (const Side& s : sides) {
        if (!(s.distance < rd)) continue;
        switch (s.index) {
            case 0: quad({{x0, bMaxZ}, {x1, bMaxZ}, {x1, bMaxZ}, {x0, bMaxZ}}, {u0x, u1x + u0x, u1x + u0x, u0x}); break;  // south
            case 1: quad({{x0, z0}, {x0, z1}, {x0, z1}, {x0, z0}}, {u0z, u1z + u0z, u1z + u0z, u0z}); break;          // west
            case 2: quad({{x1, z0}, {x0, z0}, {x0, z0}, {x1, z0}}, {u0x, u1x + u0x, u1x + u0x, u0x}); break;          // north
            default: quad({{bMaxX, z1}, {bMaxX, z0}, {bMaxX, z0}, {bMaxX, z1}}, {u0z, u1z + u0z, u1z + u0z, u0z}); break;  // east
        }
    }
}

void Sky::buildCelestial(const Value* a, float rain, bool) {
    Frame& f = *frame_;
    f.fan.clear(), f.additive.clear(), f.endSky.clear();
    f.starBrightness = 0;
    if (f.skybox == Skybox::End) {  // SkyRenderer.buildEndSky, then renderEndFlash
        endFlash(f);
        for (int i = 0; i < 6; ++i) {
            Mat3 pose;
            if (i == 1) pose = rotX(kPi / 2);
            if (i == 2) pose = rotX(-kPi / 2);
            if (i == 3) pose = rotX(kPi);
            if (i == 4) pose = rotZ(kPi / 2);
            if (i == 5) pose = rotZ(-kPi / 2);
            const float c[4][5] = {{-100, -100, -100, 0, 0}, {-100, -100, 100, 0, 16}, {100, -100, 100, 16, 16},
                                   {100, -100, -100, 16, 0}};
            Frame::Vertex q[4];
            for (int k = 0; k < 4; ++k) {
                float p[3];
                pose.apply(c[k], p);
                q[k] = vertex(p, c[k][3], c[k][4], 0xFF282828, 100);
            }
            for (int k : {0, 1, 2, 2, 3, 0}) f.endSky.push_back(q[k]);
        }
        return;
    }
    if (f.skybox != Skybox::Overworld) return;

    const float sunAngle = a[int(Attr::SunAngle)][0] * kPi / 180.0f;
    const float moonAngle = a[int(Attr::MoonAngle)][0] * kPi / 180.0f;
    const float starAngle = a[int(Attr::StarAngle)][0] * kPi / 180.0f;
    const Value& sunrise = a[int(Attr::SunriseSunsetColor)];
    // renderSunriseAndSunset: a fan from the horizon point under the sun, flattened by the colour's alpha.
    if (sunrise[3] > 0.001f) {
        const float angle = std::sin(sunAngle) < 0.0f ? 180.0f : 0.0f;
        const Mat3 m = mul(rotX(kPi / 2), rotZ((angle + 90.0f) * kPi / 180.0f));
        auto put = [&](float x, float y, float z, float alpha) {
            const float in[3] = {x, y, z * sunrise[3]};
            float p[3];
            m.apply(in, p);
            return vertex(p, 0, 0, packColor(sunrise[0], sunrise[1], sunrise[2], sunrise[3] * alpha), 0xFFFF);
        };
        const Frame::Vertex center = put(0, 100, 0, 1);
        Frame::Vertex prev = put(0, 120, -40, 0);
        for (int i = 1; i <= 16; ++i) {
            const float t = float(i) * kPi * 2.0f / 16.0f, s = std::sin(t), c = std::cos(t);
            const Frame::Vertex next = put(s * 120.0f, c * 120.0f, -c * 40.0f, 0);
            f.fan.push_back(center), f.fan.push_back(prev), f.fan.push_back(next);
            prev = next;
        }
    }
    // renderSunMoonAndStars: Y -90 deg, then X by each body's angle; quads at height 100.
    const float rainBrightness = 1.0f - rain;
    const u32 white = packColor(1, 1, 1, rainBrightness);
    const Mat3 tilt = rotX(renderer_->sunPathTilt());  // shader pipeline: tilted path matching its shadows
    auto body = [&](float angle, float size, u32 layer, bool moon) {
        const Mat3 m = mul(tilt, mul(rotY(-kPi / 2), rotX(angle)));
        const float c[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        const float uvSun[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}}, uvMoon[4][2] = {{1, 1}, {0, 1}, {0, 0}, {1, 0}};
        Frame::Vertex q[4];
        for (int k = 0; k < 4; ++k) {
            const float in[3] = {c[k][0] * size, 100.0f, c[k][1] * size};
            float p[3];
            m.apply(in, p);
            const float* uv = moon ? uvMoon[k] : uvSun[k];
            q[k] = vertex(p, uv[0], uv[1], white, layer);
        }
        for (int k : {0, 1, 2, 2, 3, 0}) f.additive.push_back(q[k]);
    };
    body(sunAngle, 30.0f, 0, false);
    body(moonAngle, 20.0f, 1 + u32(std::clamp(int(a[int(Attr::MoonPhase)][0]), 0, 7)), true);
    f.starBrightness = a[int(Attr::StarBrightness)][0];
    const Mat3 stars = mul(tilt, mul(rotY(-kPi / 2), rotX(starAngle)));
    std::copy(stars.m, stars.m + 9, f.starRot);
}

// EndFlashState: one flash per 600 ticks of the dimension's clock, at a seeded offset, duration and direction.
float Sky::endFlashIntensity(i64 tick) {
    const i64 seed = tick / 600;
    if (seed != endFlashSeed_) {  // calculateFlashParameters
        wg::LegacyRandom r(seed);
        r.nextFloat();
        endFlashOffset_ = r.nextIntBetweenInclusive(0, 200);
        endFlashDuration_ = r.nextIntBetweenInclusive(100, std::min(380, 600 - endFlashOffset_));
        endFlashAngle_[0] = r.nextFloat() * 70.0f - 60.0f;    // xAngle in [-60, 10)
        endFlashAngle_[1] = r.nextFloat() * 360.0f - 180.0f;  // yAngle in [-180, 180)
        endFlashSeed_ = seed;
    }
    const i64 within = tick % 600;
    return within >= endFlashOffset_ && within <= endFlashOffset_ + endFlashDuration_
               ? std::sin(float(within - endFlashOffset_) * kPi / float(endFlashDuration_))
               : 0.0f;
}

void Sky::endFlash(Frame& f) {
    auto it = dimension_ ? clocks_.find(dimension_->clock) : clocks_.end();
    const double clock = it != clocks_.end() ? it->second : 0.0;  // getDefaultClockTime: 0 without a clock
    const i64 tick = i64(std::floor(clock));
    const float pt = float(clock - double(tick));
    // ClientLevel.tick: the flash sound (weather.end_flash) 30 ticks after a flash starts, from its direction.
    if (endFlashTick_ != INT64_MIN && tick > endFlashTick_ && tick - endFlashTick_ < 40)
        for (i64 t = endFlashTick_ + 1; t <= tick; ++t)
            if (endFlashIntensity(t) > 0.0f && endFlashIntensity(t - 1) <= 0.0f) endFlashSoundAt_ = t + 30;
    endFlashTick_ = tick;
    const float old = endFlashIntensity(tick - 1), now = endFlashIntensity(tick);
    const float xAngle = endFlashAngle_[0], yAngle = endFlashAngle_[1];
    if (endFlashSoundAt_ != INT64_MIN && tick >= endFlashSoundAt_) {
        endFlashSoundAt_ = INT64_MIN;
        if (audio::AudioSystem* a = audio::instance(); a && a->ready()) {  // DirectionalSoundInstance: 10 blocks out
            const float xr = xAngle * kPi / 180.0f, yr = -yAngle * kPi / 180.0f;
            const double dir[3] = {std::sin(yr) * std::cos(xr) * 10.0, -std::sin(xr) * 10.0, std::cos(yr) * std::cos(xr) * 10.0};
            a->play(a->eventId("minecraft:weather.end_flash"), audio::Category::Weather, dir, 1.0f, 1.0f, i64(endFlashRng_()), false, true);
        }
    }
    const float intensity = old + (now - old) * pt;
    if (intensity <= 1e-5f) return;
    // renderEndFlash: Y by 180 - yAngle, X by -90 - xAngle, a 60-wide quad at height 100, coloured by the intensity.
    const Mat3 m = mul(rotY((180.0f - yAngle) * kPi / 180.0f), rotX((-90.0f - xAngle) * kPi / 180.0f));
    const float c[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}, uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const u32 color = packColor(intensity, intensity, intensity, intensity);
    Frame::Vertex q[4];
    for (int k = 0; k < 4; ++k) {
        const float in[3] = {c[k][0] * 60.0f, 100.0f, c[k][1] * 60.0f};
        float p[3];
        m.apply(in, p);
        q[k] = vertex(p, uv[k][0], uv[k][1], color, 9);
    }
    for (int k : {0, 1, 2, 2, 3, 0}) f.additive.push_back(q[k]);
}

void Sky::setOptions(const Options& o) {
    if (o.clouds != options_.clouds || o.cloudRange != options_.cloudRange) cloudCellX_ = INT_MIN;  // rebuild the mesh
    options_ = o;
}

// CloudRenderer.prepare + buildMesh (fancy clouds: extruded cells; fast: flat cells, seen from both sides).
void Sky::buildClouds(const CameraState& cam, const Value* a) {
    Frame& f = *frame_;
    const Value& color = a[int(Attr::CloudColor)];
    f.clouds = options_.clouds != 0 && color[3] > 0.0f && dimension_->skybox == Skybox::Overworld && cloudW_ > 0;
    if (!f.clouds) return;
    const float bottom = a[int(Attr::CloudHeight)][0];
    const float relBottom = float(bottom - cam.y), relTop = relBottom + 4.0f;
    const int band = relTop < 0.0f ? 0 : relBottom > 0.0f ? 2 : 1;  // above, inside, below
    const double offset = std::fmod(gameTime_, double(cloudW_) * 400.0);
    double cloudX = cam.x + offset * 0.030000001f, cloudZ = cam.z + 3.96f;
    const double wBlocks = cloudW_ * 12.0, hBlocks = cloudH_ * 12.0;
    cloudX -= std::floor(cloudX / wBlocks) * wBlocks;
    cloudZ -= std::floor(cloudZ / hBlocks) * hBlocks;
    const int cellX = int(std::floor(cloudX / 12.0)), cellZ = int(std::floor(cloudZ / 12.0));
    const float xIn = float(cloudX - cellX * 12.0), zIn = float(cloudZ - cellZ * 12.0);
    if (cellX != cloudCellX_ || cellZ != cloudCellZ_ || band != cloudBand_) {
        cloudCellX_ = cellX, cloudCellZ_ = cellZ, cloudBand_ = band;
        cloudFaces_.clear();
        const int radius = (options_.cloudRange * 16 + 11) / 12;
        const bool flat = options_.clouds == 1;
        auto encode = [&](int x, int z, u32 flags) {
            cloudFaces_.push_back((u32(x) & 0xFFF) | ((u32(z) & 0xFFF) << 12) | (flags << 24));
        };
        auto cell = [&](int rx, int rz) {
            const int ix = (((cellX + rx) % cloudW_) + cloudW_) % cloudW_, iz = (((cellZ + rz) % cloudH_) + cloudH_) % cloudH_;
            const u32 c = cloudCells_[size_t(iz) * cloudW_ + ix];
            if (!c) return;
            if (flat) {  // buildFlatCell: the down face with the top colour, both windings (no culling)
                encode(rx, rz, 0 | 32);
                encode(rx, rz, 0 | 16 | 32);
                return;
            }
            if (band != 2) encode(rx, rz, 1);  // up (not when below the clouds)
            if (band != 0) encode(rx, rz, 0);  // down (not when above)
            if ((c & 2) && rz > 0) encode(rx, rz, 2);   // north
            if ((c & 8) && rz < 0) encode(rx, rz, 3);   // south
            if ((c & 16) && rx > 0) encode(rx, rz, 4);  // west
            if ((c & 4) && rx < 0) encode(rx, rz, 5);   // east
            if (std::abs(rx) <= 1 && std::abs(rz) <= 1)
                for (u32 d = 0; d < 6; ++d) encode(rx, rz, d | 16);  // interior faces
        };
        for (int ring = 0; ring <= 2 * radius; ++ring)
            for (int rx = -ring; rx <= ring; ++rx) {
                const int rz = ring - std::abs(rx);
                if (rz < 0 || rz > radius || rx * rx + rz * rz > radius * radius) continue;
                if (rz != 0) cell(rx, -rz);
                cell(rx, rz);
            }
        ++cloudVersion_;
    }
    std::copy(color.begin(), color.end(), f.cloudColor);
    f.cloudOffset[0] = -xIn, f.cloudOffset[1] = relBottom, f.cloudOffset[2] = -zIn;
    f.cloudFogEnd = std::min(float(options_.cloudRange * 16), a[int(Attr::CloudFogEnd)][0]);
    if (cloudFogLimit_ < 1e29f) f.cloudFogEnd = cloudFogLimit_;  // a fog environment's own cloud end (boss fog: = fog end)
    f.cloudFaces = &cloudFaces_;
    f.cloudVersion = cloudVersion_;
}

// WeatherEffectRenderer.extractRenderState + prepareInstances.
void Sky::buildWeather(const CameraState& cam, const Value* a, float rain) {
    Frame& f = *frame_;
    f.weather.clear();
    f.rainVertices = 0;
    if (rain <= 0.0f) return;
    const int r = options_.weatherRadius;
    const int camX = int(std::floor(cam.x)), camY = int(std::floor(cam.y)), camZ = int(std::floor(cam.z));
    const i64 ticks = i64(gameTime_);
    const float partial = float(gameTime_ - std::floor(gameTime_));
    const int wrapped = int(ticks & 131071);
    wg::LegacyRandom random(0);
    std::vector<Frame::Vertex> snow;
    for (int z = camZ - r; z <= camZ + r; ++z)
        for (int x = camX - r; x <= camX + r; ++x) {
            if (x == camX && z == camZ) continue;  // the game's column table is NaN here: nothing drawn
            const int height = motionBlockingHeight(x, z);
            const int y0 = std::max(camY - r, height), y1 = std::max(camY + r, height);
            if (y1 == y0) continue;
            const int precipitation = precipitationAt(x, camY, z);  // NONE where the chunk is not loaded
            if (!precipitation) continue;
            const bool isSnow = precipitation == 2;
            const i32 seed = i32(u32(x) * u32(x) * 3121u + u32(x) * 45238971u) ^ i32(u32(z) * u32(z) * 418711u + u32(z) * 13761u);
            random.setSeed(seed);
            const int lightY = std::max(camY, height);
            const u8 light = world_->light(x >> 4, lightY >> 4, z >> 4).at(x & 15, lightY & 15, z & 15);
            int skyL = light >> 4, blockL = light & 15;
            float u = 0, v;
            if (!isSnow) {
                const int tickOffset = int((u32(x) * u32(x) * 3121u + u32(x) * 45238971u + u32(z) * u32(z) * 418711u + u32(z) * 13761u) & 0xFF);
                const float speed = 3.0f + random.nextFloat();
                const float tex = -(float(wrapped + tickOffset) + partial) / 32.0f * speed;
                v = std::fmod(tex, 32.0f);
            } else {
                const float time = float(wrapped) + partial;
                const double d0 = random.nextDouble();
                u = float(d0 + double(time * 0.01f * float(random.nextGaussian())));
                const double d1 = random.nextDouble();
                const float vv = float(d1 + double(time * float(random.nextGaussian()) * 0.001f));
                v = -(float(ticks & 511) + partial) / 512.0f + vv;
                blockL = (blockL * 3 + 15) / 4, skyL = (skyL * 3 + 15) / 4;
            }
            float lm[3];
            lightmap(blockL, skyL, a, lm);
            const float relX = float(x + 0.5 - cam.x), relZ = float(z + 0.5 - cam.z);
            const float distSq = relX * relX + relZ * relZ;
            const float maxAlpha = isSnow ? 0.8f : 1.0f;
            const float alpha = (maxAlpha + std::min(distSq / float(r * r), 1.0f) * (0.5f - maxAlpha)) * rain;
            const u32 color = packColor(lm[0], lm[1], lm[2], alpha);
            const float dx = float(x - camX), dz = float(z - camZ), len = std::sqrt(dx * dx + dz * dz);
            const float hx = -dz / len / 2.0f, hz = dx / len / 2.0f;
            const float yTop = float(y1 - cam.y), yBottom = float(y0 - cam.y);
            const float v0 = float(y0) * 0.25f + v, v1 = float(y1) * 0.25f + v;
            const u32 layer = isSnow ? 1 : 0;
            const float p0[3] = {relX - hx, yTop, relZ - hz}, p1[3] = {relX + hx, yTop, relZ + hz},
                        p2[3] = {relX + hx, yBottom, relZ + hz}, p3[3] = {relX - hx, yBottom, relZ - hz};
            const Frame::Vertex q[4] = {vertex(p0, u, v0, color, layer), vertex(p1, u + 1, v0, color, layer),
                                        vertex(p2, u + 1, v1, color, layer), vertex(p3, u, v1, color, layer)};
            auto& out = isSnow ? snow : f.weather;
            for (int k : {0, 1, 2, 2, 3, 0}) out.push_back(q[k]);
        }
    f.rainVertices = u32(f.weather.size());
    f.weather.insert(f.weather.end(), snow.begin(), snow.end());
}

}  // namespace sky
