//
// TestChannelMappingApi.cpp
// AES67 RAVENNA session layer
// IS-08: the grid a controller draws, and what moving a cell does to the
// matrix underneath.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Ravenna/ChannelMappingApi.h"

#include <algorithm>

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

/// Whether a device channel is fed by that channel of that input. Asked this
/// way round because that is the way IS-08 keys its map: one input channel
/// may feed several device channels, so "which device channel does this input
/// channel feed" has no single answer.
bool carries(const JsonValue& map, int deviceChannel, const std::string& input,
             int channelIndex) {
    const JsonValue& cell = map["map"][kDeviceOutputId][std::to_string(deviceChannel)];
    return cell["input"].isString() && cell["input"].asString() == input &&
           static_cast<int>(cell["channel_index"].asNumber(-1)) == channelIndex;
}

/// Whether anything at all feeds that device channel.
bool isFed(const JsonValue& map, int deviceChannel) {
    return map["map"][kDeviceOutputId][std::to_string(deviceChannel)]["input"].isString();
}

/// The receivers these cases route. IS-08's inputs are the connection API's
/// receivers, whether or not anything is connected to them, so a case that
/// routes one has to have added it there as well.
ConnectionApi withTwoReceivers() {
    ConnectionApi connections;
    for (const std::string& id : {"receiver-1", "receiver-2"}) {
        ConnectionReceiver receiver;
        receiver.id = id;
        connections.addReceiver(receiver);
    }
    return connections;
}

}  // namespace

TEST_CASE("io lists a connected receiver as an input and the device as the output") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 4), true, outcome, why));

    const JsonValue io = bodyOf(api.handle("GET", path("/io/"), ""));
    REQUIRE(io["inputs"].isObject());
    REQUIRE(io["inputs"].asObject().count("receiver-1") == 1);

    const JsonValue& input = io["inputs"]["receiver-1"];
    CHECK(input["properties"]["name"].asString() == "Mix A");
    CHECK(input["channels"].asArray().size() == 4);
    CHECK(input["parent"]["type"].asString() == "receiver");
    CHECK(input["caps"]["block_size"].asNumber() == 1);
    CHECK(input["caps"]["reordering"].asBool());

    const JsonValue& output = io["outputs"][kDeviceOutputId];
    CHECK(output["channels"].asArray().size() == 128);
    // Null and not a list: any input can feed any device channel.
    CHECK(output["routable_inputs"].isNull());
}

TEST_CASE("The active map is what the matrix holds, not a second copy") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 2), true, outcome, why));

    const JsonValue map = bodyOf(api.handle("GET", path("/map/active/"), ""));
    CHECK(map["map"][kDeviceOutputId].asObject().size() == 128);

    CHECK(carries(map, outcome.deviceChannelStart, "receiver-1", 0));
    CHECK(carries(map, outcome.deviceChannelStart + 1, "receiver-1", 1));

    // Every other cell is empty, and says so with nulls rather than with a
    // channel nobody feeds.
    const JsonValue& far = map["map"][kDeviceOutputId]["100"];
    CHECK(far["input"].isNull());
    CHECK(far["channel_index"].isNull());
}

TEST_CASE("Moving a cell moves the channel in the matrix") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 2), true, outcome, why));

    const std::string body =
        R"({"activation":{"mode":"activate_immediate"},"action":{"device":{)"
        R"("64":{"input":"receiver-1","channel_index":1}}}})";

    const ApiResponse response = api.handle("POST", path("/map/activations"), body);
    REQUIRE(response.status == 200);

    // The answer is the activation, keyed by the id it was given, and not the
    // grid: IS-08 reports what was done, and map/active is where the grid is.
    const JsonValue answered = bodyOf(response);
    REQUIRE(answered.isObject());
    REQUIRE(answered.asObject().size() == 1);
    const JsonValue& made = answered.asObject().begin()->second;
    CHECK(made["activation"]["mode"].asString() == "activate_immediate");
    CHECK(made["activation"]["requested_time"].isNull());
    CHECK(made["activation"]["activation_time"].isNull() == false);

    const JsonValue map = bodyOf(api.handle("GET", path("/map/active/"), ""));
    CHECK(carries(map, 64, "receiver-1", 1));

    // And the matrix agrees, which is the point: the grid is a view of it.
    const auto mapping = routing.mappingFor("receiver-1");
    REQUIRE(mapping.has_value());
    CHECK(std::find(mapping->routes.begin(), mapping->routes.end(), ChannelRoute{1, 64}) !=
          mapping->routes.end());
}

TEST_CASE("A cell emptied stops carrying anything") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 2), true, outcome, why));
    const int first = outcome.deviceChannelStart;

    const std::string body =
        R"({"activation":{"mode":"activate_immediate"},"action":{"device":{")" +
        std::to_string(first) + R"(":{"input":null,"channel_index":null}}}})";

    REQUIRE(api.handle("POST", path("/map/activations"), body).status == 200);

    const JsonValue map = bodyOf(api.handle("GET", path("/map/active/"), ""));
    CHECK(map["map"][kDeviceOutputId][std::to_string(first)]["input"].isNull());
    // The other channel is untouched.
    CHECK(carries(map, first + 1, "receiver-1", 1));
}

TEST_CASE("Two inputs cannot end up on one device channel") {
    // Not a mix: it is a fault, and the one that arrives last wins because
    // whatever fed the channel stops feeding it first.
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 2), true, outcome, why));
    REQUIRE(routing.apply("receiver-2", sdpFor("Mix B", 2), true, outcome, why));

    const std::string body =
        R"({"activation":{"mode":"activate_immediate"},"action":{"device":{)"
        R"("10":{"input":"receiver-1","channel_index":0},)"
        R"("10":{"input":"receiver-2","channel_index":0}}}})";
    REQUIRE(api.handle("POST", path("/map/activations"), body).status == 200);

    const JsonValue map = bodyOf(api.handle("GET", path("/map/active/"), ""));
    const JsonValue& cell = map["map"][kDeviceOutputId]["10"];
    REQUIRE(cell["input"].isString());
    CHECK(cell["input"].asString() == "receiver-2");
    CHECK_FALSE(carries(map, 10, "receiver-1", 0));
}

TEST_CASE("A grid the matrix refuses leaves the device carrying what it was") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 2), true, outcome, why));
    const JsonValue before = bodyOf(api.handle("GET", path("/map/active/"), ""));

    // A device channel this device does not have.
    const std::string body =
        R"({"activation":{"mode":"activate_immediate"},"action":{"device":{)"
        R"("500":{"input":"receiver-1","channel_index":0}}}})";
    CHECK(api.handle("POST", path("/map/activations"), body).status == 400);

    const JsonValue after = bodyOf(api.handle("GET", path("/map/active/"), ""));
    CHECK(after["map"][kDeviceOutputId].serialise() ==
          before["map"][kDeviceOutputId].serialise());
}

TEST_CASE("An input or a channel that does not exist is refused with which") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 2), true, outcome, why));

    ApiResponse response = api.handle(
        "POST", path("/map/activations"),
        R"({"activation":{"mode":"activate_immediate"},"action":{"device":{"5":{"input":"nope","channel_index":0}}}})");
    CHECK(response.status == 404);
    CHECK(response.body.find("no input called nope") != std::string::npos);

    response = api.handle(
        "POST", path("/map/activations"),
        R"({"activation":{"mode":"activate_immediate"},"action":{"device":{"5":{"input":"receiver-1","channel_index":9}}}})");
    CHECK(response.status == 400);
    CHECK(response.body.find("no channel 9") != std::string::npos);
}

TEST_CASE("An output block this device does not have is a 404") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    const ApiResponse response = api.handle(
        "POST", path("/map/activations"),
        R"({"activation":{"mode":"activate_immediate"},"action":{"analogue":{"0":{"input":null,"channel_index":null}}}})");
    CHECK(response.status == 404);
}

TEST_CASE("A scheduled activation is refused rather than forgotten") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    // A schedule with no time is one this device could never keep.
    CHECK(api.handle("POST", path("/map/activations"),
                     R"({"activation":{"mode":"activate_scheduled_relative"},"action":{}})")
              .status == 400);
    CHECK(api.handle("POST", path("/map/activations"),
                     R"({"activation":{"mode":"activate_eventually"},"action":{}})")
              .status == 400);
}

TEST_CASE("A scheduled activation naming a bad grid is refused now, not silently later") {
    // postActivation()'s scheduled branch used to check only that "action"
    // was a JSON object before answering 202 and queuing it -- the same
    // validation the immediate branch actually runs (output id, cell shape,
    // channel bounds, input existence) happened only later, inside
    // applyDueActivations(), which discards a refusal there with no way to
    // tell the controller: the answer already went out with the 202.
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    const ApiResponse scheduled = api.handle(
        "POST", path("/map/activations"),
        R"({"activation":{"mode":"activate_scheduled_relative","requested_time":"5:0"},)"
        R"("action":{"device":{"40":{"input":"nobody","channel_index":0}}}})");
    CHECK(scheduled.status == 404);

    // Refused now means nothing was queued: a controller that reads the
    // activation list back sees nothing waiting, not a promise that was
    // always going to fail.
    CHECK(bodyOf(api.handle("GET", path("/map/activations/"), "")).asObject().empty());

    // The matrix itself is untouched by having been asked: applyAction's
    // dry run restores it, so this is not "refused, but half-applied"
    // either.
    const JsonValue after = bodyOf(api.handle("GET", path("/map/active/"), ""));
    CHECK_FALSE(isFed(after, 40));
}

TEST_CASE("The API answers where a controller starts looking") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    // inputs, io, map, outputs: the four IS-08 v1.0 defines.
    CHECK(bodyOf(api.handle("GET", path("/"), "")).asArray().size() == 4);
    CHECK(bodyOf(api.handle("GET", path("/map/"), "")).asArray().size() == 2);
    // An object keyed by activation id, not an array: there are none, and a
    // controller that read an array here could not index it either.
    CHECK(bodyOf(api.handle("GET", path("/map/activations/"), "")).asObject().empty());

    CHECK(api.handle("GET", path("/nonsense/"), "").status == 404);
    CHECK(api.handle("POST", path("/io/"), "").status == 405);
    CHECK(api.handle("GET", "/x-nmos/connection/v1.1/", "").status == 404);
}

TEST_CASE("Every input and output answers one field at a time") {
    // io says all of this in one answer, and a controller is entitled to
    // either: the whole picture for drawing a grid, or the one field it is
    // about to show.
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    const JsonValue inputs = bodyOf(api.handle("GET", path("/inputs/"), ""));
    REQUIRE(inputs.isArray());
    CHECK(inputs.asArray().size() == 2);
    CHECK(inputs.asArray()[0].asString() == "receiver-1/");

    const JsonValue outputs = bodyOf(api.handle("GET", path("/outputs/"), ""));
    REQUIRE(outputs.asArray().size() == 1);
    CHECK(outputs.asArray()[0].asString() == std::string(kDeviceOutputId) + "/");

    // Four each, and the fourth is what tells an input from an output.
    const JsonValue input = bodyOf(api.handle("GET", path("/inputs/receiver-1/"), ""));
    REQUIRE(input.asArray().size() == 4);
    const JsonValue output =
        bodyOf(api.handle("GET", path("/outputs/") + kDeviceOutputId + "/", ""));
    REQUIRE(output.asArray().size() == 4);

    CHECK(api.handle("GET", path("/inputs/receiver-1/parent/"), "").status == 200);
    CHECK(api.handle("GET", path("/inputs/receiver-1/sourceid/"), "").status == 404);
    CHECK(bodyOf(api.handle("GET", path("/outputs/") + kDeviceOutputId + "/sourceid/", ""))
              .isNull());
    CHECK(api.handle("GET", path("/outputs/") + kDeviceOutputId + "/parent/", "").status == 404);

    // routable_inputs is a capability of the output, not a field beside its
    // channels: an output carrying it at the top is not one the schema knows.
    const JsonValue caps =
        bodyOf(api.handle("GET", path("/outputs/") + kDeviceOutputId + "/caps/", ""));
    REQUIRE(caps.isObject());
    CHECK(caps.asObject().count("routable_inputs") == 1);
    CHECK(caps["routable_inputs"].isNull());

    CHECK(api.handle("GET", path("/inputs/nobody/"), "").status == 404);
    CHECK(api.handle("GET", path("/outputs/nobody/"), "").status == 404);
    CHECK(api.handle("POST", path("/inputs/"), "").status == 405);
}

TEST_CASE("An input exists before anything is connected to it") {
    // IS-08's inputs are the ports a controller may route FROM. Publishing
    // only the connected ones left a controller with nothing to draw a grid
    // with until somebody had connected them by other means.
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    const JsonValue io = bodyOf(api.handle("GET", path("/io/"), ""));
    REQUIRE(io["inputs"].asObject().size() == 2);

    const JsonValue& input = io["inputs"]["receiver-1"];
    // Named, and with channels to show: the schema wants at least one on
    // every input, and a row with none is a row nobody can draw.
    CHECK(input["properties"]["name"].asString() == "receiver-1");
    CHECK(input["channels"].asArray().size() > 0);
    // From nowhere yet, which is what both nulls say.
    CHECK(input["parent"]["id"].isNull());
    CHECK(input["parent"]["type"].isNull());

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 4), true, outcome, why));

    const JsonValue connected = bodyOf(api.handle("GET", path("/io/"), ""));
    CHECK(connected["inputs"]["receiver-1"]["properties"]["name"].asString() == "Mix A");
    CHECK(connected["inputs"]["receiver-1"]["channels"].asArray().size() == 4);
    CHECK(connected["inputs"]["receiver-1"]["parent"]["id"].asString() == "receiver-1");
    // And the one nobody connected is still there, still routable.
    CHECK(connected["inputs"].asObject().count("receiver-2") == 1);
}

TEST_CASE("A scheduled change is promised, locks the grid, and can be called off") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 2), true, outcome, why));

    // Far enough out that it is still waiting when this reads it back.
    const std::string later =
        R"({"activation":{"mode":"activate_scheduled_relative","requested_time":"600:0"},)"
        R"("action":{"device":{"64":{"input":"receiver-1","channel_index":1}}}})";

    const ApiResponse promised = api.handle("POST", path("/map/activations"), later);
    REQUIRE(promised.status == 202);

    const JsonValue answered = bodyOf(promised);
    REQUIRE(answered.asObject().size() == 1);
    const std::string id = answered.asObject().begin()->first;
    const JsonValue& made = answered.asObject().begin()->second;
    CHECK(made["activation"]["mode"].asString() == "activate_scheduled_relative");
    CHECK(made["activation"]["requested_time"].asString() == "600:0");
    CHECK(made["activation"]["activation_time"].isString());

    // Promised, not done: the grid has not moved.
    const JsonValue map = bodyOf(api.handle("GET", path("/map/active/"), ""));
    CHECK_FALSE(carries(map, 64, "receiver-1", 1));

    // And it is readable under its id, on its own and in the list.
    CHECK(bodyOf(api.handle("GET", path("/map/activations/"), "")).asObject().count(id) == 1);
    CHECK(api.handle("GET", path("/map/activations/") + id + "/", "").status == 200);

    // A grid with a change already promised is locked: taking a second would
    // leave two answers about where the same channel is going.
    CHECK(api.handle("POST", path("/map/activations"), later).status == 423);

    // Called off, and the lock goes with it.
    const ApiResponse deleted = api.handle("DELETE", path("/map/activations/") + id + "/", "");
    CHECK(deleted.status == 204);
    CHECK(api.handle("GET", path("/map/activations/") + id + "/", "").status == 404);
    CHECK(bodyOf(api.handle("GET", path("/map/activations/"), "")).asObject().empty());
    CHECK(api.handle("POST", path("/map/activations"), later).status == 202);
}

TEST_CASE("A change whose time has passed happens at the next request") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 2), true, outcome, why));

    // Zero from now: due the moment it is made, so what is under test is the
    // mechanism and not the clock.
    REQUIRE(api.handle("POST", path("/map/activations"),
                       R"({"activation":{"mode":"activate_scheduled_relative","requested_time":"0:0"},)"
                       R"("action":{"device":{"64":{"input":"receiver-1","channel_index":1}}}})")
                .status == 202);

    // Nothing reads this API without a request, so the next request is when a
    // due change happens.
    const JsonValue map = bodyOf(api.handle("GET", path("/map/active/"), ""));
    CHECK(carries(map, 64, "receiver-1", 1));
    // And it is gone from the list, so it cannot fire twice.
    CHECK(bodyOf(api.handle("GET", path("/map/activations/"), "")).asObject().empty());
}

TEST_CASE("A grid set before the stream is kept, and taken up when it arrives") {
    // IS-08 routes ports, and a plant is patched before its streams arrive.
    // A device that refused the grid until something was flowing would make a
    // controller set it twice, in an order nobody asked it to work in.
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    REQUIRE(api.handle("POST", path("/map/activations"),
                       R"({"activation":{"mode":"activate_immediate"},)"
                       R"("action":{"device":{"70":{"input":"receiver-1","channel_index":1}}}})")
                .status == 200);

    // Read back before anything is connected: the active map says where that
    // channel will land, which is what a controller just told it.
    const JsonValue patched = bodyOf(api.handle("GET", path("/map/active/"), ""));
    CHECK(carries(patched, 70, "receiver-1", 1));

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 2), true, outcome, why));

    // And the stream lands where it was patched rather than on the default
    // block it would otherwise have taken.
    const JsonValue flowing = bodyOf(api.handle("GET", path("/map/active/"), ""));
    CHECK(carries(flowing, 70, "receiver-1", 1));
}

TEST_CASE("One input channel can feed several device channels") {
    // IS-08 keys its map by the OUTPUT channel: routing an input onto a
    // second output does not take it off the first, because that first cell
    // was never written. One source into several monitors is the ordinary
    // reason to ask.
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);
    ConnectionApi connections = withTwoReceivers();
    ChannelMappingApi api(mapper, routing, connections);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Talkback", 1), true, outcome, why));

    REQUIRE(api.handle("POST", path("/map/activations"),
                       R"({"activation":{"mode":"activate_immediate"},"action":{"device":{)"
                       R"("40":{"input":"receiver-1","channel_index":0},)"
                       R"("41":{"input":"receiver-1","channel_index":0},)"
                       R"("42":{"input":"receiver-1","channel_index":0}}}})")
                .status == 200);

    const JsonValue map = bodyOf(api.handle("GET", path("/map/active/"), ""));
    CHECK(carries(map, 40, "receiver-1", 0));
    CHECK(carries(map, 41, "receiver-1", 0));
    CHECK(carries(map, 42, "receiver-1", 0));
    // And the block it was given still carries it too: nothing emptied that
    // cell, so nothing stopped feeding it.
    CHECK(carries(map, outcome.deviceChannelStart, "receiver-1", 0));

    // The matrix holds all of them, and they are this stream's.
    const auto mapping = routing.mappingFor("receiver-1");
    REQUIRE(mapping.has_value());
    for (int deviceChannel : {40, 41, 42}) {
        CHECK(mapping->containsDeviceChannel(deviceChannel));
    }

    // Emptying one leaves the others alone.
    REQUIRE(api.handle("POST", path("/map/activations"),
                       R"({"activation":{"mode":"activate_immediate"},)"
                       R"("action":{"device":{"41":{"input":null,"channel_index":null}}}})")
                .status == 200);
    const JsonValue after = bodyOf(api.handle("GET", path("/map/active/"), ""));
    CHECK_FALSE(isFed(after, 41));
    CHECK(carries(after, 40, "receiver-1", 0));
    CHECK(carries(after, 42, "receiver-1", 0));
}
