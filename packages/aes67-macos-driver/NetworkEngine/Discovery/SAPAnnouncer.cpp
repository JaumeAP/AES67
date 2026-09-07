#include "SAPAnnouncer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace AES67 {

namespace {
// RFC 2974 shared SAP port, same as SAPListener::kSapPort.
constexpr uint16_t kSapPort = 9875;

// Announce on both SAP groups so every receiver hears us: the RFC 2974
// SAPv2 global-scope address and the address AES67/Dante actually use (see
// SAPListener::initialize and Docs/comparison_ravenna_aes67_linux_driver.md).
const char* const kSapGroups[] = {"224.2.127.254", "239.255.255.255"};

// 16-bit Message ID Hash of an SDP body. RFC 2974 requires it be stable while
// the content is unchanged and differ when the content changes, so a plain
// content hash is exactly right: an edited SDP yields a new hash the receiver
// treats as a changed session, and we remember the hash per body so the
// matching deletion carries the value we announced with. FNV-1a folded to 16
// bits — deterministic, no clock or RNG (both unavailable/again-forbidden in
// this codebase's constraints and needless here).
uint16_t hashSdp(const std::string& sdp) {
    uint32_t h = 2166136261u;
    for (unsigned char c : sdp) {
        h ^= c;
        h *= 16777619u;
    }
    return static_cast<uint16_t>((h >> 16) ^ (h & 0xFFFF));
}
} // namespace

class SAPAnnouncer::Impl {
public:
    ~Impl() {
        // stop() joins the announce thread and closes the socket; both can
        // throw, and an exception escaping a destructor during unwinding ends
        // the process.
        try {
            stop();
        } catch (const std::exception& e) {
            std::cerr << "SAPAnnouncer: teardown threw: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "SAPAnnouncer: teardown threw a non-standard exception\n";
        }
    }

    bool initialize(const std::string& interfaceIp) {
        sockFd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sockFd_ < 0) {
            std::cerr << "SAPAnnouncer: failed to create socket" << '\n';
            return false;
        }

        // Originating source for the SAP header, and the multicast egress
        // interface, both taken from the bound local IPv4 when given.
        originatingSource_ = 0;
        if (!interfaceIp.empty()) {
            in_addr ifa{};
            if (::inet_pton(AF_INET, interfaceIp.c_str(), &ifa) == 1) {
                originatingSource_ = ifa.s_addr; // network byte order
                if (::setsockopt(sockFd_, IPPROTO_IP, IP_MULTICAST_IF, &ifa,
                                 sizeof(ifa)) < 0) {
                    // Non-fatal: kernel picks a default route interface.
                    std::cerr << "SAPAnnouncer: IP_MULTICAST_IF failed, using default"
                              << '\n';
                }
            }
        }

        // Keep announcements on the local segment like other SAP senders.
        unsigned char ttl = 32;
        ::setsockopt(sockFd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

        // Do not loop our own announcements back to a local SAPListener -
        // we must not discover our own transmit streams as receivable
        // sources.
        unsigned char loop = 0;
        ::setsockopt(sockFd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

        return true;
    }

    bool start(SessionProvider provider) {
        if (sockFd_ < 0 || !provider) return false;
        if (running_.exchange(true)) return true; // already running
        provider_ = std::move(provider);
        sender_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) {
            if (sockFd_ >= 0) { ::close(sockFd_); sockFd_ = -1; }
            return;
        }
        { std::lock_guard<std::mutex> lk(wakeMutex_); }
        wake_.notify_all();
        if (sender_.joinable()) sender_.join();

        // Withdraw everything we still had announced.
        for (const auto& kv : announced_) {
            sendPacket(kv.first, kv.second, /*deletion=*/true);
        }
        announced_.clear();

        if (sockFd_ >= 0) { ::close(sockFd_); sockFd_ = -1; }
    }

private:
    void run() {
        while (running_.load()) {
            refreshAndAnnounce();

            std::unique_lock<std::mutex> lk(wakeMutex_);
            wake_.wait_for(lk, kAnnounceInterval, [this] { return !running_.load(); });
        }
    }

    void refreshAndAnnounce() {
        std::vector<std::string> current = provider_ ? provider_() : std::vector<std::string>{};

        // Deletions: bodies we announced before and no longer have.
        std::unordered_map<std::string, uint16_t> stillPresent;
        stillPresent.reserve(current.size());
        for (const auto& sdp : current) {
            if (sdp.empty()) continue;
            auto it = announced_.find(sdp);
            uint16_t hash = (it != announced_.end()) ? it->second : hashSdp(sdp);
            stillPresent.emplace(sdp, hash);
        }
        for (const auto& kv : announced_) {
            if (stillPresent.find(kv.first) == stillPresent.end()) {
                sendPacket(kv.first, kv.second, /*deletion=*/true);
            }
        }
        announced_.swap(stillPresent);

        // Announce (repeat) everything currently present.
        for (const auto& kv : announced_) {
            sendPacket(kv.first, kv.second, /*deletion=*/false);
        }
    }

    void sendPacket(const std::string& sdp, uint16_t msgIdHash, bool deletion) {
        if (sockFd_ < 0) return;

        const std::vector<uint8_t> pkt =
            SAPAnnouncer::buildPacket(sdp, msgIdHash, originatingSource_, deletion);

        for (const char* group : kSapGroups) {
            sockaddr_in dst{};
            dst.sin_family = AF_INET;
            dst.sin_port = htons(kSapPort);
            dst.sin_addr.s_addr = ::inet_addr(group);
            ::sendto(sockFd_, pkt.data(), pkt.size(), 0,
                     reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        }
    }

    int sockFd_{-1};
    uint32_t originatingSource_{0}; // network byte order
    std::atomic<bool> running_{false};
    std::thread sender_;
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    SessionProvider provider_;
    // SDP body -> Message ID Hash we announced it with.
    std::unordered_map<std::string, uint16_t> announced_;
};

uint16_t SAPAnnouncer::messageIdHash(const std::string& sdp) { return hashSdp(sdp); }

std::vector<uint8_t> SAPAnnouncer::buildPacket(const std::string& sdp, uint16_t msgIdHash,
                                               uint32_t originatingSource, bool deletion) {
    // RFC 2974 header: V=1 (bits 5-7), A=0 IPv4, T=deletion bit (bit 2),
    // E=C=0. Auth length 0. Then Message ID Hash (2) + originating source
    // (4). No "application/sdp" payload-type prefix: it is optional and
    // defaults to SDP, and omitting it matches what this driver's own
    // SAPListener parser expects. The full SDP body follows, in both
    // announcement and deletion (RFC 2974 permits it in a deletion, and it
    // lets stricter receivers identify the withdrawn session).
    std::vector<uint8_t> pkt;
    pkt.reserve(8 + sdp.size());
    uint8_t b0 = (1u << 5); // version 1
    if (deletion) b0 |= (1u << 2);
    pkt.push_back(b0);
    pkt.push_back(0); // auth length
    pkt.push_back(static_cast<uint8_t>((msgIdHash >> 8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>(msgIdHash & 0xFF));
    const uint8_t* src = reinterpret_cast<const uint8_t*>(&originatingSource);
    pkt.insert(pkt.end(), src, src + 4); // already network byte order
    pkt.insert(pkt.end(), sdp.begin(), sdp.end());
    return pkt;
}

SAPAnnouncer::SAPAnnouncer() : pimpl_(std::make_unique<Impl>()) {}
SAPAnnouncer::~SAPAnnouncer() = default;

bool SAPAnnouncer::initialize(const std::string& interfaceIp) {
    return pimpl_->initialize(interfaceIp);
}
bool SAPAnnouncer::start(SessionProvider provider) {
    return pimpl_->start(std::move(provider));
}
void SAPAnnouncer::stop() { pimpl_->stop(); }

} // namespace AES67
