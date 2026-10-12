// Signs and books: AbstractSignEditScreen (SignEditScreen / HangingSignEditScreen), BookEditScreen + BookSignScreen,
// BookViewScreen (open_book) and LecternScreen (menu "lectern").
#include "ui/signs_books.h"

#include "assets/assets.h"
#include "audio/audio.h"
#include "blockentity/block_entities.h"
#include "ui/frontend.h"
#include "ui/ui_state.h"
#include "world/world.h"

#include <shellapi.h>

#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace ui {

namespace {
constexpr u32 kBlack = 0xFF000000;

// DyeColor.getTextColor by dye id.
const u32 kDyeText[16] = {0xFFFFFF, 0xFF681F, 0xFF00FF, 0x9AC0CD, 0xFFFF00, 0xBFFF00, 0xFF69B4, 0x808080,
                          0xD3D3D3, 0x00FFFF, 0xA020F0, 0x0000FF, 0xA52A2A, 0x00FF00, 0xFF0000, 0x000000};

// PageButton.playDownSound (book edit and written books; the lectern's view plays none).
void pageSound() {
    if (audio::AudioSystem* a = audio::instance()) a->playUi("minecraft:item.book.page_turn");
}

bool cursorVisible(const UiState& s, double start) { return int64_t((s.clock - start) * 1000.0) / 300 % 2 == 0; }

// GuiGraphics.textHighlight: inverted blue.
void highlight(Canvas& c, float x0, float y0, float x1, float y1) {
    c.setInvert(true);
    c.fill(std::min(x0, x1), y0, std::max(x0, x1), y1, 0xFF0000FF);
    c.setInvert(false);
}

void sbButton(UiState& s, float x, float y, float w, Text label, std::function<void()> fn, bool active = true) {
    Button b;
    b.x = x, b.y = y, b.w = w, b.h = 20, b.label = std::move(label), b.onPress = std::move(fn), b.active = active;
    s.buttons.push_back(std::move(b));
}

void sbBackground(UiState& s, Canvas& c) {
    c.gradient(0, 0, float(c.guiWidth()), float(c.guiHeight()), 0xC0101010, 0xD0101010);
    (void)s;
}

std::string utf8Of(u32 cp) {
    std::string r;
    appendUtf8(r, cp);
    return r;
}

// --- book contents -----------------------------------------------------------------------------------------------

struct BookContent {
    bool written = false, valid = false;
    std::vector<std::string> raw;  // writable pages
    std::vector<Text> pages;       // display pages
    std::string title, author;
};

BookContent readBook(UiState& s, const ItemStack& st) {
    BookContent b;
    if (st.empty()) return b;
    const int writable = s.items.componentType("writable_book_content"), written = s.items.componentType("written_book_content");
    for (const ItemStack::Component& comp : st.added) {
        net::Reader r(reinterpret_cast<const u8*>(comp.data.data()), comp.data.size());
        if (comp.type == written && written >= 0) {
            b.written = b.valid = true;
            b.title = std::string(r.str());
            if (r.boolean()) r.str();  // filtered title
            b.author = std::string(r.str());
            r.varInt();  // generation
            const i32 n = r.varInt();
            for (i32 i = 0; i < n && r.ok; ++i) {
                Text page = parseComponent(r.nbt(), s.lang());
                if (r.boolean()) r.nbt();
                if (r.ok) b.pages.push_back(std::move(page));
            }
            return b;
        }
        if (comp.type == writable && writable >= 0) {
            b.valid = true;
            const i32 n = r.varInt();
            for (i32 i = 0; i < n && r.ok; ++i) {
                std::string page(r.str());
                if (r.boolean()) r.str();
                if (r.ok) b.raw.push_back(page), b.pages.push_back(Text(page));
            }
        }
    }
    const std::string name = s.items.name(st.item);
    if (name == "minecraft:writable_book" || name == "minecraft:written_book") b.valid = true;
    return b;
}

// --- the book background, page text, page buttons (BookViewScreen) ----------------------------------------------

int bookLeft(const Canvas& c) { return (c.guiWidth() - 192) / 2; }
constexpr int kBookTop = 2;

void drawBookBackground(UiState& s, Canvas& c) {
    blitTexture(c, s.sprites.texture("gui/book"), float(bookLeft(c)), float(kBookTop), 0, 0, 192, 192, 256, 256);
}

void drawPageIndicator(UiState& s, Canvas& c, int page, int count) {
    const Text msg = translatable("book.pageIndicator", {Text(std::to_string(page + 1)), Text(std::to_string(count))}, s.lang());
    s.font.draw(c, msg, float(bookLeft(c) + 148 - s.font.width(msg)), float(kBookTop + 16), kBlack, false);
}

bool overPageButton(const UiState& s, const Canvas& c, bool forward) {
    const float x = float(bookLeft(c) + (forward ? 116 : 43)), y = float(kBookTop + 157);
    return s.mouseX >= x && s.mouseY >= y && s.mouseX < x + 23 && s.mouseY < y + 13;
}
void drawPageButton(UiState& s, Canvas& c, bool forward) {
    const bool hover = overPageButton(s, c, forward);
    const std::string sp = std::string("widget/page_") + (forward ? "forward" : "backward") + (hover ? "_highlighted" : "");
    blitSprite(c, s.sprites.sprite(sp), float(bookLeft(c) + (forward ? 116 : 43)), float(kBookTop + 157), 23, 13);
}

void drawBookPage(UiState& s, Canvas& c, const std::vector<Text>& pages, int page) {
    const int count = int(pages.size());
    drawPageIndicator(s, c, page, std::max(count, 1));
    if (page < 0 || page >= count) return;
    const std::vector<Text> lines = s.font.split(pages[size_t(page)], 114);
    const int shown = std::min(128 / 9, int(lines.size()));
    for (int i = 0; i < shown; ++i)
        s.font.draw(c, lines[size_t(i)], float(bookLeft(c) + 36), float(kBookTop + 30 + i * 9), kBlack, false);
}

// --- sign editor ---------------------------------------------------------------------------------------------------

struct SignEdit {
    int x = 0, y = 0, z = 0;
    int slot = 1;  // SignTextSlot: 0 back, 1 front
    bool hanging = false, wall = false;
    std::string wood = "oak";
    u32 color = kBlack;
    std::string lines[4];
    int line = 0;
    UiState::TextField field;  // the current line
    int lineHeight = 10, maxWidth = 90;
    bool open = false;
} sign;

void signSetLine(int line) {
    sign.lines[sign.line] = sign.field.value;
    sign.line = line & 3;
    sign.field.value = sign.lines[sign.line];
    sign.field.cursor = sign.field.selection = int(sign.field.value.size());  // setCursorToEnd
}

void openSign(UiState& s, int x, int y, int z, int slot) {
    sign = SignEdit{};
    sign.x = x, sign.y = y, sign.z = z, sign.slot = slot;
    std::string n = s.registry->name(s.world->block(x, y, z));
    n = n.substr(n.find(':') + 1);
    n = n.substr(0, n.find('['));
    sign.hanging = n.find("hanging_sign") != std::string::npos;
    sign.wall = n.find("wall_") != std::string::npos;
    for (const char* suffix : {"_wall_hanging_sign", "_hanging_sign", "_wall_sign", "_sign"})
        if (n.ends_with(suffix)) {
            sign.wood = n.substr(0, n.size() - std::strlen(suffix));
            break;
        }
    if (!s.sprites.texture(std::string(sign.hanging ? "gui/hanging_signs/" : "gui/signs/") + sign.wood)) sign.wood = "oak";
    sign.lineHeight = sign.hanging ? 9 : 10;
    sign.maxWidth = sign.hanging ? 60 : 90;
    // The current text (block entity data) and its colour: glowing text in the dye colour, else darkened.
    if (const be::BlockEntityRenderer* ber = be::BlockEntityRenderer::instance())
        if (const be::BlockEntityRenderer::Data* d = ber->dataAt(x, y, z)) {
            const be::BlockEntityRenderer::SignSide& side = slot == 0 ? d->back : d->front;
            for (int i = 0; i < 4; ++i) sign.lines[i] = side.lines[i].plain();
            const u32 dye = kDyeText[side.color & 15];
            if (side.glow) sign.color = 0xFF000000 | dye;
            else sign.color = 0xFF000000 | (scaleRgb(dye, 0.4f) & 0xFFFFFF);
        }
    sign.field.maxLength = 384;
    sign.field.blinkStart = s.clock;
    sign.open = true;
    sign.line = 0;
    sign.field.value = sign.lines[0];
    sign.field.cursor = sign.field.selection = int(sign.field.value.size());
    s.openScreen(Screen::Sign);
}

void signBuild(UiState& s) {
    s.buttons.clear();
    const int W = s.guiW(), H = s.guiH();
    sbButton(s, float(W / 2 - 100), float(H / 4 + 144), 200, tr(s, "gui.done"), [&s] { s.closeScreen(); });
}

void signDraw(UiState& s, Canvas& c) {
    sbBackground(s, c);
    const int W = c.guiWidth();
    s.drawCentered(c, tr(s, sign.hanging ? "hanging_sign.edit" : "sign.edit"), float(W / 2), 40, 0xFFFFFFFF);
    c.push();
    c.translate(float(W) / 2.0f, sign.hanging ? 125.0f : 90.0f);
    c.push();
    if (sign.hanging) {
        c.translate(0, -13);
        c.scaleBy(4.5f);
        blitTexture(c, s.sprites.texture("gui/hanging_signs/" + sign.wood), -8, -8, 0, 0, 16, 16, 16, 16);
    } else {
        c.translate(0, 27);
        c.scaleBy(3.9f);
        blitTexture(c, s.sprites.texture("gui/signs/" + sign.wood), -12, -13, 0, 0, 24, float(sign.wall ? 12 : 26), 24, 26);
    }
    c.pop();
    if (!sign.hanging) c.scaleBy(0.9765628f);
    sign.lines[sign.line] = sign.field.value;
    const bool showCursor = cursorVisible(s, sign.field.blinkStart);
    const int lh = sign.lineHeight, mid = 4 * lh / 2, cursorY = sign.line * lh - mid;
    const int cursor = std::clamp(sign.field.cursor, 0, int(sign.field.value.size()));
    const int selection = std::clamp(sign.field.selection, 0, int(sign.field.value.size()));
    for (int i = 0; i < 4; ++i) {
        const std::string& line = sign.lines[i];
        const int w = s.font.width(line);
        s.font.draw(c, line, float(-w / 2), float(i * lh - mid), sign.color, false);
        if (i != sign.line) continue;
        const int cx = s.font.width(line.substr(0, size_t(cursor))) - w / 2;
        if (showCursor) {
            if (cursor >= int(line.size())) s.font.draw(c, "_", float(cx), float(cursorY), sign.color, false);
            else c.fill(float(cx), float(cursorY - 1), float(cx + 1), float(cursorY + lh), 0xFF000000 | sign.color);
        }
        if (selection != cursor) {
            const int a = s.font.width(line.substr(0, size_t(std::min(cursor, selection)))) - w / 2;
            const int b = s.font.width(line.substr(0, size_t(std::max(cursor, selection)))) - w / 2;
            highlight(c, float(a), float(cursorY), float(b), float(cursorY + lh));
        }
    }
    c.pop();
    s.drawButtons(c);
}

void signInput(UiState& s, const InputEvent& e) {
    using E = InputEvent;
    if (e.type == E::Key && e.down) {
        if (e.code == VK_ESCAPE) return s.closeScreen();
        if (e.code == VK_UP) return signSetLine(sign.line - 1);
        if (e.code == VK_DOWN || e.code == VK_RETURN) return signSetLine(sign.line + 1);
    }
    if (e.type == E::Char || e.type == E::Key) {
        // TextFieldHelper with the sign's line width as the filter: an edit that does not fit is rejected.
        const UiState::TextField before = sign.field;
        s.textFieldKey(sign.field, e);
        if (s.font.width(sign.field.value) > sign.maxWidth) sign.field = before;
        sign.lines[sign.line] = sign.field.value;
        return;
    }
    s.buttonsInput(e);
}

// AbstractSignEditScreen.tick: done when the sign is gone (SignBlockEntity removed) or the player left its edit range
// (SignBlockEntity.playerIsTooFarAwayToEdit: Player.isWithinBlockInteractionRange(pos, 4)).
void signTick(UiState& s) {
    if (s.screen != Screen::Sign || !sign.open || !s.world || !s.registry || !s.game) return;
    const std::string& state = s.registry->name(s.world->block(sign.x, sign.y, sign.z));
    bool removed = state.find("_sign") == std::string::npos;
    const CameraState eye = s.game->camera();
    // AABB(pos).distanceToSqr(eyes) < (block_interaction_range + 4)^2 (range 4.5, creative 5)
    auto axis = [](double v, int lo) { return v < lo ? lo - v : v > lo + 1 ? v - (lo + 1) : 0.0; };
    const double dx = axis(eye.x, sign.x), dy = axis(eye.y, sign.y), dz = axis(eye.z, sign.z);
    const double range = (s.gameMode() == 1 ? 5.0 : 4.5) + 4.0;
    const bool tooFar = dx * dx + dy * dy + dz * dz >= range * range;
    if (removed || tooFar) {
        LOG("ui: sign editor closed (%s)", removed ? "sign removed" : "too far away");
        s.closeScreen();
    }
}

void signClosed(UiState& s) {
    if (!sign.open) return;
    sign.open = false;
    sign.lines[sign.line] = sign.field.value;
    if (!s.online()) return;
    net::Writer w;  // ServerboundSignUpdatePacket
    w.blockPos(sign.x, sign.y, sign.z);
    for (const std::string& l : sign.lines) w.str(l);
    w.varInt(sign.slot);
    s.client->sendPlay("sign_update", w);
}

// --- multi-line edit box (MultiLineEditBox / MultilineTextField) for book pages --------------------------------------

struct LineView {
    size_t begin, end;
};

float cpWidth(UiState& s, u32 cp) {
    static std::unordered_map<u32, float> cache;
    if (auto it = cache.find(cp); it != cache.end()) return it->second;
    const float w = s.font.widthF(Text(utf8Of(cp)));
    cache[cp] = w;
    return w;
}

// StringSplitter.splitLines: words wrap at spaces (the breaking space ends neither line), long words break, \n.
std::vector<LineView> wrapLines(UiState& s, const std::string& v, float width) {
    std::vector<LineView> out;
    size_t p0 = 0;
    for (;;) {
        size_t p1 = v.find('\n', p0);
        if (p1 == std::string::npos) p1 = v.size();
        size_t start = p0, lastSpace = std::string::npos;
        float x = 0;
        for (size_t i = p0; i < p1;) {
            size_t j = i;
            const u32 cp = nextCodepoint(v, j);
            const float adv = cpWidth(s, cp);
            if (x + adv > width && i > start) {
                if (lastSpace != std::string::npos && lastSpace >= start) {
                    out.push_back({start, lastSpace});
                    start = lastSpace + 1;
                    x = 0;
                    for (size_t k = start; k < i;) x += cpWidth(s, nextCodepoint(v, k));
                } else {
                    out.push_back({start, i});
                    start = i, x = 0;
                }
                lastSpace = std::string::npos;
            }
            x += adv;
            if (cp == ' ') lastSpace = i;
            i = j;
        }
        out.push_back({start, p1});
        if (p1 >= v.size()) break;
        p0 = p1 + 1;
    }
    return out;
}

size_t prevCp(const std::string& s, size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (u8(s[i]) & 0xC0) == 0x80) --i;
    return i;
}
size_t nextCp(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (u8(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}
size_t cpCount(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++n) nextCodepoint(s, i);
    return n;
}

struct MultiLine {
    std::string value;
    size_t cursor = 0, selection = 0;
    double blinkStart = 0;
    int charLimit = 1024, lineLimit = 14;
    float width = 114;
};

int lineOf(const std::vector<LineView>& lines, size_t cursor) {
    for (size_t i = 0; i < lines.size(); ++i)
        if (cursor >= lines[i].begin && cursor <= lines[i].end) return int(i);
    return int(lines.size()) - 1;
}

size_t indexAtX(UiState& s, const std::string& v, const LineView& l, float x) {
    float w = 0;
    for (size_t i = l.begin; i < l.end;) {
        size_t j = i;
        const float adv = cpWidth(s, nextCodepoint(v, j));
        if (w + adv / 2 > x) return i;
        w += adv;
        i = j;
    }
    return l.end;
}

bool mlInsert(UiState& s, MultiLine& m, const std::string& text) {
    std::string clean;
    for (size_t i = 0; i < text.size();) {
        const u32 cp = nextCodepoint(text, i);
        if ((cp >= 32 && cp != 127 && cp != 0xA7) || cp == '\n') appendUtf8(clean, cp);  // StringUtil.filterText (keep \n)
    }
    const size_t a = std::min(m.cursor, m.selection), b = std::max(m.cursor, m.selection);
    std::string v = m.value.substr(0, a) + clean + m.value.substr(b);
    if (int(cpCount(v)) > m.charLimit) return false;
    if (int(wrapLines(s, v, m.width).size()) > m.lineLimit) return false;  // overflowsLineLimit
    m.value = v;
    m.cursor = m.selection = a + clean.size();
    return true;
}

size_t wordPos(const std::string& s, size_t i, int dir) {
    if (dir < 0) {
        while (i > 0 && s[i - 1] == ' ') --i;
        while (i > 0 && s[i - 1] != ' ' && s[i - 1] != '\n') --i;
    } else {
        while (i < s.size() && s[i] != ' ' && s[i] != '\n') ++i;
        while (i < s.size() && s[i] == ' ') ++i;
    }
    return i;
}

void mlKey(UiState& s, MultiLine& m, const InputEvent& e) {
    using E = InputEvent;
    if (e.type == E::Char) {
        if (e.code >= 32) mlInsert(s, m, utf8Of(u32(e.code)));
        m.blinkStart = s.clock;
        return;
    }
    if (e.type != E::Key || !e.down) return;
    const bool shift = s.window->keyDown(VK_SHIFT), ctrl = s.window->keyDown(VK_CONTROL);
    m.blinkStart = s.clock;
    auto move = [&](size_t to) {
        m.cursor = to;
        if (!shift) m.selection = m.cursor;
    };
    const std::vector<LineView> lines = wrapLines(s, m.value, m.width);
    const int li = lineOf(lines, m.cursor);
    switch (e.code) {
        case VK_LEFT: move(ctrl ? wordPos(m.value, m.cursor, -1) : prevCp(m.value, m.cursor)); return;
        case VK_RIGHT: move(ctrl ? wordPos(m.value, m.cursor, 1) : nextCp(m.value, m.cursor)); return;
        case VK_UP:
        case VK_DOWN: {
            const int to = li + (e.code == VK_UP ? -1 : 1);
            if (to < 0 || to >= int(lines.size())) return;
            float x = 0;
            for (size_t k = lines[size_t(li)].begin; k < m.cursor;) x += cpWidth(s, nextCodepoint(m.value, k));
            move(indexAtX(s, m.value, lines[size_t(to)], x));
            return;
        }
        case VK_HOME: move(ctrl ? 0 : lines[size_t(li)].begin); return;
        case VK_END: move(ctrl ? m.value.size() : lines[size_t(li)].end); return;
        case VK_RETURN: mlInsert(s, m, "\n"); return;
        case VK_BACK:
        case VK_DELETE: {
            if (m.cursor != m.selection) {
                mlInsert(s, m, "");
                return;
            }
            const size_t to = e.code == VK_BACK ? (ctrl ? wordPos(m.value, m.cursor, -1) : prevCp(m.value, m.cursor))
                                                : (ctrl ? wordPos(m.value, m.cursor, 1) : nextCp(m.value, m.cursor));
            const size_t a = std::min(m.cursor, to), b = std::max(m.cursor, to);
            m.value.erase(a, b - a);
            m.cursor = m.selection = a;
            return;
        }
        case 'A':
            if (ctrl) m.selection = 0, m.cursor = m.value.size();
            return;
        case 'C':
        case 'X':
            if (ctrl && m.cursor != m.selection) {
                const size_t a = std::min(m.cursor, m.selection), b = std::max(m.cursor, m.selection);
                s.window->setClipboard(m.value.substr(a, b - a));
                if (e.code == 'X') mlInsert(s, m, "");
            }
            return;
        case 'V':
            if (ctrl) mlInsert(s, m, s.window->clipboard());
            return;
    }
}

// MultiLineEditBox.seekCursorScreen; selecting keeps the selection anchor (shift click, drag).
void mlSeek(UiState& s, MultiLine& m, float left, float top, float mx, float my, bool selecting) {
    const std::vector<LineView> lines = wrapLines(s, m.value, m.width);
    const int li = std::clamp(int(std::floor((my - top) / 9.0f)), 0, int(lines.size()) - 1);
    m.cursor = indexAtX(s, m.value, lines[size_t(li)], mx - left);
    if (!selecting) m.selection = m.cursor;
    m.blinkStart = s.clock;
}

void mlClick(UiState& s, MultiLine& m, float left, float top, float mx, float my) {
    mlSeek(s, m, left, top, mx, my, s.window->keyDown(VK_SHIFT));
}

// MultilineTextField.selectWordAtCursor (getPreviousWord: back over spaces, then to the word's start; to its end).
void mlSelectWord(MultiLine& m) {
    if (m.value.empty()) return;
    auto space = [&](size_t i) { return std::isspace(u8(m.value[i])) != 0; };
    size_t i = std::min(m.cursor, m.value.size() - 1);
    while (i > 0 && space(i - 1)) --i;
    while (i > 0 && !space(i - 1)) --i;
    size_t end = i;
    while (end < m.value.size() && !space(end)) ++end;
    m.selection = i;
    m.cursor = end;
}

void mlDraw(UiState& s, Canvas& c, const MultiLine& m, float left, float top, bool focused) {
    const std::vector<LineView> lines = wrapLines(s, m.value, m.width);
    const bool show = focused && cursorVisible(s, m.blinkStart);
    const bool insert = m.cursor < m.value.size();
    float y = top, cx = left, cy = top;
    for (const LineView& l : lines) {
        const std::string text = m.value.substr(l.begin, l.end - l.begin);
        s.font.draw(c, text, left, y, kBlack, false);
        if (m.cursor >= l.begin && m.cursor <= l.end) {
            cx = left + float(s.font.width(m.value.substr(l.begin, m.cursor - l.begin))), cy = y;
            if (show && insert) c.fill(cx, y - 1, cx + 1, y + 10, kBlack);
        }
        y += 9;
    }
    if (show && !insert) s.font.draw(c, "_", cx, cy, kBlack, false);
    if (m.cursor != m.selection) {
        const size_t a = std::min(m.cursor, m.selection), b = std::max(m.cursor, m.selection);
        y = top;
        for (const LineView& l : lines) {
            if (a <= l.end && b >= l.begin) {
                const float x0 = left + float(s.font.width(m.value.substr(l.begin, std::max(a, l.begin) - l.begin)));
                const float x1 = b > l.end ? left + m.width : left + float(s.font.width(m.value.substr(l.begin, b - l.begin)));
                highlight(c, x0, y, x1, y + 9);
            }
            y += 9;
        }
    }
}

// --- book and quill (BookEditScreen + BookSignScreen) -------------------------------------------------------------

struct BookEdit {
    int hand = 0;
    std::vector<std::string> pages;
    int page = 0;
    MultiLine field;
    bool signing = false;
    UiState::TextField title;
    bool dragging = false;   // the left button went down in the page and is held (drag selection)
    double lastClick = -1;   // double click: select the word
} edit;

void editShowPage() {
    edit.field.value = edit.pages[size_t(edit.page)];
    edit.field.cursor = edit.field.selection = edit.field.value.size();
}
void editStore() { edit.pages[size_t(edit.page)] = edit.field.value; }

void editTurn(UiState& s, bool forward) {
    pageSound();
    editStore();
    if (forward) {
        if (edit.page < int(edit.pages.size()) - 1) ++edit.page;
        else if (edit.pages.size() < 100) edit.pages.push_back(""), ++edit.page;  // appendPageToBook
    } else if (edit.page > 0) {
        --edit.page;
    }
    editShowPage();
    edit.field.blinkStart = s.clock;
}

void editSend(UiState& s, bool sign) {
    editStore();
    if (!sign)
        while (!edit.pages.empty() && edit.pages.back().empty()) edit.pages.pop_back();  // eraseEmptyTrailingPages
    if (!s.online()) return;
    net::Writer w;  // ServerboundEditBookPacket
    w.varInt(edit.hand == 0 ? s.game->selectedSlot() : 40);
    w.varInt(i32(edit.pages.size()));
    for (const std::string& p : edit.pages) w.str(p);
    w.boolean(sign);
    if (sign) {
        std::string t = edit.title.value;
        while (!t.empty() && t.back() == ' ') t.pop_back();
        while (!t.empty() && t.front() == ' ') t.erase(t.begin());
        w.str(t);
    }
    s.client->sendPlay("edit_book", w);
    LOG("ui: edit_book %zu pages%s", edit.pages.size(), sign ? " (signed)" : "");
}

bool titleBlank() {
    for (char ch : edit.title.value)
        if (ch != ' ') return false;
    return true;
}

void editBuild(UiState& s) {
    s.buttons.clear();
    const int W = s.guiW();
    if (edit.signing) {
        sbButton(s, float(W / 2 - 100), 196, 98, tr(s, "book.finalizeButton"), [&s] {
            editSend(s, true);
            s.closeScreen();
        }, !titleBlank());
        sbButton(s, float(W / 2 + 2), 196, 98, tr(s, "gui.cancel"), [&s] {
            edit.signing = false;
            s.buildButtons();
        });
        return;
    }
    const int y = kBookTop + 192 + 2;
    sbButton(s, float(W / 2 - 98 - 2), float(y), 98, tr(s, "book.signButton"), [&s] {
        editStore();
        edit.signing = true;
        edit.title.blinkStart = s.clock;
        s.buildButtons();
    });
    sbButton(s, float(W / 2 + 2), float(y), 98, tr(s, "gui.done"), [&s] {
        editSend(s, false);
        s.closeScreen();
    });
}

float editFieldLeft(int W) { return float((W - 114) / 2 - 8 + 4); }
constexpr float kEditFieldTop = 28 + 4;

void editDraw(UiState& s, Canvas& c) {
    sbBackground(s, c);
    drawBookBackground(s, c);
    const int W = c.guiWidth(), xo = bookLeft(c);
    if (edit.signing) {
        if (!s.buttons.empty()) s.buttons[0].active = !titleBlank();
        const Text head = tr(s, "book.editTitle");
        s.font.draw(c, head, float(xo + 36 + (114 - s.font.width(head)) / 2), 34, kBlack, false);
        // The title box (EditBox, unbordered and centred: text at y 50, x (W - 114) / 2 - 3 + (114 - width) / 2).
        const std::string& t = edit.title.value;
        const int tw = s.font.width(t);
        const float tx = float((W - 114) / 2 - 3) + float((114 - tw) / 2), ty = 50;
        s.font.draw(c, t, tx, ty, kBlack, false);
        if (cursorVisible(s, edit.title.blinkStart)) {
            const float cx = tx + float(s.font.width(t.substr(0, size_t(edit.title.cursor))));
            if (edit.title.cursor >= int(t.size())) s.font.draw(c, "_", cx, ty, kBlack, false);
            else c.fill(cx, ty - 1, cx + 1, ty + 9, kBlack);
        }
        const Text owner = translatable("book.byAuthor", {Text(s.fe ? s.fe->cfg.playerName : std::string())}, s.lang(),
                                        colorStyle(0x555555));
        s.font.draw(c, owner, float(xo + 36 + (114 - s.font.width(owner)) / 2), 60, kBlack, false);
        float y = 82;
        for (const Text& l : s.font.split(tr(s, "book.finalizeWarning"), 114)) s.font.draw(c, l, float(xo + 36), y, kBlack, false), y += 9;
        s.drawButtons(c);
        return;
    }
    edit.pages[size_t(edit.page)] = edit.field.value;
    drawPageIndicator(s, c, edit.page, int(edit.pages.size()));
    if (edit.dragging) {  // MultiLineEditBox.onDrag: the cursor follows the mouse, the selection anchor stays
        if (!s.leftDown) edit.dragging = false;
        else mlSeek(s, edit.field, editFieldLeft(W), kEditFieldTop, s.mouseX, s.mouseY, true);
    }
    mlDraw(s, c, edit.field, editFieldLeft(W), kEditFieldTop, true);
    drawPageButton(s, c, true);
    if (edit.page > 0) drawPageButton(s, c, false);
    s.drawButtons(c);
}

void editInput(UiState& s, const InputEvent& e) {
    using E = InputEvent;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) return s.closeScreen();  // Screen.onClose: not saved
    if (edit.signing) {
        if (e.type == E::Key && e.down && e.code == VK_RETURN && !edit.title.value.empty()) {
            editSend(s, true);
            return s.closeScreen();
        }
        if (e.type == E::Char || e.type == E::Key) {
            s.textFieldKey(edit.title, e);
            return;
        }
        s.buttonsInput(e);
        return;
    }
    if (e.type == E::Key && e.down && (e.code == VK_PRIOR || e.code == VK_NEXT)) return editTurn(s, e.code == VK_NEXT);
    if (e.type == E::Char || e.type == E::Key) return mlKey(s, edit.field, e);
    if (e.type == E::Button && e.down && e.code == 0) {
        if (s.canvas && overPageButton(s, *s.canvas, true)) return editTurn(s, true);
        if (s.canvas && edit.page > 0 && overPageButton(s, *s.canvas, false)) return editTurn(s, false);
        const float left = editFieldLeft(s.guiW());
        if (s.mouseX >= left - 4 && s.mouseX < left + 118 && s.mouseY >= kEditFieldTop - 4 && s.mouseY < kEditFieldTop + 130) {
            const bool doubleClick = s.clock - edit.lastClick < 0.25;  // MouseHandler: 250 ms
            edit.lastClick = s.clock;
            if (doubleClick) {
                mlSelectWord(edit.field);
            } else {
                mlClick(s, edit.field, left, kEditFieldTop, s.mouseX, s.mouseY);
                edit.dragging = true;
            }
            return;
        }
    }
    s.buttonsInput(e);
}

// --- written book (BookViewScreen) ---------------------------------------------------------------------------------

struct BookView {
    std::vector<Text> pages;
    int page = 0;
} view;

// The page text's style under the mouse (BookViewScreen.visitText with a ClickableStyleFinder); null: none.
const Style* bookStyleAt(UiState& s, const Canvas& c, Style& out) {
    if (view.page < 0 || view.page >= int(view.pages.size())) return nullptr;
    const std::vector<Text> lines = s.font.split(view.pages[size_t(view.page)], 114);
    const float top = float(kBookTop + 30), left = float(bookLeft(c) + 36);
    const int i = int(std::floor((s.mouseY - top) / 9.0f));
    if (s.mouseY < top || i < 0 || i >= std::min(128 / 9, int(lines.size()))) return nullptr;
    const float x = s.mouseX - left;
    float pen = 0;
    bool found = false;
    s.font.iterate(lines[size_t(i)], [&](int, const Style& st, u32 cp) {
        const float adv = s.font.glyph(cp, st).advanceFor(st.isBold());
        if (x >= pen && x < pen + adv) {
            out = st;
            found = true;
            return false;
        }
        pen += adv;
        return true;
    });
    return found ? &out : nullptr;
}

// BookViewScreen.handleClickEvent / Screen.defaultHandleGameClickEvent.
bool bookClick(UiState& s, const TextEvents& ev) {
    if (ev.click.empty()) return false;
    LOG("ui: book click %s \"%s\"", ev.click.c_str(), ev.clickValue.c_str());
    if (ev.click == "change_page") {  // forcePage(page - 1)
        const int page = std::atoi(ev.clickValue.c_str()) - 1;
        const int clamped = std::clamp(page, 0, std::max(int(view.pages.size()) - 1, 0));
        if (clamped != view.page) view.page = clamped;
    } else if (ev.click == "run_command") {  // closeContainerOnServer; sendUnattendedCommand with no screen after
        std::string cmd = ev.clickValue;
        if (!cmd.empty() && cmd[0] == '/') cmd.erase(0, 1);  // Commands.trimOptionalPrefix
        s.closeScreen();
        if (s.online()) {
            net::Writer w;
            w.str(cmd);
            s.client->sendPlay("chat_command", w);
        }
    } else if (ev.click == "copy_to_clipboard") {
        s.window->setClipboard(ev.clickValue);
    } else if (ev.click == "open_url") {  // ConfirmLinkScreen, back to the book
        const std::string url = ev.clickValue;
        if (!url.starts_with("http://") && !url.starts_with("https://")) return true;
        UiState* sp = &s;
        const std::vector<Text> pages = view.pages;
        const int page = view.page;
        showConfirm(s, tr(s, "chat.link.confirm"), Text(url), s.res.translate("chat.link.open"), [sp, url, pages, page] {
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            view.pages = pages, view.page = page;
            sp->openScreen(Screen::BookView);
        }, Screen::BookView);
    }  // suggest_command: insertText into the screen (a written book has no text field)
    return true;
}

void viewBuild(UiState& s) {
    s.buttons.clear();
    sbButton(s, float((s.guiW() - 200) / 2), float(kBookTop + 192 + 2), 200, tr(s, "gui.done"), [&s] { s.closeScreen(); });
}

void viewDraw(UiState& s, Canvas& c) {
    sbBackground(s, c);
    drawBookBackground(s, c);
    drawBookPage(s, c, view.pages, view.page);
    if (view.page < int(view.pages.size()) - 1) drawPageButton(s, c, true);
    if (view.page > 0) drawPageButton(s, c, false);
    s.drawButtons(c);
    // HoveredTextEffects.TOOLTIP_AND_CURSOR: a hovered show_text event as a tooltip.
    Style st;
    if (bookStyleAt(s, c, st) && st.events && st.events->hover == "show_text")
        s.drawTooltip(c, s.font.split(st.events->hoverText, std::max(s.guiW() / 2, 200)), s.mouseX, s.mouseY);
}

void viewInput(UiState& s, const InputEvent& e) {
    using E = InputEvent;
    if (e.type == E::Key && e.down) {
        if (e.code == VK_ESCAPE) return s.closeScreen();
        if (e.code == VK_PRIOR && view.page > 0) --view.page, pageSound();
        if (e.code == VK_NEXT && view.page < int(view.pages.size()) - 1) ++view.page, pageSound();
        return;
    }
    if (e.type == E::Button && e.down && e.code == 0 && s.canvas) {
        if (view.page < int(view.pages.size()) - 1 && overPageButton(s, *s.canvas, true)) return void(++view.page), pageSound();
        if (view.page > 0 && overPageButton(s, *s.canvas, false)) return void(--view.page), pageSound();
        Style st;
        if (bookStyleAt(s, *s.canvas, st) && st.events && bookClick(s, *st.events)) return;
    }
    s.buttonsInput(e);
}

const ItemStack& handItem(UiState& s, int hand) { return s.inventory.slots[size_t(hand == 0 ? 36 + s.game->selectedSlot() : 45)]; }

void openBookView(UiState& s, int hand) {
    const BookContent b = readBook(s, handItem(s, hand));
    if (!b.valid) return;
    view = BookView{};
    view.pages = b.pages;
    s.openScreen(Screen::BookView);
}

// --- lectern (LecternScreen) ---------------------------------------------------------------------------------------

void lecternLayout(const Menu&, Layout& l) {
    l.background.clear();
    l.playerStart = -1;
    l.titleY = -1000, l.invLabelY = -1000;
    l.width = 192, l.height = 192;
}

bool lecternMayBuild(const UiState& s) { return s.gameMode() != 2 && s.gameMode() != 3; }

void lecternButtons(UiState& s) {
    s.buttons.clear();
    const int W = s.guiW(), y = kBookTop + 192 + 2;
    if (lecternMayBuild(s)) {
        sbButton(s, float(W / 2 - 98 - 2), float(y), 98, tr(s, "gui.done"), [&s] { s.closeScreen(); });
        sbButton(s, float(W / 2 + 2), float(y), 98, tr(s, "lectern.take_book"), [&s] { sendContainerButton(s, 3); });
    } else {
        sbButton(s, float((W - 200) / 2), float(y), 200, tr(s, "gui.done"), [&s] { s.closeScreen(); });
    }
}

int lecternPage(const Menu& m, int count) { return std::clamp(m.data[0], 0, std::max(0, count - 1)); }

void lecternDraw(UiState& s, Canvas& c, Menu& m, const Layout&, float, float) {
    lecternButtons(s);  // positions follow the window size
    drawBookBackground(s, c);
    const BookContent b = m.slots.empty() ? BookContent{} : readBook(s, m.slots[0]);
    const int count = int(b.pages.size()), page = lecternPage(m, count);
    drawBookPage(s, c, b.pages, page);
    if (page < count - 1) drawPageButton(s, c, true);
    if (page > 0) drawPageButton(s, c, false);
    s.drawButtons(c);
}

bool lecternInput(UiState& s, const InputEvent& e, Menu& m, const Layout&, float, float) {
    using E = InputEvent;
    if (e.type == E::Key && e.down) {
        if (e.code == VK_ESCAPE) return false;  // the container closes
        if (e.code == VK_PRIOR) sendContainerButton(s, 1);
        if (e.code == VK_NEXT) sendContainerButton(s, 2);
        return true;  // no slots: E and number keys do nothing
    }
    if (e.type != E::Button) return true;
    if (e.down && e.code == 0 && s.canvas) {
        const BookContent b = m.slots.empty() ? BookContent{} : readBook(s, m.slots[0]);
        const int count = int(b.pages.size()), page = lecternPage(m, count);
        if (page < count - 1 && overPageButton(s, *s.canvas, true)) {
            sendContainerButton(s, 2);
            return true;
        }
        if (page > 0 && overPageButton(s, *s.canvas, false)) {
            sendContainerButton(s, 1);
            return true;
        }
    }
    s.buttonsInput(e);
    return true;
}

// --- registration --------------------------------------------------------------------------------------------------

void registerHandlers(UiState& s, net::Client& client) {
    UiState* sp = &s;
    client.onPlay("open_sign_editor", [sp](net::Reader& r) {
        int x, y, z;
        r.blockPos(x, y, z);
        const int slot = r.varInt();
        if (!r.ok) return;
        sp->post([sp, x, y, z, slot] { openSign(*sp, x, y, z, slot); });
    });
    client.onPlay("open_book", [sp](net::Reader& r) {
        const int hand = r.varInt();
        if (!r.ok) return;
        sp->post([sp, hand] { openBookView(*sp, hand); });
    });
}

const UiModuleRegistrar kModule({.name = "signs_books", .registerHandlers = registerHandlers, .tick = signTick});
const ScreenRegistrar kSign({.screen = Screen::Sign, .build = signBuild, .draw = signDraw, .input = signInput, .closed = signClosed});
const ScreenRegistrar kEdit({.screen = Screen::BookEdit, .build = editBuild, .draw = editDraw, .input = editInput});
const ScreenRegistrar kView({.screen = Screen::BookView, .build = viewBuild, .draw = viewDraw, .input = viewInput});
const StationRegistrar kLectern({"lectern"}, {.layout = lecternLayout, .drawFg = lecternDraw, .input = lecternInput});
}  // namespace

bool openWritableBook(UiState& s, int hand) {
    const ItemStack& st = handItem(s, hand);
    if (st.empty() || s.items.name(st.item) != "minecraft:writable_book") return false;
    const BookContent b = readBook(s, st);
    edit = BookEdit{};
    edit.hand = hand;
    edit.pages = b.raw;
    if (edit.pages.empty()) edit.pages.push_back("");
    edit.title.maxLength = 15;
    editShowPage();
    edit.field.blinkStart = s.clock;
    s.openScreen(Screen::BookEdit);
    return true;
}

}  // namespace ui
