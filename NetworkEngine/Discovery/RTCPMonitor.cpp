#include "RTCPMonitor.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace AES67 {

namespace {
constexpr size_t kMaxRTCPPacket = 1500;

in_addr interfaceAddr(const std::string& name) {
    in_addr any{};
    any.s_addr = htonl(INADDR_ANY);
    if (name.empty()) return any;
    struct ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return any;
    in_addr found = any;
    for (struct ifaddrs* ifa = list; ifa; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET && name == ifa->ifa_name) {
            found = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr)->sin_addr;
            break;
        }
    }
    freeifaddrs(list);
    return found;
}

// Read-only UDP socket bound to rtcpPort, joined to the stream's multicast
// group on the chosen interface. -1 on failure.
int openRTCPSocket(const std::string& group, uint16_t port, in_addr ifAddr) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = ::inet_addr(group.c_str());
    mreq.imr_interface = ifAddr;
    ::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)); // non-fatal
    return fd;
}
} // namespace

class RTCPMonitor::Impl {
public:
    ~Impl() { stop(); }

    bool start(EndpointProvider provider, const std::string& interfaceName) {
        if (!provider) return false;
        if (running_.exchange(true)) return true;
        provider_ = std::move(provider);
        ifAddr_ = interfaceAddr(interfaceName);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
        for (auto& kv : sockets_) ::close(kv.second);
        sockets_.clear();
    }

    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    std::vector<RTCPReporter> reporters() const {
        std::lock_guard<std::mutex> lock(mutex_);
        table_.sweep(std::chrono::steady_clock::now());
        return table_.reporters();
    }

private:
    struct EndpointKey {
        std::string ip;
        uint16_t port;
        bool operator<(const EndpointKey& o) const {
            return ip < o.ip || (ip == o.ip && port < o.port);
        }
    };

    void reconcile() {
        std::vector<Endpoint> want = provider_ ? provider_() : std::vector<Endpoint>{};
        std::map<EndpointKey, bool> wanted;
        for (const auto& e : want) {
            if (e.multicastIp.empty() || e.rtcpPort == 0) continue;
            wanted[{e.multicastIp, e.rtcpPort}] = true;
        }
        // Close sockets no longer wanted.
        for (auto it = sockets_.begin(); it != sockets_.end();) {
            if (wanted.find(it->first) == wanted.end()) {
                ::close(it->second);
                it = sockets_.erase(it);
            } else {
                ++it;
            }
        }
        // Open newly wanted ones.
        for (const auto& w : wanted) {
            if (sockets_.find(w.first) != sockets_.end()) continue;
            int fd = openRTCPSocket(w.first.ip, w.first.port, ifAddr_);
            if (fd >= 0) sockets_[w.first] = fd;
        }
        // Re-issue the multicast join on every current socket so a membership
        // dropped by an interface flap is re-established — reconcile already
        // runs every couple of seconds. Harmless (EADDRINUSE) when still joined.
        for (const auto& kv : sockets_) {
            ip_mreq mreq{};
            mreq.imr_multiaddr.s_addr = ::inet_addr(kv.first.ip.c_str());
            mreq.imr_interface = ifAddr_;
            ::setsockopt(kv.second, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
        }
    }

    void run() {
        uint8_t buf[kMaxRTCPPacket];
        int sinceReconcile = 1000; // force a reconcile on the first pass
        while (running_.load(std::memory_order_acquire)) {
            if (sinceReconcile >= 8) { reconcile(); sinceReconcile = 0; } // ~2 s at 250 ms
            ++sinceReconcile;

            fd_set rd;
            FD_ZERO(&rd);
            int maxFd = -1;
            for (auto& kv : sockets_) {
                FD_SET(kv.second, &rd);
                if (kv.second > maxFd) maxFd = kv.second;
            }
            timeval tv{0, 250000};
            if (maxFd < 0) { // nothing to listen on yet
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            int n = ::select(maxFd + 1, &rd, nullptr, nullptr, &tv);
            if (n <= 0) continue;
            for (auto& kv : sockets_) {
                if (FD_ISSET(kv.second, &rd)) handleReadable(kv.second, buf, sizeof(buf));
            }
        }
    }

    void handleReadable(int fd, uint8_t* buf, size_t bufLen) {
        sockaddr_in src{};
        socklen_t srcLen = sizeof(src);
        ssize_t got = ::recvfrom(fd, buf, bufLen, 0,
                                 reinterpret_cast<sockaddr*>(&src), &srcLen);
        if (got < 4) return;
        RTCPParseResult parsed = RTCPReceiverTable::parse(buf, static_cast<size_t>(got));
        if (parsed.reporterSSRCs.empty() && parsed.cnames.empty()) return;

        char ipStr[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &src.sin_addr, ipStr, sizeof(ipStr));
        const auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t ssrc : parsed.reporterSSRCs) {
            std::string cname;
            for (const auto& c : parsed.cnames) {
                if (c.first == ssrc) { cname = c.second; break; }
            }
            table_.record(ssrc, ipStr, cname, now);
        }
    }

    std::atomic<bool> running_{false};
    std::thread thread_;
    EndpointProvider provider_;
    in_addr ifAddr_{};
    std::map<EndpointKey, int> sockets_;
    mutable std::mutex mutex_;
    mutable RTCPReceiverTable table_;
};

RTCPMonitor::RTCPMonitor() : pimpl_(std::make_unique<Impl>()) {}
RTCPMonitor::~RTCPMonitor() = default;
bool RTCPMonitor::start(EndpointProvider provider, const std::string& interfaceName) {
    return pimpl_->start(std::move(provider), interfaceName);
}
void RTCPMonitor::stop() { pimpl_->stop(); }
bool RTCPMonitor::isRunning() const { return pimpl_->isRunning(); }
std::vector<RTCPReporter> RTCPMonitor::reporters() const { return pimpl_->reporters(); }

} // namespace AES67
