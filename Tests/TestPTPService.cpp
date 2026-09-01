//
// TestPTPService.cpp
// AES67 macOS Driver
//
// The status socket between the PTP daemon and its readers. Both halves run
// in this process over a temporary path, so the protocol, the staleness rule
// and the version check are exercised without a daemon or a network.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/PTP/PTPDInterface.h"
#include "NetworkEngine/PTP/PTPService.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace AES67;

namespace {

std::string TempSocketPath(const char* name) {
    return std::string("/tmp/aes67ptpd-test-") + name + "-"
           + std::to_string(::getpid()) + ".sock";
}

template <typename Predicate>
bool waitFor(Predicate&& done, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return done();
}

} // namespace

TEST_CASE("A Published Status Reaches The Reader") {
    const std::string path = TempSocketPath("roundtrip");
    PTPServiceServer server(path);
    REQUIRE(server.start());

    PTPServiceClient client(path);
    REQUIRE(client.start());
    CHECK(waitFor([&] { return server.clientCount() == 1; },
                  std::chrono::milliseconds(2000)));

    PTPServiceStatus status;
    status.locked = 1;
    status.clockClass = 6;
    status.offsetNs = -12345;
    status.pathDelayNs = 54321;
    status.frequencyDriftPpb = 17.5;
    status.domain = 0;
    for (int i = 0; i < 8; ++i) status.grandmasterIdentity[i] = static_cast<uint8_t>(i + 1);

    // Published repeatedly: the reader may connect a moment after start().
    CHECK(waitFor([&] { server.publish(status); return client.isLocked(); },
                  std::chrono::milliseconds(3000)));

    CHECK(client.hasFreshStatus());
    CHECK(client.getOffsetNs() == -12345);
    CHECK(client.getPathDelayNs() == 54321);
    CHECK(client.getClockClass() == 6);
    CHECK(client.getFrequencyDriftPpb() == doctest::Approx(17.5));
    CHECK(client.getGrandmasterID() == "01:02:03:04:05:06:07:08");

    PTPServiceStatus copy;
    CHECK(client.lastStatus(&copy));
    CHECK(copy.sequence > 0);
    CHECK(copy.length == sizeof(PTPServiceStatus));

    client.stop();
    server.stop();
}

TEST_CASE("Nothing Is Reported Before The Daemon Is There") {
    const std::string path = TempSocketPath("absent");
    PTPServiceClient client(path);
    REQUIRE(client.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // No daemon: not locked, no offset, and no pretending otherwise.
    CHECK(!client.hasFreshStatus());
    CHECK(!client.isLocked());
    CHECK(client.getOffsetNs() == 0);
    PTPServiceStatus copy;
    CHECK(!client.lastStatus(&copy));
    client.stop();
}

TEST_CASE("A Dead Daemon Stops Being Believed") {
    const std::string path = TempSocketPath("stale");
    auto server = std::make_unique<PTPServiceServer>(path);
    REQUIRE(server->start());

    PTPServiceClient client(path);
    REQUIRE(client.start());

    PTPServiceStatus status;
    status.locked = 1;
    status.offsetNs = 1000;
    CHECK(waitFor([&] { server->publish(status); return client.isLocked(); },
                  std::chrono::milliseconds(3000)));

    // The daemon dies. The last offset stays readable as history, but the
    // lock must not survive it: a stale measurement presented as a lock is
    // exactly how a driver ends up silently free-running.
    server.reset();
    CHECK(waitFor([&] { return !client.isLocked(); },
                  std::chrono::milliseconds(kPTPServiceStaleMs + 2000)));
    CHECK(!client.hasFreshStatus());
    CHECK(client.getOffsetNs() == 1000); // still there as history
    client.stop();
}

TEST_CASE("A Daemon From Another Build Is Refused") {
    // Hand-rolled server so the test can send a status this build must not
    // accept: same socket, wrong version.
    const std::string path = TempSocketPath("version");
    ::unlink(path.c_str());

    const int listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(listenFd >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());
    REQUIRE(::bind(listenFd, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == 0);
    REQUIRE(::listen(listenFd, 4) == 0);

    PTPServiceClient client(path);
    REQUIRE(client.start());

    const int clientFd = ::accept(listenFd, nullptr, nullptr);
    REQUIRE(clientFd >= 0);

    PTPServiceStatus wrong;
    wrong.version = kPTPServiceVersion + 99;
    wrong.locked = 1;
    wrong.offsetNs = 999;
    ::send(clientFd, &wrong, sizeof(wrong), 0);

    CHECK(waitFor([&] { return client.getRejectedCount() > 0; },
                  std::chrono::milliseconds(3000)));
    CHECK(!client.isLocked());
    CHECK(client.getOffsetNs() == 0);

    client.stop();
    ::close(clientFd);
    ::close(listenFd);
    ::unlink(path.c_str());
}

TEST_CASE("The Socket Is Cleaned Up And Reusable") {
    const std::string path = TempSocketPath("reuse");
    {
        PTPServiceServer first(path);
        REQUIRE(first.start());
        // A second server on the same path replaces the node rather than
        // failing: a daemon that crashed leaves one behind.
        PTPServiceServer second(path);
        CHECK(second.start());
        second.stop();
    }
    PTPServiceServer again(path);
    CHECK(again.start());
    again.stop();
}

TEST_CASE("PTPDInterface Reads The Daemon When Its Socket Is There") {
    // When the daemon's socket exists, the measurements come from there
    // rather than from a second PTP engine started inside this process.
    const std::string path = TempSocketPath("interface");
    auto server = std::make_unique<PTPServiceServer>(path);
    REQUIRE(server->start());

    PTPDInterface ptp(false);
    ptp.setServiceSocketPath(path);
    REQUIRE(ptp.init("lo0"));
    CHECK(ptp.isUsingPrivilegedDaemon());
    CHECK(!ptp.isStubMode());
    ptp.start();

    PTPServiceStatus status;
    status.locked = 1;
    status.clockClass = 6;
    status.offsetNs = -4242;
    status.pathDelayNs = 8484;
    CHECK(waitFor([&] {
        server->publish(status);
        return ptp.getState().isLocked.load();
    }, std::chrono::milliseconds(4000)));
    CHECK(ptp.getState().masterOffsetNs.load() == -4242);

    // Daemon gone: the lock has to go with it.
    server.reset();
    CHECK(waitFor([&] { return !ptp.getState().isLocked.load(); },
                  std::chrono::milliseconds(kPTPServiceStaleMs + 3000)));
    ptp.stop();
}

TEST_CASE("Without The Daemon The In-Process Path Is Unchanged") {
    // No socket at that path: init() must take the old road, not wait for a
    // daemon that is not coming.
    PTPDInterface ptp(false);
    ptp.setServiceSocketPath(TempSocketPath("missing"));
    REQUIRE(ptp.init("lo0"));
    CHECK(!ptp.isUsingPrivilegedDaemon());

    // And it can be refused outright even when one is there.
    const std::string path = TempSocketPath("refused");
    PTPServiceServer server(path);
    REQUIRE(server.start());
    PTPDInterface direct(false);
    direct.setServiceSocketPath(path);
    direct.setPreferPrivilegedDaemon(false);
    REQUIRE(direct.init("lo0"));
    CHECK(!direct.isUsingPrivilegedDaemon());
    server.stop();
}
