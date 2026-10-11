#pragma once
// Item icons the way the inventory draws them: the item's model definition (assets/minecraft/items) picks
// models; flat "generated" models become 2D sprites, models with elements are drawn in 3D with their GUI
// display transform and the game's item lighting. Icons are rasterized on the CPU at the current GUI
// scale (16 * scale pixels) into the icon atlas on first use.
#include "ui/canvas.h"
#include "ui/items.h"
#include "ui/resources.h"

#include <memory>
#include <unordered_map>

namespace ent {
class ItemModels;
}

namespace ui {

struct ItemContext;

class ItemIcons {
public:
    ItemIcons();
    ~ItemIcons();
    void init(const Resources& res, const Items& items, Atlas& atlas);
    void setScale(int guiScale);  // re-renders lazily at the new size
    // The 3D item models (special model renderers: banners, shields, heads ... drawn from the same entity model
    // geometry); until set, special models are simple boxes.
    void setItemModels(ent::ItemModels* models);
    // Atlas UVs of the icon (u0 v0 u1 v1); false when the item draws nothing. The item model properties read the
    // context (default: the GUI display context with the local player as the owner, itemWorld().player).
    bool icon(const ItemStack& s, float uv[4], const ItemContext* context = nullptr);
    // Icons with animated textures (prismarine, magma, sea lantern...) are redrawn when a frame changes.
    void tick(u64 tick);
    int iconSize() const { return size_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int size_ = 32;
};

}  // namespace ui
