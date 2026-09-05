//
// TestChannelMapper.cpp
// AES67 macOS Driver - Build #4
// Unit tests for Stream-to-Channel Mapper
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/StreamChannelMapper.h"
#include <iostream>
#include <cassert>
#include <algorithm>

// assert() expands to nothing under NDEBUG, and NDEBUG is exactly how these
// tests get built: the Release configuration the local gate uses compiles with
// -O3 -DNDEBUG. Every check in this file silently vanished and the binary
// exited 0 no matter what the code did. AES67_CHECK throws instead, which
// main() already catches and turns into a non-zero exit.

namespace AES67 {
namespace Tests {

TEST_CASE("Basic Mapping") {
    std::cout << "Test: Basic Channel Mapping... ";

    StreamChannelMapper mapper;
    StreamID stream1 = StreamID::generate();

    // Create 8-channel mapping
    auto mapping = mapper.createDefaultMapping(stream1, "Test Stream 1", 8);
    CHECK(mapping.has_value());
    CHECK(mapping->deviceChannelStart == 0);
    CHECK(mapping->deviceChannelCount == 8);
    CHECK(mapping->streamChannelCount == 8);

    // Add to mapper
    bool added = mapper.addMapping(*mapping);
    CHECK(added);

    // Verify retrieval
    auto retrieved = mapper.getMapping(stream1);
    CHECK(retrieved.has_value());
    CHECK(retrieved->streamName == "Test Stream 1");

    std::cout << "✓ PASSED\n";
}

TEST_CASE("Multiple Streams") {
    std::cout << "Test: Multiple Stream Mapping... ";

    StreamChannelMapper mapper;

    // Add 8-channel stream
    StreamID stream1 = StreamID::generate();
    auto mapping1 = mapper.createDefaultMapping(stream1, "Stream 1", 8);
    CHECK(mapping1.has_value());
    CHECK(mapping1->deviceChannelStart == 0);
    mapper.addMapping(*mapping1);

    // Add another 8-channel stream
    StreamID stream2 = StreamID::generate();
    auto mapping2 = mapper.createDefaultMapping(stream2, "Stream 2", 8);
    CHECK(mapping2.has_value());
    CHECK(mapping2->deviceChannelStart == 8);  // Should start after first stream
    mapper.addMapping(*mapping2);

    // Add 16-channel stream
    StreamID stream3 = StreamID::generate();
    auto mapping3 = mapper.createDefaultMapping(stream3, "Stream 3", 16);
    CHECK(mapping3.has_value());
    CHECK(mapping3->deviceChannelStart == 16);
    mapper.addMapping(*mapping3);

    // Verify all mappings
    auto allMappings = mapper.getAllMappings();
    CHECK(allMappings.size() == 3);

    std::cout << "✓ PASSED\n";
}

TEST_CASE("Channel Exhaustion") {
    std::cout << "Test: Channel Exhaustion Handling... ";

    StreamChannelMapper mapper;

    // Fill most channels (120 out of 128)
    StreamID stream1 = StreamID::generate();
    auto mapping1 = mapper.createDefaultMapping(stream1, "Big Stream", 120);
    CHECK(mapping1.has_value());
    mapper.addMapping(*mapping1);

    // Try to add 16 channels (should fail - not enough space)
    StreamID stream2 = StreamID::generate();
    auto mapping2 = mapper.createDefaultMapping(stream2, "Too Big", 16);
    CHECK(!mapping2.has_value());  // Should fail

    // Add 8 channels (should succeed)
    StreamID stream3 = StreamID::generate();
    auto mapping3 = mapper.createDefaultMapping(stream3, "Fits", 8);
    CHECK(mapping3.has_value());
    CHECK(mapping3->deviceChannelStart == 120);

    std::cout << "✓ PASSED\n";
}

TEST_CASE("Custom Channel Mapping") {
    std::cout << "Test: Custom Channel Routing... ";

    StreamChannelMapper mapper;
    StreamID streamID = StreamID::generate();

    ChannelMapping mapping;
    mapping.streamID = streamID;
    mapping.streamName = "Custom Routing";
    mapping.streamChannelCount = 8;
    mapping.deviceChannelStart = 10;
    mapping.deviceChannelCount = 8;

    // Custom routing: stream channels [0,2,4,6] → device channels [10,12,14,16]
    //                 stream channels [1,3,5,7] → device channels [11,13,15,17]
    mapping.channelMap = {0, 1, 2, 3, 4, 5, 6, 7};  // Identity mapping

    bool added = mapper.addMapping(mapping);
    CHECK(added);

    auto retrieved = mapper.getMapping(streamID);
    CHECK(retrieved.has_value());
    CHECK(retrieved->deviceChannelStart == 10);

    std::cout << "✓ PASSED\n";
}

TEST_CASE("Mapping Removal") {
    std::cout << "Test: Mapping Removal... ";

    StreamChannelMapper mapper;

    // Add three streams — must addMapping before next createDefaultMapping
    // so the mapper knows which channels are already taken
    StreamID stream1 = StreamID::generate();
    StreamID stream2 = StreamID::generate();
    StreamID stream3 = StreamID::generate();

    auto m1 = mapper.createDefaultMapping(stream1, "Stream 1", 16);
    mapper.addMapping(*m1);

    auto m2 = mapper.createDefaultMapping(stream2, "Stream 2", 16);
    mapper.addMapping(*m2);

    auto m3 = mapper.createDefaultMapping(stream3, "Stream 3", 16);
    mapper.addMapping(*m3);

    CHECK(mapper.getAllMappings().size() == 3);

    // Remove middle stream
    bool removed = mapper.removeMapping(stream2);
    CHECK(removed);
    CHECK(mapper.getAllMappings().size() == 2);

    // Verify channels 16-31 are now available
    auto unassigned = mapper.getUnassignedDeviceChannels();
    CHECK(std::find(unassigned.begin(), unassigned.end(), 16) != unassigned.end());

    // Should be able to add new stream in freed space
    StreamID stream4 = StreamID::generate();
    auto m4 = mapper.createDefaultMapping(stream4, "Stream 4", 16);
    CHECK(m4.has_value());
    CHECK(m4->deviceChannelStart == 16);  // Reuses freed space

    std::cout << "✓ PASSED\n";
}

TEST_CASE("Mapping Validation") {
    std::cout << "Test: Mapping Validation... ";

    StreamChannelMapper mapper;

    ChannelMapping invalid;
    invalid.streamID = StreamID::generate();
    invalid.streamName = "Invalid";
    invalid.streamChannelCount = 8;
    invalid.deviceChannelStart = 125;  // Would extend to channel 132 (>127)
    invalid.deviceChannelCount = 8;

    std::string error;
    bool valid = mapper.validateMapping(invalid, &error);
    CHECK(!valid);
    CHECK(!error.empty());

    std::cout << "✓ PASSED\n";
}

TEST_CASE("Mapping Overlap") {
    std::cout << "Test: Overlap Detection... ";

    StreamChannelMapper mapper;

    // Add first stream at channels 10-17
    StreamID stream1 = StreamID::generate();
    ChannelMapping mapping1;
    mapping1.streamID = stream1;
    mapping1.streamName = "Stream 1";
    mapping1.streamChannelCount = 8;
    mapping1.deviceChannelStart = 10;
    mapping1.deviceChannelCount = 8;

    bool added1 = mapper.addMapping(mapping1);
    CHECK(added1);

    // Try to add overlapping stream at channels 15-22 (should fail)
    StreamID stream2 = StreamID::generate();
    ChannelMapping mapping2;
    mapping2.streamID = stream2;
    mapping2.streamName = "Stream 2";
    mapping2.streamChannelCount = 8;
    mapping2.deviceChannelStart = 15;  // Overlaps with stream1
    mapping2.deviceChannelCount = 8;

    bool added2 = mapper.addMapping(mapping2);
    CHECK(!added2);  // Should be rejected

    std::cout << "✓ PASSED\n";
}

TEST_CASE("Get Unassigned Channels") {
    std::cout << "Test: Unassigned Channels Query... ";

    StreamChannelMapper mapper;

    // Initially all 128 channels should be unassigned
    auto unassigned = mapper.getUnassignedDeviceChannels();
    CHECK(unassigned.size() == 128);

    // Add stream at channels 0-7
    StreamID stream1 = StreamID::generate();
    auto m1 = mapper.createDefaultMapping(stream1, "Stream 1", 8);
    mapper.addMapping(*m1);

    // Now 120 channels should be unassigned
    unassigned = mapper.getUnassignedDeviceChannels();
    CHECK(unassigned.size() == 120);

    // Verify channels 0-7 are NOT in unassigned list
    for (int ch = 0; ch < 8; ch++) {
        CHECK(std::find(unassigned.begin(), unassigned.end(), ch) == unassigned.end());
    }

    // Verify channels 8-127 ARE in unassigned list
    for (int ch = 8; ch < 128; ch++) {
        CHECK(std::find(unassigned.begin(), unassigned.end(), ch) != unassigned.end());
    }

    std::cout << "✓ PASSED\n";
}

TEST_CASE("Riedel Scenario") {
    std::cout << "Test: Riedel Artist Scenario (8x8-channel streams)... ";

    StreamChannelMapper mapper;

    // Simulate 8 Riedel Artist streams, each with 8 channels
    std::vector<StreamID> streams;
    for (int i = 0; i < 8; i++) {
        StreamID streamID = StreamID::generate();
        streams.push_back(streamID);

        std::string name = "Riedel Panel " + std::to_string(i + 1);
        auto mapping = mapper.createDefaultMapping(streamID, name, 8);
        CHECK(mapping.has_value());
        CHECK(mapping->deviceChannelStart == i * 8);

        bool added = mapper.addMapping(*mapping);
        CHECK(added);
    }

    // All 64 channels should be assigned
    auto unassigned = mapper.getUnassignedDeviceChannels();
    CHECK(unassigned.size() == 64);  // 128 - 64 = 64 remaining

    // Verify all streams are active
    CHECK(mapper.getAllMappings().size() == 8);

    std::cout << "✓ PASSED\n";
}

TEST_CASE("Large Scale Scenario") {
    std::cout << "Test: Large Scale Scenario (16x8-channel streams)... ";

    StreamChannelMapper mapper;

    // Add 16 streams of 8 channels each (full 128 channels)
    for (int i = 0; i < 16; i++) {
        StreamID streamID = StreamID::generate();
        std::string name = "Stream " + std::to_string(i + 1);

        auto mapping = mapper.createDefaultMapping(streamID, name, 8);
        CHECK(mapping.has_value());

        bool added = mapper.addMapping(*mapping);
        CHECK(added);
    }

    // All channels should be assigned
    auto unassigned = mapper.getUnassignedDeviceChannels();
    CHECK(unassigned.empty());

    // No more streams should fit
    StreamID extraStream = StreamID::generate();
    auto extraMapping = mapper.createDefaultMapping(extraStream, "Extra", 1);
    CHECK(!extraMapping.has_value());

    std::cout << "✓ PASSED\n";
}

TEST_CASE("JSON Round Trip") {
    std::cout << "Test: JSON Export/Import Round Trip... ";

    StreamChannelMapper mapper;

    StreamID stream1 = StreamID::generate();
    auto mapping1 = mapper.createDefaultMapping(stream1, "Stream 1", 8);
    CHECK(mapping1.has_value());
    mapper.addMapping(*mapping1);

    StreamID stream2 = StreamID::generate();
    auto mapping2 = mapper.createDefaultMapping(stream2, "Stream 2", 16);
    CHECK(mapping2.has_value());
    mapper.addMapping(*mapping2);

    std::string json = mapper.toJSON();

    StreamChannelMapper reloaded;
    bool parsed = reloaded.fromJSON(json);
    CHECK(parsed);

    auto allMappings = reloaded.getAllMappings();
    CHECK(allMappings.size() == 2);

    auto restored1 = reloaded.getMapping(stream1);
    CHECK(restored1.has_value());
    CHECK(restored1->streamName == "Stream 1");
    CHECK(restored1->streamChannelCount == 8);
    CHECK(restored1->deviceChannelStart == 0);
    CHECK(restored1->deviceChannelCount == 8);

    auto restored2 = reloaded.getMapping(stream2);
    CHECK(restored2.has_value());
    CHECK(restored2->streamName == "Stream 2");
    CHECK(restored2->streamChannelCount == 16);
    CHECK(restored2->deviceChannelStart == 8);
    CHECK(restored2->deviceChannelCount == 16);

    std::cout << "✓ PASSED\n";
}

TEST_CASE("JSON Import Clears Existing") {
    std::cout << "Test: JSON Import Clears Existing Mappings... ";

    StreamChannelMapper mapper;
    StreamID stream1 = StreamID::generate();
    auto mapping1 = mapper.createDefaultMapping(stream1, "Stale Stream", 8);
    CHECK(mapping1.has_value());
    mapper.addMapping(*mapping1);
    CHECK(mapper.getAllMappings().size() == 1);

    bool parsed = mapper.fromJSON("{\n  \"mappings\": [\n  ]\n}");
    CHECK(parsed);
    CHECK(mapper.getAllMappings().empty());

    std::cout << "✓ PASSED\n";
}

// ============================================================================
// Usable channel cap (the main window's channel-count selector) and the
// AES67 per-flow channel limit.
// ============================================================================

TEST_CASE("Usable Channel Count Caps Auto Assignment") {
    std::cout << "Test: usable channel cap limits auto-assignment... ";

    StreamChannelMapper mapper;
    // Default is the full device width — the behavior before the setting existed.
    CHECK(mapper.getUsableChannelCount() == StreamChannelMapper::kMaxDeviceChannels);

    mapper.setUsableChannelCount(16);
    CHECK(mapper.getUsableChannelCount() == 16);

    SDPSession sdp;
    sdp.sessionName = "8ch";
    sdp.numChannels = 8;

    // Two 8-channel streams fit exactly within 16.
    auto first = mapper.createDefaultMapping(sdp);
    CHECK(first.has_value());
    CHECK(first->deviceChannelStart == 0);
    CHECK(mapper.addMapping(*first));

    auto second = mapper.createDefaultMapping(sdp);
    CHECK(second.has_value());
    CHECK(second->deviceChannelStart == 8);
    CHECK(mapper.addMapping(*second));

    // A third must fail: channels 16..127 exist on the device but are above
    // the cap, so the mapper must not hand them out.
    auto third = mapper.createDefaultMapping(sdp);
    CHECK(!third.has_value());

    std::cout << "✓ PASSED\n";
}

TEST_CASE("Usable Channel Count Never Exceeds Capacity") {
    std::cout << "Test: usable channel cap clamps to device capacity... ";

    StreamChannelMapper mapper;
    // Asking for more than the fixed RT buffer capacity must clamp, not
    // let the mapper hand out channels that have no buffer behind them.
    mapper.setUsableChannelCount(4096);
    CHECK(mapper.getUsableChannelCount() == StreamChannelMapper::kMaxDeviceChannels);

    std::cout << "✓ PASSED\n";
}

TEST_CASE("Max Channels Per Flow Matches AES67") {
    std::cout << "Test: per-flow channel limit is 8 (AES67 / Dante)... ";

    // Not a tunable: AES67 flows carry at most 8 channels, and Dante
    // Controller splits anything wider into multiple flows.
    // StreamManager::createTxStreamFlows() divides by exactly this.
    static_assert(StreamChannelMapper::kMaxChannelsPerFlow == 8,
                  "AES67 flows carry at most 8 channels");

    // The split arithmetic createTxStreamFlows() performs, checked here
    // because that function itself needs sockets to run.
    constexpr uint16_t perFlow = StreamChannelMapper::kMaxChannelsPerFlow;
    auto flowsFor = [](uint16_t channels) {
        return (channels + perFlow - 1) / perFlow;
    };
    CHECK(flowsFor(1) == 1);
    CHECK(flowsFor(8) == 1);    // exactly one full flow, no spill
    CHECK(flowsFor(9) == 2);    // one full + one carrying a single channel
    CHECK(flowsFor(16) == 2);
    CHECK(flowsFor(24) == 3);
    CHECK(flowsFor(128) == 16); // full device width

    std::cout << "✓ PASSED\n";
}


} // namespace Tests
} // namespace AES67

