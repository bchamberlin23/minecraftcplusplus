// Block entity tracking, data, animation and geometry; the GPU side is in block_entity_render.cpp.
// Transforms follow the game's block entity renderers (client/renderer/blockentity/*Renderer).
#include "blockentity/block_entities.h"

#include "assets/assets.h"
#include "assets/game_reports.h"
#include "audio/audio.h"
#include "entity/entity_render.h"
#include "game/item_rules.h"
#include "net/client.h"
#include "world/nbt.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <functional>
#include <shared_mutex>

namespace be {

struct M34 {
    float m[12];
    static M34 identity() { return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}; }
};

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;
constexpr int kViewDistance = 64;  // BlockEntityRenderer.getViewDistance
constexpr int kRadius = (kViewDistance + 15) / 16 + 1;  // tracked chunk radius

// DyeColor: names (by id), getTextureDiffuseColor, getTextColor.
constexpr const char* kDyeNames[16] = {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                                       "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};
constexpr u32 kDyeDiffuse[16] = {0xF9FFFE, 0xF9801D, 0xC74EBD, 0x3AB3DA, 0xFED83D, 0x80C71F, 0xF38BAA, 0x474F52,
                                 0x9D9D97, 0x169C9C, 0x8932B8, 0x3C44AA, 0x835432, 0x5E7C16, 0xB02E26, 0x1D1D21};
constexpr u32 kDyeText[16] = {0xFFFFFF, 0xFF681F, 0xFF00FF, 0x9AC0CD, 0xFFFF00, 0xBFFF00, 0xFF69B4, 0x808080,
                              0xD3D3D3, 0x00FFFF, 0xA020F0, 0x0000FF, 0x8B4513, 0x00FF00, 0xFF0000, 0x000000};
int dyeId(std::string_view n) {
    if (const size_t c = n.find(':'); c != std::string_view::npos) n = n.substr(c + 1);
    for (int i = 0; i < 16; ++i)
        if (n == kDyeNames[i]) return i;
    return -1;
}
u32 abgr(u32 rgb, u32 a = 255) { return (a << 24) | ((rgb & 255) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 255); }

M34 mul(const M34& a, const M34& b) {
    M34 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            r.m[i * 4 + j] = a.m[i * 4] * b.m[j] + a.m[i * 4 + 1] * b.m[4 + j] + a.m[i * 4 + 2] * b.m[8 + j];
        r.m[i * 4 + 3] = a.m[i * 4] * b.m[3] + a.m[i * 4 + 1] * b.m[7] + a.m[i * 4 + 2] * b.m[11] + a.m[i * 4 + 3];
    }
    return r;
}
M34 translate(float x, float y, float z) { return {{1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z}}; }
M34 scale(float x, float y, float z) { return {{x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0}}; }
M34 scale(float s) { return scale(s, s, s); }
M34 rotX(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {{1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0}};
}
M34 rotY(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {{c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0}};
}
M34 rotZ(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {{c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0}};
}
M34 rotAxis(float x, float y, float z, float a) {  // right-handed rotation about a unit axis
    const float c = std::cos(a), s = std::sin(a), t = 1 - c;
    return {{t * x * x + c, t * x * y - s * z, t * x * z + s * y, 0, t * x * y + s * z, t * y * y + c, t * y * z - s * x, 0,
             t * x * z - s * y, t * y * z + s * x, t * z * z + c, 0}};
}
M34 rotateAround(const M34& r, float px, float py, float pz) { return mul(mul(translate(px, py, pz), r), translate(-px, -py, -pz)); }
// ModelPart.translateAndRotate: T(pivot / 16) * Rz * Ry * Rx * S.
M34 partLocal(const ent::PartPose& p) {
    M34 r = translate(p.x / 16.0f, p.y / 16.0f, p.z / 16.0f);
    if (p.zRot != 0) r = mul(r, rotZ(p.zRot));
    if (p.yRot != 0) r = mul(r, rotY(p.yRot));
    if (p.xRot != 0) r = mul(r, rotX(p.xRot));
    if (p.xScale != 1 || p.yScale != 1 || p.zScale != 1) r = mul(r, scale(p.xScale, p.yScale, p.zScale));
    return r;
}
void xform(const M34& m, const float in[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = m.m[i * 4] * in[0] + m.m[i * 4 + 1] * in[1] + m.m[i * 4 + 2] * in[2] + m.m[i * 4 + 3];
}
u32 packNormal(const M34& m, const float in[3]) {
    float n[3];
    for (int i = 0; i < 3; ++i) n[i] = m.m[i * 4] * in[0] + m.m[i * 4 + 1] * in[1] + m.m[i * 4 + 2] * in[2];
    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    u32 r = 0;
    for (int i = 0; i < 3; ++i) r |= u32(u8(i8(std::lround(std::clamp(l > 0 ? n[i] / l : 0.0f, -1.0f, 1.0f) * 127.0f)))) << (i * 8);
    return r;
}

// Direction (3D data values): 0 down, 1 up, 2 north, 3 south, 4 west, 5 east.
int dirFromName(std::string_view s) {
    static const char* kNames[6] = {"down", "up", "north", "south", "west", "east"};
    for (int i = 0; i < 6; ++i)
        if (s == kNames[i]) return i;
    return 3;
}
float yRotOf(int dir) {  // Direction.toYRot
    switch (dir) {
        case 3: return 0;
        case 4: return 90;
        case 2: return 180;
        case 5: return 270;
        default: return 0;
    }
}
int opposite(int dir) { return dir ^ 1; }
int data2d(int dir) { return dir == 3 ? 0 : dir == 4 ? 1 : dir == 2 ? 2 : 3; }
int from2d(int v) {
    static const int k[4] = {3, 4, 2, 5};
    return k[v & 3];
}
int clockwise(int dir) { return dir == 2 ? 5 : dir == 5 ? 3 : dir == 3 ? 4 : dir == 4 ? 2 : dir; }
int stepX(int dir) { return dir == 4 ? -1 : dir == 5 ? 1 : 0; }
int stepZ(int dir) { return dir == 2 ? -1 : dir == 3 ? 1 : 0; }
int stepY(int dir) { return dir == 1 ? 1 : dir == 0 ? -1 : 0; }
constexpr const char* kDirNames[6] = {"down", "up", "north", "south", "west", "east"};
// Direction.getRotation (JOML rotationXYZ(a, b, c) = Rx(a) * Ry(b) * Rz(c)).
M34 dirRotation(int dir) {
    switch (dir) {
        case 0: return rotX(kPi);
        case 1: return M34::identity();
        case 2: return mul(rotX(kPi / 2), rotZ(kPi));
        case 3: return rotX(kPi / 2);
        case 4: return mul(rotX(kPi / 2), rotZ(kPi / 2));
        default: return mul(rotX(kPi / 2), rotZ(-kPi / 2));
    }
}

u64 posKey(int x, int y, int z) {
    return ((u64(u32(x)) & 0x3FFFFFF) << 38) | ((u64(u32(z)) & 0x3FFFFFF) << 12) | (u64(u32(y)) & 0xFFF);
}
void posOf(u64 k, int& x, int& y, int& z) {
    x = int(i64(k) >> 38);
    z = int(i64(k << 26) >> 38);
    y = int(i64(k << 52) >> 52);
}
u64 sectionKey(int cx, int sy, int cz) { return posKey(cx, sy, cz); }
BlockEntityRenderer* gInstance = nullptr;  // BlockEntityRenderer::instance()

std::string_view stripNs(std::string_view s) {
    const size_t c = s.find(':');
    return c == std::string_view::npos ? s : s.substr(c + 1);
}

// --- NBT helpers (network NBT: type byte + payload) ---
// A string tag, or the "id" / "asset_id" string of a compound (item stacks, inline registry entries).
std::string readId(NbtReader& r, u8 tag) {
    if (tag == TagString) return std::string(r.str());
    if (tag != TagCompound) {
        r.skip(tag);
        return {};
    }
    std::string out;
    u8 t;
    std::string_view n;
    while (r.field(t, n)) {
        if (t == TagString && (n == "id" || n == "asset_id")) out = std::string(r.str());
        else r.skip(t);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------
// Setup

BlockEntityRenderer::BlockEntityRenderer(World& world, BlockRegistry& registry, const GameReports& reports,
                                         const ent::EntityAssets& assets, const ent::EntityRenderer& entityRenderer)
    : world_(world), registry_(registry), reports_(reports), assets_(assets), entityRenderer_(entityRenderer) {
    gInstance = this;
    initAssets();
    const std::time_t now = std::time(nullptr);  // SpecialDates.isExtendedChristmas: Dec 24-26
    std::tm tm{};
    localtime_s(&tm, &now);
    xmas_ = tm.tm_mon == 11 && tm.tm_mday >= 24 && tm.tm_mday <= 26;
}

const BlockEntityRenderer* BlockEntityRenderer::instance() { return gInstance; }

const BlockEntityRenderer::Data* BlockEntityRenderer::dataAt(int x, int y, int z) const {
    auto it = sections_.find(sectionKey(x >> 4, y >> 4, z >> 4));
    if (it == sections_.end()) return nullptr;
    for (const Instance& in : it->second.list)
        if (in.x == x && in.y == y && in.z == z)
            return in.data >= 0 && size_t(in.data) < it->second.data.size() ? &it->second.data[size_t(in.data)] : nullptr;
    return nullptr;
}

void BlockEntityRenderer::crackQuads(int x, int y, int z, std::vector<BeQuad>& out) const {
    out.clear();
    auto it = sections_.find(sectionKey(x >> 4, y >> 4, z >> 4));
    if (it == sections_.end()) return;
    for (const Instance& in : it->second.list) {
        if (in.x != x || in.y != y || in.z != z) continue;
        std::vector<BeQuad> text, translucent;
        Emit e{{&out, &text, &translucent}, {double(x), double(y), double(z)}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        const Data* data = in.data >= 0 && size_t(in.data) < it->second.data.size() ? &it->second.data[size_t(in.data)] : nullptr;
        emitInstance(e, in, data, partial_);
        out.insert(out.end(), translucent.begin(), translucent.end());
        std::erase_if(out, [](const BeQuad& q) { return q.tex == kTexAtlasRepeat || q.tex == kTexEndPortal || q.tex == kTexEndGateway; });
        return;
    }
}

void BlockEntityRenderer::shownItems(std::vector<ShownItem>& out) const {
    if (!itemDrawer_) return;
    // The same poses the geometry would use: emitInstance with emitItem / the player heads recording instead.
    std::vector<BeQuad> sink[3];
    Emit e{};
    for (int k = 0; k < 3; ++k) e.out[k] = &sink[k];
    shownOut_ = &out;
    for (const auto& [key, s] : sections_)
        for (const Instance& in : s.list) {
            if (in.data < 0 || size_t(in.data) >= s.data.size()) continue;
            const Kind kind = in.block < defs_.size() ? defs_[in.block].kind : Kind::None;
            if (kind != Kind::Campfire && kind != Kind::Shelf && kind != Kind::Vault && kind != Kind::Brushable &&
                kind != Kind::Skull)
                continue;
            const Data& d = s.data[size_t(in.data)];
            bool any = false;
            for (const std::string& n : d.itemNbt) any |= !n.empty();
            if (!any) continue;
            e.origin[0] = in.x, e.origin[1] = in.y, e.origin[2] = in.z;
            emitInstance(e, in, &d, partial_);
        }
    shownOut_ = nullptr;
}

BlockEntityRenderer::~BlockEntityRenderer() {
    if (gInstance == this) gInstance = nullptr;
    world_.trackBlockChanges(false);
    if (!frames_) return;
    const u32 n = u32(std::min<u64>(frames_, kHistory));
    double ms = 0, maxMs = 0;
    for (u32 i = 0; i < n; ++i) ms += historyMs_[i], maxMs = std::max(maxMs, double(historyMs_[i]));
    LOG("block entities: %.3f ms average CPU per frame; last %u frames %.3f ms avg (max %.3f), %u tracked in %u "
        "sections, %u static quads",
        cpuTotal_ / double(frames_), n, ms / n, maxMs, stats_.tracked, stats_.sections, stats_.staticQuads);
}

bool BlockEntityRenderer::load(const fs::path& jar, const fs::path& minecraftDir) {
    Timer t;
    res_ = std::make_unique<ui::Resources>();
    if (!res_->open(jar, minecraftDir)) {
        LOG("block entities: client resources unavailable; sign text will not be drawn");
        res_.reset();
        return false;
    }
    fontAtlas_.init(1024, 2048);
    font_ = std::make_unique<ui::Font>();
    if (!font_->load(*res_, fontAtlas_)) {
        LOG("block entities: font unavailable; sign text will not be drawn");
        font_.reset();
        return false;
    }
    LOG("blockent %7.1f ms  font for sign text", t.ms());
    return true;
}

void BlockEntityRenderer::initAssets() {
    auto M = [&](const char* layer) { return assets_.model(layer); };
    auto T = [&](const std::string& tex) { return assets_.texture("entity/" + tex); };
    m_.chest = M("chest#main"), m_.chestLeft = M("double_chest_left#main"), m_.chestRight = M("double_chest_right#main");
    m_.shulker = M("shulker_box#main"), m_.bell = M("bell#main"), m_.book = M("book#main");
    m_.conduitShell = M("conduit#shell"), m_.conduitCage = M("conduit#cage"), m_.conduitWind = M("conduit#wind");
    m_.conduitEye = M("conduit#eye");
    m_.potBase = M("decorated_pot_base#main"), m_.potSides = M("decorated_pot_sides#main");
    m_.bannerStanding = M("standing_banner#main"), m_.bannerStandingFlag = M("standing_banner#flag");
    m_.bannerWall = M("wall_banner#main"), m_.bannerWallFlag = M("wall_banner#flag");
    const char* skulls[7] = {"skeleton_skull#main", "wither_skeleton_skull#main", "zombie_head#main", "creeper_head#main",
                             "dragon_skull#main", "piglin_head#main", "player_head#main"};
    for (int i = 0; i < 7; ++i) m_.skull[i] = M(skulls[i]);
    const char* statues[4] = {"copper_golem#main", "copper_golem_running#main", "copper_golem_sitting#main",
                              "copper_golem_star#main"};
    for (int i = 0; i < 4; ++i) m_.statue[i] = M(statues[i]);

    // Chest materials: regular, trapped, ender, christmas, copper (unaffected, exposed, weathered, oxidized).
    const char* chests[8] = {"normal", "trapped", "ender", "christmas", "copper", "copper_exposed", "copper_weathered",
                             "copper_oxidized"};
    for (int i = 0; i < 8; ++i) {
        const std::string base = std::string("chest/") + chests[i];
        t_.chest[i][0] = T(base);
        t_.chest[i][1] = T(base + "_left");
        t_.chest[i][2] = T(base + "_right");
        for (int k = 1; k < 3; ++k)
            if (t_.chest[i][k] < 0) t_.chest[i][k] = t_.chest[i][0];
    }
    t_.shulker[0] = T("shulker/shulker");
    for (int i = 0; i < 16; ++i) t_.shulker[i + 1] = T(std::string("shulker/shulker_") + kDyeNames[i]);
    t_.bell = T("bell/bell_body");
    t_.book = T("enchantment/enchanting_table_book");
    t_.bannerBase = T("banner/banner_base"), t_.bannerPatternBase = T("banner/base");
    t_.conduitBase = T("conduit/base"), t_.conduitCage = T("conduit/cage"), t_.conduitWind = T("conduit/wind");
    t_.conduitWindV = T("conduit/wind_vertical"), t_.conduitOpen = T("conduit/open_eye"), t_.conduitClosed = T("conduit/closed_eye");
    t_.potBase = T("decorated_pot/decorated_pot_base"), t_.potSide = T("decorated_pot/decorated_pot_side");
    const char* skullTex[7] = {"skeleton/skeleton", "skeleton/wither_skeleton", "zombie/zombie", "creeper/creeper",
                               "enderdragon/dragon", "piglin/piglin", "player/slim/steve"};
    for (int i = 0; i < 7; ++i) t_.skull[i] = T(skullTex[i]);
    const char* skins[9] = {"alex", "ari", "efe", "kai", "makena", "noor", "steve", "sunny", "zuri"};
    for (int i = 0; i < 18; ++i) {  // DefaultPlayerSkin.DEFAULT_SKINS: slim, then wide
        t_.playerSkins[i] = T(std::string(i < 9 ? "player/slim/" : "player/wide/") + skins[i % 9]);
        if (t_.playerSkins[i] < 0) t_.playerSkins[i] = t_.skull[6];
    }
    const char* oxidation[4] = {"copper_golem/copper_golem", "copper_golem/copper_golem_exposed",
                                "copper_golem/copper_golem_weathered", "copper_golem/copper_golem_oxidized"};
    for (int i = 0; i < 4; ++i) t_.statue[i] = T(oxidation[i]);
    t_.beaconBeam = T("beacon/beacon_beam"), t_.gatewayBeam = T("end_portal/end_gateway_beam");
    t_.endPortal = T("end_portal/end_portal"), t_.endSky = assets_.texture("environment/end_sky");
    int missing = 0;
    for (int v : {m_.chest, m_.shulker, m_.bell, m_.book, m_.potBase, m_.bannerStanding, m_.skull[0], m_.statue[0]})
        missing += v < 0;
    for (int v : {t_.chest[0][0], t_.shulker[0], t_.bell, t_.book, t_.bannerBase, t_.potBase, t_.skull[0], t_.statue[0]})
        missing += v < 0;
    if (missing) LOG("block entities: %d model layers / textures missing from the game data", missing);
}

// ---------------------------------------------------------------------------------------------------------
// Block state classification

BlockEntityRenderer::BlockDef BlockEntityRenderer::classify(u16 block) const {
    BlockDef d;
    const std::string& full = registry_.name(block);
    std::string_view n = full;
    if (const size_t b = n.find('['); b != std::string_view::npos) n = n.substr(0, b);
    n = stripNs(n);
    auto prop = [&](std::string_view key) { return registry_.property(block, key); };
    auto facing = [&]() { return u8(dirFromName(prop("facing"))); };
    auto rotation = [&]() {
        const std::string_view r = prop("rotation");
        return u8(r.empty() ? 0 : std::clamp(std::atoi(std::string(r).c_str()), 0, 15));
    };
    auto ends = [&](std::string_view s) { return n.size() > s.size() && n.ends_with(s); };
    if (n == "chest" || n == "trapped_chest" || n == "ender_chest" || n.ends_with("copper_chest")) {
        d.kind = Kind::Chest;
        d.facing = facing();
        const std::string_view type = prop("type");
        d.part = type == "left" ? 1 : type == "right" ? 2 : 0;
        if (n == "trapped_chest") d.variant = 1;
        else if (n == "ender_chest") d.variant = 2;
        else if (n.ends_with("copper_chest"))
            d.variant = n.find("oxidized") != std::string_view::npos    ? 7
                        : n.find("weathered") != std::string_view::npos ? 6
                        : n.find("exposed") != std::string_view::npos   ? 5
                                                                        : 4;
    } else if (ends("_wall_hanging_sign")) {
        d.kind = Kind::HangingSign, d.wall = true, d.facing = facing();
    } else if (ends("_hanging_sign")) {
        d.kind = Kind::HangingSign, d.rotation = rotation();
    } else if (ends("_wall_sign")) {
        d.kind = Kind::Sign, d.wall = true, d.facing = facing();
    } else if (ends("_sign")) {
        d.kind = Kind::Sign, d.rotation = rotation();
    } else if (ends("_wall_banner") || ends("_banner")) {
        const bool wall = ends("_wall_banner");
        const int dye = dyeId(n.substr(0, n.size() - (wall ? 12 : 7)));
        if (dye >= 0) {
            d.kind = Kind::Banner, d.wall = wall, d.variant = u8(dye + 1);
            if (wall) d.facing = facing();
            else d.rotation = rotation();
        }
    } else if (n.ends_with("_skull") || n.ends_with("_head")) {
        static const char* kTypes[7] = {"skeleton", "wither_skeleton", "zombie", "creeper", "dragon", "piglin", "player"};
        const size_t suffix = n.ends_with("_wall_skull") ? 11 : n.ends_with("_wall_head") ? 10 : n.ends_with("_skull") ? 6 : 5;
        const bool wall = suffix >= 10;
        const std::string_view type = n.substr(0, n.size() - suffix);
        for (int i = 0; i < 7; ++i)
            if (type == kTypes[i]) {
                d.kind = Kind::Skull, d.wall = wall, d.variant = u8(i);
                if (wall) d.facing = facing();
                else d.rotation = rotation();
                d.flag = prop("powered") == "true";
            }
    } else if (n == "shulker_box" || ends("_shulker_box")) {
        const int dye = n == "shulker_box" ? -1 : dyeId(n.substr(0, n.size() - 12));
        if (n == "shulker_box" || dye >= 0) d.kind = Kind::Shulker, d.variant = u8(dye + 1), d.facing = facing();
    } else if (n == "bell") {
        d.kind = Kind::Bell, d.facing = facing();
    } else if (n == "decorated_pot") {
        d.kind = Kind::Pot, d.facing = facing();
    } else if (n == "conduit") {
        d.kind = Kind::Conduit;
    } else if (n == "enchanting_table") {
        d.kind = Kind::Enchanting;
    } else if (n == "lectern") {
        d.kind = Kind::Lectern, d.facing = facing(), d.flag = prop("has_book") == "true";
    } else if (n == "campfire" || n == "soul_campfire") {
        d.kind = Kind::Campfire, d.facing = facing();
    } else if (ends("_shelf")) {
        d.kind = Kind::Shelf, d.facing = facing();
    } else if (n.ends_with("copper_golem_statue")) {
        d.kind = Kind::Statue, d.facing = facing();
        d.variant = n.find("oxidized") != std::string_view::npos    ? 3
                    : n.find("weathered") != std::string_view::npos ? 2
                    : n.find("exposed") != std::string_view::npos   ? 1
                                                                    : 0;
        const std::string_view pose = prop("copper_golem_pose");
        d.part = pose == "running" ? 1 : pose == "sitting" ? 2 : pose == "star" ? 3 : 0;
    } else if (n == "beacon") {
        d.kind = Kind::Beacon;
    } else if (n == "end_portal") {
        d.kind = Kind::EndPortal;
    } else if (n == "end_gateway") {
        d.kind = Kind::EndGateway;
    } else if (n == "spawner") {
        d.kind = Kind::Spawner;
    } else if (n == "trial_spawner") {  // TrialSpawnerState.spinningMobSpeed: waiting_for_players, active
        const std::string_view st = prop("trial_spawner_state");
        d.kind = Kind::TrialSpawner, d.variant = st == "waiting_for_players" ? 1 : st == "active" ? 2 : 0;
    } else if (n == "vault") {
        d.kind = Kind::Vault, d.facing = facing();
    } else if (n == "test_instance_block") {
        d.kind = Kind::TestInstance;
    } else if (n == "suspicious_sand" || n == "suspicious_gravel") {
        const std::string_view dusted = prop("dusted");
        d.kind = Kind::Brushable, d.variant = u8(dusted.empty() ? 0 : std::clamp(dusted[0] - '0', 0, 3));
    }
    return d;
}

bool BlockEntityRenderer::ticking(const BlockDef& d) {
    switch (d.kind) {
        case Kind::Enchanting:
        case Kind::Conduit:
        case Kind::Beacon:
        case Kind::EndGateway:
        case Kind::Spawner:
        case Kind::TrialSpawner:
        case Kind::Vault: return true;
        case Kind::Skull: return d.flag;
        default: return false;
    }
}

const BlockEntityRenderer::BlockDef& BlockEntityRenderer::def(u16 block) {
    if (block >= defs_.size()) {
        const size_t n = std::max<size_t>(registry_.blockCount(), size_t(block) + 1);
        defs_.resize(n);
        defKnown_.resize(n, 0);
    }
    if (!defKnown_[block]) {
        defs_[block] = block ? classify(block) : BlockDef{};
        defKnown_[block] = 1;
    }
    return defs_[block];
}

// ---------------------------------------------------------------------------------------------------------
// NBT data

void BlockEntityRenderer::decode(const std::string& nbt, const BlockDef& d, Data& out) const {
    if (nbt.size() < 2 || u8(nbt[0]) != TagCompound) return;
    NbtReader r(reinterpret_cast<const u8*>(nbt.data()) + 1, nbt.size() - 1);
    auto signSide = [&](SignSide& side) {
        u8 t;
        std::string_view n;
        while (r.field(t, n)) {
            if (t == TagList && n == "messages") {
                const u8 et = r.u8v();
                const i32 count = r.i32v();
                for (i32 i = 0; i < count && r.ok; ++i) {
                    if (et == TagString) {
                        const std::string s(r.str());
                        // Old saves keep JSON text in strings.
                        nlohmann::json j = (!s.empty() && (s[0] == '{' || s[0] == '[' || (s[0] == '"' && s.size() > 1 && s.back() == '"')))
                                               ? nlohmann::json::parse(s, nullptr, false)
                                               : nlohmann::json();
                        if (i < 4) side.lines[i] = j.is_discarded() || j.is_null() ? ui::Text(s) : ui::parseJsonComponent(j, nullptr);
                        continue;
                    }
                    const u8* start = r.p;
                    r.skip(et);
                    if (i < 4 && r.ok) {
                        std::string tag(1, char(et));
                        tag.append(reinterpret_cast<const char*>(start), size_t(r.p - start));
                        side.lines[i] = ui::parseComponent(tag, nullptr);
                    }
                }
            } else if (t == TagString && n == "color") {
                const int dye = dyeId(r.str());
                if (dye >= 0) side.color = u8(dye);
            } else if (t == TagByte && n == "has_glowing_text") {
                side.glow = r.u8v() != 0;
            } else {
                r.skip(t);
            }
        }
    };
    auto items = [&](u8 et, i32 count) {
        for (i32 i = 0; i < count && r.ok; ++i) {
            if (et != TagCompound) {
                r.skip(et);
                continue;
            }
            int slot = i;
            std::string id;
            u8 t;
            std::string_view n;
            const u8* start = r.p;
            while (r.field(t, n)) {
                if (n == "Slot" && t == TagByte) slot = i8(r.u8v());
                else if (n == "id" && t == TagString) id = std::string(r.str());
                else r.skip(t);
            }
            if (slot >= 0 && slot < 4 && !id.empty()) {
                out.items[slot] = std::string(stripNs(id));
                out.itemNbt[slot].assign(reinterpret_cast<const char*>(start), size_t(r.p - start));
            }
        }
    };
    u8 t;
    std::string_view n;
    while (r.field(t, n)) {
        if ((d.kind == Kind::Sign || d.kind == Kind::HangingSign) && t == TagCompound && (n == "front_text" || n == "back_text")) {
            signSide(n == "front_text" ? out.front : out.back);
        } else if (d.kind == Kind::Banner && t == TagList && n == "patterns") {
            const u8 et = r.u8v();
            const i32 count = r.i32v();
            for (i32 i = 0; i < count && r.ok; ++i) {
                if (et != TagCompound) {
                    r.skip(et);
                    continue;
                }
                std::string pattern;
                int color = 0;
                u8 t2;
                std::string_view n2;
                while (r.field(t2, n2)) {
                    if (n2 == "pattern") pattern = readId(r, t2);
                    else if (n2 == "color" && t2 == TagString) color = std::max(0, dyeId(r.str()));
                    else r.skip(t2);
                }
                // BannerPattern.assetId: entity/banner/<path> (vanilla patterns use their own id).
                const int tex = pattern.empty() ? -1 : assets_.texture("entity/banner/" + std::string(stripNs(pattern)));
                if (tex >= 0 && out.patterns.size() < 16) out.patterns.push_back({tex, u8(color)});
            }
        } else if (d.kind == Kind::Skull && n == "profile") {
            // ResolvableProfile: a name, or {id: int[4], name, properties}. Default skins by UUID here; for an item
            // drawer, a player_head stack carrying the profile ({id, components: {profile}}).
            {
                NbtReader peek = r;
                const u8* start = peek.p;
                peek.skip(t);
                if (peek.ok) {
                    std::string& s = out.itemNbt[0];
                    auto name = [&](std::string_view v) {
                        s.push_back(char(u8(v.size() >> 8))), s.push_back(char(u8(v.size()))), s.append(v);
                    };
                    s.push_back(char(TagString)), name("id"), name("minecraft:player_head");
                    s.push_back(char(TagCompound)), name("components");
                    s.push_back(char(t)), name("minecraft:profile");
                    s.append(reinterpret_cast<const char*>(start), size_t(peek.p - start));
                    s.push_back(char(TagEnd)), s.push_back(char(TagEnd));
                }
            }
            if (t == TagCompound) {
                u8 t2;
                std::string_view n2;
                while (r.field(t2, n2)) {
                    if (t2 == TagIntArray && n2 == "id") {
                        const i32 len = r.i32v();
                        i32 v[4] = {};
                        for (i32 k = 0; k < len && r.ok; ++k) {
                            const i32 x = r.i32v();
                            if (k < 4) v[k] = x;
                        }
                        if (len == 4) {  // UUID.hashCode, DefaultPlayerSkin.get
                            const u64 most = (u64(u32(v[0])) << 32) | u32(v[1]), least = (u64(u32(v[2])) << 32) | u32(v[3]);
                            const u64 hilo = most ^ least;
                            const i32 hash = i32(u32(hilo >> 32) ^ u32(hilo));
                            out.skin = t_.playerSkins[((hash % 18) + 18) % 18];
                        }
                    } else {
                        r.skip(t2);
                    }
                }
            } else {
                r.skip(t);
            }
        } else if (d.kind == Kind::Pot && n == "sherds") {
            auto sherd = [&](const std::string& item) {
                const std::string_view s = stripNs(item);
                if (!s.ends_with("_pottery_sherd")) return t_.potSide;
                const int tex = assets_.texture("entity/decorated_pot/" + std::string(s.substr(0, s.size() - 14)) + "_pottery_pattern");
                return tex >= 0 ? tex : t_.potSide;
            };
            if (t == TagCompound) {  // {back, left, right, front}
                u8 t2;
                std::string_view n2;
                while (r.field(t2, n2)) {
                    const int side = n2 == "back" ? 0 : n2 == "left" ? 1 : n2 == "right" ? 2 : n2 == "front" ? 3 : -1;
                    if (side < 0) {
                        r.skip(t2);
                        continue;
                    }
                    out.sherds[side] = sherd(readId(r, t2));
                }
            } else if (t == TagList) {  // older: [back, left, right, front]
                const u8 et = r.u8v();
                const i32 count = r.i32v();
                for (i32 i = 0; i < count && r.ok; ++i) {
                    const std::string id = readId(r, et);
                    if (i < 4) out.sherds[i] = sherd(id);
                }
            } else {
                r.skip(t);
            }
        } else if ((d.kind == Kind::Campfire || d.kind == Kind::Shelf) && t == TagList && n == "Items") {
            const u8 et = r.u8v();
            items(et, r.i32v());
        } else if (d.kind == Kind::Shelf && t == TagByte && n == "align_items_to_bottom") {
            out.alignBottom = r.u8v() != 0;
        } else if (((d.kind == Kind::Spawner && n == "SpawnData") || (d.kind == Kind::TrialSpawner && n == "spawn_data")) &&
                   t == TagCompound) {  // SpawnData {entity: {id}}
            u8 t2;
            std::string_view n2;
            while (r.field(t2, n2)) {
                if (n2 == "entity" && t2 == TagCompound) {
                    const std::string id = readId(r, t2);
                    if (!id.empty()) out.entity = assets_.typeId(id);
                } else {
                    r.skip(t2);
                }
            }
        } else if (d.kind == Kind::Spawner && n == "Delay" && t == TagShort) {
            out.delay = r.i16v();
        } else if (d.kind == Kind::Vault && n == "shared_data" && t == TagCompound) {  // VaultSharedData.display_item
            u8 t2;
            std::string_view n2;
            while (r.field(t2, n2)) {
                if (n2 == "display_item") {
                    const u8* start = r.p;
                    out.items[0] = std::string(stripNs(readId(r, t2)));
                    if (t2 == TagCompound) out.itemNbt[0].assign(reinterpret_cast<const char*>(start), size_t(r.p - start));
                } else if (n2 == "connected_players" && t2 == TagList) {  // UUIDs as int arrays
                    const u8 et = r.u8v();
                    const i32 count = r.i32v();
                    for (i32 i = 0; i < count && r.ok; ++i) {
                        if (et != TagIntArray) {
                            r.skip(et);
                            continue;
                        }
                        const i32 len = r.i32v();
                        std::string uuid;
                        for (i32 k = 0; k < len && r.ok; ++k) {
                            const u32 v = u32(r.i32v());
                            for (int b = 3; b >= 0; --b) uuid.push_back(char((v >> (b * 8)) & 0xFF));
                        }
                        if (uuid.size() == 16) out.connected.push_back(std::move(uuid));
                    }
                } else if (n2 == "connected_particles_range" && t2 == TagDouble) {
                    out.connectedRange = r.f64v();
                } else {
                    r.skip(t2);
                }
            }
        } else if (d.kind == Kind::Brushable && n == "item") {
            const u8* start = r.p;
            out.items[0] = std::string(stripNs(readId(r, t)));
            if (t == TagCompound) out.itemNbt[0].assign(reinterpret_cast<const char*>(start), size_t(r.p - start));
        } else if (d.kind == Kind::Brushable && n == "hit_direction" && (t == TagByte || t == TagInt)) {  // Direction.LEGACY_ID_CODEC
            out.hitDir = i8(std::clamp(t == TagByte ? int(i8(r.u8v())) : r.i32v(), -1, 5));
        } else if (d.kind == Kind::EndGateway && n == "Age" && t == TagLong) {
            out.age = r.i64v();
        } else if (d.kind == Kind::TestInstance && n == "data" && t == TagCompound) {
            // TestInstanceBlockEntity.getBeamSections: running grey, finished green, or red on an error (orange for an
            // optional test, which the client cannot tell: the test registry is not sent)
            std::string status;
            bool error = false;
            u8 t2;
            std::string_view n2;
            while (r.field(t2, n2)) {
                if (n2 == "status" && t2 == TagString) status = std::string(r.str());
                else if (n2 == "error_message") error = true, r.skip(t2);
                else r.skip(t2);
            }
            out.beam = status == "running" ? 0x1808080u : status == "finished" ? (error ? 0x1FF0000u : 0x100FF00u) : 0u;
        } else {
            r.skip(t);
        }
        if (!r.ok) break;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Tracking: chunk columns within the view distance, their block entities, light and changes.

u8 BlockEntityRenderer::sampleLight(int x, int y, int z) const {
    const int sy = y >> 4;
    if (sy < world_.minSectionY() || sy > world_.maxSectionY()) return 0xF0;
    return world_.light(x >> 4, sy, z >> 4).at(x & 15, y & 15, z & 15);
}

void BlockEntityRenderer::scanColumn(int cx, int cz, bool farOnly) {
    const u64 ck = World::chunkKey(cx, cz);
    std::vector<u64>& keys = columnSections_[ck];
    std::unordered_map<u64, Section> old;
    for (u64 k : keys) {
        auto it = sections_.find(k);
        if (it != sections_.end()) old.emplace(k, std::move(it->second)), sections_.erase(it);
    }
    keys.clear();
    std::unordered_map<u64, const std::string*> nbt;
    if (const auto* list = world_.blockEntities(cx, cz))
        for (const auto& e : *list) nbt[posKey(e.x, e.y, e.z)] = &e.nbt;
    const World::Region* r = nullptr;
    const World::Column* col = world_.column(cx, cz, &r);
    // Far columns: only the beacons and end gateways among the column's block entities (their beams are seen from
    // further than the other block entities' 64 blocks).
    std::vector<std::pair<int, u32>> farCells;  // section index, cell
    if (farOnly && col)
        if (const auto* list = world_.blockEntities(cx, cz))
            for (const auto& entry : *list) {
                const int k = (entry.y >> 4) - col->minY;
                if (k < 0 || k >= col->count) continue;
                const u32 i = u32((entry.y & 15) << 8 | (entry.z & 15) << 4 | (entry.x & 15));
                const Kind kind = def(World::blockAt(*r, r->sections[col->first + k], i)).kind;
                if (kind == Kind::Beacon || kind == Kind::EndGateway) farCells.push_back({k, i});
            }
    for (int k = 0; col && k < col->count; ++k) {
        const World::Section& s = r->sections[col->first + k];
        if (!s.paletteSize) continue;
        bool any = false;
        if (farOnly) {
            for (const auto& [fk, fi] : farCells) any |= fk == k;
        } else {
            for (u32 p = 0; p < s.paletteSize && !any; ++p) any = def(r->palettes[s.paletteOffset + p]).kind != Kind::None;
        }
        if (!any) continue;
        const int sy = col->minY + k;
        Section sec;
        sec.cx = cx, sec.sy = sy, sec.cz = cz;
        size_t next = 0;
        for (u32 i = 0; i < 4096; ++i) {
            if (farOnly) {  // just this section's beam cells
                while (next < farCells.size() && farCells[next].first != k) ++next;
                if (next >= farCells.size()) break;
                i = farCells[next++].second;
            }
            const u16 id = World::blockAt(*r, s, i);
            const BlockDef& d = def(id);
            if (d.kind == Kind::None) continue;
            Instance in;
            in.x = cx * 16 + int(i & 15), in.y = sy * 16 + int(i >> 8), in.z = cz * 16 + int((i >> 4) & 15);
            in.block = id;
            in.light = sampleLight(in.x, in.y, in.z);
            if (d.kind == Kind::Chest && d.part) {  // BrightnessCombiner: brightest of both halves
                const int other = d.part == 1 ? clockwise(d.facing) : clockwise(clockwise(clockwise(d.facing)));
                const u8 l2 = sampleLight(in.x + stepX(other), in.y, in.z + stepZ(other));
                in.light = u8(std::max(in.light & 0xF0, l2 & 0xF0) | std::max(in.light & 15, l2 & 15));
            }
            const bool wantsData = d.kind == Kind::Sign || d.kind == Kind::HangingSign || d.kind == Kind::Banner ||
                                   (d.kind == Kind::Skull && d.variant == 6) || d.kind == Kind::Pot ||
                                   d.kind == Kind::Campfire || d.kind == Kind::Shelf || d.kind == Kind::Spawner ||
                                   d.kind == Kind::TrialSpawner || d.kind == Kind::Vault || d.kind == Kind::Brushable ||
                                   d.kind == Kind::EndGateway || d.kind == Kind::TestInstance;
            if (wantsData) {
                Data data;
                if (auto it = nbt.find(posKey(in.x, in.y, in.z)); it != nbt.end()) decode(*it->second, d, data);
                in.data = i32(sec.data.size());
                sec.data.push_back(std::move(data));
            }
            // Block entities that animate on their own tick.
            if (ticking(d)) {
                Anim& a = anims_.try_emplace(posKey(in.x, in.y, in.z)).first->second;
                if (d.kind == Kind::EndGateway && a.age < 0 && in.data >= 0) a.age = sec.data[size_t(in.data)].age;
                if (d.kind == Kind::Spawner && a.delay < 0 && in.data >= 0) a.delay = sec.data[size_t(in.data)].delay;
            }
            sec.beams |= d.kind == Kind::Beacon || d.kind == Kind::EndGateway || d.kind == Kind::TestInstance;
            sec.list.push_back(std::move(in));
        }
        if (sec.list.empty()) continue;
        const u64 key = sectionKey(cx, sy, cz);
        if (auto it = old.find(key); it != old.end()) {  // keep the GPU range until the rebuild replaces it
            sec.offset = it->second.offset, sec.allocated = it->second.allocated;
            std::copy(it->second.counts, it->second.counts + 3, sec.counts);
            it->second.allocated = 0;
        }
        sections_.emplace(key, std::move(sec));
        keys.push_back(key);
    }
    for (auto& [k, s] : old)
        if (s.allocated) frees_.push_back({s.offset, s.allocated, frame_});
}

void BlockEntityRenderer::dropColumn(int cx, int cz) {
    const u64 ck = World::chunkKey(cx, cz);
    auto it = columnSections_.find(ck);
    if (it != columnSections_.end()) {
        for (u64 k : it->second) {
            auto s = sections_.find(k);
            if (s == sections_.end()) continue;
            if (s->second.allocated) frees_.push_back({s->second.offset, s->second.allocated, frame_});
            sections_.erase(s);
        }
        columnSections_.erase(it);
    }
    for (auto a = anims_.begin(); a != anims_.end();) {
        int x, y, z;
        posOf(a->first, x, y, z);
        if ((x >> 4) == cx && (z >> 4) == cz) a = anims_.erase(a);
        else ++a;
    }
}

void BlockEntityRenderer::updateTracking(double camX, double camY, double camZ) {
    (void)camY;
    const int ccx = int(std::floor(camX)) >> 4, ccz = int(std::floor(camZ)) >> 4;
    bool changed = false;
    // World edits since the last frame: rescan tracked columns.
    world_.takeBlockChanges(changes_);
    if (!changes_.empty()) {
        std::sort(changes_.begin(), changes_.end());
        changes_.erase(std::unique(changes_.begin(), changes_.end()), changes_.end());
        for (u64 k : changes_)
            if (auto c = columns_.find(k); c != columns_.end()) {
                scanColumn(int(i32(k >> 32)), int(i32(u32(k))), c->second);
                changed = true;
            }
        changes_.clear();
    }
    const int farRadius = std::max(kRadius, renderChunks_ + 1);
    if (ccx != centerCx_ || ccz != centerCz_ || farRadius != trackedFar_) {
        centerCx_ = ccx, centerCz_ = ccz, trackedFar_ = farRadius;
        std::unordered_map<u64, bool> want;  // column -> farCol
        for (int dz = -farRadius; dz <= farRadius; ++dz)
            for (int dx = -farRadius; dx <= farRadius; ++dx) {
                const int d2 = dx * dx + dz * dz;
                if (d2 <= (farRadius + 1) * (farRadius + 1)) want[World::chunkKey(ccx + dx, ccz + dz)] = d2 > (kRadius + 1) * (kRadius + 1);
            }
        for (auto it = columns_.begin(); it != columns_.end();) {
            auto w = want.find(it->first);
            if (w == want.end() || w->second != it->second) {  // gone, or between near and farCol
                dropColumn(int(i32(it->first >> 32)), int(i32(u32(it->first))));
                it = columns_.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& [k, farCol] : want)
            if (columns_.emplace(k, farCol).second) scanColumn(int(i32(k >> 32)), int(i32(u32(k))), farCol);
        changed = true;
    }
    if (changed) {
        sectionOrder_.clear();
        for (const auto& [k, s] : sections_) sectionOrder_.push_back(k);
        lightCursor_ = 0;
    }
}

void BlockEntityRenderer::refreshLight() {
    if (sectionOrder_.empty()) return;
    const size_t per = std::max<size_t>(4, sectionOrder_.size() / 16);  // a full sweep every <= 16 frames
    for (size_t n = 0; n < per && n < sectionOrder_.size(); ++n) {
        lightCursor_ = (lightCursor_ + 1) % sectionOrder_.size();
        auto it = sections_.find(sectionOrder_[lightCursor_]);
        if (it == sections_.end()) continue;
        Section& s = it->second;
        for (Instance& in : s.list) {
            u8 l = sampleLight(in.x, in.y, in.z);
            const BlockDef& d = def(in.block);
            if (d.kind == Kind::Chest && d.part) {
                const int other = d.part == 1 ? clockwise(d.facing) : clockwise(clockwise(clockwise(d.facing)));
                const u8 l2 = sampleLight(in.x + stepX(other), in.y, in.z + stepZ(other));
                l = u8(std::max(l & 0xF0, l2 & 0xF0) | std::max(l & 15, l2 & 15));
            }
            if (l != in.light) in.light = l, s.dirty = true;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// Network: chunk block entity lists, block entity data and block events.

void BlockEntityRenderer::attach(net::Client& client) {
    client.onPlayPeek("level_chunk_with_light", [this](net::Reader& r) {
        NetChunk c;
        c.cx = r.i32v(), c.cz = r.i32v();
        const i32 heightmaps = r.varInt();
        for (i32 i = 0; i < heightmaps && r.ok; ++i) {
            r.varInt();
            r.skip(size_t(u32(r.varInt())) * 8);
        }
        r.byteArray();
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {
            const u8 xz = r.u8v();
            const int y = r.i16v();
            r.varInt();  // type: follows from the block state
            const std::string_view tag = r.nbt();
            if (!r.ok) break;
            c.list.push_back({c.cx * 16 + (xz >> 4), y, c.cz * 16 + (xz & 15), std::string(tag)});
        }
        if (!r.ok) return;
        std::lock_guard lock(netMutex_);
        netChunks_.push_back(std::move(c));
    });
    client.onPlayPeek("block_entity_data", [this](net::Reader& r) {
        NetBlockEntity e;
        r.blockPos(e.x, e.y, e.z);
        r.varInt();
        e.nbt = std::string(r.nbt());
        if (!r.ok) return;
        std::lock_guard lock(netMutex_);
        netData_.push_back(std::move(e));
    });
    client.onPlayPeek("ticking_state", [this](net::Reader& r) {
        const float rate = r.f32();
        const bool frozen = r.boolean();
        if (r.ok) tickRate_ = rate, frozen_ = frozen;
    });
    client.onPlayPeek("block_event", [this](net::Reader& r) {
        NetEvent e;
        r.blockPos(e.x, e.y, e.z);
        e.type = r.u8v(), e.param = r.u8v();
        if (!r.ok) return;
        std::lock_guard lock(netMutex_);
        netEvents_.push_back(e);
    });
}

void BlockEntityRenderer::applyNetwork() {
    std::vector<NetChunk> chunks;
    std::vector<NetBlockEntity> data;
    {
        std::lock_guard lock(netMutex_);
        if (netChunks_.empty() && netData_.empty()) return;
        chunks.swap(netChunks_), data.swap(netData_);  // block events: applyBlockEvents (pistons need them too)
    }
    for (NetChunk& c : chunks) {
        std::vector<World::BlockEntityNbt> list;
        list.reserve(c.list.size());
        for (NetBlockEntity& e : c.list) list.push_back({e.x, e.y, e.z, std::move(e.nbt)});
        world_.setChunkBlockEntities(c.cx, c.cz, std::move(list));
    }
    for (NetBlockEntity& e : data) world_.setBlockEntity({e.x, e.y, e.z, std::move(e.nbt)});
}

void BlockEntityRenderer::handleEvent(int x, int y, int z, int type, int param) {
    const BlockDef& d = def(world_.block(x, y, z));
    if (const std::string& n = registry_.name(world_.block(x, y, z)); n.starts_with("minecraft:note_block[")) {
        // NoteBlock.triggerEvent: a tunable instrument shows its note (the sound comes from the server).
        static constexpr std::string_view kUntuned[] = {"instrument=zombie", "instrument=skeleton", "instrument=creeper",
                                                        "instrument=dragon", "instrument=wither_skeleton", "instrument=piglin",
                                                        "instrument=custom_head"};
        bool tunable = true;
        for (std::string_view u : kUntuned) tunable &= n.find(u) == std::string::npos;
        const size_t at = n.find("note=");
        if (tunable && at != std::string::npos && particle)
            particle("note", x + 0.5, y + 1.2, z + 0.5, std::atoi(n.c_str() + at + 5) / 24.0, 0, 0);
        return;
    }
    if (type != 1) return;
    Anim* a = nullptr;
    switch (d.kind) {
        case Kind::EndGateway:  // TheEndGatewayBlockEntity.triggerEvent: teleport cooldown
            anims_[posKey(x, y, z)].cooldown = 40;
            return;
        case Kind::Spawner:  // BaseSpawner.onEventTriggered: spawnDelay = minSpawnDelay
            anims_[posKey(x, y, z)].delay = 200;
            return;
        case Kind::Chest:  // ChestLidController.shouldBeOpen
        case Kind::Shulker:
            a = &anims_[posKey(x, y, z)];
            a->shouldOpen = param > 0;
            break;
        case Kind::Bell:  // BellBlockEntity.triggerEvent
            a = &anims_[posKey(x, y, z)];
            a->shakeDir = i8(std::clamp(param, 0, 5));
            a->bellTicks = 0;
            break;
        case Kind::Pot:  // DecoratedPotBlockEntity.triggerEvent
            if (param < 0 || param > 1) return;
            a = &anims_[posKey(x, y, z)];
            a->wobbleStart = gameTicks_;
            a->wobbleStyle = u8(param);
            break;
        default: return;
    }
    const u64 sk = sectionKey(x >> 4, y >> 4, z >> 4);
    if (auto it = sections_.find(sk); it != sections_.end()) it->second.dirty = true;
}

// ---------------------------------------------------------------------------------------------------------
// Animation (20 ticks per second, like the game's client block entity tickers)

bool BlockEntityRenderer::isDynamic(const Instance& in, const BlockDef& d) const {
    if (d.kind == Kind::Enchanting || d.kind == Kind::TestInstance) return true;
    auto it = anims_.find(posKey(in.x, in.y, in.z));
    const Anim* a = it == anims_.end() ? nullptr : &it->second;
    switch (d.kind) {
        case Kind::Chest:
        case Kind::Shulker: return a && (a->open > 0 || a->oOpen > 0 || a->shouldOpen);
        case Kind::Bell: return a && a->shakeDir >= 0;
        case Kind::Pot: return a && gameTicks_ - a->wobbleStart <= 11;
        case Kind::Conduit: return a && a->active;
        case Kind::Skull: return d.flag && (d.variant == 4 || d.variant == 5);
        case Kind::Beacon: return a && a->levels > 0 && !a->beam.empty();
        case Kind::EndGateway: return a && ((a->age >= 0 && a->age < 200) || a->cooldown > 0);
        case Kind::Spawner:
        case Kind::Vault: return true;  // spinning (nothing is drawn without a mob / item)
        case Kind::TrialSpawner: return d.variant != 0;
        default: return false;
    }
}

void BlockEntityRenderer::tick() {
    ++gameTicks_;
    auto rnd = [&](int n) {
        random_ ^= random_ << 13, random_ ^= random_ >> 17, random_ ^= random_ << 5;
        return int(random_ % u32(n));
    };
    auto rndf = [&] { return double(rnd(1 << 24)) / double(1 << 24); };
    for (auto it = anims_.begin(); it != anims_.end();) {
        int x, y, z;
        posOf(it->first, x, y, z);
        Anim& a = it->second;
        const u16 block = world_.block(x, y, z);
        const BlockDef& d = def(block);
        bool keep = true;
        switch (d.kind) {
            case Kind::Chest:
            case Kind::Shulker:
                a.oOpen = a.open;
                if (!a.shouldOpen && a.open > 0) a.open = std::max(a.open - 0.1f, 0.0f);
                else if (a.shouldOpen && a.open < 1) a.open = std::min(a.open + 0.1f, 1.0f);
                keep = a.shouldOpen || a.open > 0 || a.oOpen > 0;
                break;
            case Kind::Bell:
                if (a.shakeDir >= 0 && ++a.bellTicks >= 50) a.shakeDir = -1, a.bellTicks = 0;
                keep = a.shakeDir >= 0;
                break;
            case Kind::Pot: keep = gameTicks_ - a.wobbleStart <= 12; break;
            case Kind::Enchanting: {  // EnchantingTableBlockEntity.bookAnimationTick (the camera is the player)
                a.oBook = a.book, a.oRot = a.rot;
                const double dx = cam_[0] - (x + 0.5), dy = cam_[1] - 1.62 - (y + 0.5), dz = cam_[2] - (z + 0.5);
                if (dx * dx + dy * dy + dz * dz < 9.0) {
                    a.tRot = float(std::atan2(dz, dx));
                    a.book += 0.1f;
                    if (a.book < 0.5f || rnd(40) == 0) {
                        const float old = a.flipT;
                        do a.flipT += float(rnd(4) - rnd(4));
                        while (old == a.flipT);
                    }
                } else {
                    a.tRot += 0.02f;
                    a.book -= 0.1f;
                }
                auto wrap = [](float& v) {
                    while (v >= kPi) v -= 2 * kPi;
                    while (v < -kPi) v += 2 * kPi;
                };
                wrap(a.rot), wrap(a.tRot);
                float dir = a.tRot - a.rot;
                wrap(dir);
                a.rot += dir * 0.4f;
                a.book = std::clamp(a.book, 0.0f, 1.0f);
                ++a.time;
                a.oFlip = a.flip;
                const float diff = std::clamp((a.flipT - a.flip) * 0.4f, -0.2f, 0.2f);
                a.flipA += (diff - a.flipA) * 0.9f;
                a.flip += a.flipA;
                break;
            }
            case Kind::Conduit: {  // ConduitBlockEntity.clientTick / updateShape
                ++a.tickCount;
                if (gameTicks_ % 40 == 0 || a.tickCount == 1) {
                    auto water = [&](int bx, int by, int bz) {
                        const u16 b = world_.block(bx, by, bz);
                        return b == registry_.waterId() || registry_.infos()[b].waterlogged ||
                               registry_.name(b).starts_with("minecraft:water");
                    };
                    bool ok = true;
                    for (int ox = -1; ox <= 1 && ok; ++ox)
                        for (int oy = -1; oy <= 1 && ok; ++oy)
                            for (int oz = -1; oz <= 1 && ok; ++oz) ok = water(x + ox, y + oy, z + oz);
                    int frame = 0;
                    if (ok)
                        for (int ox = -2; ox <= 2; ++ox)
                            for (int oy = -2; oy <= 2; ++oy)
                                for (int oz = -2; oz <= 2; ++oz) {
                                    const int ax = std::abs(ox), ay = std::abs(oy), az = std::abs(oz);
                                    if ((ax > 1 || ay > 1 || az > 1) &&
                                        ((ox == 0 && (ay == 2 || az == 2)) || (oy == 0 && (ax == 2 || az == 2)) ||
                                         (oz == 0 && (ax == 2 || ay == 2)))) {
                                        const std::string& n = registry_.name(world_.block(x + ox, y + oy, z + oz));
                                        if (n.starts_with("minecraft:prismarine") || n.starts_with("minecraft:dark_prismarine") ||
                                            n.starts_with("minecraft:sea_lantern"))
                                            ++frame;
                                    }
                                }
                    a.active = ok && frame >= 16;
                    a.hunting = frame >= 42;
                }
                if (a.active) a.activeRot += 1;
                break;
            }
            case Kind::Skull:
                if (d.flag) ++a.skullTicks;
                break;
            case Kind::Beacon: beaconTick(x, y, z, a); break;
            case Kind::EndGateway:  // TheEndGatewayBlockEntity.beamAnimationTick
                if (a.age >= 0) ++a.age;
                if (a.cooldown > 0) --a.cooldown;
                break;
            case Kind::Spawner: {  // BaseSpawner.clientTick (requiredPlayerRange 16; the camera stands in for the player)
                ++a.tickCount;  // display entity age
                a.oSpin = a.spin;
                const double dx = cam_[0] - (x + 0.5), dy = cam_[1] - 1.62 - (y + 0.5), dz = cam_[2] - (z + 0.5);
                if (dx * dx + dy * dy + dz * dz < 16.0 * 16.0) {
                    if (const Data* data = dataAt(x, y, z); data && data->entity >= 0 && particle) {  // with a display entity
                        const double px = x + rndf(), py = y + rndf(), pz = z + rndf();
                        particle("smoke", px, py, pz, 0, 0, 0);
                        particle("flame", px, py, pz, 0, 0, 0);
                    }
                    if (a.delay > 0) --a.delay;
                    a.spin = std::fmod(a.spin + 1000.0f / (float(std::max(a.delay, 0)) + 200.0f), 360.0f);
                }
                break;
            }
            case Kind::TrialSpawner: {  // TrialSpawner.tickClient (the spawn delay is not known here: 0)
                ++a.tickCount;
                a.oSpin = a.spin;
                if (d.variant) a.spin = std::fmod(a.spin + (d.variant == 2 ? 1000.0f : 200.0f) / 200.0f, 360.0f);
                // TrialSpawnerState.emitParticles by trial_spawner_state, and the ambient sound while it can spawn.
                const std::string& n = registry_.name(block);
                const bool ominous = n.find("ominous=true") != std::string::npos;
                auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
                auto around = [&](double r, double out[3]) {  // Vec3.atCenterOf(pos).offsetRandom(random, r)
                    out[0] = x + 0.5 + (rndf() - 0.5) * r, out[1] = y + 0.5 + (rndf() - 0.5) * r, out[2] = z + 0.5 + (rndf() - 0.5) * r;
                };
                double p[3];
                if (particle) {
                    if (has("state=waiting_for_players") || has("state=waiting_for_reward_ejection") || has("state=ejecting_reward")) {
                        if (rnd(2) == 0) around(0.9, p), particle(ominous ? "soul_fire_flame" : "small_flame", p[0], p[1], p[2], 0, 0, 0);
                    } else if (has("state=active")) {
                        around(1.0, p);
                        particle("smoke", p[0], p[1], p[2], 0, 0, 0);
                        particle(ominous ? "soul_fire_flame" : "flame", p[0], p[1], p[2], 0, 0, 0);
                    } else if (has("state=cooldown")) {
                        if (around(0.9, p), rnd(3) == 0) particle("smoke", p[0], p[1], p[2], 0, 0, 0);
                        if (gameTicks_ % 20 == 0)
                            for (int i = 20 + rnd(4); i > 0; --i) particle("smoke", x + 0.5, y + 1.0, z + 0.5, 0, 0, 0);
                    }
                }
                if ((has("state=waiting_for_players") || has("state=active")) && rndf() <= 0.02)
                    if (audio::AudioSystem* au = audio::instance())
                        au->play(ominous ? "minecraft:block.trial_spawner.ambient_ominous" : "minecraft:block.trial_spawner.ambient",
                                 audio::Category::Block, x + 0.5, y + 0.5, z + 0.5, float(rndf()) * 0.25f + 0.75f, float(rndf()) + 0.5f);
                break;
            }
            case Kind::Vault: {  // VaultBlockEntity.Client.tick
                a.oSpin = a.spin;  // VaultClientData.updateDisplayItemSpin
                a.spin = std::remainder(a.spin + 10.0f, 360.0f);
                const Data* data = dataAt(x, y, z);
                const bool active = data && !data->items[0].empty();  // shouldDisplayActiveEffects: a display item
                const std::string& n = registry_.name(block);
                const bool ominous = n.find("ominous=true") != std::string::npos;
                if (particle && data && gameTicks_ % 20 == 0 && !data->connected.empty() && playerPosition) {
                    // emitConnectionParticlesForNearbyPlayers: from the keyhole towards each connected player in range
                    int fx = 0, fz = 0;
                    if (n.find("facing=north") != std::string::npos) fz = -1;
                    else if (n.find("facing=south") != std::string::npos) fz = 1;
                    else if (n.find("facing=west") != std::string::npos) fx = -1;
                    else fx = 1;
                    const double kx = x + 0.5 + fx * 0.5, ky = y + 1.75, kz = z + 0.5 + fz * 0.5;
                    for (const std::string& uuid : data->connected) {
                        double pp[3];
                        float height;
                        if (!playerPosition(uuid, pp, height)) continue;
                        const double bx = std::floor(pp[0]) - x, by = std::floor(pp[1]) - y, bz = std::floor(pp[2]) - z;
                        if (bx * bx + by * by + bz * bz > data->connectedRange * data->connectedRange) continue;
                        const double dir[3] = {pp[0] - kx, pp[1] + height / 2.0 - ky, pp[2] - kz};
                        for (int i = 2 + rnd(4); i > 0; --i)  // Mth.nextInt(2, 5); Vec3.offsetRandom(random, 1)
                            particle("vault_connection", kx, ky, kz, dir[0] + rndf() - 0.5, dir[1] + rndf() - 0.5, dir[2] + rndf() - 0.5);
                    }
                }
                if (particle && rndf() <= 0.5) {  // emitIdleParticles: inside the cage
                    const double px = x + 0.1 + rndf() * 0.8, py = y + 0.25 + rndf() * 0.5, pz = z + 0.1 + rndf() * 0.8;
                    particle("smoke", px, py, pz, 0, 0, 0);
                    if (active) particle(ominous ? "soul_fire_flame" : "small_flame", px, py, pz, 0, 0, 0);
                }
                if (active && rndf() <= 0.02)  // playIdleSounds
                    if (audio::AudioSystem* au = audio::instance())
                        au->play("minecraft:block.vault.ambient", audio::Category::Block, x + 0.5, y + 0.5, z + 0.5, float(rndf()) * 0.25f + 0.75f,
                                 float(rndf()) + 0.5f);
                break;
            }
            default: keep = false; break;
        }
        // Static <-> per-frame transitions rebuild the section batch.
        Instance probe{x, y, z, block, 0, -1};
        const bool dyn = keep && d.kind != Kind::None && isDynamic(probe, d);
        if (dyn != a.dynamic) {
            a.dynamic = dyn;
            if (auto s = sections_.find(sectionKey(x >> 4, y >> 4, z >> 4)); s != sections_.end()) s->second.dirty = true;
        }
        if (!keep && !ticking(d) && !a.dynamic) it = anims_.erase(it);
        else ++it;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Beacons (BeaconBlockEntity.tick: beam sections and pyramid levels; the whole column each tick)

void BlockEntityRenderer::beaconTick(int x, int y, int z, Anim& a) {
    auto baseName = [&](u16 b) {
        std::string_view n = registry_.name(b);
        return n.substr(0, n.find('['));
    };
    std::vector<std::pair<u32, int>> sections;
    const int top = (world_.maxSectionY() + 1) * 16 - 1;
    for (int cy = y; cy <= top; ++cy) {
        const u16 b = world_.block(x, cy, z);
        const std::string_view n = stripNs(baseName(b));
        int dye = n == "beacon" ? 0 : -1;  // BeaconBeamBlock: the beacon (white), stained glass and panes
        if (n.ends_with("_stained_glass")) dye = dyeId(n.substr(0, n.size() - 14));
        else if (n.ends_with("_stained_glass_pane")) dye = dyeId(n.substr(0, n.size() - 19));
        if (dye >= 0) {
            const u32 color = kDyeDiffuse[dye];
            if (sections.size() <= 1) {
                sections.push_back({color, 1});
            } else if (color == sections.back().first) {
                ++sections.back().second;
            } else {  // ARGB.average
                const u32 c = sections.back().first;
                u32 avg = 0;
                for (int sh = 0; sh < 24; sh += 8) avg |= ((((c >> sh) & 255) + ((color >> sh) & 255)) / 2) << sh;
                sections.push_back({avg, 1});
            }
        } else {
            if (sections.empty() || (registry_.infos()[b].opacity >= 15 && n != "bedrock")) {
                sections.clear();
                break;
            }
            ++sections.back().second;
        }
    }
    a.beam = std::move(sections);
    // updateBase every 80 ticks (and when first seen) while there is a beam.
    if ((gameTicks_ % 80 == 0 || a.tickCount++ == 0) && !a.beam.empty()) {
        static const char* kBase[5] = {"minecraft:iron_block", "minecraft:gold_block", "minecraft:emerald_block",
                                       "minecraft:diamond_block", "minecraft:netherite_block"};
        auto isBase = [&](u16 b) {
            const std::string_view n = baseName(b);
            if (rules_ && rules_->loaded()) return rules_->inTag("minecraft:beacon_base_blocks", n);
            return std::find(std::begin(kBase), std::end(kBase), n) != std::end(kBase);
        };
        int levels = 0;
        for (int step = 1; step <= 4; levels = step++) {
            const int ly = y - step;
            if (ly < world_.minSectionY() * 16) break;
            bool ok = true;
            for (int lx = x - step; lx <= x + step && ok; ++lx)
                for (int lz = z - step; lz <= z + step && ok; ++lz) ok = isBase(world_.block(lx, ly, lz));
            if (!ok) break;
        }
        a.levels = levels;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Pistons: the client half of PistonBaseBlock.triggerEvent / moveBlocks and PistonStructureResolver, against the
// blocks as they were when the event arrived. The moved blocks wait for the server's moving_piston blocks.

void BlockEntityRenderer::applyBlockEvents() {
    std::vector<NetEvent> events;
    {
        std::lock_guard lock(netMutex_);
        events.swap(netEvents_);
    }
    for (const NetEvent& e : events) {
        pistonEvent(e.x, e.y, e.z, e.type, e.param);
        handleEvent(e.x, e.y, e.z, e.type, e.param);
    }
    // PistonMovingBlockEntity.tick: two ticks moving, five more on the client, then the moved block is placed (unless
    // the server's own block arrived first and replaced the moving_piston; drawn a little longer then, while the
    // terrain remeshes).
    const double now = nowSeconds(), tickSeconds = 1.0 / std::max(double(tickRate_), 1.0);
    std::vector<net::BlockChange> edits;
    std::erase_if(moving_, [&](Moving& m) {
        if (!registry_.name(world_.block(m.x, m.y, m.z)).starts_with("minecraft:moving_piston")) {
            if (m.gone < 0) m.gone = now;
            return now - m.gone > 0.05;
        }
        if (now - m.start < 7 * tickSeconds) return false;
        edits.push_back({m.x, m.y, m.z, m.moved});
        return true;
    });
    if (!edits.empty() && blockSink_) blockSink_(edits);
}

void BlockEntityRenderer::pistonEvent(int px, int py, int pz, int type, int param) {
    const u16 pistonState = world_.block(px, py, pz);
    static const bool debug = std::getenv("MCPP_BE_DEBUG") != nullptr;
    if (debug)
        LOG("be: block event %d %d %d type %d param %d on %s (rules %d)", px, py, pz, type, param,
            registry_.name(pistonState).c_str(), rules_ && rules_->loaded() ? 1 : 0);
    if (type < 0 || type > 2 || !rules_ || !rules_->loaded() || !blockSink_) return;
    auto baseName = [&](u16 b) {
        std::string_view n = registry_.name(b);
        return n.substr(0, n.find('['));
    };
    const std::string_view pistonName = baseName(pistonState);
    const bool sticky = pistonName == "minecraft:sticky_piston";
    if (!sticky && pistonName != "minecraft:piston") return;
    const int dir = dirFromName(registry_.property(pistonState, "facing"));

    struct P {
        int x, y, z;
        bool operator==(const P&) const = default;
    };
    auto rel = [](P p, int d, int n = 1) { return P{p.x + stepX(d) * n, p.y + stepY(d) * n, p.z + stepZ(d) * n}; };
    const P piston{px, py, pz};
    // The client's level changes as the game's client makes them (applied together at the end; reads see them).
    std::vector<net::BlockChange> edits;
    auto get = [&](P p) -> u16 {
        for (size_t i = edits.size(); i-- > 0;)
            if (edits[i].x == p.x && edits[i].y == p.y && edits[i].z == p.z) return edits[i].id;
        return world_.block(p.x, p.y, p.z);
    };
    auto set = [&](P p, u16 id) { edits.push_back({p.x, p.y, p.z, id}); };
    const u16 air = registry_.idFor("minecraft:air");
    auto movingPiston = [&](bool stickyType) {
        return registry_.idFor(std::string("minecraft:moving_piston[facing=") + kDirNames[dir] + ",type=" +
                               (stickyType ? "sticky" : "normal") + "]");
    };
    auto isMovingPiston = [&](u16 b) { return baseName(b) == "minecraft:moving_piston"; };
    auto isAir = [&](u16 b) {
        const std::string_view n = baseName(b);
        return b == 0 || n == "minecraft:air" || n == "minecraft:cave_air" || n == "minecraft:void_air";
    };
    auto isPistonBlock = [&](u16 b) {
        const std::string_view n = baseName(b);
        return n == "minecraft:piston" || n == "minecraft:sticky_piston";
    };
    auto reaction = [&](u16 b) { return ItemRules::PushReaction(rules_->push(baseName(b)).reaction); };
    const int minY = world_.minSectionY() * 16, maxY = (world_.maxSectionY() + 1) * 16 - 1;
    auto pushable = [&](u16 s, P p, int direction, bool allowDestroyable, int connection) {  // PistonBaseBlock.isPushable
        if (p.y < minY || p.y > maxY) return false;
        if (isAir(s)) return true;
        if ((direction == 0 && p.y == minY) || (direction == 1 && p.y == maxY)) return false;
        if (!isPistonBlock(s)) {
            if (rules_->hardness(registry_.name(s)).destroyTime == -1.0f) return false;
            switch (reaction(s)) {
                case ItemRules::PushReaction::Immoveable: return false;
                case ItemRules::PushReaction::Popped: return allowDestroyable;
                case ItemRules::PushReaction::Push: return direction == connection;
                default: break;
            }
        } else if (registry_.property(s, "extended") == "true") {
            return false;
        }
        return !rules_->push(baseName(s)).blockEntity;
    };
    auto isSticky = [&](u16 b) {
        const std::string_view n = baseName(b);
        return n == "minecraft:slime_block" || n == "minecraft:honey_block";
    };
    auto canStick = [&](u16 a, u16 b) {
        const std::string_view na = baseName(a), nb = baseName(b);
        if ((na == "minecraft:honey_block" && nb == "minecraft:slime_block") ||
            (na == "minecraft:slime_block" && nb == "minecraft:honey_block"))
            return false;
        return isSticky(a) || isSticky(b);
    };
    const double now = nowSeconds();
    auto add = [&](P at, u16 moved, bool extending, bool source) {  // MovingPistonBlock.newMovingBlockEntity
        std::erase_if(moving_, [&](const Moving& m) { return m.x == at.x && m.y == at.y && m.z == at.z; });
        moving_.push_back({at.x, at.y, at.z, moved, u8(dir), extending, source, now, -1.0});
        if (debug)
            LOG("be: piston %d %d %d event %d: %s moves to %d %d %d%s", px, py, pz, type, registry_.name(moved).c_str(), at.x,
                at.y, at.z, source ? " (source)" : "");
    };
    // PistonMovingBlockEntity.finalTick: the moved block in place now (a piston's own head: air).
    auto finalTick = [&](P at, bool requireExtending) {
        for (auto it = moving_.begin(); it != moving_.end(); ++it) {
            if (it->x != at.x || it->y != at.y || it->z != at.z) continue;
            if (requireExtending && (it->dir != dir || !it->extending)) return false;
            if (isMovingPiston(get(at))) set(at, it->source ? air : it->moved);
            moving_.erase(it);
            return true;
        }
        return false;
    };

    auto moveBlocks = [&](bool extending) {
        const P arm = rel(piston, dir);
        if (!extending && baseName(get(arm)) == "minecraft:piston_head") set(arm, air);
        // PistonStructureResolver
        const int pushDir = extending ? dir : opposite(dir);
        const P start = extending ? arm : rel(piston, dir, 2);
        std::vector<P> toPush, toDestroy;
        auto indexOf = [&](P p) {
            for (size_t i = 0; i < toPush.size(); ++i)
                if (toPush[i] == p) return int(i);
            return -1;
        };
        std::function<bool(P)> addBranching;
        std::function<bool(P, int)> addLine = [&](P from, int direction) {
            u16 next = get(from);
            if (isAir(next) || !pushable(next, from, pushDir, false, direction) || from == piston || indexOf(from) >= 0)
                return true;
            int count = 1;
            if (count + toPush.size() > 12) return false;
            while (isSticky(next)) {
                const P p = rel(from, opposite(pushDir), count);
                const u16 previous = next;
                next = get(p);
                if (isAir(next) || !canStick(previous, next) || !pushable(next, p, pushDir, false, opposite(pushDir)) ||
                    p == piston)
                    break;
                if (++count + toPush.size() > 12) return false;
            }
            int added = 0;
            for (int i = count - 1; i >= 0; --i) toPush.push_back(rel(from, opposite(pushDir), i)), ++added;
            for (int i = 1;; ++i) {
                const P p = rel(from, pushDir, i);
                const int collision = indexOf(p);
                if (collision > -1) {  // reorderListAtCollision
                    std::vector<P> order(toPush.begin(), toPush.begin() + collision);
                    order.insert(order.end(), toPush.end() - added, toPush.end());
                    order.insert(order.end(), toPush.begin() + collision, toPush.end() - added);
                    toPush = std::move(order);
                    for (int j = 0; j <= collision + added; ++j)
                        if (isSticky(get(toPush[size_t(j)])) && !addBranching(toPush[size_t(j)])) return false;
                    return true;
                }
                next = get(p);
                if (isAir(next)) return true;
                if (!pushable(next, p, pushDir, true, pushDir) || p == piston) return false;
                if (reaction(next) == ItemRules::PushReaction::Popped) {
                    toDestroy.push_back(p);
                    return true;
                }
                if (toPush.size() >= 12) return false;
                toPush.push_back(p);
                ++added;
            }
        };
        addBranching = [&](P from) {
            const u16 state = get(from);
            for (int d = 0; d < 6; ++d) {
                if (d >> 1 == pushDir >> 1) continue;
                const P n = rel(from, d);
                if (canStick(get(n), state) && !addLine(n, d)) return false;
            }
            return true;
        };
        const u16 first = get(start);
        if (!pushable(first, start, pushDir, false, dir)) {
            if (!extending || reaction(first) != ItemRules::PushReaction::Popped) return false;
            toDestroy.push_back(start);
        } else {
            if (!addLine(start, pushDir)) return false;
            for (size_t i = 0; i < toPush.size(); ++i)
                if (isSticky(get(toPush[i])) && !addBranching(toPush[i])) return false;
        }
        // moveBlocks: destroyed blocks go, the pushed ones become moving_piston blocks at their destinations, then the
        // extending head; the cells left behind are cleared.
        std::vector<u16> states;
        for (const P& p : toPush) states.push_back(get(p));
        std::vector<P> left = toPush;
        for (size_t i = toDestroy.size(); i-- > 0;) set(toDestroy[i], air);
        for (size_t i = toPush.size(); i-- > 0;) {
            const P dest = rel(toPush[i], pushDir);
            std::erase(left, dest);
            set(dest, movingPiston(false));
            add(dest, states[i], extending, false);
        }
        if (extending) {
            std::erase(left, arm);
            set(arm, movingPiston(sticky));
            add(arm,
                registry_.idFor(std::string("minecraft:piston_head[facing=") + kDirNames[dir] +
                                ",short=false,type=" + (sticky ? "sticky" : "normal") + "]"),
                true, true);
        }
        for (const P& p : left) set(p, air);
        return true;
    };

    if (type == 0) {
        if (moveBlocks(true)) set(piston, registry_.withProperty(pistonState, "extended", "true"));
    } else {
        // Retracting: the base becomes a moving piston carrying the retracted piston (the head is drawn from it).
        finalTick(rel(piston, dir), false);
        set(piston, movingPiston(sticky));
        add(piston,
            registry_.withProperty(registry_.withProperty(pistonState, "extended", "false"), "facing",
                                   kDirNames[std::clamp(param & 7, 0, 5)]),
            false, true);
        const P arm = rel(piston, dir);
        if (sticky) {
            const P two = rel(piston, dir, 2);
            const u16 at = get(two);
            const bool piece = isMovingPiston(at) && finalTick(two, true);
            if (!piece) {
                if (type != 1 || isAir(at) || !pushable(at, two, opposite(dir), false, dir) ||
                    (reaction(at) != ItemRules::PushReaction::PushPull && !isPistonBlock(at)))
                    set(arm, air);
                else
                    moveBlocks(false);
            }
        } else {
            set(arm, air);
        }
    }
    if (!edits.empty()) blockSink_(edits);
}

void BlockEntityRenderer::pistonMovers(std::vector<Physics::Piston>& out) {
    out.clear();
    for (const Moving& m : moving_) {
        if (m.gone >= 0 || !registry_.name(world_.block(m.x, m.y, m.z)).starts_with("minecraft:moving_piston")) continue;
        Physics::Piston p;
        p.x = m.x, p.y = m.y, p.z = m.z, p.moved = m.moved, p.dir = m.dir, p.extending = m.extending, p.source = m.source;
        u64 bits;
        std::memcpy(&bits, &m.start, sizeof(bits));
        p.key = bits ^ (u64(u32(m.x)) * 0x9E3779B97F4A7C15ull) ^ (u64(u32(m.y)) << 20) ^ (u64(u32(m.z)) << 40);
        const std::string facing = kDirNames[m.dir];
        for (int s = 0; s < 2; ++s)
            p.head[s] = registry_.idFor("minecraft:piston_head[facing=" + facing + ",short=" + (s ? "true" : "false") + ",type=normal]");
        const std::string& moved = registry_.name(m.moved);
        if (m.source && (moved.starts_with("minecraft:piston[") || moved.starts_with("minecraft:sticky_piston["))) {
            const std::string own(registry_.property(m.moved, "facing"));
            const bool sticky = moved.starts_with("minecraft:sticky_piston");
            for (int s = 0; s < 2; ++s)
                p.headMoved[s] = registry_.idFor("minecraft:piston_head[facing=" + own + ",short=" + (s ? "true" : "false") +
                                                 ",type=" + (sticky ? "sticky" : "normal") + "]");
            p.extendedBase = registry_.withProperty(m.moved, "extended", "true");
        }
        out.push_back(p);
    }
}

float BlockEntityRenderer::movingProgress(const Moving& m, double now) const {
    return float(std::clamp((now - m.start) * tickRate_ / 2.0, 0.0, 1.0));  // 0.5 per tick
}

// ---------------------------------------------------------------------------------------------------------
// Geometry

namespace {

// Appends a model layer's quads, posed by `poses` (or the initial pose), under `root`.
struct ModelEmit {
    const ent::EntityAssets& assets;
    std::vector<BeQuad>& out;
    int model;
    M34 root;
    int tex;
    u32 color = 0xFFFFFFFF, light = 0;
    const ent::PartPose* poses = nullptr;
    u64 skip = 0;                 // parts to leave out (bit per part index)
    const int* partTex = nullptr;  // per-part texture override (-1: tex)
    bool wave = false;            // banner flag: wave the "flag" part in the shader
    u32 wavePhase = 0;
    float vScale = 1.0f;          // texture frame (animated textures: the first frame)

    void run() const {
        if (model < 0 || tex < 0) return;
        const ent::Model& m = assets.modelAt(model);
        const auto& quads = assets.quads();
        M34 mats[64];
        const size_t parts = std::min<size_t>(m.parts.size(), 64);
        for (size_t i = 0; i < parts; ++i) {
            const ent::ModelPart& p = m.parts[i];
            const M34 local = partLocal(poses ? poses[i] : p.pose);
            mats[i] = mul(p.parent >= 0 ? mats[size_t(p.parent)] : root, local);
        }
        for (size_t i = 0; i < parts; ++i) {
            if ((skip >> i) & 1) continue;
            const ent::ModelPart& p = m.parts[i];
            if (p.parent >= 0 && ((skip >> p.parent) & 1)) continue;
            const int t = partTex && partTex[i] >= 0 ? partTex[i] : tex;
            const ent::TexRect& r = assets.rect(t);
            const M34& mm = mats[i];
            u32 waveBits = 0;
            float pivot[3] = {0, 0, 0};
            if (wave && p.name == "flag") {
                pivot[0] = mm.m[3], pivot[1] = mm.m[7], pivot[2] = mm.m[11];
                const float ax = mm.m[0], az = mm.m[8];
                const float ang = std::atan2(az, ax);
                waveBits = (1u << 31) | ((wavePhase % 100) << 16) | (u32(std::lround((ang < 0 ? ang + 2 * kPi : ang) / (2 * kPi) * 65536.0f)) & 0xFFFF);
            }
            for (u32 q = p.quadFirst; q < p.quadFirst + p.quadCount; ++q) {
                const ent::GpuQuad& g = quads[q];
                BeQuad b;
                for (int c = 0; c < 4; ++c) {
                    const float v[3] = {g.pos[c][0] / 16.0f, g.pos[c][1] / 16.0f, g.pos[c][2] / 16.0f};
                    xform(mm, v, b.pos[c]);
                    b.uv[c][0] = r.u0 + g.uv[c][0] * r.du;
                    b.uv[c][1] = r.v0 + g.uv[c][1] * r.dv * vScale;
                }
                b.normal = packNormal(mm, g.normal);
                b.color = color, b.light = light, b.tex = kTexAtlas;
                std::copy(pivot, pivot + 3, b.pivot);
                b.wave = waveBits;
                out.push_back(b);
            }
        }
    }
};

int partIndex(const ent::Model& m, std::string_view name) {
    for (size_t i = 0; i < m.parts.size(); ++i)
        if (m.parts[i].name == name) return int(i);
    return -1;
}

// Block faces / model quads of a block state in block-local [0,1] space (items shown as blocks).
void emitBlockGeometry(const BlockRegistry& reg, u16 block, const M34& m, u32 light, std::vector<BeQuad>& out) {
    const BlockInfo& b = reg.infos()[block];
    static const u32 kTints[8] = {0xFFFFFF, 0x91BD59, 0x77AB2F, 0x80A755, 0x619961, 0x3F76E4, 0x208030, 0x8B1A00};
    static const float kNormals[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    // Double-sided (both windings): the block's own face winding does not matter here.
    auto push = [&](const float pos[4][3], const float uv[4][2], const float n[3], u32 tex, u32 tint) {
        BeQuad q{};
        for (int c = 0; c < 4; ++c) {
            xform(m, pos[c], q.pos[c]);
            q.uv[c][0] = uv[c][0], q.uv[c][1] = uv[c][1];
        }
        q.normal = packNormal(m, n);
        q.color = abgr(kTints[tint & 7]);
        q.light = light;
        q.tex = tex;
        out.push_back(q);
        std::swap(q.pos[1], q.pos[3]);
        std::swap(q.uv[1], q.uv[3]);
        const float back[3] = {-n[0], -n[1], -n[2]};
        q.normal = packNormal(m, back);
        out.push_back(q);
    };
    if (b.kind == KindCube) {
        for (int f = 0; f < 6; ++f) {
            const CubeFace& face = b.faces[f];
            const float* nrm = kNormals[f];
            const float axisA[3] = {f < 4 ? 1.0f : 0.0f, 0, f < 4 ? 0.0f : 1.0f};
            const float axisB[3] = {0, f < 2 ? 0.0f : 1.0f, f < 2 ? 1.0f : 0.0f};
            float pos[4][3], uv[4][2];
            static const int kOrder[4] = {0, 1, 3, 2};
            for (int c = 0; c < 4; ++c) {
                const int corner = kOrder[c];
                const float a = float(corner & 1), bb = float(corner >> 1);
                for (int k = 0; k < 3; ++k) pos[c][k] = std::max(nrm[k], 0.0f) + axisA[k] * a + axisB[k] * bb;
                const float* p = pos[c];
                float u, v;
                switch (f) {
                    case 0: u = p[0], v = -p[2]; break;
                    case 1: u = p[0], v = p[2]; break;
                    case 2: u = -p[0], v = -p[1]; break;
                    case 3: u = p[0], v = -p[1]; break;
                    case 4: u = p[2], v = -p[1]; break;
                    default: u = -p[2], v = -p[1]; break;
                }
                const int rot = face.rot;
                uv[c][0] = rot == 0 ? u : rot == 1 ? v : rot == 2 ? -u : -v;
                uv[c][1] = rot == 0 ? v : rot == 1 ? -u : rot == 2 ? -v : u;
            }
            push(pos, uv, nrm, face.tex, face.tint);
        }
    } else if (b.kind == KindModel) {
        for (u32 i = 0; i < b.quadCount; ++i) {
            const ModelQuad& q = reg.modelQuad(b.quadFirst + i);
            float pos[4][3], uv[4][2];
            static const int kOrder[4] = {0, 1, 3, 2};  // Z order -> perimeter
            for (int c = 0; c < 4; ++c) {
                std::copy(q.pos[kOrder[c]], q.pos[kOrder[c]] + 3, pos[c]);
                uv[c][0] = q.uv[kOrder[c]][0], uv[c][1] = q.uv[kOrder[c]][1];
            }
            const float e1[3] = {q.pos[1][0] - q.pos[0][0], q.pos[1][1] - q.pos[0][1], q.pos[1][2] - q.pos[0][2]};
            const float e2[3] = {q.pos[2][0] - q.pos[0][0], q.pos[2][1] - q.pos[0][1], q.pos[2][2] - q.pos[0][2]};
            const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            push(pos, uv, n, q.tex, q.tint);
        }
    }
}

// Block faces drawn like the terrain draws them: the per-direction shade (top 1, bottom 0.5, z 0.8, x 0.6) in the
// colour instead of the entity lights.
void shadeBlockFaces(std::vector<BeQuad>& quads, size_t first) {
    for (size_t i = first; i < quads.size(); ++i) {
        BeQuad& q = quads[i];
        const float nx = float(i8(q.normal & 255)) / 127.0f, ny = float(i8((q.normal >> 8) & 255)) / 127.0f,
                    nz = float(i8((q.normal >> 16) & 255)) / 127.0f;
        const float shade = std::min(1.0f, nx * nx * 0.6f + ny * ny * ((3.0f + ny) / 4.0f) + nz * nz * 0.8f);
        u32 c = q.color & 0xFF000000u;
        for (int sh = 0; sh < 24; sh += 8) c |= u32(float((q.color >> sh) & 255) * shade + 0.5f) << sh;
        q.color = c;
        q.light |= 512;
    }
}

// Appends a quad textured with a repeating entity atlas texture (kTexAtlasRepeat).
void pushRepeat(std::vector<BeQuad>& out, const float pos[4][3], const float uv[4][2], const ent::TexRect& r, u32 color,
                u32 light) {
    BeQuad q{};
    for (int c = 0; c < 4; ++c) {
        std::copy(pos[c], pos[c] + 3, q.pos[c]);
        q.uv[c][0] = uv[c][0], q.uv[c][1] = uv[c][1];
    }
    q.pivot[0] = r.u0, q.pivot[1] = r.v0, q.pivot[2] = r.du;
    std::memcpy(&q.normal, &r.dv, 4);
    q.color = color, q.light = light, q.tex = kTexAtlasRepeat;
    out.push_back(q);
}

}  // namespace

// A block moved by a piston (PistonHeadRenderer): the moved state (a short head while it is mostly retracted) at its
// offset; a retracting piston draws its head moving in and the extended base in place.
void BlockEntityRenderer::emitMoving(Emit& e, const Moving& m, double now) const {
    const float progress = movingProgress(m, now);
    const float ext = m.extending ? progress - 1.0f : 1.0f - progress;  // getExtendedProgress
    u16 block = m.moved, base = 0;
    const std::string_view name = registry_.name(m.moved);
    if (name.starts_with("minecraft:piston_head")) {
        block = registry_.withProperty(m.moved, "short", progress <= 0.5f ? "true" : "false");
    } else if (m.source && !m.extending) {
        const std::string head = std::string("minecraft:piston_head[facing=") + std::string(registry_.property(m.moved, "facing")) +
                                 ",short=" + (progress >= 0.5f ? "true" : "false") +
                                 ",type=" + (name.starts_with("minecraft:sticky_piston") ? "sticky" : "normal") + "]";
        block = registry_.idFor(head);
        base = registry_.withProperty(m.moved, "extended", "true");
    }
    // Light per face from the cell it faces (like flat block lighting; the moving cell itself is dark once the
    // final block is in place), at least the moving cell's own.
    const u8 center = sampleLight(m.x, m.y, m.z);
    u8 faceLight[6];
    for (int d = 0; d < 6; ++d) {
        const u8 l = sampleLight(m.x + stepX(d), m.y + stepY(d), m.z + stepZ(d));
        faceLight[d] = u8(std::max(l & 0xF0, center & 0xF0) | std::max(l & 15, center & 15));
    }
    const float o[3] = {float(m.x - e.origin[0]), float(m.y - e.origin[1]), float(m.z - e.origin[2])};
    thread_local std::vector<BeQuad> quads;
    for (int k = 0; k < 2; ++k) {
        const u16 b = k ? base : block;
        if (!b) continue;
        quads.clear();
        const M34 t = k ? translate(o[0], o[1], o[2])
                        : translate(o[0] + stepX(m.dir) * ext, o[1] + stepY(m.dir) * ext, o[2] + stepZ(m.dir) * ext);
        emitBlockGeometry(registry_, b, t, center, quads);
        for (BeQuad& q : quads) {
            const int n[3] = {i8(q.normal & 255), i8((q.normal >> 8) & 255), i8((q.normal >> 16) & 255)};
            const int axis = std::abs(n[0]) >= std::abs(n[1]) && std::abs(n[0]) >= std::abs(n[2]) ? 0 : std::abs(n[1]) >= std::abs(n[2]) ? 1 : 2;
            const int d = axis == 0 ? (n[0] < 0 ? 4 : 5) : axis == 1 ? (n[1] < 0 ? 0 : 1) : (n[2] < 0 ? 2 : 3);
            q.light = faceLight[d];
        }
        shadeBlockFaces(quads, 0);
        // Translucent blocks (slime, honey, stained glass) blend in the translucent pass; the shader pipeline's forward
        // layer keeps one translucent layer per pixel, so there they draw as cutout.
        std::vector<BeQuad>& out = *e.out[registry_.infos()[b].layer == LayerTranslucent && !shading_ ? 2 : 0];
        out.insert(out.end(), quads.begin(), quads.end());
    }
}

// BeaconRenderer.submitBeaconBeam: the solid beam turning inside a translucent glow, both scrolling.
void BlockEntityRenderer::emitBeam(Emit& e, const M34& base, int tex, float scale, float animTime, int start, int height,
                                   u32 color, float solidRadius, float glowRadius) const {
    if (tex < 0) return;
    const ent::TexRect& r = assets_.rect(tex);
    const float y0 = float(start), y1 = float(start + height);
    const float scroll = height < 0 ? animTime : -animTime;
    const float vOff = scroll * 0.2f - std::floor(scroll * 0.1f);
    const float v2 = -1.0f + (vOff - std::floor(vOff));
    const u32 light = 256 | 512 | 2048;  // full bright, unshaded, no shadow
    // renderPart: four quads around the axis from the corners (wn, en, ws, es); u 0..1, v1 at the top, v2 at the bottom.
    auto part = [&](const M34& m, std::vector<BeQuad>& out, u32 c, const float (&k)[8], float v1) {
        const int sides[4][2] = {{0, 1}, {3, 2}, {1, 3}, {2, 0}};  // (wn, en), (es, ws), (en, es), (ws, wn)
        for (const auto& sd : sides) {
            const float ax = k[sd[0] * 2], az = k[sd[0] * 2 + 1], bx = k[sd[1] * 2], bz = k[sd[1] * 2 + 1];
            const float p[4][3] = {{ax, y1, az}, {ax, y0, az}, {bx, y0, bz}, {bx, y1, bz}};
            const float uv[4][2] = {{1, v1}, {1, v2}, {0, v2}, {0, v1}};
            float pos[4][3];
            for (int i = 0; i < 4; ++i) xform(m, p[i], pos[i]);
            pushRepeat(out, pos, uv, r, c, light);
        }
    };
    const M34 m = mul(base, translate(0.5f, 0, 0.5f));
    const float sr = solidRadius, gr = glowRadius;
    const float solid[8] = {0, sr, sr, 0, -sr, 0, 0, -sr};
    part(mul(m, rotY((animTime * 2.25f - 45.0f) * kDeg)), *e.out[0], abgr(color), solid,
         float(height) * scale * (0.5f / sr) + v2);
    const float glow[8] = {-gr, -gr, gr, -gr, -gr, gr, gr, gr};
    part(m, *e.out[2], abgr(color, 32), glow, float(height) * scale + v2);
}

// An item as the item renderer shows it in a display context: block items as their block, others as a flat sprite.
// context: 0 fixed, 1 on_shelf, 2 ground.
void BlockEntityRenderer::emitItem(Emit& e, const std::string& item, const M34& m, u32 light, int context) const {
    if (itemDrawer_) {  // the item drawer shows it (shownItems)
        if (shownOut_ && shownNbt_ && !shownNbt_->empty()) {
            ShownItem s;
            s.nbt = shownNbt_, s.context = context, s.light = light, s.seed = shownSeed_;
            std::copy(m.m, m.m + 12, s.m);
            for (int a = 0; a < 3; ++a) s.origin[a] = int(e.origin[a]);
            shownOut_->push_back(s);
        }
        return;
    }
    auto ib = itemBlocks_.try_emplace(item, -1).first;
    if (ib->second < 0) {
        const std::string key = "minecraft:" + item;
        ib->second = reports_.hasBlock(key) ? registry_.idFor(reports_.complete(key)) : 0;
    }
    const u16 block = u16(ib->second);
    const BlockInfo* bi = block ? &registry_.infos()[block] : nullptr;
    const bool asBlock = bi && (bi->kind == KindCube || (bi->kind == KindModel && bi->layer != LayerCutout && bi->quadCount));
    if (asBlock) {
        // block/block.json display: fixed = 0.5 scale, on_shelf = 180 degrees about Y, ground = 3 px up, 0.25 scale.
        const M34 disp = context == 0 ? scale(0.5f) : context == 1 ? rotY(kPi) : mul(translate(0, 3.0f / 16, 0), scale(0.25f));
        emitBlockGeometry(registry_, block, mul(m, mul(disp, translate(-0.5f, -0.5f, -0.5f))), light, *e.out[0]);
        return;
    }
    int tex = assets_.itemTexture(item);
    u32 layer = kTexAtlas;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (tex >= 0) {
        const ent::TexRect& r = assets_.rect(tex);
        u0 = r.u0, v0 = r.v0, u1 = r.u0 + r.du, v1 = r.v0 + r.dv;
    } else if (bi && bi->icon) {
        layer = bi->icon;
    } else {
        return;
    }
    // item/generated display: fixed = 180 degrees about Y, ground = 2 px up, 0.5 scale; a 1/16 thick sprite, both faces.
    const M34 im = context == 0 ? mul(m, rotY(kPi)) : context == 1 ? m : mul(m, mul(translate(0, 2.0f / 16, 0), scale(0.5f)));
    for (int face = 0; face < 2; ++face) {
        const float z = face ? -1.0f / 32 : 1.0f / 32;
        float pos[4][3] = {{-0.5f, -0.5f, z}, {0.5f, -0.5f, z}, {0.5f, 0.5f, z}, {-0.5f, 0.5f, z}};
        float uv[4][2] = {{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}};
        if (face) {
            std::swap(pos[0], pos[1]), std::swap(pos[2], pos[3]);
            std::swap(uv[0], uv[1]), std::swap(uv[2], uv[3]);
        }
        BeQuad q{};
        for (int c = 0; c < 4; ++c) {
            xform(im, pos[c], q.pos[c]);
            q.uv[c][0] = uv[c][0], q.uv[c][1] = uv[c][1];
        }
        const float n[3] = {0, 0, face ? -1.0f : 1.0f};
        q.normal = packNormal(im, n);
        q.color = 0xFFFFFFFF, q.light = light, q.tex = layer;
        e.out[0]->push_back(q);
    }
}

// A mob as the entity renderer would draw it standing still (spawner display entities): its model layers in the
// entity's own transform (LivingEntityRenderer: turned 180 degrees, flipped, the renderer's scale, 1.501 down).
void BlockEntityRenderer::emitDisplayEntity(Emit& e, int type, const M34& m, u32 light, float age) const {
    ent::EntityRenderer::Layer layers[8];
    ent::Anim anim = ent::Anim::None;
    float rendererScale = 1.0f;
    const int count = entityRenderer_.displayLayers(type, layers, 8, anim, rendererScale);
    if (!count) return;
    const M34 root = mul(m, mul(mul(rotY(kPi), scale(-1, -1, 1)), mul(scale(rendererScale), translate(0, -1.501f, 0))));
    ent::AnimState st;
    st.age = age;
    thread_local std::vector<ent::PartPose> poses;
    for (int i = 0; i < count; ++i) {
        const ent::EntityRenderer::Layer& L = layers[i];
        if (L.model < 0 || L.tex < 0) continue;
        const ent::Model& model = assets_.modelAt(L.model);
        poses.resize(model.parts.size());
        for (size_t p = 0; p < model.parts.size(); ++p) poses[p] = model.parts[p].pose;
        const u64 hiddenRoles = ent::animate(anim, st, model, poses.data());
        u64 skip = 0;
        for (size_t p = 0; p < model.parts.size() && p < 64; ++p)
            if (model.parts[p].role != ent::RoleNone && ((hiddenRoles >> model.parts[p].role) & 1)) skip |= 1ull << p;
        // entity_render.cpp LayerFlags: 1 emissive, 2 translucent
        ModelEmit me{assets_, *e.out[(L.flags & 2) ? 2 : 0], L.model, root, L.tex};
        me.color = L.color;
        me.light = (L.flags & 1) ? (light | 256u) : light;
        me.poses = poses.data();
        me.skip = skip;
        me.run();
    }
}


void BlockEntityRenderer::emitInstance(Emit& e, const Instance& in, const Data* data, float pt) const {
    const BlockDef& d = defs_[in.block];
    const M34 base = translate(float(in.x - e.origin[0]), float(in.y - e.origin[1]), float(in.z - e.origin[2]));
    const u32 light = in.light;
    auto it = anims_.find(posKey(in.x, in.y, in.z));
    const Anim* a = it == anims_.end() ? nullptr : &it->second;
    auto model = [&](int pass, int mdl, const M34& root, int tex) {
        ModelEmit me{assets_, *e.out[pass], mdl, root, tex};
        me.light = light;
        return me;
    };
    thread_local std::vector<ent::PartPose> poses;
    auto initPoses = [&](int mdl) -> ent::PartPose* {
        if (mdl < 0) return nullptr;
        const ent::Model& m = assets_.modelAt(mdl);
        poses.resize(m.parts.size());
        for (size_t i = 0; i < m.parts.size(); ++i) poses[i] = m.parts[i].pose;
        return poses.data();
    };
    auto setPart = [&](int mdl, std::string_view part, auto&& fn) {
        const int i = partIndex(assets_.modelAt(mdl), part);
        if (i >= 0) fn(poses[size_t(i)]);
    };

    switch (d.kind) {
        case Kind::Chest: {  // ChestRenderer
            int material = d.variant;
            if ((material == 0 || material == 1) && xmas_) material = 3;
            const int part = material == 2 ? 0 : d.part;  // ender chests are single
            const int mdl = part == 1 ? m_.chestLeft : part == 2 ? m_.chestRight : m_.chest;
            if (mdl < 0) return;
            float open = a ? a->oOpen + (a->open - a->oOpen) * pt : 0.0f;
            if (d.part) {  // ChestBlock.opennessCombiner: the more open half
                const int other = d.part == 1 ? clockwise(d.facing) : clockwise(clockwise(clockwise(d.facing)));
                auto o = anims_.find(posKey(in.x + stepX(other), in.y, in.z + stepZ(other)));
                if (o != anims_.end()) open = std::max(open, o->second.oOpen + (o->second.open - o->second.oOpen) * pt);
            }
            open = 1.0f - open;
            open = 1.0f - open * open * open;
            ent::PartPose* p = initPoses(mdl);
            setPart(mdl, "lid", [&](ent::PartPose& pp) { pp.xRot = -(open * kPi / 2); });
            setPart(mdl, "lock", [&](ent::PartPose& pp) { pp.xRot = -(open * kPi / 2); });
            const M34 root = mul(base, rotateAround(rotY(-yRotOf(d.facing) * kDeg), 0.5f, 0, 0.5f));
            ModelEmit me = model(0, mdl, root, t_.chest[material][part]);
            me.poses = p;
            me.run();
            break;
        }
        case Kind::Sign:
        case Kind::HangingSign: {  // StandingSignRenderer / HangingSignRenderer + AbstractSignRenderer
            if (!data || !font_) return;
            const bool hanging = d.kind == Kind::HangingSign;
            const float angle = d.wall ? yRotOf(d.facing) : float(d.rotation) * 22.5f;
            const int lineHeight = hanging ? 9 : 10, maxWidth = hanging ? 60 : 90;
            for (int side = 0; side < 2; ++side) {
                const SignSide& s = side ? data->back : data->front;
                bool any = false;
                for (const auto& l : s.lines) any |= !l.empty();
                if (!any) continue;
                M34 m;
                if (hanging) {
                    m = mul(base, mul(translate(0.5f, 0.9375f, 0.5f), mul(rotY(-angle * kDeg), translate(0, -0.3125f, 0))));
                    if (side) m = mul(m, rotY(kPi));
                    m = mul(m, mul(translate(0, -0.32f, 0.073f), scale(0.0140625f, -0.0140625f, 0.0140625f)));
                } else {
                    m = mul(base, mul(translate(0.5f, 0.5f, 0.5f), rotY(-angle * kDeg)));
                    if (d.wall) m = mul(m, translate(0, -0.3125f, -0.4375f));
                    if (side) m = mul(m, rotY(kPi));
                    m = mul(m, mul(translate(0, 0.33333334f, 0.046666667f), scale(0.010416667f, -0.010416667f, 0.010416667f)));
                }
                // AbstractSignRenderer.submitSignText
                const u32 dyeText = kDyeText[s.color];
                const u32 dark = s.color == 15 && s.glow ? 0xFFF0EBCCu
                                                         : 0xFF000000u | (u32(((dyeText >> 16) & 255) * 0.4f) << 16) |
                                                               (u32(((dyeText >> 8) & 255) * 0.4f) << 8) | u32((dyeText & 255) * 0.4f);
                const u32 textColor = s.glow ? 0xFF000000u | dyeText : dark;
                const u32 textLight = s.glow ? (0xFFu | 256u | 512u) : (light | 512u);
                const bool outline = s.glow;
                const u32 outlineLight = textLight | (s.color == 15 ? 0u : 1024u);  // non-black: within 16 blocks
                const int mid = 4 * lineHeight / 2;
                for (int i = 0; i < 4; ++i) {
                    if (s.lines[i].empty()) continue;
                    std::vector<ui::Text> split = font_->split(s.lines[i], maxWidth);
                    if (split.empty()) continue;
                    const ui::Text& line = split[0];
                    const int x = -font_->width(line) / 2;
                    const int y = i * lineHeight - mid;
                    auto glyphs = [&](float gx, float gy, u32 color, bool forceColor, u32 ql) {
                        canvas_.begin(1u << 14, 1u << 14, 1);
                        font_->draw(canvas_, line, gx, gy, color, false);
                        const auto& v = canvas_.vertices();
                        for (size_t k = 0; k + 5 < v.size(); k += 6) {
                            const ui::UiVertex* c[4] = {&v[k], &v[k + 1], &v[k + 2], &v[k + 5]};
                            BeQuad q{};
                            for (int j = 0; j < 4; ++j) {
                                const float p[3] = {c[j]->x, c[j]->y, 0};
                                xform(m, p, q.pos[j]);
                                q.uv[j][0] = c[j]->u, q.uv[j][1] = c[j]->v;
                            }
                            const float nz[3] = {0, 0, -1};
                            q.normal = packNormal(m, nz);
                            q.color = forceColor ? ui::argbToAbgr(color) : c[0]->color;
                            q.light = ql;
                            q.tex = c[0]->mode == ui::ModeSolid ? kTexSolid : kTexFont;
                            e.out[1]->push_back(q);
                        }
                    };
                    if (outline)  // Font 8x outline: the text in the outline colour at the 8 neighbouring offsets
                        for (int oy = -1; oy <= 1; ++oy)
                            for (int ox = -1; ox <= 1; ++ox)
                                if (ox || oy) glyphs(float(x + ox), float(y + oy), dark, true, outlineLight);
                    glyphs(float(x), float(y), textColor, false, textLight);
                }
            }
            break;
        }
        case Kind::Banner: {  // BannerRenderer
            const float angle = d.wall ? yRotOf(d.facing) : float(d.rotation) * 22.5f;
            const M34 root = mul(base, mul(translate(0.5f, 0, 0.5f), mul(rotY(-angle * kDeg), scale(0.6666667f, -0.6666667f, -0.6666667f))));
            const int pole = d.wall ? m_.bannerWall : m_.bannerStanding;
            const int flag = d.wall ? m_.bannerWallFlag : m_.bannerStandingFlag;
            const u32 phase = u32(((in.x * 7 + in.y * 9 + in.z * 13) % 100 + 100) % 100);
            model(0, pole, root, t_.bannerBase).run();
            ModelEmit f = model(0, flag, root, t_.bannerBase);
            f.wave = true, f.wavePhase = phase;
            f.run();
            // Pattern layers (bannerPattern: translucent), the base colour first.
            ModelEmit layer = model(2, flag, root, t_.bannerPatternBase);
            layer.wave = true, layer.wavePhase = phase;
            layer.color = abgr(kDyeDiffuse[std::max(0, d.variant - 1)]);
            layer.run();
            if (data)
                for (const auto& [tex, dye] : data->patterns) {
                    layer.tex = tex;
                    layer.color = abgr(kDyeDiffuse[dye & 15]);
                    layer.run();
                }
            break;
        }
        case Kind::Skull: {  // SkullBlockRenderer
            const int type = d.variant;
            const int mdl = m_.skull[type];
            int tex = t_.skull[type];
            if (type == 6 && data && data->skin >= 0) tex = data->skin;
            M34 root;
            if (d.wall) {
                const int f = d.facing;
                root = mul(base, mul(translate(0.5f - stepX(f) * 0.25f, 0.25f, 0.5f - stepZ(f) * 0.25f),
                                     mul(rotY(-yRotOf(opposite(f)) * kDeg), scale(-1, -1, 1))));
            } else {
                root = mul(base, mul(translate(0.5f, 0, 0.5f), mul(rotY(-float(d.rotation) * 22.5f * kDeg), scale(-1, -1, 1))));
            }
            if (type == 6 && itemDrawer_ && data && !data->itemNbt[0].empty()) {  // drawn with the profile's skin
                if (shownOut_) {
                    ShownItem s;
                    s.nbt = &data->itemNbt[0], s.kind = 1, s.light = light;
                    std::copy(root.m, root.m + 12, s.m);
                    for (int k = 0; k < 3; ++k) s.origin[k] = int(e.origin[k]);
                    shownOut_->push_back(s);
                }
                return;
            }
            ent::PartPose* p = initPoses(mdl);
            if (!p) return;
            const float anim = a ? float(a->skullTicks) + (d.flag ? pt : 0.0f) : 0.0f;
            if (type == 4)  // DragonHeadModel
                setPart(mdl, "jaw", [&](ent::PartPose& pp) { pp.xRot = float(std::sin(anim * kPi * 0.2f) + 1.0) * 0.2f; });
            if (type == 5) {  // PiglinHeadModel
                setPart(mdl, "left_ear", [&](ent::PartPose& pp) { pp.zRot = -float(std::cos(anim * kPi * 0.2f * 1.2f) + 2.5) * 0.2f; });
                setPart(mdl, "right_ear", [&](ent::PartPose& pp) { pp.zRot = float(std::cos(anim * kPi * 0.2f) + 2.5) * 0.2f; });
            }
            ModelEmit me = model(0, mdl, root, tex);
            me.poses = p;
            me.run();
            break;
        }
        case Kind::Shulker: {  // ShulkerBoxRenderer
            const float progress = a ? a->oOpen + (a->open - a->oOpen) * pt : 0.0f;
            const M34 root = mul(base, mul(mul(translate(0.5f, 0.5f, 0.5f), scale(0.9995f)),
                                           mul(dirRotation(d.facing), mul(scale(1, -1, -1), translate(0, -1, 0)))));
            ent::PartPose* p = initPoses(m_.shulker);
            if (!p) return;
            setPart(m_.shulker, "lid", [&](ent::PartPose& pp) {
                pp.y = 24.0f - progress * 0.5f * 16.0f;
                pp.yRot = 270.0f * progress * kDeg;
            });
            ModelEmit me = model(0, m_.shulker, root, t_.shulker[d.variant]);
            me.poses = p;
            me.run();
            break;
        }
        case Kind::Bell: {  // BellRenderer / BellModel
            ent::PartPose* p = initPoses(m_.bell);
            if (!p) return;
            if (a && a->shakeDir >= 0) {
                const float ticks = float(a->bellTicks) + pt;
                const float b = std::sin(ticks / kPi) / (4.0f + ticks / 3.0f);
                setPart(m_.bell, "bell_body", [&](ent::PartPose& pp) {
                    switch (a->shakeDir) {
                        case 2: pp.xRot = -b; break;
                        case 3: pp.xRot = b; break;
                        case 5: pp.zRot = -b; break;
                        case 4: pp.zRot = b; break;
                        default: break;
                    }
                });
            }
            ModelEmit me = model(0, m_.bell, base, t_.bell);
            me.poses = p;
            me.run();
            break;
        }
        case Kind::Pot: {  // DecoratedPotRenderer
            M34 root = mul(base, rotateAround(rotY((180.0f - yRotOf(d.facing)) * kDeg), 0.5f, 0.5f, 0.5f));
            if (a) {
                const float duration = a->wobbleStyle == 0 ? 7.0f : 10.0f;
                const float progress = (float(gameTicks_ - a->wobbleStart) + pt) / duration;
                if (progress >= 0 && progress <= 1) {
                    if (a->wobbleStyle == 0) {
                        const float dt = progress * 2 * kPi;
                        const float tiltX = -1.5f * (std::cos(dt) + 0.5f) * std::sin(dt / 2.0f);
                        root = mul(root, rotateAround(rotX(tiltX * 0.015625f), 0.5f, 0, 0.5f));
                        root = mul(root, rotateAround(rotZ(std::sin(dt) * 0.015625f), 0.5f, 0, 0.5f));
                    } else {
                        const float turn = std::sin(-progress * 3.0f * kPi) * 0.125f * (1.0f - progress);
                        root = mul(root, rotateAround(rotY(turn), 0.5f, 0, 0.5f));
                    }
                }
            }
            model(0, m_.potBase, root, t_.potBase).run();
            if (m_.potSides >= 0) {
                const ent::Model& sm = assets_.modelAt(m_.potSides);
                int partTex[16];
                std::fill(partTex, partTex + 16, -1);
                const char* sides[4] = {"back", "left", "right", "front"};
                for (int i = 0; i < 4; ++i) {
                    const int pi = partIndex(sm, sides[i]);
                    const int tex = data && data->sherds[i] >= 0 ? data->sherds[i] : t_.potSide;
                    if (pi >= 0 && pi < 16) partTex[pi] = tex;
                }
                ModelEmit me = model(0, m_.potSides, root, t_.potSide);
                me.partTex = partTex;
                me.run();
            }
            break;
        }
        case Kind::Conduit: {  // ConduitRenderer
            if (!a || !a->active) {
                const float rot = a ? a->activeRot * -0.0375f : 0.0f;
                model(0, m_.conduitShell, mul(base, mul(translate(0.5f, 0.5f, 0.5f), rotY(rot * kDeg))), t_.conduitBase).run();
                break;
            }
            const float time = float(a->tickCount) + pt;
            const float rotation = (a->activeRot + pt) * -0.0375f;  // radians
            float hh = std::sin(time * 0.1f) / 2.0f + 0.5f;
            hh = hh * hh + hh;
            const float ax = 0.5f / std::sqrt(1.5f), ay = 1.0f / std::sqrt(1.5f);
            model(0, m_.conduitCage, mul(base, mul(translate(0.5f, 0.3f + hh * 0.2f, 0.5f), rotAxis(ax, ay, ax, rotation))),
                  t_.conduitCage)
                .run();
            const int phase = (a->tickCount / 66) % 3;
            M34 wind = mul(base, translate(0.5f, 0.5f, 0.5f));
            if (phase == 1) wind = mul(wind, rotX(kPi / 2));
            else if (phase == 2) wind = mul(wind, rotZ(kPi / 2));
            const int windTex = phase == 1 ? t_.conduitWindV : t_.conduitWind;
            float vScale = 1.0f;
            if (windTex >= 0) {  // animated texture: first 64x32 frame
                const ent::TexRect& r = assets_.rect(windTex);
                if (r.h > r.w / 2) vScale = float(r.w / 2) / float(r.h);
            }
            ModelEmit w = model(0, m_.conduitWind, wind, windTex);
            w.vScale = vScale;
            w.run();
            w.root = mul(base, mul(mul(translate(0.5f, 0.5f, 0.5f), scale(0.875f)), mul(rotX(kPi), rotZ(kPi))));
            w.run();
            // The eye faces the camera.
            M34 bill = {{e.camRight[0], e.camUp[0], -e.camFwd[0], 0, e.camRight[1], e.camUp[1], -e.camFwd[1], 0,
                         e.camRight[2], e.camUp[2], -e.camFwd[2], 0}};
            const M34 eye = mul(base, mul(mul(translate(0.5f, 0.3f + hh * 0.2f, 0.5f), scale(0.5f)),
                                          mul(bill, mul(scale(1, -1, -1), scale(1.3333334f)))));
            model(0, m_.conduitEye, eye, a->hunting ? t_.conduitOpen : t_.conduitClosed).run();
            break;
        }
        case Kind::Enchanting:
        case Kind::Lectern: {  // EnchantTableRenderer / LecternRenderer + BookModel
            if (d.kind == Kind::Lectern && !d.flag) return;
            float openness, pf1, pf2;
            M34 root;
            if (d.kind == Kind::Enchanting) {
                const float time = a ? float(a->time) + pt : 0.0f;
                const float flip = a ? a->oFlip + (a->flip - a->oFlip) * pt : 0.0f;
                const float open = a ? a->oBook + (a->book - a->oBook) * pt : 0.0f;
                float yRot = 0;
                if (a) {
                    float o = a->rot - a->oRot;
                    while (o >= kPi) o -= 2 * kPi;
                    while (o < -kPi) o += 2 * kPi;
                    yRot = a->oRot + o * pt;
                }
                root = mul(base, mul(translate(0.5f, 0.75f + 0.1f + std::sin(time * 0.1f) * 0.01f, 0.5f),
                                     mul(rotY(-yRot), rotZ(80.0f * kDeg))));
                auto frac = [](float v) { return v - std::floor(v); };
                pf1 = std::clamp(frac(flip + 0.25f) * 1.6f - 0.3f, 0.0f, 1.0f);
                pf2 = std::clamp(frac(flip + 0.75f) * 1.6f - 0.3f, 0.0f, 1.0f);
                openness = (std::sin(time * 0.02f) * 0.1f + 1.25f) * open;
            } else {
                const float yRot = yRotOf(clockwise(d.facing));
                root = mul(base, mul(mul(translate(0.5f, 1.0625f, 0.5f), rotY(-yRot * kDeg)),
                                     mul(rotZ(67.5f * kDeg), translate(0, -0.125f, 0))));
                openness = 1.25f * 1.2f, pf1 = 0.1f, pf2 = 0.9f;
            }
            ent::PartPose* p = initPoses(m_.book);
            if (!p) return;
            const float s = std::sin(openness);
            setPart(m_.book, "left_lid", [&](ent::PartPose& pp) { pp.yRot = kPi + openness; });
            setPart(m_.book, "right_lid", [&](ent::PartPose& pp) { pp.yRot = -openness; });
            setPart(m_.book, "left_pages", [&](ent::PartPose& pp) { pp.yRot = openness, pp.x = s; });
            setPart(m_.book, "right_pages", [&](ent::PartPose& pp) { pp.yRot = -openness, pp.x = s; });
            setPart(m_.book, "flip_page1", [&](ent::PartPose& pp) { pp.yRot = openness - openness * 2.0f * pf1, pp.x = s; });
            setPart(m_.book, "flip_page2", [&](ent::PartPose& pp) { pp.yRot = openness - openness * 2.0f * pf2, pp.x = s; });
            ModelEmit me = model(0, m_.book, root, t_.book);
            me.poses = p;
            me.run();
            break;
        }
        case Kind::Campfire:
        case Kind::Shelf: {  // CampfireRenderer / ShelfRenderer (items: flat sprites or small blocks)
            if (!data) return;
            const int slots = d.kind == Kind::Campfire ? 4 : 3;
            for (int slot = 0; slot < slots; ++slot) {
                const std::string& item = data->items[slot];
                if (item.empty()) continue;
                M34 m;
                if (d.kind == Kind::Campfire) {
                    const int dir = from2d((slot + data2d(d.facing)) % 4);
                    m = mul(base, mul(mul(translate(0.5f, 0.44921875f, 0.5f), rotY(-yRotOf(dir) * kDeg)),
                                      mul(rotX(90.0f * kDeg), mul(translate(-0.3125f, -0.3125f, 0), scale(0.375f)))));
                } else {
                    const float yRot = (d.facing >= 2) ? -yRotOf(d.facing) : 180.0f;
                    m = mul(base, mul(mul(translate(0.5f, 0.5f, 0.5f), rotY(yRot * kDeg)),
                                      mul(translate((slot - 1) * 0.3125f, data->alignBottom ? -0.25f : 0.0f, -0.25f), scale(0.25f))));
                    // Model bounds after the display transform are [-0.5, 0.5]: centre or bottom-align the item.
                    m = mul(m, translate(0, data->alignBottom ? 0.5f : 0.0f, 0));
                }
                shownNbt_ = &data->itemNbt[slot], shownSeed_ = slot;
                if (itemDrawer_ && d.kind == Kind::Shelf) {  // ShelfRenderer: the model's bounding box is aligned there
                    m = mul(m, translate(0, data->alignBottom ? -0.5f : 0.0f, 0));  // (undo the fixed alignment above)
                }
                emitItem(e, item, m, light, d.kind == Kind::Campfire ? 0 : 1);
                if (shownOut_ && d.kind == Kind::Shelf && !shownOut_->empty()) shownOut_->back().shelfBottom = data->alignBottom;
                shownNbt_ = nullptr;
            }
            break;
        }
        case Kind::TestInstance: {  // TestInstanceRenderer: BeaconRenderer with the status's one section
            if (!data || !data->beam) return;
            const float animTime = float(gameTicks_ % 40) + pt;
            const double dx = cam_[0] - (in.x + 0.5), dz = cam_[2] - (in.z + 0.5);
            const float radius = scoping_ ? 1.0f : std::max(1.0f, float(std::sqrt(dx * dx + dz * dz)) / 96.0f);
            emitBeam(e, base, t_.beaconBeam, 1.0f, animTime, 0, 2048, data->beam & 0xFFFFFF, 0.2f * radius, 0.25f * radius);
            break;
        }
        case Kind::Beacon: {  // BeaconRenderer: the beam sections, the last one 2048 high
            if (!a || a->levels <= 0 || a->beam.empty()) return;
            const float animTime = float(gameTicks_ % 40) + pt;
            const double dx = cam_[0] - (in.x + 0.5), dz = cam_[2] - (in.z + 0.5);
            // beamRadiusScale: widened with the distance, unless looking through a spyglass
            const float radius = scoping_ ? 1.0f : std::max(1.0f, float(std::sqrt(dx * dx + dz * dz)) / 96.0f);
            int start = 0;
            for (size_t i = 0; i < a->beam.size(); ++i) {
                const auto& [color, height] = a->beam[i];
                emitBeam(e, base, t_.beaconBeam, 1.0f, animTime, start, i + 1 == a->beam.size() ? 2048 : height, color,
                         0.2f * radius, 0.25f * radius);
                start += height;
            }
            break;
        }
        case Kind::EndPortal:
        case Kind::EndGateway: {  // TheEndPortalRenderer / TheEndGatewayRenderer (AbstractEndPortalRenderer.submitCube)
            const bool portal = d.kind == Kind::EndPortal;
            static const float kN[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
            for (int f = 0; f < 6; ++f) {
                // shouldRenderFace: the portal shows its top and bottom; the gateway every face its neighbour does
                // not hide.
                if (portal ? f > 1 : registry_.infos()[world_.block(in.x + stepX(f), in.y + stepY(f), in.z + stepZ(f))].occludes)
                    continue;
                const float* n = kN[f];
                const int ax = f < 2 ? 0 : f < 4 ? 0 : 2, ay = f < 2 ? 2 : 1;  // the face's two in-plane axes
                float c[4][3];
                const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
                for (int k = 0; k < 4; ++k) {
                    float p[3];
                    for (int i = 0; i < 3; ++i) p[i] = n[i] > 0 ? 1.0f : 0.0f;
                    p[ax] = corners[k][0], p[ay] = corners[k][1];
                    if (portal) p[1] = 0.375f + p[1] * 0.375f;  // TRANSFORMATION: y 0.375 .. 0.75
                    xform(base, p, c[k]);
                }
                // Counter-clockwise seen from outside.
                const float e1[3] = {c[1][0] - c[0][0], c[1][1] - c[0][1], c[1][2] - c[0][2]};
                const float e2[3] = {c[2][0] - c[0][0], c[2][1] - c[0][1], c[2][2] - c[0][2]};
                const float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                if (cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2] < 0) std::swap(c[1], c[3]);
                BeQuad q{};
                std::memcpy(q.pos, c, sizeof(c));
                q.color = 0xFFFFFFFF, q.light = 256 | 512 | 2048, q.tex = portal ? kTexEndPortal : kTexEndGateway;
                e.out[0]->push_back(q);
            }
            if (portal || !a) break;
            // The gateway beam while it is spawning (Age < 200) or cooling down after a teleport.
            const bool spawning = a->age >= 0 && a->age < 200, cooling = a->cooldown > 0;
            if (!spawning && !cooling) break;
            float s = spawning ? std::clamp((float(a->age) + pt) / 200.0f, 0.0f, 1.0f)
                               : 1.0f - std::clamp((float(a->cooldown) - pt) / 40.0f, 0.0f, 1.0f);
            const float beamDistance = spawning ? float((world_.maxSectionY() + 1) * 16 - 1) : 50.0f;
            s = std::sin(s * kPi);
            const int height = int(std::floor(s * beamDistance));
            if (height > 0)
                emitBeam(e, base, t_.gatewayBeam, s, float(gameTicks_ % 40) + pt, -height, height * 2,
                         kDyeDiffuse[spawning ? 2 : 10], 0.15f, 0.175f);
            break;
        }
        case Kind::Spawner:
        case Kind::TrialSpawner: {  // SpawnerRenderer.submitEntityInSpawner
            if (!data || data->entity < 0 || (d.kind == Kind::TrialSpawner && !d.variant)) return;
            const ent::TypeInfo* info = assets_.type(data->entity);
            float s = 0.53125f;
            if (info) s /= std::max(1.0f, std::max(info->width, info->height));
            const float spin = a ? (a->oSpin + (a->spin - a->oSpin) * pt) * 10.0f : 0.0f;
            const M34 m = mul(base, mul(mul(translate(0.5f, 0.4f, 0.5f), rotY(spin * kDeg)),
                                        mul(mul(translate(0, -0.2f, 0), rotX(-30.0f * kDeg)), scale(s))));
            emitDisplayEntity(e, data->entity, m, light, a ? float(a->tickCount) + pt : 0.0f);
            break;
        }
        case Kind::Vault: {  // VaultRenderer: the display item turning in the middle
            if (!data || data->items[0].empty()) return;
            float spin = 0;
            if (a) {  // Mth.rotLerp
                spin = a->oSpin + std::remainder(a->spin - a->oSpin, 360.0f) * pt;
            }
            shownNbt_ = &data->itemNbt[0];
            emitItem(e, data->items[0], mul(base, mul(translate(0.5f, 0.4f, 0.5f), rotY(spin * kDeg))), light, 2);
            shownNbt_ = nullptr;
            break;
        }
        case Kind::Brushable: {  // BrushableBlockRenderer: the item coming out of the brushed face
            if (!data || d.variant == 0 || data->hitDir < 0 || data->items[0].empty()) return;
            const int hit = data->hitDir;
            float t[3] = {0.5f, 0.0f, 0.5f};
            const float off = float(d.variant) / 10.0f * 0.75f;
            switch (hit) {
                case 5: t[0] = 0.73f + off; break;
                case 4: t[0] = 0.25f - off; break;
                case 1: t[1] = 0.25f + off; break;
                case 0: t[1] = -0.23f - off; break;
                case 2: t[2] = 0.25f - off; break;
                default: t[2] = 0.73f + off; break;
            }
            const bool eastWest = hit == 4 || hit == 5;
            const M34 m = mul(base, mul(mul(translate(t[0], 0.5f + t[1], t[2]), rotY(75.0f * kDeg)),
                                        mul(rotY(float((eastWest ? 90 : 0) + 11) * kDeg), scale(0.5f))));
            shownNbt_ = &data->itemNbt[0];
            emitItem(e, data->items[0], m, sampleLight(in.x + stepX(hit), in.y + stepY(hit), in.z + stepZ(hit)), 0);
            shownNbt_ = nullptr;
            break;
        }
        case Kind::Statue: {  // CopperGolemStatueBlockRenderer / CopperGolemStatueModel
            const int mdl = m_.statue[d.part] >= 0 ? m_.statue[d.part] : m_.statue[0];
            ent::PartPose* p = initPoses(mdl);
            if (!p) return;
            p[0].y = 0, p[0].zRot = kPi;  // root
            const M34 root = mul(base, mul(translate(0.5f, 0, 0.5f), rotY(-yRotOf(opposite(d.facing)) * kDeg)));
            ModelEmit me = model(0, mdl, root, t_.statue[d.variant]);
            me.poses = p;
            me.run();
            break;
        }
        default: break;
    }
}

void BlockEntityRenderer::buildSection(Section& s, std::vector<BeQuad> out[3]) const {
    Emit e{{&out[0], &out[1], &out[2]}, {double(s.cx) * 16, double(s.sy) * 16, double(s.cz) * 16}, {}, {}, {}};
    s.dynamic.clear();
    for (u32 i = 0; i < s.list.size(); ++i) {
        const Instance& in = s.list[i];
        const BlockDef& d = defs_[in.block];
        if (isDynamic(in, d)) {
            s.dynamic.push_back(i);
            continue;
        }
        const size_t before = out[0].size() + out[1].size() + out[2].size();
        emitInstance(e, in, in.data >= 0 ? &s.data[size_t(in.data)] : nullptr, 0.0f);
        static const bool debug = std::getenv("MCPP_BE_DEBUG") != nullptr;
        if (debug)
            LOG("be: %d %d %d kind %d variant %d -> %zu quads (%s)", in.x, in.y, in.z, int(d.kind), int(d.variant),
                out[0].size() + out[1].size() + out[2].size() - before, registry_.name(in.block).c_str());
    }
}

}  // namespace be
