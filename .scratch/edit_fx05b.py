p = 'src/fx/particles.cpp'
s = open(p, encoding='utf-8').read()
def rep(a, b):
    global s
    assert a in s, a[:120]
    s = s.replace(a, b, 1)

# GustSeed has no sprites; let it through like the explosion seed
rep("""    if (t.sprites.empty() && t.kind != Kind::Terrain && t.kind != Kind::Crumble && t.kind != Kind::DustPillar &&
        t.kind != Kind::ExplosionSeed) {""", """    if (t.sprites.empty() && t.kind != Kind::Terrain && t.kind != Kind::Crumble && t.kind != Kind::DustPillar &&
        t.kind != Kind::ExplosionSeed && t.kind != Kind::GustSeed) {""")

# sonic boom on top of the explosion
rep("""            p.quadSize = 2.0f * (1.0f - float(xa) * 0.5f);
            p.emissive = 255;
            ageSprite();
            break;
        }""", """            p.quadSize = 2.0f * (1.0f - float(xa) * 0.5f);
            p.emissive = 255;
            if (t.name == "sonic_boom") p.lifetime = 16, p.quadSize = 1.5f;  // SonicBoomParticle
            ageSprite();
            break;
        }""")
# sneeze: the cloud tinted
rep("""            p.r = p.g = p.b = 1.0f - randf() * 0.3f;
            p.quadSize *= 1.875f;
            p.lifetime = std::max(int(8.0 / (randf() * 0.8 + 0.3) * 2.5), 1);
            p.sizeMode = SizeGrow;
            ageSprite();
            break;""", """            p.r = p.g = p.b = 1.0f - randf() * 0.3f;
            p.quadSize *= 1.875f;
            p.lifetime = std::max(int(8.0 / (randf() * 0.8 + 0.3) * 2.5), 1);
            p.sizeMode = SizeGrow;
            if (t.name == "sneeze") p.r = 0.22f, p.g = 1.0f, p.b = 0.53f, p.a = 0.4f, p.flags |= Translucent;  // SneezeProvider
            ageSprite();
            break;""")

rep("""        case Kind::Snowflake:
            baseVel(x, y, z, xa, ya, za);""", r"""        case Kind::Drip: {  // DripParticle: position constructor, then the provider's colour / gravity / lifetime
            const DripDef& d = kDrips[size_t(t.drip)];
            base(x, y, z);
            p.r = d.r, p.g = d.g, p.b = d.b;
            p.gravity = d.gravity;
            if (d.life > 0) p.lifetime = int(d.life);
            else if (t.name == "falling_spore_blossom") p.lifetime = int(64.0f / (0.1f + randf() * 0.8f));
            else p.lifetime = int(-d.life / (randf() * 0.8f + 0.2f));
            if (d.glow) p.emissive = 15;
            randomSprite();
            break;
        }
        case Kind::Spark:
        case Kind::Totem:
        case Kind::EndRod: {  // SimpleAnimatedParticle: friction 0.91, fading out over the second half, full bright
            base(x, y, z);
            p.friction = 0.91f;
            p.xd = xa, p.yd = ya, p.zd = za;
            p.quadSize *= 0.75f;
            p.flags |= Translucent;
            p.emissive = 255;
            if (t.kind == Kind::Spark) {  // FireworkParticles.SparkParticle
                p.gravity = 0.1f;
                p.lifetime = 48 + int(rng_() % 12);
                p.a = 0.99f;
                if (opt && opt->spark) {  // the firework starter's colours and flags
                    color(opt->color);
                    p.e0 = float(opt->spark);
                    if (opt->spark & kSparkFade)
                        p.e1 = float((opt->color2 >> 16) & 255) / 255.0f, p.e2 = float((opt->color2 >> 8) & 255) / 255.0f,
                        p.e3 = float(opt->color2 & 255) / 255.0f;
                }
            } else if (t.kind == Kind::Totem) {  // TotemParticle
                p.gravity = 1.25f, p.friction = 0.6f;
                p.lifetime = 60 + int(rng_() % 12);
                if (rng_() % 4 == 0) p.r = 0.6f + randf() * 0.2f, p.g = 0.6f + randf() * 0.3f, p.b = randf() * 0.2f;
                else p.r = 0.1f + randf() * 0.2f, p.g = 0.4f + randf() * 0.3f, p.b = randf() * 0.2f;
            } else {  // EndRodParticle: no collisions, fades to #F2DEC9
                p.gravity = 0.0125f;
                p.lifetime = 60 + int(rng_() % 12);
                p.flags &= u8(~Physics);
                p.e0 = float(kSparkFade);
                p.e1 = 0xF2 / 255.0f, p.e2 = 0xDE / 255.0f, p.e3 = 0xC9 / 255.0f;
            }
            ageSprite();
            break;
        }
        case Kind::Flash:  // FireworkParticles.OverlayParticle: a big flash for four ticks (size / alpha in build)
            base(x, y, z);
            p.lifetime = 4;
            p.flags |= Translucent;
            if (opt && opt->hasColor) color(opt->color), p.a = float(opt->color >> 24) / 255.0f;
            randomSprite();
            break;
        case Kind::Gust:  // GustParticle: still, full bright, animated over 12-15 ticks
            base(x, y, z);
            p.lifetime = 12 + int(rng_() % 4);
            p.quadSize = t.name == "small_gust" ? 0.15f : 1.0f;
            p.emissive = 255;
            p.flags &= u8(~Physics);
            ageSprite();
            break;
        case Kind::GustSeed: {  // GustSeedParticle: emits gusts (scale, lifetime, ticks in between)
            base(x, y, z);
            const bool large = t.name == "gust_emitter_large";
            p.e0 = large ? 3.0f : 1.0f;
            p.lifetime = large ? 7 : 3;
            p.e1 = large ? 0.0f : 2.0f;
            p.flags |= NoRender;
            break;
        }
        case Kind::Rising:  // EmissiveRisingParticle (RisingParticle): soul / sculk_soul
            baseVel(x, y, z, xa, ya, za);
            p.friction = 0.96f;
            p.xd = p.xd * 0.01f + xa, p.yd = p.yd * 0.01f + ya, p.zd = p.zd * 0.01f + za;
            p.x += (randf() - randf()) * 0.05f, p.y += (randf() - randf()) * 0.05f, p.z += (randf() - randf()) * 0.05f;
            p.xo = p.x, p.yo = p.y, p.zo = p.z;
            p.lifetime = int(8.0 / (randf() * 0.8 + 0.2)) + 4;
            p.quadSize *= 1.5f;
            p.flags |= Translucent;
            if (t.name == "sculk_soul") p.emissive = 15;
            ageSprite();
            break;
        case Kind::SculkCharge:
        case Kind::SculkChargePop:  // no collisions, glowing, flying at the given speed
            baseVel(x, y, z, xa, ya, za);
            p.friction = 0.96f;
            p.xd = xa, p.yd = ya, p.zd = za;
            p.flags &= u8(~Physics);
            p.flags |= Translucent;
            p.emissive = 15;
            if (t.kind == Kind::SculkCharge) {
                p.quadSize *= 1.5f;
                p.roll = p.oRoll = opt ? opt->scale : 0.0f;
                p.lifetime = int(rng_() % 12) + 8;
            } else {
                p.lifetime = int(rng_() % 4) + 6;
            }
            ageSprite();
            break;
        case Kind::Shriek:  // ShriekParticle: two tilted rings rising after a delay
            base(x, y, z);
            p.quadSize = 0.85f;
            p.e0 = float(opt ? opt->delay : 0);
            p.lifetime = 30;
            p.xd = 0, p.yd = 0.1, p.zd = 0;
            p.flags |= Translucent;
            p.emissive = 15;
            randomSprite();
            break;
        case Kind::Firefly:  // FireflyParticle: wanders, fading in and out, glowing with its own light
            baseVel(x, y, z, 0.5 - randf(), rng_() & 1 ? ya : -ya, 0.5 - randf());
            p.flags |= SpeedUp | Translucent;
            p.friction = 0.96f;
            p.quadSize *= 0.75f * 1.5f;
            p.xd *= 0.8f, p.yd *= 0.8f, p.zd *= 0.8f;
            p.lifetime = 200 + int(rng_() % 101);
            p.a = 0;
            randomSprite();
            break;
        case Kind::Glow: {  // GlowParticle providers: glow squid, wax on / off, electric spark, scrape
            const bool squid = t.name == "glow";
            baseVel(x, y, z, squid ? 0.5 - randf() : 0.0, squid ? ya : 0.0, squid ? 0.5 - randf() : 0.0);
            p.friction = 0.96f;
            p.flags |= SpeedUp;
            p.flags &= u8(~Physics);
            p.quadSize *= 0.75f;
            if (squid) {
                if (rng_() & 1) p.r = 0.6f, p.g = 1.0f, p.b = 0.8f;
                else p.r = 0.08f, p.g = 0.4f, p.b = 0.4f;
                p.yd *= 0.2f;
                if (xa == 0.0 && za == 0.0) p.xd *= 0.1f, p.zd *= 0.1f;
                p.lifetime = int(8.0 / (randf() * 0.8 + 0.2));
            } else if (t.name == "electric_spark") {
                p.r = 1.0f, p.g = 0.9f, p.b = 1.0f;
                p.xd = xa * 0.25, p.yd = ya * 0.25, p.zd = za * 0.25;
                p.lifetime = int(rng_() % 2) + 2;
            } else if (t.name == "scrape") {
                if (rng_() & 1) p.r = 0.29f, p.g = 0.58f, p.b = 0.51f;
                else p.r = 0.43f, p.g = 0.77f, p.b = 0.62f;
                p.xd = xa * 0.01, p.yd = ya * 0.01, p.zd = za * 0.01;
                p.lifetime = int(rng_() % 30) + 10;
            } else {  // wax_on / wax_off
                if (t.name == "wax_on") p.r = 0.91f, p.g = 0.55f, p.b = 0.08f;
                else p.r = 1.0f, p.g = 0.9f, p.b = 1.0f;
                p.xd = xa * 0.01 / 2.0, p.yd = ya * 0.01, p.zd = za * 0.01 / 2.0;
                p.lifetime = int(rng_() % 30) + 10;
            }
            ageSprite();
            break;
        }
        case Kind::Snowflake:
            baseVel(x, y, z, xa, ya, za);""")

# ---- ticks
rep("""        case Kind::DustTransition: {
            defaultTick();""", r"""        case Kind::Drip: {
            const DripDef& d = kDrips[size_t(t.drip)];
            if (d.stage == DripCoolingHang)  // CoolingDripHangParticle: lava drops cool from white to orange
                p.r = 1.0f, p.g = 16.0f / float(40 - p.lifetime + 16), p.b = 4.0f / float(40 - p.lifetime + 8);
            if (p.lifetime-- <= 0) {  // preMoveUpdate: a hanging drop lets go
                remove();
                if (d.stage == DripHang || d.stage == DripCoolingHang) add(dripNext(types_[p.type]), p.x, p.y, p.z, p.xd, p.yd, p.zd);
                return;
            }
            p.yd -= p.gravity;
            move(p, p.xd, p.yd, p.zd);
            if (d.stage == DripHang || d.stage == DripCoolingHang) {
                p.xd *= 0.02, p.yd *= 0.02, p.zd *= 0.02;
            } else if (d.stage != DripLand && (p.flags & OnGround)) {  // postMoveUpdate on the ground
                remove();
                if (d.stage != DripFall) add(dripNext(types_[p.type]), p.x, p.y, p.z, 0, 0, 0);
                const char* sound = d.stage == DripHoneyFallLand ? "minecraft:block.beehive.drip"
                                    : d.stage != DripDripstoneFallLand ? nullptr
                                    : d.fluid == 2 ? "minecraft:block.pointed_dripstone.drip_lava"
                                                   : "minecraft:block.pointed_dripstone.drip_water";
                if (sound)
                    if (audio::AudioSystem* a = audio::instance()) {
                        const double pos[3] = {p.x, p.y, p.z};
                        a->playNetwork(-1, sound, audio::Category::Block, pos, 0.3f + randf() * 0.7f, 1.0f);
                    }
                return;
            }
            p.xd *= 0.98f, p.yd *= 0.98f, p.zd *= 0.98f;
            if (d.fluid) {  // into its own fluid: gone
                const int bx = int(std::floor(p.x)), by = int(std::floor(p.y)), bz = int(std::floor(p.z));
                const u16 id = blockAt(bx, by, bz);
                const BlockInfo& b = registry_.infos()[id];
                const bool lava = registry_.name(id).starts_with("minecraft:lava");
                if ((d.fluid == 2 ? lava : (b.fluid || b.waterlogged) && !lava) && p.y < by + 0.875) remove();
            }
            break;
        }
        case Kind::Spark:
        case Kind::Totem:
        case Kind::EndRod: {
            defaultTick();
            ageSprite();
            if (p.age > p.lifetime / 2) {  // SimpleAnimatedParticle.tick: fade out, towards the fade colour
                p.a = 1.0f - (float(p.age) - float(p.lifetime / 2)) / float(p.lifetime);
                if (int(p.e0) & kSparkFade)
                    p.r += (p.e1 - p.r) * 0.2f, p.g += (p.e2 - p.g) * 0.2f, p.b += (p.e3 - p.b) * 0.2f;
            }
            // SparkParticle.tick: a trail of sparks over the first half
            if (t.kind == Kind::Spark && (int(p.e0) & kSparkTrail) && p.age < p.lifetime / 2 && (p.age + p.lifetime) % 2 == 0) {
                Options o;
                o.spark = (int(p.e0) & ~kSparkTrail) | kSparkSet;
                o.color = (u32(p.r * 255.0f) << 16) | (u32(p.g * 255.0f) << 8) | u32(p.b * 255.0f);
                o.color2 = (u32(p.e1 * 255.0f) << 16) | (u32(p.e2 * 255.0f) << 8) | u32(p.e3 * 255.0f);
                if (Particle* q = add(p.type, p.x, p.y, p.z, 0, 0, 0, &o)) q->age = q->lifetime / 2, q->r = p.r, q->g = p.g, q->b = p.b;
            }
            break;
        }
        case Kind::Flash:
            defaultTick();
            break;
        case Kind::Gust:
            if (p.age++ >= p.lifetime) return remove();
            ageSprite();
            break;
        case Kind::GustSeed: {
            static int gust = -2;
            if (gust == -2) gust = typeId("gust");
            const float scale = p.e0;
            if (p.age % (int(p.e1) + 1) == 0)
                for (int i = 0; i < 3; ++i)
                    add(gust, p.x + (randf() - randf()) * scale, p.y + (randf() - randf()) * scale, p.z + (randf() - randf()) * scale,
                        float(p.age) / float(p.lifetime), 0, 0);
            if (p.age++ == p.lifetime) remove();
            break;
        }
        case Kind::Rising:
        case Kind::SculkCharge:
        case Kind::SculkChargePop:
        case Kind::Glow:
            defaultTick();
            ageSprite();
            break;
        case Kind::Shriek:
            if (p.e0 > 0) {
                p.e0 -= 1;
                p.xo = p.x, p.yo = p.y, p.zo = p.z;
            } else {
                defaultTick();
            }
            break;
        case Kind::Firefly: {
            defaultTick();
            if (p.flags & Removed) break;
            if (registry_.infos()[blockAt(int(std::floor(p.x)), int(std::floor(p.y)), int(std::floor(p.z)))].kind != KindAir) return remove();
            p.a = fadeAmount(std::clamp(float(p.age) / float(p.lifetime), 0.0f, 1.0f), 0.3f, 0.5f);
            if (randf() > 0.95f || p.age == 1)
                p.xd = -0.05f + 0.1f * randf(), p.yd = -0.05f + 0.1f * randf(), p.zd = -0.05f + 0.1f * randf();
            break;
        }
        case Kind::DustTransition: {
            defaultTick();""")

# ---- build: per-kind size / alpha / light / orientation
rep("""        if (p.sprite < 0 || n >= back) continue;
        const bool tr = (p.flags & Translucent) != 0;""", """        if (p.sprite < 0 || n >= back) continue;
        const Kind kind = types_[p.type].kind;
        if (kind == Kind::Shriek && p.e0 > 0) continue;  // still waiting for its delay
        if (kind == Kind::Spark && (int(p.e0) & kSparkTwinkle) && p.age >= p.lifetime / 3 && (p.age + p.lifetime) / 3 % 2 != 0)
            continue;  // twinkling: hidden every other three ticks
        const bool tr = (p.flags & Translucent) != 0;""")
rep("""        g.size = size;
        const ent::ItemModels::Rect& r = items_.rect(p.sprite);""", """        float alpha = p.a;
        if (kind == Kind::Flash) {  // OverlayParticle: grows and fades
            const float t = float(p.age) + pt - 1.0f;
            size = 7.1f * std::sin(t * 0.25f * kPi), alpha = 0.6f - t * 0.25f * 0.5f;
        } else if (kind == Kind::Shriek) {
            const float t = (float(p.age) + pt) / float(p.lifetime);
            size *= std::clamp(t * 0.75f, 0.0f, 1.0f), alpha = 1.0f - std::clamp(t, 0.0f, 1.0f);
        }
        g.size = size;
        const ent::ItemModels::Rect& r = items_.rect(p.sprite);""")
rep("""        g.color = abgrF(p.r, p.g, p.b, p.a);
        u32 light = p.light;
        if (p.emissive == 255) light = 256;""", """        g.color = abgrF(p.r, p.g, p.b, alpha);
        u32 light = p.light;
        if (kind == Kind::Firefly) {  // its own light only: block light by the fade
            light = u32(int(255.0f * fadeAmount(std::clamp((float(p.age) + pt) / float(p.lifetime), 0.0f, 1.0f), 0.1f, 0.3f)) >> 4);
        } else if (kind == Kind::Glow) {  // LightCoordsUtil.addSmoothBlockEmission over the lifetime
            const u32 add = u32(std::clamp((float(p.age) + pt) / float(p.lifetime), 0.0f, 1.0f) * 15.0f);
            light = (light & 0xF0) | std::min<u32>(15, (light & 15) + add);
        } else if (p.emissive == 255) light = 256;""")
rep("""        if (types_[p.type].kind == Kind::Vibration) {  // two quads along the path, swaying (VibrationSignalParticle.extract)""",
"""        if (kind == Kind::Shriek) {  // rotationX(-1.0472) and rotationYXZ(-pi, 1.0472, 0)
            g.roll = 0;
            g.orient = orientedQuad(0, -1.0472f, 0);
            if (n < back) {
                GpuParticle& g2 = buf[--back];
                g2 = g;
                g2.orient = orientedQuad(-kPi, 1.0472f, 0);
            }
        }
        if (kind == Kind::Vibration) {  // two quads along the path, swaying (VibrationSignalParticle.extract)""")
open(p, 'w', encoding='utf-8').write(s)

p2 = 'src/fx/particles.h'
h = open(p2, encoding='utf-8').read()
a = """        int delay = 0;        // shriek"""
assert a in h
h = h.replace(a, """        int delay = 0;        // shriek
        int spark = 0;        // firework sparks: kSpark* flags (colour in color, fade colour in color2)""")
a = """    u16 localBlock(int networkState);"""
assert a in h
h = h.replace(a, """    u16 localBlock(int networkState);
    int dripNext(Type& t);  // the particle type a drip turns into""")
a = """    static constexpr u32 kMaxParticles = 16384;"""
assert a in h
h = h.replace(a, """    static constexpr u32 kMaxParticles = 16384;
    static constexpr int kSparkSet = 1, kSparkTrail = 2, kSparkTwinkle = 4, kSparkFade = 8;""")
open(p2, 'w', encoding='utf-8').write(h)
print('ok')
