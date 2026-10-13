// Debug and diagnostics packets of the play phase:
//  - F3 + I (KeyboardHandler.copyRecreateCommand): /setblock or /summon for what the crosshair is on, with the
//    server's block entity / entity data (DebugQueryHandler: block_entity_tag_query / entity_tag_query -> tag_query)
//    for players allowed to use it (permission level 2, entity_event 24..28), else the client's block state;
//  - the low disk space toast (ClientboundLowDiskSpaceWarningPacket -> SystemToast.onLowDiskSpace);
//  - GameTest position highlights (GameTestBlockHighlightRenderer: a translucent green box with the relative position
//    over it for 10 seconds);
//  - test instance block status and remote debug samples, kept for the screens / charts that show them.
#include "assets/assets.h"
#include "entity/entities.h"
#include "game/game.h"
#include "net/client.h"
#include "net/session.h"
#include "ui/frontend.h"
#include "ui/modules.h"
#include "ui/toasts.h"
#include "world/nbt.h"
#include "world/nbt_dom.h"
#include "world/world.h"

#include <algorithm>
#include <array>
#include <shared_mutex>
#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <tuple>

namespace ui {

namespace {

struct Marker {
    std::string text;
    double until = 0;  // UI clock
};

struct DebugState {
    int permission = 0;   // our op level (entity_event 24..28)
    int transaction = -1;  // DebugQueryHandler.transactionId
    std::function<void(const std::string* nbt)> pending;  // the open query's callback (null tag: nullptr)
    std::map<std::tuple<int, int, int>, Marker> markers;
    Text testStatus;  // the last test instance block status
    int testSize[3] = {-1, -1, -1};
    std::vector<i64> sample[2];  // RemoteDebugSampleType: TICK_TIME
} g;

// --- SNBT (StringTagVisitor; pretty: NbtUtils.toPrettyComponent's TextComponentTagVisitor with no indentation) ---

std::string javaNumber(double v, bool isFloat) {  // Float.toString / Double.toString
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
    if (v == 0) return std::signbit(v) ? "-0.0" : "0.0";
    char buf[64];
    const auto r = isFloat ? std::to_chars(buf, buf + sizeof(buf), float(v), std::chars_format::scientific)
                           : std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::scientific);
    std::string s(buf, r.ptr);  // "-1.2345e+02"
    const bool neg = s[0] == '-';
    if (neg) s.erase(0, 1);
    const size_t e = s.find('e');
    std::string digits = s.substr(0, 1) + (e > 2 ? s.substr(2, e - 2) : "");
    const int exp = std::atoi(s.c_str() + e + 1);
    std::string out;
    const double a = std::abs(v);
    if (a >= 1e-3 && a < 1e7) {
        if (exp >= 0) {
            if (digits.size() <= size_t(exp) + 1) digits.append(size_t(exp) + 1 - digits.size(), '0');
            out = digits.substr(0, size_t(exp) + 1) + "." + (digits.size() > size_t(exp) + 1 ? digits.substr(size_t(exp) + 1) : "0");
        } else {
            out = "0." + std::string(size_t(-exp - 1), '0') + digits;
        }
    } else {
        out = digits.substr(0, 1) + "." + (digits.size() > 1 ? digits.substr(1) : "0") + "E" + std::to_string(exp);
    }
    return neg ? "-" + out : out;
}

std::string quoteString(std::string_view s) {  // StringTag.quoteAndEscape
    char quote = 0;
    std::string body;
    for (char c : s) {
        if (c == '\\') body += "\\\\";
        else if (c == '\n') body += "\\n";
        else if (c == '\t') body += "\\t";
        else if (c == '\r') body += "\\r";
        else if (c == '\b') body += "\\b";
        else if (c == '\f') body += "\\f";
        else if (c == '"' || c == '\'') {
            if (!quote) quote = c == '"' ? '\'' : '"';
            if (quote == c) body += '\\';
            body += c;
        } else {
            body += c;
        }
    }
    if (!quote) quote = '"';
    return quote + body + quote;
}

bool simpleKey(std::string_view k, bool pretty) {
    if (k.empty()) return false;
    if (!pretty && (k == "true" || k == "false")) return false;  // equalsIgnoreCase in the game
    for (size_t i = 0; i < k.size(); ++i) {
        const char c = k[i];
        const bool letter = std::isalpha(u8(c)) || c == '.' || c == '_';
        const bool other = std::isdigit(u8(c)) || c == '+' || c == '-';
        if (!(letter || other) || (!pretty && i == 0 && !letter)) return false;
    }
    return true;
}

void snbt(net::Reader& r, u8 type, bool pretty, int depth, std::string& out) {
    const char* sep = pretty ? ", " : ",";
    // Pretty lists show their first 128 elements, then "<...>" (TextComponentTagVisitor.MAX_LENGTH).
    auto list = [&](const char* prefix, i32 n, auto&& element) {
        out += "[";
        out += prefix;
        for (i32 i = 0; i < n && r.ok; ++i) {
            const size_t mark = out.size();
            if (pretty) out += i ? ", " : " ";
            else if (i) out += ",";
            element();
            if (pretty && i >= 128) out.resize(mark);
        }
        if (pretty && n > 128) out += "<...>";
        out += "]";
    };
    switch (type) {
        case TagByte: out += std::to_string(i8(r.u8v())) + "b"; break;
        case TagShort: out += std::to_string(r.i16v()) + "s"; break;
        case TagInt: out += std::to_string(r.i32v()); break;
        case TagLong: out += std::to_string(r.i64v()) + "L"; break;
        case TagFloat: out += javaNumber(r.f32(), true) + "f"; break;
        case TagDouble: out += javaNumber(r.f64(), false) + "d"; break;
        case TagByteArray: list("B;", r.i32v(), [&] { out += std::to_string(i8(r.u8v())) + (pretty ? "b" : "B"); }); break;
        case TagIntArray: list("I;", r.i32v(), [&] { out += std::to_string(r.i32v()); }); break;
        case TagLongArray: list("L;", r.i32v(), [&] { out += std::to_string(r.i64v()) + "L"; }); break;
        case TagString: {
            const u16 n = r.u16v();
            out += quoteString(r.bytes(n));
            break;
        }
        case TagList: {
            const u8 t = r.u8v();
            const i32 n = r.i32v();
            out += "[";
            for (i32 i = 0; i < n && r.ok; ++i) {
                const size_t mark = out.size();
                if (i) out += sep;
                snbt(r, t, pretty, depth + 1, out);
                if (pretty && i >= 128) out.resize(mark);
            }
            if (pretty && n > 128) out += ", <...>";
            out += "]";
            break;
        }
        case TagCompound: {
            std::vector<std::pair<std::string, std::string>> entries;
            for (u8 t = r.u8v(); t != TagEnd && r.ok; t = r.u8v()) {
                const u16 n = r.u16v();
                std::string key(r.bytes(n)), value;
                snbt(r, t, pretty, depth + 1, value);
                entries.emplace_back(std::move(key), std::move(value));
            }
            if (!pretty) std::sort(entries.begin(), entries.end());  // StringTagVisitor sorts the keys
            out += "{";
            for (size_t i = 0; i < entries.size(); ++i) {
                if (i) out += sep;
                out += simpleKey(entries[i].first, pretty) ? entries[i].first : quoteString(entries[i].first);
                out += pretty ? ": " : ":";
                out += entries[i].second;
            }
            out += "}";
            break;
        }
        default: r.ok = false; break;
    }
}

// The compound without some keys (copyCreateEntityCommand removes UUID and Pos).
std::string toSnbt(const std::string& nbt, bool pretty, std::initializer_list<const char*> drop = {}) {
    if (nbt.size() < 2 || u8(nbt[0]) != TagCompound) return {};
    if (!drop.size()) {
        net::Reader r(reinterpret_cast<const u8*>(nbt.data()) + 1, nbt.size() - 1);
        std::string out;
        snbt(r, TagCompound, pretty, 0, out);
        return r.ok ? out : std::string();
    }
    // Rebuild the compound's payload without the dropped keys, then print it.
    net::Reader r(reinterpret_cast<const u8*>(nbt.data()) + 1, nbt.size() - 1);
    std::string kept;
    for (u8 t = r.u8v(); t != TagEnd && r.ok; t = r.u8v()) {
        const u8* start = r.p - 1;
        const u16 n = r.u16v();
        const std::string key(r.bytes(n));
        std::string ignored;
        snbt(r, t, pretty, 1, ignored);  // skips the value
        bool dropped = false;
        for (const char* d : drop) dropped |= key == d;
        if (!dropped) kept.append(reinterpret_cast<const char*>(start), size_t(r.p - start));
    }
    kept += char(TagEnd);
    net::Reader k(reinterpret_cast<const u8*>(kept.data()), kept.size());
    std::string out;
    snbt(k, TagCompound, pretty, 0, out);
    return k.ok ? out : std::string();
}

// KeyboardHandler.debugFeedbackTranslated
void feedback(UiState& s, const char* key) {
    Style prefix = colorStyle(0xFFFF55);
    prefix.bold = 1;
    Text t(s.res.translate("debug.prefix"), prefix);
    t.append(" " + s.res.translate(key));
    s.addChat(t);
}

void clipboard(UiState& s, const std::string& text) {
    LOG("ui: F3 + I copied %s", text.c_str());
    s.window->setClipboard(text);
}

void query(UiState& s, const char* packet, const net::Writer& body, std::function<void(const std::string*)> done) {
    net::Writer w;
    w.varInt(++g.transaction);  // DebugQueryHandler.startTransaction
    w.buf.insert(w.buf.end(), body.buf.begin(), body.buf.end());
    g.pending = std::move(done);
    s.client->sendPlay(packet, w);
}

// --- GameTest highlights --------------------------------------------------------------------------------------------

// TestInstanceBlockEntity.getErrorMarkers: the errors of the test instance blocks around (their "errors" data).
std::vector<std::pair<std::array<int, 3>, Text>> testErrors(UiState& s, const CameraState& cam) {
    std::vector<std::pair<std::array<int, 3>, Text>> out;
    if (!s.world || !s.registry) return out;
    const int ccx = int(std::floor(cam.x)) >> 4, ccz = int(std::floor(cam.z)) >> 4;
    std::shared_lock lock(s.world->mutex());
    for (int cz = ccz - 6; cz <= ccz + 6; ++cz)
        for (int cx = ccx - 6; cx <= ccx + 6; ++cx) {
            const auto* list = s.world->blockEntities(cx, cz);
            if (!list) continue;
            for (const World::BlockEntityNbt& be : *list) {
                if (!s.registry->name(s.world->block(be.x, be.y, be.z)).starts_with("minecraft:test_instance_block")) continue;
                NbtValue nbt;
                if (!nbtReadNetwork(reinterpret_cast<const u8*>(be.nbt.data()), be.nbt.size(), nbt)) continue;
                NbtValue* errors = nbt.find("errors");
                if (!errors || errors->type != TagList) continue;
                for (NbtValue& e : errors->items) {
                    const NbtValue* pos = e.find("pos");
                    NbtValue* text = e.find("text");
                    if (!pos || pos->type != TagIntArray || pos->ints.size() != 3 || !text) continue;
                    std::vector<u8> raw;
                    nbtWriteNetwork(*text, raw);
                    out.push_back({{pos->ints[0], pos->ints[1], pos->ints[2]},
                                   parseComponent(std::string_view(reinterpret_cast<const char*>(raw.data()), raw.size()), s.lang())});
                }
            }
        }
    return out;
}

void drawMarkers(UiState& s, Canvas& c) {
    std::erase_if(g.markers, [&](const auto& m) { return s.clock > m.second.until; });
    if (!s.game) return;
    const std::vector<std::pair<std::array<int, 3>, Text>> errors = testErrors(s, s.game->viewCamera());
    if (g.markers.empty() && errors.empty()) return;
    const CameraState cam = s.game->viewCamera();
    const float cp = std::cos(cam.pitch), sp = std::sin(cam.pitch), cy = std::cos(cam.yaw), sy = std::sin(cam.yaw);
    float fwd[3] = {-sy * cp, -sp, cy * cp}, right[3] = {-cy, 0, -sy}, up[3] = {-sy * sp, cp, cy * sp};
    if (cam.basisSet)
        for (int a = 0; a < 3; ++a) fwd[a] = cam.fwd[a], right[a] = cam.right[a], up[a] = cam.up[a];
    const float W = float(c.guiWidth()), H = float(c.guiHeight());
    // Ui::fovRadians without the fluid factor: the option with the dynamic FOV modifier.
    const float fov = s.settings.fov * (s.oldFovModifier + (s.fovModifier - s.oldFovModifier) * s.partialTick) * 3.14159265f / 180.0f;
    const float t = std::tan(fov * 0.5f);
    auto project = [&](double x, double y, double z, float out[2]) {
        const float d[3] = {float(x - cam.x), float(y - cam.y), float(z - cam.z)};
        const float f = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        if (f < 0.05f) return false;
        const float rx = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
        const float uy = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
        out[0] = W * 0.5f * (1.0f + rx / (f * t * W / H));
        out[1] = H * 0.5f * (1.0f - uy / (f * t));
        return true;
    };
    static const int kFaces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    const float uv[4][2] = {};
    auto box = [&](int bx, int by, int bz, u32 color, const Text& text) {
        float corners[8][2];
        bool visible = true;
        for (int k = 0; k < 8 && visible; ++k)  // the block grown by PADDING 0.02
            visible = project(bx - 0.02 + (k & 1) * 1.04, by - 0.02 + ((k >> 1) & 1) * 1.04, bz - 0.02 + ((k >> 2) & 1) * 1.04,
                              corners[k]);
        if (visible)
            for (const auto& f : kFaces) {
                const float xy[4][2] = {{corners[f[0]][0], corners[f[0]][1]}, {corners[f[1]][0], corners[f[1]][1]},
                                        {corners[f[2]][0], corners[f[2]][1]}, {corners[f[3]][0], corners[f[3]][1]}};
                c.quad4(xy, uv, color, ModeSolid);
            }
        float at[2];
        if (!text.empty() && project(bx + 0.5, by + 1.2, bz + 0.5, at))  // billboard text, scale 0.16, on top
            s.drawText(c, text, at[0] - float(s.font.width(text)) / 2, at[1] - 4, 0xFFFFFFFF, false);
    };
    for (const auto& [pos, m] : g.markers) {
        const auto [bx, by, bz] = pos;
        box(bx, by, bz, 0x6000FF00, Text(m.text));  // Marker colour 1610678016
    }
    for (const auto& [pos, text] : errors) box(pos[0], pos[1], pos[2], 0x5FFF0000, text);  // TestInstanceRenderer: 0.375 red
}

// --- module ---------------------------------------------------------------------------------------------------------

void handlers(UiState& s, net::Client& client) {
    UiState* sp = &s;
    client.onPlay("tag_query", [sp](net::Reader& r) {  // ClientboundTagQueryPacket: transaction, compound or nothing
        const int id = r.varInt();
        const std::string nbt(r.nbt());
        if (!r.ok) return;
        sp->post([id, nbt] {
            if (id != g.transaction || !g.pending) return;  // DebugQueryHandler.handleResponse
            auto done = std::move(g.pending);
            g.pending = nullptr;
            done(nbt.empty() ? nullptr : &nbt);
        });
    });
    client.onPlayPeek("entity_event", [sp](net::Reader& r) {  // 24..28: our permission level 0..4
        const int entity = r.i32v(), event = r.u8v();
        if (r.ok && event >= 24 && event <= 28)
            sp->post([sp, entity, event] {
                if (sp->session && entity == sp->session->entityId()) g.permission = event - 24;
            });
    });
    client.onPlay("low_disk_space_warning", [sp](net::Reader&) {
        sp->post([sp] { showToast(tr(*sp, "chunk.toast.lowDiskSpace"), tr(*sp, "chunk.toast.lowDiskSpace.description"), "low_disk_space", 10.0); });
    });
    client.onPlay("game_test_highlight_pos", [sp](net::Reader& r) {
        int x, y, z, rx, ry, rz;
        r.blockPos(x, y, z);
        r.blockPos(rx, ry, rz);
        if (!r.ok) return;
        sp->post([sp, x, y, z, rx, ry, rz] {  // BlockPos.toShortString
            g.markers[{x, y, z}] = {std::to_string(rx) + ", " + std::to_string(ry) + ", " + std::to_string(rz), sp->clock + 10.0};
        });
    });
    client.onPlay("test_instance_block_status", [sp](net::Reader& r) {  // for TestInstanceBlockEditScreen
        const std::string status(r.nbt());
        int size[3] = {-1, -1, -1};
        if (r.boolean())
            for (int& v : size) v = r.varInt();
        if (!r.ok) return;
        sp->post([sp, status, size0 = size[0], size1 = size[1], size2 = size[2]] {
            g.testStatus = parseComponent(status, sp->lang());
            g.testSize[0] = size0, g.testSize[1] = size1, g.testSize[2] = size2;
            LOG("ui: test instance block status: %s", g.testStatus.plain().c_str());
        });
    });
    client.onPlay("debug_sample", [sp](net::Reader& r) {  // DebugScreenOverlay.logRemoteSample (TPS chart)
        std::vector<i64> sample(size_t(std::max(0, std::min(r.varInt(), 1 << 16))));
        for (i64& v : sample) v = r.i64v();
        const int type = r.varInt();
        if (r.ok && type >= 0 && type < 2) sp->post([type, sample = std::move(sample)] { g.sample[type] = sample; });
    });
}

void attached(UiState&) {
    g.markers.clear();  // a new level
    g.pending = nullptr;
}

const UiModuleRegistrar kModule({.name = "debug_packets", .registerHandlers = handlers, .attached = attached,
                                 .drawHud = drawMarkers});

}  // namespace

// KeyboardHandler.copyRecreateCommand (F3 + I; with Shift: the client's own data)
void copyRecreateCommand(UiState& s, bool pullFromServer) {
    if (!s.game || s.reducedDebugInfo) return;
    const bool addNbt = s.online() && g.permission >= 2;
    const CameraState c = s.game->camera();
    const double eye[3] = {c.x, c.y, c.z}, cp = std::cos(c.pitch);
    const double dir[3] = {-std::sin(c.yaw) * cp, -std::sin(c.pitch), std::cos(c.yaw) * cp};
    const RayHit& block = s.game->target();
    ent::PickResult entity;
    if (s.entities) entity = s.entities->pick(eye, dir, block.hit ? std::min(block.distance, s.game->entityReach) : s.game->entityReach);
    if (entity.id >= 0 && s.entities) {
        const ent::Entity* e = s.entities->entity(entity.id);
        if (!e || !e->info) return;
        const std::string type = e->info->name;
        const double pos[3] = {e->pos[0], e->pos[1], e->pos[2]};
        auto copy = [&s, type, pos](const std::string* nbt) {  // copyCreateEntityCommand
            char head[256];
            std::snprintf(head, sizeof(head), "/summon %s %.2f %.2f %.2f", type.c_str(), pos[0], pos[1], pos[2]);
            const std::string data = nbt ? toSnbt(*nbt, true, {"UUID", "Pos"}) : std::string();
            clipboard(s, data.empty() ? std::string(head) : std::string(head) + " " + data);
        };
        if (addNbt && pullFromServer) {
            net::Writer w;
            w.varInt(entity.id);
            UiState* sp = &s;
            query(s, "entity_tag_query", w, [sp, copy](const std::string* nbt) {
                copy(nbt);
                feedback(*sp, "debug.inspect.server.entity");
            });
        } else {
            copy(nullptr);  // the client keeps no entity save data
            feedback(s, "debug.inspect.client.entity");
        }
        return;
    }
    if (!block.hit || !s.world || !s.registry) return;
    const int x = block.x, y = block.y, z = block.z;
    const std::string state = s.registry->name(s.world->block(x, y, z));  // BlockStateParser.serialize
    auto copy = [&s, x, y, z, state](const std::string* nbt) {  // copyCreateBlockCommand
        clipboard(s, "/setblock " + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(z) + " " + state +
                         (nbt ? toSnbt(*nbt, false) : std::string()));
    };
    if (addNbt && pullFromServer) {
        net::Writer w;
        w.blockPos(x, y, z);
        UiState* sp = &s;
        query(s, "block_entity_tag_query", w, [sp, copy](const std::string* nbt) {
            copy(nbt);
            feedback(*sp, "debug.inspect.server.block");
        });
    } else {
        copy(nullptr);  // block entity data stays on the server here
        feedback(s, "debug.inspect.client.block");
    }
}

}  // namespace ui
