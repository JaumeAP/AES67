//
// RTPReceiver.h
// AES67 macOS Driver - Build #1
// RTP packet receiver with L16/L24 decoding and channel mapping
//

#pragma once

#include "../../Shared/Types.h"
#include "../../Shared/RingBuffer.hpp"
#include "../../Driver/SDPParser.h"
#include "../StreamChannelMapper.h"
#include "SimpleRTP.h"
#include "LockFreeCircularJitterBuffer.h"
#include "../../Driver/AudioThreadPriority.h"
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <chrono>

namespace AES67 {

//
// RTP Receiver
//
// Receives RTP audio packets from network and writes decoded audio
// to device channels according to the channel mapping
//
class RTPReceiver {
public:
    using DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, 128>;

    //
    // Constructor
    //
    // sdp: SDP session describing the stream to receive
    // mapping: Channel mapping configuration
    // deviceChannels: Reference to device channel ring buffers
    //
    RTPReceiver(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        DeviceChannelBuffers& deviceChannels
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
    static constexpr size_t kPrefillPacketCount = 6;

    // Adaptive rate matching: lightweight P-controller to compensate for
    // clock drift between network sender and local Core Audio clock.
    // Adjusts consume interval based on ring buffer fill level.
    static constexpr size_t kRateCheckIntervalPackets = 48;   // check every ~48ms
    static constexpr double kMaxRateAdjustment = 0.005;       // +/- 0.5%
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
};

} // namespace AES67
