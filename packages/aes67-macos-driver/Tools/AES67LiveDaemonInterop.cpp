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
// Exit code is the number of checks that failed, capped at 255, so a CI
// step fails loudly rather than needing its output parsed.
//
#include "Driver/SDPParser.h"
#include "NetworkEngine/Discovery/RTSPClient.h"
#include "NetworkEngine/Discovery/SAPAnnouncer.h"
#include "NetworkEngine/Discovery/SAPListener.h"
#include "NetworkEngine/TxSession.h"
#include "Ravenna/HTTPClient.h"

#include <cerrno>
#include <chrono>
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

/// std::strtol over atoi: a CLI argument this driver did not write and
/// atoi has no way to report a conversion failure at all -- a malformed
/// value would silently become 0 with no way to tell it apart from a
/// deliberate one.
int parseInt(const std::string& text, int fallback) {
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || errno == ERANGE) return fallback;
    return static_cast<int>(value);
}

struct Options {
    std::string host{"127.0.0.1"};
    uint16_t rtspPort{8854};
    uint16_t httpPort{8080};
    std::string sapGroup{"239.255.255.255"};
    int daemonSourceId{0};
    std::string daemonSourceName{"CI Fake Source"};
    int pollTimeoutMs{20000};
    int pollIntervalMs{500};
};

Options parseArgs(int argc, char** argv) {
    Options opts;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const std::string value = argv[i + 1];
        if (key == "--host") opts.host = value;
        else if (key == "--rtsp-port") opts.rtspPort = static_cast<uint16_t>(parseInt(value, opts.rtspPort));
        else if (key == "--http-port") opts.httpPort = static_cast<uint16_t>(parseInt(value, opts.httpPort));
        else if (key == "--sap-group") opts.sapGroup = value;
        else if (key == "--daemon-source-id") opts.daemonSourceId = parseInt(value, opts.daemonSourceId);
        else if (key == "--daemon-source-name") opts.daemonSourceName = value;
        else if (key == "--poll-timeout-ms") opts.pollTimeoutMs = parseInt(value, opts.pollTimeoutMs);
        else if (key == "--poll-interval-ms") opts.pollIntervalMs = parseInt(value, opts.pollIntervalMs);
    }
    return opts;
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

} // namespace

int main(int argc, char** argv) {
    const Options opts = parseArgs(argc, argv);

    std::printf("=== LIVE INTEROP: this driver <-> a running aes67-linux-daemon ===\n");
    std::printf("host=%s rtsp=%u http=%u sap-group=%s\n", opts.host.c_str(), opts.rtspPort,
               opts.httpPort, opts.sapGroup.c_str());

    checkDaemonSourceIsDiscoverable(opts);
    checkThisDriverIsDiscoverable(opts);

    std::printf("\n%d check(s) failed\n", fails);
    return fails > 255 ? 255 : fails;
}
