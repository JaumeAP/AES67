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
//   --ptime-us <us>   Packet time in microseconds (default: 1000)
//   --ssrc <hex>      RTP SSRC (default: random, per RFC 3550)
//   --duration <sec>  Duration in seconds (default: 60, 0 = infinite)
//   --interface <ip>  Local interface IP to send the multicast on (default: system)
//   --no-sap          Disable SAP announcements
//

#include <iterator>
#include <algorithm>
#include <exception>
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
#include <random>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// Use the project's RTP header for consistency
#include "NetworkEngine/RTP/SimpleRTP.h"
#include "NetworkEngine/RTP/PacketBudget.h"

// ── Globals ──────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

// ── SAP Announcer ────────────────────────────────────────────────────

static std::string buildSDP(const std::string& multicastIP, uint16_t port,
                            uint16_t channels, uint32_t sampleRate,
                            const std::string& encoding, uint8_t payloadType,
                            uint32_t ptimeUs) {
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
    // ptime is milliseconds, and 125 us is a legal AES67 packet time, so it
    // has to be written as a fraction rather than truncated to an integer.
    char ptimeText[32];
    snprintf(ptimeText, sizeof(ptimeText), "%g", static_cast<double>(ptimeUs) / 1000.0);
    sdp += "a=ptime:" + std::string(ptimeText) + "\r\n";
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
    std::transform(sdp.begin(), sdp.end(), std::back_inserter(pkt),
                   [](char c) { return static_cast<uint8_t>(c); });
    return pkt;
}

static void sapAnnounceLoop(const std::string& multicastIP, uint16_t port,
                            uint16_t channels, uint32_t sampleRate,
                            const std::string& encoding, uint8_t payloadType,
                            const std::string& interfaceIP, uint32_t ptimeUs) {
    // Build SAP packet once (SDP is static)
    std::string sdp = buildSDP(multicastIP, port, channels, sampleRate,
                               encoding, payloadType, ptimeUs);
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

    // Announce on the same interface the audio goes out on, or the kernel
    // picks the default route and the announcement misses the audio network.
    if (!interfaceIP.empty()) {
        struct in_addr ifaceAddr;
        memset(&ifaceAddr, 0, sizeof(ifaceAddr));
        ifaceAddr.s_addr = inet_addr(interfaceIP.c_str());
        if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_IF, &ifaceAddr, sizeof(ifaceAddr)) < 0) {
            fprintf(stderr, "SAP: IP_MULTICAST_IF %s failed (errno=%d)\n",
                    interfaceIP.c_str(), errno);
        }
    }

    struct sockaddr_in sapAddr;
    memset(&sapAddr, 0, sizeof(sapAddr));
    sapAddr.sin_family = AF_INET;
    sapAddr.sin_addr.s_addr = inet_addr("224.2.127.254");
    sapAddr.sin_port = htons(9875);

    fprintf(stderr, "SAP: announcing on 224.2.127.254:9875 every 30s\n");

    while (g_running) {
        ssize_t sent = sendto(sockfd, sapPacket.data(), sapPacket.size(), 0,
                              reinterpret_cast<struct sockaddr*>(&sapAddr), sizeof(sapAddr));
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

int run(int argc, char* argv[]) {
    // Defaults
    std::string multicastIP = "239.1.1.1";
    uint16_t    port        = 5004;
    uint16_t    channels    = 8;
    uint32_t    sampleRate  = 48000;
    std::string encoding    = "L24";
    double      freq        = 1000.0;
    int         duration    = 60;    // seconds (0 = infinite)
    bool        enableSAP   = true;
    std::string interfaceIP;         // empty = let the system choose
    uint32_t    ptimeUs     = 1000;  // 1 ms, the AES67 default packet time
    uint32_t    ssrc        = 0;     // 0 = pick a random one below

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
        else if (arg == "--interface" && i + 1 < argc) interfaceIP = argv[++i];
        else if (arg == "--ptime-us" && i + 1 < argc) ptimeUs = static_cast<uint32_t>(atoi(argv[++i]));
        else if (arg == "--ssrc" && i + 1 < argc)   ssrc = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 0));
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
                "  --ptime-us <us>   Packet time in microseconds (default: 1000)\n"
                "  --ssrc <hex>      RTP SSRC (default: random, per RFC 3550)\n"
                "  --duration <sec>  Duration in seconds (default: 60, 0 = infinite)\n"
                "  --interface <ip>  Local interface IP to send on (default: system)\n"
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
    size_t bytesPerSample = AES67::PacketBudget::bytesPerSample(encoding);
    uint32_t samplesPerPacket =
        AES67::PacketBudget::framesPerPacket(sampleRate, ptimeUs, 0);

    if (samplesPerPacket == 0) {
        fprintf(stderr, "Error: %u us at %u Hz carries no samples\n", ptimeUs, sampleRate);
        return 1;
    }

    // A packet that does not fit one Ethernet frame is fragmented by IP, and
    // this driver's own receiver drops anything over the frame size. Refusing
    // it here, with the packet time that would fit, beats sending 18 kB
    // datagrams that only survive on loopback.
    if (!AES67::PacketBudget::fits(channels, bytesPerSample, samplesPerPacket)) {
        const size_t packetBytes =
            AES67::PacketBudget::rtpPacketBytes(channels, bytesPerSample, samplesPerPacket);
        const uint32_t maxFrames =
            AES67::PacketBudget::maxFramesPerPacket(channels, bytesPerSample);
        const uint16_t maxChannels =
            AES67::PacketBudget::maxChannelsPerPacket(bytesPerSample, samplesPerPacket);
        fprintf(stderr,
                "Error: %u channels of %s at %u Hz and %u us make a %zu-byte RTP packet, "
                "over the %zu-byte limit.\n",
                channels, encoding.c_str(), sampleRate, ptimeUs, packetBytes,
                AES67::PacketBudget::kMaxRtpPacketBytes);
        if (maxFrames > 0) {
            const uint32_t maxPtimeUs = static_cast<uint32_t>(
                (static_cast<uint64_t>(maxFrames) * 1000000ULL) / sampleRate);
            fprintf(stderr, "       %u channels fit at %u us (%u samples per packet).\n",
                    channels, maxPtimeUs, maxFrames);
        }
        fprintf(stderr, "       %u us fits %u channels.\n", ptimeUs, maxChannels);
        return 1;
    }

    fprintf(stderr, "AES67 Test Sender\n");
    fprintf(stderr, "  Multicast: %s:%u\n", multicastIP.c_str(), port);
    fprintf(stderr, "  Format:    %s/%u/%u\n", encoding.c_str(), sampleRate, channels);
    fprintf(stderr, "  Sine:      %.0f Hz\n", freq);
    fprintf(stderr, "  Duration:  %s\n", duration > 0 ? (std::to_string(duration) + "s").c_str() : "infinite");
    fprintf(stderr, "  Interface: %s\n", interfaceIP.empty() ? "system default" : interfaceIP.c_str());
    fprintf(stderr, "  SAP:       %s\n", enableSAP ? "enabled" : "disabled");
    fprintf(stderr, "  Ptime:     %u us\n", ptimeUs);
    fprintf(stderr, "  Packet:    %u samples/pkt, %zu bytes/pkt\n",
            samplesPerPacket,
            static_cast<size_t>(samplesPerPacket) * channels * bytesPerSample);
    fprintf(stderr, "\nPress Ctrl+C to stop.\n\n");

    // Install signal handler
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Start SAP announcer thread
    std::thread sapThread;
    if (enableSAP) {
        sapThread = std::thread(sapAnnounceLoop, multicastIP, port,
                                channels, sampleRate, encoding, payloadType,
                                interfaceIP, ptimeUs);
    }

    // Open RTP transmit socket
    AES67::RTP::RTPSocket rtpSocket;
    if (!rtpSocket.openTransmitter(multicastIP.c_str(), port,
                                   interfaceIP.empty() ? nullptr : interfaceIP.c_str())) {
        fprintf(stderr, "Error: failed to open RTP socket on %s:%u\n",
                multicastIP.c_str(), port);
        g_running = false;
        if (sapThread.joinable()) sapThread.join();
        return 1;
    }

    // Prepare audio and payload buffers
    const size_t totalSamples = static_cast<size_t>(samplesPerPacket) * channels;
    std::vector<float> audioBuffer(totalSamples);
    std::vector<uint8_t> payloadBuffer(totalSamples * bytesPerSample);

    // RTP state
    uint16_t sequenceNumber = 0;
    uint32_t timestamp = 0;
    // RFC 3550 SS 8: an SSRC is picked at random, so that two senders on the
    // same group are told apart. A fixed one made every instance of this tool
    // collide with every other by construction.
    if (ssrc == 0) {
        std::random_device entropy;
        std::uniform_int_distribution<uint32_t> pick(1, 0xFFFFFFFFu);
        ssrc = pick(entropy);
    }
    fprintf(stderr, "  SSRC:      0x%08X\n\n", ssrc);

    // Sine wave state
    double phase = 0.0;
    const double phaseIncrement = 2.0 * M_PI * freq / sampleRate;

    // Paced transmit loop (sleep_until pattern)
    auto packetInterval = std::chrono::microseconds(ptimeUs);
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
