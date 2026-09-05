//
// TestNetworkQoS.cpp
// AES67 macOS Driver
//
// The DSCP marking, checked on a real socket: setsockopt either took or it
// did not, and reading the option back is the only way to know. It matters
// because the value is what a switch sorts by — an audio stream or a PTP
// message that leaves unmarked shares a queue with everything else on the
// segment, which is exactly the case the marking exists for.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/NetworkUtils.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace AES67;

namespace {

int tosOf(int sock) {
    int tos = -1;
    socklen_t len = sizeof(tos);
    if (getsockopt(sock, IPPROTO_IP, IP_TOS, &tos, &len) != 0) {
        return -1;
    }
    return tos;
}

} // namespace

TEST_CASE("The DSCP Reaches The Socket") {
    const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(sock >= 0);

    // A fresh socket is unmarked, which is what everything here sends
    // until it is told otherwise.
    CHECK(tosOf(sock) == 0);

    // EF, what the AES67 guides mark time-critical traffic with. The TOS
    // octet carries the DSCP in its top six bits, so 46 arrives as 184.
    REQUIRE(NetworkUtils::setQoSTrafficClass(sock, 46));
    CHECK(tosOf(sock) == (46 << 2));

    // CS7, which is what Dante marks PTP with.
    REQUIRE(NetworkUtils::setQoSTrafficClass(sock, 56));
    CHECK(tosOf(sock) == (56 << 2));

    // Six bits and no more: the two below them are ECN and are not ours.
    REQUIRE(NetworkUtils::setQoSTrafficClass(sock, 0xff));
    CHECK(tosOf(sock) == (0x3f << 2));

    ::close(sock);
}
