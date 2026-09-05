//
// TestConnectionAPI.cpp
// AES67 macOS Driver
//
// The IS-05 Connection API: what a controller sees, and what happens when
// it patches.
//
// The routing is exercised directly and the server over a real loopback
// socket, because the two can disagree: a route that answers correctly
// and a server that never reaches it is a control that does not control.
//
// The refusals matter as much as the answers. A sender that accepted a
// patch and did nothing, or an `active` endpoint that let itself be
// written, would each look like a working connection to a controller and
// leave the audio where it was.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/ConnectionAPIServer.h"
#include "NetworkEngine/Discovery/HTTPClient.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sstream>
#include <string>
#include <vector>

using namespace AES67;

namespace {

const std::string kSenderId = "11111111-2222-4333-8444-555555555555";
const std::string kReceiverId = "66666666-7777-4888-8999-aaaaaaaaaaaa";

const std::string kSDP =
    "v=0\r\n"
    "o=- 1 1 IN IP4 192.168.0.16\r\n"
    "s=Studio Mic 1\r\n"
    "c=IN IP4 239.69.0.1/32\r\n"
    "t=0 0\r\n"
    "m=audio 5004 RTP/AVP 96\r\n"
    "a=rtpmap:96 L24/48000/2\r\n";

ConnectionSender testSender() {
    ConnectionSender sender;
    sender.id = kSenderId;
    sender.label = "Studio Mic 1";
    sender.multicastAddress = "239.69.0.1";
    sender.port = 5004;
    sender.sourceAddress = "192.168.0.16";
    sender.sdp = kSDP;
    return sender;
}

ConnectionReceiver testReceiver() {
    ConnectionReceiver receiver;
    receiver.id = kReceiverId;
    receiver.label = "Desk Return";
    receiver.multicastAddress = "239.69.0.9";
    receiver.port = 5004;
    return receiver;
}

/// A server wired to the two lists and a patcher that records what it was
/// asked for.
struct Fixture {
    ConnectionAPIServer server{0};
    std::vector<std::pair<std::string, ConnectionPatch>> patches;
    bool patchAnswer{true};

    bool start() {
        return server.start([] { return std::vector<ConnectionSender>{testSender()}; },
                            [] { return std::vector<ConnectionReceiver>{testReceiver()}; },
                            [this](const std::string& id, const ConnectionPatch& patch) {
                                patches.emplace_back(id, patch);
                                return patchAnswer;
                            });
    }
};

std::string base() { return std::string("/x-nmos/connection/v1.1"); }

} // namespace

TEST_CASE("The tree a controller walks") {
    Fixture fixture;
    REQUIRE(fixture.start());
    const ConnectionAPIServer& api = fixture.server;

    CHECK(api.route("GET", base(), "").body.find("single/") != std::string::npos);
    CHECK(api.route("GET", base() + "/single", "").body.find("receivers/") != std::string::npos);
    CHECK(api.route("GET", base() + "/single/senders", "").body.find(kSenderId) !=
          std::string::npos);
    CHECK(api.route("GET", base() + "/single/receivers", "").body.find(kReceiverId) !=
          std::string::npos);

    const auto leaves = api.route("GET", base() + "/single/senders/" + kSenderId, "");
    CHECK(leaves.body.find("constraints/") != std::string::npos);
    CHECK(leaves.body.find("transportfile/") != std::string::npos);

    // A receiver has no transport file: it is not the one describing a
    // stream.
    const auto receiverLeaves = api.route("GET", base() + "/single/receivers/" + kReceiverId, "");
    CHECK(receiverLeaves.body.find("transportfile/") == std::string::npos);
}

TEST_CASE("What is not there answers, rather than hanging or guessing") {
    Fixture fixture;
    REQUIRE(fixture.start());
    const ConnectionAPIServer& api = fixture.server;

    CHECK(api.route("GET", "/x-nmos/connection/v1.0/single/senders", "").status == 404);
    CHECK(api.route("GET", base() + "/single/senders/nope", "").status == 404);
    CHECK(api.route("GET", base() + "/single/nonsense", "").status == 404);
    CHECK(api.route("GET", "/x-nmos/node/v1.3/self", "").status == 404);
    // Bulk staging is a real part of IS-05 that this does not serve.
    CHECK(api.route("PATCH", base() + "/bulk/receivers", "{}").status == 501);
}

TEST_CASE("A sender describes itself and refuses to be told what to do") {
    Fixture fixture;
    REQUIRE(fixture.start());
    const ConnectionAPIServer& api = fixture.server;

    const auto file = api.route("GET", base() + "/single/senders/" + kSenderId + "/transportfile", "");
    CHECK(file.status == 200);
    CHECK(file.contentType == "application/sdp");
    CHECK(file.body == kSDP);

    const auto type = api.route("GET", base() + "/single/senders/" + kSenderId + "/transporttype", "");
    CHECK(type.body == "\"urn:x-nmos:transport:rtp.mcast\"");

    const auto active = api.route("GET", base() + "/single/senders/" + kSenderId + "/active", "");
    CHECK(active.status == 200);
    CHECK(active.body.find("\"destination_port\": 5004") != std::string::npos);
    CHECK(active.body.find("239.69.0.1") != std::string::npos);

    // This driver's senders are configured through its own settings.
    // Accepting the patch and doing nothing would look like a connection.
    CHECK(api.route("PATCH", base() + "/single/senders/" + kSenderId + "/staged", "{}").status ==
          501);
    CHECK(fixture.patches.empty());
}

TEST_CASE("Patching a receiver reaches the driver") {
    Fixture fixture;
    REQUIRE(fixture.start());
    ConnectionAPIServer& api = fixture.server;

    const std::string patchBody =
        "{\n"
        "  \"master_enable\": true,\n"
        "  \"sender_id\": \"" + kSenderId + "\",\n"
        "  \"transport_params\": [{ \"multicast_ip\": \"239.69.0.1\", "
        "\"destination_port\": 5004, \"interface_ip\": \"192.168.0.20\" }],\n"
        "  \"transport_file\": { \"data\": \"v=0\\r\\n\", \"type\": \"application/sdp\" },\n"
        "  \"activation\": { \"mode\": \"activate_immediate\" }\n"
        "}\n";

    const auto reply = api.route("PATCH", base() + "/single/receivers/" + kReceiverId + "/staged",
                                 patchBody);
    CHECK(reply.status == 200);

    REQUIRE(fixture.patches.size() == 1);
    const auto& [id, patch] = fixture.patches[0];
    CHECK(id == kReceiverId);
    CHECK(patch.masterEnable.value_or(false) == true);
    CHECK(patch.senderId.value_or("") == kSenderId);
    CHECK(patch.multicastAddress.value_or("") == "239.69.0.1");
    CHECK(patch.port.value_or(0) == 5004);
    CHECK(patch.interfaceAddress.value_or("") == "192.168.0.20");
    CHECK(patch.transportFile.has_value());
    CHECK(patch.activateImmediate == true);

    // What comes back is what was asked for, which is what a controller
    // reads to confirm the staging.
    CHECK(reply.body.find("\"sender_id\": \"" + kSenderId + "\"") != std::string::npos);
    CHECK(reply.body.find("\"destination_port\": 5004") != std::string::npos);
}

TEST_CASE("A driver that refuses the patch is a 500, not a silent success") {
    Fixture fixture;
    fixture.patchAnswer = false;
    REQUIRE(fixture.start());

    const auto reply = fixture.server.route(
        "PATCH", base() + "/single/receivers/" + kReceiverId + "/staged", "{}");
    CHECK(reply.status == 500);
}

TEST_CASE("active reports and is never written") {
    Fixture fixture;
    REQUIRE(fixture.start());
    CHECK(fixture.server.route("PATCH", base() + "/single/receivers/" + kReceiverId + "/active",
                               "{}").status == 405);
    CHECK(fixture.patches.empty());
}

TEST_CASE("A partial patch stays partial") {
    // IS-05 patches are partial by design: a controller turning a receiver
    // off sends master_enable and nothing else, and reading absent fields
    // as zeroes would silently re-address the stream.
    const ConnectionPatch patch = ConnectionAPIServer::parsePatch("{ \"master_enable\": false }");
    CHECK(patch.masterEnable.value_or(true) == false);
    CHECK_FALSE(patch.senderId.has_value());
    CHECK_FALSE(patch.multicastAddress.has_value());
    CHECK_FALSE(patch.port.has_value());
    CHECK_FALSE(patch.transportFile.has_value());
    CHECK_FALSE(patch.activateImmediate);

    // A null sender_id is a controller disconnecting the receiver, which
    // is not the same as not mentioning it.
    const ConnectionPatch nulled = ConnectionAPIServer::parsePatch("{ \"sender_id\": null }");
    CHECK_FALSE(nulled.senderId.has_value());

    // Garbage where a number goes leaves the field absent rather than
    // taking a zero from it.
    const ConnectionPatch garbage =
        ConnectionAPIServer::parsePatch("{ \"destination_port\": \"soon\" }");
    CHECK_FALSE(garbage.port.has_value());
}

TEST_CASE("A transport file cannot forge the fields around it") {
    // `data` is an entire SDP chosen by the caller. While the fields were
    // located with a plain find() over the whole body, an SDP carrying the
    // text of another field had it read as the request's — so a controller
    // staging a transport file could flip master_enable, or a session name
    // could decide the activation mode (2026-09-04 audit).
    const std::string forged =
        "{\n"
        "  \"transport_file\": { \"data\": \"v=0\\r\\ns=\\\"master_enable\\\": true, "
        "\\\"mode\\\": \\\"activate_immediate\\\", \\\"destination_port\\\": 9999\\r\\n\", "
        "\"type\": \"application/sdp\" }\n"
        "}\n";
    const ConnectionPatch patch = ConnectionAPIServer::parsePatch(forged);
    CHECK(patch.transportFile.has_value());
    CHECK_FALSE(patch.masterEnable.has_value());
    CHECK_FALSE(patch.port.has_value());
    CHECK_FALSE(patch.activateImmediate);
}

TEST_CASE("Transport parameters are read from their own object") {
    // multicast_ip and friends belong to transport_params, not to the body:
    // a same-named field elsewhere is not the one that addresses the stream.
    const std::string body =
        "{\n"
        "  \"transport_file\": { \"data\": \"v=0\\r\\n\", \"type\": \"application/sdp\" },\n"
        "  \"transport_params\": [{ \"multicast_ip\": \"239.69.0.7\", "
        "\"destination_port\": 5004 }]\n"
        "}\n";
    const ConnectionPatch patch = ConnectionAPIServer::parsePatch(body);
    CHECK(patch.multicastAddress.value_or("") == "239.69.0.7");
    CHECK(patch.port.value_or(0) == 5004);
}

TEST_CASE("A header name is matched whatever its case") {
    // HTTP header names are case-insensitive (RFC 9110 5.1). Matching only
    // "Content-Length:" meant a body from a client that lowercases its
    // headers was never waited for, so a PATCH could be served with a
    // truncated body.
    Fixture fixture;
    REQUIRE(fixture.start());

    const std::string body = "{ \"master_enable\": true }";
    std::ostringstream request;
    request << "PATCH " << base() << "/single/receivers/" << kReceiverId << "/staged HTTP/1.1\r\n"
            << "host: 127.0.0.1\r\n"
            << "content-type: application/json\r\n"
            << "content-length: " << body.size() << "\r\n\r\n"
            << body;

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(fixture.server.boundPort());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    const std::string text = request.str();
    REQUIRE(::send(fd, text.data(), text.size(), 0) == static_cast<ssize_t>(text.size()));

    char answer[1024];
    const ssize_t got = ::recv(fd, answer, sizeof(answer) - 1, 0);
    ::close(fd);
    REQUIRE(got > 0);
    answer[got] = '\0';
    CHECK(std::string(answer).find("200 OK") != std::string::npos);

    REQUIRE(fixture.patches.size() == 1);
    CHECK(fixture.patches[0].second.masterEnable.value_or(false) == true);
}

TEST_CASE("The same answers come back over a real socket") {
    Fixture fixture;
    REQUIRE(fixture.start());
    REQUIRE(fixture.server.isRunning());
    REQUIRE(fixture.server.boundPort() != 0);

    HTTPClient client("127.0.0.1", fixture.server.boundPort());

    const HTTPResponse senders = client.get(base() + "/single/senders/");
    CHECK(senders.status == 200);
    CHECK(senders.body.find(kSenderId) != std::string::npos);

    const HTTPResponse file =
        client.get(base() + "/single/senders/" + kSenderId + "/transportfile/");
    CHECK(file.status == 200);
    CHECK(file.body == kSDP);

    const HTTPResponse patched =
        client.perform("PATCH", base() + "/single/receivers/" + kReceiverId + "/staged/",
                       "{ \"master_enable\": false }", "application/json");
    CHECK(patched.status == 200);
    REQUIRE(fixture.patches.size() == 1);
    CHECK(fixture.patches[0].second.masterEnable.value_or(true) == false);

    const HTTPResponse missing = client.get(base() + "/single/receivers/nope/staged/");
    CHECK(missing.status == 404);

    // The control href is what the IS-04 device advertises, and it has to
    // name the port that was actually bound.
    CHECK(fixture.server.controlHref("studio.local") ==
          "http://studio.local:" + std::to_string(fixture.server.boundPort()) +
              "/x-nmos/connection/v1.1/");

    fixture.server.stop();
    CHECK_FALSE(fixture.server.isRunning());
}
