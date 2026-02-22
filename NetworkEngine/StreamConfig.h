//
// StreamConfig.h
// AES67 macOS Driver - Build #10
// Stream configuration persistence with JSON serialization
//

#pragma once

#include "../Shared/Types.h"
#include "../Driver/SDPParser.h"
#include "StreamChannelMapper.h"
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace AES67 {

//
// Persisted Stream Configuration
//
// Complete stream configuration including SDP session and channel mapping
// Used for saving/loading stream configurations across driver restarts
//
struct PersistedStreamConfig {
    SDPSession sdp;
    ChannelMapping mapping;

    // Metadata
    bool enabled{true};              // Whether stream is active
    std::string description;         // User-provided description
    uint64_t createdTimestamp{0};    // When stream was added
    uint64_t modifiedTimestamp{0};   // Last modification

    // Jitter buffer configuration: 0 = default (256 slots).
    // Values are clamped to [32, 4096] and rounded up to next power of 2.
    size_t jitterBufferDepth{0};

    // Network interface for multicast binding (e.g., "en0" or "192.168.1.10").
    // Empty string = bind to INADDR_ANY (all interfaces).
    std::string networkInterface;

    // Validation
    bool isValid() const;
};

//
// Stream Configuration Manager
//
// Handles saving and loading stream configurations to/from disk
// Configurations are stored in JSON format at:
// /Library/Application Support/AES67Driver/streams.json
//
class StreamConfigManager {
public:
    StreamConfigManager();
    ~StreamConfigManager();

    //
    // Configuration File Management
    //

    // Get the default configuration file path
    std::string getConfigPath() const;

    // Set a custom configuration file path
    void setConfigPath(const std::string& path);

    // Ensure the configuration directory exists
    bool ensureConfigDirectoryExists();

    //
    // Save/Load Operations
    //

    // Save all stream configurations to file
    bool saveConfig(const std::vector<PersistedStreamConfig>& configs);

    // Load all stream configurations from file
    std::optional<std::vector<PersistedStreamConfig>> loadConfig();

    // Save a single stream configuration (append or update)
    bool saveStream(const PersistedStreamConfig& config);

    // Remove a stream from saved configuration
    bool removeStream(const StreamID& id);

    //
    // JSON Serialization
    //

    // Convert stream configs to JSON string
    static std::string toJSON(const std::vector<PersistedStreamConfig>& configs);

    // Parse stream configs from JSON string
    static std::optional<std::vector<PersistedStreamConfig>> fromJSON(const std::string& json);

    // Convert single config to JSON object string
    static std::string configToJSON(const PersistedStreamConfig& config);

    // Parse single config from JSON object string
    static std::optional<PersistedStreamConfig> configFromJSON(const std::string& json);

    //
    // Helper Functions
    //

    // Create a persisted config from SDP and mapping
    static PersistedStreamConfig createConfig(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        const std::string& description = ""
    );

    // Get current Unix timestamp (seconds since epoch)
    static uint64_t getCurrentTimestamp();

private:
    std::string configPath_;
    std::string defaultConfigFile_{"streams.json"};

    // Helper methods for JSON serialization
    static std::string escapeJSON(const std::string& str);
    static std::string sdpToJSON(const SDPSession& sdp);
    static std::string mappingToJSON(const ChannelMapping& mapping);

    // Helper methods for JSON deserialization
    static std::optional<SDPSession> sdpFromJSON(const std::string& json);
    static std::optional<ChannelMapping> mappingFromJSON(const std::string& json);

    // Simple JSON parsing helpers
    static std::optional<std::string> extractStringField(const std::string& json, const std::string& field);
    static std::optional<uint64_t> extractUInt64Field(const std::string& json, const std::string& field);
    static std::optional<uint32_t> extractUInt32Field(const std::string& json, const std::string& field);
    static std::optional<uint16_t> extractUInt16Field(const std::string& json, const std::string& field);
    static std::optional<uint8_t> extractUInt8Field(const std::string& json, const std::string& field);
    static std::optional<double> extractDoubleField(const std::string& json, const std::string& field);
    static std::optional<bool> extractBoolField(const std::string& json, const std::string& field);
    static std::optional<int> extractIntField(const std::string& json, const std::string& field);
};

} // namespace AES67
