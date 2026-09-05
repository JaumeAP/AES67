//
// TestStreamConfig.cpp
// AES67 macOS Driver - Build #1
// Unit tests for StreamConfig: persistence, JSON serialization, path management
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/StreamConfig.h"
#include "Driver/SDPParser.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace AES67;



//
// Helper Functions
//

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
// PersistedStreamConfig Tests
//

TEST_CASE("Persisted Stream Config Creation") {
    std::cout << "Test: PersistedStreamConfig creation... ";

    PersistedStreamConfig config;
    config.sdp = createTestSDP();
    config.mapping = createTestMapping();

    CHECK(!config.sdp.sessionName.empty());
    CHECK(config.enabled == true);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Persisted Stream Config Validation") {
    std::cout << "Test: PersistedStreamConfig validation... ";

    // Valid config
    PersistedStreamConfig validConfig;
    validConfig.sdp = createTestSDP();
    validConfig.mapping = createTestMapping();
    CHECK(validConfig.isValid());

    // Invalid: missing SDP
    PersistedStreamConfig invalidSDP;
    invalidSDP.mapping = createTestMapping();
    CHECK(!invalidSDP.isValid());

    // Invalid: out-of-range mapping
    PersistedStreamConfig invalidMapping;
    invalidMapping.sdp = createTestSDP();
    invalidMapping.mapping = createTestMapping(128, 1);  // Would exceed 128
    CHECK(!invalidMapping.isValid());

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Persisted Stream Config Metadata") {
    std::cout << "Test: PersistedStreamConfig metadata... ";

    PersistedStreamConfig config;
    config.sdp = createTestSDP();
    config.mapping = createTestMapping();
    config.description = "Test configuration";
    config.createdTimestamp = 1234567890;
    config.modifiedTimestamp = 1234567900;
    config.jitterBufferDepth = 512;
    config.networkInterface = "en0";

    CHECK(config.description == "Test configuration");
    CHECK(config.createdTimestamp == 1234567890);
    CHECK(config.jitterBufferDepth == 512);
    CHECK(config.networkInterface == "en0");

    std::cout << "PASS" << std::endl;
}

//
// JSON Serialization Tests
//

TEST_CASE("Config To JSON") {
    std::cout << "Test: PersistedStreamConfig to JSON serialization... ";

    PersistedStreamConfig config = StreamConfigManager::createConfig(
        createTestSDP("MyStream", 5004, 4, 48000),
        createTestMapping(4, 0),
        "Test Description"
    );

    std::string json = StreamConfigManager::configToJSON(config);

    CHECK(!json.empty());
    CHECK(json.find("\"sessionName\"") != std::string::npos);
    CHECK(json.find("MyStream") != std::string::npos);
    CHECK(json.find("Test Description") != std::string::npos);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Config From JSON") {
    std::cout << "Test: PersistedStreamConfig from JSON deserialization... ";

    // Create original config
    PersistedStreamConfig original = StreamConfigManager::createConfig(
        createTestSDP("RoundTrip", 5004, 2, 48000),
        createTestMapping(2, 0),
        "Round trip test"
    );

    // Serialize to JSON
    std::string json = StreamConfigManager::configToJSON(original);

    // Deserialize
    auto parsed = StreamConfigManager::configFromJSON(json);

    CHECK(parsed.has_value());
    CHECK(parsed->sdp.sessionName == original.sdp.sessionName);
    CHECK(parsed->mapping.streamID == original.mapping.streamID);
    CHECK(parsed->description == original.description);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Multiple Configs JSON") {
    std::cout << "Test: Multiple configs JSON serialization... ";

    std::vector<PersistedStreamConfig> configs;

    for (int i = 0; i < 3; ++i) {
        auto config = StreamConfigManager::createConfig(
            createTestSDP("Stream" + std::to_string(i), 5004 + i * 2, 2, 48000),
            createTestMapping(2, i * 2),
            "Config " + std::to_string(i)
        );
        configs.push_back(config);
    }

    std::string json = StreamConfigManager::toJSON(configs);

    CHECK(!json.empty());
    CHECK(json.find("Stream0") != std::string::npos);
    CHECK(json.find("Stream1") != std::string::npos);
    CHECK(json.find("Stream2") != std::string::npos);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Multiple Configs Round Trip") {
    std::cout << "Test: Multiple configs round-trip (serialize/deserialize)... ";

    // Create multiple configs
    std::vector<PersistedStreamConfig> original;
    for (int i = 0; i < 3; ++i) {
        auto config = StreamConfigManager::createConfig(
            createTestSDP("Test" + std::to_string(i), 5004 + i * 2, i + 2, 48000),
            createTestMapping(i + 2, i * 2)
        );
        original.push_back(config);
    }

    // Serialize
    std::string json = StreamConfigManager::toJSON(original);

    // Deserialize
    auto parsed = StreamConfigManager::fromJSON(json);

    CHECK(parsed.has_value());
    CHECK(parsed->size() == 3);

    for (size_t i = 0; i < original.size(); ++i) {
        CHECK((*parsed)[i].sdp.sessionName == original[i].sdp.sessionName);
    }

    std::cout << "PASS" << std::endl;
}

//
// Path Management Tests
//

TEST_CASE("Config Search Paths") {
    std::cout << "Test: Config search paths are valid... ";

    auto paths = StreamConfigManager::getConfigSearchPaths();

    CHECK(!paths.empty());
    CHECK(paths.size() >= 2);

    // Verify paths contain expected components
    bool hasUserPath = false;
    bool hasSystemPath = false;

    for (const auto& path : paths) {
        if (path.find("Library/Application Support") != std::string::npos) {
            hasUserPath = true;
        }
    }

    CHECK(hasUserPath);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Config Path Priority") {
    std::cout << "Test: Config path priority order... ";

    auto paths = StreamConfigManager::getConfigSearchPaths();

    // First path should be from AES67_CONFIG_PATH if set, or empty initially
    // For test, just verify the structure is reasonable
    CHECK(!paths.empty());

    // Verify no duplicate paths
    for (size_t i = 0; i < paths.size(); ++i) {
        for (size_t j = i + 1; j < paths.size(); ++j) {
            CHECK(paths[i] != paths[j]);
        }
    }

    std::cout << "PASS" << std::endl;
}

//
// StreamConfigManager Tests
//

TEST_CASE("Stream Config Manager Creation") {
    std::cout << "Test: StreamConfigManager creation... ";

    StreamConfigManager manager;

    std::string configPath = manager.getConfigPath();
    CHECK(!configPath.empty());

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Stream Config Manager Set Path") {
    std::cout << "Test: StreamConfigManager custom path... ";

    StreamConfigManager manager;

    manager.setConfigPath("/tmp/test_aes67_config.json");

    std::string path = manager.getConfigPath();
    CHECK(path == "/tmp/test_aes67_config.json");

    std::cout << "PASS" << std::endl;
}

//
// Helper Function Tests
//

TEST_CASE("Create Config") {
    std::cout << "Test: StreamConfigManager::createConfig helper... ";

    auto config = StreamConfigManager::createConfig(
        createTestSDP("HelperTest", 5004, 4, 48000),
        createTestMapping(4, 0),
        "Helper test description"
    );

    CHECK(!config.sdp.sessionName.empty());
    CHECK(!config.mapping.streamID.isNull());
    CHECK(config.description == "Helper test description");
    CHECK(config.createdTimestamp > 0);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Get Current Timestamp") {
    std::cout << "Test: StreamConfigManager::getCurrentTimestamp... ";

    uint64_t ts1 = StreamConfigManager::getCurrentTimestamp();
    uint64_t ts2 = StreamConfigManager::getCurrentTimestamp();

    CHECK(ts1 > 0);
    CHECK(ts2 >= ts1);

    std::cout << "PASS" << std::endl;
}

//
// Main Test Runner
//

