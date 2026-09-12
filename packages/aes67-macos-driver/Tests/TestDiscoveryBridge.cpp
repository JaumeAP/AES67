//
// TestDiscoveryBridge.cpp
// AES67 macOS Driver
//
// The C face of the driver's discovery, which is what a separate application
// links against.
//
// The controller (packages/aes67-macos-controller) has no driver and no Core
// Audio: it runs SAPListener and RTSPSessionDiscovery itself, through this
// bridge, so that a machine where the plug-in was never installed can still
// find what is on the network. What the bridge owes that caller is a handle
// whose lifetime is its own, and a snapshot it can parse -- including when
// there is nothing to report and when there is no handle at all.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/DiscoveryBridge.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::string takeString(char* raw) {
    if (!raw) return {};
    std::string text(raw);
    aes67_discovery_free_string(raw);
    return text;
}

} // namespace

TEST_CASE("No handle is an empty list, not a crash") {
    // The caller checks for NULL, but it is a C API reached from another
    // language: the answer to a null handle is the same answer as an empty
    // network, never a fault.
    CHECK(takeString(aes67_discovery_sessions_json(nullptr)) == "[]");
    aes67_discovery_stop(nullptr);       // must be a no-op
    aes67_discovery_free_string(nullptr); // and so must this
}

TEST_CASE("Asking for neither route gets no handle") {
    // Nothing to run is not a handle that runs nothing: the caller is told,
    // so it can say why its list stays empty.
    AES67DiscoveryHandle* handle = aes67_discovery_start("127.0.0.1", "lo0", 0, 0, 0, 0);
    CHECK(handle == nullptr);
}

TEST_CASE("A handle starts, answers and stops") {
    // Loopback: no announcement of ours goes anywhere, and none is expected.
    // What is under test is the shape of the answer on a quiet network and
    // that the handle survives being asked repeatedly.
    AES67DiscoveryHandle* handle = aes67_discovery_start("127.0.0.1", "lo0", 1, 1, 0, 0);
    REQUIRE(handle != nullptr);

    for (int i = 0; i < 3; ++i) {
        const std::string json = takeString(aes67_discovery_sessions_json(handle));
        INFO(json);
        CHECK(json.front() == '[');
        CHECK(json.back() == ']');
    }

    aes67_discovery_stop(handle);
}

TEST_CASE("Two handles at once do not fight over the port") {
    // A controller open twice, or a controller beside this driver: the SAP
    // socket sets SO_REUSEADDR and SO_REUSEPORT for exactly that, and a
    // second handle that failed to start would leave the second window
    // permanently empty.
    AES67DiscoveryHandle* first = aes67_discovery_start("127.0.0.1", "lo0", 1, 0, 0, 0);
    REQUIRE(first != nullptr);
    AES67DiscoveryHandle* second = aes67_discovery_start("127.0.0.1", "lo0", 1, 0, 0, 0);
    CHECK(second != nullptr);

    aes67_discovery_stop(second);
    aes67_discovery_stop(first);
}

TEST_CASE("The PTP observer is how gear that announces nothing is found") {
    // Dolby Atmos Connect announces nothing at all -- no SAP, no registered
    // service, no NMOS -- so the only thing it puts on the network unasked is
    // its clock. Asking for the observer alone has to give a handle, and its
    // answer has to be a list even on a segment with no PTP on it.
    AES67DiscoveryHandle* handle = aes67_discovery_start("127.0.0.1", "lo0", 0, 0, 1, 0);
    REQUIRE(handle != nullptr);

    const std::string json = takeString(aes67_discovery_ptp_peers_json(handle));
    INFO(json);
    CHECK(json.front() == '[');
    CHECK(json.back() == ']');

    // The session list and the peer list are different questions: a peer is a
    // clock, not a session, and nothing on the wire says what its audio is.
    CHECK(takeString(aes67_discovery_sessions_json(handle)) == "[]");

    aes67_discovery_stop(handle);
}

TEST_CASE("Peers asked of a handle that has no observer are an empty list") {
    AES67DiscoveryHandle* handle = aes67_discovery_start("127.0.0.1", "lo0", 1, 0, 0, 0);
    REQUIRE(handle != nullptr);
    CHECK(takeString(aes67_discovery_ptp_peers_json(handle)) == "[]");
    aes67_discovery_stop(handle);

    CHECK(takeString(aes67_discovery_ptp_peers_json(nullptr)) == "[]");
}

TEST_CASE("Service browsing sees every kind of gear this world registers") {
    // The point of the list is that a room shows what is on it, whoever made
    // it: RAVENNA's _rtsp._tcp, NMOS's _nmos-node._tcp, Dante's
    // _netaudio-*._udp. Reading a registration is not speaking a protocol,
    // which is what makes a Dante device listable and not controllable.
    AES67DiscoveryHandle* handle = aes67_discovery_start("127.0.0.1", "lo0", 0, 0, 0, 1);
    REQUIRE(handle != nullptr);

    const std::string json = takeString(aes67_discovery_services_json(handle));
    INFO(json);
    CHECK(json.front() == '[');
    CHECK(json.back() == ']');

    aes67_discovery_stop(handle);
}

TEST_CASE("Services asked of a handle that browses none are an empty list") {
    AES67DiscoveryHandle* handle = aes67_discovery_start("127.0.0.1", "lo0", 1, 0, 0, 0);
    REQUIRE(handle != nullptr);
    CHECK(takeString(aes67_discovery_services_json(handle)) == "[]");
    aes67_discovery_stop(handle);

    CHECK(takeString(aes67_discovery_services_json(nullptr)) == "[]");
}

TEST_CASE("An interface that is not an address does not stop discovery") {
    // Passed through to SAPListener, which says so and joins on any
    // interface rather than refusing to listen at all.
    AES67DiscoveryHandle* handle = aes67_discovery_start("not-an-address", "lo0", 1, 0, 0, 0);
    CHECK(handle != nullptr);
    aes67_discovery_stop(handle);
}
