#ifndef MULTICAST_REJOINER_H
#define MULTICAST_REJOINER_H

//
// MulticastRejoiner
// AES67 macOS Driver
//
// Keeps multicast group memberships alive across a network-interface flap
// (cable unplug/replug). A socket joins a group once with IP_ADD_MEMBERSHIP,
// but when the interface goes down and comes back the kernel may drop that
// membership, leaving the socket bound but no longer receiving the group —
// so audio, PTP, SAP or RTCP reception stays silent after a replug even though
// the socket is fine. Re-issuing IP_ADD_MEMBERSHIP periodically re-establishes
// it: re-joining a group that is still joined simply returns EADDRINUSE and is
// harmless, while re-joining after a flap brings reception back on its own.
//
// A component registers each (socket, group, interface) it joined and calls
// maybeRejoin() from the loop it already runs; the rejoiner only acts every
// kInterval, so the cost is one setsockopt per membership every few seconds.
// Header-only and dependency-free so any socket-owning component can use it.
//

#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <vector>

namespace AES67 {

class MulticastRejoiner {
public:
    // How often to re-issue the joins. A few seconds is frequent enough to
    // recover reception shortly after a replug, and cheap (one setsockopt per
    // membership per interval).
    static constexpr std::chrono::seconds kInterval{5};

    // Register a membership to keep alive. group is a dotted-quad multicast
    // address; ifAddr is the interface's IPv4 in network byte order (or
    // htonl(INADDR_ANY) for the default interface) — the same values passed to
    // the original IP_ADD_MEMBERSHIP.
    void add(int fd, const std::string& group, in_addr ifAddr) {
        Membership m;
        m.fd = fd;
        m.groupAddr = ::inet_addr(group.c_str());
        m.ifAddr = ifAddr;
        memberships_.push_back(m);
    }

    // Drop every registered membership (e.g. before the sockets close).
    void clear() { memberships_.clear(); }

    // Re-issue IP_ADD_MEMBERSHIP for every registered membership, but only
    // once per kInterval. Call it freely from a receive/select loop.
    void maybeRejoin(std::chrono::steady_clock::time_point now) {
        if (memberships_.empty()) return;
        if (lastRejoin_.time_since_epoch().count() != 0 &&
            now - lastRejoin_ < kInterval) {
            return;
        }
        lastRejoin_ = now;
        for (const auto& m : memberships_) {
            if (m.fd < 0) continue;
            ip_mreq mreq{};
            mreq.imr_multiaddr.s_addr = m.groupAddr;
            mreq.imr_interface = m.ifAddr;
            // Ignore the result: EADDRINUSE means "still joined" (fine), and a
            // transient failure while the link is down is retried next time.
            ::setsockopt(m.fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
        }
    }

private:
    struct Membership {
        int fd{-1};
        in_addr_t groupAddr{0};
        in_addr ifAddr{};
    };
    std::vector<Membership> memberships_;
    std::chrono::steady_clock::time_point lastRejoin_{};
};

} // namespace AES67

#endif // MULTICAST_REJOINER_H
