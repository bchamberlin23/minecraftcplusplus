// The F3 debug screen: DebugScreenOverlay with its configurable entries (DebugScreenEntries, DebugScreenEntryList:
// debug-profile.json, the default / performance profiles), the Debug Options screen (F3 + F6), the F3 key
// combinations of KeyboardHandler.handleDebugKeys, and the debug renderers the entries switch on (chunk borders,
// entity hitboxes).
#include "assets/assets.h"
#include "assets/game_reports.h"
#include "audio/audio.h"
#include "entity/entities.h"
#include "game/game.h"
#include "net/client.h"
#include "net/session.h"
#include "render/renderer.h"
#include "ui/debug_screen.h"
#include "ui/frontend.h"
#include "ui/modules.h"
#include "ui/options_list.h"
#include "ui/sprites.h"
#include "ui/ui_state.h"
#include "world/terrain.h"
#include "world/world.h"

#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#include <json/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <shared_mutex>
#include <thread>

namespace ui {

void debugFeedback(UiState& s, const char* key);  // bed_gamemode.cpp

namespace {

enum class Status : u8 { Never, InOverlay, AlwaysOn };
const char* const kStatusNames[3] = {"never", "inOverlay", "alwaysOn"};

// DebugScreenDisplayer: priority lines balance the two columns, regular lines split in half, groups go whole to one
// side.
struct Displayer {
    std::vector<std::string> left, right, regular;
    std::vector<std::pair<std::string, std::vector<std::string>>> groups;
    void priority(std::string line) { (left.size() <= right.size() ? left : right).push_back(std::move(line)); }
    void line(std::string l) { regular.push_back(std::move(l)); }
    void group(const char* id, std::vector<std::string> lines) {
        for (auto& [gid, gl] : groups)
            if (gid == id) {
                gl.insert(gl.end(), lines.begin(), lines.end());
                return;
            }
        groups.push_back({id, std::move(lines)});
    }
};

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list a;
    va_start(a, f);
    std::vsnprintf(buf, sizeof(buf), f, a);
    va_end(a);
    return buf;
}

using DisplayFn = void (*)(UiState&, Displayer&);
struct EntryDef {
    const char* id;  // minecraft:<id>
    bool renderer;   // DebugEntryCategory.RENDERER (no text)
    bool reducedOk;  // isAllowed with reduced debug info
    Status def, perf;
    DisplayFn display;
};

// --- entries -------------------------------------------------------------------------------------------------------

const Player& player(UiState& s) { return s.game->player(); }
int blockX(UiState& s) { return int(std::floor(player(s).pos[0])); }
int blockY(UiState& s) { return int(std::floor(player(s).pos[1])); }
int blockZ(UiState& s) { return int(std::floor(player(s).pos[2])); }

void eVersion(UiState& s, Displayer& d) { d.priority("Minecraft 26.3 (26.3/minecraftCplusplus)"); (void)s; }

void eFps(UiState& s, Displayer& d) {  // DebugEntryFps: fps, the limit (or inf), the present mode, the refresh rate
    const u32 limit = s.settings.video.fpsLimit;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    const int hz = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? int(dm.dmDisplayFrequency) : 0;
    d.priority(fmt("%d fps T: %s%s @%dHz", int(s.fps + 0.5), limit == 0 || limit >= 260 ? "inf" : std::to_string(limit).c_str(),
                   s.settings.video.vsync ? " (fifo)" : " (immediate)", hz));
}

struct Rates {  // packets a second, over the last second
    double tx = 0, rx = 0, at = 0;
    u64 out = 0, in = 0;
} g_rates;

void eTps(UiState& s, Displayer& d) {  // DebugEntryTps (a dedicated server, ours included)
    if (!s.client || !s.session) return;
    const net::Client::Traffic t = s.client->traffic();
    if (s.clock - g_rates.at >= 1.0) {
        const double dt = g_rates.at > 0 ? s.clock - g_rates.at : 1.0;
        g_rates.tx = double(t.packetsOut - g_rates.out) / dt, g_rates.rx = double(t.packetsIn - g_rates.in) / dt;
        g_rates.out = t.packetsOut, g_rates.in = t.packetsIn, g_rates.at = s.clock;
    }
    const TickRate& tr = s.entities ? s.entities->tickRate : TickRate{};
    const char* status = tr.frozen ? (tr.stepTicks > 0 ? " (frozen - stepping)" : " (frozen)") : "";
    std::string brand = s.client->serverBrand();
    if (brand.empty()) brand = "vanilla";
    d.line(fmt("\"%s\" server%s, %.0f tx, %.0f rx", brand.c_str(), status, g_rates.tx, g_rates.rx));
}

void eMemory(UiState& s, Displayer& d) {  // DebugEntryMemory, with the process' memory instead of the Java heap
    (void)s;
    MEMORYSTATUSEX mem{sizeof(mem)};
    GlobalMemoryStatusEx(&mem);
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    static double lastAt = 0;
    static u64 lastBytes = 0;
    static double rate = 0;
    const double now = double(GetTickCount64()) / 1000.0;
    if (now - lastAt >= 0.5) {  // the allocation rate over half a second
        if (lastAt > 0 && pmc.PrivateUsage > lastBytes) rate = double(pmc.PrivateUsage - lastBytes) / (now - lastAt);
        else if (lastAt > 0) rate = 0;
        lastAt = now, lastBytes = pmc.PrivateUsage;
    }
    const u64 used = pmc.WorkingSetSize >> 20, total = mem.ullTotalPhys >> 20, committed = pmc.PrivateUsage >> 20;
    d.group("memory", {fmt("Mem: %2d%% %03llu/%03lluMiB", int(100 * used / std::max<u64>(total, 1)), (unsigned long long)used, (unsigned long long)total),
                       fmt("Allocation rate: %03dMiB/s", int(rate / (1 << 20))),
                       fmt("Allocated: %2d%% %03lluMiB", int(100 * committed / std::max<u64>(total, 1)), (unsigned long long)committed)});
}

void eDetailedMemory(UiState& s, Displayer& d) {  // DebugEntryDetailedMemory: the process' private / working set / peak
    (void)s;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    d.group("memory", {fmt("Memory (private): i=%03lluMiB u=%03lluMiB c=%03lluMiB m=%03lluMiB", (unsigned long long)(pmc.PrivateUsage >> 20),
                           (unsigned long long)(pmc.WorkingSetSize >> 20), (unsigned long long)(pmc.PagefileUsage >> 20),
                           (unsigned long long)(pmc.PeakWorkingSetSize >> 20))});
}

std::string cpuName() {
    static std::string name;
    if (!name.empty()) return name;
    int r[4];
    char brand[49] = {};
    __cpuid(r, int(0x80000000));
    if (unsigned(r[0]) >= 0x80000004)
        for (int i = 0; i < 3; ++i) __cpuid(reinterpret_cast<int*>(brand + 16 * i), int(0x80000002 + i));
    std::string b = brand;
    while (!b.empty() && b.back() == ' ') b.pop_back();
    while (!b.empty() && b.front() == ' ') b.erase(b.begin());
    name = fmt("%ux %s", std::thread::hardware_concurrency(), b.c_str());
    return name;
}

void eSystem(UiState& s, Displayer& d) {  // DebugEntrySystemSpecs
    d.group("system", {fmt("C++: MSVC %d 64bit", _MSC_VER), fmt("CPU: %s", cpuName().c_str()),
                       fmt("Display: %ux%u (%s)", s.renderer->width(), s.renderer->height(), s.renderer->adapterName().c_str()),
                       fmt("Window: %ux%u (%.2fx pixel density)", s.window->width(), s.window->height(), 1.0),
                       s.renderer->adapterName(), "Direct3D 12"});
}

// The crosshair's targets within 20 blocks (DebugEntryLookingAt: Minecraft.getCameraEntity().pick(20)).
void lookDir(UiState& s, double eye[3], double dir[3]) {
    const CameraState c = s.game->camera();
    eye[0] = c.x, eye[1] = c.y, eye[2] = c.z;
    const double cp = std::cos(c.pitch);
    dir[0] = -std::sin(c.yaw) * cp, dir[1] = -std::sin(c.pitch), dir[2] = std::cos(c.yaw) * cp;
}

void stateLines(UiState& s, const std::string& state, const char* title, int x, int y, int z, std::vector<std::string>& out) {
    out.push_back(fmt("\xC2\xA7n%s: %d, %d, %d", title, x, y, z));  // ChatFormatting.UNDERLINE
    const size_t b = state.find('[');
    out.push_back(state.substr(0, b));
    if (b == std::string::npos) return;
    const std::string props = state.substr(b + 1, state.size() - b - 2);
    for (size_t start = 0; start < props.size();) {
        size_t comma = props.find(',', start);
        if (comma == std::string::npos) comma = props.size();
        const std::string kv = props.substr(start, comma - start);
        const size_t eq = kv.find('=');
        std::string value = kv.substr(eq + 1);
        if (value == "true") value = "\xC2\xA7" "atrue";  // GREEN
        else if (value == "false") value = "\xC2\xA7" "cfalse";  // RED
        out.push_back(kv.substr(0, eq) + ": " + value);
        start = comma + 1;
    }
    (void)s;
}

bool blockTarget(UiState& s, int& x, int& y, int& z) {
    double eye[3], dir[3];
    lookDir(s, eye, dir);
    const RayHit h = s.game->physics().raycast(eye, dir, 20.0);
    if (!h.hit) return false;
    x = h.x, y = h.y, z = h.z;
    return true;
}

bool fluidTarget(UiState& s, int& x, int& y, int& z) {  // ClipContext.Fluid.ANY: the first fluid cell along the look
    double eye[3], dir[3];
    lookDir(s, eye, dir);
    std::shared_lock lock(s.world->mutex());
    for (double t = 0; t <= 20.0; t += 0.05) {
        const int bx = int(std::floor(eye[0] + dir[0] * t)), by = int(std::floor(eye[1] + dir[1] * t)), bz = int(std::floor(eye[2] + dir[2] * t));
        const u16 id = s.world->block(bx, by, bz);
        const BlockInfo& b = s.registry->infos()[id];
        if (b.fluid || b.waterlogged) {
            x = bx, y = by, z = bz;
            return true;
        }
        if (b.collisionCount && b.kind == KindCube) return false;  // a solid block first
    }
    return false;
}

std::string fluidState(UiState& s, int x, int y, int z) {
    std::shared_lock lock(s.world->mutex());
    const u16 id = s.world->block(x, y, z);
    const std::string& n = s.registry->name(id);
    if (s.registry->infos()[id].waterlogged) return "minecraft:water[falling=false]";
    const bool lava = n.starts_with("minecraft:lava");
    const std::string_view lv = s.registry->property(id, "level");
    const int level = lv.empty() ? 0 : std::atoi(std::string(lv).c_str());
    if (level == 0) return std::string(lava ? "minecraft:lava" : "minecraft:water") + "[falling=false]";
    // FlowingFluid: level 8+ falls; amount 8 - level otherwise
    return std::string(lava ? "minecraft:flowing_lava" : "minecraft:flowing_water") + (level >= 8 ? "[falling=true, level=8]" : fmt("[falling=false, level=%d]", 8 - level));
}

std::vector<std::string> tagLines(UiState& s, const char* registry, const std::string& name) {
    std::vector<std::string> out;
    if (!s.client || !s.reports) return out;
    const nlohmann::json& reg = s.reports->registries();
    if (!reg.contains(registry) || !reg[registry].contains("entries") || !reg[registry]["entries"].contains(name)) return out;
    const int id = reg[registry]["entries"][name].value("protocol_id", -1);
    const auto tags = s.client->tags();
    auto it = tags.find(registry);
    if (id < 0 || it == tags.end()) return out;
    std::vector<std::string> names;
    for (const auto& [tag, ids] : it->second)
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) names.push_back("#" + (tag.find(':') == std::string::npos ? "minecraft:" + tag : tag));
    std::sort(names.begin(), names.end());
    return names;
}

void eBlockState(UiState& s, Displayer& d) {
    int x, y, z;
    if (!blockTarget(s, x, y, z)) return;
    std::string state;
    {
        std::shared_lock lock(s.world->mutex());
        state = s.registry->name(s.world->block(x, y, z));
    }
    std::vector<std::string> lines;
    stateLines(s, state, "Targeted Block", x, y, z, lines);
    d.group("looking_at_block", lines);
}
void eBlockTags(UiState& s, Displayer& d) {
    int x, y, z;
    if (!blockTarget(s, x, y, z)) return;
    std::string state;
    {
        std::shared_lock lock(s.world->mutex());
        state = s.registry->name(s.world->block(x, y, z));
    }
    d.group("looking_at_block", tagLines(s, "minecraft:block", state.substr(0, state.find('['))));
}
void eFluidState(UiState& s, Displayer& d) {
    int x, y, z;
    if (!fluidTarget(s, x, y, z)) return;
    std::vector<std::string> lines;
    stateLines(s, fluidState(s, x, y, z), "Targeted Fluid", x, y, z, lines);
    d.group("looking_at_fluid", lines);
}
void eFluidTags(UiState& s, Displayer& d) {
    int x, y, z;
    if (!fluidTarget(s, x, y, z)) return;
    const std::string st = fluidState(s, x, y, z);
    d.group("looking_at_fluid", tagLines(s, "minecraft:fluid", st.substr(0, st.find('['))));
}
const ent::Entity* entityTarget(UiState& s) {
    if (!s.entities) return nullptr;
    double eye[3], dir[3];
    lookDir(s, eye, dir);
    const RayHit h = s.game->physics().raycast(eye, dir, 20.0);
    const ent::PickResult p = s.entities->pick(eye, dir, h.hit ? h.distance : 20.0);
    return p.id >= 0 ? s.entities->entity(p.id) : nullptr;
}
void eEntity(UiState& s, Displayer& d) {
    if (const ent::Entity* e = entityTarget(s); e && e->info) d.group("looking_at_entity", {"\xC2\xA7nTargeted Entity", e->info->name});
}
void eEntityTags(UiState& s, Displayer& d) {
    if (const ent::Entity* e = entityTarget(s); e && e->info) d.group("looking_at_entity", tagLines(s, "minecraft:entity_type", e->info->name));
}

void eChunkRender(UiState& s, Displayer& d) {  // levelExtractor.sectionStatistics: the engine's terrain numbers
    const RenderStats& rs = s.renderer->stats();
    const TerrainStats& ts = s.terrain->stats();
    d.line(fmt("C: %zu/%zu sections, %zu nodes, %u draws, GPU %.2f ms", ts.detailResident, ts.detailPending, ts.active,
               rs.commands[0] + rs.commands[1] + rs.commands[2] + rs.commands[3], rs.gpuMs));
    d.line(fmt("F: %zu/%zu far regions, %.0f/%.0f MB", ts.farRegions, ts.farPending, double(s.renderer->arenaBytesUsed()) / (1 << 20),
               double(s.renderer->arenaBytesTotal()) / (1 << 20)));
}
void eChunkSource(UiState& s, Displayer& d) {  // ClientChunkCache.gatherStats
    if (!s.world) return;
    std::shared_lock lock(s.world->mutex());
    d.line(fmt("Client Chunk Cache: %zu, %zu", s.world->regions().size(), s.world->blockEntityChunks()));
}
void eEngine(UiState& s, Displayer& d, const char* what) {
    if (!s.engineInfo) return;
    const std::string v = s.engineInfo(what);
    if (!v.empty()) d.line(v);
}
void eEntityRender(UiState& s, Displayer& d) { eEngine(s, d, "entities"); }
void eParticles(UiState& s, Displayer& d) { eEngine(s, d, "particles"); }
void eSoundMood(UiState& s, Displayer& d) { eEngine(s, d, "sound_mood"); }
void eSoundCache(UiState& s, Displayer& d) { eEngine(s, d, "sound_cache"); }
void ePostEffects(UiState& s, Displayer& d) { eEngine(s, d, "post_effects"); }

const char* const kDirNames[4] = {"south", "west", "north", "east"};
const char* const kTowards[4] = {"Towards positive Z", "Towards negative X", "Towards negative Z", "Towards positive X"};
float wrapDeg(float v) {
    v = std::fmod(v, 360.0f);
    if (v >= 180) v -= 360;
    if (v < -180) v += 360;
    return v;
}

void ePosition(UiState& s, Displayer& d) {  // DebugEntryPosition
    const Player& p = player(s);
    const int bx = blockX(s), by = blockY(s), bz = blockZ(s);
    const float yaw = wrapDeg(p.yaw * 180.0f / 3.14159265f), pitch = wrapDeg(p.pitch * 180.0f / 3.14159265f);
    const int dir = int(std::floor(yaw / 90.0f + 0.5f)) & 3;
    d.group("position", {fmt("XYZ: %.3f / %.5f / %.3f", p.pos[0], p.pos[1], p.pos[2]), fmt("Block: %d %d %d", bx, by, bz),
                         fmt("Chunk: %d %d %d [%d %d in r.%d.%d.mca]", bx >> 4, by >> 4, bz >> 4, (bx >> 4) & 31, (bz >> 4) & 31, bx >> 9, bz >> 9),
                         fmt("Facing: %s (%s) (%.1f / %.1f)", kDirNames[dir], kTowards[dir], yaw, pitch)});
}
void eSectionPosition(UiState& s, Displayer& d) {
    d.group("position", {fmt("Section-relative: %02d %02d %02d", blockX(s) & 15, blockY(s) & 15, blockZ(s) & 15)});
}
void eSpeed(UiState& s, Displayer& d) {
    const Player& p = player(s);
    d.group("position", {fmt("Speed: %.3f blocks/tick", std::sqrt(p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1] + p.vel[2] * p.vel[2]))});
}
void eLight(UiState& s, Displayer& d) {  // DebugEntryLight: at the feet, raw brightness without the sky darkening
    const int bx = blockX(s), by = blockY(s), bz = blockZ(s);
    std::shared_lock lock(s.world->mutex());
    const u8 l = s.world->light(bx >> 4, by >> 4, bz >> 4).at(bx & 15, by & 15, bz & 15);
    const int sky = l >> 4, block = l & 15;
    d.group("light", {fmt("Client Light: %d (%d sky, %d block)", std::max(sky, block), sky, block)});
}
void eHeightmap(UiState& s, Displayer& d) {  // DebugEntryHeightmap: the client's heightmaps, the server's unknown
    const int bx = blockX(s), bz = blockZ(s);
    int S = INT32_MIN, M = INT32_MIN, ML = INT32_MIN;
    {
        std::shared_lock lock(s.world->mutex());
        for (int y = (s.world->maxSectionY() + 1) * 16 - 1; y >= s.world->minSectionY() * 16; --y) {
            const u16 id = s.world->block(bx, y, bz);
            if (!id) continue;
            const BlockInfo& b = s.registry->infos()[id];
            const bool blocking = b.collisionCount > 0 || b.fluid || b.waterlogged;  // Heightmap.Types.MOTION_BLOCKING
            if (S == INT32_MIN) S = y + 1;
            if (M == INT32_MIN && blocking) M = y + 1;
            if (ML == INT32_MIN && blocking && s.registry->name(id).find("leaves") == std::string::npos) ML = y + 1;
            if (ML != INT32_MIN) break;
        }
    }
    const int minY = s.world->minSectionY() * 16;
    auto h = [&](int v) { return v == INT32_MIN ? minY : v; };
    d.group("heightmaps", {fmt("CH S: %d M: %d ML: %d", h(S), h(M), h(ML)), "SH S: ?? O: ?? M: ?? ML: ??"});
}
void eBiome(UiState& s, Displayer& d) {
    if (s.biomeLookup) d.line("Biome: " + s.biomeLookup(blockX(s), blockY(s), blockZ(s)));
}
void eDayCount(UiState& s, Displayer& d) { eEngine(s, d, "day_count"); }
void eGpu(UiState& s, Displayer& d) {  // DebugEntryGpuUtilization: the GPU's share of the frame
    const double u = s.frameMs > 0 ? s.renderer->stats().gpuMs / s.frameMs * 100.0 : 0.0;
    d.line(u > 100 ? "GPU: \xC2\xA7" "c100%" : fmt("GPU: %d%%", int(std::round(u))));
}
void eImpactors(UiState& s, Displayer& d) {  // DebugEntrySimplePerformanceImpactors
    d.line(fmt("%sfancy-clouds B: %d", s.settings.improvedTransparency ? "improved-transparency " : "", 2));
    const u32 mode = s.settings.textureFiltering;
    if (mode == 2) d.line(fmt("Filtering: anisotropic %dx", 1 << std::clamp(s.settings.maxAnisotropyBit, 1, 3)));
    else d.line(mode == 1 ? "Filtering: rgss" : "Filtering: none");
    d.line("Terrain Rendering: multidrawindirect");
}

const EntryDef kEntries[] = {
    {"game_version", false, true, Status::InOverlay, Status::Never, eVersion},
    {"fps", false, true, Status::InOverlay, Status::AlwaysOn, eFps},
    {"tps", false, true, Status::InOverlay, Status::InOverlay, eTps},
    {"memory", false, true, Status::InOverlay, Status::InOverlay, eMemory},
    {"detailed_memory", false, true, Status::Never, Status::Never, eDetailedMemory},
    {"system_specs", false, true, Status::InOverlay, Status::Never, eSystem},
    {"looking_at_block_state", false, false, Status::Never, Status::Never, eBlockState},
    {"looking_at_block_tags", false, false, Status::Never, Status::Never, eBlockTags},
    {"looking_at_fluid_state", false, false, Status::Never, Status::Never, eFluidState},
    {"looking_at_fluid_tags", false, false, Status::Never, Status::Never, eFluidTags},
    {"looking_at_entity", false, false, Status::Never, Status::Never, eEntity},
    {"looking_at_entity_tags", false, false, Status::Never, Status::Never, eEntityTags},
    {"chunk_render_stats", false, true, Status::Never, Status::Never, eChunkRender},
    {"chunk_generation_stats", false, false, Status::Never, Status::Never, nullptr},  // the integrated server's
    {"entity_render_stats", false, true, Status::Never, Status::Never, eEntityRender},
    {"particle_render_stats", false, false, Status::Never, Status::Never, eParticles},
    {"chunk_source_stats", false, true, Status::Never, Status::Never, eChunkSource},
    {"player_position", false, false, Status::InOverlay, Status::Never, ePosition},
    {"player_section_position", false, true, Status::InOverlay, Status::Never, eSectionPosition},
    {"player_speed", false, false, Status::Never, Status::Never, eSpeed},
    {"light_levels", false, false, Status::Never, Status::Never, eLight},
    {"heightmap", false, false, Status::Never, Status::Never, eHeightmap},
    {"biome", false, false, Status::Never, Status::Never, eBiome},
    {"local_difficulty", false, false, Status::Never, Status::Never, nullptr},  // the integrated server's
    {"day_count", false, false, Status::Never, Status::Never, eDayCount},
    {"entity_spawn_counts", false, false, Status::Never, Status::Never, nullptr},  // the integrated server's
    {"sound_mood", false, false, Status::Never, Status::Never, eSoundMood},
    {"sound_cache", false, true, Status::Never, Status::Never, eSoundCache},
    {"post_effects", false, false, Status::Never, Status::Never, ePostEffects},
    {"gpu_utilization", false, true, Status::Never, Status::InOverlay, eGpu},
    {"simple_performance_impactors", false, true, Status::InOverlay, Status::InOverlay, eImpactors},
    {"3d_crosshair", true, false, Status::InOverlay, Status::Never, nullptr},
    {"entity_hitboxes", true, false, Status::Never, Status::Never, nullptr},
    {"chunk_borders", true, false, Status::Never, Status::Never, nullptr},
    {"chunk_section_paths", true, false, Status::Never, Status::Never, nullptr},
    {"chunk_section_octree", true, false, Status::Never, Status::Never, nullptr},
    {"visualize_water_levels", true, false, Status::Never, Status::Never, nullptr},
    {"visualize_heightmap", true, false, Status::Never, Status::Never, nullptr},
    {"visualize_collision_boxes", true, false, Status::Never, Status::Never, nullptr},
    {"visualize_entity_supporting_blocks", true, false, Status::Never, Status::Never, nullptr},
    {"visualize_block_light_levels", true, false, Status::Never, Status::Never, nullptr},
    {"visualize_sky_light_levels", true, false, Status::Never, Status::Never, nullptr},
    {"visualize_solid_faces", true, false, Status::Never, Status::Never, nullptr},
    {"visualize_chunks_on_server", true, false, Status::Never, Status::Never, nullptr},
    {"visualize_sky_light_sections", true, false, Status::Never, Status::Never, nullptr},
    {"chunk_section_visibility", true, false, Status::Never, Status::Never, nullptr},
};
constexpr size_t kCount = std::size(kEntries);

// --- the entry list (DebugScreenEntryList) ---------------------------------------------------------------------

struct List {
    bool loaded = false;
    int profile = 0;  // 0 default, 1 performance, -1 custom
    Status status[kCount] = {};
    bool fpsCharts = false, networkCharts = false, profilerChart = false, lightmap = false;  // DebugScreenOverlay
} g;

fs::path profilePath() {
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    return fs::path(local ? local : L".") / L"minecraftCplusplus" / L"debug-profile.json";
}

void resetToProfile(int profile) {
    g.profile = profile;
    for (size_t i = 0; i < kCount; ++i) g.status[i] = profile == 1 ? kEntries[i].perf : kEntries[i].def;
}

void save() {
    nlohmann::json j;
    j["DataVersion"] = 4649;
    if (g.profile >= 0) {
        j["profile"] = g.profile == 1 ? "performance" : "default";
    } else {
        nlohmann::json custom = nlohmann::json::object();
        for (size_t i = 0; i < kCount; ++i)
            if (g.status[i] != Status::Never) custom[std::string("minecraft:") + kEntries[i].id] = kStatusNames[int(g.status[i])];
        j["custom"] = custom;
    }
    std::error_code ec;
    fs::create_directories(profilePath().parent_path(), ec);
    std::ofstream(profilePath()) << j.dump(2);
}

void load() {  // DebugScreenEntryList.load: no file -> the default profile (not saved)
    if (g.loaded) return;
    g.loaded = true;
    resetToProfile(0);
    std::ifstream in(profilePath());
    if (!in) return;
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) {
        save();
        return;
    }
    if (j.contains("profile") && j["profile"].is_string()) {
        resetToProfile(j["profile"].get<std::string>() == "performance" ? 1 : 0);
    } else if (j.contains("custom") && j["custom"].is_object()) {
        g.profile = -1;
        for (size_t i = 0; i < kCount; ++i) {
            g.status[i] = Status::Never;
            const std::string key = std::string("minecraft:") + kEntries[i].id;
            if (!j["custom"].contains(key) || !j["custom"][key].is_string()) continue;
            const std::string v = j["custom"][key].get<std::string>();
            for (int k = 0; k < 3; ++k)
                if (v == kStatusNames[k]) g.status[i] = Status(k);
        }
    }
}

int indexOf(std::string_view id) {
    for (size_t i = 0; i < kCount; ++i)
        if (id == kEntries[i].id) return int(i);
    return -1;
}

bool reduced(UiState& s) { return s.reducedDebugInfo || s.settings.reducedDebugInfo; }  // Minecraft.showOnlyReducedInfo

bool enabled(UiState& s, size_t i) {  // rebuildCurrentList
    load();
    const Status st = g.status[i];
    const bool on = st == Status::AlwaysOn || (s.debug && st == Status::InOverlay);
    return on && (kEntries[i].reducedOk || !reduced(s));
}

void setStatus(size_t i, Status st) {
    g.status[i] = st;
    g.profile = -1;
    save();
}

// DebugScreenEntryList.toggleStatus (the F3 + key toggles): always -> never; else on in the overlay when it shows.
bool toggle(UiState& s, const char* id) {
    load();
    const int i = indexOf(id);
    if (i < 0) return false;
    const Status st = g.status[i];
    const Status next = st == Status::AlwaysOn ? Status::Never : st == Status::InOverlay ? (s.debug ? Status::Never : Status::AlwaysOn)
                                                                                        : (s.debug ? Status::InOverlay : Status::AlwaysOn);
    setStatus(size_t(i), next);
    return next != Status::Never;
}

// --- the overlay -----------------------------------------------------------------------------------------------

int calcScale(UiState& s, int requested) {  // Window.calculateScale
    const u32 w = s.window->width(), h = s.window->height();
    int scale = 1;
    while (scale != requested && u32(scale) < w && u32(scale) < h && w / u32(scale + 1) >= 320 && h / u32(scale + 1) >= 240) ++scale;
    if (s.settings.forceUnicodeFont && scale % 2 != 0) ++scale;
    return scale;
}

std::string keyHint(UiState& s, Key k) {  // DebugScreenOverlay.formatKeybind
    const int mod = s.settings.key(Key::DebugModifier);
    return "[" + (mod ? s.keyLabel(mod).plain() + "+" : std::string()) + s.keyLabel(s.settings.key(k)).plain() + "]";
}

}  // namespace

bool debugEntryEnabled(UiState& s, std::string_view id) {
    const int i = indexOf(id);
    return i >= 0 && enabled(s, size_t(i));
}

void drawDebugScreen(UiState& s, Canvas& c) {
    if (s.hudHidden && s.screen == Screen::None) return;
    if (s.screen == Screen::DebugOptions) return;  // drawn behind its blur by the screen
    Displayer d;
    bool any = false;
    // Sorted by id like Identifier.compareTo (all minecraft:).
    static std::vector<size_t> order = [] {
        std::vector<size_t> o(kCount);
        for (size_t i = 0; i < kCount; ++i) o[i] = i;
        std::sort(o.begin(), o.end(), [](size_t a, size_t b) { return std::string_view(kEntries[a].id) < std::string_view(kEntries[b].id); });
        return o;
    }();
    for (size_t i : order) {
        if (!enabled(s, i)) continue;
        any = true;
        if (kEntries[i].display) kEntries[i].display(s, d);
    }
    if (!any) return;  // nothing enabled: not even the help
    std::vector<std::string> left = std::move(d.left), right = std::move(d.right);
    if (!left.empty()) left.emplace_back();
    if (!right.empty()) right.emplace_back();
    if (!d.regular.empty()) {
        const size_t mid = (d.regular.size() + 1) / 2;
        left.insert(left.end(), d.regular.begin(), d.regular.begin() + long(mid));
        left.emplace_back();
        if (mid < d.regular.size()) {
            right.insert(right.end(), d.regular.begin() + long(mid), d.regular.end());
            right.emplace_back();
        }
    }
    const size_t gmid = (d.groups.size() + 1) / 2;
    for (size_t gi = 0; gi < d.groups.size(); ++gi) {
        if (d.groups[gi].second.empty()) continue;
        std::vector<std::string>& side = gi < gmid ? left : right;
        side.insert(side.end(), d.groups[gi].second.begin(), d.groups[gi].second.end());
        side.emplace_back();
    }
    if (s.debug) {  // the help block
        auto chart = [&](Key k, const char* name, bool on) { return keyHint(s, k) + " " + name + " " + (on ? "visible" : "hidden"); };
        left.emplace_back();
        left.push_back("Debug charts: " + chart(Key::DebugProfilingChart, "Profiler", g.profilerChart) + "; " +
                       chart(Key::DebugFpsCharts, s.online() ? "fps + tps" : "fps", g.fpsCharts) + ";");
        left.push_back(chart(Key::DebugNetworkCharts, s.fe->cfg.singleplayer ? "Ping" : "Bandwidth + Ping", g.networkCharts) + "; " +
                       chart(Key::DebugLightmapTexture, "Lightmap", g.lightmap));
        left.push_back("To edit: press " + keyHint(s, Key::DebugDebugOptions));
    }
    // The debug GUI scale: -1 the game's, 0 half the largest, else that scale (only ever smaller).
    const int standard = c.scale();
    int scale = s.settings.debugGuiScale == -1 ? standard : s.settings.debugGuiScale == 0 ? calcScale(s, 0) / 2 : calcScale(s, s.settings.debugGuiScale);
    c.push();
    int width = c.guiWidth();
    if (scale > 0 && scale < standard) {
        c.scaleBy(float(scale) / float(standard));
        width = int(s.window->width()) / scale;
    }
    auto lines = [&](const std::vector<std::string>& v, bool alignLeft) {
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i].empty()) continue;
            const int w = s.font.width(v[i]);
            const float top = float(2 + 9 * int(i));
            const float x = alignLeft ? 2.0f : float(width - 2 - w);
            c.fill(x - 1, top - 1, x + float(w) + 1, top + 9 - 1, 0x90505050);
        }
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i].empty()) continue;
            const int w = s.font.width(v[i]);
            const float x = alignLeft ? 2.0f : float(width - 2 - w);
            s.font.draw(c, v[i], x, float(2 + 9 * int(i)), 0xFFE0E0E0, false);
        }
    };
    lines(left, true);
    lines(right, false);
    c.pop();
}

// --- the F3 key combinations (after bed_gamemode.cpp's) ---------------------------------------------------------

bool debugScreenKey(UiState& s, const InputEvent& e) {
    auto feedback = [&](const char* key) { debugFeedback(s, key); };
    if (s.isKey(Key::DebugReloadChunk, e)) {  // F3 + A
        if (s.terrain) s.terrain->rebuildAll();
        feedback("debug.reload_chunks.message");
        return true;
    }
    if (s.isKey(Key::DebugShowHitboxes, e)) {  // F3 + B
        if (s.online() && !reduced(s)) feedback(toggle(s, "entity_hitboxes") ? "debug.show_hitboxes.on" : "debug.show_hitboxes.off");
        return true;
    }
    if (s.isKey(Key::DebugShowChunkBorders, e)) {  // F3 + G
        if (s.online() && !reduced(s)) feedback(toggle(s, "chunk_borders") ? "debug.chunk_boundaries.on" : "debug.chunk_boundaries.off");
        return true;
    }
    if (s.isKey(Key::DebugCopyLocation, e)) {  // F3 + C: the tp command for here
        if (s.online() && !reduced(s) && s.session) {
            const Player& p = player(s);
            const std::string cmd = fmt("/execute in %s run tp @s %.2f %.2f %.2f %.2f %.2f", s.session->dimension().name.c_str(), p.pos[0], p.pos[1],
                                        p.pos[2], wrapDeg(p.yaw * 180.0f / 3.14159265f), wrapDeg(p.pitch * 180.0f / 3.14159265f));
            s.window->setClipboard(cmd);
            feedback("debug.copy_location.message");
        }
        return true;
    }
    if (s.isKey(Key::DebugDebugOptions, e)) {  // F3 + F6
        if (s.screen == Screen::DebugOptions) s.closeScreen();
        else if (s.screen == Screen::None) s.openScreen(Screen::DebugOptions);
        return true;
    }
    auto chartToggle = [&](bool& flag, bool exclusive) {  // DebugScreenOverlay.toggleFpsCharts and friends
        flag = !s.debug || !flag;
        if (flag) {
            s.debug = true;
            if (exclusive) {
                const bool keep = flag;
                g.fpsCharts = g.networkCharts = g.lightmap = false;
                flag = keep;
            }
        }
    };
    if (s.isKey(Key::DebugProfilingChart, e)) return chartToggle(g.profilerChart, false), true;  // F3 + 1
    if (s.isKey(Key::DebugFpsCharts, e)) return chartToggle(g.fpsCharts, true), true;            // F3 + 2
    if (s.isKey(Key::DebugNetworkCharts, e)) return chartToggle(g.networkCharts, true), true;    // F3 + 3
    if (s.isKey(Key::DebugLightmapTexture, e)) return chartToggle(g.lightmap, true), true;       // F3 + 4
    if (s.isKey(Key::DebugReloadResourcePacks, e)) {  // F3 + T
        feedback("debug.reload_resourcepacks.message");
        if (s.fe->cfg.reloadResources) s.fe->cfg.reloadResources();
        return true;
    }
    if (s.isKey(Key::DebugDumpVersion, e)) {  // F3 + V: VersionCommand.dumpVersion
        feedback("debug.version.header");
        for (const char* line : {"id = 26.3", "name = 26.3", "data = 5023", "series = main", "protocol = 777 (0x309)",
                                 "build_time = 2026", "pack_resource_major = 97", "pack_resource_minor = 1", "pack_data_major = 121",
                                 "pack_data_minor = 0", "stable = yes"})
            s.addChat(Text(line));
        return true;
    }
    if (s.isKey(Key::DebugImprovedTransparency, e)) {  // F3 + X
        s.settings.improvedTransparency = !s.settings.improvedTransparency;
        feedback(s.settings.improvedTransparency ? "debug.improvedTransparency.on" : "debug.improvedTransparency.off");
        s.settings.save();
        return true;
    }
    return false;
}

bool debugChart(int which) { return which == 0 ? g.fpsCharts : which == 1 ? g.networkCharts : which == 2 ? g.profilerChart : g.lightmap; }

// --- the debug renderers (DebugRenderer): chunk borders, entity hitboxes ------------------------------------------

void debugLines(UiState& s, std::vector<LineVertex>& out, const CameraState& cam, float screenW, float screenH) {
    if (!s.world || !s.game) return;
    auto line = [&](double ax, double ay, double az, double bx, double by, double bz, u32 argb, float width) {
        const u32 abgr = (argb & 0xFF00FF00u) | ((argb >> 16) & 0xFF) | ((argb & 0xFF) << 16);
        out.push_back({float(ax - cam.x), float(ay - cam.y), float(az - cam.z), abgr, width / screenW, width / screenH});
        out.push_back({float(bx - cam.x), float(by - cam.y), float(bz - cam.z), abgr, width / screenW, width / screenH});
    };
    auto box = [&](double x0, double y0, double z0, double x1, double y1, double z1, u32 c, float w) {
        line(x0, y0, z0, x1, y0, z0, c, w), line(x0, y1, z0, x1, y1, z0, c, w), line(x0, y0, z1, x1, y0, z1, c, w), line(x0, y1, z1, x1, y1, z1, c, w);
        line(x0, y0, z0, x0, y1, z0, c, w), line(x1, y0, z0, x1, y1, z0, c, w), line(x0, y0, z1, x0, y1, z1, c, w), line(x1, y0, z1, x1, y1, z1, c, w);
        line(x0, y0, z0, x0, y0, z1, c, w), line(x1, y0, z0, x1, y0, z1, c, w), line(x0, y1, z0, x0, y1, z1, c, w), line(x1, y1, z0, x1, y1, z1, c, w);
    };
    if (debugEntryEnabled(s, "chunk_borders")) {  // ChunkBorderRenderer around the camera's section
        const int cx = (int(std::floor(cam.x)) >> 4) << 4, cz = (int(std::floor(cam.z)) >> 4) << 4, cy = (int(std::floor(cam.y)) >> 4) << 4;
        const double ymin = s.world->minSectionY() * 16, ymax = (s.world->maxSectionY() + 1) * 16;
        const u32 cell = 0xFF009B9B, yellow = 0xFFFFFF00, major = 0xFF3F3FFF;
        for (int x = -16; x <= 32; x += 16)
            for (int z = -16; z <= 32; z += 16)
                if ((x == -16 || x == 32) || (z == -16 || z == 32)) line(cx + x, ymin, cz + z, cx + x, ymax, cz + z, 0x7FFF0000, 4.0f);
        for (int x = 2; x < 16; x += 2) {
            line(cx + x, ymin, cz, cx + x, ymax, cz, x % 4 == 0 ? cell : yellow, 1.0f);
            line(cx + x, ymin, cz + 16, cx + x, ymax, cz + 16, x % 4 == 0 ? cell : yellow, 1.0f);
            line(cx, ymin, cz + x, cx, ymax, cz + x, x % 4 == 0 ? cell : yellow, 1.0f);
            line(cx + 16, ymin, cz + x, cx + 16, ymax, cz + x, x % 4 == 0 ? cell : yellow, 1.0f);
        }
        for (int y = int(ymin); y <= int(ymax); y += 2) {
            const u32 c = y % 8 == 0 ? cell : yellow;
            line(cx, y, cz, cx + 16, y, cz, c, 1.0f), line(cx + 16, y, cz, cx + 16, y, cz + 16, c, 1.0f);
            line(cx + 16, y, cz + 16, cx, y, cz + 16, c, 1.0f), line(cx, y, cz + 16, cx, y, cz, c, 1.0f);
        }
        for (int x = 0; x <= 16; x += 16)
            for (int z = 0; z <= 16; z += 16) line(cx + x, ymin, cz + z, cx + x, ymax, cz + z, major, 4.0f);
        box(cx, cy, cz, cx + 16, cy + 16, cz + 16, major, 1.0f);
        for (int y = int(ymin); y <= int(ymax); y += 16) {
            line(cx, y, cz, cx + 16, y, cz, major, 4.0f), line(cx + 16, y, cz, cx + 16, y, cz + 16, major, 4.0f);
            line(cx + 16, y, cz + 16, cx, y, cz + 16, major, 4.0f), line(cx, y, cz + 16, cx, y, cz, major, 4.0f);
        }
    }
    if (debugEntryEnabled(s, "3d_crosshair") && s.game->cameraMode == 0 && !s.hudHidden) {  // DebugCrosshairRenderer
        // A block in front of the eye, the world's axes 0.01 x the GUI scale long: black under red / green / blue.
        const float cp = std::cos(cam.pitch);
        const double fx = -std::sin(cam.yaw) * cp, fy = -std::sin(cam.pitch), fz = std::cos(cam.yaw) * cp;
        const double ox = cam.x + fx, oy = cam.y + fy, oz = cam.z + fz, len = 0.01 * s.guiScale();
        line(ox, oy, oz, ox + len, oy, oz, 0xFF000000, 4.0f), line(ox, oy, oz, ox, oy + len, oz, 0xFF000000, 4.0f);
        line(ox, oy, oz, ox, oy, oz + len, 0xFF000000, 4.0f);
        line(ox, oy, oz, ox + len, oy, oz, 0xFFFF0000, 2.0f), line(ox, oy, oz, ox, oy + len, oz, 0xFF00FF00, 2.0f);
        line(ox, oy, oz, ox, oy, oz + len, 0xFF7F7FFF, 2.0f);
    }
    if (debugEntryEnabled(s, "visualize_collision_boxes")) {  // CollisionBoxRenderer: within 6 blocks of the player
        const AABB b = player(s).box();
        std::vector<AABB> boxes;
        {
            std::shared_lock lock(s.world->mutex());
            s.game->physics().collisionBoxes({{b.min[0] - 6, b.min[1] - 6, b.min[2] - 6}, {b.max[0] + 6, b.max[1] + 6, b.max[2] + 6}}, boxes);
        }
        for (const AABB& c : boxes) box(c.min[0], c.min[1], c.min[2], c.max[0], c.max[1], c.max[2], 0xFFFFFFFF, 2.5f);
    }
    if (debugEntryEnabled(s, "visualize_entity_supporting_blocks")) {  // SupportBlockRenderer: the player's supporting block
        const AABB b = player(s).box();
        int at[3];
        bool found;
        std::vector<AABB> shape;
        {
            std::shared_lock lock(s.world->mutex());
            found = s.game->physics().supportingBlock({{b.min[0], b.min[1] - 1.0e-6, b.min[2]}, {b.max[0], b.min[1], b.max[2]}}, player(s).pos, at);
            if (found) s.game->physics().collisionBoxes({{double(at[0]), double(at[1]), double(at[2])}, {at[0] + 1.0, at[1] + 1.0, at[2] + 1.0}}, shape);
        }
        if (found) {
            const double o = 0.02;
            box(at[0] - o, at[1] - o, at[2] - o, at[0] + 1 + o, at[1] + 1 + o, at[2] + 1 + o, 0x66FF0000, 2.5f);
            for (const AABB& c : shape) box(c.min[0], c.min[1], c.min[2], c.max[0], c.max[1], c.max[2], 0xFFFF0000, 2.5f);
        }
    }
    if (debugEntryEnabled(s, "entity_hitboxes") && s.entities) {  // EntityHitboxDebugRenderer
        const float pt = s.entities->partialTick();
        for (const auto& [id, e] : s.entities->entities()) {
            if (!e.info || (e.sharedFlags & 0x20) || id == s.entities->viewEntity) continue;  // invisible / the camera's
            double w, h;
            ent::entitySize(e, w, h);
            double p[3];
            for (int a = 0; a < 3; ++a) p[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt;
            const double dx = p[0] - cam.x, dz = p[2] - cam.z;
            if (dx * dx + dz * dz > 64.0 * 64.0) continue;
            box(p[0] - w / 2, p[1], p[2] - w / 2, p[0] + w / 2, p[1] + h, p[2] + w / 2, 0xFFFFFFFF, 2.5f);
            if (e.category == ent::Category::Living || e.category == ent::Category::Player) {
                const double eye = p[1] + e.info->eye * (h / std::max(0.01, double(e.info->height)));
                box(p[0] - w / 2, eye - 0.01, p[2] - w / 2, p[0] + w / 2, eye + 0.01, p[2] + w / 2, 0xFFFF0000, 2.5f);
                const float yaw = (e.prevHeadYaw + (e.headYaw - e.prevHeadYaw) * pt) * 3.14159265f / 180.0f;
                const float pitch = (e.prevPitch + (e.pitch - e.prevPitch) * pt) * 3.14159265f / 180.0f;
                const double vx = -std::sin(yaw) * std::cos(pitch), vy = -std::sin(pitch), vz = std::cos(yaw) * std::cos(pitch);
                line(p[0], eye, p[2], p[0] + vx * 2, eye + vy * 2, p[2] + vz * 2, 0xFF0000FF, 2.5f);  // the view vector
            }
        }
    }
}

// --- the debug renderers drawn over the view (projected): water levels, light levels, solid faces, heightmaps --------

namespace {

void drawWorldDebug(UiState& s, Canvas& c) {
    const bool water = debugEntryEnabled(s, "visualize_water_levels"), blockLight = debugEntryEnabled(s, "visualize_block_light_levels"),
               skyLight = debugEntryEnabled(s, "visualize_sky_light_levels"), solid = debugEntryEnabled(s, "visualize_solid_faces"),
               heights = debugEntryEnabled(s, "visualize_heightmap");
    if (!(water || blockLight || skyLight || solid || heights) || !s.game || !s.world) return;
    const CameraState cam = s.game->viewCamera();
    const float cp = std::cos(cam.pitch), sp = std::sin(cam.pitch), cy = std::cos(cam.yaw), sy = std::sin(cam.yaw);
    float fwd[3] = {-sy * cp, -sp, cy * cp}, right[3] = {-cy, 0, -sy}, up[3] = {-sy * sp, cp, cy * sp};
    if (cam.basisSet)
        for (int a = 0; a < 3; ++a) fwd[a] = cam.fwd[a], right[a] = cam.right[a], up[a] = cam.up[a];
    const float W = float(c.guiWidth()), H = float(c.guiHeight());
    const float t = std::tan(s.settings.fov * (s.oldFovModifier + (s.fovModifier - s.oldFovModifier) * s.partialTick) * 3.14159265f / 360.0f);
    auto project = [&](double x, double y, double z, float out[2]) {
        const float d[3] = {float(x - cam.x), float(y - cam.y), float(z - cam.z)};
        const float f = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        if (f < 0.05f) return false;
        out[0] = W * 0.5f * (1.0f + (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / (f * t * W / H));
        out[1] = H * 0.5f * (1.0f - (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / (f * t));
        return true;
    };
    const float uv[4][2] = {};
    auto face = [&](const double p[4][3], u32 argb) {
        float xy[4][2];
        for (int k = 0; k < 4; ++k)
            if (!project(p[k][0], p[k][1], p[k][2], xy[k])) return;
        c.quad4(xy, uv, argb, ModeSolid);
    };
    auto boxFill = [&](double x0, double y0, double z0, double x1, double y1, double z1, u32 argb) {
        const double f[6][4][3] = {{{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}}, {{x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}},
                                   {{x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}}, {{x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}},
                                   {{x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}}, {{x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}}};
        for (const auto& q : f) face(q, argb);
    };
    auto label = [&](const std::string& text, double x, double y, double z, u32 argb) {
        float at[2];
        if (!project(x, y, z, at)) return;
        s.font.draw(c, text, at[0] - float(s.font.width(text)) / 2, at[1] - 4, argb, false);
    };
    auto lerpColor = [](float f, u32 a, u32 b) {
        u32 out = 0xFF000000;
        for (int sh = 0; sh < 24; sh += 8) out |= u32(float((a >> sh) & 255) + (float((b >> sh) & 255) - float((a >> sh) & 255)) * f) << sh;
        return out;
    };
    const int px = int(std::floor(cam.x)), py = int(std::floor(cam.y)), pz = int(std::floor(cam.z));
    std::shared_lock lock(s.world->mutex());
    const int minY = s.world->minSectionY() * 16, maxY = (s.world->maxSectionY() + 1) * 16;
    if (water)  // WaterDebugRenderer: radius 10, the fluid height, the amount
        for (int x = px - 10; x <= px + 10; ++x)
            for (int y = std::max(minY, py - 10); y <= std::min(maxY - 1, py + 10); ++y)
                for (int z = pz - 10; z <= pz + 10; ++z) {
                    const u16 id = s.world->block(x, y, z);
                    const BlockInfo& b = s.registry->infos()[id];
                    const bool fluid = b.waterlogged || s.registry->name(id).starts_with("minecraft:water");
                    if (!fluid) continue;
                    const std::string_view lv = s.registry->property(id, "level");
                    const int level = lv.empty() ? 0 : std::atoi(std::string(lv).c_str());
                    const int amount = b.waterlogged || level == 0 || level >= 8 ? 8 : 8 - level;
                    const bool above = s.registry->name(s.world->block(x, y + 1, z)).starts_with("minecraft:water");
                    const double height = above ? 1.0 : double(amount) / 9.0;
                    boxFill(x + 0.01, y + 0.01, z + 0.01, x + 0.99, y + height, z + 0.99, 0x2600FF00);
                    label(std::to_string(amount), x + 0.5, y + height, z + 0.5, 0xFF000000);
                }
    if (blockLight || skyLight)  // LightDebugRenderer: radius 10
        for (int x = px - 10; x <= px + 10; ++x)
            for (int y = std::max(minY, py - 10); y <= std::min(maxY - 1, py + 10); ++y)
                for (int z = pz - 10; z <= pz + 10; ++z) {
                    const BlockInfo& b = s.registry->infos()[s.world->block(x, y, z)];
                    if (b.collisionCount && b.kind == KindCube) continue;
                    const u8 l = s.world->light(x >> 4, y >> 4, z >> 4).at(x & 15, y & 15, z & 15);
                    const int sky = l >> 4, block = l & 15;
                    if (skyLight && sky != 15) label(std::to_string(sky), x + 0.5, y + 0.25, z + 0.5, lerpColor(float(sky) / 15, 0xFF0000FF, 0xFF00FFFF));
                    if (blockLight && block) label(std::to_string(block), x + 0.5, y + 0.5, z + 0.5, lerpColor(float(block) / 15, 0xFFAA0000, 0xFFFFFF00));
                }
    if (solid)  // SolidFaceRenderer: the sturdy faces within 6 blocks
        for (int x = px - 6; x <= px + 6; ++x)
            for (int y = std::max(minY, py - 6); y <= std::min(maxY - 1, py + 6); ++y)
                for (int z = pz - 6; z <= pz + 6; ++z) {
                    const BlockInfo& b = s.registry->infos()[s.world->block(x, y, z)];
                    if (b.kind != KindCube || !b.collisionCount) continue;
                    const double o = 0.002;
                    boxFill(x - o, y - o, z - o, x + 1 + o, y + 1 + o, z + 1 + o, 0x80FF0000);
                }
    if (heights) {  // HeightMapRenderer: the client's heightmaps, 5 x 5 chunks
        const int cx0 = (px >> 4) - 2, cz0 = (pz >> 4) - 2;
        for (int x = cx0 * 16; x < (cx0 + 5) * 16; ++x)
            for (int z = cz0 * 16; z < (cz0 + 5) * 16; ++z) {
                int S = INT32_MIN, M = INT32_MIN, ML = INT32_MIN;
                for (int y = maxY - 1; y >= minY && ML == INT32_MIN; --y) {
                    const u16 id = s.world->block(x, y, z);
                    if (!id) continue;
                    const BlockInfo& b = s.registry->infos()[id];
                    const bool blocking = b.collisionCount > 0 || b.fluid || b.waterlogged;
                    if (S == INT32_MIN) S = y + 1;
                    if (M == INT32_MIN && blocking) M = y + 1;
                    if (blocking && s.registry->name(id).find("leaves") == std::string::npos) ML = y + 1;
                }
                // ordinals 1 (WORLD_SURFACE), 4 (MOTION_BLOCKING), 5 (MOTION_BLOCKING_NO_LEAVES)
                const int hs[3] = {S, M, ML}, ord[3] = {1, 4, 5};
                const u32 col[3] = {0xFF00B200, 0xFF004C4C, 0xFF007F7F};
                for (int k = 0; k < 3; ++k) {
                    if (hs[k] == INT32_MIN) continue;
                    const double y1 = hs[k] + ord[k] * 0.09375 + 0.09375;
                    const double q[4][3] = {{x + 0.25, y1, z + 0.25}, {x + 0.25, y1, z + 0.75}, {x + 0.75, y1, z + 0.75}, {x + 0.75, y1, z + 0.25}};
                    face(q, col[k]);
                }
            }
    }
}

const UiModuleRegistrar kWorldDebug({.name = "debug_world", .drawHud = drawWorldDebug});

}  // namespace

// --- the Debug Options screen (DebugOptionsScreen) ------------------------------------------------------------

namespace {

struct ScreenState {
    Field search;
    float scroll = 0;
    std::string lastFilter;
} gs;

constexpr int kHead = 61, kFoot = 33, kRowW = 350, kItem = 20;

struct Row {
    bool header;
    int category;  // header: 0 text, 1 renderer
    size_t entry;
};
std::vector<Row> rows() {  // sorted by category, then id; the search on the id path
    std::vector<size_t> ids;
    for (size_t i = 0; i < kCount; ++i)
        if (gs.search.text.value.empty() || std::string_view(kEntries[i].id).find(gs.search.text.value) != std::string_view::npos) ids.push_back(i);
    std::sort(ids.begin(), ids.end(), [](size_t a, size_t b) {
        if (kEntries[a].renderer != kEntries[b].renderer) return !kEntries[a].renderer;
        return std::string_view(kEntries[a].id) < std::string_view(kEntries[b].id);
    });
    std::vector<Row> out;
    int category = -1;
    for (size_t i : ids) {
        const int cat = kEntries[i].renderer ? 1 : 0;
        if (cat != category) out.push_back({true, cat, 0}), category = cat;
        out.push_back({false, cat, i});
    }
    return out;
}

float maxScroll(UiState& s) { return std::max(0.0f, float(int(rows().size()) * kItem + 4) - float(s.guiH() - kHead - kFoot)); }

void build(UiState& s) {
    load();
    s.buttons.clear();
    UiState* p = &s;
    const int W = s.guiW(), H = s.guiH();
    const float fy = float(H - kFoot + 6);
    const float total = 120 + 120 + 110 + 60 + 3 * 8;
    float x = float(W) / 2 - total / 2;
    for (int prof = 0; prof < 2; ++prof) {
        Button& b = addButton(s, x, fy, 120, tr(s, prof ? "debug.options.profile.performance" : "debug.options.profile.default"), [p, prof] {
            resetToProfile(prof);
            save();
            p->buildButtons();
        });
        b.active = g.profile != prof;
        x += 128;
    }
    Button& scale = addButton(s, x, fy, 110, Text(), [p] {  // Debug GUI scale: unchanged, auto, 1..max
        const int max = calcScale(*p, 0);
        int& v = p->settings.debugGuiScale;
        v = v >= max ? -1 : v + 1;
        p->settings.save();
    });
    scale.labelFn = [p] {
        const int v = p->settings.debugGuiScale;
        const std::string value = v == -1 ? p->res.translate("options.debugGuiScale.unchanged") : v == 0 ? p->res.translate("options.guiScale.auto") : std::to_string(v);
        return Text(p->res.translate("options.guiScale") + ": " + value);
    };
    scale.tooltip = [p] { return Text(p->res.translate("options.debugGuiScale.tooltip")); };
    x += 118;
    addButton(s, x, fy, 60, tr(s, "gui.done"), [p] { p->closeScreen(); });
    gs.search.w = float(kRowW / 3), gs.search.h = 20;
    gs.search.x = float(W / 2 + kRowW / 2 - kRowW / 3), gs.search.y = 8;
    gs.search.hint = s.res.translate("debug.options.search");
    setFields(s, {&gs.search}, -1);
}

void draw(UiState& s, Canvas& c) {
    drawDebugScreen(s, c);  // the overlay behind the blur (extractBlurredBackground)
    menuBackground(s, c);
    const int W = c.guiWidth(), H = c.guiHeight();
    const float left = float(W / 2 - kRowW / 2);
    s.drawText(c, tr(s, "debug.options.title"), left, 14, 0xFFFFFFFF, true);
    const Text warning(s.res.translate("debug.options.warning"));
    int ly = 34;
    for (const Text& l : s.font.split(warning, kRowW)) {
        s.drawCentered(c, l, float(W / 2), float(ly), 0xFFDF5050);
        ly += 10;
    }
    std::vector<Text> tooltip;
    const std::vector<Row> list = rows();
    c.pushScissor(0, float(kHead), float(W), float(H - kFoot));
    for (size_t i = 0; i < list.size(); ++i) {
        const float y = float(kHead + 2) + float(i) * kItem - gs.scroll;
        if (y + kItem < kHead || y > H - kFoot) continue;
        const Row& r = list[i];
        if (r.header) {
            Style st;
            st.bold = 1, st.underlined = 1;
            s.drawCentered(c, Text(s.res.translate(r.category ? "debug.options.category.renderer" : "debug.options.category.text"), st), float(W / 2), y + 6,
                           0xFFFFFFFF);
            continue;
        }
        const EntryDef& e = kEntries[r.entry];
        const bool allowed = e.reducedOk || !reduced(s);
        Style it;
        if (!allowed) it.italic = 1;
        s.drawText(c, Text(e.id, it), left, y + 5, allowed ? 0xFFFFFFFF : 0xFF808080, true);
        if (!allowed && s.mouseX >= left && s.mouseX < left + 150 && s.mouseY >= y && s.mouseY < y + kItem)
            tooltip = {tr(s, "debug.options.notAllowed.tooltip")};
        // Off | In Overlay | Always, right-aligned; the current one disabled and lit
        static const char* kLabels[3] = {"options.off", "debug.entry.overlay", "debug.entry.always"};
        static const u32 kOn[3] = {0xFFFFFFFF, 0xFFFFFF55, 0xFFDF5050};
        for (int k = 0; k < 3; ++k) {
            const float bx = left + kRowW - float(3 - k) * 62, by = y + 2;
            const bool current = int(g.status[r.entry]) == k;
            const bool hov = !current && s.mouseX >= bx && s.mouseX < bx + 60 && s.mouseY >= by && s.mouseY < by + 16 && s.mouseY >= kHead &&
                             s.mouseY < H - kFoot;
            blitSprite(c, s.sprites.sprite(current ? "widget/button_disabled" : hov ? "widget/button_highlighted" : "widget/button"), bx, by, 60, 16);
            s.drawCentered(c, tr(s, kLabels[k]), bx + 30, by + 4, current ? kOn[k] : 0xFFBABABA);
        }
    }
    c.popScissor();
    drawFields(s, c);
    s.drawButtons(c);
    if (!tooltip.empty()) s.drawTooltip(c, tooltip, s.mouseX, s.mouseY);
}

void input(UiState& s, const InputEvent& e) {
    using E = Window::InputEvent;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) return s.closeScreen();
    if (e.type == E::Wheel) {
        gs.scroll = std::clamp(gs.scroll - float(e.code) * 10.0f, 0.0f, maxScroll(s));
        return;
    }
    if (fieldsInput(s, e)) {
        gs.scroll = std::clamp(gs.scroll, 0.0f, maxScroll(s));
        return;
    }
    if (e.type == E::Button && e.down && e.code == 0 && s.mouseY >= kHead && s.mouseY < s.guiH() - kFoot) {
        const std::vector<Row> list = rows();
        const float left = float(s.guiW() / 2 - kRowW / 2);
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].header) continue;
            const float y = float(kHead + 2) + float(i) * kItem - gs.scroll;
            for (int k = 0; k < 3; ++k) {
                const float bx = left + kRowW - float(3 - k) * 62, by = y + 2;
                if (s.mouseX >= bx && s.mouseX < bx + 60 && s.mouseY >= by && s.mouseY < by + 16 && int(g.status[list[i].entry]) != k) {
                    audio::playUiClick();
                    setStatus(list[i].entry, Status(k));
                    s.buildButtons();
                    return;
                }
            }
        }
    }
    s.buttonsInput(e);
}

void closed(UiState& s) { setFields(s, {}, -1); }

const ScreenRegistrar kDebugOptions({Screen::DebugOptions, build, draw, input, closed});

}  // namespace

}  // namespace ui
