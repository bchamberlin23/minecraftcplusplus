p = 'src/game/riding.cpp'
s = open(p, encoding='utf-8').read()
a = """    pendingJump_ = 0;
    if (inFluid) {  // travelInWater: no gravity, 0.9 drag"""
b = """    pendingJump_ = 0;
    { static int dbg = 0; if (++dbg % 10 == 0) LOG("DBG naut yaw %.1f rider %.1f pitch %.1f in %.2f/%.2f vel %.3f %.3f %.3f fluid %d ground %d", e.yaw, riderYaw, riderPitch, strafe, forward, e.vel[0], e.vel[1], e.vel[2], int(inFluid), int(vehicleOnGround_)); }
    if (inFluid) {  // travelInWater: no gravity, 0.9 drag"""
assert a in s
s = s.replace(a, b)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
