//
// TestListenSocket.cpp
// AES67 RAVENNA
//
// The listening socket both servers in this package open, and what they say
// about the port afterwards.
//
// The port a server reports is not a detail: ravenna-announce hands it to
// mdns.start(), which writes it into the SRV record every client on the
// segment resolves. A server that failed to start and still reports its old
// port advertises one nothing is listening on, which is exactly the failure
// the getsockname-on-port-0 branch below exists to prevent in the other
// direction.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Ravenna/HttpServer.h"
#include "Ravenna/ListenSocket.h"
#include "Ravenna/RtspServer.h"
#include "Ravenna/SessionCatalogue.h"

#include <unistd.h>

#include <string>

using namespace AES67::Ravenna;

namespace {

/// A handler that is never reached: nothing here sends a request.
HttpServer::Handler noHandler() {
    return [](const std::string&, const std::string&, const std::string&) {
        return ApiResponse{};
    };
}

} // namespace

TEST_CASE("Port zero is answered with the port that was actually taken") {
    // The whole reason the caller is told the port back: ravenna-announce
    // passes 0 to mean "any free one" and then advertises what it got.
    uint16_t port = 0;
    int fd = -1;
    std::string error;

    REQUIRE(openListenSocket(port, fd, error));
    CHECK(fd >= 0);
    CHECK(port != 0);
    CHECK(error.empty());

    ::close(fd);
}

TEST_CASE("A port that cannot be bound leaves nothing open and says why") {
    uint16_t port = 0;
    int fd = -1;
    std::string error;
    REQUIRE(openListenSocket(port, fd, error));

    // The same port again, from a second socket: SO_REUSEADDR does not make
    // two listeners on one port legal.
    uint16_t second = port;
    int secondFd = -1;
    std::string secondError;
    CHECK_FALSE(openListenSocket(second, secondFd, secondError));
    CHECK(secondFd == -1);
    CHECK_FALSE(secondError.empty());

    ::close(fd);
}

TEST_CASE("A server that fails to start reports no port at all") {
    // It used to: the failure path stopped clearing port_ when the socket
    // work moved into openListenSocket, so port() kept answering with the
    // port of the run before.
    HttpServer first(noHandler());
    std::string error;
    REQUIRE(first.start(0, error));
    const uint16_t taken = first.port();
    REQUIRE(taken != 0);

    HttpServer second(noHandler());
    std::string secondError;
    REQUIRE(second.start(0, secondError));
    const uint16_t held = second.port();
    REQUIRE(held != 0);

    // Now ask it for a port somebody else has. It has to forget its own.
    CHECK_FALSE(second.start(taken, secondError));
    CHECK(second.port() == 0);
    CHECK_FALSE(secondError.empty());
}

TEST_CASE("The RTSP server forgets its port the same way") {
    SessionCatalogue catalogue;
    RtspServer first(catalogue);
    std::string error;
    REQUIRE(first.start(0, error));
    const uint16_t taken = first.port();
    REQUIRE(taken != 0);

    RtspServer second(catalogue);
    std::string secondError;
    REQUIRE(second.start(0, secondError));
    REQUIRE(second.port() != 0);

    CHECK_FALSE(second.start(taken, secondError));
    CHECK(second.port() == 0);
}
