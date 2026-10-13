#pragma once
// Multiplayer HUD (mp_hud.cpp): player list (PlayerTabOverlay), boss bars (BossHealthOverlay), scoreboard sidebar;
// chat extras (chat_ext.cpp): command suggestions (CommandSuggestions) and chat click / hover events.
#include "ui/modules.h"
#include "ui/text.h"
#include "ui/ui_state.h"

#include <string>
#include <vector>

namespace ui {

// Boss bar flags of the bars shown (BossHealthOverlay.shouldDarkenScreen / shouldCreateWorldFog / shouldPlayMusic):
// for the sky / fog / music code.
bool bossDarkenScreen();
bool bossWorldFog();
bool bossPlayMusic();
// The collision rule of a player's scoreboard team (Team.CollisionRule: 0 always, 1 never, 2 push other teams, 3 push
// own team); 0 without a team.
int teamCollisionRule(const std::string& member);
// The colour (TeamColor id 0..15) of a member's team (a player's name or an entity's UUID string), -1 none.
int teamColor(const std::string& member);

// The player info list (ClientPacketListener.playerInfoMap) and the scoreboard's teams, main thread.
struct PlayerInfoView {
    std::string uuid;  // 16 bytes
    std::string name, skin;  // skin: texture path ("entity/player/wide/steve")
    int gameMode = 0;
    bool listed = false, chatSession = false, showHat = true;
};
bool findPlayerInfo(const std::string& uuid, PlayerInfoView& out);
std::vector<PlayerInfoView> playerInfos();
std::vector<PlayerInfoView> seenPlayers();  // ClientPacketListener.getSeenPlayers: every player seen this session
// Social Interactions (social_screen.cpp): players whose chat is hidden this session (PlayerSocialManager), by UUID.
bool socialHidden(const std::string& uuid);
void drawScoreboardSidebar(UiState& s, Canvas& c);  // the sidebar objective (Hud.extractScoreboardSidebar)
bool sentSignedChat(const std::string& uuid);  // chat_net.cpp: a signed chat message of theirs this session
// Entity name tags: PlayerTeam.formatNameForTeam by the team member (a player's name, other entities' UUID text), and
// Entity.belowNameDisplay (the BELOW_NAME objective's score: "<value> <objective name>"); false without one.
Text teamFormattedName(const std::string& member, const Text& name);
bool belowNameText(const std::string& owner, Text& out);
bool seesInvisible(const std::string& us, const std::string& them);  // the same team, seeing friendly invisibles
// LivingEntityRenderer.shouldShowName's team part (`them` a player name or an entity UUID): -1 when `them` is in no
// team, else whether its name tag shows (visible: not invisible to us).
int teamNameTagShown(const std::string& us, const std::string& them, bool visible);
void openSocialInteractions(UiState& s, Screen back);
struct TeamView {
    std::string name;
    Text displayName;
    int color = -1;  // TeamColor id, -1 none
    std::vector<std::string> players;  // member names
};
std::vector<TeamView> teamViews();

// Chat screen (screens.cpp, Screen::Chat): input first (true: consumed), drawing after the input box.
bool chatExtInput(UiState& s, const InputEvent& e);
bool chatInputAllowed(UiState& s);  // CommandSuggestions.hasAllowedInput (the Chat option's restrictions)
void chatExtDraw(UiState& s, Canvas& c);
// The command block editors' command field (AbstractCommandBlockEditScreen's CommandSuggestions): the field's
// text box; input first (true: consumed), drawing after the widgets.
bool commandSuggestionsInput(UiState& s, UiState::TextField& field, float x, float y, float w, float h, const InputEvent& e);
void commandSuggestionsDraw(UiState& s, UiState::TextField& field, float x, float y, float w, float h, Canvas& c);
void commandFieldFormatter(UiState& s, UiState::TextField* field);  // the field's command colours (nullptr: off)
// hud.cpp: the chat's geometry (scale, width, line spacing options): the style under a GUI point, and clicks on the
// delayed-message line / the restricted prompt (true: handled).
bool chatLineStyleAt(UiState& s, float mx, float my, Style& out);
bool chatSpecialClick(UiState& s, float mx, float my);
int chatSpecialAt(UiState& s, float mx, float my);  // hud.cpp: 1 the queue line, 2 the restricted prompt, 0 none
// ClientPacketListener.verifyCommand / SignableCommand against the server's command tree (the command without
// the slash).
enum class CommandCheck { NoIssues, ParseErrors, SignatureRequired, PermissionsRequired };
CommandCheck verifyCommand(const std::string& command);
bool commandHasSignedArguments(const std::string& command);
// GuiGraphicsExtractor.componentHoverEffect: the tooltip of a hover event (show_text / show_item / show_entity, the
// last only with advanced tooltips); empty: none.
std::vector<Text> hoverLines(UiState& s, const TextEvents& ev);

// chat_net.cpp: ClientPacketListener.sendChat / sendCommand (unsigned: no profile key) with the last-seen
// acknowledgements; the command without the slash.
void sendChatMessage(UiState& s, const std::string& text);
void sendChatCommand(UiState& s, const std::string& command);

}  // namespace ui
