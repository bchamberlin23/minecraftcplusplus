import re
def edit(p, pairs):
    s = open(p, encoding='utf-8').read()
    for a, b in pairs:
        assert a in s, (p, a[:120])
        s = s.replace(a, b, 1)
    open(p, 'w', encoding='utf-8').write(s)

edit('src/entity/entities.h', [
("""    int attachedTo = -1;     // firework rocket: the entity it is attached to (DATA_ATTACHED_TO_TARGET), -1 none""",
 """    int attachedTo = -1;     // firework rocket: the entity it is attached to (DATA_ATTACHED_TO_TARGET), -1 none
    std::string fireworks;   // firework rocket: its item's "fireworks" component (encoded), "" without one
    bool silent = false;     // DATA_SILENT"""),
("""           paddleLeft = -1, paddleRight = -1, chest = -1;
    };""", """           paddleLeft = -1, paddleRight = -1, chest = -1, fireworksItem = -1, silent = 4;
    };"""),
("""    void setLocalControl(int id, std::function<void(Entity&)> tick) {""",
 """    // Entity events (ClientboundEntityEventPacket) after they are applied: id 17 makes a firework rocket explode.
    std::function<void(const Entity& e, int event)> onEntityEvent;
    void setLocalControl(int id, std::function<void(Entity&)> tick) {"""),
])

edit('src/entity/entities.cpp', [
("""bool readItemStack(net::Reader& r, i64& item, int& count, i32* dye = nullptr, bool* foil = nullptr) {
    if (dye) *dye = -1;
    if (foil) *foil = false;
    if (gItems) {
        ui::ItemStack st;
        const bool ok = gItems->read(r, st);
        item = st.empty() ? -1 : st.item;
        count = st.count;
        if (dye) *dye = st.dyedColor >= 0 ? i32(st.dyedColor & 0xFFFFFF) : -1;
        if (foil) *foil = gItems->foil(st);
        return ok && r.ok;
    }""", """bool readItemStack(net::Reader& r, i64& item, int& count, i32* dye = nullptr, bool* foil = nullptr,
                   std::string* fireworks = nullptr) {
    if (dye) *dye = -1;
    if (foil) *foil = false;
    if (gItems) {
        ui::ItemStack st;
        const bool ok = gItems->read(r, st);
        item = st.empty() ? -1 : st.item;
        count = st.count;
        if (dye) *dye = st.dyedColor >= 0 ? i32(st.dyedColor & 0xFFFFFF) : -1;
        if (foil) *foil = gItems->foil(st);
        if (fireworks) {
            static const int type = gItems->componentType("fireworks");
            for (const auto& c : st.added)
                if (c.type == type) *fireworks = c.data;
        }
        return ok && r.ok;
    }"""),
("""                int count = 0;
                i32 dye = -1;
                bool foil = false;
                const bool complete = readItemStack(r, v.i, count, &dye, &foil);
                v.f[0] = float(count), v.f[1] = float(dye), v.f[2] = foil ? 1.0f : 0.0f;""", """                int count = 0;
                i32 dye = -1;
                bool foil = false;
                std::string fireworks;
                const bool complete = readItemStack(r, v.i, count, &dye, &foil, &fireworks);
                v.f[0] = float(count), v.f[1] = float(dye), v.f[2] = foil ? 1.0f : 0.0f;
                if (!fireworks.empty()) q.strings.push_back(std::move(fireworks)), v.str = i32(q.strings.size() - 1);"""),
("""        d.chest = id({"DATA_ID_CHEST"});""", """        d.chest = id({"DATA_ID_CHEST"});
        d.fireworksItem = id({"DATA_ID_FIREWORKS_ITEM"});"""),
("""        if (idx == d.flags) e.sharedFlags = u8(v.i);""", """        if (idx == d.flags) e.sharedFlags = u8(v.i);
        else if (idx == d.silent && v.serializer == 8) e.silent = v.i != 0;
        else if (idx == d.fireworksItem && v.serializer == 7) e.fireworks = v.str >= 0 ? q.strings[size_t(v.str)] : std::string();"""),
("""            if (ev.a == 2) e.hurtTime = 10;                                     // legacy hurt
            break;""", """            if (ev.a == 2) e.hurtTime = 10;                                     // legacy hurt
            if (onEntityEvent) onEntityEvent(e, ev.a);
            break;"""),
])

edit('src/fx/particles.h', [
("""    void spawn(std::string_view type, double x, double y, double z, double vx, double vy, double vz);""",
 """    void spawn(std::string_view type, double x, double y, double z, double vx, double vy, double vz);
    // ClientLevel.createFireworks: a firework's explosions (its encoded "fireworks" item component; empty: a poof).
    void fireworks(const double pos[3], const double vel[3], const std::string& component, bool sound);"""),
("""    std::vector<std::pair<int, float>> weatherQueue_;  // main thread""", """    std::vector<std::pair<int, float>> weatherQueue_;  // main thread
    struct Explosion {  // FireworkExplosion
        int shape = 0;  // small ball, large ball, star, creeper, burst
        std::vector<u32> colors, fades;
        bool trail = false, twinkle = false;
    };
    struct Starter {  // FireworkParticles.Starter
        double pos[3], vel[3];
        std::vector<Explosion> explosions;
        int life = 0, lifetime = 0;
        bool sound = true, twinkleDelay = false;
    };
    std::vector<Starter> starters_;
    void tickStarter(Starter& s);"""),
])
print('ok')
