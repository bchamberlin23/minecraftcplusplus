#include "ui/item_props.h"

#include "net/client.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <mutex>
#include <random>
#include <unordered_map>

namespace ui {

namespace {
using json = nlohmann::json;
constexpr float kPi = 3.14159265f;

std::string stripNs(std::string_view s) {
    const size_t c = s.find(':');
    return std::string(c == std::string_view::npos ? s : s.substr(c + 1));
}
// Identifier normalization for comparisons: "foo" == "minecraft:foo".
std::string normId(std::string_view s) {
    return s.find(':') == std::string_view::npos ? "minecraft:" + std::string(s) : std::string(s);
}
float num(const json& v, float d) { return v.is_number() ? v.get<float>() : d; }
float positiveModulo(float v, float m) { return std::fmod(std::fmod(v, m) + m, m); }

net::Reader readerOf(const std::string& data) { return net::Reader(reinterpret_cast<const u8*>(data.data()), data.size()); }

struct DefaultComponents {
    fs::path dir;
    std::unordered_map<int, json> byItem;  // item id -> the report's "components" object
    std::mutex mutex;
} gDefaults;

const json& defaults(const Items& items, int item) {
    static const json kEmpty = json::object();
    std::lock_guard lock(gDefaults.mutex);
    auto it = gDefaults.byItem.find(item);
    if (it != gDefaults.byItem.end()) return it->second;
    json comps = json::object();
    if (!gDefaults.dir.empty() && item >= 0) {
        std::ifstream f(gDefaults.dir / (stripNs(items.name(item)) + ".json"));
        if (f) {
            json j = json::parse(f, nullptr, false);
            if (j.is_object() && j.contains("components") && j["components"].is_object()) comps = std::move(j["components"]);
        }
    }
    return gDefaults.byItem.emplace(item, std::move(comps)).first->second;
}

bool removed(const Items& items, const ItemStack& s, std::string_view name) {
    const int type = items.componentType(stripNs(name));
    return std::find(s.removed.begin(), s.removed.end(), type) != s.removed.end();
}

// GlobalPos (ResourceKey<Level>, BlockPos).
struct GlobalPos {
    std::string dimension;
    int pos[3] = {};
};
bool lodestoneTarget(const Items& items, const ItemStack& s, GlobalPos& out) {
    const std::string* d = findComponent(items, s, "lodestone_tracker");
    if (!d) return false;
    net::Reader r = readerOf(*d);
    if (!r.boolean()) return false;
    out.dimension = std::string(r.str());
    r.blockPos(out.pos[0], out.pos[1], out.pos[2]);
    return r.ok;
}

// The network form of a few simple components as their JSON (codec) value; defaults from the reports.
json componentJson(const Items& items, const ItemStack& s, std::string_view rawName) {
    const std::string name = stripNs(rawName);
    if (removed(items, s, name)) return json();
    const std::string* d = findComponent(items, s, name);
    if (!d) {
        const json& def = defaults(items, s.item);
        auto it = def.find("minecraft:" + name);
        return it == def.end() ? json() : *it;
    }
    net::Reader r = readerOf(*d);
    static const char* kRarity[4] = {"common", "uncommon", "rare", "epic"};
    if (name == "damage" || name == "max_damage" || name == "max_stack_size" || name == "repair_cost" ||
        name == "map_id" || name == "ominous_bottle_amplifier" || name == "enchantable")
        return r.varInt();
    if (name == "rarity") return kRarity[std::clamp(r.varInt(), 0, 3)];
    if (name == "base_color") return dyeName(r.varInt());
    if (name == "dyed_color") return i64(r.i32v());
    if (name == "item_model" || name == "tooltip_style" || name == "note_block_sound") return normId(r.str());
    if (name == "enchantment_glint_override") return r.boolean();
    if (name == "unbreakable" || name == "glider" || name == "creative_slot_lock") return json::object();
    if (name == "instrument") {
        const int id = r.varInt() - 1;
        if (id >= 0 && size_t(id) < items.instruments().size()) return items.instruments()[size_t(id)].id;
        return json();
    }
    if (name == "potion_contents") {
        json o = json::object();
        if (r.boolean()) o["potion"] = items.potionName(r.varInt());
        if (r.boolean()) o["custom_color"] = i64(r.i32v());
        return o;
    }
    if (name == "lodestone_tracker") {
        json o = json::object();
        GlobalPos p;
        if (lodestoneTarget(items, s, p)) o["target"] = {{"dimension", p.dimension}, {"pos", {p.pos[0], p.pos[1], p.pos[2]}}};
        return o;
    }
    if (name == "trim") {
        json o = json::object();
        const std::string m = trimMaterial(items, s);
        if (!m.empty()) o["material"] = m;
        return o;
    }
    return json::object();  // present, value not decoded
}

bool jsonMatches(const json& want, const json& have) {
    if (want.is_string() && have.is_string()) return normId(want.get<std::string>()) == normId(have.get<std::string>());
    if (want.is_number() && have.is_number()) return want.get<double>() == have.get<double>();
    if (want.is_object() && have.is_object()) {  // the fields the case names
        for (const auto& [k, v] : want.items()) {
            auto it = have.find(k);
            if (it == have.end() || !jsonMatches(v, *it)) return false;
        }
        return true;
    }
    if (want.is_array() && have.is_array()) {
        if (want.size() != have.size()) return false;
        for (size_t i = 0; i < want.size(); ++i)
            if (!jsonMatches(want[i], have[i])) return false;
        return true;
    }
    return want == have;
}

// MinMaxBounds.Ints: a number or {min, max}.
bool boundsMatch(const json& b, double v) {
    if (b.is_null()) return true;
    if (b.is_number()) return v == b.get<double>();
    if (!b.is_object()) return false;
    if (b.contains("min") && b["min"].is_number() && v < b["min"].get<double>()) return false;
    if (b.contains("max") && b["max"].is_number() && v > b["max"].get<double>()) return false;
    return true;
}

// A holder set in JSON: an id, a list of ids or a "#tag" (tags are not known on the client side here: no match).
bool idSetHas(const json& set, std::string_view id) {
    if (set.is_string()) return normId(set.get<std::string>()) == normId(id);
    if (set.is_array())
        for (const json& e : set)
            if (e.is_string() && normId(e.get<std::string>()) == normId(id)) return true;
    return false;
}

// DataComponentPredicate (ComponentMatches): the predicate types the item models can name, else an exact component value.
bool predicateMatches(const Items& items, const ItemStack& s, const std::string& type, const json& value) {
    const std::string t = stripNs(type);
    if (t == "damage") {
        if (!hasComponent(items, s, "damage")) return false;
        const int md = items.maxDamage(s), dmg = s.damage;
        return boundsMatch(value.value("durability", json()), md - dmg) && boundsMatch(value.value("damage", json()), dmg);
    }
    if (t == "potion_contents") {
        if (s.potion < 0) return false;
        return idSetHas(value, items.potionName(s.potion));
    }
    if (t == "trim") {
        const std::string m = trimMaterial(items, s);
        if (m.empty()) return false;
        return !value.contains("material") || idSetHas(value["material"], m);
    }
    if (t == "enchantments" || t == "stored_enchantments") {
        const auto& list = t == "enchantments" ? s.enchantList : s.storedEnchants;
        if (!value.is_array()) return false;
        for (const json& want : value) {  // each entry must match one enchantment
            bool hit = false;
            for (auto [id, level] : list) {
                const std::string name =
                    id >= 0 && size_t(id) < items.enchantments().size() ? items.enchantments()[size_t(id)].id : std::string();
                if (want.contains("enchantments") && !idSetHas(want["enchantments"], name)) continue;
                if (!boundsMatch(want.value("levels", json()), level)) continue;
                hit = true;
            }
            if (!hit) return false;
        }
        return true;
    }
    // A component type: the component's value equals `value`.
    const json have = componentJson(items, s, t);
    return !have.is_null() && jsonMatches(value, have);
}

// NeedleDirectionHelper.Wobbler.
struct Wobbler {
    float rotation = 0, delta = 0;
    i64 last = INT64_MIN;
    bool wobble = true;
    float factor = 0.8f;
    bool shouldUpdate(i64 tick) const { return !wobble || last != tick; }
    void update(i64 tick, float target) {
        if (!wobble) {
            rotation = target;
            return;
        }
        last = tick;
        const float d = positiveModulo(target - rotation + 0.5f, 1.0f) - 0.5f;
        delta += d * 0.1f;
        delta *= factor;
        rotation = positiveModulo(rotation + delta, 1.0f);
    }
};
struct NeedleState {
    Wobbler wobbler, noTarget;
    std::mt19937 random{std::random_device{}()};
    float next() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(random); }
};
std::unordered_map<const json*, NeedleState> gNeedles;
std::mutex gNeedleMutex;

NeedleState& needle(const json& node, float factor) {
    auto [it, fresh] = gNeedles.try_emplace(&node);
    if (fresh) {
        const bool wobble = node.value("wobble", true);
        for (Wobbler* w : {&it->second.wobbler, &it->second.noTarget}) w->wobble = wobble, w->factor = factor;
    }
    return it->second;
}

// CompassAngleState.calculate (NeedleDirectionHelper.get: 0 without an owner or level).
float compassAngle(const Items& items, const json& node, const ItemStack& s, const ItemContext& c) {
    const ItemWorld& w = itemWorld();
    if (!c.owner.present || !w.present) return 0;
    std::lock_guard lock(gNeedleMutex);
    NeedleState& st = needle(node, 0.8f);
    const std::string target = stripNs(node.value("target", "none"));
    GlobalPos pos;
    bool has = false;
    if (target == "lodestone") has = lodestoneTarget(items, s, pos);
    else if (target == "spawn" && w.hasSpawn) has = true, pos.dimension = w.spawnDimension, std::copy(w.spawn, w.spawn + 3, pos.pos);
    else if (target == "recovery" && c.owner.player && c.owner.local && w.hasDeath)
        has = true, pos.dimension = w.deathDimension, std::copy(w.death, w.death + 3, pos.pos);
    const double tx = pos.pos[0] + 0.5, ty = pos.pos[1] + 0.5, tz = pos.pos[2] + 0.5;
    const double dx = tx - c.owner.pos[0], dy = ty - c.owner.pos[1], dz = tz - c.owner.pos[2];
    const bool valid = has && normId(pos.dimension) == normId(w.dimension) && !(dx * dx + dy * dy + dz * dz < 1.0E-5);
    if (!valid) {  // getRandomlySpinningRotation
        if (st.noTarget.shouldUpdate(w.gameTime)) st.noTarget.update(w.gameTime, st.next());
        const float t = st.noTarget.rotation + float(i32(u32(c.seed) * 1327217883u)) / 2.1474836E9f;
        return positiveModulo(t, 1.0f);
    }
    const float angle = float(std::atan2(dz, dx) / (2.0 * 3.141592653589793));
    const float ownerYaw = positiveModulo(c.owner.yaw / 360.0f, 1.0f);
    float t;
    if (c.owner.player && c.owner.local && w.ticking) {
        if (st.wobbler.shouldUpdate(w.gameTime)) st.wobbler.update(w.gameTime, 0.5f - (ownerYaw - 0.25f));
        t = angle + st.wobbler.rotation;
    } else {
        t = 0.5f - (ownerYaw - 0.25f - angle);
    }
    return positiveModulo(t, 1.0f);
}

// Time.calculate (daytime / moon_phase / random sources).
float timeValue(const json& node, const ItemContext& c) {
    const ItemWorld& w = itemWorld();
    if (!c.owner.present || !w.present) return 0;
    std::lock_guard lock(gNeedleMutex);
    NeedleState& st = needle(node, 0.9f);
    const std::string source = stripNs(node.value("source", "daytime"));
    float target;
    if (source == "daytime") target = w.sunAngle / 360.0f;
    else if (source == "moon_phase") target = float(w.moonPhase) / 8.0f;
    else target = st.next();
    if (st.wobbler.shouldUpdate(w.gameTime)) st.wobbler.update(w.gameTime, target);
    return st.wobbler.rotation;
}

// LocalTime: java.text.SimpleDateFormat letters (root locale) for the local, or a named, time zone.
std::string localTime(const json& node) {
    const std::string pattern = node.value("pattern", "");
    using namespace std::chrono;
    const auto now = system_clock::now();
    std::tm tm{};
    long offsetMinutes = 0;
    std::string zoneName;
    bool zoned = false;
    if (node.contains("time_zone") && node["time_zone"].is_string()) {
        try {
            const time_zone* z = locate_zone(node["time_zone"].get<std::string>());
            const sys_info info = z->get_info(now);
            const auto local = now + info.offset;
            const std::time_t t = system_clock::to_time_t(time_point_cast<system_clock::duration>(local));
            gmtime_s(&tm, &t);
            offsetMinutes = long(duration_cast<minutes>(info.offset).count());
            zoneName = info.abbrev;
            zoned = true;
        } catch (...) {
        }
    }
    if (!zoned) {
        const std::time_t t = system_clock::to_time_t(now);
        localtime_s(&tm, &t);
        std::tm utc{};
        gmtime_s(&utc, &t);
        offsetMinutes = long(std::difftime(std::mktime(&tm), std::mktime(&utc)) / 60);
        zoneName = "";
    }
    static const char* kMonths[12] = {"January", "February", "March",     "April",   "May",      "June",
                                      "July",    "August",   "September", "October", "November", "December"};
    static const char* kDays[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    const int ms = int(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::string out;
    auto pad = [&](long v, size_t n) {
        std::string s = std::to_string(v);
        while (s.size() < n) s.insert(s.begin(), '0');
        out += s;
    };
    for (size_t i = 0; i < pattern.size();) {
        const char ch = pattern[i];
        if (ch == '\'') {  // quoted text; '' is a quote
            size_t j = i + 1;
            if (j < pattern.size() && pattern[j] == '\'') {
                out += '\'';
                i += 2;
                continue;
            }
            while (j < pattern.size()) {
                if (pattern[j] == '\'') {
                    if (j + 1 < pattern.size() && pattern[j + 1] == '\'') {
                        out += '\'';
                        j += 2;
                        continue;
                    }
                    break;
                }
                out += pattern[j++];
            }
            i = j + 1;
            continue;
        }
        if (!std::isalpha(u8(ch))) {
            out += ch;
            ++i;
            continue;
        }
        size_t n = 0;
        while (i + n < pattern.size() && pattern[i + n] == ch) ++n;
        i += n;
        const int hour = tm.tm_hour;
        switch (ch) {
            case 'G': out += "AD"; break;
            case 'y': case 'Y': case 'u':
                if (ch == 'u') pad(tm.tm_wday == 0 ? 7 : tm.tm_wday, n);
                else if (n == 2) pad((tm.tm_year + 1900) % 100, 2);
                else pad(tm.tm_year + 1900, n);
                break;
            case 'M': case 'L':
                if (n >= 4) out += kMonths[tm.tm_mon];
                else if (n == 3) out += std::string(kMonths[tm.tm_mon]).substr(0, 3);
                else pad(tm.tm_mon + 1, n);
                break;
            case 'd': pad(tm.tm_mday, n); break;
            case 'D': pad(tm.tm_yday + 1, n); break;
            case 'F': pad((tm.tm_mday - 1) / 7 + 1, n); break;
            case 'w': pad((tm.tm_yday + 7 - tm.tm_wday) / 7 + 1, n); break;
            case 'W': pad((tm.tm_mday + 6 - tm.tm_wday) / 7 + 1, n); break;
            case 'E': out += n >= 4 ? std::string(kDays[tm.tm_wday]) : std::string(kDays[tm.tm_wday]).substr(0, 3); break;
            case 'a': out += hour < 12 ? "AM" : "PM"; break;
            case 'H': pad(hour, n); break;
            case 'k': pad(hour == 0 ? 24 : hour, n); break;
            case 'K': pad(hour % 12, n); break;
            case 'h': pad(hour % 12 == 0 ? 12 : hour % 12, n); break;
            case 'm': pad(tm.tm_min, n); break;
            case 's': pad(tm.tm_sec, n); break;
            case 'S': pad(ms, n); break;
            case 'z': case 'Z': case 'X': {
                if (ch == 'z' && !zoneName.empty()) {
                    out += zoneName;
                    break;
                }
                const long a = std::abs(offsetMinutes);
                if (ch == 'X' && offsetMinutes == 0) {
                    out += 'Z';
                    break;
                }
                if (ch == 'z') out += "GMT";
                out += offsetMinutes < 0 ? '-' : '+';
                pad(a / 60, 2);
                if (ch == 'z' || (ch == 'X' && n >= 3)) out += ':';
                if (ch != 'X' || n >= 2) pad(a % 60, 2);
                break;
            }
            default: break;
        }
    }
    return out;
}

// A select property's value (null: none, the fallback applies).
json selectValue(const Items& items, const json& n, const ItemStack& s, const ItemContext& c) {
    const std::string prop = stripNs(n.value("property", ""));
    const ItemOwner& o = c.owner;
    if (prop == "display_context") return itemDisplayName(c.display);
    if (prop == "main_hand") return o.present && o.living ? json(o.mainArm == 0 ? "left" : "right") : json();
    if (prop == "charge_type") {
        const std::string* d = findComponent(items, s, "charged_projectiles");
        if (!d) return "none";
        net::Reader r = readerOf(*d);
        const i32 count = r.varInt();
        if (count <= 0) return "none";
        static const int rocket = items.id("firework_rocket");
        for (i32 i = 0; i < count && r.ok; ++i) {
            ItemStack p;
            items.readTemplate(r, p);
            if (p.item == rocket) return "rocket";
        }
        return "arrow";
    }
    if (prop == "trim_material") {
        const std::string m = trimMaterial(items, s);
        return m.empty() ? json() : json(m);
    }
    if (prop == "block_state") {
        const std::string* d = findComponent(items, s, "block_state");
        if (!d) return json();
        const std::string want = n.value("block_state_property", "");
        net::Reader r = readerOf(*d);
        const i32 count = r.varInt();
        for (i32 i = 0; i < count && r.ok; ++i) {
            const std::string k(r.str()), v(r.str());
            if (k == want) return v;
        }
        return json();
    }
    if (prop == "local_time") return localTime(n);
    if (prop == "context_entity_type") return o.present && o.living && !o.type.empty() ? json(normId(o.type)) : json();
    if (prop == "context_dimension") return itemWorld().present ? json(normId(itemWorld().dimension)) : json();
    if (prop == "custom_model_data") {
        CustomModelData cmd;
        const size_t index = size_t(std::max(0, n.value("index", 0)));
        if (customModelData(items, s, cmd) && index < cmd.strings.size()) return cmd.strings[index];
        return json();
    }
    if (prop == "component") return componentJson(items, s, n.value("component", ""));
    return json();
}

bool conditionValue(const Items& items, const json& n, const ItemStack& s, const ItemContext& c) {
    const std::string prop = stripNs(n.value("property", ""));
    const ItemOwner& o = c.owner;
    if (prop == "using_item") return o.present && o.living && o.usingThis;
    if (prop == "broken" || prop == "damaged") {
        // isDamageableItem: max_damage and damage present, not unbreakable.
        const int md = items.maxDamage(s);
        if (md <= 0 || hasComponent(items, s, "unbreakable")) return false;
        const int dmg = std::clamp(s.damage, 0, md);
        return prop == "damaged" ? dmg > 0 : dmg >= md - 1;
    }
    if (prop == "fishing_rod/cast") return o.present && o.player && o.fishing;
    if (prop == "has_component") return hasComponent(items, s, n.value("component", ""), n.value("ignore_default", false));
    if (prop == "bundle/has_selected_item") {
        ItemStack sel;
        return bundleSelectedItem(items, s, sel);
    }
    if (prop == "selected") return o.present && o.local && o.selected;
    if (prop == "carried") return o.present && o.local && o.carried;
    if (prop == "extended_view") return c.display == ItemDisplay::Gui && itemWorld().shiftDown;
    if (prop == "keybind_down") {
        const ItemWorld& w = itemWorld();
        return w.keyDown && w.keyDown(n.value("keybind", ""));
    }
    if (prop == "view_entity") return o.present && o.viewEntity;
    if (prop == "custom_model_data") {
        CustomModelData cmd;
        const size_t index = size_t(std::max(0, n.value("index", 0)));
        return customModelData(items, s, cmd) && index < cmd.flags.size() && cmd.flags[index] != 0;
    }
    if (prop == "component") return predicateMatches(items, s, n.value("predicate", ""), n.value("value", json()));
    return false;
}

float rangeValue(const Items& items, const json& n, const ItemStack& s, const ItemContext& c) {
    const std::string prop = stripNs(n.value("property", ""));
    const ItemOwner& o = c.owner;
    if (prop == "use_duration") {
        if (!(o.present && o.living && o.usingThis)) return 0;
        return n.value("remaining", false) ? o.remaining : o.useTicks;
    }
    if (prop == "crossbow/pull") {
        if (!(o.present && o.living) || selectValue(items, json{{"property", "charge_type"}}, s, c) != "none") return 0;
        return o.usingThis ? o.useTicks / std::max(1.0f, o.chargeDuration) : 0.0f;
    }
    if (prop == "use_cycle") {
        const float period = num(n.value("period", json()), 1.0f);
        return o.present && o.living && o.usingThis && period > 0 ? std::fmod(o.remaining, period) : 0.0f;
    }
    if (prop == "cooldown") return o.present && o.player ? o.cooldown : 0.0f;
    if (prop == "count") {
        const float count = float(s.count), max = float(std::max(1, items.maxStack(s)));
        return n.value("normalize", true) ? std::clamp(count / max, 0.0f, 1.0f) : std::clamp(count, 0.0f, max);
    }
    if (prop == "damage") {
        const float md = float(items.maxDamage(s)), dmg = std::clamp(float(s.damage), 0.0f, std::max(md, 0.0f));
        if (n.value("normalize", true)) return md > 0 ? std::clamp(dmg / md, 0.0f, 1.0f) : std::nanf("");
        return std::clamp(dmg, 0.0f, md);
    }
    if (prop == "custom_model_data") {
        CustomModelData cmd;
        const size_t index = size_t(std::max(0, n.value("index", 0)));
        return customModelData(items, s, cmd) && index < cmd.floats.size() ? cmd.floats[index] : 0.0f;
    }
    if (prop == "bundle/fullness") return bundleFullness(items, s);
    if (prop == "compass") return compassAngle(items, n, s, c);
    if (prop == "time") return timeValue(n, c);
    return 0;
}

const json* modelOf(const json& j) {
    if (!j.is_object()) return nullptr;
    auto it = j.find("model");
    return it == j.end() ? nullptr : &*it;
}

// Range dispatch entries sorted by threshold (stable, like RangeSelectItemModel.Unbaked.bake), per node.
std::unordered_map<const json*, std::vector<std::pair<float, const json*>>> gRanges;
std::mutex gRangeMutex;
}  // namespace

const char* itemDisplayName(ItemDisplay d) {
    static const char* kNames[10] = {"none", "thirdperson_lefthand", "thirdperson_righthand", "firstperson_lefthand",
                                     "firstperson_righthand", "head", "gui", "ground", "fixed", "on_shelf"};
    return kNames[std::min(size_t(d), size_t(9))];
}

ItemWorld& itemWorld() {
    static ItemWorld w;
    return w;
}

void setItemComponentReports(const fs::path& dir) {
    std::lock_guard lock(gDefaults.mutex);
    gDefaults.dir = dir;
    gDefaults.byItem.clear();
}

namespace {
// Packet values for itemWorld(), handed from the network thread to the main thread (updateItemWorld).
struct PendingWorld {
    std::mutex mutex;
    bool level = false, spawn = false;
    int localId = -1;
    std::string dimension;
    bool hasDeath = false;
    GlobalPos death, spawnPos;
} gPending;
}  // namespace

void attachItemWorld(net::Client& client) {
    auto spawnInfo = [](net::Reader& r, bool login) {  // ClientboundLoginPacket / ClientboundRespawnPacket
        int id = -1;
        if (login) {
            id = r.i32v();
            r.boolean();
            const i32 levels = r.varInt();
            for (i32 i = 0; i < levels && r.ok; ++i) r.str();
            r.varInt(), r.varInt(), r.varInt();
            r.boolean(), r.boolean(), r.boolean();
        }
        // CommonPlayerSpawnInfo: dimension type, dimension, seed, game type, previous game type, debug, flat, last death.
        r.varInt();
        const std::string dimension(r.str());
        r.i64v();
        r.varInt(), r.varInt();
        r.boolean(), r.boolean();
        GlobalPos death;
        const bool hasDeath = r.boolean();
        if (hasDeath) {
            death.dimension = std::string(r.str());
            r.blockPos(death.pos[0], death.pos[1], death.pos[2]);
        }
        if (!r.ok) return;
        std::lock_guard lock(gPending.mutex);
        gPending.level = true;
        if (login) gPending.localId = id;
        gPending.dimension = dimension;
        gPending.hasDeath = hasDeath;
        gPending.death = death;
    };
    client.onPlayPeek("login", [spawnInfo](net::Reader& r) { spawnInfo(r, true); });
    client.onPlayPeek("respawn", [spawnInfo](net::Reader& r) { spawnInfo(r, false); });
    client.onPlayPeek("set_default_spawn_position", [](net::Reader& r) {  // LevelData.RespawnData: GlobalPos, yaw, pitch
        GlobalPos p;
        p.dimension = std::string(r.str());
        r.blockPos(p.pos[0], p.pos[1], p.pos[2]);
        if (!r.ok) return;
        std::lock_guard lock(gPending.mutex);
        gPending.spawn = true;
        gPending.spawnPos = p;
    });
}

void updateItemWorld() {
    std::lock_guard lock(gPending.mutex);
    ItemWorld& w = itemWorld();
    if (gPending.level) {
        gPending.level = false;
        w.present = true;
        if (gPending.localId >= 0) w.localId = gPending.localId;
        w.dimension = gPending.dimension;
        w.hasDeath = gPending.hasDeath;
        w.deathDimension = gPending.death.dimension;
        std::copy(gPending.death.pos, gPending.death.pos + 3, w.death);
    }
    if (gPending.spawn) {
        gPending.spawn = false;
        w.hasSpawn = true;
        w.spawnDimension = gPending.spawnPos.dimension;
        std::copy(gPending.spawnPos.pos, gPending.spawnPos.pos + 3, w.spawn);
    }
}

// --- components -----------------------------------------------------------------------------------------------

const std::string* findComponent(const Items& items, const ItemStack& s, std::string_view name) {
    const int type = items.componentType(stripNs(name));
    if (type < 0) return nullptr;
    for (const ItemStack::Component& c : s.added)
        if (c.type == type) return &c.data;
    return nullptr;
}

bool hasComponent(const Items& items, const ItemStack& s, std::string_view rawName, bool ignoreDefault) {
    const std::string name = stripNs(rawName);
    if (findComponent(items, s, name)) return true;
    if (ignoreDefault || removed(items, s, name)) return false;
    return defaults(items, s.item).contains("minecraft:" + name);
}

std::string itemModelId(const Items& items, const ItemStack& s) {
    if (const std::string* d = findComponent(items, s, "item_model")) {
        net::Reader r = readerOf(*d);
        const std::string id(r.str());
        if (r.ok) return normId(id);
    }
    if (removed(items, s, "item_model")) return "";
    const json& def = defaults(items, s.item);
    if (auto it = def.find("minecraft:item_model"); it != def.end() && it->is_string()) return normId(it->get<std::string>());
    return items.name(s.item);
}

bool customModelData(const Items& items, const ItemStack& s, CustomModelData& out) {
    const std::string* d = findComponent(items, s, "custom_model_data");
    if (!d) return false;
    net::Reader r = readerOf(*d);
    i32 n = r.varInt();
    for (i32 i = 0; i < n && r.ok; ++i) out.floats.push_back(r.f32());
    n = r.varInt();
    for (i32 i = 0; i < n && r.ok; ++i) out.flags.push_back(r.boolean());
    n = r.varInt();
    for (i32 i = 0; i < n && r.ok; ++i) out.strings.emplace_back(r.str());
    n = r.varInt();
    for (i32 i = 0; i < n && r.ok; ++i) out.colors.push_back(r.i32v());
    return r.ok;
}

bool bundleContents(const Items& items, const ItemStack& s, std::vector<ItemStack>& out) {
    const std::string* d = findComponent(items, s, "bundle_contents");
    if (!d) return false;
    net::Reader r = readerOf(*d);
    const i32 n = r.varInt();
    for (i32 i = 0; i < n && r.ok; ++i) {
        ItemStack e;
        if (!items.readTemplate(r, e)) break;
        out.push_back(std::move(e));
    }
    return r.ok;
}

float bundleFullness(const Items& items, const ItemStack& s) {
    // BundleContents.computeContentWeight: count / max stack per stack; nested bundles weigh their contents + 1/16,
    // stacks with bees 1.
    std::function<double(const ItemStack&, int)> weight = [&](const ItemStack& b, int depth) -> double {
        std::vector<ItemStack> content;
        if (depth > 8 || !bundleContents(items, b, content)) return 0.0;
        double w = 0;
        for (const ItemStack& e : content) {
            double each;
            if (findComponent(items, e, "bundle_contents")) each = weight(e, depth + 1) + 1.0 / 16.0;
            else if (const std::string* bees = findComponent(items, e, "bees"); bees && bees->size() > 0 && (*bees)[0] != 0)
                each = 1.0;
            else each = 1.0 / double(std::max(1, items.maxStack(e)));
            w += each * e.count;
        }
        return w;
    };
    return float(weight(s, 0));
}

bool bundleSelectedItem(const Items& items, const ItemStack& s, ItemStack& out) {
    if (s.bundleSelected < 0) return false;
    std::vector<ItemStack> content;
    if (!bundleContents(items, s, content) || size_t(s.bundleSelected) >= content.size()) return false;
    out = std::move(content[size_t(s.bundleSelected)]);
    return true;
}

std::vector<BannerLayer> bannerPatterns(const Items& items, const ItemStack& s) {
    std::vector<BannerLayer> out;
    const std::string* d = findComponent(items, s, "banner_patterns");
    if (!d) return out;
    net::Reader r = readerOf(*d);
    const i32 n = r.varInt();
    for (i32 i = 0; i < n && r.ok; ++i) {
        BannerLayer l;
        const i32 holder = r.varInt();
        if (holder == 0) {  // inline BannerPattern: asset id, translation key
            l.pattern = normId(r.str());
            r.str();
        } else if (size_t(holder - 1) < items.bannerPatterns().size()) {
            l.pattern = items.bannerPatterns()[size_t(holder - 1)];  // vanilla patterns use their id as the asset id
        }
        l.dye = r.varInt();
        if (r.ok && !l.pattern.empty()) out.push_back(std::move(l));
    }
    return out;
}

int baseColor(const Items& items, const ItemStack& s) {
    const std::string* d = findComponent(items, s, "base_color");
    if (!d) return -1;
    net::Reader r = readerOf(*d);
    const int dye = r.varInt();
    return r.ok ? dye : -1;
}

bool profile(const Items& items, const ItemStack& s, ProfileInfo& out) {
    const std::string* d = findComponent(items, s, "profile");
    if (!d) return false;
    net::Reader r = readerOf(*d);
    auto properties = [&] {
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {
            const std::string name(r.str()), value(r.str());
            if (r.boolean()) r.str();  // signature
            if (name == "textures") out.textures = value;
        }
    };
    auto uuid = [&] {
        const std::string_view b = r.bytes(16);
        if (b.size() == 16) std::memcpy(out.uuid, b.data(), 16), out.hasId = true;
    };
    if (r.boolean()) {  // a complete GameProfile
        uuid();
        out.name = std::string(r.str());
        properties();
    } else {  // partial: optional name, optional id, properties
        if (r.boolean()) out.name = std::string(r.str());
        if (r.boolean()) uuid();
        properties();
    }
    // PlayerSkin.Patch: body, cape, elytra texture ids, model.
    if (r.boolean()) out.skinTexture = stripNs(r.str());
    if (r.boolean()) r.str();
    if (r.boolean()) r.str();
    if (r.boolean()) out.model = r.boolean() ? 1 : 0;
    return r.ok;
}

void potDecorations(const Items& items, const ItemStack& s, int out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = -1;
    const std::string* d = findComponent(items, s, "pot_decorations");
    if (!d) return;
    net::Reader r = readerOf(*d);
    for (int i = 0; i < 4 && r.ok; ++i)
        if (r.boolean()) {
            ItemStack e;
            if (!items.readTemplate(r, e)) return;
            out[i] = e.item;
        }
}

u32 potionColor(const Items& items, const ItemStack& s, u32 fallback) {
    const std::string* d = findComponent(items, s, "potion_contents");
    if (!d) return fallback;
    static const std::unordered_map<std::string, u32> kEffectColors = {
        {"speed", 0x33EBFF}, {"slowness", 0x8BAFE0}, {"haste", 0xD9C043}, {"mining_fatigue", 0x4A4217},
        {"strength", 0xFFC700}, {"instant_health", 0xF82423}, {"instant_damage", 0xA9656A}, {"jump_boost", 0xFDFF84},
        {"nausea", 0x551D4A}, {"regeneration", 0xCD5CAB}, {"resistance", 0x9146F0}, {"fire_resistance", 0xFF9900},
        {"water_breathing", 0x98DAC0}, {"invisibility", 0xF6F6F6}, {"blindness", 0x1F1F23}, {"night_vision", 0xC2FF66},
        {"hunger", 0x587653}, {"weakness", 0x484D48}, {"poison", 0x87A363}, {"wither", 0x736156},
        {"health_boost", 0xF87D23}, {"absorption", 0x2552A5}, {"saturation", 0xF82423}, {"glowing", 0x94A061},
        {"levitation", 0xCEFFFF}, {"luck", 0x59C106}, {"unluck", 0xC0A44D}, {"slow_falling", 0xF3CFB9},
        {"conduit_power", 0x1DC2D1}, {"dolphins_grace", 0x88A3BE}, {"bad_omen", 0x0B6138},
        {"hero_of_the_village", 0x44FF44}, {"darkness", 0x292721}, {"trial_omen", 0x16A6A6}, {"raid_omen", 0xDE4058},
        {"wind_charged", 0xBDC9FF}, {"weaving", 0x78695A}, {"oozing", 0x99FFA3}, {"infested", 0x8C9B8C},
        {"breath_of_the_nautilus", 0x00FFEE}};
    struct E {
        const char* effect;
        int amplifier;
    };
    // Potions: their effects (Potions.java).
    static const std::unordered_map<std::string, std::vector<E>> kPotions = {
        {"night_vision", {{"night_vision", 0}}}, {"long_night_vision", {{"night_vision", 0}}},
        {"invisibility", {{"invisibility", 0}}}, {"long_invisibility", {{"invisibility", 0}}},
        {"leaping", {{"jump_boost", 0}}}, {"long_leaping", {{"jump_boost", 0}}}, {"strong_leaping", {{"jump_boost", 1}}},
        {"fire_resistance", {{"fire_resistance", 0}}}, {"long_fire_resistance", {{"fire_resistance", 0}}},
        {"swiftness", {{"speed", 0}}}, {"long_swiftness", {{"speed", 0}}}, {"strong_swiftness", {{"speed", 1}}},
        {"slowness", {{"slowness", 0}}}, {"long_slowness", {{"slowness", 0}}}, {"strong_slowness", {{"slowness", 3}}},
        {"turtle_master", {{"slowness", 3}, {"resistance", 2}}}, {"long_turtle_master", {{"slowness", 3}, {"resistance", 2}}},
        {"strong_turtle_master", {{"slowness", 5}, {"resistance", 3}}},
        {"water_breathing", {{"water_breathing", 0}}}, {"long_water_breathing", {{"water_breathing", 0}}},
        {"healing", {{"instant_health", 0}}}, {"strong_healing", {{"instant_health", 1}}},
        {"harming", {{"instant_damage", 0}}}, {"strong_harming", {{"instant_damage", 1}}},
        {"poison", {{"poison", 0}}}, {"long_poison", {{"poison", 0}}}, {"strong_poison", {{"poison", 1}}},
        {"regeneration", {{"regeneration", 0}}}, {"long_regeneration", {{"regeneration", 0}}},
        {"strong_regeneration", {{"regeneration", 1}}}, {"strength", {{"strength", 0}}},
        {"long_strength", {{"strength", 0}}}, {"strong_strength", {{"strength", 1}}}, {"weakness", {{"weakness", 0}}},
        {"long_weakness", {{"weakness", 0}}}, {"luck", {{"luck", 0}}}, {"slow_falling", {{"slow_falling", 0}}},
        {"long_slow_falling", {{"slow_falling", 0}}}, {"wind_charged", {{"wind_charged", 0}}},
        {"weaving", {{"weaving", 0}}}, {"oozing", {{"oozing", 0}}}, {"infested", {{"infested", 0}}}};
    net::Reader r = readerOf(*d);
    int potion = -1;
    if (r.boolean()) potion = r.varInt();
    if (r.boolean()) return u32(r.i32v()) & 0xFFFFFF;  // custom colour
    // getAllEffects: the potion's, then the custom effects; visible ones weighted by amplifier + 1.
    int red = 0, green = 0, blue = 0, total = 0;
    auto add = [&](const std::string& effect, int amplifier, bool visible) {
        auto it = kEffectColors.find(effect);
        if (!visible || it == kEffectColors.end()) return;
        const int a = amplifier + 1;
        red += a * int((it->second >> 16) & 255), green += a * int((it->second >> 8) & 255), blue += a * int(it->second & 255);
        total += a;
    };
    if (potion >= 0)
        if (auto it = kPotions.find(stripNs(items.potionName(potion))); it != kPotions.end())
            for (const E& e : it->second) add(e.effect, e.amplifier, true);
    std::function<void()> details = [&] {  // MobEffectInstance.Details: amplifier, duration, ambient, particles, icon
        r.varInt(), r.varInt(), r.boolean(), r.boolean(), r.boolean();
        if (r.boolean()) details();
    };
    const i32 n = r.varInt();
    for (i32 i = 0; i < n && r.ok; ++i) {
        const int effect = r.varInt();
        const int amplifier = r.varInt();
        r.varInt();
        r.boolean();
        const bool visible = r.boolean();
        r.boolean();
        if (r.boolean()) details();
        if (r.ok) add(stripNs(items.mobEffectName(effect)), amplifier, visible);
    }
    if (total == 0) return fallback;
    return u32(red / total) << 16 | u32(green / total) << 8 | u32(blue / total);
}

std::vector<u32> fireworkColors(const Items& items, const ItemStack& s) {
    std::vector<u32> out;
    const std::string* d = findComponent(items, s, "firework_explosion");
    if (!d) return out;
    net::Reader r = readerOf(*d);
    r.varInt();  // shape
    const i32 n = r.varInt();
    for (i32 i = 0; i < n && r.ok; ++i) out.push_back(r.u32v() & 0xFFFFFF);
    if (!r.ok) out.clear();
    return out;
}

std::string trimMaterial(const Items& items, const ItemStack& s) {
    const std::string* d = findComponent(items, s, "trim");
    if (!d) return "";
    net::Reader r = readerOf(*d);
    const i32 holder = r.varInt();
    if (holder <= 0 || size_t(holder - 1) >= items.trimMaterials().size()) return "";  // inline materials have no key
    return items.trimMaterials()[size_t(holder - 1)];
}

int mapId(const Items& items, const ItemStack& s) {
    const std::string* d = findComponent(items, s, "map_id");
    if (!d) return -1;
    net::Reader r = readerOf(*d);
    const int id = r.varInt();
    return r.ok ? id : -1;
}

u32 dyeDiffuse(int dye) {
    static const u32 kDye[16] = {0xF9FFFE, 0xF9801D, 0xC74EBD, 0x3AB3DA, 0xFED83D, 0x80C71F, 0xF38BAA, 0x474F52,
                                 0x9D9D97, 0x169C9C, 0x8932B8, 0x3C44AA, 0x835432, 0x5E7C16, 0xB02E26, 0x1D1D21};
    return kDye[std::clamp(dye, 0, 15)];
}

const char* dyeName(int dye) {
    static const char* kNames[16] = {"white", "orange", "magenta", "light_blue", "yellow", "lime",  "pink",  "gray",
                                     "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};
    return kNames[std::clamp(dye, 0, 15)];
}

// --- evaluation -------------------------------------------------------------------------------------------------

const json* chooseItemModel(const Items& items, const json& n, const ItemStack& s, const ItemContext& c) {
    const std::string type = stripNs(n.value("type", ""));
    auto field = [&](const char* k) -> const json* {
        auto it = n.find(k);
        return it == n.end() ? nullptr : &*it;
    };
    if (type == "condition") return field(conditionValue(items, n, s, c) ? "on_true" : "on_false");
    if (type == "select") {
        const json value = selectValue(items, n, s, c);
        if (!value.is_null())
            if (const json* cases = field("cases"); cases && cases->is_array())
                for (const json& cs : *cases) {
                    const json& when = cs.contains("when") ? cs["when"] : json();
                    // Values compare equal (component objects: the fields the case names).
                    bool hit = false;
                    if (when.is_array())
                        for (const json& w : when) hit |= jsonMatches(w, value);
                    else hit = jsonMatches(when, value);
                    if (hit) return modelOf(cs);
                }
        return field("fallback");
    }
    if (type == "range_dispatch") {
        const float value = rangeValue(items, n, s, c) * num(n.value("scale", json()), 1.0f);
        if (std::isnan(value)) return field("fallback");
        const std::vector<std::pair<float, const json*>>* entries;
        {
            std::lock_guard lock(gRangeMutex);
            auto it = gRanges.find(&n);
            if (it == gRanges.end()) {
                std::vector<std::pair<float, const json*>> sorted;
                if (const json* e = field("entries"); e && e->is_array())
                    for (const json& entry : *e)
                        if (const json* m = modelOf(entry)) sorted.emplace_back(num(entry.value("threshold", json()), 0), m);
                std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                it = gRanges.emplace(&n, std::move(sorted)).first;
            }
            entries = &it->second;
        }
        int index = -1;  // lastIndexLessOrEqual
        for (size_t i = 0; i < entries->size(); ++i) {
            if ((*entries)[i].first > value) break;
            index = int(i);
        }
        return index < 0 ? field("fallback") : (*entries)[size_t(index)].second;
    }
    return nullptr;
}

u32 itemTint(const Items& items, const json& t, const ItemStack& s, const ItemContext& c,
             const std::function<u32(bool foliage, float temperature, float downfall)>& colormap) {
    const std::string type = stripNs(t.value("type", ""));
    auto def = [&](const char* k, u32 d) { return t.contains(k) && t[k].is_number() ? u32(t[k].get<i64>()) : d; };
    auto opaque = [](u32 rgb) { return 0xFF000000u | (rgb & 0xFFFFFF); };
    if (type == "constant") return opaque(def("value", 0xFFFFFFFF));
    if (type == "dye") return s.dyedColor >= 0 ? opaque(u32(s.dyedColor)) : def("default", 0xFFFFFFFF);
    if (type == "grass" || type == "foliage")
        return colormap(type == "foliage", num(t.value("temperature", json()), 0.5f), num(t.value("downfall", json()), 1.0f));
    if (type == "potion") return opaque(potionColor(items, s, def("default", u32(-13083194))));
    if (type == "firework") {
        const std::vector<u32> colors = fireworkColors(items, s);
        if (colors.empty()) return def("default", u32(-7697782));
        if (colors.size() == 1) return opaque(colors[0]);
        u32 r = 0, g = 0, b = 0;
        for (u32 col : colors) r += (col >> 16) & 255, g += (col >> 8) & 255, b += col & 255;
        const u32 n = u32(colors.size());
        return 0xFF000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
    }
    if (type == "team") return opaque(c.owner.present && c.owner.teamColor >= 0 ? u32(c.owner.teamColor) : def("default", 0xFFFFFF));
    if (type == "custom_model_data") {
        CustomModelData cmd;
        const size_t index = size_t(std::max(0, t.value("index", 0)));
        if (customModelData(items, s, cmd) && index < cmd.colors.size()) return opaque(u32(cmd.colors[index]));
        return opaque(def("default", 0xFFFFFF));
    }
    return def("default", 0xFFFFFFFF);
}

}  // namespace ui
