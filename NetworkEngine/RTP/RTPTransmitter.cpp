//
// RTPTransmitter.cpp
// AES67 macOS Driver - Build #9
// RTP packet transmitter with L16/L24 encoding and channel mapping
//

#include "RTPTransmitter.h"
#include "SimpleRTP.h"
#include "PCMCodec.h"
#include "Driver/DebugLog.h"
#include <cstring>
#include <random>
#include <chrono>
#include <cerrno>

namespace AES67 {

RTPTransmitter::RTPTransmitter(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    DeviceChannelBuffers& deviceChannels,
    const std::string& networkInterface,
    uint16_t sourcePort,
    int dscp
)
    : sdp_(sdp)
    , mapping_(mapping)
    , deviceChannels_(deviceChannels)
    , networkInterface_(networkInterface)
    , sourcePort_(sourcePort)
    , dscp_(dscp)
{
    // See RTPReceiver: Statistics holds atomics, so memset over it is undefined
    // behaviour. reset() stores zero into each counter.
    stats_.reset();

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
    // size_t throughout: 3 * channels * frames is a buffer size, and an int
    // product overflows long before size_t would.
    const size_t maxPayloadSize =
        size_t{3} * static_cast<size_t>(sdp_.numChannels) * static_cast<size_t>(maxFrames);
    payloadBuffer_.resize(12 + maxPayloadSize);

    // Packet interval and payload size must agree, or the stream drifts:
    // send N samples every T, and the receiver's clock recovery expects
    // N/sampleRate == T. This used to hardcode T = 1 ms while taking N
    // from sdp_.framecount, so any stream whose framecount didn't happen
    // to be sampleRate/1000 — a 96 kHz stream with the default 48, say —
    // transmitted at the wrong rate.
    //
    // framecount is authoritative when present (it's an exact integer
    // sample count); ptime derives it otherwise. Either way both numbers
    // come from the same place.
    samplesPerPacket_ = sdp_.framecount > 0
        ? sdp_.framecount
        : static_cast<uint32_t>((static_cast<uint64_t>(sdp_.sampleRate) * sdp_.ptimeUs) / 1000000ULL);
    if (samplesPerPacket_ == 0) samplesPerPacket_ = 1; // never a zero-length packet

    const uint64_t intervalUs =
        (static_cast<uint64_t>(samplesPerPacket_) * 1000000ULL) / std::max<uint32_t>(sdp_.sampleRate, 1);
    packetInterval_ = std::chrono::microseconds(std::max<uint64_t>(intervalUs, 1));
}

RTPTransmitter::~RTPTransmitter() {
    stop();
}

bool RTPTransmitter::start() {
    if (running_ || rtpSocket_.isOpen()) {
        AES67_LOGF("RTPTransmitter::start: already running or socket open (stream=%s)",
                   sdp_.sessionName.c_str());
        return false; // Already running
    }

    // Validate SDP configuration
    if (sdp_.connectionAddress.empty() || sdp_.port == 0) {
        AES67_LOGF("RTPTransmitter::start: invalid SDP - address='%s' port=%u (stream=%s)",
                   sdp_.connectionAddress.c_str(), sdp_.port, sdp_.sessionName.c_str());
        return false;
    }

    if (sdp_.numChannels == 0 || sdp_.numChannels > 128) {
        AES67_LOGF("RTPTransmitter::start: invalid channel count %u (stream=%s)",
                   sdp_.numChannels, sdp_.sessionName.c_str());
        return false;
    }

    // Open RTP transmitter socket
    const char* ifaceIP = networkInterface_.empty() ? nullptr : networkInterface_.c_str();
    if (!rtpSocket_.openTransmitter(sdp_.connectionAddress.c_str(), sdp_.port, ifaceIP, sourcePort_, dscp_)) {
        AES67_LOGF("RTPTransmitter::start: socket open failed for %s:%u iface=%s (stream=%s)",
                   sdp_.connectionAddress.c_str(), sdp_.port,
                   networkInterface_.empty() ? "ANY" : networkInterface_.c_str(),
                   sdp_.sessionName.c_str());
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
        // Real cycle length is the stream's own packet time, not a guess.
        if (!AudioThreadPriority::configureForRealTime(sdp_.ptimeUs / 1000.0)) {
            AES67_LOGF("RTPTransmitter: failed to set RT priority on transmit thread (stream=%s)",
                       sdp_.sessionName.c_str());
        }
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
    // Derived in the constructor, consistent with packetInterval_.
    const size_t samplesPerPacket = samplesPerPacket_;

    auto nextTransmitTime = startTime_;
    bool unsupportedEncodingLogged = false;

    while (running_) {
        // Wait until next transmit time (precise 1ms intervals)
        std::this_thread::sleep_until(nextTransmitTime);
        nextTransmitTime += packetInterval_;

        // Read audio from device channels (silence-fills on underrun)
        // Always send packets even with empty ring buffers — AES67 requires
        // continuous packet flow for receiver clock recovery
        readDeviceChannels(audioBuffer_.data(), samplesPerPacket);

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
            if (!unsupportedEncodingLogged) {
                AES67_LOGF("RTPTransmitter::transmitLoop: unsupported encoding '%s' - no packets will be sent (stream=%s)",
                           sdp_.encoding.c_str(), sdp_.sessionName.c_str());
                unsupportedEncodingLogged = true;
            }
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
        interleaveChannel(channelBuffer, interleavedAudio, frameCount,
                          sdp_.numChannels, streamChannel);
    }

    // Return false if we had underrun (indicates audio not ready)
    return !hadUnderrun;
}

void RTPTransmitter::encodeL16(const float* audio, size_t frameCount, uint8_t* payload) {
    // vDSP-accelerated on Apple, scalar fallback elsewhere — identical
    // bytes either way, pinned by TestPCMCodec. See PCMCodec.h.
    encodeL16BE(audio, frameCount * sdp_.numChannels, payload);
}

void RTPTransmitter::encodeL24(const float* audio, size_t frameCount, uint8_t* payload) {
    encodeL24BE(audio, frameCount * sdp_.numChannels, payload);
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
        // Send failed — count via malformedPackets (repurposed as send error counter for TX)
        uint64_t count = stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed) + 1;
        // Log first occurrence and then every 100th to avoid flooding
        if (count == 1 || count % 100 == 0) {
            AES67_LOGF("RTPTransmitter::sendPacket: send failed #%llu (seq=%u, errno=%d: %s) stream=%s",
                       (unsigned long long)count, sequenceNumber_ - 1,
                       errno, strerror(errno), sdp_.sessionName.c_str());
        }
    }
}

} // namespace AES67
