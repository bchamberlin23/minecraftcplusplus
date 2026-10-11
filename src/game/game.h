#pragma once
#include "core/common.h"
#include "game/player.h"
#include "render/renderer.h"
#include "world/lighting.h"

#include <functional>

class World;
class BlockRegistry;
class Terrain;
class Window;
struct PlayerStart;

// Single-player creative gameplay on top of the world: movement, targeting, breaking,
// placing and picking blocks, with relighting and immediate remeshing of edits.
class Game {
public:
    Game(World& world, BlockRegistry& registry, Terrain& terrain, Renderer& renderer, const PlayerStart& start);

    // Reads input, advances the simulation in fixed 20 Hz ticks and builds the HUD.
    void frame(Window& window, double dt);
    CameraState camera() const;
    std::string status() const;
    Player& player() { return player_; }

    // Used by input handling and by the scripted self-test.
    void retarget();
    const RayHit& target() const { return target_; }
    bool useTarget(bool place);  // break or place at the current target; true if the world changed
    void setHeld(u16 id) { hotbar_[selected_] = id; }
    int selectedSlot() const { return selected_; }
    u16 hotbarBlock(int slot) const { return hotbar_[slot]; }

    // Multiplayer hook, called for every break or place before it is applied locally.
    // (x, y, z) and face: the targeted block; hit: the targeted point; at: the cell that changes.
    struct EditAction {
        bool place = false;
        int x = 0, y = 0, z = 0, face = 0;
        double hit[3] = {};
        int at[3] = {};
        u16 block = 0;  // state placed (0 when breaking)
    };
    std::function<void(const EditAction&)> onEdit;
    // Entity hook (multiplayer), called on clicks before blocks: returns true when an entity closer than
    // blockDistance (< 0: no block targeted) is under the crosshair; it then takes the click.
    std::function<bool(bool attack, bool use, double blockDistance)> onEntityClick;
    // Sound hooks (audio): a block the player broke (its old state) or placed (the new state), once
    // applied; and the end of every 20 Hz player tick.
    std::function<void(bool place, int x, int y, int z, u16 state)> onBlockChanged;
    std::function<void()> onPlayerTick;
    // UI hooks: while inputSuspended (a screen is open) the player gets no input; with drawHud false the UI draws
    // the crosshair and hotbar; onPickBlock replaces the local middle-click pick.
    bool inputSuspended = false, drawHud = true, handleEscape = true;
    float sensitivity = 0.0025f;  // mouse look, radians per count
    std::function<void(int x, int y, int z)> onPickBlock;
    // Right click on a block that reacts to use (containers, doors...): true sends the use to the server as is
    // instead of placing the held block.
    std::function<bool(int x, int y, int z)> onInteract;
    void setHotbarBlock(int slot, u16 id) { hotbar_[slot] = id; }
    void setSelectedSlot(int slot) { selected_ = slot; }
    void simulate(const PlayerInput& input, int ticks);
    void buildOverlay();

private:
    PlayerInput readInput(Window& window);
    void handleClicks(Window& window, double dt);
    void edit(int x, int y, int z, u16 id);
    u16 orientForPlacement(u16 id, const RayHit& hit, const int target[3]);

    World& world_;
    BlockRegistry& registry_;
    Terrain& terrain_;
    Renderer& renderer_;
    Physics physics_;
    Lighting lighting_;
    Player player_;
    double tickAccumulator_ = 0, alpha_ = 0;
    RayHit target_;
    u16 hotbar_[9] = {};
    int selected_ = 0;
    double repeatTimer_ = 0;
    double lastJumpPress_ = -1, clock_ = 0;
    bool jumpWasDown_ = false, escWasDown_ = false;
    bool keyWasDown_[9] = {};
};
