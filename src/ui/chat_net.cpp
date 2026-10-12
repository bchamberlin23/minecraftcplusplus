// Chat from and to the server (ClientPacketListener's chat handlers, ChatListener, ChatComponent): system, player and
// disguised chat decorated by the server's chat types (the minecraft:chat_type registry or an inline type), the
// GuiMessageTag indicators, message deletion (delete_chat through the MessageSignatureCache), and the
// LastSeenMessagesTracker: acknowledgements sent with chat messages and signed-argument commands, and chat_ack.
// There is no profile key: messages and commands go unsigned, as the game sends them without one.
#include "ui/frontend.h"
#include "ui/mp_hud.h"
#include "ui/ui_state.h"
#include "world/nbt_dom.h"

#include <array>
#include <chrono>
#include <random>

namespace ui {

namespace {

using Sig = std::string;  // MessageSignature: 256 bytes

// ChatTypeDecoration: translation key, the parameters (0 sender, 1 target, 2 content), the style.
struct Decoration {
    std::string key = "chat.type.text";
    std::vector<int> params = {0, 2};
    Style style;
};
struct ChatType {
    Decoration chat, narration;
};
// ChatType.Bound
struct Bound {
    ChatType type;
    Text name, target;
    bool hasTarget = false;
};

Text decorate(const Decoration& d, const Bound& b, const Text& content, const Lang* lang) {  // ChatTypeDecoration.decorate
    std::vector<Text> args;
    for (int p : d.params) args.push_back(p == 0 ? b.name : p == 1 ? (b.hasTarget ? b.target : Text()) : content);
    return translatable(d.key, args, lang, d.style);
}

std::string netNbt(const NbtValue& v) {
    std::vector<u8> out;
    nbtWriteNetwork(v, out);
    return std::string(out.begin(), out.end());
}

Decoration decorationFromNbt(NbtValue* v, const Lang* lang) {
    Decoration d;
    if (!v || v->type != TagCompound) return d;
    if (NbtValue* k = v->find("translation_key"); k && k->type == TagString) d.key = k->s;
    if (NbtValue* p = v->find("parameters"); p && p->type == TagList) {
        d.params.clear();
        for (const NbtValue& e : p->items) d.params.push_back(e.s == "sender" ? 0 : e.s == "target" ? 1 : 2);
    }
    if (NbtValue* st = v->find("style"); st && st->type == TagCompound) d.style = parseStyle(netNbt(*st), lang);
    return d;
}

// The registry entry `id` of minecraft:chat_type (network NBT {chat, narration}).
bool registryChatType(const net::Client& client, int id, const Lang* lang, ChatType& out) {
    const auto& entries = client.registryEntries("minecraft:chat_type");
    if (id < 0 || size_t(id) >= entries.size()) return false;
    NbtValue root;
    const std::string& nbt = entries[size_t(id)].nbt;
    if (!nbtReadNetwork(reinterpret_cast<const u8*>(nbt.data()), nbt.size(), root)) return false;
    out.chat = decorationFromNbt(root.find("chat"), lang);
    out.narration = decorationFromNbt(root.find("narration"), lang);
    return true;
}

Decoration readDecoration(net::Reader& r, const Lang* lang) {  // ChatTypeDecoration.STREAM_CODEC
    Decoration d;
    d.key = std::string(r.str());
    d.params.clear();
    const i32 n = r.varInt();
    for (i32 i = 0; i < n && r.ok; ++i) d.params.push_back(r.varInt());
    d.style = parseStyle(r.nbt(), lang);
    return d;
}

bool readBound(net::Reader& r, const net::Client& client, const Lang* lang, Bound& b) {  // ChatType.Bound.STREAM_CODEC
    const int holder = r.varInt();
    if (holder == 0) {  // inline (ChatType.DIRECT_STREAM_CODEC)
        b.type.chat = readDecoration(r, lang);
        b.type.narration = readDecoration(r, lang);
    } else if (!registryChatType(client, holder - 1, lang, b.type)) {
        LOG("ui: unknown chat type %d", holder - 1);
    }
    b.name = parseComponent(r.nbt(), lang);
    b.hasTarget = r.boolean();
    if (b.hasTarget) b.target = parseComponent(r.nbt(), lang);
    return r.ok;
}

// MessageSignature.Packed: a cache id, or the full signature.
struct PackedSig {
    int id = -1;
    Sig full;
};
PackedSig readPacked(net::Reader& r) {
    PackedSig p;
    p.id = r.varInt() - 1;
    if (p.id == -1) p.full = std::string(r.bytes(256));
    return p;
}

struct PlayerChat {
    int globalIndex = 0;
    std::string sender;  // UUID bytes
    std::optional<Sig> signature;
    std::string content;
    std::vector<PackedSig> lastSeen;
    std::optional<Text> unsignedContent;
    int filter = 0;  // FilterMask type: 0 pass through, 1 fully filtered, 2 partially
    std::vector<u64> mask;
    Bound bound;
};

struct ChatNet {
    int nextChatIndex = 0;
    std::array<std::optional<Sig>, 128> cache;  // MessageSignatureCache
    struct Tracked {                            // LastSeenTrackedEntry
        Sig signature;
        bool pending = true;
    };
    std::array<std::optional<Tracked>, 20> tracked;  // LastSeenMessagesTracker
    int tail = 0, offset = 0;
    std::optional<Sig> lastTracked;
    std::vector<Sig> deletions;  // ChatComponent.messageDeletionQueue (messages younger than 60 ticks)
    bool enforcesSecureChat = false;
};
ChatNet g;

std::optional<Sig> unpack(const PackedSig& p) {
    if (p.id < 0) return p.full;
    if (p.id >= int(g.cache.size())) return std::nullopt;
    return g.cache[size_t(p.id)];
}

void cachePush(std::vector<Sig> queue) {  // MessageSignatureCache.push
    std::vector<Sig> fresh = queue;
    for (size_t i = 0; !queue.empty() && i < g.cache.size(); ++i) {
        std::optional<Sig> entry = g.cache[i];
        g.cache[i] = queue.back();
        queue.pop_back();
        if (entry && std::find(fresh.begin(), fresh.end(), *entry) == fresh.end()) queue.insert(queue.begin(), *entry);
    }
}

void sendAck(UiState& s) {  // ClientPacketListener.sendChatAcknowledgement
    const int offset = g.offset;
    g.offset = 0;
    if (offset > 0 && s.online()) {
        net::Writer w;
        w.varInt(offset);
        s.client->sendPlay("chat_ack", w);
    }
}

void markProcessed(UiState& s, const Sig& signature, bool wasShown) {  // markMessageAsProcessed / addPending
    if (g.lastTracked == signature) return;
    g.lastTracked = signature;
    g.tracked[size_t(g.tail)] = wasShown ? std::optional<ChatNet::Tracked>(ChatNet::Tracked{signature, true}) : std::nullopt;
    g.tail = (g.tail + 1) % int(g.tracked.size());
    if (++g.offset > 64) sendAck(s);
}

// LastSeenMessagesTracker.generateAndApplyUpdate: the offset, the acknowledged bits and the checksum of the last seen
// signatures (LastSeenMessages.computeChecksum over Arrays.hashCode of each).
void writeLastSeenUpdate(net::Writer& w) {
    const int offset = g.offset;
    g.offset = 0;
    u8 bits[3] = {};
    i32 checksum = 1;
    for (int i = 0; i < int(g.tracked.size()); ++i) {
        auto& e = g.tracked[size_t((g.tail + i) % int(g.tracked.size()))];
        if (!e) continue;
        bits[i / 8] |= u8(1 << (i % 8));
        e->pending = false;
        i32 h = 1;
        for (char c : e->signature) h = 31 * h + i32(i8(c));
        checksum = 31 * checksum + h;
    }
    const i8 sum = i8(checksum);
    w.varInt(offset);
    w.bytes(bits, 3);
    w.u8v(u8(sum == 0 ? 1 : sum));
}

u64 nowMillis() {
    return u64(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}
u64 salt() {  // Crypt.SaltSupplier
    static std::mt19937_64 rng(std::random_device{}());
    return rng();
}

// The chat line's text for a (partly) filtered message: filtered characters as dark gray #s (FilterMask.applyWithFormatting).
Text applyMask(const std::string& text, const std::vector<u64>& mask, const Lang* lang) {
    auto bit = [&](size_t i) { return i / 64 < mask.size() && (mask[i / 64] >> (i % 64)) & 1; };
    Style filtered = colorStyle(0x555555);
    auto ev = std::make_shared<TextEvents>();
    ev->hover = "show_text";
    ev->hoverText = Text(translatable("chat.filtered", {}, lang));
    filtered.events = ev;
    Text out;
    size_t unit = 0;  // UTF-16 index of the mask
    for (size_t i = 0; i < text.size();) {
        const size_t start = i;
        const u32 cp = nextCodepoint(text, i);
        const bool f = bit(unit);
        unit += cp >= 0x10000 ? 2 : 1;
        if (f) out.append(std::string(cp >= 0x10000 ? 2 : 1, '#'), filtered);
        else out.append(text.substr(start, i - start));
    }
    return out;
}

void disconnect(UiState& s, const char* key) {
    LOG("ui: chat: %s", key);
    if (s.client) s.client->disconnect(s.res.translate(key));
}

// ClientPacketListener.handlePlayerChat + ChatListener.handlePlayerChatMessage (main thread, in packet order).
void handlePlayerChat(UiState& s, PlayerChat& m) {
    const Lang* lang = s.lang();
    const int expected = g.nextChatIndex++;
    if (m.globalIndex != expected) {
        LOG("ui: missing or out-of-order chat message from server, expected index %d but got %d", expected, m.globalIndex);
        return disconnect(s, "multiplayer.disconnect.bad_chat_index");
    }
    std::vector<Sig> seen;
    for (const PackedSig& p : m.lastSeen) {
        const std::optional<Sig> sig = unpack(p);
        if (!sig) return disconnect(s, "multiplayer.disconnect.invalid_packet");
        seen.push_back(*sig);
    }
    if (m.signature) seen.push_back(*m.signature);
    cachePush(std::move(seen));
    PlayerInfoView sender;
    const bool known = findPlayerInfo(m.sender, sender);
    // PlayerInfo.messageValidator without a chat session: REJECT_ALL when the server enforces secure chat, else
    // ACCEPT_UNSIGNED (the signature is dropped). Senders with a session keep theirs (not verified here).
    const bool rejected = known && !sender.chatSession && g.enforcesSecureChat;
    std::optional<Sig> signature = known && sender.chatSession ? m.signature : std::nullopt;
    if (!known || rejected) {  // ChatListener.handleChatMessageError
        if (!known) LOG("ui: received player chat packet for unknown player");
        if (m.signature) markProcessed(s, *m.signature, false);
        Style st = colorStyle(0xFF5555);
        st.italic = 1;
        const Text error = translatable("chat.validation_error", {}, lang, st);
        s.addChatMessage({decorate(m.bound.type.chat, m.bound, error, lang), s.guiTicks, {}, kTagError, 2});
        return;
    }
    // ChatTrustLevel: our own messages in singleplayer are secure, unsigned ones are not.
    const bool local = s.fe->cfg.singleplayer && s.client && m.sender == s.client->playerUuid();
    const ChatTag tag = local || signature ? ChatTag{} : kTagNotSecure;
    bool shown = false;
    // Only Show Secure Chat: the signed body without the server's unsigned decoration, and nothing unsigned (our own
    // messages are secure).
    const bool secureOnly = s.settings.onlyShowSecureChat;
    if (m.filter != 1 && !(secureOnly && !signature && !local)) {
        const Text content = m.filter == 2                       ? applyMask(m.content, m.mask, lang)
                             : m.unsignedContent && !secureOnly ? *m.unsignedContent
                                                                 : Text(m.content);
        const Text decorated = decorate(m.bound.type.chat, m.bound, content, lang);
        LOG("[chat] %s", decorated.plain().c_str());
        ChatMessage msg{decorated, s.guiTicks, signature.value_or(Sig()), tag, 2};
        UiState* sp = &s;
        s.handleChatMessage([sp, msg] {  // ChatListener: after the Chat Delay
            if (!sp->chatVisible(2)) return false;
            ChatMessage shownMsg = msg;
            shownMsg.addedTick = sp->guiTicks;
            sp->addChatMessage(std::move(shownMsg));
            return true;
        });
        shown = true;
    }
    if (signature) markProcessed(s, *signature, shown);
}

// ChatComponent.deleteMessageOrDelay: true when done (replaced, or no such message).
bool deleteMessage(UiState& s, const Sig& signature) {
    for (ChatMessage& msg : s.chatMessages) {
        if (msg.signature != signature) continue;
        if (s.guiTicks < msg.addedTick + 60) return false;
        Style st = colorStyle(0xAAAAAA);
        st.italic = 1;
        msg = ChatMessage{translatable("chat.deleted_marker", {}, s.lang(), st), msg.addedTick, {}, kTagSystem, msg.source};
        s.refreshChatLines();
        return true;
    }
    return true;
}

void registerHandlers(UiState& s, net::Client& client) {
    UiState* sp = &s;
    const Lang* lang = &s.res.lang();
    net::Client* cl = &client;
    client.onPlayPeek("login", [sp](net::Reader& r) {  // ClientboundLoginPacket ends with enforcesSecureChat
        if (r.left() < 1) return;
        const bool enforces = r.end[-1] != 0;
        sp->post([enforces] {
            g = ChatNet{};  // a new LastSeenMessagesTracker, MessageSignatureCache and chat index
            g.enforcesSecureChat = enforces;
        });
    });
    client.onPlay("system_chat", [sp, lang](net::Reader& r) {
        Text t = parseComponent(r.nbt(), lang);
        if (r.boolean()) {  // ChatListener.handleOverlay
            sp->post([sp, t] { sp->actionbar = t, sp->actionbarTime = 60; });
            return;
        }
        LOG("[chat] %s", t.plain().c_str());
        sp->post([sp, t] { sp->addChat(t, 1); });  // addServerSystemMessage
    });
    client.onPlay("player_chat", [sp, lang, cl](net::Reader& r) {
        auto m = std::make_shared<PlayerChat>();
        m->globalIndex = r.varInt();
        m->sender = std::string(r.bytes(16));
        r.varInt();  // index (of the sender's chain)
        if (r.boolean()) m->signature = std::string(r.bytes(256));
        m->content = std::string(r.str());
        r.i64v(), r.i64v();  // timestamp, salt
        const i32 seen = r.varInt();
        for (i32 i = 0; i < seen && i < 20 && r.ok; ++i) m->lastSeen.push_back(readPacked(r));
        if (r.boolean()) m->unsignedContent = parseComponent(r.nbt(), lang);
        m->filter = r.varInt();
        if (m->filter == 2) {
            const i32 longs = r.varInt();
            for (i32 i = 0; i < longs && r.ok; ++i) m->mask.push_back(r.u64v());
        }
        if (!readBound(r, *cl, lang, m->bound)) return;
        sp->post([sp, m] { handlePlayerChat(*sp, *m); });
    });
    client.onPlay("disguised_chat", [sp, lang, cl](net::Reader& r) {  // ChatListener.handleDisguisedChatMessage
        const Text content = parseComponent(r.nbt(), lang);
        Bound b;
        if (!readBound(r, *cl, lang, b)) return;
        const Text t = decorate(b.type.chat, b, content, lang);
        LOG("[chat] %s", t.plain().c_str());
        sp->post([sp, t] {
            sp->handleChatMessage([sp, t] {
                if (!sp->chatVisible(2)) return false;
                sp->addChatMessage({t, sp->guiTicks, {}, kTagSystem, 2});
                return true;
            });
        });
    });
    client.onPlay("delete_chat", [sp](net::Reader& r) {  // ClientPacketListener.handleDeleteChat
        const PackedSig p = readPacked(r);
        if (!r.ok) return;
        sp->post([sp, p] {
            const std::optional<Sig> sig = unpack(p);
            if (!sig) return disconnect(*sp, "multiplayer.disconnect.invalid_packet");
            for (auto& e : g.tracked)  // LastSeenMessagesTracker.ignorePending
                if (e && e->pending && e->signature == *sig) {
                    e.reset();
                    break;
                }
            if (!deleteMessage(*sp, *sig)) g.deletions.push_back(*sig);
        });
    });
}

void tick(UiState& s) {  // ChatComponent.processMessageDeletionQueue
    if (g.deletions.empty()) return;
    std::vector<Sig> left;
    for (const Sig& d : g.deletions)
        if (!deleteMessage(s, d)) left.push_back(d);
    g.deletions = std::move(left);
}

const UiModuleRegistrar kModule({.name = "chat_net", .registerHandlers = registerHandlers, .tick = tick});

}  // namespace

void sendChatMessage(UiState& s, const std::string& text) {  // ServerboundChatPacket without a signature
    net::Writer w;
    w.str(text);
    w.u64v(nowMillis());
    w.u64v(salt());
    w.boolean(false);
    writeLastSeenUpdate(w);
    s.client->sendPlay("chat", w);
    LOG("[chat] sent message %s", text.c_str());
}

void sendChatCommand(UiState& s, const std::string& command) {  // ClientPacketListener.sendCommand
    net::Writer w;
    w.str(command);
    if (!commandHasSignedArguments(command)) {
        s.client->sendPlay("chat_command", w);
    } else {  // ServerboundChatCommandSignedPacket: no argument signatures without a key, the acknowledgements
        w.u64v(nowMillis());
        w.u64v(salt());
        w.varInt(0);
        writeLastSeenUpdate(w);
        s.client->sendPlay("chat_command_signed", w);
    }
    LOG("[chat] sent command /%s", command.c_str());
}

}  // namespace ui
