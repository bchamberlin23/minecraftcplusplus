#include "ui/modules.h"

#include "ui/ui_state.h"

#include <unordered_map>

namespace ui {

namespace {
// Function-local statics: registrars in other files run during static initialization in any order.
std::vector<UiModule>& modules() {
    static std::vector<UiModule> v;
    return v;
}
std::vector<ScreenDef>& screens() {
    static std::vector<ScreenDef> v;
    return v;
}
std::unordered_map<std::string, StationDef>& stations() {
    static std::unordered_map<std::string, StationDef> m;
    return m;
}
}  // namespace

UiModuleRegistrar::UiModuleRegistrar(const UiModule& m) { modules().push_back(m); }
const std::vector<UiModule>& uiModules() { return modules(); }

ScreenRegistrar::ScreenRegistrar(const ScreenDef& d) { screens().push_back(d); }
const ScreenDef* screenDef(Screen s) {
    for (const ScreenDef& d : screens())
        if (d.screen == s) return &d;
    return nullptr;
}

StationRegistrar::StationRegistrar(std::initializer_list<const char*> types, const StationDef& def) {
    for (const char* t : types) stations()[t] = def;
}
const StationDef* stationFor(const std::string& menuType) {
    auto it = stations().find(menuType);
    return it == stations().end() ? nullptr : &it->second;
}

void sendContainerButton(UiState& s, int buttonId) {
    if (!s.online() || !s.container) return;
    net::Writer w;
    w.varInt(s.container->containerId);
    w.varInt(buttonId);
    s.client->sendPlay("container_button_click", w);
}

}  // namespace ui
