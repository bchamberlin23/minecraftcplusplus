// Item state the tooltips and stations read from the server: the player's permission level (entity events), tags
// updated in play (update_tags), recipe property sets (update_recipes; stations_a2.cpp decodes the packet), and the
// container screens' item tooltip with its image.
#include "ui/item_ui.h"

#include <unordered_map>

namespace ui {

bool mapInfo(int id, int& scale, bool& locked);           // stations_a2.cpp

namespace {
int g_permission = 0;
std::unordered_map<std::string, std::vector<int>> g_propertySets;  // RecipePropertySet key -> item ids (sorted)

void registerHandlers(UiState& s, net::Client& client) {
    UiState* sp = &s;
    client.onPlayPeek("entity_event", [sp](net::Reader& r) {  // EntityEvent.PERMISSION_LEVEL_ALL (24) .. OWNERS (28)
        const int id = r.i32v(), event = r.u8v();
        if (r.ok && event >= 24 && event <= 28) sp->post([sp, id, event] {
            if (sp->session && id == sp->session->entityId()) g_permission = event - 24;
        });
    });
    client.onPlay("update_tags", [sp](net::Reader& r) {  // /reload: the tags again
        auto tags = std::make_shared<std::unordered_map<std::string, net::Client::TagMap>>();
        if (net::Client::readTags(r, *tags)) sp->post([sp, tags] { sp->items.setTags(*tags); });
    });
}

void attached(UiState&) {
    g_permission = 0;
    g_propertySets.clear();
}

void tick(UiState& s) {
    Items::TooltipEnv& env = s.items.tooltipEnv;
    env.player = true;
    env.permissionLevel = g_permission;
    env.peaceful = s.difficulty == 0;
    if (!env.mapData) env.mapData = [](int id, int& scale, bool& locked) { return mapInfo(id, scale, locked); };
}

const UiModuleRegistrar kModule([] {
    UiModule m;
    m.name = "item_ui";
    m.registerHandlers = registerHandlers;
    m.attached = attached;
    m.tick = tick;
    return m;
}());
}  // namespace

int permissionLevel() { return g_permission; }

void setPropertySets(std::unordered_map<std::string, std::vector<int>> sets) {
    for (auto& [k, v] : sets) std::sort(v.begin(), v.end());
    g_propertySets = std::move(sets);
}

bool propertySetKnown(std::string_view set) { return g_propertySets.count(std::string(set)) != 0; }

bool inPropertySet(std::string_view set, int item) {
    auto it = g_propertySets.find(std::string(set));
    return it != g_propertySets.end() && std::binary_search(it->second.begin(), it->second.end(), item);
}

void drawItemTooltip(UiState& s, Canvas& c, const ItemStack& st, const std::vector<Text>& lines, float x, float y) {
    TooltipImage image;
    const bool hasImage = !lines.empty() && bundleTooltipImage(s, st, image);
    s.drawTooltip(c, lines, x, y, hasImage ? &image : nullptr);
}

}  // namespace ui
