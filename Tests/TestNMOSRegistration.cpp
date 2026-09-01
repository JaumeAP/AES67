//
// TestNMOSRegistration.cpp
// AES67 macOS Driver
//
// Registering with an NMOS registry, against a registry that is a socket
// in this test rather than a broadcast plant.
//
// The point of a registry is that a controller reads it instead of
// probing the network, so what matters is what this driver actually PUTS
// there: the right endpoint, a body a registry will accept, and a
// heartbeat that keeps the entry alive. Each of those is checked by
// reading the requests off the wire.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/NMOSRegistrationClient.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace AES67;

namespace {

/// A registry that answers whatever the test tells it to and keeps every
/// request it was sent.
class FakeRegistry {
public:
    explicit FakeRegistry(std::string answer) : answer_(std::move(answer)) {}
    ~FakeRegistry() { stop(); }

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
        if (::listen(listen_, 4) != 0) return false;

        running_.store(true);
        thread_ = std::thread([this] {
            while (running_.load()) {
                const int client = ::accept(listen_, nullptr, nullptr);
                if (client < 0) return;
                std::string request;
                char chunk[2048];
                // One read is enough: the client sends head and body in a
                // single write and then waits.
                const ssize_t n = ::recv(client, chunk, sizeof(chunk), 0);
                if (n > 0) request.append(chunk, static_cast<size_t>(n));
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    requests_.push_back(request);
                }
                ::send(client, answer_.data(), answer_.size(), 0);
                ::close(client);
            }
        });
        return true;
    }

    void stop() {
        running_.store(false);
        if (listen_ >= 0) {
            ::shutdown(listen_, SHUT_RDWR);
            ::close(listen_);
            listen_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    uint16_t port() const { return port_; }

    std::vector<std::string> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    std::string answer_;
    int listen_{-1};
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<std::string> requests_;
};

std::string answer(const char* status, const std::string& body = {}) {
    return std::string("HTTP/1.1 ") + status + "\r\n" +
           "Content-Type: application/json\r\n" +
           "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

NMOSNodeInfo testNode() {
    NMOSNodeInfo node;
    node.id = "8c4a5f2e-0000-4000-8000-000000000001";
    node.label = "Studio Mac";
    node.hostname = "studio.local";
    return node;
}

} // namespace

TEST_CASE("The Endpoints Are The Ones IS-04 Names") {
    CHECK(NMOSRegistrationClient::registrationPath("v1.3") ==
          "/x-nmos/registration/v1.3/resource");
    CHECK(NMOSRegistrationClient::healthPath("v1.3", "abc") ==
          "/x-nmos/registration/v1.3/health/nodes/abc");
}

TEST_CASE("The Body Is A Node Resource") {
    const std::string body =
        NMOSRegistrationClient::buildRegistrationBody(testNode(), 1756000000, 250);

    CHECK(body.find("\"type\": \"node\"") != std::string::npos);
    CHECK(body.find("\"id\": \"8c4a5f2e-0000-4000-8000-000000000001\"") != std::string::npos);
    CHECK(body.find("\"label\": \"Studio Mac\"") != std::string::npos);
    CHECK(body.find("\"hostname\": \"studio.local\"") != std::string::npos);
    // The version orders updates to a resource, and its shape is
    // "<seconds>:<nanoseconds>".
    CHECK(body.find("\"version\": \"1756000000:250\"") != std::string::npos);
    // A clock this node really has, not one it wishes it had.
    CHECK(body.find("\"ref_type\": \"internal\"") != std::string::npos);
}

TEST_CASE("A Label With A Quote In It Does Not Break The Body") {
    NMOSNodeInfo node = testNode();
    node.label = "Studio \"B\"";
    const std::string body = NMOSRegistrationClient::buildRegistrationBody(node, 1, 0);
    CHECK(body.find("\"label\": \"Studio \\\"B\\\"\"") != std::string::npos);
}

TEST_CASE("Registration Posts The Node And Reads The Answer") {
    SUBCASE("201 is a new registration") {
        FakeRegistry registry(answer("201 Created"));
        REQUIRE(registry.start());

        NMOSRegistrationClient client(testNode());
        NMOSRegistry where{"127.0.0.1", registry.port(), "v1.3"};
        CHECK(client.registerWith(where));
        CHECK(client.isRegistered());

        const auto requests = registry.requests();
        REQUIRE(requests.size() == 1);
        CHECK(requests[0].find("POST /x-nmos/registration/v1.3/resource") == 0);
        CHECK(requests[0].find("Content-Type: application/json") != std::string::npos);
        CHECK(requests[0].find("\"type\": \"node\"") != std::string::npos);
    }

    SUBCASE("200 is the registry recognising an id it already holds") {
        FakeRegistry registry(answer("200 OK"));
        REQUIRE(registry.start());

        NMOSRegistrationClient client(testNode());
        CHECK(client.registerWith({"127.0.0.1", registry.port(), "v1.3"}));
    }

    SUBCASE("a registry that refuses leaves us unregistered") {
        FakeRegistry registry(answer("500 Internal Server Error"));
        REQUIRE(registry.start());

        NMOSRegistrationClient client(testNode());
        CHECK_FALSE(client.registerWith({"127.0.0.1", registry.port(), "v1.3"}));
        CHECK_FALSE(client.isRegistered());
    }

    SUBCASE("no registry there at all is a false, not a wait") {
        NMOSRegistrationClient client(testNode());
        CHECK_FALSE(client.registerWith({"127.0.0.1", 1, "v1.3"}));
    }

    SUBCASE("a registry that was never given is refused before any socket") {
        NMOSRegistrationClient client(testNode());
        CHECK_FALSE(client.heartbeat());
        CHECK_FALSE(client.unregister());
    }
}

TEST_CASE("The Heartbeat Goes To The Node's Own Health Endpoint") {
    FakeRegistry registry(answer("200 OK"));
    REQUIRE(registry.start());

    NMOSRegistrationClient client(testNode());
    REQUIRE(client.registerWith({"127.0.0.1", registry.port(), "v1.3"}));
    CHECK(client.heartbeat());

    const auto requests = registry.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[1].find(
              "POST /x-nmos/registration/v1.3/health/nodes/"
              "8c4a5f2e-0000-4000-8000-000000000001") == 0);
}

TEST_CASE("A 404 On The Heartbeat Registers Again") {
    // What a registry answers once it has garbage-collected a node that
    // went quiet. Registering again is the documented way back in, and
    // the alternative — treating it as a failure — leaves this driver
    // invisible to every controller until it is restarted.
    FakeRegistry registry(answer("404 Not Found"));
    REQUIRE(registry.start());

    NMOSRegistrationClient client(testNode());
    client.registerWith({"127.0.0.1", registry.port(), "v1.3"});
    client.heartbeat();

    const auto requests = registry.requests();
    REQUIRE(requests.size() >= 3);
    // The first POST, the heartbeat that came back 404, and the
    // registration it triggered.
    CHECK(requests[1].find("/health/nodes/") != std::string::npos);
    CHECK(requests[2].find("POST /x-nmos/registration/v1.3/resource") == 0);
}

TEST_CASE("Unregistering Deletes The Node") {
    FakeRegistry registry(answer("204 No Content"));
    REQUIRE(registry.start());

    NMOSRegistrationClient client(testNode());
    client.registerWith({"127.0.0.1", registry.port(), "v1.3"});
    CHECK(client.unregister());
    CHECK_FALSE(client.isRegistered());

    const auto requests = registry.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[1].find("DELETE /x-nmos/registration/v1.3/resource/nodes/"
                           "8c4a5f2e-0000-4000-8000-000000000001") == 0);
}

TEST_CASE("Stopping Without Starting Is Safe, And So Is Stopping Twice") {
    NMOSRegistrationClient client(testNode());
    client.stop();
    client.startHeartbeats();
    client.stop();
    client.stop();
}

TEST_CASE("Derived ids are stable, distinct, and version 5") {
    const std::string node = "8c4a5f2e-0000-4000-8000-000000000001";

    const std::string device = NMOSRegistrationClient::deriveId(node, "device");
    CHECK(device.size() == 36);
    CHECK(device[14] == '5');
    CHECK(std::string("89ab").find(device[19]) != std::string::npos);

    // The same input gives the same id, which is the whole point: a
    // registry keyed by fresh ids each launch accumulates copies of the
    // same stream.
    CHECK(NMOSRegistrationClient::deriveId(node, "device") == device);

    // Different names, different ids; and the same name under another
    // node is another id.
    CHECK(NMOSRegistrationClient::deriveId(node, "sender:Studio Mic 1") != device);
    CHECK(NMOSRegistrationClient::deriveId(node, "sender:Studio Mic 1") !=
          NMOSRegistrationClient::deriveId(node, "sender:Studio Mic 2"));
    CHECK(NMOSRegistrationClient::deriveId("8c4a5f2e-0000-4000-8000-000000000002", "device") !=
          device);
}

TEST_CASE("The resource bodies say what IS-04 expects") {
    NMOSSenderResource sender;
    sender.name = "Studio Mic 1";
    sender.multicastAddress = "239.69.0.1";
    sender.port = 5004;
    sender.sampleRate = 48000;
    sender.channels = 4;
    sender.encoding = "L24";

    SUBCASE("device") {
        const std::string body = NMOSRegistrationClient::buildDeviceBody(
            "dev-id", "node-id", "Studio Mac", {"snd-1"}, {"rcv-1"}, 1756000000, 0);
        CHECK(body.find("\"type\": \"device\"") != std::string::npos);
        CHECK(body.find("\"type\": \"urn:x-nmos:device:audio\"") != std::string::npos);
        CHECK(body.find("\"node_id\": \"node-id\"") != std::string::npos);
        CHECK(body.find("\"senders\": [\"snd-1\"]") != std::string::npos);
        CHECK(body.find("\"receivers\": [\"rcv-1\"]") != std::string::npos);
        // No IS-05 here yet, and an empty list is how that is said.
        CHECK(body.find("\"controls\": []") != std::string::npos);
    }

    SUBCASE("source names its channels") {
        const std::string body = NMOSRegistrationClient::buildSourceBody(
            "src-id", "dev-id", sender, 1756000000, 0);
        CHECK(body.find("\"format\": \"urn:x-nmos:format:audio\"") != std::string::npos);
        CHECK(body.find("\"device_id\": \"dev-id\"") != std::string::npos);
        CHECK(body.find("\"clock_name\": \"clk0\"") != std::string::npos);
        CHECK(body.find("Channel 1") != std::string::npos);
        CHECK(body.find("Channel 4") != std::string::npos);
        CHECK(body.find("Channel 5") == std::string::npos);
    }

    SUBCASE("flow carries the media type and the depth that goes with it") {
        const std::string l24 = NMOSRegistrationClient::buildFlowBody(
            "flow-id", "src-id", "dev-id", sender, 1756000000, 0);
        CHECK(l24.find("\"media_type\": \"audio/L24\"") != std::string::npos);
        CHECK(l24.find("\"bit_depth\": 24") != std::string::npos);
        CHECK(l24.find("\"numerator\": 48000") != std::string::npos);

        sender.encoding = "L16";
        const std::string l16 = NMOSRegistrationClient::buildFlowBody(
            "flow-id", "src-id", "dev-id", sender, 1756000000, 0);
        CHECK(l16.find("\"media_type\": \"audio/L16\"") != std::string::npos);
        CHECK(l16.find("\"bit_depth\": 16") != std::string::npos);
    }

    SUBCASE("sender names its flow and admits it has no HTTP manifest") {
        const std::string body = NMOSRegistrationClient::buildSenderBody(
            "snd-id", "flow-id", "dev-id", sender, 1756000000, 0);
        CHECK(body.find("\"flow_id\": \"flow-id\"") != std::string::npos);
        CHECK(body.find("\"transport\": \"urn:x-nmos:transport:rtp.mcast\"") !=
              std::string::npos);
        // The SDP is served over RTSP DESCRIBE, and manifest_href names an
        // HTTP URL: null beats pointing at something that will 404.
        CHECK(body.find("\"manifest_href\": null") != std::string::npos);
    }

    SUBCASE("receiver advertises what the RTP path can decode") {
        NMOSReceiverResource receiver;
        receiver.name = "Desk Return";
        receiver.active = true;
        const std::string body = NMOSRegistrationClient::buildReceiverBody(
            "rcv-id", "dev-id", receiver, 1756000000, 0);
        CHECK(body.find("\"media_types\": [\"audio/L16\", \"audio/L24\"]") !=
              std::string::npos);
        CHECK(body.find("\"active\": true") != std::string::npos);
    }
}

TEST_CASE("A sync posts the whole tree, and the next one takes away what went") {
    FakeRegistry registry(answer("201 Created"));
    REQUIRE(registry.start());

    NMOSRegistrationClient client(testNode());
    REQUIRE(client.registerWith({"127.0.0.1", registry.port(), "v1.3"}));

    NMOSSenderResource first;
    first.name = "Studio Mic 1";
    first.channels = 2;
    NMOSSenderResource second;
    second.name = "Studio Mic 2";
    second.channels = 2;
    NMOSReceiverResource receiver;
    receiver.name = "Desk Return";

    CHECK(client.syncResources({first, second}, {receiver}));

    auto requests = registry.requests();
    // The node, the device, source/flow/sender twice, and the receiver:
    // nine in all, with the device before anything it names.
    REQUIRE(requests.size() == 9);
    CHECK(requests[1].find("\"type\": \"device\"") != std::string::npos);
    CHECK(requests[2].find("\"type\": \"source\"") != std::string::npos);
    CHECK(requests[3].find("\"type\": \"flow\"") != std::string::npos);
    CHECK(requests[4].find("\"type\": \"sender\"") != std::string::npos);
    CHECK(requests[5].find("\"type\": \"source\"") != std::string::npos);
    CHECK(requests[8].find("\"type\": \"receiver\"") != std::string::npos);

    // One stream goes away: what the registry still holds for it has to
    // go with it, or a controller is sent after a stream nobody sends.
    CHECK(client.syncResources({first}, {receiver}));

    requests = registry.requests();
    std::string deletes;
    for (const std::string& request : requests) {
        if (request.rfind("DELETE ", 0) == 0) deletes += request.substr(0, request.find(' ', 7));
    }
    CHECK(deletes.find("/sources/") != std::string::npos);
    CHECK(deletes.find("/flows/") != std::string::npos);
    CHECK(deletes.find("/senders/") != std::string::npos);
    // The receiver stayed, so nothing of it was removed.
    CHECK(deletes.find("/receivers/") == std::string::npos);
}

TEST_CASE("Unregistering takes the tree down before the node") {
    FakeRegistry registry(answer("204 No Content"));
    REQUIRE(registry.start());

    NMOSRegistrationClient client(testNode());
    client.registerWith({"127.0.0.1", registry.port(), "v1.3"});

    NMOSSenderResource sender;
    sender.name = "Studio Mic 1";
    client.syncResources({sender}, {});

    CHECK(client.unregister());

    const auto requests = registry.requests();
    size_t nodeDelete = 0;
    size_t senderDelete = 0;
    for (size_t i = 0; i < requests.size(); i++) {
        if (requests[i].find("DELETE") == 0 && requests[i].find("/nodes/") != std::string::npos) {
            nodeDelete = i;
        }
        if (requests[i].find("DELETE") == 0 && requests[i].find("/senders/") != std::string::npos) {
            senderDelete = i;
        }
    }
    CHECK(senderDelete > 0);
    CHECK(senderDelete < nodeDelete);
}
