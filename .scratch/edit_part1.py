import re
p = 'src/fx/particles.h'
s = open(p, encoding='utf-8').read()
def rep(a, b, cnt=1):
    global s
    assert a in s, a[:80]
    s = s.replace(a, b, cnt)

rep("""namespace net {
class Client;
}""", """namespace net {
class Client;
struct Reader;
}""")
rep("""    u32 tex;    // ent::kTexItems or a block texture layer
    float roll;
    float motion[3];  // movement since the previous frame (blocks): motion vectors
    float pad;
};""", """    u32 tex;    // ent::kTexItems or a block texture layer
    float roll;
    float motion[3];  // movement since the previous frame (blocks): motion vectors
    // 0: a camera-facing billboard. Else (bit 31 set) a quad turned by rotationY(a).rotateX(b).rotateY(c) instead of
    // the camera, the angles in 10 bits each (a | b << 10 | c << 20, units of 2 pi / 1024): orientedQuad().
    u32 orient;
};""")
rep("""    void setRainOverride(float level) { rainOverride_ = level; }
""", """    void setRainOverride(float level) { rainOverride_ = level; }
    // ItemParticleOption (ItemStackTemplate.STREAM_CODEC): reads the stack and returns its item network id (network
    // thread; unset or false: the packet is dropped).
    std::function<bool(net::Reader& r, int& item)> readItem;
    // EntityPositionSource: an entity's position (main thread); false when unknown (the vibration ends).
    std::function<bool(int entity, double pos[3])> entityPosition;
""")
rep("""        float scale = 1, power = 1;
        bool hasColor = false;
    };""", """        float scale = 1, power = 1;
        bool hasColor = false;
        u32 color2 = 0;       // dust_color_transition: the colour it fades to (ARGB)
        int item = -1;        // item: network item id
        // vibration: the destination (a block's centre, or entity >= 0 with a y offset) and the travel time
        int entity = -1;
        double target[3] = {};
        float yOffset = 0;
        int arrival = 0;
    };""")
rep("""    void itemShard(int item, double x, double y, double z, double xa, double ya, double za);
""", """    void itemShard(int item, double x, double y, double z, double xa, double ya, double za);
""")
rep("""    u16 localBlock(int networkState);""", """    u16 localBlock(int networkState);
    // BreakingItemParticle with an item atlas rect: scaled keeps a tenth of the random push plus (xa, ya, za).
    void shard(int rect, double x, double y, double z, double xa, double ya, double za, bool scaled);
    bool vibrationTarget(const Particle& p, double out[3]);""")
rep("#include <mutex>\n", "#include <functional>\n#include <mutex>\n")
open(p, 'w', encoding='utf-8').write(s)

p = 'src/fx/particles.cpp'
s = open(p, encoding='utf-8').read()
rep("""    Suspended, HappyVillager, Lava, Dust, Note, Cloud, Snowflake
};
enum class Opt : u8 { None, Block, Dust, DustTransition, Spell, Color, Float, VarInt, Int, IntFloat, Trail, Unsupported };""",
"""    Suspended, HappyVillager, Lava, Dust, DustTransition, Note, Cloud, Snowflake, Item, Vibration
};
enum class Opt : u8 { None, Block, Dust, DustTransition, Spell, Color, Float, VarInt, Int, IntFloat, Trail, Item, Vibration };""")
rep("""    {"lava", Kind::Lava}, {"dust", Kind::Dust}, {"dust_color_transition", Kind::Dust}, {"note", Kind::Note},
    {"cloud", Kind::Cloud}, {"sneeze", Kind::Cloud}, {"snowflake", Kind::Snowflake}};""",
"""    {"lava", Kind::Lava}, {"dust", Kind::Dust}, {"dust_color_transition", Kind::DustTransition}, {"note", Kind::Note},
    {"cloud", Kind::Cloud}, {"sneeze", Kind::Cloud}, {"snowflake", Kind::Snowflake}, {"item", Kind::Item},
    {"item_slime", Kind::Item}, {"item_snowball", Kind::Item}, {"item_cobweb", Kind::Item}, {"vibration", Kind::Vibration}};""")
rep("""    {"trail", Opt::Trail}, {"item", Opt::Unsupported}, {"vibration", Opt::Unsupported}};""",
"""    {"trail", Opt::Trail}, {"item", Opt::Item}, {"vibration", Opt::Vibration}};""")
rep("""u32 abgrF(float r, float g, float b, float a) {
    auto c = [](float v) { return u32(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (c(a) << 24) | (c(b) << 16) | (c(g) << 8) | c(r);
}""", """u32 abgrF(float r, float g, float b, float a) {
    auto c = [](float v) { return u32(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (c(a) << 24) | (c(b) << 16) | (c(g) << 8) | c(r);
}

// GpuParticle::orient for a quad turned by rotationY(a).rotateX(b).rotateY(c).
u32 orientedQuad(float a, float b, float c) {
    auto q = [](float v) {
        const float t = v / (2.0f * kPi);
        return u32(int(std::floor((t - std::floor(t)) * 1024.0f + 0.5f)) & 1023);
    };
    return 0x80000000u | q(a) | q(b) << 10 | q(c) << 20;
}""")
rep("""    float uo, vo;          // terrain shard sub-rectangle (-1: whole sprite)
};""", """    float uo, vo;          // terrain shard sub-rectangle (-1: whole sprite)
    i32 target;            // vibration: destination entity (-1: the block centre in sx, sy, sz)
};""")
# level_particles: decode the options through readOptions
a = s[s.index("        const Type& t = types_[size_t(s.type)];\n        auto argb = [&] { return u32(r.i32v()); };"):s.index("        r.boolean();  // override limiter")]
rep(a, """        const Type& t = types_[size_t(s.type)];
        if (!readOptions(&r, s.type, s.opt)) return;  // options that cannot be decoded: the rest is unreadable
""")
rep("""        case Opt::DustTransition: o.color = 0xFF000000u | u32(r.i32v()), r.i32v(), o.scale = r.f32(), o.hasColor = true; break;""",
"""        case Opt::DustTransition:
            o.color = 0xFF000000u | u32(r.i32v()), o.color2 = 0xFF000000u | u32(r.i32v()), o.scale = r.f32(), o.hasColor = true;
            break;""")
rep("""        case Opt::Trail: r.f64(), r.f64(), r.f64(), r.i32v(), r.varInt(); break;
        case Opt::Unsupported: return false;
    }
    return r.ok;""", """        case Opt::Trail: r.f64(), r.f64(), r.f64(), r.i32v(), r.varInt(); break;
        case Opt::Item:
            if (!readItem || !readItem(r, o.item)) return false;
            break;
        case Opt::Vibration: {  // PositionSource (position_source_type registry: 0 block, 1 entity), arrival ticks
            const int source = r.varInt();
            if (source == 0) {
                int bx, by, bz;
                r.blockPos(bx, by, bz);
                o.target[0] = bx + 0.5, o.target[1] = by + 0.5, o.target[2] = bz + 0.5;  // Vec3.atCenterOf
            } else if (source == 1) {
                o.entity = r.varInt(), o.yOffset = r.f32();
            } else {
                return false;
            }
            o.arrival = r.varInt();
            break;
        }
    }
    return r.ok;""")
open(p, 'w', encoding='utf-8').write(s)
print("ok")
