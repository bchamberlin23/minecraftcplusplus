#pragma once
// Nostr (NIP-01) relay client: the friends system's only network service besides the peers themselves. Public relays
// pass small signed JSON events between clients that subscribe to them; nobody needs an account.
//  - One WebSocket per relay (WinHTTP's native WebSocket API, TLS by the system), reconnecting with backoff.
//  - publish() queues an event for every relay. Each relay's queue is sent at a gentle pace (public relays rate limit
//    bursts); durable events wait for a relay that is down, ephemeral ones are dropped after a few seconds. Urgent
//    ones (connection setup) go first. Received events are deduplicated by id across relays.
//  - One subscription (a list of filters) per pool, sent again on every (re)connection and whenever it changes.
#include "core/common.h"
#include "social/crypto.h"

#include <json/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace social {

// An event signed by `author` (kind, tags, content) with its id (NIP-01).
nlohmann::json signEvent(const KeyPair& author, int kind, const nlohmann::json& tags, const std::string& content, i64 createdAt);
bool verifyEvent(const nlohmann::json& event);  // id and signature
i64 unixNow();

class RelayPool {
public:
    using EventFn = std::function<void(const nlohmann::json& event, const std::string& relay)>;
    RelayPool();
    ~RelayPool();
    void start(const std::vector<std::string>& urls, EventFn onEvent);
    void stop();
    // durable: delivered to each relay once it is connected (else only to the connected ones).
    void publish(const nlohmann::json& event, bool durable, bool urgent = false);
    void subscribe(const nlohmann::json& filters);
    void flush(double timeoutSeconds);  // waits until the connected relays have been sent everything queued  // array of NIP-01 filters; replaces the previous subscription
    int connected() const;
    int total() const;

private:
    struct Relay;
    void run(Relay& r);
    void sendLoop();  // paces every relay's queue
    bool deliver(const std::string& id);  // first time this event id is seen
    std::vector<std::unique_ptr<Relay>> relays_;
    EventFn onEvent_;
    std::atomic<bool> running_{false};
    std::thread sender_;
    std::mutex mutex_;  // subscription, seen ids
    nlohmann::json filters_;
    u64 subGeneration_ = 0;
    std::unordered_set<std::string> seen_;
    std::deque<std::string> seenOrder_;
};

}  // namespace social
