//
// TestGrandmasterOnTheWire.cpp
// aes67-linux-ptpd - Tests
// The daemon running, and the RAVENNA module reading what it actually sent.
//
// TestRavennaSlaveInterop already holds this project's messages against the
// Merging module's own code. It builds them with PtpWire and hands them over
// in memory, which answers "do the bytes this project writes satisfy that
// module" and leaves the other half open: whether the daemon puts those bytes
// on a socket, in that order, at those intervals, with the sequence numbers
// the module insists are contiguous.
//
// So this starts aes67-ptpd on the loopback, takes what it sends off the
// group, and feeds it to the same module. Loopback because it is the one
// interface a build machine is guaranteed to have and the one nobody else is
// listening on; the daemon binds it like any other.
//
// Linux gives lo no MULTICAST flag by default -- macOS gives lo0 one, which is
// why this looked fine until a runner said otherwise -- so whoever runs this
// turns it on first. The workflow does. AES67_TEST_INTERFACE names another
// interface for a machine where that is not wanted.
//
// Wants privilege, and says so if it does not have it: PTP is on 319 and 320,
// which are below 1024. The daemon needs the same to bind them, so this is a
// property of what is being tested and not of the test. Labelled network and
// run on its own, as root, by whoever runs it.
//
// Linux only, and only where the daemon was built: it is the thing under
// test. CMake builds this nowhere else.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "PtpWire.h"
#include "support/RavennaSlave.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using namespace AES67;
using namespace AES67::LinuxPtpd;
using namespace AES67::LinuxPtpd::Tests;

namespace {

/// Where the daemon was built. The test binary and it land in the same
/// directory, and ctest runs from there.
constexpr char kDaemon[] = "./aes67-ptpd";

/// The interface to run over, and the address to join the group on.
std::string testInterface() {
    const char* named = ::getenv("AES67_TEST_INTERFACE");
    return named != nullptr && *named != '\0' ? named : "lo";
}

std::string testAddress() {
    const char* named = ::getenv("AES67_TEST_ADDRESS");
    return named != nullptr && *named != '\0' ? named : "127.0.0.1";
}

/// One PTP message as it came off the wire, and which port it came in on.
struct Captured {
    std::vector<uint8_t> payload;
    uint16_t port = 0;
};

/// A socket on one of the two PTP ports, joined to the primary group on the
/// loopback and set not to block.
int openPtpPort(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        MESSAGE("socket(): " << std::strerror(errno));
        return -1;
    }

    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif

    struct sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) < 0) {
        // 319 and 320 are below 1024, so this wants privilege -- the daemon
        // under test needs the same, for the same reason.
        MESSAGE("bind " << port << ": " << std::strerror(errno));
        ::close(fd);
        return -1;
    }

    struct ip_mreq join {};
    join.imr_multiaddr.s_addr = ::inet_addr(kPtpPrimaryGroup);
    join.imr_interface.s_addr = ::inet_addr(testAddress().c_str());
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &join, sizeof(join)) < 0) {
        // Said out loud: the usual reason is an interface with no MULTICAST
        // flag, and "the socket would not open" sends whoever reads it looking
        // in the wrong place.
        MESSAGE("IP_ADD_MEMBERSHIP on " << testAddress() << " for port " << port << ": "
                                        << std::strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Everything that arrives on either port inside the window, in the order it
/// arrived. The order is the point: a Follow_Up before its Sync is a lock the
/// module never takes.
std::vector<Captured> capture(int eventFd, int generalFd, int milliseconds) {
    std::vector<Captured> heard;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);

    uint8_t buffer[2048];
    while (std::chrono::steady_clock::now() < deadline) {
        struct pollfd waiting[2] {};
        waiting[0].fd = eventFd;
        waiting[0].events = POLLIN;
        waiting[1].fd = generalFd;
        waiting[1].events = POLLIN;

        if (::poll(waiting, 2, 200) <= 0) continue;

        for (int i = 0; i < 2; ++i) {
            if ((waiting[i].revents & POLLIN) == 0) continue;
            const ssize_t received = ::recv(waiting[i].fd, buffer, sizeof(buffer), 0);
            if (received <= 0) continue;
            heard.push_back({std::vector<uint8_t>(buffer, buffer + received),
                             i == 0 ? kEventPort : kGeneralPort});
        }
    }
    return heard;
}

/// The daemon, running until it is stopped. Zero when it could not be started.
pid_t startDaemon() {
    const pid_t child = ::fork();
    if (child != 0) return child;

    // The child. Software timestamps because the loopback has no hardware
    // ones, and quiet because its output is not what is being read.
    ::freopen("/dev/null", "w", stdout);
    const std::string interfaceName = testInterface();
    ::execl(kDaemon, kDaemon, "--interface", interfaceName.c_str(),
            "--allow-software-timestamps", static_cast<char*>(nullptr));
    ::_exit(127);
}

void stopDaemon(pid_t child) {
    if (child <= 0) return;
    ::kill(child, SIGTERM);
    int status = 0;
    ::waitpid(child, &status, 0);
}

uint8_t messageTypeOf(const std::vector<uint8_t>& payload) {
    return payload.empty() ? 0xFF : static_cast<uint8_t>(payload[0] & 0x0F);
}

}  // namespace

TEST_CASE("The module takes what the daemon puts on the wire") {
    const int eventFd = openPtpPort(kEventPort);
    const int generalFd = openPtpPort(kGeneralPort);
    REQUIRE(eventFd >= 0);
    REQUIRE(generalFd >= 0);

    const pid_t daemon = startDaemon();
    REQUIRE(daemon > 0);

    // Long enough for an announce and several sync pairs at any profile this
    // daemon serves: the AES67 profiles are eight syncs a second and the
    // 1588 default one a second.
    const std::vector<Captured> heard = capture(eventFd, generalFd, 4000);

    stopDaemon(daemon);
    ::close(eventFd);
    ::close(generalFd);

    REQUIRE_FALSE(heard.empty());

    size_t announces = 0;
    size_t syncs = 0;
    size_t followUps = 0;
    for (const Captured& message : heard) {
        switch (messageTypeOf(message.payload)) {
            case 0x0: ++syncs; break;
            case 0x8: ++followUps; break;
            case 0xB: ++announces; break;
            default: break;
        }
    }

    INFO("heard ", heard.size(), " messages: ", announces, " announce, ", syncs, " sync, ",
         followUps, " follow_up");
    CHECK(announces > 0);
    CHECK(syncs > 0);
    // Two-step, so every Sync is followed by the Follow_Up that carries the
    // departure time. One without the other is a lock that never closes.
    CHECK(followUps > 0);

    // And now the part no in-memory test can do: the module's own code, fed
    // the packets in the order the daemon actually sent them.
    SlaveState slave;
    setCounterTime(0);

    bool elected = false;
    for (const Captured& message : heard) {
        const SlaveVerdict verdict =
            feed(slave, message.payload.data(), message.payload.size(), message.port);
        if (verdict.elected) elected = true;
        // Nothing this daemon sends may reset the module's lock: that is what
        // contiguous sequence numbers are for, and a gap here is a daemon
        // whose messages a RAVENNA device would keep dropping.
        CHECK_FALSE(verdict.lockReset);
    }

    CHECK(elected);
    CHECK(slave.masterClockIdentity != 0);
    CHECK(slave.grandmasterIdentity == slave.masterClockIdentity);
}
