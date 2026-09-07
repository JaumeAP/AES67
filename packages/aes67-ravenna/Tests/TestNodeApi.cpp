//
// TestNodeApi.cpp
// AES67 RAVENNA session layer
// IS-04: what a controller reads before it can route anything.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Ravenna/NodeApi.h"

using namespace AES67;
using namespace AES67::Ravenna;

namespace {

RavennaSession sessionNamed(const std::string& name, uint16_t channels = 2) {
    RavennaSession session;
    session.name = name;
    session.sdp.sessionName = name;
    session.sdp.originAddress = "192.168.1.50";
    session.sdp.connectionAddress = "239.69.1.10";
    session.sdp.port = 5004;
    session.sdp.numChannels = channels;
    session.sdp.encoding = "L24";
    session.sdp.sampleRate = 48000;
    session.mapping.deviceChannelStart = 0;
    session.mapping.deviceChannelCount = channels;
    session.mapping.streamChannelCount = channels;
    return session;
}

NodeIdentity identity() {
    NodeIdentity id;
    id.nodeId = stableUuidFrom("node");
    id.deviceId = stableUuidFrom("device");
    id.label = "Master box";
    id.hostName = "box.local";
    id.addressV4 = 0xC0A80132;  // 192.168.1.50
    id.apiPort = 8080;
    return id;
}

JsonValue bodyOf(const ApiResponse& response) {
    JsonValue value;
    std::string error;
    parseJson(response.body, value, error);
    return value;
}

std::string path(const std::string& tail) { return std::string(kNodeApiRoot) + tail; }

}  // namespace

TEST_CASE("A name always gives the same id, and two names never give one") {
    // A controller keys everything on these. If they moved between restarts,
    // every restart would look like a new device and every route would be
    // pointing at something that no longer exists.
    CHECK(stableUuidFrom("Mix A") == stableUuidFrom("Mix A"));
    CHECK(stableUuidFrom("Mix A") != stableUuidFrom("Mix B"));
    CHECK(stableUuidFrom("Mix A").size() == 36);
    CHECK(stableUuidFrom("Mix A")[14] == '5');  // shaped as a version 5 UUID
}

TEST_CASE("self says what this node is and where its APIs are") {
    SessionCatalogue catalogue;
    ConnectionApi connections;
    NodeApi node(identity(), catalogue, connections);

    const JsonValue self = bodyOf(node.handle("GET", path("/self/"), ""));
    CHECK(self["id"].asString() == identity().nodeId);
    CHECK(self["label"].asString() == "Master box");
    CHECK(self["hostname"].asString() == "box.local");
    CHECK(self["api"]["versions"].asArray().size() == 1);
    CHECK(self["api"]["endpoints"].asArray()[0]["host"].asString() == "192.168.1.50");
    CHECK(self["api"]["endpoints"].asArray()[0]["port"].asNumber() == 8080);

    // No reference: it says internal rather than claiming a clock.
    CHECK(self["clocks"].asArray()[0]["ref_type"].asString() == "internal");
}

TEST_CASE("A grandmaster named is a clock announced") {
    SessionCatalogue catalogue;
    ConnectionApi connections;
    NodeIdentity locked = identity();
    locked.ptpGrandmaster = "00-1D-C1-FF-FE-00-00-01";
    NodeApi node(locked, catalogue, connections);

    const JsonValue clock = bodyOf(node.handle("GET", path("/self/"), ""))["clocks"].asArray()[0];
    CHECK(clock["ref_type"].asString() == "ptp");
    CHECK(clock["gmid"].asString() == "00-1D-C1-FF-FE-00-00-01");
    CHECK(clock["version"].asString() == "IEEE1588-2008");
}

TEST_CASE("Every session becomes a source, a flow and a sender") {
    SessionCatalogue catalogue;
    std::string error;
    REQUIRE(catalogue.add(sessionNamed("Mix A", 4), error));

    ConnectionApi connections;
    NodeApi node(identity(), catalogue, connections);

    const JsonValue sources = bodyOf(node.handle("GET", path("/sources/"), ""));
    REQUIRE(sources.asArray().size() == 1);
    CHECK(sources.asArray()[0]["label"].asString() == "Mix A");
    CHECK(sources.asArray()[0]["channels"].asArray().size() == 4);
    CHECK(sources.asArray()[0]["format"].asString() == "urn:x-nmos:format:audio");

    const JsonValue flows = bodyOf(node.handle("GET", path("/flows/"), ""));
    REQUIRE(flows.asArray().size() == 1);
    CHECK(flows.asArray()[0]["media_type"].asString() == "audio/L24");
    CHECK(flows.asArray()[0]["bit_depth"].asNumber() == 24);
    CHECK(flows.asArray()[0]["sample_rate"]["numerator"].asNumber() == 48000);
    // The flow belongs to the source it came from, which is how a controller
    // knows what a sender is carrying.
    CHECK(flows.asArray()[0]["source_id"].asString() == sources.asArray()[0]["id"].asString());

    const JsonValue senders = bodyOf(node.handle("GET", path("/senders/"), ""));
    REQUIRE(senders.asArray().size() == 1);
    CHECK(senders.asArray()[0]["flow_id"].asString() == flows.asArray()[0]["id"].asString());
    CHECK(senders.asArray()[0]["transport"].asString() == kTransportRtpMulticast);
    // Where the SDP is, which is the one thing a controller has to fetch to
    // connect anything.
    CHECK(senders.asArray()[0]["manifest_href"].asString().find(
              "/single/senders/sender-Mix A/transportfile/") != std::string::npos);
}

TEST_CASE("The device points at the APIs that route it") {
    SessionCatalogue catalogue;
    ConnectionApi connections;
    NodeApi node(identity(), catalogue, connections);

    const JsonValue devices = bodyOf(node.handle("GET", path("/devices/"), ""));
    REQUIRE(devices.asArray().size() == 1);

    const JsonValue& controls = devices.asArray()[0]["controls"];
    REQUIRE(controls.asArray().size() == 2);

    bool hasConnection = false;
    bool hasChannelMapping = false;
    for (const JsonValue& control : controls.asArray()) {
        const std::string type = control["type"].asString();
        if (type.find("sr-ctrl") != std::string::npos) hasConnection = true;
        if (type.find("cm-ctrl") != std::string::npos) hasChannelMapping = true;
    }
    CHECK(hasConnection);
    CHECK(hasChannelMapping);
}

TEST_CASE("Receivers come from the connection API, not a second list") {
    SessionCatalogue catalogue;
    ConnectionApi connections;

    ConnectionReceiver receiver;
    receiver.id = "receiver-1";
    receiver.label = "Inputs 1-2";
    connections.addReceiver(receiver);

    NodeApi node(identity(), catalogue, connections);
    const JsonValue receivers = bodyOf(node.handle("GET", path("/receivers/"), ""));
    REQUIRE(receivers.asArray().size() == 1);
    CHECK(receivers.asArray()[0]["label"].asString() == "Inputs 1-2");
    CHECK(receivers.asArray()[0]["subscription"]["active"].asBool() == false);
}

TEST_CASE("One resource can be fetched by its id") {
    SessionCatalogue catalogue;
    std::string error;
    REQUIRE(catalogue.add(sessionNamed("Mix A"), error));

    ConnectionApi connections;
    NodeApi node(identity(), catalogue, connections);

    const JsonValue senders = bodyOf(node.handle("GET", path("/senders/"), ""));
    const std::string id = senders.asArray()[0]["id"].asString();

    const ApiResponse one = node.handle("GET", path("/senders/" + id), "");
    CHECK(one.status == 200);
    CHECK(bodyOf(one)["id"].asString() == id);

    CHECK(node.handle("GET", path("/senders/not-an-id"), "").status == 404);
}

TEST_CASE("The node API is read-only and says so") {
    SessionCatalogue catalogue;
    ConnectionApi connections;
    NodeApi node(identity(), catalogue, connections);

    CHECK(node.handle("POST", path("/self/"), "{}").status == 405);
    CHECK(node.handle("GET", path("/nonsense/"), "").status == 404);
    CHECK(node.handle("GET", "/x-nmos/connection/v1.1/", "").status == 404);
    CHECK(bodyOf(node.handle("GET", path("/"), "")).asArray().size() == 6);
}

TEST_CASE("The node advertises itself for a controller with no registry") {
    SessionCatalogue catalogue;
    ConnectionApi connections;
    NodeApi node(identity(), catalogue, connections);

    const SessionAdvertisement advertised = node.advertisement();
    CHECK(advertised.serviceType == kNmosNodeService);
    CHECK(advertised.subtype.empty());
    CHECK(advertised.port == 8080);

    bool hasVersion = false;
    bool hasProtocol = false;
    for (const std::string& entry : advertised.txtEntries) {
        if (entry == "api_ver=v1.3") hasVersion = true;
        if (entry == "api_proto=http") hasProtocol = true;
    }
    CHECK(hasVersion);
    CHECK(hasProtocol);
}
