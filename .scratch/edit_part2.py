p = 'src/fx/particles.cpp'
s = open(p, encoding='utf-8').read()
def rep(a, b, cnt=1):
    global s
    assert a in s, a[:100]
    s = s.replace(a, b, cnt)

# Sprite sets: item / vibration types do not need one loaded up front for item (the item's own texture).
rep("""    if (t.sprites.empty() && t.kind != Kind::Terrain && t.kind != Kind::Crumble && t.kind != Kind::DustPillar &&
        t.kind != Kind::ExplosionSeed) {""", """    if (t.kind == Kind::Item) {  // BreakingItemParticle: the item's particle texture, no sprite set
        if (t.name == "item") {
            if (!opt || opt->item < 0) return nullptr;
            int& sprite = itemSprites_[opt->item];  // 0 unresolved, -1 none, else rect + 1
            if (sprite == 0) {
                const std::string tex = items_.itemParticleTexture(opt->item);
                const int rect = tex.empty() ? -1 : items_.texture(tex);
                sprite = rect >= 0 ? rect + 1 : -1;
            }
            if (sprite > 0) shard(sprite - 1, x, y, z, xa, ya, za, true);
        } else {  // SlimeProvider / SnowballProvider / CobwebProvider: a fixed item, the plain random push
            const char* tex = t.name == "item_slime" ? "item/slime_ball" : t.name == "item_snowball" ? "item/snowball" : "block/cobweb";
            const int rect = items_.texture(tex);
            if (rect >= 0) shard(rect, x, y, z, 0, 0, 0, false);
        }
        return nullptr;
    }
    if (t.sprites.empty() && t.kind != Kind::Terrain && t.kind != Kind::Crumble && t.kind != Kind::DustPillar &&
        t.kind != Kind::ExplosionSeed) {""")
rep("""    p.uo = -1;
    p.sprite = -1;
    p.light = lightAt(x, y, z);""", """    p.uo = -1;
    p.sprite = -1;
    p.target = -1;
    p.light = lightAt(x, y, z);""")
rep("""        case Kind::Dust: {  // DustParticleBase
            baseVel(x, y, z, xa, ya, za);
            p.friction = 0.96f;
            p.flags |= SpeedUp;
            p.xd *= 0.1f, p.yd *= 0.1f, p.zd *= 0.1f;
            const float scale = opt ? std::clamp(opt->scale, 0.01f, 4.0f) : 1.0f;
            const float br = randf() * 0.4f + 0.6f;
            if (opt && opt->hasColor) color(opt->color);
            p.r *= br, p.g *= br, p.b *= br;""", """        case Kind::Dust:
        case Kind::DustTransition: {  // DustParticleBase
            baseVel(x, y, z, xa, ya, za);
            p.friction = 0.96f;
            p.flags |= SpeedUp;
            p.xd *= 0.1f, p.yd *= 0.1f, p.zd *= 0.1f;
            const float scale = opt ? std::clamp(opt->scale, 0.01f, 4.0f) : 1.0f;
            const float br = randf() * 0.4f + 0.6f;
            if (opt && opt->hasColor) color(opt->color);
            p.r *= br, p.g *= br, p.b *= br;
            if (t.kind == Kind::DustTransition) {  // DustColorTransitionParticle: from (r, g, b) to (e1, e2, e3)
                const u32 to = opt ? opt->color2 : 0xFFFFFFFF;
                p.e1 = float((to >> 16) & 255) / 255.0f * br, p.e2 = float((to >> 8) & 255) / 255.0f * br;
                p.e3 = float(to & 255) / 255.0f * br;
                p.sx = p.r, p.sy = p.g, p.sz = p.b;
            }""")
rep("""        case Kind::Snowflake:
            baseVel(x, y, z, xa, ya, za);""", """        case Kind::Vibration: {  // VibrationSignalParticle: flies to its destination, arriving after `arrival` ticks
            if (!opt) return nullptr;
            base(x, y, z);
            p.quadSize = 0.3f;
            p.lifetime = opt->arrival;
            p.target = opt->entity;
            p.sx = opt->target[0], p.sy = opt->target[1], p.sz = opt->target[2];
            p.e0 = opt->yOffset;
            p.flags &= u8(~Physics);
            p.flags |= Translucent;
            p.emissive = 15;
            double dest[3];
            if (vibrationTarget(p, dest)) {
                const double dx = x - dest[0], dy = y - dest[1], dz = z - dest[2];
                p.roll = p.oRoll = float(std::atan2(dx, dz));                          // rot
                p.e1 = p.e2 = float(std::atan2(dy, std::sqrt(dx * dx + dz * dz)));   // pitch, pitchO
            }
            randomSprite();
            break;
        }
        case Kind::Snowflake:
            baseVel(x, y, z, xa, ya, za);""")

# shard helper + itemShard through it
a = s[s.index("void Particles::itemShard(int item"):s.index("// ---------------------------------------------------------------------------------------------------------\n// Simulation")]
b = '''void Particles::itemShard(int item, double x, double y, double z, double xa, double ya, double za) {
    int& sprite = itemSprites_[item];  // 0 unresolved, -1 none, else rect + 1
    if (sprite == 0) {
        const std::string tex = items_.itemParticleTexture(item);
        const int rect = tex.empty() ? -1 : items_.texture(tex);
        sprite = rect >= 0 ? rect + 1 : -1;
    }
    if (sprite > 0) shard(sprite - 1, x, y, z, xa, ya, za, true);
}

void Particles::shard(int rect, double x, double y, double z, double xa, double ya, double za, bool scaled) {
    static int itemType = -2;  // ticked like terrain shards (same motion as BreakingItemParticle)
    if (itemType == -2) itemType = typeId("block");
    if (live_.size() + spawnQueue_.size() >= kMaxParticles) return;
    Particle p{};
    p.type = u16(std::max(itemType, 0));
    p.a = 1, p.r = p.g = p.b = 1;
    p.flags = Physics;
    p.target = -1;
    p.x = p.xo = x, p.y = p.yo = y, p.z = p.zo = z;
    p.lifetime = int(4.0f / (randf() * 0.9f + 0.1f));
    // Particle(level, x, y, z, 0, 0, 0): a random push; with a velocity BreakingItemParticle keeps a tenth of it plus
    // (xa, ya, za).
    p.xd = (randf() * 2.0f - 1.0f) * 0.4f, p.yd = (randf() * 2.0f - 1.0f) * 0.4f, p.zd = (randf() * 2.0f - 1.0f) * 0.4f;
    const double speed = (randf() + randf() + 1.0f) * 0.15f;
    const double dd = std::max(std::sqrt(p.xd * p.xd + p.yd * p.yd + p.zd * p.zd), 1e-9);
    p.xd = p.xd / dd * speed * 0.4f, p.yd = p.yd / dd * speed * 0.4f + 0.1f, p.zd = p.zd / dd * speed * 0.4f;
    if (scaled) p.xd = p.xd * 0.1f + xa, p.yd = p.yd * 0.1f + ya, p.zd = p.zd * 0.1f + za;
    p.quadSize = 0.1f * (randf() * 0.5f + 0.5f) * 2.0f / 2.0f;
    p.gravity = 1.0f, p.friction = 0.98f;
    p.sprite = rect;
    p.uo = randf() * 3.0f, p.vo = randf() * 3.0f;
    if (items_.rect(p.sprite).translucent) p.flags |= Translucent;
    p.light = lightAt(x, y, z);
    spawnQueue_.push_back(p);
    ++stats_.spawned;
}

// PositionSource.getPosition: the block centre, or the entity's position raised by its y offset.
bool Particles::vibrationTarget(const Particle& p, double out[3]) {
    if (p.target < 0) {
        out[0] = p.sx, out[1] = p.sy, out[2] = p.sz;
        return true;
    }
    if (!entityPosition || !entityPosition(p.target, out)) return false;
    out[1] += p.e0;
    return true;
}

'''
rep(a, b)

# tick: vibration and dust transition
rep("""        case Kind::Lava:
            defaultTick();
            if (!(p.flags & Removed) && randf() > float(p.age) / float(p.lifetime)) {""", """        case Kind::Vibration: {
            if (p.age++ >= p.lifetime) return remove();
            double dest[3];
            if (!vibrationTarget(p, dest)) return remove();
            const double alpha = 1.0 / double(p.lifetime - p.age);  // the rest of the way over the ticks remaining
            p.x += (dest[0] - p.x) * alpha, p.y += (dest[1] - p.y) * alpha, p.z += (dest[2] - p.z) * alpha;
            const double dx = p.x - dest[0], dy = p.y - dest[1], dz = p.z - dest[2];
            p.oRoll = p.roll, p.roll = float(std::atan2(dx, dz));
            p.e2 = p.e1, p.e1 = float(std::atan2(dy, std::sqrt(dx * dx + dz * dz)));
            break;
        }
        case Kind::DustTransition: {
            defaultTick();
            ageSprite();
            const float f = float(p.age) / (float(p.lifetime) + 1.0f);  // lerpColors (per tick)
            p.r = float(p.sx) + (p.e1 - float(p.sx)) * f, p.g = float(p.sy) + (p.e2 - float(p.sy)) * f;
            p.b = float(p.sz) + (p.e3 - float(p.sz)) * f;
            break;
        }
        case Kind::Lava:
            defaultTick();
            if (!(p.flags & Removed) && randf() > float(p.age) / float(p.lifetime)) {""")

# build: oriented quads for the vibration
rep("""        g.motion[2] = float(p.z - p.zo) * frameTicks_, g.pad = 0;
    }""", """        g.motion[2] = float(p.z - p.zo) * frameTicks_, g.orient = 0;
        if (types_[p.type].kind == Kind::Vibration) {  // two quads along the path, swaying (VibrationSignalParticle.extract)
            const float sway = std::sin((float(p.age) + pt - 2.0f * kPi) * 0.05f) * 2.0f;
            const float rot = p.oRoll + (p.roll - p.oRoll) * pt, pitch = p.e2 + (p.e1 - p.e2) * pt + kPi / 2.0f;
            g.roll = 0;
            g.orient = orientedQuad(rot, -pitch, sway);
            if (n < back) {
                GpuParticle& g2 = buf[--back];
                g2 = g;
                g2.orient = orientedQuad(-kPi + rot, pitch, sway);
            }
        }
    }""")
open(p, 'w', encoding='utf-8').write(s)
print("ok")
