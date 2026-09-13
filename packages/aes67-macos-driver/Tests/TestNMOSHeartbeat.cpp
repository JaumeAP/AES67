//
// TestNMOSHeartbeat.cpp
// AES67 macOS Driver
//
// How often the driver tells a registry it is still there, and what it does
// when that registry stops answering.
//
// Separate from TestNMOSRegistration because it spends real seconds: a
// heartbeat interval is five of them and two gaps take eleven, which is the
// `timing` label and not something every push should wait for.
//
// It exists because the suite that found the bug was not ours. The heartbeat
// loop slept for `kHeartbeatPeriod / 50`, and std::chrono::seconds divides
// integrally, so it slept for nothing at all and beat two hundred thousand
// times in twenty-four seconds. Every test passed. What noticed was the AMWA
// IS-04-01 suite, run by hand, saying "Heartbeats are too frequent."
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/NMOSRegistrationClient.h"
#include "Ravenna/RegistryBrowser.h"
#include "support/FakeRegistry.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace AES67;
using AES67::Testing::answer;
using AES67::Testing::FakeRegistry;

namespace {

NMOSNodeInfo testNode() {
    NMOSNodeInfo node;
    node.id = "8c4a5f2e-0000-4000-8000-000000000001";
    node.label = "Studio Mac";
    node.hostname = "studio.local";
    return node;
}

/// The health POSTs a registry was sent, in the order they arrived.
std::vector<std::chrono::steady_clock::time_point> heartbeatsOf(const FakeRegistry& registry) {
    const std::vector<std::string> requests = registry.requests();
    const std::vector<std::chrono::steady_clock::time_point> arrivals = registry.arrivals();

    std::vector<std::chrono::steady_clock::time_point> beats;
    for (size_t i = 0; i < requests.size() && i < arrivals.size(); i++) {
        if (requests[i].find("POST /x-nmos/registration/v1.3/health/nodes/") == 0) {
            beats.push_back(arrivals[i]);
        }
    }
    return beats;
}

} // namespace

TEST_CASE("The heartbeat keeps the interval IS-04 asks for") {
    FakeRegistry registry({answer("201 Created")}, answer("200 OK"));
    REQUIRE(registry.start());

    NMOSRegistrationClient client(testNode());
    REQUIRE(client.registerWith({"127.0.0.1", registry.port(), "v1.3"}));

    client.startHeartbeats();
    // Long enough for three beats, so there are two gaps to measure. One gap
    // would not tell a stopped clock from a running one.
    std::this_thread::sleep_for(NMOSRegistrationClient::kHeartbeatPeriod * 2 +
                                std::chrono::seconds(1));
    client.stop();

    const auto beats = heartbeatsOf(registry);
    REQUIRE(beats.size() >= 3);

    for (size_t i = 1; i < beats.size(); i++) {
        const auto gap =
            std::chrono::duration_cast<std::chrono::milliseconds>(beats[i] - beats[i - 1]);
        // IS-04 sec 4.2: a registry forgets a node it has not heard from in
        // twelve seconds, and a controller watching the interval allows about
        // one of them either way. The bound that matters is the lower one:
        // this was zero, and the registry was being hammered.
        CHECK(gap >= NMOSRegistrationClient::kHeartbeatPeriod -
                         std::chrono::milliseconds(500));
        CHECK(gap <= NMOSRegistrationClient::kHeartbeatPeriod +
                         std::chrono::milliseconds(1500));
    }
}

TEST_CASE("A registry that stops answering is left for another") {
    // The one in use, which will go away.
    auto failing = std::make_unique<FakeRegistry>(answer("201 Created"));
    REQUIRE(failing->start());
    FakeRegistry standby(answer("201 Created"));
    REQUIRE(standby.start());

    NMOSRegistrationClient client(testNode());
    REQUIRE(client.registerWith({"127.0.0.1", failing->port(), "v1.3"}));

    Ravenna::NmosRegistry advertisedFailing;
    advertisedFailing.host = "127.0.0.1";
    advertisedFailing.port = failing->port();
    advertisedFailing.priority = 0;

    Ravenna::NmosRegistry advertisedStandby;
    advertisedStandby.host = "127.0.0.1";
    advertisedStandby.port = standby.port();
    advertisedStandby.priority = 10;

    SUBCASE("the one that failed is not chosen again") {
        // Both advertised, the first still answering: a node that takes the
        // one it is already on has not failed over at all, and the next lost
        // beat finds it in the same place.
        CHECK(client.failOverTo({advertisedFailing, advertisedStandby}));
        CHECK(standby.requests().size() == 1);
    }

    SUBCASE("nowhere to go leaves it where it was") {
        failing.reset();  // closes the socket
        CHECK_FALSE(client.failOverTo({advertisedFailing}));
    }

    SUBCASE("the registry it moves to is the one told about the node") {
        failing.reset();
        REQUIRE(client.failOverTo({advertisedFailing, advertisedStandby}));

        const std::vector<std::string> requests = standby.requests();
        REQUIRE(requests.size() == 1);
        CHECK(requests[0].find("POST /x-nmos/registration/v1.3/resource") == 0);
        CHECK(requests[0].find("\"type\": \"node\"") != std::string::npos);
        CHECK(requests[0].find("8c4a5f2e-0000-4000-8000-000000000001") != std::string::npos);
    }
}
