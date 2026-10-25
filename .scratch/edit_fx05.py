p = 'src/fx/particles.cpp'
s = open(p, encoding='utf-8').read()
def rep(a, b):
    global s
    assert a in s, a[:120]
    s = s.replace(a, b, 1)

# ---- kinds and option types
rep("""    Suspended, HappyVillager, Lava, Dust, DustTransition, Note, Cloud, Snowflake, Item, Vibration
};""", """    Suspended, HappyVillager, Lava, Dust, DustTransition, Note, Cloud, Snowflake, Item, Vibration, Drip, Spark, Flash,
    Totem, EndRod, Gust, GustSeed, Rising, SculkCharge, SculkChargePop, Shriek, Firefly, Glow
};""")
rep("""    {"item_slime", Kind::Item}, {"item_snowball", Kind::Item}, {"item_cobweb", Kind::Item}, {"vibration", Kind::Vibration}};""",
"""    {"item_slime", Kind::Item}, {"item_snowball", Kind::Item}, {"item_cobweb", Kind::Item}, {"vibration", Kind::Vibration},
    {"firework", Kind::Spark}, {"flash", Kind::Flash}, {"totem_of_undying", Kind::Totem}, {"end_rod", Kind::EndRod},
    {"gust", Kind::Gust}, {"small_gust", Kind::Gust}, {"gust_emitter_large", Kind::GustSeed},
    {"gust_emitter_small", Kind::GustSeed}, {"sonic_boom", Kind::Explosion}, {"soul", Kind::Rising},
    {"sculk_soul", Kind::Rising}, {"sculk_charge", Kind::SculkCharge}, {"sculk_charge_pop", Kind::SculkChargePop},
    {"shriek", Kind::Shriek}, {"firefly", Kind::Firefly}, {"glow", Kind::Glow}, {"wax_on", Kind::Glow},
    {"wax_off", Kind::Glow}, {"electric_spark", Kind::Glow}, {"scrape", Kind::Glow}};

// DripParticle and its providers: the hanging drop falls into the next type, which lands as another one.
enum DripStage : u8 { DripHang, DripCoolingHang, DripFall, DripFallLand, DripDripstoneFallLand, DripHoneyFallLand, DripLand };
struct DripDef {
    const char* name;
    DripStage stage;
    u8 fluid;  // 0 none, 1 water, 2 lava: the drop ends inside that fluid
    float r, g, b, gravity;
    float life;  // > 0: lifetime; < 0: (int)(-life / (random * 0.8 + 0.2))
    bool glow;
    const char* next;  // the falling / landing particle
};
constexpr float kTear[3] = {0.51171875f, 0.03125f, 0.890625f};
const DripDef kDrips[] = {
    {"dripping_water", DripHang, 1, 0.2f, 0.3f, 1.0f, 0.06f * 0.02f, 40, false, "falling_water"},
    {"falling_water", DripFallLand, 1, 0.2f, 0.3f, 1.0f, 0.06f, -64, false, "splash"},
    {"dripping_lava", DripCoolingHang, 2, 1, 1, 1, 0.06f * 0.02f, 40, false, "falling_lava"},
    {"falling_lava", DripFallLand, 2, 1.0f, 0.2857143f, 0.083333336f, 0.06f, -64, false, "landing_lava"},
    {"landing_lava", DripLand, 2, 1.0f, 0.2857143f, 0.083333336f, 0.06f, -16, false, nullptr},
    {"dripping_honey", DripHang, 0, 0.622f, 0.508f, 0.082f, 0.06f * 0.02f * 0.01f, 100, false, "falling_honey"},
    {"falling_honey", DripHoneyFallLand, 0, 0.582f, 0.448f, 0.082f, 0.01f, -64, false, "landing_honey"},
    {"landing_honey", DripLand, 0, 0.522f, 0.408f, 0.082f, 0.06f, -128, false, nullptr},
    {"falling_nectar", DripFall, 0, 0.92f, 0.782f, 0.72f, 0.007f, -16, false, nullptr},
    {"dripping_obsidian_tear", DripHang, 0, kTear[0], kTear[1], kTear[2], 0.06f * 0.02f * 0.01f, 100, true, "falling_obsidian_tear"},
    {"falling_obsidian_tear", DripFallLand, 0, kTear[0], kTear[1], kTear[2], 0.01f, -64, true, "landing_obsidian_tear"},
    {"landing_obsidian_tear", DripLand, 0, kTear[0], kTear[1], kTear[2], 0.06f, -28, true, nullptr},
    {"falling_spore_blossom", DripFall, 0, 0.32f, 0.5f, 0.22f, 0.005f, -64, false, nullptr},
    {"dripping_dripstone_water", DripHang, 1, 0.2f, 0.3f, 1.0f, 0.06f * 0.02f, 40, false, "falling_dripstone_water"},
    {"falling_dripstone_water", DripDripstoneFallLand, 1, 0.2f, 0.3f, 1.0f, 0.06f, -64, false, "splash"},
    {"dripping_dripstone_lava", DripCoolingHang, 2, 1, 1, 1, 0.06f * 0.02f, 40, false, "falling_dripstone_lava"},
    {"falling_dripstone_lava", DripDripstoneFallLand, 2, 1.0f, 0.2857143f, 0.083333336f, 0.06f, -64, false, "landing_lava"},
};

// FireflyParticle.getFadeAmount.
float fadeAmount(float progress, float fadeIn, float fadeOut) {
    if (progress >= 1.0f - fadeIn) return (1.0f - progress) / fadeIn;
    return progress <= fadeOut ? progress / fadeOut : 1.0f;
}""")
rep("""struct Particles::Type {
    std::string name;
    Kind kind = Kind::None;
    Opt opt = Opt::None;
    std::vector<int> sprites;  // item atlas rects
};""", """struct Particles::Type {
    std::string name;
    Kind kind = Kind::None;
    Opt opt = Opt::None;
    std::vector<int> sprites;  // item atlas rects
    int drip = -1;             // kDrips index
    int next = -2;             // drips: the particle type they turn into (-2 unresolved)
};""")
rep("""        for (auto [n, o] : kOptions)
            if (t.name == n) t.opt = o;
    }""", """        for (auto [n, o] : kOptions)
            if (t.name == n) t.opt = o;
        for (size_t d = 0; d < std::size(kDrips); ++d)
            if (t.name == kDrips[d].name) t.kind = Kind::Drip, t.drip = int(d);
    }""")
rep("""        case Opt::Float: r.f32(); break;
        case Opt::VarInt: r.varInt(); break;""", """        case Opt::Float: o.scale = r.f32(); break;  // sculk_charge: the roll
        case Opt::VarInt: o.delay = r.varInt(); break;  // shriek: the delay""")
open(p, 'w', encoding='utf-8').write(s)

p2 = 'src/fx/particles.h'
h = open(p2, encoding='utf-8').read()
a = """        u32 color2 = 0;       // dust_color_transition: the colour it fades to (ARGB)"""
assert a in h
h = h.replace(a, """        u32 color2 = 0;       // dust_color_transition: the colour it fades to (ARGB)
        int delay = 0;        // shriek""")
a = """    float scale = 1, power = 1;"""
assert a in h
h = h.replace(a, """    float scale = 1, power = 1;  // scale: dust size, sculk_charge roll""")
open(p2, 'w', encoding='utf-8').write(h)
print('ok')
