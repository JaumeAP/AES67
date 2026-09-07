//
// TestConnectionApi.cpp
// AES67 RAVENNA session layer
// IS-05: staging a transport file onto a receiver and activating it, which is
// the exchange that assigns one device's stream to another's channels.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Ravenna/ConnectionApi.h"
#include "Ravenna/HttpServer.h"

using namespace AES67::Ravenna;

namespace {

const std::string kSdp =
    "v=0\r\no=- 1 1 IN IP4 192.168.1.50\r\ns=Mix A\r\nc=IN IP4 239.69.1.10/32\r\n"
    "t=0 0\r\nm=audio 5004 RTP/AVP 96\r\na=rtpmap:96 L24/48000/2\r\n";

ConnectionApi apiWithOne() {
    ConnectionApi api;

    ConnectionSender sender;
    sender.id = "sender-1";
    sender.label = "Mix A";
    sender.sdp = kSdp;
    api.addSender(sender);

    ConnectionReceiver receiver;
    receiver.id = "receiver-1";
    receiver.label = "Inputs 1-2";
    api.addReceiver(receiver);

    return api;
}

JsonValue bodyOf(const ApiResponse& response) {
    JsonValue value;
    std::string error;
    parseJson(response.body, value, error);
    return value;
}

std::string path(const std::string& tail) {
    return std::string(kConnectionApiRoot) + tail;
}

}  // namespace

TEST_CASE("The API answers where a controller starts looking") {
    ConnectionApi api = apiWithOne();

    CHECK(api.handle("GET", path("/"), "").status == 200);
    CHECK(bodyOf(api.handle("GET", path("/"), "")).asArray().size() == 2);

    const JsonValue single = bodyOf(api.handle("GET", path("/single/"), ""));
    REQUIRE(single.isArray());
    CHECK(single.asArray().size() == 2);

    const JsonValue senders = bodyOf(api.handle("GET", path("/single/senders/"), ""));
    REQUIRE(senders.isArray());
    REQUIRE(senders.asArray().size() == 1);
    CHECK(senders.asArray()[0].asString() == "sender-1/");
}

TEST_CASE("A sender serves its own SDP as the transport file") {
    ConnectionApi api = apiWithOne();

    const ApiResponse response =
        api.handle("GET", path("/single/senders/sender-1/transportfile/"), "");
    CHECK(response.status == 200);
    CHECK(response.contentType == "application/sdp");
    CHECK(response.body == kSdp);

    CHECK(bodyOf(api.handle("GET", path("/single/senders/sender-1/transporttype/"), ""))
              .asString() == kTransportRtpMulticast);
}

TEST_CASE("A receiver has no transport file to serve") {
    ConnectionApi api = apiWithOne();
    CHECK(api.handle("GET", path("/single/receivers/receiver-1/transportfile/"), "").status == 404);
}

TEST_CASE("Staging a transport file does not start anything") {
    ConnectionApi api = apiWithOne();
    bool activated = false;
    api.onReceiverActivation([&](const std::string&, const std::string&, bool, std::string&) {
        activated = true;
        return true;
    });

    const std::string body =
        R"({"master_enable":true,"transport_file":{"data":)" +
        jsonQuote(kSdp) + R"(,"type":"application/sdp"}})";

    const ApiResponse response =
        api.handle("PATCH", path("/single/receivers/receiver-1/staged/"), body);
    CHECK(response.status == 200);
    CHECK(activated == false);

    // Staged holds it; active has nothing yet.
    CHECK(bodyOf(response)["transport_file"]["data"].asString() == kSdp);
    const JsonValue active = bodyOf(api.handle("GET", path("/single/receivers/receiver-1/active/"), ""));
    CHECK(active["master_enable"].asBool() == false);
    CHECK(active["transport_file"]["data"].isNull());
}

TEST_CASE("Activating it is what hands the stream to the device") {
    ConnectionApi api = apiWithOne();

    std::string handedSdp;
    bool handedEnable = false;
    api.onReceiverActivation([&](const std::string& id, const std::string& sdp, bool enable,
                                 std::string&) {
        CHECK(id == "receiver-1");
        handedSdp = sdp;
        handedEnable = enable;
        return true;
    });

    const std::string body =
        R"({"master_enable":true,"transport_file":{"data":)" + jsonQuote(kSdp) +
        R"(,"type":"application/sdp"},"activation":{"mode":"activate_immediate"}})";

    const ApiResponse response =
        api.handle("PATCH", path("/single/receivers/receiver-1/staged/"), body);
    REQUIRE(response.status == 200);

    CHECK(handedSdp == kSdp);
    CHECK(handedEnable);

    const JsonValue active = bodyOf(api.handle("GET", path("/single/receivers/receiver-1/active/"), ""));
    CHECK(active["master_enable"].asBool());
    CHECK(active["transport_file"]["data"].asString() == kSdp);
    CHECK(active["activation"]["mode"].asString() == "activate_immediate");
    CHECK(active["activation"]["activation_time"].isNull() == false);

    // And staged no longer carries an activation waiting to happen again.
    const JsonValue staged = bodyOf(api.handle("GET", path("/single/receivers/receiver-1/staged/"), ""));
    CHECK(staged["activation"]["mode"].isNull());
}

TEST_CASE("A receiver that refuses the stream is not left saying it took it") {
    ConnectionApi api = apiWithOne();
    api.onReceiverActivation([](const std::string&, const std::string&, bool, std::string& why) {
        why = "no free device channels";
        return false;
    });

    const std::string body =
        R"({"master_enable":true,"transport_file":{"data":)" + jsonQuote(kSdp) +
        R"(,"type":"application/sdp"},"activation":{"mode":"activate_immediate"}})";

    const ApiResponse response =
        api.handle("PATCH", path("/single/receivers/receiver-1/staged/"), body);
    CHECK(response.status == 400);
    CHECK(response.body.find("no free device channels") != std::string::npos);

    const JsonValue active = bodyOf(api.handle("GET", path("/single/receivers/receiver-1/active/"), ""));
    CHECK(active["master_enable"].asBool() == false);
}

TEST_CASE("Enabling a receiver with no transport file is refused") {
    ConnectionApi api = apiWithOne();
    const ApiResponse response =
        api.handle("PATCH", path("/single/receivers/receiver-1/staged/"),
                   R"({"master_enable":true,"activation":{"mode":"activate_immediate"}})");
    CHECK(response.status == 400);
}

TEST_CASE("A transport file that is not SDP is refused before the wire") {
    ConnectionApi api = apiWithOne();
    const ApiResponse response =
        api.handle("PATCH", path("/single/receivers/receiver-1/staged/"),
                   R"({"transport_file":{"data":"whatever","type":"application/json"}})");
    CHECK(response.status == 400);
    CHECK(response.body.find("application/sdp") != std::string::npos);
}

TEST_CASE("A scheduled activation is refused rather than forgotten") {
    // Accepting one and never acting on it is a stream that a controller
    // believes it has connected and nobody is sending.
    ConnectionApi api = apiWithOne();
    const ApiResponse response =
        api.handle("PATCH", path("/single/receivers/receiver-1/staged/"),
                   R"({"activation":{"mode":"activate_scheduled_absolute"}})");
    CHECK(response.status == 501);
}

TEST_CASE("Active is read-only and bulk says it is not implemented") {
    ConnectionApi api = apiWithOne();
    CHECK(api.handle("PATCH", path("/single/receivers/receiver-1/active/"), "{}").status == 405);
    CHECK(api.handle("POST", path("/bulk/receivers"), "[]").status == 501);
}

TEST_CASE("What does not exist is a 404, and it says which") {
    ConnectionApi api = apiWithOne();
    CHECK(api.handle("GET", path("/single/receivers/nope/staged/"), "").status == 404);
    CHECK(api.handle("GET", path("/single/nonsense/"), "").status == 404);
    CHECK(api.handle("GET", "/x-nmos/node/v1.3/", "").status == 404);
}

TEST_CASE("A body that is not JSON is refused with the reason") {
    ConnectionApi api = apiWithOne();
    const ApiResponse response =
        api.handle("PATCH", path("/single/receivers/receiver-1/staged/"), "{not json");
    CHECK(response.status == 400);
    CHECK(response.body.find("not JSON") != std::string::npos);
}

TEST_CASE("A percent-encoded path segment reaches the resource it names") {
    // A client is entitled to escape any character of a path segment, and
    // several do. Comparing the raw segment makes every one of those a 404 for
    // a resource that is right there.
    ConnectionApi api = apiWithOne();

    std::string method;
    std::string decoded;
    std::string body;
    REQUIRE(parseHttpRequest(
        "GET /x-nmos/connection/v1.1/single/senders/sender%2D1/transportfile/ HTTP/1.1\r\n"
        "Host: box.local\r\n\r\n",
        method, decoded, body));
    CHECK(decoded == path("/single/senders/sender-1/transportfile/"));

    const ApiResponse answer = api.handle(method, decoded, body);
    CHECK(answer.status == 200);
    CHECK(answer.contentType == "application/sdp");
}

TEST_CASE("Decoding a path leaves the path alone") {
    std::string method;
    std::string decoded;
    std::string body;

    // A space, which is the escape a two-word name produces.
    REQUIRE(parseHttpRequest("GET /single/senders/Mix%20A/ HTTP/1.1\r\n\r\n", method, decoded,
                             body));
    CHECK(decoded == "/single/senders/Mix A/");

    // Lower case hex is the same escape.
    REQUIRE(parseHttpRequest("GET /single/senders/Mix%2fA/ HTTP/1.1\r\n\r\n", method, decoded,
                             body));
    CHECK(decoded == "/single/senders/Mix%2fA/");

    // A percent that is not an escape stays a percent rather than eating what
    // follows it: this reads paths off the network.
    REQUIRE(parseHttpRequest("GET /100%/%zz/% HTTP/1.1\r\n\r\n", method, decoded, body));
    CHECK(decoded == "/100%/%zz/%");
}
