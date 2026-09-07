//
// TestChannelMappingApi.cpp
// AES67 RAVENNA session layer
// IS-08: the grid a controller draws, and what moving a cell does to the
// matrix underneath.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Ravenna/ChannelMappingApi.h"

using namespace AES67;
using namespace AES67::Ravenna;

namespace {

std::string sdpFor(const std::string& name, uint16_t channels) {
    SDPSession session;
    session.sessionName = name;
    session.originAddress = "192.168.1.50";
    session.connectionAddress = "239.69.1.10";
    session.port = 5004;
    session.numChannels = channels;
    session.encoding = "L24";
    session.sampleRate = 48000;
    session.ptimeUs = 1000;
    return SDPParser::generate(session);
}

std::string path(const std::string& tail) {
    return std::string(kChannelMappingApiRoot) + tail;
}

JsonValue bodyOf(const ApiResponse& response) {
    JsonValue value;
    std::string error;
    parseJson(response.body, value, error);
    return value;
}

/// The device channel that a given input channel feeds, or -1.
int deviceChannelOf(const JsonValue& map, const std::string& input, int channelIndex) {
    const JsonValue& cells = map["action"][kDeviceOutputId];
    for (const auto& [channel, cell] : cells.asObject()) {
        if (cell["input"].isString() && cell["input"].asString() == input &&
            static_cast<int>(cell["channel_index"].asNumber(-1)) == channelIndex) {
            return std::stoi(channel);
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("io lists a connected receiver as an input and the device as the output") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ChannelMappingApi api(mapper, routing);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 4), true, outcome, why));

    const JsonValue io = bodyOf(api.handle("GET", path("/io/"), ""));
    REQUIRE(io["inputs"].isObject());
    REQUIRE(io["inputs"].asObject().count("receiver-1") == 1);

    const JsonValue& input = io["inputs"]["receiver-1"];
    CHECK(input["name"].asString() == "Mix A");
    CHECK(input["channels"].asArray().size() == 4);
    CHECK(input["parent"]["type"].asString() == "receiver");
    CHECK(input["block_size"].asNumber() == 1);
    CHECK(input["reordering"].asBool());

    const JsonValue& output = io["outputs"][kDeviceOutputId];
    CHECK(output["channels"].asArray().size() == 128);
    // Null and not a list: any input can feed any device channel.
    CHECK(output["routable_inputs"].isNull());
}

TEST_CASE("The active map is what the matrix holds, not a second copy") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ChannelMappingApi api(mapper, routing);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 2), true, outcome, why));

    const JsonValue map = bodyOf(api.handle("GET", path("/map/active/"), ""));
    CHECK(map["action"][kDeviceOutputId].asObject().size() == 128);

    CHECK(deviceChannelOf(map, "receiver-1", 0) == outcome.deviceChannelStart);
    CHECK(deviceChannelOf(map, "receiver-1", 1) == outcome.deviceChannelStart + 1);

    // Every other cell is empty, and says so with nulls rather than with a
    // channel nobody feeds.
    const JsonValue& far = map["action"][kDeviceOutputId]["100"];
    CHECK(far["input"].isNull());
    CHECK(far["channel_index"].isNull());
}

TEST_CASE("Moving a cell moves the channel in the matrix") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ChannelMappingApi api(mapper, routing);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 2), true, outcome, why));

    const std::string body =
        R"({"activation":{"mode":"activate_immediate"},"action":{"device":{)"
        R"("64":{"input":"receiver-1","channel_index":1}}}})";

    const ApiResponse response = api.handle("POST", path("/map/activate"), body);
    REQUIRE(response.status == 200);

    const JsonValue map = bodyOf(response);
    CHECK(deviceChannelOf(map, "receiver-1", 1) == 64);
    CHECK(map["activation"]["mode"].asString() == "activate_immediate");
    CHECK(map["activation"]["activation_time"].isNull() == false);

    // And the matrix agrees, which is the point: the grid is a view of it.
    const auto mapping = routing.mappingFor("receiver-1");
    REQUIRE(mapping.has_value());
    REQUIRE(mapping->channelMap.size() == 2);
    CHECK(mapping->channelMap[1] == 64);
}

TEST_CASE("A cell emptied stops carrying anything") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ChannelMappingApi api(mapper, routing);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 2), true, outcome, why));
    const int first = outcome.deviceChannelStart;

    const std::string body =
        R"({"activation":{"mode":"activate_immediate"},"action":{"device":{")" +
        std::to_string(first) + R"(":{"input":null,"channel_index":null}}}})";

    REQUIRE(api.handle("POST", path("/map/activate"), body).status == 200);

    const JsonValue map = bodyOf(api.handle("GET", path("/map/active/"), ""));
    CHECK(map["action"][kDeviceOutputId][std::to_string(first)]["input"].isNull());
    // The other channel is untouched.
    CHECK(deviceChannelOf(map, "receiver-1", 1) == first + 1);
}

TEST_CASE("Two inputs cannot end up on one device channel") {
    // Not a mix: it is a fault, and the one that arrives last wins because
    // whatever fed the channel stops feeding it first.
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ChannelMappingApi api(mapper, routing);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 2), true, outcome, why));
    REQUIRE(routing.apply("receiver-2", sdpFor("Mix B", 2), true, outcome, why));

    const std::string body =
        R"({"activation":{"mode":"activate_immediate"},"action":{"device":{)"
        R"("10":{"input":"receiver-1","channel_index":0},)"
        R"("10":{"input":"receiver-2","channel_index":0}}}})";
    REQUIRE(api.handle("POST", path("/map/activate"), body).status == 200);

    const JsonValue map = bodyOf(api.handle("GET", path("/map/active/"), ""));
    const JsonValue& cell = map["action"][kDeviceOutputId]["10"];
    REQUIRE(cell["input"].isString());
    CHECK(cell["input"].asString() == "receiver-2");
    CHECK(deviceChannelOf(map, "receiver-1", 0) != 10);
}

TEST_CASE("A grid the matrix refuses leaves the device carrying what it was") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ChannelMappingApi api(mapper, routing);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 2), true, outcome, why));
    const JsonValue before = bodyOf(api.handle("GET", path("/map/active/"), ""));

    // A device channel this device does not have.
    const std::string body =
        R"({"activation":{"mode":"activate_immediate"},"action":{"device":{)"
        R"("500":{"input":"receiver-1","channel_index":0}}}})";
    CHECK(api.handle("POST", path("/map/activate"), body).status == 400);

    const JsonValue after = bodyOf(api.handle("GET", path("/map/active/"), ""));
    CHECK(deviceChannelOf(after, "receiver-1", 0) == deviceChannelOf(before, "receiver-1", 0));
}

TEST_CASE("An input or a channel that does not exist is refused with which") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ChannelMappingApi api(mapper, routing);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 2), true, outcome, why));

    ApiResponse response = api.handle(
        "POST", path("/map/activate"),
        R"({"activation":{"mode":"activate_immediate"},"action":{"device":{"5":{"input":"nope","channel_index":0}}}})");
    CHECK(response.status == 404);
    CHECK(response.body.find("no input called nope") != std::string::npos);

    response = api.handle(
        "POST", path("/map/activate"),
        R"({"activation":{"mode":"activate_immediate"},"action":{"device":{"5":{"input":"receiver-1","channel_index":9}}}})");
    CHECK(response.status == 400);
    CHECK(response.body.find("no channel 9") != std::string::npos);
}

TEST_CASE("An output block this device does not have is a 404") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ChannelMappingApi api(mapper, routing);

    const ApiResponse response = api.handle(
        "POST", path("/map/activate"),
        R"({"activation":{"mode":"activate_immediate"},"action":{"analogue":{"0":{"input":null,"channel_index":null}}}})");
    CHECK(response.status == 404);
}

TEST_CASE("A scheduled activation is refused rather than forgotten") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ChannelMappingApi api(mapper, routing);

    CHECK(api.handle("POST", path("/map/activate"),
                     R"({"activation":{"mode":"activate_scheduled_relative"},"action":{}})")
              .status == 501);
}

TEST_CASE("The API answers where a controller starts looking") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ChannelMappingApi api(mapper, routing);

    CHECK(bodyOf(api.handle("GET", path("/"), "")).asArray().size() == 2);
    CHECK(bodyOf(api.handle("GET", path("/map/"), "")).asArray().size() == 2);
    CHECK(bodyOf(api.handle("GET", path("/map/activations/"), "")).asArray().empty());

    CHECK(api.handle("GET", path("/nonsense/"), "").status == 404);
    CHECK(api.handle("POST", path("/io/"), "").status == 405);
    CHECK(api.handle("GET", path("/map/activate"), "").status == 405);
    CHECK(api.handle("GET", "/x-nmos/connection/v1.1/", "").status == 404);
}
