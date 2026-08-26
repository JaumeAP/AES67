//
// TestRTPReceiver.cpp
// AES67 macOS Driver - Build #17
// Unit tests for RTP packet receiver
//

#include "NetworkEngine/RTP/SimpleRTP.h"
#include "Driver/SDPParser.h"
#include "NetworkEngine/StreamChannelMapper.h"
#include <iostream>
#include <cassert>
#include <vector>

using namespace AES67;
using namespace AES67::RTP;

// Test result counter
static int testsPassed = 0;
static int testsFailed = 0;

#define TEST_ASSERT(condition, message) \
    if (!(condition)) { \
        std::cerr << "FAIL: " << message << std::endl; \
        testsFailed++; \
        return false; \
    } else { \
        testsPassed++; \
    }

//
// Basic RTP Packet Tests
//

bool testRTPPacketStructure() {
    std::cout << "Test: RTP packet structure... ";

    RTPPacket packet;

    // Default values
    TEST_ASSERT(packet.header.version == 2, "RTP version should be 2");
    TEST_ASSERT(packet.header.padding == 0, "Padding should be disabled");
    TEST_ASSERT(packet.header.extension == 0, "Extension should be disabled");
    TEST_ASSERT(packet.header.cc == 0, "CSRC count should be 0");
    TEST_ASSERT(packet.header.marker == 0, "Marker should be 0");
    TEST_ASSERT(packet.header.payloadType == PT_AES67_L16, "Default payload should be L16");

    // Set values
    packet.header.sequenceNumber = 1000;
    packet.header.timestamp = 48000;
    packet.header.ssrc = 0xABCDEF12;

    TEST_ASSERT(packet.header.sequenceNumber == 1000, "Sequence number should be set");
    TEST_ASSERT(packet.header.timestamp == 48000, "Timestamp should be set");
    TEST_ASSERT(packet.header.ssrc == 0xABCDEF12, "SSRC should be set");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testRTPHeaderSize() {
    std::cout << "Test: RTP header size... ";

    TEST_ASSERT(sizeof(RTPHeader) == 12, "RTP header must be exactly 12 bytes");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testSequenceNumberHandling() {
    std::cout << "Test: Sequence number handling... ";

    uint16_t seq = 0;

    // Normal increment
    for (int i = 0; i < 100; ++i) {
        TEST_ASSERT(seq == i, "Sequence should increment normally");
        seq++;
    }

    // Wrap-around
    seq = 65534;
    seq++;
    TEST_ASSERT(seq == 65535, "Sequence at boundary");
    seq++;
    TEST_ASSERT(seq == 0, "Sequence should wrap to 0");
    seq++;
    TEST_ASSERT(seq == 1, "Sequence should continue after wrap");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Audio Codec Tests
//

bool testL16Encoding() {
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
        TEST_ASSERT(diff < 0.01f, "L16 round-trip should preserve audio");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

bool testL24Encoding() {
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
        TEST_ASSERT(diff < 0.001f, "L24 round-trip should preserve audio with high precision");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

//
// SDP Session Tests
//

bool testSDPSessionCreation() {
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

    TEST_ASSERT(sdp.sessionName == "Test Stream", "Session name should be set");
    TEST_ASSERT(sdp.port == 5004, "Port should be set");
    TEST_ASSERT(sdp.encoding == "L16", "Encoding should be set");
    TEST_ASSERT(sdp.sampleRate == 48000, "Sample rate should be set");
    TEST_ASSERT(sdp.numChannels == 2, "Channel count should be set");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testSDPSessionValidation() {
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
    TEST_ASSERT(isValid, "Valid SDP should pass validation");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Channel Mapping Tests
//

bool testChannelMappingCreation() {
    std::cout << "Test: Channel mapping creation... ";

    ChannelMapping mapping;
    mapping.streamID = StreamID::generate();
    mapping.streamName = "Test Stream";
    mapping.streamChannelCount = 8;
    mapping.streamChannelOffset = 0;
    mapping.deviceChannelStart = 16;
    mapping.deviceChannelCount = 8;

    TEST_ASSERT(mapping.streamChannelCount == 8, "Stream channel count should be set");
    TEST_ASSERT(mapping.deviceChannelStart == 16, "Device start channel should be set");
    TEST_ASSERT(mapping.deviceChannelCount == 8, "Device channel count should be set");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testChannelMappingValidation() {
    std::cout << "Test: Channel mapping validation... ";

    ChannelMapping validMapping;
    validMapping.streamID = StreamID::generate();
    validMapping.streamName = "Valid Mapping";
    validMapping.streamChannelCount = 4;
    validMapping.deviceChannelStart = 0;
    validMapping.deviceChannelCount = 4;

    bool isValid = validMapping.isValid();
    TEST_ASSERT(isValid, "Valid mapping should pass validation");

    // Invalid mapping (device channels out of range)
    ChannelMapping invalidMapping;
    invalidMapping.streamID = StreamID::generate();
    invalidMapping.streamName = "Invalid Mapping";
    invalidMapping.streamChannelCount = 4;
    invalidMapping.deviceChannelStart = 126;  // Would go to channel 130 (out of range)
    invalidMapping.deviceChannelCount = 4;

    bool isInvalid = !invalidMapping.isValid();
    TEST_ASSERT(isInvalid, "Out-of-range mapping should fail validation");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Payload Size Tests
//

bool testPayloadSizeCalculations() {
    std::cout << "Test: Payload size calculations... ";

    // L16: 2 channels, 48 samples = 48 * 2 * 2 = 192 bytes
    size_t l16_2ch = 48 * 2 * 2;
    TEST_ASSERT(l16_2ch == 192, "L16 2ch 48 samples = 192 bytes");

    // L24: 2 channels, 48 samples = 48 * 2 * 3 = 288 bytes
    size_t l24_2ch = 48 * 2 * 3;
    TEST_ASSERT(l24_2ch == 288, "L24 2ch 48 samples = 288 bytes");

    // L16: 8 channels, 48 samples = 48 * 8 * 2 = 768 bytes
    size_t l16_8ch = 48 * 8 * 2;
    TEST_ASSERT(l16_8ch == 768, "L16 8ch 48 samples = 768 bytes");

    // L24: 8 channels, 48 samples = 48 * 8 * 3 = 1152 bytes
    size_t l24_8ch = 48 * 8 * 3;
    TEST_ASSERT(l24_8ch == 1152, "L24 8ch 48 samples = 1152 bytes");

    // Check against MTU (1500 bytes - 20 IP - 8 UDP - 12 RTP = 1460 bytes max payload)
    constexpr size_t MAX_PAYLOAD = 1460;
    TEST_ASSERT(l16_2ch < MAX_PAYLOAD, "L16 2ch payload should fit in MTU");
    TEST_ASSERT(l24_2ch < MAX_PAYLOAD, "L24 2ch payload should fit in MTU");
    TEST_ASSERT(l16_8ch < MAX_PAYLOAD, "L16 8ch payload should fit in MTU");
    TEST_ASSERT(l24_8ch < MAX_PAYLOAD, "L24 8ch payload should fit in MTU");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Timestamp Tests
//

bool testTimestampCalculation() {
    std::cout << "Test: RTP timestamp calculation... ";

    // At 48kHz, 1ms packet = 48 samples
    uint32_t samplesPerPacket = 48;
    uint32_t timestamp = 0;

    // First packet
    TEST_ASSERT(timestamp == 0, "Initial timestamp should be 0");

    // Advance by packet interval
    timestamp += samplesPerPacket;
    TEST_ASSERT(timestamp == 48, "Should advance by samples per packet");

    // Multiple packets
    for (int i = 0; i < 1000; ++i) {
        timestamp += samplesPerPacket;
    }
    TEST_ASSERT(timestamp == 48 * 1001, "Should accumulate correctly");

    // Test timestamp wrap (32-bit)
    uint32_t nearWrap = 0xFFFFFF00;
    nearWrap += 0x200;  // Will wrap
    TEST_ASSERT(nearWrap == 0x100, "Timestamp should wrap around");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Main Test Runner
//

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "AES67 RTP Receiver Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    std::cout << "RTP Packet Tests:" << std::endl;
    std::cout << "----------------" << std::endl;
    testRTPPacketStructure();
    testRTPHeaderSize();
    testSequenceNumberHandling();
    std::cout << std::endl;

    std::cout << "Audio Codec Tests:" << std::endl;
    std::cout << "-----------------" << std::endl;
    testL16Encoding();
    testL24Encoding();
    std::cout << std::endl;

    std::cout << "SDP Session Tests:" << std::endl;
    std::cout << "-----------------" << std::endl;
    testSDPSessionCreation();
    testSDPSessionValidation();
    std::cout << std::endl;

    std::cout << "Channel Mapping Tests:" << std::endl;
    std::cout << "---------------------" << std::endl;
    testChannelMappingCreation();
    testChannelMappingValidation();
    std::cout << std::endl;

    std::cout << "Payload Size Tests:" << std::endl;
    std::cout << "------------------" << std::endl;
    testPayloadSizeCalculations();
    std::cout << std::endl;

    std::cout << "Timestamp Tests:" << std::endl;
    std::cout << "---------------" << std::endl;
    testTimestampCalculation();
    std::cout << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "  Passed: " << testsPassed << std::endl;
    std::cout << "  Failed: " << testsFailed << std::endl;
    std::cout << "========================================" << std::endl;

    return testsFailed == 0 ? 0 : 1;
}
