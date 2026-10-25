p = 'src/fx/particles.cpp'
s = open(p, encoding='utf-8').read()
def rep(a, b):
    global s
    assert a in s, a[:120]
    s = s.replace(a, b, 1)

rep("""    static int ids[8] = {-2};
    if (ids[0] == -2) {
        const char* names[8] = {"flame", "smoke", "soul_fire_flame", "tinted_leaves", "cherry_leaves", "pale_oak_leaves",
                                "lava", "campfire_cosy_smoke"};
        for (int i = 0; i < 8; ++i) ids[i] = typeId(names[i]);
    }
    enum Amb : u8 { AmbNone, AmbTorch, AmbWallTorch, AmbSoulTorch, AmbSoulWallTorch, AmbTinted, AmbCherry, AmbPale, AmbLava,
                    AmbCampfire, AmbFurnace };""", """    static int ids[10] = {-2};
    if (ids[0] == -2) {
        const char* names[10] = {"flame", "smoke", "soul_fire_flame", "tinted_leaves", "cherry_leaves", "pale_oak_leaves",
                                 "lava", "campfire_cosy_smoke", "dripping_water", "dripping_lava"};
        for (int i = 0; i < 10; ++i) ids[i] = typeId(names[i]);
    }
    enum Amb : u8 { AmbNone, AmbTorch, AmbWallTorch, AmbSoulTorch, AmbSoulWallTorch, AmbTinted, AmbCherry, AmbPale, AmbLava,
                    AmbCampfire, AmbFurnace, AmbWater };""")
rep("""            else if (n.starts_with("minecraft:lava")) k = AmbLava;""", """            else if (n.starts_with("minecraft:lava")) k = AmbLava;
            else if (n.starts_with("minecraft:water") || n.starts_with("minecraft:bubble_column") || registry_.infos()[id].waterlogged)
                k = AmbWater;""")
rep("""                case AmbLava:
                    if (rng_() % 100 == 0 && blockAt(x, y + 1, z) == 0)
                        add(ids[6], x + double(randf()), y + 1.0, z + double(randf()), 0, 0, 0);
                    break;""", """                case AmbLava:
                    if (rng_() % 100 == 0 && blockAt(x, y + 1, z) == 0)
                        add(ids[6], x + double(randf()), y + 1.0, z + double(randf()), 0, 0, 0);
                    if (rng_() % 10 == 0) fluidDrip(id, x, y, z, ids[9]);
                    break;
                case AmbWater:
                    if (rng_() % 10 == 0) fluidDrip(id, x, y, z, ids[8]);
                    break;""")
rep("""void Particles::weather(const double cam[3]) {""", """// ClientLevel.trySpawnDripParticles: a fluid drips through the ceiling block below it into the air under that.
void Particles::fluidDrip(u16 fluidBlock, int x, int y, int z, int drip) {
    auto shape = [&](u16 id, Box& out) {  // the collision shape's bounds; false: empty
        const BlockInfo& b = registry_.infos()[id];
        if (!b.collisionCount) return false;
        out = {{1, 1, 1}, {0, 0, 0}};
        for (u32 i = 0; i < b.collisionCount; ++i) {
            const Box& c = registry_.box(b.collisionFirst + i);
            for (int a = 0; a < 3; ++a) out.min[a] = std::min(out.min[a], c.min[a]), out.max[a] = std::max(out.max[a], c.max[a]);
        }
        return true;
    };
    auto hasFluid = [&](u16 id) {
        const BlockInfo& b = registry_.infos()[id];
        return b.fluid || b.waterlogged || registry_.name(id).starts_with("minecraft:bubble_column");
    };
    auto spawn = [&](double x0, double x1, double z0, double z1, double py) {
        add(drip, x0 + (x1 - x0) * double(randf()), py, z0 + (z1 - z0) * double(randf()), 0, 0, 0);
    };
    const u16 below = blockAt(x, y - 1, z);
    if (hasFluid(below)) return;
    Box s;
    if (!shape(below, s) || s.max[1] < 1.0f) {
        // isFaceSturdy(DOWN) of the fluid's own block (a waterlogged one): a full bottom face
        Box f;
        if (shape(fluidBlock, f) && f.min[1] <= 0.0f && f.min[0] <= 0.0f && f.min[2] <= 0.0f && f.max[0] >= 1.0f && f.max[2] >= 1.0f)
            spawn(x, x + 1, z, z + 1, y - 1 + 1 - 0.05);
        return;
    }
    const std::string& name = registry_.name(below);  // BlockTags.IMPERMEABLE: glass
    if ((name.find("glass") != std::string::npos && name.find("pane") == std::string::npos) || name == "minecraft:barrier") return;
    if (s.min[1] > 0.0f) {
        spawn(x + s.min[0], x + s.max[0], z + s.min[2], z + s.max[2], y - 1 + s.min[1] - 0.05);
        return;
    }
    const u16 under = blockAt(x, y - 2, z);
    Box u;
    if ((!shape(under, u) || u.max[1] < 1.0f) && !hasFluid(under))
        spawn(x + s.min[0], x + s.max[0], z + s.min[2], z + s.max[2], y - 1 - 0.05);
}

void Particles::weather(const double cam[3]) {""")
open(p, 'w', encoding='utf-8').write(s)

p2 = 'src/fx/particles.h'
h = open(p2, encoding='utf-8').read()
a = """    void weather(const double cam[3]);"""
assert a in h
h = h.replace(a, """    void weather(const double cam[3]);
    void fluidDrip(u16 fluidBlock, int x, int y, int z, int drip);""")
open(p2, 'w', encoding='utf-8').write(h)
print('ok')
