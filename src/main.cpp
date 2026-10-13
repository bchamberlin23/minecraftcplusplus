#include "assets/assets.h"
#include "assets/block_shapes.h"
#include "assets/game_reports.h"
#include "audio/audio.h"
#include "audio/tickable_sounds.h"
#include "render/post_effects.h"
#include "audio/audio_test.h"
#include "core/common.h"
#include "core/jobs.h"
#include "entity/entities.h"
#include "blockentity/block_entities.h"
#include "entity/entity_render.h"
#include "entity/visuals.h"
#include "game/game.h"
#include "game/gameplay.h"
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
#include "social/friends.h"
#include "ui/mp_hud.h"
#include "ui/ui.h"
#include "mesh/mesher.h"
#include "world/biome_colors.h"
#include "world/lighting.h"
#include "world/save.h"
#include "world/io_test.h"
#include "world/terrain.h"
#include "worldgen/dimensions.h"
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

#include <windows.h>
#include <psapi.h>

namespace {

constexpr float kPi = 3.14159265f;

struct Options {
    fs::path world, jar, screenshot;
    int radius = -1;  // automatic: nearby preview in --play, whole save in the viewer; 0 = all
    int detail = 32;  // full-detail radius in chunks; LOD beyond
    float fog = 16384.0f;
    int width = 1920, height = 1080;
    bool bench = false, fly = false, debug = false, vsync = false, selftest = false;
    bool readonly = false, forceSave = false;
    bool worldIoTest = false;
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
    std::string pathTracing;  // --path-tracing low|medium|high|ultra (with --shaders pathtraced)
    float sharpness = -1;                  // --sharpness 0..1
    int fpsLimit = -1;                     // --fps-limit (0 = unlimited)
    // Front end (ui/title.cpp): the title screen when no world or server is given.
    fs::path savesDir, serversFile, serversDat;  // --saves-dir / --servers-file / --servers-dat (tests: scratch copies)
    std::string menu;                            // --menu worlds|servers: the title screen opens that list
    std::wstring childScript;                    // --child-script: test script for the instance the menu starts
    std::optional<LocalServer::Config::Create> create;  // --create-*: a new world, made by the local server
    std::string friendJoin;                      // --friend-join <code>: a friend's world through the friends tunnel
    fs::path transfer;                           // --transfer <file>: a server's transfer (cookies, the server entry)
};

void usage() {
    LOG("usage: minecraftCplusplus [--world <name|path>] [--radius <chunks to load, 0 = whole save>]\n"
        "                          (default radius: detail + 16 in --play, whole save in viewer)\n"
        "                          [--detail <full-detail chunks>] [--fog <blocks>] [--jar <client.jar>]\n"
        "                          [--size WxH] [--vsync] [--debug] [--bench] [--fly] [--screenshot <file.png>]\n"
        "                          [--cam x,y,z[,yaw,pitch]] [--readonly] [--selftest [--save]] [--world-io-test]\n"
        "                          [--no-gen] [--gen-radius <chunks>] [--far <blocks>] [--lod-quality <x, default 2>] [--gentest <region dir> <seed>]\n"
        "                          [--connect <host[:port]> [--name <player>] [--transfer <file>]] [--play [--lan]]\n"
        "                          [--seconds <n>] [--mods-dir <dir>]\n"
        "                          [--shaders off|low|medium|high|ultra|pathtraced (default high)] [--render-scale <0.33..1>]\n"
        "                          [--path-tracing low|medium|high|ultra (default high)]\n"
        "                          [--upscaler taa|dlss|fsr|xess] [--upscale-quality native|ultraqualityplus|ultraquality|quality|\n"
        "                           balanced|performance|ultraperformance] [--sharpness 0..1]\n"
        "                          [--framegen off|dlss|dlss2x|dlss3x|dlss4x|fsr|xess] [--low-latency off|on|boost] [--fps-limit <n>]\n"
        "                          [--saves-dir <dir>] [--servers-file <servers.json>] [--servers-dat <servers.dat>]\n"
        "                          [--menu worlds|servers] [--child-script <file>] [--create-name <name> --create-seed <seed>\n"
        "                           --create-mode survival|hardcore|creative --create-difficulty <d> --create-cheats 0|1]\n"
        "                          [--friend-join <friend code> [--name <player>]] | --friends-selftest | --friends-node ...\n"
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
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--friends-selftest") return social::runFriendsSelfTest();  // social/
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--friends-node") return social::runFriendsNode(argc - 2, argv + 2);
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
        else if (a == L"--world-io-test") opt.worldIoTest = true;
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
        else if (a == L"--path-tracing") opt.pathTracing = fs::path(next()).string();
        else if (a == L"--framegen") opt.frameGen = fs::path(next()).string();
        else if (a == L"--low-latency") opt.lowLatency = fs::path(next()).string();
        else if (a == L"--sharpness") opt.sharpness = float(_wtof(next().c_str()));
        else if (a == L"--fps-limit") opt.fpsLimit = _wtoi(next().c_str());
        else if (a == L"--saves-dir") opt.savesDir = next();
        else if (a == L"--servers-file") opt.serversFile = next();
        else if (a == L"--servers-dat") opt.serversDat = next();
        else if (a == L"--menu") opt.menu = fs::path(next()).string();
        else if (a == L"--child-script") opt.childScript = next();
        else if (a == L"--transfer") opt.transfer = next();
        else if (a == L"--friend-join") opt.friendJoin = fs::path(next()).string(), opt.connect = "friend", opt.generate = false;
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
            else if (a == L"--create-type") opt.create->levelType = s;          // ui/world_create.cpp
            else if (a == L"--create-generator") opt.create->generatorSettings = s;
            else if (a == L"--create-biome") opt.create->singleBiome = s;
            else if (a == L"--create-bonus") opt.create->bonusChest = v == L"1";
            else if (a == L"--create-gamerule" && s.find('=') != std::string::npos)
                opt.create->gameRules.emplace_back(s.substr(0, s.find('=')), s.substr(s.find('=') + 1));
            else if (a == L"--create-packs" || a == L"--create-disabled-packs") {
                auto& list = a == L"--create-packs" ? opt.create->enabledPacks : opt.create->disabledPacks;
                for (size_t i = 0; i <= s.size();) {
                    const size_t comma = std::min(s.find(',', i), s.size());
                    if (comma > i) list.push_back(s.substr(i, comma - i));
                    i = comma + 1;
                }
            }
        } else {
            usage();
            return 1;
        }
    }

    if (opt.create && opt.create->gameMode == "hardcore")  // hardcore: survival, hard, one life
        opt.create->gameMode = "survival", opt.create->hardcore = true, opt.create->difficulty = "hard";
    // No world, server or tool given: the title screen, over a live world panorama (ui/title.cpp).
    const bool menu = opt.world.empty() && opt.connect.empty() && !opt.play && opt.gentestDir.empty() && !opt.bench &&
                      !opt.selftest && !opt.audioTest && !opt.worldIoTest;
    if (menu) {
        opt.detail = std::min(opt.detail, 10);  // like the game's panorama: full detail only, fogged at its edge
        if (opt.genRadius < 0) opt.genRadius = 10;
        if (opt.farBlocks < 0) opt.farBlocks = 0, opt.fog = std::min(opt.fog, float(opt.detail * 16));
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
    // Friends (social/friends.h): presence, invites and friends' worlds through a peer-to-peer tunnel. Off for tools and
    // benchmarks, and for scripted test runs unless they name a friends folder (MCPP_FRIENDS_DIR).
    const bool friendsOn = !opt.friendJoin.empty() || (!opt.bench && !opt.selftest && !opt.audioTest && !opt.worldIoTest && opt.gentestDir.empty() &&
                                                       (opt.script.empty() || _wgetenv(L"MCPP_FRIENDS_DIR")));
    if (friendsOn &&
        !social::Friends::get().start({social::Friends::defaultDir(), opt.name != "Player" ? opt.name : ownership.playerName, {}}))
        LOG("warning: friends are unavailable in this run");
    std::string friendError;
    std::future<u16> friendTunnel;  // --friend-join: set up while the game loads
    if (!opt.friendJoin.empty())
        friendTunnel = std::async(std::launch::async, [&] {
            social::Key32 key;
            if (!social::Friends::parseCode(opt.friendJoin, key)) return friendError = "That is not a friend code.", u16(0);
            return social::Friends::get().join(key, 40.0, friendError);
        });
    if (opt.gentestDir.empty() && !opt.audioTest && !opt.worldIoTest) upscale::preinit();  // Streamline's slInit, beside the loading
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
    if (opt.radius < 0) opt.radius = opt.play ? opt.detail + 16 : 0;
    if (opt.connect.empty() && !menu && !opt.worldIoTest && !(opt.play && opt.create) &&
        (worldDir.empty() || !fs::exists(worldDir / "level.dat"))) {
        LOG("error: world not found");
        return 1;
    }
    LOG("cpu threads %u | jar %s | world %s | load radius %d | detail radius %d chunks", Jobs::threadCount(),
        version.c_str(), worldDir.filename().string().c_str(), opt.radius, opt.detail);

    // Mods and packs (mods/mod_manager.h): resource and data packs layered over the game's files for every loader
    // below, Fabric mods for the local server, native mods.
    // A broken pack from the internet must not take the game down (one data pack's tag file did).
    try {
        mods::Manager::get().init({mc, version, jar, opt.connect.empty() ? worldDir : fs::path(), opt.modsDir,
                                   opt.play && opt.connect.empty()});
    } catch (const std::exception& e) {
        LOG("error: mods and packs: %s (continuing without some of them)", e.what());
    }

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
    }

    Timer t;
    GameReports reports;
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    const bool haveReports = reports.load(mc, version, fs::path(local ? local : L".") / "minecraftCplusplus" / "reports");
    if (!haveReports) LOG("warning: block definitions unavailable; generated terrain may miss some block states");
    audio::AudioSystem audio;  // loads in the background
    // Automated runs (a script, or MCPP_HEADLESS=1) stay out of the owner's way: no focus, no cursor, no sound.
    const bool headless = !opt.script.empty() || _wgetenv(L"MCPP_HEADLESS");
    audio.init({mc, version, fs::path(local ? local : L".") / "minecraftCplusplus", haveReports ? &reports : nullptr,
                opt.audioCapture, headless});
    if (!opt.audioCapture.empty()) audio.waitReady(60.0);
    BlockShapes blockShapes;  // exact selection / collision shapes from the game's code (cached), parsed meanwhile
    auto shapesTask = std::async(std::launch::async, [&] {
        return blockShapes.load(mc, version, fs::path(local ? local : L".") / "minecraftCplusplus" / "shapes");
    });
    BlockRegistry registry;
    if (!registry.init(jar, haveReports ? &reports : nullptr)) return 1;
    if (shapesTask.get()) registry.setShapes(&blockShapes);  // before the world creates its block states
    if (opt.worldIoTest) return runWorldIoTest(registry, gameDataVersion(jar));
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

    // Creating/upgrading writes the save first. Load independent assets while the JVM does that work,
    // then wait only at the first read of player/chunk data.
    if (localServer && (localServer->upgrading() || localServer->creating())) {
        LOG(localServer->creating() ? "server  creating the world first..." : "server  upgrading the world to %s first...",
            version.c_str());
        while (!localServer->ready() && localServer->running()) Sleep(10);
        if (!localServer->ready()) return 1;
    }
    PlayerStart player = World::readPlayer(worldDir);
    if (opt.haveCam) {
        player.x = opt.cam[0], player.y = opt.cam[1] - 1.62, player.z = opt.cam[2];
        player.yaw = float(opt.cam[3]), player.pitch = float(opt.cam[4]);
    }
    // The save's dimension to show: --dimension, else where the player is (Nether / End / a data pack's own).
    const std::string viewDim = !opt.sky.dimension.empty() ? opt.sky.dimension : menu ? "minecraft:overworld" : player.dimension;
    const bool viewOverworld = viewDim == "minecraft:overworld";
    if (!opt.play && opt.connect.empty() && opt.sky.dimension.empty()) opt.sky.dimension = viewDim;  // its sky / fog
    t = Timer();
    World world;
    LoadStats ls;
    // The save loads while the other loaders finish (they are only needed once a client or the window exists).
    std::future<bool> worldTask;
    if (menu && !titleChunks) world.initEmpty(-4, 19);  // the title screen's world is all generated (overworld height)
    if (opt.connect.empty() && (!menu || titleChunks))
        worldTask = std::async(std::launch::async, [&] {
            if (localServer && !fs::exists(worldDir / "region") &&
                !fs::exists(worldDir / "dimensions" / "minecraft" / "overworld" / "region")) {
                // A freshly created server may still hold all its chunks in memory.
                world.initEmpty(-4, 19);
                return true;
            }
            const bool loaded = world.load(worldDir, registry, int(std::floor(player.x)) >> 4,
                                           int(std::floor(player.z)) >> 4, opt.radius, ls, viewDim);
            // An existing world can have no saved overworld chunks near this position (e.g. its
            // owner logged out in the Nether). The server supplies the authoritative destination.
            if (!loaded && localServer) world.initEmpty(-4, 19);
            return loaded || bool(localServer);
        });
    const bool haveBiomeColors = biomeTask.get();
    if (!haveBiomeColors) LOG("warning: biome colours unavailable; using the default tints");
    // Entities: models, textures and type data from the installed game; state from the server.
    const bool haveEntities = entityTask.get();
    ent::EntityWorld entities(entityAssets, world, registry, reports);
    ent::EntityRenderer entityRenderer(entities);  // declared before the renderer: destroyed after it
    PostEffects postEffects;                       // the spectated entity's post effect (likewise)
    // Hands, held / worn items, item frames, paintings, name tags, particles (same lifetime rule).
    ent::Visuals visuals(entities, entityRenderer);
    // Local player gameplay rules and server state (mining speed, effects, attributes, cracks of others).
    Gameplay gameplay(world, registry, reports);
    auto gameplayTask = std::async(std::launch::async, [&] {
        return !menu && haveReports &&
               gameplay.load(mc, version, fs::path(local ? local : L".") / "minecraftCplusplus" / "shapes",
                             fs::path(local ? local : L".") / "minecraftCplusplus" / "reports" / version / "reports" /
                                 "minecraft" / "components" / "item");
    });
    if (haveEntities)
        visuals.load(jar, mc, reports,
                     fs::path(local ? local : L".") / "minecraftCplusplus" / "reports" / version / "reports" / "minecraft" /
                         "components" / "item");
    // Chests, sign text, banners, heads, ...: the game's block entity renderers (same lifetime rule).
    be::BlockEntityRenderer blockEntityRenderer(world, registry, reports, entityAssets, entityRenderer);
    if (haveEntities) blockEntityRenderer.load(jar, mc);
    blockEntityRenderer.setItemDrawer(haveEntities);  // shown items and profile heads: the entity visuals (VIS-14)
    sky::Sky sky;                                  // same
    if (!uiTask.get()) return 1;
    // --- multiplayer (--connect): log in; the world then comes from the server instead of a save ---
    std::unique_ptr<net::Client> netClient;
    std::unique_ptr<net::Session> session;
    std::unique_ptr<net::Session> joiningSession;  // logging in on a worker (see the loop)
    bool connectDeferred = false;  // --connect: the configuration waits for the player (prompts): joined in the loop
    std::string serverEntry = opt.connect;  // the server list entry of this connection (a transfer keeps the first)
    if (friendTunnel.valid()) {  // the friend's world: through the tunnel's loopback port
        const u16 port = friendTunnel.get();
        if (!port) {
            LOG("error: %s", friendError.c_str());
            return 1;
        }
        opt.connect = "127.0.0.1:" + std::to_string(port);
    }
    if (!opt.connect.empty()) {
        const net::ServerAddress address = net::resolveAddress(net::parseAddress(opt.connect));  // + SRV, like the game
        const std::string host = address.host;
        const u16 port = address.port;
        netClient = std::make_unique<net::Client>(reports, registry);
        audio.attachNet(*netClient);
        if (haveEntities) entities.attach(*netClient), blockEntityRenderer.attach(*netClient), visuals.attach(*netClient);
        gameplay.attach(*netClient);
        sky.attach(*netClient, haveReports ? &reports : nullptr);
        packs::attachNetwork(*netClient);  // shader pack uniforms (isElytraFlying, heavyFog)
        ui.registerHandlers(*netClient);
        net::Client::Config config{host, port, opt.name, jar};
        if (!opt.transfer.empty()) {  // ClientboundTransferPacket: the previous server's cookies and list entry
            config.transfer = true;
            std::vector<u8> bytes;
            const auto j = readFile(opt.transfer, bytes) ? nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false)
                                                         : nlohmann::json();
            if (j.is_object()) {
                serverEntry = j.value("entry", serverEntry);
                for (const auto& [key, value] : j.value("cookies", nlohmann::json::object()).items()) {
                    const std::string h = value.is_string() ? value.get<std::string>() : std::string();
                    std::string& payload = config.cookies[key];
                    for (size_t i = 0; i + 1 < h.size(); i += 2) payload += char(std::stoi(h.substr(i, 2), nullptr, 16));
                }
            }
            std::error_code ec;
            fs::remove(opt.transfer, ec);
        }
        if (!netClient->start(config)) return 1;
        session = std::make_unique<net::Session>(*netClient, world, registry, reports);
        net::Dimension dim;
        if (!session->join(30.0, player, dim, &connectDeferred) && !connectDeferred) return 1;
        if (connectDeferred) {  // a pack prompt or the code of conduct: the window shows them, the join goes on there
            joiningSession = std::move(session);
            world.initEmpty(-4, 19);
            LOG("server  %8.1f ms  the server's configuration needs the player; joining continues in the window", t.ms());
        } else {
            world.initEmpty(std::min(-4, dim.minY >> 4), std::max(19, (dim.minY + dim.height - 1) >> 4));
            LOG("server  %8.1f ms  joined %s as %s at %.1f %.1f %.1f", t.ms(), opt.connect.c_str(), opt.name.c_str(),
                player.x, player.y, player.z);
        }
    }
    // --- end multiplayer ---
    if (worldTask.valid() && !worldTask.get()) return 1;
    if (!session) LOG("world   %8.1f ms  %zu regions, %zu chunks, %zu sections, %.0f MB read, %zu block states%s", t.ms(),
        ls.regions, ls.chunks, ls.sections, double(ls.compressedBytes) / (1 << 20), registry.blockCount(),
        ls.unsupportedChunks ? " (some chunks in an unreadable format skipped)" : "");
    if (!session && !viewOverworld) LOG("world   showing %s", viewDim.c_str());

    std::future<bool> joining;
    PlayerStart joinStart;
    net::Dimension joinDim;
    Timer joinTimer;
    bool joinFailed = false;
    auto beginLocalJoin = [&] {
        joinTimer = Timer();
        netClient = std::make_unique<net::Client>(reports, registry);
        audio.attachNet(*netClient);
        if (haveEntities) entities.attach(*netClient), blockEntityRenderer.attach(*netClient), visuals.attach(*netClient);
        gameplay.attach(*netClient);
        sky.attach(*netClient, haveReports ? &reports : nullptr);
        packs::attachNetwork(*netClient);
        ui.registerHandlers(*netClient);
        const net::Client::Config config{"127.0.0.1", localServer->port(), opt.name, jar, localServer->stateTable()};
        joining = std::async(std::launch::async, [&, config] {
            if (!netClient->start(config)) return false;
            joiningSession = std::make_unique<net::Session>(*netClient, world, registry, reports);
            return joiningSession->join(30.0, joinStart, joinDim);
        });
    };
    // New/upgraded worlds are already ready here: overlap their protocol setup/login with
    // GPU initialization and the initial terrain builds instead of waiting for the frame loop.
    if (localServer && localServer->ready()) beginLocalJoin();

    // Benchmarks and the self-test never write to a world unless --save is given.
    WorldSaver saver(world, registry, worldDir, viewDim);
    const bool wantSave = !session && !localServer && (opt.forceSave || (!opt.readonly && !opt.bench && !opt.selftest));
    const bool canSave = wantSave && saver.lock();
    if (wantSave && !canSave) LOG("note: the world is open in another program (session.lock); running read-only");

    Window window;
    // An instance started by another (platform/launch.h) opens where that window was, invisible until its first frames.
    window.setHeadless(headless);
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
    if (haveEntities) renderer.addPass(&entityRenderer.outlinePass(), PassStage::Hud);  // before the UI's Hud pass
    renderer.addPass(&postEffects, PassStage::Hud);
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
    if (opt.bench || opt.selftest || menu) terrain.update(cam.x, cam.y, cam.z, true);
    else terrain.prime(cam.x, cam.y, cam.z);
    LOG("detail  %8.1f ms  %zu full-detail sections around the camera", t.ms(), terrain.stats().detailResident);
    LOG("total   %8.1f ms  launch to first frame | %.0f / %.0f MB geometry arena on %s", total.ms(),
        double(renderer.arenaBytesUsed()) / (1 << 20), double(renderer.arenaBytesTotal()) / (1 << 20),
        renderer.adapterName().c_str());

    // Chunks the save does not have are generated from the world seed with the game's own rules: each dimension
    // with the generator its WorldGenSettings name (noise settings and biome source, flat, debug; data-pack ones).
    std::unique_ptr<wg::DataPack> gamedata;
    wg::WorldGenSettings genSettings;
    std::string genDimension;  // the dimension the generators below are for ("" while there are none)
    std::shared_ptr<wg::ChunkGenerator> generator;  // shared with the far threads (they keep the one they started with)
    std::mutex generatorMutex;
    std::unique_ptr<wg::FeatureSystem> features;
    std::unique_ptr<wg::StructureSystem> structures;  // villages, mineshafts... placed with the features
    std::unique_ptr<wg::GenService> genService;
    std::deque<std::unique_ptr<wg::GenChunk>> genBacklog;  // generated chunks waiting for the per-frame budget
    struct FarStop {  // destroyed before the generator the far threads call into
        Terrain& terrain;
        ~FarStop() { terrain.stopFar(); }
    } farStop{terrain};
    i64 seed = 0;
    std::mt19937_64 titleRandom(std::random_device{}());
    if (const char* s = std::getenv("MCPP_TITLE_SEED")) titleRandom.seed(std::strtoull(s, nullptr, 10));  // tests
    // Saved chunks next to generated ones are read-only neighbours for decoration.
    auto exists = [&world](int cx, int cz) {
        std::shared_lock lock(world.mutex());
        return world.hasChunk(cx, cz);
    };
    auto load = [&world, &generator](int cx, int cz, wg::GenChunk& out) {
        out.init(cx, cz, generator->minY(), generator->height());
        u16 ids[4096];
        for (int sy = out.minY >> 4; sy < (out.minY + out.height) >> 4; ++sy) {
            {  // the lock per section only: a preempted worker holding it for a whole chunk made the frame
               // thread (inserting chunks) wait a scheduler quantum
                std::shared_lock lock(world.mutex());
                const World::Region* r = nullptr;
                const World::Section* s = world.section(cx, sy, cz, &r);
                if (!s) continue;
                World::decode(*r, *s, ids);
            }
            for (int i = 0; i < 4096; ++i) out.set(i & 15, sy * 16 + (i >> 8), (i >> 4) & 15, ids[i]);
        }
    };
    // The generator stack of one dimension; the previous one's workers are stopped first. In singleplayer too: the
    // server sends the chunks it simulates, these fill the view beyond them (and are replaced by the server's).
    bool farStarted = false;
    auto startWorldgen = [&](const std::string& dim) {
        genService.reset();           // joins its workers
        terrain.setFarActive(false);  // drops the previous dimension's far terrain (running jobs finish unused)
        genBacklog.clear();
        structures.reset(), features.reset();
        {
            std::lock_guard lock(generatorMutex);
            generator.reset();
        }
        genDimension.clear();
        const wg::DimensionSpec* spec = genSettings.find(dim);
        if (!spec) return LOG("worldgen: no generator for %s", dim.c_str()), false;
        auto fresh = std::make_shared<wg::ChunkGenerator>(*gamedata, registry, genSettings.seed, *spec);
        if (!fresh->ok()) return false;
        {
            std::lock_guard lock(generatorMutex);
            generator = std::move(fresh);
        }
        Timer ft;
        try {  // data packs can change worldgen in ways the generator cannot read: then no local generation
            features = std::make_unique<wg::FeatureSystem>(*generator, registry);
            if (genSettings.generateStructures) {
                structures = std::make_unique<wg::StructureSystem>(*generator, features.get(), registry, genSettings.seed);
                if (structures->ok()) features->setStructures(structures.get());
            }
        } catch (const std::exception& e) {
            LOG("error: decoration of %s from the data packs failed: %s (far terrain only)", dim.c_str(), e.what());
            structures.reset(), features.reset();
        }
        LOG("features    %6.1f ms  %s (%s)", ft.ms(), dim.c_str(), spec->generator.value("type", std::string("?")).c_str());
        const char* genThreads = std::getenv("MCPP_GEN_THREADS");  // profiling override
        if (features)
            genService = std::make_unique<wg::GenService>(*generator, features.get(), exists, load,
                                                          genThreads ? unsigned(std::atoi(genThreads))
                                                                     : std::max(2u, Jobs::threadCount() / 2));
        // Beyond the generated chunks: terrain approximated per region at LOD resolution.
        if (!farStarted)
            terrain.enableFar(
                [&generator, &generatorMutex](int rx, int rz, int level, const FarColumns* coarser, FarColumns& out) {
                    std::shared_ptr<wg::ChunkGenerator> gen;
                    {
                        std::lock_guard lock(generatorMutex);
                        gen = generator;
                    }
                    if (!gen) return;
                    thread_local wg::DensityContext ctx;
                    thread_local std::vector<u16> genBiomes;
                    thread_local std::vector<u8> toWorld;  // generator biome id -> World biome id
                    thread_local const wg::ChunkGenerator* owner = nullptr;
                    if (owner != gen.get()) toWorld.clear(), ctx.clear(), owner = gen.get();
                    gen->generateCoarse(rx, rz, level, out, ctx, coarser, nullptr, &genBiomes);
                    std::vector<u8>& biomes = out.biomes;
                    biomes.resize(genBiomes.size());
                    for (size_t i = 0; i < genBiomes.size(); ++i) {
                        const u16 b = genBiomes[i];
                        if (b >= toWorld.size()) toWorld.resize(size_t(b) + 1, 0);
                        if (!toWorld[b]) toWorld[b] = u8(World::biomeId(gen->biomes().name(b)) + 1);
                        biomes[i] = u8(toWorld[b] - 1);
                    }
                },
                generator->minY(), std::max(2u, Jobs::threadCount() / 4));
        farStarted = true;
        terrain.setFarActive(true);
        genDimension = dim;
        return true;
    };
    if (opt.generate && (menu && !titleCached ? (seed = i64(titleRandom()), true) : World::readSeed(worldDir, seed))) {
        t = Timer();
        gamedata = std::make_unique<wg::DataPack>();
        try {
            if (gamedata->open(jar)) {
                if (menu || !wg::readWorldGenSettings(worldDir, *gamedata, genSettings))
                    genSettings = wg::defaultWorldGenSettings(*gamedata, seed);
                startWorldgen(viewDim);  // the save's dimension; play mode switches with the server's
            }
        } catch (const std::exception& e) {
            LOG("error: world generation from the data packs failed: %s (the server still generates the world)", e.what());
            genService.reset(), structures.reset(), features.reset(), generator.reset();
            genDimension.clear();
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
    double genScan = 0, evictScan = 0;
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
    bool texturesReloaded = false;  // server resource packs: the block textures need uploading
    Game game(world, registry, terrain, renderer, player);
    audio::TickableSounds tickableSounds(audio, entities, game);
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
            if (cheats) {  // commands for everyone here (and the host), and for everyone who joins later
                localServer->command("op @a");
                localServer->setOpJoiners(true);
            }
            LOG("lan: open to LAN on port %u as \"%s\"", port, motd.c_str());
            return true;
        };
        fe.serverCommand = [&](const std::string& line) {
            if (localServer) localServer->command(line);
        };
        fe.publishLan = [&](const std::string& motd, u16 port, std::string& error) {
            if (!localServer) return false;
            if (!lanHost.start(motd, port, localServer->port(), !opt.lan, error)) return false;
            LOG("lan: open to LAN on port %u as \"%s\"", port, motd.c_str());
            return true;
        };
        fe.unpublishLan = [&] { lanHost.stop(); };
        fe.setGuestCommands = [&](bool on) {
            if (localServer) localServer->setOpJoiners(on);
        };
        fe.setForceGameMode = [&](bool on, const std::string& mode) {
            if (localServer) localServer->setForceGameMode(on, mode);
        };
        fe.setAllowCommands = [&](bool on) {
            if (localServer) localServer->setAllowCommands(on);
        };
        fe.lockDifficulty = [&] {
            if (localServer) localServer->setDifficultyLocked();
        };
        fe.serverAddress = serverEntry;
        fe.reloadResources = [&] {  // server resource packs: the block textures (uploaded before the next frame)
            texturesReloaded = registry.reloadTextures();
            return texturesReloaded;
        };
        fe.pauseServer = [&](bool paused) {  // the local server's console (op level 4)
            if (localServer) localServer->command(paused ? "tick freeze" : "tick unfreeze");
        };
        ui.setFrontEnd(fe);
    }
    if (!ui.init({&registry, &renderer, &window, &game, &world, &terrain, opt.detail})) return 1;
    if (session) ui.attachSession(netClient.get(), session.get());
    if (!opt.script.empty()) ui.loadScript(opt.script);
    // The saved shader pack (render/packs), unless this run's command line picks a pack or a shader mode.
    if (!packs::commandLinePack() && !opt.shadersGiven && !ui.settings().shaderPack.empty())
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
        if (!opt.pathTracing.empty() && !VideoSettings::parsePathTracing(opt.pathTracing, v.pathTracing))
            LOG("warning: unknown --path-tracing %s", opt.pathTracing.c_str());
        if (!opt.lowLatency.empty() && !VideoSettings::parseLowLatency(opt.lowLatency, v.lowLatency))
            LOG("warning: unknown --low-latency %s", opt.lowLatency.c_str());
        if (opt.sharpness >= 0) v.sharpness = opt.sharpness;
        if (opt.fpsLimit >= 0) v.fpsLimit = u32(opt.fpsLimit);
        if (opt.vsync) v.vsync = true;
        if (opt.renderScale != 1.0f) v.renderScaleOverride = opt.renderScale;
        renderer.upscalers().apply(v);
    }
    std::string lastDimension;  // temporal history (TAA / upscalers / frame generation) resets on dimension change
    audio.attachGame(game, world, registry);
    if (haveEntities) visuals.setPlayer(game, ui, opt.name, [&] { return session ? session->gameMode() : 1; });
    gameplayTask.get();
    if (haveEntities) {
        gameplay.setEntities(&entities);
        blockEntityRenderer.setRules(&gameplay.rules());
        visuals.setItemRules(&gameplay.rules());
        visuals.setParticleSources([&](std::string_view tag, std::string_view block) { return gameplay.rules().inTag(tag, block); },
                                   [&](int x, int z) {
                                       return haveBiomeColors ? biomeColors.color(world.biomeAt(x, z), BiomeColors::Foliage) : 0x48B518u;
                                   });
        entities.onUntrackedEntityEvent = [&](int id, int event) {  // our own entity events
            if (event == 2 && session && id == session->entityId()) gameplay.kineticHit();  // EntityEvent.KINETIC_HIT
        };
        blockEntityRenderer.setBlockSink([&](const std::vector<net::BlockChange>& c) {
            if (session) session->applyClientBlocks(c);
        });
        game.pistonSource = [&](std::vector<Physics::Piston>& out) { blockEntityRenderer.pistonMovers(out); };
    }
    gameplay.setPlayer(game, ui);
    game.playerName = opt.name;
    if (haveEntities) {  // transient blocks wait for their section's new mesh
        entities.meshSerial = [&] { return terrain.meshSerial(); };
        entities.sectionMeshSerial = [&](int cx, int sy, int cz) { return terrain.sectionMeshSerial(cx, sy, cz); };
    }
    if (haveEntities) ui.setEntities(&entities);  // inventory / mount previews, player list heads
    game.onEntityClick = [&](bool attack, bool use, double blockDistance) {  // attack / use entities first
        if (!session || !haveEntities) return false;
        const CameraState c = game.camera();
        const double eye[3] = {c.x, c.y, c.z}, cp = std::cos(c.pitch);
        const double dir[3] = {-std::sin(c.yaw) * cp, -std::sin(c.pitch), std::cos(c.yaw) * cp};
        if (attack && game.attackMaxReach >= 0) {  // the held weapon's attack_range (LocalPlayer.raycastHitResult)
            const double* v = game.player().vel;
            const double forward = v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2];  // getKnownMovement . look
            return entities.attackInRange(eye, dir, game.attackMinReach, game.attackMaxReach, game.attackMargin, forward,
                                          [&](const double o[3], const double d[3], double max) {
                                              const RayHit h = game.physics().raycast(o, d, max);
                                              return h.hit ? h.distance : max;
                                          });
        }
        return entities.click(eye, dir, attack, use, blockDistance, game.entityReach, game.player().sneaking);
    };
    // Minecraft.shouldEntityAppearGlowing (the glowing flag, or players while a spectator holds Spectator Outlines) and
    // Entity.getTeamColor (the team's colour, white without one).
    entityRenderer.outlineColor = [&](const ent::Entity& e) {
        const bool player = e.category == ent::Category::Player;
        const int outlinesKey = ui.settings().key(ui::Key::SpectatorOutlines);
        const bool spectatorOutlines = player && game.gameMode == 3 && outlinesKey && window.keyDown(outlinesKey);
        if (!(e.sharedFlags & 0x40) && !spectatorOutlines) return -1;
        std::string member = e.name;
        if (!player) {
            static const char* kHex = "0123456789abcdef";
            member.clear();
            for (int i = 0; i < 16; ++i) {
                if (i == 4 || i == 6 || i == 8 || i == 10) member += '-';
                member += kHex[e.uuid[i] >> 4], member += kHex[e.uuid[i] & 15];
            }
        }
        const int team = ui::teamColor(member);
        return team >= 0 && team < 16 ? int(ui::kLegacyColors[team]) : 0xFFFFFF;
    };
    // F3 entries the UI cannot see (debug_screen.cpp).
    ui.setEngineInfo([&](std::string_view what) -> std::string {
        char buf[160];
        if (what == "particles") return "P: " + std::to_string(haveEntities ? visuals.stats().particles : 0u);
        if (what == "entities" && haveEntities) {
            const auto& st = entityRenderer.stats();
            std::snprintf(buf, sizeof(buf), "E: %u/%u, quads %u", st.drawn, st.entities, st.quads);
            return buf;
        }
        if (what == "sound_mood") return audio.ready() ? audio.channelDebug() : std::string();
        if (what == "sound_cache") return audio.ready() ? audio.cacheDebug() : std::string();
        if (what == "day_count") return session ? "Day #" + std::to_string(i64(sky.dayTicks() / 24000.0)) : std::string();
        return {};
    });
    game.onSpectatorAttack = [&](double blockDistance) {
        if (!session || !haveEntities) return;
        const CameraState c = game.camera();
        const double eye[3] = {c.x, c.y, c.z}, cp = std::cos(c.pitch);
        const double dir[3] = {-std::sin(c.yaw) * cp, -std::sin(c.pitch), std::cos(c.yaw) * cp};
        entities.spectate(eye, dir, blockDistance, game.entityReach);
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
    const double loopStart = nowSeconds();
    const bool smoothReveal = !opt.bench && !opt.selftest && !menu;
    if (smoothReveal) renderer.beginWorldReveal(cam.x, cam.z);
    double previousStreamX = cam.x, previousStreamZ = cam.z;
    Timer frameEnd;  // window.pump() + anything between frames
    for (int frame = 0; window.pump(); ++frame) {
        const double pumpMs = frameEnd.ms();
        u32 w, h;
        if (window.takeResize(w, h)) renderer.resize(w, h);
        const Timer frameTop;
        if (texturesReloaded) {  // server resource packs changed block textures (ui/server_config.cpp)
            texturesReloaded = false;
            renderer.uploadTextures(registry.texturePixels(), registry.textureCount());
        }
        renderer.beginFrame();  // low-latency sleep (Reflex / XeLL / Anti-Lag 2), frame limiter, simulation markers
        const double beginMs = frameTop.ms();
        double netMs = 0;
        double now = nowSeconds();
        float dt = float(now - last);
        last = now;
        // Singleplayer: once the local server is up, connect and log in on a worker (protocol tables, language,
        // block ids, then the server's login: ~1 s that used to freeze the already visible world), then attach here.
        if (localServer && !session && !joining.valid() && !joinFailed && localServer->ready()) {
            beginLocalJoin();
        }
        if (connectDeferred && !joining.valid()) {  // --connect after the prompts: no timeout while configuring
            connectDeferred = false;
            joinTimer = Timer();
            joining = std::async(std::launch::async, [&] { return joiningSession->join(30.0, joinStart, joinDim, nullptr, true); });
        }
        if (joining.valid() && joining.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const bool joined = joining.get();
            if (joined || !localServer) {  // --connect: a failure shows as the session's disconnect
                if (!joined && netClient->alive()) netClient->disconnect("Timed out");
                session = std::move(joiningSession);
                Player& p = game.player();
                if (joined) {
                    p.pos[0] = joinStart.x, p.pos[1] = joinStart.y, p.pos[2] = joinStart.z;
                    p.yaw = joinStart.yaw * kPi / 180.0f, p.pitch = joinStart.pitch * kPi / 180.0f;
                }
                if (localServer) session->setKeepForgotten(true);  // unloaded server chunks stay visible
                session->attach(game, terrain);
                ui.attachSession(netClient.get(), session.get());
                if (joined)
                    LOG("server  %8.1f ms  playing through %s as %s", joinTimer.ms(),
                        localServer ? "the local server" : opt.connect.c_str(), opt.name.c_str());
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
            if (haveEntities) blockEntityRenderer.applyBlockEvents();  // before the block updates that follow them
            session->frame(dt);  // multiplayer: apply what the server sent, send movement
            gameplay.frame();
            netMs = t.ms();
        }
        if (session && session->dimension().name != lastDimension) {
            if (smoothReveal && (!lastDimension.empty() || session->dimension().name != "minecraft:overworld")) {
                renderer.beginWorldReveal(game.player().pos[0], game.player().pos[2]);
                previousStreamX = game.player().pos[0], previousStreamZ = game.player().pos[2];
            }
            lastDimension = session->dimension().name;
            renderer.resetTemporal();
            // Each dimension is generated (chunks beyond the server's, far terrain) by its own generator.
            if (gamedata && lastDimension != genDimension && !(menu && ui.switching())) {
                const Timer tw;
                startWorldgen(lastDimension);
                LOG("worldgen %7.1f ms  now generating %s", tw.ms(), lastDimension.c_str());
            }
        }
        if (session) terrain.setFarActive(!genDimension.empty() && session->dimension().name == genDimension);
        if (session) game.player().fastLava = session->dimension().fastLava;
        if (session) game.player().waterEvaporates = session->dimension().waterEvaporates;
        if (haveEntities) entities.frame(dt);
        if (haveEntities) entities.viewEntity = game.cameraMode == 0 ? game.cameraEntity : -1;  // set_camera
        {  // GameRenderer.checkEntityPostEffect: on a camera or perspective change (first person only); the toggle key
            static int lastCamera = INT_MIN, lastMode = -1, lastToggles = 0;
            static bool effectActive = false;
            static PostEffects::Effect effect = PostEffects::None;
            if (game.cameraEntity != lastCamera || game.cameraMode != lastMode) {
                lastCamera = game.cameraEntity, lastMode = game.cameraMode;
                effect = PostEffects::None;
                const ent::Entity* e = haveEntities && game.cameraMode == 0 && game.cameraEntity >= 0 ? entities.entity(game.cameraEntity) : nullptr;
                if (e && e->info) {
                    const std::string& n = e->info->name;
                    effect = n == "minecraft:creeper" ? PostEffects::Creeper
                             : n == "minecraft:spider" || n == "minecraft:cave_spider" ? PostEffects::Spider
                             : n == "minecraft:enderman" ? PostEffects::Invert
                                                         : PostEffects::None;
                }
                effectActive = effect != PostEffects::None;
            }
            if (ui.spectatorEffectToggles() != lastToggles) {
                if ((ui.spectatorEffectToggles() - lastToggles) % 2) effectActive = !effectActive;
                lastToggles = ui.spectatorEffectToggles();
            }
            // GameRenderer.update: the player's post effects, then the spectated entity's
            std::vector<PostEffects::Effect> chain;
            if (session)
                for (const std::string& id : ui.postEffects()) chain.push_back(PostEffects::byName(id));
            if (session && effectActive && effect != PostEffects::None) chain.push_back(effect);
            postEffects.setEffects(std::move(chain), float(ui.settings().menuBackgroundBlurriness));
        }
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
            cam.yaw += dt * 0.035f * float(ui.settings().panoramaSpeed);  // the title panorama turns slowly (Panorama Scroll Speed)
        } else {
            game.frame(window, dt);
            cam = game.viewCamera();  // F5: the detached camera
        }
        audio.update(dt, cam, {session ? session->gameMode() : 1,
                               session ? std::string_view(session->dimension().name) : std::string_view()});  // none: no player
        tickableSounds.frame(dt, session && haveEntities && !menu, session ? session->gameMode() : 1);

        const double preMs = frameTop.ms();
        // Only a discontinuous move restarts the entrance wave; walking does not.
        const double stepX = cam.x - previousStreamX, stepZ = cam.z - previousStreamZ;
        if (smoothReveal && stepX * stepX + stepZ * stepZ > 128.0 * 128.0) renderer.beginWorldReveal(cam.x, cam.z);
        previousStreamX = cam.x, previousStreamZ = cam.z;
        Timer phase;
        double genMs = 0, terrainMs = 0;
        if (genService && (!session || session->dimension().name == genDimension) && !(menu && ui.switching())) {
            // Play mode: chunks far behind the player leave memory (the server owns the world; chunks it no longer
            // sends and locally generated ones come back when needed). Kept, they grew ~150 MB/s while travelling.
            if (session && now - evictScan > 2.0) {
                evictScan = now;
                const int ccx = int(std::floor(cam.x)) >> 4, ccz = int(std::floor(cam.z)) >> 4;
                const int keep = genRadius + 8;
                // Structure starts / heights reach 8 chunks past the chunks being generated: trim well beyond.
                if (structures) structures->trim(ccx, ccz, keep + 16);
                std::vector<std::pair<int, int>> evict;
                {
                    std::shared_lock lock(world.mutex());
                    for (const auto& r : world.regions()) {
                        if (std::max(std::abs(r->rx * 32 + 16 - ccx), std::abs(r->rz * 32 + 16 - ccz)) <= keep - 16) continue;
                        for (int i = 0; i < 1024; ++i) {
                            if (!r->columns[i].count) continue;
                            const int cx = r->rx * 32 + (i & 31), cz = r->rz * 32 + (i >> 5);
                            if (std::max(std::abs(cx - ccx), std::abs(cz - ccz)) > keep && !session->tracksChunk(cx, cz))
                                evict.push_back({cx, cz});
                        }
                    }
                }
                if (!evict.empty()) {
                    const Timer te;
                    {
                        std::unique_lock lock(world.mutex());
                        for (const auto& [cx, cz] : evict) world.removeChunk(cx, cz);
                    }
                    for (const auto& [cx, cz] : evict) terrain.removeChunk(cx, cz);
                    if (opt.profile) {
                        size_t bytes = 0, columns = 0, regions = 0;
                        std::shared_lock lock(world.mutex());
                        for (const auto& r : world.regions()) {
                            ++regions;
                            bytes += r->sections.capacity() * sizeof(World::Section) + r->palettes.capacity() * 2 +
                                     r->words.capacity() * 8 + r->lightArrays.size() * sizeof(World::LightArray);
                            for (const auto& c : r->columns) columns += c.count > 0;
                        }
                        PROCESS_MEMORY_COUNTERS_EX pmc{};
                        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
                        HEAP_SUMMARY hs{};
                        hs.cb = sizeof(hs);
                        HeapSummary(GetProcessHeap(), 0, &hs);
                        LOG("evicted %zu chunks beyond %d in %.1f ms | world: %zu regions, %zu chunks, %.0f MB | process private %.0f MB"
                            " | heap allocated %.0f MB, committed %.0f MB", evict.size(), keep, te.ms(), regions, columns,
                            bytes / 1048576.0, pmc.PrivateUsage / 1048576.0, hs.cbAllocated / 1048576.0, hs.cbCommitted / 1048576.0);
                    }
                }
            }
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
            if (genBacklog.size() < 64) {
                const Timer tt;
                const int ccx = int(std::floor(cam.x)) >> 4, ccz = int(std::floor(cam.z)) >> 4;
                for (auto& c : genService->take(64 - genBacklog.size(), ccx, ccz)) genBacklog.push_back(std::move(c));
                if (opt.profile && tt.ms() > 5.0) LOG("gen: take %.1f ms", tt.ms());
            }
            std::sort(genBacklog.begin(), genBacklog.end(), [&](const auto& a, const auto& b) {
                auto distance = [&](const auto& c) {
                    const double dx = c->cx * 16.0 + 8 - cam.x, dz = c->cz * 16.0 + 8 - cam.z;
                    return dx * dx + dz * dz;
                };
                return distance(a) < distance(b);
            });
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
                    world.setGenExtras(c->cx, c->cz, wg::saveExtras(*c, [&registry](u16 s) { return registry.name(s); }));
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
        sky.setBossOverlay(ui::bossDarkenScreen(), ui::bossWorldFog());  // boss bar flags (mp_hud.h)
        sky.setRenderDistance(ui.settings().renderDistance);
        {  // Video / Accessibility Settings the sky reads (Clouds, Cloud Distance, Weather Radius, Brightness, Hide Lightning Flashes)
            const ui::Settings& vs = ui.settings();
            sky.setOptions({vs.cloudStatus, vs.cloudRange, vs.weatherRadius, float(vs.gamma), vs.hideLightningFlash});
        }
        if (haveEntities)
            entityRenderer.setShadows(ui.settings().entityShadows, sky.skyDarken(), session ? session->dimension().ambientLight : 0.0f);
        if (haveEntities && session) {  // the day timeline's creaking_active / firefly_bush_sounds stretch: 12600..23400
            const std::string& dim = session->dimension().name;
            const i64 t = i64(sky.dayTicks()) % 24000;
            visuals.setAmbientEnv(dim == "minecraft:overworld" && t >= 12600 && t < 23401, dim == "minecraft:the_nether", sky.skyDarken());
        }
        if (haveEntities)
            entityRenderer.setViewScale(std::clamp(float(ui.settings().renderDistance) / 8.0f, 1.0f, 2.5f) *
                                        float(ui.settings().entityDistanceScaling));
        {  // Video Settings the meshes depend on (Smooth Lighting, See-Through Leaves, Biome Blend): re-mesh on change
            const ui::Settings& vs = ui.settings();
            const MeshOptions mo{vs.ao, vs.cutoutLeaves};
            terrain.setChunkUpdates(vs.prioritizeChunkUpdates);  // Chunk Builder (PrioritizeChunkUpdates)
            static int blendRadius = -1;
            if (!(mo == meshOptions()) || vs.biomeBlendRadius != blendRadius) {
                const bool first = blendRadius < 0;
                setMeshOptions(mo);
                setBiomeBlendRadius(vs.biomeBlendRadius);
                blendRadius = vs.biomeBlendRadius;
                if (!first || !(mo == MeshOptions{}) || blendRadius != 2) terrain.rebuildAll();
            }
        }
        sky.setTickRate(game.tickRate.runs ? 1.0 / game.tickRate.tickSeconds() : 0.0);
        sky.setWorldBorder(session ? &game.border : nullptr, game.partialTick(),
                           float(std::min(ui.settings().renderDistance, opt.detail) * 16));
        sky.setViewEffects(ui.viewEffects());  // blindness, darkness, night vision, water vision (fog, lightmap)
        renderer.setFadeSeconds(float(ui.settings().chunkFade));  // Video Settings: Chunk Fade (chunkSectionFadeInTime)
        sky.update(cam, dt);
        ui.endFrame();
        if (haveReports) packs::updatePlayer(renderer, ui, game.selectedSlot(), reports, registry);  // held light
        renderer.upscalers().setMenuActive(ui.menuOpen());  // frame generation pauses in menus
        ui.setServerSaving(localServer && localServer->saving());
        {  // FramerateLimitTracker.getThrottleReason: minimized 10, AFK (the Inactivity FPS Limit option) 30 after a
           // minute and 10 after ten, menus without a level 60
            const u64 afkMs = GetTickCount64() - window.lastInputMs();
            const u32 limit = renderer.upscalers().settings().fpsLimit;
            u32 throttle = 0;
            if (window.iconified()) throttle = 10;
            else if (ui.settings().inactivityFpsLimit == 1 && afkMs > 600000) throttle = 10;
            else if (ui.settings().inactivityFpsLimit == 1 && afkMs > 60000) throttle = limit ? std::min(limit, 30u) : 30;
            else if (menu && !session) throttle = 60;
            if (opt.bench || !opt.script.empty()) throttle = 0;  // automated runs keep their pace
            renderer.upscalers().setThrottle(throttle);
        }
        if (!opt.bench) cam.fovY = ui.fovRadians();
        if (!opt.bench && !menu) ui.warpCamera(cam);  // nausea / nether portal
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
            // The saving JVM should not compete with queues of terrain work for a world we
            // are leaving. Executing jobs finish; the idle UI never dispatches new ones.
            terrain.stopBackground();
            if (genService) genService->stop();
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
        stopping.get();
        if (open) {
            ui.startTitle();
            while (window.pump() && !ui.quitRequested()) idleFrame();
        }
    }
    terrain.stopBackground();
    if (genService) genService->stop();
    saveNow("exit");
    social::Friends::get().stop();  // goodbye to friends; closes tunnels (a guest's connection ends here)
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
