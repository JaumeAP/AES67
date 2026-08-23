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

    /// Per-channel custom routing. If empty, sequential: streamCh[i] -> deviceCh[start+i].
    std::vector<int> channelMap;

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

/// Central coordinator for mapping AES67 streams to the 128-channel device.
///
/// Prevents channel overlaps, auto-assigns channels, validates mappings,
/// and persists mapping state to disk. Thread-safe (internal mutex).
class StreamChannelMapper {
public:
    static constexpr size_t kMaxDeviceChannels = 128;  ///< Device channel limit

    /// AES67 flow limit: a single RTP stream carries at most 8 channels.
    /// Not a convention — Dante Controller splits anything wider into
    /// multiple flows, and interoperating with it depends on doing the same.
    static constexpr uint16_t kMaxChannelsPerFlow = 8;

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
