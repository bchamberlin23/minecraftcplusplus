#pragma once
// Main-thread side of a server connection: applies decoded events to the World, Terrain and the
// player once per frame (chunks within a time budget), sends the player's movement every tick and
// turns the player's block edits into the game's predicted actions (sequence numbers + acks).
#include "game/game.h"
#include "game/player.h"
#include "net/client.h"
#include "world/lighting.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>

class Terrain;

namespace net {

class Session {
public:
    // ClientPacketListener's LevelLoadTracker: the new level is not ready yet (the loading screen shows for a respawn)
    bool levelLoading() const { return loadState_ != 0; }
    bool levelLoadingRespawn() const { return loadRespawn_; }
    int levelLoadingReason() const { return loadReason_; }  // LevelLoadingScreen.Reason: 0 other, 1 nether, 2 end
    bool tracksChunk(int cx, int cz) const { return chunks_.count(World::chunkKey(cx, cz)) != 0; }  // loaded by the server
    Session(Client& client, World& world, BlockRegistry& registry, const GameReports& reports);

    // Before the window opens: waits for the login and the first position from the server. With `deferred`, returns
    // early (false, *deferred set) when the configuration needs the main thread (Client::pollCommon: pack and code of
    // conduct prompts); `patient`: the timeout only runs once the configuration is over (the player answers prompts).
    bool join(double timeoutSeconds, PlayerStart& start, Dimension& dimension, bool* deferred = nullptr,
              bool patient = false);
    // Starts applying the connection to the game (installs Game::onEdit).
    void attach(Game& game, Terrain& terrain);
    // Call once per frame before the game update.
    void frame(double dt);
    bool connected() const { return client_.alive(); }
    const std::string& disconnectReason() const { return disconnectReason_; }  // why the server closed the connection
    // Keep chunks the server unloads (they stay visible at a distance, like the rest of the save).
    void setKeepForgotten(bool keep) { keepForgotten_ = keep; }
    int entityId() const { return entityId_; }
    bool creativeHotbarSync = true;  // false once the UI owns the (server-driven) inventory
    void report() const;  // timings
    // The held item's use (food, bows, buckets...) and, unpredicted, a use on a block with the off hand.
    void useItem(int hand);
    // Block changes the game's client makes itself (piston moves, blockentity/block_entities.h); the server's updates
    // follow. Main thread, outside the world lock.
    void applyClientBlocks(const std::vector<BlockChange>& changes) { applyBlocks(changes, false); }
    int gameMode() const { return gameMode_; }                  // main thread
    const Dimension& dimension() const { return dimension_; }  // main thread

private:
    void handle(Event& e);
    void applyChunk(ChunkEvent& e);
    void forget(int cx, int cz, bool drop = false);  // drop: remove even when forgotten chunks are kept
    void clearWorld();                                // every chunk, also ones not from the server
    void setBlock(int x, int y, int z, u16 id, std::unordered_set<u64>& dirty);  // holds the world lock
    void applyBlocks(const std::vector<BlockChange>& changes, bool server = true);
    void applyLight(const LightEvent& e);
    void applyBiomes(const BiomesEvent& e);
    void acknowledge(int sequence);
    void teleport(const TeleportEvent& e);
    void onEdit(const Game::EditAction& a);
    void syncHotbar();
    void tick();
    void sendPosition();  // move_player_pos_rot with the current state
    int itemFor(u16 block) const;

    Client& client_;
    World& world_;
    BlockRegistry& registry_;
    Game* game_ = nullptr;
    Terrain* terrain_ = nullptr;
    Lighting lighting_;
    Physics physics_;
    std::deque<Event> pending_;
    std::vector<Event> incoming_;
    std::unordered_set<u64> chunks_;  // World::chunkKey of every column received
    struct Prediction {
        int sequence;
        u16 state;  // last state the server confirmed
    };
    std::unordered_map<u64, Prediction> predictions_;  // block positions edited locally, awaiting an ack
    std::unordered_map<std::string, int> items_;        // item name -> network id
    int sequence_ = 0, gameMode_ = 0, entityId_ = 0;
    int previousGameMode_ = -1;  // the login's previous game type, for the game when it attaches
    int portalCooldown_ = 0;  // the login's spawn info, for the player once attached
    bool instabuild_ = false;
    double tickClock_ = 0;
    double serverPos_[3] = {};  // last teleport, or last position over a loaded chunk
    double sentPos_[3] = {};
    float sentRot_[2] = {};
    int positionReminder_ = 0;  // LocalPlayer.positionReminder
    bool lastOnGround_ = false, lastHorizontal_ = false;
    bool haveServerPos_ = false, loadedSent_ = false;
    // LevelLoadTracker: 1 waiting for the server's chunks (LEVEL_CHUNKS_LOAD_START), 2 for the player's chunk, 0 ready
    // (or 30 s passed); the reason for the loading screen (0 other, 1 nether portal, 2 end portal) and a respawn's
    int loadState_ = 0, loadReason_ = 0;
    bool loadRespawn_ = false;
    double loadTimeout_ = 0;
    double levelStart_ = 0;
    int sentSlot_ = -1;
    int sentInput_ = 0;  // ServerboundPlayerInputPacket flags last sent
    bool sentSprinting_ = false, sentFlying_ = false;
    int sentItems_[9] = {-2, -2, -2, -2, -2, -2, -2, -2, -2};
    Dimension dimension_;
    double applyMs_ = 0, maxApplyMs_ = 0;
    u64 applied_ = 0, edits_ = 0, acks_ = 0, corrections_ = 0, serverChanges_ = 0;
    bool verbose_ = false;  // MCPP_NET_VERBOSE=1: log edits sent and block changes received
    bool keepForgotten_ = false;
    std::string disconnectReason_;
};

}  // namespace net
