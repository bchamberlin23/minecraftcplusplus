#include "entity/entity_render.h"

#include "entity/species.h"

#include "assets/assets.h"
#include "assets/game_reports.h"
#include "world/world.h"
#include "ui/item_props.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
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
#include "EntityOutlinePS.h"
#include "BlitPS.h"
#include "BlurPS.h"
#include "OutlineVS.h"
#include "SobelPS.h"

namespace ent {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;

// Per-frame upload capacities.
constexpr u32 kMaxMatrices = 1 << 15, kMaxInstances = 1 << 14, kMaxRefs = 1 << 18, kMaxFree = 1 << 13;
constexpr u64 kConstBytes = 2048;
constexpr u64 kOutlineConstOffset = 1024;  // the frame constants again with the unjittered view (the glowing outline)
constexpr u64 kMatOffset = kConstBytes;
constexpr u64 kPrevMatOffset = kMatOffset + u64(kMaxMatrices) * 48;  // last frame's part matrices (motion vectors)
constexpr u64 kInstOffset = kPrevMatOffset + u64(kMaxMatrices) * 48;
constexpr u64 kRefOffset = kInstOffset + u64(kMaxInstances) * 32;
constexpr u64 kFreeOffset = kRefOffset + u64(kMaxRefs) * 8;
constexpr u64 kFrameBytes = (kFreeOffset + u64(kMaxFree) * 128 + 65535) & ~u64(65535);

enum LayerFlags : u16 {
    Emissive = 1, Translucent = 2, Additive = 4, TintWool = 8, TintCollar = 16, Armor = 32, Elytra = 64, Glint = 128,
    Scroll = 256,     // texture coordinates wrap within the texture, scrolled by Layer::scroll (breeze wind, energy swirl)
    NoOverlay = 512,  // OverlayTexture.NO_OVERLAY (no hurt tint)
    SpinFx = 1024,    // the riptide swirl (SpinAttackEffectLayer)
    Cape = 2048,      // CapeLayer (PlayerCapeModel): posed like the base model, the cape swinging
    DisplayBlock = 4096,  // a minecart's display block drawn as a model (the chest): under the display transform
    Dissolve = 8192,      // entityCutoutDissolve: scroll[] = the mask texture's atlas offset; alpha in the colour
    Shoulder = 16384,     // ParrotOnShoulderLayer: scroll[0] = the side (+0.4 left, -0.4 right)
    Stuck = 32768,        // StuckInBodyLayer: scroll[0] = the item's index, scroll[1] = 0 arrow (in cube) / 1 stinger
};
enum Special : u8 {
    SpNone, SpCow, SpPig, SpChicken, SpSheep, SpWolf, SpCat, SpHorse, SpLlama, SpRabbit, SpParrot, SpFox, SpAxolotl,
    SpPanda, SpMooshroom, SpVillager, SpZombieVillager, SpSlime, SpPlayer, SpFrog, SpArmorStand, SpCreeper, SpBoat,
    SpMinecart, SpArrow, SpXpOrb, SpTrident, SpDisplay, SpShulker, SpCushion, SpSulfurCube, SpZombieNautilus,
    SpSalmon, SpPufferfish, SpTropicalFish, SpWither, SpDragon, SpObject
};
// DyeColor names by id.
const char* const kDyeNames[16] = {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                                   "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};

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

// EntityRenderer.shadowRadius by entity type (the renderers' constructors); types not listed cast none.
float shadowRadiusOf(std::string_view id) {
    static const std::unordered_map<std::string_view, float> kRadius = {
        {"allay", 0.4f}, {"armadillo", 0.4f}, {"axolotl", 0.5f}, {"bat", 0.25f}, {"bee", 0.4f}, {"blaze", 0.5f},
        {"bogged", 0.5f}, {"breeze", 0.5f}, {"camel", 0.7f}, {"camel_husk", 0.7f}, {"cat", 0.4f}, {"cave_spider", 0.56f},
        {"chicken", 0.3f}, {"cod", 0.3f}, {"copper_golem", 0.5f}, {"cow", 0.7f}, {"creaking", 0.6f}, {"creeper", 0.5f},
        {"dolphin", 0.7f}, {"donkey", 0.75f}, {"drowned", 0.5f}, {"elder_guardian", 1.2f}, {"enderman", 0.5f},
        {"endermite", 0.3f}, {"ender_dragon", 0.5f}, {"end_crystal", 0.5f}, {"evoker", 0.5f}, {"experience_orb", 0.15f},
        {"falling_block", 0.5f}, {"fox", 0.4f}, {"frog", 0.3f}, {"ghast", 1.5f}, {"giant", 3.0f}, {"glow_squid", 0.7f},
        {"goat", 0.7f}, {"guardian", 0.5f}, {"happy_ghast", 2.0f}, {"hoglin", 0.7f}, {"horse", 0.75f}, {"husk", 0.5f},
        {"illusioner", 0.5f}, {"iron_golem", 0.7f}, {"item", 0.15f}, {"llama", 0.7f}, {"mooshroom", 0.7f}, {"mule", 0.75f},
        {"nautilus", 0.7f}, {"ocelot", 0.4f}, {"panda", 0.9f}, {"parched", 0.5f}, {"parrot", 0.3f}, {"phantom", 0.75f},
        {"pig", 0.7f}, {"piglin", 0.5f}, {"piglin_brute", 0.5f}, {"pillager", 0.5f}, {"player", 0.5f}, {"polar_bear", 0.9f},
        {"pufferfish", 0.2f}, {"rabbit", 0.3f}, {"ravager", 1.1f}, {"salmon", 0.4f}, {"sheep", 0.7f}, {"silverfish", 0.3f},
        {"skeleton", 0.5f}, {"skeleton_horse", 0.75f}, {"sniffer", 1.1f}, {"snow_golem", 0.5f}, {"spider", 0.8f},
        {"squid", 0.7f}, {"stray", 0.5f}, {"strider", 0.5f}, {"tadpole", 0.14f}, {"tnt", 0.5f}, {"trader_llama", 0.7f},
        {"tropical_fish", 0.15f}, {"turtle", 0.7f}, {"vex", 0.3f}, {"villager", 0.5f}, {"vindicator", 0.5f},
        {"wandering_trader", 0.5f}, {"warden", 0.9f}, {"witch", 0.5f}, {"wither", 1.0f}, {"wither_skeleton", 0.5f},
        {"wolf", 0.5f}, {"zoglin", 0.7f}, {"zombie", 0.5f}, {"zombie_horse", 0.75f}, {"zombie_nautilus", 0.7f},
        {"zombie_villager", 0.5f}, {"zombified_piglin", 0.5f}, {"minecart", 0.7f}, {"chest_minecart", 0.7f},
        {"command_block_minecart", 0.7f}, {"furnace_minecart", 0.7f}, {"hopper_minecart", 0.7f},
        {"spawner_minecart", 0.7f}, {"tnt_minecart", 0.7f}};
    if (id.starts_with("minecraft:")) id.remove_prefix(10);
    if (id.ends_with("_boat") || id.ends_with("_raft")) return 0.8f;
    const auto it = kRadius.find(id);
    return it == kRadius.end() ? 0.0f : it->second;
}

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
    outlinePass_.owner = this;
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
        auto extra = [&](const std::string& model, const std::string& tex, u16 flags, const std::string& babyModel = "",
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
            mob(Anim::Drowned, "drowned#main", "zombie/drowned", "drowned_baby#main", "zombie/drowned_baby");
            extra("drowned#outer", "zombie/drowned_outer_layer", 0, "drowned_baby#outer", "zombie/drowned_outer_layer_baby");
        } else if (n == "zombie_villager") {
            mob(Anim::Zombie, "zombie_villager#main", "zombie_villager/zombie_villager", "zombie_villager_baby#main",
                "zombie_villager/zombie_villager_baby");
            d.special = SpZombieVillager;
        } else if (n == "zombified_piglin")
            mob(Anim::ZombifiedPiglin, "zombified_piglin#main", "piglin/zombified_piglin", "zombified_piglin_baby#main",
                "piglin/zombified_piglin_baby");
        else if (n == "piglin") mob(Anim::Piglin, "piglin#main", "piglin/piglin", "piglin_baby#main", "piglin/piglin_baby");
        else if (n == "piglin_brute") mob(Anim::Piglin, "piglin_brute#main", "piglin/piglin_brute");
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
            mob(Anim::Enderman, "enderman#main", "enderman/enderman");
            extra("enderman#main", "enderman/enderman_eyes", Emissive | Translucent);
        } else if (n == "witch") mob(Anim::Witch, "witch#main", "witch/witch");
        else if (n == "villager") mob(Anim::Villager, "villager#main", "villager/villager", "villager_baby#main", "villager/villager_baby"), d.special = SpVillager;
        else if (n == "wandering_trader") mob(Anim::WanderingTrader, "wandering_trader#main", "wandering_trader/wandering_trader");
        else if (n == "pillager" || n == "vindicator" || n == "evoker" || n == "illusioner") mob(Anim::Illager, n + "#main", "illager/" + n);
        else if (n == "vex") mob(Anim::Vex, "vex#main", "illager/vex"), d.base.flags = Translucent;
        else if (n == "ravager") mob(Anim::Ravager, "ravager#main", "illager/ravager");
        else if (n == "evoker_fangs") mob(Anim::EvokerFangs, "evoker_fangs#main", "illager/evoker_fangs"), d.special = SpObject;
        else if (n == "iron_golem") mob(Anim::IronGolem, "iron_golem#main", "iron_golem/iron_golem");
        else if (n == "cow") mob(Anim::Quadruped, "cow#main", "cow/cow_temperate", "cow_baby#main", "cow/cow_temperate_baby"), d.special = SpCow;
        else if (n == "mooshroom") mob(Anim::Quadruped, "mooshroom#main", "cow/mooshroom_red", "mooshroom_baby#main", "cow/mooshroom_red_baby"), d.special = SpMooshroom;
        else if (n == "pig") mob(Anim::Quadruped, "pig#main", "pig/pig_temperate", "pig_baby#main", "pig/pig_temperate_baby"), d.special = SpPig;
        else if (n == "sheep") {
            mob(Anim::Sheep, "sheep#main", "sheep/sheep", "sheep_baby#main", "sheep/sheep_baby");
            d.special = SpSheep;
        } else if (n == "chicken") mob(Anim::Chicken, "chicken#main", "chicken/chicken_temperate", "chicken_baby#main", "chicken/chicken_temperate_baby"), d.special = SpChicken;
        else if (n == "wolf") mob(Anim::Wolf, "wolf#main", "wolf/wolf", "wolf_baby#main", "wolf/wolf_baby"), d.special = SpWolf;
        else if (n == "cat") mob(Anim::Feline, "cat#main", "cat/cat_tabby", "cat_baby#main", "cat/cat_tabby_baby"), d.special = SpCat;
        else if (n == "ocelot") mob(Anim::Feline, "ocelot#main", "cat/ocelot", "ocelot_baby#main", "cat/ocelot_baby");
        else if (n == "horse") mob(Anim::Horse, "horse#main", "horse/horse_white", "horse_baby#main", "horse/horse_white_baby"), d.special = SpHorse;
        else if (n == "donkey" || n == "mule") mob(Anim::Horse, n + "#main", "horse/" + n, n + "_baby#main", "horse/" + n + "_baby");
        else if (n == "skeleton_horse") mob(Anim::Horse, "skeleton_horse#main", "horse/horse_skeleton", "skeleton_horse_baby#main", "horse/horse_skeleton_baby");
        else if (n == "zombie_horse") mob(Anim::Horse, "zombie_horse#main", "horse/horse_zombie", "zombie_horse_baby#main", "horse/horse_zombie_baby");
        else if (n == "llama" || n == "trader_llama") mob(Anim::Llama, n + "#main", "llama/llama_creamy", n + "_baby#main", "llama/llama_creamy_baby"), d.special = SpLlama;
        else if (n == "rabbit") mob(Anim::Rabbit, "rabbit#main", "rabbit/rabbit_brown", "rabbit_baby#main", "rabbit/rabbit_brown_baby"), d.special = SpRabbit;
        else if (n == "parrot") mob(Anim::Parrot, "parrot#main", "parrot/parrot_red_blue"), d.special = SpParrot;
        else if (n == "fox") mob(Anim::Fox, "fox#main", "fox/fox", "fox_baby#main", "fox/fox_baby"), d.special = SpFox;
        else if (n == "axolotl") mob(Anim::Axolotl, "axolotl#main", "axolotl/axolotl_lucy", "axolotl_baby#main", "axolotl/axolotl_lucy_baby"), d.special = SpAxolotl;
        else if (n == "panda") mob(Anim::Panda, "panda#main", "panda/panda", "panda_baby#main", "panda/panda_baby"), d.special = SpPanda;
        else if (n == "frog") mob(Anim::Frog, "frog#main", "frog/frog_temperate"), d.special = SpFrog;
        else if (n == "polar_bear") mob(Anim::PolarBear, "polar_bear#main", "bear/polarbear", "polar_bear_baby#main", "bear/polarbear_baby");
        else if (n == "goat") mob(Anim::Goat, "goat#main", "goat/goat", "goat_baby#main", "goat/goat_baby");
        else if (n == "hoglin") mob(Anim::Hoglin, "hoglin#main", "hoglin/hoglin", "hoglin_baby#main", "hoglin/hoglin_baby");
        else if (n == "zoglin") mob(Anim::Hoglin, "zoglin#main", "hoglin/zoglin", "zoglin_baby#main", "hoglin/zoglin_baby");
        else if (n == "sniffer") mob(Anim::Sniffer, "sniffer#main", "sniffer/sniffer", "sniffer_baby#main", "sniffer/snifflet");
        else if (n == "glow_squid") mob(Anim::Squid, "glow_squid#main", "squid/glow_squid", "glow_squid_baby#main", "squid/glow_squid_baby");
        else if (n == "squid") mob(Anim::Squid, "squid#main", "squid/squid", "squid_baby#main", "squid/squid_baby");
        else if (n == "cod") mob(Anim::Cod, "cod#main", "fish/cod");
        else if (n == "salmon") mob(Anim::Salmon, "salmon#main", "fish/salmon"), d.special = SpSalmon;
        else if (n == "pufferfish") mob(Anim::Pufferfish, "pufferfish_big#main", "fish/pufferfish"), d.special = SpPufferfish;
        else if (n == "tropical_fish") mob(Anim::TropicalFish, "tropical_fish_small#main", "fish/tropical_a"), d.special = SpTropicalFish;
        else if (n == "strider") mob(Anim::Strider, "strider#main", "strider/strider", "strider_baby#main", "strider/strider_baby");
        else if (n == "slime") {
            mob(Anim::None, "slime#main", "slime/slime");
            extra("slime#outer", "slime/slime", Translucent);
            d.special = SpSlime;
        } else if (n == "magma_cube") mob(Anim::MagmaCube, "magma_cube#main", "slime/magmacube"), d.special = SpSlime;
        else if (n == "phantom") mob(Anim::Phantom, "phantom#main", "phantom/phantom"), extra("phantom#main", "phantom/phantom_eyes", Emissive | Translucent);
        else if (n == "guardian") mob(Anim::Guardian, "guardian#main", "guardian/guardian");
        else if (n == "elder_guardian") mob(Anim::Guardian, "elder_guardian#main", "guardian/guardian_elder");
        else if (n == "ghast") mob(Anim::Ghast, "ghast#main", "ghast/ghast");
        else if (n == "happy_ghast") mob(Anim::HappyGhast, "happy_ghast#main", "ghast/happy_ghast", "happy_ghast_baby#main", "ghast/happy_ghast_baby");
        else if (n == "snow_golem") mob(Anim::SnowGolem, "snow_golem#main", "snow_golem/snow_golem");
        else if (n == "copper_golem") mob(Anim::CopperGolem, "copper_golem#main", "copper_golem/copper_golem");
        else if (n == "turtle") mob(Anim::Turtle, "turtle#main", "turtle/turtle", "turtle_baby#main", "turtle/turtle_baby");
        else if (n == "dolphin") mob(Anim::Dolphin, "dolphin#main", "dolphin/dolphin", "dolphin_baby#main", "dolphin/dolphin_baby");
        else if (n == "camel") mob(Anim::Camel, "camel#main", "camel/camel", "camel_baby#main", "camel/camel_baby");
        else if (n == "camel_husk") mob(Anim::Camel, "camel#main", "camel/camel_husk");
        else if (n == "armadillo") mob(Anim::Armadillo, "armadillo#main", "armadillo/armadillo", "armadillo_baby#main", "armadillo/armadillo_baby");
        else if (n == "bee") mob(Anim::Bee, "bee#main", "bee/bee", "bee_baby#main", "bee/bee_baby");
        else if (n == "warden") mob(Anim::Warden, "warden#main", "warden/warden");
        else if (n == "wither") mob(Anim::Wither, "wither#main", "wither/wither"), d.special = SpWither;
        else if (n == "ender_dragon") mob(Anim::Dragon, "ender_dragon#main", "enderdragon/dragon"), d.special = SpDragon;
        else if (n == "allay") mob(Anim::Allay, "allay#main", "allay/allay"), d.base.flags = Translucent;
        else if (n == "bat") mob(Anim::Bat, "bat#main", "bat/bat");
        else if (n == "blaze") mob(Anim::Blaze, "blaze#main", "blaze/blaze");
        else if (n == "breeze") mob(Anim::Breeze, "breeze#main", "breeze/breeze"), d.base.flags = Translucent;
        else if (n == "creaking") mob(Anim::Creaking, "creaking#main", "creaking/creaking");
        else if (n == "endermite") mob(Anim::Endermite, "endermite#main", "endermite/endermite"), d.flip = 180.0f;
        else if (n == "silverfish") mob(Anim::Silverfish, "silverfish#main", "silverfish/silverfish"), d.flip = 180.0f;
        else if (n == "tadpole") mob(Anim::Tadpole, "tadpole#main", "tadpole/tadpole");
        else if (n == "nautilus") mob(Anim::Nautilus, "nautilus#main", "nautilus/nautilus", "nautilus_baby#main", "nautilus/nautilus_baby");
        else if (n == "armor_stand") {
            mob(Anim::ArmorStand, "armor_stand#main", "armorstand/armorstand", "armor_stand_small#main");
            d.special = SpArmorStand;
        } else if (n.ends_with("_chest_boat") || n.ends_with("_boat") || n.ends_with("_raft")) {
            std::string wood = n.substr(0, n.find(n.ends_with("_chest_boat") ? "_chest_boat" : n.ends_with("_boat") ? "_boat" : n.ends_with("_chest_raft") ? "_chest_raft" : "_raft"));
            const bool chest = n.find("_chest_") != std::string::npos;
            const std::string dir = chest ? "chest_boat" : "boat";
            mob(Anim::None, dir + "/" + wood + "#main", dir + "/" + wood);
            d.special = SpBoat;
            d.waterPatch = !n.ends_with("_raft");  // RaftRenderer has none
        } else if (n.ends_with("minecart")) {
            mob(Anim::None, n + "#main", "minecart/minecart");
            d.special = SpMinecart;
        } else if (n == "arrow") mob(Anim::Arrow, "arrow#main", "projectiles/arrow"), d.special = SpArrow;
        else if (n == "spectral_arrow") mob(Anim::Arrow, "arrow#main", "projectiles/arrow_spectral"), d.special = SpArrow;
        else if (n == "trident") mob(Anim::None, "trident#main", "trident/trident"), d.special = SpTrident;
        else if (n == "shulker_bullet") mob(Anim::ShulkerBullet, "shulker_bullet#main", "shulker/spark"), d.special = SpObject;
        else if (n == "wind_charge" || n == "breeze_wind_charge")
            mob(Anim::WindCharge, "wind_charge#main", "projectiles/wind_charge"), d.special = SpObject, d.base.flags = Translucent | Scroll;
        else if (n == "llama_spit") mob(Anim::LlamaSpit, "llama_spit#main", "llama/llama_spit"), d.special = SpObject;
        else if (n == "wither_skull") mob(Anim::WitherSkull, "wither_skull#main", "wither/wither"), d.special = SpObject;
        else if (n == "leash_knot") mob(Anim::None, "leash_knot#main", "lead_knot/lead_knot"), d.special = SpObject;
        else if (n == "end_crystal") mob(Anim::EndCrystal, "end_crystal#main", "end_crystal/end_crystal"), d.special = SpObject;
        else if (n == "shulker") mob(Anim::Shulker, "shulker#main", "shulker/shulker"), d.special = SpShulker;
        else if (n == "cushion") mob(Anim::None, "cushion#main", "cushion/white_cushion"), d.special = SpCushion;
        else if (n == "sulfur_cube") {  // SulfurCubeRenderer: the inner cube (SulfurCubeInnerLayer, drawn first), the outer
            mob(Anim::None, "sulfur_cube#inner", "sulfur_cube/sulfur_cube_inner", "sulfur_cube_small#inner",
                "sulfur_cube/sulfur_cube_inner_small");
            d.base.flags = Translucent;
            extra("sulfur_cube#main", "sulfur_cube/sulfur_cube_outer", Translucent, "sulfur_cube_small#main",
                  "sulfur_cube/sulfur_cube_outer_small");
            d.special = SpSulfurCube;
        } else if (n == "zombie_nautilus")
            mob(Anim::Nautilus, "zombie_nautilus#main", "nautilus/zombie_nautilus"), d.special = SpZombieNautilus;
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
        else if (n == "block_display") d.kind = Kind::Block, d.special = SpDisplay;  // DisplayRenderer.BlockDisplayRenderer
        else if (n == "area_effect_cloud" || n == "marker" || n == "interaction" || n == "item_display" ||
                 n == "text_display" || n == "lightning_bolt" || n == "ominous_item_spawner")
            d.kind = Kind::None;  // invisible, effect-only or drawn by the visuals pass (item / text displays)
        else if (n == "item_frame" || n == "glow_item_frame" || n == "painting")
            d.kind = Kind::None;  // block-attached decorations: the visuals pass
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
        for (const char* b : {"allay", "blaze", "magma_cube", "vex", "wither", "wither_skull", "shulker_bullet", "dragon_fireball",
                              "fireball", "small_fireball", "eye_of_ender"})
            if (n == b) d.blockLight = 1;  // full bright renderers (ThrownItemRenderer's fullBright ones too)
        if (n == "experience_orb") d.blockLight = 2;
        if (n == "glow_squid") d.blockLight = 3;
        if (d.kind == Kind::Mob && (d.base.model < 0 || d.base.tex < 0)) d.kind = Kind::Box;  // geometry fallback
        if (d.kind == Kind::Sprite && d.sprite < 0) d.kind = Kind::Box;
    }
    elytraModel_ = M("elytra#main"), elytraBabyModel_ = M("elytra_baby#main");
    spinModel_ = M("spin_attack#main"), spinTex_ = T("trident/trident_riptide");
    capeModel_ = M("player#cape");
    chestModel_ = M("chest#main"), chestTex_ = T("chest/normal");
    waterPatchModel_ = M("boat#water_patch");
    parrotModel_ = M("parrot#main"), earsModel_ = M("player#ears");
    arrowModel_ = M("arrow#main"), arrowTex_ = T("projectiles/arrow");
    stingerModel_ = M("bee_stinger#main"), stingerTex_ = T("bee/bee_stinger");
    u32 boxes = 0;
    std::string names;
    for (size_t i = 0; i < defs_.size(); ++i)
        if (defs_[i].kind == Kind::Box) ++boxes, names += " " + types[i].name.substr(types[i].name.find(':') + 1);
    LOG("entities: %u of %zu entity types fall back to a box:%s", boxes, defs_.size(), names.c_str());
}

// Layers depend on a few synched values only: resolve each combination once (texture names are built from
// strings) and reuse it, so steady-state frames do not allocate. Tints (wool, collar) are applied per frame.
int EntityRenderer::displayLayers(int type, Layer* out, int max, Anim& anim, float& scale) const {
    if (type < 0 || size_t(type) >= defs_.size() || defs_[size_t(type)].kind != Kind::Mob) return 0;
    const TypeDef& d = defs_[size_t(type)];
    Entity e;
    e.type = type;
    e.info = assets_.type(type);
    Layer layers[16];
    const int n = std::min(resolveUncached(e, d, layers), max);
    std::copy(layers, layers + n, out);
    anim = d.anim, scale = d.scale;
    return n;
}

int EntityRenderer::resolve(const Entity& e, const TypeDef& d, Layer* out) {
    u64 v1 = u64(d.special == SpPlayer ? e.skin : e.variant) & 0xFFF;
    if (d.special == SpSalmon) v1 = u64(dataInt(e, "DATA_TYPE", 1));            // Salmon.Variant (medium by default)
    else if (d.special == SpPufferfish) v1 = u64(dataInt(e, "PUFF_STATE", 0));  // the puff state picks the model
    const u64 key = (u64(u32(e.type)) << 48) | (v1 << 36) | ((u64(e.variant2) & 0xFFF) << 24) |
                    ((u64(e.villagerType) & 0xFF) << 16) | (u64(e.villagerLevel & 7) << 13) | (u64(e.baby) << 0) | (u64(e.tame) << 1) |
                    (u64(e.angry) << 2) | (u64(e.slim) << 3) | (u64((e.wool & 0x10) != 0) << 4) |
                    (u64((e.wool & 15) != 0 || (d.special == SpSheep && e.name == "jeb_")) << 5) | (u64(e.dyeColor & 31) << 6);
    auto it = layerCache_.find(key);
    if (it == layerCache_.end()) {
        CachedLayers c;
        c.count = u8(resolveUncached(e, d, c.layers));
        it = layerCache_.emplace(key, c).first;
    }
    const CachedLayers& c = it->second;
    const int wool = e.wool & 15;
    auto sheepColor = [](int dye) -> u32 {  // ColorLerper SHEEP: white is 0xE6E6E6, others diffuse * 0.75
        return dye == 0 ? 0xE6E6E6u
                        : (u32(((kDye[dye] >> 16) & 255) * 0.75f) << 16) | (u32(((kDye[dye] >> 8) & 255) * 0.75f) << 8) |
                              u32((kDye[dye] & 255) * 0.75f);
    };
    u32 woolRgb = sheepColor(wool);
    if (d.special == SpSheep && e.name == "jeb_") {  // ColorLerper.getLerpedColor(SHEEP, ageInTicks): 25 ticks a colour
        const float tick = float(e.age) + world_.partialTick();
        const int ticks = int(std::floor(tick)), value = ticks / 25;
        const float sub = (float(ticks % 25) + (tick - std::floor(tick))) / 25.0f;
        const u32 c1 = sheepColor(value % 16), c2 = sheepColor((value + 1) % 16);
        woolRgb = 0;
        for (int k = 0; k < 24; k += 8)
            woolRgb |= u32(int((c1 >> k) & 255) + int(std::floor(sub * float(int((c2 >> k) & 255) - int((c1 >> k) & 255))))) << k;
    }
    for (int i = 0; i < c.count; ++i) {
        out[i] = c.layers[i];
        if (out[i].flags & TintWool) out[i].color = abgr(woolRgb);
        if (out[i].flags & TintCollar) out[i].color = abgr(kDye[std::clamp(e.collar, 0, 15)]);
        if (d.special == SpTropicalFish && i < 2) {  // TropicalFishRenderer.getModelTint / TropicalFishPatternLayer
            const i64 packed = dataInt(e, "DATA_ID_TYPE_VARIANT");
            out[i].color = abgr(kDye[((i == 0 ? packed >> 16 : packed >> 24) & 255) % 16]);
        }
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
        case SpCushion:  // CushionRenderer.TEXTURES_BY_COLOR (white by default)
            if (const int t = T(std::string("cushion/") + kDyeNames[std::clamp(e.dyeColor, 0, 15)] + "_cushion"); t >= 0) tex = t;
            break;
        case SpShulker:  // ShulkerRenderer.getTextureLocation: no colour (16) is the default texture
            if (e.dyeColor >= 0 && e.dyeColor < 16)
                if (const int t = T(std::string("shulker/shulker_") + kDyeNames[e.dyeColor]); t >= 0) tex = t;
            break;
        case SpSalmon: {  // SalmonRenderer: the model by size
            static const char* kModels[3] = {"salmon_small#main", "salmon#main", "salmon_large#main"};
            if (const int m = assets_.model(kModels[std::clamp(int(dataInt(e, "DATA_TYPE", 1)), 0, 2)]); m >= 0) model = m;
            break;
        }
        case SpPufferfish: {  // PufferfishRenderer: the model by puff state
            static const char* kModels[3] = {"pufferfish_small#main", "pufferfish_medium#main", "pufferfish_big#main"};
            if (const int m = assets_.model(kModels[std::clamp(int(dataInt(e, "PUFF_STATE", 0)), 0, 2)]); m >= 0) model = m;
            break;
        }
        case SpTropicalFish: {  // TropicalFishRenderer: small / large model and texture (+ the pattern layer below)
            const bool large = (e.variant & 0xFF) == 1;
            if (const int m = assets_.model(large ? "tropical_fish_large#main" : "tropical_fish_small#main"); m >= 0) model = m;
            if (const int t = T(large ? "fish/tropical_b" : "fish/tropical_a"); t >= 0) tex = t;
            break;
        }
        case SpZombieNautilus:  // ZombieNautilusRenderer: the variant's model type and texture
            if (const Variant* v = variantOf("zombie_nautilus_variant")) {
                if (v->tex >= 0) tex = v->tex;
                if (v->model == "warm")
                    if (const int m = assets_.model("zombie_nautilus_coral#main"); m >= 0) model = m;
            }
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
    if (d.anim == Anim::ShulkerBullet) out[n++] = {model, tex, 0x26FFFFFFu, u16(Translucent | NoOverlay)};  // the halo (1.5 x)
    if (d.special == SpSheep && !(e.wool & 0x10)) {  // wool (SheepWoolLayer / SheepWoolUndercoatLayer)
        const int color = e.wool & 15;
        const int woolModel = assets_.model(baby ? "sheep_baby#wool" : "sheep#wool");
        const int woolTex = T(baby ? "sheep/sheep_wool_baby" : "sheep/sheep_wool");
        if (!baby && (color != 0 || e.name == "jeb_")) {
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
    if ((d.special == SpVillager || d.special == SpZombieVillager) && n < 4) {
        // VillagerProfessionLayer: biome type (on the no-hat model when the profession's hat covers it), profession,
        // profession level badge.
        const std::string dir = d.special == SpVillager ? "villager/" : "zombie_villager/";
        const std::string& type = world_.villagerType(e.villagerType);
        const std::string& prof = world_.profession(e.variant2);
        const int typeHat = assets_.villagerHat("entity/" + dir + "type/" + type);
        const int profHat = prof.empty() ? 0 : assets_.villagerHat("entity/" + dir + "profession/" + prof);
        const bool typeHatVisible = profHat == 0 || (profHat == 1 && typeHat != 2);
        int typeModel = out[0].model;
        if (!typeHatVisible) {
            const std::string base = d.special == SpVillager ? "villager" : "zombie_villager";
            if (const int m = assets_.model(base + (baby ? "_baby_no_hat#main" : "_no_hat#main")); m >= 0) typeModel = m;
        }
        const int typeTex = T(dir + (baby ? "baby/" : "type/") + type);
        if (!type.empty() && typeTex >= 0) out[n++] = {typeModel, typeTex, kWhite, 0};
        const int profTex = baby || prof.empty() || prof == "none" ? -1 : T(dir + "profession/" + prof);
        if (profTex >= 0) {
            out[n++] = {out[0].model, profTex, kWhite, 0};
            static const char* kLevels[5] = {"stone", "iron", "gold", "emerald", "diamond"};
            const int levelTex = prof == "nitwit" ? -1 : T(dir + "profession_level/" + kLevels[std::clamp(e.villagerLevel, 1, 5) - 1]);
            if (levelTex >= 0) out[n++] = {out[0].model, levelTex, kWhite, 0};
        }
    }
    if (d.special == SpTropicalFish && n < 6) {  // TropicalFishPatternLayer
        const bool large = (e.variant & 0xFF) == 1;
        const int pm = assets_.model(large ? "tropical_fish_large#pattern" : "tropical_fish_small#pattern");
        const int pt = T(std::string(large ? "fish/tropical_b_pattern_" : "fish/tropical_a_pattern_") + std::to_string(std::clamp(e.variant2, 0, 5) + 1));
        if (pm >= 0 && pt >= 0) out[n++] = {pm, pt, kWhite, 0};
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
    // additive: BlendFunction.ADDITIVE (energy swirl: creeper charge, wither armour)
    auto pso = [&](const void* vs, size_t vsSize, const void* ps, size_t psSize, bool blend, D3D12_CULL_MODE cull,
                   ComPtr<ID3D12PipelineState>& out, int targets = 1, bool additive = false, bool depthOnly = false) {
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
        } else if (additive) {
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_ONE, rt.DestBlend = D3D12_BLEND_ONE, rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ZERO, rt.DestBlendAlpha = D3D12_BLEND_ONE, rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
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
        if (depthOnly) {  // RenderPipelines.WATER_MASK: depth written, no colour
            for (int t = 0; t < 2; ++t) d.BlendState.RenderTarget[t].RenderTargetWriteMask = 0;
            d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
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
        !pso(SH(EntityModelVS), SH(EntityTranslucentPS), true, noCull, psoAdditive_, 1, true) ||
        !pso(SH(EntityModelVS), SH(EntityTranslucentPS), true, noCull, psoAdditiveMv_, 3, true) ||
        !pso(SH(EntityModelVS), nullptr, 0, false, back, psoWaterMask_, 1, false, true) ||
        !pso(SH(EntityModelVS), nullptr, 0, false, back, psoWaterMaskMv_, 2, false, true) ||
        !pso(SH(EntityFreeVS), SH(EntityTranslucentPS), true, noCull, psoBlob_) ||
        !pso(SH(EntityFreeVS), SH(EntityTranslucentPS), true, noCull, psoBlobMv_, 3) ||
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
    if (!createOutline(r)) LOG("warning: glowing entity outlines unavailable");
    return true;
}

bool EntityRenderer::createOutline(Renderer& r) {
    ID3D12Device* device = r.device();
    // Silhouettes: the entity root signature and vertex shaders, no depth (RenderType outline: seen through walls).
    auto silhouette = [&](const void* vs, size_t vsSize, D3D12_CULL_MODE cull, ComPtr<ID3D12PipelineState>& out) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = rootSig_.Get();
        d.VS = {vs, vsSize};
        d.PS = {g_EntityOutlinePS, sizeof(g_EntityOutlinePS)};
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = cull;
        d.RasterizerState.FrontCounterClockwise = TRUE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        return check(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)), "CreateGraphicsPipelineState");
    };
    if (!silhouette(g_EntityModelVS, sizeof(g_EntityModelVS), D3D12_CULL_MODE_BACK, psoOutline_) ||
        !silhouette(g_EntityFreeVS, sizeof(g_EntityFreeVS), D3D12_CULL_MODE_NONE, psoOutlineFree_))
        return false;
    // The post passes: 8 constants (b0), the input (t0), point and linear clamp samplers.
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 8;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samp[2]{};
    for (int i = 0; i < 2; ++i) {
        samp[i].Filter = i ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp[i].AddressU = samp[i].AddressV = samp[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp[i].MaxLOD = D3D12_FLOAT32_MAX;
        samp[i].ShaderRegister = UINT(i);
        samp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rd{2, params, 2, samp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        !check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&postSig_)), "CreateRootSignature"))
        return false;
    auto post = [&](const void* ps, size_t psSize, bool blend, ComPtr<ID3D12PipelineState>& out) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = postSig_.Get();
        d.VS = {g_OutlineVS, sizeof(g_OutlineVS)};
        d.PS = {ps, psSize};
        auto& rt = d.BlendState.RenderTarget[0];
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (blend) {  // ENTITY_OUTLINE_BLIT: src alpha, 1 - src alpha (alpha: one, 1 - src alpha for the premultiplied UI layer)
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_SRC_ALPHA, rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA, rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE, rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA, rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        return check(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)), "CreateGraphicsPipelineState");
    };
    if (!post(g_SobelPS, sizeof(g_SobelPS), false, psoSobel_) || !post(g_BlurPS, sizeof(g_BlurPS), false, psoBlur_) ||
        !post(g_BlitPS, sizeof(g_BlitPS), true, psoBlit_))
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    return check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&outlineRtvs_)), "CreateDescriptorHeap");
}

void EntityRenderer::drawOutline(const PassContext& ctx) {
    ++outlineFrame_;
    std::erase_if(outlineRetired_, [&](const auto& r) { return outlineFrame_ > r.first + Renderer::kFrames + 1; });
    if ((glowing_.empty() && !outlineExtraActive) || !built_ || !postSig_ || !ctx.colorRtv.ptr || !ctx.width || !ctx.height) return;
    ID3D12Device* device = renderer_->device();
    ID3D12GraphicsCommandList* list = ctx.list;
    const u32 w = ctx.width, h = ctx.height;
    if (w > outlineW_ || h > outlineH_) {  // the targets only grow (the old ones live until the GPU is past them)
        const u32 nw = std::max(w, outlineW_), nh = std::max(h, outlineH_);
        const u32 srv = renderer_->allocDescriptors(2);
        if (srv == UINT32_MAX) return;
        for (int i = 0; i < 2; ++i) {
            if (outlineTex_[i]) outlineRetired_.push_back({outlineFrame_, outlineTex_[i]});
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = nw, td.Height = nh, td.DepthOrArraySize = 1, td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv{DXGI_FORMAT_R8G8B8A8_UNORM, {0, 0, 0, 0}};
            outlineTex_[i].Reset();
            if (!check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
                                                       IID_PPV_ARGS(&outlineTex_[i])),
                       "CreateCommittedResource")) {
                outlineW_ = outlineH_ = 0;
                return;
            }
            outlineState_[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(outlineTex_[i].Get(), &sd, renderer_->cpuDescriptor(srv + u32(i)));
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = outlineRtvs_->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += size_t(i) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            device->CreateRenderTargetView(outlineTex_[i].Get(), nullptr, rtv);
        }
        outlineSrv_ = srv, outlineW_ = nw, outlineH_ = nh;
    }
    auto rtvOf = [&](int i) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = outlineRtvs_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += size_t(i) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        return rtv;
    };
    auto toState = [&](int i, D3D12_RESOURCE_STATES s) {
        if (outlineState_[i] == s) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = outlineTex_[i].Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = outlineState_[i], b.Transition.StateAfter = s;
        list->ResourceBarrier(1, &b);
        outlineState_[i] = s;
    };
    const D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {renderer_->descriptorHeap()};
    list->SetDescriptorHeaps(1, heaps);

    // The silhouettes (LevelRenderer: the entity outline target, cleared to transparent, no depth).
    toState(0, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float clear[4] = {0, 0, 0, 0};
    const D3D12_CPU_DESCRIPTOR_HANDLE t0 = rtvOf(0);
    list->ClearRenderTargetView(t0, clear, 0, nullptr);
    list->OMSetRenderTargets(1, &t0, FALSE, nullptr);
    const D3D12_GPU_VIRTUAL_ADDRESS va = upload_->GetGPUVirtualAddress() + u64(ctx.frame) * kFrameBytes;
    list->SetGraphicsRootSignature(rootSig_.Get());
    list->SetGraphicsRootConstantBufferView(0, va + kOutlineConstOffset);
    list->SetGraphicsRootShaderResourceView(2, quads_->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(3, va + kMatOffset);
    list->SetGraphicsRootShaderResourceView(4, va + kInstOffset);
    list->SetGraphicsRootShaderResourceView(5, va + kRefOffset);
    list->SetGraphicsRootShaderResourceView(6, va + kFreeOffset);
    list->SetGraphicsRootDescriptorTable(7, renderer_->gpuDescriptor(atlasDescriptor_));
    list->SetGraphicsRootDescriptorTable(8, ctx.blockTextures);
    list->SetGraphicsRootShaderResourceView(9, va + kPrevMatOffset);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (int kind = 0; kind < 3; ++kind) {  // model cutout, model translucent, free quads
        list->SetPipelineState((kind == 2 ? psoOutlineFree_ : psoOutline_).Get());
        for (const Glow& g : glowing_) {
            const u32 first = kind == 0 ? g.ref : kind == 1 ? cutoutRefs_ + g.tr : g.free;
            const u32 count = kind == 0 ? g.refN : kind == 1 ? g.trN : g.freeN;
            if (!count) continue;
            const u32 consts[2] = {first, g.color};
            list->SetGraphicsRoot32BitConstants(1, 2, consts, 0);
            list->DrawInstanced(count * 6, 1, 0, 0);
        }
    }
    if (outlineExtraActive && outlineExtra) outlineExtra(ctx);

    // post_effect/entity_outline: entity_sobel, the horizontal and vertical box blur, then the blit over the frame.
    list->SetGraphicsRootSignature(postSig_.Get());
    auto pass = [&](ID3D12PipelineState* pso, int in, int out, float dx, float dy) {
        toState(in, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        D3D12_CPU_DESCRIPTOR_HANDLE target = ctx.colorRtv;
        if (out >= 0) toState(out, D3D12_RESOURCE_STATE_RENDER_TARGET), target = rtvOf(out);
        list->OMSetRenderTargets(1, &target, FALSE, nullptr);
        const float k[8] = {float(w), float(h), float(w) / float(outlineW_), float(h) / float(outlineH_), dx, dy, 0, 0};
        list->SetGraphicsRoot32BitConstants(0, 8, k, 0);
        list->SetGraphicsRootDescriptorTable(1, renderer_->gpuDescriptor(outlineSrv_ + u32(in)));
        list->SetPipelineState(pso);
        list->DrawInstanced(3, 1, 0, 0);
    };
    pass(psoSobel_.Get(), 0, 1, 0, 0);
    pass(psoBlur_.Get(), 1, 0, 1, 0);
    pass(psoBlur_.Get(), 0, 1, 0, 1);
    pass(psoBlit_.Get(), 1, -1, 0, 0);
    list->OMSetRenderTargets(1, &ctx.colorRtv, FALSE, ctx.dsv.ptr ? &ctx.dsv : nullptr);
}

// ---------------------------------------------------------------------------------------------------------
// Per-frame building.

int EntityRenderer::trimTexture(const ui::ItemStack& stack, const std::string& asset, const char* layerType) {
    const ui::Items* items = items_ ? items_->items() : nullptr;
    const ui::Resources* res = items_ ? items_->resources() : nullptr;
    if (!items || !res) return -1;
    const std::string* data = ui::findComponent(*items, stack, "trim");
    if (!data) return -1;
    const std::string key = *data + '|' + asset + '|' + layerType;
    if (auto it = trimTextures_.find(key); it != trimTextures_.end()) return it->second;
    int& tex = trimTextures_[key];
    tex = -1;
    ui::Image img;
    std::string id;
    if (!ui::armorTrimImage(*items, *res, stack, asset, layerType, img, id) || img.w <= 0 || img.w > 64 || img.h > 64) return -1;
    tex = assets_.allocSlot();
    if (tex >= 0) assets_.writeSlot(tex, img.rgba.data(), img.w, img.h);
    return tex;
}

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
        // EquipmentLayerRenderer: a trim (not on the baby layer) is drawn over the pieces and takes the glint
        const int trim = type != 2 && e.equipStack[slot] ? trimTexture(*e.equipStack[slot], eq.asset, type == 1 ? "humanoid_leggings" : "humanoid") : -1;
        bool glint = trim < 0 && ((e.equipFoil >> slot) & 1);  // the first drawn layer carries the glint
        for (const EquipmentLayer& l : items_->itemLayers(item, type)) {
            if (l.tex < 0 || n >= max) continue;
            u32 color = kWhite;
            if (l.dyeable) {
                const i32 dye = e.equipDye[slot] >= 0 ? e.equipDye[slot] : l.undyed;
                if (dye < 0) continue;  // getColorForLayer: 0 = not drawn
                color = abgr(u32(dye));
            }
            out[n++] = {model, l.tex, color, u16(Armor | (glint ? Glint : 0))};
            glint = false;
        }
        if (trim >= 0 && n < max) out[n++] = {model, trim, kWhite, u16(Armor | (((e.equipFoil >> slot) & 1) ? Glint : 0))};
    }
    const int chest = e.equipment[4];
    const int elytra = smallModel ? elytraBabyModel_ : elytraModel_;
    if (chest >= 0 && elytra >= 0 &&
        (d.anim == Anim::Humanoid || d.anim == Anim::Zombie || d.anim == Anim::Skeleton || d.anim == Anim::ArmorStand)) {
        // WingsLayer.getPlayerElytraTexture: a player's cape (when shown) instead of the elytra's own texture
        const int capeTex = d.special != SpPlayer ? -1 : e.elytraTexture >= 0 ? e.elytraTexture : e.cape >= 0 && (e.skinParts & 1) ? e.cape : -1;
        for (const EquipmentLayer& l : items_->itemLayers(chest, 3))
            if (l.tex >= 0 && n < max) out[n++] = {elytra, capeTex >= 0 ? capeTex : l.tex, kWhite, u16(Elytra | ((e.equipFoil & 16) ? Glint : 0))};
    }
    // SimpleEquipmentLayer / LlamaDecorLayer: body armour, carpets and saddles (posed like the base model).
    if (e.info && d.anim != Anim::Humanoid && d.anim != Anim::Zombie && d.anim != Anim::Skeleton) {
        const std::string& name = e.info->name;
        for (int slot : {6, 7}) {
            const char *modelName, *layerType;
            if (!ItemModels::mountLayer(name, slot, e.baby, modelName, layerType)) continue;
            const int item = e.equipment[slot];
            std::string asset = item >= 0 ? items_->equippable(item).asset : std::string();
            if (item >= 0 && items_->equippable(item).slot != slot) asset.clear();
            if (slot == 6 && name.ends_with("llama") && e.baby) asset.clear();  // carpets: adults only
            if (asset.empty() && slot == 6 && name == "minecraft:trader_llama") asset = e.baby ? "trader_llama_baby" : "trader_llama";
            if (asset.empty()) continue;
            const int model = assets_.model(modelName);
            if (model < 0) continue;
            bool glint = (e.equipFoil >> slot) & 1;
            for (const EquipmentLayer& l : items_->equipmentLayers(asset, layerType)) {
                if (l.tex < 0 || n >= max) continue;
                u32 color = kWhite;
                if (l.dyeable) {
                    const i32 dye = e.equipDye[slot] >= 0 ? e.equipDye[slot] : l.undyed;
                    if (dye < 0) continue;
                    color = abgr(u32(dye));
                }
                out[n++] = {model, l.tex, color, u16(Armor | (glint ? Glint : 0))};
                glint = false;
            }
            const ui::ItemStack* stack = e.equipStack[slot].get();
            const int maxDamage = stack && items_->items() ? items_->items()->maxDamage(*stack) : -1;
            if (slot == 6 && name == "minecraft:wolf" && maxDamage > 0 && n < max) {
                // WolfArmorLayer.maybeRenderCracks: Crackiness.WOLF_ARMOR by the armour's remaining durability
                const float f = float(maxDamage - stack->damage) / float(maxDamage);
                const char* crack = f < 0.32f ? "entity/wolf/wolf_armor_crackiness_high" : f < 0.69f ? "entity/wolf/wolf_armor_crackiness_medium"
                                    : f < 0.95f ? "entity/wolf/wolf_armor_crackiness_low" : nullptr;
                if (const int t = crack ? assets_.texture(crack) : -1; t >= 0) out[n++] = {model, t, kWhite, u16(Armor | Translucent | NoOverlay)};
            }
        }
    }
    return n;
}

namespace {
// LivingEntityRenderer.isShaking and the overrides: fully frozen, converting zombies / skeletons / zombie villagers,
// suffocating striders.
bool isShaking(const Entity& e, bool piglinsZombify) {
    if (fullyFrozen(e)) return true;
    if (!e.info) return false;
    const std::string& n = e.info->name;
    if (n == "minecraft:piglin" || n == "minecraft:piglin_brute" || n == "minecraft:hoglin")  // isConverting
        return piglinsZombify && !(e.mobFlags & 1) && dataInt(e, "DATA_IMMUNE_TO_ZOMBIFICATION") == 0;
    if (n == "minecraft:zombie" || n == "minecraft:husk" || n == "minecraft:drowned") return dataInt(e, "DATA_DROWNED_CONVERSION_ID") != 0;
    if (n == "minecraft:skeleton") return dataInt(e, "DATA_STRAY_CONVERSION_ID") != 0;
    if (n == "minecraft:zombie_villager") return dataInt(e, "DATA_CONVERTING_ID") != 0;
    if (n == "minecraft:strider") return dataInt(e, "DATA_SUFFOCATING") != 0;
    return false;
}
}  // namespace

// The species renderers' setupRotations after LivingEntityRenderer's (before the model flip).
void EntityRenderer::speciesRotations(const Entity& e, const TypeDef& d, float pt, float pitch, Mat34& rootOut) const {
    M34& root = *reinterpret_cast<M34*>(&rootOut);
    const float age = float(e.age) + pt;
    auto lerpA = [&](int i) { return e.sp.ao[i] + (e.sp.a[i] - e.sp.ao[i]) * pt; };
    switch (d.anim) {
        case Anim::Feline: {  // CatRenderer: lying down
            const float lie = d.special == SpCat ? lerpA(kLie) : 0.0f;
            if (lie > 0) {
                root = mul(root, translate(0.4f * lie, 0.15f * lie, 0.1f * lie));
                root = mul(root, rotZ(lerpAngle(lie, 0.0f, 90.0f) * kDeg));
                // isLyingOnTopOfSleepingPlayer: a sleeping player within 2 blocks
                for (const auto& [id, other] : world_.entities())
                    if (other.category == Category::Player && other.pose == 2 && std::abs(other.pos[0] - e.pos[0]) < 3 &&
                        std::abs(other.pos[1] - e.pos[1]) < 3 && std::abs(other.pos[2] - e.pos[2]) < 3) {
                        root = mul(root, translate(0.15f * lie, 0, 0));
                        break;
                    }
            }
            break;
        }
        case Anim::Fox: {  // FoxRenderer: pouncing / faceplanted foxes tilt with their pitch
            const i64 flags = dataInt(e, "DATA_FLAGS_ID");
            if (flags & (16 | 64)) root = mul(root, rotX(-pitch * kDeg));
            break;
        }
        case Anim::Panda: {  // PandaRenderer: rolling, sitting, lying on the back
            const float rollTime = e.sp.t[0] > 0 ? float(e.sp.t[0]) + pt : 0.0f;
            if (rollTime > 0) {
                const float frac = rollTime - std::floor(rollTime);
                const int pos = int(std::floor(rollTime)), next = pos + 1;
                const float y = e.baby ? 0.3f : 0.8f;
                auto angle = [&](float a, float b, float threshold) { return float(next) < threshold ? a + (b - a) * frac : a; };
                if (pos < 8) {
                    const float ang = angle(90.0f * float(pos) / 7.0f, 90.0f * float(next) / 7.0f, 8.0f);
                    root = mul(mul(root, translate(0, (y + 0.2f) * (ang / 90.0f), 0)), rotX(-ang * kDeg));
                } else if (pos < 16) {
                    const float ang = angle(90.0f + 90.0f * (float(pos) - 8.0f) / 7.0f, 90.0f + 90.0f * (float(next) - 8.0f) / 7.0f, 16.0f);
                    root = mul(mul(root, translate(0, y + 0.2f + (y - 0.2f) * (ang - 90.0f) / 90.0f, 0)), rotX(-ang * kDeg));
                } else if (pos < 24) {
                    const float ang = angle(180.0f + 90.0f * (float(pos) - 16.0f) / 7.0f, 180.0f + 90.0f * (float(next) - 16.0f) / 7.0f, 24.0f);
                    root = mul(mul(root, translate(0, y + y * (270.0f - ang) / 90.0f, 0)), rotX(-ang * kDeg));
                } else if (pos < 32) {
                    const float ang = angle(270.0f + 90.0f * (float(pos) - 24.0f) / 7.0f, 270.0f + 90.0f * (float(next) - 24.0f) / 7.0f, 32.0f);
                    root = mul(mul(root, translate(0, y * ((360.0f - ang) / 90.0f), 0)), rotX(-ang * kDeg));
                }
            }
            const float sit = lerpA(kSit);
            if (sit > 0) {
                root = mul(root, translate(0, 0.8f * sit, 0));
                root = mul(root, rotX((pitch + sit * 90.0f) * kDeg));
                root = mul(root, translate(0, -1.0f * sit, 0));
                if (e.variant == 2 && speciesThundering()) {  // scared
                    root = mul(root, rotY(float(std::cos(age * 1.25f) * kPi * 0.05f) * kDeg));
                    if (e.baby) root = mul(root, translate(0, 0.8f, 0.55f));
                }
            }
            const float back = lerpA(kOnBack);
            if (back > 0) {
                root = mul(root, translate(0, (e.baby ? 0.5f : 1.3f) * back, 0));
                root = mul(root, rotX((pitch + back * 180.0f) * kDeg));
            }
            break;
        }
        case Anim::Cod:
        case Anim::Salmon:
        case Anim::TropicalFish: {  // Cod / Salmon / TropicalFishRenderer: swimming wiggle, flopping on land
            const bool salmon = d.anim == Anim::Salmon;
            const float amp = salmon && !e.inWater ? 1.3f : 1.0f, ang = salmon && !e.inWater ? 1.7f : 1.0f;
            root = mul(root, rotY(amp * 4.3f * std::sin(ang * 0.6f * age) * kDeg));
            if (!e.inWater) {
                root = mul(root, d.anim == Anim::Cod ? translate(0.1f, 0.1f, -0.1f) : translate(0.2f, 0.1f, 0));
                root = mul(root, rotZ(kPi / 2));
            }
            break;
        }
        case Anim::Drowned: {  // DrownedRenderer: swimming tilt about the middle of the body
            const float swim = e.swimAmountO + (e.swimAmount - e.swimAmountO) * pt;
            if (swim > 0) {
                const float h = (e.info ? e.info->height : 1.95f) * (e.baby ? 0.5f : 1.0f) / 2.0f;
                root = mul(mul(mul(root, translate(0, h, 0)), rotX(swim * (-10.0f - pitch) * kDeg)), translate(0, -h, 0));
            }
            break;
        }
        case Anim::IronGolem: {  // IronGolemRenderer: the body sways while walking
            const float speed = std::min(e.walkSpeedOld + (e.walkSpeed - e.walkSpeedOld) * pt, 1.0f);
            if (speed >= 0.01f) {
                const float wp = (e.walkPos - e.walkSpeed * (1.0f - pt)) * e.walkScale + 6.0f;
                root = mul(root, rotZ(6.5f * ((std::abs(std::fmod(wp, 13.0f) - 6.5f) - 3.25f) / 3.25f) * kDeg));
            }
            break;
        }
        case Anim::Phantom: root = mul(root, rotX(pitch * kDeg)); break;  // PhantomRenderer
        case Anim::ArmorStand:
            break;
        default: break;
    }
}

int EntityRenderer::tex(const char* path) const {
    thread_local std::unordered_map<const char*, int> cache;
    auto it = cache.find(path);
    if (it == cache.end()) it = cache.emplace(path, assets_.texture(std::string("entity/") + path)).first;
    return it->second;
}

int EntityRenderer::model(const char* layer) const {
    thread_local std::unordered_map<const char*, int> cache;
    auto it = cache.find(layer);
    if (it == cache.end()) it = cache.emplace(layer, assets_.model(layer)).first;
    return it->second;
}

// Per-frame textures and layers of the species renderers: getTextureLocation (charging, angry, sleeping, cold,
// invulnerable...), eyes / emissive / energy swirl layers, iron golem cracks, model tints.
int EntityRenderer::speciesLayers(const Entity& e, const TypeDef& d, float pt, Layer* L, int n, int max) const {
    const float age = float(e.age) + pt;
    auto add = [&](int m, int t, u32 color, u16 flags) -> Layer* {
        if (m < 0 || t < 0 || n >= max) return nullptr;
        L[n] = {m, t, color, flags};
        return &L[n++];
    };
    auto setTex = [&](int t) {
        if (t >= 0) L[0].tex = t;
    };
    auto white = [](float a) { return (u32(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f) << 24) | 0xFFFFFFu; };
    const bool baby = e.baby;
    switch (d.anim) {
        case Anim::Bee: {  // BeeRenderer.getTextureLocation
            const bool nectar = (dataInt(e, "DATA_FLAGS_ID") & 8) != 0;
            if (e.angry) setTex(nectar ? tex(baby ? "bee/bee_angry_nectar_baby" : "bee/bee_angry_nectar") : tex(baby ? "bee/bee_angry_baby" : "bee/bee_angry"));
            else if (nectar) setTex(tex(baby ? "bee/bee_nectar_baby" : "bee/bee_nectar"));
            break;
        }
        case Anim::Fox:  // FoxRenderer: the sleeping texture
            if (dataInt(e, "DATA_FLAGS_ID") & 32)
                setTex(e.variant == 1 ? tex(baby ? "fox/fox_snow_sleep_baby" : "fox/fox_snow_sleep")
                                      : tex(baby ? "fox/fox_sleep_baby" : "fox/fox_sleep"));
            break;
        case Anim::Ghast:  // GhastRenderer
            if (dataInt(e, "DATA_IS_CHARGING")) setTex(tex("ghast/ghast_shooting"));
            break;
        case Anim::Vex:  // VexRenderer
            if (dataInt(e, "DATA_FLAGS_ID") & 1) setTex(tex("illager/vex_charging"));
            break;
        case Anim::Strider:  // StriderRenderer: cold while out of lava
            if (dataInt(e, "DATA_SUFFOCATING")) setTex(tex(baby ? "strider/strider_cold_baby" : "strider/strider_cold"));
            break;
        case Anim::Rabbit:  // RabbitRenderer: "Toast"
            if (e.name == "Toast") setTex(tex(baby ? "rabbit/rabbit_toast_baby" : "rabbit/rabbit_toast"));
            break;
        case Anim::WitherSkull:  // WitherSkullRenderer: dangerous (blue) skulls
            if (dataInt(e, "DATA_DANGEROUS")) setTex(tex("wither/wither_invulnerable"));
            break;
        case Anim::Arrow:  // TippableArrowRenderer
            if (e.info && e.info->name == "minecraft:arrow" && dataInt(e, "ID_EFFECT_COLOR", -1) > 0) setTex(tex("projectiles/arrow_tipped"));
            break;
        case Anim::Wither: {  // WitherBossRenderer: invulnerable texture blinking; WitherArmorLayer below half health
            const int inv = int(std::floor(std::max(0.0f, float(dataInt(e, "DATA_ID_INV")) - pt)));
            if (inv > 0 && (inv > 80 || inv / 5 % 2 != 1)) setTex(tex("wither/wither_invulnerable"));
            if (e.health <= 150.0f) {
                if (Layer* l = add(model("wither#armor"), tex("wither/wither_armor"), 0xFF808080u, Additive | Scroll | Emissive | NoOverlay))
                    l->scroll[0] = std::cos(age * 0.02f) * 3.0f, l->scroll[1] = age * 0.01f;
            }
            break;
        }
        case Anim::Creeper:  // CreeperPowerLayer
            if (e.powered)
                if (Layer* l = add(model("creeper#armor"), tex("creeper/creeper_armor"), 0xFF808080u, Additive | Scroll | Emissive | NoOverlay))
                    l->scroll[0] = age * 0.01f, l->scroll[1] = age * 0.01f;
            break;
        case Anim::Breeze:  // BreezeWindLayer, BreezeEyesLayer
            if (Layer* l = add(model("breeze#wind"), tex("breeze/breeze_wind"), kWhite, Translucent | Scroll | NoOverlay))
                l->scroll[0] = age * 0.02f;
            add(model("breeze#eyes"), tex("breeze/breeze_eyes"), kWhite, Emissive | Translucent | NoOverlay);
            break;
        case Anim::Creaking: {  // the eyes (LivingEntityEmissiveLayer, eyes render type): active, or blinking while torn down
            const bool tearing = dataInt(e, "IS_TEARING_DOWN") != 0;
            if (tearing ? e.sp.t[4] != 0 : dataInt(e, "IS_ACTIVE") != 0)
                add(model("creaking#eyes"), tex("creaking/creaking_eyes"), kWhite, Emissive | Translucent | NoOverlay);
            break;
        }
        case Anim::CopperGolem: {  // CopperGolemRenderer: oxidation texture and its eyes
            static const char* kTex[4] = {"copper_golem/copper_golem", "copper_golem/copper_golem_exposed",
                                          "copper_golem/copper_golem_weathered", "copper_golem/copper_golem_oxidized"};
            static const char* kEyes[4] = {"copper_golem/copper_golem_eyes", "copper_golem/copper_golem_eyes_exposed",
                                           "copper_golem/copper_golem_eyes_weathered", "copper_golem/copper_golem_eyes_oxidized"};
            const int w = std::clamp(int(dataInt(e, "DATA_WEATHER_STATE")), 0, 3);
            setTex(tex(kTex[w]));
            add(L[0].model, tex(kEyes[w]), kWhite, Emissive | Translucent | NoOverlay);
            break;
        }
        case Anim::Warden: {  // WardenRenderer's LivingEntityEmissiveLayers (entity translucent emissive, with alpha)
            auto lerpA = [&](int i) { return (e.sp.ao[i] + (e.sp.a[i] - e.sp.ao[i]) * pt) / 10.0f; };
            add(model("warden#bioluminescent"), tex("warden/warden_bioluminescent_layer"), kWhite, Emissive | Translucent);
            const float spots = std::cos(age * 0.045f) * 0.25f;
            if (spots > 1e-5f) add(model("warden#pulsating_spots"), tex("warden/warden_pulsating_spots_1"), white(spots), Emissive | Translucent);
            if (-spots > 1e-5f) add(model("warden#pulsating_spots"), tex("warden/warden_pulsating_spots_2"), white(-spots), Emissive | Translucent);
            if (const float t = lerpA(kTendril); t > 1e-5f) add(model("warden#tendrils"), tex("warden/warden"), white(t), Emissive | Translucent);
            if (const float h = lerpA(kHeart); h > 1e-5f) add(model("warden#heart"), tex("warden/warden_heart"), white(h), Emissive | Translucent);
            break;
        }
        case Anim::IronGolem: {  // IronGolemCrackinessLayer (Crackiness.GOLEM by health, 100 max)
            const float f = e.health / 100.0f;
            const char* crack = f < 0.25f ? "iron_golem/iron_golem_crackiness_high" : f < 0.5f ? "iron_golem/iron_golem_crackiness_medium"
                                : f < 0.75f ? "iron_golem/iron_golem_crackiness_low" : nullptr;
            if (crack) add(L[0].model, tex(crack), kWhite, 0);
            break;
        }
        case Anim::Wolf: {  // WolfRenderer.getModelTint: darker while wet
            if (e.sp.t[1]) {
                const float shake = e.sp.ao[kShake] + (e.sp.a[kShake] - e.sp.ao[kShake]) * pt;
                const float shade = std::min(0.75f + shake / 2.0f * 0.25f, 1.0f);
                const u32 g = u32(shade * 255.0f + 0.5f);
                L[0].color = 0xFF000000u | g << 16 | g << 8 | g;
            }
            break;
        }
        case Anim::Dragon:  // EnderDragonRenderer: the eyes; dying, the body dissolves (dragon_exploding as the mask)
            if (e.sp.t[1] > 0) {
                const int mask = tex("enderdragon/dragon_exploding");
                if (mask >= 0 && L[0].tex >= 0) {
                    const TexRect& a = assets_.rect(L[0].tex), &b = assets_.rect(mask);
                    const float alpha = std::clamp(1.0f - (float(e.sp.t[1]) + pt) / 200.0f, 0.0f, 1.0f);
                    L[0].color = u32(alpha * 255.0f) << 24 | 0xFFFFFF;
                    L[0].flags |= Dissolve | NoOverlay;
                    L[0].scroll[0] = b.u0 - a.u0, L[0].scroll[1] = b.v0 - a.v0;
                }
            }
            add(L[0].model, tex("enderdragon/dragon_eyes"), kWhite, Emissive | Translucent | NoOverlay);
            break;
        default: break;
    }
    return n;
}

// Block models worn or carried by mobs: MushroomCowMushroomLayer, CarriedBlockLayer, IronGolemFlowerLayer,
// SnowGolemHeadLayer, BlockDecorationLayer (copper golem antenna). The posed base model's matrices are in mats_.
void EntityRenderer::blockDecorations(const Entity& e, const TypeDef& d, const Model& m, float pt, const Mat34& rootIn,
                                      u32 light, u32 overlay) {
    const M34& root = *reinterpret_cast<const M34*>(&rootIn);
    const M34* pm = reinterpret_cast<const M34*>(mats_.data());
    BlockRegistry& reg = world_.registry();
    auto blockId = [&](const char* name) -> u16 {
        thread_local std::unordered_map<const char*, u16> cache;
        auto it = cache.find(name);
        if (it == cache.end()) it = cache.emplace(name, reg.idFor(world_.reports().complete(name))).first;
        return it->second;
    };
    auto emit = [&](u16 block, const M34& mm, u32 ov) {
        if (block) emitBlock(block, *reinterpret_cast<const Mat34*>(&mm), light, ov);
    };
    auto part = [&](std::string_view name, M34& out) {
        const int i = m.part(name);
        if (i < 0) return false;
        out = pm[i];
        return true;
    };
    switch (d.anim) {
        case Anim::Quadruped:
            if (d.special == SpMooshroom && !e.baby) {  // three mushrooms: two on the back, one on the head
                const u16 b = blockId(e.variant == 1 ? "minecraft:brown_mushroom" : "minecraft:red_mushroom");
                auto finish = [&](M34 x) { return mul(mul(x, scale(-1, -1, 1)), translate(-0.5f, -0.5f, -0.5f)); };
                emit(b, finish(mul(mul(root, translate(0.2f, -0.35f, 0.5f)), rotY(-48.0f * kDeg))), overlay);
                emit(b, finish(mul(mul(mul(mul(root, translate(0.2f, -0.35f, 0.5f)), rotY(42.0f * kDeg)), translate(0.1f, 0, -0.6f)),
                                   rotY(-48.0f * kDeg))), overlay);
                M34 head;
                if (part("head", head)) emit(b, finish(mul(mul(head, translate(0, -0.7f, -0.2f)), rotY(-78.0f * kDeg))), overlay);
            }
            break;
        case Anim::Enderman: {  // CarriedBlockLayer
            const int state = int(dataInt(e, "DATA_CARRY_STATE"));
            if (state <= 0) break;
            M34 x = mul(mul(mul(root, translate(0, 0.6875f, -0.75f)), rotX(20.0f * kDeg)), rotY(45.0f * kDeg));
            x = mul(mul(mul(x, translate(0.25f, 0.1875f, 0.25f)), scale(-0.5f, -0.5f, 0.5f)), rotY(kPi / 2));
            emit(world_.localBlock(state), x, 0);
            break;
        }
        case Anim::IronGolem: {  // IronGolemFlowerLayer (while offering a flower)
            M34 arm;
            if (e.sp.t[1] <= 0 || !part("right_arm", arm)) break;
            M34 x = mul(mul(arm, translate(-1.1875f, 1.0625f, -0.9375f)), translate(0.5f, 0.5f, 0.5f));
            x = mul(mul(mul(x, scale(0.5f, 0.5f, 0.5f)), rotX(-kPi / 2)), translate(-0.5f, -0.5f, -0.5f));
            emit(blockId("minecraft:poppy"), x, 0);
            break;
        }
        case Anim::SnowGolem: {  // SnowGolemHeadLayer (SnowGolem.hasPumpkin)
            M34 head;
            if (!(dataInt(e, "DATA_PUMPKIN_ID", 16) & 16) || !part("head", head)) break;
            M34 x = mul(mul(mul(head, translate(0, -0.34375f, 0)), rotY(kPi)), scale(0.625f, -0.625f, -0.625f));
            emit(blockId("minecraft:carved_pumpkin"), mul(x, translate(-0.5f, -0.5f, -0.5f)), overlay);
            break;
        }
        case Anim::CopperGolem: {  // BlockDecorationLayer: a block item on the antenna (the saddle slot)
            const int item = e.equipment[7];
            M34 head;
            if (item < 0 || !part("head", head)) break;
            if (itemBlocks_.size() < assets_.itemNames().size()) itemBlocks_.assign(assets_.itemNames().size(), -1);
            i32& cached = itemBlocks_[size_t(item)];
            if (cached < 0) {
                const std::string key = "minecraft:" + assets_.itemNames()[size_t(item)];
                cached = world_.reports().hasBlock(key) ? reg.idFor(world_.reports().complete(key)) : 0;
            }
            // applyBlockOnAntennaTransform, then translation(-0.5, 0, -0.5).rotateAround(Z 180, 0.5, 0.5, 0.5)
            M34 x = mul(mul(head, translate(0, -1.75f, 0)), translate(-0.5f, 0, -0.5f));
            x = mul(mul(mul(x, translate(0.5f, 0.5f, 0.5f)), rotZ(kPi)), translate(-0.5f, -0.5f, -0.5f));
            emit(u16(cached), x, 0);
            break;
        }
        default: break;
    }
}

// Mob arm poses: HumanoidMobRenderer.getArmPose (spears; the skeletons' bow, the drowned's trident), PiglinArmPose,
// IllagerArmPose; the guardian's eye target (GuardianRenderer.getEntityToLookAt).
void EntityRenderer::mobPoses(const Entity& e, const TypeDef& d, const double cam[3], AnimState& s) const {
    const auto& names = assets_.itemNames();
    auto item = [&](int slot) -> std::string_view {
        const int id = e.equipment[slot];
        return id >= 0 && size_t(id) < names.size() ? std::string_view(names[size_t(id)]) : std::string_view();
    };
    const std::string_view main = item(0), off = item(1);
    const bool right = rightHanded(e), aggressive = (e.mobFlags & 4) != 0;
    auto melee = [](std::string_view n) {
        return n.ends_with("_sword") || n.ends_with("_axe") || n.ends_with("_spear") || n == "mace" || n == "trident";
    };
    switch (d.anim) {
        case Anim::Zombie:
        case Anim::Skeleton:
        case Anim::Drowned:
        case Anim::Piglin:
        case Anim::ZombifiedPiglin:
        case Anim::Enderman: {
            ArmPose mainPose = main.ends_with("_spear") ? ArmPose::Spear : ArmPose::Empty;
            const ArmPose offPose = off.ends_with("_spear") ? ArmPose::Spear : ArmPose::Empty;
            if (d.anim == Anim::Skeleton && aggressive && main == "bow") mainPose = ArmPose::BowAndArrow;
            if (d.anim == Anim::Drowned && aggressive && main == "trident") mainPose = ArmPose::ThrowTrident;
            s.havePoses = true;
            s.rightPose = right ? mainPose : offPose;
            s.leftPose = right ? offPose : mainPose;
            s.usingItem = (e.livingFlags & 1) != 0;
            s.useRight = ((e.livingFlags & 2) != 0) != right;
            s.useTicks = float(e.useTicks) + (s.usingItem ? s.pt : 0.0f);
            break;
        }
        default: break;
    }
    const std::string& n = e.info ? e.info->name : std::string();
    if (d.anim == Anim::Piglin) {  // Piglin.getArmPose / PiglinBrute.getArmPose
        const bool meleeAttack = aggressive && melee(main);
        if (n == "minecraft:piglin_brute") s.mobPose = meleeAttack ? 1 : 0;
        else if (dataInt(e, "DATA_IS_DANCING")) s.mobPose = 5;
        else if (off.find("gold") != std::string_view::npos || off == "bell" || off == "clock" ||
                 off == "glistering_melon_slice" || off == "light_weighted_pressure_plate")
            s.mobPose = 4;  // PiglinAi.isLovedItem
        else if (meleeAttack) s.mobPose = 1;
        else if (dataInt(e, "DATA_IS_CHARGING_CROSSBOW")) s.mobPose = 3;
        else s.mobPose = (main == "crossbow" || off == "crossbow") && aggressive ? 2 : 0;  // a charged crossbow
        s.useTicks = float(e.useTicks) + s.pt;
    } else if (d.anim == Anim::Illager) {  // Pillager / Vindicator / SpellcasterIllager / Illusioner.getArmPose
        const bool casting = dataInt(e, "DATA_SPELL_CASTING_ID") > 0, celebrating = dataInt(e, "IS_CELEBRATING") != 0;
        if (n == "minecraft:pillager")
            s.mobPose = dataInt(e, "IS_CHARGING_CROSSBOW") ? 5 : (main == "crossbow" || off == "crossbow") ? 4 : aggressive ? 1 : 7;
        else if (n == "minecraft:vindicator") s.mobPose = aggressive ? 1 : celebrating ? 6 : 0;
        else if (n == "minecraft:illusioner") s.mobPose = casting ? 2 : aggressive ? 3 : 0;
        else s.mobPose = casting ? 2 : celebrating ? 6 : 0;
        s.useTicks = float(e.useTicks) + s.pt;
    } else if (d.anim == Anim::Guardian) {
        const float pt = s.pt;
        for (int a = 0; a < 3; ++a) s.eyePos[a] = float(e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt);
        s.eyePos[1] += e.info ? e.info->eye : 0.4f;
        const float yaw = lerpAngle(pt, e.prevYaw, e.yaw) * kDeg, pitch = (e.prevPitch + (e.pitch - e.prevPitch) * pt) * kDeg;
        s.lookDir[0] = -std::sin(yaw) * std::cos(pitch), s.lookDir[1] = -std::sin(pitch), s.lookDir[2] = std::cos(yaw) * std::cos(pitch);
        s.lookValid = true;
        const int target = int(dataInt(e, "DATA_ID_ATTACK_TARGET"));
        auto it = target ? world_.entities().find(target) : world_.entities().end();
        if (it != world_.entities().end()) {
            const Entity& t = it->second;
            for (int a = 0; a < 3; ++a) s.lookAt[a] = float(t.prevPos[a] + (t.pos[a] - t.prevPos[a]) * pt);
            s.lookAt[1] += t.info ? t.info->eye : 1.62f;
        } else {
            for (int a = 0; a < 3; ++a) s.lookAt[a] = float(cam[a]);  // the camera entity's eyes
        }
    }
}

// AbstractBoat.isUnderWater (its eyes in water).
bool EntityRenderer::boatUnderWater(const Entity& e) const {
    const World& world = world_.world();
    const int x = int(std::floor(e.pos[0])), y = int(std::floor(e.pos[1] + e.info->eye)), z = int(std::floor(e.pos[2]));
    const u16 at = y >= world.minSectionY() * 16 && y < (world.maxSectionY() + 1) * 16 ? world.block(x, y, z) : 0;
    const BlockInfo& b = world_.registry().infos()[at];
    return b.waterlogged || (b.fluid && world_.registry().name(at).find("water") != std::string::npos);
}

bool EntityRenderer::modelEntity(const Entity& e, const TypeDef& d, float pt, const double cam[3], u32 light) {
    Layer layers[16];
    int layerCount = 0;
    if (d.kind == Kind::Mob) layerCount = resolve(e, d, layers);
    if (!layerCount) return false;
    layerCount = speciesLayers(e, d, pt, layers, layerCount, 16);
    if (d.waterPatch && waterPatchModel_ >= 0 && layerCount < 16 && !pack_ && !shadowOnly_ && !boatUnderWater(e))
        layers[layerCount++] = {waterPatchModel_, layers[0].tex, kWhite, u16(NoOverlay)};
    if (e.category == Category::Living || e.category == Category::Player)
        layerCount += equipmentLayers(e, d, layers + layerCount, 16 - layerCount);
    // CapeLayer: the profile's cape unless invisible, hidden in the skin options or under an elytra (WINGS layer).
    bool capeOverArmor = false;
    if (d.special == SpPlayer && e.cape >= 0 && capeModel_ >= 0 && (e.skinParts & 1) && !(e.sharedFlags & 0x20) && layerCount < 16) {
        const int chest = e.equipment[4];
        const bool wings = chest >= 0 && items_ && !items_->itemLayers(chest, 3).empty();
        capeOverArmor = chest >= 0 && items_ && !items_->itemLayers(chest, 0).empty();
        if (!wings) layers[layerCount++] = {capeModel_, e.cape, kWhite, u16(Cape | NoOverlay)};
    }
    // AvatarRenderer's other player layers: deadmau5's ears, stuck arrows and bee stingers, parrots on the shoulders.
    if (d.special == SpPlayer && !(e.sharedFlags & 0x20)) {
        if (e.name == "deadmau5" && earsModel_ >= 0 && layerCount < 16 && layerCount > 0)  // Deadmau5EarsLayer: the skin
            layers[layerCount++] = {earsModel_, layers[0].tex, kWhite, u16(Armor)};
        for (int k = 0; k < e.arrowCount && arrowModel_ >= 0 && layerCount < 16; ++k) {
            layers[layerCount] = {arrowModel_, arrowTex_, kWhite, u16(Stuck | NoOverlay)};
            layers[layerCount++].scroll[0] = float(k);
        }
        for (int k = 0; k < e.stingerCount && stingerModel_ >= 0 && layerCount < 16; ++k) {
            layers[layerCount] = {stingerModel_, stingerTex_, kWhite, u16(Stuck | NoOverlay)};
            layers[layerCount].scroll[0] = float(k), layers[layerCount++].scroll[1] = 1;
        }
        static const char* kParrots[5] = {"parrot/parrot_red_blue", "parrot/parrot_blue", "parrot/parrot_green", "parrot/parrot_yellow_blue",
                                          "parrot/parrot_grey"};
        for (int side = 0; side < 2 && parrotModel_ >= 0; ++side) {
            const int v = e.shoulderParrot[side];
            if (v < 0 || v > 4 || layerCount >= 16) continue;
            const int t = assets_.texture(std::string("entity/") + kParrots[v]);
            if (t < 0) continue;
            layers[layerCount] = {parrotModel_, t, kWhite, u16(Shoulder | NoOverlay)};
            layers[layerCount++].scroll[0] = side == 0 ? 0.4f : -0.4f;
        }
    }
    // SpinAttackEffectLayer: the riptide swirl around a spinning player.
    if (d.special == SpPlayer && (e.livingFlags & 4) && spinModel_ >= 0 && spinTex_ >= 0 && layerCount < 16)
        layers[layerCount++] = {spinModel_, spinTex_, kWhite, SpinFx};
    double p[3];
    for (int a = 0; a < 3; ++a) p[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt - cam[a];
    const float yaw = lerpAngle(pt, e.prevYaw, e.yaw), pitch = e.prevPitch + (e.pitch - e.prevPitch) * pt;
    const bool living = e.category == Category::Living || e.category == Category::Player;

    // Root transform (LivingEntityRenderer.submit / setupRotations, or the vehicle / projectile renderers).
    M34 root = translate(float(p[0]), float(p[1]), float(p[2]));
    if (d.anim == Anim::Enderman && dataInt(e, "DATA_CREEPY") != 0) {  // EndermanRenderer.getRenderOffset: the creepy jitter
        static std::mt19937 jitter(0x5EED);
        std::normal_distribution<float> gauss(0.0f, 1.0f);
        root = mul(root, translate(gauss(jitter) * 0.02f, 0, gauss(jitter) * 0.02f));
    }
    AnimState s;
    s.pt = pt, s.entity = &e, s.assets = &assets_;
    s.age = float(e.age) + pt;
    u32 overlay = 0;
    const bool ridden = std::find(ridden_.begin(), ridden_.end(), e.id) != ridden_.end();
    s.riding = ridden;  // non-humanoid models: "is ridden" (camel reins, strider body); humanoids: below
    if (d.special == SpBoat) {
        root = mul(root, translate(0, 0.375f, 0));
        root = mul(root, rotY((180.0f - yaw) * kDeg));
        const float hurt = float(e.vehicleHurt) - pt;
        if (hurt > 0) root = mul(root, rotX(std::sin(hurt) * hurt * std::max(e.vehicleDamage - pt, 0.0f) / 10.0f * float(e.vehicleHurtDir) * kDeg));
        const float bubble = e.bubbleAngleO + (e.bubbleAngle - e.bubbleAngleO) * pt;  // getBubbleAngle
        if (bubble != 0.0f && !boatUnderWater(e)) {
            // Quaternionf.setAngleAxis(angle, 1, 0, 1): the axis is not normalised, so neither is the quaternion
            // (Matrix4f.rotate takes it as it is).
            const float hs = std::sin(bubble * kDeg * 0.5f), x = hs, z = hs, w = std::cos(bubble * kDeg * 0.5f);
            const float w2 = w * w, x2 = x * x, z2 = z * z, dzw = 2 * z * w, dxz = 2 * x * z, dxw = 2 * x * w;
            root = mul(root, M34{{w2 + x2 - z2, -dzw, dxz, 0, dzw, -z2 + w2 - x2, -dxw, 0, dxz, dxw, z2 - x2 + w2, 0}});
        }
        root = mul(root, scale(-1, -1, 1));
        root = mul(root, rotY(90.0f * kDeg));
    } else if (d.special == SpMinecart) {
        root = mul(root, translate(0, 0.375f, 0));
        root = mul(root, rotY((180.0f - yaw) * kDeg));
        root = mul(root, rotZ(-pitch * kDeg));
        const float hurt = float(e.vehicleHurt) - pt;
        if (hurt > 0) root = mul(root, rotX(std::sin(hurt) * hurt * std::max(e.vehicleDamage - pt, 0.0f) / 10.0f * float(e.vehicleHurtDir) * kDeg));
        minecartContents(e, pt, *reinterpret_cast<const Mat34*>(&root), light, layers, layerCount);
        root = mul(root, scale(-1, -1, 1));
    } else if (d.special == SpCushion) {  // CushionRenderer.submit: facing the yaw's direction, upside down
        const float facing = std::floor(yaw / 90.0f + 0.5f) * 90.0f;  // Direction.fromYRot(...).toYRot()
        root = mul(root, rotY((180.0f - facing) * kDeg));
        root = mul(root, rotX(kPi));
        root = mul(root, translate(0, -0.25f, 0));
    } else if (d.special == SpArrow || d.special == SpTrident) {
        root = mul(root, rotY((yaw - 90.0f) * kDeg));
        root = mul(root, rotZ((pitch + (d.special == SpTrident ? 90.0f : 0.0f)) * kDeg));
    } else if (d.special == SpObject) {  // the projectile / decoration renderers' own transforms
        if (d.anim == Anim::EvokerFangs) {  // EvokerFangsRenderer: drawn once the bite started
            if (!e.sp.t[1]) return true;
            root = mul(root, rotY((90.0f - e.yaw) * kDeg));
            root = mul(mul(root, scale(-1, -1, 1)), translate(0, -1.501f, 0));
        } else if (d.anim == Anim::EndCrystal) {  // EndCrystalRenderer
            root = mul(mul(root, scale(2, 2, 2)), translate(0, -0.5f, 0));
        } else if (d.anim == Anim::ShulkerBullet) {  // ShulkerBulletRenderer
            const float tc = s.age;
            root = mul(root, translate(0, 0.15f, 0));
            root = mul(root, rotY(std::sin(tc * 0.1f) * 180.0f * kDeg));
            root = mul(root, rotX(std::cos(tc * 0.1f) * 180.0f * kDeg));
            root = mul(root, rotZ(std::sin(tc * 0.15f) * 360.0f * kDeg));
            root = mul(root, scale(-0.5f, -0.5f, 0.5f));
        } else if (d.anim == Anim::LlamaSpit) {  // LlamaSpitRenderer
            root = mul(root, translate(0, 0.15f, 0));
            root = mul(mul(root, rotY((yaw - 90.0f) * kDeg)), rotZ(pitch * kDeg));
        } else if (d.anim != Anim::WindCharge) {  // leash knot, wither skull: flipped
            root = mul(root, scale(-1, -1, 1));
        }
    } else if (d.special == SpDragon) {  // EnderDragonRenderer: the flight history turns the body
        float h7[2], h5[2], h10[2];
        auto sample = [&](int delay, float out[2]) {  // DragonFlightHistory.get(delay, partialTicks)
            const float a = e.health <= 0 ? 0.0f : pt;
            const int head = std::max(e.sp.historyPos, 0);
            const float* x = e.sp.history[(head - delay) & 63];
            const float* y = e.sp.history[(head - delay - 1) & 63];
            out[0] = y[0] + (x[0] - y[0]) * a;
            out[1] = lerpAngle(a, y[1], x[1]);
        };
        sample(7, h7), sample(5, h5), sample(10, h10);
        root = mul(root, rotY(-h7[1] * kDeg));
        root = mul(root, rotX((h5[0] - h10[0]) * 10.0f * kDeg));
        root = mul(root, translate(0, 0, 1.0f));
        root = mul(mul(root, scale(-1, -1, 1)), translate(0, -1.501f, 0));
        if (e.hurtTime > 0) overlay |= 77;
        s.walkPos = s.walkSpeed = 0;
    } else if (living) {
        const float bodyYaw = lerpAngle(pt, e.prevBodyYaw, e.bodyYaw);
        const float headYaw = lerpAngle(pt, e.prevHeadYaw, e.headYaw);
        const bool player = e.category == Category::Player;
        const bool sleeping = e.pose == 2, spinAttack = (e.livingFlags & 4) != 0, fallFlying = (e.sharedFlags & 0x80) != 0;
        const float swimAmount = e.swimAmountO + (e.swimAmount - e.swimAmountO) * pt;
        if (player && e.pose == 5) root = mul(root, translate(0, -2.0f / 16.0f, 0));
        // LivingEntityRenderer.submit: a sleeping entity lies with its head on the bed's pillow.
        static constexpr int kBedStep[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};  // Direction 2D: S, W, N, E
        if (sleeping && e.bedFacing >= 0) {
            const float headOffset = (e.info ? e.info->eye : 1.62f) - 0.1f;
            root = mul(root, translate(-kBedStep[e.bedFacing][0] * headOffset, 0, -kBedStep[e.bedFacing][1] * headOffset));
        }
        if (d.special == SpArmorStand) {  // ArmorStandRenderer.setupRotations: a hit makes it wiggle
            root = mul(root, rotY((180.0f - yaw) * kDeg));
            const float wiggle = e.sp.t[1] ? float(e.age - e.sp.t[0]) + pt : 99.0f;
            if (wiggle < 5.0f) root = mul(root, rotY(std::sin(wiggle / 1.5f * kPi) * 3.0f * kDeg));
        } else {
            // LivingEntityRenderer.setupRotations (frozen / converting shaking, death roll, riptide spin, sleeping,
            // upside-down names) and the species' own (SquidRenderer replaces it).
            float bodyRot = d.special == SpShulker ? bodyYaw + 180.0f : bodyYaw;  // ShulkerRenderer.setupRotations
            if (isShaking(e, world_.piglinsZombify)) bodyRot += float(std::cos(std::floor(float(e.age) + pt) * 3.25f) * kPi * 0.4f);
            if (d.anim == Anim::Pufferfish) root = mul(root, translate(0, std::cos(s.age * 0.05f) * 0.08f, 0));
            const bool upsideDown = (e.name == "Dinnerbone" || e.name == "Grumm") && (!player || (e.skinParts & 1));
            if (d.anim == Anim::Squid) {  // SquidRenderer.setupRotations
                const float xb = e.sp.ao[kXBody] + (e.sp.a[kXBody] - e.sp.ao[kXBody]) * pt;
                const float zb = e.sp.ao[kZBody] + (e.sp.a[kZBody] - e.sp.ao[kZBody]) * pt;
                root = mul(root, translate(0, e.baby ? 0.25f : 0.5f, 0));
                root = mul(root, rotY((180.0f - bodyRot) * kDeg));
                root = mul(mul(root, rotX(xb * kDeg)), rotY(zb * kDeg));
                root = mul(root, translate(0, e.baby ? -0.6f : -1.2f, 0));
            } else if (!sleeping) root = mul(root, rotY((180.0f - bodyRot) * kDeg));
            if (d.anim == Anim::Squid) {
            } else if (e.deathTime > 0 && !(d.anim == Anim::Creaking && dataInt(e, "IS_TEARING_DOWN"))) {
                float fall = std::sqrt(std::max(0.0f, (float(e.deathTime) + pt - 1.0f) / 20.0f * 1.6f));
                root = mul(root, rotZ(std::min(fall, 1.0f) * d.flip * kDeg));
            } else if (spinAttack) {
                root = mul(root, rotX((-90.0f - pitch) * kDeg));
                root = mul(root, rotY((float(e.age) + pt) * -75.0f * kDeg));
            } else if (sleeping) {
                static constexpr float kSleepRot[4] = {90.0f, 0.0f, 270.0f, 180.0f};  // sleepDirectionToRotation
                root = mul(root, rotY((e.bedFacing >= 0 ? kSleepRot[e.bedFacing] : bodyRot) * kDeg));
                root = mul(root, rotZ(d.flip * kDeg));
                root = mul(root, rotY(270.0f * kDeg));
            } else if (upsideDown) {
                root = mul(root, translate(0, (e.info ? e.info->height : 1.8f) + 0.1f, 0));
                root = mul(root, rotZ(kPi));
            }
            speciesRotations(e, d, pt, pitch, *reinterpret_cast<Mat34*>(&root));
            // AvatarRenderer.setupRotations: gliding and swimming / crawling bodies.
            if (player && fallFlying) {
                const float ft = float(e.fallFlyTicks) + pt;
                const float flyScale = std::clamp(ft * ft / 100.0f, 0.0f, 1.0f);
                if (!spinAttack) root = mul(root, rotX(flyScale * (-90.0f - pitch) * kDeg));
                float mv[3], look[3];
                for (int k = 0; k < 3; ++k) mv[k] = e.moveO[k] + (e.move[k] - e.moveO[k]) * pt;
                look[0] = -std::sin(yaw * kDeg) * std::cos(pitch * kDeg), look[2] = std::cos(yaw * kDeg) * std::cos(pitch * kDeg);
                const float mh = mv[0] * mv[0] + mv[2] * mv[2], lh = look[0] * look[0] + look[2] * look[2];
                if (mh > 1e-5f && lh > 1e-5f) {
                    const float dot = (mv[0] * look[0] + mv[2] * look[2]) / std::sqrt(mh * lh);
                    const float sgn = mv[0] * look[2] - mv[2] * look[0];
                    root = mul(root, rotY((sgn > 0 ? 1.0f : sgn < 0 ? -1.0f : 0.0f) * std::acos(std::min(1.0f, std::abs(dot)))));
                }
            } else if (player && swimAmount > 0) {
                const float target = e.inWater ? -90.0f - pitch : -90.0f;
                root = mul(root, rotX(swimAmount * target * kDeg));
                if (e.pose == 3) root = mul(root, translate(0, -1.0f, 0.3f));
            }
            if (d.special == SpShulker) {  // rotateAround(attachFace.getOpposite().getRotation(), 0, 0.5, 0)
                M34 face = M34::identity();
                switch (e.attachFace) {
                    case 1: face = rotX(kPi); break;                              // up: down's rotation
                    case 2: face = rotX(kPi / 2); break;                          // north: south's
                    case 3: face = mul(rotX(kPi / 2), rotZ(kPi)); break;          // south: north's
                    case 4: face = mul(rotX(kPi / 2), rotZ(-kPi / 2)); break;     // west: east's
                    case 5: face = mul(rotX(kPi / 2), rotZ(kPi / 2)); break;      // east: west's
                    default: break;                                                // down: up's (none)
                }
                root = mul(mul(mul(root, translate(0, 0.5f, 0)), face), translate(0, -0.5f, 0));
            }
        }
        root = mul(root, scale(-1, -1, 1));
        float sc = d.scale;
        float sx = sc, sy = sc, sz = sc;
        if (d.special == SpSlime) {  // AbstractCubeMobRenderer: downscaleSlightly, size and squish
            root = mul(mul(root, scale(0.999f, 0.999f, 0.999f)), translate(0, 0.001f, 0));
            const float size = float(e.size), squish = e.sp.ao[kSquish] + (e.sp.a[kSquish] - e.sp.ao[kSquish]) * pt;
            const float w = 1.0f / (squish / (size * 0.5f + 1.0f) + 1.0f);
            sx *= w * size, sy *= 1.0f / w * size, sz *= w * size;
        } else if (d.anim == Anim::Phantom) {  // PhantomRenderer.scale
            const float k = 1.0f + 0.15f * float(dataInt(e, "ID_SIZE"));
            root = mul(mul(root, scale(k, k, k)), translate(0, 1.3125f, 0.1875f));
        } else if (d.special == SpWither) {  // WitherBossRenderer.scale: shrunk while invulnerable
            const int inv = int(dataInt(e, "DATA_ID_INV"));
            const float k = 2.0f - (inv > 0 ? (float(inv) - pt) / 220.0f * 0.5f : 0.0f);
            sx *= k, sy *= k, sz *= k;
        }
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
        float lift = 0;
        if (d.special == SpSulfurCube) {  // SulfurCubeRenderer.scale: 0.999, the size, adults halved, then lifted
            const float k = 0.999f * float(e.size) * (e.baby ? 1.0f : 0.5f);
            sx = sy = sz = k;
            lift = (e.baby ? 1.24f : 0.98f) - ((e.sharedFlags & 0x20) ? 0.0f : 1.0f / 16.0f) + 0.001f / k;
        }
        if (sx != 1 || sy != 1 || sz != 1) root = mul(root, scale(sx, sy, sz));
        root = mul(root, translate(0, lift - 1.501f, 0));
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
        if (d.anim == Anim::Camel || d.anim == Anim::Strider) s.riding = ridden;  // their models want "is ridden"
        s.swimAmount = swimAmount;
        s.fallFlying = fallFlying;
        if (fallFlying) {  // HumanoidMobRenderer.extractHumanoidRenderState: speedValue from the delta movement
            float v = 0;
            for (int k = 0; k < 3; ++k) {
                const float m = e.moveO[k] + (e.move[k] - e.moveO[k]) * pt;
                v += m * m;
            }
            v /= 0.2f;
            s.speedValue = std::max(1.0f, v * v * v);
        }
        s.aggressive = (e.mobFlags & 4) != 0;
        const bool rightHanded = ent::rightHanded(e);
        s.rightItem = rightHanded ? e.equipment[0] >= 0 : e.equipment[1] >= 0;
        s.leftItem = rightHanded ? e.equipment[1] >= 0 : e.equipment[0] >= 0;
        s.leftHanded = !rightHanded;
        s.swingLeft = (e.swingHand == 0) != rightHanded;  // the main hand swings the main arm
        auto nameOf = [&](int slot) { return e.equipment[slot] >= 0 && items_ ? items_->itemName(e.equipment[slot]) : std::string(); };
        auto isSpear = [&](int slot) { return nameOf(slot).ends_with("_spear"); };  // ItemTags.SPEARS
        s.swingType = e.swingType >= 0 ? e.swingType : isSpear(e.swingHand) ? 2 : 1;
        {
            KineticParams k[2];
            for (int slot = 0; slot < 2 && rules_; ++slot)
                if (const std::string n = nameOf(slot); !n.empty()) {
                    const ItemRules::Components& c = rules_->components(n);
                    k[slot] = {c.kineticValid, c.kineticDelay, c.kineticDismount, c.kineticKnockback, c.kineticDamage, c.kineticForward};
                }
            s.rightKinetic = rightHanded ? k[0] : k[1], s.leftKinetic = rightHanded ? k[1] : k[0];
        }
        // HumanoidMobRenderer.usesSpearPose: the hand's STAB swing, or a spear in it.
        auto spearPose = [&](int slot) { return (e.swingTime >= 0 && s.swingType == 2 && e.swingHand == slot) || isSpear(slot); };
        if (e.category != Category::Player && items_ && (d.anim == Anim::Zombie || d.anim == Anim::Skeleton)) {
            // HumanoidMobRenderer.getArmPose: SPEAR or EMPTY (skeletons draw an aggressive main-hand bow, the drowned
            // an aggressive main-hand trident).
            const bool drowned = e.info && e.info->name == "minecraft:drowned";
            auto mobPose = [&](int slot) {
                const std::string n = nameOf(slot);
                if (slot == 0 && s.aggressive && d.anim == Anim::Skeleton && n == "bow") return ArmPose::BowAndArrow;
                if (slot == 0 && s.aggressive && drowned && n == "trident") return ArmPose::ThrowTrident;
                return spearPose(slot) ? ArmPose::Spear : ArmPose::Empty;
            };
            const ArmPose mainPose = mobPose(0), offPose = mobPose(1);
            s.havePoses = true;
            s.rightPose = rightHanded ? mainPose : offPose;
            s.leftPose = rightHanded ? offPose : mainPose;
            s.holdingBow = nameOf(0) == "bow";
        }
        if (e.category == Category::Player && items_) {
            // AvatarRenderer.getArmPose: the use animation of the hand in use, a charged crossbow, else ITEM / EMPTY.
            auto poseFor = [&](int slot) {
                const int item = e.equipment[slot];
                if (item < 0) return ArmPose::Empty;
                const std::string n = items_->itemName(item);
                if (n == "crossbow" && (e.chargedCrossbow >> slot) & 1 && e.swingTime < 0) return ArmPose::CrossbowHold;
                if ((e.livingFlags & 1) && ((e.livingFlags & 2) ? 1 : 0) == slot) {  // the use animation of the stack
                    const ui::ItemStack* st = e.equipStack[slot].get();
                    const int anim = st && items_->items() ? ui::useAnimation(*items_->items(), *st) : 0;
                    switch (anim) {
                        case 3: return ArmPose::Block;
                        case 4: return ArmPose::BowAndArrow;
                        case 5: return ArmPose::ThrowTrident;
                        case 6: return ArmPose::CrossbowCharge;
                        case 7: return ArmPose::Spyglass;
                        case 8: return ArmPose::TootHorn;
                        case 9: return ArmPose::Brush;
                        case 11: return ArmPose::Spear;
                        default: break;
                    }
                }
                return spearPose(slot) ? ArmPose::Spear : ArmPose::Item;
            };
            ArmPose mainPose = poseFor(0), offPose = poseFor(1);
            if (mainPose == ArmPose::BowAndArrow || mainPose == ArmPose::CrossbowCharge || mainPose == ArmPose::CrossbowHold)
                offPose = e.equipment[1] < 0 ? ArmPose::Empty : ArmPose::Item;
            s.havePoses = true;
            s.rightPose = rightHanded ? mainPose : offPose;
            s.leftPose = rightHanded ? offPose : mainPose;
            s.usingItem = (e.livingFlags & 1) != 0;
            s.useRight = ((e.livingFlags & 2) != 0) != rightHanded;
            s.useTicks = s.usingItem ? float(e.useTicks) + pt : 0.0f;
            {  // HumanoidMobRenderer: maxCrossbowChargeDuration (CrossbowItem.getChargeDuration of the used stack)
                const ui::ItemStack* used = e.equipStack[(e.livingFlags & 2) ? 1 : 0].get();
                if (used && items_->items()) s.crossbowCharge = float(ui::crossbowChargeTicks(*items_->items(), *used));
            }
        }
        if ((e.name == "Dinnerbone" || e.name == "Grumm") && (e.category != Category::Player || (e.skinParts & 1)))
            s.pitch = -s.pitch, s.headYaw = -s.headYaw;  // upside down
        if (e.category == Category::Living) mobPoses(e, d, cam, s);
        s.standFlags = e.standFlags;
        s.standPose = e.standPose;
        s.peek = e.peekO + (e.peek - e.peekO) * pt;
        s.skinParts = e.category == Category::Player ? e.skinParts : 0x7F;
    } else {
        root = mul(root, rotY((180.0f - yaw) * kDeg));
        root = mul(root, scale(-1, -1, 1));
        root = mul(root, translate(0, -1.501f, 0));
    }

    if (rootOverride_) root = *reinterpret_cast<const M34*>(rootOverride_);  // the elder guardian appearance
    // DonkeyModel / LlamaModel: the chests show only on chested animals.
    const bool hideChests = !e.hasChest && e.info &&
                            (e.info->name.ends_with("llama") || e.info->name == "minecraft:donkey" || e.info->name == "minecraft:mule");
    // EquineSaddleModel: the reins show only while ridden.
    const bool hideReins = e.equipment[7] >= 0 && e.info && (e.info->name.ends_with("horse") || e.info->name == "minecraft:donkey" ||
                                                             e.info->name == "minecraft:mule") &&
                           std::find(ridden_.begin(), ridden_.end(), e.id) == ridden_.end();
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
        hidden_.assign(parts, 0);
        for (size_t i = 0; i < parts; ++i) poses_[i] = m.parts[i].pose;
        u64 hiddenRoles = 0;
        M34 layerRoot = (L.flags & DisplayBlock) ? *reinterpret_cast<const M34*>(&displayRoot_) : root;
        if (d.anim == Anim::ShulkerBullet && li == 1) layerRoot = mul(root, scale(1.5f, 1.5f, 1.5f));  // ShulkerBulletRenderer
        if (d.special == SpBoat) {  // BoatModel / RaftModel.animatePaddle from AbstractBoat.getRowingTime
            for (size_t i = 0; i < parts; ++i) {
                const bool left = m.parts[i].name == "left_paddle";
                if (!left && m.parts[i].name != "right_paddle") continue;
                const int side = left ? 0 : 1;
                const float t = e.paddle[side] ? e.paddlePos[side] - kPi / 8 * (1.0f - pt) : 0.0f;
                auto clampedLerp = [](float a, float lo, float hi) { return lo + std::clamp(a, 0.0f, 1.0f) * (hi - lo); };
                poses_[i].xRot = clampedLerp((std::sin(-t) + 1.0f) / 2.0f, -kPi / 3, -kPi / 12);
                poses_[i].yRot = clampedLerp((std::sin(-t + 1.0f) + 1.0f) / 2.0f, -kPi / 4, kPi / 4);
                if (side == 1) poses_[i].yRot = kPi - poses_[i].yRot;
            }
        }
        if (L.flags & SpinFx) {  // SpinAttackEffectModel.setupAnim: box i turns at -(45 + 5 (i + 1)) degrees per tick
            for (size_t i = 0; i < parts; ++i)
                if (m.parts[i].name == "box0" || m.parts[i].name == "box1") {
                    const float rate = 45.0f + float(m.parts[i].name == "box0" ? 1 : 2) * 5.0f;
                    const float deg = (float(e.age) + pt) * -rate;
                    poses_[i].yRot = (deg - 360.0f * std::floor((deg + 180.0f) / 360.0f)) * kDeg;  // Mth.wrapDegrees
                }
        } else if (L.flags & Elytra) {  // WingsLayer: translate(0, 0, 0.125); ElytraModel.setupAnim (ElytraAnimationState)
            layerRoot = mul(root, translate(0, 0, 0.125f));
            const i16 lw = m.role(LeftWing), rw = m.role(RightWing);
            float r[3];
            for (int k = 0; k < 3; ++k) r[k] = e.elytraRotO[k] + (e.elytraRot[k] - e.elytraRotO[k]) * pt;
            const float wy = s.crouching ? 3.0f : 0.0f;
            if (lw >= 0) poses_[size_t(lw)].y = wy, poses_[size_t(lw)].xRot = r[0], poses_[size_t(lw)].yRot = r[1], poses_[size_t(lw)].zRot = r[2];
            if (rw >= 0) poses_[size_t(rw)].y = wy, poses_[size_t(rw)].xRot = r[0], poses_[size_t(rw)].yRot = -r[1], poses_[size_t(rw)].zRot = -r[2];
        } else if ((L.flags & Cape) && roleSet) {  // PlayerCapeModel.setupAnim: the player's pose, then the cape
            if (capeOverArmor) layerRoot = mul(root, translate(0, -0.053125f, 0.06875f));
            for (size_t i = 0; i < parts; ++i) {
                const Role r = m.parts[i].role;
                if (r != RoleNone && ((roleSet >> r) & 1)) poses_[i] = rolePose[r];
            }
            // AvatarRenderer.extractCapeState: the cloak's lag behind the body, the walking bob.
            const float yBody = lerpAngle(pt, e.prevBodyYaw, e.bodyYaw) * kDeg;
            double dl[3];
            for (int a = 0; a < 3; ++a)
                dl[a] = (e.cloakO[a] + (e.cloak[a] - e.cloakO[a]) * pt) - (e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt);
            const double fx = std::sin(yBody), fz = -std::cos(yBody);
            const float ft = (e.sharedFlags & 0x80) ? float(e.fallFlyTicks) + pt : 0.0f;
            const float fallFlyingScale = std::clamp(ft * ft / 100.0f, 0.0f, 1.0f);
            float flap = std::clamp(float(dl[1]) * 10.0f, -6.0f, 32.0f);
            const float lean = std::clamp(float(dl[0] * fx + dl[2] * fz) * 100.0f * (1.0f - fallFlyingScale), 0.0f, 150.0f);
            const float lean2 = std::clamp(float(dl[0] * fz - dl[2] * fx) * 100.0f, -20.0f, 20.0f);
            const float bob = e.bobO + (e.bob - e.bobO) * pt, walk = e.walkDistO + (e.walkDist - e.walkDistO) * pt;
            flap += std::sin(walk * 6.0f) * 32.0f * bob;
            const int capePart = m.part("cape");
            if (capePart >= 0) {  // rotateBy: Ry(pi) * Ry(-pi) Rx(a) Rz(b) Ry(c) = Rx(a) Rz(b) Ry(c), as ZYX Euler angles
                const float a = (6.0f + lean / 2.0f + flap) * kDeg, b = lean2 / 2.0f * kDeg, cc = (180.0f - lean2 / 2.0f) * kDeg;
                const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cy = std::cos(cc), sy = std::sin(cc);
                // Rx(a) * Rz(b) * Ry(c), row-major
                const float rz[3][3] = {{cb, -sb, 0}, {sb, cb, 0}, {0, 0, 1}};
                const float ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
                const float rx[3][3] = {{1, 0, 0}, {0, ca, -sa}, {0, sa, ca}};
                float t[3][3] = {}, R[3][3] = {};
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        for (int k = 0; k < 3; ++k) t[i][j] += rx[i][k] * rz[k][j];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        for (int k = 0; k < 3; ++k) R[i][j] += t[i][k] * ry[k][j];
                PartPose& cp = poses_[size_t(capePart)];  // R = Rz(z) Ry(y) Rx(x)
                cp.yRot = std::asin(std::clamp(-R[2][0], -1.0f, 1.0f));
                cp.xRot = std::atan2(R[2][1], R[2][2]);
                cp.zRot = std::atan2(R[1][0], R[0][0]);
            }
        } else if (L.flags & Shoulder) {  // ParrotModel ON_SHOULDER: looking with the player, the tail swinging with its walk
            layerRoot = mul(root, translate(L.scroll[0], s.crouching ? -1.3f : -1.5f, 0.0f));
            const i16 head = m.part("head"), tail = m.part("tail"), lw = m.part("left_wing"), rw = m.part("right_wing");
            if (head >= 0) poses_[size_t(head)].xRot = s.pitch * kDeg, poses_[size_t(head)].yRot = s.headYaw * kDeg;
            if (tail >= 0) poses_[size_t(tail)].xRot += std::cos(s.walkPos * 0.6662f) * 0.3f * s.walkSpeed;
            if (lw >= 0) poses_[size_t(lw)].zRot = -0.0873f;
            if (rw >= 0) poses_[size_t(rw)].zRot = 0.0873f;
        } else if ((L.flags & Stuck) && !baseMats_.empty()) {  // StuckInBodyLayer: a random cube of a random body part
            const Model& bm = assets_.modelAt(layers[0].model);
            std::vector<u32> bodyParts;
            for (size_t i = 0; i < bm.parts.size() && i < baseMats_.size(); ++i)
                if (bm.parts[i].quadCount) bodyParts.push_back(u32(i));
            if (!bodyParts.empty()) {
                const bool stinger = L.scroll[1] > 0.5f;
                u64 seed = (u64(i64(e.id)) ^ 0x5DEECE66Dull) & ((1ull << 48) - 1);  // RandomSource(id)
                auto next = [&](int bits) {
                    seed = (seed * 0x5DEECE66Dull + 0xBull) & ((1ull << 48) - 1);
                    return int(i64(seed) >> (48 - bits));
                };
                auto nextInt = [&](int bound) {
                    if ((bound & -bound) == bound) return int((i64(bound) * i64(next(31))) >> 31);
                    int bits, val;
                    do bits = next(31), val = bits % bound;
                    while (bits - val + (bound - 1) < 0);
                    return val;
                };
                auto nextFloat = [&] { return float(next(24)) / float(1 << 24); };
                u32 partIdx = 0;
                float cube[6] = {}, mid[3] = {};
                for (int k = 0; k <= int(L.scroll[0]); ++k) {
                    partIdx = bodyParts[size_t(nextInt(int(bodyParts.size())))];
                    const ModelPart& bp = bm.parts[partIdx];
                    const int cubes = std::max(1, int(bp.quadCount / 6));
                    const int ci = nextInt(cubes);
                    const auto& quads = assets_.quads();
                    cube[0] = cube[1] = cube[2] = 1e9f, cube[3] = cube[4] = cube[5] = -1e9f;
                    for (u32 q = bp.quadFirst + u32(ci) * 6; q < std::min(bp.quadFirst + bp.quadCount, bp.quadFirst + u32(ci + 1) * 6); ++q)
                        for (int vtx = 0; vtx < 4; ++vtx)
                            for (int a = 0; a < 3; ++a)
                                cube[a] = std::min(cube[a], quads[q].pos[vtx][a]), cube[3 + a] = std::max(cube[3 + a], quads[q].pos[vtx][a]);
                    for (float& mv : mid) mv = nextFloat();
                    if (stinger) {  // ON_SURFACE
                        const int plane = nextInt(3);
                        mid[plane] = mid[plane] > 0.5f ? 1.0f : 0.5f;
                    }
                }
                M34 pr = *reinterpret_cast<const M34*>(&baseMats_[partIdx]);
                pr = mul(pr, translate((cube[0] + (cube[3] - cube[0]) * mid[0]) / 16.0f, (cube[1] + (cube[4] - cube[1]) * mid[1]) / 16.0f,
                                       (cube[2] + (cube[5] - cube[2]) * mid[2]) / 16.0f));
                const float dx = -(mid[0] * 2 - 1), dy = -(mid[1] * 2 - 1), dz = -(mid[2] * 2 - 1);
                const float yr = std::atan2(dx, dz) / kDeg, xr = std::atan2(dy, std::sqrt(dx * dx + dz * dz)) / kDeg;
                layerRoot = mul(mul(pr, rotY((yr - 90.0f) * kDeg)), rotZ(xr * kDeg));
            }
        } else if ((L.flags & Armor) && roleSet) {  // HumanoidModel.copyPropertiesTo: the parent's pose by part
            for (size_t i = 0; i < parts; ++i) {
                const Role r = m.parts[i].role;
                if (r != RoleNone && ((roleSet >> r) & 1)) poses_[i] = rolePose[r];
            }
            hiddenRoles = baseHidden & ((1ull << LeftArm) | (1ull << RightArm));
            if (const int g = m.part("goggles"); g >= 0 && d.anim == Anim::HappyGhast) {
                // HappyGhastHarnessModel.setupAnim: the goggles over the eyes while ridden, up on the forehead else
                if (e.passengerCount > 0) poses_[size_t(g)].xRot = 0.0f, poses_[size_t(g)].y = 14.0f;
                else poses_[size_t(g)].xRot = -0.7854f, poses_[size_t(g)].y = 9.0f;
            }
        } else if (living || d.anim != Anim::None) {
            AnimState sl = s;
            if (L.flags & Armor) sl.skinParts = 0x7F;
            sl.hide = hidden_.data();
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
            if (((hiddenRoles >> part.role) & 1) && part.role != RoleNone) hidden_[i] = 1;
            if (hideChests && (part.name == "left_chest" || part.name == "right_chest")) hidden_[i] = 1;
            if (hideReins && (part.name == "left_saddle_line" || part.name == "right_saddle_line")) hidden_[i] = 1;
            if (part.parent >= 0 && hidden_[size_t(part.parent)] == 1) hidden_[i] = 1;  // 2: skipDraw (children drawn)
        }
        if (li == 0 && living)
            for (size_t i = 0; i < parts; ++i)
                if (const Role r = m.parts[i].role; r != RoleNone)
                    roleMat[r] = *reinterpret_cast<const M34*>(&mats_[i]), roleMatSet |= 1ull << r;
        if (li == 0) baseMats_.assign(mats_.begin(), mats_.begin() + ptrdiff_t(parts));  // the stuck items' anchors
        if (li == 0 && living && attachments_.size() < 4096) {  // ItemInHandLayer / CustomHeadLayer anchors
            Attachment a{};
            a.entity = &e == localPlayer_ ? kLocalPlayerId : e.id;
            a.light = light;
            a.babyOffset = e.baby && d.special != SpArmorStand;
            a.shadowOnly = shadowOnly_;
            const M34* pm = reinterpret_cast<const M34*>(mats_.data());
            auto partMat = [&](std::string_view name, M34& out) {
                const int i = m.part(name);
                if (i < 0 || hidden_[size_t(i)] == 1) return false;
                out = pm[i];
                return true;
            };
            // ArmedModel.translateToHand of each model (an arm moved sideways for skeletons and slim players; the
            // allay's body, the vex's / copper golem's scaled arms).
            const i16 arms[2] = {m.role(RightArm), m.role(LeftArm)};
            bool handItems = true;  // illagers show items only while attacking / casting
            if (d.anim == Anim::Illager) {
                const std::string& n = e.info ? e.info->name : std::string();
                const bool casting = dataInt(e, "DATA_SPELL_CASTING_ID") > 0, aggressive = (e.mobFlags & 4) != 0;
                handItems = n == "minecraft:vindicator" ? aggressive : n == "minecraft:evoker" ? casting
                          : n == "minecraft:illusioner" ? casting || aggressive : true;
            }
            for (int k = 0; k < 2 && handItems; ++k) {
                if (arms[k] < 0 || hidden_[size_t(arms[k])]) continue;
                M34 hm = pm[arms[k]];
                const float side = k == 0 ? 1.0f : -1.0f;
                const float shift = d.special == SpPlayer && e.slim ? 0.5f : d.anim == Anim::Skeleton ? 1.0f : 0.0f;
                if (shift != 0) {  // part.x += offset while translating
                    const ModelPart& ap = m.parts[size_t(arms[k])];
                    const M34 par = ap.parent >= 0 ? pm[ap.parent] : layerRoot;
                    for (int r = 0; r < 3; ++r) hm.m[r * 4 + 3] += par.m[r * 4] * side * shift / 16.0f;
                }
                if (d.anim == Anim::Allay) {
                    M34 body;
                    if (!partMat("body", body)) continue;
                    hm = mul(mul(body, translate(0, 0.0625f, 0.1875f)), rotX(poses_[size_t(m.part("right_arm"))].xRot));
                    hm = mul(mul(hm, scale(0.7f, 0.7f, 0.7f)), translate(0.0625f, 0, 0));
                } else if (d.anim == Anim::Vex) {
                    hm = mul(mul(hm, scale(0.55f, 0.55f, 0.55f)), translate(side * 0.046875f, -0.15625f, 0.078125f));
                } else if (d.anim == Anim::CopperGolem) {
                    if (dataInt(e, "COPPER_GOLEM_STATE") == 0) hm = mul(mul(hm, rotY((k == 0 ? -90.0f : 90.0f) * kDeg)), translate(0, 0, 0.125f));
                    else hm = mul(mul(hm, scale(0.55f, 0.55f, 0.55f)), translate(-0.125f, 0.3125f, -0.1875f));
                }
                std::memcpy(a.hand[k], hm.m, sizeof(a.hand[k]));
                a.valid |= u8(1 << k);
            }
            if (const i16 h = m.role(Head); h >= 0) {  // CustomHeadLayer: Transforms' scale outside the model
                float hs = 1.0f, vs = 1.0f;
                if (d.anim == Anim::Piglin || d.anim == Anim::ZombifiedPiglin) hs = 1.0019531f;
                else if (d.special == SpVillager || d.special == SpZombieVillager) a.headY = -0.1171875f, a.skullY = -0.07421875f;
                M34 hm = pm[h];
                if (hs != 1.0f || vs != 1.0f) {  // root * scale * (the model's own chain)
                    M34 chain = M34::identity();
                    for (int i = h; i >= 0; i = m.parts[size_t(i)].parent) chain = mul(partLocal(poses_[size_t(i)]), chain);
                    hm = mul(mul(layerRoot, scale(hs, vs, hs)), chain);
                }
                if (d.anim == Anim::CopperGolem) hm = mul(mul(hm, translate(0, 0.125f, 0)), scale(1.0625f, 1.0625f, 1.0625f));
                std::memcpy(a.head, hm.m, sizeof(a.head));
                a.valid |= 4;
            }
            // HoldingEntityRenderState layers: the main hand item (ground transform) in the mouth / by the arms.
            if (e.equipment[0] >= 0) {
                M34 im;
                bool item = false;
                const float headYaw = s.headYaw, headPitch = s.pitch;
                switch (d.anim) {
                    case Anim::Fox: {  // FoxHeldItemLayer
                        const int hi = m.part("head");
                        if (hi < 0) break;
                        const PartPose& hp = poses_[size_t(hi)];
                        const bool sleeping = (dataInt(e, "DATA_FLAGS_ID") & 32) != 0;
                        im = mul(root, translate(hp.x / 16.0f, hp.y / 16.0f, hp.z / 16.0f));
                        if (e.baby) im = mul(im, scale(0.75f, 0.75f, 0.75f));
                        const float roll = (e.sp.ao[kFoxInterest] + (e.sp.a[kFoxInterest] - e.sp.ao[kFoxInterest]) * pt) * 0.11f * kPi;
                        im = mul(mul(mul(im, rotZ(roll)), rotY(headYaw * kDeg)), rotX(headPitch * kDeg));
                        im = mul(im, e.baby ? (sleeping ? translate(0.4f, 0.26f, 0.15f) : translate(0.06f, 0.26f, -0.5f))
                                            : (sleeping ? translate(0.46f, 0.26f, 0.22f) : translate(0.06f, 0.27f, -0.5f)));
                        im = mul(im, rotX(kPi / 2));
                        if (sleeping) im = mul(im, rotZ(kPi / 2));
                        item = true;
                        break;
                    }
                    case Anim::Dolphin: {  // DolphinCarryingItemLayer
                        const float ax = std::abs(headPitch) / 60.0f;
                        im = mul(root, headPitch < 0 ? translate(0, 1.0f - ax * 0.5f, -1.0f + ax * 0.5f)
                                                     : translate(0, 1.0f + ax * 0.8f, -1.0f + ax * 0.2f));
                        item = true;
                        break;
                    }
                    case Anim::Panda: {  // PandaHoldsItemLayer: while sitting and not scared
                        if (!(dataInt(e, "DATA_ID_FLAGS") & 8) || (e.variant == 2 && speciesThundering())) break;
                        float z = -0.6f, y = 1.4f;
                        if (dataInt(e, "EAT_COUNTER") > 0) {
                            const float a6 = (float(e.age) + pt) * 0.6f;
                            z -= 0.2f * std::sin(a6) + 0.2f, y -= 0.09f * std::sin(a6);
                        }
                        im = mul(root, translate(0.1f, y, z));
                        item = true;
                        break;
                    }
                    case Anim::Witch:
                    case Anim::Villager:
                    case Anim::WanderingTrader: {  // WitchItemLayer (a potion by the nose) / CrossedArmsItemLayer
                        const std::string& held = assets_.itemNames()[size_t(e.equipment[0])];
                        if (d.anim == Anim::Witch && held == "potion") {
                            if (!partMat("nose", im)) break;
                            im = mul(mul(im, translate(0.0625f, 0.25f, 0)), rotZ(kPi));
                            im = mul(mul(mul(im, rotX(140.0f * kDeg)), rotZ(10.0f * kDeg)), rotX(kPi));
                        } else {
                            if (!partMat("arms", im)) break;
                            im = mul(mul(im, rotX(0.75f)), scale(1.07f, 1.07f, 1.07f));
                            im = mul(mul(im, translate(0, 0.13f, -0.34f)), rotX(kPi));
                        }
                        item = true;
                        break;
                    }
                    default: break;
                }
                if (item) std::memcpy(a.item, im.m, sizeof(a.item)), a.valid |= 8;
            }
            if (a.valid) attachments_.push_back(a);
            blockDecorations(e, d, m, pt, *reinterpret_cast<const Mat34*>(&root), light, overlay);
        }
        const u32 base = matCount_;
        std::memcpy(frameBase_ + kMatOffset + u64(base) * 48, mats_.data(), parts * 48);
        matCount_ += u32(parts);
        if (!pack_) {  // motion vectors: this entity's matrices for the next frame
            entMats_.insert(entMats_.end(), mats_.begin(), mats_.begin() + ptrdiff_t(parts));
            entHash_ = entHash_ * 31 + u32(L.model) * 7 + u32(parts);
        }
        // LivingEntityRenderer: an invisible entity's body shows only to those who see it (spectators, teammates seeing
        // friendly invisibles: translucent, 0x26 alpha); its skin layers do not; equipment, eyes and swirls stay.
        float alpha = appearanceAlpha_;
        if (living && (e.sharedFlags & 0x20)) {
            if (li == 0 && seeInvisible && seeInvisible(e)) alpha = 38.0f / 255.0f;
            else if (li == 0 || !(L.flags & (Armor | Emissive | Elytra | Glint | Scroll))) continue;
        }
        const TexRect& tr = assets_.rect(L.tex);
        const u32 layerColor = alpha >= 0 ? u32(alpha * 255.0f) << 24 | (L.color & 0xFFFFFF) : L.color;
        GpuInstance inst{base, layerColor, (L.flags & Emissive) ? (light | 256u) : light, (L.flags & NoOverlay) || alpha >= 0 ? 0u : overlay,
                         {tr.u0, tr.v0, tr.du, tr.dv}};
        if (L.flags & Dissolve) {  // the mask's atlas offset, as two signed 16-bit fractions
            auto q = [](float v) { return u32(u16(i16(std::clamp(v, -1.0f, 1.0f) * 32767.0f))); };
            inst.light |= 2048u;
            inst.overlay = q(L.scroll[0]) | q(L.scroll[1]) << 16;
        }
        if (L.flags & Scroll) {  // RenderTypes.energySwirl / breezeWind: the texture offset (bits 16..31, 1/256 steps)
            auto frac256 = [](float v) { return u32(int(std::floor((v - std::floor(v)) * 256.0f)) & 255); };
            inst.light |= 1024u | 512u;  // scrolled, NO_CARDINAL_LIGHTING
            inst.overlay = frac256(L.scroll[0]) << 16 | frac256(L.scroll[1]) << 24;
        }
        const u32 instId = instCount_++;
        std::memcpy(frameBase_ + kInstOffset + u64(instId) * 32, &inst, 32);
        const bool translucent = (L.flags & (Translucent | Additive)) != 0 || alpha >= 0;
        if (pack_) {  // shader pack: the posed quads as vertices (entity atlas coordinates)
            const auto& quads = assets_.quads();
            // Armour glint (TextureTransform.setupGlintTexturing, scale 0.16, the Glint Speed) as gbuffers_armor_glint
            // geometry: the texture transform is applied here (the glint texture wraps).
            const bool glint = (L.flags & Glint) && pack_->glintTexture(false);
            float scroll[2] = {};
            if (glint) {
                const double ms = nowSeconds() * 1000.0 * 8.0 * glintOptions().speed;
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
                        packQuad(4, pos, uv, n, glintOptions().white(), 256u, 0);  // alpha: the Glint Strength
                    }
                }
            }
            continue;
        }
        u32* refs = reinterpret_cast<u32*>(frameBase_ + kRefOffset);
        for (size_t i = 0; i < parts; ++i) {
            if (hidden_[i]) continue;  // hidden, or skipDraw (2): the children are drawn
            const ModelPart& part = m.parts[i];
            for (u32 q = part.quadFirst; q < part.quadFirst + part.quadCount; ++q) {
                if ((L.flags & Glint) && !shadowOnly_) glint_.push_back(q), glint_.push_back(instId);
                if (L.model == waterPatchModel_) {
                    mask_.push_back(q), mask_.push_back(instId);
                } else if (L.flags & Additive) {
                    if (!shadowOnly_) additive_.push_back(q), additive_.push_back(instId);
                } else if (translucent) {
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

namespace {
struct V3 {
    double x = 0, y = 0, z = 0;
    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(double s) const { return {x * s, y * s, z * s}; }
};
V3 yRot(V3 v, double a) {  // Vec3.yRot
    const double c = std::cos(a), s = std::sin(a);
    return {v.x * c + v.z * s, v.y, v.z * c - v.x * s};
}
V3 xRot(V3 v, double a) {  // Vec3.xRot
    const double c = std::cos(a), s = std::sin(a);
    return {v.x, v.y * c + v.z * s, v.z * c - v.y * s};
}
// The eye height by pose (players crouch to 1.27, swim / glide at 0.4), scaled like the box (babies).
double eyeHeight(const Entity& e) {
    if (!e.info) return 1.62;
    if (e.category == Category::Player) {
        if (e.pose == 5) return 1.27;
        if (e.pose == 1 || e.pose == 3 || e.pose == 4) return 0.4;
        if (e.pose == 2) return 0.2;
    }
    double w, h;
    entitySize(e, w, h);
    return e.info->height > 0 ? e.info->eye * h / e.info->height : e.info->eye;
}
float wrapDeg(float d) { return d - 360.0f * std::floor((d + 180.0f) / 360.0f); }  // Mth.wrapDegrees
bool isBoat(const std::string& n) { return n.ends_with("_boat") || n.ends_with("_raft"); }
// Leashable.createQuadLeashOffsets
void quadOffsets(double w, double h, double front, double frontBack, double leftRight, double height, V3 out[4]) {
    const double f = front * w, fb = frontBack * w, lr = leftRight * w, hh = height * h;
    out[0] = {-lr, hh, fb + f}, out[1] = {-lr, hh, -fb + f}, out[2] = {lr, hh, -fb + f}, out[3] = {lr, hh, fb + f};
}
}  // namespace

// Leash ropes (EntityRenderer.extractRenderState leash states, LeashFeatureRenderer): 24 steps from the leashed
// entity's attach point to its holder's rope hold position, sagging, alternately shaded, as two crossed 0.05 ribbons;
// quad ropes (four, taut) between a horse / camel / sniffer / boat and a (happy) ghast. Fishing lines
// (FishingHookRenderer): 16 black segments from the hook to the angler's hand, one window line width wide.
void EntityRenderer::emitRopes(const double cam[3], float pt, float projY) {
    const auto& entities = world_.entities();
    World& world = world_.world();
    auto find = [&](int id) -> const Entity* {
        if (id == localNetId_ && localAvatar_) return localAvatar_;
        auto it = entities.find(id);
        return it == entities.end() ? nullptr : &it->second;
    };
    auto lerpPos = [&](const Entity& e) {
        return V3{e.prevPos[0] + (e.pos[0] - e.prevPos[0]) * pt, e.prevPos[1] + (e.pos[1] - e.prevPos[1]) * pt,
                  e.prevPos[2] + (e.pos[2] - e.prevPos[2]) * pt};
    };
    auto living = [](const Entity& e) { return e.category == Category::Living || e.category == Category::Player; };
    auto bodyRot = [&](const Entity& e) {  // getPreciseBodyRotation (radians)
        const float a = living(e) ? e.prevBodyYaw + wrapDeg(e.bodyYaw - e.prevBodyYaw) * pt : e.prevYaw + wrapDeg(e.yaw - e.prevYaw) * pt;
        return double(a) * kDeg;
    };
    auto lightAt = [&](const V3& p) -> u32 {
        const int x = int(std::floor(p.x)), y = int(std::floor(p.y)), z = int(std::floor(p.z));
        if (y < world.minSectionY() * 16 || y >= (world.maxSectionY() + 1) * 16) return 0xF0;
        return world.light(x >> 4, y >> 4, z >> 4).at(x & 15, y & 15, z & 15);
    };
    const TexRect& white = assets_.rect(0);
    const float uv[4][2] = {{white.u0, white.v0}, {white.u0, white.v0}, {white.u0, white.v0}, {white.u0, white.v0}};
    const float up[3] = {0, 1, 0};
    auto rope = [&](V3 start, V3 end, bool slack, u32 l0, u32 l1) {  // LeashFeatureRenderer.prepare
        const double dx = end.x - start.x, dy = end.y - start.y, dz = end.z - start.z;
        const double hl = std::sqrt(dx * dx + dz * dz);
        const double factor = hl > 1e-6 ? 0.05 / 2.0 / hl : 0.0;
        const double dxOff = dz * factor, dzOff = dx * factor;
        auto point = [&](int k, double fudge, bool second, float out[3]) {
            const double p = k / 24.0;
            double y = slack ? (dy > 0 ? dy * p * p : dy - dy * (1 - p) * (1 - p)) : dy * p;
            const double sx = second ? dxOff : -dxOff, sz = second ? -dzOff : dzOff;
            const double yy = second ? y + 0.05 - fudge : y + fudge;
            out[0] = float(start.x + dx * p + sx - cam[0]), out[1] = float(start.y + yy - cam[1]), out[2] = float(start.z + dz * p + sz - cam[2]);
        };
        for (int pass = 0; pass < 2; ++pass) {  // fudge 0.05 forwards, then 0 backwards: the two diagonals
            const double fudge = pass == 0 ? 0.05 : 0.0;
            for (int k = 0; k < 24; ++k) {
                // The leash pipeline's flat colour: the far pair of each step (k + 1 forwards, k backwards).
                const int kc = pass == 0 ? k + 1 : k;
                const float shade = kc % 2 == (pass == 1 ? 1 : 0) ? 0.7f : 1.0f;
                const u32 color = 0xFF000000u | u32(0.3f * shade * 255.0f) << 16 | u32(0.4f * shade * 255.0f) << 8 | u32(0.5f * shade * 255.0f);
                const double p = kc / 24.0;
                const u32 hi = u32(double((l0 >> 4) & 15) + (double((l1 >> 4) & 15) - double((l0 >> 4) & 15)) * p);
                const u32 lo = u32(double(l0 & 15) + (double(l1 & 15) - double(l0 & 15)) * p);
                float pos[4][3];
                point(k, fudge, false, pos[0]);
                point(k, fudge, true, pos[1]);
                point(k + 1, fudge, true, pos[2]);
                point(k + 1, fudge, false, pos[3]);
                emitFree(pos, uv, up, 0xFFFFFFFF, color, (hi << 4 | lo) | 512, 0);
            }
        }
    };
    for (const auto& [id, e] : entities) {
        if (!e.info) continue;
        if (e.leashHolder != 0) {
            const Entity* holder = find(e.leashHolder);
            if (!holder || !holder->info) continue;
            const std::string& n = e.info->name;
            const std::string& hn = holder->info->name;
            double w, hgt;
            entitySize(e, w, hgt);
            const double eye = eyeHeight(e);
            const V3 pos = lerpPos(e), hpos = lerpPos(*holder);
            const u32 l0 = lightAt(pos + V3{0, eye, 0}), l1 = lightAt(hpos + V3{0, eyeHeight(*holder), 0});
            const double rot = bodyRot(e);
            const bool holderQuad = hn == "minecraft:happy_ghast" || hn == "minecraft:ghast";
            const bool horse = n == "minecraft:horse" || n == "minecraft:donkey" || n == "minecraft:mule" || n == "minecraft:skeleton_horse" ||
                               n == "minecraft:zombie_horse" || n == "minecraft:camel" || n == "minecraft:camel_husk";
            const bool quad = holderQuad && (horse || n == "minecraft:sniffer" || isBoat(n));
            if (quad) {
                V3 a[4], b[4];
                if (n == "minecraft:camel" || n == "minecraft:camel_husk") quadOffsets(w, hgt, 0.02, 0.48, 0.25, 0.82, a);
                else if (n == "minecraft:sniffer") quadOffsets(w, hgt, -0.01, 0.63, 0.38, 1.15, a);
                else if (isBoat(n)) quadOffsets(w, hgt, 0.0, 0.64, 0.382, 0.88, a);
                else if (n == "minecraft:donkey" || n == "minecraft:mule" || n == "minecraft:zombie_horse") quadOffsets(w, hgt, 0.04, 0.41, 0.18, 0.73, a);
                else quadOffsets(w, hgt, 0.04, 0.52, 0.23, 0.87, a);
                double hw, hh;
                entitySize(*holder, hw, hh);
                if (hn == "minecraft:happy_ghast") quadOffsets(hw, hh, -0.03125, 0.4375, 0.46875, 0.03125, b);
                else quadOffsets(hw, hh, 0.0, 0.5, 0.5, 0.0, b);
                const double hrot = bodyRot(*holder);
                for (int i = 0; i < 4; ++i) rope(pos + yRot(a[i], -rot), hpos + yRot(b[i], -hrot), false, l0, l1);
                continue;
            }
            // Leashable.getLeashOffset (by type)
            V3 off{0, eye, w * 0.4};
            if (n == "minecraft:allay") off = {0, eye * 0.6, w * 0.1};
            else if (n == "minecraft:bee") off = {0, 0.5 * eye, w * 0.2};
            else if (n == "minecraft:wolf" || n == "minecraft:pig" || n == "minecraft:rabbit" || n == "minecraft:strider") off = {0, 0.6 * eye, w * 0.4};
            else if (n == "minecraft:fox") off = {0, 0.55 * eye, w * 0.4};
            else if (n == "minecraft:ocelot" || n == "minecraft:parrot") off = {0, 0.5 * eye, w * 0.4};
            else if (n == "minecraft:snow_golem") off = {0, 0.75 * eye, w * 0.4};
            else if (n == "minecraft:iron_golem") off = {0, 0.875 * eye, w * 0.4};
            else if (n == "minecraft:copper_golem") off = {0, 0.75 * eye, 0};
            else if (n == "minecraft:llama" || n == "minecraft:trader_llama") off = {0, 0.75 * eye, w * 0.5};
            else if (n == "minecraft:happy_ghast") off = {0, 0, 0};
            else if (n == "minecraft:sulfur_cube") off = {0, hgt / 2.0, 0};
            else if (isBoat(n)) off = {0, 0.88 * hgt, 0.64 * w};
            else if (n == "minecraft:camel" || n == "minecraft:camel_husk") {  // getBodyAnchorAnimationYOffset (settled poses)
                const double scale = e.baby ? 0.6 : 1.0;
                const bool sitting = e.pose == 10;  // Pose.SITTING
                const double dimsH = e.info->height * scale - (sitting ? 1.43 * scale : 0.0);
                off = {0, dimsH - (e.baby ? 0.09375 : 0.375) + (sitting ? 0.2 * scale : 0.0) - 0.2 * scale, e.info->width * scale * 0.56};
            }
            const V3 rotated = yRot(off, -rot);
            // getRopeHoldPosition (by holder)
            V3 end = hpos + V3{0, eyeHeight(*holder) * 0.7, 0};
            if (hn == "minecraft:leash_knot") {
                end = hpos + V3{0, 0.2, 0};
            } else if (hn == "minecraft:villager" || hn == "minecraft:wandering_trader") {
                double hw, hh;
                entitySize(*holder, hw, hh);
                end = hpos + yRot(V3{0, hh - 1.0, 0.2}, -bodyRot(*holder));
            } else if (holder->category == Category::Player) {
                const double side = holder->mainArm != 0 ? -1.0 : 1.0;  // right handed: -1
                const double xr = double(holder->pitch + (holder->prevPitch - holder->pitch) * pt * 0.5f) * kDeg;
                if (holder == localAvatar_ && localFirstPerson_) {  // LocalPlayer: from the eyes, the hand's offset
                    const double yr = double(holder->yaw + (holder->prevYaw - holder->yaw) * pt * 0.5f) * kDeg;
                    end = xRot(V3{0.39 * side, -0.6, 0.3}, -xr);
                    end = yRot(end, -yr) + hpos + V3{0, eyeHeight(*holder), 0};
                } else if (holder->pose == 3) {  // isVisuallySwimming
                    end = hpos + yRot(xRot(V3{0.22 * side, 0.2, -0.15}, -xr), -bodyRot(*holder));
                } else {
                    double hw, hh;
                    entitySize(*holder, hw, hh);
                    end = hpos + yRot(V3{0.22 * side, hh - 1.0, holder->pose == 5 ? -0.2 : 0.07}, -bodyRot(*holder));
                }
            }
            rope(pos + rotated, end, true, l0, l1);
        }
    }
    // Fishing lines: the hook's owner (its data) and the hand holding the rod.
    static const int bobber = assets_.typeId("minecraft:fishing_bobber");
    const float lineWidth = std::max(2.5f, float(renderer_ ? renderer_->width() : 1920) / 1920.0f * 2.5f);
    const float screenH = float(renderer_ ? std::max(1u, renderer_->height()) : 1080u);
    for (const auto& [id, e] : entities) {
        if (e.type != bobber) continue;
        const Entity* owner = find(e.direction);
        if (!owner || owner->category != Category::Player) continue;
        const bool rodInMain = items_ && items_->itemName(owner->equipment[0]) == "fishing_rod";
        const bool rightArm = rodInMain ? owner->mainArm != 0 : owner->mainArm == 0;  // getHoldingArm
        const double invert = rightArm ? 1.0 : -1.0;
        const float swingT = owner->swingTime >= 0 ? std::clamp((float(owner->swingTime) + pt) / float(owner->swingDuration), 0.0f, 1.0f) : 0.0f;
        const double swing = std::sin(std::sqrt(swingT) * kPi);
        const V3 opos = lerpPos(*owner), eyePos = opos + V3{0, eyeHeight(*owner), 0};
        V3 hand;
        if (owner == localAvatar_ && localFirstPerson_) {
            // camera.getNearPlane(fov).getPointOnPlane(invert * 0.525, -0.1) * 960 / fov, swung
            const double fovR = double(fovOption_) * kDeg;
            const double aspect = renderer_ ? double(renderer_->width()) / double(std::max(1u, renderer_->height())) : 16.0 / 9.0;
            const double planeH = std::tan(fovR / 2.0) * 0.05, planeW = planeH * aspect;
            const double yaw = double(owner->prevYaw + (owner->yaw - owner->prevYaw) * pt) * kDeg;
            const double pitch = double(owner->prevPitch + (owner->pitch - owner->prevPitch) * pt) * kDeg;
            const V3 fwd{-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
            const V3 upv{-std::sin(yaw) * std::sin(pitch), std::cos(pitch), std::cos(yaw) * std::sin(pitch)};
            const V3 left{std::cos(yaw), 0, std::sin(yaw)};
            V3 p = fwd * 0.05 + upv * (planeH * -0.1) - left * (planeW * invert * 0.525);
            p = p * (960.0 / double(fovOption_));
            p = xRot(yRot(p, swing * 0.5), -swing * 0.7);
            hand = eyePos + p;
        } else {
            const double r = bodyRot(*owner), s = std::sin(r), c = std::cos(r);
            const double right = invert * 0.35, forward = 0.8;
            hand = eyePos + V3{-c * right - s * forward, (owner->pose == 5 ? -0.1875 : 0.0) - 0.45, -s * right + c * forward};
        }
        const V3 hook = lerpPos(e) + V3{0, 0.25, 0};
        const V3 d = hand - hook;
        auto at = [&](double t) { return hook + V3{d.x * t, d.y * (t * t + t) * 0.5, d.z * t}; };  // stringVertex
        for (int i = 0; i < 16; ++i) {
            const V3 a = at(i / 16.0), b = at((i + 1) / 16.0);
            // A camera-facing strip one line width wide (the lines pipeline), black and unlit.
            auto rel = [&](const V3& v) { return V3{v.x - cam[0], v.y - cam[1], v.z - cam[2]}; };
            const V3 ra = rel(a), rb = rel(b), seg = rb - ra;
            const V3 mid = (ra + rb) * 0.5;
            V3 side{seg.y * mid.z - seg.z * mid.y, seg.z * mid.x - seg.x * mid.z, seg.x * mid.y - seg.y * mid.x};
            const double sl = std::sqrt(side.x * side.x + side.y * side.y + side.z * side.z);
            if (sl < 1e-9) continue;
            auto half = [&](const V3& v) {  // half the line width in world units at that distance
                return double(lineWidth) * 0.5 * std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) * 2.0 / (double(screenH) * double(projY));
            };
            const V3 sa = side * (half(ra) / sl), sb = side * (half(rb) / sl);
            const V3 q[4] = {ra - sa, ra + sa, rb + sb, rb - sb};
            float pos[4][3];
            for (int k = 0; k < 4; ++k) pos[k][0] = float(q[k].x), pos[k][1] = float(q[k].y), pos[k][2] = float(q[k].z);
            emitFree(pos, uv, up, 0xFFFFFFFF, 0xFF000000, 256, 0);
        }
    }
}

// EntityRenderer.extractShadow / ShadowFeatureRenderer: misc/shadow on the tops of the full blocks under each drawn
// entity, fading with the distance to the camera (16 blocks), the depth below the entity and the light there.
void EntityRenderer::emitBlobShadows(const double cam[3]) {
    static const int tex = assets_.texture("misc/shadow");
    if (blobs_.empty() || tex < 0) return;
    const TexRect& t = assets_.rect(tex);
    const World& world = world_.world();
    const BlockRegistry& registry = world_.registry();
    const int minY = world.minSectionY() * 16, maxY = (world.maxSectionY() + 1) * 16;
    auto brightness = [&](int level) {  // Lightmap.getBrightness
        const float f = float(level) / 15.0f, g = f / (4.0f - 3.0f * f);
        return g + (1.0f - g) * ambient_;
    };
    static constexpr float kUp[3] = {0, 1, 0};
    for (const BlobShadow& b : blobs_) {
        const double dx = b.pos[0] - cam[0], dy = b.pos[1] - cam[1], dz = b.pos[2] - cam[2];
        const float pow = float((1.0 - (dx * dx + dy * dy + dz * dz) / 256.0) * b.strength);
        if (pow <= 0.0f) continue;
        const float r = b.radius;
        const int x0 = int(std::floor(b.pos[0] - r)), x1 = int(std::floor(b.pos[0] + r));
        const int z0 = int(std::floor(b.pos[2] - r)), z1 = int(std::floor(b.pos[2] + r));
        const float depth = std::min(pow / 0.5f - 1.0f, r);
        const int y0 = int(std::floor(b.pos[1] - depth)), y1 = int(std::floor(b.pos[1]));
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
                for (int y = y0; y <= y1; ++y) {
                    if (y - 1 < minY || y >= maxY) continue;
                    const float power = pow - float(b.pos[1] - y) * 0.5f;
                    const u16 below = world.block(x, y - 1, z);
                    const BlockInfo& info = registry.infos()[below];
                    if (info.kind == KindAir) continue;  // RenderShape.INVISIBLE
                    const std::string& name = registry.name(below);
                    if (name.starts_with("minecraft:barrier") || name.starts_with("minecraft:light[") ||
                        name.starts_with("minecraft:structure_void"))
                        continue;
                    const u8 lit = world.light(x >> 4, y >> 4, z >> 4).at(x & 15, y & 15, z & 15);
                    const int level = std::max(int(lit >> 4) - skyDarken_, int(lit & 15));  // getMaxLocalRawBrightness
                    if (level <= 3) continue;
                    // isCollisionShapeFullBlock, then the outline's bounds.
                    if (info.collisionCount != 1) continue;
                    const Box& cb = registry.box(info.collisionFirst);
                    if (cb.min[0] > 0 || cb.min[1] > 0 || cb.min[2] > 0 || cb.max[0] < 1 || cb.max[1] < 1 || cb.max[2] < 1) continue;
                    float lo[3] = {0, 0, 0}, hi[3] = {1, 1, 1};
                    if (info.selectionCount) {
                        for (int a = 0; a < 3; ++a) lo[a] = 1e9f, hi[a] = -1e9f;
                        for (u32 i = 0; i < info.selectionCount; ++i) {
                            const Box& s = registry.box(info.selectionFirst + i);
                            for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], s.min[a]), hi[a] = std::max(hi[a], s.max[a]);
                        }
                    }
                    const float alpha = std::clamp(power * 0.5f * brightness(level), 0.0f, 1.0f);
                    const float rx = float(x - b.pos[0]), ry = float(y - b.pos[1]), rz = float(z - b.pos[2]);
                    // Clipped to the shadow's square: beyond it the texture is clamped transparent, while the atlas
                    // would show its neighbours.
                    const float x01 = std::max(rx + lo[0], -r), x11 = std::min(rx + hi[0], r), y01 = ry + lo[1];
                    const float z01 = std::max(rz + lo[2], -r), z11 = std::min(rz + hi[2], r);
                    if (x01 >= x11 || z01 >= z11) continue;
                    const float u0 = -x01 / 2.0f / r + 0.5f, u1 = -x11 / 2.0f / r + 0.5f;
                    const float v0 = -z01 / 2.0f / r + 0.5f, v1 = -z11 / 2.0f / r + 0.5f;
                    // Camera-relative corners, lifted a hair above the block top (RenderTypes: view offset layering).
                    const float ex = float(b.pos[0] - cam[0]), ey = float(b.pos[1] - cam[1]) + 0.0015f, ez = float(b.pos[2] - cam[2]);
                    const float pos[4][3] = {{ex + x01, ey + y01, ez + z01}, {ex + x01, ey + y01, ez + z11},
                                             {ex + x11, ey + y01, ez + z11}, {ex + x11, ey + y01, ez + z01}};
                    const float uv[4][2] = {{t.u0 + u0 * t.du, t.v0 + v0 * t.dv}, {t.u0 + u0 * t.du, t.v0 + v1 * t.dv},
                                            {t.u0 + u1 * t.du, t.v0 + v1 * t.dv}, {t.u0 + u1 * t.du, t.v0 + v0 * t.dv}};
                    emitFree(pos, uv, kUp, 0xFFFFFFFF, (u32(alpha * 255.0f) << 24) | 0xFFFFFF, 256, 0);
                }
    }
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

// TntRenderer.getSwellAmount / isLit (fuse: ticks left, partial).
float EntityRenderer::tntSwell(float fuse) {
    float g = std::clamp(1.0f - fuse / 10.0f, 0.0f, 1.0f);
    g *= g, g *= g;
    return g * 0.3f;
}
bool EntityRenderer::tntLit(float fuse) { return fuse >= 0 && int(fuse / 5.0f) % 2 == 0; }

// AbstractMinecartRenderer.submit: the display block (the type's own or DATA_ID_CUSTOM_DISPLAY_BLOCK) at 0.75 scale,
// raised by the display offset; root: the cart's pose before the model's flip. A chest goes in as a model layer.
void EntityRenderer::minecartContents(const Entity& e, float pt, const Mat34& rootIn, u32 light, Layer* layers, int& layerCount) {
    const M34& root = *reinterpret_cast<const M34*>(&rootIn);
    BlockRegistry& reg = world_.registry();
    const std::string& type = e.info ? e.info->name : std::string();
    std::string state;
    int offset = 6;  // AbstractMinecart.getDefaultDisplayOffset
    if (type == "minecraft:chest_minecart") state = "minecraft:chest[facing=north]", offset = 8;
    else if (type == "minecraft:furnace_minecart") state = e.furnaceFuel ? "minecraft:furnace[facing=north,lit=true]" : "minecraft:furnace[facing=north,lit=false]";
    else if (type == "minecraft:tnt_minecart") state = "minecraft:tnt";
    else if (type == "minecraft:hopper_minecart") state = "minecraft:hopper", offset = 1;
    else if (type == "minecraft:spawner_minecart") state = "minecraft:spawner";
    else if (type == "minecraft:command_block_minecart") state = "minecraft:command_block";
    u16 block = 0;
    if (e.displayBlock > 0) block = world_.localBlock(e.displayBlock);
    else if (!state.empty()) {
        auto it = displayStates_.find(state);
        if (it == displayStates_.end()) it = displayStates_.emplace(state, reg.idFor(world_.reports().complete(state))).first;
        block = it->second;
    }
    if (e.displayOffset != INT_MIN) offset = e.displayOffset;
    if (!block) return;
    M34 m = mul(root, scale(0.75f, 0.75f, 0.75f));
    m = mul(m, translate(-0.5f, float(offset - 8) / 16.0f, 0.5f));
    m = mul(m, rotY(90.0f * kDeg));
    u32 overlay = 0;
    if (type == "minecraft:tnt_minecart" && e.displayBlock <= 0) {  // TntMinecartRenderer.submitMinecartContents
        const float fuse = e.fuse > -1 ? float(e.fuse) - pt + 1.0f : -1.0f;
        if (fuse > -1 && fuse < 10) {
            const float swell = tntSwell(fuse);
            m = mul(mul(m, translate(-swell * 0.5f, 0, -swell * 0.5f)), scale(1 + swell, 1 + swell, 1 + swell));
        }
        if (tntLit(fuse)) overlay = 191u << 8;
    }
    const std::string& name = reg.name(block);
    const bool chest = name.starts_with("minecraft:chest") || name.starts_with("minecraft:trapped_chest");
    if (chest && chestModel_ >= 0 && chestTex_ >= 0 && layerCount < 16) {  // ChestRenderer's model, facing as the state
        const std::string_view facing = reg.property(block, "facing");
        const float yRot = facing == "south" ? 0.0f : facing == "west" ? 90.0f : facing == "east" ? 270.0f : 180.0f;
        m = mul(m, translate(0.5f, 0, 0.5f));
        m = mul(mul(m, rotY(-yRot * kDeg)), translate(-0.5f, 0, -0.5f));
        displayRoot_ = *reinterpret_cast<const Mat34*>(&m);
        layers[layerCount++] = {chestModel_, chestTex_, kWhite, u16(DisplayBlock | NoOverlay)};
        return;
    }
    emitBlock(block, *reinterpret_cast<const Mat34*>(&m), light, overlay);
}

void EntityRenderer::freeEntity(const Entity& e, const TypeDef& d, float pt, const double cam[3], u32 light,
                                const float* camRight) {
    double p[3];
    for (int a = 0; a < 3; ++a) p[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt - cam[a];
    BlockRegistry& reg = world_.registry();
    if (d.kind == Kind::Block && d.special == SpDisplay) {  // the block model in block space, under the display's pose
        const u16 block = world_.localBlock(e.blockState);
        M34 m;
        if (!block || !displayMatrix(e, pt, camYaw_, camPitch_, m.m)) return;
        m.m[3] += float(p[0]), m.m[7] += float(p[1]), m.m[11] += float(p[2]);
        emitBlock(block, *reinterpret_cast<const Mat34*>(&m), displayLight(e, light), 0);
        return;
    }
    if (d.kind == Kind::Block) {
        if (!d.block.empty() && !d.blockId) d.blockId = reg.idFor(world_.reports().complete(d.block));
        const u16 block = !d.block.empty() ? d.blockId : world_.localBlock(e.blockState);
        if (!block) return;
        if (d.block.empty() && d.special != SpDisplay) {  // FallingBlockRenderer.shouldRender: not where the same block is
            const World& world = world_.world();
            const int bx = int(std::floor(e.pos[0])), by = int(std::floor(e.pos[1])), bz = int(std::floor(e.pos[2]));
            if (by >= world.minSectionY() * 16 && by < (world.maxSectionY() + 1) * 16 && world.block(bx, by, bz) == block) return;
        }
        M34 m = translate(float(p[0]) - 0.5f, float(p[1]), float(p[2]) - 0.5f);
        u32 overlay = 0;
        if (!d.block.empty()) {  // TntRenderer: swells over its last 10 ticks, flashes white every 5
            const float fuse = float(e.fuse) - pt + 1.0f, s = 1.0f + tntSwell(fuse);
            m = mul(mul(translate(float(p[0]), float(p[1]) + 0.5f, float(p[2])), scale(s, s, s)), translate(-0.5f, -0.5f, -0.5f));
            if (tntLit(fuse)) overlay = 191u << 8;  // OverlayTexture.u(1): 75% white
        }
        emitBlock(block, *reinterpret_cast<const Mat34*>(&m), light, overlay);
        return;
    }
    if (d.special == SpXpOrb) {
        // ExperienceOrbRenderer: 0.1 up, camera-facing, 0.3 x (-0.5 .. 0.5, -0.25 .. 0.75); the icon by value; red and
        // blue cycling with the age over full green, alpha 128 (translucent).
        const TexRect& t = assets_.rect(d.sprite);
        const i64 value = dataInt(e, "DATA_VALUE");
        static constexpr int kIconAt[10] = {3, 7, 17, 37, 73, 149, 307, 617, 1237, 2477};  // ExperienceOrb.getIcon
        int icon = 0;
        while (icon < 10 && value >= kIconAt[icon]) ++icon;
        const float up[3] = {camRight[3], camRight[4], camRight[5]};
        LateFree q;
        const float sx[4] = {-0.5f, 0.5f, 0.5f, -0.5f}, sy[4] = {-0.25f, -0.25f, 0.75f, 0.75f};
        for (int k = 0; k < 4; ++k)
            for (int a = 0; a < 3; ++a)
                q.pos[k][a] = float(p[a]) + (a == 1 ? 0.1f : 0.0f) + (camRight[a] * sx[k] + up[a] * sy[k]) * 0.3f;
        const float u0 = t.u0 + float(icon % 4) * 0.25f * t.du, v0 = t.v0 + float(icon / 4) * 0.25f * t.dv;
        const float u1 = u0 + 0.25f * t.du, v1 = v0 + 0.25f * t.dv;
        const float uv[4][2] = {{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}};
        std::memcpy(q.uv, uv, sizeof(q.uv));
        const float rr = (float(e.age) + pt) / 2.0f;
        const u32 rc = u32((std::sin(rr) + 1.0f) * 0.5f * 255.0f), bc = u32((std::sin(rr + kPi * 4.0f / 3.0f) + 1.0f) * 0.1f * 255.0f);
        q.color = 0x80000000u | bc << 16 | 0xFF00u | rc, q.light = light | 512u;
        std::copy(freeMotion_, freeMotion_ + 3, q.motion);
        const float n[3] = {0, 1, 0};
        if (pack_ || shadowOnly_) emitFree(q.pos, q.uv, n, 0xFFFFFFFF, q.color, q.light, 0);
        else lateFree_.push_back(q);
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
        const float uv[4][2] = {{t.u0, t.v0 + t.dv}, {t.u0 + t.du, t.v0 + t.dv}, {t.u0 + t.du, t.v0}, {t.u0, t.v0}};
        const float n[3] = {0, 1, 0};
        emitFree(pos, uv, n, 0xFFFFFFFF, kWhite, light | 512u, 0);
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

void EntityRenderer::uploadSlots(ID3D12GraphicsCommandList* list, u32 frame) {
    for (int t : assets_.takeDirtySlots())
        if (std::find(pendingSlots_.begin(), pendingSlots_.end(), t) == pendingSlots_.end()) pendingSlots_.push_back(t);
    if (pendingSlots_.empty() || !atlas_) return;
    constexpr u32 kPerFrame = 8, kSlotBytes = 64 * 256;  // 64 rows of 64 texels (256-byte pitch)
    if (!slotUpload_) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = u64(kSlotBytes) * kPerFrame * Renderer::kFrames;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RANGE none{0, 0};
        if (!check(renderer_->device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                IID_PPV_ARGS(&slotUpload_)),
                   "CreateCommittedResource(slots)") ||
            !check(slotUpload_->Map(0, &none, reinterpret_cast<void**>(&slotMapped_)), "Map")) {
            slotUpload_.Reset();
            pendingSlots_.clear();
            return;
        }
    }
    auto barrier = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = atlas_.Get();
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = from, b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
    };
    barrier(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    const u32 n = std::min<u32>(u32(pendingSlots_.size()), kPerFrame);
    const std::vector<u8>& cpu = assets_.atlas();
    for (u32 i = 0; i < n; ++i) {
        u32 x, y;
        assets_.slotOrigin(pendingSlots_[i], x, y);
        const u64 offset = (u64(frame) * kPerFrame + i) * kSlotBytes;
        for (u32 row = 0; row < 64; ++row)
            std::memcpy(slotMapped_ + offset + row * 256, &cpu[(size_t(y + row) * assets_.atlasWidth() + x) * 4], 256);
        D3D12_TEXTURE_COPY_LOCATION dst{atlas_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{slotUpload_.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        src.PlacedFootprint = {offset, {DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256}};
        list->CopyTextureRegion(&dst, x, y, 0, &src, nullptr);
    }
    barrier(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pendingSlots_.erase(pendingSlots_.begin(), pendingSlots_.begin() + n);
}

void EntityRenderer::build(const PassContext& ctx) {
    Timer timer;
    uploadSlots(ctx.list, ctx.frame);
    const auto& entities = world_.entities();
    stats_ = {};
    stats_.entities = u32(entities.size());
    attachments_.clear();
    ridden_.clear();
    for (const auto& [id, en] : entities)
        if (en.vehicle >= 0) ridden_.push_back(en.vehicle);
    if (world_.localControl() >= 0) ridden_.push_back(world_.localControl());
    casters_.clear();
    glowing_.clear();
    built_ = false;
    if ((entities.empty() && !localPlayer_ && !localShadow_) || !mapped_) return;
    frameBase_ = mapped_ + u64(ctx.frame) * kFrameBytes;
    matCount_ = instCount_ = refCount_ = freeCount_ = 0;
    lateFree_.clear();
    pack_ = ctx.pack;
    translucent_.clear();
    additive_.clear();
    glint_.clear();
    mask_.clear();
    blobs_.clear();
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
    {  // camera rotation from its forward direction (the projection's w row)
        float f[3], l = 0;
        for (int c = 0; c < 3; ++c) f[c] = vp(3, c), l += f[c] * f[c];
        l = std::sqrt(std::max(l, 1e-12f));
        camYaw_ = std::atan2(-f[0], f[2]) / kDeg, camPitch_ = std::asin(std::clamp(-f[1] / l, -1.0f, 1.0f)) / kDeg;
    }
    const double cam[3] = {ctx.camX, ctx.camY, ctx.camZ};
    const float pt = std::clamp(world_.partialTick(), 0.0f, 1.0f);
    const float maxDist = ctx.fogEnd;
    const u32 shadowMask = pack_ || !ctx.shadowMats ? 0u : ctx.shadowMask;
    World& world = world_.world();
    std::shared_lock lock(world.mutex());
    // 0: not drawn, 1: in view, 2: only casts a shadow into the view.
    auto classify = [&](const Entity& e, float sphere[4]) -> int {
        if (e.type < 0 || size_t(e.type) >= defs_.size()) return 0;
        const TypeDef& d = defs_[size_t(e.type)];
        if (d.kind == Kind::None || !e.info) return 0;
        if (d.kind == Kind::Item && externalItems_) return 0;  // 3D item models: the visuals pass
        // invisible: still drawn (equipment, eyes; the body for spectators / teammates), see modelEntity
        float w = e.info->width, h = e.info->height;
        if (d.special == SpSlime || d.special == SpSulfurCube) w *= float(e.size), h *= float(e.size);
        sphere[3] = std::max(w, h) * 0.75f * d.scale + 0.5f;
        if (d.special == SpDisplay) {  // bounds of the transformed block (rotations keep lengths)
            const DisplayState::Xform& x = e.display.to;
            const float s = std::max({std::abs(x.s[0]), std::abs(x.s[1]), std::abs(x.s[2])});
            sphere[3] = std::sqrt(x.t[0] * x.t[0] + x.t[1] * x.t[1] + x.t[2] * x.t[2]) + s * 1.75f + 0.5f;
        }
        for (int a = 0; a < 3; ++a) sphere[a] = float(e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt - cam[a]);
        sphere[1] += h * 0.5f;
        const float dist = std::sqrt(sphere[0] * sphere[0] + sphere[1] * sphere[1] + sphere[2] * sphere[2]);
        if (dist - sphere[3] > maxDist) return 0;
        if (d.special == SpDisplay) {
            if (dist > e.display.viewRange * 64.0f) return 0;  // Display.shouldRenderAtSqrDistance
        } else if (e.id != world_.viewEntity) {
            // Entity.shouldRenderAtSqrDistance: the bounding box's mean size x 64 x the view scale.
            const float size = (2.0f * w + h) / 3.0f * 64.0f * viewScale_;
            const float dx = float(e.pos[0] - cam[0]), dy = float(e.pos[1] - cam[1]), dz = float(e.pos[2] - cam[2]);
            if (dx * dx + dy * dy + dz * dz >= size * size) return 0;
        }
        if (&e != localShadow_ && e.id != world_.viewEntity) {
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
        if (d.special == SpSlime || d.special == SpSulfurCube) w *= float(e.size), h *= float(e.size);
        const float cx = float(c[0]), cz = float(c[2]);
        // Light at the entity's eye position (EntityRenderer samples getLightProbePosition = the eyes).
        const int lx = int(std::floor(e.pos[0])), ly = int(std::floor(e.pos[1] + e.info->eye)),
                  lz = int(std::floor(e.pos[2]));
        u32 light = 0xF0;
        if (ly >= world.minSectionY() * 16 && ly < (world.maxSectionY() + 1) * 16)
            light = world.light(lx >> 4, ly >> 4, lz >> 4).at(lx & 15, ly & 15, lz & 15);
        if (d.blockLight) {  // getBlockLightLevel overrides
            int block = int(light & 15);
            if (d.blockLight == 1) block = 15;
            else if (d.blockLight == 2) block = std::min(block + 7, 15);
            else {  // GlowSquidRenderer: lit while it is not darkened
                const int glow = int(std::clamp(1.0f - float(dataInt(e, "DATA_DARK_TICKS_REMAINING")) / 10.0f, 0.0f, 1.0f) * 15.0f);
                block = glow == 15 ? 15 : std::max(glow, block);
            }
            light = (light & ~15u) | u32(block);
        }
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
        if (drawn && !shadowOnly_ && !pack_ && blobShadows_ && !(e.sharedFlags & 0x20)) {
            // getShadowRadius: slimes by size, mobs by their scale and age (babies half), items and orbs weaker.
            const std::string& id = e.info->name;
            float radius = shadowRadiusOf(id), strength = 1.0f;
            if (d.special == SpSlime || d.special == SpSulfurCube) radius = 0.25f * float(e.size);
            else if (e.baby) radius *= 0.5f;
            if (id == "minecraft:item" || id == "minecraft:experience_orb") strength = 0.75f;
            if (e.display.is) displayShadow(e, pt, radius, strength);
            if (radius > 0)
                blobs_.push_back({{c[0] + cam[0], c[1] + cam[1], c[2] + cam[2]}, std::min(radius, 32.0f), strength});
        }
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
        if (!shadowOnly_ && outlineColor && (k.refN || k.trN || k.freeN))
            if (const int color = outlineColor(e); color >= 0) glowing_.push_back({k.ref, k.refN, k.tr, k.trN, k.free, k.freeN, u32(color)});
    };
    // Entities in view first, then those that only cast a shadow into it (their ranges follow the view's).
    static thread_local std::vector<std::pair<const Entity*, std::array<float, 4>>> shadowOnly;
    shadowOnly.clear();
    shadowOnly_ = false;
    auto visit = [&](const Entity& e) {
        float sphere[4];
        const int k = classify(e, sphere);
        if (k == 0 && e.display.is && !pack_ && blobShadows_ && !(e.sharedFlags & 0x20)) {  // item / text displays' shadows
            float radius, strength;
            displayShadow(e, pt, radius, strength);
            double p[3];
            for (int a = 0; a < 3; ++a) p[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt;
            const double dx = p[0] - cam[0], dy = p[1] - cam[1], dz = p[2] - cam[2];
            if (radius > 0 && dx * dx + dy * dy + dz * dz <= double(e.display.viewRange) * 64.0 * double(e.display.viewRange) * 64.0)
                blobs_.push_back({{p[0], p[1], p[2]}, std::min(radius, 32.0f), strength});
        }
        if (k == 1) drawEntity(e, sphere);
        else if (k == 2) shadowOnly.push_back({&e, {sphere[0], sphere[1], sphere[2], sphere[3]}});
    };
    for (const auto& [id, e] : entities) visit(e);
    if (localPlayer_) visit(*localPlayer_);
    if (localShadow_ && !pack_) visit(*localShadow_);
    {  // transient blocks (a falling block landed): until the section's new mesh is in, at most a second
        auto& list = world_.transientBlocks;
        const double now = nowSeconds();
        std::erase_if(list, [&](const EntityWorld::TransientBlock& t) {
            return now > t.until || (world_.sectionMeshSerial && world_.sectionMeshSerial(t.x >> 4, t.y >> 4, t.z >> 4) > t.serial);
        });
        for (const EntityWorld::TransientBlock& t : list) {
            const u16 block = world_.localBlock(t.state);
            if (!block) continue;
            u32 light = 0xF0;
            if (t.y >= world.minSectionY() * 16 && t.y < (world.maxSectionY() + 1) * 16)
                light = world.light(t.x >> 4, t.y >> 4, t.z >> 4).at(t.x & 15, t.y & 15, t.z & 15);
            M34 m = translate(float(t.x - cam[0]), float(t.y - cam[1]), float(t.z - cam[2]));
            emitBlock(block, *reinterpret_cast<const Mat34*>(&m), light, 0);
        }
    }
    // ElderGuardianParticleGroup: the elder guardian in front of the camera, tilting down and fading in and out over 30
    // ticks (full bright, translucent; the camera's rotation, then 60 - 150 x age degrees about x).
    {
        auto& starts = world_.mobAppearances;
        const double now = nowSeconds();
        std::erase_if(starts, [&](double t) { return now - t > 1.5; });
        static const int elder = assets_.typeId("minecraft:elder_guardian");
        if (!starts.empty() && !shadowOnly_ && !pack_ && elder >= 0 && size_t(elder) < defs_.size() && defs_[size_t(elder)].kind == Kind::Mob) {
            static Entity fake;
            fake = Entity{};
            fake.id = INT_MIN + 7, fake.type = elder, fake.info = &assets_.types()[size_t(elder)], fake.category = Category::Living;
            for (int a = 0; a < 3; ++a) fake.pos[a] = fake.prevPos[a] = cam[a];
            fake.health = 1;
            for (double t : starts) {
                const float age = std::clamp(float((now - t) * 20.0) / 30.0f, 0.0f, 1.0f);
                M34 m = mul(rotY((180.0f - camYaw_) * kDeg), rotX(-camPitch_ * kDeg));
                m = mul(m, rotX((60.0f - 150.0f * age) * kDeg));
                m = mul(m, scale(0.42553192f, -0.42553192f, -0.42553192f));
                m = mul(m, translate(0.0f, -0.56f, 3.5f));
                const Mat34 root = *reinterpret_cast<const Mat34*>(&m);
                rootOverride_ = &root;
                appearanceAlpha_ = 0.05f + 0.5f * std::sin(age * kPi);
                modelEntity(fake, defs_[size_t(elder)], pt, cam, 0xFF);
                rootOverride_ = nullptr;
                appearanceAlpha_ = -1;
            }
        }
    }
    if (!shadowOnly_) {  // the projection's y scale (1 / tan(fov / 2)): the fishing line's pixel width
        float l = 0;
        for (int c = 0; c < 3; ++c) l += vp(1, c) * vp(1, c);
        emitRopes(cam, pt, std::sqrt(l));
    }
    camCutout_ = refCount_, camFree_ = freeCount_, camTranslucent_ = u32(translucent_.size() / 2);
    blobFirst_ = freeCount_;
    if (!pack_) emitBlobShadows(cam);
    for (const LateFree& q : lateFree_) {
        static constexpr float kUp[3] = {0, 1, 0};
        std::copy(q.motion, q.motion + 3, freeMotion_);
        emitFree(q.pos, q.uv, kUp, 0xFFFFFFFF, q.color, q.light, 0);
    }
    lateFree_.clear();
    blobCount_ = freeCount_ - blobFirst_;
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
    additiveRefs_ = refCount_;
    const u32 additives = std::min<u32>(u32(additive_.size() / 2), kMaxRefs - refCount_);
    std::memcpy(refs + u64(refCount_) * 2, additive_.data(), u64(additives) * 8);
    refCount_ += additives;
    glintRefs_ = refCount_;
    const u32 glints = std::min<u32>(u32(glint_.size() / 2), kMaxRefs - refCount_);
    std::memcpy(refs + u64(refCount_) * 2, glint_.data(), u64(glints) * 8);
    refCount_ += glints;
    maskRefs_ = refCount_;
    const u32 masks = std::min<u32>(u32(mask_.size() / 2), kMaxRefs - refCount_);
    std::memcpy(refs + u64(refCount_) * 2, mask_.data(), u64(masks) * 8);
    refCount_ += masks;

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
    static_assert(sizeof(Constants) <= kOutlineConstOffset);
    std::memcpy(k.lightmap, ctx.lightmap, sizeof(k.lightmap));
    std::memcpy(k.viewProj, ctx.viewProj, sizeof(k.viewProj));
    std::memcpy(k.fogColor, ctx.fogColor, sizeof(k.fogColor));
    k.fogParams[0] = ctx.fogStart, k.fogParams[1] = ctx.fogEnd, k.fogParams[2] = ctx.daylight;
    k.fogParams[3] = ctx.shading ? 1.0f : 0.0f;  // shader pipeline: output for its relighting (entity.hlsl)
    const float l0[3] = {0.2f, 1.0f, -0.7f}, l1[3] = {-0.2f, 1.0f, 0.7f};
    const float n0 = 1.0f / std::sqrt(0.04f + 1.0f + 0.49f);
    for (int a = 0; a < 3; ++a) k.light0[a] = l0[a] * n0, k.light1[a] = l1[a] * n0;
    static const int glintTex = assets_.texture("misc/enchanted_glint_armor");
    if (glintTex >= 0) {  // TextureTransform.setupGlintTexturing (the Glint Speed / Strength)
        const TexRect& g = assets_.rect(glintTex);
        k.glintRect[0] = g.u0, k.glintRect[1] = g.v0, k.glintRect[2] = g.du, k.glintRect[3] = g.dv;
        const double ms = nowSeconds() * 1000.0 * 8.0 * glintOptions().speed;
        k.glint[0] = -float(std::fmod(ms, 110000.0) / 110000.0), k.glint[1] = float(std::fmod(ms, 30000.0) / 30000.0);
        k.glint[2] = glintOptions().strength;
    }
    if (ctx.prevViewProj) std::memcpy(k.prevViewProj, ctx.prevViewProj, sizeof(k.prevViewProj));
    std::memcpy(k.motion, ctx.motionScale, sizeof(k.motion));
    if (ctx.shadowMats) std::memcpy(k.shadowMat, ctx.shadowMats, sizeof(k.shadowMat));
    std::memcpy(frameBase_, &k, sizeof(k));
    if (!glowing_.empty()) {  // the outline pass draws after TAA / upscaling: without the jitter
        if (ctx.viewProjNoJitter) std::memcpy(k.viewProj, ctx.viewProjNoJitter, sizeof(k.viewProj));
        std::memcpy(frameBase_ + kOutlineConstOffset, &k, sizeof(k));
    }

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
    drawRange((mv ? psoWaterMaskMv_ : psoWaterMask_).Get(), maskRefs_, refCount_ - maskRefs_);
    drawRange((mv ? psoBlobMv_ : psoBlob_).Get(), blobFirst_, blobCount_);
    drawRange((mv ? psoTranslucentMv_ : psoTranslucent_).Get(), cutoutRefs_, camTranslucent_);
    drawRange((mv ? psoAdditiveMv_ : psoAdditive_).Get(), additiveRefs_, glintRefs_ - additiveRefs_);
    if (assets_.texture("misc/enchanted_glint_armor") >= 0)
        drawRange((mv ? psoGlintMv_ : psoGlint_).Get(), glintRefs_, maskRefs_ - glintRefs_);
    if (mv) list->OMSetRenderTargets(1, &ctx.colorRtv, FALSE, &ctx.dsv);
}

bool EntityRenderer::rtEntities(u32 frame, RtEntities& out) {
    if (!quads_ || !upload_ || (!cutoutRefs_ && !freeCount_)) return false;
    const u64 base = u64(frame) * kFrameBytes;
    out = {quads_.Get(), upload_.Get(), base + kMatOffset, base + kInstOffset, base + kRefOffset, base + kFreeOffset,
           cutoutRefs_, freeCount_};
    return true;
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
