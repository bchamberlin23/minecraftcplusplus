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

}  // namespace ui
