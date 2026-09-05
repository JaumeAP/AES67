//
// TestNetworkErrorHandler.cpp
// Error accounting and the recovery latch, which had no tests.
//
// The class does two things a caller depends on: it counts what went wrong and
// it makes sure only one recovery runs at a time. The second is the one worth
// pinning -- a second recovery starting while the first is in flight would have
// two code paths reopening the same sockets.
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
    CHECK(handler.isInRecovery() == false);
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

TEST_CASE("Recovery runs synchronously and releases its own latch") {
    // Pinning what the code does, which is not what its comment says.
    //
    // attemptRecovery() sets recoveryActive_, does the work and clears the
    // flag before returning -- all inside one call. The comment above that
    // clear reads "Clear recovery flag after a delay", and there is no delay.
    //
    // So the guard against a second recovery only covers a window that lasts
    // as long as the call itself, and with the recovery body still a
    // placeholder that window is empty: a second call always succeeds, and
    // isInRecovery() is false anywhere a caller could observe it.
    //
    // That is worth a test rather than a fix here: making the latch mean
    // something is a behaviour change in a driver, and the recovery it guards
    // has not been written yet.
    NetworkErrorHandler handler;

    CHECK(handler.attemptRecovery());
    CHECK(handler.isInRecovery() == false);
    CHECK(handler.attemptRecovery());
}

TEST_CASE("Reset clears the count and the recovery latch") {
    NetworkErrorHandler handler;
    handler.reportError(NetworkErrorType::CONNECTION_TIMEOUT, "no reply", "RTSPClient");
    handler.attemptRecovery();
    REQUIRE(handler.getErrorCount() > 0);

    handler.reset();

    CHECK(handler.getErrorCount() == 0);
    CHECK(handler.isInRecovery() == false);
    CHECK(handler.attemptRecovery());
}
