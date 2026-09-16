//
// TestRTPReceiver.cpp
// AES67 macOS Driver - Build #17
// Unit tests for RTP packet receiver
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/RTP/SimpleRTP.h"
#include "Driver/SDPParser.h"
#include "NetworkEngine/StreamChannelMapper.h"
#include <cstdint>
#include <iostream>
#include <vector>

using namespace AES67;
using namespace AES67::RTP;

// Test result counter


//
// Framing: which bytes of a datagram are audio
//

namespace {

/// A datagram with `csrcCount` CSRC entries, an optional extension of
/// `extensionWords` 32-bit words, `payload` bytes of audio and `padding`
/// bytes of trailing padding (0 = none).
std::vector<uint8_t> buildDatagram(int csrcCount, int extensionWords,
                                   size_t payloadBytes, size_t padding) {
    std::vector<uint8_t> packet;
    packet.push_back(static_cast<uint8_t>(0x80 | (extensionWords >= 0 ? 0x10 : 0) |
                                          (padding > 0 ? 0x20 : 0) | csrcCount));
    packet.push_back(97);                       // payload type, marker clear
    packet.insert(packet.end(), {0x00, 0x01});  // sequence number
    packet.insert(packet.end(), {0, 0, 0, 0});  // timestamp
    packet.insert(packet.end(), {0, 0, 0, 1});  // SSRC
    for (int i = 0; i < csrcCount; ++i) packet.insert(packet.end(), {0xC5, 0xC5, 0xC5, 0xC5});
    if (extensionWords >= 0) {
        packet.insert(packet.end(), {0xBE, 0xDE});  // profile
        packet.push_back(static_cast<uint8_t>((extensionWords >> 8) & 0xFF));
        packet.push_back(static_cast<uint8_t>(extensionWords & 0xFF));
        for (int i = 0; i < extensionWords * 4; ++i) packet.push_back(0xEE);
    }
    for (size_t i = 0; i < payloadBytes; ++i) packet.push_back(0xA0);
    for (size_t i = 0; i + 1 < padding; ++i) packet.push_back(0x00);
    if (padding > 0) packet.push_back(static_cast<uint8_t>(padding));
    return packet;
}

} // namespace

TEST_CASE("Framing skips the CSRC list") {
    std::cout << "Test: a contributing-source list is not audio... ";

    const std::vector<uint8_t> datagram = buildDatagram(3, -1, 64, 0);
    RTPPacket packet;
    REQUIRE(RTPSocket::parseFrame(datagram.data(), datagram.size(), packet));
    CHECK(packet.header.cc == 3);
    CHECK(packet.payloadSize == 64);
    CHECK(packet.payload[0] == 0xA0);   // audio, not a CSRC byte

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Framing skips the extension header") {
    std::cout << "Test: an extension header is not audio... ";

    const std::vector<uint8_t> datagram = buildDatagram(0, 2, 48, 0);
    RTPPacket packet;
    REQUIRE(RTPSocket::parseFrame(datagram.data(), datagram.size(), packet));
    CHECK(packet.payloadSize == 48);
    CHECK(packet.payload[0] == 0xA0);

    // Both at once, which is what makes the offset arithmetic worth testing.
    const std::vector<uint8_t> both = buildDatagram(2, 1, 32, 0);
    RTPPacket second;
    REQUIRE(RTPSocket::parseFrame(both.data(), both.size(), second));
    CHECK(second.payloadSize == 32);
    CHECK(second.payload[0] == 0xA0);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Framing drops trailing padding") {
    std::cout << "Test: padding is not audio... ";

    const std::vector<uint8_t> datagram = buildDatagram(0, -1, 40, 8);
    RTPPacket packet;
    REQUIRE(RTPSocket::parseFrame(datagram.data(), datagram.size(), packet));
    CHECK(packet.header.padding == 1);
    CHECK(packet.payloadSize == 40);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Framing refuses what does not add up") {
    std::cout << "Test: a datagram smaller than what it declares is rejected... ";

    RTPPacket packet;

    // Shorter than the fixed header.
    const std::vector<uint8_t> runt(8, 0x80);
    CHECK_FALSE(RTPSocket::parseFrame(runt.data(), runt.size(), packet));

    // A CSRC count that eats the whole datagram, leaving no payload.
    const std::vector<uint8_t> allHeader = buildDatagram(4, -1, 0, 0);
    CHECK_FALSE(RTPSocket::parseFrame(allHeader.data(), allHeader.size(), packet));

    // An extension length longer than the datagram.
    std::vector<uint8_t> lying = buildDatagram(0, 1, 16, 0);
    lying[15] = 0xFF; // extension length, low byte: 255 words that are not there
    CHECK_FALSE(RTPSocket::parseFrame(lying.data(), lying.size(), packet));

    // Padding that claims more than the payload holds.
    std::vector<uint8_t> overPadded = buildDatagram(0, -1, 16, 4);
    overPadded.back() = 0xFF;
    CHECK_FALSE(RTPSocket::parseFrame(overPadded.data(), overPadded.size(), packet));

    std::cout << "PASS" << std::endl;
}

//
// Basic RTP Packet Tests
//

TEST_CASE("RTP Packet Structure") {
    std::cout << "Test: RTP packet structure... ";

    RTPPacket packet;

    // Default values
    CHECK(packet.header.version == 2);
    CHECK(packet.header.padding == 0);
    CHECK(packet.header.extension == 0);
    CHECK(packet.header.cc == 0);
    CHECK(packet.header.marker == 0);
    CHECK(packet.header.payloadType == PT_AES67_L16);

    // Set values
    packet.header.sequenceNumber = 1000;
    packet.header.timestamp = 48000;
    packet.header.ssrc = 0xABCDEF12;

    CHECK(packet.header.sequenceNumber == 1000);
    CHECK(packet.header.timestamp == 48000);
    CHECK(packet.header.ssrc == 0xABCDEF12);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("RTP Header Size") {
    std::cout << "Test: RTP header size... ";

    CHECK(sizeof(RTPHeader) == 12);

    std::cout << "PASS" << std::endl;
}

//
// What a sequence number says about the one before it
//
// This replaced two test cases, one here and one in TestRTPTransmitter, that
// declared a local uint16_t and checked that C++ wraps it at 65535. Neither
// reached a line of this repository. What decides whether a packet was lost
// or merely late is RTP::sequenceGap, which RTPReceiver::updateStats counts
// with, and nothing had ever run it.
//

TEST_CASE("A sequence number one on from the last loses nothing") {
    const SequenceGap gap = sequenceGap(/*expected=*/41, /*received=*/41);

    CHECK(gap.lost == 0);
    CHECK_FALSE(gap.outOfOrder);
}

TEST_CASE("A forward gap is the packets that did not arrive") {
    // Expecting 42 and getting 45 means 42, 43 and 44 never came: the one
    // that was due and the two behind it. The count is the distance, which
    // is what RFC 3550's expected-minus-received is.
    CHECK(sequenceGap(42, 45).lost == 3);
    CHECK_FALSE(sequenceGap(42, 45).outOfOrder);

    CHECK(sequenceGap(0, 1).lost == 1);
    CHECK(sequenceGap(1000, 1001).lost == 1);
}

TEST_CASE("A backward gap is one packet that came late") {
    // Behind is not a loss: the packet is here, out of order, and counting it
    // as tens of thousands lost is what unsigned arithmetic would have done.
    const SequenceGap late = sequenceGap(/*expected=*/100, /*received=*/98);

    CHECK(late.lost == 0);
    CHECK(late.outOfOrder);

    // A repeat of the one before is backward by one, and counts the same.
    CHECK(sequenceGap(100, 99).outOfOrder);
}

TEST_CASE("The wrap at 65535 is a step forward, not sixty-five thousand lost") {
    // The whole reason the difference is read as a signed 16-bit value.
    // Unsigned, 0 minus 65535 is 1 -- right by luck -- but 1 minus 65535 is
    // 2 and 65535 minus 0 is 65535, which as a loss count would swamp every
    // statistic the driver reports.
    CHECK(sequenceGap(/*expected=*/0, /*received=*/0).lost == 0);
    CHECK(sequenceGap(/*expected=*/65535, /*received=*/0).lost == 1);
    CHECK(sequenceGap(/*expected=*/65535, /*received=*/2).lost == 3);

    // And backward across the same wrap is still just late.
    const SequenceGap late = sequenceGap(/*expected=*/1, /*received=*/65534);
    CHECK(late.lost == 0);
    CHECK(late.outOfOrder);
}

TEST_CASE("Half the sequence space is where forward stops being forward") {
    // A two's-complement difference splits the 65536 values in half: up to
    // 32767 ahead reads as a gap, and 32768 or more reads as behind. That is
    // the definition, and it is worth having written down -- 32768 packets
    // is 32 seconds at 1 ms, so either answer is a stream that is gone.
    CHECK(sequenceGap(0, 32767).lost == 32767);
    CHECK_FALSE(sequenceGap(0, 32767).outOfOrder);

    CHECK(sequenceGap(0, 32768).lost == 0);
    CHECK(sequenceGap(0, 32768).outOfOrder);
}

//
// Audio Codec Tests
//

TEST_CASE("L16 Encoding") {
    std::cout << "Test: L16 audio encoding/decoding... ";

    // Create test audio: 4 samples
    float audio[4] = {0.5f, -0.5f, 1.0f, -1.0f};

    // Encode to L16
    uint8_t encoded[8];  // 4 samples * 2 bytes
    L16Codec::encode(audio, 4, encoded);

    // Decode back
    float decoded[4];
    L16Codec::decode(encoded, 8, decoded);

    // Verify round-trip (allow small tolerance)
    for (int i = 0; i < 4; ++i) {
        float diff = std::abs(decoded[i] - audio[i]);
        CHECK(diff < 0.01f);
    }

    std::cout << "PASS" << std::endl;
}

TEST_CASE("L24 Encoding") {
    std::cout << "Test: L24 audio encoding/decoding... ";

    // Create test audio: 4 samples
    float audio[4] = {0.5f, -0.5f, 1.0f, -1.0f};

    // Encode to L24
    uint8_t encoded[12];  // 4 samples * 3 bytes
    L24Codec::encode(audio, 4, encoded);

    // Decode back
    float decoded[4];
    L24Codec::decode(encoded, 12, decoded);

    // Verify round-trip (L24 has better precision)
    for (int i = 0; i < 4; ++i) {
        float diff = std::abs(decoded[i] - audio[i]);
        CHECK(diff < 0.001f);
    }

    std::cout << "PASS" << std::endl;
}

//
// SDP Session Tests
//

TEST_CASE("SDP Session Creation") {
    std::cout << "Test: SDP session creation... ";

    SDPSession sdp;
    sdp.sessionName = "Test Stream";
    sdp.port = 5004;
    sdp.encoding = "L16";
    sdp.sampleRate = 48000;
    sdp.numChannels = 2;
    sdp.connectionAddress = "239.1.1.1";
    sdp.ttl = 32;
    sdp.payloadType = PT_AES67_L16;

    CHECK(sdp.sessionName == "Test Stream");
    CHECK(sdp.port == 5004);
    CHECK(sdp.encoding == "L16");
    CHECK(sdp.sampleRate == 48000);
    CHECK(sdp.numChannels == 2);
    CHECK(sdp.connectionAddress == "239.1.1.1");
    CHECK(sdp.ttl == 32);
    CHECK(sdp.payloadType == PT_AES67_L16);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("SDP Session Validation") {
    std::cout << "Test: SDP session validation... ";

    // Create valid SDP
    SDPSession validSDP;
    validSDP.sessionName = "Valid Stream";
    validSDP.port = 5004;
    validSDP.encoding = "L24";
    validSDP.sampleRate = 48000;
    validSDP.numChannels = 8;
    validSDP.connectionAddress = "239.1.1.1";

    bool isValid = validSDP.isValid();
    CHECK(isValid);

    std::cout << "PASS" << std::endl;
}

//
// Channel Mapping Tests
//

TEST_CASE("Channel Mapping Creation") {
    std::cout << "Test: Channel mapping creation... ";

    ChannelMapping mapping;
    mapping.streamID = StreamID::generate();
    mapping.streamName = "Test Stream";
    mapping.streamChannelCount = 8;
    mapping.streamChannelOffset = 0;
    mapping.deviceChannelStart = 16;
    mapping.deviceChannelCount = 8;

    CHECK(mapping.streamName == "Test Stream");
    CHECK(mapping.streamChannelCount == 8);
    CHECK(mapping.streamChannelOffset == 0);
    CHECK(mapping.deviceChannelStart == 16);
    CHECK(mapping.deviceChannelCount == 8);
    CHECK(!mapping.streamID.isNull());

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Channel Mapping Validation") {
    std::cout << "Test: Channel mapping validation... ";

    ChannelMapping validMapping;
    validMapping.streamID = StreamID::generate();
    validMapping.streamName = "Valid Mapping";
    validMapping.streamChannelCount = 4;
    validMapping.deviceChannelStart = 0;
    validMapping.deviceChannelCount = 4;

    bool isValid = validMapping.isValid();
    CHECK(isValid);

    // Invalid mapping (device channels out of range)
    ChannelMapping invalidMapping;
    invalidMapping.streamID = StreamID::generate();
    invalidMapping.streamName = "Invalid Mapping";
    invalidMapping.streamChannelCount = 4;
    invalidMapping.deviceChannelStart = 126;  // Would go to channel 130 (out of range)
    invalidMapping.deviceChannelCount = 4;

    bool isInvalid = !invalidMapping.isValid();
    CHECK(isInvalid);

    std::cout << "PASS" << std::endl;
}

//
// Payload Size Tests
//

TEST_CASE("Payload Size Calculations") {
    std::cout << "Test: Payload size calculations... ";

    // L16: 2 channels, 48 samples = 48 * 2 * 2 = 192 bytes
    size_t l16_2ch = 48 * 2 * 2;
    CHECK(l16_2ch == 192);

    // L24: 2 channels, 48 samples = 48 * 2 * 3 = 288 bytes
    size_t l24_2ch = 48 * 2 * 3;
    CHECK(l24_2ch == 288);

    // L16: 8 channels, 48 samples = 48 * 8 * 2 = 768 bytes
    size_t l16_8ch = 48 * 8 * 2;
    CHECK(l16_8ch == 768);

    // L24: 8 channels, 48 samples = 48 * 8 * 3 = 1152 bytes
    size_t l24_8ch = 48 * 8 * 3;
    CHECK(l24_8ch == 1152);

    // Check against MTU (1500 bytes - 20 IP - 8 UDP - 12 RTP = 1460 bytes max payload)
    constexpr size_t MAX_PAYLOAD = 1460;
    CHECK(l16_2ch < MAX_PAYLOAD);
    CHECK(l24_2ch < MAX_PAYLOAD);
    CHECK(l16_8ch < MAX_PAYLOAD);
    CHECK(l24_8ch < MAX_PAYLOAD);

    std::cout << "PASS" << std::endl;
}

//
// Timestamp Tests
//

TEST_CASE("Timestamp Calculation") {
    std::cout << "Test: RTP timestamp calculation... ";

    // At 48kHz, 1ms packet = 48 samples
    uint32_t samplesPerPacket = 48;
    uint32_t timestamp = 0;

    // First packet
    CHECK(timestamp == 0);

    // Advance by packet interval
    timestamp += samplesPerPacket;
    CHECK(timestamp == 48);

    // Multiple packets
    for (int i = 0; i < 1000; ++i) {
        timestamp += samplesPerPacket;
    }
    CHECK(timestamp == 48 * 1001);

    // Test timestamp wrap (32-bit)
    uint32_t nearWrap = 0xFFFFFF00;
    nearWrap += 0x200;  // Will wrap
    CHECK(nearWrap == 0x100);

    std::cout << "PASS" << std::endl;
}

//
// Main Test Runner
//


TEST_CASE("A unicast destination opens without a group to join") {
    // openReceiver() joined a multicast group whatever address it was given,
    // and RTPReceiver hands it the connection address out of the session
    // description. A description naming a unicast address -- which the parser
    // takes, which IS-05 can stage, and which this package's own test sender
    // sends to -- failed at IP_ADD_MEMBERSHIP with EINVAL and the stream never
    // opened. Found by pointing ffmpeg at the receiver over the loopback.
    AES67::RTP::RTPSocket unicast;
    REQUIRE(unicast.openReceiver("127.0.0.1", 45111, nullptr));
    CHECK(unicast.isOpen());
    unicast.close();     // and closing one drops no membership it never took

    // The multicast path is unchanged: still joined, still on the interface it
    // was given. 239.255.0.0/16 is administratively scoped and goes nowhere.
    AES67::RTP::RTPSocket multicast;
    if (multicast.openReceiver("239.255.77.77", 45113, "127.0.0.1")) {
        CHECK(multicast.isOpen());
        multicast.close();
    } else {
        // A machine with no multicast route is not a failing driver: say so
        // and let the unicast half of the case stand on its own.
        MESSAGE("no multicast on this host; the unicast half of this case ran");
    }
}
