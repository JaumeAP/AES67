//
// TestNetworkUtils.cpp
// AES67 macOS Driver
//
// The socket-layer helpers this repository supplies to the platform-free
// core: address classification, interface resolution, and the QoS marking
// every transmit socket goes through.
//
// The core declares four of these without implementing them (see the README's
// account of the split), so what is checked here is the contract the other
// side relies on. It was at zero coverage until 2026-09-04, apart from the
// DSCP path, which TestNetworkQoS already covers.
//
// Anything that depends on this machine's actual interfaces is checked for
// its shape and its invariants, never for a particular name or address: a
// test that demands "en0" passes on one Mac and fails on the next.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/NetworkUtils.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <vector>

using namespace AES67;

TEST_CASE("A multicast address is one in 224.0.0.0/4") {
    // The bounds are the whole point: 223.255.255.255 and 240.0.0.0 are the
    // addresses either side of the range, and an off-by-one here would have
    // the driver join a group that is not one.
    CHECK(NetworkUtils::isValidMulticastAddress("224.0.0.0"));
    CHECK(NetworkUtils::isValidMulticastAddress("224.0.1.129"));  // the PTP group
    CHECK(NetworkUtils::isValidMulticastAddress("239.69.0.1"));   // an AES67 stream
    CHECK(NetworkUtils::isValidMulticastAddress("239.255.255.255"));

    CHECK_FALSE(NetworkUtils::isValidMulticastAddress("223.255.255.255"));
    CHECK_FALSE(NetworkUtils::isValidMulticastAddress("240.0.0.0"));
    CHECK_FALSE(NetworkUtils::isValidMulticastAddress("192.168.1.1"));
    CHECK_FALSE(NetworkUtils::isValidMulticastAddress("127.0.0.1"));
    CHECK_FALSE(NetworkUtils::isValidMulticastAddress("0.0.0.0"));
}

TEST_CASE("Something that is not an address is not a multicast one") {
    CHECK_FALSE(NetworkUtils::isValidMulticastAddress(""));
    CHECK_FALSE(NetworkUtils::isValidMulticastAddress("not an address"));
    CHECK_FALSE(NetworkUtils::isValidMulticastAddress("239.69.0"));
    CHECK_FALSE(NetworkUtils::isValidMulticastAddress("ff02::1"));  // IPv6, not handled
}

TEST_CASE("An IPv4 address is four dotted numbers, and nothing else") {
    CHECK(NetworkUtils::isIPv4Address("192.168.1.20"));
    CHECK(NetworkUtils::isIPv4Address("0.0.0.0"));
    CHECK(NetworkUtils::isIPv4Address("255.255.255.255"));

    CHECK_FALSE(NetworkUtils::isIPv4Address(""));
    CHECK_FALSE(NetworkUtils::isIPv4Address("en0"));
    CHECK_FALSE(NetworkUtils::isIPv4Address("192.168.1"));
    CHECK_FALSE(NetworkUtils::isIPv4Address("192.168.1.256"));
    CHECK_FALSE(NetworkUtils::isIPv4Address("192.168.1.20 "));
    CHECK_FALSE(NetworkUtils::isIPv4Address("::1"));

    // inet_pton, unlike inet_aton, refuses the shorthand and octal forms.
    // The distinction matters: an interface spec is either an address or a
    // name, and "0177.0.0.1" must not be read as the first.
    CHECK_FALSE(NetworkUtils::isIPv4Address("192.168.257"));
    CHECK_FALSE(NetworkUtils::isIPv4Address("0177.0.0.1"));
}

TEST_CASE("An interface spec that is already an address resolves to itself") {
    // The Config field accepts either form, and this is the branch that
    // keeps a literal address from being looked up as an interface name.
    CHECK(NetworkUtils::resolveInterfaceToIP("192.168.1.20") == "192.168.1.20");
    CHECK(NetworkUtils::resolveInterfaceToIP("127.0.0.1") == "127.0.0.1");
}

TEST_CASE("An interface name that does not exist resolves to nothing") {
    // Empty, not a guess: the caller binds the socket to whatever comes back
    // and an invented address would bind it somewhere wrong.
    CHECK(NetworkUtils::resolveInterfaceToIP("nosuchif0").empty());
    CHECK(NetworkUtils::getInterfaceIP("nosuchif0").empty());
}

TEST_CASE("Loopback resolves, whatever this machine is") {
    // The one interface every machine has. Which name it carries differs
    // (lo0 on macOS, lo on Linux), so both are tried and the address is what
    // is checked.
    std::string ip = NetworkUtils::getInterfaceIP("lo0");
    if (ip.empty()) ip = NetworkUtils::getInterfaceIP("lo");
    REQUIRE_FALSE(ip.empty());
    CHECK(ip == "127.0.0.1");
    CHECK(NetworkUtils::isIPv4Address(ip));
}

TEST_CASE("The interfaces this reports are real ones with real addresses") {
    const auto pairs = NetworkUtils::getActiveInterfacesWithIPs();
    const auto names = NetworkUtils::getNetworkInterfaces();

    // Every machine that can run this has at least loopback.
    REQUIRE_FALSE(pairs.empty());
    for (const auto& entry : pairs) {
        CHECK_FALSE(entry.first.empty());
        CHECK(NetworkUtils::isIPv4Address(entry.second));
        // An interface reported with an address is an interface.
        CHECK(NetworkUtils::getInterfaceIP(entry.first) == entry.second);
    }

    // The name-only list is the same set or a superset of it: an interface
    // with no IPv4 address still exists.
    for (const auto& entry : pairs) {
        bool found = false;
        for (const auto& name : names) {
            if (name == entry.first) { found = true; break; }
        }
        CHECK(found);
    }
}

TEST_CASE("The suggested route command names the interface it was asked about") {
    // It is printed for a human to paste into a shell, so what it names has
    // to be the interface that is actually short of a route.
    CHECK(NetworkUtils::getMulticastRouteCommand("en5").find("en5") != std::string::npos);
    CHECK(NetworkUtils::getMulticastRouteCommand("en5").find("239.0.0.0/8") !=
          std::string::npos);

    // Asked about nothing in particular, it falls back to the usual port
    // rather than producing a command with a hole in it.
    const std::string fallback = NetworkUtils::getMulticastRouteCommand("");
    CHECK(fallback.find("en0") != std::string::npos);
    CHECK(fallback.find("-interface ") != std::string::npos);
}

TEST_CASE("DSCP marking is applied to a socket, and refused on a bad one") {
    // The transmit path marks audio EF (46) so switches prioritise it; a
    // socket that silently stays unmarked is a stream that arrives late
    // under load, which is invisible until the network is busy.
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(fd >= 0);

    CHECK(NetworkUtils::setQoSTrafficClass(fd, 46));

    int tos = 0;
    socklen_t len = sizeof(tos);
    REQUIRE(::getsockopt(fd, IPPROTO_IP, IP_TOS, &tos, &len) == 0);
    CHECK(((tos >> 2) & 0x3F) == 46);

    ::close(fd);

    // A closed descriptor cannot be marked, and saying it was would hide a
    // stream going out unprioritised.
    CHECK_FALSE(NetworkUtils::setQoSTrafficClass(fd, 46));
    CHECK_FALSE(NetworkUtils::setQoSTrafficClass(-1, 46));
}

TEST_CASE("Joining a group that is not one fails") {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(fd >= 0);

    CHECK_FALSE(NetworkUtils::joinMulticastGroup(fd, "192.168.1.20", ""));
    CHECK_FALSE(NetworkUtils::joinMulticastGroup(fd, "not an address", ""));
    CHECK_FALSE(NetworkUtils::joinMulticastGroup(fd, "", ""));

    ::close(fd);
}
