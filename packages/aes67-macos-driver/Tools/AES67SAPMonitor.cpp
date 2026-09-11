//
// AES67SAPMonitor.cpp
// Standalone CLI tool: listens for SAP announcements (RFC 2974) on
// 224.2.127.254:9875 and prints what each one describes.
//
// The repository could emit announcements -- AES67TestSender's SAP thread,
// ravenna-announce -- but nothing could hear them, so a question like "two
// announcements claim the same multicast address, which one does a receiver
// end up with" had no observable answer. This is the observer.
//
// It uses SAPListener::parseAnnouncement(), the same static parser the driver
// runs on this traffic, rather than a second SAP reader that could disagree
// with it. The socket is its own because SAPListener joins on INADDR_ANY,
// which follows the default route; a test on loopback needs the interface
// named explicitly.
//
// Usage:
//   ./AES67SAPMonitor [options]
//
// Options:
//   --interface <ip>  Local interface IP to join the SAP group on (default: system)
//   --group <addr>    SAP group (default: 224.2.127.254)
//   --port <port>     SAP port (default: 9875)
//   --duration <sec>  Duration in seconds (default: 0 = infinite)
//   --verbose         Print the whole SDP of every announcement
//

#include <exception>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <chrono>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>

#include "NetworkEngine/Discovery/SAPListener.h"

// -- Globals --
static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

namespace {

// One SDP line, for the fields worth showing without --verbose.
std::string sdpLine(const std::string& sdp, const std::string& prefix) {
    size_t at = sdp.find(prefix);
    while (at != std::string::npos) {
        const bool atLineStart = (at == 0) || sdp[at - 1] == '\n';
        if (atLineStart) {
            const size_t end = sdp.find_first_of("\r\n", at);
            return sdp.substr(at, (end == std::string::npos ? sdp.size() : end) - at);
        }
        at = sdp.find(prefix, at + 1);
    }
    return {};
}

// What has been announced for one multicast destination. Two announcers
// claiming the same address and port is the case worth catching: the receiver
// has to pick one, and whichever it picks, the other stream is unreachable.
struct Destination {
    std::string firstSdp;
    std::string firstSource;
    uint64_t    announcements{0};
    uint64_t    conflicting{0};       // announcements whose SDP differs from the first
    std::map<std::string, uint64_t> bySource;
};

} // namespace

// -- Main --

int run(int argc, char* argv[]) {
    std::string interfaceIP;
    std::string group = "224.2.127.254";
    uint16_t    port  = 9875;
    int         duration = 0;
    bool        verbose  = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--interface" && i + 1 < argc)     interfaceIP = argv[++i];
        else if (arg == "--group" && i + 1 < argc)    group = argv[++i];
        else if (arg == "--port" && i + 1 < argc)     port = static_cast<uint16_t>(atoi(argv[++i]));
        else if (arg == "--duration" && i + 1 < argc) duration = atoi(argv[++i]);
        else if (arg == "--verbose")                  verbose = true;
        else if (arg == "--help" || arg == "-h") {
            fprintf(stderr,
                "AES67 SAP Monitor - listens for SAP announcements and prints what they describe\n\n"
                "Usage: %s [options]\n\n"
                "Options:\n"
                "  --interface <ip>  Local interface IP to join the SAP group on (default: system)\n"
                "  --group <addr>    SAP group (default: 224.2.127.254)\n"
                "  --port <port>     SAP port (default: 9875)\n"
                "  --duration <sec>  Duration in seconds (default: 0 = infinite)\n"
                "  --verbose         Print the whole SDP of every announcement\n",
                argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s (use --help)\n", arg.c_str());
            return 1;
        }
    }

    fprintf(stderr, "AES67 SAP Monitor\n");
    fprintf(stderr, "  Group:     %s:%u\n", group.c_str(), port);
    fprintf(stderr, "  Interface: %s\n", interfaceIP.empty() ? "system default" : interfaceIP.c_str());
    fprintf(stderr, "  Duration:  %s\n",
            duration > 0 ? (std::to_string(duration) + "s").c_str() : "infinite");
    fprintf(stderr, "\nPress Ctrl+C to stop.\n\n");

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "Error: socket() failed (errno=%d: %s)\n", errno, strerror(errno));
        return 1;
    }

    int reuse = 1;
    (void)setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    (void)setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    struct sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons(port);
    if (bind(sockfd, reinterpret_cast<struct sockaddr*>(&bindAddr), sizeof(bindAddr)) < 0) {
        fprintf(stderr, "Error: bind() on port %u failed (errno=%d: %s)\n",
                port, errno, strerror(errno));
        close(sockfd);
        return 1;
    }

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(group.c_str());
    mreq.imr_interface.s_addr = interfaceIP.empty() ? htonl(INADDR_ANY)
                                                    : inet_addr(interfaceIP.c_str());
    if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        fprintf(stderr, "Error: joining %s failed (errno=%d: %s)\n",
                group.c_str(), errno, strerror(errno));
        close(sockfd);
        return 1;
    }

    std::map<std::string, Destination> destinations;   // keyed by address:port
    uint64_t packets = 0, unparsed = 0, deletions = 0;

    std::vector<char> buffer(65536);
    const auto startTime = std::chrono::steady_clock::now();

    struct pollfd pfd;
    pfd.fd = sockfd;
    pfd.events = POLLIN;

    while (g_running) {
        if (duration > 0 &&
            std::chrono::steady_clock::now() - startTime >= std::chrono::seconds(duration)) {
            break;
        }

        if (poll(&pfd, 1, 200) <= 0) continue;

        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        ssize_t bytes = recvfrom(sockfd, buffer.data(), buffer.size(), 0,
                                 reinterpret_cast<struct sockaddr*>(&from), &fromLen);
        if (bytes <= 0) continue;
        ++packets;

        char fromText[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &from.sin_addr, fromText, sizeof(fromText));

        const AES67::SAPAnnouncement announcement =
            AES67::SAPListener::parseAnnouncement(buffer.data(), static_cast<size_t>(bytes),
                                                  fromText);
        if (announcement.sessionDescription.empty()) {
            ++unparsed;
            fprintf(stderr, "[%s] unparsable SAP packet, %zd bytes\n", fromText, bytes);
            continue;
        }
        if (announcement.isDeletion) {
            ++deletions;
            fprintf(stderr, "[%s] DELETE \"%s\" (%s:%d)\n", fromText,
                    announcement.sessionName.c_str(),
                    announcement.multicastAddress.c_str(), announcement.port);
            continue;
        }

        const std::string key = announcement.multicastAddress + ":" +
                                std::to_string(announcement.port);
        Destination& destination = destinations[key];
        ++destination.announcements;
        ++destination.bySource[fromText];

        const bool first = destination.firstSdp.empty();
        if (first) {
            destination.firstSdp    = announcement.sessionDescription;
            destination.firstSource = fromText;
        } else if (announcement.sessionDescription != destination.firstSdp) {
            ++destination.conflicting;
        }

        if (first || destination.conflicting == 1 || verbose) {
            fprintf(stderr, "[%s] \"%s\" -> %s:%d  hash=0x%04X origin=0x%08X\n",
                    fromText, announcement.sessionName.c_str(),
                    announcement.multicastAddress.c_str(), announcement.port,
                    announcement.msgIdHash, announcement.originatingSource);
            const std::string rtpmap = sdpLine(announcement.sessionDescription, "a=rtpmap:");
            const std::string ptime  = sdpLine(announcement.sessionDescription, "a=ptime:");
            const std::string clock  = sdpLine(announcement.sessionDescription, "a=ts-refclk:");
            if (!rtpmap.empty()) fprintf(stderr, "        %s\n", rtpmap.c_str());
            if (!ptime.empty())  fprintf(stderr, "        %s\n", ptime.c_str());
            fprintf(stderr, "        %s\n",
                    clock.empty() ? "a=ts-refclk: (absent)" : clock.c_str());
            if (verbose) {
                fprintf(stderr, "--- SDP ---\n%s\n-----------\n",
                        announcement.sessionDescription.c_str());
            }
        }
    }

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "AES67 SAP Monitor - Results\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "  Packets:        %llu\n", static_cast<unsigned long long>(packets));
    fprintf(stderr, "  Unparsable:     %llu\n", static_cast<unsigned long long>(unparsed));
    fprintf(stderr, "  Deletions:      %llu\n", static_cast<unsigned long long>(deletions));
    fprintf(stderr, "  Destinations:   %zu\n", destinations.size());

    for (const auto& pair : destinations) {
        fprintf(stderr, "\n  %s\n", pair.first.c_str());
        fprintf(stderr, "    Announcements: %llu\n",
                static_cast<unsigned long long>(pair.second.announcements));
        fprintf(stderr, "    Announcers:    %zu\n", pair.second.bySource.size());
        for (const auto& source : pair.second.bySource) {
            fprintf(stderr, "      %s: %llu\n", source.first.c_str(),
                    static_cast<unsigned long long>(source.second));
        }
        if (pair.second.conflicting > 0) {
            fprintf(stderr, "    CONFLICTING DESCRIPTIONS: %llu announcements disagree "
                    "with the first one from %s\n",
                    static_cast<unsigned long long>(pair.second.conflicting),
                    pair.second.firstSource.c_str());
        }
    }
    fprintf(stderr, "========================================\n");

    close(sockfd);
    return packets > 0 ? 0 : 1;
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
