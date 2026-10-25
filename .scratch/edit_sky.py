p = 'src/sky/sky.cpp'
s = open(p, encoding='utf-8').read()
old_start = s.index("    // FogRenderer.computeFogColor with AtmosphericFogEnvironment.getBaseColor.")
old_end = s.index("    // getHorizonHeight: 63 unless the world is flat")
new = r'''    // FogRenderer: the fluid at the camera picks the fog type (Camera.getFluidInCamera).
    const u16 eye = world_->block(int(std::floor(cam.x)), int(std::floor(cam.y)), int(std::floor(cam.z)));
    const std::string& eyeName = registry_->name(eye);
    const bool eyeWater = registry_->infos()[eye].waterlogged || eyeName.rfind("minecraft:water", 0) == 0 ||
                          eyeName.rfind("minecraft:bubble_column", 0) == 0;
    enum class FogType { Atmospheric, Water, Lava, PowderSnow };
    const FogType fogType = eyeWater                                         ? FogType::Water
                            : eyeName.rfind("minecraft:lava", 0) == 0        ? FogType::Lava
                            : eyeName.rfind("minecraft:powder_snow", 0) == 0 ? FogType::PowderSnow
                                                                             : FogType::Atmospheric;
    // computeFogColor: the first environment providing a colour (lava, powder snow, water, the atmosphere).
    const float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    const float forward[3] = {-sy * cp, -sp, cy * cp};
    float skyDark[3] = {a[int(Attr::SkyColor)][0], a[int(Attr::SkyColor)][1], a[int(Attr::SkyColor)][2]};
    if (rain > 0.0f) {
        const float m = 1.0f - rain * 0.5f, mb = 1.0f - rain * 0.4f;
        skyDark[0] = std::clamp(skyDark[0] * m, 0.0f, 1.0f), skyDark[1] = std::clamp(skyDark[1] * m, 0.0f, 1.0f);
        skyDark[2] = std::clamp(skyDark[2] * mb, 0.0f, 1.0f);
    }
    if (thunder > 0.0f)
        for (float& c : skyDark) c = std::clamp(c * (1.0f - thunder * 0.5f), 0.0f, 1.0f);
    float fog[3];
    if (fogType == FogType::Lava) {  // LavaFogEnvironment #991900
        fog[0] = 0x99 / 255.0f, fog[1] = 0x19 / 255.0f, fog[2] = 0;
    } else if (fogType == FogType::PowderSnow) {  // PowderedSnowFogEnvironment #9FBBCC
        fog[0] = 0x9F / 255.0f, fog[1] = 0xBB / 255.0f, fog[2] = 0xCC / 255.0f;
    } else if (fogType == FogType::Water) {  // WaterFogEnvironment: the biome's water fog colour
        for (int k = 0; k < 3; ++k) fog[k] = a[int(Attr::WaterFogColor)][k];
    } else {  // AtmosphericFogEnvironment.getBaseColor
        for (int k = 0; k < 3; ++k) fog[k] = a[int(Attr::FogColor)][k];
        const float sunAngle = a[int(Attr::SunAngle)][0] * kPi / 180.0f;
        const Value& sunrise = a[int(Attr::SunriseSunsetColor)];
        const float lookingAtSun = forward[0] * (std::sin(sunAngle) > 0.0f ? -1.0f : 1.0f);
        if (lookingAtSun > 0.0f && sunrise[3] > 0.0f)
            for (int k = 0; k < 3; ++k) fog[k] += (sunrise[k] - fog[k]) * lookingAtSun * sunrise[3];
        const float skyFogEnd = std::min(a[int(Attr::SkyFogEnd)][0] / 16.0f, float(kRenderDistance));
        const float mix = 1.0f - std::pow(0.25f + 0.75f * std::clamp(skyFogEnd / 32.0f, 0.0f, 1.0f), 0.25f);
        for (int k = 0; k < 3; ++k) fog[k] += (skyDark[k] - fog[k]) * mix;
    }
    // The void darkness, raised by the first mob effect environment that modifies it (blindness, then darkness).
    float voidDark = std::clamp((32.0f + float(dimension_->minY) - float(cam.y)) / 32.0f, 0.0f, 1.0f);
    if (fx_.blindFog >= 0.0f) voidDark = std::max(fx_.blindDarkness, voidDark);
    else if (fx_.darkness) voidDark = std::max(fx_.darknessBlend, voidDark);
    if (voidDark > 0.0f && fogType != FogType::Lava && fogType != FogType::PowderSnow)
        for (float& c : fog) c *= (1.0f - voidDark) * (1.0f - voidDark);
    // FogRenderer.computeFogColor: the boss overlay's world darkening.
    const float darken = std::clamp(darkenO_ + (darken_ - darkenO_) * float(tickAccumulator_), 0.0f, 1.0f);
    static constexpr float kDarkenRgb[3] = {0.7f, 0.6f, 0.6f};
    if (darken > 0.0f)
        for (int k = 0; k < 3; ++k) fog[k] += (fog[k] * kDarkenRgb[k] - fog[k]) * darken;
    // Brightened towards full brightness by the water vision underwater, else by night vision.
    const float brighten = fogType == FogType::Water ? fx_.waterVision : fx_.fogBrighten;
    if (brighten > 0.0f && fog[0] != 0.0f && fog[1] != 0.0f && fog[2] != 0.0f) {
        const float scale = 1.0f / std::max(fog[0], std::max(fog[1], fog[2]));
        for (float& c : fog) c += (c * scale - c) * brighten;
    }

    Frame& f = *frame_;
    f.skybox = dimension_->skybox;
    std::copy(fog, fog + 3, f.fogColor);
    f.fogColor[3] = 1.0f;
    std::copy(a[int(Attr::SkyColor)].begin(), a[int(Attr::SkyColor)].begin() + 3, f.skyColor);
    f.skyEnd = std::min(float(kRenderDistance * 16), a[int(Attr::SkyFogEnd)][0]);
    // FogRenderer.setupFog: the first applicable environment sets the fog distances. The atmosphere's own are beyond
    // the view distance except under a boss bar's world fog (start <= 10, end <= 96, sky end = end).
    const float renderDistance = float(kRenderDistance * 16);
    float envStart = 1e30f, envEnd = 1e30f, skyEnd = 1e30f;
    // LevelRenderer.addSkyPass: no sky in lava or powder snow, or with blindness / darkness.
    bool hideSky = fogType == FogType::Lava || fogType == FogType::PowderSnow || fx_.blindFog >= 0.0f || fx_.darkness;
    if (fogType == FogType::Lava) {
        if (fx_.spectator) envStart = -8.0f, envEnd = renderDistance * 0.5f;
        else if (fx_.fireResistance) envStart = 0.0f, envEnd = 5.0f;
        else envStart = 0.25f, envEnd = 1.0f;
        skyEnd = envEnd;
    } else if (fogType == FogType::PowderSnow) {
        if (fx_.spectator) envStart = -8.0f, envEnd = renderDistance * 0.5f;
        else envStart = 0.0f, envEnd = 2.0f;
        skyEnd = envEnd;
    } else if (fx_.blindFog >= 0.0f) {  // BlindnessFogEnvironment
        const float d = renderDistance + (5.0f - renderDistance) * fx_.blindFog;
        envStart = d * 0.25f, envEnd = d, skyEnd = d * 0.8f;
    } else if (fx_.darkness) {  // DarknessFogEnvironment
        const float d = renderDistance + (15.0f - renderDistance) * fx_.darknessBlend;
        envStart = d * 0.75f, envEnd = d, skyEnd = d;
    } else if (fogType == FogType::Water) {  // WaterFogEnvironment
        envStart = a[int(Attr::WaterFogStart)][0];
        envEnd = a[int(Attr::WaterFogEnd)][0] * std::max(0.25f, fx_.waterVision);
        skyEnd = envEnd;
    } else if (bossFog_) {  // AtmosphericFogEnvironment under a boss bar's world fog
        envStart = 10.0f, envEnd = std::min(96.0f, f.skyEnd), skyEnd = envEnd, hideSky = true;
    }
    f.skyEnd = std::min(f.skyEnd, skyEnd);
    cloudFogLimit_ = skyEnd;  // FogData.cloudEnd follows the sky end in every environment but the plain atmosphere
    f.renderSky = !hideSky;
    renderer_->setFogLimit(envStart, envEnd);
'''
s = s[:old_start] + new + s[old_end:]
a = """        const u16 eye = world_->block(int(std::floor(cam.x)), int(std::floor(cam.y)), int(std::floor(cam.z)));
        sl.cameraInWater = registry_->infos()[eye].waterlogged || registry_->name(eye).rfind("minecraft:water", 0) == 0;
"""
assert a in s
s = s.replace(a, """        sl.cameraInWater = fogType == FogType::Water;
""")
a = """        const std::string& eyeName = registry_->name(eye);
        g.isEyeInWater = sl.cameraInWater ? 1 : eyeName.rfind("minecraft:lava", 0) == 0 ? 2
                                              : eyeName.rfind("minecraft:powder_snow", 0) == 0 ? 3 : 0;"""
assert a in s
s = s.replace(a, """        g.isEyeInWater = sl.cameraInWater ? 1 : fogType == FogType::Lava ? 2 : fogType == FogType::PowderSnow ? 3 : 0;""")
a = """    if (bossFog_) f.cloudFogEnd = std::min(f.cloudFogEnd, 96.0f);  // a boss bar's world fog: cloud end = fog end"""
assert a in s
s = s.replace(a, """    f.cloudFogEnd = std::min(f.cloudFogEnd, cloudFogLimit_);  // a fog environment's cloud end (boss fog: = fog end)""")
open(p, 'w', encoding='utf-8').write(s)
