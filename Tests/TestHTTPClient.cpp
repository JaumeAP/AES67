//
// TestHTTPClient.cpp
// AES67 macOS Driver
//
// The small HTTP client the discovery layer is built on, against a server
// made of a loopback socket that answers whatever the test tells it to.
//
// Tested through the socket rather than through a seam because the request
// this writes matters as much as the answer it reads: an NMOS registry that
// gets no Content-Length, or a `GET` without the Accept it was asked for,
// fails in ways no parse test would show. It was at zero coverage until
// 2026-09-04, reached only indirectly through the tests of its callers.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/HTTPClient.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>

using namespace AES67;

namespace {

/// A one-shot HTTP server: accepts one connection, keeps what it was sent,
/// writes back a fixed answer and closes. `answer` is written verbatim, so a
/// test can send something no real server would.
class FakeServer {
public:
    explicit FakeServer(std::string answer, bool replyAtAll = true)
        : answer_(std::move(answer)), reply_(replyAtAll) {}

    ~FakeServer() { stop(); }

    bool start() {
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_ < 0) return false;
        int yes = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // any free port
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
        port_ = ntohs(addr.sin_port);
        if (::listen(listen_, 4) != 0) return false;

        thread_ = std::thread([this] { serve(); });
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

    /// The request the client sent, once served.
    std::string request() {
        if (thread_.joinable()) thread_.join();
        return request_;
    }

private:
    void serve() {
        const int client = ::accept(listen_, nullptr, nullptr);
        if (client < 0) return;

        // Read the head, and the body when the request declares one.
        char chunk[2048];
        while (true) {
            const ssize_t n = ::recv(client, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            request_.append(chunk, static_cast<size_t>(n));
            const size_t headEnd = request_.find("\r\n\r\n");
            if (headEnd == std::string::npos) continue;
            const size_t at = request_.find("Content-Length: ");
            const long declared = (at == std::string::npos)
                                      ? 0
                                      : std::strtol(request_.c_str() + at + 16, nullptr, 10);
            if (request_.size() >= headEnd + 4 + static_cast<size_t>(declared)) break;
        }

        if (reply_) {
            size_t sent = 0;
            while (sent < answer_.size()) {
                const ssize_t wrote =
                    ::send(client, answer_.data() + sent, answer_.size() - sent, 0);
                if (wrote <= 0) break;
                sent += static_cast<size_t>(wrote);
            }
        }
        ::close(client);
    }

    std::string answer_;
    bool reply_{true};
    int listen_{-1};
    uint16_t port_{0};
    std::string request_;
    std::thread thread_;
};

std::string okAnswer(const std::string& body,
                     const std::string& contentType = "application/sdp") {
    return "HTTP/1.1 200 OK\r\nContent-Type: " + contentType +
           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

} // namespace

TEST_CASE("A GET reaches the server and its body comes back") {
    FakeServer server(okAnswer("v=0\r\ns=Studio\r\n"));
    REQUIRE(server.start());

    HTTPClient client("127.0.0.1", server.port(), 2000);
    const HTTPResponse response = client.get("/session.sdp", "application/sdp");

    CHECK(response.ok());
    CHECK(response.status == 200);
    CHECK(response.body == "v=0\r\ns=Studio\r\n");

    const std::string request = server.request();
    CHECK(request.find("GET /session.sdp HTTP/1.1\r\n") == 0);
    CHECK(request.find("Accept: application/sdp\r\n") != std::string::npos);
    CHECK(request.find("Host: 127.0.0.1:") != std::string::npos);
}

TEST_CASE("Accept belongs to the request that asked for it") {
    // get() sets Accept for one call and clears it again; a later request
    // that did not ask for one must not inherit it.
    FakeServer first(okAnswer("{}", "application/json"));
    REQUIRE(first.start());
    HTTPClient client("127.0.0.1", first.port(), 2000);
    CHECK(client.get("/one", "application/json").ok());
    CHECK(first.request().find("Accept:") != std::string::npos);
    first.stop();

    FakeServer second(okAnswer("{}", "application/json"));
    REQUIRE(second.start());
    HTTPClient plain("127.0.0.1", second.port(), 2000);
    CHECK(plain.get("/two").ok());
    CHECK(second.request().find("Accept:") == std::string::npos);
}

TEST_CASE("A POST carries its body, its type and its length") {
    // What an IS-04 registration is: a JSON body a registry will refuse
    // without a correct Content-Length.
    const std::string payload = "{\"type\":\"node\"}";
    FakeServer server("HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n");
    REQUIRE(server.start());

    HTTPClient client("127.0.0.1", server.port(), 2000);
    const HTTPResponse response = client.post("/x-nmos/registration/v1.3/resource", payload,
                                              "application/json");

    CHECK(response.status == 201);
    CHECK(response.ok());   // ok() is any 2xx that completed, which 201 is
    CHECK(response.error.empty());

    const std::string request = server.request();
    CHECK(request.find("POST /x-nmos/registration/v1.3/resource HTTP/1.1\r\n") == 0);
    CHECK(request.find("Content-Type: application/json\r\n") != std::string::npos);
    CHECK(request.find("Content-Length: " + std::to_string(payload.size()) + "\r\n") !=
          std::string::npos);
    CHECK(request.size() >= payload.size());
    CHECK(request.substr(request.size() - payload.size()) == payload);
}

TEST_CASE("A DELETE is a DELETE") {
    FakeServer server("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");
    REQUIRE(server.start());

    HTTPClient client("127.0.0.1", server.port(), 2000);
    CHECK(client.del("/x-nmos/registration/v1.3/resource/nodes/abc").status == 204);
    CHECK(server.request().find("DELETE /") == 0);
}

TEST_CASE("An error status is reported, not turned into an error") {
    // A 404 is an answer. Confusing it with a failure to reach the server
    // would have the caller retry something that will never work.
    FakeServer server("HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\n\r\nno!");
    REQUIRE(server.start());

    HTTPClient client("127.0.0.1", server.port(), 2000);
    const HTTPResponse response = client.get("/missing");

    CHECK(response.status == 404);
    CHECK(response.body == "no!");
    CHECK(response.error.empty());
    CHECK_FALSE(response.ok());
}

TEST_CASE("The body is cut to the length the server declared") {
    // A server that sends more than it declares does not get to decide how
    // much of it this keeps.
    FakeServer server("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirsttrailing garbage");
    REQUIRE(server.start());

    HTTPClient client("127.0.0.1", server.port(), 2000);
    const HTTPResponse response = client.get("/");

    CHECK(response.status == 200);
    CHECK(response.body == "first");
}

TEST_CASE("A body larger than this will read is refused, not truncated") {
    // Declaring a gigabyte is how a hostile server would ask this to
    // allocate one. It is answered with an error and no status.
    FakeServer server("HTTP/1.1 200 OK\r\nContent-Length: 1073741824\r\n\r\nnope");
    REQUIRE(server.start());

    HTTPClient client("127.0.0.1", server.port(), 2000);
    const HTTPResponse response = client.get("/huge");

    CHECK(response.status == 0);
    CHECK_FALSE(response.error.empty());
    CHECK_FALSE(response.ok());
}

TEST_CASE("Nonsense where a status line goes is an error") {
    SUBCASE("not a status line at all") {
        FakeServer server("GARBAGE\r\n\r\nbody");
        REQUIRE(server.start());
        HTTPClient client("127.0.0.1", server.port(), 2000);
        const HTTPResponse response = client.get("/");
        CHECK(response.status == 0);
        CHECK_FALSE(response.error.empty());
    }

    SUBCASE("a status code that is not a number") {
        FakeServer server("HTTP/1.1 OK fine\r\nContent-Length: 0\r\n\r\n");
        REQUIRE(server.start());
        HTTPClient client("127.0.0.1", server.port(), 2000);
        const HTTPResponse response = client.get("/");
        CHECK(response.status == 0);
        CHECK_FALSE(response.error.empty());
    }

    SUBCASE("a status code outside the range HTTP defines") {
        FakeServer server("HTTP/1.1 999 Nope\r\nContent-Length: 0\r\n\r\n");
        REQUIRE(server.start());
        HTTPClient client("127.0.0.1", server.port(), 2000);
        CHECK(client.get("/").status == 0);
    }

    SUBCASE("headers that never end") {
        FakeServer server("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n");
        REQUIRE(server.start());
        HTTPClient client("127.0.0.1", server.port(), 2000);
        const HTTPResponse response = client.get("/");
        CHECK(response.status == 0);
        CHECK_FALSE(response.error.empty());
    }
}

TEST_CASE("A server that says nothing is an error, not a hang") {
    FakeServer server("", /*replyAtAll=*/false);
    REQUIRE(server.start());

    HTTPClient client("127.0.0.1", server.port(), 300);
    const HTTPResponse response = client.get("/silent");

    CHECK(response.status == 0);
    CHECK_FALSE(response.error.empty());
}

TEST_CASE("A port with nothing behind it fails at connect") {
    // Port 1 on loopback: nothing listens there, and the caller has to be
    // told that rather than left waiting.
    HTTPClient client("127.0.0.1", 1, 300);
    const HTTPResponse response = client.get("/");

    CHECK(response.status == 0);
    CHECK(response.error.find("127.0.0.1") != std::string::npos);
}
