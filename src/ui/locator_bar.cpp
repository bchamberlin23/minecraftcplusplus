// Waypoints and the locator bar (ClientWaypointManager, LocatorBar): the server tracks other players (and anything
// with a waypoint transmit range) as a block position, a chunk or an azimuth; the bar over the hotbar shows each one
// within 60 degrees of the view as a dot of its style's sprite for the distance, tinted with its colour (else a colour
// from its id), with an arrow when it is above or below the view.
#include "ui/locator_bar.h"

#include "ui/modules.h"
#include "ui/ui_state.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace ui {

namespace {

constexpr float kPi = 3.14159265f;

struct Waypoint {
    bool uuid = false;
    std::string id;  // 16 UUID bytes or the name
    std::string style = "minecraft:default";
    bool hasColor = false;
    u32 color = 0;  // 0xRRGGBB
    int type = 0;   // TrackedWaypoint.Type: 0 empty, 1 vec3i, 2 chunk, 3 azimuth
    int x = 0, y = 0, z = 0;
    float angle = 0;  // azimuth (radians)
};
std::map<std::string, Waypoint> g_waypoints;  // by identifier

struct WaypointStyle {  // WaypointStyle
    int nearDistance = 128, farDistance = 332;
    std::vector<std::string> sprites;  // hud/locator_bar_dot/<sprite>
};
std::map<std::string, WaypointStyle> g_styles;

const WaypointStyle& style(UiState& s, const std::string& key) {  // WaypointStyleManager.get (MISSING: no sprites)
    auto it = g_styles.find(key);
    if (it != g_styles.end()) return it->second;
    WaypointStyle st;
    const size_t colon = key.find(':');
    const std::string ns = colon == std::string::npos ? "minecraft" : key.substr(0, colon);
    const std::string path = colon == std::string::npos ? key : key.substr(colon + 1);
    const nlohmann::json j = s.res.json("assets/" + ns + "/waypoint_style/" + path + ".json");
    if (j.is_object()) {
        st.nearDistance = j.value("near_distance", 128), st.farDistance = j.value("far_distance", 332);
        for (const auto& sp : j.value("sprites", nlohmann::json::array()))
            if (sp.is_string()) {
                std::string id = sp.get<std::string>();
                const size_t c = id.find(':');
                st.sprites.push_back("hud/locator_bar_dot/" + (c == std::string::npos ? id : id.substr(c + 1)));
            }
    } else if (key == "minecraft:default" || key == "minecraft:bowtie") {  // WaypointStyleProvider's built-in styles
        if (key == "minecraft:bowtie") st.nearDistance = 64, st.sprites.push_back("hud/locator_bar_dot/bowtie");
        for (int i = 0; i < 4; ++i) st.sprites.push_back("hud/locator_bar_dot/default_" + std::to_string(i));
    }
    return g_styles[key] = st;
}

std::string spriteFor(const WaypointStyle& st, float distance) {  // WaypointStyle.sprite
    if (st.sprites.empty()) return "missingno";
    if (distance < float(st.nearDistance)) return st.sprites.front();
    if (distance >= float(st.farDistance)) return st.sprites.back();
    const size_t n = st.sprites.size();
    if (n == 1) return st.sprites.front();
    if (n == 3) return st.sprites[1];
    const float t = (distance - float(st.nearDistance)) / float(st.farDistance - st.nearDistance);
    return st.sprites[size_t(1 + int(std::floor(t * float(n - 1 - 1))))];  // Mth.lerpInt(t, 1, n - 1)
}

i32 javaHash(const Waypoint& w) {  // UUID.hashCode / String.hashCode
    if (w.uuid && w.id.size() == 16) {
        u64 hi = 0, lo = 0;
        for (int i = 0; i < 8; ++i) hi = hi << 8 | u8(w.id[size_t(i)]), lo = lo << 8 | u8(w.id[size_t(8 + i)]);
        const u64 hilo = hi ^ lo;
        return i32(u32(hilo ^ (hilo >> 32)));
    }
    u32 h = 0;
    for (char ch : w.id) h = 31 * h + u8(ch);
    return i32(h);
}

u32 setBrightness(u32 argb, float brightness) {  // ARGB.setBrightness (HSB with the brightness replaced)
    const int r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255, a = argb >> 24;
    const int mx = std::max({r, g, b}), mn = std::min({r, g, b});
    const float range = float(mx - mn), sat = mx != 0 ? range / float(mx) : 0.0f;
    auto pack = [&](int rr, int gg, int bb) { return u32(a) << 24 | u32(rr) << 16 | u32(gg) << 8 | u32(bb); };
    if (sat == 0.0f) {
        const int v = int(std::lround(brightness * 255.0f));
        return pack(v, v, v);
    }
    const float cr = float(mx - r) / range, cg = float(mx - g) / range, cb = float(mx - b) / range;
    float hue = r == mx ? cb - cg : g == mx ? 2.0f + cr - cb : 4.0f + cg - cr;
    hue /= 6.0f;
    if (hue < 0.0f) hue += 1.0f;
    const float seg = (hue - std::floor(hue)) * 6.0f, off = seg - std::floor(seg);
    const float p = brightness * (1.0f - sat), q = brightness * (1.0f - sat * off), t = brightness * (1.0f - sat * (1.0f - off));
    auto c = [](float v) { return int(std::lround(v * 255.0f)); };
    switch (int(seg)) {
        case 0: return pack(c(brightness), c(t), c(p));
        case 1: return pack(c(q), c(brightness), c(p));
        case 2: return pack(c(p), c(brightness), c(t));
        case 3: return pack(c(p), c(q), c(brightness));
        case 4: return pack(c(t), c(p), c(brightness));
        default: return pack(c(brightness), c(p), c(q));
    }
}

float wrapDegrees(float d) { return d - 360.0f * std::floor((d + 180.0f) / 360.0f); }

// ClientboundTrackedWaypointPacket: track / untrack / update a TrackedWaypoint.
void registerHandlers(UiState& s, net::Client& client) {
    UiState* sp = &s;
    client.onPlay("waypoint", [sp](net::Reader& r) {
        const int op = r.varInt();
        Waypoint w;
        w.uuid = r.boolean();
        if (w.uuid) {
            for (int i = 0; i < 16; ++i) w.id += char(r.u8v());
        } else {
            w.id = std::string(r.str());
        }
        w.style = std::string(r.str());
        if (w.style.find(':') == std::string::npos) w.style = "minecraft:" + w.style;
        if ((w.hasColor = r.boolean())) {
            const u32 red = r.u8v(), green = r.u8v(), blue = r.u8v();
            w.color = red << 16 | green << 8 | blue;
        }
        w.type = r.varInt();
        if (w.type == 1) w.x = r.varInt(), w.y = r.varInt(), w.z = r.varInt();
        else if (w.type == 2) w.x = r.varInt(), w.z = r.varInt();
        else if (w.type == 3) w.angle = r.f32();
        if (!r.ok) return;
        sp->post([w, op] {
            const std::string key = (w.uuid ? "u" : "s") + w.id;
            if (op == 0) g_waypoints[key] = w;                      // TRACK
            else if (op == 1) g_waypoints.erase(key);               // UNTRACK
            else if (auto it = g_waypoints.find(key); it != g_waypoints.end() && it->second.type == w.type) {  // UPDATE
                it->second.x = w.x, it->second.y = w.y, it->second.z = w.z, it->second.angle = w.angle;
            }
        });
    });
}

void attached(UiState&) { g_waypoints.clear(); }

const UiModuleRegistrar kModule({.name = "locator_bar", .registerHandlers = registerHandlers, .attached = attached});

}  // namespace

bool hasWaypoints() { return !g_waypoints.empty(); }

void drawLocatorBarBackground(UiState& s, Canvas& c, int left, int top) {
    blitSprite(c, s.sprites.sprite("hud/locator_bar_background"), float(left), float(top), 182, 5);
}

void drawLocatorBarDots(UiState& s, Canvas& c, int top) {
    if (!s.game) return;
    const CameraState cam = s.game->viewCamera();
    const Player& player = s.game->player();
    const double cpos[3] = {cam.x, cam.y, cam.z};
    const float camYaw = wrapDegrees(cam.yaw * 180.0f / kPi);
    const float fovDeg = s.settings.fov * (s.oldFovModifier + (s.fovModifier - s.oldFovModifier) * s.partialTick);
    const double tanHalf = std::tan(double(fovDeg) * 0.5 * kPi / 180.0);
    const double sy = std::sin(cam.yaw), cy = std::cos(cam.yaw), sp = std::sin(cam.pitch), cp = std::cos(cam.pitch);
    const double fwd[3] = {-sy * cp, -sp, cy * cp}, up[3] = {-sy * sp, cp, cy * sp};
    const std::string self = s.client ? s.client->playerUuid() : std::string();
    const float pitchDeg = cam.pitch * 180.0f / kPi;
    // projectHorizonToScreen: the horizon's height on screen in units of the half screen
    const double horizon = pitchDeg <= -90.0f ? -1e30 : pitchDeg >= 90.0f ? 1e30 : std::tan(double(pitchDeg) * kPi / 180.0) / tanHalf;
    struct Entry {
        const Waypoint* w;
        double dist2;
    };
    std::vector<Entry> list;
    for (const auto& [key, w] : g_waypoints) {
        double d2 = 1e300;  // distanceSquared from the camera entity (empty / azimuth: infinitely far)
        if (w.type == 1) {
            const double dx = player.pos[0] - (w.x + 0.5), dy = player.pos[1] - (w.y + 0.5), dz = player.pos[2] - (w.z + 0.5);
            d2 = dx * dx + dy * dy + dz * dz;
        } else if (w.type == 2) {
            const double dx = player.pos[0] - (w.x * 16 + 8 + 0.5), dy = player.pos[1] - (std::floor(player.pos[1]) + 0.5),
                         dz = player.pos[2] - (w.z * 16 + 8 + 0.5);
            d2 = dx * dx + dy * dy + dz * dz;
        }
        list.push_back({&w, d2});
    }
    std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) { return a.dist2 > b.dist2; });  // nearest on top
    const int screenMiddle = int(std::ceil(float(c.guiWidth() - 9) / 2.0f));
    for (const Entry& e : list) {
        const Waypoint& w = *e.w;
        if (w.uuid && w.id == self) continue;  // the camera entity itself
        double target[3] = {0, 0, 0};
        float angle;
        if (w.type == 1 || w.type == 2) {
            if (w.type == 1) target[0] = w.x + 0.5, target[1] = w.y + 0.5, target[2] = w.z + 0.5;
            else target[0] = w.x * 16 + 8 + 0.5, target[1] = std::floor(cam.y) + 0.5, target[2] = w.z * 16 + 8 + 0.5;
            // (camera - waypoint).rotateClockwise90() = (-dz, dy, dx); its angle against the camera's yaw
            const double dx = cpos[0] - target[0], dz = cpos[2] - target[2];
            const float wa = float(std::atan2(dx, -dz)) * 180.0f / kPi;
            angle = wrapDegrees(wa - camYaw);
        } else if (w.type == 3) {
            angle = wrapDegrees(w.angle * 180.0f / kPi - camYaw);
        } else {
            continue;  // empty: NaN
        }
        if (angle <= -60.0f || angle > 60.0f) continue;
        const WaypointStyle& st = style(s, w.style);
        const float distance = float(std::sqrt(e.dist2));
        const std::string sprite = spriteFor(st, distance);
        const u32 color = w.hasColor ? 0xFF000000u | w.color : setBrightness(0xFF000000u | (u32(javaHash(w)) & 0xFFFFFF), 0.9f);
        const int dot = int(std::floor(double(angle) * 173.0 / 2.0 / 60.0));
        blitSprite(c, s.sprites.sprite(sprite), float(screenMiddle + dot), float(top - 2), 9, 9, color);
        int pitch = 0;  // TrackedWaypoint.PitchDirection: 1 up, -1 down
        if (w.type == 1) {  // projectPointToScreen
            const double d[3] = {target[0] - cpos[0], target[1] - cpos[1], target[2] - cpos[2]};
            const double depth = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
            const double y = depth == 0.0 ? 0.0 : (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / tanHalf / depth;
            const bool behind = depth < 0.0;
            const double inFront = behind ? -y : y;
            if (inFront < -1.0) pitch = -1;
            else if (inFront > 1.0) pitch = 1;
            else if (behind) pitch = y > 0.0 ? 1 : y < 0.0 ? -1 : 0;
        } else {
            pitch = horizon < -1.0 ? -1 : horizon > 1.0 ? 1 : 0;
        }
        if (pitch != 0)
            blitSprite(c, s.sprites.sprite(pitch < 0 ? "hud/locator_bar_arrow_down" : "hud/locator_bar_arrow_up"),
                       float(screenMiddle + dot + 1), float(top + (pitch < 0 ? 6 : -6)), 7, 5);
    }
}

}  // namespace ui
