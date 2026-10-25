def edit(p, pairs):
    s = open(p, encoding='utf-8').read()
    for a, b in pairs:
        assert a in s, (p, a[:120])
        s = s.replace(a, b, 1)
    open(p, 'w', encoding='utf-8').write(s)

edit('src/entity/entities.h', [
("""    bool silent = false;     // DATA_SILENT""", """    bool silent = false;     // DATA_SILENT
    bool staysStill = false; // happy ghast: STAYS_STILL (holding still: its rider does not steer)"""),
("""           paddleLeft = -1, paddleRight = -1, chest = -1, fireworksItem = -1, silent = 4;""",
 """           paddleLeft = -1, paddleRight = -1, chest = -1, fireworksItem = -1, silent = 4, staysStill = -1;"""),
])
edit('src/entity/entities.cpp', [
("""        d.fireworksItem = id({"DATA_ID_FIREWORKS_ITEM"});""", """        d.fireworksItem = id({"DATA_ID_FIREWORKS_ITEM"});
        d.staysStill = id({"STAYS_STILL"});"""),
("""        else if (idx == d.silent && v.serializer == 8) e.silent = v.i != 0;""", """        else if (idx == d.silent && v.serializer == 8) e.silent = v.i != 0;
        else if (idx == d.staysStill && v.serializer == 8) e.staysStill = v.i != 0;"""),
])

edit('src/game/riding.h', [
("""//    camels and pigs / striders steered with their item (LivingEntity.travel with the ridden input and speed); others
//    (minecarts, llamas, a second boat seat) follow the server;
//  - jumping vehicles: the jump bar charge (LocalPlayer.aiStep) and START_RIDING_JUMP; the client's horse jump.""",
 """//    camels and pigs / striders steered with their item (LivingEntity.travel with the ridden input and speed), saddled
//    nautiluses / zombie nautiluses swimming where the rider looks, harnessed happy ghasts flying (unless holding still);
//    others (minecarts, llamas, a second boat seat) follow the server;
//  - jumping vehicles: the jump bar charge (LocalPlayer.aiStep) and START_RIDING_JUMP; the client's horse jump, the
//    camel's and the nautilus's dash and its cooldown (Camel / AbstractNautilus.getJumpCooldown)."""),
("""    float jumpScale_ = 0, pendingJump_ = 0;""", """    float jumpScale_ = 0, pendingJump_ = 0;
    int dashCooldown_ = 0;  // camel (55 ticks) / nautilus (40 ticks) after a dash"""),
])

edit('src/game/riding.cpp', [
("""bool isBoat(std::string_view n) { return n.ends_with("_boat") || n.ends_with("_raft"); }""",
 """bool isBoat(std::string_view n) { return n.ends_with("_boat") || n.ends_with("_raft"); }
bool isCamel(std::string_view n) { return n == "camel" || n == "camel_husk"; }
bool isNautilus(std::string_view n) { return n == "nautilus" || n == "zombie_nautilus"; }"""),
("""        jumpRidingTicks_ = 0, jumpScale_ = 0, pendingJump_ = 0, wasJumping_ = false;""",
 """        jumpRidingTicks_ = 0, jumpScale_ = 0, pendingJump_ = 0, wasJumping_ = false, dashCooldown_ = 0;"""),
("""    if (n == "pig") return saddled && holding("minecraft:carrot_on_a_stick");
    if (n == "strider") return saddled && holding("minecraft:warped_fungus_on_a_stick");
    return false;""", """    if (n == "pig") return saddled && holding("minecraft:carrot_on_a_stick");
    if (n == "strider") return saddled && holding("minecraft:warped_fungus_on_a_stick");
    if (isNautilus(n)) return saddled;  // AbstractNautilus.getControllingPassenger
    // HappyGhast.getControllingPassenger: wearing a harness (body slot) and not holding still
    if (n == "happy_ghast") return e.equipment[6] >= 0 && !e.staysStill;
    return false;"""),
("""bool Riding::jumpable() const {
    if (!controlled_ || kind_ != Kind::Living) return false;
    return isHorseLike(shortName(typeName_));  // PlayerRideableJumping.canJump: saddled (checked by controlled_)
}

int Riding::jumpCooldown() const { return 0; }""", """bool Riding::jumpable() const {
    if (!controlled_ || kind_ != Kind::Living) return false;
    const std::string_view n = shortName(typeName_);
    return isHorseLike(n) || isNautilus(n);  // PlayerRideableJumping.canJump: saddled (checked by controlled_)
}

int Riding::jumpCooldown() const {  // the dash cooldown of camels and nautiluses
    const std::string_view n = shortName(typeName_);
    return isCamel(n) || isNautilus(n) ? dashCooldown_ : 0;
}"""),
("""            // AbstractHorse.onPlayerJump -> getPlayerJumpPendingScale
            pendingJump_ = amount >= 90 ? 1.0f : 0.4f + 0.4f * float(std::max(amount, 0)) / 90.0f;""",
 """            // AbstractHorse.onPlayerJump -> getPlayerJumpPendingScale (Camel.onPlayerJump: only on the ground)
            pendingJump_ = amount >= 90 ? 1.0f : 0.4f + 0.4f * float(std::max(amount, 0)) / 90.0f;
            if (isCamel(shortName(typeName_)) && !vehicleOnGround_) pendingJump_ = 0;"""),
("""    if (kind_ == Kind::Boat) boatTick(e);
    else livingTick(e);
    lock.unlock();""", """    if (kind_ == Kind::Boat) boatTick(e);
    else livingTick(e);
    if (dashCooldown_ > 0) --dashCooldown_;  // Camel / AbstractNautilus.tick
    lock.unlock();"""),
("""void Riding::livingTick(ent::Entity& e) {
    const std::string_view n = shortName(typeName_);
    const bool horse = isHorseLike(n);
    const Player& p = game_.player();""", """void Riding::livingTick(ent::Entity& e) {
    const std::string_view n = shortName(typeName_);
    if (n == "happy_ghast" || isNautilus(n)) return swimFlyTick(e);
    const bool horse = isHorseLike(n);
    const Player& p = game_.player();"""),
("""    double speed = gameplay_.entityAttribute(vehicle_, "movement_speed", horse ? 0.225 : n == "pig" ? 0.25 : 0.175);
    if (n == "pig") speed *= 0.225;""", """    double speed = gameplay_.entityAttribute(vehicle_, "movement_speed", horse ? 0.225 : n == "pig" ? 0.25 : 0.175);
    if (isCamel(n) && input_.sprint && dashCooldown_ == 0) speed += 0.1;  // Camel.getRiddenSpeed
    if (n == "pig") speed *= 0.225;"""),
("""        if (pendingJump_ > 0) {
            const double jumpStrength = gameplay_.entityAttribute(vehicle_, "jump_strength", 0.7);
            e.vel[1] = jumpStrength * pendingJump_;
            if (forward > 0) {
                e.vel[0] += -0.4 * std::sin(e.yaw * kDegf) * pendingJump_;
                e.vel[2] += 0.4 * std::cos(e.yaw * kDegf) * pendingJump_;
            }
        }""", """        if (pendingJump_ > 0 && isCamel(n)) {  // Camel.executeRidersJump: the dash along the look, up a little
            const double jumpStrength = gameplay_.entityAttribute(vehicle_, "jump_strength", 0.42);
            const double h = 22.2222 * pendingJump_ * speed;  // x the block speed factor (1)
            e.vel[0] += -std::sin(e.yaw * kDegf) * h;
            e.vel[2] += std::cos(e.yaw * kDegf) * h;
            e.vel[1] += 1.4285 * pendingJump_ * jumpStrength;
            dashCooldown_ = 55;
        } else if (pendingJump_ > 0) {
            const double jumpStrength = gameplay_.entityAttribute(vehicle_, "jump_strength", 0.7);
            e.vel[1] = jumpStrength * pendingJump_;
            if (forward > 0) {
                e.vel[0] += -0.4 * std::sin(e.yaw * kDegf) * pendingJump_;
                e.vel[2] += 0.4 * std::cos(e.yaw * kDegf) * pendingJump_;
            }
        }"""),
])

s = open('src/game/riding.cpp', encoding='utf-8').read()
s += """
// ---------------------------------------------------------------------------------------------------------
// HappyGhast (flying where the rider looks) and AbstractNautilus (swimming where the rider looks, dashing)

void Riding::swimFlyTick(ent::Entity& e) {
    const std::string_view n = shortName(typeName_);
    const bool ghast = n == "happy_ghast";
    const Player& p = game_.player();
    // tickRidden: the vehicle turns towards the rider's look (ghast 0.08, nautilus 0.5 of the way), pitch halved.
    const float riderYaw = p.yaw / kDegf, riderPitch = p.pitch / kDegf;
    e.yaw += wrapDegrees(riderYaw - e.yaw) * (ghast ? 0.08f : 0.5f);
    e.pitch = riderPitch * 0.5f;
    e.bodyYaw = e.headYaw = e.yaw;
    // getRiddenInput: strafe, and forward / up along the rider's pitch (backwards at half); the ghast also rises with jump.
    const double strafe = input_.strafe * 0.98, zza = input_.forward * 0.98;
    double forward = 0, up = 0;
    if (zza != 0) {
        double f = std::cos(riderPitch * kDegf), u = -std::sin(riderPitch * kDegf);
        if (zza < 0) f *= -0.5, u *= -0.5;
        forward = f, up = u;
    }
    const ItemRules::EntityDims* dims = rules_.entity(typeName_);
    const double width = dims ? dims->width : ghast ? 4.0 : 0.875, height = dims ? dims->height : ghast ? 4.0 : 0.95;
    const double step = gameplay_.entityAttribute(vehicle_, "step_height", 0.6);
    auto moveRelative = [&](double sp, double ix, double iy, double iz) {  // Entity.getInputVector
        const double len = ix * ix + iy * iy + iz * iz;
        if (len < 1e-7) return;
        if (len > 1.0) {
            const double inv = 1.0 / std::sqrt(len);
            ix *= inv, iy *= inv, iz *= inv;
        }
        ix *= sp, iy *= sp, iz *= sp;
        const double sy = std::sin(e.yaw * kDegf), cy = std::cos(e.yaw * kDegf);
        e.vel[0] += ix * cy - iz * sy;
        e.vel[1] += iy;
        e.vel[2] += iz * cy + ix * sy;
    };
    const Physics physics(gameplay_.world(), gameplay_.registry());
    const AABB body{{e.pos[0] - width / 2, e.pos[1], e.pos[2] - width / 2}, {e.pos[0] + width / 2, e.pos[1] + height, e.pos[2] + width / 2}};
    const bool inFluid = physics.inFluid(body);
    auto move = [&] {
        double d[3] = {e.vel[0], e.vel[1], e.vel[2]};
        collideMove(e, width, height, step, d, vehicleOnGround_, vehicleHCollision_);
        e.onGround = vehicleOnGround_;
    };
    if (ghast) {  // HappyGhast.travel: travelFlying at flying_speed * 5 / 3 with the input scaled by 3.9 x flying_speed
        const double flying = gameplay_.entityAttribute(vehicle_, "flying_speed", 0.05);
        if (input_.jump) up += 0.5;
        const double k = 3.9 * flying;
        moveRelative(flying * 5.0 / 3.0, strafe * k, up * k, forward * k);
        move();
        const double drag = inFluid ? 0.8 : 0.91;  // water 0.8 (lava 0.5), air 0.91
        for (double& v : e.vel) v *= drag;
        return;
    }
    // AbstractNautilus: the dash (tickRidden -> executeRidersJump) along the rider's look, then swimming or walking.
    const double ms = gameplay_.entityAttribute(vehicle_, "movement_speed", 1.0);
    if (pendingJump_ > 0) {
        const double cp = std::cos(riderPitch * kDegf), sp = std::sin(riderPitch * kDegf);
        const double look[3] = {-std::sin(riderYaw * kDegf) * cp, -sp, std::cos(riderYaw * kDegf) * cp};
        const double dash = (inFluid ? 1.2 : 0.5) * pendingJump_ * ms;
        for (int a = 0; a < 3; ++a) e.vel[a] += look[a] * dash;
        dashCooldown_ = 40;
    }
    pendingJump_ = 0;
    if (inFluid) {  // travelInWater: no gravity, 0.9 drag
        moveRelative(0.0325 * ms, strafe, up, forward);
        move();
        for (double& v : e.vel) v *= 0.9;
        return;
    }
    // travelInAir with the ridden speed 0.02 x movement_speed
    const double gravity = gameplay_.entityAttribute(vehicle_, "gravity", 0.08);
    const float friction = vehicleOnGround_ ? physics.slipperiness(int(std::floor(e.pos[0])), int(std::floor(e.pos[1] - 0.5)),
                                                                   int(std::floor(e.pos[2])))
                                            : 1.0f;
    const double speed = 0.02 * ms;
    moveRelative(vehicleOnGround_ ? speed * (0.21600002 / (double(friction) * friction * friction)) : speed * 0.1, strafe, up, forward);
    move();
    e.vel[1] = (e.vel[1] - gravity) * 0.98;
    e.vel[0] *= double(friction) * 0.91, e.vel[2] *= double(friction) * 0.91;
}
"""
open('src/game/riding.cpp', 'w', encoding='utf-8').write(s)

edit('src/game/riding.h', [
("""    void livingTick(ent::Entity& e);""", """    void livingTick(ent::Entity& e);
    void swimFlyTick(ent::Entity& e);  // happy ghast, nautilus"""),
])
print('ok')
