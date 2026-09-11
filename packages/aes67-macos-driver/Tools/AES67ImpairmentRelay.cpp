//
// AES67ImpairmentRelay.cpp
// Standalone CLI tool: relays an RTP multicast stream from one group to
// another while degrading it — loss, loss bursts, fixed delay, jitter and
// reordering — so a receiver can be stressed without touching the kernel's
// network stack.
//
// This exists because the usual way of degrading a live stream on macOS
// (dnctl/pfctl dummynet pipes) needs root and changes the whole machine's
// packet filter state. The relay does the same thing in user space for one
// stream only: it joins the input group, applies the impairment policy to
// every datagram it receives, and forwards the bytes verbatim to the output
// group. Payload is never parsed, so malformed or unknown packets are
// relayed exactly as they arrived.
//
// Usage:
//   ./AES67ImpairmentRelay [options]
//
// Options:
//   --in-ip <addr>      Input multicast IP (default: 239.69.0.1)
//   --in-port <port>    Input RTP port (default: 5004)
//   --out-ip <addr>     Output multicast IP (default: 239.69.0.2)
//   --out-port <port>   Output RTP port (default: 5006)
//   --interface <ip>    Local interface IP for both groups (default: system)
//   --loss <pct>        Uniform packet loss, 0-100 (default: 0)
//   --burst-ms <ms>     Length of a total-blackout burst (default: 0 = off)
//   --burst-every <ms>  Period between burst starts (default: 10000)
//   --delay-ms <ms>     Fixed extra delay applied to every packet (default: 0)
//   --jitter-ms <ms>    Uniform +/- jitter added to the delay (default: 0)
//   --reorder <pct>     Extra packets given a one-packet-time head start (default: 0)
//   --seed <n>          Random seed, for a repeatable run (default: 1)
//   --duration <sec>    Duration in seconds (default: 0 = infinite)
//

#include <exception>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <string>
#include <vector>
#include <queue>
#include <random>
#include <atomic>
#include <chrono>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>

#include "NetworkEngine/RTP/SimpleRTP.h"

// -- Globals --
static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

namespace {

using Clock = std::chrono::steady_clock;

// One datagram waiting for its due time. The bytes are copied because the
// receive buffer is reused for the next packet while this one is still queued.
struct PendingPacket {
    Clock::time_point dueTime;
    uint64_t          sequence;   // arrival order, to break due-time ties
    std::vector<uint8_t> bytes;
};

// Earliest due time first; equal due times keep arrival order, so a packet is
// only reordered when the policy actually moved its due time.
struct DueTimeGreater {
    bool operator()(const PendingPacket& a, const PendingPacket& b) const {
        if (a.dueTime != b.dueTime) return a.dueTime > b.dueTime;
        return a.sequence > b.sequence;
    }
};

// Transmit socket for the output group. Kept separate from RTPSocket because
// the relay forwards raw datagrams rather than re-encoding RTP packets.
int openOutputSocket(const std::string& outIP, uint16_t outPort,
                     const std::string& interfaceIP, struct sockaddr_in& outAddr) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "Error: could not create output socket (errno=%d: %s)\n",
                errno, strerror(errno));
        return -1;
    }

    uint8_t ttl = 32;
    if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        fprintf(stderr, "Warning: IP_MULTICAST_TTL failed (errno=%d)\n", errno);
    }

    // Same 4 MB send buffer RTPSocket::openTransmitter() uses. The default
    // (9216 bytes on macOS) is smaller than a high-channel-count packet, and
    // sendto() then fails with EMSGSIZE on every single datagram.
    int sndbuf = 4 * 1024 * 1024;
    (void)setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    if (!interfaceIP.empty()) {
        struct in_addr ifaceAddr;
        memset(&ifaceAddr, 0, sizeof(ifaceAddr));
        ifaceAddr.s_addr = inet_addr(interfaceIP.c_str());
        if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_IF, &ifaceAddr, sizeof(ifaceAddr)) < 0) {
            fprintf(stderr, "Error: IP_MULTICAST_IF %s failed (errno=%d: %s)\n",
                    interfaceIP.c_str(), errno, strerror(errno));
            close(sockfd);
            return -1;
        }
    }

    memset(&outAddr, 0, sizeof(outAddr));
    outAddr.sin_family = AF_INET;
    outAddr.sin_addr.s_addr = inet_addr(outIP.c_str());
    outAddr.sin_port = htons(outPort);
    return sockfd;
}

} // namespace

// -- Main --

int run(int argc, char* argv[]) {
    std::string inIP        = "239.69.0.1";
    uint16_t    inPort      = 5004;
    std::string outIP       = "239.69.0.2";
    uint16_t    outPort     = 5006;
    std::string interfaceIP;
    double      lossPercent = 0.0;
    int         burstMs     = 0;
    int         burstEveryMs = 10000;
    double      delayMs     = 0.0;
    double      jitterMs    = 0.0;
    double      reorderPercent = 0.0;
    unsigned    seed        = 1;
    int         duration    = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--in-ip" && i + 1 < argc)             inIP = argv[++i];
        else if (arg == "--in-port" && i + 1 < argc)      inPort = static_cast<uint16_t>(atoi(argv[++i]));
        else if (arg == "--out-ip" && i + 1 < argc)       outIP = argv[++i];
        else if (arg == "--out-port" && i + 1 < argc)     outPort = static_cast<uint16_t>(atoi(argv[++i]));
        else if (arg == "--interface" && i + 1 < argc)    interfaceIP = argv[++i];
        else if (arg == "--loss" && i + 1 < argc)         lossPercent = atof(argv[++i]);
        else if (arg == "--burst-ms" && i + 1 < argc)     burstMs = atoi(argv[++i]);
        else if (arg == "--burst-every" && i + 1 < argc)  burstEveryMs = atoi(argv[++i]);
        else if (arg == "--delay-ms" && i + 1 < argc)     delayMs = atof(argv[++i]);
        else if (arg == "--jitter-ms" && i + 1 < argc)    jitterMs = atof(argv[++i]);
        else if (arg == "--reorder" && i + 1 < argc)      reorderPercent = atof(argv[++i]);
        else if (arg == "--seed" && i + 1 < argc)         seed = static_cast<unsigned>(atoi(argv[++i]));
        else if (arg == "--duration" && i + 1 < argc)     duration = atoi(argv[++i]);
        else if (arg == "--help" || arg == "-h") {
            fprintf(stderr,
                "AES67 Impairment Relay - degrades a live RTP multicast stream in user space\n\n"
                "Usage: %s [options]\n\n"
                "Options:\n"
                "  --in-ip <addr>      Input multicast IP (default: 239.69.0.1)\n"
                "  --in-port <port>    Input RTP port (default: 5004)\n"
                "  --out-ip <addr>     Output multicast IP (default: 239.69.0.2)\n"
                "  --out-port <port>   Output RTP port (default: 5006)\n"
                "  --interface <ip>    Local interface IP for both groups (default: system)\n"
                "  --loss <pct>        Uniform packet loss, 0-100 (default: 0)\n"
                "  --burst-ms <ms>     Length of a total-blackout burst (default: 0 = off)\n"
                "  --burst-every <ms>  Period between burst starts (default: 10000)\n"
                "  --delay-ms <ms>     Fixed extra delay on every packet (default: 0)\n"
                "  --jitter-ms <ms>    Uniform +/- jitter added to the delay (default: 0)\n"
                "  --reorder <pct>     Packets given a one-packet-time head start (default: 0)\n"
                "  --seed <n>          Random seed, for a repeatable run (default: 1)\n"
                "  --duration <sec>    Duration in seconds (default: 0 = infinite)\n",
                argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s (use --help)\n", arg.c_str());
            return 1;
        }
    }

    if (lossPercent < 0.0 || lossPercent > 100.0) {
        fprintf(stderr, "Error: --loss must be 0-100\n");
        return 1;
    }
    if (reorderPercent < 0.0 || reorderPercent > 100.0) {
        fprintf(stderr, "Error: --reorder must be 0-100\n");
        return 1;
    }
    if (jitterMs > delayMs) {
        // Negative jitter cannot pull a packet earlier than its arrival: the
        // relay has no time machine. Say so rather than silently clamping.
        fprintf(stderr, "Note: --jitter-ms %.1f exceeds --delay-ms %.1f; "
                "early packets are clamped to immediate send\n", jitterMs, delayMs);
    }
    if (burstMs > 0 && burstEveryMs <= burstMs) {
        fprintf(stderr, "Error: --burst-every must be greater than --burst-ms\n");
        return 1;
    }

    fprintf(stderr, "AES67 Impairment Relay\n");
    fprintf(stderr, "  In:        %s:%u\n", inIP.c_str(), inPort);
    fprintf(stderr, "  Out:       %s:%u\n", outIP.c_str(), outPort);
    fprintf(stderr, "  Interface: %s\n", interfaceIP.empty() ? "system default" : interfaceIP.c_str());
    fprintf(stderr, "  Loss:      %.2f%%\n", lossPercent);
    if (burstMs > 0) {
        fprintf(stderr, "  Burst:     %d ms blackout every %d ms\n", burstMs, burstEveryMs);
    }
    fprintf(stderr, "  Delay:     %.1f ms +/- %.1f ms jitter\n", delayMs, jitterMs);
    fprintf(stderr, "  Reorder:   %.2f%%\n", reorderPercent);
    fprintf(stderr, "  Duration:  %s\n",
            duration > 0 ? (std::to_string(duration) + "s").c_str() : "infinite");
    fprintf(stderr, "\nPress Ctrl+C to stop.\n\n");

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    AES67::RTP::RTPSocket inSocket;
    if (!inSocket.openReceiver(inIP.c_str(), inPort,
                               interfaceIP.empty() ? nullptr : interfaceIP.c_str())) {
        fprintf(stderr, "Error: failed to open input socket on %s:%u\n", inIP.c_str(), inPort);
        return 1;
    }

    struct sockaddr_in outAddr;
    int outFd = openOutputSocket(outIP, outPort, interfaceIP, outAddr);
    if (outFd < 0) {
        return 1;
    }

    // Max AES67 packet: 128ch * 3 bytes * 48 frames + 12-byte header, rounded up.
    constexpr size_t kMaxPacketSize = 20000;
    std::vector<uint8_t> recvBuffer(kMaxPacketSize);

    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> unit(0.0, 100.0);
    std::uniform_real_distribution<double> jitterDist(-jitterMs, jitterMs);

    std::priority_queue<PendingPacket, std::vector<PendingPacket>, DueTimeGreater> pending;

    uint64_t received = 0, droppedUniform = 0, droppedBurst = 0, forwarded = 0;
    uint64_t reordered = 0, sendFailures = 0;

    const auto startTime = Clock::now();
    auto lastReportTime = startTime;

    struct pollfd pfd;
    pfd.fd = inSocket.getFd();
    pfd.events = POLLIN;

    while (g_running) {
        if (duration > 0 && Clock::now() - startTime >= std::chrono::seconds(duration)) {
            break;
        }

        // Wait no longer than the next queued packet's due time, so a delayed
        // packet is not held back by a quiet input.
        int timeoutMs = 100;
        if (!pending.empty()) {
            auto waitFor = std::chrono::duration_cast<std::chrono::milliseconds>(
                pending.top().dueTime - Clock::now()).count();
            if (waitFor < 0) waitFor = 0;
            if (waitFor < timeoutMs) timeoutMs = static_cast<int>(waitFor);
        }

        int ready = poll(&pfd, 1, timeoutMs);
        if (ready > 0 && (pfd.revents & POLLIN) != 0) {
            ssize_t bytes = recv(inSocket.getFd(), recvBuffer.data(), recvBuffer.size(), 0);
            if (bytes > 0) {
                ++received;

                // Burst blackout wins over uniform loss: inside the window
                // nothing gets through at all.
                bool inBurst = false;
                if (burstMs > 0) {
                    auto sinceStart = std::chrono::duration_cast<std::chrono::milliseconds>(
                        Clock::now() - startTime).count();
                    inBurst = (sinceStart % burstEveryMs) < burstMs;
                }

                if (inBurst) {
                    ++droppedBurst;
                } else if (lossPercent > 0.0 && unit(rng) < lossPercent) {
                    ++droppedUniform;
                } else {
                    double offsetMs = delayMs;
                    if (jitterMs > 0.0) offsetMs += jitterDist(rng);
                    // A "reordered" packet is sent one packet time (1 ms)
                    // earlier than its neighbours, which puts it ahead of the
                    // packet before it whenever the delay is at least that.
                    if (reorderPercent > 0.0 && unit(rng) < reorderPercent) {
                        offsetMs -= 1.0;
                        ++reordered;
                    }
                    if (offsetMs < 0.0) offsetMs = 0.0;

                    PendingPacket packet;
                    packet.dueTime = Clock::now() +
                        std::chrono::microseconds(static_cast<long long>(offsetMs * 1000.0));
                    packet.sequence = received;
                    packet.bytes.assign(recvBuffer.begin(), recvBuffer.begin() + bytes);
                    pending.push(std::move(packet));
                }
            }
        }

        // Send everything now due.
        auto now = Clock::now();
        while (!pending.empty() && pending.top().dueTime <= now) {
            const PendingPacket& packet = pending.top();
            ssize_t sent = sendto(outFd, packet.bytes.data(), packet.bytes.size(), 0,
                                  reinterpret_cast<struct sockaddr*>(&outAddr), sizeof(outAddr));
            if (sent < 0) {
                ++sendFailures;
                if (sendFailures == 1) {
                    fprintf(stderr, "Relay: send failed (errno=%d: %s)\n", errno, strerror(errno));
                }
            } else {
                ++forwarded;
            }
            pending.pop();
        }

        if (now - lastReportTime >= std::chrono::seconds(1)) {
            fprintf(stderr, "rx=%llu fwd=%llu drop_uniform=%llu drop_burst=%llu "
                    "reorder=%llu queued=%zu\n",
                    static_cast<unsigned long long>(received),
                    static_cast<unsigned long long>(forwarded),
                    static_cast<unsigned long long>(droppedUniform),
                    static_cast<unsigned long long>(droppedBurst),
                    static_cast<unsigned long long>(reordered),
                    pending.size());
            lastReportTime = now;
        }
    }

    // Anything still queued was never delivered; count it so the totals add up.
    const size_t abandoned = pending.size();

    fprintf(stderr, "\nRelay summary\n");
    fprintf(stderr, "  Received:       %llu\n", static_cast<unsigned long long>(received));
    fprintf(stderr, "  Forwarded:      %llu\n", static_cast<unsigned long long>(forwarded));
    fprintf(stderr, "  Dropped (loss): %llu\n", static_cast<unsigned long long>(droppedUniform));
    fprintf(stderr, "  Dropped (burst):%llu\n", static_cast<unsigned long long>(droppedBurst));
    fprintf(stderr, "  Reordered:      %llu\n", static_cast<unsigned long long>(reordered));
    fprintf(stderr, "  Send failures:  %llu\n", static_cast<unsigned long long>(sendFailures));
    fprintf(stderr, "  Abandoned:      %zu\n", abandoned);

    close(outFd);
    inSocket.close();
    return 0;
}

// main only guards run(): a tool that dies on an uncaught exception prints
// "libc++abi: terminating" and nothing about what it was doing.
int main(int argc, char* argv[]) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
