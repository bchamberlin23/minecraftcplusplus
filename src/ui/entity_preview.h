#pragma once
// InventoryScreen.extractEntityInInventoryFollowsMouse on the GUI canvas: an entity model (the game's baked model
// layers, entity/entity_assets.h) posed by its animation, projected orthographically like the game's
// picture-in-picture entity renderer, back faces culled and faces painter-sorted, lit like ENTITY_IN_UI.
#include "entity/entity_anim.h"
#include "ui/items.h"
#include "ui/modules.h"

#include <string>
#include <vector>

namespace ui {

struct PreviewLayer {
    int model = -1;       // EntityAssets model index
    std::string texture;  // texture below textures/ ("entity/player/wide/steve")
    u32 color = 0xFFFFFFFF;
    bool wings = false;   // WingsLayer (elytra): its own resting pose, 2 px behind the body
};

struct PreviewEntity {
    std::vector<PreviewLayer> layers;  // the base model first; same-model layers (markings, overlays) after it
    ent::Anim anim = ent::Anim::Head;
    ent::AnimState state;              // headYaw / pitch are set from the mouse
    float height = 1.8f;               // bounding box height (blocks)
    float modelScale = 1.0f;           // the renderer's own scale (players 0.9375)
    ItemStack held[2];                 // ItemInHandLayer: right arm, left arm
    ItemStack headItem;                // CustomHeadLayer: an item model on the head (HEAD display)
    std::vector<std::string> hiddenParts;  // parts not drawn (chests of unchested animals, reins when not ridden)
    // GuiGraphicsExtractor.entity with a fixed view instead of following the mouse: translation y, rotation
    // rotationXYZ(viewRotX, 0, viewRotZ) and the body's rotation (degrees).
    bool fixedView = false;
    float viewY = 0, viewRotX = 0, viewRotZ = 0, bodyRot = 0;
};

// HumanoidArmorLayer pieces of a stack worn on `model` (equipment asset layers, then its trim).
void addArmorLayers(UiState& s, PreviewEntity& out, int model, const ItemStack& stack, const std::string& asset, const char* layerType);
// SmithingScreen's armour stand preview: the result in its equipment slot (else in the left hand).
bool previewSmithingArmorStand(UiState& s, const ItemStack& result, PreviewEntity& out);

// The local player (skin from the player list, default skin otherwise). false: no entity models.
bool previewLocalPlayer(UiState& s, PreviewEntity& out);
// An entity of the world by network id (horses, llamas, camels...). false: unknown / no model.
bool previewEntity(UiState& s, int entityId, PreviewEntity& out);

// x0..y1: the GUI box (clipped to it); size: GUI pixels per block; offsetY: blocks below the box centre.
void drawEntityFollowsMouse(UiState& s, Canvas& c, const PreviewEntity& e, int x0, int y0, int x1, int y1, float size,
                            float offsetY, float mouseX, float mouseY);

}  // namespace ui
