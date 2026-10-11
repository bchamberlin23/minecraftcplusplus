#include "entity/entity_render.h"

#include "assets/assets.h"
#include "assets/game_reports.h"
#include "world/world.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <shared_mutex>

#include "EntityCutoutMvPS.h"
#include "EntityCutoutPS.h"
#include "EntityFreeVS.h"
#include "EntityGlintPS.h"
#include "EntityModelVS.h"
#include "EntityShadowFreeVS.h"
#include "EntityShadowPS.h"
#include "EntityShadowVS.h"
#include "EntityTranslucentPS.h"

namespace ent {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;

// Per-frame upload capacities.
constexpr u32 kMaxMatrices = 1 << 15, kMaxInstances = 1 << 14, kMaxRefs = 1 << 18, kMaxFree = 1 << 13;
constexpr u64 kConstBytes = 1024;
constexpr u64 kMatOffset = kConstBytes;
constexpr u64 kPrevMatOffset = kMatOffset + u64(kMaxMatrices) * 48;  // last frame's part matrices (motion vectors)
constexpr u64 kInstOffset = kPrevMatOffset + u64(kMaxMatrices) * 48;
constexpr u64 kRefOffset = kInstOffset + u64(kMaxInstances) * 32;
constexpr u64 kFreeOffset = kRefOffset + u64(kMaxRefs) * 8;
constexpr u64 kFrameBytes = (kFreeOffset + u64(kMaxFree) * 128 + 65535) & ~u64(65535);

enum LayerFlags : u8 { Emissive = 1, Translucent = 2, TintWool = 8, TintCollar = 16, Armor = 32, Elytra = 64, Glint = 128 };
enum Special : u8 {
    SpNone, SpCow, SpPig, SpChicken, SpSheep, SpWolf, SpCat, SpHorse, SpLlama, SpRabbit, SpParrot, SpFox, SpAxolotl,
    SpPanda, SpMooshroom, SpVillager, SpZombieVillager, SpSlime, SpPlayer, SpFrog, SpArmorStand, SpCreeper, SpBoat,
    SpMinecart, SpArrow, SpXpOrb, SpTrident
};

struct GpuInstance {
    u32 matrixBase, color, light, overlay;
    float uvRect[4];
};
struct FreeQuad {
    float pos[4][3];
    float uv[4][2];
    float normal[3];
    u32 tex, color, light, overlay;
    float motion[3];  // current - previous frame position (blocks)
    u32 pad[2];
};
static_assert(sizeof(FreeQuad) == 128);

// DyeColor.getTextureDiffuseColor, by dye id.
constexpr u32 kDye[16] = {0xF9FFFE, 0xF9801D, 0xC74EBD, 0x3AB3DA, 0xFED83D, 0x80C71F, 0xF38BAA, 0x474F52,
                          0x9D9D97, 0x169C9C, 0x8932B8, 0x3C44AA, 0x835432, 0x5E7C16, 0xB02E26, 0x1D1D21};
// RGB (0xRRGGBB) -> 0xAABBGGRR.
u32 abgr(u32 rgb, u32 a = 255) { return (a << 24) | ((rgb & 255) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 255); }
constexpr u32 kWhite = 0xFFFFFFFF;

struct M34 {
    float m[12];
    static M34 identity() { return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}; }
};
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
// ModelPart.translateAndRotate: T(pivot / 16) * Rz * Ry * Rx * S.
M34 partLocal(const PartPose& p) {
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
void xformDir(const M34& m, const float in[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = m.m[i * 4] * in[0] + m.m[i * 4 + 1] * in[1] + m.m[i * 4 + 2] * in[2];
    const float l = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (l > 0) out[0] /= l, out[1] /= l, out[2] /= l;
}
float lerpAngle(float a, float from, float to) {
    float d = std::fmod(to - from, 360.0f);
    if (d >= 180) d -= 360;
    if (d < -180) d += 360;
    return from + a * d;
}

std::vector<PackVertex> g_packGlint;  // shader packs: armour glint quads of the entity being built (packQuad bucket 4)

bool check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    LOG("entities: %s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    return false;
}

}  // namespace

EntityRenderer::EntityRenderer(EntityWorld& world) : world_(world), assets_(world.assets()) {
    buildDefs();
    mats_.reserve(256);
    poses_.reserve(256);
    hidden_.reserve(256);
    translucent_.reserve(1 << 14);
    glint_.reserve(1 << 12);
}

EntityRenderer::~EntityRenderer() {
    if (!frames_) return;
    LOG("entities: %.3f ms average CPU per frame (cull + animate + upload), %.0f entities drawn on average",
        cpuTotal_ / double(frames_), double(entityFrames_) / double(frames_));
    const u32 n = u32(std::min<u64>(frames_, kHistory));
    double ms = 0, worldMs = 0, maxMs = 0, drawn = 0, quads = 0;
    for (u32 i = 0; i < n; ++i)
        ms += historyMs_[i], worldMs += historyWorldMs_[i], maxMs = std::max(maxMs, double(historyMs_[i])),
            drawn += historyDrawn_[i], quads += historyQuads_[i];
    LOG("entities: last %u frames: %.0f entities, %.0f model quads drawn | render CPU %.3f ms avg (max %.3f) + "
        "packets/ticks %.3f ms avg",
        n, drawn / n, quads / n, ms / n, maxMs, worldMs / n);
}

// ---------------------------------------------------------------------------------------------------------
// Entity type -> model layers, textures and animation, following the game's entity renderers.

void EntityRenderer::buildDefs() {
    const auto& types = assets_.types();
    defs_.assign(types.size(), TypeDef{});
    auto M = [&](const std::string& layer) { return assets_.model(layer); };
    auto T = [&](const std::string& tex) { return assets_.texture("entity/" + tex); };
    for (size_t i = 0; i < types.size(); ++i) {
        const std::string full = types[i].name;
        if (full.empty()) continue;
        const std::string n = full.substr(full.find(':') + 1);
        TypeDef& d = defs_[i];
        d.kind = Kind::Mob;
        // Default: "<name>#main" with textures/entity/<name>/<name>.png (most mobs follow this pattern).
        d.base = {M(n + "#main"), M(n + "_baby#main"), T(n + "/" + n), T(n + "/" + n + "_baby")};
        auto mob = [&](Anim anim, const std::string& model, const std::string& tex, const std::string& babyModel = "",
                       const std::string& babyTex = "") {
            d.anim = anim;
            d.base = {M(model), babyModel.empty() ? -1 : M(babyModel), T(tex), babyTex.empty() ? -1 : T(babyTex)};
        };
        auto extra = [&](const std::string& model, const std::string& tex, u8 flags, const std::string& babyModel = "",
                         const std::string& babyTex = "") {
            if (d.extraCount < 3)
                d.extra[d.extraCount++] = {M(model), babyModel.empty() ? -1 : M(babyModel), T(tex),
                                           babyTex.empty() ? -1 : T(babyTex), flags};
        };
        auto sprite = [&](int tex, float scale) {
            d.kind = Kind::Sprite;
            d.sprite = tex;
            d.spriteScale = scale;
        };
        if (n == "player" || n == "mannequin") {
            d.anim = Anim::Humanoid, d.special = SpPlayer, d.scale = 0.9375f;
            d.base = {M("player#main"), M("player_slim#main"), T("player/wide/steve"), -1};
        } else if (n == "zombie") mob(Anim::Zombie, "zombie#main", "zombie/zombie", "zombie_baby#main", "zombie/zombie_baby");
        else if (n == "husk") mob(Anim::Zombie, "husk#main", "zombie/husk", "husk_baby#main", "zombie/husk_baby");
        else if (n == "drowned") {
            mob(Anim::Zombie, "drowned#main", "zombie/drowned", "drowned_baby#main", "zombie/drowned_baby");
            extra("drowned#outer", "zombie/drowned_outer_layer", 0, "drowned_baby#outer", "zombie/drowned_outer_layer_baby");
        } else if (n == "zombie_villager") {
            mob(Anim::Zombie, "zombie_villager#main", "zombie_villager/zombie_villager", "zombie_villager_baby#main",
                "zombie_villager/zombie_villager_baby");
            d.special = SpZombieVillager;
        } else if (n == "zombified_piglin")
            mob(Anim::Zombie, "zombified_piglin#main", "piglin/zombified_piglin", "zombified_piglin_baby#main",
                "piglin/zombified_piglin_baby");
        else if (n == "piglin") mob(Anim::Humanoid, "piglin#main", "piglin/piglin", "piglin_baby#main", "piglin/piglin_baby");
        else if (n == "piglin_brute") mob(Anim::Humanoid, "piglin_brute#main", "piglin/piglin_brute");
        else if (n == "giant") mob(Anim::Zombie, "giant#main", "zombie/zombie"), d.scale = 6.0f;
        else if (n == "skeleton") mob(Anim::Skeleton, "skeleton#main", "skeleton/skeleton");
        else if (n == "stray") mob(Anim::Skeleton, "stray#main", "skeleton/stray"), extra("stray#outer", "skeleton/stray_overlay", 0);
        else if (n == "bogged") mob(Anim::Skeleton, "bogged#main", "skeleton/bogged"), extra("bogged#outer", "skeleton/bogged_overlay", 0);
        else if (n == "wither_skeleton") mob(Anim::Skeleton, "wither_skeleton#main", "skeleton/wither_skeleton"), d.scale = 1.2f;
        else if (n == "parched") mob(Anim::Skeleton, "parched#main", "skeleton/parched");
        else if (n == "creeper") mob(Anim::Creeper, "creeper#main", "creeper/creeper"), d.special = SpCreeper;
        else if (n == "spider" || n == "cave_spider") {
            mob(Anim::Spider, n + "#main", "spider/" + n);
            extra(n + "#main", "spider/spider_eyes", Emissive | Translucent);
            d.flip = 180.0f;
        } else if (n == "enderman") {
            mob(Anim::Humanoid, "enderman#main", "enderman/enderman");
            extra("enderman#main", "enderman/enderman_eyes", Emissive | Translucent);
        } else if (n == "witch") mob(Anim::Villager, "witch#main", "witch/witch");
        else if (n == "villager") mob(Anim::Villager, "villager#main", "villager/villager", "villager_baby#main", "villager/villager_baby"), d.special = SpVillager;
        else if (n == "wandering_trader") mob(Anim::Villager, "wandering_trader#main", "wandering_trader/wandering_trader");
        else if (n == "pillager" || n == "vindicator" || n == "evoker" || n == "illusioner") mob(Anim::Villager, n + "#main", "illager/" + n);
        else if (n == "vex" || n == "ravager") mob(Anim::Head, n + "#main", "illager/" + n);
        else if (n == "evoker_fangs") mob(Anim::None, "evoker_fangs#main", "illager/evoker_fangs");
        else if (n == "iron_golem") mob(Anim::IronGolem, "iron_golem#main", "iron_golem/iron_golem");
        else if (n == "cow") mob(Anim::Quadruped, "cow#main", "cow/cow_temperate", "cow_baby#main", "cow/cow_temperate_baby"), d.special = SpCow;
        else if (n == "mooshroom") mob(Anim::Quadruped, "mooshroom#main", "cow/mooshroom_red", "mooshroom_baby#main", "cow/mooshroom_red_baby"), d.special = SpMooshroom;
        else if (n == "pig") mob(Anim::Quadruped, "pig#main", "pig/pig_temperate", "pig_baby#main", "pig/pig_temperate_baby"), d.special = SpPig;
        else if (n == "sheep") {
            mob(Anim::Quadruped, "sheep#main", "sheep/sheep", "sheep_baby#main", "sheep/sheep_baby");
            d.special = SpSheep;
        } else if (n == "chicken") mob(Anim::Chicken, "chicken#main", "chicken/chicken_temperate", "chicken_baby#main", "chicken/chicken_temperate_baby"), d.special = SpChicken;
        else if (n == "wolf") mob(Anim::Quadruped, "wolf#main", "wolf/wolf", "wolf_baby#main", "wolf/wolf_baby"), d.special = SpWolf;
        else if (n == "cat") mob(Anim::Quadruped, "cat#main", "cat/cat_tabby", "cat_baby#main", "cat/cat_tabby_baby"), d.special = SpCat;
        else if (n == "ocelot") mob(Anim::Quadruped, "ocelot#main", "cat/ocelot", "ocelot_baby#main", "cat/ocelot_baby");
        else if (n == "horse") mob(Anim::Quadruped, "horse#main", "horse/horse_white", "horse_baby#main", "horse/horse_white_baby"), d.special = SpHorse;
        else if (n == "donkey" || n == "mule") mob(Anim::Quadruped, n + "#main", "horse/" + n, n + "_baby#main", "horse/" + n + "_baby");
        else if (n == "skeleton_horse") mob(Anim::Quadruped, "skeleton_horse#main", "horse/horse_skeleton", "skeleton_horse_baby#main", "horse/horse_skeleton_baby");
        else if (n == "zombie_horse") mob(Anim::Quadruped, "zombie_horse#main", "horse/horse_zombie", "zombie_horse_baby#main", "horse/horse_zombie_baby");
        else if (n == "llama" || n == "trader_llama") mob(Anim::Head, n + "#main", "llama/llama_creamy", n + "_baby#main", "llama/llama_creamy_baby"), d.special = SpLlama;
        else if (n == "rabbit") mob(Anim::Head, "rabbit#main", "rabbit/rabbit_brown", "rabbit_baby#main", "rabbit/rabbit_brown_baby"), d.special = SpRabbit;
        else if (n == "parrot") mob(Anim::Head, "parrot#main", "parrot/parrot_red_blue"), d.special = SpParrot;
        else if (n == "fox") mob(Anim::Quadruped, "fox#main", "fox/fox", "fox_baby#main", "fox/fox_baby"), d.special = SpFox;
        else if (n == "axolotl") mob(Anim::Head, "axolotl#main", "axolotl/axolotl_lucy", "axolotl_baby#main", "axolotl/axolotl_lucy_baby"), d.special = SpAxolotl;
        else if (n == "panda") mob(Anim::Quadruped, "panda#main", "panda/panda", "panda_baby#main", "panda/panda_baby"), d.special = SpPanda;
        else if (n == "frog") mob(Anim::Head, "frog#main", "frog/frog_temperate"), d.special = SpFrog;
        else if (n == "polar_bear") mob(Anim::Quadruped, "polar_bear#main", "bear/polarbear", "polar_bear_baby#main", "bear/polarbear_baby");
        else if (n == "goat") mob(Anim::Quadruped, "goat#main", "goat/goat", "goat_baby#main", "goat/goat_baby");
        else if (n == "hoglin") mob(Anim::Quadruped, "hoglin#main", "hoglin/hoglin", "hoglin_baby#main", "hoglin/hoglin_baby");
        else if (n == "zoglin") mob(Anim::Quadruped, "zoglin#main", "hoglin/zoglin", "zoglin_baby#main", "hoglin/zoglin_baby");
        else if (n == "sniffer") mob(Anim::Head, "sniffer#main", "sniffer/sniffer", "sniffer_baby#main", "sniffer/snifflet");
        else if (n == "glow_squid") mob(Anim::None, "glow_squid#main", "squid/glow_squid", "glow_squid_baby#main", "squid/glow_squid_baby");
        else if (n == "squid") mob(Anim::None, "squid#main", "squid/squid", "squid_baby#main", "squid/squid_baby");
        else if (n == "cod") mob(Anim::None, "cod#main", "fish/cod");
        else if (n == "salmon") mob(Anim::None, "salmon#main", "fish/salmon");
        else if (n == "pufferfish") mob(Anim::None, "pufferfish_big#main", "fish/pufferfish");
        else if (n == "tropical_fish") mob(Anim::None, "tropical_fish_small#main", "fish/tropical_a");
        else if (n == "strider") mob(Anim::Head, "strider#main", "strider/strider", "strider_baby#main", "strider/strider_baby");
        else if (n == "slime") {
            mob(Anim::None, "slime#main", "slime/slime");
            extra("slime#outer", "slime/slime", Translucent);
            d.special = SpSlime;
        } else if (n == "magma_cube") mob(Anim::None, "magma_cube#main", "slime/magmacube"), d.special = SpSlime;
        else if (n == "phantom") mob(Anim::None, "phantom#main", "phantom/phantom"), extra("phantom#main", "phantom/phantom_eyes", Emissive | Translucent);
        else if (n == "guardian") mob(Anim::None, "guardian#main", "guardian/guardian");
        else if (n == "elder_guardian") mob(Anim::None, "elder_guardian#main", "guardian/guardian_elder");
        else if (n == "ghast") mob(Anim::None, "ghast#main", "ghast/ghast");
        else if (n == "happy_ghast") mob(Anim::None, "happy_ghast#main", "ghast/happy_ghast", "happy_ghast_baby#main", "ghast/happy_ghast_baby");
        else if (n == "snow_golem") mob(Anim::Head, "snow_golem#main", "snow_golem/snow_golem");
        else if (n == "copper_golem") mob(Anim::Head, "copper_golem#main", "copper_golem/copper_golem");
        else if (n == "turtle") mob(Anim::Head, "turtle#main", "turtle/turtle", "turtle_baby#main", "turtle/turtle_baby");
        else if (n == "dolphin") mob(Anim::Head, "dolphin#main", "dolphin/dolphin", "dolphin_baby#main", "dolphin/dolphin_baby");
        else if (n == "camel") mob(Anim::Head, "camel#main", "camel/camel", "camel_baby#main", "camel/camel_baby");
        else if (n == "camel_husk") mob(Anim::Head, "camel#main", "camel/camel_husk");
        else if (n == "armadillo") mob(Anim::Head, "armadillo#main", "armadillo/armadillo", "armadillo_baby#main", "armadillo/armadillo_baby");
        else if (n == "bee") mob(Anim::Head, "bee#main", "bee/bee", "bee_baby#main", "bee/bee_baby");
        else if (n == "warden") mob(Anim::Head, "warden#main", "warden/warden");
        else if (n == "wither") mob(Anim::None, "wither#main", "wither/wither");
        else if (n == "ender_dragon") mob(Anim::None, "ender_dragon#main", "enderdragon/dragon");
        else if (n == "armor_stand") {
            mob(Anim::ArmorStand, "armor_stand#main", "armorstand/armorstand", "armor_stand_small#main");
            d.special = SpArmorStand;
        } else if (n.ends_with("_chest_boat") || n.ends_with("_boat") || n.ends_with("_raft")) {
            std::string wood = n.substr(0, n.find(n.ends_with("_chest_boat") ? "_chest_boat" : n.ends_with("_boat") ? "_boat" : n.ends_with("_chest_raft") ? "_chest_raft" : "_raft"));
            const bool chest = n.find("_chest_") != std::string::npos;
            const std::string dir = chest ? "chest_boat" : "boat";
            mob(Anim::None, dir + "/" + wood + "#main", dir + "/" + wood);
            d.special = SpBoat;
        } else if (n.ends_with("minecart")) {
            mob(Anim::None, n + "#main", "minecart/minecart");
            d.special = SpMinecart;
        } else if (n == "arrow") mob(Anim::None, "arrow#main", "projectiles/arrow"), d.special = SpArrow;
        else if (n == "spectral_arrow") mob(Anim::None, "arrow#main", "projectiles/arrow_spectral"), d.special = SpArrow;
        else if (n == "trident") mob(Anim::None, "trident#main", "trident/trident"), d.special = SpTrident;
        else if (n == "shulker_bullet") mob(Anim::None, "shulker_bullet#main", "shulker/spark");
        else if (n == "wind_charge" || n == "breeze_wind_charge") mob(Anim::None, "wind_charge#main", "projectiles/wind_charge");
        else if (n == "llama_spit") mob(Anim::None, "llama_spit#main", "llama/llama_spit");
        else if (n == "wither_skull") mob(Anim::None, "wither_skull#main", "wither/wither");
        else if (n == "leash_knot") mob(Anim::None, "leash_knot#main", "lead_knot/lead_knot");
        else if (n == "end_crystal") mob(Anim::None, "end_crystal#main", "end_crystal/end_crystal");
        else if (n == "item") d.kind = Kind::Item;
        else if (n == "falling_block") d.kind = Kind::Block;
        else if (n == "tnt") d.kind = Kind::Block, d.block = "minecraft:tnt";
        else if (n == "snowball" || n == "egg" || n == "ender_pearl" || n == "experience_bottle" || n == "firework_rocket")
            sprite(assets_.itemTexture(n), 1.0f);
        else if (n == "splash_potion" || n == "lingering_potion") sprite(assets_.itemTexture(n), 1.0f);
        else if (n == "eye_of_ender") sprite(assets_.itemTexture("ender_eye"), 1.0f);
        else if (n == "fireball") sprite(assets_.itemTexture("fire_charge"), 3.0f);
        else if (n == "small_fireball") sprite(assets_.itemTexture("fire_charge"), 0.75f);
        else if (n == "dragon_fireball") sprite(T("enderdragon/dragon_fireball"), 2.0f);
        else if (n == "fishing_bobber") sprite(T("fishing/fishing_hook"), 1.0f);
        else if (n == "experience_orb") sprite(T("experience/experience_orb"), 0.6f), d.special = SpXpOrb;
        else if (n == "area_effect_cloud" || n == "marker" || n == "interaction" || n == "block_display" ||
                 n == "item_display" || n == "text_display" || n == "lightning_bolt" || n == "ominous_item_spawner")
            d.kind = Kind::None;  // invisible or effect-only
        else if (n == "item_frame" || n == "glow_item_frame" || n == "painting")
            d.kind = Kind::None;  // block-attached decorations: not drawn yet
        {  // ArmorModelSet (HumanoidArmorLayer)
            static const char* kPieces[4] = {"#helmet", "#chestplate", "#leggings", "#boots"};
            const std::string set = n == "player" || n == "mannequin" ? "player" : n;
            const std::string babySet = n == "armor_stand" ? "armor_stand_small" : n + "_baby";
            for (int k = 0; k < 4; ++k) {
                d.armor[k] = M(set + kPieces[k]);
                d.armorBaby[k] = M(babySet + kPieces[k]);
                d.armorSlim[k] = set == "player" ? M(std::string("player_slim") + kPieces[k]) : -1;
            }
        }
        if (d.kind == Kind::Mob && (d.base.model < 0 || d.base.tex < 0)) d.kind = Kind::Box;  // geometry fallback
        if (d.kind == Kind::Sprite && d.sprite < 0) d.kind = Kind::Box;
    }
    elytraModel_ = M("elytra#main"), elytraBabyModel_ = M("elytra_baby#main");
    u32 boxes = 0;
    std::string names;
    for (size_t i = 0; i < defs_.size(); ++i)
        if (defs_[i].kind == Kind::Box) ++boxes, names += " " + types[i].name.substr(types[i].name.find(':') + 1);
    LOG("entities: %u of %zu entity types fall back to a box:%s", boxes, defs_.size(), names.c_str());
}

// Layers depend on a few synched values only: resolve each combination once (texture names are built from
// strings) and reuse it, so steady-state frames do not allocate. Tints (wool, collar) are applied per frame.
int EntityRenderer::resolve(const Entity& e, const TypeDef& d, Layer* out) {
    const u64 v1 = u64(d.special == SpPlayer ? e.skin : e.variant) & 0xFFF;
    const u64 key = (u64(u32(e.type)) << 48) | (v1 << 36) | ((u64(e.variant2) & 0xFFF) << 24) |
                    ((u64(e.villagerType) & 0xFF) << 16) | (u64(e.baby) << 0) | (u64(e.tame) << 1) |
                    (u64(e.angry) << 2) | (u64(e.slim) << 3) | (u64((e.wool & 0x10) != 0) << 4) |
                    (u64((e.wool & 15) != 0) << 5);
    auto it = layerCache_.find(key);
    if (it == layerCache_.end()) {
        CachedLayers c;
        c.count = u8(resolveUncached(e, d, c.layers));
        it = layerCache_.emplace(key, c).first;
    }
    const CachedLayers& c = it->second;
    const int wool = e.wool & 15;
    const u32 woolRgb = wool == 0 ? 0xE6E6E6  // ColorLerper SHEEP: white is 0xE6E6E6, others diffuse * 0.75
                                  : (u32(((kDye[wool] >> 16) & 255) * 0.75f) << 16) |
                                        (u32(((kDye[wool] >> 8) & 255) * 0.75f) << 8) | u32((kDye[wool] & 255) * 0.75f);
    for (int i = 0; i < c.count; ++i) {
        out[i] = c.layers[i];
        if (out[i].flags & TintWool) out[i].color = abgr(woolRgb);
        if (out[i].flags & TintCollar) out[i].color = abgr(kDye[std::clamp(e.collar, 0, 15)]);
    }
    return c.count;
}

int EntityRenderer::resolveUncached(const Entity& e, const TypeDef& d, Layer* out) const {
    int n = 0;
    const bool baby = e.baby;
    auto pick = [&](const LayerDef& l, int& model, int& tex) {
        model = baby && l.babyModel >= 0 ? l.babyModel : l.model;
        tex = baby && l.babyTex >= 0 ? l.babyTex : l.tex;
    };
    int model, tex;
    pick(d.base, model, tex);
    u32 color = kWhite;
    auto variantOf = [&](const char* reg) -> const Variant* {
        const auto& v = assets_.variants(reg);
        return e.variant >= 0 && size_t(e.variant) < v.size() ? &v[size_t(e.variant)] : nullptr;
    };
    auto T = [&](const std::string& t) { return assets_.texture("entity/" + t); };
    auto withBaby = [&](const std::string& t) {
        const int b = baby ? T(t + "_baby") : -1;
        return b >= 0 ? b : T(t);
    };
    auto variantModel = [&](const Variant* v, const char* animal) {
        if (!v || v->model == "normal" || baby) return;
        const int m = assets_.model(v->model + "_" + animal + "#main");
        if (m >= 0) model = m;
    };
    switch (d.special) {
        case SpCow:
        case SpPig:
        case SpChicken: {
            const char* animal = d.special == SpCow ? "cow" : d.special == SpPig ? "pig" : "chicken";
            const Variant* v = variantOf(d.special == SpCow ? "cow_variant" : d.special == SpPig ? "pig_variant" : "chicken_variant");
            if (v) {
                const int t = baby && v->babyTex >= 0 ? v->babyTex : v->tex;
                if (t >= 0) tex = t;
                variantModel(v, animal);
                if (d.special == SpPig && baby) model = d.base.babyModel >= 0 ? d.base.babyModel : model;
            }
            break;
        }
        case SpMooshroom: tex = withBaby(e.variant == 1 ? "cow/mooshroom_brown" : "cow/mooshroom_red"); break;
        case SpWolf:
            if (const Variant* v = variantOf("wolf_variant")) {
                const int t = e.tame ? (baby ? v->babyTame : v->tame) : e.angry ? (baby ? v->babyAngry : v->angry)
                                                                              : (baby ? v->babyWild : v->wild);
                if (t >= 0) tex = t;
            }
            break;
        case SpCat:
            if (const Variant* v = variantOf("cat_variant")) {
                const int t = baby && v->babyTex >= 0 ? v->babyTex : v->tex;
                if (t >= 0) tex = t;
            }
            break;
        case SpFrog:
            if (const Variant* v = variantOf("frog_variant"))
                if (v->tex >= 0) tex = v->tex;
            break;
        case SpHorse: {
            static const char* kColors[7] = {"white", "creamy", "chestnut", "brown", "black", "gray", "darkbrown"};
            tex = withBaby(std::string("horse/horse_") + kColors[std::clamp(e.variant, 0, 6)]);
            break;
        }
        case SpLlama: {
            static const char* kColors[4] = {"creamy", "white", "brown", "gray"};
            tex = withBaby(std::string("llama/llama_") + kColors[std::clamp(e.variant, 0, 3)]);
            break;
        }
        case SpRabbit: {
            static const char* kTypes[6] = {"brown", "white", "black", "white_splotched", "gold", "salt"};
            tex = withBaby(e.variant == 99 ? "rabbit/rabbit_caerbannog"
                                           : std::string("rabbit/rabbit_") + kTypes[std::clamp(e.variant, 0, 5)]);
            break;
        }
        case SpParrot: {
            static const char* kTypes[5] = {"red_blue", "blue", "green", "yellow_blue", "grey"};
            tex = T(std::string("parrot/parrot_") + kTypes[std::clamp(e.variant, 0, 4)]);
            break;
        }
        case SpFox: tex = withBaby(e.variant == 1 ? "fox/fox_snow" : "fox/fox"); break;
        case SpAxolotl: {
            static const char* kTypes[5] = {"lucy", "wild", "gold", "cyan", "blue"};
            tex = withBaby(std::string("axolotl/axolotl_") + kTypes[std::clamp(e.variant, 0, 4)]);
            break;
        }
        case SpPanda: {
            // Visible gene: recessive genes (brown 4, weak 5) only show when both genes match.
            int gene = std::clamp(e.variant, 0, 6);
            if ((gene == 4 || gene == 5) && e.variant2 != gene) gene = 0;
            static const char* kGenes[7] = {"", "lazy", "worried", "playful", "brown", "weak", "aggressive"};
            if (gene == 0) tex = withBaby("panda/panda");
            else if (baby) tex = T(std::string("panda/") + kGenes[gene] + "_panda_baby");
            else tex = T(std::string("panda/panda_") + kGenes[gene]);
            if (tex < 0) tex = d.base.tex;
            break;
        }
        case SpPlayer:
            model = e.slim && d.base.babyModel >= 0 ? d.base.babyModel : d.base.model;
            if (e.skin >= 0) tex = e.skin;
            break;
        default: break;
    }
    if (model < 0 || tex < 0) return 0;
    out[n++] = {model, tex, color, d.base.flags};
    for (int i = 0; i < d.extraCount && n < 6; ++i) {
        int m, t;
        pick(d.extra[i], m, t);
        if (m >= 0 && t >= 0) out[n++] = {m, t, kWhite, d.extra[i].flags};
    }
    if (d.special == SpSheep && !(e.wool & 0x10)) {  // wool (SheepWoolLayer / SheepWoolUndercoatLayer)
        const int color = e.wool & 15;
        const int woolModel = assets_.model(baby ? "sheep_baby#wool" : "sheep#wool");
        const int woolTex = T(baby ? "sheep/sheep_wool_baby" : "sheep/sheep_wool");
        if (!baby && color != 0) {
            const int under = assets_.model("sheep#wool_undercoat"), underTex = T("sheep/sheep_wool_undercoat");
            if (under >= 0 && underTex >= 0) out[n++] = {under, underTex, kWhite, TintWool};
        }
        if (woolModel >= 0 && woolTex >= 0) out[n++] = {woolModel, woolTex, kWhite, TintWool};
    }
    if ((d.special == SpWolf || d.special == SpCat) && e.tame && n < 6) {  // collar layer
        const char* base = d.special == SpWolf ? "wolf/wolf_collar" : "cat/cat_collar";
        const int t = withBaby(base);
        if (t >= 0) out[n++] = {out[0].model, t, kWhite, TintCollar};
    }
    if ((d.special == SpVillager || d.special == SpZombieVillager) && n < 5) {
        // VillagerProfessionLayer: biome type, then profession (none / nitwit-free) on the same model.
        const std::string dir = d.special == SpVillager ? "villager/" : "zombie_villager/";
        const std::string& type = world_.villagerType(e.villagerType);
        const std::string& prof = world_.profession(e.variant2);
        const int typeTex = T(dir + (baby ? "baby/" : "type/") + type);
        if (!type.empty() && typeTex >= 0) out[n++] = {out[0].model, typeTex, kWhite, 0};
        const int profTex = baby || prof.empty() || prof == "none" ? -1 : T(dir + "profession/" + prof);
        if (profTex >= 0) out[n++] = {out[0].model, profTex, kWhite, 0};
    }
    if (d.special == SpHorse && e.variant2 > 0 && e.variant2 <= 4 && n < 6) {
        static const char* kMarks[5] = {"", "white", "whitefield", "whitedots", "blackdots"};
        const int t = withBaby(std::string("horse/horse_markings_") + kMarks[e.variant2]);
        if (t >= 0) out[n++] = {out[0].model, t, kWhite, 0};
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------------

bool EntityRenderer::create(Renderer& r) {
    renderer_ = &r;
    ID3D12Device* device = r.device();

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 5;
    ranges[1] = ranges[0];
    ranges[1].BaseShaderRegister = 6;
    D3D12_ROOT_PARAMETER params[10]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b0
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b1
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.Num32BitValues = 2;
    for (int i = 0; i < 5; ++i) {  // t0..t4
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2 + i].Descriptor.ShaderRegister = UINT(i);
        params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    for (int i = 0; i < 2; ++i) {
        params[7 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[7 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[7 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[7 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t7: last frame's part matrices
    params[9].Descriptor.ShaderRegister = 7;
    params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_STATIC_SAMPLER_DESC samp[2]{};
    samp[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp[0].AddressU = samp[0].AddressV = samp[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp[0].MaxLOD = D3D12_FLOAT32_MAX;
    samp[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samp[1] = samp[0];
    samp[1].Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    samp[1].AddressU = samp[1].AddressV = samp[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp[1].ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 10;
    rd.pParameters = params;
    rd.NumStaticSamplers = 2;
    rd.pStaticSamplers = samp;
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        LOG("entities: root signature: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    if (!check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig_)),
               "CreateRootSignature"))
        return false;

    // targets: 1 colour; 2 colour + motion (written); 3 colour + motion (kept); 0 shadow map (depth only)
    auto pso = [&](const void* vs, size_t vsSize, const void* ps, size_t psSize, bool blend, D3D12_CULL_MODE cull,
                   ComPtr<ID3D12PipelineState>& out, int targets = 1) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = rootSig_.Get();
        d.VS = {vs, vsSize};
        d.PS = {ps, psSize};
        auto& rt = d.BlendState.RenderTarget[0];
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (ps == g_EntityGlintPS) {  // GLINT_TRANSPARENCY: src colour * src colour + dst, alpha kept
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_SRC_COLOR, rt.DestBlend = D3D12_BLEND_ONE, rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ZERO, rt.DestBlendAlpha = D3D12_BLEND_ONE, rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        } else if (blend) {
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = cull;
        d.RasterizerState.FrontCounterClockwise = TRUE;  // model quads keep the game's (OpenGL) counter-clockwise winding
        d.RasterizerState.DepthClipEnable = TRUE;
        d.DepthStencilState.DepthEnable = TRUE;
        d.DepthStencilState.DepthWriteMask = blend ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reversed Z
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = targets == 0 ? 0 : targets == 1 ? 1 : 2;
        d.RTVFormats[0] = targets ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_UNKNOWN;
        if (targets >= 2) {
            d.RTVFormats[1] = RenderPass::kMotionFormat;
            d.BlendState.IndependentBlendEnable = TRUE;
            d.BlendState.RenderTarget[1].RenderTargetWriteMask = targets == 2 ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
        }
        if (targets == 0) {  // shadow caster: depth only, no depth clip, slope bias like the terrain's model shadows
            d.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
            d.RasterizerState.DepthClipEnable = FALSE;
            d.RasterizerState.SlopeScaledDepthBias = 1.5f;
            d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        }
        d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        d.SampleDesc.Count = 1;
        return check(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)), "CreateGraphicsPipelineState");
    };
#define SH(n) g_##n, sizeof(g_##n)
    const auto back = D3D12_CULL_MODE_BACK, noCull = D3D12_CULL_MODE_NONE;
    if (!pso(SH(EntityModelVS), SH(EntityCutoutPS), false, back, psoCutout_) ||
        !pso(SH(EntityModelVS), SH(EntityTranslucentPS), true, back, psoTranslucent_) ||
        !pso(SH(EntityFreeVS), SH(EntityCutoutPS), false, noCull, psoFree_) ||
        !pso(SH(EntityModelVS), SH(EntityGlintPS), true, noCull, psoGlint_) ||
        !pso(SH(EntityModelVS), SH(EntityCutoutMvPS), false, back, psoCutoutMv_, 2) ||
        !pso(SH(EntityModelVS), SH(EntityTranslucentPS), true, back, psoTranslucentMv_, 3) ||
        !pso(SH(EntityFreeVS), SH(EntityCutoutMvPS), false, noCull, psoFreeMv_, 2) ||
        !pso(SH(EntityModelVS), SH(EntityGlintPS), true, noCull, psoGlintMv_, 3) ||
        !pso(SH(EntityShadowVS), SH(EntityShadowPS), false, noCull, psoShadow_, 0) ||
        !pso(SH(EntityShadowFreeVS), SH(EntityShadowPS), false, noCull, psoShadowFree_, 0))
        return false;
#undef SH

    auto buffer = [&](u64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp{heap};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = std::max<u64>(size, 256);
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(&out)),
                     "CreateCommittedResource");
    };
    const auto& quads = assets_.quads();
    if (!buffer(quads.size() * sizeof(GpuQuad), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, quads_) ||
        !r.uploadBuffer(quads_.Get(), quads.data(), quads.size() * sizeof(GpuQuad),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
        return false;
    if (!buffer(kFrameBytes * Renderer::kFrames, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, upload_))
        return false;
    D3D12_RANGE none{0, 0};
    if (!check(upload_->Map(0, &none, reinterpret_cast<void**>(&mapped_)), "Map")) return false;

    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = assets_.atlasWidth();
    td.Height = assets_.atlasHeight();
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    if (!check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&atlas_)),
               "CreateCommittedResource(atlas)") ||
        !r.uploadTexture(atlas_.Get(), {assets_.atlas().data()}))
        return false;
    atlasDescriptor_ = r.allocDescriptors(1);
    if (atlasDescriptor_ == UINT32_MAX) {
        LOG("entities: no free descriptor for the atlas");
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(atlas_.Get(), &sd, r.cpuDescriptor(atlasDescriptor_));
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// Per-frame building.

// HumanoidArmorLayer + WingsLayer: armour pieces (equipment assets) and elytra as extra model layers.
int EntityRenderer::equipmentLayers(const Entity& e, const TypeDef& d, Layer* out, int max) {
    if (!items_ || max <= 0) return 0;
    int n = 0;
    const bool smallModel = d.special == SpArmorStand ? (e.standFlags & 1) != 0 : e.baby;
    static const int kOrder[4] = {4, 3, 2, 5};  // chest, legs, feet, head (EquipmentSlot)
    for (int slot : kOrder) {
        const int item = e.equipment[slot];
        if (item < 0) continue;
        const ItemModels::Equippable& eq = items_->equippable(item);
        if (eq.slot != slot || eq.asset.empty()) continue;
        const int piece = 5 - slot;
        const int model = smallModel ? d.armorBaby[piece] : (d.special == SpPlayer && e.slim) ? d.armorSlim[piece] : d.armor[piece];
        if (model < 0) continue;
        const int type = smallModel && d.special != SpArmorStand ? 2 : slot == 3 ? 1 : 0;  // humanoid(_leggings/_baby)
        bool glint = (e.equipFoil >> slot) & 1;  // EquipmentLayerRenderer: the first drawn layer carries the glint
        for (const EquipmentLayer& l : items_->itemLayers(item, type)) {
            if (l.tex < 0 || n >= max) continue;
            u32 color = kWhite;
            if (l.dyeable) {
                const i32 dye = e.equipDye[slot] >= 0 ? e.equipDye[slot] : l.undyed;
                if (dye < 0) continue;  // getColorForLayer: 0 = not drawn
                color = abgr(u32(dye));
            }
            out[n++] = {model, l.tex, color, u8(Armor | (glint ? Glint : 0))};
            glint = false;
        }
    }
    const int chest = e.equipment[4];
    const int elytra = smallModel ? elytraBabyModel_ : elytraModel_;
    if (chest >= 0 && elytra >= 0 &&
        (d.anim == Anim::Humanoid || d.anim == Anim::Zombie || d.anim == Anim::Skeleton || d.anim == Anim::ArmorStand)) {
        for (const EquipmentLayer& l : items_->itemLayers(chest, 3))
                if (l.tex >= 0 && n < max) out[n++] = {elytra, l.tex, kWhite, u8(Elytra | ((e.equipFoil & 16) ? Glint : 0))};
    }
    return n;
}

bool EntityRenderer::modelEntity(const Entity& e, const TypeDef& d, float pt, const double cam[3], u32 light) {
    Layer layers[16];
    int layerCount = 0;
    if (d.kind == Kind::Mob) layerCount = resolve(e, d, layers);
    if (!layerCount) return false;
    if (e.category == Category::Living || e.category == Category::Player)
        layerCount += equipmentLayers(e, d, layers + layerCount, 16 - layerCount);

    double p[3];
    for (int a = 0; a < 3; ++a) p[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt - cam[a];
    const float yaw = lerpAngle(pt, e.prevYaw, e.yaw), pitch = e.prevPitch + (e.pitch - e.prevPitch) * pt;
    const bool living = e.category == Category::Living || e.category == Category::Player;

    // Root transform (LivingEntityRenderer.submit / setupRotations, or the vehicle / projectile renderers).
    M34 root = translate(float(p[0]), float(p[1]), float(p[2]));
    AnimState s;
    u32 overlay = 0;
    if (d.special == SpBoat) {
        root = mul(root, translate(0, 0.375f, 0));
        root = mul(root, rotY((180.0f - yaw) * kDeg));
        const float hurt = float(e.vehicleHurt) - pt;
        if (hurt > 0) root = mul(root, rotX(std::sin(hurt) * hurt * std::max(e.vehicleDamage - pt, 0.0f) / 10.0f * float(e.vehicleHurtDir) * kDeg));
        root = mul(root, scale(-1, -1, 1));
        root = mul(root, rotY(90.0f * kDeg));
    } else if (d.special == SpMinecart) {
        root = mul(root, translate(0, 0.375f, 0));
        root = mul(root, rotY((180.0f - yaw) * kDeg));
        root = mul(root, rotZ(-pitch * kDeg));
        const float hurt = float(e.vehicleHurt) - pt;
        if (hurt > 0) root = mul(root, rotX(std::sin(hurt) * hurt * std::max(e.vehicleDamage - pt, 0.0f) / 10.0f * float(e.vehicleHurtDir) * kDeg));
        root = mul(root, scale(-1, -1, 1));
    } else if (d.special == SpArrow || d.special == SpTrident) {
        root = mul(root, rotY((yaw - 90.0f) * kDeg));
        root = mul(root, rotZ((pitch + (d.special == SpTrident ? 90.0f : 0.0f)) * kDeg));
    } else if (living) {
        const float bodyYaw = lerpAngle(pt, e.prevBodyYaw, e.bodyYaw);
        const float headYaw = lerpAngle(pt, e.prevHeadYaw, e.headYaw);
        if (e.category == Category::Player && e.pose == 5) root = mul(root, translate(0, -2.0f / 16.0f, 0));
        if (d.special == SpArmorStand) {
            root = mul(root, rotY((180.0f - yaw) * kDeg));
        } else {
            root = mul(root, rotY((180.0f - bodyYaw) * kDeg));
            if (e.deathTime > 0) {
                float fall = std::sqrt(std::max(0.0f, (float(e.deathTime) + pt - 1.0f) / 20.0f * 1.6f));
                root = mul(root, rotZ(std::min(fall, 1.0f) * d.flip * kDeg));
            }
        }
        root = mul(root, scale(-1, -1, 1));
        float sc = d.scale;
        float sx = sc, sy = sc, sz = sc;
        if (d.special == SpSlime) sx *= float(e.size), sy *= float(e.size), sz *= float(e.size);
        if (d.special == SpCreeper) {
            const float swelling = (float(e.oldSwell) + (float(e.swell) - float(e.oldSwell)) * pt) / 28.0f;
            const float wobble = 1.0f + std::sin(swelling * 100.0f) * swelling * 0.01f;
            float g = std::clamp(swelling, 0.0f, 1.0f);
            g *= g, g *= g;
            sx *= (1.0f + g * 0.4f) * wobble, sz *= (1.0f + g * 0.4f) * wobble, sy *= (1.0f + g * 0.1f) / wobble;
            if (int(swelling * 10.0f) % 2 != 0) overlay |= u32(std::clamp(swelling, 0.5f, 1.0f) * 0.75f * 255.0f) << 8;
        }
        const bool babyFallback = e.baby && d.base.babyModel < 0 && d.special != SpPlayer && d.special != SpArmorStand;
        if (babyFallback) sx *= 0.5f, sy *= 0.5f, sz *= 0.5f;
        if (sx != 1 || sy != 1 || sz != 1) root = mul(root, scale(sx, sy, sz));
        root = mul(root, translate(0, -1.501f, 0));
        if (e.hurtTime > 0 || e.deathTime > 0) overlay |= 77;  // OverlayTexture red: 30 %

        s.walkPos = (e.walkPos - e.walkSpeed * (1.0f - pt)) * e.walkScale;
        s.walkSpeed = std::min(e.walkSpeedOld + (e.walkSpeed - e.walkSpeedOld) * pt, 1.0f);
        float rel = headYaw - bodyYaw;
        rel = std::fmod(rel, 360.0f);
        if (rel >= 180) rel -= 360;
        if (rel < -180) rel += 360;
        s.headYaw = rel;
        s.pitch = pitch;
        if (d.special == SpArmorStand) s.headYaw = 0, s.pitch = 0;
        s.age = float(e.age) + pt;
        s.swing = e.swingTime >= 0 ? (float(e.swingTime) + pt) / float(e.swingDuration) : 0.0f;
        s.flap = e.oFlap + (e.flap - e.oFlap) * pt;
        s.flapSpeed = e.oFlapSpeed + (e.flapSpeed - e.oFlapSpeed) * pt;
        s.baby = e.baby;
        s.crouching = e.pose == 5;
        s.riding = e.vehicle >= 0;
        s.aggressive = (e.mobFlags & 4) != 0;
        const bool rightHanded = e.mainArm != 0;
        s.rightItem = rightHanded ? e.equipment[0] >= 0 : e.equipment[1] >= 0;
        s.leftItem = rightHanded ? e.equipment[1] >= 0 : e.equipment[0] >= 0;
        s.swingLeft = !rightHanded;
        s.standFlags = e.standFlags;
        s.standPose = e.standPose;
        s.skinParts = e.category == Category::Player ? e.skinParts : 0x7F;
    } else {
        root = mul(root, rotY((180.0f - yaw) * kDeg));
        root = mul(root, scale(-1, -1, 1));
        root = mul(root, translate(0, -1.501f, 0));
    }

    PartPose rolePose[RoleCount];
    M34 roleMat[RoleCount];
    u64 roleSet = 0, baseHidden = 0, roleMatSet = 0;
    for (int li = 0; li < layerCount; ++li) {
        const Layer& L = layers[li];
        const Model& m = assets_.modelAt(L.model);
        const size_t parts = m.parts.size();
        if (matCount_ + parts > kMaxMatrices || instCount_ >= kMaxInstances || refCount_ + m.quadCount > kMaxRefs)
            return false;
        poses_.resize(parts);
        mats_.resize(parts);
        hidden_.resize(parts);
        for (size_t i = 0; i < parts; ++i) poses_[i] = m.parts[i].pose;
        u64 hiddenRoles = 0;
        M34 layerRoot = root;
        if (L.flags & Elytra) {  // WingsLayer: translate(0, 0, 0.125); ElytraModel crouching pose
            layerRoot = mul(root, translate(0, 0, 0.125f));
            if (s.crouching) {
                const i16 lw = m.role(LeftWing), rw = m.role(RightWing);
                if (lw >= 0) poses_[size_t(lw)].y = 3, poses_[size_t(lw)].xRot = 0.6981317f, poses_[size_t(lw)].zRot = -kPi / 4;
                if (rw >= 0) poses_[size_t(rw)].y = 3, poses_[size_t(rw)].xRot = 0.6981317f, poses_[size_t(rw)].zRot = kPi / 4;
            }
        } else if ((L.flags & Armor) && roleSet) {  // HumanoidModel.copyPropertiesTo: the parent's pose by part
            for (size_t i = 0; i < parts; ++i) {
                const Role r = m.parts[i].role;
                if (r != RoleNone && ((roleSet >> r) & 1)) poses_[i] = rolePose[r];
            }
            hiddenRoles = baseHidden & ((1ull << LeftArm) | (1ull << RightArm));
        } else if (living) {
            AnimState sl = s;
            if (L.flags & Armor) sl.skinParts = 0x7F;
            hiddenRoles = animate(d.anim, sl, m, poses_.data());
            if (li == 0) {
                baseHidden = hiddenRoles;
                for (size_t i = 0; i < parts; ++i)
                    if (const Role r = m.parts[i].role; r != RoleNone) rolePose[r] = poses_[i], roleSet |= 1ull << r;
            }
        }
        const bool armor = (L.flags & Armor) && roleMatSet;
        for (size_t i = 0; i < parts; ++i) {
            const ModelPart& part = m.parts[i];
            if (armor && part.role != RoleNone && ((roleMatSet >> part.role) & 1)) {
                *reinterpret_cast<M34*>(&mats_[i]) = roleMat[part.role];  // same pose and chain as the parent model
            } else {
                const M34 local = partLocal(poses_[i]);
                const M34 parent = part.parent >= 0 ? *reinterpret_cast<const M34*>(&mats_[size_t(part.parent)]) : layerRoot;
                *reinterpret_cast<M34*>(&mats_[i]) = mul(parent, local);
            }
            hidden_[i] = ((hiddenRoles >> part.role) & 1) && part.role != RoleNone ? 1 : 0;
            if (part.parent >= 0 && hidden_[size_t(part.parent)]) hidden_[i] = 1;
        }
        if (li == 0 && living)
            for (size_t i = 0; i < parts; ++i)
                if (const Role r = m.parts[i].role; r != RoleNone)
                    roleMat[r] = *reinterpret_cast<const M34*>(&mats_[i]), roleMatSet |= 1ull << r;
        if (li == 0 && living && attachments_.size() < 4096) {  // ItemInHandLayer / CustomHeadLayer anchors
            Attachment a{};
            a.entity = &e == localPlayer_ ? kLocalPlayerId : e.id;
            a.light = light;
            a.babyOffset = e.baby && d.special != SpArmorStand;
            a.shadowOnly = shadowOnly_;
            const i16 arms[2] = {m.role(RightArm), m.role(LeftArm)};
            for (int k = 0; k < 2; ++k) {
                if (arms[k] < 0 || hidden_[size_t(arms[k])]) continue;
                M34 hm = *reinterpret_cast<const M34*>(&mats_[size_t(arms[k])]);
                if (d.special == SpPlayer && e.slim) {  // PlayerModel.translateToHand: slim arms move 0.5 px out
                    const ModelPart& ap = m.parts[size_t(arms[k])];
                    const M34 par = ap.parent >= 0 ? *reinterpret_cast<const M34*>(&mats_[size_t(ap.parent)]) : root;
                    const float off = (k == 0 ? 0.5f : -0.5f) / 16.0f;
                    for (int r = 0; r < 3; ++r) hm.m[r * 4 + 3] += par.m[r * 4] * off;
                }
                std::memcpy(a.hand[k], hm.m, sizeof(a.hand[k]));
                a.valid |= u8(1 << k);
            }
            if (const i16 h = m.role(Head); h >= 0) {
                std::memcpy(a.head, &mats_[size_t(h)], sizeof(a.head));
                a.valid |= 4;
            }
            if (a.valid) attachments_.push_back(a);
        }
        const u32 base = matCount_;
        std::memcpy(frameBase_ + kMatOffset + u64(base) * 48, mats_.data(), parts * 48);
        matCount_ += u32(parts);
        if (!pack_) {  // motion vectors: this entity's matrices for the next frame
            entMats_.insert(entMats_.end(), mats_.begin(), mats_.begin() + ptrdiff_t(parts));
            entHash_ = entHash_ * 31 + u32(L.model) * 7 + u32(parts);
        }
        const TexRect& tr = assets_.rect(L.tex);
        GpuInstance inst{base, L.color, (L.flags & Emissive) ? (light | 256u) : light, overlay,
                         {tr.u0, tr.v0, tr.du, tr.dv}};
        const u32 instId = instCount_++;
        std::memcpy(frameBase_ + kInstOffset + u64(instId) * 32, &inst, 32);
        const bool translucent = (L.flags & Translucent) != 0;
        if (pack_) {  // shader pack: the posed quads as vertices (entity atlas coordinates)
            const auto& quads = assets_.quads();
            // Armour glint (TextureTransform.setupGlintTexturing, scale 0.16, glint speed 0.5) as gbuffers_armor_glint
            // geometry: the texture transform is applied here (the glint texture wraps).
            const bool glint = (L.flags & Glint) && pack_->glintTexture(false);
            float scroll[2] = {};
            if (glint) {
                const double ms = nowSeconds() * 1000.0 * 8.0 * 0.5;
                scroll[0] = -float(std::fmod(ms, 110000.0) / 110000.0), scroll[1] = float(std::fmod(ms, 30000.0) / 30000.0);
            }
            for (size_t i = 0; i < parts; ++i) {
                if (hidden_[i]) continue;
                const ModelPart& part = m.parts[i];
                const float* pm = mats_[i].m;
                for (u32 q = part.quadFirst; q < part.quadFirst + part.quadCount && q < quads.size(); ++q) {
                    const GpuQuad& gq = quads[q];
                    float pos[4][3], uv[4][2], n[3];
                    for (int c = 0; c < 4; ++c) {
                        const float lp[3] = {gq.pos[c][0] / 16.0f, gq.pos[c][1] / 16.0f, gq.pos[c][2] / 16.0f};
                        for (int r = 0; r < 3; ++r) pos[c][r] = pm[r * 4] * lp[0] + pm[r * 4 + 1] * lp[1] + pm[r * 4 + 2] * lp[2] + pm[r * 4 + 3];
                        uv[c][0] = tr.u0 + gq.uv[c][0] * tr.du, uv[c][1] = tr.v0 + gq.uv[c][1] * tr.dv;
                    }
                    for (int r = 0; r < 3; ++r) n[r] = pm[r * 4] * gq.normal[0] + pm[r * 4 + 1] * gq.normal[1] + pm[r * 4 + 2] * gq.normal[2];
                    packQuad(translucent ? 1 : 0, pos, uv, n, L.color, inst.light, overlay);
                    if (glint) {
                        const float gc = 0.98480775f, gs = 0.17364818f;  // 10 degrees
                        for (int c = 0; c < 4; ++c) {
                            const float gx = gq.uv[c][0] * 0.16f, gy = gq.uv[c][1] * 0.16f;
                            uv[c][0] = gx * gc - gy * gs + scroll[0], uv[c][1] = gx * gs + gy * gc + scroll[1];
                        }
                        packQuad(4, pos, uv, n, 0xBFFFFFFFu, 256u, 0);  // alpha: glint strength 0.75 (option default)
                    }
                }
            }
            continue;
        }
        u32* refs = reinterpret_cast<u32*>(frameBase_ + kRefOffset);
        for (size_t i = 0; i < parts; ++i) {
            if (hidden_[i]) continue;
            const ModelPart& part = m.parts[i];
            for (u32 q = part.quadFirst; q < part.quadFirst + part.quadCount; ++q) {
                if ((L.flags & Glint) && !shadowOnly_) glint_.push_back(q), glint_.push_back(instId);
                if (translucent) {
                    translucent_.push_back(q);
                    translucent_.push_back(instId);
                } else {
                    refs[refCount_ * 2] = q;
                    refs[refCount_ * 2 + 1] = instId;
                    ++refCount_;
                }
            }
        }
    }
    return true;
}

void EntityRenderer::emitFree(const float pos[4][3], const float uv[4][2], const float n[3], u32 tex, u32 color,
                              u32 light, u32 overlay) {
    if (pack_) {
        if (tex == 0xFFFFFFFF) return packQuad(2, pos, uv, n, color, light, overlay);
        float auv[4][2];  // block texture layer -> block atlas
        for (int c = 0; c < 4; ++c) pack_->atlasUv(tex, uv[c][0], uv[c][1], auv[c]);
        return packQuad(3, pos, auv, n, color, light, overlay);
    }
    if (freeCount_ >= kMaxFree) return;
    FreeQuad q;
    std::memcpy(q.pos, pos, sizeof(q.pos));
    std::memcpy(q.uv, uv, sizeof(q.uv));
    std::memcpy(q.normal, n, sizeof(q.normal));
    q.tex = tex, q.color = color, q.light = light, q.overlay = overlay;
    std::copy(freeMotion_, freeMotion_ + 3, q.motion);
    q.pad[0] = q.pad[1] = 0;
    std::memcpy(frameBase_ + kFreeOffset + u64(freeCount_++) * sizeof(FreeQuad), &q, sizeof(FreeQuad));
}

// One block model in block-local [0,1] space transformed by m (cubes: the 6 faces with the terrain's UV rules).
void EntityRenderer::emitBlock(u16 block, const Mat34& mm, u32 light, u32 overlay) {
    const M34& m = *reinterpret_cast<const M34*>(&mm);
    BlockRegistry& reg = world_.registry();
    const BlockInfo& b = reg.infos()[block];
    static const u32 kTints[8] = {0xFFFFFF, 0x91BD59, 0x77AB2F, 0x80A755, 0x619961, 0x3F76E4, 0x208030, 0x8B1A00};
    static const float kNormals[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    if (b.kind == KindCube) {
        for (int f = 0; f < 6; ++f) {
            const CubeFace& face = b.faces[f];
            const float* nrm = kNormals[f];
            const float axisA[3] = {f < 4 ? 1.0f : 0.0f, 0, f < 4 ? 0.0f : 1.0f};
            const float axisB[3] = {0, f < 2 ? 0.0f : 1.0f, f < 2 ? 1.0f : 0.0f};
            float pos[4][3], uv[4][2];
            static const int kOrder[4] = {0, 1, 3, 2};  // Z order -> perimeter
            for (int c = 0; c < 4; ++c) {
                const int corner = kOrder[c];
                const float a = float(corner & 1), bb = float(corner >> 1);
                float p[3];
                for (int k = 0; k < 3; ++k) p[k] = std::max(nrm[k], 0.0f) + axisA[k] * a + axisB[k] * bb;
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
                const float su = rot == 0 ? u : rot == 1 ? v : rot == 2 ? -u : -v;
                const float sv = rot == 0 ? v : rot == 1 ? -u : rot == 2 ? -v : u;
                uv[c][0] = su, uv[c][1] = sv;
                xform(m, p, pos[c]);
            }
            float n[3];
            xformDir(m, nrm, n);
            emitFree(pos, uv, n, face.tex, abgr(kTints[face.tint & 7]), light, overlay);
        }
    } else if (b.kind == KindModel) {
        for (u32 i = 0; i < b.quadCount; ++i) {
            const ModelQuad& q = reg.modelQuad(b.quadFirst + i);
            float pos[4][3], uv[4][2];
            static const int kOrder[4] = {0, 1, 3, 2};
            for (int c = 0; c < 4; ++c) {
                xform(m, q.pos[kOrder[c]], pos[c]);
                uv[c][0] = q.uv[kOrder[c]][0], uv[c][1] = q.uv[kOrder[c]][1];
            }
            const float e1[3] = {q.pos[1][0] - q.pos[0][0], q.pos[1][1] - q.pos[0][1], q.pos[1][2] - q.pos[0][2]};
            const float e2[3] = {q.pos[2][0] - q.pos[0][0], q.pos[2][1] - q.pos[0][1], q.pos[2][2] - q.pos[0][2]};
            const float nl[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            float n[3];
            xformDir(m, nl, n);
            emitFree(pos, uv, n, q.tex, abgr(kTints[q.tint & 7]), light, overlay);
        }
    }
}

void EntityRenderer::freeEntity(const Entity& e, const TypeDef& d, float pt, const double cam[3], u32 light,
                                const float* camRight) {
    double p[3];
    for (int a = 0; a < 3; ++a) p[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt - cam[a];
    BlockRegistry& reg = world_.registry();
    if (d.kind == Kind::Block) {
        if (!d.block.empty() && !d.blockId) d.blockId = reg.idFor(world_.reports().complete(d.block));
        const u16 block = !d.block.empty() ? d.blockId : world_.localBlock(e.blockState);
        if (!block) return;
        M34 m = translate(float(p[0]) - 0.5f, float(p[1]), float(p[2]) - 0.5f);
        emitBlock(block, *reinterpret_cast<const Mat34*>(&m), light, 0);
        return;
    }
    if (d.kind == Kind::Sprite) {
        // ThrownItemRenderer: a camera-facing item sprite (GROUND transform: 0.5 blocks, centre 2/16 up).
        const TexRect& t = assets_.rect(d.sprite);
        const float size = 0.5f * d.spriteScale;
        const float up[3] = {camRight[3], camRight[4], camRight[5]};
        const float c[3] = {float(p[0]), float(p[1]) + 0.125f * d.spriteScale, float(p[2])};
        float pos[4][3];
        const float sx[4] = {-0.5f, 0.5f, 0.5f, -0.5f}, sy[4] = {-0.5f, -0.5f, 0.5f, 0.5f};
        for (int k = 0; k < 4; ++k)
            for (int a = 0; a < 3; ++a) pos[k][a] = c[a] + (camRight[a] * sx[k] + up[a] * sy[k]) * size;
        float du = t.du, dv = t.dv;
        if (d.special == SpXpOrb) du *= 0.25f, dv *= 0.25f;
        const float uv[4][2] = {{t.u0, t.v0 + dv}, {t.u0 + du, t.v0 + dv}, {t.u0 + du, t.v0}, {t.u0, t.v0}};
        const float n[3] = {0, 1, 0};
        const u32 color = d.special == SpXpOrb ? abgr(0xB0FF40) : kWhite;
        emitFree(pos, uv, n, 0xFFFFFFFF, color, light | 512u, 0);
        return;
    }
    // ItemEntityRenderer: bob and spin, GROUND display transform; several copies for larger stacks.
    if (e.itemId < 0 || size_t(e.itemId) >= assets_.itemNames().size()) return;
    const std::string& item = assets_.itemNames()[size_t(e.itemId)];
    const float age = float(e.age) + pt;
    const float bob = std::sin(age / 10.0f + e.bobOffset) * 0.1f + 0.1f;
    const float spin = age / 20.0f + e.bobOffset;
    const int copies = 1 + (e.itemCount > 1) + (e.itemCount > 16) + (e.itemCount > 32) + (e.itemCount > 48);
    // Full blocks render as small cubes, everything else as a flat sprite (item texture or block icon).
    if (itemBlocks_.size() < assets_.itemNames().size()) itemBlocks_.assign(assets_.itemNames().size(), -1);
    i32& cached = itemBlocks_[size_t(e.itemId)];
    if (cached < 0) {
        const std::string key = "minecraft:" + item;
        cached = world_.reports().hasBlock(key) ? reg.idFor(world_.reports().complete(key)) : 0;
    }
    const u16 block = u16(cached);
    const BlockInfo* bi = block ? &reg.infos()[block] : nullptr;
    const bool cube = bi && (bi->kind == KindCube || (bi->kind == KindModel && bi->layer != LayerCutout && bi->quadCount));
    const M34 base = mul(translate(float(p[0]), float(p[1]) + bob + 1.0f / 16.0f, float(p[2])), rotY(spin));
    u32 rng = u32(e.itemId) * 2654435761u + 1;
    auto rnd = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return float(rng >> 8) / float(1 << 24) * 2.0f - 1.0f;
    };
    if (cube) {
        for (int c = 0; c < copies; ++c) {
            const float o[3] = {c ? rnd() * 0.15f : 0, c ? rnd() * 0.15f : 0, c ? rnd() * 0.15f : 0};
            M34 m = mul(base, translate(o[0] - 0.125f, o[1], o[2] - 0.125f));
            m = mul(m, scale(0.25f, 0.25f, 0.25f));
            emitBlock(block, *reinterpret_cast<const Mat34*>(&m), light, 0);
        }
        return;
    }
    int atlasTex = assets_.itemTexture(item);
    u32 tex = 0xFFFFFFFF;
    float uv[4][2];
    if (atlasTex >= 0) {
        const TexRect& t = assets_.rect(atlasTex);
        const float u[4][2] = {{t.u0, t.v0 + t.dv}, {t.u0 + t.du, t.v0 + t.dv}, {t.u0 + t.du, t.v0}, {t.u0, t.v0}};
        std::memcpy(uv, u, sizeof(uv));
    } else if (bi && bi->icon) {
        tex = bi->icon;
        const float u[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        std::memcpy(uv, u, sizeof(uv));
    } else {
        return;
    }
    for (int c = 0; c < copies; ++c) {
        const M34 m = mul(base, translate(c ? rnd() * 0.075f : 0, c ? rnd() * 0.075f : 0, float(c) * 0.046875f));
        const float corners[4][3] = {{-0.25f, 0, 0}, {0.25f, 0, 0}, {0.25f, 0.5f, 0}, {-0.25f, 0.5f, 0}};
        float pos[4][3];
        for (int k = 0; k < 4; ++k) xform(m, corners[k], pos[k]);
        const float nl[3] = {0, 0, 1};
        float n[3];
        xformDir(m, nl, n);
        emitFree(pos, uv, n, tex, kWhite, light | 512u, 0);
    }
}

void EntityRenderer::prepare(const PassContext& ctx) {
    if (ctx.pack) return;  // shader packs: built in draw (the pack's own frame)
    build(ctx);
    prepared_ = true;
}

void EntityRenderer::draw(const PassContext& ctx) {
    if (!prepared_) build(ctx);
    prepared_ = false;
    if (built_) record(ctx);
}

namespace {
// Sphere (camera-relative centre, radius) against a shadow cascade's light matrix (column-major, ortho).
bool inCascade(const float* m, const float c[3], float r) {
    for (int row = 0; row < 3; ++row) {
        const float v = m[row] * c[0] + m[4 + row] * c[1] + m[8 + row] * c[2] + m[12 + row];
        const float len = std::sqrt(m[row] * m[row] + m[4 + row] * m[4 + row] + m[8 + row] * m[8 + row]);
        if (row < 2 ? std::abs(v) > 1 + r * len : v > 1 + r * len) return false;  // casters before the near plane clamp
    }
    return true;
}
}  // namespace

void EntityRenderer::build(const PassContext& ctx) {
    Timer timer;
    const auto& entities = world_.entities();
    stats_ = {};
    stats_.entities = u32(entities.size());
    attachments_.clear();
    casters_.clear();
    built_ = false;
    if ((entities.empty() && !localPlayer_ && !localShadow_) || !mapped_) return;
    frameBase_ = mapped_ + u64(ctx.frame) * kFrameBytes;
    matCount_ = instCount_ = refCount_ = freeCount_ = 0;
    pack_ = ctx.pack;
    translucent_.clear();
    glint_.clear();
    ++buildFrame_;

    // Frustum planes from the camera-relative view-projection (column-major memory, like the renderer's).
    auto vp = [&](int r, int c) { return ctx.viewProj[c * 4 + r]; };
    float planes[5][4];
    for (int c = 0; c < 4; ++c) {
        planes[0][c] = vp(3, c) + vp(0, c);
        planes[1][c] = vp(3, c) - vp(0, c);
        planes[2][c] = vp(3, c) + vp(1, c);
        planes[3][c] = vp(3, c) - vp(1, c);
        planes[4][c] = vp(3, c) - vp(2, c);
    }
    float camVec[6];  // right, up (world space) for sprites
    for (int r = 0; r < 2; ++r) {
        float l = 0;
        for (int c = 0; c < 3; ++c) l += vp(r, c) * vp(r, c);
        l = std::sqrt(std::max(l, 1e-12f));
        for (int c = 0; c < 3; ++c) camVec[r * 3 + c] = vp(r, c) / l;
    }
    const double cam[3] = {ctx.camX, ctx.camY, ctx.camZ};
    const float pt = std::clamp(world_.partialTick(), 0.0f, 1.0f);
    const float maxDist = std::min(ctx.fogEnd, 192.0f);
    const u32 shadowMask = pack_ || !ctx.shadowMats ? 0u : ctx.shadowMask;
    World& world = world_.world();
    std::shared_lock lock(world.mutex());
    // 0: not drawn, 1: in view, 2: only casts a shadow into the view.
    auto classify = [&](const Entity& e, float sphere[4]) -> int {
        if (e.type < 0 || size_t(e.type) >= defs_.size()) return 0;
        const TypeDef& d = defs_[size_t(e.type)];
        if (d.kind == Kind::None || !e.info) return 0;
        if (d.kind == Kind::Item && externalItems_) return 0;  // 3D item models: the visuals pass
        if (e.category == Category::Living && (e.sharedFlags & 0x20)) return 0;  // invisible
        float w = e.info->width, h = e.info->height;
        if (d.special == SpSlime) w *= float(e.size), h *= float(e.size);
        sphere[3] = std::max(w, h) * 0.75f * d.scale + 0.5f;
        for (int a = 0; a < 3; ++a) sphere[a] = float(e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt - cam[a]);
        sphere[1] += h * 0.5f;
        const float dist = std::sqrt(sphere[0] * sphere[0] + sphere[1] * sphere[1] + sphere[2] * sphere[2]);
        if (dist - sphere[3] > maxDist) return 0;
        if (&e != localShadow_) {
            bool inside = true;
            for (const auto& pl : planes) {
                const float len = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
                if (pl[0] * sphere[0] + pl[1] * sphere[1] + pl[2] * sphere[2] + pl[3] < -sphere[3] * len) {
                    inside = false;
                    break;
                }
            }
            if (inside) return 1;
        }
        for (u32 c = 0; c < 4; ++c)
            if ((shadowMask >> c) & 1 && inCascade(ctx.shadowMats[c], sphere, sphere[3])) return 2;
        return 0;
    };
    auto drawEntity = [&](const Entity& e, const float sphere[4]) {
        const TypeDef& d = defs_[size_t(e.type)];
        double c[3];
        for (int a = 0; a < 3; ++a) c[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt - cam[a];
        float w = e.info->width, h = e.info->height;
        if (d.special == SpSlime) w *= float(e.size), h *= float(e.size);
        const float cx = float(c[0]), cz = float(c[2]);
        // Light at the entity's eye position (EntityRenderer samples getLightProbePosition = the eyes).
        const int lx = int(std::floor(e.pos[0])), ly = int(std::floor(e.pos[1] + e.info->eye)),
                  lz = int(std::floor(e.pos[2]));
        u32 light = 0xF0;
        if (ly >= world.minSectionY() * 16 && ly < (world.maxSectionY() + 1) * 16)
            light = world.light(lx >> 4, ly >> 4, lz >> 4).at(lx & 15, ly & 15, lz & 15);
        // Motion: last frame's pose of this entity (prev_), compared after posing.
        PrevPose* prev = nullptr;
        if (!pack_) {
            prev = &prev_[e.id];
            const bool fresh = prev->frame + 1 == buildFrame_;
            for (int a = 0; a < 3; ++a)
                freeMotion_[a] = fresh ? float(c[a] + cam[a] - prev->pos[a]) : 0.0f;
            entMats_.clear();
            entHash_ = 0;
        }
        Caster k{{sphere[0], sphere[1], sphere[2]}, sphere[3], refCount_, 0, u32(translucent_.size() / 2), 0, freeCount_, 0};
        const u32 matBase = matCount_;
        bool drawn = false;
        if (d.kind == Kind::Mob) drawn = modelEntity(e, d, pt, cam, light);
        else if (d.kind == Kind::Item || d.kind == Kind::Block || d.kind == Kind::Sprite) {
            const u32 before = freeCount_;
            freeEntity(e, d, pt, cam, light, camVec);
            drawn = freeCount_ > before;
        }
        if (d.kind == Kind::Box || (!drawn && d.kind == Kind::Mob)) {
            // Fallback: the entity's bounding box as a white cube.
            const float half = w * 0.5f;
            const float lo[3] = {cx - half, float(c[1]), cz - half}, hi[3] = {cx + half, float(c[1]) + h, cz + half};
            static const float kN[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
            for (int f = 0; f < 6; ++f) {
                const int axis = f / 2;
                const float v = (f & 1) ? hi[axis] : lo[axis];
                const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
                float pos[4][3];
                const float s1[4] = {0, 1, 1, 0}, s2[4] = {0, 0, 1, 1};
                for (int q = 0; q < 4; ++q) {
                    pos[q][axis] = v;
                    pos[q][a1] = s1[q] ? hi[a1] : lo[a1];
                    pos[q][a2] = s2[q] ? hi[a2] : lo[a2];
                }
                const TexRect& t = assets_.rect(0);
                const float uv[4][2] = {{t.u0, t.v0}, {t.u0, t.v0}, {t.u0, t.v0}, {t.u0, t.v0}};
                emitFree(pos, uv, kN[f], 0xFFFFFFFF, 0xFFB0B0B0, light, 0);
            }
            drawn = true;
        }
        stats_.drawn += drawn && !shadowOnly_;
        if (pack_) {
            packFlush(e);
            return;
        }
        // Last frame's matrices for the motion vectors (same model layers), re-expressed relative to this camera.
        const u32 mats = matCount_ - matBase;
        if (mats) {
            Mat34* out = reinterpret_cast<Mat34*>(frameBase_ + kPrevMatOffset) + matBase;
            if (prev->frame + 1 == buildFrame_ && prev->hash == entHash_ && prev->mats.size() == mats && mats == entMats_.size()) {
                const float shift[3] = {float(prev->cam[0] - cam[0]), float(prev->cam[1] - cam[1]), float(prev->cam[2] - cam[2])};
                for (u32 i = 0; i < mats; ++i) {
                    Mat34 m = prev->mats[i];
                    m.m[3] += shift[0], m.m[7] += shift[1], m.m[11] += shift[2];
                    out[i] = m;
                }
            } else {
                std::memcpy(out, entMats_.data(), std::min<size_t>(entMats_.size(), mats) * sizeof(Mat34));
            }
        }
        prev->frame = buildFrame_;
        prev->hash = entHash_;
        prev->mats.swap(entMats_);
        std::copy(cam, cam + 3, prev->cam);
        for (int a = 0; a < 3; ++a) prev->pos[a] = c[a] + cam[a];
        std::fill(freeMotion_, freeMotion_ + 3, 0.0f);
        k.refN = refCount_ - k.ref;
        k.trN = u32(translucent_.size() / 2) - k.tr;
        k.freeN = freeCount_ - k.free;
        if (shadowMask && (k.refN || k.trN || k.freeN)) casters_.push_back(k);
    };
    // Entities in view first, then those that only cast a shadow into it (their ranges follow the view's).
    static thread_local std::vector<std::pair<const Entity*, std::array<float, 4>>> shadowOnly;
    shadowOnly.clear();
    shadowOnly_ = false;
    auto visit = [&](const Entity& e) {
        float sphere[4];
        const int k = classify(e, sphere);
        if (k == 1) drawEntity(e, sphere);
        else if (k == 2) shadowOnly.push_back({&e, {sphere[0], sphere[1], sphere[2], sphere[3]}});
    };
    for (const auto& [id, e] : entities) visit(e);
    if (localPlayer_) visit(*localPlayer_);
    if (localShadow_ && !pack_) visit(*localShadow_);
    camCutout_ = refCount_, camFree_ = freeCount_, camTranslucent_ = u32(translucent_.size() / 2);
    shadowOnly_ = true;
    for (const auto& [e, s] : shadowOnly) drawEntity(*e, s.data());
    shadowOnly_ = false;
    lock.unlock();
    if (!pack_ && buildFrame_ % 256 == 0)  // forget entities not seen for a while
        for (auto it = prev_.begin(); it != prev_.end();) it = it->second.frame + 8 < buildFrame_ ? prev_.erase(it) : std::next(it);
    cutoutRefs_ = refCount_;
    u32* refs = reinterpret_cast<u32*>(frameBase_ + kRefOffset);
    const u32 extra = std::min<u32>(u32(translucent_.size() / 2), kMaxRefs - refCount_);
    std::memcpy(refs + u64(refCount_) * 2, translucent_.data(), u64(extra) * 8);
    refCount_ += extra;
    camTranslucent_ = std::min(camTranslucent_, extra);
    glintRefs_ = refCount_;
    const u32 glints = std::min<u32>(u32(glint_.size() / 2), kMaxRefs - refCount_);
    std::memcpy(refs + u64(refCount_) * 2, glint_.data(), u64(glints) * 8);
    refCount_ += glints;

    // Frame constants.
    struct Constants {
        float viewProj[16];
        float fogColor[4];
        float fogParams[4];
        float light0[4], light1[4];
        float glintRect[4], glint[4];
        float lightmap[3][4];
        float prevViewProj[16];
        float motion[4];
        float shadowMat[4][16];
    } k{};
    static_assert(sizeof(Constants) <= kConstBytes);
    std::memcpy(k.lightmap, ctx.lightmap, sizeof(k.lightmap));
    std::memcpy(k.viewProj, ctx.viewProj, sizeof(k.viewProj));
    std::memcpy(k.fogColor, ctx.fogColor, sizeof(k.fogColor));
    k.fogParams[0] = ctx.fogStart, k.fogParams[1] = ctx.fogEnd, k.fogParams[2] = ctx.daylight;
    k.fogParams[3] = ctx.shading ? 1.0f : 0.0f;  // shader pipeline: output for its relighting (entity.hlsl)
    const float l0[3] = {0.2f, 1.0f, -0.7f}, l1[3] = {-0.2f, 1.0f, 0.7f};
    const float n0 = 1.0f / std::sqrt(0.04f + 1.0f + 0.49f);
    for (int a = 0; a < 3; ++a) k.light0[a] = l0[a] * n0, k.light1[a] = l1[a] * n0;
    static const int glintTex = assets_.texture("misc/enchanted_glint_armor");
    if (glintTex >= 0) {  // TextureTransform.setupGlintTexturing (glint speed 0.5)
        const TexRect& g = assets_.rect(glintTex);
        k.glintRect[0] = g.u0, k.glintRect[1] = g.v0, k.glintRect[2] = g.du, k.glintRect[3] = g.dv;
        const double ms = nowSeconds() * 1000.0 * 8.0 * 0.5;
        k.glint[0] = -float(std::fmod(ms, 110000.0) / 110000.0), k.glint[1] = float(std::fmod(ms, 30000.0) / 30000.0);
        k.glint[2] = 0.75f;
    }
    if (ctx.prevViewProj) std::memcpy(k.prevViewProj, ctx.prevViewProj, sizeof(k.prevViewProj));
    std::memcpy(k.motion, ctx.motionScale, sizeof(k.motion));
    if (ctx.shadowMats) std::memcpy(k.shadowMat, ctx.shadowMats, sizeof(k.shadowMat));
    std::memcpy(frameBase_, &k, sizeof(k));

    stats_.instances = instCount_;
    stats_.quads = refCount_;
    stats_.freeQuads = freeCount_;
    stats_.cpuMs = timer.ms();
    cpuTotal_ += stats_.cpuMs;
    {
        const u32 h = u32(frames_ % kHistory);
        historyMs_[h] = float(stats_.cpuMs);
        historyWorldMs_[h] = float(world_.stats().applyMs + world_.stats().tickMs);
        historyDrawn_[h] = stats_.drawn;
        historyQuads_[h] = refCount_;
    }
    entityFrames_ += stats_.drawn;
    ++frames_;
    if (pack_) {  // drawn by the shader pack (packFlush)
        pack_ = nullptr;
        return;
    }
    built_ = refCount_ || freeCount_;
}

void EntityRenderer::record(const PassContext& ctx) {
    ID3D12GraphicsCommandList* list = ctx.list;
    const D3D12_GPU_VIRTUAL_ADDRESS va = upload_->GetGPUVirtualAddress() + u64(ctx.frame) * kFrameBytes;
    ID3D12DescriptorHeap* heaps[] = {renderer_->descriptorHeap()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(rootSig_.Get());
    list->SetGraphicsRootConstantBufferView(0, va);
    list->SetGraphicsRootShaderResourceView(2, quads_->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(3, va + kMatOffset);
    list->SetGraphicsRootShaderResourceView(4, va + kInstOffset);
    list->SetGraphicsRootShaderResourceView(5, va + kRefOffset);
    list->SetGraphicsRootShaderResourceView(6, va + kFreeOffset);
    list->SetGraphicsRootDescriptorTable(7, renderer_->gpuDescriptor(atlasDescriptor_));
    list->SetGraphicsRootDescriptorTable(8, ctx.blockTextures);
    list->SetGraphicsRootShaderResourceView(9, va + kPrevMatOffset);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto drawRange = [&](ID3D12PipelineState* pso, u32 first, u32 count) {
        if (!count) return;
        const u32 consts[2] = {first, 0};
        list->SetPipelineState(pso);
        list->SetGraphicsRoot32BitConstants(1, 2, consts, 0);
        list->DrawInstanced(count * 6, 1, 0, 0);
    };
    const bool mv = ctx.motionRtv.ptr != 0;  // shader pipeline: motion vectors into the second target
    if (mv) {
        const D3D12_CPU_DESCRIPTOR_HANDLE rts[2] = {ctx.colorRtv, ctx.motionRtv};
        list->OMSetRenderTargets(2, rts, FALSE, &ctx.dsv);
    }
    drawRange((mv ? psoCutoutMv_ : psoCutout_).Get(), 0, camCutout_);
    drawRange((mv ? psoFreeMv_ : psoFree_).Get(), 0, camFree_);
    drawRange((mv ? psoTranslucentMv_ : psoTranslucent_).Get(), cutoutRefs_, camTranslucent_);
    if (assets_.texture("misc/enchanted_glint_armor") >= 0)
        drawRange((mv ? psoGlintMv_ : psoGlint_).Get(), glintRefs_, refCount_ - glintRefs_);
    if (mv) list->OMSetRenderTargets(1, &ctx.colorRtv, FALSE, &ctx.dsv);
}

void EntityRenderer::drawShadow(const ShadowContext& sc) {
    if (casters_.empty() || !built_) return;
    ID3D12GraphicsCommandList* list = sc.list;
    const D3D12_GPU_VIRTUAL_ADDRESS va = upload_->GetGPUVirtualAddress() + u64(sc.frame) * kFrameBytes;
    list->SetGraphicsRootSignature(rootSig_.Get());
    list->SetGraphicsRootConstantBufferView(0, va);
    list->SetGraphicsRootShaderResourceView(2, quads_->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(3, va + kMatOffset);
    list->SetGraphicsRootShaderResourceView(4, va + kInstOffset);
    list->SetGraphicsRootShaderResourceView(5, va + kRefOffset);
    list->SetGraphicsRootShaderResourceView(6, va + kFreeOffset);
    list->SetGraphicsRootDescriptorTable(7, renderer_->gpuDescriptor(atlasDescriptor_));
    list->SetGraphicsRootDescriptorTable(8, sc.blockTextures);
    list->SetGraphicsRootShaderResourceView(9, va + kPrevMatOffset);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // Casters inside this cascade; neighbouring entities' ranges merge into one draw.
    ID3D12PipelineState* bound = nullptr;
    auto draw = [&](ID3D12PipelineState* pso, u32 first, u32 count) {
        if (!count) return;
        if (pso != bound) list->SetPipelineState(bound = pso);
        const u32 consts[2] = {first, sc.cascade};
        list->SetGraphicsRoot32BitConstants(1, 2, consts, 0);
        list->DrawInstanced(count * 6, 1, 0, 0);
    };
    for (int kind = 0; kind < 3; ++kind) {  // model cutout, model translucent, free quads
        ID3D12PipelineState* pso = (kind == 2 ? psoShadowFree_ : psoShadow_).Get();
        u32 runFirst = 0, runCount = 0;
        for (const Caster& c : casters_) {
            const u32 first = kind == 0 ? c.ref : kind == 1 ? cutoutRefs_ + c.tr : c.free;
            const u32 count = kind == 0 ? c.refN : kind == 1 ? c.trN : c.freeN;
            if (!count || !inCascade(sc.matrix, c.c, c.r)) continue;
            if (runCount && runFirst + runCount == first) {
                runCount += count;
                continue;
            }
            draw(pso, runFirst, runCount);
            runFirst = first, runCount = count;
        }
        draw(pso, runFirst, runCount);
    }
}

// ---- shader packs ----

void EntityRenderer::packQuad(int bucket, const float pos[4][3], const float uv[4][2], const float n[3], u32 color,
                              u32 light, u32 overlay) {
    // Vanilla UV2 (light * 16; emissive layers full bright) and UV1 overlay (white flash u, hurt v = 3 else 10).
    const bool bright = (light & 256) != 0;
    const u32 sky = bright ? 15 : (light >> 4) & 15, block = bright ? 15 : light & 15;
    const u32 white = (overlay >> 8) & 255, red = overlay & 255;
    packOverlay_ |= overlay;
    PackVertex v{};
    v.color = color;
    v.light = (block << 4) | ((sky << 4) << 16);
    v.overlay = (white * 15 / 255) | ((red ? 3u : 10u) << 16);
    v.normal = packNormal(n[0], n[1], n[2]);
    v.entity = -1;
    const float mid[2] = {(uv[0][0] + uv[2][0]) * 0.5f, (uv[0][1] + uv[2][1]) * 0.5f};
    std::vector<PackVertex>& out = bucket == 4 ? g_packGlint : bucket == 3 ? packBlock_ : packVerts_[bucket];
    for (int c = 0; c < 4; ++c) {
        std::copy(pos[c], pos[c] + 3, v.pos);
        v.uv[0] = uv[c][0], v.uv[1] = uv[c][1];
        v.midUv[0] = mid[0], v.midUv[1] = mid[1];
        out.push_back(v);
    }
}

void EntityRenderer::packFlush(const Entity& e) {
    const int id = pack_->entityId(e.info ? e.info->name : std::string_view());
    float color[4] = {0, 0, 0, 0};  // entityColor: hurt red / creeper white flash
    if ((packOverlay_ >> 8) & 255) color[0] = color[1] = color[2] = 1, color[3] = float((packOverlay_ >> 8) & 255) / 255.0f;
    else if (packOverlay_ & 255) color[0] = 1, color[3] = float(packOverlay_ & 255) / 255.0f;
    auto submit = [&](std::vector<PackVertex>& v, PackRole role, PackBlend blend, bool blockAtlas) {
        if (v.empty()) return;
        PackDraw d;
        d.role = role, d.blend = blend;
        d.blockAtlas = blockAtlas;
        d.texture = blockAtlas ? nullptr : atlas_.Get();
        d.vertices = v.data(), d.vertexCount = u32(v.size());
        d.entityId = id;
        std::copy(color, color + 4, d.entityColor);
        d.cull = role != PackRole::EntitiesTranslucent;
        pack_->draw(d);
        v.clear();
    };
    submit(packVerts_[0], PackRole::Entities, PackBlend::Opaque, false);
    submit(packVerts_[2], PackRole::Entities, PackBlend::Opaque, false);
    submit(packBlock_, PackRole::Entities, PackBlend::Opaque, true);
    submit(packVerts_[1], PackRole::EntitiesTranslucent, PackBlend::Translucent, false);
    if (!g_packGlint.empty()) {
        PackDraw d;
        d.role = PackRole::ArmorGlint, d.blend = PackBlend::Glint;
        d.texture = pack_->glintTexture(false);
        d.depthWrite = false, d.depthEqual = true;  // culled like the base: faces behind it never show the glint
        d.vertices = g_packGlint.data(), d.vertexCount = u32(g_packGlint.size());
        d.entityId = id;
        pack_->draw(d);
        g_packGlint.clear();
    }
    packOverlay_ = 0;
}

}  // namespace ent
