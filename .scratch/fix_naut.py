def edit(p, pairs):
    s = open(p, encoding='utf-8').read()
    for a, b in pairs:
        assert a in s, (p, a[:120])
        s = s.replace(a, b, 1)
    open(p, 'w', encoding='utf-8').write(s)

edit('src/game/riding.cpp', [
("""    LOG("DBG move_vehicle %.2f %.2f %.2f yaw %.1f (ours %.2f %.2f %.2f)", pos[0], pos[1], pos[2], yRot, e ? e->pos[0] : 0, e ? e->pos[1] : 0, e ? e->pos[2] : 0);
""", ""),
("""    { static int dbg = 0; if (++dbg % 10 == 0) LOG("DBG naut yaw %.1f rider %.1f pitch %.1f in %.2f/%.2f vel %.3f %.3f %.3f fluid %d ground %d", e.yaw, riderYaw, riderPitch, strafe, forward, e.vel[0], e.vel[1], e.vel[2], int(inFluid), int(vehicleOnGround_)); }
""", ""),
("""    if (isNautilus(n)) return saddled;  // AbstractNautilus.getControllingPassenger""",
 """    if (isNautilus(n)) return saddled && e.tame;  // AbstractNautilus: a saddle counts on a tame one (canUseSlot)"""),
("""    if (n == "happy_ghast") return e.equipment[6] >= 0 && !e.staysStill;""",
 """    if (n == "happy_ghast") return e.equipment[6] >= 0 && !e.baby && !e.staysStill;"""),
])
edit('src/entity/entities.cpp', [
("""        else if (idx == d.tameFlags && (name == "minecraft:wolf" || name == "minecraft:cat")) e.tame = (v.i & 4) != 0;""",
 """        else if (idx == d.tameFlags && (name == "minecraft:wolf" || name == "minecraft:cat" || name == "minecraft:nautilus" ||
                                        name == "minecraft:zombie_nautilus"))
            e.tame = (v.i & 4) != 0;  // TamableAnimal.DATA_FLAGS_ID"""),
])
print('ok')
