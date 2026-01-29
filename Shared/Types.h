//
// Types.h
// AES67 macOS Driver - Build #1
// Common types and structures used throughout the driver
//

#pragma once

#include <cstdint>
#include <string>
#include <array>
#include <chrono>
#include <atomic>
#include <uuid/uuid.h>

namespace AES67 {

// ============================================================================
// Stream Identification
// ============================================================================

class StreamID {
public:
    StreamID();
    explicit StreamID(const uint8_t uuid[16]);
    explicit StreamID(const std::string& uuidString);

    bool operator==(const StreamID& other) const;
    bool operator!=(const StreamID& other) const;
    bool operator<(const StreamID& other) const;  // For std::map

    std::string toString() const;
    bool isNull() const;

    static StreamID null();
    static StreamID generate();

private:
    std::array<uint8_t, 16> uuid_;
};

// ============================================================================
// Audio Formats
// ============================================================================

enum class AudioEncoding {
    L16,     // 16-bit linear PCM
    L24,     // 24-bit linear PCM
    DoP,     // DSD over PCM
    Unknown
};

enum class SampleRate : uint32_t {
    SR_44100  = 44100,
    SR_48000  = 48000,
    SR_88200  = 88200,
    SR_96000  = 96000,
    SR_176400 = 176400,
    SR_192000 = 192000,
    SR_352800 = 352800,
    SR_384000 = 384000
};

// ============================================================================
// Statistics
// ============================================================================

// Non-atomic snapshot structure for reading statistics
struct StatisticsSnapshot {
    uint64_t packetsReceived{0};
    uint64_t packetsLost{0};
    uint64_t malformedPackets{0};
    uint64_t outOfOrderPackets{0};
    uint64_t underruns{0};
    uint64_t overruns{0};
    std::chrono::steady_clock::time_point lastPacketTime;
    int64_t jitterNs{0};
    int64_t latencyNs{0};
    uint64_t bytesReceived{0};
    uint64_t bytesSent{0};

    double getPacketLossPercent() const {
        if (packetsReceived == 0) return 0.0;
        return (static_cast<double>(packetsLost) / (packetsReceived + packetsLost)) * 100.0;
    }

    int64_t timeSinceLastPacketMs() const {
        if (lastPacketTime.time_since_epoch().count() == 0) return -1;
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPacketTime);
        return duration.count();
    }
};

struct Statistics {
    // Packet statistics (atomic for lock-free updates)
    std::atomic<uint64_t> packetsReceived{0};
    std::atomic<uint64_t> packetsLost{0};
    std::atomic<uint64_t> malformedPackets{0};
    std::atomic<uint64_t> outOfOrderPackets{0};

    // Audio statistics (atomic for lock-free updates)
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> overruns{0};

    // Timing (non-atomic, requires synchronization if updated from multiple threads)
    std::chrono::steady_clock::time_point lastPacketTime;
    std::atomic<int64_t> jitterNs{0};
    std::atomic<int64_t> latencyNs{0};

    // Byte counters (atomic for lock-free updates)
    std::atomic<uint64_t> bytesReceived{0};
    std::atomic<uint64_t> bytesSent{0};

    // Default constructor
    Statistics() = default;

    // Move constructor (atomics need explicit handling)
    Statistics(Statistics&& other) noexcept
        : packetsReceived(other.packetsReceived.load(std::memory_order_relaxed))
        , packetsLost(other.packetsLost.load(std::memory_order_relaxed))
        , malformedPackets(other.malformedPackets.load(std::memory_order_relaxed))
        , outOfOrderPackets(other.outOfOrderPackets.load(std::memory_order_relaxed))
        , underruns(other.underruns.load(std::memory_order_relaxed))
        , overruns(other.overruns.load(std::memory_order_relaxed))
        , lastPacketTime(other.lastPacketTime)
        , jitterNs(other.jitterNs.load(std::memory_order_relaxed))
        , latencyNs(other.latencyNs.load(std::memory_order_relaxed))
        , bytesReceived(other.bytesReceived.load(std::memory_order_relaxed))
        , bytesSent(other.bytesSent.load(std::memory_order_relaxed))
    {}

    // Move assignment
    Statistics& operator=(Statistics&& other) noexcept {
        if (this != &other) {
            packetsReceived.store(other.packetsReceived.load(std::memory_order_relaxed), std::memory_order_relaxed);
            packetsLost.store(other.packetsLost.load(std::memory_order_relaxed), std::memory_order_relaxed);
            malformedPackets.store(other.malformedPackets.load(std::memory_order_relaxed), std::memory_order_relaxed);
            outOfOrderPackets.store(other.outOfOrderPackets.load(std::memory_order_relaxed), std::memory_order_relaxed);
            underruns.store(other.underruns.load(std::memory_order_relaxed), std::memory_order_relaxed);
            overruns.store(other.overruns.load(std::memory_order_relaxed), std::memory_order_relaxed);
            lastPacketTime = other.lastPacketTime;
            jitterNs.store(other.jitterNs.load(std::memory_order_relaxed), std::memory_order_relaxed);
            latencyNs.store(other.latencyNs.load(std::memory_order_relaxed), std::memory_order_relaxed);
            bytesReceived.store(other.bytesReceived.load(std::memory_order_relaxed), std::memory_order_relaxed);
            bytesSent.store(other.bytesSent.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    // Copy constructor (creates a snapshot of current values)
    Statistics(const Statistics& other)
        : packetsReceived(other.packetsReceived.load(std::memory_order_relaxed))
        , packetsLost(other.packetsLost.load(std::memory_order_relaxed))
        , malformedPackets(other.malformedPackets.load(std::memory_order_relaxed))
        , outOfOrderPackets(other.outOfOrderPackets.load(std::memory_order_relaxed))
        , underruns(other.underruns.load(std::memory_order_relaxed))
        , overruns(other.overruns.load(std::memory_order_relaxed))
        , lastPacketTime(other.lastPacketTime)
        , jitterNs(other.jitterNs.load(std::memory_order_relaxed))
        , latencyNs(other.latencyNs.load(std::memory_order_relaxed))
        , bytesReceived(other.bytesReceived.load(std::memory_order_relaxed))
        , bytesSent(other.bytesSent.load(std::memory_order_relaxed))
    {}

    // Copy assignment
    Statistics& operator=(const Statistics& other) {
        if (this != &other) {
            packetsReceived.store(other.packetsReceived.load(std::memory_order_relaxed), std::memory_order_relaxed);
            packetsLost.store(other.packetsLost.load(std::memory_order_relaxed), std::memory_order_relaxed);
            malformedPackets.store(other.malformedPackets.load(std::memory_order_relaxed), std::memory_order_relaxed);
            outOfOrderPackets.store(other.outOfOrderPackets.load(std::memory_order_relaxed), std::memory_order_relaxed);
            underruns.store(other.underruns.load(std::memory_order_relaxed), std::memory_order_relaxed);
            overruns.store(other.overruns.load(std::memory_order_relaxed), std::memory_order_relaxed);
            lastPacketTime = other.lastPacketTime;
            jitterNs.store(other.jitterNs.load(std::memory_order_relaxed), std::memory_order_relaxed);
            latencyNs.store(other.latencyNs.load(std::memory_order_relaxed), std::memory_order_relaxed);
            bytesReceived.store(other.bytesReceived.load(std::memory_order_relaxed), std::memory_order_relaxed);
            bytesSent.store(other.bytesSent.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    // Reset all counters
    void reset();

    // Calculate packet loss percentage
    double getPacketLossPercent() const;

    // Time since last packet (milliseconds)
    int64_t timeSinceLastPacketMs() const;

    // Create a non-atomic snapshot copy (for consistent reads across multiple fields)
    StatisticsSnapshot snapshot() const;
};

// ============================================================================
// Network Types
// ============================================================================

struct NetworkAddress {
    std::string ip;
    uint16_t port;
    uint8_t ttl{32};

    bool isValid() const;
    bool isMulticast() const;
    bool isAES67Multicast() const;  // 239.x.x.x range

    std::string toString() const;
};

struct PTPConfig {
    int domain{0};              // PTP domain number (-1 = no PTP)
    std::string masterMAC;      // Master clock MAC address
    bool enabled{true};

    bool isValid() const;
};

// ============================================================================
// Stream Information
// ============================================================================

struct StreamInfo {
    StreamID id;
    std::string name;
    std::string description;

    // Network
    NetworkAddress source;
    NetworkAddress multicast;

    // Audio format
    AudioEncoding encoding;
    uint32_t sampleRate;
    uint16_t numChannels;
    uint8_t payloadType;

    // Timing
    uint32_t ptime;         // Packet time in milliseconds
    uint32_t framecount;    // Samples per packet

    // PTP
    PTPConfig ptp;

    // Statistics
    Statistics stats;

    // State
    bool isActive{false};
    bool isConnected{false};
    std::chrono::steady_clock::time_point startTime;

    bool isValid() const;
};

// ============================================================================
// Device Configuration
// ============================================================================

struct DeviceConfig {
    static constexpr size_t kMaxChannels = 128;
    static constexpr size_t kMaxStreams = 64;

    // Audio settings
    double sampleRate{48000.0};
    uint32_t bufferSize{64};

    // Network settings
    bool ptpEnabled{true};
    bool sapDiscoveryEnabled{true};

    // Ring buffer settings (deprecated - calculated dynamically based on sample rate)
    // See AES67Device::CalculateRingBufferSize() for actual sizing logic
    size_t ringBufferSize{512};  // Minimum size (power of 2)

    // Device identification
    std::string deviceName{"AES67 Device"};
    std::string manufacturerName{"AES67 Driver"};
    std::string deviceUID{"AES67-Device-001"};

    // Paths
    std::string configPath{"/Library/Application Support/AES67Driver/config.json"};
    std::string mappingsPath{"/Library/Application Support/AES67Driver/mappings.json"};

    bool isValid() const;
};

// ============================================================================
// Error Types
// ============================================================================

enum class ErrorCode {
    Success = 0,

    // Network errors
    NetworkSocketError,
    NetworkBindError,
    NetworkMulticastJoinError,
    NetworkSendError,
    NetworkReceiveError,

    // SDP errors
    SDPParseError,
    SDPInvalidFormat,
    SDPMissingField,
    SDPInvalidValue,

    // Mapping errors
    MappingOverlap,
    MappingOutOfRange,
    MappingNoChannelsAvailable,
    MappingInvalidStream,

    // Stream errors
    StreamNotFound,
    StreamAlreadyExists,
    StreamSampleRateMismatch,
    StreamInvalidConfiguration,

    // PTP errors
    PTPNotAvailable,
    PTPNotLocked,
    PTPDomainInvalid,

    // Audio errors
    AudioDeviceNotFound,
    AudioFormatNotSupported,
    AudioBufferOverrun,
    AudioBufferUnderrun,

    // Generic errors
    InvalidParameter,
    OutOfMemory,
    FileNotFound,
    FileReadError,
    FileWriteError,
    NotImplemented,
    InternalError
};

struct Error {
    ErrorCode code;
    std::string message;
    std::string context;

    Error(ErrorCode c, const std::string& msg = "", const std::string& ctx = "")
        : code(c), message(msg), context(ctx) {}

    bool isSuccess() const { return code == ErrorCode::Success; }
    std::string toString() const;
};

// ============================================================================
// Utility Functions
// ============================================================================

namespace Utils {
    // Convert sample rate enum to Hz
    uint32_t sampleRateToHz(SampleRate sr);
    SampleRate hzToSampleRate(uint32_t hz);

    // Validate IP addresses
    bool isValidIPv4(const std::string& ip);
    bool isMulticastIP(const std::string& ip);
    bool isAES67MulticastIP(const std::string& ip);

    // Time utilities
    uint64_t getNanoseconds();
    uint64_t getMicroseconds();
    uint64_t getMilliseconds();

    // String utilities
    std::string formatBytes(uint64_t bytes);
    std::string formatDuration(std::chrono::milliseconds ms);
}

} // namespace AES67
