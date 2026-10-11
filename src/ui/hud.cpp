// HUD (Hud.java), chat (ChatComponent / ChatScreen input), F3 debug overlay, item slots and tooltips.
#include "assets/assets.h"
#include "ui/gameplay_hud.h"
#include "render/renderer.h"
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

// java.util.Random-compatible enough for HUD jitter (Hud uses its own RandomSource seeded per tick).
struct Jitter {
    u64 seed;
    explicit Jitter(u64 s) : seed((s ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1)) {}
    int next(int bits) {
        seed = (seed * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
        return int(seed >> (48 - bits));
    }
    int nextInt(int bound) { return int((u64(next(31)) * u64(bound)) >> 31); }
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

const char* kDirNames[4] = {"south", "west", "north", "east"};
const char* kTowards[4] = {"Towards positive Z", "Towards negative X", "Towards negative Z", "Towards positive X"};
}  // namespace

// --- per tick -----------------------------------------------------------------------------------------------

void Ui::State::tickHud() {
    ++guiTicks;
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
        toolHighlightTimer = 40;  // 40 * notificationDisplayTime (1.0)
    } else if (toolHighlightTimer > 0) {
        --toolHighlightTimer;
    }
    lastToolHighlight = selected;
    // Air supply, simulated locally like LivingEntity.baseTick (the server's entity data is not decoded).
    // Entity.isEyeInFluid(WATER): the player's fluid tracker (the eyes under the water's surface).
    const Player& p = game->player();
    const bool eyeInWater = p.eyeInWater;
    tickViewEffects(eyeInWater);  // view_effects.cpp
    if (!serverAir) {
        const u16 eye = world->block(int(std::floor(p.pos[0])), int(std::floor(p.pos[1] + p.eyeHeight())), int(std::floor(p.pos[2])));
        if (eyeInWater && !registry->name(eye).starts_with("minecraft:bubble_column") && survivalLike()) air = std::max(-20, air - 1);
        else air = std::min(maxAir, air + 4);
    }
    // Health blink (Hud.extractPlayerHealth bookkeeping).
    const int hp = int(std::ceil(health));
    if (hp < lastHealthInt) heartBlinkEnd = guiTicks + 20;
    else if (hp > lastHealthInt) heartBlinkEnd = guiTicks + 10;
    if (hp != lastHealthInt) lastHealthTime = clock;
    if (clock - lastHealthTime > 1.0) displayHealth = hp;
    lastHealthInt = hp;
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

void Ui::State::drawItem(Canvas& c, const ItemStack& s, float x, float y, bool decorations, const char* countOverride) {
    if (s.empty()) return;
    float uv[4];
    if (icons.icon(s, uv)) {
        c.quad(x, y, x + 16, y + 16, uv[0], uv[1], uv[2], uv[3], kWhite, ModeIcon);
        if (items.foil(s)) {  // enchantment glint (glintStrength 0.75)
            c.setBlend(2);
            c.quad(x, y, x + 16, y + 16, uv[0], uv[1], uv[2], uv[3], withAlpha(0xFFFFFF, 0.75f), ModeGlint);
            c.setBlend(0);
        }
    }
    if (!decorations) return;
    // GuiGraphics.itemDecorations: durability bar, then the count.
    const int md = items.maxDamage(s);
    if (s.damage > 0 && md > 0) {
        const float f = std::max(0.0f, float(md - s.damage) / float(md));
        const int width = int(std::lround(13.0f - float(s.damage) * 13.0f / float(md)));
        c.fill(x + 2, y + 13, x + 15, y + 15, 0xFF000000);
        c.fill(x + 2, y + 13, x + 2 + float(std::clamp(width, 0, 13)), y + 14, hsvToRgb(f / 3.0f, 1.0f, 1.0f));
    }
    if (s.count != 1 || countOverride) {
        const std::string n = countOverride ? countOverride : std::to_string(s.count);
        font.draw(c, n, x + 19 - 2 - float(font.width(n)), y + 6 + 3, kWhite, true);
    }
    if (game) {  // GuiGraphics.itemCooldown: the remaining part of the cooldown as a white sweep from the top
        const float cooldown = game->cooldownPercent(items.cooldownGroup(s), game->partialTick());
        if (cooldown > 0.0f) {
            const float top = y + std::floor(16.0f * (1.0f - cooldown)), bottom = top + std::ceil(16.0f * cooldown);
            c.fill(x, top, x + 16, bottom, 0x7FFFFFFF);
        }
    }
}

void Ui::State::drawTooltip(Canvas& c, const std::vector<Text>& lines, float mx, float my) {
    if (lines.empty()) return;
    int w = 0, h = lines.size() == 1 ? -2 : 0;
    for (const Text& l : lines) w = std::max(w, font.width(l)), h += 10;
    float x = mx + 12, y = my - 12;
    if (x + float(w) > float(c.guiWidth())) x = std::max(x - 24 - float(w), 4.0f);
    if (y + float(h) + 3 > float(c.guiHeight())) y = float(c.guiHeight() - h - 3);
    const float x0 = x - 12, y0 = y - 12, pw = float(w + 24), ph = float(h + 24);
    blitSprite(c, sprites.sprite("tooltip/background"), x0, y0, pw, ph);
    blitSprite(c, sprites.sprite("tooltip/frame"), x0, y0, pw, ph);
    float ly = y;
    for (size_t i = 0; i < lines.size(); ++i) {
        font.draw(c, lines[i], x, ly, kWhite, true);
        ly += 10 + (i == 0 ? 2 : 0);
    }
}

// --- HUD ----------------------------------------------------------------------------------------------------

void Ui::State::drawHud(Canvas& c) {
    const float W = float(c.guiWidth()), H = float(c.guiHeight());
    const int gw = c.guiWidth(), gh = c.guiHeight();
    drawCameraOverlays(*this, c);  // gameplay_hud.cpp: spyglass scope
    // Crosshair (inverting blend), first person only (Hud.extractCrosshair).
    if (!spectator() && screen == Screen::None && game->cameraMode == 0) {
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
        if (!offhand.empty()) blitSprite(c, sprites.sprite("hud/hotbar_offhand_left"), float(center - 91 - 29), H - 23, 29, 24);
        for (int i = 0; i < 9; ++i) drawItem(c, inventory.slots[size_t(36 + i)], float(center - 90 + i * 20 + 2), H - 16 - 3);
        if (!offhand.empty()) drawItem(c, offhand, float(center - 91 - 26), H - 16 - 3);
        drawHotbarAttackIndicator(*this, c);  // gameplay_hud.cpp
    }
    // Status effect icons (Hud.extractEffects), top right: beneficial first row, the rest below.
    if (screen != Screen::Inventory) {
        static const char* kHarmful[] = {"slowness", "mining_fatigue", "instant_damage", "nausea", "blindness", "hunger",
                                         "weakness", "poison", "wither", "levitation", "unluck", "darkness", "wind_charged",
                                         "weaving", "oozing", "infested", "bad_omen", "trial_omen", "raid_omen", "glowing"};
        int good = 0, bad = 0;
        for (auto it = effects.rbegin(); it != effects.rend(); ++it) {
            const Effect& e = it->second;
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
        Jitter rnd(u64(guiTicks) * 312871ULL);
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
        const std::string heartType = hasEffect("poison") ? "poisoned" : hasEffect("wither") ? "withered" : "";
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
                if (ah < absorb) heart("absorbing", ah + 1 == absorb, false);
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
        const CameraState cam = game->camera();
        const u16 eye = world->block(int(std::floor(cam.x)), int(std::floor(cam.y)), int(std::floor(cam.z)));
        const bool underWater = registry->name(eye).starts_with("minecraft:water");
        if (underWater || cur < maxA) {
            const int yAir = yBase - 20;
            auto bubble = [&](int ticks, int offset) { return int(std::ceil(float((ticks + offset) * 10) / float(maxA))); };
            const int full = bubble(cur, -2), popping = bubble(cur, 0);
            const int empty = 10 - bubble(cur, cur != 0 && underWater ? 1 : 0);
            for (int b = 1; b <= 10; ++b) {
                const float bx = float(xRight - (b - 1) * 8 - 9);
                if (b <= full) blitSprite(c, sprites.sprite("hud/air"), bx, float(yAir), 9, 9);
                else if (full != popping && b == popping && underWater) blitSprite(c, sprites.sprite("hud/air_bursting"), bx, float(yAir), 9, 9);
                else if (b > 10 - empty) blitSprite(c, sprites.sprite("hud/air_empty"), bx, float(yAir), 9, 9);
            }
        }
        // Experience bar and level.
        const int left = (gw - 182) / 2, top = gh - 24 - 5;
        if (!drawJumpBar(*this, c, left, top)) {  // a jumping mount's charge bar (gameplay_hud.cpp)
            blitSprite(c, sprites.sprite("hud/experience_bar_background"), float(left), float(top), 182, 5);
            const int progress = int(xpProgress * 183.0f);
            if (progress > 0) blitSpriteRegion(c, sprites.sprite("hud/experience_bar_progress"), 182, 5, 0, 0, float(left), float(top), float(progress), 5);
        }
        if (xpLevel > 0) {
            const Text lv = translatable("gui.experience.level", {Text(std::to_string(xpLevel))}, lang());
            const float x = float((gw - font.width(lv)) / 2), y = float(gh - 24 - 9 - 2);
            for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) font.draw(c, lv, x + float(dx), y + float(dy), 0xFF000000, false);
            font.draw(c, lv, x, y, 0xFF80FF20, false);
        }
    }
    // Selected item name.
    if (toolHighlightTimer > 0 && !lastToolHighlight.empty() && !spectator()) {
        const Text name = items.hoverName(lastToolHighlight);
        const int w = font.width(name);
        int y = gh - 59;
        if (!hurtable) y += 14;
        const int alpha = std::min(255, int(float(toolHighlightTimer) * 256.0f / 10.0f));
        if (alpha > 0) font.draw(c, name, float((gw - w) / 2), float(y), u32(alpha) << 24 | 0xFFFFFF, true);
    }
    // Action bar.
    if (actionbarTime > 0) {
        const float t = float(actionbarTime) - partialTick;
        const int alpha = std::min(255, int(t * 255.0f / 20.0f));
        if (alpha > 0) {
            const int w = font.width(actionbar);
            font.draw(c, actionbar, float(gw / 2 - w / 2), float(gh - 68 - 4), u32(alpha) << 24 | 0xFFFFFF, true);
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
            font.draw(c, title, float(-tw / 2), -10, color, true);
            c.pop();
            if (hasSubtitle) {
                c.push();
                c.scaleBy(2);
                const int sw = font.width(subtitle);
                font.draw(c, subtitle, float(-sw / 2), 5, color, true);
                c.pop();
            }
            c.pop();
        }
    }
    for (const UiModule& m : uiModules())  // modules.h: boss bars, scoreboard...
        if (m.drawHud) m.drawHud(*this, c);
    if (screen != Screen::Chat) drawChat(c, false);
    for (const UiModule& m : uiModules())  // over the chat: player list
        if (m.drawHudTop) m.drawHudTop(*this, c);
    (void)W;
}

// --- chat ---------------------------------------------------------------------------------------------------

void Ui::State::drawChat(Canvas& c, bool focused) {
    if (chatLines.empty()) return;
    const int maxWidth = 320, entryHeight = 9;
    const int chatBottom = c.guiHeight() - 40;
    const int linesPerPage = (focused ? 180 : 90) / 9;
    const float textOpacity = 1.0f * 0.9f + 0.1f, bgOpacity = 0.5f;
    c.push();
    c.translate(4, 0);
    const int total = int(chatLines.size());
    int count = 0;
    for (int i = std::min(total - chatScroll, linesPerPage) - 1; i >= 0; --i) {
        const ChatLine& line = chatLines[size_t(i + chatScroll)];
        float alpha = 1;
        if (!focused) {
            double t = double(guiTicks - line.addedTick) / 200.0;
            t = std::clamp((1.0 - t) * 10.0, 0.0, 1.0);
            alpha = float(t * t);
        }
        if (alpha <= 1e-5f) continue;
        ++count;
        const int entryBottom = chatBottom - i * entryHeight, entryTop = entryBottom - entryHeight;
        c.fill(-4, float(entryTop), float(maxWidth + 4 + 4), float(entryBottom), withAlpha(0, alpha * bgOpacity));
        const int textTop = entryBottom - 8;
        font.draw(c, line.text, 0, float(textTop), withAlpha(0xFFFFFF, alpha * textOpacity), true);
    }
    if (focused && total > 0) {  // scroll bar
        const int chatHeight = count * entryHeight, virtualHeight = total * entryHeight;
        if (virtualHeight != chatHeight && total > 0) {
            const int y = chatScroll * chatHeight / total - chatBottom;
            const int h = chatHeight * chatHeight / virtualHeight;
            const int a = y > 0 ? 170 : 96;
            c.fill(float(maxWidth + 4), float(-y), float(maxWidth + 6), float(-y - h), u32(a) << 24 | 0x3333AA);
            c.fill(float(maxWidth + 6), float(-y), float(maxWidth + 5), float(-y - h), u32(a) << 24 | 0xCCCCCC);
        }
    }
    c.pop();
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
        std::string v = f.value.substr(0, a) + clean + f.value.substr(b);
        if (int(v.size()) > f.maxLength) {
            const size_t room = size_t(std::max(0, f.maxLength - int(f.value.size() - (b - a))));
            std::string cut;
            for (size_t i = 0; i < clean.size();) {
                size_t j = i;
                nextCodepoint(clean, j);
                if (cut.size() + (j - i) > room) break;
                cut += clean.substr(i, j - i);
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

void Ui::State::drawTextField(Canvas& c, TextField& f, float x, float y, float w, u32 color, bool focused, bool shadow) {
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
    float endX = &f == &chatInput && chatFormatter && chatFormatter(visible, f.scroll, formatted)
                     ? font.draw(c, formatted, x, y, color, shadow)
                     : font.draw(c, visible, x, y, color, shadow);
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

// --- F3 -----------------------------------------------------------------------------------------------------

void Ui::State::drawDebug(Canvas& c) {
    const CameraState cam = game->camera();
    const Player& p = game->player();
    std::vector<std::string> left, right;
    char buf[512];
    auto add = [&](std::vector<std::string>& v, const char* fmt, auto... args) {
        std::snprintf(buf, sizeof(buf), fmt, args...);
        v.emplace_back(buf);
    };
    add(left, "minecraftCplusplus (26.3%s)", online() ? ", multiplayer" : ", local world");
    const RenderStats& rs = renderer->stats();
    add(left, "%d fps T: %.1f ms frame, GPU %.2f ms", int(fps + 0.5), frameMs, rs.gpuMs);
    const TerrainStats& ts = terrain->stats();
    add(left, "C: %zu/%zu sections, %zu nodes, %u draws", ts.detailResident, ts.detailPending, ts.active,
        rs.commands[0] + rs.commands[1] + rs.commands[2] + rs.commands[3]);
    add(left, "F: %zu/%zu far regions, %.0f/%.0f MB", ts.farRegions, ts.farPending,
        double(renderer->arenaBytesUsed()) / (1 << 20), double(renderer->arenaBytesTotal()) / (1 << 20));
    static const char* const kDifficulty[4] = {"peaceful", "easy", "normal", "hard"};
    if (session)
        add(left, "Server: game mode %d, entity %d, %s%s", gameMode(), session->entityId(), kDifficulty[difficulty & 3],
            difficultyLocked ? " (locked)" : "");
    // Reduced debug info (the server's gamerule): only the entries DebugScreenEntry.isAllowed keeps, no positions.
    if (!reducedDebugInfo) {
        left.emplace_back("");
        add(left, "XYZ: %.3f / %.5f / %.3f", p.pos[0], p.pos[1], p.pos[2]);
        const int bx = int(std::floor(p.pos[0])), by = int(std::floor(p.pos[1])), bz = int(std::floor(p.pos[2]));
        add(left, "Block: %d %d %d [%d %d %d]", bx, by, bz, bx & 15, by & 15, bz & 15);
        add(left, "Chunk: %d %d %d [%d %d in r.%d.%d.mca]", bx >> 4, by >> 4, bz >> 4, (bx >> 4) & 31, (bz >> 4) & 31, bx >> 9, bz >> 9);
        float yaw = std::fmod(p.yaw * 180.0f / 3.14159265f, 360.0f);
        if (yaw >= 180) yaw -= 360;
        if (yaw < -180) yaw += 360;
        const int dir = int(std::floor(yaw / 90.0f + 0.5f)) & 3;
        add(left, "Facing: %s (%s) (%.1f / %.1f)", kDirNames[dir], kTowards[dir], yaw, p.pitch * 180.0f / 3.14159265f);
        {
            std::shared_lock lock(world->mutex());
            const int ey = int(std::floor(cam.y));
            const World::LightView lv = world->light(bx >> 4, ey >> 4, bz >> 4);
            const u8 l = lv.at(bx & 15, ey & 15, bz & 15);
            add(left, "Light: %d (%d sky, %d block)", std::max(l >> 4, l & 15), l >> 4, l & 15);
        }
        if (biomeLookup) add(left, "Biome: %s", biomeLookup(bx, int(std::floor(cam.y)), bz).c_str());
        add(left, "%s%s", p.flying ? "Flying" : p.inWater ? "Swimming" : p.onGround ? "On ground" : "Airborne",
            p.sneaking ? ", sneaking" : "");
    }

    MEMORYSTATUSEX mem{sizeof(mem)};
    GlobalMemoryStatusEx(&mem);
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    add(right, "C++20 MSVC %d 64bit", _MSC_VER);
    add(right, "Mem: %.0f%% %llu/%lluMB", 100.0 * double(pmc.PrivateUsage) / double(mem.ullTotalPhys),
        (unsigned long long)(pmc.PrivateUsage >> 20), (unsigned long long)(mem.ullTotalPhys >> 20));
    add(right, "Working set: %lluMB", (unsigned long long)(pmc.WorkingSetSize >> 20));
    add(right, "CPU: %u threads", std::thread::hardware_concurrency());
    right.emplace_back("");
    add(right, "Display: %ux%u (%s)", renderer->width(), renderer->height(), renderer->adapterName().c_str());
    add(right, "Direct3D 12, GUI scale %d", c.scale());
    const RayHit& t = game->target();
    if (t.hit && !reducedDebugInfo) {
        right.emplace_back("");
        add(right, "\xC2\xA7nTargeted Block: %d, %d, %d", t.x, t.y, t.z);
        const std::string& state = registry->name(world->block(t.x, t.y, t.z));
        const size_t b = state.find('[');
        right.push_back(state.substr(0, b));
        if (b != std::string::npos) {
            std::string props = state.substr(b + 1, state.size() - b - 2);
            size_t start = 0;
            while (start < props.size()) {
                size_t comma = props.find(',', start);
                if (comma == std::string::npos) comma = props.size();
                std::string kv = props.substr(start, comma - start);
                const size_t eq = kv.find('=');
                std::string value = kv.substr(eq + 1);
                if (value == "true") value = "\xC2\xA7" "atrue";
                else if (value == "false") value = "\xC2\xA7" "cfalse";
                right.push_back(kv.substr(0, eq) + ": " + value);
                start = comma + 1;
            }
        }
    }
    auto lines = [&](const std::vector<std::string>& v, bool alignRight) {
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i].empty()) continue;
            const int w = font.width(v[i]);
            const float top = float(2 + 9 * int(i));
            const float x = alignRight ? float(c.guiWidth() - 2 - w) : 2.0f;
            c.fill(x - 1, top - 1, x + float(w) + 1, top + 9 - 1, 0x90505050);
            font.draw(c, v[i], x, top, 0xFFE0E0E0, false);
        }
    };
    lines(left, false);
    lines(right, true);
}

}  // namespace ui
