#include "assets/assets.h"
#include "assets/block_shapes.h"
#include "assets/game_reports.h"
#include "audio/audio.h"
#include "audio/audio_test.h"
#include "core/common.h"
#include "core/jobs.h"
#include "entity/entities.h"
#include "blockentity/block_entities.h"
#include "entity/entity_render.h"
#include "entity/visuals.h"
#include "game/game.h"
#include "mods/mod_manager.h"
#include "net/local_server.h"
#include "net/session.h"
#include "net/status.h"
#include "platform/launch.h"
#include "platform/crash.h"
#include "platform/ownership.h"
#include "platform/window.h"
#include "render/renderer.h"
#include "render/upscalers/upscalers.h"
#include "sky/sky.h"
#include "ui/ui.h"
#include "world/biome_colors.h"
#include "world/lighting.h"
#include "world/save.h"
#include "world/terrain.h"
#include "worldgen/feature_api.h"
#include "worldgen/gen_service.h"
#include "worldgen/gentest.h"
#include "render/packs/pack_api.h"
#include "worldgen/structures.h"
#include "world/world.h"

#include <algorithm>
#include <deque>
#include <future>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <random>

namespace {

constexpr float kPi = 3.14159265f;

struct Options {
    fs::path world, jar, screenshot;
    int radius = 0;   // chunks of the save to load (0 = all)
    int detail = 32;  // full-detail radius in chunks; LOD beyond
    float fog = 16384.0f;
    int width = 1920, height = 1080;
    bool bench = false, fly = false, debug = false, vsync = false, selftest = false;
    bool readonly = false, forceSave = false;
    fs::path gentestDir;
    long long gentestSeed = 0;
    int gentestChunks = 1000;
    int shotFrame = 30;
    bool generate = true;
    int genRadius = -1;  // chunks; default: detail radius + 2
    bool profile = false;  // log the phases of slow frames
    float lodQuality = 2.0f;  // how far each coarser LOD level is pushed out
    float farBlocks = -1.0f;  // blocks of generated far terrain; default: the fog distance
    bool haveCam = false;
    double cam[5] = {0, 0, 0, 0, 0};  // x y z yaw pitch (degrees)
    std::string connect, name = "Player";  // multiplayer: server host[:port] and player name
    double seconds = 0;                    // quit after this many seconds (0: run until closed)
    bool play = false, lan = false;        // singleplayer through a local official server; open it to friends
    fs::path audioCapture;                 // debug: WAV of the audio output (+ .json log)
    bool audioTest = false;                // scripted gameplay for checking audio
    fs::path script;                       // UI test script (timed input, screenshots)
    fs::path modsDir;                      // mods / packs folder (default %APPDATA%\minecraftCplusplus\mods)
    sky::DebugOptions sky;                 // --time / --weather / --dimension / --biome
    std::string shaders = "high";          // --shaders off|low|medium|high|ultra (built-in shader pipeline)
    float renderScale = 1.0f;              // --render-scale: shader pipeline internal resolution (0.33..1, TAA upscaled)
    // Video settings for this run (override options.txt, not saved; render/upscalers/video_settings.h).
    bool shadersGiven = false;
    std::string upscaler, upscaleQuality, frameGen, lowLatency;  // --upscaler / --upscale-quality / --framegen / --low-latency
    float sharpness = -1;                  // --sharpness 0..1
    int fpsLimit = -1;                     // --fps-limit (0 = unlimited)
    // Front end (ui/title.cpp): the title screen when no world or server is given.
    fs::path savesDir, serversFile, serversDat;  // --saves-dir / --servers-file / --servers-dat (tests: scratch copies)
    std::string menu;                            // --menu worlds|servers: the title screen opens that list
    std::wstring childScript;                    // --child-script: test script for the instance the menu starts
    std::optional<LocalServer::Config::Create> create;  // --create-*: a new world, made by the local server
};

void usage() {
    LOG("usage: minecraftCplusplus [--world <name|path>] [--radius <chunks to load, 0 = whole save>]\n"
        "                          [--detail <full-detail chunks>] [--fog <blocks>] [--jar <client.jar>]\n"
        "                          [--size WxH] [--vsync] [--debug] [--bench] [--fly] [--screenshot <file.png>]\n"
        "                          [--cam x,y,z[,yaw,pitch]] [--readonly] [--selftest [--save]]\n"
        "                          [--no-gen] [--gen-radius <chunks>] [--far <blocks>] [--lod-quality <x, default 2>] [--gentest <region dir> <seed>]\n"
        "                          [--connect <host[:port]> [--name <player>]] [--play [--lan]] [--seconds <n>] [--mods-dir <dir>]\n"
        "                          [--shaders off|low|medium|high|ultra (default high)] [--render-scale <0.33..1>]\n"
        "                          [--upscaler taa|dlss|fsr|xess] [--upscale-quality native|ultraqualityplus|ultraquality|quality|\n"
        "                           balanced|performance|ultraperformance] [--sharpness 0..1]\n"
        "                          [--framegen off|dlss|dlss2x|dlss3x|dlss4x|fsr|xess] [--low-latency off|on|boost] [--fps-limit <n>]\n"
        "                          [--saves-dir <dir>] [--servers-file <servers.json>] [--servers-dat <servers.dat>]\n"
        "                          [--menu worlds|servers] [--child-script <file>] [--create-name <name> --create-seed <seed>\n"
        "                           --create-mode survival|hardcore|creative --create-difficulty <d> --create-cheats 0|1]\n"
        "without --world / --connect / --play: the title screen\n"
        "controls: click to capture the mouse, WASD move, Space jump (double-tap: fly), Shift sneak / descend,\n"
        "          Ctrl sprint, left click break, right click place, middle click pick, 1-9 / wheel hotbar, Esc release");
}

fs::path newestWorld(const fs::path& saves) {
    fs::path best;
    fs::file_time_type bestTime{};
    std::error_code ec;
    for (const auto& d : fs::directory_iterator(saves, ec)) {
        fs::path level = d.path() / "level.dat";
        if (!fs::exists(level, ec)) continue;
        auto t = fs::last_write_time(level, ec);
        if (best.empty() || t > bestTime) best = d.path(), bestTime = t;
    }
    return best;
}

float percentile(std::vector<float> v, float p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * float(v.size() - 1) + 0.5f))];
}

// Scripted gameplay check: walking physics, targeting, breaking and placing with relighting.
void selfTest(Game& game, World& world, BlockRegistry& registry, int frame) {
    Player& p = game.player();
    if (frame == 2) {
        p.flying = false;
        const double start[3] = {p.pos[0], p.pos[1], p.pos[2]};
        game.simulate({}, 40);  // settle onto the ground
        LOG("selftest fall: y %.2f -> %.2f, onGround=%d", start[1], p.pos[1], p.onGround);
        const double s0[3] = {p.pos[0], p.pos[1], p.pos[2]};
        PlayerInput walk;
        walk.forward = 1;
        game.simulate(walk, 20);
        LOG("selftest walk 1 s: moved %.2f blocks (vanilla ~4.3 if unobstructed), onGround=%d",
            std::hypot(p.pos[0] - s0[0], p.pos[2] - s0[2]), p.onGround);
    }
    if (frame == 4) {
        p.pitch = 0.75f;  // look down at the floor a couple of blocks ahead
        game.retarget();
        const RayHit& t = game.target();
        if (!t.hit) {
            LOG("selftest: nothing targeted");
            return;
        }
        LOG("selftest target: %d %d %d face %d = %s", t.x, t.y, t.z, t.face, registry.name(world.block(t.x, t.y, t.z)).c_str());
        const int bx = t.x, by = t.y, bz = t.z;
        Timer tb;
        game.useTarget(false);
        LOG("selftest break: %.2f ms, block now %s", tb.ms(), registry.name(world.block(bx, by, bz)).c_str());
        game.retarget();
        game.setHeld(registry.idFor("minecraft:glowstone"));
        Timer tp;
        const bool placed = game.useTarget(true);
        LOG("selftest place glowstone: %s in %.2f ms", placed ? "ok" : "refused", tp.ms());
        game.retarget();
        game.setHeld(registry.idFor("minecraft:oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=false]"));
        if (game.useTarget(true)) {
            game.retarget();
            const RayHit& s = game.target();
            LOG("selftest place stairs: now targeting %s", registry.name(world.block(s.x, s.y, s.z)).c_str());
        }
    }
    if (frame >= 4) game.buildOverlay();
}

}  // namespace

namespace packs {
int runPackTest(int argc, wchar_t** argv);  // render/packs/pack_test.cpp
}

int wmain(int argc, wchar_t** argv) {
    launch::initProcess(argc, argv);  // log file without a console, handoff from the previous instance
    installCrashHandler();             // stack + minidump in the log on a crash (platform/crash.h)
    if (argc >= 3 && std::wstring_view(argv[1]) == L"--pack-test") return packs::runPackTest(argc, argv);
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--world") opt.world = next();
        else if (a == L"--jar") opt.jar = next();
        else if (a == L"--radius") opt.radius = _wtoi(next().c_str());
        else if (a == L"--detail") opt.detail = std::max(2, _wtoi(next().c_str()));
        else if (a == L"--fog") opt.fog = float(_wtof(next().c_str()));
        else if (a == L"--screenshot") opt.screenshot = next();
        else if (a == L"--size") swscanf(next().c_str(), L"%dx%d", &opt.width, &opt.height);
        else if (a == L"--cam")
            opt.haveCam = swscanf(next().c_str(), L"%lf,%lf,%lf,%lf,%lf", &opt.cam[0], &opt.cam[1], &opt.cam[2],
                                  &opt.cam[3], &opt.cam[4]) >= 3;
        else if (a == L"--bench") opt.bench = true;
        else if (a == L"--selftest") opt.selftest = true;
        else if (a == L"--readonly") opt.readonly = true;
        else if (a == L"--no-gen") opt.generate = false;
        else if (a == L"--shot-frame") opt.shotFrame = _wtoi(next().c_str());
        else if (a == L"--gen-radius") opt.genRadius = _wtoi(next().c_str());
        else if (a == L"--profile") opt.profile = true;
        else if (a == L"--lod-quality") opt.lodQuality = float(_wtof(next().c_str()));
        else if (a == L"--far") opt.farBlocks = float(_wtof(next().c_str()));
        else if (a == L"--gentest") {
            opt.gentestDir = next();
            opt.gentestSeed = _wtoi64(next().c_str());
        } else if (a == L"--gentest-chunks") opt.gentestChunks = _wtoi(next().c_str());
        else if (a == L"--connect") opt.connect = fs::path(next()).string(), opt.generate = false;
        else if (a == L"--name") opt.name = fs::path(next()).string();
        else if (a == L"--play") opt.play = true;
        else if (a == L"--lan") opt.lan = true;
        else if (a == L"--seconds") opt.seconds = _wtof(next().c_str());
        else if (a == L"--audio-capture") opt.audioCapture = next();
        else if (a == L"--audio-test") opt.audioTest = true;
        else if (a == L"--script") opt.script = next();
        else if (a == L"--mods-dir") opt.modsDir = next();
        else if (a == L"--save") opt.forceSave = true;
        else if (a == L"--fly") opt.bench = opt.fly = true;
        else if (a == L"--vsync") opt.vsync = true;
        else if (a == L"--debug") opt.debug = true;
        else if (sky::isOption(a)) sky::parseOption(opt.sky, a, next());
        else if (ShaderPreset p; a == L"--shaders" && Renderer::parseShaderPreset(fs::path(next()).string(), p))
            opt.shaders = Renderer::shaderPresetName(p), opt.shadersGiven = true;
        else if (a == L"--render-scale") opt.renderScale = float(_wtof(next().c_str()));
        else if (a == L"--upscaler") opt.upscaler = fs::path(next()).string();
        else if (a == L"--upscale-quality") opt.upscaleQuality = fs::path(next()).string();
        else if (a == L"--framegen") opt.frameGen = fs::path(next()).string();
        else if (a == L"--low-latency") opt.lowLatency = fs::path(next()).string();
        else if (a == L"--sharpness") opt.sharpness = float(_wtof(next().c_str()));
        else if (a == L"--fps-limit") opt.fpsLimit = _wtoi(next().c_str());
        else if (a == L"--saves-dir") opt.savesDir = next();
        else if (a == L"--servers-file") opt.serversFile = next();
        else if (a == L"--servers-dat") opt.serversDat = next();
        else if (a == L"--menu") opt.menu = fs::path(next()).string();
        else if (a == L"--child-script") opt.childScript = next();
        else if (a == L"--handoff" || a == L"--window") next();  // platform/launch.h
        else if (a == L"--shaderpack" || a == L"--pack-option") packs::commandLine(a, next());  // render/packs/pack_api.h
        else if (a.starts_with(L"--create-")) {
            if (!opt.create) opt.create.emplace();
            const std::wstring v = next();
            const std::u8string u = fs::path(v).u8string();
            const std::string s(u.begin(), u.end());
            if (a == L"--create-name") opt.create->levelName = s;
            else if (a == L"--create-seed") opt.create->seed = s;
            else if (a == L"--create-mode") opt.create->gameMode = s;
            else if (a == L"--create-difficulty") opt.create->difficulty = s;
            else if (a == L"--create-cheats") opt.create->cheats = v == L"1";
            else if (a == L"--create-structures") opt.create->structures = v != L"0";
        } else {
            usage();
            return 1;
        }
    }

    if (opt.create && opt.create->gameMode == "hardcore")  // hardcore: survival, hard, one life
        opt.create->gameMode = "survival", opt.create->hardcore = true, opt.create->difficulty = "hard";
    // No world, server or tool given: the title screen, over a live world panorama (ui/title.cpp).
    const bool menu = opt.world.empty() && opt.connect.empty() && !opt.play && opt.gentestDir.empty() && !opt.bench &&
                      !opt.selftest && !opt.audioTest;
    if (menu) {
        opt.detail = std::min(opt.detail, 10);        // a vista: little full detail, far terrain beyond
        if (opt.genRadius < 0) opt.genRadius = 10;
        if (opt.farBlocks < 0) opt.farBlocks = opt.fog = std::min(opt.fog, 8192.0f);
        if (opt.sky.time < 0) opt.sky.time = 1500;  // morning light
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // The frame thread above the generation / meshing workers and the local server's JVM, which together keep every
    // core busy while travelling: at normal priority it lost whole scheduler quanta (20-50 ms frames).
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    Jobs::init();
    struct JobsGuard {
        ~JobsGuard() { Jobs::shutdown(); }
    } jobsGuard;
    Timer total;

    const wchar_t* appdata = _wgetenv(L"APPDATA");
    fs::path mc = fs::path(appdata ? appdata : L".") / ".minecraft";
    std::string version = "custom";
    fs::path jar = opt.jar.empty() ? findMinecraftJar(mc, &version) : opt.jar;
    if (jar.empty()) {
        LOG("error: no Minecraft client jar found under %s (install the game or pass --jar)", mc.string().c_str());
        return 1;
    }
    // Only for owners of Minecraft: Java Edition (see platform/ownership.h).
    const Ownership ownership = checkOwnership(mc);
    if (!ownership.owned) {
        LOG("error: could not verify that you own Minecraft: Java Edition: %s.\n"
            "       Sign in to the official Minecraft Launcher with your account, then start this again.",
            ownership.reason.c_str());
        return 1;
    }
    if (opt.gentestDir.empty() && !opt.audioTest) upscale::preinit();  // Streamline's slInit, beside the loading
    const fs::path saves = opt.savesDir.empty() ? mc / "saves" : opt.savesDir;
    // The title screen's panorama is a world of its own, generated once into a cache (seed and spot picked at
    // random, ui::writeTitleWorld) and loaded from it afterwards: detailed from the first frame.
    const wchar_t* localApp = _wgetenv(L"LOCALAPPDATA");
    const fs::path titleDir = fs::path(localApp ? localApp : L".") / "minecraftCplusplus" / "title" / version;
    std::error_code titleEc;
    if (menu) fs::create_directories(titleDir, titleEc);
    i64 cachedSeed = 0;
    const bool titleCached = menu && World::readSeed(titleDir, cachedSeed);
    const bool titleChunks =
        titleCached &&
        fs::directory_iterator(titleDir / "dimensions" / "minecraft" / "overworld" / "region", titleEc) != fs::directory_iterator();
    fs::path worldDir = menu                                    ? titleDir
                        : opt.world.empty()                     ? newestWorld(saves)
                        : fs::exists(opt.world / "level.dat") ? opt.world
                                                              : saves / opt.world;
    if (opt.create && fs::exists(worldDir / "level.dat")) opt.create.reset();  // made already (a restart)
    if (opt.connect.empty() && !menu && !(opt.play && opt.create) &&
        (worldDir.empty() || !fs::exists(worldDir / "level.dat"))) {
        LOG("error: world not found");
        return 1;
    }
    LOG("cpu threads %u | jar %s | world %s | load radius %d | detail radius %d chunks", Jobs::threadCount(),
        version.c_str(), worldDir.filename().string().c_str(), opt.radius, opt.detail);

    // Mods and packs (mods/mod_manager.h): resource and data packs layered over the game's files for every loader
    // below, Fabric mods for the local server, native mods.
    mods::Manager::get().init({mc, version, jar, opt.connect.empty() ? worldDir : fs::path(), opt.modsDir,
                               opt.play && opt.connect.empty()});

    // --- singleplayer (--play): the official server runs the game's rules on this world in the background
    // while the save is shown right away; the game switches to it once it is up ---
    std::unique_ptr<LocalServer> localServer;
    net::LanHost lanHost;  // Open to LAN (pause menu) / --lan: announcements (and a forwarded port)
    if (opt.play && opt.connect.empty()) {
        if (opt.name == "Player") opt.name = ownership.playerName;
        localServer = std::make_unique<LocalServer>();
        LocalServer::Config sc{mc, version, worldDir, opt.name, 0, opt.lan, std::min(opt.detail, 32)};
        sc.create = opt.create;
        sc.ownerUuid = ownership.playerId;
        if (!localServer->start(sc)) return 1;
        if (opt.lan) {  // already listening on all interfaces: announce it on the LAN like the game
            std::string error;
            lanHost.start(opt.name + " - " + worldDir.filename().string(), localServer->port(), localServer->port(), false, error);
        }
        if (localServer->upgrading() || localServer->creating()) {  // the server writes the save first: read it after
            LOG(localServer->creating() ? "server  creating the world first..." : "server  upgrading the world to %s first...",
                version.c_str());
            while (!localServer->ready() && localServer->running()) Sleep(100);
            if (!localServer->ready()) return 1;
        }
    }

    Timer t;
    GameReports reports;
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    const bool haveReports = reports.load(mc, version, fs::path(local ? local : L".") / "minecraftCplusplus" / "reports");
    if (!haveReports) LOG("warning: block definitions unavailable; generated terrain may miss some block states");
    audio::AudioSystem audio;  // loads in the background
    audio.init({mc, version, fs::path(local ? local : L".") / "minecraftCplusplus", haveReports ? &reports : nullptr,
                opt.audioCapture});
    if (!opt.audioCapture.empty()) audio.waitReady(60.0);
    BlockShapes blockShapes;  // exact selection / collision shapes from the game's code (cached), parsed meanwhile
    auto shapesTask = std::async(std::launch::async, [&] {
        return blockShapes.load(mc, version, fs::path(local ? local : L".") / "minecraftCplusplus" / "shapes");
    });
    BlockRegistry registry;
    if (!registry.init(jar, haveReports ? &reports : nullptr)) return 1;
    if (shapesTask.get()) registry.setShapes(&blockShapes);  // before the world creates its block states
    // Independent loaders run alongside the world load below (each on its own thread).
    BiomeColors biomeColors;  // grass / foliage / water tints from the game's biome data
    auto biomeTask = std::async(std::launch::async, [&] { return biomeColors.init(jar); });
    ent::EntityAssets entityAssets;
    auto entityTask = std::async(std::launch::async, [&] {
        return !menu && haveReports && entityAssets.load(mc, version, jar, fs::path(local ? local : L".") / "minecraftCplusplus" / "models",
                                                reports);
    });
    ui::Ui ui;  // before the clients (their handlers post to it) and the renderer (its pass)
    auto uiTask = std::async(std::launch::async, [&] { return ui.load(jar, mc, reports); });
    LOG("assets  %8.1f ms  %u textures from the installed game", t.ms(), registry.textureCount());
    if (!opt.gentestDir.empty()) return runGenTest(registry, jar, opt.gentestDir, opt.gentestSeed, opt.gentestChunks);

    PlayerStart player = World::readPlayer(worldDir);
    if (opt.haveCam) {
        player.x = opt.cam[0], player.y = opt.cam[1] - 1.62, player.z = opt.cam[2];
        player.yaw = float(opt.cam[3]), player.pitch = float(opt.cam[4]);
    }
    t = Timer();
    World world;
    LoadStats ls;
    // The save loads while the other loaders finish (they are only needed once a client or the window exists).
    std::future<bool> worldTask;
    if (menu && !titleChunks) world.initEmpty(-4, 19);  // the title screen's world is all generated (overworld height)
    if (opt.connect.empty() && (!menu || titleChunks))
        worldTask = std::async(std::launch::async, [&] {
            return world.load(worldDir, registry, int(std::floor(player.x)) >> 4, int(std::floor(player.z)) >> 4,
                              opt.radius, ls);
        });
    const bool haveBiomeColors = biomeTask.get();
    if (!haveBiomeColors) LOG("warning: biome colours unavailable; using the default tints");
    // Entities: models, textures and type data from the installed game; state from the server.
    const bool haveEntities = entityTask.get();
    ent::EntityWorld entities(entityAssets, world, registry, reports);
    ent::EntityRenderer entityRenderer(entities);  // declared before the renderer: destroyed after it
    // Hands, held / worn items, item frames, paintings, name tags, particles (same lifetime rule).
    ent::Visuals visuals(entities, entityRenderer);
    if (haveEntities)
        visuals.load(jar, mc, reports,
                     fs::path(local ? local : L".") / "minecraftCplusplus" / "reports" / version / "reports" / "minecraft" /
                         "components" / "item");
    // Chests, sign text, banners, heads, ...: the game's block entity renderers (same lifetime rule).
    be::BlockEntityRenderer blockEntityRenderer(world, registry, reports, entityAssets, entityRenderer);
    if (haveEntities) blockEntityRenderer.load(jar, mc);
    sky::Sky sky;                                  // same
    if (!uiTask.get()) return 1;
    // --- multiplayer (--connect): log in; the world then comes from the server instead of a save ---
    std::unique_ptr<net::Client> netClient;
    std::unique_ptr<net::Session> session;
    if (!opt.connect.empty()) {
        const net::ServerAddress address = net::resolveAddress(net::parseAddress(opt.connect));  // + SRV, like the game
        const std::string host = address.host;
        const u16 port = address.port;
        netClient = std::make_unique<net::Client>(reports, registry);
        audio.attachNet(*netClient);
        if (haveEntities) entities.attach(*netClient), blockEntityRenderer.attach(*netClient), visuals.attach(*netClient);
        sky.attach(*netClient, haveReports ? &reports : nullptr);
        packs::attachNetwork(*netClient);  // shader pack uniforms (isElytraFlying, heavyFog)
        ui.registerHandlers(*netClient);
        if (!netClient->start({host, port, opt.name, jar})) return 1;
        session = std::make_unique<net::Session>(*netClient, world, registry, reports);
        net::Dimension dim;
        if (!session->join(30.0, player, dim)) return 1;
        world.initEmpty(std::min(-4, dim.minY >> 4), std::max(19, (dim.minY + dim.height - 1) >> 4));
        LOG("server  %8.1f ms  joined %s as %s at %.1f %.1f %.1f", t.ms(), opt.connect.c_str(), opt.name.c_str(),
            player.x, player.y, player.z);
    }
    // --- end multiplayer ---
    if (worldTask.valid() && !worldTask.get()) return 1;
    if (!session) LOG("world   %8.1f ms  %zu regions, %zu chunks, %zu sections, %.0f MB read, %zu block states%s", t.ms(),
        ls.regions, ls.chunks, ls.sections, double(ls.compressedBytes) / (1 << 20), registry.blockCount(),
        ls.unsupportedChunks ? " (some LZ4 chunks skipped)" : "");

    // Benchmarks and the self-test never write to a world unless --save is given.
    WorldSaver saver(world, registry, worldDir);
    const bool wantSave = !session && !localServer && (opt.forceSave || (!opt.readonly && !opt.bench && !opt.selftest));
    const bool canSave = wantSave && saver.lock();
    if (wantSave && !canSave) LOG("note: the world is open in another program (session.lock); running read-only");

    Window window;
    // An instance started by another (platform/launch.h) opens where that window was, invisible until its first frames.
    if (!window.create(L"minecraftCplusplus", opt.width, opt.height, launch::placement(), launch::handoffChild())) {
        LOG("error: cannot create window");
        return 1;
    }
    Renderer renderer;
    if (!renderer.init(window.hwnd(), window.width(), window.height(), opt.debug, opt.vsync)) return 1;
    if (!renderer.uploadTextures(registry.texturePixels(), registry.textureCount())) return 1;
    renderer.setTextureNames(registry.textureNames());
    packs::applyCommandLine(renderer, &registry);  // shader packs: block ids, --shaderpack (render/packs/pack_api.h)
    if (ShaderPreset p; Renderer::parseShaderPreset(opt.shaders, p)) renderer.setShaderPreset(p);
    if (opt.renderScale != 1.0f && renderer.shaderPreset() != ShaderPreset::Off) {
        ShaderSettings s = renderer.shaderSettings();
        s.renderScale = opt.renderScale;
        renderer.setShaderSettings(s);
    }
    if (haveEntities && !renderer.addPass(&entityRenderer, PassStage::World)) LOG("warning: entity rendering unavailable");
    if (haveEntities && !renderer.addPass(&blockEntityRenderer, PassStage::World))
        LOG("warning: block entity rendering unavailable");
    if (haveEntities && !visuals.addPasses(renderer)) LOG("warning: hands / items / particles unavailable");
    if (sky.init(renderer, world, registry, jar, opt.sky) && !session) sky.loadSave(worldDir);

    CameraState cam;
    cam.x = player.x, cam.y = player.y + 1.62, cam.z = player.z;
    cam.yaw = player.yaw * kPi / 180.0f;
    cam.pitch = player.pitch * kPi / 180.0f;

    t = Timer();
    Terrain terrain(world, registry, renderer, float(opt.detail) * 16.0f);
    if (opt.generate) terrain.reserveFar(opt.farBlocks >= 0 ? opt.farBlocks : opt.fog);
    if (haveBiomeColors) terrain.setBiomeColors(&biomeColors);
    terrain.setLodQuality(opt.lodQuality);
    if (!terrain.buildCoarse()) return 1;
    LOG("lod     %8.1f ms  %zu coarse nodes (levels 1-%d) for the whole world", t.ms(), terrain.stats().coarseNodes,
        Terrain::kMaxLevel);
    t = Timer();
    terrain.update(cam.x, cam.y, cam.z, true);
    LOG("detail  %8.1f ms  %zu full-detail sections around the camera", t.ms(), terrain.stats().detailResident);
    LOG("total   %8.1f ms  launch to first frame | %.0f / %.0f MB geometry arena on %s", total.ms(),
        double(renderer.arenaBytesUsed()) / (1 << 20), double(renderer.arenaBytesTotal()) / (1 << 20),
        renderer.adapterName().c_str());

    // Chunks the save does not have are generated from the world seed with the game's own rules.
    std::unique_ptr<wg::DataPack> gamedata;
    std::unique_ptr<wg::ChunkGenerator> generator;
    std::unique_ptr<wg::FeatureSystem> features;
    std::unique_ptr<wg::StructureSystem> structures;  // villages, mineshafts... placed with the features
    std::unique_ptr<wg::GenService> genService;
    struct FarStop {  // destroyed before the generator the far threads call into
        Terrain& terrain;
        ~FarStop() { terrain.stopFar(); }
    } farStop{terrain};
    i64 seed = 0;
    std::mt19937_64 titleRandom(std::random_device{}());
    if (const char* s = std::getenv("MCPP_TITLE_SEED")) titleRandom.seed(std::strtoull(s, nullptr, 10));  // tests
    if (opt.generate && (menu && !titleCached ? (seed = i64(titleRandom()), true) : World::readSeed(worldDir, seed))) {
        t = Timer();
        gamedata = std::make_unique<wg::DataPack>();
        if (gamedata->open(jar)) generator = std::make_unique<wg::ChunkGenerator>(*gamedata, registry, seed);
        if (generator && generator->ok()) {
            // Saved chunks next to generated ones are read-only neighbours for decoration.
            auto exists = [&world](int cx, int cz) {
                std::shared_lock lock(world.mutex());
                return world.hasChunk(cx, cz);
            };
            auto load = [&world, &generator](int cx, int cz, wg::GenChunk& out) {
                std::shared_lock lock(world.mutex());
                out.init(cx, cz, generator->minY(), generator->height());
                u16 ids[4096];
                for (int sy = out.minY >> 4; sy < (out.minY + out.height) >> 4; ++sy) {
                    const World::Region* r = nullptr;
                    const World::Section* s = world.section(cx, sy, cz, &r);
                    if (!s) continue;
                    World::decode(*r, *s, ids);
                    for (int i = 0; i < 4096; ++i) out.set(i & 15, sy * 16 + (i >> 8), (i >> 4) & 15, ids[i]);
                }
            };
            // In singleplayer too: the server sends the chunks it simulates, these fill the view beyond them
            // (and are replaced by the server's when it sends them).
            Timer ft;
            features = std::make_unique<wg::FeatureSystem>(generator->noise(), registry, generator->biomes(),
                                                           generator->possibleBiomes());
            LOG("features    %6.1f ms", ft.ms());
            structures = std::make_unique<wg::StructureSystem>(*generator, features.get(), registry, seed);
            if (structures->ok()) features->setStructures(structures.get());
            const char* genThreads = std::getenv("MCPP_GEN_THREADS");  // profiling override
            genService = std::make_unique<wg::GenService>(*generator, features.get(), exists, load,
                                                          genThreads ? unsigned(std::atoi(genThreads))
                                                                     : std::max(2u, Jobs::threadCount() / 2));
            // Beyond the generated chunks: terrain approximated per region at LOD resolution.
            terrain.enableFar(
                [&generator](int rx, int rz, int level, const FarColumns* coarser, FarColumns& out) {
                    thread_local wg::DensityContext ctx;
                    thread_local std::vector<u16> genBiomes;
                    thread_local std::vector<u8> toWorld;  // generator biome id -> World biome id
                    generator->generateCoarse(rx, rz, level, out, ctx, coarser, nullptr, &genBiomes);
                    std::vector<u8>& biomes = out.biomes;
                    biomes.resize(genBiomes.size());
                    for (size_t i = 0; i < genBiomes.size(); ++i) {
                        const u16 b = genBiomes[i];
                        if (b >= toWorld.size()) toWorld.resize(size_t(b) + 1, 0);
                        if (!toWorld[b]) toWorld[b] = u8(World::biomeId(generator->biomes().name(b)) + 1);
                        biomes[i] = u8(toWorld[b] - 1);
                    }
                },
                generator->minY(), std::max(2u, Jobs::threadCount() / 4));
        }
        LOG("worldgen %7.1f ms  seed %lld, %s", t.ms(), (long long)seed,
            genService                       ? "generating missing chunks in the background"
            : generator && generator->ok() ? "far terrain only"
                                           : "unavailable");
    }
    // Real chunks (with trees) reach a ring past full detail, where the coarse levels are built from them.
    const int genRadius = opt.genRadius >= 0 ? opt.genRadius : opt.detail + 16;
    wg::DensityContext saveBiomeContext;  // biome lookups for saving generated chunks (main thread)
    if (generator && generator->ok())
        saver.enableNewChunks(
            [&](int qx, int qy, int qz) {
                return generator->biomes().name(generator->noiseBiome(saveBiomeContext, qx, qy, qz));
            },
            gameDataVersion(jar));
    double genScan = 0;
    std::deque<std::unique_ptr<wg::GenChunk>> genBacklog;  // generated chunks waiting for the per-frame budget
    Lighting genLighting(world, registry);

    if (menu && !titleCached && generator && generator->ok()) {
        // Title screen panorama: of a few spots near spawn the most varied land, seen from above it.
        Timer vt;
        wg::DensityContext ctx;
        std::vector<u16> column;
        const u16 stone = registry.idFor("minecraft:stone");
        auto ground = [&](int x, int z, bool& water) {
            generator->baseColumn(x, z, column, ctx);
            int top = -1, solid = -1;
            for (int i = int(column.size()) - 1; i >= 0 && solid < 0; --i) {
                if (column[size_t(i)] != wg::GenChunk::kAir && top < 0) top = i;
                if (column[size_t(i)] == stone) solid = i;
            }
            water = top > solid;
            return generator->minY() + std::max(top, 0);
        };
        double bestScore = -1e9;
        int bx = 0, bz = 0, by = 100;
        std::uniform_real_distribution<double> angle(0, 2 * 3.14159265), dist(0, 1200);
        for (int cand = 0; cand < 10; ++cand) {
            const double a = angle(titleRandom), d = dist(titleRandom);
            const int cx = int(std::cos(a) * d), cz = int(std::sin(a) * d);
            int lo = 1 << 20, hi = -(1 << 20), wet = 0, center = 0;
            for (int gz = -2; gz <= 2; ++gz)
                for (int gx = -2; gx <= 2; ++gx) {
                    bool water = false;
                    const int h = ground(cx + gx * 48, cz + gz * 48, water);
                    lo = std::min(lo, h), hi = std::max(hi, h), wet += water;
                    if (std::abs(gx) <= 1 && std::abs(gz) <= 1) center = std::max(center, h);
                }
            const double score = double(hi - lo) - 6.0 * wet + 0.2 * double(center - generator->seaLevel());
            if (score > bestScore) bestScore = score, bx = cx, bz = cz, by = std::max(center, generator->seaLevel());
        }
        player.x = bx + 0.5, player.z = bz + 0.5, player.y = by + 22;
        player.yaw = float(angle(titleRandom) * 180.0 / 3.14159265), player.pitch = 9;
        cam.x = player.x, cam.y = player.y + 1.62, cam.z = player.z;
        cam.yaw = player.yaw * kPi / 180.0f, cam.pitch = player.pitch * kPi / 180.0f;
        LOG("title   %8.1f ms  panorama at %d %d %d (seed %lld)", vt.ms(), bx, by, bz, (long long)seed);
        const double pos[3] = {player.x, player.y, player.z};
        if (!ui::writeTitleWorld(titleDir, seed, gameDataVersion(jar), pos, player.yaw, player.pitch))
            LOG("warning: could not write the title screen world to %s", titleDir.string().c_str());
    }
    Game game(world, registry, terrain, renderer, player);
    if (session) session->attach(game, terrain);
    {  // Front end: title screen, lists, switching instances (ui/title.cpp)
        ui::FrontEnd fe;
        const wchar_t* localDir = _wgetenv(L"LOCALAPPDATA");
        fe.menu = menu;
        fe.minecraftDir = mc, fe.savesDir = saves, fe.jar = jar, fe.version = version;
        fe.serversFile = !opt.serversFile.empty() ? opt.serversFile
                                                   : fs::path(localDir ? localDir : L".") / "minecraftCplusplus" / "servers.json";
        fe.serversDat = !opt.serversDat.empty() ? opt.serversDat : mc / "servers.dat";
        fe.playerName = opt.name != "Player" ? opt.name : ownership.playerName;
        fe.openScreen = opt.menu;
        fe.childScript = opt.childScript;
        fe.worldDir = worldDir;
        if (opt.create) fe.worldName = opt.create->levelName;
        fe.singleplayer = localServer != nullptr;
        fe.lanPort = opt.lan && localServer ? localServer->port() : 0;
        fe.openToLan = [&](const std::string& motd, const std::string& mode, bool cheats, u16 port, std::string& error) {
            if (!localServer) return false;
            // The server listens on loopback only (unless --lan): the LAN port is forwarded to it.
            if (!lanHost.start(motd, port, localServer->port(), !opt.lan, error)) return false;
            localServer->command("defaultgamemode " + mode);
            if (cheats) localServer->command("op @a");  // commands for everyone here (and the host)
            LOG("lan: open to LAN on port %u as \"%s\"", port, motd.c_str());
            return true;
        };
        ui.setFrontEnd(fe);
    }
    if (!ui.init({&registry, &renderer, &window, &game, &world, &terrain, opt.detail})) return 1;
    if (session) ui.attachSession(netClient.get(), session.get());
    if (!opt.script.empty()) ui.loadScript(opt.script);
    if (!packs::commandLinePack() && !ui.settings().shaderPack.empty())  // the saved shader pack (render/packs)
        packs::selectPack(renderer, ui.settings().shaderPack);
    {  // Video settings: options.txt, then this run's command line (not saved). Applies shaders, upscaler, frame gen.
        VideoSettings v = ui.settings().video;
        if (!ui.settings().videoSaved || opt.shadersGiven) {
            ShaderPreset p = ShaderPreset::High;
            Renderer::parseShaderPreset(opt.shaders, p);
            v.shaders = u8(p);
        }
        if (!opt.upscaler.empty() && !VideoSettings::parseUpscaler(opt.upscaler, v.upscaler))
            LOG("warning: unknown --upscaler %s", opt.upscaler.c_str());
        if (!opt.upscaleQuality.empty() && !VideoSettings::parseQuality(opt.upscaleQuality, v.quality))
            LOG("warning: unknown --upscale-quality %s", opt.upscaleQuality.c_str());
        if (!opt.frameGen.empty() && !v.parseFrameGen(opt.frameGen)) LOG("warning: unknown --framegen %s", opt.frameGen.c_str());
        if (!opt.lowLatency.empty() && !VideoSettings::parseLowLatency(opt.lowLatency, v.lowLatency))
            LOG("warning: unknown --low-latency %s", opt.lowLatency.c_str());
        if (opt.sharpness >= 0) v.sharpness = opt.sharpness;
        if (opt.fpsLimit >= 0) v.fpsLimit = u32(opt.fpsLimit);
        if (menu && (v.fpsLimit == 0 || v.fpsLimit > 60)) v.fpsLimit = 60;  // menus need no more
        if (opt.vsync) v.vsync = true;
        if (opt.renderScale != 1.0f) v.renderScaleOverride = opt.renderScale;
        renderer.upscalers().apply(v);
    }
    std::string lastDimension;  // temporal history (TAA / upscalers / frame generation) resets on dimension change
    audio.attachGame(game, world, registry);
    if (haveEntities) visuals.setPlayer(game, ui, opt.name, [&] { return session ? session->gameMode() : 1; });
    game.onEntityClick = [&](bool attack, bool use, double blockDistance) {  // attack / use entities first
        if (!session || !haveEntities) return false;
        const CameraState c = game.camera();
        const double eye[3] = {c.x, c.y, c.z}, cp = std::cos(c.pitch);
        const double dir[3] = {-std::sin(c.yaw) * cp, -std::sin(c.pitch), std::cos(c.yaw) * cp};
        return entities.click(eye, dir, attack, use, blockDistance, 5.0);
    };
    // Music, ambience, sky / fog colours and rain or snow follow the surface biome the world stores (from the
    // save, the server or our generator); where it has none, the generator's biome at that position.
    wg::DensityContext audioBiomeContext, skyBiomeContext;
    auto biomeAt = [&](wg::DensityContext& ctx, int x, int y, int z) -> std::string_view {
        u8 b;
        {
            std::shared_lock lock(world.mutex());
            b = world.biomeAt(x, z);
        }
        if (b != World::kUnknownBiome) return World::biomeNameView(b);
        if (generator && generator->ok() && !session) return generator->biomes().name(generator->noiseBiome(ctx, x >> 2, y >> 2, z >> 2));
        return "minecraft:plains";
    };
    audio.setBiomeSource([&](int x, int y, int z) { return biomeAt(audioBiomeContext, x, y, z); });
    sky.setBiomeLookup([&](int qx, int qy, int qz) { return std::string(biomeAt(skyBiomeContext, qx * 4, qy * 4, qz * 4)); });
    wg::DensityContext uiBiomeContext;  // F3
    ui.setBiomeLookup([&](int x, int y, int z) { return std::string(biomeAt(uiBiomeContext, x, y, z)); });
    if (haveEntities)  // entity-bound server sounds follow their entity (or the local player)
        audio.setEntityPositionSource([&](int id, double pos[3]) {
            if (session && id == session->entityId()) {
                std::copy(game.player().pos, game.player().pos + 3, pos);
                return true;
            }
            const auto it = entities.entities().find(id);
            if (it == entities.entities().end()) return false;
            std::copy(it->second.pos, it->second.pos + 3, pos);
            return true;
        });
    audio::SelfTest audioTest;
    const float startYaw = cam.yaw;
    constexpr int kBenchFrames = 720;
    std::vector<float> cpuMs, gpuMs, draws, nodes;
    double last = nowSeconds(), titleTime = last, saveTime = last;
    auto saveNow = [&](const char* why) {
        if (!canSave || world.dirtyChunks().empty()) return;
        const Player& p = game.player();
        const SavedPlayer sp{p.pos[0], p.pos[1], p.pos[2], float(p.yaw * 180.0 / kPi), float(p.pitch * 180.0 / kPi)};
        const SaveStats st = saver.save(&sp);
        LOG("saved (%s): %zu chunks in %zu region files, %.1f ms%s", why, st.chunks, st.regions, st.ms,
            st.ok ? "" : " (with errors)");
    };
    bool escWas = false;
    std::future<bool> joining;  // singleplayer login on a worker (see the loop)
    std::unique_ptr<net::Session> joiningSession;
    PlayerStart joinStart;
    net::Dimension joinDim;
    Timer joinTimer;
    bool joinFailed = false;
    const double loopStart = nowSeconds();
    Timer frameEnd;  // window.pump() + anything between frames
    for (int frame = 0; window.pump(); ++frame) {
        const double pumpMs = frameEnd.ms();
        u32 w, h;
        if (window.takeResize(w, h)) renderer.resize(w, h);
        const Timer frameTop;
        renderer.beginFrame();  // low-latency sleep (Reflex / XeLL / Anti-Lag 2), frame limiter, simulation markers
        const double beginMs = frameTop.ms();
        double netMs = 0;
        double now = nowSeconds();
        float dt = float(now - last);
        last = now;
        // Singleplayer: once the local server is up, connect and log in on a worker (protocol tables, language,
        // block ids, then the server's login: ~1 s that used to freeze the already visible world), then attach here.
        if (localServer && !session && !joining.valid() && !joinFailed && localServer->ready()) {
            joinTimer = Timer();
            netClient = std::make_unique<net::Client>(reports, registry);
            audio.attachNet(*netClient);
            if (haveEntities) entities.attach(*netClient), blockEntityRenderer.attach(*netClient), visuals.attach(*netClient);
            sky.attach(*netClient, haveReports ? &reports : nullptr);
            packs::attachNetwork(*netClient);
            ui.registerHandlers(*netClient);
            const net::Client::Config config{"127.0.0.1", localServer->port(), opt.name, jar, localServer->stateTable()};
            joining = std::async(std::launch::async, [&, config] {
                if (!netClient->start(config)) return false;
                joiningSession = std::make_unique<net::Session>(*netClient, world, registry, reports);
                return joiningSession->join(30.0, joinStart, joinDim);
            });
        }
        if (joining.valid() && joining.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            if (joining.get()) {
                session = std::move(joiningSession);
                Player& p = game.player();
                p.pos[0] = joinStart.x, p.pos[1] = joinStart.y, p.pos[2] = joinStart.z;
                p.yaw = joinStart.yaw * kPi / 180.0f, p.pitch = joinStart.pitch * kPi / 180.0f;
                session->setKeepForgotten(true);  // unloaded server chunks stay visible
                session->attach(game, terrain);
                ui.attachSession(netClient.get(), session.get());
                LOG("server  %8.1f ms  playing through the local server as %s", joinTimer.ms(), opt.name.c_str());
            } else {
                joinFailed = true;
                joiningSession.reset();
                netClient.reset();
                localServer->stop();
                localServer.reset();
                LOG("error: could not join the local server; continuing without it");
            }
        }
        if (session) {
            const Timer t;
            session->frame(dt);  // multiplayer: apply what the server sent, send movement
            netMs = t.ms();
        }
        if (session && session->dimension().name != lastDimension) {
            lastDimension = session->dimension().name;
            renderer.resetTemporal();
        }
        if (session) terrain.setFarActive(session->dimension().name == "minecraft:overworld");
        if (haveEntities) entities.frame(dt);
        ui.beginFrame(dt);  // UI input first: an open screen takes the game's input

        if (opt.bench && opt.fly) {
            cam.yaw = startYaw;  // fly straight ahead at 100 m/s to exercise streaming
            cam.pitch = 0.15f;
            cam.x += -std::sin(cam.yaw) * 100.0f / 60.0f;
            cam.z += std::cos(cam.yaw) * 100.0f / 60.0f;
        } else if (opt.bench) {
            cam.yaw = startYaw + 2.0f * kPi * float(frame) / kBenchFrames;
            cam.pitch = 0.15f;
        } else if (opt.audioTest) {
            audioTest.frame(game, world, registry, audio, dt);
            cam = game.camera();
        } else if (opt.selftest) {
            const int base = opt.shotFrame > 30 ? opt.shotFrame - 20 : 0;  // let generation settle first
            selfTest(game, world, registry, frame - base);
            cam = game.camera();
        } else if (menu) {
            cam.yaw += dt * 0.035f;  // the title panorama turns slowly
        } else {
            game.frame(window, dt);
            cam = game.viewCamera();  // F5: the detached camera
        }
        audio.update(dt, cam, {session ? session->gameMode() : 1,
                               session ? std::string_view(session->dimension().name) : "minecraft:overworld"});

        const double preMs = frameTop.ms();
        Timer phase;
        double genMs = 0, terrainMs = 0;
        if (genService && (!session || session->dimension().name == "minecraft:overworld") && !(menu && ui.switching())) {
            if (now - genScan > 0.25) {  // ask for missing chunks around the camera, nearest first
                genScan = now;
                const int ccx = int(std::floor(cam.x)) >> 4, ccz = int(std::floor(cam.z)) >> 4;
                std::vector<std::pair<int, int>> wanted;
                for (int dz = -genRadius; dz <= genRadius; ++dz)
                    for (int dx = -genRadius; dx <= genRadius; ++dx)
                        if (dx * dx + dz * dz <= genRadius * genRadius && !world.hasChunk(ccx + dx, ccz + dz))
                            wanted.push_back({ccx + dx, ccz + dz});
                std::sort(wanted.begin(), wanted.end(), [&](const auto& a, const auto& b) {
                    return (a.first - ccx) * (a.first - ccx) + (a.second - ccz) * (a.second - ccz) <
                           (b.first - ccx) * (b.first - ccx) + (b.second - ccz) * (b.second - ccz);
                });
                const Timer tw;
                genService->setWanted(wanted);
                if (opt.profile && tw.ms() > 5.0) LOG("gen: setWanted %.1f ms (%zu wanted)", tw.ms(), wanted.size());
                if (opt.profile && int(now) != int(now - 0.25))
                    LOG("gen: %zu wanted, %zu waiting to apply, %.1f ms per chunk | %s", wanted.size(), genBacklog.size(),
                        genService->averageMs(), genService->debugCounts().c_str());
            }
            // Apply generated chunks within a time budget (each costs ~2 ms of insert + lighting).
            if (genBacklog.size() < 16) {
                const Timer tt;
                for (auto& c : genService->take(32)) genBacklog.push_back(std::move(c));
                if (opt.profile && tt.ms() > 5.0) LOG("gen: take %.1f ms", tt.ms());
            }
            Timer budget;
            while (!genBacklog.empty() && budget.ms() < 3.0) {
                std::unique_ptr<wg::GenChunk> c = std::move(genBacklog.front());
                genBacklog.pop_front();
                std::unordered_set<u64> dirty;
                Timer ct;
                double insertMs = 0, lightMs = 0, waitMs = 0;
                {
                    std::unique_lock lock(world.mutex());
                    waitMs = ct.ms();
                    if (!world.insertChunk(c->cx, c->cz, c->minY, c->height, c->blocks.data())) continue;
                    if (menu) world.markChunkDirty(c->cx, c->cz);  // the title screen's world keeps what it generates
                    insertMs = ct.ms();
                    u8 surface[16];  // surface biome of each 4x4 column, for biome colours
                    for (int q = 0; q < 16; ++q) {
                        const int h = c->worldSurfaceWG[((q >> 2) * 4 + 1) * 16 + (q & 3) * 4 + 1];
                        const u16 b = c->biome(q & 3, (std::max(h, c->minY + 1) - 1 - c->minY) >> 2, q >> 2);
                        surface[q] = World::biomeId(generator->biomes().name(b));
                    }
                    world.setChunkBiomes(c->cx, c->cz, surface);
                    world.setTrackChanges(false);
                    genLighting.initChunk(c->cx, c->cz, dirty);
                    world.setTrackChanges(true);
                    lightMs = ct.ms() - insertMs;
                }
                terrain.addChunk(c->cx, c->cz, std::vector<u64>(dirty.begin(), dirty.end()));
                if (opt.profile && ct.ms() > 1.0)
                    LOG("chunk %d %d: %.2f ms | lock wait %.2f, insert %.2f, light %.2f, terrain %.2f", c->cx, c->cz, ct.ms(),
                        waitMs, insertMs - waitMs, lightMs, ct.ms() - insertMs - lightMs);
            }
        }
        genMs = phase.ms();
        phase = Timer();
        terrain.update(cam.x, cam.y, cam.z, false);
        terrainMs = phase.ms();
        phase = Timer();
        bool shot = !opt.screenshot.empty() && frame == (opt.bench ? kBenchFrames - 1 : opt.selftest ? std::max(10, opt.shotFrame - 10) : opt.shotFrame);
        const bool timeUp = opt.seconds > 0 && now - loopStart >= opt.seconds;  // --seconds: shoot the last frame
        if (opt.seconds > 0) shot = timeUp && !opt.screenshot.empty();
        sky.update(cam, dt);
        ui.endFrame();
        if (haveReports) packs::updatePlayer(renderer, ui, game.selectedSlot(), reports, registry);  // held light
        renderer.upscalers().setMenuActive(ui.menuOpen());  // frame generation pauses in menus
        if (!opt.bench) cam.fovY = ui.fovRadians();
        const fs::path* uiShot = ui.screenshotRequest();
        // Animated block textures advance with the game's 20 ticks per second.
        static std::vector<u32> textureRemap;
        if (registry.textureRemap(u64(now * 20.0), textureRemap)) renderer.setTextureRemap(textureRemap);
        if (opt.profile && frame % 120 == 0) {
            size_t moved = 0;
            for (size_t i = 0; i < textureRemap.size(); ++i) moved += textureRemap[i] != i;
            LOG("texture remap: %zu layers, %zu showing another frame (tick %llu)", textureRemap.size(), moved,
                (unsigned long long)u64(now * 20.0));
        }
        renderer.render(cam, opt.fog, shot ? &opt.screenshot : uiShot);
        if (uiShot && !shot) LOG("screenshot written to %s", uiShot->string().c_str());

        if (const double frameMs = pumpMs + frameTop.ms(); opt.profile && frameMs > 20.0 && frame > 5)
            LOG("slow frame %d: %.1f ms | pump %.1f, begin (latency sleep) %.1f, net %.1f, other input/game %.1f, "
                "gen apply %.1f, terrain %.1f, render %.1f", frame, frameMs, pumpMs, beginMs, netMs,
                preMs - beginMs - netMs, genMs, terrainMs, phase.ms());
        const RenderStats& rs = renderer.stats();
        u32 commands = rs.commands[0] + rs.commands[1] + rs.commands[2] + rs.commands[3];
        if (frame >= 5) {
            cpuMs.push_back(dt * 1000.0f);
            gpuMs.push_back(rs.gpuMs);
            draws.push_back(float(commands));
            nodes.push_back(float(terrain.stats().active));
        }
        if (now - titleTime > 0.25) {
            titleTime = now;
            char title[256];
            const u32 shown = renderer.upscalers().lastFramesPresented();  // frame generation: frames per rendered frame
            std::snprintf(title, sizeof(title), "minecraftCplusplus | %.0f fps%s | gpu %.2f ms | %u draws | %s",
                          dt > 0 ? 1.0f / dt : 0.0f,
                          shown > 1 ? (" (" + std::to_string(int(float(shown) / std::max(dt, 1e-4f))) + " shown)").c_str() : "",
                          rs.gpuMs, commands, game.status().c_str());
            window.setTitle(menu ? "minecraftCplusplus" : title);  // the title screen: no debug line
        }
        if (shot) LOG("screenshot written to %s", opt.screenshot.string().c_str());
        if (now - saveTime > 300.0) {  // the game autosaves every 5 minutes too
            saveTime = now;
            saveNow("autosave");
        }
        // First frames drawn: an instance started by another shows its window now (the title screen waits for some
        // far terrain, briefly).
        if (frame >= 2 && (!menu || terrain.stats().farRegions >= 24 || now - loopStart > 2.0)) launch::ready(window);
        if (menu && ui.switching()) {  // the next instance is loading: give it the machine
            if (genService) genService->setWanted({});
            Sleep(15);
        }
        if ((opt.bench && frame + 1 >= kBenchFrames) || (shot && !opt.bench) || timeUp || ui.quitRequested()) break;
        frameEnd = Timer();
    }
    // "Save and Quit to Title" / "Disconnect" / "Back to Title Screen": save, stop the local server, then hand the
    // window over to a title instance (platform/launch.h), showing progress meanwhile.
    if (ui.titleRequested()) {
        ui.showSaving();
        if (netClient) netClient->stop();
        auto stopping = std::async(std::launch::async, [&] {
            saveNow("exit");
            if (localServer) localServer->stop();
        });
        double prev = nowSeconds();
        bool open = true;
        auto idleFrame = [&] {
            u32 w, h;
            if (window.takeResize(w, h)) renderer.resize(w, h);
            renderer.beginFrame();
            const double now = nowSeconds();
            ui.beginFrame(now - prev);
            prev = now;
            ui.endFrame();
            renderer.render(cam, opt.fog, ui.screenshotRequest());
        };
        while ((open = window.pump()) && stopping.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) idleFrame();
        stopping.wait();
        if (open) {
            ui.startTitle();
            while (window.pump() && !ui.quitRequested()) idleFrame();
        }
    }
    saveNow("exit");
    if (session) session->report();
    audio.report();

    if (!cpuMs.empty()) {
        auto avg = [](const std::vector<float>& v) { return std::accumulate(v.begin(), v.end(), 0.0f) / float(v.size()); };
        LOG("frames  %zu | avg %.0f fps (%.2f ms) | p99 frame %.2f ms | gpu avg %.2f ms, p99 %.2f ms | %.0f draws, %.0f "
            "nodes/frame",
            cpuMs.size(), 1000.0f / avg(cpuMs), avg(cpuMs), percentile(cpuMs, 0.99f), avg(gpuMs),
            percentile(gpuMs, 0.99f), avg(draws), avg(nodes));
    }
    LOG("detail  %zu sections resident, %zu pending | %zu nodes in the cut", terrain.stats().detailResident,
        terrain.stats().detailPending, terrain.stats().active);
    if (terrain.stats().farRegions || terrain.stats().farPending)
        LOG("far     %zu regions generated, %zu pending | %.0f / %.0f MB geometry arena", terrain.stats().farRegions,
            terrain.stats().farPending, double(renderer.arenaBytesUsed()) / (1 << 20),
            double(renderer.arenaBytesTotal()) / (1 << 20));
    for (int level = 2; level <= Terrain::kMaxLevel; ++level)
        if (terrain.stats().farJobs[level])
            LOG("far     level %d: %zu regions, %.1f ms each", level, terrain.stats().farJobs[level],
                terrain.stats().farMs[level] / double(terrain.stats().farJobs[level]));
    return 0;
}
