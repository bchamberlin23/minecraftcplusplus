#pragma once
// Client-side block placement prediction (BlockItem.place on the client, as MultiPlayerGameMode.useItemOn runs it):
// getStateForPlacement of the common block families, the cells setPlacedBy adds (door and tall plant tops, bed
// heads), canBeReplaced for stackable blocks, and the neighbours' updateShape (fence / pane connections, stair
// shapes, double chests). The server's block updates correct anything not predicted here.
#include "core/common.h"

#include <array>
#include <functional>
#include <vector>

class BlockRegistry;

namespace place {

// Faces as everywhere: 0 down, 1 up, 2 north, 3 south, 4 west, 5 east (Direction's 3D data values).
struct Context {
    BlockRegistry& reg;
    std::function<u16(int x, int y, int z)> block;
    std::function<bool(int x, int y, int z)> waterSource;  // a water source there (waterlogging)
    int clicked[3] = {};   // the block the ray hit
    int face = 1;          // the hit face
    double hit[3] = {};    // the hit point
    float yaw = 0, pitch = 0;  // the player's view (radians; yaw 0 = south, pitch > 0 = down)
    bool sneaking = false;     // isSecondaryUseActive
    int maxY = 319;            // the level's top block
};

// BlockPlaceContext.canBeReplaced for the held block: whether the state at `at` (the clicked block when
// replacingClicked) gives way to it (replaceable blocks, a slab's other half, stackables below their maximum).
bool canBeReplaced(const Context& c, u16 held, u16 state, bool replacingClicked, const int at[3]);

struct Result {
    u16 state = 0;                         // 0: cannot be placed
    std::vector<std::array<int, 4>> extra;  // x, y, z, state: cells setPlacedBy adds
};
// The state the held block takes at `pos` (replaceClicked: placing into the clicked cell).
Result placementState(const Context& c, u16 held, const int pos[3], bool replaceClicked);

// Block.updateShape of the six neighbours of a changed cell: the cells whose state changes (x, y, z, state).
void neighbourShapes(const Context& c, int x, int y, int z, std::vector<std::array<int, 4>>& out);

}  // namespace place
