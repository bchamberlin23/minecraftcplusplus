#pragma once
// UPnP-IGD port mapping (miniupnpc) for the friends system's UDP port while hosting: on routers that allow it, friends
// reach the host even through NATs that hole punching alone cannot open. The mapped address is offered to the friend
// as one more ICE candidate. The mapping has a lease (renewed while hosting) and is removed when hosting stops.
#include "core/common.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace social {

class UpnpMapping {
public:
    ~UpnpMapping();
    void start(u16 udpPort);  // in the background; does nothing when already running
    void stop();              // removes the mapping
    std::string candidate() const;  // "a=candidate:..." for the mapped address; empty until mapped
    static std::string candidateFor(const std::string& ip, u16 port);
    std::string status() const;     // for logs / the UI

private:
    void run();
    u16 port_ = 0;
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::string external_, status_ = "not started";
};

// An ICE description with one more candidate line (before its end-of-candidates marker).
std::string withCandidate(std::string sdp, const std::string& candidate);

}  // namespace social
