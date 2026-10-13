// The operator block editors, opened on the client when a creative operator (canUseGameMasterBlocks) uses the block:
// command blocks (CommandBlockEditScreen: filled in when the server's block entity data arrives) and command block
// minecarts, jigsaws (with Generate), structure blocks (by mode: save / load / corner / data), test blocks and test
// instance blocks. Their values come from the block entities' NBT the world keeps; Done sends the game's packets.
#include "ui/block_editors.h"
#include "ui/mp_hud.h"

#include "assets/assets.h"

#include "ui/frontend.h"
#include "ui/item_ui.h"
#include "ui/modules.h"
#include "world/world.h"

#include <cmath>
#include <cstdio>

namespace ui {

namespace {

constexpr u32 kGrey = 0xFFA0A0A0;

struct Editor {
    Screen screen = Screen::None;
    int x = 0, y = 0, z = 0;  // the block
    int entity = -1;          // the command block minecart
    std::string block;        // block id without namespace and state
    u64 serial = 0;           // the block entity's version when opened (command blocks wait for a newer one)
    bool ready = true;        // command blocks: the server's data arrived
    // command blocks
    Field command, previous;
    bool trackOutput = true, conditional = false, autoexec = false;
    int mode = 2;  // CommandBlockEntity.Mode: 0 sequence, 1 auto, 2 redstone
    Text lastOutput;
    // jigsaw
    Field pool, name, target, finalState, selectionPriority, placementPriority;
    int joint = 0;  // 0 rollable, 1 aligned
    bool vertical = false, keepJigsaws = true;
    int levels = 0;
    // structure block
    int structMode = 0, mirror = 0, rotation = 0;  // StructureMode, Mirror, Rotation ordinals
    bool ignoreEntities = true, strict = false, showAir = false, showBoundingBox = true;
    Field structName, posX, posY, posZ, sizeX, sizeY, sizeZ, integrity, seed, data;
    // test blocks
    int testMode = 2;  // TestBlockMode: start, log, fail, accept
    Field message, testId;
    Text info;
    bool includeEntities = true;
};
Editor g;

const char* const kStructModes[4] = {"save", "load", "corner", "data"};
const char* const kMirrors[3] = {"none", "left_right", "front_back"};
const char* const kTestModes[4] = {"start", "log", "fail", "accept"};

const NbtValue* nbtString(const NbtValue* c, std::string_view key) {
    if (!c || c->type != TagCompound) return nullptr;
    for (size_t i = 0; i < c->names.size(); ++i)
        if (c->names[i] == key) return &c->items[i];
    return nullptr;
}
std::string str(const NbtValue* c, std::string_view key, std::string def = {}) {
    const NbtValue* v = nbtString(c, key);
    return v && v->type == TagString ? v->s : def;
}
i64 num(const NbtValue* c, std::string_view key, i64 def) {
    const NbtValue* v = nbtString(c, key);
    return v && v->type >= TagByte && v->type <= TagLong ? v->i : def;
}
double real(const NbtValue* c, std::string_view key, double def) {
    const NbtValue* v = nbtString(c, key);
    if (!v) return def;
    if (v->type == TagFloat || v->type == TagDouble) return v->d;
    return v->type >= TagByte && v->type <= TagLong ? double(v->i) : def;
}

// The block entity's NBT (and its version) from the world's block entity data.
bool blockEntity(UiState& s, int x, int y, int z, NbtValue& out, u64* serial = nullptr) {
    const auto* list = s.world ? s.world->blockEntities(x >> 4, z >> 4) : nullptr;
    if (!list) return false;
    for (const World::BlockEntityNbt& be : *list)
        if (be.x == x && be.y == y && be.z == z) {
            if (serial) *serial = be.serial;
            return nbtReadNetwork(reinterpret_cast<const u8*>(be.nbt.data()), be.nbt.size(), out);
        }
    return false;
}

void setField(Field& f, const std::string& v, int maxLength) {
    f.text = UiState::TextField{};
    f.text.value = v.substr(0, size_t(maxLength));
    f.text.maxLength = maxLength;
    f.text.cursor = f.text.selection = int(f.text.value.size());
}

Text componentText(UiState& s, const NbtValue* v) {  // a text component saved in NBT (LastOutput)
    if (!v) return Text();
    std::vector<u8> bytes;
    nbtWriteNetwork(*v, bytes);
    return parseComponent(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), s.lang());
}

std::string blockName(UiState& s) {
    std::string n = s.registry->name(s.world->block(g.x, g.y, g.z));
    const size_t colon = n.find(':'), bracket = n.find('[');
    return n.substr(colon + 1, bracket == std::string::npos ? std::string::npos : bracket - colon - 1);
}
bool blockStateFlag(UiState& s, const char* property) {
    const std::string n = s.registry->name(s.world->block(g.x, g.y, g.z));
    return n.find(std::string(property) + "=true") != std::string::npos;
}

void label(UiState& s, Canvas& c, const char* key, float x, float y) { s.drawText(c, tr(s, key), x, y, kGrey, true); }
void labelRight(UiState& s, Canvas& c, const char* key, float right, float y) {
    const Text t = tr(s, key);
    s.drawText(c, t, right - float(s.font.width(t)), y, kGrey, true);
}
Text caption(UiState& s, const char* key, const Text& value) { return trf(s, "options.generic_value", {tr(s, key), value}); }
Text onOff(UiState& s, bool on) { return tr(s, on ? "options.on" : "options.off"); }

void sendPos(net::Writer& w) { w.blockPos(g.x, g.y, g.z); }

// --- command blocks (AbstractCommandBlockEditScreen) ------------------------------------------------------------

void loadCommandBlock(UiState& s) {  // updateGui
    NbtValue nbt;
    if (!blockEntity(s, g.x, g.y, g.z, nbt, &g.serial)) return;
    setField(g.command, str(&nbt, "Command"), 32500);
    g.trackOutput = num(&nbt, "TrackOutput", 1) != 0;
    g.autoexec = num(&nbt, "auto", 0) != 0;
    g.lastOutput = componentText(s, nbtString(&nbt, "LastOutput"));
    const std::string b = blockName(s);
    g.mode = b == "repeating_command_block" ? 1 : b == "chain_command_block" ? 0 : 2;
    g.conditional = blockStateFlag(s, "conditional");
}

void commandBuild(UiState& s) {
    UiState* p = &s;
    s.buttons.clear();
    const int W = s.guiW(), H = s.guiH();
    const bool minecart = s.screen == Screen::MinecartCommandEdit;
    const float prevY = minecart ? 150.0f : 135.0f;
    g.command.x = float(W / 2 - 150) + 1, g.command.y = 51, g.command.w = 298, g.command.h = 18;
    setField(g.previous, g.trackOutput ? g.lastOutput.plain() : "-", 32500);
    g.previous.x = float(W / 2 - 150) + 1, g.previous.y = prevY + 1, g.previous.w = 274, g.previous.h = 18;
    Button& out = addButton(s, float(W / 2 + 130), prevY, 20, Text(), [p] {  // O / X: track output
        g.trackOutput = !g.trackOutput;
        setField(g.previous, g.trackOutput ? g.lastOutput.plain() : "-", 32500);
    });
    out.labelFn = [] { return Text(g.trackOutput ? "O" : "X"); };
    if (!minecart) {
        Button& mode = addButton(s, float(W / 2 - 154), 165, 100, Text(), [] { g.mode = (g.mode + 1) % 3; });
        mode.labelFn = [p] {
            static const char* const kModes[3] = {"advMode.mode.sequence", "advMode.mode.auto", "advMode.mode.redstone"};
            return tr(*p, kModes[g.mode]);
        };
        Button& cond = addButton(s, float(W / 2 - 50), 165, 100, Text(), [] { g.conditional = !g.conditional; });
        cond.labelFn = [p] { return tr(*p, g.conditional ? "advMode.mode.conditional" : "advMode.mode.unconditional"); };
        Button& autoexec = addButton(s, float(W / 2 + 54), 165, 100, Text(), [] { g.autoexec = !g.autoexec; });
        autoexec.labelFn = [p] { return tr(*p, g.autoexec ? "advMode.mode.autoexec.bat" : "advMode.mode.redstoneTriggered"); };
    }
    addButton(s, float(W / 2 - 154), float(H / 4 + 132), 150, tr(s, "gui.done"), [p, minecart] {
        if (p->online()) {
            net::Writer w;
            if (minecart) {  // ServerboundSetCommandMinecartPacket
                w.varInt(g.entity), w.str(g.command.text.value), w.boolean(g.trackOutput);
                p->client->sendPlay("set_command_minecart", w);
            } else {  // ServerboundSetCommandBlockPacket
                sendPos(w);
                w.str(g.command.text.value), w.varInt(g.mode);
                w.u8v(u8((g.trackOutput ? 1 : 0) | (g.conditional ? 2 : 0) | (g.autoexec ? 4 : 0)));
                p->client->sendPlay("set_command_block", w);
            }
        }
        p->closeScreen();
    });
    addButton(s, float(W / 2 + 4), float(H / 4 + 132), 150, tr(s, "gui.cancel"), [p] { p->closeScreen(); });
    for (Button& b : s.buttons)  // enableControls(false) until the block's data came (Cancel stays)
        if (&b != &s.buttons.back()) b.active = g.ready;
    setFields(s, {&g.command}, 0);
}

void commandDraw(UiState& s, Canvas& c) {
    if (!g.ready && s.screen == Screen::CommandBlockEdit) {  // ClientPacketListener.handleBlockEntityData: updateGui
        u64 serial = 0;
        NbtValue nbt;
        if (blockEntity(s, g.x, g.y, g.z, nbt, &serial) && serial > g.serial) {
            loadCommandBlock(s);
            g.ready = true;
            s.buildButtons();
        }
    }
    const int W = c.guiWidth();
    menuBackground(s, c);
    s.drawCentered(c, tr(s, "advMode.setCommand"), float(W / 2), 20, 0xFFFFFFFF);
    label(s, c, "advMode.command", float(W / 2 - 149), 40);
    drawFields(s, c);
    const float prevY = s.screen == Screen::MinecartCommandEdit ? 150.0f : 135.0f;
    if (!g.previous.text.value.empty()) {  // the previous output, read only
        label(s, c, "advMode.previousOutput", float(W / 2 - 149), prevY - 10);
        const Field& f = g.previous;
        blitSprite(c, s.sprites.sprite("widget/text_field"), f.x - 1, f.y - 1, f.w + 2, f.h + 2);
        s.drawText(c, Text(fitText(s.font, f.text.value, int(f.w) - 8)), f.x + 3, f.y + (f.h - 8) / 2, 0xFF707070, true);
    }
    s.drawButtons(c);
    commandSuggestionsDraw(s, g.command.text, g.command.x, g.command.y, g.command.w, g.command.h, c);
}

void commandInput(UiState& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) return s.closeScreen();
    if (e.type == E::Key && e.down && e.code == VK_RETURN && g.ready) {  // isConfirmation: Done
        if (s.buttons.size() >= 2 && s.buttons[s.buttons.size() - 2].onPress) s.buttons[s.buttons.size() - 2].onPress();
        return;
    }
    if (commandSuggestionsInput(s, g.command.text, g.command.x, g.command.y, g.command.w, g.command.h, e)) return;
    if (fieldsInput(s, e)) return;
    s.buttonsInput(e);
}

// --- jigsaw (JigsawBlockEditScreen) -------------------------------------------------------------------------------

bool validId(const std::string& v) {  // Identifier.tryParse
    const size_t colon = v.find(':');
    auto ok = [](std::string_view part, bool path) {
        for (char ch : part)
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' || (path && ch == '/')))
                return false;
        return true;
    };
    return colon == std::string::npos ? ok(v, true) : ok(std::string_view(v).substr(0, colon), false) && ok(std::string_view(v).substr(colon + 1), true);
}
bool jigsawValid() { return validId(g.name.text.value) && validId(g.target.text.value) && validId(g.pool.text.value); }

void sendJigsaw(UiState& s) {  // ServerboundSetJigsawBlockPacket
    if (!s.online()) return;
    auto priority = [](const Field& f) {
        char* end = nullptr;
        const long v = std::strtol(f.text.value.c_str(), &end, 10);
        return f.text.value.empty() || *end ? 0 : int(v);
    };
    net::Writer w;
    sendPos(w);
    auto full = [](const std::string& id) { return id.find(':') == std::string::npos ? "minecraft:" + id : id; };
    w.str(full(g.name.text.value)), w.str(full(g.target.text.value)), w.str(full(g.pool.text.value));
    w.str(g.finalState.text.value), w.str(g.joint == 0 ? "rollable" : "aligned");
    w.varInt(priority(g.selectionPriority)), w.varInt(priority(g.placementPriority));
    s.client->sendPlay("set_jigsaw_block", w);
}

void jigsawBuild(UiState& s) {
    UiState* p = &s;
    s.buttons.clear();
    const int W = s.guiW();
    auto place = [&](Field& f, float x, float y, float w) { f.x = x + 1, f.y = y + 1, f.w = w - 2, f.h = 18; };
    place(g.pool, float(W / 2 - 153), 20, 300);
    place(g.name, float(W / 2 - 153), 55, 300);
    place(g.target, float(W / 2 - 153), 90, 300);
    place(g.finalState, float(W / 2 - 153), 125, 300);
    place(g.selectionPriority, float(W / 2 - 153), 160, 98);
    place(g.placementPriority, float(W / 2 - 50), 160, 98);
    if (g.vertical) {
        Button& joint = addButton(s, float(W / 2 + 54), 160, 100, Text(), [] { g.joint ^= 1; });
        joint.labelFn = [p] { return tr(*p, g.joint == 0 ? "jigsaw_block.joint.rollable" : "jigsaw_block.joint.aligned"); };
    }
    Button levels;  // the levels slider (0..20)
    levels.x = float(W / 2 - 154), levels.y = 185, levels.w = 100, levels.slider = true, levels.value = float(g.levels) / 20.0f;
    levels.labelFn = [p] { return trf(*p, "jigsaw_block.levels", {Text(std::to_string(g.levels))}); };
    levels.onSlide = [](float v) { g.levels = int(std::floor(std::clamp(v, 0.0f, 1.0f) * 20.0f)); };
    s.buttons.push_back(std::move(levels));
    Button& keep = addButton(s, float(W / 2 - 50), 185, 100, Text(), [] { g.keepJigsaws = !g.keepJigsaws; });
    keep.labelFn = [p] { return caption(*p, "jigsaw_block.keep_jigsaws", onOff(*p, g.keepJigsaws)); };
    addButton(s, float(W / 2 + 54), 185, 100, tr(s, "jigsaw_block.generate"), [p] {
        sendJigsaw(*p);
        if (p->online()) {  // ServerboundJigsawGeneratePacket
            net::Writer w;
            sendPos(w);
            w.varInt(g.levels), w.boolean(g.keepJigsaws);
            p->client->sendPlay("jigsaw_generate", w);
        }
        p->closeScreen();
    }, jigsawValid());
    addButton(s, float(W / 2 - 154), 210, 150, tr(s, "gui.done"), [p] {
        sendJigsaw(*p);
        p->closeScreen();
    }, jigsawValid());
    addButton(s, float(W / 2 + 4), 210, 150, tr(s, "gui.cancel"), [p] { p->closeScreen(); });
    setFields(s, {&g.pool, &g.name, &g.target, &g.finalState, &g.selectionPriority, &g.placementPriority}, 0);
}

void jigsawDraw(UiState& s, Canvas& c) {
    const int W = c.guiWidth();
    menuBackground(s, c);
    label(s, c, "jigsaw_block.pool", float(W / 2 - 153), 10);
    label(s, c, "jigsaw_block.name", float(W / 2 - 153), 45);
    label(s, c, "jigsaw_block.target", float(W / 2 - 153), 80);
    label(s, c, "jigsaw_block.final_state", float(W / 2 - 153), 115);
    label(s, c, "jigsaw_block.selection_priority", float(W / 2 - 153), 150);
    label(s, c, "jigsaw_block.placement_priority", float(W / 2 - 50), 150);
    if (g.vertical) label(s, c, "jigsaw_block.joint_label", float(W / 2 + 53), 150);
    drawFields(s, c);
    s.drawButtons(c);
}

void jigsawInput(UiState& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) return s.closeScreen();
    if (e.type == E::Key && e.down && e.code == VK_RETURN && jigsawValid()) {
        sendJigsaw(s);
        return s.closeScreen();
    }
    if (fieldsInput(s, e)) {  // updateValidity: Done and Generate need valid ids
        const bool ok = jigsawValid();
        for (Button& b : s.buttons)
            if (b.label.plain() == s.res.translate("gui.done") || b.label.plain() == s.res.translate("jigsaw_block.generate")) b.active = ok;
        return;
    }
    s.buttonsInput(e);
}

// --- structure block (StructureBlockEditScreen) -----------------------------------------------------------------

struct StructureBackup {
    int mode, mirror, rotation;
    bool ignoreEntities, strict, showAir, showBoundingBox;
};
StructureBackup g_backup;

void sendStructure(UiState& s, int updateType) {  // ServerboundSetStructureBlockPacket
    if (!s.online()) return;
    auto integer = [](const Field& f) {
        char* end = nullptr;
        const long v = std::strtol(f.text.value.c_str(), &end, 10);
        return f.text.value.empty() || *end ? 0 : int(v);
    };
    char* end = nullptr;
    float integrity = std::strtof(g.integrity.text.value.c_str(), &end);
    if (g.integrity.text.value.empty() || *end) integrity = 1.0f;
    long long seed = std::strtoll(g.seed.text.value.c_str(), &end, 10);
    if (g.seed.text.value.empty() || *end) seed = 0;
    net::Writer w;
    sendPos(w);
    w.varInt(updateType), w.varInt(g.structMode);
    w.str(g.structName.text.value);
    w.u8v(u8(integer(g.posX))), w.u8v(u8(integer(g.posY))), w.u8v(u8(integer(g.posZ)));
    w.u8v(u8(integer(g.sizeX))), w.u8v(u8(integer(g.sizeY))), w.u8v(u8(integer(g.sizeZ)));
    w.varInt(g.mirror), w.varInt(g.rotation);
    w.str(g.data.text.value);
    w.f32(integrity);
    w.varLong(seed);
    w.u8v(u8((g.ignoreEntities ? 1 : 0) | (g.showAir ? 2 : 0) | (g.showBoundingBox ? 4 : 0) | (g.strict ? 8 : 0)));
    s.client->sendPlay("set_structure_block", w);
}

void structureBuild(UiState& s) {
    UiState* p = &s;
    s.buttons.clear();
    const int W = s.guiW();
    const int m = g.structMode;  // 0 save, 1 load, 2 corner, 3 data
    addButton(s, float(W / 2 - 154), 210, 150, tr(s, "gui.done"), [p] {
        sendStructure(*p, 0);
        p->closeScreen();
    });
    addButton(s, float(W / 2 + 4), 210, 150, tr(s, "gui.cancel"), [p] {  // the toggles back as they were
        const StructureBackup& b = g_backup;
        g.structMode = b.mode, g.mirror = b.mirror, g.rotation = b.rotation;
        g.ignoreEntities = b.ignoreEntities, g.strict = b.strict, g.showAir = b.showAir, g.showBoundingBox = b.showBoundingBox;
        p->closeScreen();
    });
    if (m == 0) addButton(s, float(W / 2 + 104), 185, 50, tr(s, "structure_block.button.save"), [p] { sendStructure(*p, 1), p->closeScreen(); });
    if (m == 1) addButton(s, float(W / 2 + 104), 185, 50, tr(s, "structure_block.button.load"), [p] { sendStructure(*p, 2), p->closeScreen(); });
    Button& mode = addButton(s, float(W / 2 - 154), 185, 50, Text(), [p] {  // SAVE, LOAD, CORNER (DATA with Alt)
        const bool alt = p->window->keyDown(VK_MENU);
        g.structMode = (g.structMode + 1) % (alt ? 4 : 3);
        p->buildButtons();
    });
    mode.labelFn = [p] { return tr(*p, (std::string("structure_block.mode.") + kStructModes[g.structMode]).c_str()); };
    if (m == 0) addButton(s, float(W / 2 + 104), 120, 50, tr(s, "structure_block.button.detect_size"), [p] { sendStructure(*p, 3), p->closeScreen(); });
    if (m == 0 || m == 1) {
        Button& ent = addButton(s, float(W / 2 + 104), 160, 50, Text(), [] { g.ignoreEntities = !g.ignoreEntities; });
        ent.labelFn = [p] { return onOff(*p, !g.ignoreEntities); };
    }
    if (m == 1) {
        Button& strict = addButton(s, float(W / 2 + 104), 120, 50, Text(), [] { g.strict = !g.strict; });
        strict.labelFn = [p] { return onOff(*p, g.strict); };
        Button& mirror = addButton(s, float(W / 2 - 20), 185, 40, Text(), [] { g.mirror = (g.mirror + 1) % 3; });
        mirror.labelFn = [] { return Text(g.mirror == 0 ? "|" : g.mirror == 1 ? "< >" : "^ v"); };  // Mirror.symbol
        Button& box = addButton(s, float(W / 2 + 104), 80, 50, Text(), [] { g.showBoundingBox = !g.showBoundingBox; });
        box.labelFn = [p] { return onOff(*p, g.showBoundingBox); };
        static const char* const kRot[4] = {"0", "90", "180", "270"};
        static const float kRotX[4] = {-102, -61, 21, 62};
        for (int r = 0; r < 4; ++r)
            addButton(s, float(W / 2) + kRotX[r], 185, 40, Text(kRot[r]), [p, r] {
                g.rotation = r;
                p->buildButtons();
            }, g.rotation != r);
    }
    if (m == 0) {
        Button& air = addButton(s, float(W / 2 + 104), 80, 50, Text(), [] { g.showAir = !g.showAir; });
        air.labelFn = [p] { return onOff(*p, g.showAir); };
    }
    auto place = [&](Field& f, float x, float y, float w) { f.x = x + 1, f.y = y + 1, f.w = w - 2, f.h = 18; };
    place(g.structName, float(W / 2 - 152), 40, 300);
    place(g.posX, float(W / 2 - 152), 80, 80), place(g.posY, float(W / 2 - 72), 80, 80), place(g.posZ, float(W / 2 + 8), 80, 80);
    place(g.sizeX, float(W / 2 - 152), 120, 80), place(g.sizeY, float(W / 2 - 72), 120, 80), place(g.sizeZ, float(W / 2 + 8), 120, 80);
    place(g.integrity, float(W / 2 - 152), 120, 80), place(g.seed, float(W / 2 - 72), 120, 80);
    place(g.data, float(W / 2 - 152), 120, 240);
    std::vector<Field*> fields;
    if (m != 3) fields.push_back(&g.structName);
    if (m == 0 || m == 1) fields.insert(fields.end(), {&g.posX, &g.posY, &g.posZ});
    if (m == 0) fields.insert(fields.end(), {&g.sizeX, &g.sizeY, &g.sizeZ});
    if (m == 1) fields.insert(fields.end(), {&g.integrity, &g.seed});
    if (m == 3) fields.push_back(&g.data);
    setFields(s, fields, 0);
}

void structureDraw(UiState& s, Canvas& c) {
    const int W = c.guiWidth();
    const int m = g.structMode;
    menuBackground(s, c);
    s.drawCentered(c, Text(s.res.translate("block.minecraft.structure_block")), float(W / 2), 10, 0xFFFFFFFF);
    if (m != 3) label(s, c, "structure_block.structure_name", float(W / 2 - 153), 30);
    if (m == 0 || m == 1) {
        label(s, c, "structure_block.position", float(W / 2 - 153), 70);
        labelRight(s, c, "structure_block.include_entities", float(W / 2 + 154), 150);
    }
    if (m == 0) {
        label(s, c, "structure_block.size", float(W / 2 - 153), 110);
        labelRight(s, c, "structure_block.detect_size", float(W / 2 + 154), 110);
        labelRight(s, c, "structure_block.show_air", float(W / 2 + 154), 70);
    }
    if (m == 1) {
        label(s, c, "structure_block.integrity", float(W / 2 - 153), 110);
        labelRight(s, c, "structure_block.strict", float(W / 2 + 154), 110);
        labelRight(s, c, "structure_block.show_boundingbox", float(W / 2 + 154), 70);
    }
    if (m == 3) label(s, c, "structure_block.custom_data", float(W / 2 - 153), 110);
    label(s, c, (std::string("structure_block.mode_info.") + kStructModes[m]).c_str(), float(W / 2 - 153), 174);
    drawFields(s, c);
    s.drawButtons(c);
}

void structureInput(UiState& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) return s.buttons[1].onPress();  // Cancel
    if (e.type == E::Key && e.down && e.code == VK_RETURN) return s.buttons[0].onPress();  // Done
    if (fieldsInput(s, e)) return;
    s.buttonsInput(e);
}

// --- test block (TestBlockEditScreen) ----------------------------------------------------------------------------

void testBuild(UiState& s) {
    UiState* p = &s;
    s.buttons.clear();
    const int W = s.guiW();
    g.message.x = float(W / 2 - 152) + 1, g.message.y = 81, g.message.w = 238, g.message.h = 18;
    Button& mode = addButton(s, float(W / 2 - 154), 185, 50, Text(), [p] {
        g.testMode = (g.testMode + 1) % 4;
        p->buildButtons();
    });
    mode.labelFn = [p] { return tr(*p, (std::string("test_block.mode.") + kTestModes[g.testMode]).c_str()); };
    addButton(s, float(W / 2 - 154), 210, 150, tr(s, "gui.done"), [p] {
        if (p->online()) {  // ServerboundSetTestBlockPacket
            net::Writer w;
            sendPos(w);
            w.varInt(g.testMode), w.str(g.message.text.value);
            p->client->sendPlay("set_test_block", w);
        }
        p->closeScreen();
    });
    addButton(s, float(W / 2 + 4), 210, 150, tr(s, "gui.cancel"), [p] { p->closeScreen(); });
    setFields(s, g.testMode != 0 ? std::vector<Field*>{&g.message} : std::vector<Field*>{}, 0);
}

void testDraw(UiState& s, Canvas& c) {
    const int W = c.guiWidth();
    menuBackground(s, c);
    s.drawCentered(c, Text(s.res.translate("block.minecraft.test_block")), float(W / 2), 10, 0xFFFFFFFF);
    if (g.testMode != 0) label(s, c, "test_block.message", float(W / 2 - 153), 70);
    label(s, c, (std::string("test_block.mode_info.") + kTestModes[g.testMode]).c_str(), float(W / 2 - 153), 174);
    drawFields(s, c);
    s.drawButtons(c);
}

void simpleInput(UiState& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) return s.closeScreen();
    if (fieldsInput(s, e)) return;
    s.buttonsInput(e);
}

// --- test instance block (TestInstanceBlockEditScreen) ------------------------------------------------------------

void sendTestInstance(UiState& s, int action) {  // ServerboundTestInstanceBlockActionPacket
    if (!s.online()) return;
    auto size = [](const Field& f) {
        char* end = nullptr;
        const long v = std::strtol(f.text.value.c_str(), &end, 10);
        return f.text.value.empty() || *end ? 1 : std::clamp(int(v), 1, 48);
    };
    net::Writer w;
    sendPos(w);
    w.varInt(action);
    const bool id = !g.testId.text.value.empty() && validId(g.testId.text.value);
    w.boolean(id);
    if (id) w.str(g.testId.text.value.find(':') == std::string::npos ? "minecraft:" + g.testId.text.value : g.testId.text.value);
    w.varInt(size(g.sizeX)), w.varInt(size(g.sizeY)), w.varInt(size(g.sizeZ));
    w.varInt(g.rotation);
    w.boolean(!g.includeEntities);
    w.varInt(0);       // status: cleared
    w.boolean(false);  // no error message
    s.client->sendPlay("test_instance_block_action", w);
}

void testInstanceBuild(UiState& s) {
    UiState* p = &s;
    s.buttons.clear();
    const int W = s.guiW();
    const float L = float(W / 2 - 158);
    auto column = [&](int index, int count) {  // widgetX / exactSize
        const float size = (316.0f - float(count - 1) * 8.0f) / float(count);
        return std::pair<float, float>{float(int(L + float(index) * (8.0f + size))), size};
    };
    g.testId.x = L + 1, g.testId.y = 41, g.testId.w = 314, g.testId.h = 18;
    Field* sizes[3] = {&g.sizeX, &g.sizeY, &g.sizeZ};
    for (int i = 0; i < 3; ++i) {
        const auto [x, w] = column(i, 5);
        sizes[i]->x = x + 1, sizes[i]->y = 161, sizes[i]->w = float(int(w)) - 2, sizes[i]->h = 18;
    }
    const auto [rx, rw] = column(3, 5);
    Button& rot = addButton(s, rx, 160, float(int(rw)), Text(), [] { g.rotation = (g.rotation + 1) % 4; });
    rot.labelFn = [] {
        static const char* const kRot[4] = {"0", "90", "180", "270"};
        return Text(kRot[g.rotation]);
    };
    const auto [ex, ew] = column(4, 5);
    Button& ent = addButton(s, ex, 160, float(int(ew)), Text(), [] { g.includeEntities = !g.includeEntities; });
    ent.labelFn = [p] { return onOff(*p, g.includeEntities); };
    const auto [r0, rw2] = column(0, 2);
    const auto [r1, rw3] = column(1, 2);
    (void)rw3;
    addButton(s, r0, 185, float(int(rw2)), tr(s, "test_instance.action.reset"), [p] { sendTestInstance(*p, 3), p->closeScreen(); });
    const bool saveOk = g.rotation == 0 && validId(g.testId.text.value) && !g.testId.text.value.empty();
    addButton(s, r1, 185, float(int(rw2)), tr(s, "test_instance.action.save"), [p] { sendTestInstance(*p, 4), p->closeScreen(); }, saveOk);
    addButton(s, L, 210, 100, tr(s, "test_instance.action.run"), [p] { sendTestInstance(*p, 6), p->closeScreen(); });
    addButton(s, L + 108, 210, 100, tr(s, "gui.done"), [p] { sendTestInstance(*p, 2), p->closeScreen(); });
    addButton(s, L + 216, 210, 100, tr(s, "gui.cancel"), [p] { p->closeScreen(); });
    setFields(s, {&g.testId, &g.sizeX, &g.sizeY, &g.sizeZ}, 0);
}

void testInstanceDraw(UiState& s, Canvas& c) {
    const int W = c.guiWidth();
    const float L = float(W / 2 - 158);
    menuBackground(s, c);
    s.drawCentered(c, Text(s.res.translate("block.minecraft.test_instance_block")), float(W / 2), 10, 0xFFFFFFFF);
    label(s, c, "test_instance_block.test_id", L, 30);
    label(s, c, "test_instance_block.size", L, 150);
    if (s.buttons.size() >= 2) {
        label(s, c, "test_instance_block.rotation", s.buttons[0].x, 150);
        label(s, c, "test_instance_block.entities", s.buttons[1].x, 150);
    }
    float y = 70;  // the info text (status from the server)
    for (const Text& line : s.font.split(g.info, 316)) {
        if (y > 70 + 72 - 9) break;
        s.drawText(c, line, L, y, 0xFFFFFFFF, true);
        y += 9;
    }
    drawFields(s, c);
    s.drawButtons(c);
}

void testInstanceInput(UiState& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) return s.closeScreen();
    const std::string before = g.testId.text.value;
    if (fieldsInput(s, e)) {
        if (g.testId.text.value != before) {  // updateTestInfo(QUERY) as the id changes
            sendTestInstance(s, 1);
            s.buildButtons();
        }
        return;
    }
    s.buttonsInput(e);
}

void closed(UiState& s) { setFields(s, {}, -1); }

const ScreenRegistrar kCommand({Screen::CommandBlockEdit, commandBuild, commandDraw, commandInput, closed});
const ScreenRegistrar kMinecart({Screen::MinecartCommandEdit, commandBuild, commandDraw, commandInput, closed});
const ScreenRegistrar kJigsaw({Screen::JigsawEdit, jigsawBuild, jigsawDraw, jigsawInput, closed});
const ScreenRegistrar kStructure({Screen::StructureEdit, structureBuild, structureDraw, structureInput, closed});
const ScreenRegistrar kTest({Screen::TestBlockEdit, testBuild, testDraw, simpleInput, closed});
const ScreenRegistrar kTestInstance({Screen::TestInstanceEdit, testInstanceBuild, testInstanceDraw, testInstanceInput, closed});

// ClientboundTestInstanceBlockStatus: the status text (and the structure's size) for the open editor.
void registerHandlers(UiState& s, net::Client& client) {
    UiState* sp = &s;
    client.onPlay("test_instance_block_status", [sp](net::Reader& r) {
        Text status = parseComponent(r.nbt(), sp->lang());
        int size[3] = {0, 0, 0};
        const bool hasSize = r.boolean();
        if (hasSize) size[0] = r.varInt(), size[1] = r.varInt(), size[2] = r.varInt();
        if (!r.ok) return;
        sp->post([sp, status, hasSize, size] {
            if (sp->screen != Screen::TestInstanceEdit) return;
            g.info = status;
            if (hasSize) setField(g.sizeX, std::to_string(size[0]), 15), setField(g.sizeY, std::to_string(size[1]), 15),
                setField(g.sizeZ, std::to_string(size[2]), 15);
            sp->buildButtons();
        });
    });
}

const UiModuleRegistrar kModule({.name = "block_editors", .registerHandlers = registerHandlers});

}  // namespace

bool canUseGameMasterBlocks(UiState& s) { return s.creative() && permissionLevel() >= 2; }  // Player.canUseGameMasterBlocks

void operatorBoxes(UiState& s, std::vector<LineVertex>& lines, const CameraState& cam, float screenW, float screenH) {
    if (!s.world || !s.registry || !(canUseGameMasterBlocks(s) || s.spectator())) return;
    const float wx = 2.5f / screenW, wy = 2.5f / screenH;  // GizmoStyle.stroke: 2.5 wide
    auto line = [&](double ax, double ay, double az, double bx, double by, double bz, u32 abgr) {
        lines.push_back({float(ax - cam.x), float(ay - cam.y), float(az - cam.z), abgr, wx, wy});
        lines.push_back({float(bx - cam.x), float(by - cam.y), float(bz - cam.z), abgr, wx, wy});
    };
    // CuboidGizmo: twelve edges; the three from the low corner tinted red / green / blue along x / y / z.
    auto cuboid = [&](double x0, double y0, double z0, double x1, double y1, double z1, u32 c, bool corner) {
        line(x0, y0, z0, x1, y0, z0, corner ? 0xFF6A6AE5u : c);
        line(x0, y0, z0, x0, y1, z0, corner ? 0xFF6AE56Au : c);
        line(x0, y0, z0, x0, y0, z1, corner ? 0xFFE56A6Au : c);
        line(x1, y0, z0, x1, y1, z0, c), line(x1, y1, z0, x0, y1, z0, c), line(x0, y1, z0, x0, y1, z1, c);
        line(x0, y1, z1, x0, y0, z1, c), line(x0, y0, z1, x1, y0, z1, c), line(x1, y0, z1, x1, y0, z0, c);
        line(x0, y1, z1, x1, y1, z1, c), line(x1, y0, z1, x1, y1, z1, c), line(x1, y1, z0, x1, y1, z1, c);
    };
    const int ccx = int(std::floor(cam.x)) >> 4, ccz = int(std::floor(cam.z)) >> 4;
    std::shared_lock lock(s.world->mutex());
    for (int cz = ccz - 6; cz <= ccz + 6; ++cz)
        for (int cx = ccx - 6; cx <= ccx + 6; ++cx) {
            const auto* list = s.world->blockEntities(cx, cz);
            if (!list) continue;
            for (const World::BlockEntityNbt& be : *list) {
                const double dx = be.x + 0.5 - cam.x, dy = be.y + 0.5 - cam.y, dz = be.z + 0.5 - cam.z;
                if (dx * dx + dy * dy + dz * dz > 96.0 * 96.0) continue;  // getViewDistance
                std::string n = s.registry->name(s.world->block(be.x, be.y, be.z));
                const bool structure = n.starts_with("minecraft:structure_block"), test = n.starts_with("minecraft:test_instance_block");
                if (!structure && !test) continue;
                NbtValue nbt;
                if (!nbtReadNetwork(reinterpret_cast<const u8*>(be.nbt.data()), be.nbt.size(), nbt)) continue;
                int box[6];  // local min, size
                bool invisible = false;
                if (structure) {  // StructureBlockEntity.renderMode / getRenderableBox
                    const std::string mode = str(&nbt, "mode", "DATA");
                    if (mode != "SAVE" && mode != "LOAD") continue;
                    invisible = mode == "SAVE" && num(&nbt, "showair", 0) != 0;
                    if (!invisible && mode != "SAVE" && num(&nbt, "showboundingbox", 1) == 0) continue;
                    const int px = int(num(&nbt, "posX", 0)), py = int(num(&nbt, "posY", 1)), pz = int(num(&nbt, "posZ", 0));
                    const int sx = int(num(&nbt, "sizeX", 0)), sy = int(num(&nbt, "sizeY", 0)), sz = int(num(&nbt, "sizeZ", 0));
                    const std::string mirror = str(&nbt, "mirror", "NONE"), rotation = str(&nbt, "rotation", "NONE");
                    const int xDiff = mirror == "FRONT_BACK" ? -sx : sx, zDiff = mirror == "LEFT_RIGHT" ? -sz : sz;
                    int x0, z0, x1, z1;
                    if (rotation == "CLOCKWISE_90") x0 = zDiff < 0 ? px : px + 1, z0 = xDiff < 0 ? pz + 1 : pz, x1 = x0 - zDiff, z1 = z0 + xDiff;
                    else if (rotation == "CLOCKWISE_180") x0 = xDiff < 0 ? px : px + 1, z0 = zDiff < 0 ? pz : pz + 1, x1 = x0 - xDiff, z1 = z0 - zDiff;
                    else if (rotation == "COUNTERCLOCKWISE_90") x0 = zDiff < 0 ? px + 1 : px, z0 = xDiff < 0 ? pz : pz + 1, x1 = x0 + zDiff, z1 = z0 - xDiff;
                    else x0 = xDiff < 0 ? px + 1 : px, z0 = zDiff < 0 ? pz + 1 : pz, x1 = x0 + xDiff, z1 = z0 + zDiff;
                    box[0] = std::min(x0, x1), box[1] = py, box[2] = std::min(z0, z1);
                    box[3] = std::abs(x1 - x0), box[4] = sy, box[5] = std::abs(z1 - z0);
                } else {  // TestInstanceBlockEntity: STRUCTURE_OFFSET (0, 1, 1), the size turned with the rotation
                    const NbtValue* d = nbtString(&nbt, "data");
                    const NbtValue* size = nbtString(d, "size");
                    if (!size || size->type != TagIntArray || size->ints.size() != 3) continue;
                    const std::string rot = str(d, "rotation", "none");
                    const bool swap = rot == "clockwise_90" || rot == "counterclockwise_90";
                    box[0] = 0, box[1] = 1, box[2] = 1;
                    box[3] = swap ? size->ints[2] : size->ints[0], box[4] = size->ints[1], box[5] = swap ? size->ints[0] : size->ints[2];
                }
                if (box[3] < 1 || box[4] < 1 || box[5] < 1) continue;
                const double ox = be.x + box[0], oy = be.y + box[1], oz = be.z + box[2];
                cuboid(ox, oy, oz, ox + box[3], oy + box[4], oz + box[5], 0xFFE5E5E5u, true);
                if (!invisible) continue;
                for (int x = 0; x < box[3] && lines.size() < 400000; ++x)  // renderInvisibleBlocks
                    for (int y = 0; y < box[4]; ++y)
                        for (int z = 0; z < box[5]; ++z) {
                            const int bx = int(ox) + x, by = int(oy) + y, bz = int(oz) + z;
                            const std::string bn = s.registry->name(s.world->block(bx, by, bz));
                            u32 c;
                            double scale = 0.0;
                            if (bn == "minecraft:air" || bn == "minecraft:cave_air" || bn == "minecraft:void_air") c = 0xFFFF7F7Fu, scale = 0.05;
                            else if (bn == "minecraft:structure_void") c = 0xFFBFBFFFu;
                            else if (bn.starts_with("minecraft:barrier")) c = 0xFF0000FFu;
                            else if (bn.starts_with("minecraft:light")) c = 0xFF00FFFFu;
                            else continue;
                            cuboid(bx + 0.45 - scale, by + 0.45 - scale, bz + 0.45 - scale, bx + 0.55 + scale, by + 0.55 + scale, bz + 0.55 + scale, c, false);
                        }
            }
        }
}

bool openBlockEditor(UiState& s, int x, int y, int z) {
    if (!s.world || !s.registry || !canUseGameMasterBlocks(s)) return false;
    g = Editor{};
    g.x = x, g.y = y, g.z = z;
    g.block = blockName(s);
    NbtValue nbt;
    const bool have = blockEntity(s, x, y, z, nbt, &g.serial);
    const NbtValue* n = have ? &nbt : nullptr;
    if (g.block == "command_block" || g.block == "chain_command_block" || g.block == "repeating_command_block") {
        if (have) loadCommandBlock(s);
        g.ready = false;  // until the server's block entity data
        s.openScreen(Screen::CommandBlockEdit);
        return true;
    }
    if (g.block == "jigsaw") {
        auto plain = [](std::string v) { return v.empty() ? std::string("minecraft:empty") : v; };
        setField(g.pool, plain(str(n, "pool")), 128), setField(g.name, plain(str(n, "name")), 128), setField(g.target, plain(str(n, "target")), 128);
        setField(g.finalState, str(n, "final_state", "minecraft:air"), 256);
        setField(g.selectionPriority, std::to_string(num(n, "selection_priority", 0)), 3);
        setField(g.placementPriority, std::to_string(num(n, "placement_priority", 0)), 3);
        const std::string state = s.registry->name(s.world->block(x, y, z));
        g.vertical = state.find("orientation=up_") != std::string::npos || state.find("orientation=down_") != std::string::npos;
        g.joint = str(n, "joint", g.vertical ? "rollable" : "aligned") == "aligned" ? 1 : 0;
        s.openScreen(Screen::JigsawEdit);
        return true;
    }
    if (g.block == "structure_block") {
        auto enumIndex = [&](const char* key, std::initializer_list<const char*> names, int def) {
            const std::string v = str(n, key);
            int i = 0;
            for (const char* name : names) {
                if (v == name) return i;
                ++i;
            }
            return def;
        };
        g.structMode = enumIndex("mode", {"SAVE", "LOAD", "CORNER", "DATA"}, 3);
        g.mirror = enumIndex("mirror", {"NONE", "LEFT_RIGHT", "FRONT_BACK"}, 0);
        g.rotation = enumIndex("rotation", {"NONE", "CLOCKWISE_90", "CLOCKWISE_180", "COUNTERCLOCKWISE_90"}, 0);
        g.ignoreEntities = num(n, "ignoreEntities", 1) != 0, g.strict = num(n, "strict", 0) != 0;
        g.showAir = num(n, "showair", 0) != 0, g.showBoundingBox = num(n, "showboundingbox", 1) != 0;
        g_backup = {g.structMode, g.mirror, g.rotation, g.ignoreEntities, g.strict, g.showAir, g.showBoundingBox};
        setField(g.structName, str(n, "name"), 128);
        setField(g.posX, std::to_string(num(n, "posX", 0)), 15), setField(g.posY, std::to_string(num(n, "posY", 1)), 15),
            setField(g.posZ, std::to_string(num(n, "posZ", 0)), 15);
        setField(g.sizeX, std::to_string(num(n, "sizeX", 0)), 15), setField(g.sizeY, std::to_string(num(n, "sizeY", 0)), 15),
            setField(g.sizeZ, std::to_string(num(n, "sizeZ", 0)), 15);
        char buf[32];  // DecimalFormat("0.0###")
        std::snprintf(buf, sizeof(buf), "%.4f", real(n, "integrity", 1.0));
        std::string integ = buf;
        while (integ.size() > 3 && integ.back() == '0' && integ[integ.size() - 2] != '.') integ.pop_back();
        setField(g.integrity, integ, 15);
        setField(g.seed, std::to_string(num(n, "seed", 0)), 31);
        setField(g.data, str(n, "metadata"), 128);
        s.openScreen(Screen::StructureEdit);
        return true;
    }
    if (g.block == "test_block") {
        const std::string mode = str(n, "mode", "fail");
        for (int i = 0; i < 4; ++i)
            if (mode == kTestModes[i]) g.testMode = i;
        setField(g.message, str(n, "message"), 128);
        s.openScreen(Screen::TestBlockEdit);
        return true;
    }
    if (g.block == "test_instance_block") {
        const NbtValue* d = nbtString(n, "data");
        setField(g.testId, str(d, "test"), 128);
        const NbtValue* size = nbtString(d, "size");
        int sz[3] = {5, 5, 5};
        if (size && size->type == TagIntArray && size->ints.size() == 3)
            for (int i = 0; i < 3; ++i) sz[i] = size->ints[size_t(i)];
        setField(g.sizeX, std::to_string(sz[0]), 15), setField(g.sizeY, std::to_string(sz[1]), 15), setField(g.sizeZ, std::to_string(sz[2]), 15);
        const std::string rot = str(d, "rotation", "none");
        static const char* const kRot[4] = {"none", "clockwise_90", "180", "counterclockwise_90"};
        for (int i = 0; i < 4; ++i)
            if (rot == kRot[i]) g.rotation = i;
        g.includeEntities = num(d, "ignore_entities", 0) == 0;
        s.openScreen(Screen::TestInstanceEdit);
        sendTestInstance(s, 0);  // updateTestInfo(INIT)
        return true;
    }
    return false;
}

bool openMinecartCommandEditor(UiState& s, int entity, const std::string& command, const Text& lastOutput) {
    if (!canUseGameMasterBlocks(s)) return false;
    g = Editor{};
    g.entity = entity;
    setField(g.command, command, 32500);
    g.lastOutput = lastOutput;
    s.openScreen(Screen::MinecartCommandEdit);
    return true;
}

}  // namespace ui
