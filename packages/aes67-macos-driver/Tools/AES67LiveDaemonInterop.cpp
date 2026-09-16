//
// AES67LiveDaemonInterop.cpp
// AES67 macOS Driver - Tools
// Protocol-level interop against a REAL, running aes67-linux-daemon --
// bondagit/aes67-linux-daemon, built with -DFAKE_DRIVER=ON (no kernel
// module, no ALSA hardware) and started by the CI job that runs this, not
// by anything here.
//
// Tools/AES67InteropSim.cpp and Tests/support/DaemonSap.{h,cpp} /
// Tests/support/DaemonRtsp.{h,cpp} check this driver against the daemon's
// wire formats and decision logic, reproduced from its source, with no
// network and no daemon process. That is a strong check of fidelity to the
// source read at one point in time; it is not proof that two real processes
// on a real loopback actually agree, which is what this is for -- the local
// counterpart of the offline oracle, run only where a Linux runner and the
// daemon's own fake-driver build make it possible.
//
// Two directions, matching the two roles a receiver and a source play:
//
//   B) The daemon already has one RTP source configured (the CI job writes
//      its status.json before starting it). This driver's own SAPListener
//      has to hear its SAP announcement, and this driver's own RTSPClient
//      has to DESCRIBE it at /by-id/<id> and get back a session with the
//      fields a receiver needs.
//
//   A) This driver announces a source of its own with SAPAnnouncer. The
//      daemon's own SAP browser has to pick it up, which is read back
//      through its REST API (GET /api/browse/sources/sap) rather than
//      through anything internal to this driver.
//
//   C) The audio itself, which A and B do not touch: a sink is configured on
//      the daemon with the SDP this driver announces for a transmit stream,
//      RTP is sent to the group that SDP names, and the daemon's own sink
//      status says whether it is receiving and whether its sequence, SSRC or
//      payload-type checks fired. This is the first thing in this project
//      that puts audio in front of a receiver somebody else wrote.
//
// Exit code is the number of checks that failed, capped at 255, so a CI
// step fails loudly rather than needing its output parsed. A command line it
// cannot read exits 1 before any check has run, with the reason on stderr.
//
#include "ToolOptions.h"

#include "Driver/SDPParser.h"
#include "NetworkEngine/JsonEscape.h"
#include "NetworkEngine/JsonFields.h"
#include "NetworkEngine/RTP/PacketBudget.h"
#include "NetworkEngine/RTP/RTPHeader.h"
#include "NetworkEngine/Discovery/RTSPClient.h"
#include "NetworkEngine/TxSession.h"
#include "NetworkEngine/Discovery/SAPAnnouncer.h"
#include "NetworkEngine/Discovery/SAPListener.h"
#include "NetworkEngine/TxSession.h"
#include "Ravenna/HTTPClient.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace AES67;

namespace {

int fails = 0;

void check(const char* what, bool condition, const std::string& detail) {
    std::printf("  [%s] %s -- %s\n", condition ? "OK" : "XX", what, detail.c_str());
    if (!condition) ++fails;
}

/// Something this configuration cannot settle: printed, never failed. The two
/// offline simulations carry the same category for the same reason -- a
/// question left open is worth saying out loud, and worth not pretending to
/// have answered.
void observe(const char* what, const std::string& detail) {
    std::printf("  [??] %s -- %s\n", what, detail.c_str());
}

struct Options {
    std::string host{"127.0.0.1"};
    std::string audioGroup{"239.1.0.77"};
    uint16_t audioPort{5104};
    int sinkId{1};
    int audioSeconds{3};
    uint16_t rtspPort{8854};
    uint16_t httpPort{8080};
    std::string sapGroup{"239.255.255.255"};
    int daemonSourceId{0};
    std::string daemonSourceName{"CI Fake Source"};
    int pollTimeoutMs{20000};
    int pollIntervalMs{500};
};

void printUsage(const char* argv0) {
    const Options defaults;
    std::printf("Usage: %s [options]\n\n", argv0);
    std::printf("Options:\n");
    std::printf("  --host <addr>              Daemon host (default: %s)\n", defaults.host.c_str());
    std::printf("  --rtsp-port <port>         Daemon RTSP port (default: %u)\n", defaults.rtspPort);
    std::printf("  --http-port <port>         Daemon REST port (default: %u)\n", defaults.httpPort);
    std::printf("  --sap-group <addr>         SAP group (default: %s)\n", defaults.sapGroup.c_str());
    std::printf("  --daemon-source-id <n>     Its pre-configured source (default: %d)\n",
                defaults.daemonSourceId);
    std::printf("  --daemon-source-name <s>   That source's name (default: %s)\n",
                defaults.daemonSourceName.c_str());
    std::printf("  --audio-group <addr>       Group to send direction C's audio to (default: %s)\n",
                defaults.audioGroup.c_str());
    std::printf("  --audio-port <port>        Port for that audio (default: %u)\n", defaults.audioPort);
    std::printf("  --sink-id <n>              Sink to configure on the daemon (default: %d)\n",
                defaults.sinkId);
    std::printf("  --audio-seconds <n>        How long to send it (default: %d)\n",
                defaults.audioSeconds);
    std::printf("  --poll-timeout-ms <n>      Give up waiting after this (default: %d)\n",
                defaults.pollTimeoutMs);
    std::printf("  --poll-interval-ms <n>     How often to ask while waiting (default: %d)\n",
                defaults.pollIntervalMs);
}

enum class ArgsResult {
    Ok,            // opts is filled in; run the checks
    UsagePrinted,  // --help; there is nothing to fail
    Bad,           // the command line could not be read; the reason is printed
};

/// The command line, read the way the other tools in this directory read
/// theirs.
///
/// What this replaced stepped `i += 2` on the assumption that every argument
/// was half of a key/value pair, so a flag written without its value shifted
/// everything after it -- keys landed where values were expected and were
/// silently dropped. Unknown keys were dropped in the same silence, which
/// means a typo in the CI job's command line ran the whole interop against a
/// default the job had not asked for and reported nothing. The numbers went
/// through a parseInt that returned the default when the text was not a
/// number, so "8080x" and "eight thousand" configured port 8080 too.
ArgsResult parseArgs(int argc, char** argv, Options& opts) {
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        long long number = 0;

        if (key == "--host") {
            const char* text = ToolOptions::value(argc, argv, i);
            if (text == nullptr) return ArgsResult::Bad;
            opts.host = text;
        }
        else if (key == "--rtsp-port") {
            if (!ToolOptions::integerOption(argc, argv, i, 1, 65535, number)) return ArgsResult::Bad;
            opts.rtspPort = static_cast<uint16_t>(number);
        }
        else if (key == "--http-port") {
            if (!ToolOptions::integerOption(argc, argv, i, 1, 65535, number)) return ArgsResult::Bad;
            opts.httpPort = static_cast<uint16_t>(number);
        }
        else if (key == "--sap-group") {
            const char* text = ToolOptions::value(argc, argv, i);
            if (text == nullptr) return ArgsResult::Bad;
            opts.sapGroup = text;
        }
        else if (key == "--daemon-source-id") {
            if (!ToolOptions::integerOption(argc, argv, i, 0, 2147483647LL, number)) return ArgsResult::Bad;
            opts.daemonSourceId = static_cast<int>(number);
        }
        else if (key == "--daemon-source-name") {
            const char* text = ToolOptions::value(argc, argv, i);
            if (text == nullptr) return ArgsResult::Bad;
            opts.daemonSourceName = text;
        }
        else if (key == "--audio-group") {
            const char* text = ToolOptions::value(argc, argv, i);
            if (text == nullptr) return ArgsResult::Bad;
            opts.audioGroup = text;
        }
        else if (key == "--audio-port") {
            if (!ToolOptions::integerOption(argc, argv, i, 1, 65535, number)) return ArgsResult::Bad;
            opts.audioPort = static_cast<uint16_t>(number);
        }
        else if (key == "--sink-id") {
            if (!ToolOptions::integerOption(argc, argv, i, 0, 2147483647LL, number)) return ArgsResult::Bad;
            opts.sinkId = static_cast<int>(number);
        }
        else if (key == "--audio-seconds") {
            // At least one: direction C is "send audio and ask the daemon
            // what it made of it", and zero seconds of audio asks nothing.
            if (!ToolOptions::integerOption(argc, argv, i, 1, 2147483647LL, number)) return ArgsResult::Bad;
            opts.audioSeconds = static_cast<int>(number);
        }
        else if (key == "--poll-timeout-ms") {
            if (!ToolOptions::integerOption(argc, argv, i, 1, 2147483647LL, number)) return ArgsResult::Bad;
            opts.pollTimeoutMs = static_cast<int>(number);
        }
        else if (key == "--poll-interval-ms") {
            // Zero here is a busy loop against another process's REST API,
            // which is not a thing to let a command line ask for by accident.
            if (!ToolOptions::integerOption(argc, argv, i, 1, 2147483647LL, number)) return ArgsResult::Bad;
            opts.pollIntervalMs = static_cast<int>(number);
        }
        else if (key == "--help" || key == "-h") {
            printUsage(argv[0]);
            return ArgsResult::UsagePrinted;
        }
        else {
            std::fprintf(stderr, "Unknown option: %s (use --help)\n", key.c_str());
            return ArgsResult::Bad;
        }
    }
    return ArgsResult::Ok;
}

/// Direction B: what the daemon already has configured, read through this
/// driver's own SAPListener and RTSPClient.
void checkDaemonSourceIsDiscoverable(const Options& opts) {
    std::printf("\n[B] The daemon's own source, heard by this driver's SAPListener\n");

    SAPListener listener;
    check("SAP listener init", listener.initialize(opts.host), opts.host);
    check("SAP listener start", listener.start(), "");

    SAPAnnouncement found;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(opts.pollTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& ann : listener.getDiscoveredStreams()) {
            // Contains, not equals: the daemon's own SAP announcer prefixes
            // its own s= line with "Daemon <node id> " (sap.cpp), so the
            // configured name survives as a suffix rather than verbatim.
            if (ann.sessionName.find(opts.daemonSourceName) != std::string::npos) {
                found = ann;
                break;
            }
        }
        if (!found.sessionDescription.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(opts.pollIntervalMs));
    }
    listener.stop();

    check("heard the daemon's SAP announcement", !found.sessionDescription.empty(),
          found.sessionDescription.empty() ? "nothing matching \"" + opts.daemonSourceName + "\""
                                            : found.sessionDescription);

    if (!found.sessionDescription.empty()) {
        const auto parsed = SDPParser::parseString(found.sessionDescription);
        check("this driver's own parser reads it", parsed.has_value(), "");
        if (parsed) {
            check("connection address present", !parsed->connectionAddress.empty(),
                  parsed->connectionAddress);
            check("port set", parsed->port > 0, std::to_string(parsed->port));
            check("sample rate set", parsed->sampleRate > 0, std::to_string(parsed->sampleRate));
        }
    }

    std::printf("\n[B] The daemon's own source, DESCRIBEd by this driver's RTSPClient\n");
    const std::string url = "rtsp://" + opts.host + ":" + std::to_string(opts.rtspPort) +
                            "/by-id/" + std::to_string(opts.daemonSourceId);
    RTSPClient client(url);
    const auto session = client.describe();
    const RTSPResponse& response = client.getLastResponse();
    check("DESCRIBE answered 200", session.has_value(),
          "status " + std::to_string(response.statusCode) + " " + response.statusMessage);
    if (session) {
        check("connection address present", !session->connectionAddress.empty(),
              session->connectionAddress);
        check("port set", session->port > 0, std::to_string(session->port));
        check("channel count set", session->numChannels > 0, std::to_string(session->numChannels));
        check("encoding is L16 or L24", session->encoding == "L16" || session->encoding == "L24",
              session->encoding);
    }
}

/// Direction A: a source this driver announces, read back through the
/// daemon's own REST API rather than anything internal to this driver.
void checkThisDriverIsDiscoverable(const Options& opts) {
    std::printf("\n[A] This driver's own announcement, heard by the daemon\n");

    // announcedTxSession(), the same call StreamManager::createTxStream()
    // makes, and not SDPParser::createDefaultTxSession(), which this used
    // until 2026-09-15: no production path calls that one -- only tests and
    // this tool did -- and the two differ exactly where a receiver looks.
    // createDefaultTxSession writes a=sendonly, a=framecount and
    // a=source-filter; what the driver actually announces writes none of the
    // three. The one live check of whether the daemon sees us was checking a
    // description this driver never sends.
    const std::string sourceName = "CI Announce Test";
    SDPSession session = announcedTxSession(sourceName, "239.1.0.99", 6004, 2, 48000,
                                            /*dscp=*/-1);
    session.originAddress = opts.host;
    const std::string sdp = SDPParser::generate(session);

    SAPAnnouncer announcer;
    check("SAP announcer init", announcer.initialize(opts.host), opts.host);
    check("SAP announcer start", announcer.start([sdp] { return std::vector<std::string>{sdp}; }),
          "");

    std::string body;
    bool seen = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(opts.pollTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        HTTPClient http(opts.host, opts.httpPort);
        const HTTPResponse resp = http.get("/api/browse/sources/sap");
        if (resp.ok() && resp.body.find(sourceName) != std::string::npos) {
            body = resp.body;
            seen = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(opts.pollIntervalMs));
    }
    announcer.stop();

    check("the daemon's browse-sources-sap lists it", seen,
          seen ? body : "\"" + sourceName + "\" never appeared");
    if (seen) {
        check("the SDP it recorded carries our multicast address",
              body.find("239.1.0.99") != std::string::npos, "239.1.0.99");
    }
}


/// Direction C: the audio. A sink on the daemon, configured with the SDP this
/// driver announces, and RTP sent to the group that SDP names.
void checkTheDaemonReceivesOurAudio(const Options& opts) {
    std::printf("\n[C] The SDP this driver announces, as a sink the daemon opens\n");

    // The session this driver would announce for a transmit stream, not one
    // written for the occasion: announcedTxSession() is what createTxStream()
    // calls, so what the daemon is handed here is what a real stream carries.
    SDPSession session = announcedTxSession("CI Audio Test", opts.audioGroup, opts.audioPort,
                                            2, 48000, /*dscp=*/-1);
    session.originAddress = opts.host;
    const std::string sdp = SDPParser::generate(session);

    // PUT /api/sink/<id>, daemon/README.md. use_sdp takes the description from
    // the body rather than fetching a URL; ignore_refclk_gmid because this
    // driver announces no a=ts-refclk until a grandmaster is known, and the
    // daemon otherwise holds the sink waiting for a clock to agree with --
    // which is a separate question from whether the packets arrive.
    const std::string body =
        std::string("{\"name\":\"CI Audio Test\",\"io\":\"Audio Device\",\"delay\":576,") +
        "\"use_sdp\":true,\"source\":\"\",\"sdp\":\"" + jsonEscape(sdp) + "\"," +
        "\"ignore_refclk_gmid\":true,\"map\":[0,1]}";

    const std::string sinkPath = "/api/sink/" + std::to_string(opts.sinkId);
    {
        HTTPClient http(opts.host, opts.httpPort);
        const HTTPResponse put = http.perform("PUT", sinkPath, body, "application/json");
        check("the daemon took the sink", put.ok(),
              put.ok() ? sinkPath : "status " + std::to_string(put.status) + " " +
                                        (put.error.empty() ? put.body : put.error));
        if (!put.ok()) return;
    }

    // The sender: a plain UDP socket and the core's RTP header, because what
    // is under test is the daemon's receive path, not ours. L24 at 48 kHz in
    // 1 ms packets, paced on the exact length of a packet.
    const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        check("audio socket", false, std::strerror(errno));
        return;
    }
    in_addr iface{};
    ::inet_pton(AF_INET, opts.host.c_str(), &iface);
    ::setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));
    const unsigned char ttl = 1;
    ::setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    const unsigned char loop = 1;   // both ends are this host
    ::setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(opts.audioPort);
    ::inet_pton(AF_INET, opts.audioGroup.c_str(), &destination.sin_addr);

    constexpr uint32_t kFrames = 48;      // 1 ms at 48 kHz
    constexpr uint16_t kChannels = 2;
    constexpr uint32_t kRate = 48000;
    constexpr double kPi = 3.14159265358979323846;
    const auto step = PacketBudget::packetInterval(kFrames, kRate);
    uint32_t carry = 0;

    std::vector<uint8_t> packet(sizeof(RTP::RTPHeader) + kFrames * kChannels * 3);
    uint16_t sequence = 1;
    uint32_t timestamp = 0;
    double phase = 0.0;
    const double increment = 2.0 * kPi * 1000.0 / kRate;

    auto next = std::chrono::steady_clock::now();
    const auto until = next + std::chrono::seconds(opts.audioSeconds);
    uint64_t sent = 0, sendFailures = 0;

    while (std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_until(next);
        uint64_t stepNs = step.wholeNs;
        carry += step.remainder;
        if (carry >= kRate) { stepNs += carry / kRate; carry %= kRate; }
        next += std::chrono::nanoseconds(stepNs);

        RTP::RTPHeader header{};
        header.version = 2;
        header.payloadType = 97;
        header.sequenceNumber = sequence++;
        header.timestamp = timestamp;
        header.ssrc = 0x43490001;
        header.toNetworkOrder();
        std::memcpy(packet.data(), &header, sizeof(header));

        uint8_t* payload = packet.data() + sizeof(header);
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            const double sample = std::sin(phase);
            phase += increment;
            if (phase >= 2.0 * kPi) phase -= 2.0 * kPi;
            const int32_t value = static_cast<int32_t>(sample * 8388607.0);
            for (uint16_t channel = 0; channel < kChannels; ++channel) {
                const size_t at = (static_cast<size_t>(frame) * kChannels + channel) * 3;
                payload[at] = static_cast<uint8_t>((value >> 16) & 0xFF);
                payload[at + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
                payload[at + 2] = static_cast<uint8_t>(value & 0xFF);
            }
        }

        if (::sendto(sock, packet.data(), packet.size(), 0,
                     reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) < 0) {
            ++sendFailures;
        } else {
            ++sent;
        }
        timestamp += kFrames;
    }
    ::close(sock);

    check("audio went out", sent > 0 && sendFailures == 0,
          std::to_string(sent) + " packets, " + std::to_string(sendFailures) + " send failures");

    // What the daemon itself made of it: GET /api/sink/status/<id>.
    HTTPClient http(opts.host, opts.httpPort);
    const HTTPResponse status = http.get("/api/sink/status/" + std::to_string(opts.sinkId));
    check("the daemon answered for the sink", status.ok(),
          status.ok() ? status.body : "status " + std::to_string(status.status) + " " + status.error);
    if (status.ok()) {
        const auto flag = [&status](const char* name) {
            const auto value = extractBoolField(status.body, name);
            return value.has_value() && *value;
        };
        // All four come from the driver manager, which is the fake one here.
        // Printed so a run against a daemon with the real kernel module shows
        // the difference at a glance, and so that reading this output never
        // suggests the packets were checked off the wire when they were not.
        observe("receiving_rtp_packet",
                std::string(flag("receiving_rtp_packet") ? "true" : "false") +
                    " -- with the fake driver manager this job builds, nothing is "
                    "listening; against the kernel module it is the answer");
        observe("format errors",
                std::string("seq ") + (flag("rtp_seq_id_error") ? "true" : "false") +
                    ", ssrc " + (flag("rtp_ssrc_error") ? "true" : "false") +
                    ", payload type " + (flag("rtp_payload_type_error") ? "true" : "false") +
                    " -- same source, same caveat");
    }

    const HTTPResponse removed = http.perform("DELETE", sinkPath, "", "");
    check("the sink was removed", removed.ok(),
          removed.ok() ? sinkPath : "status " + std::to_string(removed.status));
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    switch (parseArgs(argc, argv, opts)) {
        case ArgsResult::UsagePrinted: return 0;
        case ArgsResult::Bad: return 1;
        case ArgsResult::Ok: break;
    }

    std::printf("=== LIVE INTEROP: this driver <-> a running aes67-linux-daemon ===\n");
    std::printf("host=%s rtsp=%u http=%u sap-group=%s\n", opts.host.c_str(), opts.rtspPort,
               opts.httpPort, opts.sapGroup.c_str());

    checkDaemonSourceIsDiscoverable(opts);
    checkThisDriverIsDiscoverable(opts);
    checkTheDaemonReceivesOurAudio(opts);

    std::printf("\n%d check(s) failed\n", fails);
    return fails > 255 ? 255 : fails;
}
