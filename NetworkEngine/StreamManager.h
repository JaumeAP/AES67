//
// StreamManager.h
// AES67 macOS Driver - Build #1
// Unified management of all AES67 streams (RX and TX)
//

#pragma once

#include "../Shared/Types.h"
#include "../Shared/RingBuffer.hpp"
#include "../Driver/SDPParser.h"
#include "StreamChannelMapper.h"
#include "StreamConfig.h"
#include "RTP/RTPReceiver.h"
#include "RTP/RTPTransmitter.h"
#include "PTP/PTPClock.h"
#include <map>
#include <memory>
#include <mutex>
#include <functional>

namespace AES67 {

//
// Stream Manager
//
// Central coordinator for all AES67 streams.
// Manages receivers, transmitters, channel mapping, and validation.
//
// WARNING: NOT REAL-TIME SAFE
// All public methods acquire streamsMutex_ and must NEVER be called from
// the Core Audio IO thread (AES67IOHandler callbacks) or any thread with
// real-time constraints. Doing so will cause priority inversion and audio
// dropouts. Only call from: initialization, UI/manager app, or control threads.
//
class StreamManager {
public:
    using DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, 128>;
    using StreamCallback = std::function<void(const StreamInfo&)>;

    StreamManager(DeviceChannelBuffers& inputChannels, DeviceChannelBuffers& outputChannels);
    ~StreamManager();

    // Prevent copy/move
    StreamManager(const StreamManager&) = delete;
    StreamManager& operator=(const StreamManager&) = delete;

    //
    // Stream Management - RX
    //

    // Add stream with automatic channel mapping
    StreamID addStream(const SDPSession& sdp);

    // Add stream with custom mapping
    StreamID addStream(const SDPSession& sdp, const ChannelMapping& mapping);

    // Import from SDP file
    StreamID importSDPFile(const std::string& filepath);

    // Remove stream
    bool removeStream(const StreamID& id);

    // Remove all streams
    void removeAllStreams();

    //
    // Stream Management - TX
    //

    // Create transmit stream
    StreamID createTxStream(
        const std::string& name,
        const std::string& multicastIP,
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

    // Check if stream can be added
    bool canAddStream(const SDPSession& sdp, std::string* errorOut = nullptr) const;

    // Get detailed error message for why stream can't be added
    std::string getAddStreamError(const SDPSession& sdp) const;

    //
    // Device State
    //

    // Set current device sample rate (validates against streams)
    bool setDeviceSampleRate(double sampleRate);

    // Get current device sample rate
    double getDeviceSampleRate() const { return currentDeviceSampleRate_; }

    // Get available channel count
    size_t getAvailableChannelCount() const;

    //
    // Configuration Persistence
    //

    // Load saved stream configurations from disk
    bool loadSavedStreams();

    // Save all current streams to disk
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
        const ChannelMapping& mapping
    );

    std::unique_ptr<RTPTransmitter> createTransmitter(
        const SDPSession& sdp,
        const ChannelMapping& mapping
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
    std::map<StreamID, ManagedStream> streams_;
    mutable std::mutex streamsMutex_;

    // Configuration management
    std::unique_ptr<StreamConfigManager> configManager_;
    bool autoSaveEnabled_{true};

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
