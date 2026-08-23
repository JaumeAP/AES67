/// @file RTPTransmitter.h
/// @brief RTP packet transmitter with L16/L24 encoding and channel mapping.

#pragma once

#include "../../Shared/Types.h"
#include "../../Shared/RingBuffer.hpp"
#include "../../Driver/SDPParser.h"
#include "../StreamChannelMapper.h"
#include "SimpleRTP.h"
#include "../../Driver/AudioThreadPriority.h"
#include <thread>
#include <atomic>
#include <memory>

namespace AES67 {

/// Reads audio from device output ring buffers and transmits as RTP multicast packets.
///
/// Single transmit thread using sleep_until pacing for drift-free timing.
/// Sends continuous packets (including silence) for receiver clock recovery.
class RTPTransmitter {
public:
    using DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, 128>;

    /// @param sdp SDP session describing the TX stream configuration.
    /// @param mapping Channel mapping from device channels to stream channels.
    /// @param deviceChannels Reference to device output ring buffers.
    /// @param networkInterface Interface name ("en0") or IP to bind multicast. Empty = default.
    /// @param sourcePort Explicit local UDP port to bind before sending, or
    ///   0 (default) for the kernel-assigned ephemeral one every profile
    ///   but DMA uses. See CompatibilityProfile::
    ///   useFixedMulticastWithPerFlowSourcePort and RTPSocket::openTransmitter.
    /// @param dscp DSCP codepoint to mark outgoing packets with, or -1
    ///   (default) to leave them unmarked. Comes from the active profile's
    ///   CompatibilityProfile::recommendedDscp.
    RTPTransmitter(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        DeviceChannelBuffers& deviceChannels,
        const std::string& networkInterface = "",
        uint16_t sourcePort = 0,
        int dscp = -1
    );

    ~RTPTransmitter();

    // Prevent copy/move
    RTPTransmitter(const RTPTransmitter&) = delete;
    RTPTransmitter& operator=(const RTPTransmitter&) = delete;

    //
    // Control
    //

    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }

    //
    // Status
    //

    StatisticsSnapshot getStatistics() const;
    void resetStatistics();

    //
    // Configuration
    //

    bool updateMapping(const ChannelMapping& newMapping);
    const SDPSession& getSDPSession() const { return sdp_; }
    const ChannelMapping& getMapping() const { return mapping_; }

private:
    // Transmit thread function
    void transmitLoop();

    // Read audio from device channels and interleave
    bool readDeviceChannels(float* interleavedAudio, size_t frameCount);

    // Audio encoding
    void encodeL16(const float* audio, size_t frameCount, uint8_t* payload);
    void encodeL24(const float* audio, size_t frameCount, uint8_t* payload);

    // Send RTP packet
    void sendPacket(const uint8_t* payload, size_t payloadSize, uint32_t timestamp);

    // Configuration
    SDPSession sdp_;
    ChannelMapping mapping_;
    DeviceChannelBuffers& deviceChannels_;
    std::string networkInterface_;
    uint16_t sourcePort_{0};
    int dscp_{-1};

    // RTP socket
    RTP::RTPSocket rtpSocket_;

    // Threading
    std::thread transmitThread_;
    std::atomic<bool> running_{false};

    // Statistics (atomic operations, no mutex needed for individual updates)
    Statistics stats_;

    // RTP state
    uint16_t sequenceNumber_{0};
    uint32_t timestamp_{0};
    uint32_t ssrc_{0};

    // Timing
    std::chrono::steady_clock::time_point startTime_;
    std::chrono::microseconds packetInterval_;
    /// Samples per packet, derived alongside packetInterval_ so the two
    /// always agree — see the constructor.
    uint32_t samplesPerPacket_{48};

    // Audio buffer (reused to avoid allocations)
    std::vector<float> audioBuffer_;
    std::vector<uint8_t> payloadBuffer_;
};

} // namespace AES67
