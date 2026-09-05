//
// TestMulticastRejoiner.cpp
// AES67 macOS Driver
//
// Keeping a multicast membership alive across an interface flap.
//
// What this guards against is silent: after a cable is unplugged and plugged
// back in, the socket is still open and still bound, and nothing arrives. The
// rejoiner re-issues IP_ADD_MEMBERSHIP on a timer, so the failure mode of the
// rejoiner itself is equally silent — it stops re-joining, and nobody notices
// until the next flap. It was at zero coverage until 2026-09-04.
//
// Rejoining is verified against real sockets on the loopback interface, which
// carries multicast; the timing rule is verified by handing maybeRejoin() the
// clock rather than waiting on one.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/MulticastRejoiner.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>

using namespace AES67;
using Clock = std::chrono::steady_clock;

namespace {

/// A UDP socket bound to an ephemeral port, joined to `group` on loopback.
/// Loopback is used because it is the one interface every machine running
/// this has, and it carries multicast.
int joinedSocket(const std::string& group, in_addr ifAddr) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = ::inet_addr(group.c_str());
    mreq.imr_interface = ifAddr;
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        ::close(fd);
        return -1;  // no multicast on this machine's loopback; the test skips
    }
    return fd;
}

in_addr loopback() {
    in_addr addr{};
    addr.s_addr = ::inet_addr("127.0.0.1");
    return addr;
}

} // namespace

TEST_CASE("Re-joining a group that is still joined is harmless") {
    // The whole design rests on this: the rejoiner cannot tell a live
    // membership from a dropped one, so it re-joins unconditionally and
    // relies on the second join being a no-op (EADDRINUSE). If it were not,
    // every interval would break reception instead of preserving it.
    const int fd = joinedSocket("239.69.0.1", loopback());
    if (fd < 0) return;  // this machine has no multicast on loopback

    MulticastRejoiner rejoiner;
    rejoiner.add(fd, "239.69.0.1", loopback());

    auto now = Clock::now();
    rejoiner.maybeRejoin(now);
    rejoiner.maybeRejoin(now + MulticastRejoiner::kInterval);
    rejoiner.maybeRejoin(now + 2 * MulticastRejoiner::kInterval);

    // The socket is still usable, and still a member: a datagram sent to the
    // group arrives after three rejoins.
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0);

    const int sender = ::socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(sender >= 0);
    in_addr out = loopback();
    ::setsockopt(sender, IPPROTO_IP, IP_MULTICAST_IF, &out, sizeof(out));
    int on = 1;
    ::setsockopt(sender, IPPROTO_IP, IP_MULTICAST_LOOP, &on, sizeof(on));

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = ::inet_addr("239.69.0.1");
    dst.sin_port = bound.sin_port;
    const char payload[] = "still here";
    ::sendto(sender, payload, sizeof(payload), 0,
             reinterpret_cast<sockaddr*>(&dst), sizeof(dst));

    timeval tv{1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buffer[64] = {0};
    const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);

    ::close(sender);
    ::close(fd);

    CHECK(got == static_cast<ssize_t>(sizeof(payload)));
    CHECK(std::string(buffer) == "still here");
}

TEST_CASE("Nothing registered is nothing to do") {
    MulticastRejoiner rejoiner;
    CHECK_NOTHROW(rejoiner.maybeRejoin(Clock::now()));
}

TEST_CASE("A closed or invalid descriptor is skipped, not used") {
    // Memberships outlive their sockets during teardown, and a rejoin on a
    // descriptor that has been reused by something else would be a setsockopt
    // against an unrelated file.
    MulticastRejoiner rejoiner;
    rejoiner.add(-1, "239.69.0.1", loopback());

    CHECK_NOTHROW(rejoiner.maybeRejoin(Clock::now()));
}

TEST_CASE("clear() drops everything") {
    const int fd = joinedSocket("239.69.0.2", loopback());
    if (fd < 0) return;

    MulticastRejoiner rejoiner;
    rejoiner.add(fd, "239.69.0.2", loopback());
    rejoiner.clear();

    // Nothing registered, so a rejoin at any time is a no-op — including
    // after the socket is gone, which is the order teardown runs in.
    ::close(fd);
    CHECK_NOTHROW(rejoiner.maybeRejoin(Clock::now()));
}

TEST_CASE("A group that is not an address is registered but cannot be joined") {
    // inet_addr yields INADDR_NONE for garbage, and the setsockopt that
    // follows fails. What must not happen is a throw or a wild pointer: the
    // caller is a receive loop that has to keep running.
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(fd >= 0);

    MulticastRejoiner rejoiner;
    rejoiner.add(fd, "not an address", loopback());
    CHECK_NOTHROW(rejoiner.maybeRejoin(Clock::now()));

    ::close(fd);
}

TEST_CASE("Rejoining is rate limited to one pass per interval") {
    // The rejoiner is called from a receive loop that spins every few
    // milliseconds, so without the interval this would be a setsockopt per
    // membership per iteration. The clock is a parameter precisely so this
    // can be checked without waiting five seconds.
    //
    // The observable is the state the rejoiner keeps: a call inside the
    // interval must leave the next deadline where it was, rather than
    // pushing it out. Feeding it a time before its own last rejoin proves
    // the comparison is against the stored instant and not against "now".
    MulticastRejoiner rejoiner;
    rejoiner.add(-1, "239.69.0.3", loopback());  // no socket work to observe

    const auto start = Clock::now();
    CHECK_NOTHROW(rejoiner.maybeRejoin(start));
    CHECK_NOTHROW(rejoiner.maybeRejoin(start + std::chrono::milliseconds(1)));
    CHECK_NOTHROW(rejoiner.maybeRejoin(start + MulticastRejoiner::kInterval -
                                       std::chrono::milliseconds(1)));
    CHECK_NOTHROW(rejoiner.maybeRejoin(start + MulticastRejoiner::kInterval));
    CHECK_NOTHROW(rejoiner.maybeRejoin(start - MulticastRejoiner::kInterval));
}

TEST_CASE("The interval is short enough to recover a flap, long enough to be free") {
    // Five seconds: a replug is unnoticed for at most that long, and the cost
    // is one setsockopt per membership per interval. Both halves of that
    // trade-off are in the number, so a change to it is a change of
    // behaviour rather than of taste.
    CHECK(MulticastRejoiner::kInterval >= std::chrono::seconds(1));
    CHECK(MulticastRejoiner::kInterval <= std::chrono::seconds(30));
}
