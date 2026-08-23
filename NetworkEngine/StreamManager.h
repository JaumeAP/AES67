/// @file StreamManager.h
/// @brief Central coordinator for all AES67 RX and TX streams.

#pragma once

#include "../Shared/Types.h"
#include "../Shared/RingBuffer.hpp"
#include "../Driver/SDPParser.h"
#include "CompatibilityProfile.h"
#include "StreamChannelMapper.h"
#include "StreamConfig.h"
#include "RTP/RTPReceiver.h"
#include "RTP/RTPTransmitter.h"
#include "PTP/PTPClock.h"
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <functional>

namespace AES67 {

/// Central coordinator for all AES67 streams (receivers, transmitters, channel mapping).
///
/// @warning NOT REAL-TIME SAFE. All public methods acquire a mutex. Never call from
/// the Core Audio IO thread or any RT-constrained thread. Safe to call from
/// initialization, UI/manager app, or control threads.
class StreamManager {
public:
    using DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, 128>;
    using StreamCallback = std::function<void(const StreamInfo&)>;

    /// @param inputChannels  Ring buffers written by RTP receivers (RX path).
    /// @param outputChannels Ring buffers read by RTP transmitters (TX path).
    StreamManager(DeviceChannelBuffers& inputChannels, DeviceChannelBuffers& outputChannels);
    ~StreamManager();

    // Prevent copy/move
    StreamManager(const StreamManager&) = delete;
    StreamManager& operator=(const StreamManager&) = delete;

    /// Full PTP diagnostic snapshot for a domain (default 0 — every stream
    /// this driver serves defaults to domain 0 unless its SDP says
    /// otherwise). What AES67Device's custom property
    /// (Shared/CustomProperties.h) serves to ManagerApp.
    PTPDiagnostics getPTPDiagnostics(int domain = 0);

    /// Caps how many device channels streams may be assigned — see
    /// StreamChannelMapper::setUsableChannelCount(). AES67Device passes the
    /// user's persisted setting here at startup.
    void setUsableChannelCount(size_t count) { mapper_.setUsableChannelCount(count); }

    /// Which flavour of AoIP gear this driver is being pointed at. Every
    /// stream added from here on is validated against the profile's limits
    /// (canAddStream). Defaults to AES67, which imposes only AES67's own
    /// mandatory configuration. See CompatibilityProfile.h.
    void setCompatibilityProfile(CompatibilityProfileKind kind);
    CompatibilityProfileKind getCompatibilityProfileKind() const;

    //
    // Stream Management - RX
    //

    /// Add an RX stream with automatic channel assignment. Returns the new StreamID.
    StreamID addStream(const SDPSession& sdp);

    /// Add an RX stream with explicit channel mapping.
    StreamID addStream(const SDPSession& sdp, const ChannelMapping& mapping);

    /// Import an RX stream from an SDP file on disk.
    StreamID importSDPFile(const std::string& filepath);

    // Remove stream
    bool removeStream(const StreamID& id);

    // Remove all streams
    void removeAllStreams();

    //
    // Stream Management - TX
    //

    /// Create a TX stream that reads from device output channels and sends RTP.
    StreamID createTxStream(
        const std::string& name,
        const std::string& multicastIP,
        uint16_t port,
        uint16_t numChannels,
        const ChannelMapping& mapping
    );

    /// Creates a TX stream of any width as one or more AES67 flows, each
    /// carrying at most StreamChannelMapper::kMaxChannelsPerFlow (8)
    /// channels — the actual limit in the standard, and what Dante
    /// Controller does when you tick more than 8 channels.
    ///
    /// Flow N gets `baseMulticastIP` with its last octet advanced by N (so
    /// 239.69.1.10 with 24 channels becomes .10, .11, .12), the same port,
    /// and the next 8 device channels starting from
    /// `mapping.deviceChannelStart`. Each flow is a normal stream created
    /// through createTxStream(), so nothing downstream needs to know these
    /// were grouped.
    ///
    /// Returns every created StreamID, or an empty vector on failure —
    /// partial flows are rolled back rather than left half-configured.
    /// A base IP whose last octet would exceed 255 fails outright.
    std::vector<StreamID> createTxStreamFlows(
        const std::string& baseName,
        const std::string& baseMulticastIP,
        uint16_t port,
        uint16_t numChannels,
        const ChannelMapping& mapping
    );

    // Export stream to SDP file
    bool exportSDPFile(const StreamID& id, const std::string& filepath);

    //
    // Channel Mapping
    //

    // Update channel mapping for a stream
    bool updateMapping(const StreamID& id, const ChannelMapping& newMapping);

    // Get mapping for a stream
    std::optional<ChannelMapping> getMapping(const StreamID& id) const;

    // Get all mappings
    std::vector<ChannelMapping> getAllMappings() const;

    //
    // Query
    //

    // Get all active streams
    std::vector<StreamInfo> getActiveStreams() const;

    // Get stream info
    std::optional<StreamInfo> getStreamInfo(const StreamID& id) const;

    // Check if stream exists
    bool hasStream(const StreamID& id) const;

    // Get stream count
    size_t getStreamCount() const;

    //
    // Validation
    //

    // Check if stream can be added. isTransmit distinguishes an RX stream
    // (addStream) from a TX one (createTxStream) — the active profile's
    // direction/maxTotalChannels are checked per-direction. Defaults to
    // false (RX) for callers that predate this distinction.
    bool canAddStream(const SDPSession& sdp, bool isTransmit = false, std::string* errorOut = nullptr) const;

    // Get detailed error message for why stream can't be added
    std::string getAddStreamError(const SDPSession& sdp, bool isTransmit = false) const;

    //
    // Device State
    //

    /// Notify that Core Audio IO has started or stopped.
    /// When active, starts all dormant receivers/transmitters; when inactive, stops them.
    void setIOActive(bool active);

    // Set current device sample rate (validates against streams)
    bool setDeviceSampleRate(double sampleRate);

    // Get current device sample rate
    double getDeviceSampleRate() const { return currentDeviceSampleRate_; }

    // Get available channel count
    size_t getAvailableChannelCount() const;

    //
    // Configuration Persistence
    //

    /// Load saved stream configurations from /tmp/AES67Driver/streams.json.
    bool loadSavedStreams();

    /// Persist all current streams to disk.
    bool saveAllStreams();

    // Enable/disable auto-save (automatically save after add/remove/update)
    void setAutoSave(bool enabled) { autoSaveEnabled_ = enabled; }

    // Get auto-save state
    bool isAutoSaveEnabled() const { return autoSaveEnabled_; }

    //
    // Callbacks
    //

    // Register callback for stream added
    void setStreamAddedCallback(StreamCallback callback) {
        streamAddedCallback_ = callback;
    }

    // Register callback for stream removed
    void setStreamRemovedCallback(StreamCallback callback) {
        streamRemovedCallback_ = callback;
    }

    // Register callback for stream status changed
    void setStreamStatusCallback(StreamCallback callback) {
        streamStatusCallback_ = callback;
    }

private:
    // Internal stream state
    struct ManagedStream {
        SDPSession sdp;
        ChannelMapping mapping;
        std::unique_ptr<RTPReceiver> receiver;
        std::unique_ptr<RTPTransmitter> transmitter;
        StreamInfo info;
        bool isTransmit{false};
    };

    // Validation helpers
    bool validateSampleRate(const SDPSession& sdp, std::string* errorOut) const;
    bool validateChannelAvailability(uint16_t numChannels, std::string* errorOut) const;
    bool validateNetworkConfig(const SDPSession& sdp, std::string* errorOut) const;

    // Stream creation helpers
    std::unique_ptr<RTPReceiver> createReceiver(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        size_t jitterBufferDepth = 0,
        const std::string& networkInterface = ""
    );

    std::unique_ptr<RTPTransmitter> createTransmitter(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        const std::string& networkInterface = ""
    );

    // Callback invocation
    void notifyStreamAdded(const StreamInfo& info);
    void notifyStreamRemoved(const StreamInfo& info);
    void notifyStreamStatusChanged(const StreamInfo& info);

    // Configuration helpers
    void autoSaveIfEnabled();
    bool saveAllStreamsInternal();  // Internal version without locking

    // Data members
    DeviceChannelBuffers& inputChannels_;   // RTP receivers write here (Network → Core Audio)
    DeviceChannelBuffers& outputChannels_;  // RTP transmitters read here (Core Audio → Network)
    StreamChannelMapper mapper_;

    // Active compatibility profile. Atomic, not mutex-guarded: canAddStream()
    // reads it both with streamsMutex_ held (via addStream) and without it
    // (via getAddStreamError), so it can't take the lock itself.
    std::atomic<CompatibilityProfileKind> profileKind_{CompatibilityProfileKind::AES67};

    // Running totals for CompatibilityProfile::maxTotalChannels — atomic
    // for the same reason profileKind_ is: canAddStream() must be callable
    // both with streamsMutex_ held (addStream/createTxStream) and without
    // it (getAddStreamError), so it can't take the lock to sum streams_
    // itself. Kept in step by addStream/createTxStream on success and by
    // removeStream/removeAllStreams on removal — see those for the actual
    // increment/decrement.
    std::atomic<uint32_t> rxChannelsInUse_{0};
    std::atomic<uint32_t> txChannelsInUse_{0};
    std::map<StreamID, ManagedStream> streams_;
    mutable std::mutex streamsMutex_;

    // Configuration management
    std::unique_ptr<StreamConfigManager> configManager_;
    bool autoSaveEnabled_{true};

    // IO lifecycle state — true when Core Audio IO is active (StartIO/StopIO)
    std::atomic<bool> ioActive_{false};

    // Device state
    std::atomic<double> currentDeviceSampleRate_{48000.0};

    // PTP clock manager reference
    std::shared_ptr<PTPClockManager> ptpManager_;

    // Callbacks
    StreamCallback streamAddedCallback_;
    StreamCallback streamRemovedCallback_;
    StreamCallback streamStatusCallback_;
};

} // namespace AES67
