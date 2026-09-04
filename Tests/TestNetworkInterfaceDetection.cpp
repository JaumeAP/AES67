//
// TestNetworkInterfaceDetection.cpp
// AES67 macOS Driver
//
// Which interface the driver picks when nobody names one, and what it says
// about the interfaces it finds.
//
// Every one of these answers depends on the machine the test runs on, so
// nothing here asserts a name or an address: what is checked is the
// invariants a caller relies on — that a reported interface exists, that a
// name nobody has does not resolve, that the fallback is never empty. A test
// that demanded "en0" would pass on one Mac and fail on the next, which is
// worse than the zero coverage this had until 2026-09-04.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/NetworkInterfaceDetection.h"
#include "NetworkEngine/NetworkUtils.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace AES67;

namespace {

bool contains(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

TEST_CASE("The interfaces reported are distinct and named") {
    const auto interfaces = NetworkInterfaceDetection::getAllInterfaces();
    REQUIRE_FALSE(interfaces.empty());  // loopback, at least

    for (const auto& name : interfaces) {
        CHECK_FALSE(name.empty());
    }

    std::vector<std::string> sorted = interfaces;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
}

TEST_CASE("A multicast-capable interface is one of the interfaces, and says so") {
    const auto all = NetworkInterfaceDetection::getAllInterfaces();
    const auto multicast = NetworkInterfaceDetection::getMulticastCapableInterfaces();

    for (const auto& name : multicast) {
        CHECK(contains(all, name));
        CHECK(NetworkInterfaceDetection::supportsMulticast(name));
        CHECK(NetworkInterfaceDetection::isInterfaceActive(name));
        // It was selected for carrying an IPv4 address, so it has one.
        CHECK(NetworkUtils::isIPv4Address(
            NetworkInterfaceDetection::getInterfaceIPAddress(name)));
    }
}

TEST_CASE("An interface nobody has answers no to everything") {
    // The failure mode that matters: a name out of a settings file that no
    // longer matches anything must not come back looking usable.
    const std::string absent = "nosuchif0";

    CHECK_FALSE(NetworkInterfaceDetection::isInterfaceActive(absent));
    CHECK_FALSE(NetworkInterfaceDetection::isEthernetInterface(absent));
    CHECK_FALSE(NetworkInterfaceDetection::supportsMulticast(absent));
    CHECK(NetworkInterfaceDetection::getInterfaceIPAddress(absent).empty());

    // And the empty name is not a wildcard that matches the first interface.
    CHECK_FALSE(NetworkInterfaceDetection::isInterfaceActive(""));
    CHECK(NetworkInterfaceDetection::getInterfaceIPAddress("").empty());
}

TEST_CASE("The PTP interface is always something, and something real") {
    // PTP has to start somewhere even on a machine with nothing plugged in,
    // so this never returns empty; what it must not do is return a name that
    // is neither an interface nor the documented fallback.
    const std::string chosen = NetworkInterfaceDetection::detectPTPInterface();
    REQUIRE_FALSE(chosen.empty());

    const auto all = NetworkInterfaceDetection::getAllInterfaces();
    const bool real = contains(all, chosen);
    CHECK((real || chosen == "en0"));  // en0 is the documented fallback

    if (real) {
        CHECK(NetworkInterfaceDetection::supportsMulticast(chosen));
    }
}

TEST_CASE("The PTP interface is one of the multicast-capable ones when there are any") {
    // PTP is multicast (224.0.1.129), so an interface that cannot carry it
    // is not a candidate however good its name looks.
    const auto multicast = NetworkInterfaceDetection::getMulticastCapableInterfaces();
    const std::string chosen = NetworkInterfaceDetection::detectPTPInterface();

    if (!multicast.empty()) {
        CHECK(contains(multicast, chosen));
    }
}

TEST_CASE("Choosing twice chooses the same") {
    // The driver resolves this at start and again on reconfiguration; an
    // answer that moved between calls would put PTP and RTP on different
    // interfaces.
    CHECK(NetworkInterfaceDetection::detectPTPInterface() ==
          NetworkInterfaceDetection::detectPTPInterface());
    CHECK(NetworkInterfaceDetection::getPrimaryEthernetInterface() ==
          NetworkInterfaceDetection::getPrimaryEthernetInterface());
}

TEST_CASE("The primary ethernet interface, when there is one, is real and up") {
    const std::string primary = NetworkInterfaceDetection::getPrimaryEthernetInterface();
    if (primary.empty()) return;  // a machine with no ethernet is allowed

    CHECK(contains(NetworkInterfaceDetection::getAllInterfaces(), primary));
    CHECK(NetworkInterfaceDetection::isInterfaceActive(primary));
}
