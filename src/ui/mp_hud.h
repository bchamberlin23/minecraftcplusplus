#pragma once
// Multiplayer HUD (mp_hud.cpp): player list (PlayerTabOverlay), boss bars (BossHealthOverlay), scoreboard sidebar;
// chat extras (chat_ext.cpp): command suggestions (CommandSuggestions) and chat click / hover events.
#include "ui/modules.h"
#include "ui/text.h"

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

// The player info list (ClientPacketListener.playerInfoMap) and the scoreboard's teams, main thread.
struct PlayerInfoView {
    std::string uuid;  // 16 bytes
    std::string name, skin;  // skin: texture path ("entity/player/wide/steve")
    int gameMode = 0;
    bool listed = false, chatSession = false, showHat = true;
};
bool findPlayerInfo(const std::string& uuid, PlayerInfoView& out);
std::vector<PlayerInfoView> playerInfos();
struct TeamView {
    std::string name;
    Text displayName;
    int color = -1;  // TeamColor id, -1 none
    std::vector<std::string> players;  // member names
};
std::vector<TeamView> teamViews();

// Chat screen (screens.cpp, Screen::Chat): input first (true: consumed), drawing after the input box.
bool chatExtInput(UiState& s, const InputEvent& e);
void chatExtDraw(UiState& s, Canvas& c);
// hud.cpp: the chat's geometry (scale, width, line spacing options): the style under a GUI point, and clicks on the
// delayed-message line / the restricted prompt (true: handled).
bool chatLineStyleAt(UiState& s, float mx, float my, Style& out);
bool chatSpecialClick(UiState& s, float mx, float my);
// ClientPacketListener.verifyCommand / SignableCommand against the server's command tree (the command without
// the slash).
enum class CommandCheck { NoIssues, ParseErrors, SignatureRequired, PermissionsRequired };
CommandCheck verifyCommand(const std::string& command);
bool commandHasSignedArguments(const std::string& command);

// chat_net.cpp: ClientPacketListener.sendChat / sendCommand (unsigned: no profile key) with the last-seen
// acknowledgements; the command without the slash.
void sendChatMessage(UiState& s, const std::string& text);
void sendChatCommand(UiState& s, const std::string& command);

}  // namespace ui
