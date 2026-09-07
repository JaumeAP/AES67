//
// TestReceiverRouting.cpp
// AES67 RAVENNA session layer
// What an activation does to the channel matrix.
//
// This is the half of IS-05 that a protocol test cannot see: the API answers
// 200 either way, and whether the device actually has room is a question for
// the matrix.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Ravenna/ReceiverRouting.h"

using namespace AES67;
using namespace AES67::Ravenna;

namespace {

std::string sdpFor(const std::string& name, uint16_t channels,
                   const std::string& group = "239.69.1.10") {
    SDPSession session;
    session.sessionName = name;
    session.originAddress = "192.168.1.50";
    session.connectionAddress = group;
    session.port = 5004;
    session.numChannels = channels;
    session.encoding = "L24";
    session.sampleRate = 48000;
    session.ptimeUs = 1000;
    return SDPParser::generate(session);
}

}  // namespace

TEST_CASE("Activating a receiver takes a block of device channels") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 2), true, outcome, why));
    CHECK(why.empty());

    CHECK(outcome.connected);
    CHECK(outcome.streamName == "Mix A");
    CHECK(outcome.channelCount == 2);

    const auto mapping = routing.mappingFor("receiver-1");
    REQUIRE(mapping.has_value());
    CHECK(mapping->deviceChannelCount == 2);
    CHECK(mapping->deviceChannelStart == outcome.deviceChannelStart);
}

TEST_CASE("Two receivers do not land on the same channels") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);

    RoutingOutcome first;
    RoutingOutcome second;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 8), true, first, why));
    REQUIRE(routing.apply("receiver-2", sdpFor("Mix B", 8), true, second, why));

    const uint16_t firstEnd = first.deviceChannelStart + first.channelCount;
    const uint16_t secondEnd = second.deviceChannelStart + second.channelCount;
    CHECK((firstEnd <= second.deviceChannelStart || secondEnd <= first.deviceChannelStart));
}

TEST_CASE("Disabling gives the channels back") {
    // Without this the next receiver finds the channels taken by a connection
    // nobody has any more.
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 64), true, outcome, why));
    REQUIRE(routing.apply("receiver-2", sdpFor("Mix B", 64), true, outcome, why));

    // Full: 128 channels, both blocks taken.
    RoutingOutcome refused;
    CHECK(routing.apply("receiver-3", sdpFor("Mix C", 8), true, refused, why) == false);
    CHECK(why.find("no free device channels") != std::string::npos);

    RoutingOutcome released;
    REQUIRE(routing.apply("receiver-1", "", false, released, why));
    CHECK(released.connected == false);
    CHECK(routing.mappingFor("receiver-1").has_value() == false);

    // And now there is room again.
    CHECK(routing.apply("receiver-3", sdpFor("Mix C", 8), true, refused, why));
}

TEST_CASE("Pointing a receiver at another stream replaces its connection") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);

    RoutingOutcome outcome;
    std::string why;
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix A", 64), true, outcome, why));
    const uint16_t firstStart = outcome.deviceChannelStart;

    // The same width again: this only fits if the receiver's own block was
    // released before room was asked for.
    REQUIRE(routing.apply("receiver-1", sdpFor("Mix B", 64), true, outcome, why));
    CHECK(outcome.streamName == "Mix B");
    CHECK(outcome.deviceChannelStart == firstStart);

    const auto mapping = routing.mappingFor("receiver-1");
    REQUIRE(mapping.has_value());
    CHECK(mapping->streamName == "Mix B");

    // One receiver, one mapping.
    CHECK(mapper.getAllMappings().size() == 1);
}

TEST_CASE("A transport file that is not an SDP is refused and takes nothing") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);

    RoutingOutcome outcome;
    std::string why;
    CHECK(routing.apply("receiver-1", "not an sdp at all", true, outcome, why) == false);
    CHECK(why.empty() == false);
    CHECK(routing.mappingFor("receiver-1").has_value() == false);
    CHECK(mapper.getAllMappings().empty());
}

TEST_CASE("Disabling a receiver that holds nothing is not an error") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);

    RoutingOutcome outcome;
    std::string why;
    CHECK(routing.apply("receiver-9", "", false, outcome, why));
    CHECK(outcome.connected == false);
}

TEST_CASE("The callback is the same behaviour, shaped for the API") {
    StreamChannelMapper mapper;
    ReceiverRouting routing(mapper);

    ConnectionApi api;
    ConnectionReceiver receiver;
    receiver.id = "receiver-1";
    api.addReceiver(receiver);
    api.onReceiverActivation(routing.callback());

    const std::string body =
        R"({"master_enable":true,"transport_file":{"data":)" +
        jsonQuote(sdpFor("Mix A", 4)) +
        R"(,"type":"application/sdp"},"activation":{"mode":"activate_immediate"}})";

    const ApiResponse response = api.handle(
        "PATCH", std::string(kConnectionApiRoot) + "/single/receivers/receiver-1/staged/", body);
    CHECK(response.status == 200);

    const auto mapping = routing.mappingFor("receiver-1");
    REQUIRE(mapping.has_value());
    CHECK(mapping->deviceChannelCount == 4);
}
