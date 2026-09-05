//
// RTPReceiver.cpp
// AES67 macOS Driver - Build #9
// RTP packet receiver with L16/L24 decoding and channel mapping integration
//

#include "RTPReceiver.h"
#include "SimpleRTP.h"
#include "PCMCodec.h"
#include "Driver/DebugLog.h"
#include "NetworkEngine/SelectWait.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <sys/select.h>

namespace AES67 {

RTPReceiver::RTPReceiver(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    DeviceChannelBuffers& deviceChannels,
    size_t jitterBufferDepth,
    const std::string& networkInterface,
    uint32_t playoutDelaySamples
)
    : sdp_(sdp)
    , mapping_(mapping)
    , deviceChannels_(deviceChannels)
    , jitterBuffer_(jitterBufferDepth > 0 ? jitterBufferDepth
                                          : LockFreeCircularJitterBuffer::DEFAULT_BUFFER_SIZE)
    , networkInterface_(networkInterface)
{
    // Samples per packet, derived the SAME way RTPTransmitter does — this
    // must match, or the receiver consumes at a different cadence than the
    // sender produces and starves or overruns. Explicit a=framecount wins
    // when present; otherwise derive from ptimeUs (a bare sub-millisecond
    // a=ptime with no framecount — ST 2110-30 Level B's 125 us — landed on
    // the wrong value while framecount defaulted to 48 and was treated as
    // "present"). Never zero.
    const uint64_t rate = std::max<uint32_t>(sdp_.sampleRate, 1);
    uint32_t samplesPerPacket = sdp_.framecount > 0
        ? sdp_.framecount
        : static_cast<uint32_t>((rate * sdp_.ptimeUs) / 1000000ULL);
    if (samplesPerPacket == 0) samplesPerPacket = 1;

    // Playout delay, expressed the way installers think about it (samples)
    // and applied the way this receiver can honour it (packets of cushion
    // before paced consumption begins).
    if (playoutDelaySamples > 0) {
        const size_t packets = (playoutDelaySamples + samplesPerPacket - 1) / samplesPerPacket;
        prefillPacketCount_ = std::max<size_t>(packets, 1); // a zero cushion starves immediately
    }

    // stats_.reset(), not memset: Statistics is eleven std::atomic members, so
    // writing over it with memset is undefined behaviour -- and the counters are
    // read from another thread while the receiver runs, which is exactly where
    // that bites. reset() stores zero into each one.
    stats_.reset();

    // Pre-allocate audio buffer to avoid allocations in receiveLoop()
    // Max kMaxFramesPerPacket frames × stream channels (512 × 8 = 4096 floats)
    const size_t maxFrames = kMaxFramesPerPacket;
    const size_t maxSamples = maxFrames * sdp_.numChannels;
    audioBuffer_.resize(maxSamples);

    // Interval derived FROM samplesPerPacket so the two always agree, same
    // as the transmitter — consume one packet's worth of samples per
    // interval.
    const uint64_t intervalUs = (static_cast<uint64_t>(samplesPerPacket) * 1000000ULL) / rate;
    packetInterval_ = std::chrono::microseconds(std::max<uint64_t>(intervalUs, 1));

    // Resolve network interface name to IP address
    if (!networkInterface_.empty()) {
        bool looksLikeIP = true;
        for (char c : networkInterface_) {
            if (c != '.' && !isdigit(c)) {
                looksLikeIP = false;
                break;
            }
        }

        if (looksLikeIP) {
            resolvedInterfaceIP_ = networkInterface_;
            AES67_LOGF("RTPReceiver: using interface IP %s directly (stream=%s)",
                       resolvedInterfaceIP_.c_str(), sdp_.sessionName.c_str());
        } else {
            resolvedInterfaceIP_ = NetworkInterfaceDetection::getInterfaceIPAddress(networkInterface_);
            if (resolvedInterfaceIP_.empty()) {
                AES67_LOGF("RTPReceiver: WARNING - failed to resolve interface '%s' to IP, "
                           "falling back to INADDR_ANY (stream=%s)",
                           networkInterface_.c_str(), sdp_.sessionName.c_str());
            } else {
                AES67_LOGF("RTPReceiver: resolved interface '%s' to IP %s (stream=%s)",
                           networkInterface_.c_str(), resolvedInterfaceIP_.c_str(),
                           sdp_.sessionName.c_str());
            }
        }
    }
}

RTPReceiver::~RTPReceiver() {
    stop();
}

bool RTPReceiver::start() {
    if (running_ || rtpSocket_.isOpen()) {
        AES67_LOGF("RTPReceiver::start: already running or socket open (stream=%s)",
                   sdp_.sessionName.c_str());
        return false; // Already running
    }

    // Validate SDP configuration
    if (sdp_.connectionAddress.empty() || sdp_.port == 0) {
        AES67_LOGF("RTPReceiver::start: invalid SDP - address='%s' port=%u (stream=%s)",
                   sdp_.connectionAddress.c_str(), sdp_.port, sdp_.sessionName.c_str());
        return false;
    }

    if (sdp_.numChannels == 0 || sdp_.numChannels > 128) {
        AES67_LOGF("RTPReceiver::start: invalid channel count %u (stream=%s)",
                   sdp_.numChannels, sdp_.sessionName.c_str());
        return false;
    }

    // Open RTP receiver socket, optionally bound to a specific interface
    const char* ifaceIP = resolvedInterfaceIP_.empty() ? nullptr : resolvedInterfaceIP_.c_str();
    if (!rtpSocket_.openReceiver(sdp_.connectionAddress.c_str(), sdp_.port, ifaceIP)) {
        AES67_LOGF("RTPReceiver::start: socket open failed for %s:%u iface=%s (stream=%s)",
                   sdp_.connectionAddress.c_str(), sdp_.port,
                   resolvedInterfaceIP_.empty() ? "ANY" : resolvedInterfaceIP_.c_str(),
                   sdp_.sessionName.c_str());
        return false;
    }

    AES67_LOGF("RTPReceiver::start: opened socket for %s:%u on interface %s (stream=%s)",
               sdp_.connectionAddress.c_str(), sdp_.port,
               resolvedInterfaceIP_.empty() ? "INADDR_ANY" : resolvedInterfaceIP_.c_str(),
               sdp_.sessionName.c_str());

    // Reset jitter buffer and prefill gate
    jitterBuffer_.reset();
    expectedSequenceNumber_.store(0, std::memory_order_relaxed);
    prefillComplete_.store(false, std::memory_order_relaxed);

    // Start receive thread (producer - adds packets to jitter buffer)
    running_ = true;
    receiveThread_ = std::thread([this]() {
        // Real cycle length is the stream's own packet time, not a guess.
        if (!AudioThreadPriority::configureForRealTime(sdp_.ptimeUs / 1000.0)) {
            AES67_LOGF("RTPReceiver: failed to set RT priority on receive thread (stream=%s)",
                       sdp_.sessionName.c_str());
        }
        receiveLoop();
    });

    // Start consume thread (consumer - reads from jitter buffer and writes to ring buffers)
    consumeThread_ = std::thread([this]() {
        if (!AudioThreadPriority::configureForRealTime(sdp_.ptimeUs / 1000.0)) {
            AES67_LOGF("RTPReceiver: failed to set RT priority on consume thread (stream=%s)",
                       sdp_.sessionName.c_str());
        }
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
    RTP::RTPPacket packet;

    // Re-join the multicast group every few seconds so reception recovers on
    // its own after a network-interface flap (cable unplug/replug), which can
    // silently drop the membership while leaving the socket open. This is not
    // the real-time audio callback — it runs on the receive thread — so a
    // periodic setsockopt here is safe.
    auto lastRejoin = std::chrono::steady_clock::now();

    while (running_) {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastRejoin >= std::chrono::seconds(5)) {
            rtpSocket_.rejoinMulticast();
            lastRejoin = now;
        }

        // Set up select with 1ms timeout (responsive but not spinning)
        FD_ZERO(&readfds);
        int sockfd = rtpSocket_.getFd();
        if (sockfd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        FD_SET(sockfd, &readfds);

        // 1 ms: responsive without spinning.
        const SelectOutcome outcome = waitReadable(sockfd, &readfds, 1);
        if (outcome == SelectOutcome::Failed) {
            // A descriptor that select() rejects will reject every following
            // call too, and continuing on it burned a core doing nothing
            // (2026-09-04 audit). Stop the loop; the socket comes back with a
            // restart, not by asking again.
            AES67_LOGF("RTPReceiver: select() failed on the RTP socket (errno=%d) — "
                       "receive loop stopping (stream=%s)",
                       errno, sdp_.sessionName.c_str());
            break;
        }
        if (outcome == SelectOutcome::Ready && FD_ISSET(sockfd, &readfds)) {
            ssize_t bytesReceived = rtpSocket_.receive(packet, receiveBuffer_, sizeof(receiveBuffer_));
            if (bytesReceived > 0) {
                processPacket(packet);
            }
        }
    }
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

    // ── Phase 1: Pre-fill wait ──────────────────────────────────────
    // The receive thread is already running and populating the jitter buffer.
    // We wait here until enough packets have accumulated before starting
    // paced consumption. During this time Core Audio reads empty ring buffers
    // and fills silence (existing AES67IOHandler behavior).
    while (running_ && !prefillComplete_.load(std::memory_order_relaxed)) {
        size_t buffered = jitterBuffer_.getBufferedPacketCount();
        if (buffered >= prefillPacketCount_) {
            prefillComplete_.store(true, std::memory_order_relaxed);
            break;
        }
        std::this_thread::sleep_for(packetInterval_);
    }

    // ── Phase 2: Paced consumption via sleep_until ──────────────────
    // Target absolute wall-clock times so we don't accumulate drift from
    // variable decode/write latency. This is the same pattern used by
    // RTPTransmitter::transmitLoop().
    auto nextConsumeTime = std::chrono::steady_clock::now();

    // Adaptive rate matching state
    double rateAdjustment = 0.0;        // current fractional adjustment
    size_t packetsSinceRateCheck = 0;

    while (running_) {
        std::this_thread::sleep_until(nextConsumeTime);

        // Advance target time (with rate adjustment applied)
        auto adjustedInterval = std::chrono::microseconds(
            static_cast<int64_t>(packetInterval_.count() * (1.0 + rateAdjustment))
        );
        nextConsumeTime += adjustedInterval;

        uint32_t expectedSeq = expectedSequenceNumber_.load(std::memory_order_acquire);

        bool gotPacket = jitterBuffer_.getNextPacket(
            jitterReadBuffer_,
            sizeof(jitterReadBuffer_),
            outputLength,
            presentationTime,
            expectedSeq
        );

        if (gotPacket) {
            // Successfully got the expected packet — decode and map to ring buffers
            expectedSequenceNumber_.store((expectedSeq + 1) & 0xFFFF, std::memory_order_release);

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
                expectedSequenceNumber_.store((expectedSeq + 1) & 0xFFFF, std::memory_order_release);
                stats_.packetsLost.fetch_add(1, std::memory_order_relaxed);
            } else {
                // Buffer completely empty — underrun.
                // Do NOT advance sequence number; the packet may still arrive.
                stats_.underruns.fetch_add(1, std::memory_order_relaxed);
            }
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
    if (!validatePacket(packet)) {
        uint64_t count = stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed) + 1;
        // Log first occurrence and then every 100th to avoid flooding
        if (count == 1 || count % 100 == 0) {
            AES67_LOGF("RTPReceiver: malformed packet #%llu (ver=%u pt=%u size=%zu, expected pt=%u) stream=%s",
                       (unsigned long long)count, packet.header.version,
                       packet.header.payloadType, packet.payloadSize,
                       sdp_.payloadType, sdp_.sessionName.c_str());
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
        uint64_t count = stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 100 == 0) {
            AES67_LOGF("RTPReceiver: empty payload in packet #%llu (seq=%u) stream=%s",
                       (unsigned long long)count, sequenceNumber, sdp_.sessionName.c_str());
        }
        return;
    }

    // Update connection state
    if (!connected_) {
        connected_ = true;
        // Initialize expected sequence number from first packet.
        // Use release ordering so the consume thread (which loads with
        // acquire) is guaranteed to see this initial value.
        expectedSequenceNumber_.store(sequenceNumber, std::memory_order_release);
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

    if (frameCount == 0 || frameCount > kMaxFramesPerPacket) {
        return; // Invalid or excessive frame count
    }

    // Ensure audio buffer is large enough
    const size_t totalSamples = frameCount * sdp_.numChannels;
    if (totalSamples > audioBuffer_.size()) {
        stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Decode big-endian int16 -> float. vDSP-accelerated on Apple, scalar
    // fallback elsewhere, identical result either way (TestPCMCodec).
    // frameCount is derived from payloadSize and bounded above, so
    // totalSamples samples are fully present in the payload.
    decodeL16BE(payload, totalSamples, audioBuffer_.data());

    // Map to device channels
    mapChannelsToDevice(audioBuffer_.data(), frameCount);
}

void RTPReceiver::decodeL24(const uint8_t* payload, size_t payloadSize) {
    // L24: 24-bit big-endian signed PCM
    const size_t bytesPerSample = 3;
    const size_t bytesPerFrame = bytesPerSample * sdp_.numChannels;
    const size_t frameCount = payloadSize / bytesPerFrame;

    if (frameCount == 0 || frameCount > kMaxFramesPerPacket) {
        return; // Invalid or excessive frame count
    }

    // Ensure audio buffer is large enough
    const size_t totalSamples = frameCount * sdp_.numChannels;
    if (totalSamples > audioBuffer_.size()) {
        stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Decode big-endian 24-bit -> float. See decodeL16 above; same split of
    // vectorised arithmetic (vDSP) and scalar byte unpacking, pinned by
    // TestPCMCodec.
    decodeL24BE(payload, totalSamples, audioBuffer_.data());

    // Map to device channels
    mapChannelsToDevice(audioBuffer_.data(), frameCount);
}

void RTPReceiver::mapChannelsToDevice(const float* interleavedAudio, size_t frameCount) {
    // Validate mapping
    const size_t deviceChannelEnd = mapping_.deviceChannelStart + sdp_.numChannels;
    if (deviceChannelEnd > 128) {
        return; // Mapping out of range
    }

    // Stack-allocated temporary buffer for de-interleaving, sized by the same
    // bound the decoders enforce.
    if (frameCount > kMaxFramesPerPacket) {
        return;
    }

    float channelBuffer[kMaxFramesPerPacket];

    // Write each stream channel to its mapped device channel
    // This de-interleaves: [ch0_f0, ch1_f0, ch0_f1, ch1_f1, ...]
    //                   → deviceChannels[0]: [ch0_f0, ch0_f1, ...]
    //                   → deviceChannels[1]: [ch1_f0, ch1_f1, ...]

    bool hadUnderrun = false;

    for (size_t streamChannel = 0; streamChannel < sdp_.numChannels; ++streamChannel) {
        const size_t deviceChannel = mapping_.deviceChannelStart + streamChannel;

        // Extract this channel from the interleaved stream (deinterleave).
        deinterleaveChannel(interleavedAudio, channelBuffer, frameCount,
                            sdp_.numChannels, streamChannel);

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
}

} // namespace AES67
