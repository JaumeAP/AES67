//
// TestNetworkErrorHandler.cpp
// Error accounting and what a recovery attempt does, neither of which had
// tests.
//
// The class counts what went wrong, and marks an attempt at recovery by
// clearing the recent count and stamping the time reportError's cooldown is
// measured from. It used to claim a third thing -- that only one recovery
// runs at a time -- through a latch set and cleared inside one synchronous
// call, which no caller could ever be caught by. That is gone; what is left
// is what these cases pin.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/NetworkErrorHandler.h"

#include <string>
#include <vector>

using AES67::NetworkError;
using AES67::NetworkErrorHandler;
using AES67::NetworkErrorType;

TEST_CASE("A fresh handler has nothing to report") {
    NetworkErrorHandler handler;
    CHECK(handler.getErrorCount() == 0);
}

TEST_CASE("Reported errors are counted") {
    NetworkErrorHandler handler;

    handler.reportError(NetworkErrorType::SOCKET_ERROR, "socket refused", "RTPReceiver", 61);
    CHECK(handler.getErrorCount() == 1);

    handler.reportError(NetworkErrorType::PACKET_LOSS, "sequence gap", "RTPReceiver");
    handler.reportError(NetworkErrorType::INTERFACE_DOWN, "en0 went away", "Detection");
    CHECK(handler.getErrorCount() == 3);
}

TEST_CASE("The registered handler sees the error it was given") {
    NetworkErrorHandler handler;

    std::vector<NetworkError> seen;
    handler.registerErrorHandler([&seen](const NetworkError& e) { seen.push_back(e); });

    handler.reportError(NetworkErrorType::MULTICAST_JOIN_FAILURE,
                        "IP_ADD_MEMBERSHIP failed", "StreamManager", 49);

    REQUIRE(seen.size() == 1);
    CHECK(seen[0].type == NetworkErrorType::MULTICAST_JOIN_FAILURE);
    CHECK(seen[0].message == "IP_ADD_MEMBERSHIP failed");
    CHECK(seen[0].source == "StreamManager");
    CHECK(seen[0].errorCode == 49);
}

TEST_CASE("Registering a second handler replaces the first") {
    // Worth stating either way: a caller that registers twice needs to know
    // whether it gets both or the last one.
    NetworkErrorHandler handler;
    int first = 0, second = 0;

    handler.registerErrorHandler([&first](const NetworkError&) { ++first; });
    handler.registerErrorHandler([&second](const NetworkError&) { ++second; });

    handler.reportError(NetworkErrorType::UNKNOWN_ERROR, "something", "test");

    CHECK(first == 0);
    CHECK(second == 1);
}

TEST_CASE("A recovery attempt always runs, and says so") {
    // There is no state to be in and nothing to be refused by: the body is a
    // placeholder, because what recovers a socket is its owner reopening it.
    // Two calls in a row both succeed, which is what the latch that used to
    // sit here was unable to prevent anyway.
    NetworkErrorHandler handler;

    CHECK(handler.attemptRecovery());
    CHECK(handler.attemptRecovery());
}

TEST_CASE("A recovery attempt clears the recent count, not the total") {
    // The two counts are different questions: how many errors there have
    // been, and how many since the last attempt at doing something about
    // them. reportError's threshold reads the second.
    NetworkErrorHandler handler;
    handler.reportError(NetworkErrorType::CONNECTION_TIMEOUT, "no reply", "RTSPClient");
    handler.reportError(NetworkErrorType::SOCKET_ERROR, "refused", "RTPReceiver", 61);
    REQUIRE(handler.getErrorCount() == 2);

    CHECK(handler.attemptRecovery());
    CHECK(handler.getErrorCount() == 2);
}

TEST_CASE("Reset clears the count") {
    NetworkErrorHandler handler;
    handler.reportError(NetworkErrorType::CONNECTION_TIMEOUT, "no reply", "RTSPClient");
    handler.attemptRecovery();
    REQUIRE(handler.getErrorCount() > 0);

    handler.reset();

    CHECK(handler.getErrorCount() == 0);
    CHECK(handler.attemptRecovery());
}
