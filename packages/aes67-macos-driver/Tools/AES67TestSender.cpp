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

#include "ToolOptions.h"

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
#include "NetworkEngine/Discovery/SAPAnnouncer.h"

// ── Globals ──────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

// ── SAP Announcer ────────────────────────────────────────────────────

static std::string buildSDP(const std::string& multicastIP, uint16_t port,
                            uint16_t channels, uint32_t sampleRate,
                            const std::string& encoding, uint8_t payloadType,
                            uint32_t ptimeUs, uint32_t sessionId,
                            const std::string& originAddress) {
    std::string sdp;
    sdp += "v=0\r\n";
    // The origin is what identifies a session (RFC 4566 SS 5.2), and a
    // constant one made two senders on different groups merge into a single
    // entry in any directory that keys on it -- including this driver's.
    sdp += "o=- " + std::to_string(sessionId) + " 1 IN IP4 " +
           (originAddress.empty() ? std::string("127.0.0.1") : originAddress) + "\r\n";
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

/// The datagram for one announcement, or for the deletion that withdraws it.
///
/// SAPAnnouncer::buildPacket, the driver's own, rather than a third copy of
/// the RFC 2974 header beside the driver's and the daemon mirror in
/// Tests/support/DaemonSap. The copy that used to be here omitted the
/// "application/sdp" payload type, on the same reasoning the driver's did
/// until it was fixed: the type is optional to write and not optional to be
/// heard, and the AES67 Linux daemon drops a packet whose sixteen bytes at
/// offset 8 are not exactly "application/sdp\0". This tool announces to
/// whatever is on the network, not only to our own SAPListener.
///
/// `originatingSource` is an IPv4 address in network byte order. It used to
/// be a constant here, along with the message id hash, which gave every
/// announcer on the network the same SAP identity: two senders describing two
/// different streams looked to a receiver like one session whose description
/// kept changing, and it could hold only one of them.
static std::vector<uint8_t> buildSAPPacket(const std::string& sdp,
                                           uint32_t originatingSourceNetworkOrder,
                                           bool deletion) {
    return AES67::SAPAnnouncer::buildPacket(sdp, AES67::SAPAnnouncer::messageIdHash(sdp),
                                            originatingSourceNetworkOrder, deletion);
}

static void sapAnnounceLoop(const std::string& multicastIP, uint16_t port,
                            uint16_t channels, uint32_t sampleRate,
                            const std::string& encoding, uint8_t payloadType,
                            const std::string& interfaceIP, uint32_t ptimeUs,
                            uint32_t sessionId) {
    // Build the SDP once; the SAP packet waits until the socket can say which
    // address this announcer is sending from.
    std::string sdp = buildSDP(multicastIP, port, channels, sampleRate,
                               encoding, payloadType, ptimeUs, sessionId, interfaceIP);

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

    // The originating source is this announcer's own address, so ask the
    // kernel which one it will use rather than assuming. connect() on a UDP
    // socket only fixes the destination, which is where every announcement
    // goes anyway.
    // Network byte order throughout: it is what goes in the header and what
    // buildPacket takes. The host-order copy below exists only to print it.
    uint32_t originatingSource = 0;
    const bool connected =
        connect(sockfd, reinterpret_cast<struct sockaddr*>(&sapAddr), sizeof(sapAddr)) == 0;
    if (connected) {
        struct sockaddr_in local;
        socklen_t localLen = sizeof(local);
        if (getsockname(sockfd, reinterpret_cast<struct sockaddr*>(&local), &localLen) == 0) {
            originatingSource = local.sin_addr.s_addr;
        }
    }
    if (originatingSource == 0 && !interfaceIP.empty()) {
        originatingSource = inet_addr(interfaceIP.c_str());
    }

    const std::vector<uint8_t> sapPacket = buildSAPPacket(sdp, originatingSource, false);

    const uint32_t printable = ntohl(originatingSource);
    fprintf(stderr, "SAP: announcing on 224.2.127.254:9875 every 30s"
            " (origin %u.%u.%u.%u)\n",
            (printable >> 24) & 0xFF, (printable >> 16) & 0xFF,
            (printable >> 8) & 0xFF, printable & 0xFF);

    // A connected UDP socket refuses sendto() with a destination (EISCONN on
    // macOS), so the send has to match how the socket was set up above.
    const auto sendPacket = [&](const std::vector<uint8_t>& packet) {
        const ssize_t sent = connected
            ? send(sockfd, packet.data(), packet.size(), 0)
            : sendto(sockfd, packet.data(), packet.size(), 0,
                     reinterpret_cast<struct sockaddr*>(&sapAddr), sizeof(sapAddr));
        if (sent < 0) {
            fprintf(stderr, "SAP: send failed (errno=%d)\n", errno);
        }
    };

    while (g_running) {
        sendPacket(sapPacket);

        // Sleep 30 seconds in 1-second intervals so we can check g_running
        for (int i = 0; i < 30 && g_running; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    // The deletion this announcer owes whoever heard it. Without it the
    // session sits in a receiver's list until the timeout runs out -- 300 s in
    // this driver's own SAPListener, which is most of an afternoon's worth of
    // stale entries after a few runs of a tool whose default duration is 60 s.
    // Same message id hash: RFC 2974 SS 6 identifies the session by it, so the
    // withdrawal has to carry the value the announcement went out with, which
    // is what building it from the same SDP gives.
    sendPacket(buildSAPPacket(sdp, originatingSource, true));

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
        long long number = 0;
        double real = 0.0;
        unsigned long long unsignedNumber = 0;

        if (arg == "--ip") {
            const char* text = AES67::ToolOptions::value(argc, argv, i);
            if (text == nullptr) return 1;
            multicastIP = text;
        }
        else if (arg == "--port") {
            if (!AES67::ToolOptions::integerOption(argc, argv, i, 1, 65535, number)) return 1;
            port = static_cast<uint16_t>(number);
        }
        else if (arg == "--channels") {
            if (!AES67::ToolOptions::integerOption(argc, argv, i, 1, 128, number)) return 1;
            channels = static_cast<uint16_t>(number);
        }
        else if (arg == "--rate") {
            if (!AES67::ToolOptions::integerOption(argc, argv, i, 1, 4294967295LL, number)) return 1;
            sampleRate = static_cast<uint32_t>(number);
        }
        else if (arg == "--encoding") {
            const char* text = AES67::ToolOptions::value(argc, argv, i);
            if (text == nullptr) return 1;
            encoding = text;
        }
        else if (arg == "--freq") {
            // Nyquist is not checked here: the rate may still be parsed
            // after this flag, and a tone above it is a legal thing to ask
            // a generator for when what is being tested is what happens.
            if (!AES67::ToolOptions::realOption(argc, argv, i, 0.0, 1.0e9, real)) return 1;
            freq = real;
        }
        else if (arg == "--duration") {
            if (!AES67::ToolOptions::integerOption(argc, argv, i, 0, 2147483647LL, number)) return 1;
            duration = static_cast<int>(number);
        }
        else if (arg == "--interface") {
            const char* text = AES67::ToolOptions::value(argc, argv, i);
            if (text == nullptr) return 1;
            interfaceIP = text;
        }
        else if (arg == "--ptime-us") {
            // Zero is left to the packet-size check below, which already
            // refuses a packet time that carries no samples and says so with
            // the rate in hand.
            if (!AES67::ToolOptions::integerOption(argc, argv, i, 0, 4294967295LL, number)) return 1;
            ptimeUs = static_cast<uint32_t>(number);
        }
        else if (arg == "--ssrc") {
            if (!AES67::ToolOptions::unsignedOption(argc, argv, i, 4294967295ULL, unsignedNumber)) return 1;
            ssrc = static_cast<uint32_t>(unsignedNumber);
        }
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

    // Validate. The channel count is checked where it is parsed, along with
    // every other number.
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

    // RFC 3550 SS 8: an SSRC is picked at random, so that two senders on the
    // same group are told apart. A fixed one made every instance of this tool
    // collide with every other by construction.
    if (ssrc == 0) {
        std::random_device entropy;
        std::uniform_int_distribution<uint32_t> pick(1, 0xFFFFFFFFu);
        ssrc = pick(entropy);
    }
    fprintf(stderr, "  SSRC:      0x%08X\n\n", ssrc);

    // Chosen before the announcer starts: the SDP's o= session id is this
    // number, and an announcement carrying a zero would be an identity two
    // senders shared -- which is the merge this was meant to stop.

    // Start SAP announcer thread
    std::thread sapThread;
    if (enableSAP) {
        sapThread = std::thread(sapAnnounceLoop, multicastIP, port,
                                channels, sampleRate, encoding, payloadType,
                                interfaceIP, ptimeUs, ssrc);
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

    // RTP state. The sequence number starts somewhere random, RFC 3550 SS 5.1,
    // for the reason the SSRC above does: two runs of this tool on one group
    // are told apart by what is in the packets, and a receiver that keys on
    // the sequence number saw every run start at 0 and read the second one as
    // a catastrophic reordering of the first.
    std::random_device seqEntropy;
    uint16_t sequenceNumber =
        static_cast<uint16_t>(std::uniform_int_distribution<uint32_t>(0, 0xFFFFu)(seqEntropy));
    uint32_t timestamp = 0;

    // Sine wave state
    double phase = 0.0;
    const double phaseIncrement = 2.0 * M_PI * freq / sampleRate;

    // Paced on the frames actually in a packet, not on --ptime-us: at 44.1 kHz
    // and 1 ms a packet holds 44 frames, which last 997.7 us, so pacing at
    // 1000 us sends 0.23% slow -- a stream that claims one rate and runs at
    // another. The remainder is carried, the way RTPTransmitter carries it.
    const AES67::PacketBudget::PacketInterval exactInterval =
        AES67::PacketBudget::packetInterval(samplesPerPacket, sampleRate);
    uint32_t intervalRemainder = 0;
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
        uint64_t stepNs = exactInterval.wholeNs;
        intervalRemainder += exactInterval.remainder;
        if (intervalRemainder >= sampleRate) {
            stepNs += intervalRemainder / sampleRate;
            intervalRemainder %= sampleRate;
        }
        nextTransmitTime += std::chrono::nanoseconds(stepNs);

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
