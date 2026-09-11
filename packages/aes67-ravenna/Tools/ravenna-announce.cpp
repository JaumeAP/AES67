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
//                    [--rate 48000] [--encoding L24]
//                    [--device-channel 0] [--ptime-us 1000]
//                    [--ptp-gmid 00-1D-C1-FF-FE-00-00-01] [--ptp-domain 0]
//                    [--nmos-port 8080]
//
// It serves NMOS IS-05 as well as RAVENNA's own discovery, which is what a
// controller uses to hand this device somebody else's stream: PATCH the SDP
// onto the receiver's staged endpoint, activate it, and the channels are
// assigned through aes67-core's StreamChannelMapper.
//
#include "NetworkEngine/RTP/PacketBudget.h"
#include "NetworkEngine/RTP/RTPHeader.h"
#include "NetworkEngine/StreamChannelMapper.h"
#include "Ravenna/ChannelMappingApi.h"
#include "Ravenna/ConnectionApi.h"
#include "Ravenna/HttpServer.h"
#include "Ravenna/MdnsResponder.h"
#include "Ravenna/NodeApi.h"
#include "Ravenna/ReceiverRouting.h"
#include "Ravenna/RtspServer.h"
#include "Ravenna/SessionCatalogue.h"

#include <optional>
#include <cerrno>
#include <arpa/inet.h>

#include <atomic>
#include <map>
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
                 "                        [--rate N] [--encoding L16|L24]\n"
                 "                        [--ptp-gmid ID] [--ptp-domain N]\n"
                 "                        [--nmos-port N]\n");
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

// strtol rather than atoi: atoi answers 0 to "abc" and to "0" alike, and a
// port of 0 from a typo is a bug that only shows on the wire.
static std::optional<long> parseNumber(const char* text) {
    if (text == nullptr || *text == '\0') return std::nullopt;
    char* endptr = nullptr;
    errno = 0;
    const long parsed = std::strtol(text, &endptr, 10);
    if (errno != 0 || *endptr != '\0') return std::nullopt;
    return parsed;
}

int main(int argc, char** argv) {
    using namespace AES67;
    using namespace AES67::Ravenna;

    std::string interfaceName;
    std::string addressText;
    std::string hostName = "aes67.local";
    std::string sessionName = "AES67 Session";
    bool nameGiven = false;
    std::string group = "239.69.1.10";
    uint16_t streamPort = 5004;
    uint16_t rtspPort = 8554;
    uint16_t channels = 2;
    uint16_t deviceChannel = 0;
    uint32_t ptimeUs = 1000;
    uint32_t sampleRate = 48000;
    std::string encoding = "L24";
    std::string ptpGrandmaster;
    int ptpDomain = 0;
    uint16_t nmosPort = 8080;

    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        const char* value = nullptr;
        auto need = [&]() { value = valueFor(argc, argv, i); return value != nullptr; };

        if (option == "--help" || option == "-h") { usage(); return 0; }
        else if (option == "--interface") { if (!need()) { usage(); return 2; } interfaceName = value; }
        else if (option == "--address") { if (!need()) { usage(); return 2; } addressText = value; }
        else if (option == "--host") { if (!need()) { usage(); return 2; } hostName = value; }
        else if (option == "--name") { if (!need()) { usage(); return 2; } sessionName = value; nameGiven = true; }
        else if (option == "--group") { if (!need()) { usage(); return 2; } group = value; }
        else if (option == "--port") { if (!need()) { usage(); return 2; } const auto n = parseNumber(value); if (!n) { usage(); return 2; } streamPort = static_cast<uint16_t>(*n); }
        else if (option == "--rtsp-port") { if (!need()) { usage(); return 2; } const auto n = parseNumber(value); if (!n) { usage(); return 2; } rtspPort = static_cast<uint16_t>(*n); }
        else if (option == "--channels") { if (!need()) { usage(); return 2; } const auto n = parseNumber(value); if (!n) { usage(); return 2; } channels = static_cast<uint16_t>(*n); }
        else if (option == "--device-channel") { if (!need()) { usage(); return 2; } const auto n = parseNumber(value); if (!n) { usage(); return 2; } deviceChannel = static_cast<uint16_t>(*n); }
        else if (option == "--rate") { if (!need()) { usage(); return 2; } const auto n = parseNumber(value); if (!n) { usage(); return 2; } sampleRate = static_cast<uint32_t>(*n); }
        else if (option == "--encoding") { if (!need()) { usage(); return 2; } encoding = value; }
        else if (option == "--ptime-us") { if (!need()) { usage(); return 2; } const auto n = parseNumber(value); if (!n) { usage(); return 2; } ptimeUs = static_cast<uint32_t>(*n); }
        else if (option == "--ptp-gmid") { if (!need()) { usage(); return 2; } ptpGrandmaster = value; }
        else if (option == "--ptp-domain") { if (!need()) { usage(); return 2; } const auto n = parseNumber(value); if (!n) { usage(); return 2; } ptpDomain = static_cast<int>(*n); }
        else if (option == "--nmos-port") { if (!need()) { usage(); return 2; } const auto n = parseNumber(value); if (!n) { usage(); return 2; } nmosPort = static_cast<uint16_t>(*n); }
        else { (void)std::fprintf(stderr, "unknown option: %s\n", option.c_str()); usage(); return 2; }
    }

    if (interfaceName.empty() || addressText.empty()) { usage(); return 2; }

    // An announcement for a stream that cannot be sent is worse than no
    // announcement: a receiver accepts the session, subscribes, and waits for
    // packets that will never come whole -- our own transmitter refuses the
    // configuration, and any receiver drops what exceeds the frame. Refuse it
    // here too, with the packet time that would fit.
    const size_t bytesPerSample = PacketBudget::bytesPerSample(encoding);
    if (bytesPerSample == 0) {
        (void)std::fprintf(stderr, "unknown encoding: %s (L16, L24 or AM824)\n",
                           encoding.c_str());
        return 2;
    }
    const uint32_t framesPerPacket = PacketBudget::framesPerPacket(sampleRate, ptimeUs, 0);
    if (framesPerPacket == 0) {
        (void)std::fprintf(stderr, "%u us at %u Hz carries no samples\n", ptimeUs, sampleRate);
        return 2;
    }
    if (!PacketBudget::fits(channels, bytesPerSample, framesPerPacket)) {
        const size_t packetBytes =
            PacketBudget::rtpPacketBytes(channels, bytesPerSample, framesPerPacket);
        const uint32_t maxFrames = PacketBudget::maxFramesPerPacket(channels, bytesPerSample);
        (void)std::fprintf(stderr,
                           "%u channels of %s at %u Hz and %u us make a %zu-byte RTP packet, "
                           "over the %zu-byte limit\n",
                           static_cast<unsigned>(channels), encoding.c_str(), sampleRate,
                           ptimeUs, packetBytes, PacketBudget::kMaxRtpPacketBytes);
        if (maxFrames > 0) {
            (void)std::fprintf(stderr, "  %u channels fit at %u us\n",
                               static_cast<unsigned>(channels),
                               static_cast<uint32_t>((static_cast<uint64_t>(maxFrames) * 1000000ULL)
                                                     / sampleRate));
        }
        (void)std::fprintf(stderr, "  %u us fits %u channels\n", ptimeUs,
                           static_cast<unsigned>(
                               PacketBudget::maxChannelsPerPacket(bytesPerSample, framesPerPacket)));
        return 2;
    }

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
    session.sdp.sampleRate = sampleRate;
    session.sdp.encoding = encoding;
    // The payload type has to be the one the stream actually carries, not a
    // constant: a receiver binds the format to it, and announcing 96 for a
    // stream sent as 97 describes a session nobody can play.
    session.sdp.payloadType = RTP::payloadTypeFor(encoding);
    // a=framecount, RAVENNA's own extension: how many samples per channel one
    // packet holds. RAVENNA gear reads it rather than deriving the number
    // from a=ptime, and at 125 us it is the difference between six samples
    // and a rounding argument. Left out of every announcement until now,
    // because SDPParser::generate omits the line when the field is 0.
    session.sdp.framecount = framesPerPacket;
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

    // IS-05. The sender is what this machine offers; the receiver is what a
    // controller can point at somebody else's stream, and activating it is
    // where the channels get assigned.
    StreamChannelMapper mapper;
    ConnectionApi connections;

    ConnectionSender nmosSender;
    // UUID-shaped, because a controller that validates an IS-04 id rejects
    // anything else, and it is the same id IS-04 publishes for this sender.
    nmosSender.id = stableUuidFrom("sender:" + sessionName);
    nmosSender.label = sessionName;
    nmosSender.sdp = SDPParser::generate(session.sdp);
    connections.addSender(nmosSender);

    ConnectionReceiver nmosReceiver;
    nmosReceiver.id = stableUuidFrom("receiver:1");
    nmosReceiver.label = "Device channels " + std::to_string(deviceChannel) + " and up";
    connections.addReceiver(nmosReceiver);

    // What an activation does to the channels is ReceiverRouting's, not this
    // tool's: it is behaviour, and behaviour written inside a demonstration
    // binary is behaviour nothing tests.
    ReceiverRouting routing(mapper);
    connections.onReceiverActivation([&routing](const std::string& id, const std::string& sdp,
                                                bool enable, std::string& why) {
        RoutingOutcome outcome;
        if (!routing.apply(id, sdp, enable, outcome, why)) return false;

        if (!outcome.connected) {
            std::printf("[ravenna] receiver %s disabled, channels freed\n", id.c_str());
        } else {
            std::printf("[ravenna] receiver %s: %u channels of \"%s\" onto device channels "
                        "%u..%u\n",
                        id.c_str(), static_cast<unsigned>(outcome.channelCount),
                        outcome.streamName.c_str(),
                        static_cast<unsigned>(outcome.deviceChannelStart),
                        static_cast<unsigned>(outcome.deviceChannelStart +
                                              outcome.channelCount - 1));
        }
        return true;
    });

    // The grid, channel by channel: IS-08 over the same matrix.
    ChannelMappingApi channelMapping(mapper, routing);

    // IS-04, so a controller browsing the link finds this device at all and
    // knows what it is made of before it routes anything.
    NodeIdentity identity;
    identity.nodeId = stableUuidFrom(hostName + "/node");
    identity.deviceId = stableUuidFrom(hostName + "/device");
    // What a person picks this node out by in a controller's list, and the
    // single DNS-SD instance label it is browsed under. Not the host name:
    // that names a machine, and it has a dot in it.
    identity.label =
        nameGiven ? sessionName : "AES67 " + hostName.substr(0, hostName.find('.'));
    identity.description = "AES67 sender over RAVENNA discovery";
    identity.hostName = hostName;
    identity.addressV4 = address;
    identity.apiPort = nmosPort;
    identity.ptpGrandmaster = ptpGrandmaster;
    NodeApi nodeApi(identity, catalogue, connections);

    HttpServer nmos([&connections, &channelMapping, &nodeApi](const std::string& method,
                                                              const std::string& path,
                                                              const std::string& body) {
        if (path.rfind(kChannelMappingApiRoot, 0) == 0) {
            return channelMapping.handle(method, path, body);
        }
        if (path.rfind(kNodeApiRoot, 0) == 0) {
            return nodeApi.handle(method, path, body);
        }
        return connections.handle(method, path, body);
    });
    if (!nmos.start(nmosPort, error)) {
        std::fprintf(stderr, "nmos: %s\n", error.c_str());
        return 1;
    }

    MdnsResponder mdns(catalogue);
    mdns.alsoAdvertise(nodeApi.advertisement());
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

    std::printf("[ravenna] on port %u: IS-04 at %s/, IS-05 at %s/single/, "
                "IS-08 at %s/map/\n",
                static_cast<unsigned>(nmos.port()), kNodeApiRoot, kConnectionApiRoot,
                kChannelMappingApiRoot);

    size_t queries = 0;
    size_t describes = 0;
    size_t requests = 0;
    auto nextReport = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    while (g_running.load(std::memory_order_acquire)) {
        queries += mdns.service();
        describes += rtsp.service();
        requests += nmos.service();

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextReport) {
            std::printf("[ravenna] answered %zu queries, %zu describes, %zu IS-05 requests\n",
                        queries, describes, requests);
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
