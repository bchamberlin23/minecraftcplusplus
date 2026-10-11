#pragma once
// Entity-side visuals on top of the entity renderer:
//  - first-person hands (FirstPersonHandsAndItemsRenderer): the player's arm with its skin, held items with the
//    item models' first-person display transforms, attack / break swings, equip (slot switch) animation, hand
//    sway and view bobbing; drawn in the HUD stage with its own depth range so it never clips into terrain;
//  - third-person camera (F5, Game::cameraMode): the local player drawn as a player entity;
//  - held items of mobs and players (ItemInHandLayer), worn heads / blocks (CustomHeadLayer), dropped items as 3D
//    item models (ItemEntityRenderer); armour and elytra are entity renderer layers (entity_render.cpp);
//  - item frames (with the framed item and its rotation), glow item frames and paintings;
//  - name tags (players, named mobs): see-through and depth-tested parts like the game, dimmed while sneaking;
//  - enchantment glint on items;
//  - particles (fx/particles.h).
//
// Item models, the item / particle texture atlas and the font are loaded from the installed game.
#include "core/common.h"
#include "entity/item_models.h"
#include "fx/particles.h"
#include "render/renderer.h"

#include <functional>
#include <memory>
#include <unordered_map>

class Game;
class GameReports;
namespace net {
class Client;
}
namespace ui {
class Ui;
class Resources;
class Items;
class Font;
struct ItemStack;
}  // namespace ui

namespace ent {

class EntityWorld;
class EntityRenderer;
struct Entity;

class Visuals {
public:
    Visuals(EntityWorld& world, EntityRenderer& entityRenderer);
    ~Visuals();
    // Game resources (item models, font, particle definitions); before any client attaches.
    // componentReports: the game's item component reports (reports/minecraft/components/item).
    bool load(const fs::path& jar, const fs::path& minecraftDir, const GameReports& reports, const fs::path& componentReports);
    void attach(net::Client& client);  // particle / weather packets; before Client::start
    bool addPasses(Renderer& renderer);  // World, AfterTranslucent and Hud stages (+ an update in the Sky stage)
    // The local player: inventory from the UI, input hooks on the game. After the game, the UI and audio exist.
    void setPlayer(Game& game, ui::Ui& ui, const std::string& name, std::function<int()> gameMode);

    struct Stats {
        double updateMs = 0, worldMs = 0, afterMs = 0, hudMs = 0;
        u32 items = 0, tags = 0, particles = 0, instances = 0;
    };
    const Stats& stats() const { return stats_; }
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    Stats stats_;
};

}  // namespace ent
