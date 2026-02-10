//
// RTPReceiver.cpp
// AES67 macOS Driver - Build #9
// RTP packet receiver with L16/L24 decoding and channel mapping integration
//

#include "RTPReceiver.h"
#include "SimpleRTP.h"
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <sys/select.h>

namespace AES67 {

RTPReceiver::RTPReceiver(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    DeviceChannelBuffers& deviceChannels
)
    : sdp_(sdp)
    , mapping_(mapping)
    , deviceChannels_(deviceChannels)
{
    std::memset(&stats_, 0, sizeof(stats_));

    // Pre-allocate audio buffer to avoid allocations in receiveLoop()
    // Max 512 frames × stream channels (e.g., 512 × 8 = 4096 floats)
    const size_t maxFrames = 512;
    const size_t maxSamples = maxFrames * sdp_.numChannels;
    audioBuffer_.resize(maxSamples);
}

RTPReceiver::~RTPReceiver() {
    stop();
}

bool RTPReceiver::start() {
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

    // Open RTP receiver socket
    if (!rtpSocket_.openReceiver(sdp_.connectionAddress.c_str(), sdp_.port)) {
        return false;
    }

    // Reset jitter buffer
    jitterBuffer_.reset();
    expectedSequenceNumber_.store(0, std::memory_order_relaxed);

    // Start receive thread (producer - adds packets to jitter buffer)
    running_ = true;
    receiveThread_ = std::thread([this]() {
        AudioThreadPriority::configureForRealTime();
        receiveLoop();
    });

    // Start consume thread (consumer - reads from jitter buffer and writes to ring buffers)
    consumeThread_ = std::thread([this]() {
        AudioThreadPriority::configureForRealTime();
        consumeLoop();
    });

    return true;
}

void RTPReceiver::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }

    if (consumeThread_.joinable()) {
        consumeThread_.join();
    }

    rtpSocket_.close();
    connected_ = false;
}

StatisticsSnapshot RTPReceiver::getStatistics() const {
    // Return a consistent snapshot of atomic statistics
    return stats_.snapshot();
}

void RTPReceiver::resetStatistics() {
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
    lastSequenceNumber_.store(0, std::memory_order_relaxed);
    lastTimestamp_.store(0, std::memory_order_relaxed);
    expectedSequenceNumber_.store(0, std::memory_order_relaxed);
    firstTimestampSet_.store(false, std::memory_order_relaxed);
    firstTimestamp_.store(0, std::memory_order_relaxed);

    // Reset jitter buffer
    jitterBuffer_.reset();
}

bool RTPReceiver::isConnected() const {
    if (!connected_) {
        return false;
    }

    // Consider disconnected if no packet in last 1 second
    int64_t lastNs = lastPacketTimeNs_.load(std::memory_order_acquire);
    if (lastNs == 0) return false;
    auto now = std::chrono::steady_clock::now();
    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    return (nowNs - lastNs) < 1000000000LL; // 1 second in ns
}

int64_t RTPReceiver::getTimeSinceLastPacket() const {
    if (!connected_) {
        return -1;
    }

    int64_t lastNs = lastPacketTimeNs_.load(std::memory_order_acquire);
    if (lastNs == 0) return -1;
    auto now = std::chrono::steady_clock::now();
    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    return (nowNs - lastNs) / 1000000; // ns to ms
}

bool RTPReceiver::updateMapping(const ChannelMapping& newMapping) {
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

void RTPReceiver::receiveLoop() {
    fd_set readfds;
    struct timeval tv;

    RTP::RTPPacket packet;

    while (running_) {
        // Set up select with 1ms timeout (responsive but not spinning)
        FD_ZERO(&readfds);
        int sockfd = rtpSocket_.getFd();
        if (sockfd < 0) {
            // Socket not valid, wait and retry
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        FD_SET(sockfd, &readfds);

        tv.tv_sec = 0;
        tv.tv_usec = 1000;  // 1ms timeout

        int ret = select(sockfd + 1, &readfds, nullptr, nullptr, &tv);

        if (ret > 0 && FD_ISSET(sockfd, &readfds)) {
            // Data available - receive it
            ssize_t bytesReceived = rtpSocket_.receive(packet, receiveBuffer_, sizeof(receiveBuffer_));
            if (bytesReceived > 0) {
                processPacket(packet);
            }
            // bytesReceived == 0 means no data (EAGAIN) - handled by select timeout
            // bytesReceived < 0 means error - will be caught by getFd() < 0 check or next iteration
        }
        // ret == 0 means timeout - just loop again
        // ret < 0 means select error - loop again and check socket validity
    }
}

void RTPReceiver::consumeLoop() {
    // This thread pulls packets from the jitter buffer in sequence order
    // and writes them to the ring buffers after decoding

    // Calculate packet interval based on sample rate and frame count
    // For AES67, typical packet time is 1ms (48 samples at 48kHz)
    // Use a conservative interval to avoid spinning too much
    const auto packetInterval = std::chrono::microseconds(500); // 500us (2x faster than 1ms packets)

    size_t outputLength = 0;
    uint64_t presentationTime = 0;

    while (running_) {
        // Get the expected next packet from the jitter buffer
        uint32_t expectedSeq = expectedSequenceNumber_.load(std::memory_order_relaxed);

        bool gotPacket = jitterBuffer_.getNextPacket(
            jitterReadBuffer_,
            sizeof(jitterReadBuffer_),
            outputLength,
            presentationTime,
            expectedSeq
        );

        if (gotPacket) {
            // Successfully got the next packet in sequence
            // Increment expected sequence number (handle 16-bit wraparound)
            expectedSequenceNumber_.store((expectedSeq + 1) & 0xFFFF, std::memory_order_relaxed);

            // Decode based on encoding type
            if (sdp_.encoding == "L16") {
                decodeL16(jitterReadBuffer_, outputLength);
            } else if (sdp_.encoding == "L24") {
                decodeL24(jitterReadBuffer_, outputLength);
            }

            // Note: decodeL16/L24 already call mapChannelsToDevice internally
        } else {
            // Packet not yet available - this could be:
            // 1. Jitter buffer underrun (packet hasn't arrived yet)
            // 2. Packet was dropped or lost
            // 3. Out-of-order packet in buffer but not the one we want

            // Check if there are any packets in the buffer
            size_t bufferedCount = jitterBuffer_.getBufferedPacketCount();

            if (bufferedCount > 0) {
                // There are packets, but not the one we want
                // This indicates out-of-order delivery or packet loss

                // Try waiting a bit for the expected packet to arrive
                // If it doesn't arrive within a reasonable time, skip it
                static constexpr int kMaxRetries = 3;
                int retries = 0;

                while (retries < kMaxRetries && running_) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));

                    gotPacket = jitterBuffer_.getNextPacket(
                        jitterReadBuffer_,
                        sizeof(jitterReadBuffer_),
                        outputLength,
                        presentationTime,
                        expectedSeq
                    );

                    if (gotPacket) {
                        // Got it on retry
                        expectedSequenceNumber_.store((expectedSeq + 1) & 0xFFFF, std::memory_order_relaxed);

                        if (sdp_.encoding == "L16") {
                            decodeL16(jitterReadBuffer_, outputLength);
                        } else if (sdp_.encoding == "L24") {
                            decodeL24(jitterReadBuffer_, outputLength);
                        }
                        break;
                    }

                    retries++;
                }

                if (!gotPacket) {
                    // Packet loss - skip to next sequence number
                    expectedSequenceNumber_.store((expectedSeq + 1) & 0xFFFF, std::memory_order_relaxed);
                    stats_.packetsLost.fetch_add(1, std::memory_order_relaxed);
                    // Only count as underrun if we actually had packets but not the right one
                    stats_.outOfOrderPackets.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                // No packets in buffer at all - underrun
                // Sleep a bit to allow packets to arrive
                std::this_thread::sleep_for(packetInterval);
                stats_.underruns.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

void RTPReceiver::processPacket(const RTP::RTPPacket& packet) {
    if (!validatePacket(packet)) {
        stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Get RTP header info
    uint16_t sequenceNumber = packet.header.sequenceNumber;
    uint32_t timestamp = packet.header.timestamp;

    // Get payload
    uint8_t* payload = packet.payload;
    size_t payloadSize = packet.payloadSize;

    if (!payload || payloadSize == 0) {
        stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Update connection state
    if (!connected_) {
        connected_ = true;
        // Initialize expected sequence number from first packet
        expectedSequenceNumber_.store(sequenceNumber, std::memory_order_relaxed);
    }
    auto now = std::chrono::steady_clock::now();
    lastPacketTimeNs_.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count(),
        std::memory_order_release);

    // Update statistics
    updateStats(sequenceNumber, payloadSize);

    // Calculate presentation time from RTP timestamp delta (wraparound-safe)
    // RTP timestamps are 32-bit and wrap every ~24h at 48kHz.
    // We track the delta from the first timestamp to avoid overflow when
    // multiplying by 1e9, and to correctly handle 32-bit wraparound.
    uint64_t presentationTime = 0;
    if (sdp_.sampleRate > 0) {
        uint32_t firstTs = firstTimestamp_.load(std::memory_order_relaxed);
        if (firstTimestampSet_.load(std::memory_order_relaxed)) {
            // Wraparound-safe delta: unsigned subtraction handles 32-bit wrap
            uint32_t delta = timestamp - firstTs;
            // delta is at most 2^32-1 (~4.29e9). At 384kHz that's ~11184s.
            // 4.29e9 * 1e9 = 4.29e18 < UINT64_MAX (1.84e19), so no overflow.
            presentationTime = (static_cast<uint64_t>(delta) * 1000000000ULL) / sdp_.sampleRate;
        } else {
            // First packet — record baseline timestamp
            firstTimestamp_.store(timestamp, std::memory_order_relaxed);
            firstTimestampSet_.store(true, std::memory_order_relaxed);
            presentationTime = 0;
        }
    }

    // Add packet to jitter buffer (lock-free operation)
    // The jitter buffer will handle reordering based on sequence numbers
    if (!jitterBuffer_.addPacket(payload, payloadSize, sequenceNumber, presentationTime)) {
        // Jitter buffer full or slot already occupied
        stats_.overruns.fetch_add(1, std::memory_order_relaxed);
    }
}

bool RTPReceiver::validatePacket(const RTP::RTPPacket& packet) {
    // Check RTP version (should be 2)
    if (packet.header.version != 2) {
        return false;
    }

    // Check payload type matches SDP
    if (packet.header.payloadType != sdp_.payloadType) {
        return false;
    }

    // Check payload size is reasonable
    if (packet.payloadSize == 0 || packet.payloadSize > 1500) {
        return false;
    }

    return true;
}

void RTPReceiver::decodeL16(const uint8_t* payload, size_t payloadSize) {
    // L16: 16-bit big-endian signed PCM
    const size_t bytesPerSample = 2;
    const size_t bytesPerFrame = bytesPerSample * sdp_.numChannels;
    const size_t frameCount = payloadSize / bytesPerFrame;

    if (frameCount == 0 || frameCount > 512) {
        return; // Invalid or excessive frame count
    }

    // Ensure audio buffer is large enough
    const size_t totalSamples = frameCount * sdp_.numChannels;
    if (totalSamples > audioBuffer_.size()) {
        stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Decode: big-endian int16 → float [-1.0, 1.0)
    for (size_t i = 0; i < totalSamples; ++i) {
        const size_t offset = i * bytesPerSample;
        if (offset + bytesPerSample > payloadSize) break;

        // Big-endian 16-bit signed
        int16_t pcmSample = (payload[offset] << 8) | payload[offset + 1];

        // Convert to float: divide by 32768 (2^15)
        audioBuffer_[i] = pcmSample / 32768.0f;
    }

    // Map to device channels
    mapChannelsToDevice(audioBuffer_.data(), frameCount);
}

void RTPReceiver::decodeL24(const uint8_t* payload, size_t payloadSize) {
    // L24: 24-bit big-endian signed PCM
    const size_t bytesPerSample = 3;
    const size_t bytesPerFrame = bytesPerSample * sdp_.numChannels;
    const size_t frameCount = payloadSize / bytesPerFrame;

    if (frameCount == 0 || frameCount > 512) {
        return; // Invalid or excessive frame count
    }

    // Ensure audio buffer is large enough
    const size_t totalSamples = frameCount * sdp_.numChannels;
    if (totalSamples > audioBuffer_.size()) {
        stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Decode: big-endian int24 → float [-1.0, 1.0)
    for (size_t i = 0; i < totalSamples; ++i) {
        const size_t offset = i * bytesPerSample;
        if (offset + bytesPerSample > payloadSize) break;

        // Big-endian 24-bit signed (sign-extend to 32-bit)
        int32_t pcmSample = (payload[offset] << 24) |
                           (payload[offset + 1] << 16) |
                           (payload[offset + 2] << 8);
        pcmSample >>= 8; // Arithmetic right shift preserves sign

        // Convert to float: divide by 8388608 (2^23)
        audioBuffer_[i] = pcmSample / 8388608.0f;
    }

    // Map to device channels
    mapChannelsToDevice(audioBuffer_.data(), frameCount);
}

void RTPReceiver::mapChannelsToDevice(const float* interleavedAudio, size_t frameCount) {
    // Validate mapping
    const size_t deviceChannelEnd = mapping_.deviceChannelStart + sdp_.numChannels;
    if (deviceChannelEnd > 128) {
        return; // Mapping out of range
    }

    // Stack-allocated temporary buffer for de-interleaving (max 512 frames)
    constexpr size_t kMaxFrames = 512;
    if (frameCount > kMaxFrames) {
        return;
    }

    float channelBuffer[kMaxFrames];

    // Write each stream channel to its mapped device channel
    // This de-interleaves: [ch0_f0, ch1_f0, ch0_f1, ch1_f1, ...]
    //                   → deviceChannels[0]: [ch0_f0, ch0_f1, ...]
    //                   → deviceChannels[1]: [ch1_f0, ch1_f1, ...]

    bool hadUnderrun = false;

    for (size_t streamChannel = 0; streamChannel < sdp_.numChannels; ++streamChannel) {
        const size_t deviceChannel = mapping_.deviceChannelStart + streamChannel;

        // Extract this channel from interleaved stream
        for (size_t frame = 0; frame < frameCount; ++frame) {
            channelBuffer[frame] = interleavedAudio[frame * sdp_.numChannels + streamChannel];
        }

        // Write to device ring buffer (batch write)
        const size_t written = deviceChannels_[deviceChannel].write(channelBuffer, frameCount);

        if (written < frameCount && !hadUnderrun) {
            // Ring buffer full - count underrun once per packet
            stats_.underruns.fetch_add(1, std::memory_order_relaxed);
            hadUnderrun = true;
        }
    }
}

void RTPReceiver::updateStats(uint16_t sequenceNumber, size_t payloadSize) {
    // Atomic operations eliminate need for mutex lock

    // Detect packet loss (sequence number gaps)
    uint64_t currentPacketCount = stats_.packetsReceived.load(std::memory_order_relaxed);
    if (currentPacketCount > 0) {
        uint16_t expected = lastSequenceNumber_.load(std::memory_order_relaxed) + 1;
        if (sequenceNumber != expected) {
            // Handle sequence number wrap-around
            uint16_t gap = sequenceNumber - expected;
            stats_.packetsLost.fetch_add(gap, std::memory_order_relaxed);
        }
    }

    lastSequenceNumber_.store(sequenceNumber, std::memory_order_relaxed);
    stats_.packetsReceived.fetch_add(1, std::memory_order_relaxed);
    stats_.bytesReceived.fetch_add(payloadSize, std::memory_order_relaxed);

    // Note: Packet loss percentage is calculated by Statistics::getPacketLossPercent()
}

} // namespace AES67
