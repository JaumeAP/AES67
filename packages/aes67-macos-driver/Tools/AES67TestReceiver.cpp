//
// AES67TestReceiver.cpp
// Standalone CLI tool: receives RTP multicast packets and reports statistics
// for verifying the driver's TX path (Core Audio -> Network).
//
// Packet accounting follows RFC 3550 SS A.1/A.3, per synchronisation source:
// an extended sequence number with a cycle count, loss as expected minus
// received, and reordering counted apart from duplication. The earlier
// counters compared each packet with the one before it, which made a
// reordered stream report loss it had not suffered -- under 20 ms of jitter,
// 118k packets arrived intact and were reported as 336k gaps.
//
// It also watches what the stream says it is: payload type, payload size and
// the timestamp step between consecutive packets. A sender that changes
// sample rate or encoding mid-flight keeps the same address and port, so
// without these the receiver decodes the new bytes with the old format and
// reports nothing.
//
// Usage:
//   ./AES67TestReceiver [options]
//
// Options:
//   --ip <addr>       Multicast IP (default: 239.1.1.2)
//   --port <port>     RTP port (default: 5004)
//   --channels <n>    Expected channels (default: 8)
//   --encoding <enc>  L16 or L24 (default: L24)
//   --duration <sec>  Duration in seconds (default: 10, 0 = infinite)
//   --interface <ip>  Local interface IP to join the group on (default: system)
//

#include <exception>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <csignal>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <map>
#include <deque>
#include <set>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>

// Use the project's RTP header for consistency
#include "NetworkEngine/RTP/SimpleRTP.h"

// -- Globals --
static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

namespace {

// RFC 3550 SS A.1 constants.
constexpr uint32_t kRtpSeqMod    = 1u << 16;
constexpr uint16_t kMaxDropout   = 3000;   // forward jump treated as a restart
constexpr uint16_t kMaxMisorder  = 100;    // backward jump treated as reordering
constexpr size_t   kSeenWindow   = 8192;   // extended sequence numbers kept

// What one synchronisation source has sent, and what arrived of it.
struct SourceStats {
    // RFC 3550 SS A.1 sequence state.
    uint32_t baseSeq{0};
    uint32_t maxSeq{0};
    uint32_t cycles{0};
    uint32_t badSeq{kRtpSeqMod + 1};
    uint64_t received{0};

    uint64_t duplicates{0};
    uint64_t outOfOrder{0};
    uint64_t restarts{0};       // sequence started over: a new sender on the group

    // Recently seen extended sequence numbers, so that a late packet is told
    // apart from a repeated one. Bounded: a window, not the whole history.
    std::set<uint64_t>   seen;
    std::deque<uint64_t> seenOrder;

    // What the stream claims to be. First value wins; every later change is
    // counted and the new value recorded.
    uint8_t  payloadType{0};
    uint64_t payloadTypeChanges{0};
    std::set<uint8_t> payloadTypesSeen;

    size_t   payloadSize{0};
    uint64_t payloadSizeChanges{0};

    uint32_t timestampStep{0};      // frames per packet, as the sender counts them
    uint64_t timestampStepChanges{0};
    std::set<uint32_t> timestampStepsSeen;

    uint16_t prevSeq{0};
    uint32_t prevTimestamp{0};
    bool     havePrev{false};

    uint64_t payloadBytes{0};

    void initSeq(uint16_t seq) {
        baseSeq = seq;
        maxSeq  = seq;
        cycles  = 0;
        badSeq  = kRtpSeqMod + 1;
        received = 0;
        seen.clear();
        seenOrder.clear();
    }

    void remember(uint64_t extendedSeq) {
        seen.insert(extendedSeq);
        seenOrder.push_back(extendedSeq);
        if (seenOrder.size() > kSeenWindow) {
            seen.erase(seenOrder.front());
            seenOrder.pop_front();
        }
    }

    uint64_t extendedMax() const { return static_cast<uint64_t>(cycles) + maxSeq; }

    uint64_t expected() const { return extendedMax() - baseSeq + 1; }

    // Loss is what the sequence numbers say was sent minus what arrived.
    // Duplicates can push received past expected, hence the signed result.
    int64_t lost() const {
        return static_cast<int64_t>(expected()) - static_cast<int64_t>(received);
    }
};

// RFC 3550 SS A.1 update_seq(), with duplicates and reordering separated.
void updateSeq(SourceStats& source, uint16_t seq) {
    const uint16_t udelta = static_cast<uint16_t>(seq - source.maxSeq);

    if (udelta < kMaxDropout) {
        // In order, possibly with a gap in front of it.
        if (seq < source.maxSeq) {
            source.cycles += kRtpSeqMod;   // sequence number wrapped
        }
        source.maxSeq = seq;
    } else if (udelta <= kRtpSeqMod - kMaxMisorder) {
        // Too far forward to be a gap: either the sender restarted or this is
        // a different stream on the same address. Two in a row settle it.
        if (seq == source.badSeq) {
            source.initSeq(seq);
            ++source.restarts;
        } else {
            source.badSeq = (seq + 1) & (kRtpSeqMod - 1);
            ++source.outOfOrder;
            ++source.received;
            return;
        }
    } else {
        // Behind the highest seen: late or repeated, decided by the window.
        uint64_t extended = static_cast<uint64_t>(source.cycles) + seq;
        if (extended > source.extendedMax()) {
            extended -= kRtpSeqMod;        // arrived before its own wrap
        }
        if (source.seen.count(extended) > 0) {
            ++source.duplicates;
        } else {
            ++source.outOfOrder;
            source.remember(extended);
        }
        ++source.received;
        return;
    }

    const uint64_t extended = static_cast<uint64_t>(source.cycles) + seq;
    if (source.seen.count(extended) > 0) {
        ++source.duplicates;
    } else {
        source.remember(extended);
    }
    ++source.received;
}

} // namespace

// -- Main --

int run(int argc, char* argv[]) {
    // Defaults
    std::string multicastIP = "239.1.1.2";
    uint16_t    port        = 5004;
    uint16_t    channels    = 8;
    std::string encoding    = "L24";
    int         duration    = 10;    // seconds (0 = infinite)
    std::string interfaceIP;         // empty = let the system choose

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ip" && i + 1 < argc)           multicastIP = argv[++i];
        else if (arg == "--port" && i + 1 < argc)     port = static_cast<uint16_t>(atoi(argv[++i]));
        else if (arg == "--channels" && i + 1 < argc)  channels = static_cast<uint16_t>(atoi(argv[++i]));
        else if (arg == "--encoding" && i + 1 < argc)  encoding = argv[++i];
        else if (arg == "--duration" && i + 1 < argc)  duration = atoi(argv[++i]);
        else if (arg == "--interface" && i + 1 < argc) interfaceIP = argv[++i];
        else if (arg == "--help" || arg == "-h") {
            fprintf(stderr,
                "AES67 Test Receiver - receives RTP multicast and reports statistics\n\n"
                "Usage: %s [options]\n\n"
                "Options:\n"
                "  --ip <addr>       Multicast IP (default: 239.1.1.2)\n"
                "  --port <port>     RTP port (default: 5004)\n"
                "  --channels <n>    Expected channels (default: 8)\n"
                "  --encoding <enc>  L16 or L24 (default: L24)\n"
                "  --duration <sec>  Duration in seconds (default: 10, 0 = infinite)\n"
                "  --interface <ip>  Local interface IP to join on (default: system)\n",
                argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s (use --help)\n", arg.c_str());
            return 1;
        }
    }

    // Validate
    if (channels == 0 || channels > 128) {
        fprintf(stderr, "Error: channels must be 1-128\n");
        return 1;
    }
    if (encoding != "L16" && encoding != "L24") {
        fprintf(stderr, "Error: encoding must be L16 or L24\n");
        return 1;
    }

    size_t bytesPerSample = (encoding == "L16") ? 2 : 3;

    fprintf(stderr, "AES67 Test Receiver\n");
    fprintf(stderr, "  Multicast: %s:%u\n", multicastIP.c_str(), port);
    fprintf(stderr, "  Expected:  %s, %u channels\n", encoding.c_str(), channels);
    fprintf(stderr, "  Interface: %s\n", interfaceIP.empty() ? "system default" : interfaceIP.c_str());
    fprintf(stderr, "  Duration:  %s\n", duration > 0 ? (std::to_string(duration) + "s").c_str() : "infinite");
    fprintf(stderr, "\nPress Ctrl+C to stop.\n\n");

    // Install signal handler
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Open RTP receive socket
    AES67::RTP::RTPSocket rtpSocket;
    if (!rtpSocket.openReceiver(multicastIP.c_str(), port,
                                interfaceIP.empty() ? nullptr : interfaceIP.c_str())) {
        fprintf(stderr, "Error: failed to open RTP receiver on %s:%u\n",
                multicastIP.c_str(), port);
        return 1;
    }

    fprintf(stderr, "Listening for RTP packets on %s:%u...\n\n", multicastIP.c_str(), port);

    // Receive buffer (max RTP packet: 12-byte header + payload)
    // AES67 max: 128ch * 3 bytes/sample * 48 samples/packet = 18432 bytes + 12 header
    constexpr size_t kMaxPacketSize = 20000;
    std::vector<uint8_t> recvBuffer(kMaxPacketSize);

    // Decode buffer for audio analysis
    // Max samples per packet: 128ch * 48 frames = 6144 samples
    constexpr size_t kMaxSamplesPerPacket = size_t{128} * 48;
    std::vector<float> decodeBuffer(kMaxSamplesPerPacket);

    // Statistics
    uint64_t packetCount = 0;
    uint64_t totalPayloadBytes = 0;
    std::map<uint32_t, SourceStats> sources;   // keyed by SSRC
    double peakLevel = 0.0;
    uint64_t nonZeroSamples = 0;
    uint64_t totalSamples = 0;

    auto startTime = std::chrono::steady_clock::now();
    auto lastReportTime = startTime;

    // Use poll() for non-blocking receive with timeout
    struct pollfd pfd;
    pfd.fd = rtpSocket.getFd();
    pfd.events = POLLIN;

    while (g_running) {
        // Check duration
        if (duration > 0) {
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            if (elapsed >= std::chrono::seconds(duration)) {
                break;
            }
        }

        // Poll with 100ms timeout so we can check g_running
        int pollResult = poll(&pfd, 1, 100);
        if (pollResult <= 0) {
            continue;  // Timeout or error
        }

        // Receive packet
        AES67::RTP::RTPPacket packet;
        ssize_t bytesReceived = rtpSocket.receive(packet, recvBuffer.data(), kMaxPacketSize);
        if (bytesReceived <= 0) {
            continue;
        }

        ++packetCount;
        totalPayloadBytes += packet.payloadSize;

        // Account for this packet against its own source. Two senders on one
        // group are two sources, not one stream full of holes.
        const uint32_t ssrc   = packet.header.ssrc;
        const uint16_t seqNum = packet.header.sequenceNumber;
        auto entry = sources.find(ssrc);
        if (entry == sources.end()) {
            SourceStats fresh;
            fresh.initSeq(seqNum);
            fresh.payloadType = packet.header.payloadType;
            fresh.payloadTypesSeen.insert(packet.header.payloadType);
            fresh.payloadSize = packet.payloadSize;
            entry = sources.emplace(ssrc, std::move(fresh)).first;
            ++entry->second.received;
            entry->second.remember(seqNum);
        } else {
            updateSeq(entry->second, seqNum);
        }
        SourceStats& source = entry->second;
        source.payloadBytes += packet.payloadSize;

        // What the stream says it is, and whether that has changed under us.
        if (packet.header.payloadType != source.payloadType) {
            ++source.payloadTypeChanges;
            source.payloadType = packet.header.payloadType;
        }
        source.payloadTypesSeen.insert(packet.header.payloadType);

        if (packet.payloadSize != source.payloadSize) {
            ++source.payloadSizeChanges;
            source.payloadSize = packet.payloadSize;
        }

        // The timestamp step between consecutive packets is the frame count
        // per packet: it doubles when the sender switches 48 kHz to 96 kHz at
        // the same packet time, which nothing else in the packet reveals.
        if (source.havePrev && seqNum == static_cast<uint16_t>(source.prevSeq + 1)) {
            const uint32_t step = packet.header.timestamp - source.prevTimestamp;
            if (source.timestampStep == 0) {
                source.timestampStep = step;
            } else if (step != source.timestampStep) {
                ++source.timestampStepChanges;
                source.timestampStep = step;
            }
            source.timestampStepsSeen.insert(step);
        }
        source.prevSeq = seqNum;
        source.prevTimestamp = packet.header.timestamp;
        source.havePrev = true;

        // Decode and analyze audio levels
        size_t numSamples = packet.payloadSize / bytesPerSample;
        if (numSamples > 0 && numSamples <= kMaxSamplesPerPacket) {
            if (encoding == "L16") {
                AES67::RTP::L16Codec::decode(packet.payload, packet.payloadSize,
                                              decodeBuffer.data());
            } else {
                AES67::RTP::L24Codec::decode(packet.payload, packet.payloadSize,
                                              decodeBuffer.data());
            }

            for (size_t i = 0; i < numSamples; ++i) {
                double absVal = fabs(decodeBuffer[i]);
                if (absVal > 0.0001) ++nonZeroSamples;
                if (absVal > peakLevel) peakLevel = absVal;
            }
            totalSamples += numSamples;
        }

        // Report every second
        auto now = std::chrono::steady_clock::now();
        auto sinceReport = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReportTime);
        if (sinceReport.count() >= 1000) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
            double pctNonZero = totalSamples > 0 ? (100.0 * nonZeroSamples / totalSamples) : 0.0;
            int64_t lostSoFar = 0;
            uint64_t reorderedSoFar = 0, duplicateSoFar = 0;
            for (const auto& pair : sources) {
                lostSoFar      += pair.second.lost();
                reorderedSoFar += pair.second.outOfOrder;
                duplicateSoFar += pair.second.duplicates;
            }
            fprintf(stderr, "\r  [%llds] pkts=%llu  src=%zu  lost=%lld  reorder=%llu  "
                    "dup=%llu  non-zero=%.1f%%  peak=%.4f",
                    elapsed, packetCount, sources.size(),
                    static_cast<long long>(lostSoFar), reorderedSoFar, duplicateSoFar,
                    pctNonZero, peakLevel);
            fflush(stderr);
            lastReportTime = now;
        }
    }

    // Final report
    auto elapsed = std::chrono::steady_clock::now() - startTime;
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    fprintf(stderr, "\n\n========================================\n");
    fprintf(stderr, "AES67 Test Receiver - Results\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "  Duration:       %.1f seconds\n", elapsedMs / 1000.0);
    fprintf(stderr, "  Packets:        %llu\n", packetCount);
    fprintf(stderr, "  Payload bytes:  %llu\n", totalPayloadBytes);
    fprintf(stderr, "  Sources:        %zu\n", sources.size());

    if (packetCount > 0) {
        double avgPktRate = (packetCount * 1000.0) / elapsedMs;
        fprintf(stderr, "  Packet rate:    %.1f pkt/s\n", avgPktRate);
    }

    const uint8_t expectedPayloadType = (encoding == "L16") ? AES67::RTP::PT_AES67_L16
                                                            : AES67::RTP::PT_AES67_L24;
    uint64_t totalReordered = 0, totalDuplicates = 0, totalRestarts = 0;
    uint64_t totalFormatChanges = 0, totalWrongPayloadType = 0;
    int64_t  totalLost = 0;

    for (const auto& pair : sources) {
        const SourceStats& source = pair.second;
        const double lossPercent = source.expected() > 0
            ? (100.0 * static_cast<double>(source.lost()) / static_cast<double>(source.expected()))
            : 0.0;

        fprintf(stderr, "\n  Source 0x%08X\n", pair.first);
        fprintf(stderr, "    Expected:     %llu\n", source.expected());
        fprintf(stderr, "    Received:     %llu\n", source.received);
        fprintf(stderr, "    Lost:         %lld (%.3f%%)\n",
                static_cast<long long>(source.lost()), lossPercent);
        fprintf(stderr, "    Reordered:    %llu\n", source.outOfOrder);
        fprintf(stderr, "    Duplicated:   %llu\n", source.duplicates);
        fprintf(stderr, "    Seq restarts: %llu\n", source.restarts);

        fprintf(stderr, "    Payload type: %u", source.payloadType);
        if (source.payloadTypesSeen.size() > 1) {
            fprintf(stderr, " (changed %llu times, seen:", source.payloadTypeChanges);
            for (uint8_t seenType : source.payloadTypesSeen) fprintf(stderr, " %u", seenType);
            fprintf(stderr, ")");
        }
        if (source.payloadTypesSeen.count(expectedPayloadType) == 0) {
            fprintf(stderr, "  [expected %u for %s]", expectedPayloadType, encoding.c_str());
            ++totalWrongPayloadType;
        }
        fprintf(stderr, "\n");

        fprintf(stderr, "    Payload size: %zu bytes", source.payloadSize);
        if (source.payloadSizeChanges > 0) {
            fprintf(stderr, " (changed %llu times)", source.payloadSizeChanges);
        }
        fprintf(stderr, "\n");

        fprintf(stderr, "    Frames/pkt:   %u", source.timestampStep);
        if (source.timestampStepsSeen.size() > 1) {
            fprintf(stderr, " (changed %llu times, seen:", source.timestampStepChanges);
            for (uint32_t step : source.timestampStepsSeen) fprintf(stderr, " %u", step);
            fprintf(stderr, ")");
        }
        fprintf(stderr, "\n");

        totalLost       += source.lost();
        totalReordered  += source.outOfOrder;
        totalDuplicates += source.duplicates;
        totalRestarts   += source.restarts;
        totalFormatChanges += source.payloadTypeChanges + source.payloadSizeChanges +
                              source.timestampStepChanges;
    }

    fprintf(stderr, "\n  Lost total:     %lld\n", static_cast<long long>(totalLost));
    fprintf(stderr, "  Reordered:      %llu\n", totalReordered);
    fprintf(stderr, "  Duplicated:     %llu\n", totalDuplicates);
    fprintf(stderr, "  Total samples:  %llu\n", totalSamples);
    if (totalSamples > 0) {
        double pctNonZero = 100.0 * nonZeroSamples / totalSamples;
        fprintf(stderr, "  Non-zero:       %llu (%.1f%%)\n", nonZeroSamples, pctNonZero);
        if (peakLevel > 0.0) {
            fprintf(stderr, "  Peak level:     %.4f (%.1f dBFS)\n", peakLevel, 20.0 * log10(peakLevel));
        } else {
            fprintf(stderr, "  Peak level:     0.0000 (silence)\n");
        }
    }

    if (packetCount == 0) {
        fprintf(stderr, "\n  NO PACKETS RECEIVED\n");
        fprintf(stderr, "  Check: multicast routing, firewall, sender is running\n");
    } else if (nonZeroSamples == 0) {
        fprintf(stderr, "\n  PACKETS RECEIVED BUT ALL SILENCE\n");
        fprintf(stderr, "  Check: audio is routed to driver output channels 9-16\n");
    } else {
        fprintf(stderr, "\n  AUDIO DETECTED!\n");
    }

    // These are the things that used to pass unnoticed: a second sender on the
    // group, and a stream that changes what it is without changing where it is.
    if (sources.size() > 1) {
        fprintf(stderr, "  MULTIPLE SOURCES ON THIS GROUP (%zu SSRCs)\n", sources.size());
    }
    if (totalFormatChanges > 0) {
        fprintf(stderr, "  STREAM FORMAT CHANGED MID-FLIGHT (%llu changes)\n", totalFormatChanges);
    }
    if (totalWrongPayloadType > 0) {
        fprintf(stderr, "  PAYLOAD TYPE NEVER MATCHED --encoding %s\n", encoding.c_str());
    }
    if (totalRestarts > 0) {
        fprintf(stderr, "  SEQUENCE RESTARTED (%llu times)\n", totalRestarts);
    }
    fprintf(stderr, "========================================\n");

    rtpSocket.close();

    return (packetCount > 0 && nonZeroSamples > 0) ? 0 : 1;
}

// main only guards run(): a tool that dies on an uncaught exception prints
// "libc++abi: terminating" and nothing about what it was doing.
int main(int argc, char* argv[]) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
