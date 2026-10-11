#include "social/upnp.h"

#include <miniupnpc.h>
#include <upnpcommands.h>

namespace social {

UpnpMapping::~UpnpMapping() { stop(); }

void UpnpMapping::start(u16 udpPort) {
    if (running_ || udpPort == 0) return;
    if (thread_.joinable()) thread_.join();
    port_ = udpPort;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void UpnpMapping::stop() {
    {
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

std::string UpnpMapping::candidate() const {
    std::lock_guard lock(mutex_);
    return external_.empty() ? std::string() : candidateFor(external_, port_);
}

std::string UpnpMapping::candidateFor(const std::string& ip, u16 port) {
    // Server reflexive like a STUN-discovered address (type preference 100, component 1).
    return "a=candidate:upnp 1 UDP 1694498815 " + ip + " " + std::to_string(port) + " typ srflx raddr 0.0.0.0 rport 0";
}

std::string withCandidate(std::string sdp, const std::string& candidate) {
    const size_t end = sdp.find("a=end-of-candidates");
    if (end == std::string::npos && !sdp.empty() && sdp.back() != '\n') sdp += "\r\n";
    sdp.insert(end == std::string::npos ? sdp.size() : end, candidate + "\r\n");
    return sdp;
}

std::string UpnpMapping::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

void UpnpMapping::run() {
    auto setStatus = [&](std::string s) {
        LOG("friends: UPnP: %s", s.c_str());
        std::lock_guard lock(mutex_);
        status_ = std::move(s);
    };
    int error = 0;
    UPNPDev* devices = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
    UPNPUrls urls{};
    IGDdatas data{};
    char lan[64] = {}, wan[64] = {};
    const int igd = devices ? UPNP_GetValidIGD(devices, &urls, &data, lan, sizeof(lan), wan, sizeof(wan)) : 0;
    if (devices) freeUPNPDevlist(devices);
    if (igd != 1) {  // 1: a connected IGD; 2: behind another NAT (a mapping would not be reachable)
        setStatus(igd == 0 ? "no router with UPnP found" : "the router's UPnP cannot open a public port");
        if (igd) FreeUPNPUrls(&urls);
        running_ = false;
        return;
    }
    const std::string port = std::to_string(port_);
    char external[40] = {};
    bool mapped = false;
    while (running_) {
        const int rc = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, port.c_str(), port.c_str(), lan,
                                           "minecraftCplusplus friends", "UDP", nullptr, "3600");
        if (rc == UPNPCOMMAND_SUCCESS && UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, external) ==
                                             UPNPCOMMAND_SUCCESS && external[0]) {
            if (!mapped) setStatus(std::string("UDP port ") + port + " mapped to " + external);
            mapped = true;
            std::lock_guard lock(mutex_);
            external_ = external;
        } else if (!mapped) {
            setStatus("the router refused the port mapping (" + std::to_string(rc) + ")");
            break;
        }
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, std::chrono::minutes(30), [this] { return !running_.load(); });  // renew before the lease ends
    }
    if (mapped) {
        UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port.c_str(), "UDP", nullptr);
        LOG("friends: UPnP: mapping removed");
    }
    {
        std::lock_guard lock(mutex_);
        external_.clear();
    }
    FreeUPNPUrls(&urls);
    running_ = false;
}

}  // namespace social
