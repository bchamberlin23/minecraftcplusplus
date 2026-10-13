// Multiplayer HUD: the player list (PlayerTabOverlay, held Tab), boss bars (BossHealthOverlay) and the scoreboard
// sidebar (Hud.displayScoreboardSidebar), from the server's player info, tab list, boss event and scoreboard packets.
#include "ui/mp_hud.h"

#include "audio/audio.h"
#include "entity/entities.h"
#include "entity/skin_cache.h"
#include "ui/resources.h"
#include "ui/frontend.h"
#include "ui/ui_state.h"
#include "world/nbt.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <map>

namespace ui {

namespace {

std::string uuidKey(const u8 u[16]) { return std::string(reinterpret_cast<const char*>(u), 16); }

// --- player list ------------------------------------------------------------------------------------------------

struct PlayerEntry {
    u8 uuid[16] = {};
    std::string name;
    std::string skin = "entity/player/wide/steve";  // default skin from the UUID (offline mode)
    int gameMode = 0;
    bool listed = false;
    int latency = 0;
    bool hasDisplay = false;
    Text display;
    int listOrder = 0;
    bool showHat = true;
    bool chatSession = false;  // a profile key (RemoteChatSession) was sent for this player
    std::string textures;      // the profile's "textures" property; skinState 1 while its skin downloads
    int skinState = 0;
};

struct HealthState {  // PlayerTabOverlay.HealthState (hearts render type)
    int lastValue = 0, displayed = 0;
    long long lastUpdate = 0, blinkUntil = 0;
};

// --- scoreboard -------------------------------------------------------------------------------------------------

struct NumFmt {  // NumberFormat: -1 none, 0 blank, 1 styled, 2 fixed
    int type = -1;
    Style style;
    Text fixed;
};
struct Objective {
    Text display;
    int renderType = 0;  // 0 integer, 1 hearts
    NumFmt format;
};
struct Score {
    int value = 0;
    bool hasDisplay = false;
    Text display;
    NumFmt format;
};
struct Team {
    Text displayName;
    Text prefix, suffix;
    int color = -1;      // TeamColor id (0..15), -1 none
    int collision = 0;   // Team.CollisionRule: 0 always, 1 never, 2 push other teams, 3 push own team
    u8 options = 0;      // 1 friendly fire, 2 see friendly invisibles
    int nameTags = 0;    // Team.Visibility: 0 always, 1 never, 2 hide for other teams, 3 hide for own team
};

// --- boss bars --------------------------------------------------------------------------------------------------

struct Boss {
    std::string id;
    Text name;
    float progress = 0, target = 0;  // LerpingBossEvent
    double setTime = 0;
    int color = 0, overlay = 0;
    u8 flags = 0;  // 1 darken screen, 2 music, 4 fog
    float current() const {
        const float t = std::clamp(float((nowSeconds() - setTime) * 1000.0 / 100.0), 0.0f, 1.0f);
        return progress + (target - progress) * t;
    }
};

struct MpState {
    std::map<std::string, PlayerEntry> players;  // by UUID bytes
    std::map<std::string, PlayerEntry> seen;     // every player added this session (getSeenPlayers)
    bool hasHeader = false, hasFooter = false;
    Text header, footer;
    bool onlineMode = false;
    std::map<std::string, HealthState> health;
    bool tabVisible = false;
    std::map<std::string, Objective> objectives;
    std::string slots[19];
    std::map<std::string, std::map<std::string, Score>> scores;  // owner -> objective -> score
    std::map<std::string, Team> teams;
    std::map<std::string, std::string> playerTeam;  // member -> team
    std::vector<Boss> bosses;  // insertion order
};
MpState g;

void reset() { g = MpState{}; }

}  // namespace

int teamCollisionRule(const std::string& member) {
    auto it = g.playerTeam.find(member);
    if (it == g.playerTeam.end()) return 0;
    auto t = g.teams.find(it->second);
    return t == g.teams.end() ? 0 : t->second.collision;
}

int teamColor(const std::string& member) {
    auto it = g.playerTeam.find(member);
    if (it == g.playerTeam.end()) return -1;
    auto t = g.teams.find(it->second);
    return t == g.teams.end() ? -1 : t->second.color;
}

namespace {

const Team* teamOf(const std::string& member) {
    auto it = g.playerTeam.find(member);
    if (it == g.playerTeam.end()) return nullptr;
    auto t = g.teams.find(it->second);
    return t == g.teams.end() ? nullptr : &t->second;
}

// PlayerTeam.formatNameForTeam: prefix + name + suffix in the team colour.
Text formatForTeam(const Team* team, const Text& name) {
    if (!team) return name;
    Text t;
    t.append(team->prefix).append(name).append(team->suffix);
    if (team->color >= 0) t = t.withStyle(colorStyle(kLegacyColors[team->color & 15]));
    return t;
}

Text formatValue(int value, const NumFmt& fmt) {
    switch (fmt.type) {
        case 0: return Text();
        case 2: return fmt.fixed;
        default: return Text(std::to_string(value), fmt.type == 1 ? fmt.style : Style{});
    }
}

// ReadOnlyScoreInfo.formatValue with the objective's format or the slot default.
Text formatScore(const Score& sc, const Objective& obj, const NumFmt& slotDefault) {
    const NumFmt& f = sc.format.type >= 0 ? sc.format : obj.format.type >= 0 ? obj.format : slotDefault;
    return formatValue(sc.value, f);
}

const Objective* displayObjective(int slot) {
    if (slot < 0 || slot >= 19 || g.slots[slot].empty()) return nullptr;
    auto it = g.objectives.find(g.slots[slot]);
    return it == g.objectives.end() ? nullptr : &it->second;
}

// --- decoding (network thread) ----------------------------------------------------------------------------------

// Style.Serializer codec as network NBT: the colour and the format flags.
Style readStyleNbt(std::string_view nbt) {
    Style st;
    if (nbt.size() < 2 || u8(nbt[0]) != TagCompound) return st;
    NbtReader r(reinterpret_cast<const u8*>(nbt.data()) + 1, nbt.size() - 1);
    u8 t;
    std::string_view name;
    auto flag = [&](u8 tag) -> i8 {
        if (tag == TagByte) return i8(r.u8v() != 0);
        r.skip(tag);
        return -1;
    };
    while (r.field(t, name)) {
        if (name == "color" && t == TagString) st.color = colorByName(r.str());
        else if (name == "bold") st.bold = flag(t);
        else if (name == "italic") st.italic = flag(t);
        else if (name == "underlined") st.underlined = flag(t);
        else if (name == "strikethrough") st.strikethrough = flag(t);
        else if (name == "obfuscated") st.obfuscated = flag(t);
        else r.skip(t);
    }
    return st;
}

// NumberFormatTypes.OPTIONAL_STREAM_CODEC (registry order: blank, styled, fixed).
NumFmt readNumberFormat(net::Reader& r, const Lang* lang) {
    NumFmt f;
    if (!r.boolean()) return f;
    f.type = r.varInt();
    if (f.type == 1) f.style = readStyleNbt(r.nbt());
    else if (f.type == 2) f.fixed = parseComponent(r.nbt(), lang);
    else if (f.type != 0) f.type = -1;
    return f;
}

const char* kSkins[9] = {"alex", "ari", "efe", "kai", "makena", "noor", "steve", "sunny", "zuri"};

void registerHandlers(UiState& s, net::Client& client) {
    UiState* sp = &s;
    const Lang* lang = &s.res.lang();
    // A new connection starts with the login packet: forget the last one's list, bars and scoreboard (not on
    // Ui::attachSession, which singleplayer calls after the first packets were applied).
    client.onPlayPeek("login", [sp](net::Reader& r) {  // ClientboundLoginPacket ends with onlineMode, enforcesSecureChat
        if (r.left() < 2) return;
        const bool online = r.end[-2] != 0;
        sp->post([online] {
            reset();
            g.onlineMode = online;
        });
    });
    client.onPlay("player_info_update", [sp, lang](net::Reader& r) {
        const u8 actions = r.u8v();
        const i32 n = r.varInt();
        std::vector<PlayerEntry> entries;
        for (i32 i = 0; i < n && r.ok; ++i) {
            PlayerEntry e;
            for (u8& b : e.uuid) b = r.u8v();
            if (actions & 1) {
                e.name = std::string(r.str());
                const i32 props = r.varInt();
                for (i32 p = 0; p < props && r.ok; ++p) {
                    const std::string name(r.str());
                    std::string value(r.str());
                    if (r.boolean()) r.str();
                    if (name == "textures") e.textures = std::move(value), e.skinState = 1;
                }
                const int skin = ent::defaultSkinIndex(e.uuid);
                e.skin = std::string("entity/player/") + (skin < 9 ? "slim/" : "wide/") + kSkins[skin % 9];
            }
            if (actions & 2) {
                e.chatSession = r.boolean();
                if (e.chatSession) r.skip(16), r.i64v(), r.byteArray(), r.byteArray();
            }
            if (actions & 4) e.gameMode = r.varInt();
            if (actions & 8) e.listed = r.boolean();
            if (actions & 16) e.latency = r.varInt();
            if (actions & 32) {
                e.hasDisplay = r.boolean();
                if (e.hasDisplay) e.display = parseComponent(r.nbt(), lang);
            }
            if (actions & 64) e.listOrder = r.varInt();
            if (actions & 128) e.showHat = r.boolean();
            if (r.ok) entries.push_back(std::move(e));
        }
        sp->post([actions, entries = std::move(entries)] {
            for (const PlayerEntry& e : entries) {
                const std::string key = uuidKey(e.uuid);
                if (actions & 1) {
                    g.seen[key] = e;
                    PlayerEntry& p = g.players[key];
                    std::copy(e.uuid, e.uuid + 16, p.uuid);
                    p.name = e.name, p.skin = e.skin, p.textures = e.textures, p.skinState = e.skinState;
                }
                auto it = g.players.find(key);
                if (it == g.players.end()) continue;
                PlayerEntry& p = it->second;
                if (actions & 2) p.chatSession = e.chatSession;
                if (actions & 4) p.gameMode = e.gameMode;
                if (actions & 8) p.listed = e.listed;
                if (actions & 16) p.latency = e.latency;
                if (actions & 32) p.hasDisplay = e.hasDisplay, p.display = e.display;
                if (actions & 64) p.listOrder = e.listOrder;
                if (actions & 128) p.showHat = e.showHat;
            }
        });
    });
    client.onPlay("player_info_remove", [sp](net::Reader& r) {
        const i32 n = r.varInt();
        std::vector<std::string> keys;
        for (i32 i = 0; i < n && r.ok; ++i) keys.emplace_back(std::string(r.bytes(16)));
        sp->post([keys] {
            for (const auto& k : keys) g.players.erase(k);
        });
    });
    client.onPlay("tab_list", [sp, lang](net::Reader& r) {
        Text header = parseComponent(r.nbt(), lang), footer = parseComponent(r.nbt(), lang);
        sp->post([header, footer] {  // handleTabListCustomisation: empty -> none
            g.hasHeader = !header.plain().empty(), g.header = header;
            g.hasFooter = !footer.plain().empty(), g.footer = footer;
        });
    });
    client.onPlay("boss_event", [sp, lang](net::Reader& r) {
        u8 id[16];
        for (u8& b : id) b = r.u8v();
        const int op = r.varInt();
        Boss b;
        b.id = uuidKey(id);
        if (op == 0) {  // ADD
            b.name = parseComponent(r.nbt(), lang);
            b.progress = b.target = r.f32();
            b.color = r.varInt(), b.overlay = r.varInt();
            b.flags = r.u8v();
        } else if (op == 2) {
            b.target = r.f32();
        } else if (op == 3) {
            b.name = parseComponent(r.nbt(), lang);
        } else if (op == 4) {
            b.color = r.varInt(), b.overlay = r.varInt();
        } else if (op == 5) {
            b.flags = r.u8v();
        }
        if (!r.ok) return;
        sp->post([op, b] {
            auto it = std::find_if(g.bosses.begin(), g.bosses.end(), [&](const Boss& x) { return x.id == b.id; });
            if (op == 0) {
                Boss nb = b;
                nb.setTime = nowSeconds();
                if (it != g.bosses.end()) *it = nb;
                else g.bosses.push_back(nb);
                return;
            }
            if (it == g.bosses.end()) return;
            if (op == 1) g.bosses.erase(it);
            else if (op == 2) it->progress = it->current(), it->target = b.target, it->setTime = nowSeconds();
            else if (op == 3) it->name = b.name;
            else if (op == 4) it->color = b.color, it->overlay = b.overlay;
            else if (op == 5) it->flags = b.flags;
        });
    });
    client.onPlay("set_objective", [sp, lang](net::Reader& r) {
        const std::string name(r.str());
        const int method = i8(r.u8v());
        Objective o;
        if (method == 0 || method == 2) {
            o.display = parseComponent(r.nbt(), lang);
            o.renderType = r.varInt();
            o.format = readNumberFormat(r, lang);
        }
        if (!r.ok) return;
        sp->post([name, method, o] {
            if (method == 1) {
                g.objectives.erase(name);
                for (auto& sl : g.slots)
                    if (sl == name) sl.clear();
                for (auto& [owner, m] : g.scores) m.erase(name);
            } else {
                g.objectives[name] = o;
            }
        });
    });
    client.onPlay("set_display_objective", [sp](net::Reader& r) {
        const int slot = r.varInt();
        const std::string name(r.str());
        sp->post([slot, name] {
            if (slot >= 0 && slot < 19) g.slots[slot] = name;
        });
    });
    client.onPlay("set_score", [sp, lang](net::Reader& r) {
        const std::string owner(r.str()), objective(r.str());
        Score sc;
        sc.value = r.varInt();
        sc.hasDisplay = r.boolean();
        if (sc.hasDisplay) sc.display = parseComponent(r.nbt(), lang);
        sc.format = readNumberFormat(r, lang);
        if (!r.ok) return;
        sp->post([owner, objective, sc] { g.scores[owner][objective] = sc; });
    });
    client.onPlay("reset_score", [sp](net::Reader& r) {
        const std::string owner(r.str());
        const bool has = r.boolean();
        const std::string objective = has ? std::string(r.str()) : std::string();
        sp->post([owner, has, objective] {
            if (!has) g.scores.erase(owner);
            else if (auto it = g.scores.find(owner); it != g.scores.end()) it->second.erase(objective);
        });
    });
    client.onPlay("set_player_team", [sp, lang](net::Reader& r) {
        const std::string name(r.str());
        const int method = i8(r.u8v());
        Team t;
        bool params = method == 0 || method == 2;
        if (params) {
            t.displayName = parseComponent(r.nbt(), lang);
            t.prefix = parseComponent(r.nbt(), lang);
            t.suffix = parseComponent(r.nbt(), lang);
            t.nameTags = r.varInt();
            t.collision = r.varInt();
            if (r.boolean()) t.color = r.varInt();
            t.options = r.u8v();
        }
        std::vector<std::string> players;
        if (method == 0 || method == 3 || method == 4) {
            const i32 n = r.varInt();
            for (i32 i = 0; i < n && r.ok; ++i) players.emplace_back(r.str());
        }
        if (!r.ok) return;
        sp->post([name, method, params, t, players] {
            if (method == 1) {
                g.teams.erase(name);
                for (auto it = g.playerTeam.begin(); it != g.playerTeam.end();)
                    it = it->second == name ? g.playerTeam.erase(it) : std::next(it);
                return;
            }
            if (params) g.teams[name] = t;
            if (method == 0 || method == 3)
                for (const auto& p : players) g.playerTeam[p] = name;
            if (method == 4)
                for (const auto& p : players)
                    if (auto it = g.playerTeam.find(p); it != g.playerTeam.end() && it->second == name) g.playerTeam.erase(it);
        });
    });
}

// --- drawing ----------------------------------------------------------------------------------------------------

void drawBossBars(UiState& s, Canvas& c) {
    if (g.bosses.empty()) return;
    static const char* kColors[7] = {"pink", "blue", "red", "green", "yellow", "purple", "white"};
    static const char* kNotches[4] = {"notched_6", "notched_10", "notched_12", "notched_20"};
    const int W = c.guiWidth();
    int yOffset = 12;
    for (const Boss& b : g.bosses) {
        const float x = float(W / 2 - 91);
        auto bar = [&](int width, const char* part) {
            const std::string col = kColors[std::clamp(b.color, 0, 6)];
            blitSpriteRegion(c, s.sprites.sprite("boss_bar/" + col + "_" + part), 182, 5, 0, 0, x, float(yOffset), float(width), 5);
            if (b.overlay > 0 && b.overlay <= 4)
                blitSpriteRegion(c, s.sprites.sprite(std::string("boss_bar/") + kNotches[b.overlay - 1] + "_" + part), 182, 5, 0, 0,
                                 x, float(yOffset), float(width), 5);
        };
        bar(182, "background");
        const float p = b.current();
        const int width = int(std::floor(p * float(182 - 1))) + (p > 0 ? 1 : 0);  // Mth.lerpDiscrete(p, 0, 182)
        if (width > 0) bar(width, "progress");
        const int tw = s.font.width(b.name);
        s.font.draw(c, b.name, float(W / 2 - tw / 2), float(yOffset - 9), 0xFFFFFFFF, true);
        yOffset += 10 + 9;
        if (yOffset >= c.guiHeight() / 3) break;
    }
}


void drawTabHearts(UiState& s, Canvas& c, const std::string& key, int score, int left, int right, int y) {
    // PlayerTabOverlay.extractTablistHearts.
    const long long tick = s.guiTicks;
    auto [it, fresh] = g.health.try_emplace(key);
    HealthState& h = it->second;
    if (fresh) h.lastValue = h.displayed = score;
    if (score != h.lastValue) {
        h.blinkUntil = tick + (score < h.lastValue ? 20 : 10);
        h.lastValue = score, h.lastUpdate = tick;
    }
    if (tick - h.lastUpdate > 20) h.displayed = score;
    const bool blink = h.blinkUntil > tick && (h.blinkUntil - tick) % 6 >= 3;
    const int full = (std::max(score, h.displayed) + 1) / 2;
    const int toRender = std::max(score, std::max(h.displayed, 20)) / 2;
    if (full <= 0) return;
    const int per = int(std::floor(std::min(float(right - left - 4) / float(toRender), 9.0f)));
    if (per <= 3) {
        const float pct = std::clamp(float(score) / 20.0f, 0.0f, 1.0f);
        const u32 color = 0xFF000000 | u32(int((1.0f - pct) * 255.0f)) << 16 | u32(int(pct * 255.0f)) << 8;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", double(score) / 2.0);
        Text t = translatable("multiplayer.player.list.hp", {Text(buf)}, s.lang());
        if (right - s.font.width(t) < left) t = Text(buf);
        s.font.draw(c, t, float((right + left - s.font.width(t)) / 2), float(y), color, true);
        return;
    }
    const char* container = blink ? "hud/heart/container_blinking" : "hud/heart/container";
    for (int k = full; k < toRender; ++k) blitSprite(c, s.sprites.sprite(container), float(left + k * per), float(y), 9, 9);
    for (int k = 0; k < full; ++k) {
        const float x = float(left + k * per);
        blitSprite(c, s.sprites.sprite(container), x, float(y), 9, 9);
        if (blink) {
            if (k * 2 + 1 < h.displayed) blitSprite(c, s.sprites.sprite("hud/heart/full_blinking"), x, float(y), 9, 9);
            if (k * 2 + 1 == h.displayed) blitSprite(c, s.sprites.sprite("hud/heart/half_blinking"), x, float(y), 9, 9);
        }
        if (k * 2 + 1 < score)
            blitSprite(c, s.sprites.sprite(k >= 10 ? "hud/heart/absorbing_full_blinking" : "hud/heart/full"), x, float(y), 9, 9);
        if (k * 2 + 1 == score)
            blitSprite(c, s.sprites.sprite(k >= 10 ? "hud/heart/absorbing_half_blinking" : "hud/heart/half"), x, float(y), 9, 9);
    }
}

void drawPlayerList(UiState& s, Canvas& c) {
    const Objective* listObj = displayObjective(0);
    const std::string listName = listObj ? g.slots[0] : std::string();
    std::vector<const PlayerEntry*> list;
    for (const auto& [k, p] : g.players)
        if (p.listed) list.push_back(&p);
    // Hud.extractTabList: held key; a local (singleplayer) server with one player and no list objective shows nothing.
    const int key = s.settings.key(Key::PlayerList);
    const bool held = s.screen == Screen::None && key && s.window->keyDown(key);
    if (!held || (s.fe->cfg.singleplayer && list.size() <= 1 && !listObj)) {
        if (g.tabVisible) g.health.clear();
        g.tabVisible = false;
        return;
    }
    g.tabVisible = true;
    auto lower = [](std::string v) {
        for (char& ch : v) ch = char(std::tolower(u8(ch)));
        return v;
    };
    auto teamName = [](const PlayerEntry* p) {
        auto it = g.playerTeam.find(p->name);
        return it == g.playerTeam.end() ? std::string() : it->second;
    };
    std::sort(list.begin(), list.end(), [&](const PlayerEntry* a, const PlayerEntry* b) {  // PLAYER_COMPARATOR
        if (a->listOrder != b->listOrder) return a->listOrder > b->listOrder;
        const int sa = a->gameMode == 3, sb = b->gameMode == 3;
        if (sa != sb) return sa < sb;
        const std::string ta = teamName(a), tb = teamName(b);
        if (ta != tb) return ta < tb;
        return lower(a->name) < lower(b->name);
    });
    if (list.size() > 80) list.resize(80);
    // healthStates.keySet().removeIf: hearts of players no longer listed start over
    std::erase_if(g.health, [&](const auto& kv) {
        return std::none_of(list.begin(), list.end(), [&](const PlayerEntry* p) { return uuidKey(p->uuid) == kv.first; });
    });
    // level.getPlayerByUUID: the players loaded here (ourselves included)
    std::unordered_set<std::string> loaded;
    if (s.client) loaded.insert(s.client->playerUuid());
    if (s.entities)
        for (const auto& [id, en] : s.entities->entities())
            if (en.info && en.info->name == "minecraft:player") loaded.insert(uuidKey(en.uuid));

    const int W = c.guiWidth();
    struct Entry {
        Text name, score;
        int value = 0, scoreWidth = 0;
    };
    std::vector<Entry> entries;
    const int spacer = s.font.width(" ");
    int maxName = 0, maxScore = 0;
    NumFmt listDefault;
    listDefault.type = 1, listDefault.style = colorStyle(0xFFFF55);  // StyledFormat.PLAYER_LIST_DEFAULT (yellow)
    for (const PlayerEntry* p : list) {
        Entry e;
        e.name = p->hasDisplay ? p->display : formatForTeam(teamOf(p->name), Text(p->name));
        if (p->gameMode == 3) {  // spectators: italic
            Style it;
            it.italic = 1;
            e.name = e.name.withStyle(it);
        }
        maxName = std::max(maxName, s.font.width(e.name));
        if (listObj) {
            const Score* sc = nullptr;
            if (auto o = g.scores.find(p->name); o != g.scores.end())
                if (auto f = o->second.find(listName); f != o->second.end()) sc = &f->second;
            if (sc) e.value = sc->value;
            if (listObj->renderType != 1) {
                if (sc) e.score = formatScore(*sc, *listObj, listDefault), e.scoreWidth = s.font.width(e.score);
                maxScore = std::max(maxScore, e.scoreWidth > 0 ? spacer + e.scoreWidth : 0);
            }
        }
        entries.push_back(std::move(e));
    }
    const int slots = int(list.size());
    int rows = slots, cols = 1;
    while (rows > 20) ++cols, rows = (slots + cols - 1) / cols;
    // Heads: the game draws them only with an online-mode (authenticated) server, which its singleplayer server is
    // (here the local server runs offline for want of the Microsoft login); MCPP_TAB_HEADS=1 forces them.
    static const bool forceHeads = [] {
        const char* v = std::getenv("MCPP_TAB_HEADS");
        return v && *v == '1';
    }();
    const bool showHead = g.onlineMode || s.fe->cfg.singleplayer || forceHeads;
    const int widthForScore = listObj ? (listObj->renderType == 1 ? 90 : maxScore) : 0;
    const int slotWidth = std::min(cols * ((showHead ? 9 : 0) + maxName + widthForScore + 13), W - 50) / cols;
    const int xxo = W / 2 - (slotWidth * cols + (cols - 1) * 5) / 2;
    int yyo = 10;
    int maxLine = slotWidth * cols + (cols - 1) * 5;
    std::vector<Text> headerLines, footerLines;
    if (g.hasHeader) headerLines = s.font.split(g.header, W - 50);
    if (g.hasFooter) footerLines = s.font.split(g.footer, W - 50);
    for (const Text& l : headerLines) maxLine = std::max(maxLine, s.font.width(l));
    for (const Text& l : footerLines) maxLine = std::max(maxLine, s.font.width(l));
    auto block = [&](const std::vector<Text>& lines) {
        c.fill(float(W / 2 - maxLine / 2 - 1), float(yyo - 1), float(W / 2 + maxLine / 2 + 1), float(yyo + int(lines.size()) * 9), 0x80000000);
        for (const Text& l : lines) {
            s.font.draw(c, l, float(W / 2 - s.font.width(l) / 2), float(yyo), 0xFFFFFFFF, true);
            yyo += 9;
        }
    };
    if (!headerLines.empty()) {
        block(headerLines);
        ++yyo;
    }
    c.fill(float(W / 2 - maxLine / 2 - 1), float(yyo - 1), float(W / 2 + maxLine / 2 + 1), float(yyo + rows * 9), 0x80000000);
    for (int i = 0; i < slots; ++i) {
        const int col = i / rows, row = i % rows;
        int xo = xxo + col * slotWidth + col * 5;
        const int yo = yyo + row * 9;
        c.fill(float(xo), float(yo), float(xo + slotWidth), float(yo + 8), s.textBackground(0x20FFFFFFu));
        const PlayerEntry& p = *list[size_t(i)];
        const Entry& e = entries[size_t(i)];
        if (showHead) {  // PlayerFaceExtractor: face (8, 8) and hat (40, 8) of the 64x64 skin, upside down for
                         // Dinnerbone / Grumm (AvatarRenderer.isPlayerUpsideDown)
            const Sprite* skin = s.sprites.texture(p.skin);
            const bool flip = (p.name == "Dinnerbone" || p.name == "Grumm") && loaded.count(uuidKey(p.uuid));
            const float v = flip ? 16.0f : 8.0f, h = flip ? -8.0f : 8.0f;
            blitTexture(c, skin, float(xo), float(yo), 8, v, 8, h, 64, 64);
            if (p.showHat) blitTexture(c, skin, float(xo), float(yo), 40, v, 8, h, 64, 64);
            xo += 9;
        }
        s.font.draw(c, e.name, float(xo), float(yo), p.gameMode == 3 ? 0x90FFFFFF : 0xFFFFFFFF, true);
        if (listObj && p.gameMode != 3) {
            const int left = xo + maxName + 1, right = left + widthForScore;
            if (right - left > 5) {
                if (listObj->renderType == 1) drawTabHearts(s, c, uuidKey(p.uuid), e.value, left, right, yo);
                else if (!e.score.empty() || e.scoreWidth > 0) s.font.draw(c, e.score, float(right - e.scoreWidth), float(yo), 0xFFFFFFFF, true);
            }
        }
        // Ping bars.
        const int lat = p.latency;
        const char* ping = lat < 0 ? "icon/ping_unknown" : lat < 150 ? "icon/ping_5" : lat < 300 ? "icon/ping_4"
                         : lat < 600 ? "icon/ping_3" : lat < 1000 ? "icon/ping_2" : "icon/ping_1";
        blitSprite(c, s.sprites.sprite(ping), float(xo - (showHead ? 9 : 0) + slotWidth - 11), float(yo), 10, 8);
    }
    if (!footerLines.empty()) {
        yyo += rows * 9 + 1;
        block(footerLines);
    }
}

void drawHud(UiState& s, Canvas& c) { drawBossBars(s, c); }  // the sidebar: drawScoreboardSidebar, after the sleep overlay

void drawHudTop(UiState& s, Canvas& c) { drawPlayerList(s, c); }

void tick(UiState& s) {  // downloaded profile skins for the faces (registered as UI textures "skin/<hash>")
    if (audio::AudioSystem* a = audio::instance()) a->setBossMusic(bossPlayMusic());
    for (auto& [key, p] : g.players) {
        if (p.skinState != 1) continue;
        ent::ProfileTextures t;
        if (!ent::parseProfileTextures(p.textures, t) || t.skinUrl.empty()) {
            p.skinState = 3;
            continue;
        }
        Image img;
        const int state = ent::fetchProfileTexture(t.skinUrl, true, &img.rgba, &img.w, &img.h);
        if (state == 1) continue;
        p.skinState = 3;
        if (state != 2) continue;
        const std::string name = "skin/" + ent::profileTextureHash(t.skinUrl);
        if (s.sprites.addImage(name, img)) p.skin = name, p.skinState = 2;
    }
}

UiModuleRegistrar reg([] {
    UiModule m;
    m.name = "mp_hud";
    m.registerHandlers = registerHandlers;
    m.drawHud = drawHud;
    m.drawHudTop = drawHudTop;
    m.tick = tick;
    return m;
}());

}  // namespace

bool findPlayerInfo(const std::string& uuid, PlayerInfoView& out) {
    auto it = g.players.find(uuid);
    if (it == g.players.end()) return false;
    const PlayerEntry& p = it->second;
    out = PlayerInfoView{uuidKey(p.uuid), p.name, p.skin, p.gameMode, p.listed, p.chatSession, p.showHat};
    return true;
}

std::vector<PlayerInfoView> seenPlayers() {
    std::vector<PlayerInfoView> v;
    for (const auto& [key, s] : g.seen) {
        const auto it = g.players.find(key);
        const PlayerEntry& p = it != g.players.end() ? it->second : s;  // online: the current entry
        v.push_back(PlayerInfoView{uuidKey(p.uuid), p.name, p.skin, p.gameMode, p.listed, p.chatSession, p.showHat});
    }
    return v;
}

std::vector<PlayerInfoView> playerInfos() {
    std::vector<PlayerInfoView> v;
    for (const auto& [key, p] : g.players) v.push_back({key, p.name, p.skin, p.gameMode, p.listed, p.chatSession, p.showHat});
    return v;
}

std::vector<TeamView> teamViews() {
    std::vector<TeamView> v;
    for (const auto& [name, t] : g.teams) {
        TeamView tv{name, t.displayName, t.color, {}};
        for (const auto& [member, team] : g.playerTeam)
            if (team == name) tv.players.push_back(member);
        v.push_back(std::move(tv));
    }
    return v;
}

bool bossDarkenScreen() {
    for (const Boss& b : g.bosses)
        if (b.flags & 1) return true;
    return false;
}
bool bossPlayMusic() {
    for (const Boss& b : g.bosses)
        if (b.flags & 2) return true;
    return false;
}
bool bossWorldFog() {
    for (const Boss& b : g.bosses)
        if (b.flags & 4) return true;
    return false;
}

Text teamFormattedName(const std::string& member, const Text& name) { return formatForTeam(teamOf(member), name); }

bool belowNameText(const std::string& owner, Text& out) {
    const Objective* obj = displayObjective(2);
    if (!obj) return false;
    auto so = g.scores.find(owner);
    if (so == g.scores.end()) return false;
    auto sc = so->second.find(g.slots[2]);
    if (sc == so->second.end()) return false;
    NumFmt noStyle;  // objective.numberFormatOrDefault(StyledFormat.NO_STYLE)
    noStyle.type = 1;
    out = Text();
    out.append(formatScore(sc->second, *obj, noStyle)).append(" ").append(obj->display);
    return true;
}

int teamNameTagShown(const std::string& us, const std::string& them, bool visible) {
    auto b = g.playerTeam.find(them);
    if (b == g.playerTeam.end()) return -1;
    auto t = g.teams.find(b->second);
    if (t == g.teams.end()) return -1;
    auto a = g.playerTeam.find(us);
    const bool mine = a != g.playerTeam.end() && g.teams.count(a->second), allied = mine && a->second == b->second;
    switch (t->second.nameTags) {
        case 1: return 0;
        case 2: return !mine ? visible : allied && ((t->second.options & 2) || visible);
        case 3: return !mine ? visible : !allied && visible;
        default: return visible;
    }
}

bool seesInvisible(const std::string& us, const std::string& them) {  // Entity.isInvisibleTo: canSeeFriendlyInvisibles
    auto a = g.playerTeam.find(us), b = g.playerTeam.find(them);
    if (a == g.playerTeam.end() || b == g.playerTeam.end() || a->second != b->second) return false;
    auto t = g.teams.find(a->second);
    return t != g.teams.end() && (t->second.options & 2);
}

// Hud.extractScoreboardSidebar (hud.cpp draws it between the sleep overlay and the action bar).
void drawScoreboardSidebar(UiState& s, Canvas& c) {
    // Hud.extractScoreboardSidebar: the player's team colour slot first, then the sidebar.
    const Objective* obj = nullptr;
    std::string objName;
    const std::string& self = s.fe->cfg.playerName;
    if (const Team* t = teamOf(self); t && t->color >= 0) {
        obj = displayObjective(3 + t->color);
        if (obj) objName = g.slots[3 + t->color];
    }
    if (!obj) obj = displayObjective(1), objName = g.slots[1];
    if (!obj) return;
    NumFmt sidebarDefault;
    sidebarDefault.type = 1, sidebarDefault.style = colorStyle(0xFF5555);  // StyledFormat.SIDEBAR_DEFAULT (red)
    struct Row {
        std::string owner;
        int value;
        Text name, score;
        int scoreWidth;
    };
    std::vector<Row> rows;
    for (const auto& [owner, m] : g.scores) {
        auto it = m.find(objName);
        if (it == m.end() || owner.starts_with("#")) continue;  // PlayerScoreEntry.isHidden
        rows.push_back({owner, it->second.value, {}, {}, 0});
    }
    // SCORE_DISPLAY_ORDER: value descending, then the owner case-insensitively.
    auto lower = [](std::string v) {
        for (char& ch : v) ch = char(std::tolower(u8(ch)));
        return v;
    };
    std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        if (a.value != b.value) return a.value > b.value;
        return lower(a.owner) < lower(b.owner);
    });
    if (rows.size() > 15) rows.resize(15);
    const Text title = obj->display;
    const int titleWidth = s.font.width(title);
    int biggest = titleWidth;
    const int spacer = s.font.width(": ");
    for (Row& r : rows) {
        const Score& sc = g.scores[r.owner][objName];
        r.name = formatForTeam(teamOf(r.owner), sc.hasDisplay ? sc.display : Text(r.owner));
        r.score = formatScore(sc, *obj, sidebarDefault);
        r.scoreWidth = s.font.width(r.score);
        biggest = std::max(biggest, s.font.width(r.name) + (r.scoreWidth > 0 ? spacer + r.scoreWidth : 0));
    }
    const int count = int(rows.size()), height = count * 9;
    const int bottom = c.guiHeight() / 2 + height / 3;
    const int left = c.guiWidth() - biggest - 3, right = c.guiWidth() - 3 + 2;
    const int headerY = bottom - count * 9;
    c.fill(float(left - 2), float(headerY - 9 - 1), float(right), float(headerY - 1), s.textBackground(0.4f));
    c.fill(float(left - 2), float(headerY - 1), float(right), float(bottom), s.textBackground(0.3f));
    s.font.draw(c, title, float(left + biggest / 2 - titleWidth / 2), float(headerY - 9), 0xFFFFFFFF, false);
    for (int i = 0; i < count; ++i) {
        const int y = bottom - (count - i) * 9;
        s.font.draw(c, rows[size_t(i)].name, float(left), float(y), 0xFFFFFFFF, false);
        s.font.draw(c, rows[size_t(i)].score, float(right - rows[size_t(i)].scoreWidth), float(y), 0xFFFFFFFF, false);
    }
}

}  // namespace ui
