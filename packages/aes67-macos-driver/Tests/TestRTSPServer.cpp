//
// TestRTSPServer.cpp
// AES67 macOS Driver
//
// Drives the DESCRIBE endpoint over a real loopback TCP socket, because
// that is the only way to prove the thing a remote receiver actually
// does: connect, ask, read bytes back. The server binds an ephemeral
// port (0), so nothing here collides with a real RTSP service or with a
// parallel test run.
//
// The hostile cases matter as much as the happy one: this endpoint is
// reachable by anyone on the segment and it lives inside coreaudiod,
// where an escaped exception takes the system audio daemon down with it.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/RTSPServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace AES67;

namespace {

const std::string kSDP =
    "v=0\r\n"
    "o=- 1 1 IN IP4 192.168.0.16\r\n"
    "s=Studio Mic 1\r\n"
    "c=IN IP4 239.69.0.1/32\r\n"
    "t=0 0\r\n"
    "m=audio 5004 RTP/AVP 96\r\n"
    "a=rtpmap:96 L24/48000/2\r\n"
    "a=ptime:1\r\n";

std::vector<RTSPPublishedStream> provider() {
    return {
        {"/by-name/Studio Mic 1", kSDP},
        {"/", kSDP},
    };
}

/// Sends one request to the server and returns the whole response.
/// Returns empty on any socket failure, so a test can fail loudly
/// instead of hanging.
std::string ask(uint16_t port, const std::string& request) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};

    struct timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return {};
    }

    if (send(fd, request.data(), request.size(), 0) < 0) {
        close(fd);
        return {};
    }

    std::string response;
    char buffer[1024];
    while (true) {
        const ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        response.append(buffer, static_cast<size_t>(got));
        if (response.size() > 64 * 1024) break;
    }
    close(fd);
    return response;
}

/// A started server on an ephemeral port, stopped on scope exit.
class RunningServer {
public:
    RunningServer() : server_(0) { started_ = server_.start(provider); }
    ~RunningServer() { server_.stop(); }

    bool started() const { return started_; }
    uint16_t port() const { return server_.boundPort(); }
    RTSPServer& get() { return server_; }

private:
    RTSPServer server_;
    bool started_{false};
};

} // namespace

TEST_CASE("DESCRIBE Returns The SDP For A Published Path") {
    std::cout << "Test: RTSP DESCRIBE serves our session description... ";
    RunningServer server;
    REQUIRE(server.started());
    REQUIRE(server.port() != 0);

    // A URL carries no raw spaces (RFC 3986): a client percent-encodes
    // them and the server decodes before matching.
    const std::string response = ask(
        server.port(),
        "DESCRIBE rtsp://127.0.0.1/by-name/Studio%20Mic%201 RTSP/1.0\r\n"
        "CSeq: 7\r\n\r\n");

    REQUIRE(!response.empty());
    CHECK(response.rfind("RTSP/1.0 200 OK", 0) == 0);
    CHECK(response.find("CSeq: 7") != std::string::npos);        // echoed, RFC 2326 §12.17
    CHECK(response.find("Content-Type: application/sdp") != std::string::npos);
    CHECK(response.find("Content-Length: " + std::to_string(kSDP.size())) != std::string::npos);
    // The body must be the description verbatim — a receiver parses it.
    CHECK(response.find(kSDP) != std::string::npos);

    // The unencoded form is a malformed request line, and saying so is
    // the correct answer -- not a silent match.
    const std::string raw = ask(
        server.port(),
        "DESCRIBE rtsp://127.0.0.1/by-name/Studio Mic 1 RTSP/1.0\r\n"
        "CSeq: 8\r\n\r\n");
    CHECK(raw.rfind("RTSP/1.0 400", 0) == 0);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("An Absolute Or Bare Path Both Resolve") {
    std::cout << "Test: RTSP accepts absolute URLs and bare paths... ";
    RunningServer server;
    REQUIRE(server.started());

    const std::string absolute = ask(
        server.port(), "DESCRIBE rtsp://127.0.0.1:8554/ RTSP/1.0\r\nCSeq: 1\r\n\r\n");
    const std::string bare = ask(
        server.port(), "DESCRIBE / RTSP/1.0\r\nCSeq: 2\r\n\r\n");

    CHECK(absolute.rfind("RTSP/1.0 200 OK", 0) == 0);
    CHECK(bare.rfind("RTSP/1.0 200 OK", 0) == 0);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("OPTIONS Advertises Only What Is Answered") {
    std::cout << "Test: RTSP OPTIONS is honest about its methods... ";
    RunningServer server;
    REQUIRE(server.started());

    const std::string response = ask(
        server.port(), "OPTIONS * RTSP/1.0\r\nCSeq: 3\r\n\r\n");

    CHECK(response.rfind("RTSP/1.0 200 OK", 0) == 0);
    CHECK(response.find("Public: OPTIONS, DESCRIBE") != std::string::npos);
    // Not advertised because not implemented: an AES67 stream is already
    // running on its multicast group, so there is nothing to negotiate.
    CHECK(response.find("SETUP") == std::string::npos);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Unknown Path Is 404 And Unknown Method Is 501") {
    std::cout << "Test: RTSP rejects what it cannot serve... ";
    RunningServer server;
    REQUIRE(server.started());

    const std::string missing = ask(
        server.port(), "DESCRIBE /nope RTSP/1.0\r\nCSeq: 4\r\n\r\n");
    CHECK(missing.rfind("RTSP/1.0 404 Not Found", 0) == 0);
    CHECK(missing.find("CSeq: 4") != std::string::npos);

    const std::string unsupported = ask(
        server.port(), "PLAY / RTSP/1.0\r\nCSeq: 5\r\n\r\n");
    CHECK(unsupported.rfind("RTSP/1.0 501 Not Implemented", 0) == 0);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Malformed Requests Are Answered, Never Fatal") {
    std::cout << "Test: RTSP survives garbage from an unauthenticated peer... ";
    RunningServer server;
    REQUIRE(server.started());

    // No version token, no CSeq, not even a method: every one of these
    // reaches the parser, and none of them may throw — this endpoint
    // runs inside coreaudiod.
    CHECK(ask(server.port(), "garbage\r\n\r\n").rfind("RTSP/1.0 400", 0) == 0);
    CHECK(ask(server.port(), "DESCRIBE / HTTP/1.1\r\n\r\n").rfind("RTSP/1.0 400", 0) == 0);
    CHECK(!ask(server.port(), "DESCRIBE / RTSP/1.0\r\n\r\n").empty()); // no CSeq: CSeq 0
    // An absurd CSeq must not overflow into nonsense or hang.
    CHECK(!ask(server.port(),
               "DESCRIBE / RTSP/1.0\r\nCSeq: 99999999999999999999\r\n\r\n").empty());

    // A client that connects and says nothing is dropped on the receive
    // timeout, and the server keeps serving afterwards.
    const int silent = socket(AF_INET, SOCK_STREAM, 0);
    if (silent >= 0) {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(server.port());
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        connect(silent, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        close(silent);
    }
    CHECK(ask(server.port(), "OPTIONS * RTSP/1.0\r\nCSeq: 6\r\n\r\n")
              .rfind("RTSP/1.0 200 OK", 0) == 0);

    CHECK(server.get().requestCount() > 0);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Lifecycle Edges") {
    std::cout << "Test: RTSP server lifecycle... ";
    RTSPServer server(0);

    server.stop(); // never started
    CHECK(!server.isRunning());
    CHECK(!server.start(nullptr)); // a server with nothing to serve is refused

    CHECK(server.start(provider));
    CHECK(server.isRunning());
    CHECK(!server.start(provider)); // no second thread
    server.stop();
    server.stop();                  // idempotent
    CHECK(!server.isRunning());
    std::cout << "PASS" << std::endl;
}
