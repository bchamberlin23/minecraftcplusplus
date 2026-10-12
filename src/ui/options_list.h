#pragma once
// The game's options screen layout (OptionsSubScreen: HeaderAndFooterLayout + OptionsList): the title in a 33 pixel
// header, a scrolled list of 310 pixel rows (two 150 pixel widgets 10 apart, or one 310 pixel widget; 25 high; bold
// underlined section headers) between the header / footer separators, and Done in a 33 pixel footer. Used by
// options_screens.cpp and video_screen.cpp; one list screen is open at a time.
#include "ui/ui_state.h"

namespace ui {

class OptionsList {
public:
    // Starts building the open screen's list (clears the buttons). Esc and Done go back to `back`.
    OptionsList(UiState& s, Text title, Screen back);
    void subtitle(Text text, u32 color);  // a second header line (VideoSettingsScreen's restart warning)
    void header(Text text);               // OptionsList.addHeader: bold, underlined, centred
    void big(Button b);                   // addBig: one 310 wide widget
    void half(Button a);                  // addSmall: a row with one 150 wide widget
    void half(Button a, Button b);
    void halves(std::vector<Button> v);   // two per row
    // The footer: Done (200 wide) by default, else these buttons (150 wide, 8 apart).
    void footer(std::vector<Button> v = {});

    // KeyBindsList: rows of `height` (20) in a `width` (340) wide list; widgets and labels at row-relative positions.
    void setRows(int width, int height);
    float addRow(int height = -1);  // the next row's top in list coordinates
    void rowButton(Button b, float rowTop, float yOffset);
    void rowLabel(Text text, float x, float rowTop, float yOffset, bool centred);

private:
    UiState& s_;
};

// The open list screen: draws / handles input for what OptionsList built.
void drawOptionsList(UiState& s, Canvas& c, const std::function<void(Canvas&, float originY)>& extra = {});
void optionsListInput(UiState& s, const Window::InputEvent& e);
void optionsListBack(UiState& s);  // Done / Esc: saves options.txt, back to the parent screen
// Widgets (CycleButton / OptionInstance slider): labels and tooltips follow the current values.
Text optionLabel(UiState& s, const char* captionKey, const Text& value);  // "Caption: value" (options.generic_value)
Button cycleButton(std::function<Text()> label, std::function<void()> next, std::function<Text()> tooltip = {});
Button onOffButton(UiState& s, const char* captionKey, bool* value, std::function<void()> changed = {},
                   const char* tooltipKey = nullptr);
Button sliderButton(float value, std::function<Text()> label, std::function<void(float)> slide);
Button screenButton(UiState& s, const char* labelKey, Screen target, bool active = true);
std::vector<Text> tooltipLines(UiState& s, const Text& text);  // split at new lines and 170 pixels
// OptionInstance widgets on a Settings field (each change saves options.txt, then runs `changed`):
std::function<Text()> tooltipKey(UiState& s, const char* key);  // a constant translated tooltip
Text percentLabel(UiState& s, const char* captionKey, double v);       // Options.percentValueLabel
Text percentOrOffLabel(UiState& s, const char* captionKey, double v);  // Options.percentValueOrOffLabel
// OptionInstance.Enum: "Caption: value", cycling through the values (their translation keys) in order.
Button enumButton(UiState& s, const char* captionKey, int* value, std::vector<const char*> valueKeys,
                  std::function<void()> changed = {}, std::function<Text()> tooltip = {});
// OptionInstance.IntRange slider (lo..hi inclusive, the game's slider mapping) over get / set.
Button rangeSlider(UiState& s, int lo, int hi, std::function<int()> get, std::function<void(int)> set, std::function<Text()> label);
// OptionInstance.UnitDouble slider (0..1), optionally through the square map (fovEffectScale, darknessEffectScale).
Button unitSlider(UiState& s, double* value, std::function<Text()> label, std::function<void()> changed = {}, bool squared = false);
void openConfirmLink(UiState& s, const std::string& url, Screen back, bool trusted = true);  // ConfirmLinkScreen (confirm_link.cpp)
void openUrlInBrowser(const std::string& url);  // Util.getPlatform().openUri

// Option effects that live elsewhere:
void chatRescale(UiState& s);          // ChatComponent.rescaleChat: re-wraps the chat at the new width / scale (hud.cpp)
int chatWidthPixels(double pct);       // ChatComponent.getWidth / getHeight
int chatHeightPixels(double pct);
bool highContrastAvailable(UiState& s);  // the game's high_contrast pack exists (mods_screen.cpp)
void applyHighContrast(UiState& s);      // enables / disables it after Options.highContrast
void reloadFonts(UiState& s);            // Force Unicode / Japanese glyph variants / language changed (ui.cpp)
void setLanguage(UiState& s, const std::string& code);  // LanguageManager.setSelected + reload (language_screen.cpp)

}  // namespace ui
