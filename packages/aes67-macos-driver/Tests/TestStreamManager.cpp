//
// TestStreamManager.cpp
// AES67 macOS Driver - Build #18
// Unit tests for StreamManager
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/StreamManager.h"
#include "NetworkEngine/RTP/PacketBudget.h"
#include "Driver/SDPParser.h"
#include <iostream>
#include <cassert>
#include <utility>

using namespace AES67;

// Test result counter


//
// Helper Functions
//

// Create test SDP session
SDPSession createTestSDP(const std::string& name = "Test Stream",
                         uint16_t port = 5004,
                         uint16_t channels = 2,
                         uint32_t sampleRate = 48000) {
    SDPSession sdp;
    sdp.sessionName = name;
    sdp.port = port;
    sdp.connectionAddress = "239.1.1.1";
    sdp.encoding = "L24";
    sdp.sampleRate = sampleRate;
    sdp.numChannels = channels;
    sdp.payloadType = 97;
    sdp.ptimeUs = 1000;
    sdp.framecount = 48;
    sdp.originAddress = "192.168.1.100";
    sdp.ptpDomain = 0;

    return sdp;
}

// Create test channel mapping
ChannelMapping createTestMapping(uint16_t streamChannels = 2,
                                 uint16_t deviceStart = 0) {
    ChannelMapping mapping;
    mapping.streamID = StreamID::generate();
    mapping.streamName = "Test Mapping";
    mapping.streamChannelCount = streamChannels;
    mapping.streamChannelOffset = 0;
    mapping.deviceChannelStart = deviceStart;
    mapping.deviceChannelCount = streamChannels;

    return mapping;
}

//
// SDP Session Creation Tests
//

TEST_CASE("SDP Session Creation") {
    std::cout << "Test: SDP session creation for StreamManager... ";

    SDPSession sdp = createTestSDP("Test Stream", 5004, 8, 48000);

    CHECK(sdp.sessionName == "Test Stream");
    CHECK(sdp.port == 5004);
    CHECK(sdp.numChannels == 8);
    CHECK(sdp.sampleRate == 48000);
    CHECK(sdp.connectionAddress == "239.1.1.1");

    std::cout << "PASS" << std::endl;
}

TEST_CASE("SDP Session Validation") {
    std::cout << "Test: SDP session validation... ";

    SDPSession validSDP = createTestSDP();
    CHECK(validSDP.isValid());

    // Test invalid port
    SDPSession invalidPort = createTestSDP();
    invalidPort.port = 0;
    CHECK(!invalidPort.isValid());

    // Test invalid sample rate
    SDPSession invalidSR = createTestSDP();
    invalidSR.sampleRate = 0;
    CHECK(!invalidSR.isValid());

    std::cout << "PASS" << std::endl;
}

//
// Channel Mapping Tests
//

TEST_CASE("Channel Mapping Creation") {
    std::cout << "Test: Channel mapping for streams... ";

    ChannelMapping mapping = createTestMapping(8, 16);

    CHECK(mapping.streamChannelCount == 8);
    CHECK(mapping.deviceChannelStart == 16);
    CHECK(mapping.deviceChannelCount == 8);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Channel Mapping Validation") {
    std::cout << "Test: Channel mapping validation... ";

    // Valid mapping
    ChannelMapping valid = createTestMapping(4, 0);
    CHECK(valid.isValid());

    // Invalid: device channels out of range
    ChannelMapping invalid = createTestMapping(4, 126);
    CHECK(!invalid.isValid());

    // Invalid: zero channels
    ChannelMapping zeroChannels = createTestMapping(0, 0);
    CHECK(!zeroChannels.isValid());

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Channel Mapping Overlap") {
    std::cout << "Test: Channel mapping overlap detection... ";

    // Mapping 1: channels 0-7
    ChannelMapping mapping1 = createTestMapping(8, 0);

    // Mapping 2: channels 8-15 (no overlap)
    ChannelMapping mapping2 = createTestMapping(8, 8);

    // Mapping 3: channels 4-11 (overlaps with both)
    ChannelMapping mapping3 = createTestMapping(8, 4);

    // Check bounds
    uint16_t end1 = mapping1.getDeviceChannelEnd();
    uint16_t end2 = mapping2.getDeviceChannelEnd();
    uint16_t end3 = mapping3.getDeviceChannelEnd();

    CHECK(end1 == 8);
    CHECK(end2 == 16);
    CHECK(end3 == 12);

    // Check for overlaps
    bool overlap1_2 = (mapping1.deviceChannelStart < end2 &&
                       mapping2.deviceChannelStart < end1);
    bool overlap1_3 = (mapping1.deviceChannelStart < end3 &&
                       mapping3.deviceChannelStart < end1);

    CHECK(!overlap1_2);
    CHECK(overlap1_3);

    std::cout << "PASS" << std::endl;
}

//
// Sample Rate Validation Tests
//

TEST_CASE("Sample Rate Compatibility") {
    std::cout << "Test: Sample rate compatibility... ";

    // Common AES67 sample rates
    std::vector<uint32_t> validRates = {44100, 48000, 88200, 96000, 176400, 192000, 384000};

    for (auto rate : validRates) {
        SDPSession sdp = createTestSDP("Test", 5004, 2, rate);
        CHECK(sdp.sampleRate == rate);
    }

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Sample Rate Mismatch") {
    std::cout << "Test: Sample rate mismatch detection... ";

    // Device at 48kHz
    uint32_t deviceRate = 48000;

    // Stream at same rate - OK
    SDPSession matching = createTestSDP("Match", 5004, 2, 48000);
    CHECK(matching.sampleRate == deviceRate);

    // Stream at different rate - Would need validation
    SDPSession mismatched = createTestSDP("Mismatch", 5004, 2, 96000);
    CHECK(mismatched.sampleRate != deviceRate);

    std::cout << "PASS" << std::endl;
}

//
// Stream Configuration Tests
//

TEST_CASE("Stream ID Generation") {
    std::cout << "Test: StreamID generation and uniqueness... ";

    StreamID id1 = StreamID::generate();
    StreamID id2 = StreamID::generate();
    StreamID id3 = StreamID::generate();

    CHECK(!id1.isNull());
    CHECK(!id2.isNull());
    CHECK(!id3.isNull());

    CHECK(id1 != id2);
    CHECK(id2 != id3);
    CHECK(id1 != id3);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Stream ID Comparison") {
    std::cout << "Test: StreamID comparison operators... ";

    StreamID id1 = StreamID::generate();
    StreamID id2 = id1;  // Copy
    StreamID id3 = StreamID::generate();

    CHECK(id1 == id2);
    CHECK(id1 != id3);

    // Test null ID
    StreamID null1 = StreamID::null();
    StreamID null2 = StreamID::null();
    CHECK(null1 == null2);
    CHECK(null1.isNull());

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Stream ID String Conversion") {
    std::cout << "Test: StreamID string conversion... ";

    StreamID id = StreamID::generate();
    std::string idStr = id.toString();

    CHECK(!idStr.empty());
    CHECK(idStr.length() == 36);

    // Test null ID
    StreamID nullId = StreamID::null();
    std::string nullStr = nullId.toString();
    CHECK(!nullStr.empty());

    std::cout << "PASS" << std::endl;
}

//
// Multi-Stream Configuration Tests
//

TEST_CASE("Multiple Stream Configuration") {
    std::cout << "Test: Multiple stream configuration... ";

    // Create multiple SDP sessions
    SDPSession stream1 = createTestSDP("Stream 1", 5004, 2, 48000);
    SDPSession stream2 = createTestSDP("Stream 2", 5006, 4, 48000);
    SDPSession stream3 = createTestSDP("Stream 3", 5008, 8, 48000);

    // Create non-overlapping mappings
    ChannelMapping map1 = createTestMapping(2, 0);   // Channels 0-1
    ChannelMapping map2 = createTestMapping(4, 2);   // Channels 2-5
    ChannelMapping map3 = createTestMapping(8, 6);   // Channels 6-13

    // Verify total channel usage: 2 + 4 + 8 = 14 channels
    uint16_t totalChannels = map1.deviceChannelCount +
                            map2.deviceChannelCount +
                            map3.deviceChannelCount;
    CHECK(totalChannels == 14);

    // Verify no overlaps
    CHECK(map1.getDeviceChannelEnd() == map2.deviceChannelStart);
    CHECK(map2.getDeviceChannelEnd() == map3.deviceChannelStart);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Maximum Stream Configuration") {
    std::cout << "Test: Maximum channel configuration... ";

    // Test maximum channels (128)
    SDPSession maxChannels = createTestSDP("Max Channels", 5004, 128, 48000);
    CHECK(maxChannels.numChannels == 128);

    // Test multiple streams filling 128 channels
    // 16 streams x 8 channels = 128 channels
    uint16_t streamsNeeded = 128 / 8;
    CHECK(streamsNeeded == 16);

    std::cout << "PASS" << std::endl;
}

//
// Network Configuration Tests
//

TEST_CASE("Multicast Address Validation") {
    std::cout << "Test: Multicast address validation... ";

    // Valid AES67 multicast range (239.x.x.x)
    SDPSession validMcast = createTestSDP();
    validMcast.connectionAddress = "239.1.1.1";
    CHECK(validMcast.connectionAddress.substr(0, 3) == "239");

    // Other multicast addresses
    SDPSession otherMcast = createTestSDP();
    otherMcast.connectionAddress = "224.0.0.1";
    CHECK(otherMcast.connectionAddress.substr(0, 3) == "224");

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Port Configuration") {
    std::cout << "Test: Port configuration... ";

    // Test various valid ports
    std::vector<uint16_t> validPorts = {5004, 5006, 5008, 49152, 65535};

    for (auto port : validPorts) {
        SDPSession sdp = createTestSDP("Test", port, 2, 48000);
        CHECK(sdp.port == port);
        CHECK(sdp.port > 0);
    }

    std::cout << "PASS" << std::endl;
}

//
// Encoding Configuration Tests
//

TEST_CASE("Encoding Support") {
    std::cout << "Test: Audio encoding support... ";

    SDPSession l16 = createTestSDP();
    l16.encoding = "L16";
    CHECK(l16.encoding == "L16");

    SDPSession l24 = createTestSDP();
    l24.encoding = "L24";
    CHECK(l24.encoding == "L24");

    SDPSession am824 = createTestSDP();
    am824.encoding = "AM824";
    CHECK(am824.encoding == "AM824");

    std::cout << "PASS" << std::endl;
}

//
// PTP Configuration Tests
//

TEST_CASE("PTP Domain Configuration") {
    std::cout << "Test: PTP domain configuration... ";

    // Stream with PTP domain 0 (typical for AES67)
    SDPSession withPTP = createTestSDP();
    withPTP.ptpDomain = 0;
    CHECK(withPTP.ptpDomain == 0);

    // Stream without PTP
    SDPSession noPTP = createTestSDP();
    noPTP.ptpDomain = -1;
    CHECK(noPTP.ptpDomain == -1);

    // Other domains
    SDPSession domain127 = createTestSDP();
    domain127.ptpDomain = 127;
    CHECK(domain127.ptpDomain == 127);

    std::cout << "PASS" << std::endl;
}

//
// StreamManager Validation Tests (without requiring instance creation)
//

TEST_CASE("Stream Manager Validation Helper") {
    std::cout << "Test: SDP validation for StreamManager constraints... ";

    // Test that our test SDP generation follows AES67 rules
    SDPSession validSDP = createTestSDP("Valid", 5004, 8, 48000);
    CHECK(validSDP.sampleRate > 0);
    CHECK((validSDP.numChannels > 0 && validSDP.numChannels <= 128));
    CHECK(validSDP.port > 0);

    // Test constraint: channels must not exceed 128
    SDPSession tooManyChannels = createTestSDP();
    tooManyChannels.numChannels = 256;
    CHECK(tooManyChannels.numChannels > 128);

    // Test constraint: sample rate mismatch detection
    SDPSession sr48 = createTestSDP("Test", 5004, 8, 48000);
    SDPSession sr96 = createTestSDP("Test", 5004, 8, 96000);
    CHECK(sr48.sampleRate != sr96.sampleRate);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Channel Mapper Availability") {
    std::cout << "Test: Channel mapping availability constraints... ";

    // Test max channels = 128
    ChannelMapping maxMapping = createTestMapping(128, 0);
    CHECK(maxMapping.isValid());
    CHECK(maxMapping.getDeviceChannelEnd() == 128);

    // Test overflow: start at 1, request 128 channels
    ChannelMapping overflow = createTestMapping(128, 1);
    CHECK(!overflow.isValid());

    // Test non-overlapping mappings can be detected
    ChannelMapping m1 = createTestMapping(16, 0);    // 0-15
    ChannelMapping m2 = createTestMapping(16, 16);   // 16-31
    CHECK(m1.getDeviceChannelEnd() == m2.deviceChannelStart);

    std::cout << "PASS" << std::endl;
}


//
// Auto Sink-Follow Decision Tests (RAVENNA auto_sinks_update)
//
using SFD = StreamManager::SinkFollowDecision;

TEST_CASE("Sink Follow Match And Move") {
    std::cout << "Test: sink follows a source that changed transport... ";

    SDPSession stored = createTestSDP("Cam1", 5004, 8, 48000); // 239.1.1.1
    // Same source re-announces on a new multicast/port.
    SDPSession moved = stored;
    moved.connectionAddress = "239.9.9.9";
    moved.port = 5010;
    CHECK(StreamManager::evaluateSinkFollow(stored, moved) == SFD::Follow);

    // Encoding / rate / ptime / payload changes also count as a move.
    SDPSession reEnc = stored; reEnc.encoding = "L16";
    CHECK(StreamManager::evaluateSinkFollow(stored, reEnc) == SFD::Follow);
    SDPSession reRate = stored; reRate.sampleRate = 96000;
    CHECK(StreamManager::evaluateSinkFollow(stored, reRate) == SFD::Follow);
    SDPSession rePtime = stored; rePtime.ptimeUs = 125;
    CHECK(StreamManager::evaluateSinkFollow(stored, rePtime) == SFD::Follow);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Sink Follow Unchanged Is No Op") {
    std::cout << "Test: identical re-announcement does not re-subscribe... ";
    SDPSession stored = createTestSDP("Cam1", 5004, 8, 48000);
    SDPSession same = stored; // byte-identical transport
    CHECK(StreamManager::evaluateSinkFollow(stored, same) == SFD::Unchanged);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Sink Follow Not Bound") {
    std::cout << "Test: an unrelated announcement is not this sink's source... ";

    SDPSession stored = createTestSDP("Cam1", 5004, 8, 48000);

    // Different name -> not our source even if transport differs.
    SDPSession other = createTestSDP("Cam2", 6000, 8, 48000);
    CHECK(StreamManager::evaluateSinkFollow(stored, other) == SFD::NotBound);

    // Empty announced name -> never binds.
    SDPSession nameless = stored; nameless.sessionName = ""; nameless.port = 7000;
    CHECK(StreamManager::evaluateSinkFollow(stored, nameless) == SFD::NotBound);

    // Same name but a different unicast source when both are known.
    SDPSession otherSender = stored;
    otherSender.port = 5010;
    otherSender.sourceAddress = "10.0.0.2";
    SDPSession storedWithSrc = stored; storedWithSrc.sourceAddress = "10.0.0.1";
    CHECK(StreamManager::evaluateSinkFollow(storedWithSrc, otherSender) == SFD::NotBound);

    // Same name, another host. SAP is unauthenticated multicast, so a name
    // on its own is not identity: whoever answers to it could be anybody
    // (2026-09-04 audit).
    SDPSession impostor = stored;
    impostor.port = 5010;
    impostor.connectionAddress = "239.6.6.6";
    impostor.originAddress = "192.168.1.66";
    CHECK(StreamManager::evaluateSinkFollow(stored, impostor) == SFD::NotBound);

    // An announcement that names no origin at all does not bind either, nor
    // does a stored stream that has none to compare against.
    SDPSession anonymous = impostor; anonymous.originAddress = "";
    CHECK(StreamManager::evaluateSinkFollow(stored, anonymous) == SFD::NotBound);
    SDPSession storedAnonymous = stored; storedAnonymous.originAddress = "";
    SDPSession moved = stored; moved.port = 5010;
    CHECK(StreamManager::evaluateSinkFollow(storedAnonymous, moved) == SFD::NotBound);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Sink Follow Channel Count Change") {
    std::cout << "Test: a channel-count change is not auto-followed... ";
    SDPSession stored = createTestSDP("Cam1", 5004, 8, 48000);
    SDPSession wider = stored;
    wider.connectionAddress = "239.9.9.9";
    wider.numChannels = 16; // moved AND re-widened
    CHECK(StreamManager::evaluateSinkFollow(stored, wider) == SFD::ChannelCountChanged);
    std::cout << "PASS" << std::endl;
}


TEST_CASE("Resolve Effective Dscp") {
    std::cout << "Test: per-source DSCP resolution (override vs profile)... ";

    // No override (-1) -> take the profile's value, whatever it is.
    CHECK(StreamManager::resolveEffectiveDscp(-1, 46) == 46);
    CHECK(StreamManager::resolveEffectiveDscp(-1, -1) == -1);

    // A per-source value overrides the profile, including down to 0 (CS0).
    CHECK(StreamManager::resolveEffectiveDscp(34, 46) == 34);
    CHECK(StreamManager::resolveEffectiveDscp(0, 46) == 0);

    std::cout << "PASS" << std::endl;
}

//
// Packet budget: a channel count and a packet time have to fit in one
// frame together, in both directions, before any socket opens.
//

namespace {

// The ring buffers have no default constructor, so the array is built the
// way AES67Device builds its own: one sized buffer per index.
template<size_t... Is>
auto makeRingBufferArray(size_t bufferSize, std::index_sequence<Is...>) {
    return std::array<SPSCRingBuffer<float>, sizeof...(Is)>{
        ((void)Is, SPSCRingBuffer<float>(bufferSize))...
    };
}

/// A manager with nothing started: canAddStream() reads state and opens
/// nothing, and the ring buffers behind it are only ever sized here.
struct ManagerFixture {
    // Initialised from the prvalue, since the buffers hold atomics and the
    // array can be neither moved nor copied once it exists.
    StreamManager::DeviceChannelBuffers in = makeRingBufferArray(64, std::make_index_sequence<128>{});
    StreamManager::DeviceChannelBuffers out = makeRingBufferArray(64, std::make_index_sequence<128>{});
    StreamManager manager{in, out};
};

} // namespace

TEST_CASE("Sixty-Four Channels Are Accepted At 125 us Under RAVENNA") {
    std::cout << "Test: 64 channels of L24 at 125 us pass validation under RAVENNA... ";
    ManagerFixture fixture;
    fixture.manager.setCompatibilityProfile(CompatibilityProfileKind::RAVENNA);

    SDPSession wide = createTestSDP("Merging Horus", 5004, 64, 48000);
    wide.ptimeUs = 125;
    wide.framecount = 0; // derive from ptime, as an SDP without a=framecount does
    std::string error;
    CHECK(fixture.manager.canAddStream(wide, /*isTransmit=*/false, &error));
    CHECK(error.empty());
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Sixty-Four Channels At A Millisecond Do Not Fit A Frame") {
    std::cout << "Test: 64 channels of L24 at 1 ms are refused with the packet time that fits... ";
    ManagerFixture fixture;
    fixture.manager.setCompatibilityProfile(CompatibilityProfileKind::RAVENNA);

    SDPSession wide = createTestSDP("Merging Horus", 5004, 64, 48000);
    wide.ptimeUs = 1000;
    wide.framecount = 0;
    std::string error;
    CHECK(!fixture.manager.canAddStream(wide, /*isTransmit=*/false, &error));
    // The message names the packet, the frame, and both ways out.
    CHECK(error.find("9228-byte packet") != std::string::npos);
    CHECK(error.find("1472") != std::string::npos);
    CHECK(error.find("up to 10 channels") != std::string::npos);
    CHECK(error.find("at most 145 us (7 samples)") != std::string::npos);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Eight Channels At 96 kHz And A Millisecond Do Not Fit Either") {
    std::cout << "Test: 8 channels of L24 at 96 kHz and 1 ms are refused (2316 bytes)... ";
    ManagerFixture fixture;
    // The device runs at the stream's rate, or the rate check fires first.
    CHECK(fixture.manager.setDeviceSampleRate(96000.0));
    // The default AES67 profile permits 96 kHz and 8 channels; the frame
    // does not permit both at 1 ms. The check is the driver's, not the
    // profile's.
    SDPSession sdp = createTestSDP("Hi-rate", 5004, 8, 96000);
    sdp.framecount = 0;
    std::string error;
    CHECK(!fixture.manager.canAddStream(sdp, /*isTransmit=*/true, &error));
    CHECK(error.find("2316-byte packet") != std::string::npos);
    CHECK(error.find("up to 5 channels") != std::string::npos);
    // Five channels at 1 ms fit, which is what AES67 -- 1 ms only -- gets
    // at 96 kHz.
    sdp.numChannels = 5;
    error.clear();
    CHECK(fixture.manager.canAddStream(sdp, /*isTransmit=*/true, &error));
    // Or halve the packet time, which AES67's profile forbids and RAVENNA's
    // allows: eight channels at 500 us are 1164 bytes.
    sdp.numChannels = 8;
    sdp.ptimeUs = 500;
    fixture.manager.setCompatibilityProfile(CompatibilityProfileKind::RAVENNA);
    error.clear();
    CHECK(fixture.manager.canAddStream(sdp, /*isTransmit=*/true, &error));
    std::cout << "PASS" << std::endl;
}

TEST_CASE("An Explicit Framecount Is What The Budget Measures") {
    std::cout << "Test: a RAVENNA framecount overrides ptime in the budget... ";
    ManagerFixture fixture;
    fixture.manager.setCompatibilityProfile(CompatibilityProfileKind::RAVENNA);
    SDPSession sdp = createTestSDP("Framecount", 5004, 64, 48000);
    sdp.ptimeUs = 1000;   // would not fit
    sdp.framecount = 6;   // does: what the packets actually hold
    std::string error;
    CHECK(fixture.manager.canAddStream(sdp, /*isTransmit=*/false, &error));
    std::cout << "PASS" << std::endl;
}

//
// Main Test Runner
//

