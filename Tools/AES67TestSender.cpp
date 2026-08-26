//
// AES67TestSender.cpp
// Standalone CLI tool: sends a sine-wave RTP multicast stream
// with optional SAP announcements for testing the AES67 driver.
//
// Usage:
//   ./AES67TestSender [options]
//
// Options:
//   --ip <addr>       Multicast IP (default: 239.1.1.1)
//   --port <port>     RTP port (default: 5004)
//   --channels <n>    Number of channels (default: 8)
//   --rate <hz>       Sample rate (default: 48000)
//   --encoding <enc>  L16 or L24 (default: L24)
//   --freq <hz>       Sine wave frequency (default: 1000)
//   --duration <sec>  Duration in seconds (default: 60, 0 = infinite)
//   --no-sap          Disable SAP announcements
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <csignal>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// Use the project's RTP header for consistency
#include "NetworkEngine/RTP/SimpleRTP.h"

// ── Globals ──────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

// ── SAP Announcer ────────────────────────────────────────────────────

static std::string buildSDP(const std::string& multicastIP, uint16_t port,
                            uint16_t channels, uint32_t sampleRate,
                            const std::string& encoding, uint8_t payloadType) {
    std::string sdp;
    sdp += "v=0\r\n";
    sdp += "o=- 1 1 IN IP4 127.0.0.1\r\n";
    sdp += "s=AES67 Test Stream\r\n";
    sdp += "c=IN IP4 " + multicastIP + "/32\r\n";
    sdp += "t=0 0\r\n";
    sdp += "m=audio " + std::to_string(port) + " RTP/AVP " +
           std::to_string(payloadType) + "\r\n";
    sdp += "a=rtpmap:" + std::to_string(payloadType) + " " + encoding + "/" +
           std::to_string(sampleRate) + "/" + std::to_string(channels) + "\r\n";
    sdp += "a=ptime:1\r\n";
    sdp += "a=recvonly\r\n";
    return sdp;
}

static std::vector<uint8_t> buildSAPPacket(const std::string& sdp) {
    // SAP header (RFC 2974):
    // Byte 0: V=1 (bits 5-7), A=0, R=0, T=0(announce), E=0, C=0 → 0x20
    // Byte 1: Auth length = 0
    // Bytes 2-3: Message ID hash (arbitrary)
    // Bytes 4-7: Originating source (127.0.0.1)
    // Then: optional "application/sdp\0" content-type, then SDP payload

    std::vector<uint8_t> pkt;
    pkt.push_back(0x20);  // V=1, all other bits 0
    pkt.push_back(0x00);  // auth length = 0
    pkt.push_back(0x00);  // msg id hash high
    pkt.push_back(0x01);  // msg id hash low
    // Originating source: 127.0.0.1
    pkt.push_back(127);
    pkt.push_back(0);
    pkt.push_back(0);
    pkt.push_back(1);
    // SDP payload (no content-type header — matches what SAPListener expects)
    for (char c : sdp) {
        pkt.push_back(static_cast<uint8_t>(c));
    }
    return pkt;
}

static void sapAnnounceLoop(const std::string& multicastIP, uint16_t port,
                            uint16_t channels, uint32_t sampleRate,
                            const std::string& encoding, uint8_t payloadType) {
    // Build SAP packet once (SDP is static)
    std::string sdp = buildSDP(multicastIP, port, channels, sampleRate,
                               encoding, payloadType);
    std::vector<uint8_t> sapPacket = buildSAPPacket(sdp);

    // Create UDP socket for SAP
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "Warning: could not create SAP socket\n");
        return;
    }

    // Set multicast TTL
    uint8_t ttl = 32;
    setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    struct sockaddr_in sapAddr;
    memset(&sapAddr, 0, sizeof(sapAddr));
    sapAddr.sin_family = AF_INET;
    sapAddr.sin_addr.s_addr = inet_addr("224.2.127.254");
    sapAddr.sin_port = htons(9875);

    fprintf(stderr, "SAP: announcing on 224.2.127.254:9875 every 30s\n");

    while (g_running) {
        ssize_t sent = sendto(sockfd, sapPacket.data(), sapPacket.size(), 0,
                              (struct sockaddr*)&sapAddr, sizeof(sapAddr));
        if (sent < 0) {
            fprintf(stderr, "SAP: send failed (errno=%d)\n", errno);
        }

        // Sleep 30 seconds in 1-second intervals so we can check g_running
        for (int i = 0; i < 30 && g_running; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    close(sockfd);
}

// ── Main ─────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Defaults
    std::string multicastIP = "239.1.1.1";
    uint16_t    port        = 5004;
    uint16_t    channels    = 8;
    uint32_t    sampleRate  = 48000;
    std::string encoding    = "L24";
    double      freq        = 1000.0;
    int         duration    = 60;    // seconds (0 = infinite)
    bool        enableSAP   = true;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ip" && i + 1 < argc)        multicastIP = argv[++i];
        else if (arg == "--port" && i + 1 < argc)  port = static_cast<uint16_t>(atoi(argv[++i]));
        else if (arg == "--channels" && i + 1 < argc) channels = static_cast<uint16_t>(atoi(argv[++i]));
        else if (arg == "--rate" && i + 1 < argc)  sampleRate = static_cast<uint32_t>(atoi(argv[++i]));
        else if (arg == "--encoding" && i + 1 < argc) encoding = argv[++i];
        else if (arg == "--freq" && i + 1 < argc)  freq = atof(argv[++i]);
        else if (arg == "--duration" && i + 1 < argc) duration = atoi(argv[++i]);
        else if (arg == "--no-sap")                 enableSAP = false;
        else if (arg == "--help" || arg == "-h") {
            fprintf(stderr,
                "AES67 Test Sender - generates a sine wave RTP multicast stream\n\n"
                "Usage: %s [options]\n\n"
                "Options:\n"
                "  --ip <addr>       Multicast IP (default: 239.1.1.1)\n"
                "  --port <port>     RTP port (default: 5004)\n"
                "  --channels <n>    Number of channels (default: 8)\n"
                "  --rate <hz>       Sample rate (default: 48000)\n"
                "  --encoding <enc>  L16 or L24 (default: L24)\n"
                "  --freq <hz>       Sine wave frequency (default: 1000)\n"
                "  --duration <sec>  Duration in seconds (default: 60, 0 = infinite)\n"
                "  --no-sap          Disable SAP announcements\n",
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

    uint8_t payloadType = (encoding == "L16") ? AES67::RTP::PT_AES67_L16
                                               : AES67::RTP::PT_AES67_L24;
    size_t bytesPerSample = (encoding == "L16") ? 2 : 3;
    uint32_t samplesPerPacket = sampleRate / 1000;  // 1ms packets

    fprintf(stderr, "AES67 Test Sender\n");
    fprintf(stderr, "  Multicast: %s:%u\n", multicastIP.c_str(), port);
    fprintf(stderr, "  Format:    %s/%u/%u\n", encoding.c_str(), sampleRate, channels);
    fprintf(stderr, "  Sine:      %.0f Hz\n", freq);
    fprintf(stderr, "  Duration:  %s\n", duration > 0 ? (std::to_string(duration) + "s").c_str() : "infinite");
    fprintf(stderr, "  SAP:       %s\n", enableSAP ? "enabled" : "disabled");
    fprintf(stderr, "  Packet:    %u samples/pkt, %zu bytes/pkt\n",
            samplesPerPacket, samplesPerPacket * channels * bytesPerSample);
    fprintf(stderr, "\nPress Ctrl+C to stop.\n\n");

    // Install signal handler
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Start SAP announcer thread
    std::thread sapThread;
    if (enableSAP) {
        sapThread = std::thread(sapAnnounceLoop, multicastIP, port,
                                channels, sampleRate, encoding, payloadType);
    }

    // Open RTP transmit socket
    AES67::RTP::RTPSocket rtpSocket;
    if (!rtpSocket.openTransmitter(multicastIP.c_str(), port)) {
        fprintf(stderr, "Error: failed to open RTP socket on %s:%u\n",
                multicastIP.c_str(), port);
        g_running = false;
        if (sapThread.joinable()) sapThread.join();
        return 1;
    }

    // Prepare audio and payload buffers
    const size_t totalSamples = samplesPerPacket * channels;
    std::vector<float> audioBuffer(totalSamples);
    std::vector<uint8_t> payloadBuffer(totalSamples * bytesPerSample);

    // RTP state
    uint16_t sequenceNumber = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0x12345678;  // fixed SSRC for test tool

    // Sine wave state
    double phase = 0.0;
    const double phaseIncrement = 2.0 * M_PI * freq / sampleRate;

    // Paced transmit loop (sleep_until pattern)
    auto packetInterval = std::chrono::microseconds(1000); // 1ms
    auto nextTransmitTime = std::chrono::steady_clock::now();
    auto startTime = std::chrono::steady_clock::now();

    uint64_t packetCount = 0;

    while (g_running) {
        // Check duration
        if (duration > 0) {
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            if (elapsed >= std::chrono::seconds(duration)) {
                break;
            }
        }

        std::this_thread::sleep_until(nextTransmitTime);
        nextTransmitTime += packetInterval;

        // Generate sine wave (same tone on all channels)
        for (uint32_t frame = 0; frame < samplesPerPacket; ++frame) {
            float sample = static_cast<float>(sin(phase));
            phase += phaseIncrement;

            // Wrap phase to avoid precision loss over long runs
            if (phase >= 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }

            for (uint16_t ch = 0; ch < channels; ++ch) {
                audioBuffer[frame * channels + ch] = sample;
            }
        }

        // Encode
        if (encoding == "L16") {
            AES67::RTP::L16Codec::encode(audioBuffer.data(), totalSamples,
                                          payloadBuffer.data());
        } else {
            AES67::RTP::L24Codec::encode(audioBuffer.data(), totalSamples,
                                          payloadBuffer.data());
        }

        // Build and send RTP packet
        AES67::RTP::RTPPacket packet;
        packet.header.version = 2;
        packet.header.padding = 0;
        packet.header.extension = 0;
        packet.header.cc = 0;
        packet.header.marker = 0;
        packet.header.payloadType = payloadType;
        packet.header.sequenceNumber = sequenceNumber++;
        packet.header.timestamp = timestamp;
        packet.header.ssrc = ssrc;
        packet.payload = payloadBuffer.data();
        packet.payloadSize = totalSamples * bytesPerSample;

        ssize_t sent = rtpSocket.send(packet);
        if (sent < 0) {
            fprintf(stderr, "RTP send failed at packet %llu\n", packetCount);
        }

        timestamp += samplesPerPacket;
        ++packetCount;

        // Print progress every 1000 packets (every ~1 second)
        if (packetCount % 1000 == 0) {
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            fprintf(stderr, "\r  Sent %llu packets (%llds elapsed)", packetCount, secs);
            fflush(stderr);
        }
    }

    fprintf(stderr, "\n\nStopping... sent %llu packets total.\n", packetCount);

    // Cleanup
    g_running = false;
    rtpSocket.close();
    if (sapThread.joinable()) sapThread.join();

    return 0;
}
