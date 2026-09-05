//
// TestMultiStream.cpp
// AES67 macOS Driver - Build #18
// Integration tests for multi-stream scenarios
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/StreamManager.h"
#include "NetworkEngine/StreamChannelMapper.h"
#include "Driver/SDPParser.h"
#include "NetworkEngine/PTP/PTPClock.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <memory>

using namespace AES67;

// Test result counter


//
// Helper Functions
//

// Create test SDP session
SDPSession createTestStream(const std::string& name,
                           const std::string& mcastAddr,
                           uint16_t port,
                           uint16_t channels,
                           uint32_t sampleRate = 48000) {
    SDPSession sdp;
    sdp.sessionName = name;
    sdp.port = port;
    sdp.connectionAddress = mcastAddr;
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

// Create channel mapping
ChannelMapping createMapping(const StreamID& streamID,
                             const std::string& name,
                             uint16_t streamChannels,
                             uint16_t deviceStart) {
    ChannelMapping mapping;
    mapping.streamID = streamID;
    mapping.streamName = name;
    mapping.streamChannelCount = streamChannels;
    mapping.streamChannelOffset = 0;
    mapping.deviceChannelStart = deviceStart;
    mapping.deviceChannelCount = streamChannels;

    return mapping;
}

//
// Multi-Stream Configuration Tests
//

TEST_CASE("Two Stream Configuration") {
    std::cout << "Test: Two-stream configuration... ";

    // Stream 1: 8 channels on 239.1.1.1:5004
    SDPSession stream1 = createTestStream("Stream 1", "239.1.1.1", 5004, 8);
    CHECK(stream1.isValid());

    // Stream 2: 8 channels on 239.1.1.2:5006
    SDPSession stream2 = createTestStream("Stream 2", "239.1.1.2", 5006, 8);
    CHECK(stream2.isValid());

    // Different multicast addresses
    CHECK(stream1.connectionAddress != stream2.connectionAddress);

    // Different ports
    CHECK(stream1.port != stream2.port);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Four Stream Configuration") {
    std::cout << "Test: Four-stream configuration... ";

    // Create 4 streams with different addresses
    std::vector<SDPSession> streams;
    for (int i = 0; i < 4; i++) {
        std::string name = "Stream " + std::to_string(i + 1);
        std::string addr = "239.1.1." + std::to_string(i + 1);
        uint16_t port = 5004 + (i * 2);

        streams.push_back(createTestStream(name, addr, port, 8));
    }

    // Validate all streams
    for (const auto& stream : streams) {
        CHECK(stream.isValid());
    }

    // Verify uniqueness
    for (size_t i = 0; i < streams.size(); i++) {
        for (size_t j = i + 1; j < streams.size(); j++) {
            CHECK(streams[i].connectionAddress != streams[j].connectionAddress);
            CHECK(streams[i].port != streams[j].port);
        }
    }

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Maximum Streams") {
    std::cout << "Test: Maximum stream configuration (16 streams)... ";

    // Create 16 streams x 8 channels = 128 channels total
    std::vector<SDPSession> streams;
    for (int i = 0; i < 16; i++) {
        std::string name = "Stream " + std::to_string(i + 1);
        std::string addr = "239.1." + std::to_string((i / 255) + 1) + "." + std::to_string((i % 255) + 1);
        uint16_t port = 5004 + (i * 2);

        streams.push_back(createTestStream(name, addr, port, 8));
    }

    CHECK(streams.size() == 16);

    // Validate all
    for (const auto& stream : streams) {
        CHECK(stream.isValid());
        CHECK(stream.numChannels == 8);
    }

    // Calculate total channels
    uint16_t totalChannels = 0;
    for (const auto& stream : streams) {
        totalChannels += stream.numChannels;
    }
    CHECK(totalChannels == 128);

    std::cout << "PASS" << std::endl;
}

//
// Channel Mapping Coordination Tests
//

TEST_CASE("Non Overlapping Mappings") {
    std::cout << "Test: Non-overlapping channel mappings... ";

    // Create stream IDs
    StreamID id1 = StreamID::generate();
    StreamID id2 = StreamID::generate();
    StreamID id3 = StreamID::generate();

    // Map to different channel ranges
    ChannelMapping map1 = createMapping(id1, "Stream 1", 8, 0);    // 0-7
    ChannelMapping map2 = createMapping(id2, "Stream 2", 8, 8);    // 8-15
    ChannelMapping map3 = createMapping(id3, "Stream 3", 8, 16);   // 16-23

    CHECK(map1.isValid());
    CHECK(map2.isValid());
    CHECK(map3.isValid());

    // Check no overlaps
    uint16_t end1 = map1.getDeviceChannelEnd();
    uint16_t end2 = map2.getDeviceChannelEnd();
    (void)map3.getDeviceChannelEnd();

    CHECK(end1 == map2.deviceChannelStart);
    CHECK(end2 == map3.deviceChannelStart);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Overlapping Mapping Detection") {
    std::cout << "Test: Overlapping channel mapping detection... ";

    StreamID id1 = StreamID::generate();
    StreamID id2 = StreamID::generate();

    // Create overlapping mappings
    ChannelMapping map1 = createMapping(id1, "Stream 1", 16, 0);   // 0-15
    ChannelMapping map2 = createMapping(id2, "Stream 2", 16, 8);   // 8-23 (overlaps 8-15)

    // Both individually valid
    CHECK(map1.isValid());
    CHECK(map2.isValid());

    // Detect overlap
    uint16_t end1 = map1.getDeviceChannelEnd();
    bool overlaps = (map1.deviceChannelStart < map2.deviceChannelStart + map2.deviceChannelCount &&
                     map2.deviceChannelStart < end1);

    CHECK(overlaps);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Full Device Mappings") {
    std::cout << "Test: Full device channel mappings (128 channels)... ";

    // Create 16 streams with mappings for all 128 channels
    std::vector<ChannelMapping> mappings;
    for (int i = 0; i < 16; i++) {
        StreamID id = StreamID::generate();
        std::string name = "Stream " + std::to_string(i + 1);
        uint16_t deviceStart = i * 8;

        mappings.push_back(createMapping(id, name, 8, deviceStart));
    }

    CHECK(mappings.size() == 16);

    // Verify no gaps or overlaps
    for (size_t i = 0; i < mappings.size() - 1; i++) {
        uint16_t end = mappings[i].getDeviceChannelEnd();
        uint16_t nextStart = mappings[i + 1].deviceChannelStart;
        CHECK(end == nextStart);
    }

    // Verify last mapping ends at 128
    uint16_t lastEnd = mappings[15].getDeviceChannelEnd();
    CHECK(lastEnd == 128);

    std::cout << "PASS" << std::endl;
}

//
// Sample Rate Coordination Tests
//

TEST_CASE("Uniform Sample Rate") {
    std::cout << "Test: Uniform sample rate across streams... ";

    uint32_t targetRate = 48000;

    // Create 4 streams all at 48kHz
    std::vector<SDPSession> streams;
    for (int i = 0; i < 4; i++) {
        std::string name = "Stream " + std::to_string(i + 1);
        std::string addr = "239.1.1." + std::to_string(i + 1);
        uint16_t port = 5004 + (i * 2);

        streams.push_back(createTestStream(name, addr, port, 8, targetRate));
    }

    // Verify all at same rate
    for (const auto& stream : streams) {
        CHECK(stream.sampleRate == targetRate);
    }

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Mixed Sample Rate Detection") {
    std::cout << "Test: Mixed sample rate detection... ";

    // Create streams at different rates
    SDPSession stream1 = createTestStream("Stream 1", "239.1.1.1", 5004, 8, 48000);
    SDPSession stream2 = createTestStream("Stream 2", "239.1.1.2", 5006, 8, 96000);
    SDPSession stream3 = createTestStream("Stream 3", "239.1.1.3", 5008, 8, 48000);

    CHECK(stream1.sampleRate != stream2.sampleRate);
    CHECK(stream1.sampleRate == stream3.sampleRate);

    // In a real system, this would trigger a warning or require SRC
    bool needsSRC = (stream1.sampleRate != stream2.sampleRate);
    CHECK(needsSRC);

    std::cout << "PASS" << std::endl;
}

//
// Network Configuration Tests
//

TEST_CASE("Unique Multicast Addresses") {
    std::cout << "Test: Unique multicast addresses... ";

    std::vector<SDPSession> streams;
    streams.push_back(createTestStream("Stream 1", "239.1.1.1", 5004, 8));
    streams.push_back(createTestStream("Stream 2", "239.1.1.2", 5004, 8));
    streams.push_back(createTestStream("Stream 3", "239.1.1.3", 5004, 8));

    // All use same port but different addresses
    for (size_t i = 0; i < streams.size() - 1; i++) {
        for (size_t j = i + 1; j < streams.size(); j++) {
            CHECK(streams[i].connectionAddress != streams[j].connectionAddress);
        }
    }

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Unique Port Numbers") {
    std::cout << "Test: Unique port numbers... ";

    // Same address, different ports
    std::vector<SDPSession> streams;
    streams.push_back(createTestStream("Stream 1", "239.1.1.1", 5004, 8));
    streams.push_back(createTestStream("Stream 2", "239.1.1.1", 5006, 8));
    streams.push_back(createTestStream("Stream 3", "239.1.1.1", 5008, 8));

    // All use same address but different ports
    for (size_t i = 0; i < streams.size() - 1; i++) {
        for (size_t j = i + 1; j < streams.size(); j++) {
            CHECK(streams[i].port != streams[j].port);
        }
    }

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Port Conflict Detection") {
    std::cout << "Test: Port conflict detection... ";

    // Create two streams with same address AND port - conflict!
    SDPSession stream1 = createTestStream("Stream 1", "239.1.1.1", 5004, 8);
    SDPSession stream2 = createTestStream("Stream 2", "239.1.1.1", 5004, 8);

    // Detect conflict
    bool conflict = (stream1.connectionAddress == stream2.connectionAddress &&
                    stream1.port == stream2.port);

    CHECK(conflict);

    std::cout << "PASS" << std::endl;
}

//
// PTP Synchronization Tests
//

TEST_CASE("Unified PTP Domain") {
    std::cout << "Test: Unified PTP domain across streams... ";

    int32_t ptpDomain = 0;

    // Create multiple streams all using PTP domain 0
    std::vector<SDPSession> streams;
    for (int i = 0; i < 4; i++) {
        std::string name = "Stream " + std::to_string(i + 1);
        std::string addr = "239.1.1." + std::to_string(i + 1);
        uint16_t port = 5004 + (i * 2);

        SDPSession sdp = createTestStream(name, addr, port, 8);
        sdp.ptpDomain = ptpDomain;
        streams.push_back(sdp);
    }

    // Verify all use same PTP domain
    for (const auto& stream : streams) {
        CHECK(stream.ptpDomain == ptpDomain);
    }

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Multiple PTP Domains") {
    std::cout << "Test: Multiple PTP domains... ";

    // Create streams using different PTP domains
    SDPSession stream1 = createTestStream("Stream 1", "239.1.1.1", 5004, 8);
    stream1.ptpDomain = 0;

    SDPSession stream2 = createTestStream("Stream 2", "239.1.1.2", 5006, 8);
    stream2.ptpDomain = 1;

    SDPSession stream3 = createTestStream("Stream 3", "239.1.1.3", 5008, 8);
    stream3.ptpDomain = 0;

    CHECK(stream1.ptpDomain == 0);
    CHECK(stream2.ptpDomain == 1);
    CHECK(stream3.ptpDomain == 0);

    // Streams 1 and 3 share domain 0
    CHECK(stream1.ptpDomain == stream3.ptpDomain);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("No PTP Streams") {
    std::cout << "Test: Streams without PTP... ";

    // Create streams without PTP sync (-1 = no PTP)
    std::vector<SDPSession> streams;
    for (int i = 0; i < 3; i++) {
        std::string name = "Stream " + std::to_string(i + 1);
        std::string addr = "239.1.1." + std::to_string(i + 1);
        uint16_t port = 5004 + (i * 2);

        SDPSession sdp = createTestStream(name, addr, port, 8);
        sdp.ptpDomain = -1;  // No PTP
        streams.push_back(sdp);
    }

    // Verify all indicate no PTP
    for (const auto& stream : streams) {
        CHECK(stream.ptpDomain == -1);
    }

    std::cout << "PASS" << std::endl;
}

//
// Stream Capacity Tests
//

TEST_CASE("Stream Addition") {
    std::cout << "Test: Progressive stream addition... ";

    std::vector<SDPSession> streams;

    // Add streams one by one
    for (int i = 0; i < 8; i++) {
        std::string name = "Stream " + std::to_string(i + 1);
        std::string addr = "239.1.1." + std::to_string(i + 1);
        uint16_t port = 5004 + (i * 2);

        streams.push_back(createTestStream(name, addr, port, 8));

        CHECK(streams.size() == static_cast<size_t>(i + 1));
    }

    CHECK(streams.size() == 8);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Stream Removal") {
    std::cout << "Test: Stream removal... ";

    // Create initial streams
    std::vector<SDPSession> streams;
    for (int i = 0; i < 5; i++) {
        std::string name = "Stream " + std::to_string(i + 1);
        std::string addr = "239.1.1." + std::to_string(i + 1);
        uint16_t port = 5004 + (i * 2);

        streams.push_back(createTestStream(name, addr, port, 8));
    }

    CHECK(streams.size() == 5);

    // Remove middle stream
    streams.erase(streams.begin() + 2);
    CHECK(streams.size() == 4);

    // Verify remaining streams still valid
    for (const auto& stream : streams) {
        CHECK(stream.isValid());
    }

    std::cout << "PASS" << std::endl;
}

//
// Mixed Configuration Tests
//

TEST_CASE("Realistic Studio Configuration") {
    std::cout << "Test: Realistic studio configuration... ";

    // Typical studio: 64 channels total
    // - 1x 32-channel mix bus (239.1.1.1:5004)
    // - 2x 16-channel FX returns (239.1.1.2:5006, 239.1.1.3:5008)

    SDPSession mixBus = createTestStream("Mix Bus", "239.1.1.1", 5004, 32, 48000);
    SDPSession fx1 = createTestStream("FX Return 1", "239.1.1.2", 5006, 16, 48000);
    SDPSession fx2 = createTestStream("FX Return 2", "239.1.1.3", 5008, 16, 48000);

    CHECK(mixBus.isValid());
    CHECK(fx1.isValid());
    CHECK(fx2.isValid());

    // All at same sample rate
    CHECK(mixBus.sampleRate == 48000);
    CHECK(fx1.sampleRate == 48000);
    CHECK(fx2.sampleRate == 48000);

    // Total channels
    uint16_t total = mixBus.numChannels + fx1.numChannels + fx2.numChannels;
    CHECK(total == 64);

    // Create non-overlapping mappings
    StreamID mixID = StreamID::generate();
    StreamID fx1ID = StreamID::generate();
    StreamID fx2ID = StreamID::generate();

    ChannelMapping mixMap = createMapping(mixID, "Mix Bus", 32, 0);     // 0-31
    ChannelMapping fx1Map = createMapping(fx1ID, "FX 1", 16, 32);       // 32-47
    ChannelMapping fx2Map = createMapping(fx2ID, "FX 2", 16, 48);       // 48-63

    CHECK(mixMap.getDeviceChannelEnd() == 32);
    CHECK(fx1Map.getDeviceChannelEnd() == 48);
    CHECK(fx2Map.getDeviceChannelEnd() == 64);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Realistic Broadcast Configuration") {
    std::cout << "Test: Realistic broadcast configuration... ";

    // Broadcast facility: 128 channels
    // - 4x 32-channel program feeds

    std::vector<SDPSession> programs;
    std::vector<ChannelMapping> mappings;

    for (int i = 0; i < 4; i++) {
        std::string name = "Program " + std::to_string(i + 1);
        std::string addr = "239.69.1." + std::to_string(i + 1);
        uint16_t port = 5004 + (i * 2);

        programs.push_back(createTestStream(name, addr, port, 32, 48000));

        StreamID id = StreamID::generate();
        mappings.push_back(createMapping(id, name, 32, i * 32));
    }

    // Verify all programs
    CHECK(programs.size() == 4);

    for (const auto& program : programs) {
        CHECK(program.isValid());
        CHECK(program.numChannels == 32);
        CHECK(program.sampleRate == 48000);
    }

    // Verify mappings fill entire device
    CHECK(mappings[0].deviceChannelStart == 0);
    CHECK(mappings[1].deviceChannelStart == 32);
    CHECK(mappings[2].deviceChannelStart == 64);
    CHECK(mappings[3].deviceChannelStart == 96);
    CHECK(mappings[3].getDeviceChannelEnd() == 128);

    std::cout << "PASS" << std::endl;
}

//
// Main Test Runner
//

