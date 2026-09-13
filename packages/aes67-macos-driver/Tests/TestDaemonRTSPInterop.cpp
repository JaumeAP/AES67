//
// TestDaemonRTSPInterop.cpp
// AES67 macOS Driver - Tests
// This driver's RTSP DESCRIBE against the AES67 Linux daemon's rules for it.
//
// TestRTSPClient and TestRTSPServer cover RFC 2326 as this code reads it.
// This suite covers what bondagit/aes67-linux-daemon's RTSP actually does --
// not the same test, the same way TestDaemonSAPInterop's SAP oracle is not
// TestSAP* over RFC 2974. It found a real gap: RTSPClient::sendRequest sent
// url_ and path verbatim, with no escaping at all, while every by-name path
// the daemon itself serves carries a space by convention
// ("/by-name/<node id> <name>", rtsp_server.cpp's build_response). An
// unescaped space splits the RTSP request line into extra fields, which the
// daemon's own request parser answers with 400 Bad Request rather than the
// description that was asked for. Fixed alongside this suite
// (RTSPClient.cpp's encodeRequestURL); regression-pinned in the round trip
// below, which is exactly the shape that broke.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/RTSPClient.h"
#include "NetworkEngine/Discovery/RTSPServer.h"
#include "support/DaemonRtsp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace AES67;
using namespace AES67::Tests;

namespace {

const std::string kSDP =
    "v=0\r\n"
    "o=- 1 1 IN IP4 192.168.1.20\r\n"
    "s=Studio Mic 1\r\n"
    "c=IN IP4 239.69.0.1/32\r\n"
    "t=0 0\r\n"
    "m=audio 5004 RTP/AVP 96\r\n"
    "a=rtpmap:96 L24/48000/2\r\n"
    "a=ptime:1\r\n";

/// Sends one request to a server and returns the whole response, up to the
/// peer closing the connection -- which is what RTSPServer does after every
/// answer, so a plain read-to-EOF is enough.
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
        if (response.size() > static_cast<size_t>(64 * 1024)) break;
    }
    close(fd);
    return response;
}

std::vector<RTSPPublishedStream> provider() {
    return {{"/by-name/node1 Studio Mic 1", kSDP}};
}

class RunningServer {
public:
    RunningServer() : server_(0) { started_ = server_.start(provider); }
    ~RunningServer() { server_.stop(); }
    bool started() const { return started_; }
    uint16_t port() const { return server_.boundPort(); }

private:
    RTSPServer server_;
    bool started_{false};
};

/// One accept, one request captured, one canned response sent back --
/// standing in for the daemon's own RTSP server for the tests that drive
/// this driver's RTSPClient rather than its RTSPServer.
class CapturingServer {
public:
    CapturingServer() {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd_ >= 0);
        int reuse = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = 0;
        REQUIRE(bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(listen(fd_, 1) == 0);
        struct sockaddr_in bound{};
        socklen_t boundLen = sizeof(bound);
        getsockname(fd_, reinterpret_cast<struct sockaddr*>(&bound), &boundLen);
        port_ = ntohs(bound.sin_port);
    }
    ~CapturingServer() {
        if (thread_.joinable()) thread_.join();
        close(fd_);
    }

    uint16_t port() const { return port_; }
    const std::string& capturedRequest() const { return request_; }

    /// Accepts one connection in the background, reads its headers, hands
    /// them to `respond` and sends back whatever it returns.
    void serveOnce(const std::function<std::string(const std::string&)>& respond) {
        thread_ = std::thread([this, respond] {
            const int client = accept(fd_, nullptr, nullptr);
            if (client < 0) return;
            struct timeval tv{3, 0};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buffer[4096];
            while (request_.find("\r\n\r\n") == std::string::npos) {
                const ssize_t got = recv(client, buffer, sizeof(buffer), 0);
                if (got <= 0) break;
                request_.append(buffer, static_cast<size_t>(got));
            }
            const std::string response = respond(request_);
            send(client, response.data(), response.size(), 0);
            close(client);
        });
    }

private:
    int fd_{-1};
    uint16_t port_{0};
    std::string request_;
    std::thread thread_;
};

} // namespace

TEST_CASE("The daemon's client can DESCRIBE what this driver's RTSPServer serves") {
    RunningServer server;
    REQUIRE(server.started());

    // The path itself is the daemon's own by-name convention -- a node id, a
    // space, then the name -- so encoding the space is exercised here too.
    const std::string request =
        daemonDescribeRequest("127.0.0.1", server.port(), "/by-name/node1 Studio Mic 1", 42);
    CHECK(request.find("node1%20Studio%20Mic%201") != std::string::npos);

    const std::string response = ask(server.port(), request);
    const auto result = daemonReadDescribeResponse(response, 42);

    INFO("refused because: ", result.refusal);
    REQUIRE(result.accepted);
    CHECK(result.sdp == kSDP);
}

TEST_CASE("A path this driver's RTSPServer does not have is a DESCRIBE the daemon's client refuses") {
    RunningServer server;
    REQUIRE(server.started());

    const std::string request = daemonDescribeRequest("127.0.0.1", server.port(), "/by-name/nope", 7);
    const std::string response = ask(server.port(), request);
    const auto result = daemonReadDescribeResponse(response, 7);

    CHECK_FALSE(result.accepted);
    CHECK(result.refusal.find("404") != std::string::npos);
}

TEST_CASE("This driver's RTSPClient parses the daemon's exact response shapes") {
    SUBCASE("a 200 with the SDP body") {
        const auto parsed = RTSPClient::parseResponse(daemonDescribeOkResponse(9, kSDP));
        REQUIRE(parsed.has_value());
        CHECK(parsed->statusCode == 200);
        CHECK(parsed->isSuccess());
        CHECK(parsed->body == kSDP);
    }
    SUBCASE("a 404 with no body") {
        const auto parsed = RTSPClient::parseResponse(daemonErrorResponse(404, "Not found", 9));
        REQUIRE(parsed.has_value());
        CHECK(parsed->statusCode == 404);
        CHECK_FALSE(parsed->isSuccess());
    }
    SUBCASE("a 400 sent before a CSeq was ever read, so it carries none") {
        const auto parsed = RTSPClient::parseResponse(daemonErrorResponse(400, "Bad Request", -1));
        REQUIRE(parsed.has_value());
        CHECK(parsed->statusCode == 400);
        CHECK(parsed->headers.find("CSeq") == parsed->headers.end());
    }
    SUBCASE("a 405 for a method the daemon does not serve") {
        const auto parsed = RTSPClient::parseResponse(daemonErrorResponse(405, "Method Not Allowed", 3));
        REQUIRE(parsed.has_value());
        CHECK(parsed->statusCode == 405);
        CHECK_FALSE(parsed->isSuccess());
    }
}

TEST_CASE("The daemon's server would accept what this driver's RTSPClient actually sends") {
    CapturingServer daemon;
    daemon.serveOnce([](const std::string& request) {
        const auto decision = daemonDecideRequest(request, /*pathExists=*/true);
        REQUIRE(decision.statusCode == 200);
        return daemonDescribeOkResponse(decision.cseq, kSDP);
    });

    // The daemon's own naming convention: a node id, a space, then a name
    // with a space of its own -- the exact shape that used to split the
    // request line into extra fields before encodeRequestURL existed.
    RTSPClient client("rtsp://127.0.0.1:" + std::to_string(daemon.port()) +
                      "/by-name/node1 Studio Mic 1");
    const auto session = client.describe();

    REQUIRE(session.has_value());
    CHECK(session->sessionName == "Studio Mic 1");
    CHECK(session->connectionAddress == "239.69.0.1");
    CHECK(session->sampleRate == 48000);
    CHECK(session->numChannels == 2);

    CHECK(daemon.capturedRequest().find("node1%20Studio%20Mic%201") != std::string::npos);
    // Exactly three fields on the request line: a raw space here is the
    // regression this pins.
    const auto lineEnd = daemon.capturedRequest().find("\r\n");
    const std::string requestLine = daemon.capturedRequest().substr(0, lineEnd);
    CHECK(std::count(requestLine.begin(), requestLine.end(), ' ') == 2);
}

TEST_CASE("A path this driver's RTSPClient asks for and the daemon does not have is read as a failure") {
    CapturingServer daemon;
    daemon.serveOnce([](const std::string& request) {
        const auto decision = daemonDecideRequest(request, /*pathExists=*/false);
        return daemonErrorResponse(decision.statusCode, decision.reason, decision.cseq);
    });

    RTSPClient client("rtsp://127.0.0.1:" + std::to_string(daemon.port()) + "/by-name/nope");
    CHECK_FALSE(client.describe().has_value());
}
