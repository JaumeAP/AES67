//
// RTPReceiver.cpp
// AES67 macOS Driver - Build #9
// RTP packet receiver with L16/L24 decoding and channel mapping integration
//

#include "RTPReceiver.h"
#include "SimpleRTP.h"
#include "../../Driver/DebugLog.h"
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

    // Calculate packet interval from SDP (mirrors RTPTransmitter constructor)
    // Priority: ptime field > framecount/sampleRate > 1ms fallback
    if (sdp_.ptime > 0) {
        packetInterval_ = std::chrono::microseconds(sdp_.ptime * 1000);
    } else if (sdp_.framecount > 0 && sdp_.sampleRate > 0) {
        uint64_t intervalUs = (static_cast<uint64_t>(sdp_.framecount) * 1000000ULL) / sdp_.sampleRate;
        packetInterval_ = std::chrono::microseconds(intervalUs);
    } else {
        packetInterval_ = std::chrono::microseconds(1000); // 1ms default for AES67
    }
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

    // Reset jitter buffer and prefill gate
    jitterBuffer_.reset();
    expectedSequenceNumber_.store(0, std::memory_order_relaxed);
    prefillComplete_.store(false, std::memory_order_relaxed);

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
    uint64_t selectCalls = 0;
    uint64_t selectReady = 0;
    uint64_t recvSuccess = 0;
    uint64_t recvFail = 0;

    AES67_LOGF("receiveLoop: STARTED on %s:%u (fd=%d, packetInterval=%lldus)",
               sdp_.connectionAddress.c_str(), sdp_.port,
               rtpSocket_.getFd(), (long long)packetInterval_.count());

    while (running_) {
        // Set up select with 1ms timeout (responsive but not spinning)
        FD_ZERO(&readfds);
        int sockfd = rtpSocket_.getFd();
        if (sockfd < 0) {
            // Socket not valid, wait and retry
            AES67_LOG("receiveLoop: socket fd < 0, waiting...");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        FD_SET(sockfd, &readfds);

        tv.tv_sec = 0;
        tv.tv_usec = 1000;  // 1ms timeout

        int ret = select(sockfd + 1, &readfds, nullptr, nullptr, &tv);
        ++selectCalls;

        if (ret > 0 && FD_ISSET(sockfd, &readfds)) {
            ++selectReady;
            // Data available - receive it
            ssize_t bytesReceived = rtpSocket_.receive(packet, receiveBuffer_, sizeof(receiveBuffer_));
            if (bytesReceived > 0) {
                ++recvSuccess;
                if (recvSuccess == 1) {
                    AES67_LOGF("receiveLoop: FIRST PACKET! %zd bytes, ver=%u pt=%u seq=%u ts=%u",
                               bytesReceived, packet.header.version, packet.header.payloadType,
                               packet.header.sequenceNumber, packet.header.timestamp);
                }
                processPacket(packet);
            } else {
                ++recvFail;
                if (recvFail <= 3) {
                    AES67_LOGF("receiveLoop: receive returned %zd (errno=%d)", bytesReceived, errno);
                }
            }
        }

        // Log progress every 5 seconds (~5000 select calls at 1ms timeout)
        if (selectCalls % 5000 == 0) {
            AES67_LOGF("receiveLoop: selectCalls=%llu ready=%llu recvOK=%llu recvFail=%llu jbuf=%zu",
                       selectCalls, selectReady, recvSuccess, recvFail,
                       jitterBuffer_.getBufferedPacketCount());
        }
    }

    AES67_LOGF("receiveLoop: STOPPED (recvOK=%llu, recvFail=%llu)", recvSuccess, recvFail);
}

void RTPReceiver::consumeLoop() {
    // This thread pulls packets from the jitter buffer in sequence order
    // and writes decoded audio to the device ring buffers.
    //
    // Two phases:
    //   1. Pre-fill: wait for the jitter buffer to accumulate enough packets
    //      so that the consumer has a cushion against network jitter.
    //   2. Paced consumption: consume one packet per packetInterval_ using
    //      sleep_until (mirrors RTPTransmitter::transmitLoop pattern).

    size_t outputLength = 0;
    uint64_t presentationTime = 0;

    AES67_LOG("consumeLoop: STARTED - entering Phase 1 (prefill wait)");

    // ── Phase 1: Pre-fill wait ──────────────────────────────────────
    // The receive thread is already running and populating the jitter buffer.
    // We wait here until enough packets have accumulated before starting
    // paced consumption. During this time Core Audio reads empty ring buffers
    // and fills silence (existing AES67IOHandler behavior).
    uint64_t prefillChecks = 0;
    while (running_ && !prefillComplete_.load(std::memory_order_relaxed)) {
        size_t buffered = jitterBuffer_.getBufferedPacketCount();
        ++prefillChecks;
        if (prefillChecks % 5000 == 0) {
            AES67_LOGF("consumeLoop: prefill waiting... buffered=%zu (need %zu) checks=%llu",
                       buffered, kPrefillPacketCount, prefillChecks);
        }
        if (buffered >= kPrefillPacketCount) {
            prefillComplete_.store(true, std::memory_order_relaxed);
            AES67_LOGF("consumeLoop: PREFILL COMPLETE! buffered=%zu after %llu checks", buffered, prefillChecks);
            break;
        }
        std::this_thread::sleep_for(packetInterval_);
    }

    AES67_LOGF("consumeLoop: entering Phase 2 (paced consumption) expectedSeq=%u",
               expectedSequenceNumber_.load(std::memory_order_relaxed));

    // ── Phase 2: Paced consumption via sleep_until ──────────────────
    // Target absolute wall-clock times so we don't accumulate drift from
    // variable decode/write latency. This is the same pattern used by
    // RTPTransmitter::transmitLoop().
    auto nextConsumeTime = std::chrono::steady_clock::now();

    // Adaptive rate matching state
    double rateAdjustment = 0.0;        // current fractional adjustment
    size_t packetsSinceRateCheck = 0;

    uint64_t consumeOK = 0, consumeMiss = 0, consumeUnderrun = 0;

    while (running_) {
        std::this_thread::sleep_until(nextConsumeTime);

        // Advance target time (with rate adjustment applied)
        auto adjustedInterval = std::chrono::microseconds(
            static_cast<int64_t>(packetInterval_.count() * (1.0 + rateAdjustment))
        );
        nextConsumeTime += adjustedInterval;

        uint32_t expectedSeq = expectedSequenceNumber_.load(std::memory_order_relaxed);

        bool gotPacket = jitterBuffer_.getNextPacket(
            jitterReadBuffer_,
            sizeof(jitterReadBuffer_),
            outputLength,
            presentationTime,
            expectedSeq
        );

        if (gotPacket) {
            ++consumeOK;
            // Successfully got the expected packet — decode and map to ring buffers
            expectedSequenceNumber_.store((expectedSeq + 1) & 0xFFFF, std::memory_order_relaxed);

            if (consumeOK <= 3) {
                AES67_LOGF("consumeLoop: GOT PACKET seq=%u len=%zu (ok#%llu)", expectedSeq, outputLength, consumeOK);
            }

            if (sdp_.encoding == "L16") {
                decodeL16(jitterReadBuffer_, outputLength);
            } else if (sdp_.encoding == "L24") {
                decodeL24(jitterReadBuffer_, outputLength);
            }
        } else {
            // Expected packet not available
            size_t bufferedCount = jitterBuffer_.getBufferedPacketCount();

            if (bufferedCount > 0) {
                // Buffer has packets but not the one we want — packet loss.
                // Skip to the next sequence number so we don't stall.
                expectedSequenceNumber_.store((expectedSeq + 1) & 0xFFFF, std::memory_order_relaxed);
                stats_.packetsLost.fetch_add(1, std::memory_order_relaxed);
                ++consumeMiss;
                if (consumeMiss <= 5) {
                    AES67_LOGF("consumeLoop: MISS seq=%u buffered=%zu (miss#%llu)", expectedSeq, bufferedCount, consumeMiss);
                }
            } else {
                // Buffer completely empty — underrun.
                // Do NOT advance sequence number; the packet may still arrive.
                stats_.underruns.fetch_add(1, std::memory_order_relaxed);
                ++consumeUnderrun;
            }
        }

        // Log periodic summary
        uint64_t totalAttempts = consumeOK + consumeMiss + consumeUnderrun;
        if (totalAttempts % 5000 == 0 && totalAttempts > 0) {
            AES67_LOGF("consumeLoop: ok=%llu miss=%llu underrun=%llu expectedSeq=%u jbuf=%zu rateAdj=%.6f",
                       consumeOK, consumeMiss, consumeUnderrun, expectedSeq,
                       jitterBuffer_.getBufferedPacketCount(), rateAdjustment);
        }

        // ── Adaptive rate matching ──────────────────────────────────
        // Every kRateCheckIntervalPackets, sample the first mapped channel's
        // ring buffer fill level and nudge the consume rate to keep it near 50%.
        ++packetsSinceRateCheck;
        if (packetsSinceRateCheck >= kRateCheckIntervalPackets) {
            packetsSinceRateCheck = 0;

            size_t deviceCh = mapping_.deviceChannelStart;
            if (deviceCh < 128) {
                auto& ringBuf = deviceChannels_[deviceCh];
                size_t cap = ringBuf.capacity();
                if (cap > 0) {
                    double fillRatio = static_cast<double>(ringBuf.available()) /
                                       static_cast<double>(cap);
                    double error = fillRatio - kTargetFillRatio;

                    // Positive error (buffer too full) → speed up consumption (negative adjust)
                    // Negative error (buffer too empty) → slow down consumption (positive adjust)
                    rateAdjustment -= error * kRateAdjustmentGain;

                    // Clamp to maximum adjustment range
                    if (rateAdjustment > kMaxRateAdjustment) rateAdjustment = kMaxRateAdjustment;
                    if (rateAdjustment < -kMaxRateAdjustment) rateAdjustment = -kMaxRateAdjustment;
                }
            }
        }
    }
}

void RTPReceiver::processPacket(const RTP::RTPPacket& packet) {
    static uint64_t validateFailCount = 0;
    if (!validatePacket(packet)) {
        stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed);
        ++validateFailCount;
        if (validateFailCount <= 5) {
            AES67_LOGF("processPacket: VALIDATION FAILED #%llu (ver=%u pt=%u expected_pt=%u payloadSize=%zu)",
                       validateFailCount, packet.header.version, packet.header.payloadType,
                       sdp_.payloadType, packet.payloadSize);
        }
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
        AES67_LOGF("processPacket: null/empty payload (payload=%p size=%zu)", (void*)payload, payloadSize);
        return;
    }

    // Update connection state
    if (!connected_) {
        connected_ = true;
        // Initialize expected sequence number from first packet
        expectedSequenceNumber_.store(sequenceNumber, std::memory_order_relaxed);
        AES67_LOGF("processPacket: CONNECTED! firstSeq=%u firstTs=%u payloadSize=%zu encoding=%s",
                   sequenceNumber, timestamp, payloadSize, sdp_.encoding.c_str());
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
    static uint64_t addOK = 0, addFail = 0;
    if (!jitterBuffer_.addPacket(payload, payloadSize, sequenceNumber, presentationTime)) {
        // Jitter buffer full or slot already occupied
        stats_.overruns.fetch_add(1, std::memory_order_relaxed);
        ++addFail;
        if (addFail <= 5) {
            AES67_LOGF("processPacket: jitterBuffer.addPacket FAILED seq=%u (fail#%llu, buffered=%zu)",
                       sequenceNumber, addFail, jitterBuffer_.getBufferedPacketCount());
        }
    } else {
        ++addOK;
        if (addOK <= 3) {
            AES67_LOGF("processPacket: jitterBuffer.addPacket OK seq=%u (ok#%llu, buffered=%zu)",
                       sequenceNumber, addOK, jitterBuffer_.getBufferedPacketCount());
        }
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

    // Log first decode
    static uint64_t decodeCount = 0;
    ++decodeCount;
    if (decodeCount <= 2) {
        float peak = 0.0f;
        for (size_t i = 0; i < totalSamples && i < 48; ++i) {
            float abs = audioBuffer_[i] < 0 ? -audioBuffer_[i] : audioBuffer_[i];
            if (abs > peak) peak = abs;
        }
        AES67_LOGF("decodeL24: frames=%zu ch=%u totalSamples=%zu peak=%.6f firstBytes=[%02x %02x %02x]",
                   frameCount, sdp_.numChannels, totalSamples, peak,
                   payload[0], payload[1], payload[2]);
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

    // Stack-allocated temporary buffer for de-interleaving
    constexpr size_t kMaxFrames = 4096;
    if (frameCount > kMaxFrames) {
        return;
    }

    float channelBuffer[kMaxFrames];

    // Write each stream channel to its mapped device channel
    // This de-interleaves: [ch0_f0, ch1_f0, ch0_f1, ch1_f1, ...]
    //                   → deviceChannels[0]: [ch0_f0, ch0_f1, ...]
    //                   → deviceChannels[1]: [ch1_f0, ch1_f1, ...]

    bool hadUnderrun = false;

    static uint64_t mapCount = 0;
    ++mapCount;

    for (size_t streamChannel = 0; streamChannel < sdp_.numChannels; ++streamChannel) {
        const size_t deviceChannel = mapping_.deviceChannelStart + streamChannel;

        // Extract this channel from interleaved stream
        for (size_t frame = 0; frame < frameCount; ++frame) {
            channelBuffer[frame] = interleavedAudio[frame * sdp_.numChannels + streamChannel];
        }

        // Write to device ring buffer (batch write)
        const size_t written = deviceChannels_[deviceChannel].write(channelBuffer, frameCount);

        if (mapCount <= 2 && streamChannel == 0) {
            AES67_LOGF("mapChannels: ch%zu→devCh%zu frames=%zu written=%zu cap=%zu avail=%zu first=%.6f",
                       streamChannel, deviceChannel, frameCount, written,
                       deviceChannels_[deviceChannel].capacity(),
                       deviceChannels_[deviceChannel].available(),
                       channelBuffer[0]);
        }

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
    // Use signed 16-bit arithmetic so that the two's-complement result
    // correctly distinguishes forward gaps (lost packets) from backward
    // gaps (reordered/duplicate packets), even across 16-bit wraparound.
    uint64_t currentPacketCount = stats_.packetsReceived.load(std::memory_order_relaxed);
    if (currentPacketCount > 0) {
        uint16_t expected = lastSequenceNumber_.load(std::memory_order_relaxed) + 1;
        if (sequenceNumber != expected) {
            int16_t gap = static_cast<int16_t>(sequenceNumber - expected);
            if (gap > 0) {
                // Forward gap: packets between expected and sequenceNumber were lost
                stats_.packetsLost.fetch_add(static_cast<uint64_t>(gap), std::memory_order_relaxed);
            } else {
                // Negative gap: packet arrived out of order (or duplicate)
                stats_.outOfOrderPackets.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    lastSequenceNumber_.store(sequenceNumber, std::memory_order_relaxed);
    stats_.packetsReceived.fetch_add(1, std::memory_order_relaxed);
    stats_.bytesReceived.fetch_add(payloadSize, std::memory_order_relaxed);

    // Note: Packet loss percentage is calculated by Statistics::getPacketLossPercent()
}

} // namespace AES67
