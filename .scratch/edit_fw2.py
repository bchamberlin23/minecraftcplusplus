def edit(p, pairs):
    s = open(p, encoding='utf-8').read()
    for a, b in pairs:
        assert a in s, (p, a[:120])
        s = s.replace(a, b, 1)
    open(p, 'w', encoding='utf-8').write(s)

edit('src/fx/particles.cpp', [
("""void Particles::spawn(std::string_view type, double x, double y, double z, double vx, double vy, double vz) {
    add(typeId(type), x, y, z, vx, vy, vz);
}""", """void Particles::spawn(std::string_view type, double x, double y, double z, double vx, double vy, double vz) {
    add(typeId(type), x, y, z, vx, vy, vz);
}

void Particles::fireworks(const double pos[3], const double vel[3], const std::string& component, bool sound) {
    // Fireworks.STREAM_CODEC: flight duration, then the FireworkExplosion list.
    Starter s{{pos[0], pos[1], pos[2]}, {vel[0], vel[1], vel[2]}};
    s.sound = sound;
    net::Reader r(reinterpret_cast<const u8*>(component.data()), component.size());
    if (!component.empty()) {
        r.varInt();
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && i < 256 && r.ok; ++i) {
            Explosion e;
            e.shape = std::clamp(r.varInt(), 0, 4);
            for (std::vector<u32>* list : {&e.colors, &e.fades}) {
                const i32 c = r.varInt();
                for (i32 k = 0; k < c && k < 256 && r.ok; ++k) list->push_back(u32(r.i32v()) & 0xFFFFFF);
            }
            e.trail = r.boolean(), e.twinkle = r.boolean();
            if (r.ok) s.explosions.push_back(std::move(e));
        }
    }
    if (s.explosions.empty()) {  // no explosions: a few poofs
        static int poof = -2;
        if (poof == -2) poof = typeId("poof");
        for (int i = 0, n = int(rng_() % 3) + 2; i < n; ++i) add(poof, pos[0], pos[1], pos[2], gauss() * 0.05, 0.005, gauss() * 0.05);
        return;
    }
    s.lifetime = int(s.explosions.size()) * 2 - 1;
    for (const Explosion& e : s.explosions)
        if (e.twinkle) {
            s.twinkleDelay = true;
            s.lifetime += 15;
            break;
        }
    starters_.push_back(std::move(s));
}

void Particles::tickStarter(Starter& s) {
    static int spark = -2, flash = -2;
    if (spark == -2) spark = typeId("firework"), flash = typeId("flash");
    const double dx = s.pos[0] - cam_[0], dy = s.pos[1] - cam_[1], dz = s.pos[2] - cam_[2];
    const bool far = dx * dx + dy * dy + dz * dz >= 256.0;
    auto play = [&](const char* name, float pitch) {
        if (audio::AudioSystem* a = audio::instance()) a->playNetwork(-1, name, audio::Category::Ambient, s.pos, 20.0f, pitch);
    };
    if (s.life == 0 && s.sound) {
        bool large = s.explosions.size() >= 3;
        for (const Explosion& e : s.explosions) large |= e.shape == 1;
        play(large ? (far ? "minecraft:entity.firework_rocket.large_blast_far" : "minecraft:entity.firework_rocket.large_blast")
                   : (far ? "minecraft:entity.firework_rocket.blast_far" : "minecraft:entity.firework_rocket.blast"),
             0.95f + randf() * 0.1f);
    }
    if (s.life % 2 == 0 && size_t(s.life / 2) < s.explosions.size()) {
        const Explosion& e = s.explosions[size_t(s.life / 2)];
        const std::vector<u32> black = {0x1E1B1B};  // DyeColor.BLACK firework colour
        const std::vector<u32>& colors = e.colors.empty() ? black : e.colors;
        auto one = [&](double xa, double ya, double za) {  // Starter.createParticle
            Options o;
            o.spark = kSparkSet | (e.trail ? kSparkTrail : 0) | (e.twinkle ? kSparkTwinkle : 0) | (e.fades.empty() ? 0 : kSparkFade);
            o.color = colors[rng_() % colors.size()];
            if (!e.fades.empty()) o.color2 = e.fades[rng_() % e.fades.size()];
            add(spark, s.pos[0], s.pos[1], s.pos[2], xa, ya, za, &o);
        };
        auto ball = [&](double speed, int steps) {
            for (int y = -steps; y <= steps; ++y)
                for (int x = -steps; x <= steps; ++x)
                    for (int z = -steps; z <= steps; ++z) {
                        const double xa = x + (randf() - randf()) * 0.5, ya = y + (randf() - randf()) * 0.5, za = z + (randf() - randf()) * 0.5;
                        const double len = std::sqrt(xa * xa + ya * ya + za * za) / speed + gauss() * 0.05;
                        one(xa / len, ya / len, za / len);
                        if (y != -steps && y != steps && x != -steps && x != steps) z += steps * 2 - 1;
                    }
        };
        auto shape = [&](double speed, const double (*coords)[2], int count, bool flat) {
            one(coords[0][0] * speed, coords[0][1] * speed, 0);
            const float baseAngle = randf() * kPi;
            const double mod = flat ? 0.034 : 0.34;
            for (int step = 0; step < 3; ++step) {
                const double angle = baseAngle + step * kPi * mod;
                double ox = coords[0][0], oy = coords[0][1];
                for (int c = 1; c < count; ++c) {
                    const double tx = coords[c][0], ty = coords[c][1];
                    for (double sub = 0.25; sub <= 1.0; sub += 0.25) {
                        double xa = (ox + (tx - ox) * sub) * speed;
                        const double ya = (oy + (ty - oy) * sub) * speed;
                        const double za = xa * std::sin(angle);
                        xa *= std::cos(angle);
                        for (double flip = -1.0; flip <= 1.0; flip += 2.0) one(xa * flip, ya, za * flip);
                    }
                    ox = tx, oy = ty;
                }
            }
        };
        static const double kCreeper[][2] = {{0.0, 0.2}, {0.2, 0.2}, {0.2, 0.6}, {0.6, 0.6}, {0.6, 0.2}, {0.2, 0.2},
                                             {0.2, 0.0}, {0.4, 0.0}, {0.4, -0.6}, {0.2, -0.6}, {0.2, -0.4}, {0.0, -0.4}};
        static const double kStar[][2] = {{0.0, 1.0}, {0.3455, 0.309}, {0.9511, 0.309}, {0.3795918367346939, -0.12653061224489795},
                                          {0.6122448979591837, -0.8040816326530612}, {0.0, -0.35918367346938773}};
        switch (e.shape) {
            case 0: ball(0.25, 2); break;
            case 1: ball(0.5, 4); break;
            case 2: shape(0.5, kStar, 6, false); break;
            case 3: shape(0.5, kCreeper, 12, true); break;
            default: {  // burst
                const double offX = gauss() * 0.05, offZ = gauss() * 0.05;
                for (int i = 0; i < 70; ++i)
                    one(s.vel[0] * 0.5 + gauss() * 0.15 + offX, s.vel[1] * 0.5 + randf() * 0.5, s.vel[2] * 0.5 + gauss() * 0.15 + offZ);
            }
        }
        Options o;  // ColorParticleOption.create(FLASH, colour): the flash takes the first colour
        o.hasColor = true;
        o.color = colors[0];
        add(flash, s.pos[0], s.pos[1], s.pos[2], 0, 0, 0, &o);
    }
    if (++s.life > s.lifetime) {
        if (s.twinkleDelay && s.sound)
            play(far ? "minecraft:entity.firework_rocket.twinkle_far" : "minecraft:entity.firework_rocket.twinkle", 0.9f + randf() * 0.15f);
        s.life = -1;  // done
    }
}"""),
("""    live_.erase(std::remove_if(live_.begin(), live_.end(), [](const Particle& p) { return (p.flags & Removed) != 0; }),
                live_.end());""", """    live_.erase(std::remove_if(live_.begin(), live_.end(), [](const Particle& p) { return (p.flags & Removed) != 0; }),
                live_.end());
    for (Starter& s : starters_) tickStarter(s);
    starters_.erase(std::remove_if(starters_.begin(), starters_.end(), [](const Starter& s) { return s.life < 0; }), starters_.end());"""),
])

edit('src/entity/visuals.cpp', [
("""    m.particles->entityPosition = [&world = m.world](int id, double pos[3]) {  // vibrations travelling to an entity""",
 """    m.world.onEntityEvent = [&m](const Entity& e, int event) {  // FireworkRocketEntity.handleEntityEvent: 17 explodes
        if (event != 17 || !m.particles || !e.info || e.info->name != "minecraft:firework_rocket") return;
        std::shared_lock lock(m.world.world().mutex());
        m.particles->fireworks(e.pos, e.vel, e.fireworks, !e.silent);
    };
    m.particles->entityPosition = [&world = m.world](int id, double pos[3]) {  // vibrations travelling to an entity"""),
("""    animClock += dt;
    models.tick(u64(animClock * 20.0));  // animated textures in the item atlas (block items, vibration particles)
    if (particles) {""", """    animClock += dt;
    models.tick(u64(animClock * 20.0));  // animated textures in the item atlas (block items, vibration particles)
    // FireworkRocketEntity.tick on the client: a spark behind the rocket every tick.
    for (; particles && rocketTicks < u64(animClock * 20.0); ++rocketTicks)
        for (const auto& [id, e] : world.entities()) {
            if (!e.info || e.info->name != "minecraft:firework_rocket") continue;
            static std::normal_distribution<double> gauss;
            static std::mt19937 rng(17);
            std::shared_lock lock(world.world().mutex());
            particles->spawn("firework", e.pos[0], e.pos[1], e.pos[2], gauss(rng) * 0.05, -(e.pos[1] - e.prevPos[1]) * 0.5,
                             gauss(rng) * 0.05);
        }
    if (particles) {"""),
("""    double animClock = 0;  // seconds of updates: the client ticks of the item atlas animations""",
 """    double animClock = 0;  // seconds of updates: the client ticks of the item atlas animations
    u64 rocketTicks = 0;   // firework rocket trails emitted up to this tick"""),
])
print('ok')
