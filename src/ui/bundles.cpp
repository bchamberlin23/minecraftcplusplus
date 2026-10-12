// Bundles: BundleContents (weights as fractions, insert / remove), BundleItem's click overrides, the fullness bar,
// ClientBundleTooltip (the contents grid, the selected item, the fullness bar) and BundleMouseActions (the wheel selects
// an item; ServerboundSelectBundleItemPacket).
#include "audio/audio.h"
#include "ui/item_ui.h"

#include <numeric>
#include <random>

namespace ui {

namespace {
struct Fraction {
    i64 n = 0, d = 1;
    Fraction() = default;
    Fraction(i64 num, i64 den) : n(num), d(den) {
        if (d < 0) n = -n, d = -d;
        const i64 g = std::gcd(n < 0 ? -n : n, d);
        if (g > 1) n /= g, d /= g;
    }
    Fraction operator+(const Fraction& o) const { return Fraction(n * o.d + o.n * d, d * o.d); }
    Fraction operator-(const Fraction& o) const { return Fraction(n * o.d - o.n * d, d * o.d); }
    Fraction operator*(i64 k) const { return Fraction(n * k, d); }
    bool operator<(const Fraction& o) const { return n * o.d < o.n * d; }
    bool operator>=(const Fraction& o) const { return !(*this < o); }
    i64 mulTrunc(i64 k) const { return n * k / d; }  // Mth.mulAndTruncate
};

std::string shortName(const std::string& n) { return n.substr(n.find(':') + 1); }

void writeTemplate(net::Writer& w, const ItemStack& s) {  // ItemStackTemplate.STREAM_CODEC
    w.varInt(s.item);
    w.varInt(s.count);
    w.varInt(i32(s.added.size()));
    w.varInt(i32(s.removed.size()));
    for (const auto& c : s.added) {
        w.varInt(c.type);
        w.bytes(c.data.data(), c.data.size());
    }
    for (int t : s.removed) w.varInt(t);
}

Fraction weightOf(const Items& items, const ItemStack& s, int depth = 0);
Fraction contentWeight(const Items& items, const std::vector<ItemStack>& list, int depth) {
    Fraction w;
    for (const ItemStack& st : list) w = w + weightOf(items, st, depth + 1) * st.count;
    return w;
}
// BundleContents.getWeight: a nested bundle weighs its contents plus 1/16, an occupied beehive 1, else 1 / max stack.
Fraction weightOf(const Items& items, const ItemStack& s, int depth) {
    BundleContents nested;
    if (depth < 8 && BundleContents::read(items, s, nested)) return Fraction(nested.weightNum, nested.weightDen) + Fraction(1, 16);
    if (Items::ComponentValue v = items.component(s, "bees"); v.patched) {
        net::Reader r(reinterpret_cast<const u8*>(v.patched->data()), v.patched->size());
        if (r.varInt() > 0) return Fraction(1, 1);
    }
    return Fraction(1, std::max(1, items.maxStack(s)));
}

bool canBeInBundle(const Items& items, const ItemStack& s) {  // Item.canFitInsideContainerItems: not shulker boxes
    return !s.empty() && !shortName(items.name(s.item)).ends_with("shulker_box");
}

void playerSound(UiState& s, const char* event, float volume, float pitch) {
    audio::AudioSystem* a = audio::instance();
    if (!a || !s.game) return;
    const Player& p = s.game->player();
    a->play(event, audio::Category::Player, p.pos[0], p.pos[1], p.pos[2], volume, pitch);
}
float randomPitch() {  // 0.8 + random.nextFloat() * 0.4
    static std::mt19937 rng{std::random_device{}()};
    return 0.8f + std::uniform_real_distribution<float>(0.0f, 0.4f)(rng);
}

// BundleMouseActions state: the slot whose bundle has a selected item.
struct Selection {
    int container = -1, slot = -1;
    int lastHovered = -1;
    double accumulated = 0;  // ScrollWheelHandler
} sel;

void sendSelected(UiState& s, Menu& m, int slot, int index) {  // toggleSelectedBundleItem
    ItemStack& st = m.slots[size_t(slot)];
    BundleContents c;
    if (!BundleContents::read(s.items, st, c) || index >= c.numberOfItemsToShow()) return;
    st.bundleSelected = st.bundleSelected != index && index >= 0 && index < int(c.stacks.size()) ? index : -1;
    if (s.online()) {
        net::Writer w;
        w.varInt(slot);
        w.varInt(index);
        s.client->sendPlay("bundle_item_selected", w);
    }
    sel.container = m.containerId, sel.slot = st.bundleSelected >= 0 ? slot : -1;
}
}  // namespace

bool BundleContents::read(const Items& items, const ItemStack& bundle, BundleContents& out) {
    Items::ComponentValue v = items.component(bundle, "bundle_contents");
    if (!v.present()) return false;
    out = BundleContents{};
    if (v.patched) {
        net::Reader r(reinterpret_cast<const u8*>(v.patched->data()), v.patched->size());
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {
            ItemStack st;
            if (!items.readTemplate(r, st)) return false;
            out.stacks.push_back(std::move(st));
        }
    }
    const Fraction w = contentWeight(items, out.stacks, 0);
    out.weightNum = w.n, out.weightDen = w.d;
    out.selected = bundle.bundleSelected >= 0 && bundle.bundleSelected < int(out.stacks.size()) ? bundle.bundleSelected : -1;
    return true;
}

int BundleContents::maxAmountToAdd(const Items& items, const ItemStack& other) const {
    const Fraction remaining = Fraction(1, 1) - Fraction(weightNum, weightDen), w = weightOf(items, other);
    return int(std::max<i64>(0, (remaining.n * w.d) / (remaining.d * w.n)));
}

int BundleContents::tryInsert(const Items& items, ItemStack& other) {
    if (!canBeInBundle(items, other)) return 0;
    const int amount = std::min(other.count, maxAmountToAdd(items, other));
    if (amount <= 0) return 0;
    const Fraction w = Fraction(weightNum, weightDen) + weightOf(items, other) * amount;
    weightNum = w.n, weightDen = w.d;
    int index = -1;  // findStackIndex: stackable and the same item and components
    if (items.maxStack(other) > 1)
        for (size_t i = 0; i < this->stacks.size(); ++i)
            if (this->stacks[i].sameItemSameComponents(other)) index = int(i);
    ItemStack moved = other.withCount(amount);
    if (index >= 0) {
        moved = this->stacks[size_t(index)];
        moved.count += amount;
        this->stacks.erase(this->stacks.begin() + index);
    }
    moved.bundleSelected = -1;
    this->stacks.insert(this->stacks.begin(), moved);
    other = other.withCount(other.count - amount);
    return amount;
}

bool BundleContents::removeOne(const Items& items, ItemStack& out) {
    if (stacks.empty()) return false;
    const int index = selected < 0 || selected >= int(this->stacks.size()) ? 0 : selected;
    out = this->stacks[size_t(index)];
    this->stacks.erase(this->stacks.begin() + index);
    const Fraction w = Fraction(weightNum, weightDen) - weightOf(items, out) * out.count;
    weightNum = w.n, weightDen = w.d;
    selected = -1;
    return true;
}

void BundleContents::toggleSelected(int index) {
    selected = selected != index && index >= 0 && index < int(stacks.size()) ? index : -1;
}

void BundleContents::write(const Items& items, ItemStack& bundle) const {
    net::Writer w;
    w.varInt(i32(this->stacks.size()));
    for (const ItemStack& st : this->stacks) writeTemplate(w, st);
    const int type = items.componentType("bundle_contents");
    std::string data(w.buf.begin(), w.buf.end());
    bool set = false;
    for (auto& c : bundle.added)
        if (c.type == type) c.data = data, set = true;
    if (!set) bundle.added.push_back({type, data});
    bundle.removed.erase(std::remove(bundle.removed.begin(), bundle.removed.end(), type), bundle.removed.end());
    bundle.bundleSelected = selected;
}

int BundleContents::numberOfItemsToShow() const {
    const int size = int(stacks.size());
    const int available = size > 12 ? 11 : 12;
    const int onRow = size % 4;
    return std::min(size, available - (onRow == 0 ? 0 : 4 - onRow));
}

bool isBundleItem(const Items& items, const ItemStack& st) {
    if (st.empty()) return false;
    const std::string n = shortName(items.name(st.item));
    return n == "bundle" || n.ends_with("_bundle");
}

bool bundleClickOverride(UiState& s, ItemStack& slotStack, ItemStack& carried, int button, const SlotOps& ops) {
    // carried.overrideStackedOnOther(slot)
    BundleContents c;
    if (isBundleItem(s.items, carried) && BundleContents::read(s.items, carried, c)) {
        if (button == 0 && !slotStack.empty()) {
            const int max = c.maxAmountToAdd(s.items, slotStack);
            int moved = 0;
            if (canBeInBundle(s.items, slotStack)) {  // tryTransfer: safeTake what fits, then insert it
                ItemStack taken = ops.safeTake(slotStack.count, max);
                moved = c.tryInsert(s.items, taken);
            }
            moved > 0 ? playerSound(s, "minecraft:item.bundle.insert", 0.8f, randomPitch())
                      : playerSound(s, "minecraft:item.bundle.insert_fail", 1.0f, 1.0f);
            c.write(s.items, carried);
            return true;
        }
        if (button == 1 && slotStack.empty()) {
            ItemStack removed;
            if (c.removeOne(s.items, removed)) {
                ItemStack rest = ops.safeInsert(removed);
                if (rest.count > 0) c.tryInsert(s.items, rest);
                else playerSound(s, "minecraft:item.bundle.remove_one", 0.8f, randomPitch());
            }
            c.write(s.items, carried);
            return true;
        }
    }
    // clicked.overrideOtherStackedOnMe(carried, slot)
    if (!isBundleItem(s.items, slotStack) || !BundleContents::read(s.items, slotStack, c)) return false;
    if (button == 0 && carried.empty()) {
        c.toggleSelected(-1);
        slotStack.bundleSelected = c.selected;
        return false;
    }
    if (button == 0 && !carried.empty()) {
        if (ops.allowModification && c.tryInsert(s.items, carried) > 0) playerSound(s, "minecraft:item.bundle.insert", 0.8f, randomPitch());
        else playerSound(s, "minecraft:item.bundle.insert_fail", 1.0f, 1.0f);
        c.write(s.items, slotStack);
        return true;
    }
    if (button == 1 && carried.empty()) {
        if (ops.allowModification) {
            ItemStack removed;
            if (c.removeOne(s.items, removed)) {
                playerSound(s, "minecraft:item.bundle.remove_one", 0.8f, randomPitch());
                carried = removed;
            }
        }
        c.write(s.items, slotStack);
        return true;
    }
    c.toggleSelected(-1);
    slotStack.bundleSelected = c.selected;
    return false;
}

bool bundleScroll(UiState& s, Menu& m, int slot, int wheel) {
    if (slot < 0 || size_t(slot) >= m.slots.size()) return false;
    ItemStack& st = m.slots[size_t(slot)];
    if (st.empty() || !s.items.itemHasTag(st.item, "minecraft:bundles")) return false;  // matches: #bundles
    BundleContents c;
    if (!BundleContents::read(s.items, st, c)) return false;
    const int shown = c.numberOfItemsToShow();
    if (shown == 0) return false;
    // ScrollWheelHandler.onMouseScroll (whole notches) and getNextScrollWheelSelection.
    if (sel.accumulated != 0 && (wheel > 0) != (sel.accumulated > 0)) sel.accumulated = 0;
    sel.accumulated += wheel;
    const int steps = int(sel.accumulated);
    sel.accumulated -= steps;
    if (steps != 0) {
        const int current = st.bundleSelected;
        int next = std::max(-1, current - (steps > 0 ? 1 : -1));
        while (next < 0) next += shown;
        while (next >= shown) next -= shown;
        if (next != current) sendSelected(s, m, slot, next);
    }
    return true;
}

void bundleHovered(UiState& s, Menu& m, int slot) {
    if (sel.lastHovered != slot && sel.lastHovered >= 0 && size_t(sel.lastHovered) < m.slots.size()) {
        ItemStack& last = m.slots[size_t(sel.lastHovered)];  // onStopHovering: unselect
        if (!last.empty() && s.items.itemHasTag(last.item, "minecraft:bundles") && last.bundleSelected >= 0)
            sendSelected(s, m, sel.lastHovered, -1);
    }
    sel.lastHovered = slot;
}

void bundleSlotClicked(UiState& s, Menu& m, int slot, int input) {
    if (input != 1 && input != 2) return;  // QUICK_MOVE, SWAP
    if (slot < 0 || size_t(slot) >= m.slots.size()) return;
    const ItemStack& st = m.slots[size_t(slot)];
    if (!st.empty() && s.items.itemHasTag(st.item, "minecraft:bundles") && st.bundleSelected >= 0) sendSelected(s, m, slot, -1);
}

bool bundleBar(const Items& items, const ItemStack& st, int& width, u32& color) {
    BundleContents c;
    if (!isBundleItem(items, st) || !BundleContents::read(items, st, c) || c.weightNum <= 0) return false;
    const Fraction w(c.weightNum, c.weightDen);
    width = int(std::min<i64>(1 + w.mulTrunc(12), 13));
    color = w >= Fraction(1, 1) ? 0xFFFF5454 : 0xFF7087FF;  // ARGB.colorFromFloat(1, 1, 0.33, 0.33) / (1, 0.44, 0.53, 1)
    return true;
}

bool bundleTooltipImage(UiState& s, const ItemStack& st, TooltipImage& out) {
    const int type = s.items.componentType("bundle_contents");
    if (std::find(st.hiddenTooltip.begin(), st.hiddenTooltip.end(), type) != st.hiddenTooltip.end()) return false;
    BundleContents c;
    if (!isBundleItem(s.items, st) || !BundleContents::read(s.items, st, c)) return false;
    const Text emptyText = translatable("item.minecraft.bundle.empty.description", {}, s.lang());
    const int descLines = int(s.font.split(emptyText, 96).size());
    const int gridY = (std::min(12, int(c.stacks.size())) + 3) / 4;
    out.width = 96;
    out.height = c.stacks.empty() ? descLines * 9 + 13 + 8 : gridY * 24 + 13 + 8;
    UiState* sp = &s;
    out.draw = [sp, c, emptyText, gridY](Canvas& cv, float x, float y, int w) {
        UiState& s = *sp;
        const float left = x + float((w - 96) / 2);
        const Fraction weight(c.weightNum, c.weightDen);
        auto progressbar = [&](float bx, float by) {
            const int fill = int(std::clamp<i64>(weight.mulTrunc(94), 0, 94));
            blitSprite(cv, s.sprites.sprite(weight >= Fraction(1, 1) ? "container/bundle/bundle_progressbar_full"
                                                                     : "container/bundle/bundle_progressbar_fill"),
                       bx + 1, by, float(fill), 13);
            blitSprite(cv, s.sprites.sprite("container/bundle/bundle_progressbar_border"), bx, by, 96, 13);
            const char* key = weight.n == 0 ? "item.minecraft.bundle.empty" : weight >= Fraction(1, 1) ? "item.minecraft.bundle.full" : nullptr;
            if (key) s.drawCentered(cv, translatable(key, {}, s.lang()), bx + 48, by + 3, 0xFFFFFFFF);
        };
        if (c.stacks.empty()) {
            float ly = y;
            const std::vector<Text> lines = s.font.split(emptyText, 96);
            for (const Text& l : lines) s.drawText(cv, l, left, ly, 0xFFAAAAAA, true), ly += 9;
            progressbar(left, y + float(lines.size() * 9) + 4);
            return;
        }
        const bool overflowing = c.stacks.size() > 12;
        const int shownCount = std::min(int(c.stacks.size()), c.numberOfItemsToShow());
        const float xStart = left + 96, yStart = y + float(gridY * 24);
        int slotNumber = 1;
        for (int row = 1; row <= gridY; ++row)
            for (int col = 1; col <= 4; ++col) {
                const float dx = xStart - float(col * 24), dy = yStart - float(row * 24);
                if (overflowing && col * row == 1) {
                    int hidden = 0;
                    for (size_t i = size_t(shownCount); i < c.stacks.size(); ++i) hidden += c.stacks[i].count;
                    s.drawCentered(cv, Text("+" + std::to_string(hidden)), dx + 12, dy + 10, 0xFFFFFFFF);
                } else if (shownCount >= slotNumber) {
                    const int index = shownCount - slotNumber;
                    const bool highlight = index == c.selected;
                    blitSprite(cv, s.sprites.sprite(highlight ? "container/bundle/slot_highlight_back" : "container/bundle/slot_background"), dx, dy, 24, 24);
                    s.drawItem(cv, c.stacks[size_t(index)], dx + 4, dy + 4);
                    if (highlight) blitSprite(cv, s.sprites.sprite("container/bundle/slot_highlight_front"), dx, dy, 24, 24);
                    ++slotNumber;
                }
            }
        if (c.selected >= 0) {  // the selected item's name in a small tooltip above
            const Text name = s.items.hoverName(c.stacks[size_t(c.selected)]);
            const float cx = x + float(w / 2 - 12);
            s.drawTooltip(cv, {name}, cx - float(s.font.width(name) / 2), y - 15);
        }
        progressbar(left, y + float(gridY * 24) + 4);
    };
    return true;
}

}  // namespace ui
