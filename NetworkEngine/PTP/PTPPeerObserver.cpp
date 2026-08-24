#include "PTPPeerObserver.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

namespace AES67 {

namespace {
constexpr const char* kPTPMulticast = "224.0.1.129"; // default-domain PTP group
constexpr uint16_t kPTPEventPort = 319;
constexpr uint16_t kPTPGeneralPort = 320;
constexpr size_t kMaxPTPMessageSize = 1500;

// Resolve a named interface's IPv4 for multicast join/egress. Returns INADDR_ANY
// when the name is empty or not found.
in_addr interfaceAddr(const std::string& interfaceName) {
    in_addr any{};
    any.s_addr = htonl(INADDR_ANY);
    if (interfaceName.empty()) return any;

    struct ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return any;
    in_addr found = any;
    for (struct ifaddrs* ifa = list; ifa; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET &&
            interfaceName == ifa->ifa_name) {
            found = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr)->sin_addr;
            break;
        }
    }
    freeifaddrs(list);
    return found;
}

// Open a read-only UDP socket bound to `port`, joined to the PTP group on the
// chosen interface. Returns -1 on failure.
int openPTPSocket(uint16_t port, in_addr ifAddr) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    // A short receive timeout so the read loop can re-check the running flag.
    timeval tv{0, 250000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = ::inet_addr(kPTPMulticast);
    mreq.imr_interface = ifAddr;
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        // A join failure isn't fatal on its own — some interfaces still
        // deliver the group — but log it.
        std::cerr << "PTPPeerObserver: multicast join failed on port " << port << std::endl;
    }
    return fd;
}
} // namespace

class PTPPeerObserver::Impl {
public:
    ~Impl() { stop(); }

    bool start(const std::string& interfaceName) {
        if (running_.exchange(true)) return true;

        in_addr ifAddr = interfaceAddr(interfaceName);
        eventFd_ = openPTPSocket(kPTPEventPort, ifAddr);
        generalFd_ = openPTPSocket(kPTPGeneralPort, ifAddr);
        if (eventFd_ < 0 && generalFd_ < 0) {
            running_.store(false);
            closeSockets();
            return false;
        }

        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        // Break any blocking recv by shutting the sockets down before join.
        if (eventFd_ >= 0) ::shutdown(eventFd_, SHUT_RDWR);
        if (generalFd_ >= 0) ::shutdown(generalFd_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        closeSockets();
    }

    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    std::vector<PTPPeerObservation> peers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        table_.sweep(std::chrono::steady_clock::now());
        return table_.peers();
    }

private:
    void closeSockets() {
        if (eventFd_ >= 0) { ::close(eventFd_); eventFd_ = -1; }
        if (generalFd_ >= 0) { ::close(generalFd_); generalFd_ = -1; }
    }

    void run() {
        uint8_t buf[kMaxPTPMessageSize];
        while (running_.load(std::memory_order_acquire)) {
            fd_set rd;
            FD_ZERO(&rd);
            int maxFd = -1;
            for (int fd : {eventFd_, generalFd_}) {
                if (fd >= 0) { FD_SET(fd, &rd); if (fd > maxFd) maxFd = fd; }
            }
            if (maxFd < 0) break;

            timeval tv{0, 250000};
            int n = ::select(maxFd + 1, &rd, nullptr, nullptr, &tv);
            if (n <= 0) continue; // timeout or error — loop re-checks running_

            for (int fd : {eventFd_, generalFd_}) {
                if (fd >= 0 && FD_ISSET(fd, &rd)) handleReadable(fd, buf, sizeof(buf));
            }
        }
    }

    void handleReadable(int fd, uint8_t* buf, size_t bufLen) {
        sockaddr_in src{};
        socklen_t srcLen = sizeof(src);
        ssize_t got = ::recvfrom(fd, buf, bufLen, 0,
                                 reinterpret_cast<sockaddr*>(&src), &srcLen);
        if (got < 34) return; // shorter than a PTP header — ignore

        const uint8_t messageType = buf[0] & 0x0F;
        const int domain = buf[4];
        std::array<uint8_t, 8> clockId{};
        for (size_t i = 0; i < 8; ++i) clockId[i] = buf[20 + i];

        char ipStr[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &src.sin_addr, ipStr, sizeof(ipStr));

        std::lock_guard<std::mutex> lock(mutex_);
        table_.record(clockId, messageType, ipStr, domain,
                      std::chrono::steady_clock::now());
    }

    std::atomic<bool> running_{false};
    int eventFd_{-1};
    int generalFd_{-1};
    std::thread thread_;
    mutable std::mutex mutex_;
    mutable PTPPeerTable table_;
};

PTPPeerObserver::PTPPeerObserver() : pimpl_(std::make_unique<Impl>()) {}
PTPPeerObserver::~PTPPeerObserver() = default;

bool PTPPeerObserver::start(const std::string& interfaceName) {
    return pimpl_->start(interfaceName);
}
void PTPPeerObserver::stop() { pimpl_->stop(); }
bool PTPPeerObserver::isRunning() const { return pimpl_->isRunning(); }
std::vector<PTPPeerObservation> PTPPeerObserver::peers() const { return pimpl_->peers(); }

} // namespace AES67
