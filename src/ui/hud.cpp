// HUD (Hud.java), chat (ChatComponent / ChatScreen input), F3 debug overlay, item slots and tooltips.
#include "assets/assets.h"
#include "audio/audio.h"
#include "ui/debug_screen.h"
#include "ui/gameplay_hud.h"
#include "ui/mp_hud.h"
#include "render/renderer.h"
#include "ui/narrator.h"
#include "ui/options_list.h"
#include "ui/item_props.h"
#include "ui/item_ui.h"
#include "ui/locator_bar.h"
#include "ui/toasts.h"
#include "ui/ui_state.h"
#include "world/terrain.h"
#include "world/world.h"

#include <psapi.h>

#include <cmath>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace ui {

namespace {
constexpr u32 kWhite = 0xFFFFFFFF;

// java.util.Random (LegacyRandomSource) for the HUD jitter (Hud uses its own RandomSource seeded per tick).
struct Jitter {
    u64 seed;
    explicit Jitter(u64 s) : seed((s ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1)) {}
    int next(int bits) {
        seed = (seed * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
        return int(seed >> (48 - bits));
    }
    int nextInt(int bound) {  // BitRandomSource.nextInt: powers of two by the high bits, else rejection sampling
        if ((bound & -bound) == bound) return int((u64(next(31)) * u64(bound)) >> 31);
        int bits, val;
        do bits = next(31), val = bits % bound;
        while (i32(u32(bits) - u32(val) + u32(bound - 1)) < 0);  // Java int overflow
        return val;
    }
};

u32 hsvToRgb(float h, float s, float v) {  // Mth.hsvToRgb
    const int i = int(h * 6.0f) % 6;
    const float f = h * 6.0f - float(int(h * 6.0f)), p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    float r, g, b;
    switch (i) {
        case 0: r = v, g = t, b = p; break;
        case 1: r = q, g = v, b = p; break;
        case 2: r = p, g = v, b = t; break;
        case 3: r = p, g = q, b = v; break;
        case 4: r = t, g = p, b = v; break;
        default: r = v, g = p, b = q; break;
    }
    return argb(255, int(r * 255), int(g * 255), int(b * 255));
}

// GuiGraphics.textWithBackdrop: the Text Background Opacity behind HUD text when Text Background is Everywhere.
void backdrop(UiState& s, Canvas& c, float x, float y, int width, u32 textColor) {
    const u32 bg = s.textBackground(0.0f);
    if (!alphaOf(bg)) return;
    c.fill(x - 2, y - 2, x + float(width) + 2, y + 9 + 2, withAlpha(0, float(alphaOf(bg)) * float(alphaOf(textColor)) / (255.0f * 255.0f)));
}

}  // namespace

// MobEffectInstance.compareTo: not ambient first, then not infinite, then by duration and colour (long ambient
// effects: by ambience and colour only).
std::vector<const Ui::State::Effect*> Ui::State::sortedEffects(bool descending) const {
    std::vector<const Effect*> out;
    for (const auto& [id, e] : effects) out.push_back(&e);
    auto cmp = [](const Effect* a, const Effect* b) -> int {
        auto c3 = [](auto x, auto y) { return x < y ? -1 : x > y ? 1 : 0; };
        const i64 ca = effectColor(a->name), cb = effectColor(b->name);
        if ((a->duration <= 32147 || b->duration <= 32147) && !(a->ambient && b->ambient)) {
            if (int r = c3(a->ambient, b->ambient)) return r;
            if (int r = c3(a->duration == -1, b->duration == -1)) return r;
            if (int r = c3(a->duration, b->duration)) return r;
            return c3(ca, cb);
        }
        if (int r = c3(a->ambient, b->ambient)) return r;
        return c3(ca, cb);
    };
    std::stable_sort(out.begin(), out.end(), [&](const Effect* a, const Effect* b) { return descending ? cmp(a, b) > 0 : cmp(a, b) < 0; });
    return out;
}

void Ui::State::drawEffectsInInventory(Canvas& c, Screen self, int leftPos, int imageWidth, int topPos) {
    const int x0 = leftPos + imageWidth + 2, available = c.guiWidth() - x0;
    (void)self;
    if (effects.empty() || available < 32) return;  // canSeeEffects
    const int maxWidth = available >= 120 ? available - 7 : 32;
    const int yStep = effects.size() > 5 ? 132 / int(effects.size() - 1) : 33;
    int y0 = topPos;
    for (const Effect* e : sortedEffects(false)) {
        std::string name = res.translate("effect.minecraft." + e->name);
        if (e->amplifier >= 1 && e->amplifier <= 9) name += " " + res.translate("enchantment.level." + std::to_string(e->amplifier + 1));
        const Text effectText(name);
        const Text duration = e->duration == -1 ? Text(res.translate("effect.duration.infinite")) : Text(tickDuration(e->duration));
        const int textureWidth = std::min(maxWidth, std::max(32 + font.width(effectText) + 7, 32 + font.width(duration) + 7));
        blitSprite(c, sprites.sprite(e->ambient ? "container/inventory/effect_background_ambient" : "container/inventory/effect_background"),
                   float(x0), float(y0), float(textureWidth), 32);
        const int maxTextWidth = textureWidth - 32 - 7;
        bool compact = true;
        if (maxTextWidth > 0) {
            const bool clip = font.width(effectText) > maxTextWidth;  // ComponentRenderUtils.clipText: "..." at the end
            const Text shown = clip ? Text(font.headByWidth(name, maxTextWidth - font.width("...")) + "...") : effectText;
            font.draw(c, shown, float(x0 + 32), float(y0 + 7), 0xFFFFFFFF, true);
            font.draw(c, duration, float(x0 + 32), float(y0 + 16), 0xFF808080, true);
            compact = clip;
        }
        if (const Sprite* icon = sprites.texture("mob_effect/" + e->name)) blitTexture(c, icon, float(x0 + 7), float(y0 + 7), 0, 0, 18, 18, 18, 18);
        if (compact && mouseX >= x0 && mouseX <= x0 + textureWidth && mouseY >= y0 && mouseY <= y0 + yStep)
            drawTooltip(c, {effectText, duration}, mouseX, mouseY);
        y0 += yStep;
    }
}

// --- per tick -----------------------------------------------------------------------------------------------

void Ui::State::tickHud() {
    lastAutosaveIndicator = autosaveIndicator;  // Hud.tick(pause): the autosave indicator always, the rest unpaused
    autosaveIndicator += 0.2f * ((serverSaving ? 1.0f : 0.0f) - autosaveIndicator);
    if (screen == Screen::Pause && rendersNowPlayingToast()) tickNowPlayingNotes(*this);  // PauseScreen.tick
    tickChat();
    if (paused) {
        for (const UiModule& m : uiModules())
            if (m.tick) m.tick(*this);
        return;
    }
    ++guiTicks;
    for (int& p : popTime)  // ItemStack.inventoryTick
        if (p > 0) --p;
    for (auto& [id, e] : effects)
        if (e.duration > 0) --e.duration;
    if (actionbarTime > 0) --actionbarTime;
    if (titleTime > 0 && --titleTime <= 0) hasTitle = hasSubtitle = false;
    // Selected item name (Hud.tick).
    const int sel = game->selectedSlot();
    const ItemStack& selected = inventory.slots[size_t(36 + sel)];
    if (selected.empty()) {
        toolHighlightTimer = 0;
    } else if (lastToolHighlight.empty() || selected.item != lastToolHighlight.item ||
               !(items.displayName(selected) == items.displayName(lastToolHighlight))) {
        toolHighlightTimer = int(40.0 * settings.notificationDisplayTime);
    } else if (toolHighlightTimer > 0) {
        --toolHighlightTimer;
    }
    lastToolHighlight = selected;
    {  // Hud.updateVignetteBrightness: the camera's light level, smoothed
        const CameraState cam = game->camera();
        const int x = int(std::floor(cam.x)), y = int(std::floor(cam.y)), z = int(std::floor(cam.z));
        int raw = 15;
        if (y >= world->minSectionY() * 16 && y < (world->maxSectionY() + 1) * 16) {
            const u8 l = world->light(x >> 4, y >> 4, z >> 4).at(x & 15, y & 15, z & 15);
            raw = std::max(int(l >> 4) - skyDarken, int(l & 15));  // getMaxLocalRawBrightness
        }
        const float v = float(raw) / 15.0f, curved = v / (4.0f - 3.0f * v);
        const float level = ambientLight + (1.0f - ambientLight) * curved;  // Lightmap.getBrightness
        vignetteBrightness += (std::clamp(1.0f - level, 0.0f, 1.0f) - vignetteBrightness) * 0.01f;
    }
    // Air supply, simulated locally like LivingEntity.baseTick (the server's entity data is not decoded).
    // Entity.isEyeInFluid(WATER): the player's fluid tracker (the eyes under the water's surface).
    const Player& p = game->player();
    const bool eyeInWater = p.eyeInWater;
    tickViewEffects(eyeInWater);  // view_effects.cpp
    if (!serverAir) {
        const u16 eye = world->block(int(std::floor(p.pos[0])), int(std::floor(p.pos[1] + p.eyeHeight())), int(std::floor(p.pos[2])));
        // LivingEntity.baseTick: no drain with water breathing (the effect, conduit power, the nautilus' breath)
        const bool breathes = hasEffect("water_breathing") || hasEffect("conduit_power") || hasEffect("breath_of_the_nautilus");
        if (eyeInWater && !registry->name(eye).starts_with("minecraft:bubble_column") && survivalLike() && !breathes)
            air = std::max(-20, air - 1);
        else air = std::min(maxAir, air + 4);
    }
    // Health blink (Hud.extractPlayerHealth bookkeeping).
    const int hp = int(std::ceil(health));
    if (hp < lastHealthInt && damageCooldown > 0) heartBlinkEnd = guiTicks + 20, lastHealthTime = clock;
    else if (hp > lastHealthInt && damageCooldown > 0) heartBlinkEnd = guiTicks + 10, lastHealthTime = clock;
    if (clock - lastHealthTime > 1.0) displayHealth = hp, lastHealthTime = clock;
    lastHealthInt = hp;
    if (damageCooldown > 0) --damageCooldown;  // LivingEntity.baseTick
    // Camera.tickFov with AbstractClientPlayer.getFieldOfViewModifier: flying, and the movement speed attribute
    // (the server's value with effects and equipment, x1.3 sprinting) against the ability's walking speed.
    // The FOV Effects option scales the change (Mth.lerp(effectScale, 1, modifier)), except the spyglass's 0.1; a
    // camera on another entity keeps 1.
    float target = 1.0f;
    if (p.flying) target *= 1.1f;
    if (p.walkingSpeed != 0.0f) {
        const float speed = float(p.attr.moveSpeed * (p.sprinting ? 1.3 : 1.0)) / p.walkingSpeed;
        target *= (speed + 1.0f) / 2.0f;
    }
    target *= game->useFovModifier();  // bow drawing (game/game.h)
    target = 1.0f + (target - 1.0f) * settings.fovEffectScale;
    if (game->scoping() && game->cameraMode == 0) target = 0.1f;  // spyglass
    if (game->cameraEntity >= 0) target = 1.0f;
    oldFovModifier = fovModifier;
    fovModifier = std::clamp(fovModifier + (target - fovModifier) * 0.5f, 0.1f, 1.5f);
    for (const UiModule& m : uiModules())  // modules.h
        if (m.tick) m.tick(*this);
}

// --- items --------------------------------------------------------------------------------------------------

void Ui::State::drawItem(Canvas& c, const ItemStack& s, float x, float y, bool decorations, const char* countOverride,
                         int playerSlot, float pop, bool icon) {
    if (s.empty()) return;
    // The item model context: GUI, the local player as the owner and what its state says about this stack (the game
    // compares stack identity: the player inventory slot it is).
    const ItemWorld& w = itemWorld();
    ItemContext ctx;
    ctx.owner = w.player;
    ctx.seed = int(x) + int(y) * 1000;  // GuiGraphics.renderItem seed: varies per slot position
    auto is = [&](int slot) {
        if (slot < 0 || size_t(slot) >= inventory.slots.size()) return false;
        return slot == playerSlot || &inventory.slots[size_t(slot)] == &s;
    };
    if (ctx.owner.present) {
        ctx.owner.selected = is(w.selectedSlot);
        ctx.owner.carried = &s == &carried;
        if (w.useSlot >= 0 && is(w.useSlot))
            ctx.owner.usingThis = true, ctx.owner.useTicks = w.useTicks, ctx.owner.remaining = w.useRemaining;
        ctx.owner.fishing = w.fishingSlot >= 0 && is(w.fishingSlot);
        ctx.owner.cooldown = w.cooldown ? w.cooldown(s) : 0.0f;
    }
    float uv[4];
    if (pop > 0) {  // Hud.extractSlot: squeezed about (x + 8, y + 12) while it pops in
        const float squeeze = 1.0f + pop / 5.0f;
        c.push();
        c.translate(x + 8, y + 12);
        c.scaleBy(1.0f / squeeze, (squeeze + 1.0f) / 2.0f);
        c.translate(-(x + 8), -(y + 12));
    }
    if (icon && icons.icon(s, uv, &ctx)) {
        c.quad(x, y, x + 16, y + 16, uv[0], uv[1], uv[2], uv[3], kWhite, ModeIcon);
        if (items.foil(s)) {  // enchantment glint (the Glint Strength)
            c.setBlend(2);
            c.quad(x, y, x + 16, y + 16, uv[0], uv[1], uv[2], uv[3], withAlpha(0xFFFFFF, float(glintOptions().strength)), ModeGlint);
            c.setBlend(0);
        }
    }
    if (pop > 0) c.pop();
    if (!decorations) return;
    // GuiGraphics.itemDecorations: durability bar, cooldown, then the count.
    const int md = items.maxDamage(s);
    int barWidth = 0;
    u32 barColor = 0;
    // ItemStack.isDamaged: damageable (a max damage, not unbreakable) and damaged
    static const int unbreakableType = items.componentType("unbreakable");
    const bool unbreakable = std::any_of(s.added.begin(), s.added.end(), [](const ItemStack::Component& c) { return c.type == unbreakableType; });
    if (s.damage > 0 && md > 0 && !unbreakable) {
        const float f = std::max(0.0f, float(md - s.damage) / float(md));
        const int width = int(std::lround(13.0f - float(s.damage) * 13.0f / float(md)));
        c.fill(x + 2, y + 13, x + 15, y + 15, 0xFF000000);
        c.fill(x + 2, y + 13, x + 2 + float(std::clamp(width, 0, 13)), y + 14, hsvToRgb(f / 3.0f, 1.0f, 1.0f));
    } else if (bundleBar(items, s, barWidth, barColor)) {  // item_ui.h: a bundle's fullness
        c.fill(x + 2, y + 13, x + 15, y + 15, 0xFF000000);
        c.fill(x + 2, y + 13, x + 2 + float(barWidth), y + 14, barColor);
    }
    if (game) {  // GuiGraphics.itemCooldown: the remaining part of the cooldown as a white sweep from the top
        const float cooldown = game->cooldownPercent(items.cooldownGroup(s), game->partialTick());
        if (cooldown > 0.0f) {
            const float top = y + std::floor(16.0f * (1.0f - cooldown)), bottom = top + std::ceil(16.0f * cooldown);
            c.fill(x, top, x + 16, bottom, 0x7FFFFFFF);
        }
    }
    if (s.count != 1 || countOverride) {  // the count last (over the cooldown)
        const std::string n = countOverride ? countOverride : std::to_string(s.count);
        font.draw(c, n, x + 19 - 2 - float(font.width(n)), y + 6 + 3, kWhite, true);
    }
}

void Ui::State::drawTooltip(Canvas& c, const std::vector<Text>& lines, float mx, float my, const TooltipImage* image,
                            bool itemTooltip, const std::string& style, const float* widget) {
    if (lines.empty()) return;
    const size_t count = lines.size() + (image ? 1 : 0);
    int w = 0, h = itemTooltip && count != 1 ? 0 : -2;
    for (const Text& l : lines) w = std::max(w, font.width(l)), h += 10;
    if (image) w = std::max(w, image->width), h += image->height;
    float x = mx + 12, y = my - 12;
    if (widget) {  // MenuTooltipPositioner
        const int sw = c.guiWidth(), sh = c.guiHeight(), top = int(widget[1]), height = int(widget[3]), bottom = top + height;
        auto offset = [&](int mouseY, int widgetY) {  // getOffset
            const int distance = std::min(std::abs(mouseY - widgetY), height);
            return int(std::lround(float(height - 3) + (5.0f - float(height - 3)) * (float(distance) / float(height))));
        };
        int rx = int(mx) + 12, ry = int(my);
        if (rx + w > sw - 5) rx = std::max(int(mx) - 12 - w, 9);
        ry += 3;
        const int padded = h + 3 + 3;
        if (bottom + 3 + offset(0, 0) + padded <= sh - 5) ry += offset(ry, top);
        else ry -= padded + offset(ry, bottom);
        x = float(rx), y = float(ry);
    } else {  // DefaultTooltipPositioner
        if (x + float(w) > float(c.guiWidth())) x = std::max(x - 24 - float(w), 4.0f);
        if (y + float(h) + 3 > float(c.guiHeight())) y = float(c.guiHeight() - h - 3);
    }
    const float x0 = x - 12, y0 = y - 12, pw = float(w + 24), ph = float(h + 24);
    // TooltipRenderUtil: tooltip/<style>_background / _frame (the style's namespace kept), else tooltip/background / frame
    std::string prefix = "tooltip/";
    if (!style.empty()) {
        const size_t colon = style.find(':');
        const std::string ns = colon == std::string::npos ? "minecraft" : style.substr(0, colon);
        const std::string path = colon == std::string::npos ? style : style.substr(colon + 1);
        prefix = (ns == "minecraft" ? std::string() : ns + ":") + "tooltip/" + path + "_";
    }
    const Sprite* bg = sprites.sprite(prefix + "background");
    const Sprite* frame = sprites.sprite(prefix + "frame");
    blitSprite(c, bg ? bg : sprites.sprite("tooltip/background"), x0, y0, pw, ph);
    blitSprite(c, frame ? frame : sprites.sprite("tooltip/frame"), x0, y0, pw, ph);
    float ly = y;
    for (size_t i = 0; i < lines.size(); ++i) {
        font.draw(c, lines[i], x, ly, kWhite, true);
        ly += 10 + (itemTooltip && i == 0 ? 2 : 0);
        if (i == 0 && image) {  // the image follows the first line
            image->draw(c, x, ly, w);
            ly += float(image->height);
        }
    }
}

// --- HUD ----------------------------------------------------------------------------------------------------

// PauseScreen.rendersNowPlayingToast: the Music Toast option shows it in the pause menu (not F3 + Esc's), with music
// audible.
bool Ui::State::rendersNowPlayingToast() const {
    const audio::AudioSystem* a = audio::instance();
    return settings.musicToast != 0 && a && a->volume(audio::Category::Music) * a->volume(audio::Category::Master) > 0.0f &&
           pauseMenuShown;
}

// Hud.extractSavingIndicator: "Saving world" bottom right while the local server saves (Autosave Indicator option).
void Ui::State::drawSavingIndicator(Canvas& c) {
    if (!settings.showAutosaveIndicator || (autosaveIndicator <= 0 && lastAutosaveIndicator <= 0)) return;
    const float v = lastAutosaveIndicator + (autosaveIndicator - lastAutosaveIndicator) * partialTick;
    const int alpha = int(std::floor(255.0f * std::clamp(v, 0.0f, 1.0f)));
    if (alpha <= 0) return;
    const Text text(res.translate("menu.savingLevel"));
    const int w = font.width(text);
    const float x = float(c.guiWidth() - w - 5), y = float(c.guiHeight() - 9 - 5);
    backdrop(*this, c, x, y, w, u32(alpha) << 24 | 0xFFFFFF);
    font.draw(c, text, x, y, u32(alpha) << 24 | 0xFFFFFF, true);
}

void Ui::State::drawHud(Canvas& c) {
    const float W = float(c.guiWidth()), H = float(c.guiHeight());
    const int gw = c.guiWidth(), gh = c.guiHeight();
    drawCameraOverlays(*this, c);  // gameplay_hud.cpp: spyglass scope
    // Crosshair (inverting blend), first person only (Hud.extractCrosshair); spectators only on something with a menu
    // or a portal (canRenderCrosshairForSpectator).
    auto spectatorCrosshair = [&] {
        if (game->crosshairOnMenuEntity) return true;
        const RayHit& t = game->target();
        if (!t.hit || !world || !registry) return false;
        std::string n;
        {
            std::shared_lock lock(world->mutex());
            n = registry->name(world->block(t.x, t.y, t.z));
        }
        n = n.substr(0, n.find('['));
        static const char* kMenus[] = {"chest", "trapped_chest", "barrel", "furnace", "blast_furnace", "smoker", "beacon", "brewing_stand",
                                       "crafter", "dispenser", "dropper", "hopper", "lectern", "cartography_table", "crafting_table",
                                       "enchanting_table", "grindstone", "loom", "smithing_table", "stonecutter", "nether_portal",
                                       "end_portal", "end_gateway"};
        for (const char* m : kMenus)
            if (n == std::string("minecraft:") + m) return true;
        return n.ends_with("shulker_box") || n.ends_with("anvil") || n.ends_with("copper_chest");
    };
    if ((!spectator() || spectatorCrosshair()) && screen == Screen::None && game->cameraMode == 0 &&
        !debugEntryEnabled(*this, "3d_crosshair")) {  // F3: the 3D crosshair instead (debug_screen.cpp)
        c.setInvert(true);
        blitSprite(c, sprites.sprite("hud/crosshair"), float((gw - 15) / 2), float((gh - 15) / 2), 15, 15);
        c.setInvert(false);
        if (survivalLike() || creative()) drawAttackIndicator(*this, c);  // gameplay_hud.cpp
    }
    // Hotbar.
    const int center = gw / 2;
    if (!spectator()) {
        blitSprite(c, sprites.sprite("hud/hotbar"), float(center - 91), H - 22, 182, 22);
        blitSprite(c, sprites.sprite("hud/hotbar_selection"), float(center - 91 - 1 + game->selectedSlot() * 20), H - 23, 24, 23);
        const ItemStack& offhand = inventory.slots[45];
        const bool offhandLeft = settings.mainHand == 1;  // the off hand opposite the main arm
        if (!offhand.empty()) {
            if (offhandLeft) blitSprite(c, sprites.sprite("hud/hotbar_offhand_left"), float(center - 91 - 29), H - 23, 29, 24);
            else blitSprite(c, sprites.sprite("hud/hotbar_offhand_right"), float(center + 91), H - 23, 29, 24);
        }
        const float pt = game ? game->partialTick() : 0.0f;
        for (int i = 0; i < 9; ++i)
            drawItem(c, inventory.slots[size_t(36 + i)], float(center - 90 + i * 20 + 2), H - 16 - 3, true, nullptr, 36 + i,
                     float(popTime[36 + i]) - pt);
        if (!offhand.empty()) drawItem(c, offhand, float(offhandLeft ? center - 91 - 26 : center + 91 + 10), H - 16 - 3, true, nullptr, 45);
        drawHotbarAttackIndicator(*this, c);  // gameplay_hud.cpp
    }
    // Status effect icons (Hud.extractEffects), top right: beneficial first row, the rest below.
    // Hud: unless the open screen shows the effects itself (showsActiveEffects: canSeeEffects, decided this frame)
    bool screenShowsEffects = false;
    if (screen == Screen::Inventory) {
        const Layout l = layoutFor(inventory);
        screenShowsEffects = guiW() - (recipeBookLeft(*this, inventory, l.width) + l.width + 2) >= 32;
    } else if (screen == Screen::Creative) {
        screenShowsEffects = guiW() - ((guiW() - 195) / 2 + 195 + 2) >= 32;  // CreativeModeInventoryScreen: 195 wide
    }
    if (!screenShowsEffects) {
        static const char* kHarmful[] = {"slowness", "mining_fatigue", "instant_damage", "nausea", "blindness", "hunger",
                                         "weakness", "poison", "wither", "levitation", "unluck", "darkness", "wind_charged",
                                         "weaving", "oozing", "infested", "bad_omen", "trial_omen", "raid_omen", "glowing"};
        int good = 0, bad = 0;
        for (const Effect* ep : sortedEffects(true)) {
            const Effect& e = *ep;
            if (!e.showIcon) continue;
            bool harmful = false;
            for (const char* h : kHarmful) harmful |= e.name == h;
            float x = W, y = 1;
            if (!harmful) x -= float(25 * ++good);
            else x -= float(25 * ++bad), y += 26;
            float alpha = 1;
            if (e.ambient) {
                blitSprite(c, sprites.sprite("hud/effect_background_ambient"), x, y, 24, 24);
            } else {
                blitSprite(c, sprites.sprite("hud/effect_background"), x, y, 24, 24);
                if (e.duration >= 0 && e.duration <= 200) {
                    const int used = 10 - e.duration / 20;
                    alpha = std::clamp(float(e.duration) / 10.0f / 5.0f * 0.5f, 0.f, 0.5f) +
                            std::cos(float(e.duration) * 3.14159265f / 5.0f) * std::clamp(float(used) / 10.0f * 0.25f, 0.f, 0.25f);
                    alpha = std::clamp(alpha, 0.f, 1.f);
                }
            }
            const Sprite* icon = sprites.texture("mob_effect/" + e.name);
            if (icon) blitTexture(c, icon, x + 3, y + 3, 0, 0, 18, 18, 18, 18, withAlpha(0xFFFFFF, alpha));
        }
    }
    const bool hurtable = survivalLike();
    if (hurtable) {
        const int xLeft = center - 91, xRight = center + 91, yBase = gh - 39;
        const int current = int(std::ceil(health));
        const bool blink = heartBlinkEnd > guiTicks && (heartBlinkEnd - guiTicks) / 3 % 2 == 1;
        const float maxH = std::max(maxHealth, float(std::max(displayHealth, current)));
        const int absorb = int(std::ceil(absorption));
        const int rows = int(std::ceil((maxH + float(absorb)) / 2.0f / 10.0f));
        const int rowHeight = std::max(10 - (rows - 2), 3);
        Jitter rnd(u64(i64(i32(u32(guiTicks) * 312871u))));  // setSeed(tickCount * 312871): an int product, sign-extended
        // Armor.
        const int armorValue = int(armor);
        if (armorValue > 0) {
            const int y = yBase - (rows - 1) * rowHeight - 10;
            for (int i = 0; i < 10; ++i) {
                const char* sp = i * 2 + 1 < armorValue ? "hud/armor_full" : i * 2 + 1 == armorValue ? "hud/armor_half" : "hud/armor_empty";
                blitSprite(c, sprites.sprite(sp), float(xLeft + i * 8), float(y), 9, 9);
            }
        }
        // Hearts.
        const std::string hc = hardcore ? "hardcore_" : "";
        const std::string heartType = hasEffect("poison") ? "poisoned" : hasEffect("wither") ? "withered" : ticksFrozen >= 140 ? "frozen" : "";
        const int regenIndex = hasEffect("regeneration") ? guiTicks % int(std::ceil(maxH + 5.0f)) : -1;
        const int containers = int(std::ceil(maxH / 2.0)), absorbContainers = int(std::ceil(absorb / 2.0));
        for (int idx = containers + absorbContainers - 1; idx >= 0; --idx) {
            const int row = idx / 10, col = idx % 10;
            const int xo = xLeft + col * 8;
            int yo = yBase - row * rowHeight;
            if (current + absorb <= 4) yo += rnd.nextInt(2);
            if (idx < containers && idx == regenIndex) yo -= 2;
            auto heart = [&](const std::string& type, bool half, bool blinking) {
                std::string name;
                if (type == "container") name = hardcore ? "container_hardcore" : "container";
                else name = (type.empty() ? "" : type + "_") + hc + (half ? "half" : "full");
                if (blinking) name += "_blinking";
                blitSprite(c, sprites.sprite("hud/heart/" + name), float(xo), float(yo), 9, 9);
            };
            heart("container", false, blink);
            const int halves = idx * 2;
            if (idx >= containers) {
                const int ah = halves - containers * 2;
                if (ah < absorb) heart(heartType == "withered" ? heartType : "absorbing", ah + 1 == absorb, false);
            }
            if (blink && halves < displayHealth) heart(heartType, halves + 1 == displayHealth, true);
            if (halves < current) heart(heartType, halves + 1 == current, false);
        }
        // Food (a ridden mount shows its hearts instead: gameplay_hud.cpp).
        const int mountHearts = drawVehicleHealth(*this, c, xRight, yBase);
        for (int i = 0; i < 10 && mountHearts == 0; ++i) {
            int yo = yBase;
            if (saturation <= 0 && guiTicks % (food * 3 + 1) == 0) yo += rnd.nextInt(3) - 1;
            const float xo = float(xRight - i * 8 - 9);
            const std::string h = hasEffect("hunger") ? "_hunger" : "";
            blitSprite(c, sprites.sprite("hud/food_empty" + h), xo, float(yo), 9, 9);
            if (i * 2 + 1 < food) blitSprite(c, sprites.sprite("hud/food_full" + h), xo, float(yo), 9, 9);
            if (i * 2 + 1 == food) blitSprite(c, sprites.sprite("hud/food_half" + h), xo, float(yo), 9, 9);
        }
        // Air bubbles.
        const int maxA = maxAir, cur = std::clamp(air, 0, maxA);
        const bool underWater = game->player().eyeInWater;  // isEyeInFluid(WATER): the player's eyes, waterlogged too
        if (underWater || cur < maxA) {
            // getAirBubbleYLine: above the food, or above the ridden mount's heart rows
            const int yAir = mountHearts == 0 ? yBase - 10 : yBase - 10 - (int(std::ceil(mountHearts / 10.0)) - 1) * 10;
            auto bubble = [&](int ticks, int offset) { return int(std::ceil(float((ticks + offset) * 10) / float(maxA))); };
            const int full = bubble(cur, -2), popping = bubble(cur, 0);
            const int empty = 10 - bubble(cur, cur != 0 && underWater ? 1 : 0);
            if (!underWater) lastBubblePopSound = 0;
            for (int b = 1; b <= 10; ++b) {
                const float bx = float(xRight - (b - 1) * 8 - 9);
                if (b <= full) blitSprite(c, sprites.sprite("hud/air"), bx, float(yAir), 9, 9);
                else if (full != popping && b == popping && underWater) {
                    blitSprite(c, sprites.sprite("hud/air_bursting"), bx, float(yAir), 9, 9);
                    if (lastBubblePopSound != b) {  // Hud.playAirBubblePoppedSound: louder / higher as the air runs out
                        lastBubblePopSound = b;
                        if (audio::AudioSystem* a = audio::instance()) {
                            const Player& pl = game->player();
                            a->play("minecraft:ui.hud.bubble_pop", audio::Category::Player, pl.pos[0], pl.pos[1], pl.pos[2],
                                    0.5f + 0.1f * float(std::max(0, empty - 3 + 1)), 1.0f + 0.1f * float(std::max(0, empty - 5 + 1)));
                        }
                    }
                }
                else if (b > 10 - empty) {  // all empty: a wobble on even ticks
                    const int wobble = empty == 10 && guiTicks % 2 == 0 ? rnd.nextInt(2) : 0;
                    blitSprite(c, sprites.sprite("hud/air_empty"), bx, float(yAir + wobble), 9, 9);
                }
            }
        }
    } else {
        drawVehicleHealth(*this, c, center + 91, gh - 39);  // Hud.extractVehicleHealth: also when the player cannot be hurt
    }
    if (!spectator() || hasWaypoints()) {
        // Hud.nextContextualInfoState: with waypoints the locator bar (but the jump bar while it charges and the
        // experience bar for 5 seconds after it changed), else a jumping mount's bar, else the experience bar.
        const int left = (gw - 182) / 2, top = gh - 24 - 5;
        const Game::VehicleHud& v = game->vehicleHud;
        const bool experience = hurtable, jumpFirst = v.jumpScale > 0 || v.jumpCooldown > 0;
        enum { Empty, Experience, Locator, Jump } bar;
        if (hasWaypoints()) bar = v.jumpable && jumpFirst ? Jump : experience && xpDisplayStart + 100 > guiTicks ? Experience : Locator;
        else bar = v.jumpable ? Jump : experience ? Experience : Empty;
        if (bar == Jump) {
            drawJumpBar(*this, c, left, top);  // gameplay_hud.cpp
        } else if (bar == Locator) {
            drawLocatorBarBackground(*this, c, left, top);  // locator_bar.cpp
        } else if (bar == Experience) {
            blitSprite(c, sprites.sprite("hud/experience_bar_background"), float(left), float(top), 182, 5);
            const int progress = int(xpProgress * 183.0f);
            if (progress > 0) blitSpriteRegion(c, sprites.sprite("hud/experience_bar_progress"), 182, 5, 0, 0, float(left), float(top), float(progress), 5);
        }
        if (experience && xpLevel > 0) {  // ContextualBar.extractExperienceLevel
            const Text lv = translatable("gui.experience.level", {Text(std::to_string(xpLevel))}, lang());
            const float x = float((gw - font.width(lv)) / 2), y = float(gh - 24 - 9 - 2);
            for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) font.draw(c, lv, x + float(dx), y + float(dy), 0xFF000000, false);
            font.draw(c, lv, x, y, 0xFF80FF20, false);
        }
        if (bar == Locator) drawLocatorBarDots(*this, c, top);
    }
    // Selected item name.
    if (toolHighlightTimer > 0 && !lastToolHighlight.empty() && !spectator()) {
        const Text name = items.hoverName(lastToolHighlight);
        const int w = font.width(name);
        int y = gh - 59;
        if (!hurtable) y += 14;
        const int alpha = std::min(255, int(float(toolHighlightTimer) * 256.0f / 10.0f));
        if (alpha > 0) {
            backdrop(*this, c, float((gw - w) / 2), float(y), w, u32(alpha) << 24 | 0xFFFFFF);
            font.draw(c, name, float((gw - w) / 2), float(y), u32(alpha) << 24 | 0xFFFFFF, true);
        }
    }
    // Hud.extractRenderState: boss bars (and the other modules' HUD parts), the sleep overlay, the sidebar, the action
    // bar, the title, the chat.
    for (const UiModule& m : uiModules())  // modules.h: boss bars, the spectator menu, debug markers
        if (m.drawHud) m.drawHud(*this, c);
    drawSleepOverlay(*this, c);
    drawScoreboardSidebar(*this, c);
    // Action bar.
    if (actionbarTime > 0) {
        const float t = float(actionbarTime) - partialTick;
        const int alpha = std::min(255, int(t * 255.0f / 20.0f));
        if (alpha > 0) {
            const int w = font.width(actionbar);
            u32 color = u32(alpha) << 24 | 0xFFFFFF;
            if (actionbarAnimated) {  // Mth.hsvToArgb(t / 50, 0.7, 0.6, alpha)
                const float hue = t / 50.0f, sat = 0.7f, val = 0.6f;
                const int i = int(hue * 6.0f) % 6;
                const float f = hue * 6.0f - float(i), p = val * (1 - sat), q = val * (1 - f * sat), u = val * (1 - (1 - f) * sat);
                float r, g, b;
                switch ((i + 6) % 6) {
                    case 0: r = val, g = u, b = p; break;
                    case 1: r = q, g = val, b = p; break;
                    case 2: r = p, g = val, b = u; break;
                    case 3: r = p, g = q, b = val; break;
                    case 4: r = u, g = p, b = val; break;
                    default: r = val, g = p, b = q; break;
                }
                color = u32(alpha) << 24 | u32(std::clamp(int(r * 255), 0, 255)) << 16 | u32(std::clamp(int(g * 255), 0, 255)) << 8 |
                        u32(std::clamp(int(b * 255), 0, 255));
            }
            backdrop(*this, c, float(gw / 2 - w / 2), float(gh - 68 - 4), w, color);
            font.draw(c, actionbar, float(gw / 2 - w / 2), float(gh - 68 - 4), color, true);
        }
    }
    // Title and subtitle.
    if (hasTitle && titleTime > 0) {
        const float t = float(titleTime) - partialTick;
        int alpha = 255;
        if (titleTime > titleFadeOut + titleStay) alpha = int((float(titleFadeIn + titleStay + titleFadeOut) - t) * 255.0f / float(std::max(1, titleFadeIn)));
        if (titleTime <= titleFadeOut) alpha = int(t * 255.0f / float(std::max(1, titleFadeOut)));
        alpha = std::clamp(alpha, 0, 255);
        if (alpha > 0) {
            const u32 color = u32(alpha) << 24 | 0xFFFFFF;
            c.push();
            c.translate(float(gw / 2), float(gh / 2));
            c.push();
            c.scaleBy(4);
            const int tw = font.width(title);
            backdrop(*this, c, float(-tw / 2), -10, tw, color);
            font.draw(c, title, float(-tw / 2), -10, color, true);
            c.pop();
            if (hasSubtitle) {
                c.push();
                c.scaleBy(2);
                const int sw = font.width(subtitle);
                backdrop(*this, c, float(-sw / 2), 5, sw, color);
                font.draw(c, subtitle, float(-sw / 2), 5, color, true);
                c.pop();
            }
            c.pop();
        }
    }
    if (screen != Screen::Chat) drawChat(c, false);
    for (const UiModule& m : uiModules())  // over the chat: player list
        if (m.drawHudTop) m.drawHudTop(*this, c);
    (void)W;
}

// --- chat ---------------------------------------------------------------------------------------------------

int chatWidthPixels(double pct) { return int(std::floor(pct * 280.0 + 40.0)); }   // ChatComponent.getWidth
int chatHeightPixels(double pct) { return int(std::floor(pct * 160.0 + 20.0)); }  // ChatComponent.getHeight

namespace {
// ComponentRenderUtils.wrapComponents: legacy codes stripped without Colors, wrapped lines indented by a space.
std::vector<Text> wrapChat(UiState& s, const Text& message, int maxWidth) {
    Text src;
    for (const Span& sp : message.spans) {
        if (s.settings.chatColors || sp.text.find("\xC2\xA7") == std::string::npos) {
            src.spans.push_back(sp);
            continue;
        }
        std::string t;  // ChatFormatting.stripFormatting
        for (size_t i = 0; i < sp.text.size(); ++i) {
            if (u8(sp.text[i]) == 0xC2 && i + 1 < sp.text.size() && u8(sp.text[i + 1]) == 0xA7) {
                i += 2;  // the code character after the section sign
                continue;
            }
            t += sp.text[i];
        }
        src.spans.push_back({t, sp.style});
    }
    std::vector<Text> lines;
    for (const Text& paragraph : s.font.split(src, 1 << 20)) {  // explicit line breaks are not indented
        const std::vector<Text> wrapped = s.font.split(paragraph, maxWidth);
        for (size_t i = 0; i < wrapped.size(); ++i) {
            if (i == 0) {
                lines.push_back(wrapped[i]);
                continue;
            }
            Text indented(" ");
            indented.append(wrapped[i]);
            lines.push_back(std::move(indented));
        }
    }
    return lines;
}

double chatScale(const UiState& s) { return s.settings.chatScale; }
}  // namespace

bool Ui::State::chatVisible(int source) const {
    if (source == 0) return true;                       // client system messages always show
    if (source == 2) return settings.chatVisibility == 0;  // player chat: Shown only
    return settings.chatVisibility != 2;                // server system messages: not when Hidden
}

int Ui::State::chatLinesPerPage() const {  // getHeight / getLineHeight
    const int height = chatHeightPixels(screen == Screen::Chat ? settings.chatHeightFocused : settings.chatHeightUnfocused);
    return height / std::max(1, int(9.0 * (settings.chatLineSpacing + 1.0)));
}

void Ui::State::scrollChat(int lines) {
    chatScroll += lines;
    const int max = int(chatLines.size());
    if (chatScroll > max - chatLinesPerPage()) chatScroll = max - chatLinesPerPage();
    if (chatScroll <= 0) chatScroll = 0, newMessageSinceScroll = false;
}

void Ui::State::addChat(const Text& message, int source) {
    addChatMessage({message, guiTicks, {}, kTagSystemSinglePlayer, u8(source)});  // add{Client,Server}SystemMessage
}

void Ui::State::addChatMessage(ChatMessage m) {
    if (!chatVisible(m.source)) return;
    nativeMods.chatReceived(m.text.plain());
    const int maxWidth = int(std::floor(double(chatWidthPixels(settings.chatWidth)) / std::max(0.01, chatScale(*this))));
    // GuiMessage.splitLines: a tag's icon takes its width + 4 + 2 off the line
    const std::vector<Text> lines = wrapChat(*this, m.text, maxWidth - (m.tag.icon ? 9 + 4 + 2 : 0));
    for (size_t i = 0; i < lines.size(); ++i) {
        if (screen == Screen::Chat && chatScroll > 0) newMessageSinceScroll = true, scrollChat(1);
        chatLines.insert(chatLines.begin(), ChatLine{lines[i], m.addedTick, i == lines.size() - 1, m.tag});
    }
    if (chatLines.size() > 100) chatLines.resize(100);
    narrator::chat(*this, m.text, m.source);
    chatMessages.insert(chatMessages.begin(), std::move(m));
    if (chatMessages.size() > 100) chatMessages.pop_back();
}

void Ui::State::refreshChatLines() {  // ChatComponent.refreshTrimmedMessages: the messages the Chat option shows
    chatLines.clear();
    const int maxWidth = int(std::floor(double(chatWidthPixels(settings.chatWidth)) / std::max(0.01, settings.chatScale)));
    for (size_t i = chatMessages.size(); i-- > 0;) {
        const ChatMessage& msg = chatMessages[i];
        if (!chatVisible(msg.source)) continue;
        const std::vector<Text> lines = wrapChat(*this, msg.text, maxWidth - (msg.tag.icon ? 9 + 4 + 2 : 0));
        for (size_t k = 0; k < lines.size(); ++k)
            chatLines.insert(chatLines.begin(), ChatLine{lines[k], msg.addedTick, k == lines.size() - 1, msg.tag});
    }
    if (chatLines.size() > 100) chatLines.resize(100);
}

void chatRescale(UiState& s) {  // rescaleChat: resetChatScroll + refreshTrimmedMessages
    s.chatScroll = 0, s.newMessageSinceScroll = false;
    s.refreshChatLines();
}

void Ui::State::handleChatMessage(std::function<bool()> show) {
    if (settings.chatDelay > 0.0) chatQueue.push_back(std::move(show));
    else if (show()) previousMessageTime = clock;
}

void Ui::State::tickChat() {  // ChatListener.tick
    if (paused) {
        if (settings.chatDelay > 0.0) previousMessageTime += 0.05;
        return;
    }
    if (settings.chatDelay <= 0.0) {
        while (!chatQueue.empty()) {  // flushQueue
            auto fn = std::move(chatQueue.front());
            chatQueue.pop_front();
            fn();
        }
        previousMessageTime = 0;
    } else if (clock >= previousMessageTime + settings.chatDelay) {
        while (!chatQueue.empty()) {
            auto fn = std::move(chatQueue.front());
            chatQueue.pop_front();
            if (fn()) {
                previousMessageTime = clock;
                break;
            }
        }
    }
}

// ChatComponent.extractRenderState: the chat scale, width, line spacing, text opacity and background opacity
// options, the message delay queue line and (focused) the restricted prompt and the scroll bar.
void Ui::State::drawChat(Canvas& c, bool focused) {
    const bool restricted = focused && settings.chatVisibility != 0;
    if (chatLines.empty() && !restricted) return;  // nothing to show (a lone delay queue shows with its first line)
    const float scale = float(settings.chatScale);
    const int maxWidth = int(std::ceil(float(chatWidthPixels(settings.chatWidth)) / std::max(0.01f, scale)));
    const int chatBottom = int(std::floor(float(c.guiHeight() - 40) / std::max(0.01f, scale)));
    const float textOpacity = float(settings.chatOpacity) * 0.9f + 0.1f;
    const float bgOpacity = float(settings.textBackgroundOpacity);
    const double spacing = settings.chatLineSpacing;
    const int entryHeight = int(9.0 * (spacing + 1.0));
    const int entryBottomToMessageY = int(std::lround(8.0 * (spacing + 1.0) - 4.0 * spacing));
    const int linesPerPage = chatLinesPerPage();
    c.push();
    c.scaleBy(scale);
    c.translate(4, 0);
    const int total = int(chatLines.size());
    auto alphaOf = [&](const ChatLine& line) {  // AlphaCalculator.timeBased (focused: fully visible)
        if (focused) return 1.0f;
        double t = double(guiTicks - line.addedTick) / 200.0;
        t = std::clamp((1.0 - t) * 10.0, 0.0, 1.0);
        return float(t * t);
    };
    int count = 0;
    for (int i = std::min(total - chatScroll, linesPerPage) - 1; i >= 0; --i) {
        const float alpha = alphaOf(chatLines[size_t(i + chatScroll)]);
        if (alpha <= 1e-5f) continue;
        ++count;
        const int entryBottom = chatBottom - i * entryHeight, entryTop = entryBottom - entryHeight;
        c.fill(-4, float(entryTop), float(maxWidth + 4 + 4), float(entryBottom), withAlpha(0, alpha * bgOpacity));
    }
    const int lineAboveMessagesY = chatBottom - (count + 1) * entryHeight;
    if (!chatQueue.empty()) c.fill(-2, float(chatBottom), float(maxWidth + 4), float(chatBottom + 9), withAlpha(0, bgOpacity));
    if (restricted) c.fill(-2, float(lineAboveMessagesY), float(maxWidth + 4 + 4), float(lineAboveMessagesY + entryHeight), withAlpha(0, bgOpacity));
    for (int i = std::min(total - chatScroll, linesPerPage) - 1; i >= 0; --i) {
        const ChatLine& line = chatLines[size_t(i + chatScroll)];
        const float alpha = alphaOf(line);
        if (alpha <= 1e-5f) continue;
        const int entryBottom = chatBottom - i * entryHeight, entryTop = entryBottom - entryHeight;
        const int textTop = entryBottom - entryBottomToMessageY;
        font.draw(c, line.text, 0, float(textTop), withAlpha(0xFFFFFF, alpha * textOpacity), true);
        if (line.tag.color)  // GuiMessageTag indicator
            c.fill(-4, float(entryTop), -2, float(entryBottom), withAlpha(line.tag.color, alpha * textOpacity));
    }
    if (focused) {  // DrawingFocusedGraphicsAccess.handleTagIcon: shown over a hovered message (on its last line) or icon
        const float lmx = mouseX / scale - 4.0f, lmy = mouseY / scale;
        bool hoveredMessage = false;
        for (int i = std::min(total - chatScroll, linesPerPage) - 1; i >= 0; --i) {  // top first, as forEachLine
            const ChatLine& line = chatLines[size_t(i + chatScroll)];
            const int entryBottom = chatBottom - i * entryHeight, textTop = entryBottom - entryBottomToMessageY;
            const int width = font.width(line.text);
            hoveredMessage |= lmx >= 0 && lmx < float(width) && lmy >= float(textTop) && lmy < float(textTop + 9);
            const bool force = line.endOfEntry && hoveredMessage;
            if (line.endOfEntry) hoveredMessage = false;
            if (!line.tag.icon) continue;
            const int left = width + 4, top = textTop + 9 - 9 - 1;  // handleTagIcon: bottom - height - 1 .. bottom
            const bool over = lmx >= float(left) && lmx < float(left + 9) && lmy >= float(top) && lmy < float(textTop + 9);
            if (force || over) blitSprite(c, sprites.sprite("icon/chat_modified"), float(left), float(top), 9, 9);
        }
    }
    if (!chatQueue.empty())  // "[+n pending lines]": clicking it shows the next one (QUEUE_EXPAND_ID)
        font.draw(c, translatable("chat.queue", {Text(std::to_string(chatQueue.size()))}, lang()), 0, float(chatBottom + 9 - 8),
                  withAlpha(0xFFFFFF, 0.5f * textOpacity), true);
    if (restricted) {  // ChatScreen with restrictions: the red, underlined prompt (opens the Restrictions screen)
        Style st = colorStyle(0xFF5555);
        st.underlined = 1;
        std::string prompt = res.translate("chat_screen.restricted");  // ComponentRenderUtils.clipText to the chat width
        if (font.width(Text(prompt)) > maxWidth) prompt = font.headByWidth(prompt, maxWidth - font.width(Text("..."))) + "...";
        font.draw(c, Text(prompt, st), 0, float(lineAboveMessagesY + entryHeight - entryBottomToMessageY - 1),
                  withAlpha(0xFFFFFF, textOpacity), true);
    }
    if (focused && total > 0) {  // scroll bar
        const int chatHeight = count * entryHeight, virtualHeight = total * entryHeight;
        if (virtualHeight != chatHeight) {
            const int y = chatScroll * chatHeight / total - chatBottom;
            const int h = chatHeight * chatHeight / virtualHeight;
            const int a = y > 0 ? 170 : 96;
            const u32 color = newMessageSinceScroll ? 0xCC3333 : 0x3333AA;
            c.fill(float(maxWidth + 4), float(-y), float(maxWidth + 6), float(-y - h), u32(a) << 24 | color);
            c.fill(float(maxWidth + 6), float(-y), float(maxWidth + 5), float(-y - h), u32(a) << 24 | 0xCCCCCC);
        }
    }
    c.pop();
}

// Chat clicks below / above the lines: the queue line shows the next waiting message, the restricted prompt opens
// the Restrictions screen. true when handled.
// The queue line ("[+n pending lines]", 1) or the restricted prompt (2) under a GUI point: their text only (the click
// and hover events are on the styled text); 0 none.
int chatSpecialAt(UiState& s, float mx, float my) {
    const float scale = float(std::max(0.01, s.settings.chatScale));
    const int chatBottom = int(std::floor(float(s.guiH() - 40) / scale));
    const float x = mx / scale - 4, y = my / scale;
    if (x < 0) return 0;
    if (!s.chatQueue.empty()) {
        const int w = s.font.width(translatable("chat.queue", {Text(std::to_string(s.chatQueue.size()))}, s.lang()));
        const int top = chatBottom + 9 - 8;
        if (x < float(w) && y >= float(top) && y < float(top + 9)) return 1;
    }
    if (s.settings.chatVisibility != 0) {
        const double spacing = s.settings.chatLineSpacing;
        const int entryHeight = int(9.0 * (spacing + 1.0));
        const int entryBottomToMessageY = int(std::lround(8.0 * (spacing + 1.0) - 4.0 * spacing));
        const int count = std::max(0, std::min(int(s.chatLines.size()) - s.chatScroll, s.chatLinesPerPage()));
        const int top = chatBottom - (count + 1) * entryHeight + entryHeight - entryBottomToMessageY - 1;
        const int maxWidth = int(std::ceil(float(chatWidthPixels(s.settings.chatWidth)) / scale));
        const int width = std::min(s.font.width(Text(s.res.translate("chat_screen.restricted"))), maxWidth);  // clipped
        if (x < float(width) && y >= float(top) && y < float(top + 9)) return 2;
    }
    return 0;
}

bool chatSpecialClick(UiState& s, float mx, float my) {
    const int at = chatSpecialAt(s, mx, my);
    if (at == 1) {  // QUEUE_EXPAND_ID
        auto fn = std::move(s.chatQueue.front());  // acceptNextDelayedMessage: a shown message restarts the delay
        s.chatQueue.pop_front();
        if (fn()) s.previousMessageTime = s.clock;
        return true;
    }
    if (at == 2) {  // GO_TO_RESTRICTIONS_SCREEN
        s.restrictionsParent = Screen::Chat;
        s.openScreen(Screen::Restrictions);
        return true;
    }
    return false;
}

// The style under a GUI point in the open chat (the drawChat geometry); false: none.
bool chatLineStyleAt(UiState& s, float mx, float my, Style& out) {
    const float scale = float(std::max(0.01, s.settings.chatScale));
    const int chatBottom = int(std::floor(float(s.guiH() - 40) / scale));
    const float x = mx / scale - 4, y = my / scale;
    const int entryHeight = int(9.0 * (s.settings.chatLineSpacing + 1.0));
    if (y >= float(chatBottom) || x < 0) return false;
    const int i = int(std::floor((float(chatBottom) - y) / float(entryHeight)));
    if (i < 0 || i >= std::min(int(s.chatLines.size()) - s.chatScroll, s.chatLinesPerPage())) return false;
    const Text& line = s.chatLines[size_t(i + s.chatScroll)].text;
    float pen = 0;
    bool found = false;
    s.font.iterate(line, [&](int, const Style& st, u32 cp) {
        const float adv = s.font.glyph(cp, st).advanceFor(st.isBold());
        if (x >= pen && x < pen + adv) {
            out = st;
            found = true;
            return false;
        }
        pen += adv;
        return true;
    });
    return found;
}

// --- text fields (EditBox) ----------------------------------------------------------------------------------

namespace {
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
size_t wordPos(const std::string& s, size_t i, int dir) {  // EditBox.getWordPosition
    if (dir < 0) {
        while (i > 0 && s[i - 1] == ' ') --i;
        while (i > 0 && s[i - 1] != ' ') --i;
    } else {
        while (i < s.size() && s[i] != ' ') ++i;
        while (i < s.size() && s[i] == ' ') ++i;
    }
    return i;
}
}  // namespace

bool Ui::State::textFieldKey(TextField& f, const Window::InputEvent& e) {
    auto insert = [&](const std::string& text) {
        std::string clean;
        for (size_t i = 0; i < text.size();) {
            const u32 cp = nextCodepoint(text, i);
            if (cp >= 32 && cp != 127 && cp != 0xA7) appendUtf8(clean, cp);  // StringUtil.filterText
        }
        const size_t a = size_t(std::min(f.cursor, f.selection)), b = size_t(std::max(f.cursor, f.selection));
        auto chars = [](const std::string& t) {  // String.length: UTF-16 units
            int n = 0;
            for (size_t i = 0; i < t.size();) n += nextCodepoint(t, i) > 0xFFFF ? 2 : 1;
            return n;
        };
        std::string v = f.value.substr(0, a) + clean + f.value.substr(b);
        if (chars(v) > f.maxLength) {  // EditBox.insertText: maxLength - length + the selection, in characters
            const int room = std::max(0, f.maxLength - chars(f.value) + chars(f.value.substr(a, b - a)));
            std::string cut;
            int used = 0;
            for (size_t i = 0; i < clean.size();) {
                size_t j = i;
                const int w = nextCodepoint(clean, j) > 0xFFFF ? 2 : 1;
                if (used + w > room) break;
                cut += clean.substr(i, j - i), used += w;
                i = j;
            }
            clean = cut;
            v = f.value.substr(0, a) + clean + f.value.substr(b);
        }
        f.value = v;
        f.cursor = f.selection = int(a + clean.size());
    };
    if (e.type == Window::InputEvent::Char) {
        std::string s;
        appendUtf8(s, u32(e.code));
        insert(s);
        f.blinkStart = clock;
        return true;
    }
    if (e.type != Window::InputEvent::Key || !e.down) return false;
    const bool shift = window->keyDown(VK_SHIFT), ctrl = window->keyDown(VK_CONTROL);
    const size_t cur = size_t(f.cursor);
    auto move = [&](size_t to) {
        f.cursor = int(to);
        if (!shift) f.selection = f.cursor;
    };
    f.blinkStart = clock;
    switch (e.code) {
        case VK_LEFT: move(ctrl ? wordPos(f.value, cur, -1) : prevCp(f.value, cur)); return true;
        case VK_RIGHT: move(ctrl ? wordPos(f.value, cur, 1) : nextCp(f.value, cur)); return true;
        case VK_HOME: move(0); return true;
        case VK_END: move(f.value.size()); return true;
        case VK_BACK:
        case VK_DELETE: {
            if (f.cursor != f.selection) {
                insert("");
                return true;
            }
            const size_t to = e.code == VK_BACK ? (ctrl ? wordPos(f.value, cur, -1) : prevCp(f.value, cur))
                                                : (ctrl ? wordPos(f.value, cur, 1) : nextCp(f.value, cur));
            const size_t a = std::min(cur, to), b = std::max(cur, to);
            f.value.erase(a, b - a);
            f.cursor = f.selection = int(a);
            return true;
        }
        case 'A':
            if (!ctrl) return false;
            f.selection = 0, f.cursor = int(f.value.size());
            return true;
        case 'C':
        case 'X':
            if (!ctrl) return false;
            if (f.cursor != f.selection) {
                const size_t a = size_t(std::min(f.cursor, f.selection)), b = size_t(std::max(f.cursor, f.selection));
                window->setClipboard(f.value.substr(a, b - a));
                if (e.code == 'X') insert("");
            }
            return true;
        case 'V':
            if (!ctrl) return false;
            insert(window->clipboard());
            return true;
    }
    return false;
}

namespace {
int fieldPosAt(Ui::State& s, const Ui::State::TextField& f, float mouseX) {  // EditBox.findClickedPositionInText
    const int rel = std::min(int(std::floor(mouseX)) - int(f.textX), int(f.textW));
    const std::string_view visible = std::string_view(f.value).substr(size_t(std::min<int>(f.scroll, int(f.value.size()))));
    const std::string head = s.font.headByWidth(visible, std::max(0, rel));
    return std::min(int(f.value.size()), f.scroll + int(head.size()));
}
}  // namespace

bool Ui::State::textFieldHit(const TextField& f) const {
    return mouseX >= f.textX - 4 && mouseX < f.textX + f.textW + 4 && mouseY >= f.textY - 4 && mouseY < f.textY + 12;
}

void Ui::State::textFieldMouseDown(TextField& f) {
    const int pos = fieldPosAt(*this, f, mouseX);
    const bool dbl = f.lastClick >= 0 && clock - f.lastClick < 0.25;  // MouseHandler: the same button within 250 ms
    f.lastClick = dbl ? -1 : clock;
    if (dbl) {  // selectWord
        f.selection = int(wordPos(f.value, size_t(pos), -1));
        f.cursor = int(wordPos(f.value, size_t(pos), 1));
    } else {
        f.cursor = pos;
        if (!window || !window->keyDown(VK_SHIFT)) f.selection = pos;
    }
    f.blinkStart = clock;
    dragField = &f;
}

void Ui::State::drawTextField(Canvas& c, TextField& f, float x, float y, float w, u32 color, bool focused, bool shadow) {
    f.textX = x, f.textY = y, f.textW = w;
    if (textFieldHit(f)) requestCursor(CursorIBeam);  // EditBox: an I-beam over the field
    if (dragField == &f) {  // EditBox.onDrag: the cursor follows the mouse while the left button is down
        if (leftDown) f.cursor = fieldPosAt(*this, f, mouseX);
        else dragField = nullptr;
    }
    // Keep the cursor visible (EditBox.scrollTo).
    const int width = int(w);
    f.scroll = std::min(f.scroll, int(f.value.size()));
    if (f.cursor < f.scroll) f.scroll = f.cursor;
    while (f.scroll < f.cursor && font.width(f.value.substr(size_t(f.scroll), size_t(f.cursor - f.scroll))) > width)
        f.scroll = int(nextCp(f.value, size_t(f.scroll)));
    const std::string visible = font.headByWidth(std::string_view(f.value).substr(size_t(f.scroll)), width);
    const int curRel = f.cursor - f.scroll;
    const bool cursorVisible = curRel >= 0 && curRel <= int(visible.size());
    const bool blink = focused && int((clock - f.blinkStart) / 0.3) % 2 == 0;
    Text formatted;
    Style draft = colorStyle(0xAAAAAA);
    draft.italic = 1;
    if (&f == &chatInput && chatIsDraft) formatted = Text(visible, draft);  // ChatScreen.formatChat
    const bool format = &f == &chatInput ? chatIsDraft || (chatFormatter && chatFormatter(visible, f.scroll, formatted))
                                         : &f == formattedField && fieldFormatter && fieldFormatter(visible, f.scroll, formatted);
    float endX = format ? font.draw(c, formatted, x, y, color, shadow, true) : font.draw(c, visible, x, y, color, shadow, true);
    const bool atEnd = f.cursor >= int(f.value.size());
    const float cursorX = cursorVisible ? x + float(font.width(visible.substr(0, size_t(curRel)))) : endX;
    if (blink && cursorVisible) {
        if (atEnd) font.draw(c, "_", cursorX, y, color, shadow);
        else c.fill(cursorX, y - 1, cursorX + 1, y + 1 + 9, 0xFFD0D0D0);
    }
    if (f.selection != f.cursor) {
        const int a = std::clamp(std::min(f.cursor, f.selection) - f.scroll, 0, int(visible.size()));
        const int b = std::clamp(std::max(f.cursor, f.selection) - f.scroll, 0, int(visible.size()));
        const float x0 = x + float(font.width(visible.substr(0, size_t(a)))), x1 = x + float(font.width(visible.substr(0, size_t(b))));
        c.setInvert(true);
        c.fill(x0, y - 1, x1, y + 1 + 9, 0xFF0000FF);
        c.setInvert(false);
    }
    (void)endX;
}

}  // namespace ui
