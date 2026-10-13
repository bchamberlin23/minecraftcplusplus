#include "sky/environment.h"

#include "assets/asset_source.h"

#include <json/json.hpp>

#include <algorithm>
#include <cmath>
#include <functional>

namespace sky {

namespace {

using json = nlohmann::json;

enum class Type : u8 { Rgb, Argb, Float, Angle, Phase };
struct AttrDef {
    const char* id;
    Type type;
    Value def;
    float min, max;
};
constexpr float kInf = 3.0e38f;
// Defaults and value ranges from EnvironmentAttributes.
const AttrDef kDefs[kAttrCount] = {
    {"minecraft:visual/fog_color", Type::Rgb, {0, 0, 0, 1}, -kInf, kInf},
    {"minecraft:visual/sky_color", Type::Rgb, {0, 0, 0, 1}, -kInf, kInf},
    {"minecraft:visual/sunrise_sunset_color", Type::Argb, {0, 0, 0, 0}, -kInf, kInf},
    {"minecraft:visual/cloud_color", Type::Argb, {0, 0, 0, 0}, -kInf, kInf},
    {"minecraft:visual/cloud_height", Type::Float, {192.33f}, -kInf, kInf},
    {"minecraft:visual/sun_angle", Type::Angle, {0}, -kInf, kInf},
    {"minecraft:visual/moon_angle", Type::Angle, {0}, -kInf, kInf},
    {"minecraft:visual/star_angle", Type::Angle, {0}, -kInf, kInf},
    {"minecraft:visual/moon_phase", Type::Phase, {0}, -kInf, kInf},
    {"minecraft:visual/star_brightness", Type::Float, {0}, 0, 1},
    {"minecraft:visual/sky_light_color", Type::Rgb, {1, 1, 1, 1}, -kInf, kInf},
    {"minecraft:visual/sky_light_factor", Type::Float, {1}, 0, 1},
    {"minecraft:visual/ambient_light_color", Type::Rgb, {0, 0, 0, 1}, -kInf, kInf},
    {"minecraft:visual/block_light_tint", Type::Rgb, {1, 216 / 255.0f, 140 / 255.0f, 1}, -kInf, kInf},
    {"minecraft:visual/sky_fog_end_distance", Type::Float, {512}, 0, kInf},
    {"minecraft:visual/cloud_fog_end_distance", Type::Float, {2048}, 0, kInf},
    {"minecraft:visual/water_fog_color", Type::Rgb, {5 / 255.0f, 5 / 255.0f, 51 / 255.0f, 1}, -kInf, kInf},
    {"minecraft:visual/water_fog_start_distance", Type::Float, {-8}, -kInf, kInf},
    {"minecraft:visual/water_fog_end_distance", Type::Float, {96}, 0, kInf},
    {"minecraft:visual/night_vision_color", Type::Rgb, {0.6f, 0.6f, 0.6f, 1}, -kInf, kInf},
    {"minecraft:gameplay/sky_light_level", Type::Float, {15}, 0, 15},
    {"minecraft:visual/fog_start_distance", Type::Float, {0}, -kInf, kInf},
    {"minecraft:visual/fog_end_distance", Type::Float, {1024}, 0, kInf},
};
const char* const kMoonPhases[8] = {"full_moon",       "waning_gibbous",  "third_quarter", "waning_crescent",
                                    "new_moon",        "waxing_crescent", "first_quarter", "waxing_gibbous"};

std::string withNamespace(std::string s) { return s.find(':') == std::string::npos ? "minecraft:" + s : s; }

int attrIndex(const std::string& id) {
    const std::string full = withNamespace(id);
    for (int i = 0; i < kAttrCount; ++i)
        if (full == kDefs[i].id) return i;
    return -1;
}

bool isColor(Type t) { return t == Type::Rgb || t == Type::Argb; }

bool parseColor(const json& j, bool argb, Value& out) {
    u32 v = 0;
    if (j.is_string()) {
        std::string s = j.get<std::string>();
        if (!s.empty() && s[0] == '#') s.erase(0, 1);
        if (s.size() != 6 && s.size() != 8) return false;
        v = u32(std::stoul(s, nullptr, 16));
        if (s.size() == 6) v |= 0xFF000000u;
    } else if (j.is_number_integer()) {
        v = u32(j.get<i64>());
        if (!argb) v |= 0xFF000000u;
    } else {
        return false;
    }
    out = {float((v >> 16) & 255) / 255.0f, float((v >> 8) & 255) / 255.0f, float(v & 255) / 255.0f,
           argb ? float(v >> 24) / 255.0f : 1.0f};
    return true;
}

bool parseOp(const std::string& name, Op& op) {
    static const std::pair<const char*, Op> kOps[] = {
        {"override", Op::Override}, {"add", Op::Add},         {"subtract", Op::Subtract},
        {"multiply", Op::Multiply}, {"minimum", Op::Minimum}, {"maximum", Op::Maximum},
        {"alpha_blend", Op::AlphaBlend}, {"blend_to_gray", Op::BlendToGray}};
    for (const auto& [n, o] : kOps)
        if (name == n) return op = o, true;
    return false;
}

// The argument of `op` for an attribute of type `t` (the modifier's argument codec).
bool parseArgument(Type t, Op op, const json& j, Value& out) {
    out = {0, 0, 0, 1};
    if (op == Op::BlendToGray) {
        if (!j.is_object()) return false;
        out = {j.value("brightness", 1.0f), j.value("factor", 0.0f), 0, 0};
        return true;
    }
    if (isColor(t)) {
        const bool argb = op == Op::AlphaBlend || (t == Type::Argb && (op == Op::Multiply || op == Op::Override));
        return parseColor(j, argb, out);
    }
    if (t == Type::Phase) {
        if (!j.is_string()) return false;
        const std::string s = j.get<std::string>();
        for (int i = 0; i < 8; ++i)
            if (s == kMoonPhases[i]) return out = {float(i)}, true;
        return false;
    }
    if (op == Op::AlphaBlend) {  // FloatWithAlpha
        if (j.is_number()) return out = {j.get<float>(), 1.0f}, true;
        if (!j.is_object()) return false;
        out = {j.value("value", 0.0f), j.value("alpha", 1.0f)};
        return true;
    }
    if (!j.is_number()) return false;
    out = {j.get<float>()};
    return true;
}

// A "attributes" object entry: a plain value (override) or {"modifier": ..., "argument": ...}.
void parseAttributes(const json& attrs, AttributeMap& map) {
    if (!attrs.is_object()) return;
    for (const auto& [key, value] : attrs.items()) {
        const int a = attrIndex(key);
        if (a < 0) continue;
        Modifier m;
        const json* arg = &value;
        if (value.is_object() && value.contains("modifier")) {
            if (!parseOp(value["modifier"].get<std::string>(), m.op) || !value.contains("argument")) continue;
            arg = &value["argument"];
        }
        if (parseArgument(kDefs[a].type, m.op, *arg, m.arg)) map.entries[size_t(a)] = m;
    }
}

Value apply(Type t, const Value& base, Op op, const Value& arg) {
    Value r = base;
    if (op == Op::Override) return arg;
    if (isColor(t)) {
        switch (op) {
            case Op::Multiply:
                for (int c = 0; c < 3; ++c) r[c] = base[c] * arg[c];
                if (t == Type::Argb) r[3] = base[3] * arg[3];
                break;
            case Op::Add:
                for (int c = 0; c < 3; ++c) r[c] = std::clamp(base[c] + arg[c], 0.0f, 1.0f);
                break;
            case Op::Subtract:
                for (int c = 0; c < 3; ++c) r[c] = std::clamp(base[c] - arg[c], 0.0f, 1.0f);
                break;
            case Op::AlphaBlend:  // ARGB.alphaBlend
                if (arg[3] == 0.0f) break;
                if (t == Type::Rgb) {
                    for (int c = 0; c < 3; ++c) r[c] = base[c] + (arg[c] - base[c]) * arg[3];
                } else if (arg[3] == 1.0f) {
                    r = arg;
                } else {
                    const float a = arg[3] + base[3] * (1.0f - arg[3]);
                    for (int c = 0; c < 3; ++c) r[c] = (arg[c] * arg[3] + base[c] * (a - arg[3])) / a;
                    r[3] = a;
                }
                break;
            case Op::BlendToGray: {
                const float g = std::clamp((base[0] * 0.3f + base[1] * 0.59f + base[2] * 0.11f) * arg[0], 0.0f, 1.0f);
                for (int c = 0; c < 3; ++c) r[c] = base[c] + (g - base[c]) * arg[1];
                break;
            }
            default:
                break;
        }
        return r;
    }
    if (t == Type::Phase) return base;
    switch (op) {
        case Op::Add: r[0] = base[0] + arg[0]; break;
        case Op::Subtract: r[0] = base[0] - arg[0]; break;
        case Op::Multiply: r[0] = base[0] * arg[0]; break;
        case Op::Minimum: r[0] = std::min(base[0], arg[0]); break;
        case Op::Maximum: r[0] = std::max(base[0], arg[0]); break;
        case Op::AlphaBlend: r[0] = base[0] + (arg[0] - base[0]) * arg[1]; break;
        default: break;
    }
    return r;
}

Value lerpValue(float alpha, const Value& a, const Value& b) {
    Value r;
    for (int c = 0; c < 4; ++c) r[c] = a[c] + alpha * (b[c] - a[c]);
    return r;
}

float bezierEase(const float ctl[4], float x) {  // EasingType.CubicBezier
    auto curve = [](float v1, float v2, float t) {
        const float a = 3.0f * v1 - 3.0f * v2 + 1.0f, b = -6.0f * v1 + 3.0f * v2, c = 3.0f * v1;
        return ((a * t + b) * t + c) * t;
    };
    auto gradient = [](float v1, float v2, float t) {
        const float a = 3.0f * v1 - 3.0f * v2 + 1.0f, b = -6.0f * v1 + 3.0f * v2, c = 3.0f * v1;
        return (3.0f * a * t + 2.0f * b) * t + c;
    };
    float t = x;
    bool solved = false;
    for (int i = 0; i < 4; ++i) {
        const float err = curve(ctl[0], ctl[2], t) - x;
        if (std::abs(err) < 1.0e-5f) {
            solved = true;
            break;
        }
        const float g = gradient(ctl[0], ctl[2], t);
        if (g < 1.0e-5f) break;
        t -= std::clamp(err / g, -0.25f, 0.25f);
    }
    if (!solved) {
        float t0 = 0.0f, t1 = 1.0f;
        for (int i = 0; i < 64 && t0 < t1; ++i) {
            const float err = curve(ctl[0], ctl[2], t) - x;
            if (std::abs(err) < 1.0e-5f) break;
            (err < 0.0f ? t0 : t1) = t;
            t = (t1 + t0) / 2.0f;
        }
    }
    return curve(ctl[1], ctl[3], t);
}

// KeyframeTrackSampler.sample at a fractional time.
Value sampleTrack(const Track& tr, Type type, int period, double ticks) {
    const auto& k = tr.keys;
    if (k.size() == 1) return k[0].second;
    if (period > 0) ticks -= std::floor(ticks / period) * period;
    struct Seg {
        const Value* from;
        double fromT;
        const Value* to;
        double toT;
    };
    auto pick = [&]() -> Seg {
        if (period > 0 && ticks < k.front().first)
            return {&k.back().second, double(k.back().first - period), &k.front().second, double(k.front().first)};
        for (size_t i = 0; i + 1 < k.size(); ++i)
            if (ticks < k[i + 1].first) return {&k[i].second, double(k[i].first), &k[i + 1].second, double(k[i + 1].first)};
        if (period > 0)
            return {&k.back().second, double(k.back().first), &k.front().second, double(k.front().first + period)};
        return {&k[k.size() - 2].second, double(k[k.size() - 2].first), &k.back().second, double(k.back().first)};
    };
    const Seg s = pick();
    if (ticks <= s.fromT) return *s.from;
    if (ticks >= s.toT) return *s.to;
    const float alpha = float((ticks - s.fromT) / (s.toT - s.fromT));
    const float eased = tr.ease == 1 ? 0.0f : tr.ease == 2 ? bezierEase(tr.bezier, alpha) : alpha;
    if (type == Type::Phase) return eased >= 1.0f ? *s.to : *s.from;  // not interpolated
    return lerpValue(eased, *s.from, *s.to);
}

bool readJson(const AssetSource& src, const std::string& file, json& out) {
    std::vector<u8> bytes;
    if (!src.read(file, bytes)) return false;
    out = json::parse(bytes.begin(), bytes.end(), nullptr, false);
    return !out.is_discarded();
}

// WeatherAttributes.RAIN / THUNDER.
struct WeatherLayer {
    std::array<std::optional<Modifier>, kAttrCount> m;
};
WeatherLayer weatherLayer(bool thunder) {
    WeatherLayer w;
    const float level = thunder ? 0.52734375f : 0.3125f;
    auto set = [&](Attr a, Op op, Value arg) { w.m[size_t(a)] = Modifier{op, arg}; };
    set(Attr::SkyColor, Op::BlendToGray, thunder ? Value{0.24f, 0.94f} : Value{0.6f, 0.75f});
    set(Attr::FogColor, Op::Multiply, thunder ? Value{0.25f, 0.25f, 0.3f, 1} : Value{0.5f, 0.5f, 0.6f, 1});
    set(Attr::CloudColor, Op::BlendToGray, thunder ? Value{0.095f, 0.94f} : Value{0.24f, 0.5f});
    set(Attr::SkyLightColor, Op::AlphaBlend, {0.48f, 0.48f, 1.0f, level});
    set(Attr::SkyLightFactor, Op::AlphaBlend, {0.24f, level});
    set(Attr::SkyLightLevel, Op::AlphaBlend, {4.0f, level});
    set(Attr::StarBrightness, Op::Override, {0});
    set(Attr::SunriseSunsetColor, Op::Multiply, thunder ? Value{0.25f, 0.25f, 0.3f, 1} : Value{0.5f, 0.5f, 0.6f, 1});
    return w;
}

}  // namespace

const char* attrName(Attr a) { return kDefs[int(a)].id; }

bool Environment::load(const AssetSource& jar) {
    // Every namespace (data packs can add biomes): prefixes are matched below data/<namespace>/.
    const std::string biomePrefix = "worldgen/biome/", timelinePrefix = "timeline/", tagPrefix = "tags/timeline/",
                      dimPrefix = "dimension_type/";
    auto stem = [](const std::string& file, const std::string& prefix) -> std::string {
        const size_t slash = file.find('/', 5);
        if (slash == std::string::npos) return {};
        const std::string ns = file.substr(5, slash - 5), path = file.substr(slash + 1);
        if (path.rfind(prefix, 0) != 0 || path.size() < prefix.size() + 6 || path.compare(path.size() - 5, 5, ".json"))
            return {};
        const std::string s = path.substr(prefix.size(), path.size() - prefix.size() - 5);
        return s.find('/') == std::string::npos ? ns + ":" + s : std::string{};
    };
    std::unordered_map<std::string, const Timeline*> timelineByName;
    std::unordered_map<std::string, json> tags;
    std::vector<std::pair<std::string, json>> dimJson;
    for (const std::string& file : jar.list("data/", ".json")) {
        std::string name;
        json j;
        if (!(name = stem(file, biomePrefix)).empty()) {
            if (!readJson(jar, file, j)) continue;
            BiomeInfo b;
            b.name = name;
            b.temperature = j.value("temperature", 0.5f);
            b.precipitation = j.value("has_precipitation", true);
            b.frozen = j.value("temperature_modifier", std::string("none")) == "frozen";
            if (j.contains("attributes")) parseAttributes(j["attributes"], b.attributes);
            biomeIndex_[name] = int(biomes_.size());
            biomes_.push_back(std::move(b));
        } else if (!(name = stem(file, timelinePrefix)).empty()) {
            if (!readJson(jar, file, j)) continue;
            auto t = std::make_unique<Timeline>();
            t->clock = withNamespace(j.value("clock", std::string("minecraft:overworld")));
            t->period = j.value("period_ticks", 0);
            if (j.contains("tracks") && j["tracks"].is_object())
                for (const auto& [key, tj] : j["tracks"].items()) {
                    const int a = attrIndex(key);
                    if (a < 0 || !tj.contains("keyframes")) continue;
                    Track tr;
                    tr.attr = Attr(a);
                    if (tj.contains("modifier") && !parseOp(tj["modifier"].get<std::string>(), tr.op)) continue;
                    if (tj.contains("ease")) {
                        const json& ease = tj["ease"];
                        if (ease.is_string()) {
                            const std::string s = ease.get<std::string>();
                            tr.ease = s == "constant" ? 1 : 0;
                            if (s != "constant" && s != "linear") LOG("sky: easing %s treated as linear", s.c_str());
                        } else if (ease.is_object() && ease.contains("cubic_bezier") && ease["cubic_bezier"].size() == 4) {
                            tr.ease = 2;
                            for (int i = 0; i < 4; ++i) tr.bezier[i] = ease["cubic_bezier"][size_t(i)].get<float>();
                        }
                    }
                    bool ok = true;
                    for (const json& kf : tj["keyframes"]) {
                        Value v;
                        if (!kf.contains("value") || !parseArgument(kDefs[a].type, tr.op, kf["value"], v)) {
                            ok = false;
                            break;
                        }
                        tr.keys.push_back({kf.value("ticks", 0), v});
                    }
                    if (ok && !tr.keys.empty()) t->tracks.push_back(std::move(tr));
                }
            timelineByName[name] = t.get();
            timelines_.push_back(std::move(t));
        } else if (!(name = stem(file, tagPrefix)).empty()) {
            if (readJson(jar, file, j)) tags[name] = std::move(j);
        } else if (!(name = stem(file, dimPrefix)).empty()) {
            if (readJson(jar, file, j)) dimJson.push_back({name, std::move(j)});
        }
    }
    // Timeline references: an id, a #tag or a list of them.
    std::function<void(const json&, std::vector<const Timeline*>&, int)> resolve = [&](const json& ref, auto& out,
                                                                                      int depth) {
        if (depth > 8) return;
        if (ref.is_array()) {
            for (const json& r : ref) resolve(r, out, depth + 1);
        } else if (ref.is_string()) {
            const std::string s = ref.get<std::string>();
            if (!s.empty() && s[0] == '#') {
                auto t = tags.find(withNamespace(s.substr(1)));
                if (t != tags.end() && t->second.contains("values")) resolve(t->second["values"], out, depth + 1);
            } else if (auto t = timelineByName.find(withNamespace(s)); t != timelineByName.end()) {
                if (std::find(out.begin(), out.end(), t->second) == out.end()) out.push_back(t->second);
            }
        }
    };
    for (auto& [name, j] : dimJson) {
        DimensionInfo d;
        d.name = name;
        if (j.contains("attributes")) {
            parseAttributes(j["attributes"], d.attributes);
            if (const auto& a = j["attributes"]; a.is_object() && a.contains("minecraft:gameplay/piglins_zombify") &&
                                                   a["minecraft:gameplay/piglins_zombify"].is_boolean())
                d.piglinsZombify = a["minecraft:gameplay/piglins_zombify"].get<bool>();
        }
        if (j.contains("timelines")) resolve(j["timelines"], d.timelines, 0);
        d.clock = j.contains("default_clock") ? withNamespace(j["default_clock"].get<std::string>()) : std::string{};
        const std::string box = j.value("skybox", std::string("overworld"));
        d.skybox = box == "end" ? Skybox::End : box == "none" ? Skybox::None : Skybox::Overworld;
        d.hasSkylight = j.value("has_skylight", true);
        d.netherShade = j.value("cardinal_light", std::string("default")) == "nether";
        d.hasCeiling = j.value("has_ceiling", false);
        d.minY = j.value("min_y", -64);
        dimensions_.push_back(std::move(d));
    }
    LOG("sky: %zu biomes, %zu timelines, %zu dimension types from the game data", biomes_.size(), timelines_.size(),
        dimensions_.size());
    return dimension("minecraft:overworld") != nullptr;
}

const DimensionInfo* Environment::dimension(std::string_view name) const {
    for (const DimensionInfo& d : dimensions_)
        if (d.name == name) return &d;
    return nullptr;
}

int Environment::biome(std::string_view name) const {
    auto it = biomeIndex_.find(std::string(name));
    return it == biomeIndex_.end() ? -1 : it->second;
}

void Environment::evaluate(const Inputs& in, Value out[kAttrCount]) const {
    static const WeatherLayer kRain = weatherLayer(false), kThunder = weatherLayer(true);
    const DimensionInfo* dim = in.dimension;
    for (int a = 0; a < kAttrCount; ++a) {
        const Type type = kDefs[a].type;
        Value v = kDefs[a].def;
        if (dim && dim->attributes.entries[size_t(a)]) {
            const Modifier& m = *dim->attributes.entries[size_t(a)];
            v = apply(type, v, m.op, m.arg);
        }
        if (in.biomeWeights && !in.biomeWeights->empty()) {  // SpatialAttributeInterpolator
            Value acc = {0, 0, 0, 0};
            double total = 0;
            bool any = false;
            for (const auto& [b, w] : *in.biomeWeights) {
                const auto& e = biomes_[size_t(b)].attributes.entries[size_t(a)];
                any |= e.has_value();
                const Value bv = e ? apply(type, v, e->op, e->arg) : v;
                for (int c = 0; c < 4; ++c) acc[c] += float(w) * bv[c];
                total += w;
            }
            if (any && total > 0 && type != Type::Phase)
                for (int c = 0; c < 4; ++c) v[c] = acc[c] / float(total);
        }
        if (dim)
            for (const Timeline* t : dim->timelines) {
                double ticks = 0;
                if (in.clocks)
                    if (auto c = in.clocks->find(t->clock); c != in.clocks->end()) ticks = c->second;
                for (const Track& tr : t->tracks)
                    if (int(tr.attr) == a) v = apply(type, v, tr.op, sampleTrack(tr, type, t->period, ticks));
            }
        if (dim && dim->canHaveWeather()) {  // WeatherAttributes.addLayer
            const float rainOnly = in.rain - in.thunder;
            if (kRain.m[size_t(a)] && rainOnly > 0)
                v = type == Type::Phase ? v : lerpValue(rainOnly, v, apply(type, v, kRain.m[size_t(a)]->op, kRain.m[size_t(a)]->arg));
            if (kThunder.m[size_t(a)] && in.thunder > 0)
                v = lerpValue(in.thunder, v, apply(type, v, kThunder.m[size_t(a)]->op, kThunder.m[size_t(a)]->arg));
        }
        if (in.flash && Attr(a) == Attr::SkyColor) v = lerpValue(0.22f, v, {0.8f, 0.8f, 1.0f, 1.0f});
        if (in.flash && Attr(a) == Attr::SkyLightFactor) v[0] = 1.0f;
        if (!isColor(type) && type != Type::Phase) v[0] = std::clamp(v[0], kDefs[a].min, kDefs[a].max);
        out[a] = v;
    }
}

}  // namespace sky
