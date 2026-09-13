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
              .asString() == kTransportRtp);
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
    // The controller asked for something legal and this host could not do
    // it, which is a 500 and not a 400: a 400 would send a controller off
    // rewriting a request that was never wrong.
    CHECK(response.status == 500);
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

TEST_CASE("A scheduled activation needs a time, and a mode has to be one of the three") {
    ConnectionApi api = apiWithOne();

    // Accepting a schedule with no time and never acting on it is a stream a
    // controller believes it has connected and nobody is sending.
    CHECK(api.handle("PATCH", path("/single/receivers/receiver-1/staged/"),
                     R"({"activation":{"mode":"activate_scheduled_absolute"}})")
              .status == 400);
    CHECK(api.handle("PATCH", path("/single/receivers/receiver-1/staged/"),
                     R"({"activation":{"mode":"activate_scheduled_relative","requested_time":"soon"}})")
              .status == 400);
    CHECK(api.handle("PATCH", path("/single/receivers/receiver-1/staged/"),
                     R"({"activation":{"mode":"activate_eventually"}})")
              .status == 400);
}

TEST_CASE("Active is read-only, and bulk takes a POST and nothing else") {
    ConnectionApi api = apiWithOne();
    CHECK(api.handle("PATCH", path("/single/receivers/receiver-1/active/"), "{}").status == 405);

    // A bulk request is a batch of patches, not a resource: there is nothing
    // to GET, and 405 says so on a resource that is plainly there.
    CHECK(api.handle("GET", path("/bulk/senders"), "").status == 405);
    CHECK(api.handle("GET", path("/bulk/receivers"), "").status == 405);
    // The listing above them is a listing.
    CHECK(api.handle("GET", path("/bulk/"), "").status == 200);
    CHECK(api.handle("POST", path("/bulk/receivers"), "[]").status == 200);
    CHECK(api.handle("POST", path("/bulk/senders"), "[]").status == 200);
}

TEST_CASE("A leg's constraints name the parameters that leg has") {
    // IS-05 SS 4.2: a controller reads this to know what it may stage, and an
    // empty object told it there was nothing to stage at all.
    ConnectionApi api = apiWithOne();
    const ApiResponse receiver =
        api.handle("GET", path("/single/receivers/receiver-1/constraints/"), "");
    CHECK(receiver.status == 200);
    CHECK(receiver.body.find("destination_port") != std::string::npos);
    CHECK(receiver.body.find("multicast_ip") != std::string::npos);

    const ApiResponse sender =
        api.handle("GET", path("/single/senders/sender-1/constraints/"), "");
    CHECK(sender.status == 200);
    CHECK(sender.body.find("destination_port") != std::string::npos);
    CHECK(sender.body.find("destination_ip") != std::string::npos);
}

TEST_CASE("A sender's state is a sender's shape, not a receiver's") {
    // The schemas differ and the suite checks both: a sender carries
    // receiver_id and no transport file, a receiver carries sender_id and the
    // file it was given.
    ConnectionApi api = apiWithOne();
    const ApiResponse sender = api.handle("GET", path("/single/senders/sender-1/staged/"), "");
    CHECK(sender.body.find("receiver_id") != std::string::npos);
    CHECK(sender.body.find("transport_file") == std::string::npos);

    const ApiResponse receiver = api.handle("GET", path("/single/receivers/receiver-1/staged/"), "");
    CHECK(receiver.body.find("sender_id") != std::string::npos);
    CHECK(receiver.body.find("transport_file") != std::string::npos);
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

TEST_CASE("A receiver reports which sender it was connected to") {
    // IS-05 sec 6: a receiver's staged and active carry sender_id, and it is
    // the only place a controller can read back what a crosspoint was set to.
    // Accepting it in the PATCH and never reporting it leaves every route
    // looking unmade.
    ConnectionApi api = apiWithOne();

    JsonObject file;
    file["data"] = JsonValue(kSdp);
    file["type"] = JsonValue("application/sdp");
    JsonObject connect;
    connect["sender_id"] = JsonValue("5cd71600-d803-50aa-a55c-d2489b9545e9");
    connect["master_enable"] = JsonValue(true);
    connect["transport_file"] = JsonValue(file);
    connect["activation"] = JsonValue(JsonObject{{"mode", JsonValue("activate_immediate")}});

    const ApiResponse staged = api.handle(
        "PATCH", path("/single/receivers/receiver-1/staged/"), JsonValue(connect).serialise());
    REQUIRE(staged.status == 200);

    const JsonValue active = bodyOf(api.handle("GET", path("/single/receivers/receiver-1/active/"), ""));
    CHECK(active["master_enable"].asBool() == true);
    CHECK(active["sender_id"].asString() == "5cd71600-d803-50aa-a55c-d2489b9545e9");

    // Disconnecting clears it, so nothing reads as routed to a sender it no
    // longer takes.
    JsonObject disconnect;
    disconnect["sender_id"] = JsonValue();
    disconnect["master_enable"] = JsonValue(false);
    disconnect["activation"] = JsonValue(JsonObject{{"mode", JsonValue("activate_immediate")}});
    REQUIRE(api.handle("PATCH", path("/single/receivers/receiver-1/staged/"),
                       JsonValue(disconnect).serialise())
                .status == 200);

    const JsonValue cleared = bodyOf(api.handle("GET", path("/single/receivers/receiver-1/active/"), ""));
    CHECK(cleared["master_enable"].asBool() == false);
    CHECK(cleared["sender_id"].isNull());

    // A sender_id that is neither a string nor null is a controller in error.
    CHECK(api.handle("PATCH", path("/single/receivers/receiver-1/staged/"),
                     R"({"sender_id":7})")
              .status == 400);
}

TEST_CASE("A sender's transport parameters are the ones its own SDP announces") {
    // IS-05 sec 4: transport_params is what a controller reads to see where a
    // stream actually goes. An empty object there is schema-valid and useless,
    // and the suite reads these back after every connection it makes.
    ConnectionApi api = apiWithOne();

    const JsonValue staged = bodyOf(api.handle("GET", path("/single/senders/sender-1/staged/"), ""));
    REQUIRE(staged["transport_params"].isArray());
    REQUIRE(staged["transport_params"].asArray().size() == 1);

    const JsonValue& leg = staged["transport_params"].asArray().front();
    CHECK(leg["source_ip"].asString() == "192.168.1.50");       // o=
    CHECK(leg["destination_ip"].asString() == "239.69.1.10");   // c=
    CHECK(leg["destination_port"].asNumber() == 5004);          // m=
    // Not in an SDP and not a controller's to choose here.
    CHECK(leg["source_port"].asString() == "auto");
    // Whether the leg carries RTP, which it does. Whether the sender is
    // switched on is master_enable, a separate field, and reporting one as
    // the other told a controller a live leg was dead.
    CHECK(leg["rtp_enabled"].asBool() == true);
}

TEST_CASE("A receiver's transport parameters come from the file it was given") {
    ConnectionApi api = apiWithOne();

    // Before anything is staged there is nothing to report, and null is how
    // the schema says so.
    const JsonValue empty = bodyOf(api.handle("GET", path("/single/receivers/receiver-1/staged/"), ""));
    CHECK(empty["transport_params"].asArray().front()["multicast_ip"].isNull());

    JsonObject file;
    file["data"] = JsonValue(kSdp);
    file["type"] = JsonValue("application/sdp");
    JsonObject connect;
    connect["master_enable"] = JsonValue(true);
    connect["transport_file"] = JsonValue(file);
    connect["activation"] = JsonValue(JsonObject{{"mode", JsonValue("activate_immediate")}});
    REQUIRE(api.handle("PATCH", path("/single/receivers/receiver-1/staged/"),
                       JsonValue(connect).serialise())
                .status == 200);

    const JsonValue active = bodyOf(api.handle("GET", path("/single/receivers/receiver-1/active/"), ""));
    const JsonValue& leg = active["transport_params"].asArray().front();
    CHECK(leg["multicast_ip"].asString() == "239.69.1.10");
    CHECK(leg["destination_port"].asNumber() == 5004);
    // Activated, so the interface is the one actually in use rather than the
    // request to pick one.
    CHECK(leg["interface_ip"].asString() == "0.0.0.0");
    CHECK(leg["rtp_enabled"].asBool() == true);
    // This SDP names no a=source-filter, so the receiver takes the group from
    // any source and says so.
    CHECK(leg["source_ip"].isNull());
}

TEST_CASE("What a controller fixes wins over the transport file, and null gives it back") {
    ConnectionApi api = apiWithOne();

    JsonObject file;
    file["data"] = JsonValue(kSdp);
    file["type"] = JsonValue("application/sdp");
    REQUIRE(api.handle("PATCH", path("/single/receivers/receiver-1/staged/"),
                       JsonValue(JsonObject{{"transport_file", JsonValue(file)}}).serialise())
                .status == 200);

    const ApiResponse fixed =
        api.handle("PATCH", path("/single/receivers/receiver-1/staged/"),
                   R"({"transport_params":[{"destination_port":5006,"interface_ip":"10.0.0.7"}]})");
    REQUIRE(fixed.status == 200);
    const JsonValue leg = bodyOf(fixed)["transport_params"].asArray().front();
    CHECK(leg["destination_port"].asNumber() == 5006);
    CHECK(leg["interface_ip"].asString() == "10.0.0.7");
    // Untouched, so still the file's.
    CHECK(leg["multicast_ip"].asString() == "239.69.1.10");

    const ApiResponse released =
        api.handle("PATCH", path("/single/receivers/receiver-1/staged/"),
                   R"({"transport_params":[{"destination_port":null}]})");
    REQUIRE(released.status == 200);
    CHECK(bodyOf(released)["transport_params"].asArray().front()["destination_port"].asNumber() ==
          5004);
}

TEST_CASE("A transport_params a controller cannot have meant is a 400") {
    ConnectionApi api = apiWithOne();
    const std::string receiver = path("/single/receivers/receiver-1/staged/");
    const std::string sender = path("/single/senders/sender-1/staged/");

    // Not an array, and one leg is what this device has.
    CHECK(api.handle("PATCH", receiver, R"({"transport_params":{}})").status == 400);
    CHECK(api.handle("PATCH", receiver, R"({"transport_params":[{},{}]})").status == 400);
    CHECK(api.handle("PATCH", receiver, R"({"transport_params":[7]})").status == 400);

    // A name this transport does not have, and one that belongs to the other
    // side: constraints publishes the list and a PATCH may not go past it.
    CHECK(api.handle("PATCH", receiver, R"({"transport_params":[{"bit_rate":3}]})").status == 400);
    CHECK(api.handle("PATCH", receiver, R"({"transport_params":[{"destination_ip":"239.1.1.1"}]})")
              .status == 400);
    CHECK(api.handle("PATCH", sender, R"({"transport_params":[{"multicast_ip":"239.1.1.1"}]})")
              .status == 400);

    // Right names, wrong values.
    CHECK(api.handle("PATCH", receiver, R"({"transport_params":[{"rtp_enabled":"yes"}]})").status ==
          400);
    CHECK(api.handle("PATCH", receiver, R"({"transport_params":[{"destination_port":70000}]})")
              .status == 400);
    CHECK(api.handle("PATCH", receiver, R"({"transport_params":[{"interface_ip":7}]})").status ==
          400);

    // "auto" is a port: it asks the device to pick.
    CHECK(api.handle("PATCH", receiver, R"({"transport_params":[{"destination_port":"auto"}]})")
              .status == 200);

    // A leg with one bad name changes nothing, not even the names beside it.
    CHECK(api.handle("PATCH", receiver,
                     R"({"transport_params":[{"interface_ip":"10.0.0.7","bit_rate":3}]})")
              .status == 400);
    const JsonValue staged = bodyOf(api.handle("GET", receiver, ""));
    CHECK(staged["transport_params"].asArray().front()["interface_ip"].asString() == "auto");
}

TEST_CASE("A sender reports, and takes, which receiver asked for it") {
    // IS-05 sec 6: receiver_id on a sender is the other half of sender_id on a
    // receiver. It was reported from the receiver's own field, so every sender
    // read back as subscribed to itself, and no PATCH could set it.
    ConnectionApi api = apiWithOne();
    const std::string sender = path("/single/senders/sender-1/staged/");

    CHECK(bodyOf(api.handle("GET", sender, ""))["receiver_id"].isNull());

    const ApiResponse set = api.handle(
        "PATCH", sender, R"({"receiver_id":"a3b2c1d0-0000-5000-8000-000000000001"})");
    REQUIRE(set.status == 200);
    CHECK(bodyOf(set)["receiver_id"].asString() == "a3b2c1d0-0000-5000-8000-000000000001");

    // And it survives an activation, which is where a controller reads it.
    REQUIRE(api.handle("PATCH", sender,
                       R"({"activation":{"mode":"activate_immediate"}})")
                .status == 200);
    CHECK(bodyOf(api.handle("GET", path("/single/senders/sender-1/active/"), ""))["receiver_id"]
              .asString() == "a3b2c1d0-0000-5000-8000-000000000001");

    // Null is how a controller says the subscription is over.
    REQUIRE(api.handle("PATCH", sender, R"({"receiver_id":null})").status == 200);
    CHECK(bodyOf(api.handle("GET", sender, ""))["receiver_id"].isNull());

    CHECK(api.handle("PATCH", sender, R"({"receiver_id":7})").status == 400);
    // And a body that is not an object is not a patch at all, on this side as
    // much as on the receiver's.
    CHECK(api.handle("PATCH", sender, "[]").status == 400);
}

TEST_CASE("A patch naming something this device has not is refused") {
    // The AMWA suite sends {"bad": "data"} and expects a 400. Taking it,
    // applying nothing and answering 200 leaves a controller believing it
    // changed something.
    ConnectionApi api = apiWithOne();
    CHECK(api.handle("PATCH", path("/single/receivers/receiver-1/staged/"),
                     R"({"bad":"data"})")
              .status == 400);
    CHECK(api.handle("PATCH", path("/single/senders/sender-1/staged/"), R"({"bad":"data"})")
              .status == 400);
    // Each side refuses the other's names too.
    CHECK(api.handle("PATCH", path("/single/senders/sender-1/staged/"),
                     R"({"transport_file":{"data":null}})")
              .status == 400);
    CHECK(api.handle("PATCH", path("/single/receivers/receiver-1/staged/"),
                     R"({"receiver_id":null})")
              .status == 400);
}

TEST_CASE("A scheduled activation is promised with a 202 and kept when its time comes") {
    ConnectionApi api = apiWithOne();

    JsonObject file;
    file["data"] = JsonValue(kSdp);
    file["type"] = JsonValue("application/sdp");
    JsonObject schedule;
    schedule["master_enable"] = JsonValue(true);
    schedule["transport_file"] = JsonValue(file);
    // Zero from now: due the moment it is made, so the mechanism is what is
    // under test and not the clock.
    schedule["activation"] = JsonValue(JsonObject{
        {"mode", JsonValue("activate_scheduled_relative")},
        {"requested_time", JsonValue("0:0")}});

    const ApiResponse promised = api.handle(
        "PATCH", path("/single/receivers/receiver-1/staged/"), JsonValue(schedule).serialise());
    REQUIRE(promised.status == 202);

    const JsonValue staged = bodyOf(promised);
    CHECK(staged["activation"]["mode"].asString() == "activate_scheduled_relative");
    CHECK(staged["activation"]["requested_time"].asString() == "0:0");
    // The instant it is due, which is what a controller waits for.
    CHECK(staged["activation"]["activation_time"].isString());

    // Nothing reads this API without a request, so the next request is when a
    // due activation happens.
    const JsonValue active = bodyOf(api.handle("GET", path("/single/receivers/receiver-1/active/"), ""));
    CHECK(active["master_enable"].asBool() == true);
    CHECK(active["transport_params"].asArray().front()["multicast_ip"].asString() ==
          "239.69.1.10");

    // And the staged activation is cleared, so it does not fire twice.
    const JsonValue after = bodyOf(api.handle("GET", path("/single/receivers/receiver-1/staged/"), ""));
    CHECK(after["activation"]["mode"].isNull());
    CHECK(after["activation"]["requested_time"].isNull());
}

TEST_CASE("An absolute activation already past happens at the next request") {
    ConnectionApi api = apiWithOne();
    const ApiResponse promised =
        api.handle("PATCH", path("/single/senders/sender-1/staged/"),
                   R"({"master_enable":true,
                       "activation":{"mode":"activate_scheduled_absolute",
                                     "requested_time":"1:0"}})");
    REQUIRE(promised.status == 202);
    CHECK(bodyOf(promised)["activation"]["requested_time"].asString() == "1:0");

    const JsonValue active = bodyOf(api.handle("GET", path("/single/senders/sender-1/active/"), ""));
    CHECK(active["master_enable"].asBool() == true);
    CHECK(active["activation"]["mode"].asString() == "activate_scheduled_absolute");
}

TEST_CASE("What is active names what the device chose, not what it was asked to pick") {
    // IS-05 sec 4: "auto" is a request. A controller reading it back off
    // /active learns nothing about where the stream went.
    ConnectionApi api = apiWithOne();
    api.setInterfaceAddress("192.168.1.50");

    const JsonValue staged = bodyOf(api.handle("GET", path("/single/senders/sender-1/staged/"), ""));
    CHECK(staged["transport_params"].asArray().front()["source_port"].asString() == "auto");

    REQUIRE(api.handle("PATCH", path("/single/senders/sender-1/staged/"),
                       R"({"activation":{"mode":"activate_immediate"}})")
                .status == 200);
    const JsonValue senderActive =
        bodyOf(api.handle("GET", path("/single/senders/sender-1/active/"), ""));
    CHECK(senderActive["transport_params"].asArray().front()["source_port"].asNumber() == 5004);

    JsonObject file;
    file["data"] = JsonValue(kSdp);
    file["type"] = JsonValue("application/sdp");
    JsonObject connect;
    connect["master_enable"] = JsonValue(true);
    connect["transport_file"] = JsonValue(file);
    connect["activation"] = JsonValue(JsonObject{{"mode", JsonValue("activate_immediate")}});
    REQUIRE(api.handle("PATCH", path("/single/receivers/receiver-1/staged/"),
                       JsonValue(connect).serialise())
                .status == 200);

    const JsonValue receiverActive =
        bodyOf(api.handle("GET", path("/single/receivers/receiver-1/active/"), ""));
    CHECK(receiverActive["transport_params"].asArray().front()["interface_ip"].asString() ==
          "192.168.1.50");
}

TEST_CASE("A sender's transport file follows where it was told to send") {
    // A controller is entitled to move a sender's destination and then read
    // the SDP. One that still named the old group would send every receiver
    // somewhere nothing arrives.
    ConnectionApi api = apiWithOne();
    REQUIRE(api.handle("PATCH", path("/single/senders/sender-1/staged/"),
                       R"({"transport_params":[{"destination_port":5006}],
                           "activation":{"mode":"activate_immediate"}})")
                .status == 200);

    const ApiResponse file =
        api.handle("GET", path("/single/senders/sender-1/transportfile/"), "");
    CHECK(file.status == 200);
    CHECK(file.body.find("m=audio 5006") != std::string::npos);

    const JsonValue active = bodyOf(api.handle("GET", path("/single/senders/sender-1/active/"), ""));
    CHECK(active["transport_params"].asArray().front()["destination_port"].asNumber() == 5006);
}

TEST_CASE("Bulk stages every resource it names and answers for each one") {
    ConnectionApi api = apiWithOne();

    const std::string request =
        R"([{"id":"receiver-1","params":{"transport_params":[{"destination_port":5008}]}},
            {"id":"nobody","params":{"master_enable":true}}])";
    const ApiResponse response = api.handle("POST", path("/bulk/receivers"), request);
    REQUIRE(response.status == 200);

    const JsonValue answers = bodyOf(response);
    REQUIRE(answers.isArray());
    REQUIRE(answers.asArray().size() == 2);
    CHECK(answers.asArray()[0]["id"].asString() == "receiver-1");
    CHECK(answers.asArray()[0]["code"].asNumber() == 200);
    // One that fails does not take the batch down with it: the answer says
    // which one, and with what the single endpoint would have said.
    CHECK(answers.asArray()[1]["id"].asString() == "nobody");
    CHECK(answers.asArray()[1]["code"].asNumber() == 404);

    const JsonValue staged = bodyOf(api.handle("GET", path("/single/receivers/receiver-1/staged/"), ""));
    CHECK(staged["transport_params"].asArray().front()["destination_port"].asNumber() == 5008);

    CHECK(api.handle("POST", path("/bulk/senders"), R"({"id":"sender-1"})").status == 400);
}
