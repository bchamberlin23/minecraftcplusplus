def edit(p, pairs):
    s = open(p, encoding='utf-8').read()
    for a, b in pairs:
        assert a in s, (p, a[:120])
        s = s.replace(a, b, 1)
    open(p, 'w', encoding='utf-8').write(s)

edit('src/ui/ui_state.h', [
("""    bool onFire = false;                              // our shared flag 0 (Entity.isOnFire on the client)""",
 """    bool onFire = false;                              // our shared flag 0 (Entity.isOnFire on the client)
    bool invertPostEffect = false;                    // ClientboundPostEffectsPacket has minecraft:invert"""),
])
edit('src/ui/ui.cpp', [
("""    client.onPlayPeek("remove_mob_effect", [s](net::Reader& r) {""",
 """    // ClientboundPostEffectsPacket (/posteffect): of the game's post chains only minecraft:invert is drawn (as an
    // inverting full-screen quad); others are ignored.
    client.onPlay("post_effects", [s](net::Reader& r) {
        bool invert = false;
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) invert |= r.str() == "minecraft:invert";
        if (r.ok) s->post([s, invert] { s->invertPostEffect = invert; });
    });
    client.onPlayPeek("remove_mob_effect", [s](net::Reader& r) {"""),
("""    if (!s.fe->cfg.menu) drawScreenEffects(s, c);  // view_effects.cpp: in the 3D view, with or without the HUD""",
 """    if (!s.fe->cfg.menu) drawScreenEffects(s, c);  // view_effects.cpp: in the 3D view, with or without the HUD
    if (s.invertPostEffect && !s.fe->cfg.menu) {  // post/invert.fsh: mix(c, 1 - c, 0.8) = the inverting blend of 0.8 grey
        c.setInvert(true);
        c.fill(0, 0, float(c.guiWidth()), float(c.guiHeight()), 0xFFCCCCCC);
        c.setInvert(false);
    }"""),
])
print('ok')
