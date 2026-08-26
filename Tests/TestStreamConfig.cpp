//
// TestStreamConfig.cpp
// AES67 macOS Driver - Build #1
// Unit tests for StreamConfig: persistence, JSON serialization, path management
//

#include "NetworkEngine/StreamConfig.h"
#include "Driver/SDPParser.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace AES67;

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

bool testPersistedStreamConfigCreation() {
    std::cout << "Test: PersistedStreamConfig creation... ";

    PersistedStreamConfig config;
    config.sdp = createTestSDP();
    config.mapping = createTestMapping();

    TEST_ASSERT(!config.sdp.sessionName.empty(), "SDP should have name");
    TEST_ASSERT(config.enabled == true, "Should be enabled by default");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPersistedStreamConfigValidation() {
    std::cout << "Test: PersistedStreamConfig validation... ";

    // Valid config
    PersistedStreamConfig validConfig;
    validConfig.sdp = createTestSDP();
    validConfig.mapping = createTestMapping();
    TEST_ASSERT(validConfig.isValid(), "Complete config should be valid");

    // Invalid: missing SDP
    PersistedStreamConfig invalidSDP;
    invalidSDP.mapping = createTestMapping();
    TEST_ASSERT(!invalidSDP.isValid(), "Config without SDP should be invalid");

    // Invalid: out-of-range mapping
    PersistedStreamConfig invalidMapping;
    invalidMapping.sdp = createTestSDP();
    invalidMapping.mapping = createTestMapping(128, 1);  // Would exceed 128
    TEST_ASSERT(!invalidMapping.isValid(), "Config with invalid mapping should be invalid");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPersistedStreamConfigMetadata() {
    std::cout << "Test: PersistedStreamConfig metadata... ";

    PersistedStreamConfig config;
    config.sdp = createTestSDP();
    config.mapping = createTestMapping();
    config.description = "Test configuration";
    config.createdTimestamp = 1234567890;
    config.modifiedTimestamp = 1234567900;
    config.jitterBufferDepth = 512;
    config.networkInterface = "en0";

    TEST_ASSERT(config.description == "Test configuration", "Description should match");
    TEST_ASSERT(config.createdTimestamp == 1234567890, "Created timestamp should match");
    TEST_ASSERT(config.jitterBufferDepth == 512, "Jitter buffer depth should match");
    TEST_ASSERT(config.networkInterface == "en0", "Network interface should match");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// JSON Serialization Tests
//

bool testConfigToJSON() {
    std::cout << "Test: PersistedStreamConfig to JSON serialization... ";

    PersistedStreamConfig config = StreamConfigManager::createConfig(
        createTestSDP("MyStream", 5004, 4, 48000),
        createTestMapping(4, 0),
        "Test Description"
    );

    std::string json = StreamConfigManager::configToJSON(config);

    TEST_ASSERT(!json.empty(), "JSON should not be empty");
    TEST_ASSERT(json.find("\"sessionName\"") != std::string::npos, "JSON should contain sessionName");
    TEST_ASSERT(json.find("MyStream") != std::string::npos, "JSON should contain stream name");
    TEST_ASSERT(json.find("Test Description") != std::string::npos, "JSON should contain description");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testConfigFromJSON() {
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

    TEST_ASSERT(parsed.has_value(), "Should successfully parse JSON");
    TEST_ASSERT(parsed->sdp.sessionName == original.sdp.sessionName, "Session name should match");
    TEST_ASSERT(parsed->mapping.streamID == original.mapping.streamID, "Stream ID should match");
    TEST_ASSERT(parsed->description == original.description, "Description should match");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testMultipleConfigsJSON() {
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

    TEST_ASSERT(!json.empty(), "JSON should not be empty");
    TEST_ASSERT(json.find("Stream0") != std::string::npos, "Should contain Stream0");
    TEST_ASSERT(json.find("Stream1") != std::string::npos, "Should contain Stream1");
    TEST_ASSERT(json.find("Stream2") != std::string::npos, "Should contain Stream2");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testMultipleConfigsRoundTrip() {
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

    TEST_ASSERT(parsed.has_value(), "Should parse multiple configs");
    TEST_ASSERT(parsed->size() == 3, "Should have 3 configs");

    for (size_t i = 0; i < original.size(); ++i) {
        TEST_ASSERT((*parsed)[i].sdp.sessionName == original[i].sdp.sessionName,
                   "Session names should match for config " + std::to_string(i));
    }

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Path Management Tests
//

bool testConfigSearchPaths() {
    std::cout << "Test: Config search paths are valid... ";

    auto paths = StreamConfigManager::getConfigSearchPaths();

    TEST_ASSERT(!paths.empty(), "Should return at least one search path");
    TEST_ASSERT(paths.size() >= 2, "Should have at least 2 default search paths (user + system)");

    // Verify paths contain expected components
    bool hasUserPath = false;
    bool hasSystemPath = false;

    for (const auto& path : paths) {
        if (path.find("Library/Application Support") != std::string::npos) {
            hasUserPath = true;
        }
    }

    TEST_ASSERT(hasUserPath, "Should include Application Support path");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testConfigPathPriority() {
    std::cout << "Test: Config path priority order... ";

    auto paths = StreamConfigManager::getConfigSearchPaths();

    // First path should be from AES67_CONFIG_PATH if set, or empty initially
    // For test, just verify the structure is reasonable
    TEST_ASSERT(!paths.empty(), "Paths should not be empty");

    // Verify no duplicate paths
    for (size_t i = 0; i < paths.size(); ++i) {
        for (size_t j = i + 1; j < paths.size(); ++j) {
            TEST_ASSERT(paths[i] != paths[j], "No duplicate paths allowed");
        }
    }

    std::cout << "PASS" << std::endl;
    return true;
}

//
// StreamConfigManager Tests
//

bool testStreamConfigManagerCreation() {
    std::cout << "Test: StreamConfigManager creation... ";

    StreamConfigManager manager;

    std::string configPath = manager.getConfigPath();
    TEST_ASSERT(!configPath.empty(), "Should have a config path");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testStreamConfigManagerSetPath() {
    std::cout << "Test: StreamConfigManager custom path... ";

    StreamConfigManager manager;

    manager.setConfigPath("/tmp/test_aes67_config.json");

    std::string path = manager.getConfigPath();
    TEST_ASSERT(path == "/tmp/test_aes67_config.json", "Custom path should be set");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Helper Function Tests
//

bool testCreateConfig() {
    std::cout << "Test: StreamConfigManager::createConfig helper... ";

    auto config = StreamConfigManager::createConfig(
        createTestSDP("HelperTest", 5004, 4, 48000),
        createTestMapping(4, 0),
        "Helper test description"
    );

    TEST_ASSERT(!config.sdp.sessionName.empty(), "SDP should be set");
    TEST_ASSERT(!config.mapping.streamID.isNull(), "Mapping should be set");
    TEST_ASSERT(config.description == "Helper test description", "Description should match");
    TEST_ASSERT(config.createdTimestamp > 0, "Timestamp should be set");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testGetCurrentTimestamp() {
    std::cout << "Test: StreamConfigManager::getCurrentTimestamp... ";

    uint64_t ts1 = StreamConfigManager::getCurrentTimestamp();
    uint64_t ts2 = StreamConfigManager::getCurrentTimestamp();

    TEST_ASSERT(ts1 > 0, "Timestamp should be positive");
    TEST_ASSERT(ts2 >= ts1, "Later timestamp should be >= earlier");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Main Test Runner
//

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "AES67 StreamConfig Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    std::cout << "PersistedStreamConfig Tests:" << std::endl;
    std::cout << "---------------------------" << std::endl;
    testPersistedStreamConfigCreation();
    testPersistedStreamConfigValidation();
    testPersistedStreamConfigMetadata();
    std::cout << std::endl;

    std::cout << "JSON Serialization Tests:" << std::endl;
    std::cout << "------------------------" << std::endl;
    testConfigToJSON();
    testConfigFromJSON();
    testMultipleConfigsJSON();
    testMultipleConfigsRoundTrip();
    std::cout << std::endl;

    std::cout << "Path Management Tests:" << std::endl;
    std::cout << "---------------------" << std::endl;
    testConfigSearchPaths();
    testConfigPathPriority();
    std::cout << std::endl;

    std::cout << "StreamConfigManager Tests:" << std::endl;
    std::cout << "--------------------------" << std::endl;
    testStreamConfigManagerCreation();
    testStreamConfigManagerSetPath();
    std::cout << std::endl;

    std::cout << "Helper Function Tests:" << std::endl;
    std::cout << "---------------------" << std::endl;
    testCreateConfig();
    testGetCurrentTimestamp();
    std::cout << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "  Passed: " << testsPassed << std::endl;
    std::cout << "  Failed: " << testsFailed << std::endl;
    std::cout << "========================================" << std::endl;

    return testsFailed == 0 ? 0 : 1;
}
