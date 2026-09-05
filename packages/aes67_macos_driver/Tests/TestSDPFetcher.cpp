//
// TestSDPFetcher.cpp
// AES67 macOS Driver
//
// Fetching a session description from where the gear publishes it, over
// real loopback sockets: a description that only ever came from a local
// file is a description somebody had to copy across by hand.
//
// The hostile cases are the point as much as the happy one. Whatever
// answers at the other end is not ours, this code is linked into
// coreaudiod, and an escaped exception there takes the system audio
// daemon with it: a garbage status line, a Content-Length in megabytes
// and a body that never ends all have to come back as an error string.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/SDPFetcher.h"
#include "NetworkEngine/Discovery/RTSPServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

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

/// A one-shot HTTP server on an ephemeral loopback port. It answers the
/// first connection with whatever raw response the test hands it, so a
/// malformed answer is as easy to stage as a well-formed one.
class OneShotHTTP {
public:
    explicit OneShotHTTP(std::string rawResponse)
        : response_(std::move(rawResponse)) {}

    ~OneShotHTTP() { stop(); }

    bool start() {
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_ < 0) return false;
        int yes = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
        port_ = ntohs(addr.sin_port);
        if (::listen(listen_, 1) != 0) return false;

        thread_ = std::thread([this] {
            const int client = ::accept(listen_, nullptr, nullptr);
            if (client < 0) return;
            char scratch[2048];
            ::recv(client, scratch, sizeof(scratch), 0);   // the request, unread
            ::send(client, response_.data(), response_.size(), 0);
            ::close(client);                               // close ends the body
        });
        return true;
    }

    void stop() {
        if (listen_ >= 0) {
            ::shutdown(listen_, SHUT_RDWR);
            ::close(listen_);
            listen_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    uint16_t port() const { return port_; }

private:
    std::string response_;
    int listen_{-1};
    uint16_t port_{0};
    std::thread thread_;
};

std::string httpAnswer(const std::string& body, const char* status = "200 OK",
                       bool withLength = true) {
    std::string head = std::string("HTTP/1.1 ") + status + "\r\n"
                     + "Content-Type: application/sdp\r\n";
    if (withLength) head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    return head + "\r\n" + body;
}

std::string writeTempSDP(const std::string& contents) {
    std::string path = std::string(std::tmpnam(nullptr)) + ".sdp";
    std::ofstream out(path);
    out << contents;
    out.close();
    return path;
}

std::vector<RTSPPublishedStream> rtspProvider() {
    RTSPPublishedStream entry;
    entry.path = "/";
    entry.sdp = kSDP;
    return {entry};
}

} // namespace

TEST_CASE("A URL Splits Into Its Pieces") {
    SDPFetcher::URLParts parts;

    REQUIRE(SDPFetcher::parseURL("http://192.168.0.16/session.sdp", parts));
    CHECK(parts.scheme == "http");
    CHECK(parts.host == "192.168.0.16");
    CHECK(parts.port == 80);
    CHECK(parts.path == "/session.sdp");

    REQUIRE(SDPFetcher::parseURL("RTSP://cp850.local:8554/by-name/Mic", parts));
    CHECK(parts.scheme == "rtsp");        // the scheme is compared lowercased
    CHECK(parts.host == "cp850.local");
    CHECK(parts.port == 8554);
    CHECK(parts.path == "/by-name/Mic");

    // No path at all still addresses the root.
    REQUIRE(SDPFetcher::parseURL("rtsp://10.0.0.5", parts));
    CHECK(parts.port == 554);
    CHECK(parts.path == "/");

    // A bare path is not a failure: it is what every settings file held
    // before this existed.
    REQUIRE(SDPFetcher::parseURL("/etc/aes67/session.sdp", parts));
    CHECK(parts.scheme.empty());
    CHECK(parts.path == "/etc/aes67/session.sdp");

    REQUIRE(SDPFetcher::parseURL("file:///etc/aes67/session.sdp", parts));
    CHECK(parts.scheme == "file");
    CHECK(parts.path == "/etc/aes67/session.sdp");

    // Refused outright, without a socket being opened.
    CHECK_FALSE(SDPFetcher::parseURL("ftp://host/file.sdp", parts));
    CHECK_FALSE(SDPFetcher::parseURL("http://:80/x", parts));
    CHECK_FALSE(SDPFetcher::parseURL("http://host:0/x", parts));
    CHECK_FALSE(SDPFetcher::parseURL("http://host:99999/x", parts));
    CHECK_FALSE(SDPFetcher::parseURL("http://host:eighty/x", parts));
    CHECK_FALSE(SDPFetcher::parseURL("", parts));
}

TEST_CASE("A Local File Reads The Same Either Way") {
    const std::string path = writeTempSDP(kSDP);

    const auto bare = SDPFetcher::fetch(path);
    REQUIRE(bare.ok());
    CHECK(bare.text == kSDP);

    const auto viaScheme = SDPFetcher::fetch("file://" + path);
    REQUIRE(viaScheme.ok());
    CHECK(viaScheme.text == kSDP);

    std::remove(path.c_str());

    const auto gone = SDPFetcher::fetch(path);
    CHECK_FALSE(gone.ok());
    CHECK(gone.text.empty());
    CHECK(gone.error.find("cannot open") != std::string::npos);
}

TEST_CASE("An Empty File Is An Error, Not An Empty Description") {
    const std::string path = writeTempSDP("");
    const auto result = SDPFetcher::fetch(path);
    std::remove(path.c_str());
    CHECK_FALSE(result.ok());
    CHECK(result.error.find("empty file") != std::string::npos);
}

TEST_CASE("HTTP Brings Back What The Server Published") {
    OneShotHTTP server(httpAnswer(kSDP));
    REQUIRE(server.start());

    const auto result = SDPFetcher::fetch(
        "http://127.0.0.1:" + std::to_string(server.port()) + "/session.sdp");
    REQUIRE(result.ok());
    CHECK(result.text == kSDP);
}

TEST_CASE("HTTP Without Content-Length Still Reads To The Close") {
    OneShotHTTP server(httpAnswer(kSDP, "200 OK", /*withLength=*/false));
    REQUIRE(server.start());

    const auto result = SDPFetcher::fetch(
        "http://127.0.0.1:" + std::to_string(server.port()) + "/");
    REQUIRE(result.ok());
    CHECK(result.text == kSDP);
}

TEST_CASE("HTTP Failures Come Back Named") {
    SUBCASE("a 404 is not a description") {
        OneShotHTTP server(httpAnswer("nothing here", "404 Not Found"));
        REQUIRE(server.start());
        const auto result = SDPFetcher::fetch(
            "http://127.0.0.1:" + std::to_string(server.port()) + "/missing.sdp");
        CHECK_FALSE(result.ok());
        CHECK(result.error.find("HTTP 404") != std::string::npos);
    }

    SUBCASE("a garbage status line does not throw") {
        OneShotHTTP server("HTTP/1.1 not-a-number OK\r\n\r\nbody");
        REQUIRE(server.start());
        const auto result = SDPFetcher::fetch(
            "http://127.0.0.1:" + std::to_string(server.port()) + "/");
        CHECK_FALSE(result.ok());
        CHECK(result.error.find("status line") != std::string::npos);
    }

    SUBCASE("a body announced in megabytes is refused") {
        OneShotHTTP server("HTTP/1.1 200 OK\r\nContent-Length: 99999999\r\n\r\nshort");
        REQUIRE(server.start());
        const auto result = SDPFetcher::fetch(
            "http://127.0.0.1:" + std::to_string(server.port()) + "/");
        CHECK_FALSE(result.ok());
        CHECK(result.error.find("will not read") != std::string::npos);
    }

    SUBCASE("a head with no end of head is not a description") {
        OneShotHTTP server("HTTP/1.1 200 OK\r\nContent-Type: application/sdp\r\n");
        REQUIRE(server.start());
        const auto result = SDPFetcher::fetch(
            "http://127.0.0.1:" + std::to_string(server.port()) + "/");
        CHECK_FALSE(result.ok());
        CHECK(result.error.find("malformed") != std::string::npos);
    }

    SUBCASE("a 200 with nothing in it is an error") {
        OneShotHTTP server(httpAnswer(""));
        REQUIRE(server.start());
        const auto result = SDPFetcher::fetch(
            "http://127.0.0.1:" + std::to_string(server.port()) + "/");
        CHECK_FALSE(result.ok());
        CHECK(result.error.find("empty body") != std::string::npos);
    }

    SUBCASE("nobody listening is an error, not a wait") {
        // Port 1 on loopback: nothing binds it, so connect() refuses at once.
        const auto result = SDPFetcher::fetch("http://127.0.0.1:1/", 500);
        CHECK_FALSE(result.ok());
        CHECK(result.error.find("cannot connect") != std::string::npos);
    }
}

TEST_CASE("RTSP DESCRIBE Is A Source Like Any Other") {
    RTSPServer server(0);
    REQUIRE(server.start(rtspProvider));

    const auto result = SDPFetcher::fetch(
        "rtsp://127.0.0.1:" + std::to_string(server.boundPort()) + "/");
    REQUIRE(result.ok());
    CHECK(result.text.find("s=Studio Mic 1") != std::string::npos);

    const auto missing = SDPFetcher::fetch(
        "rtsp://127.0.0.1:" + std::to_string(server.boundPort()) + "/by-name/Nothing");
    CHECK_FALSE(missing.ok());
    CHECK_FALSE(missing.error.empty());

    server.stop();
}

TEST_CASE("HTTPS Says So Instead Of Failing Late") {
    const auto result = SDPFetcher::fetch("https://cp850.local/session.sdp");
    CHECK_FALSE(result.ok());
    CHECK(result.error.find("https") != std::string::npos);
    CHECK(result.error.find("TLS") != std::string::npos);
}
