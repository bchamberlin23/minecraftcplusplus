#pragma once
// Item UI shared by the container screens: bundles (bundles.cpp), the recipe book (recipe_book.cpp) and the item state
// the tooltips read (item_ui.cpp).
#include "ui/ui_state.h"

#include <unordered_map>

namespace ui {

// --- item_ui.cpp ---
int permissionLevel();  // the local player's permission level (entity events 24..28)
// Server recipe data (ClientboundUpdateRecipesPacket): the property set "minecraft:furnace_input" etc. holds the item?
// Unknown sets (no packet yet) accept nothing.
bool inPropertySet(std::string_view set, int item);
bool propertySetKnown(std::string_view set);
void setPropertySets(std::unordered_map<std::string, std::vector<int>> sets);  // main thread
// An item tooltip with its image (a bundle's contents) as the container screens draw it.
void drawItemTooltip(UiState& s, Canvas& c, const ItemStack& st, const std::vector<Text>& lines, float x, float y);

// --- recipe_book.cpp: the recipe book of the inventory, crafting table and furnaces ---
int recipeBookLeft(UiState& s, const Menu& m, int imageWidth);  // the menu's left edge (the open book moves it right)
bool recipeBookCovers(const Menu& m, float x, float y, float left, float top, int imageHeight);  // a click on the book
bool recipeBookHidesMenu(const Menu& m);  // the book open over a window too narrow (widthTooNarrow)
void recipeBookDrawBackground(UiState& s, Canvas& c, Menu& m, float left);  // the toggle button
void recipeBookDrawGhosts(UiState& s, Canvas& c, Menu& m, const Layout& l, float left, float top);  // after the slots
void recipeBookDraw(UiState& s, Canvas& c, Menu& m);                        // the book, over the menu
void recipeBookTooltip(UiState& s, Canvas& c, Menu& m, int hoveredSlot);    // after the menu's tooltip
bool recipeBookInput(UiState& s, const InputEvent& e, Menu& m, float left);  // true: consumed
void recipeBookSlotClicked(UiState& s, Menu& m, int slot);

// --- bundles.cpp ---
// BundleContents.Mutable: the contents of a bundle stack, edited and written back into its bundle_contents component.
struct BundleContents {
    std::vector<ItemStack> stacks;
    i64 weightNum = 0, weightDen = 1;  // Fraction
    int selected = -1;

    static bool read(const Items& items, const ItemStack& bundle, BundleContents& out);  // false: not a bundle
    int tryInsert(const Items& items, ItemStack& other);  // BundleContents.Mutable.tryInsert: the amount taken
    int maxAmountToAdd(const Items& items, const ItemStack& other) const;
    bool removeOne(const Items& items, ItemStack& out);
    void toggleSelected(int index);
    void write(const Items& items, ItemStack& bundle) const;
    int numberOfItemsToShow() const;
};
bool isBundleItem(const Items& items, const ItemStack& st);  // a BundleItem (bundle, <colour>_bundle)
// BundleItem.overrideStackedOnOther / overrideOtherStackedOnMe for a PICKUP click on a slot (button 0 / 1).
// mayPlace / safeInsert / maxTake answer for the clicked slot. true: handled (the generic click does nothing).
struct SlotOps {
    std::function<bool(const ItemStack&)> mayPlace;
    std::function<ItemStack(ItemStack stack)> safeInsert;     // Slot.safeInsert: the rest
    std::function<ItemStack(int amount, int max)> safeTake;    // Slot.safeTake
    bool allowModification = true;                            // Slot.allowModification
};
bool bundleClickOverride(UiState& s, ItemStack& slotStack, ItemStack& carried, int button, const SlotOps& ops);
// BundleMouseActions: the wheel over a bundle in a menu slot selects an item; leaving the slot / quick move / swap
// clears it. true: the wheel was used.
bool bundleScroll(UiState& s, Menu& m, int slot, int wheel);
void bundleHovered(UiState& s, Menu& m, int slot);  // the hovered slot this frame (onStopHovering)
void bundleSlotClicked(UiState& s, Menu& m, int slot, int input);
// The tooltip image (ClientBundleTooltip); false when the stack shows none.
bool bundleTooltipImage(UiState& s, const ItemStack& st, TooltipImage& out);
// BundleItem.isBarVisible / getBarWidth / getBarColor (ARGB).
bool bundleBar(const Items& items, const ItemStack& st, int& width, u32& color);

}  // namespace ui
