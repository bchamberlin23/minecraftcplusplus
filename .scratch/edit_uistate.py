p = 'src/ui/ui_state.h'
s = open(p, encoding='utf-8').read()
a = """    struct Effect {
        std::string name;  // "poison"
        int amplifier = 0, duration = 0;  // ticks, -1 infinite
        bool ambient = false, showIcon = true;
    };"""
b = """    struct Effect {
        std::string name;  // "poison"
        int amplifier = 0, duration = 0;  // ticks, -1 infinite
        bool ambient = false, showIcon = true;
        float blend = 0, blendO = 0;  // MobEffectInstance.BlendState factor (this / previous tick)
    };"""
assert a in s
s = s.replace(a, b)
a = """    bool hardcore = false;
    Text deathMessage;"""
b = """    const Effect* findEffect(const char* name) const {
        for (const auto& [id, e] : effects)
            if (e.name == name) return &e;
        return nullptr;
    }
    // View effects (view_effects.cpp): the local player's state behind the fog, lightmap, warp and screen overlays.
    int waterVisionTime = 0;                          // LocalPlayer.waterVisionTime
    bool eyeInWater = false;                          // this tick
    float portalIntensity = 0, portalIntensityO = 0;  // LocalPlayer.portalEffectIntensity (nether portal)
    float spinTime = 0, spinSpeed = 0;                // LocalPlayer.spinningEffectTime / spinningEffectSpeed
    bool onFire = false;                              // our shared flag 0 (Entity.isOnFire on the client)
    void tickViewEffects(bool eyeInWater);
    float effectBlend(const char* name) const;        // LivingEntity.getEffectBlendFactor at the partial tick
    bool hardcore = false;
    Text deathMessage;"""
assert a in s
s = s.replace(a, b)
open(p, 'w', encoding='utf-8').write(s)

p = 'src/ui/ui.h'
s = open(p, encoding='utf-8').read()
a = """    int ticksFrozen() const;  // our DATA_TICKS_FROZEN (the third-person avatar shakes when fully frozen)
"""
b = """    int ticksFrozen() const;  // our DATA_TICKS_FROZEN (the third-person avatar shakes when fully frozen)
    // The local player's mob effects as the view shows them (fog, lightmap: view_effects.cpp), and the nausea / nether
    // portal warp of the world camera (GameRenderer.renderLevel).
    ViewEffects viewEffects() const;
    void warpCamera(CameraState& cam) const;
"""
assert a in s
s = s.replace(a, b)
open(p, 'w', encoding='utf-8').write(s)

p = 'src/ui/ui.cpp'
s = open(p, encoding='utf-8').read()
a = """            s->effects[id] = State::Effect{name, amp, dur, (flags & 1) != 0, (flags & 4) != 0};"""
b = """            State::Effect e{name, amp, dur, (flags & 1) != 0, (flags & 4) != 0};
            if (auto it = s->effects.find(id); it != s->effects.end()) e.blend = it->second.blend, e.blendO = it->second.blendO;
            if (!(flags & 8)) e.blend = e.blendO = -1;  // no FLAG_BLEND: skipBlending (set on the next tick)
            s->effects[id] = e;"""
assert a in s
s = s.replace(a, b)
a = """    client.onPlayPeek("set_entity_data", [s](net::Reader& r) {  // our air supply (index 1) and ticks frozen (7)
        const int entity = r.varInt();
        int air = -1, frozen = -1;
        for (int guard = 0; guard < 8 && r.ok; ++guard) {
            const int index = r.u8v();
            if (index == 0xFF) break;
            const int type = r.varInt();
            if (type == 0) r.u8v();  // byte (shared flags)
            else if (type == 1) {"""
b = """    client.onPlayPeek("set_entity_data", [s](net::Reader& r) {  // our shared flags (0), air supply (1), ticks frozen (7)
        const int entity = r.varInt();
        int air = -1, frozen = -1, flags = -1;
        for (int guard = 0; guard < 8 && r.ok; ++guard) {
            const int index = r.u8v();
            if (index == 0xFF) break;
            const int type = r.varInt();
            if (type == 0) {  // byte
                const int v = r.u8v();
                if (index == 0) flags = v;
            } else if (type == 1) {"""
assert a in s
s = s.replace(a, b)
a = """        if (air >= 0 || frozen >= 0) s->post([s, entity, air, frozen] {
            if (!s->session || entity != s->session->entityId()) return;
            if (air >= 0) s->air = air, s->serverAir = true;
            if (frozen >= 0) s->ticksFrozen = frozen;
        });"""
b = """        if (air >= 0 || frozen >= 0 || flags >= 0) s->post([s, entity, air, frozen, flags] {
            if (!s->session || entity != s->session->entityId()) return;
            if (air >= 0) s->air = air, s->serverAir = true;
            if (frozen >= 0) s->ticksFrozen = frozen;
            if (flags >= 0) s->onFire = (flags & 1) != 0;
        });"""
assert a in s
s = s.replace(a, b)
open(p, 'w', encoding='utf-8').write(s)

p = 'src/ui/hud.cpp'
s = open(p, encoding='utf-8').read()
a = """    (void)p;
    if (!serverAir) {"""
b = """    (void)p;
    tickViewEffects(eyeInWater);  // view_effects.cpp
    if (!serverAir) {"""
assert a in s
s = s.replace(a, b)
open(p, 'w', encoding='utf-8').write(s)
print("ok")
