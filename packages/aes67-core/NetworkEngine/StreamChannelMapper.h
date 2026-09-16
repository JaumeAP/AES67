/// @file StreamChannelMapper.h
/// @brief Maps AES67 streams to the 128-channel device with overlap prevention.

#pragma once

#include "../Shared/Types.h"
#include "../Driver/SDPParser.h"
#include <map>
#include <vector>
#include <array>
#include <mutex>
#include <optional>

namespace AES67 {

/// One of a stream's channels landing on one of the device's.
///
/// A stream channel may appear more than once. IS-08 keys its map by the
/// OUTPUT channel, and a source feeding several device channels -- one mono
/// talkback into every monitor, say -- is an ordinary thing for a controller
/// to ask for. A device channel may appear only once: two sources on one
/// output is not a mix, it is a fault.
struct ChannelRoute {
    uint16_t streamChannel{0};
    uint16_t deviceChannel{0};

    bool operator==(const ChannelRoute& other) const {
        return streamChannel == other.streamChannel && deviceChannel == other.deviceChannel;
    }
};

/// Defines how channels from an AES67 stream map to device channels.
/// Supports sequential mapping with offsets or per-channel custom routing.
struct ChannelMapping {
    // Stream identification
    StreamID streamID;
    std::string streamName;

    uint16_t streamChannelCount{0};      ///< Total channels in the stream
    uint16_t streamChannelOffset{0};     ///< First stream channel to use

    uint16_t deviceChannelStart{0};      ///< First device channel (0-127)
    uint16_t deviceChannelCount{0};      ///< Number of channels to map

    /// Where this stream's channels go, when they do not simply go in order.
    /// Empty is the ordinary case and means sequential: streamCh[i] lands on
    /// deviceCh[start+i].
    ///
    /// A list rather than one device channel per stream channel, because the
    /// two are not one to one: a stream channel with no route is not carried,
    /// and one with several is carried to each of them.
    std::vector<ChannelRoute> routes;

    /// The device channels this stream lands on, in order, whether it routes
    /// them itself or takes the block it was given.
    std::vector<int> deviceChannels() const;

    // Validation
    bool isValid() const;
    std::string getValidationError() const;

    // Helper to check if a device channel is used by this mapping
    bool containsDeviceChannel(int deviceCh) const;

    // Get device channel end (exclusive)
    uint16_t getDeviceChannelEnd() const {
        return deviceChannelStart + deviceChannelCount;
    }
};

/// Whether every channel this mapping touches exists: each device channel
/// below StreamChannelMapper::kMaxDeviceChannels, and each stream channel
/// below `streamChannelCount`.
///
/// A range check and nothing more. ChannelMapping::getValidationError() is
/// the full answer to whether a mapping can be applied -- it also refuses a
/// null stream id, a zero count and two routes landing on one device channel
/// -- and StreamChannelMapper::addMapping asks it. This is what the two RTP
/// classes need before swapping a mapping under a running stream, which is
/// that nothing indexes off the end.
///
/// One answer rather than two: RTPReceiver::updateMapping and
/// RTPTransmitter::updateMapping each carried this, and the transmitter's had
/// only the block half of it until somebody noticed -- a routed mapping went
/// through unchecked in the direction that reads device channels.
bool mappingFitsDevice(const ChannelMapping& mapping, uint16_t streamChannelCount);

/// Central coordinator for mapping AES67 streams to the 128-channel device.
///
/// Prevents channel overlaps, auto-assigns channels, validates mappings,
/// and persists mapping state to disk. Thread-safe (internal mutex).
class StreamChannelMapper {
public:
    static constexpr size_t kMaxDeviceChannels = 128;  ///< Device channel limit

    /// The widest flow this driver carries: 64 channels, RAVENNA's and
    /// ST 2110-30 Level C's ceiling. Not what a flow gets by default -- the
    /// AES67 and Dante profiles cap theirs at 8, and Dante Controller splits
    /// anything wider, so interoperating with it depends on doing the same.
    /// What fits on the wire is bytes, not channels: 64 channels of L24 need
    /// a 125 us packet, and at 1 ms only 10 fit in a frame (see
    /// NetworkEngine/RTP/PacketBudget.h). StreamManager::createTxStreamFlows()
    /// takes the smaller of this, the profile's cap and the byte budget.
    static constexpr uint16_t kMaxChannelsPerFlow = 64;

    StreamChannelMapper();
    ~StreamChannelMapper();

    /// Caps how many device channels auto-assignment may hand out. Defaults
    /// to kMaxDeviceChannels; AES67Device narrows it from the user's
    /// persisted channel-count setting. Channels above the cap stay
    /// advertised to Core Audio but are never assigned to a stream.
    void setUsableChannelCount(size_t count);
    size_t getUsableChannelCount() const;

    //
    // Mapping Management
    //

    // Add a new mapping (validates no overlaps)
    bool addMapping(const ChannelMapping& mapping);

    // Remove a mapping
    bool removeMapping(const StreamID& streamID);

    // Update an existing mapping (validates no overlaps with other streams)
    bool updateMapping(const ChannelMapping& mapping);

    // Get a specific mapping
    std::optional<ChannelMapping> getMapping(const StreamID& streamID) const;

    // Get all mappings
    std::vector<ChannelMapping> getAllMappings() const;

    // Clear all mappings
    void clearAll();

    //
    // Auto-Assignment
    //

    /// Auto-assign channels for a stream described by SDP.
    std::optional<ChannelMapping> createDefaultMapping(const SDPSession& sdp);

    /// Auto-assign channels for a stream by ID, name, and channel count.
    std::optional<ChannelMapping> createDefaultMapping(
        const StreamID& streamID,
        const std::string& streamName,
        uint16_t numChannels
    );

    //
    // Validation
    //

    // Validate a mapping (check ranges, no overlaps)
    bool validateMapping(const ChannelMapping& mapping, std::string* errorOut = nullptr) const;

    // Check if mapping would overlap with existing mappings
    bool hasOverlap(const ChannelMapping& mapping) const;

    // Get all overlapping mappings
    std::vector<StreamID> getOverlappingStreams(const ChannelMapping& mapping) const;

    //
    // Query Functions
    //

    // Get which stream owns a specific device channel
    std::optional<StreamID> getStreamForDeviceChannel(int deviceCh) const;

    // Get all unassigned device channels
    std::vector<int> getUnassignedDeviceChannels() const;

    // Get number of available channels
    size_t getAvailableChannelCount() const;

    // Get number of used channels
    size_t getUsedChannelCount() const;

    // Check if device channel is assigned
    bool isChannelAssigned(int deviceCh) const;

    /// Find first contiguous block of N free channels. Returns start index or nullopt.
    std::optional<int> findContiguousBlock(size_t numChannels) const;

    //
    // Persistence
    //

    // Save mappings to JSON file
    bool save(const std::string& filepath);

    // Load mappings from JSON file
    bool load(const std::string& filepath);

    // Export mappings as JSON string
    std::string toJSON() const;

    // Import mappings from JSON string
    bool fromJSON(const std::string& json);

private:
    // Internal storage
    std::map<StreamID, ChannelMapping> mappings_;

    // Fast lookup: deviceChannel → streamID
    // Uses StreamID::null() for unassigned channels
    std::array<StreamID, kMaxDeviceChannels> deviceChannelOwners_;

    // Ceiling for auto-assignment; see setUsableChannelCount().
    size_t usableChannelCount_{kMaxDeviceChannels};

    // Thread safety for concurrent access
    mutable std::mutex mutex_;

    // Helper functions
    void updateDeviceChannelOwners(const ChannelMapping& mapping);
    void clearDeviceChannelOwners(const StreamID& streamID);
    bool isRangeValid(uint16_t start, uint16_t count) const;
    bool isOverlapWithStream(const ChannelMapping& mapping, const StreamID& excludeStream) const;
};

} // namespace AES67
