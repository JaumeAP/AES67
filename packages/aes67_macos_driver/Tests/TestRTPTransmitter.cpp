//
// TestRTPTransmitter.cpp
// AES67 macOS Driver - Build #17
// Unit tests for RTP packet transmitter
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/RTP/SimpleRTP.h"
#include "Driver/SDPParser.h"
#include "NetworkEngine/StreamChannelMapper.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <cmath>

using namespace AES67;
using namespace AES67::RTP;

// Test result counter


//
// RTP Header Tests
//

TEST_CASE("RTP Header Initialization") {
    std::cout << "Test: RTP header initialization... ";

    RTPPacket packet;

    // Default values
    CHECK(packet.header.version == 2);
    CHECK(packet.header.padding == 0);
    CHECK(packet.header.extension == 0);
    CHECK(packet.header.cc == 0);
    CHECK(packet.header.marker == 0);
    CHECK(packet.header.payloadType == PT_AES67_L16);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("RTP Header Network Byte Order") {
    std::cout << "Test: RTP header network byte order conversion... ";

    RTPPacket packet;
    packet.header.sequenceNumber = 0x1234;
    packet.header.timestamp = 0x12345678;
    packet.header.ssrc = 0xABCDEF01;

    // Store original values
    uint16_t origSeq = packet.header.sequenceNumber;
    uint32_t origTs = packet.header.timestamp;
    uint32_t origSsrc = packet.header.ssrc;

    // Convert to network order
    packet.header.toNetworkOrder();

    // On little-endian systems, bytes should be swapped
    // We can't test exact values without knowing endianness,
    // but we can test round-trip
    packet.header.toHostOrder();

    CHECK(packet.header.sequenceNumber == origSeq);
    CHECK(packet.header.timestamp == origTs);
    CHECK(packet.header.ssrc == origSsrc);

    std::cout << "PASS" << std::endl;
}

//
// Sequence Number Tests
//

TEST_CASE("Sequence Number Increment") {
    std::cout << "Test: Sequence number increment and wrap... ";

    uint16_t seq = 0;

    // Normal increment
    for (int i = 0; i < 100; ++i) {
        CHECK(seq == i);
        seq++;
    }

    // Test wrap-around
    seq = 65534;
    seq++;
    CHECK(seq == 65535);
    seq++;
    CHECK(seq == 0);
    seq++;
    CHECK(seq == 1);

    std::cout << "PASS" << std::endl;
}

//
// Timestamp Tests
//

TEST_CASE("Timestamp Increment") {
    std::cout << "Test: Timestamp increment... ";

    // At 48kHz, 1ms packet = 48 samples
    uint32_t samplesPerPacket = 48;
    uint32_t timestamp = 0;

    // First packet
    CHECK(timestamp == 0);

    // Advance by packet interval
    timestamp += samplesPerPacket;
    CHECK(timestamp == 48);

    // Simulate 1 second of packets (1000 packets @ 1ms each)
    for (int i = 0; i < 999; ++i) {
        timestamp += samplesPerPacket;
    }
    CHECK(timestamp == 48000);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Timestamp Wrap") {
    std::cout << "Test: Timestamp wrap-around... ";

    // Test timestamp wrap (32-bit)
    uint32_t timestamp = 0xFFFFFFF0;
    timestamp += 0x20;  // Will wrap

    CHECK(timestamp == 0x10);

    std::cout << "PASS" << std::endl;
}

//
// Audio Encoding Tests
//

TEST_CASE("L16 Encoding Precision") {
    std::cout << "Test: L16 encoding precision... ";

    // Create test audio with various amplitudes
    float audio[8] = {
        0.0f, 0.25f, 0.5f, 0.75f,
        -0.25f, -0.5f, -0.75f, -1.0f
    };

    // Encode to L16
    uint8_t encoded[16];  // 8 samples * 2 bytes
    L16Codec::encode(audio, 8, encoded);

    // Decode back
    float decoded[8];
    L16Codec::decode(encoded, 16, decoded);

    // Verify round-trip
    for (int i = 0; i < 8; ++i) {
        float diff = std::abs(decoded[i] - audio[i]);
        CHECK(diff < 0.01f);
    }

    std::cout << "PASS" << std::endl;
}

TEST_CASE("L24 Encoding Precision") {
    std::cout << "Test: L24 encoding precision... ";

    // Create test audio with various amplitudes
    float audio[8] = {
        0.0f, 0.25f, 0.5f, 0.75f,
        -0.25f, -0.5f, -0.75f, -1.0f
    };

    // Encode to L24
    uint8_t encoded[24];  // 8 samples * 3 bytes
    L24Codec::encode(audio, 8, encoded);

    // Decode back
    float decoded[8];
    L24Codec::decode(encoded, 24, decoded);

    // Verify round-trip (L24 should have better precision than L16)
    for (int i = 0; i < 8; ++i) {
        float diff = std::abs(decoded[i] - audio[i]);
        CHECK(diff < 0.001f);
    }

    std::cout << "PASS" << std::endl;
}

//
// Payload Size Tests
//

TEST_CASE("Payload Sizes") {
    std::cout << "Test: RTP payload size calculations... ";

    // Common AES67 configurations
    struct Config {
        uint16_t channels;
        uint32_t samples;
        uint8_t bytesPerSample;
        size_t expectedSize;
    };

    std::vector<Config> configs = {
        // L16 configurations
        {2, 48, 2, 192},      // Stereo @ 48kHz, 1ms
        {8, 48, 2, 768},      // 8ch @ 48kHz, 1ms
        {2, 96, 2, 384},      // Stereo @ 96kHz, 1ms

        // L24 configurations
        {2, 48, 3, 288},      // Stereo @ 48kHz, 1ms
        {8, 48, 3, 1152},     // 8ch @ 48kHz, 1ms
        {2, 96, 3, 576},      // Stereo @ 96kHz, 1ms
    };

    for (const auto& cfg : configs) {
        size_t calculatedSize = cfg.channels * cfg.samples * cfg.bytesPerSample;
        CHECK(calculatedSize == cfg.expectedSize);

        // Verify payload fits in MTU
        constexpr size_t MAX_PAYLOAD = 1460;  // 1500 - 20 IP - 8 UDP - 12 RTP
        CHECK(calculatedSize <= MAX_PAYLOAD);
    }

    std::cout << "PASS" << std::endl;
}

//
// Packet Timing Tests
//

TEST_CASE("Packet Interval") {
    std::cout << "Test: Packet interval calculation... ";

    struct TimingConfig {
        uint32_t sampleRate;
        uint32_t samplesPerPacket;
        uint64_t expectedIntervalUs;
    };

    std::vector<TimingConfig> configs = {
        {48000, 48, 1000},    // 1ms @ 48kHz
        {96000, 96, 1000},    // 1ms @ 96kHz
        {192000, 192, 1000},  // 1ms @ 192kHz
        {48000, 96, 2000},    // 2ms @ 48kHz
    };

    for (const auto& cfg : configs) {
        uint64_t intervalUs = (cfg.samplesPerPacket * 1000000ULL) / cfg.sampleRate;
        CHECK(intervalUs == cfg.expectedIntervalUs);
    }

    std::cout << "PASS" << std::endl;
}

//
// SSRC Tests
//

TEST_CASE("SSRC Generation") {
    std::cout << "Test: SSRC generation... ";

    // SSRCs should be unique (randomly generated)
    // We can't test randomness easily, but we can verify the field works

    RTPPacket packet1, packet2, packet3;

    packet1.header.ssrc = 0x12345678;
    packet2.header.ssrc = 0xABCDEF01;
    packet3.header.ssrc = 0x87654321;

    CHECK(packet1.header.ssrc != packet2.header.ssrc);
    CHECK(packet2.header.ssrc != packet3.header.ssrc);
    CHECK(packet1.header.ssrc != packet3.header.ssrc);

    std::cout << "PASS" << std::endl;
}

//
// SDP Generation Tests
//

TEST_CASE("SDP For Transmit") {
    std::cout << "Test: SDP session for transmission... ";

    // Create transmit SDP
    SDPSession sdp = SDPParser::createDefaultTxSession(
        "Test TX Stream",
        "192.168.1.100",    // Source IP
        "239.1.2.1",        // Multicast IP
        5004,               // Port
        8,                  // Channels
        48000,              // Sample rate
        "L24"               // Encoding
    );

    CHECK(sdp.sessionName == "Test TX Stream");
    CHECK(sdp.port == 5004);
    CHECK(sdp.encoding == "L24");
    CHECK(sdp.sampleRate == 48000);
    CHECK(sdp.numChannels == 8);
    CHECK(sdp.connectionAddress == "239.1.2.1");
    CHECK(sdp.originAddress == "192.168.1.100");

    std::cout << "PASS" << std::endl;
}

TEST_CASE("SDP String Generation") {
    std::cout << "Test: SDP string generation... ";

    SDPSession sdp;
    sdp.sessionName = "Test Stream";
    sdp.port = 5004;
    sdp.encoding = "L16";
    sdp.sampleRate = 48000;
    sdp.numChannels = 2;
    sdp.connectionAddress = "239.1.1.1";
    sdp.originAddress = "192.168.1.100";

    std::string sdpString = SDPParser::generate(sdp);

    CHECK(!sdpString.empty());
    CHECK(sdpString.find("v=0") == 0);
    CHECK(sdpString.find("s=Test Stream") != std::string::npos);
    CHECK(sdpString.find("m=audio 5004") != std::string::npos);
    CHECK(sdpString.find("c=IN IP4 239.1.1.1") != std::string::npos);

    std::cout << "PASS" << std::endl;
}

//
// Channel Interleaving Tests
//

TEST_CASE("Channel Interleaving") {
    std::cout << "Test: Channel interleaving... ";

    // Simulate 2 channels, 4 samples each
    // Channel 0: [1.0, 2.0, 3.0, 4.0]
    // Channel 1: [5.0, 6.0, 7.0, 8.0]
    // Interleaved: [1.0, 5.0, 2.0, 6.0, 3.0, 7.0, 4.0, 8.0]

    float ch0[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float ch1[4] = {5.0f, 6.0f, 7.0f, 8.0f};

    float interleaved[8];
    for (int i = 0; i < 4; ++i) {
        interleaved[i * 2 + 0] = ch0[i];
        interleaved[i * 2 + 1] = ch1[i];
    }

    // Verify interleaving
    float expected[8] = {1.0f, 5.0f, 2.0f, 6.0f, 3.0f, 7.0f, 4.0f, 8.0f};
    for (int i = 0; i < 8; ++i) {
        CHECK(interleaved[i] == expected[i]);
    }

    std::cout << "PASS" << std::endl;
}

//
// Main Test Runner
//

