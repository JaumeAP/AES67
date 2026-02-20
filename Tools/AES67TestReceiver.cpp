//
// AES67TestReceiver.cpp
// Standalone CLI tool: receives RTP multicast packets and reports statistics
// for verifying the driver's TX path (Core Audio -> Network).
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
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <csignal>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>

// Use the project's RTP header for consistency
#include "../NetworkEngine/RTP/SimpleRTP.h"

// -- Globals --
static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

// -- Main --

int main(int argc, char* argv[]) {
    // Defaults
    std::string multicastIP = "239.1.1.2";
    uint16_t    port        = 5004;
    uint16_t    channels    = 8;
    std::string encoding    = "L24";
    int         duration    = 10;    // seconds (0 = infinite)

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ip" && i + 1 < argc)           multicastIP = argv[++i];
        else if (arg == "--port" && i + 1 < argc)     port = static_cast<uint16_t>(atoi(argv[++i]));
        else if (arg == "--channels" && i + 1 < argc)  channels = static_cast<uint16_t>(atoi(argv[++i]));
        else if (arg == "--encoding" && i + 1 < argc)  encoding = argv[++i];
        else if (arg == "--duration" && i + 1 < argc)  duration = atoi(argv[++i]);
        else if (arg == "--help" || arg == "-h") {
            fprintf(stderr,
                "AES67 Test Receiver - receives RTP multicast and reports statistics\n\n"
                "Usage: %s [options]\n\n"
                "Options:\n"
                "  --ip <addr>       Multicast IP (default: 239.1.1.2)\n"
                "  --port <port>     RTP port (default: 5004)\n"
                "  --channels <n>    Expected channels (default: 8)\n"
                "  --encoding <enc>  L16 or L24 (default: L24)\n"
                "  --duration <sec>  Duration in seconds (default: 10, 0 = infinite)\n",
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
    fprintf(stderr, "  Duration:  %s\n", duration > 0 ? (std::to_string(duration) + "s").c_str() : "infinite");
    fprintf(stderr, "\nPress Ctrl+C to stop.\n\n");

    // Install signal handler
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Open RTP receive socket
    AES67::RTP::RTPSocket rtpSocket;
    if (!rtpSocket.openReceiver(multicastIP.c_str(), port)) {
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
    constexpr size_t kMaxSamplesPerPacket = 128 * 48;
    std::vector<float> decodeBuffer(kMaxSamplesPerPacket);

    // Statistics
    uint64_t packetCount = 0;
    uint64_t totalPayloadBytes = 0;
    uint16_t lastSeqNum = 0;
    uint64_t seqGaps = 0;
    uint64_t seqDups = 0;
    bool firstPacket = true;
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

        // Check sequence continuity
        uint16_t seqNum = packet.header.sequenceNumber;
        if (!firstPacket) {
            uint16_t expected = static_cast<uint16_t>(lastSeqNum + 1);
            if (seqNum != expected) {
                int16_t diff = static_cast<int16_t>(seqNum - expected);
                if (diff > 0) {
                    seqGaps += diff;
                } else {
                    ++seqDups;
                }
            }
        }
        lastSeqNum = seqNum;
        firstPacket = false;

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
            fprintf(stderr, "\r  [%llds] pkts=%llu  gaps=%llu  dups=%llu  bytes=%llu  "
                    "non-zero=%.1f%%  peak=%.4f",
                    elapsed, packetCount, seqGaps, seqDups, totalPayloadBytes,
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
    fprintf(stderr, "  Seq gaps:       %llu\n", seqGaps);
    fprintf(stderr, "  Seq duplicates: %llu\n", seqDups);

    if (packetCount > 0) {
        double avgPktRate = (packetCount * 1000.0) / elapsedMs;
        fprintf(stderr, "  Packet rate:    %.1f pkt/s\n", avgPktRate);
    }

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
    fprintf(stderr, "========================================\n");

    rtpSocket.close();

    return (packetCount > 0 && nonZeroSamples > 0) ? 0 : 1;
}
