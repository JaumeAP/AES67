/// @file RTPReceiver.h
/// @brief RTP packet receiver with L16/L24 decoding and channel mapping.

#pragma once

#include "Shared/Types.h"
#include "Shared/RingBuffer.hpp"
#include "Driver/SDPParser.h"
#include "NetworkEngine/StreamChannelMapper.h"
#include "NetworkEngine/NetworkInterfaceDetection.h"
#include "SimpleRTP.h"
#include "NetworkEngine/RTP/LockFreeCircularJitterBuffer.h"
#include "Driver/AudioThreadPriority.h"
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <chrono>

namespace AES67 {

/// Receives RTP audio packets from a multicast group and writes decoded
/// audio to device ring buffers via channel mapping.
///
/// Uses two threads: receiveLoop() for network I/O into a jitter buffer,
/// and consumeLoop() for paced readout into per-channel ring buffers.
/// Includes adaptive rate matching (P-controller) to compensate for clock drift.
class RTPReceiver {
public:
    using DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, 128>;

    /// @param sdp SDP session describing the stream to receive.
    /// @param mapping Channel mapping from stream channels to device channels.
    /// @param deviceChannels Reference to device input ring buffers.
    /// @param jitterBufferDepth Jitter buffer slots (0=default 256, clamped [32,4096], rounded to power-of-2).
    /// @param networkInterface Interface name ("en0") or IP to bind multicast. Empty = INADDR_ANY.
    RTPReceiver(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        DeviceChannelBuffers& deviceChannels,
        size_t jitterBufferDepth = 0,
        const std::string& networkInterface = "",
        uint32_t playoutDelaySamples = 0
    );

    ~RTPReceiver();

    // Prevent copy/move
    RTPReceiver(const RTPReceiver&) = delete;
    RTPReceiver& operator=(const RTPReceiver&) = delete;

    //
    // Control
    //

    // Start receiving
    bool start();

    // Stop receiving
    void stop();

    // Check if currently receiving
    bool isRunning() const { return running_.load(); }

    //
    // Status
    //

    // Get statistics (returns a non-atomic snapshot)
    StatisticsSnapshot getStatistics() const;

    // Reset statistics
    void resetStatistics();

    // Get connection status
    bool isConnected() const;

    // Get time since last packet (milliseconds)
    int64_t getTimeSinceLastPacket() const;

    //
    // Configuration
    //

    // Update channel mapping (stops and restarts receiver)
    bool updateMapping(const ChannelMapping& newMapping);

    // Get current SDP session
    const SDPSession& getSDPSession() const { return sdp_; }

    // Get current mapping
    const ChannelMapping& getMapping() const { return mapping_; }

private:
    // Network thread function (producer - adds packets to jitter buffer)
    void receiveLoop();

    // Consumer thread function (reads from jitter buffer and writes to ring buffers)
    void consumeLoop();

    // Packet processing
    void processPacket(const RTP::RTPPacket& packet);
    bool validatePacket(const RTP::RTPPacket& packet);

    // Audio decoding
    void decodeL16(const uint8_t* payload, size_t payloadSize);
    void decodeL24(const uint8_t* payload, size_t payloadSize);

    // Channel mapping: stream audio → device channels
    void mapChannelsToDevice(const float* interleavedAudio, size_t frameCount);

    // Statistics tracking
    void updateStats(uint16_t sequenceNumber, size_t payloadSize);

    // Consumer pacing (mirrors RTPTransmitter::packetInterval_)
    std::chrono::microseconds packetInterval_;

    // Pre-fill gate: consumer waits until jitter buffer has enough packets
    // before starting paced consumption, preventing initial starvation
    std::atomic<bool> prefillComplete_{false};

    /// Default cushion when no playout delay is configured — the value
    /// this receiver used unconditionally before playout delay existed.
    static constexpr size_t kPrefillPacketCount = 6;

    /// How many packets must accumulate before paced consumption starts.
    /// This IS the playout delay: the cushion is the latency. Derived in
    /// the constructor from the configured delay in samples, never below
    /// one packet (a zero-length cushion would starve immediately) and
    /// defaulting to kPrefillPacketCount.
    size_t prefillPacketCount_{kPrefillPacketCount};

    // Adaptive rate matching: lightweight P-controller to compensate for
    // clock drift between network sender and local Core Audio clock.
    // Adjusts consume interval based on ring buffer fill level.
    static constexpr size_t kRateCheckIntervalPackets = 48;   // check every ~48ms
    static constexpr double kMaxRateAdjustment = 0.005;       // +/- 0.5%

    /// Frames one packet may carry, and the one bound every buffer here is
    /// sized from. 512 frames is 10.6 ms at 48 kHz, an order of magnitude
    /// above the 1 ms AES67 asks for, so a packet above it is malformed
    /// rather than merely large. It used to be stated twice — 512 where a
    /// packet was decoded, 4096 where the result was de-interleaved — which
    /// left the de-interleave buffer eight times the size anything could
    /// reach and the two free to drift apart (2026-09-04 audit).
    static constexpr size_t kMaxFramesPerPacket = 512;
    static constexpr double kTargetFillRatio = 0.5;           // 50% ring buffer fill
    static constexpr double kRateAdjustmentGain = 0.0001;     // very gentle P-controller

    // Configuration
    SDPSession sdp_;
    ChannelMapping mapping_;
    DeviceChannelBuffers& deviceChannels_;

    // RTP socket
    RTP::RTPSocket rtpSocket_;

    // Jitter buffer for packet reordering
    LockFreeCircularJitterBuffer jitterBuffer_;

    // Threading
    std::thread receiveThread_;
    std::thread consumeThread_;
    std::atomic<bool> running_{false};

    // Expected sequence number for consumer
    std::atomic<uint32_t> expectedSequenceNumber_{0};

    // Statistics (atomic operations, no mutex needed for individual updates)
    Statistics stats_;
    std::atomic<uint16_t> lastSequenceNumber_{0};
    std::atomic<uint32_t> lastTimestamp_{0};

    // Connection state
    std::atomic<bool> connected_{false};
    std::atomic<int64_t> lastPacketTimeNs_{0};

    // RTP timestamp wraparound tracking
    std::atomic<uint32_t> firstTimestamp_{0};
    std::atomic<bool> firstTimestampSet_{false};

    // Audio buffer (reused to avoid allocations)
    std::vector<float> audioBuffer_;

    // Receive buffer for network packets (max MTU 1500 bytes)
    uint8_t receiveBuffer_[2048];

    // Jitter buffer read buffer
    uint8_t jitterReadBuffer_[1500];

    // Network interface binding
    std::string networkInterface_;   // Interface name or IP from config
    std::string resolvedInterfaceIP_; // Resolved IP address (empty = INADDR_ANY)
};

} // namespace AES67
