#pragma once
// The operator block editors (command block, command block minecart, jigsaw, structure, test, test instance).
#include "render/renderer.h"
#include "ui/text.h"
#include "ui/ui.h"

#include <string>
#include <vector>

namespace ui {

bool canUseGameMasterBlocks(Ui::State& s);  // creative with permission level 2
// Opens the editor of the block at a position when it is one of the operator blocks and the player may use it.
bool openBlockEditor(Ui::State& s, int x, int y, int z);
bool openMinecartCommandEditor(Ui::State& s, int entity, const std::string& command, const Text& lastOutput);
// BlockEntityWithBoundingBoxRenderer: the boxes of structure blocks (save / load) and test instance blocks near the
// camera, with the invisible blocks of a structure block showing air, for operators and spectators.
void operatorBoxes(Ui::State& s, std::vector<LineVertex>& lines, const CameraState& cam, float screenW, float screenH);

}  // namespace ui
