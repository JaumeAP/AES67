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
    AES67DiscoveryHandle* handle = aes67_discovery_start("127.0.0.1", 0, 0);
    CHECK(handle == nullptr);
}

TEST_CASE("A handle starts, answers and stops") {
    // Loopback: no announcement of ours goes anywhere, and none is expected.
    // What is under test is the shape of the answer on a quiet network and
    // that the handle survives being asked repeatedly.
    AES67DiscoveryHandle* handle = aes67_discovery_start("127.0.0.1", 1, 1);
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
    AES67DiscoveryHandle* first = aes67_discovery_start("127.0.0.1", 1, 0);
    REQUIRE(first != nullptr);
    AES67DiscoveryHandle* second = aes67_discovery_start("127.0.0.1", 1, 0);
    CHECK(second != nullptr);

    aes67_discovery_stop(second);
    aes67_discovery_stop(first);
}

TEST_CASE("An interface that is not an address does not stop discovery") {
    // Passed through to SAPListener, which says so and joins on any
    // interface rather than refusing to listen at all.
    AES67DiscoveryHandle* handle = aes67_discovery_start("not-an-address", 1, 0);
    CHECK(handle != nullptr);
    aes67_discovery_stop(handle);
}
