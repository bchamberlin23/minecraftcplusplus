p = 'src/render/renderer.h'
s = open(p, encoding='utf-8').read()
a = """    bool overworld = true;   // sun / moon / weather exist
    bool cameraInWater = false;
};"""
b = """    bool overworld = true;   // sun / moon / weather exist
    bool cameraInWater = false;
    bool skyHidden = false;  // a fog environment hides the sky (lava, powder snow, blindness, darkness, boss fog)
    float dim = 1.0f;        // the darkness effect's pulse as a light scale (1: none)
};"""
assert a in s
s = s.replace(a, b)
open(p, 'w', encoding='utf-8').write(s)

p = 'src/render/shading.cpp'
s = open(p, encoding='utf-8').read()
a = """    k.lightColor[3] = sunIsLight ? 1.0f : 0.0f;"""
b = """    k.lightColor[3] = sunIsLight && !sky.skyHidden ? 1.0f : 0.0f;  // the sun's glow in the sky (none behind fog)"""
assert a in s
s = s.replace(a, b)
a = """    k.blockColor[3] = 0.2126f * k.cave[0] + 0.7152f * k.cave[1] + 0.0722f * k.cave[2];
"""
b = """    k.blockColor[3] = 0.2126f * k.cave[0] + 0.7152f * k.cave[1] + 0.0722f * k.cave[2];
    for (int c = 0; c < 3; ++c)  // the darkness effect's pulse dims every light like the lightmap's subtraction
        k.lightColor[c] *= sky.dim, k.ambient[c] *= sky.dim, k.blockColor[c] *= sky.dim, k.cave[c] *= sky.dim;
"""
assert a in s
s = s.replace(a, b)
a = """    k.fog[3] = 0.5f * (1.0f + 1.2f * (1 - smoothstep(0.0f, 0.5f, sun[1]))) * (set.volumetricSteps ? 1.0f : 0.0f);"""
b = """    k.fog[3] = 0.5f * (1.0f + 1.2f * (1 - smoothstep(0.0f, 0.5f, sun[1]))) * (set.volumetricSteps && !sky.skyHidden ? 1.0f : 0.0f);"""
assert a in s
s = s.replace(a, b)
open(p, 'w', encoding='utf-8').write(s)

p = 'src/sky/sky.cpp'
s = open(p, encoding='utf-8').read()
a = """        sl.cameraInWater = fogType == FogType::Water;
"""
b = """        sl.cameraInWater = fogType == FogType::Water;
        sl.skyHidden = hideSky;
        sl.dim = 1.0f - std::min(1.0f, fx_.darknessScale * 2.0f);
"""
assert a in s
s = s.replace(a, b)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
