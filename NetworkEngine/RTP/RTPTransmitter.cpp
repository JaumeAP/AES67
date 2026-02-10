//
// RTPTransmitter.cpp
// AES67 macOS Driver - Build #9
// RTP packet transmitter with L16/L24 encoding and channel mapping
//

#include "RTPTransmitter.h"
#include "SimpleRTP.h"
#include <cstring>
#include <random>
#include <chrono>

namespace AES67 {

RTPTransmitter::RTPTransmitter(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    DeviceChannelBuffers& deviceChannels
)
    : sdp_(sdp)
    , mapping_(mapping)
    , deviceChannels_(deviceChannels)
{
    std::memset(&stats_, 0, sizeof(stats_));

    // Generate random SSRC
    std::random_device rd;
    ssrc_ = rd();

    // Pre-allocate buffers to avoid allocations in transmitLoop()
    // Audio buffer: max 512 frames × stream channels
    const size_t maxFrames = 512;
    const size_t maxAudioSamples = maxFrames * sdp_.numChannels;
    audioBuffer_.resize(maxAudioSamples);

    // Payload buffer: RTP header (12 bytes) + max audio payload
    // L24 is largest: 3 bytes/sample × channels × frames
    const size_t maxPayloadSize = 3 * sdp_.numChannels * maxFrames;
    payloadBuffer_.resize(12 + maxPayloadSize);

    // Calculate packet interval based on sample rate
    // AES67 standard: 1ms packets = sample_rate / 1000 samples per packet
    // Example: 48000 Hz → 48 samples/packet → 1ms interval
    const uint32_t samplesPerPacket = sdp_.sampleRate / 1000;
    const uint64_t intervalUs = 1000; // 1ms in microseconds
    packetInterval_ = std::chrono::microseconds(intervalUs);
}

RTPTransmitter::~RTPTransmitter() {
    stop();
}

bool RTPTransmitter::start() {
    if (running_ || rtpSocket_.isOpen()) {
        return false; // Already running
    }

    // Validate SDP configuration
    if (sdp_.connectionAddress.empty() || sdp_.port == 0) {
        return false;
    }

    if (sdp_.numChannels == 0 || sdp_.numChannels > 128) {
        return false;
    }

    // Open RTP transmitter socket
    if (!rtpSocket_.openTransmitter(sdp_.connectionAddress.c_str(), sdp_.port)) {
        return false;
    }

    // Initialize timestamp and sequence number
    timestamp_ = 0;
    sequenceNumber_ = 0;

    // Record start time for precise packet timing
    startTime_ = std::chrono::steady_clock::now();

    // Start transmit thread (elevated priority to prevent audio dropouts)
    running_ = true;
    transmitThread_ = std::thread([this]() {
        AudioThreadPriority::configureForRealTime();
        transmitLoop();
    });

    return true;
}

void RTPTransmitter::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (transmitThread_.joinable()) {
        transmitThread_.join();
    }

    rtpSocket_.close();
}

StatisticsSnapshot RTPTransmitter::getStatistics() const {
    // Return a consistent snapshot of atomic statistics
    return stats_.snapshot();
}

void RTPTransmitter::resetStatistics() {
    // Reset all atomic counters
    stats_.packetsReceived.store(0, std::memory_order_relaxed);
    stats_.packetsLost.store(0, std::memory_order_relaxed);
    stats_.malformedPackets.store(0, std::memory_order_relaxed);
    stats_.outOfOrderPackets.store(0, std::memory_order_relaxed);
    stats_.underruns.store(0, std::memory_order_relaxed);
    stats_.overruns.store(0, std::memory_order_relaxed);
    stats_.jitterNs.store(0, std::memory_order_relaxed);
    stats_.latencyNs.store(0, std::memory_order_relaxed);
    stats_.bytesReceived.store(0, std::memory_order_relaxed);
    stats_.bytesSent.store(0, std::memory_order_relaxed);
}

bool RTPTransmitter::updateMapping(const ChannelMapping& newMapping) {
    // Validate mapping
    if (newMapping.deviceChannelStart + sdp_.numChannels > 128) {
        return false;
    }

    // Stop, update, restart
    const bool wasRunning = running_;
    if (wasRunning) {
        stop();
    }

    mapping_ = newMapping;

    if (wasRunning) {
        return start();
    }

    return true;
}

void RTPTransmitter::transmitLoop() {
    // Calculate samples per packet (typically 48 for 48kHz @ 1ms)
    const size_t samplesPerPacket = sdp_.sampleRate / 1000;

    auto nextTransmitTime = startTime_;

    while (running_) {
        // Wait until next transmit time (precise 1ms intervals)
        std::this_thread::sleep_until(nextTransmitTime);
        nextTransmitTime += packetInterval_;

        // Read audio from device channels
        if (!readDeviceChannels(audioBuffer_.data(), samplesPerPacket)) {
            // No audio available or error
            stats_.overruns.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // Encode payload based on encoding type
        uint8_t* payload = payloadBuffer_.data();
        size_t payloadSize = 0;

        if (sdp_.encoding == "L16") {
            encodeL16(audioBuffer_.data(), samplesPerPacket, payload);
            payloadSize = samplesPerPacket * sdp_.numChannels * 2; // 2 bytes/sample
        } else if (sdp_.encoding == "L24") {
            encodeL24(audioBuffer_.data(), samplesPerPacket, payload);
            payloadSize = samplesPerPacket * sdp_.numChannels * 3; // 3 bytes/sample
        } else {
            continue; // Unsupported encoding
        }

        // Send RTP packet
        sendPacket(payload, payloadSize, timestamp_);

        // Update timestamp (increment by samples per packet)
        timestamp_ += samplesPerPacket;

        // Update statistics
        stats_.bytesSent.fetch_add(payloadSize, std::memory_order_relaxed);
    }
}

bool RTPTransmitter::readDeviceChannels(float* interleavedAudio, size_t frameCount) {
    // Validate mapping
    const size_t deviceChannelEnd = mapping_.deviceChannelStart + sdp_.numChannels;
    if (deviceChannelEnd > 128) {
        return false;
    }

    // Stack-allocated temporary buffer for reading each channel
    constexpr size_t kMaxFrames = 512;
    if (frameCount > kMaxFrames) {
        return false;
    }

    float channelBuffer[kMaxFrames];
    bool hadUnderrun = false;

    // Read each device channel and interleave into output
    // Result: [ch0_f0, ch1_f0, ch0_f1, ch1_f1, ...]
    for (size_t streamChannel = 0; streamChannel < sdp_.numChannels; ++streamChannel) {
        const size_t deviceChannel = mapping_.deviceChannelStart + streamChannel;

        // Batch read from ring buffer
        const size_t samplesRead = deviceChannels_[deviceChannel].read(channelBuffer, frameCount);

        if (samplesRead < frameCount) {
            // Ring buffer underrun - fill remainder with silence
            std::memset(&channelBuffer[samplesRead], 0,
                       (frameCount - samplesRead) * sizeof(float));
            hadUnderrun = true;
        }

        // Interleave this channel into output
        for (size_t frame = 0; frame < frameCount; ++frame) {
            interleavedAudio[frame * sdp_.numChannels + streamChannel] = channelBuffer[frame];
        }
    }

    // Return false if we had underrun (indicates audio not ready)
    return !hadUnderrun;
}

void RTPTransmitter::encodeL16(const float* audio, size_t frameCount, uint8_t* payload) {
    // L16: 16-bit big-endian signed PCM
    const size_t totalSamples = frameCount * sdp_.numChannels;

    for (size_t i = 0; i < totalSamples; ++i) {
        // Clamp to [-1.0, 1.0] and convert to int16
        float value = std::max(-1.0f, std::min(1.0f, audio[i]));
        int16_t pcmSample = static_cast<int16_t>(value * 32767.0f);

        // Big-endian encoding
        payload[i * 2 + 0] = (pcmSample >> 8) & 0xFF;
        payload[i * 2 + 1] = pcmSample & 0xFF;
    }
}

void RTPTransmitter::encodeL24(const float* audio, size_t frameCount, uint8_t* payload) {
    // L24: 24-bit big-endian signed PCM
    const size_t totalSamples = frameCount * sdp_.numChannels;

    for (size_t i = 0; i < totalSamples; ++i) {
        // Clamp to [-1.0, 1.0] and convert to int32 (24-bit range)
        float value = std::max(-1.0f, std::min(1.0f, audio[i]));
        int32_t pcmSample = static_cast<int32_t>(value * 8388607.0f); // 2^23 - 1

        // Big-endian 24-bit encoding (only write 3 bytes)
        payload[i * 3 + 0] = (pcmSample >> 16) & 0xFF;
        payload[i * 3 + 1] = (pcmSample >> 8) & 0xFF;
        payload[i * 3 + 2] = pcmSample & 0xFF;
    }
}

void RTPTransmitter::sendPacket(const uint8_t* payload, size_t payloadSize, uint32_t timestamp) {
    if (!rtpSocket_.isOpen() || !payload || payloadSize == 0) {
        return;
    }

    // Build RTP packet
    RTP::RTPPacket packet;
    packet.header.version = 2;
    packet.header.padding = 0;
    packet.header.extension = 0;
    packet.header.cc = 0;
    packet.header.marker = 0;
    packet.header.payloadType = sdp_.payloadType;
    packet.header.sequenceNumber = sequenceNumber_++;
    packet.header.timestamp = timestamp;
    packet.header.ssrc = ssrc_;
    packet.payload = const_cast<uint8_t*>(payload);
    packet.payloadSize = payloadSize;

    // Send packet
    ssize_t bytesSent = rtpSocket_.send(packet);

    if (bytesSent < 0) {
        // Send failed
        stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed); // Reuse this field for send errors
    }
}

} // namespace AES67
