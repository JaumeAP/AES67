//
// ravenna-announce.cpp
// AES67 RAVENNA session layer - Tools
//
// Advertises one session and answers what asks about it. It sends no audio:
// the stream an SDP describes is somebody else's job -- the Teensy box, the
// macOS driver -- and this is the half that makes it findable.
//
// Usage:
//   ravenna-announce --interface en0 --address 192.168.1.50
//                    [--name "Mix A"] [--group 239.69.1.10] [--port 5004]
//                    [--channels 2] [--rtsp-port 8554] [--host box.local]
//                    [--device-channel 0] [--ptime-us 1000]
//                    [--ptp-gmid 00-1D-C1-FF-FE-00-00-01] [--ptp-domain 0]
//
#include "Ravenna/MdnsResponder.h"
#include "Ravenna/RtspServer.h"
#include "Ravenna/SessionCatalogue.h"

#include <arpa/inet.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

void handleSignal(int) { g_running.store(false, std::memory_order_release); }

void usage() {
    std::fprintf(stderr,
                 "usage: ravenna-announce --interface NAME --address A.B.C.D\n"
                 "                        [--name TEXT] [--group A.B.C.D] [--port N]\n"
                 "                        [--channels N] [--rtsp-port N] [--host NAME]\n"
                 "                        [--device-channel N] [--ptime-us N]\n"
                 "                        [--ptp-gmid ID] [--ptp-domain N]\n");
}

const char* valueFor(int argc, char** argv, int& index) {
    if (index + 1 >= argc) return nullptr;
    return argv[++index];
}

/// Host byte order, which is what the advertisement holds: the encoder writes
/// it big-endian itself, and holding it already swapped is how a field ends up
/// reversed on the wire.
bool addressFrom(const std::string& text, uint32_t& out) {
    struct in_addr parsed {};
    if (::inet_pton(AF_INET, text.c_str(), &parsed) != 1) return false;
    out = ntohl(parsed.s_addr);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace AES67;
    using namespace AES67::Ravenna;

    std::string interfaceName;
    std::string addressText;
    std::string hostName = "aes67.local";
    std::string sessionName = "AES67 Session";
    std::string group = "239.69.1.10";
    uint16_t streamPort = 5004;
    uint16_t rtspPort = 8554;
    uint16_t channels = 2;
    uint16_t deviceChannel = 0;
    uint32_t ptimeUs = 1000;
    std::string ptpGrandmaster;
    int ptpDomain = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        const char* value = nullptr;
        auto need = [&]() { value = valueFor(argc, argv, i); return value != nullptr; };

        if (option == "--help" || option == "-h") { usage(); return 0; }
        else if (option == "--interface") { if (!need()) { usage(); return 2; } interfaceName = value; }
        else if (option == "--address") { if (!need()) { usage(); return 2; } addressText = value; }
        else if (option == "--host") { if (!need()) { usage(); return 2; } hostName = value; }
        else if (option == "--name") { if (!need()) { usage(); return 2; } sessionName = value; }
        else if (option == "--group") { if (!need()) { usage(); return 2; } group = value; }
        else if (option == "--port") { if (!need()) { usage(); return 2; } streamPort = static_cast<uint16_t>(std::atoi(value)); }
        else if (option == "--rtsp-port") { if (!need()) { usage(); return 2; } rtspPort = static_cast<uint16_t>(std::atoi(value)); }
        else if (option == "--channels") { if (!need()) { usage(); return 2; } channels = static_cast<uint16_t>(std::atoi(value)); }
        else if (option == "--device-channel") { if (!need()) { usage(); return 2; } deviceChannel = static_cast<uint16_t>(std::atoi(value)); }
        else if (option == "--ptime-us") { if (!need()) { usage(); return 2; } ptimeUs = static_cast<uint32_t>(std::atoi(value)); }
        else if (option == "--ptp-gmid") { if (!need()) { usage(); return 2; } ptpGrandmaster = value; }
        else if (option == "--ptp-domain") { if (!need()) { usage(); return 2; } ptpDomain = std::atoi(value); }
        else { std::fprintf(stderr, "unknown option: %s\n", option.c_str()); usage(); return 2; }
    }

    if (interfaceName.empty() || addressText.empty()) { usage(); return 2; }

    uint32_t address = 0;
    if (!addressFrom(addressText, address)) {
        std::fprintf(stderr, "not an IPv4 address: %s\n", addressText.c_str());
        return 2;
    }

    RavennaSession session;
    session.name = sessionName;
    session.sdp.sessionName = sessionName;
    session.sdp.originAddress = addressText;
    session.sdp.connectionAddress = group;
    session.sdp.port = streamPort;
    session.sdp.numChannels = channels;
    session.sdp.ptimeUs = ptimeUs;
    session.sdp.direction = "sendonly";
    // a=ts-refclk, RFC 7273: which clock the timestamps are against. Without
    // it the SDP says when a packet was taken and not by whose clock, and a
    // receiver that insists on knowing -- RAVENNA gear does -- will not lock
    // to the stream. It is left out rather than invented when no grandmaster
    // is given, because naming the wrong clock is worse than naming none.
    session.sdp.ptpMasterMAC = ptpGrandmaster;
    session.sdp.ptpDomain = ptpGrandmaster.empty() ? -1 : ptpDomain;
    // The routing matrix is aes67-core's: these are device channels, and the
    // count has to be the one the SDP announces.
    session.mapping.deviceChannelStart = deviceChannel;
    session.mapping.deviceChannelCount = channels;
    session.mapping.streamChannelCount = channels;

    SessionCatalogue catalogue;
    std::string error;
    if (!catalogue.add(session, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    RtspServer rtsp(catalogue);
    if (!rtsp.start(rtspPort, error)) {
        std::fprintf(stderr, "rtsp: %s\n", error.c_str());
        return 1;
    }

    MdnsResponder mdns(catalogue);
    if (!mdns.start(interfaceName, hostName, address, rtsp.port(), error)) {
        std::fprintf(stderr, "mdns: %s\n", error.c_str());
        return 1;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::signal(SIGPIPE, SIG_IGN);
    ::setvbuf(stdout, nullptr, _IOLBF, 0);

    // RFC 6762 sec 8.3 asks for a few announcements a second apart when a
    // service appears, so a browser that was already open sees it.
    for (int i = 0; i < 3 && g_running.load(); ++i) {
        mdns.announce();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::printf("[ravenna] \"%s\" on %s, %u channels, RTSP %s:%u%s\n",
                sessionName.c_str(), group.c_str(), static_cast<unsigned>(channels),
                hostName.c_str(), static_cast<unsigned>(rtsp.port()),
                SessionCatalogue::pathFor(sessionName).c_str());
    if (ptpGrandmaster.empty()) {
        std::fprintf(stderr,
                     "[ravenna] no --ptp-gmid: the SDP carries no a=ts-refclk, and a "
                     "receiver that requires one will not lock to this stream\n");
    }

    size_t queries = 0;
    size_t describes = 0;
    auto nextReport = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    while (g_running.load(std::memory_order_acquire)) {
        queries += mdns.service();
        describes += rtsp.service();

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextReport) {
            std::printf("[ravenna] answered %zu queries, %zu describes\n", queries, describes);
            nextReport = now + std::chrono::seconds(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // The goodbye matters: without it a browser holds the session for another
    // 75 minutes after this stops.
    mdns.goodbye();
    std::printf("[ravenna] withdrawn\n");
    return 0;
}
