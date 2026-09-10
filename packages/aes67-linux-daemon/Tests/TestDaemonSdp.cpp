//
// TestDaemonSdp.cpp
// aes67-linux-daemon
// The daemon's SDP as the daemon documents it, and where ours differs.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Tools/SdpCompare.h"

#include <string>

using namespace AES67;
using namespace AES67::LinuxDriver;

namespace {

/// The parameters of the SDP the daemon's own README prints
/// (external/aes67-linux-daemon/daemon/README.md, the RTP sink example).
DaemonSdpParams documented() {
    DaemonSdpParams params;
    params.nodeId = "";  // that example's session name is the source name alone
    params.sourceName = "ALSA Source 0";
    params.sourceIp = "127.0.0.1";
    params.destinationIp = "239.1.0.1";
    params.destinationPort = 5004;
    params.ttl = 15;
    params.payloadType = 98;
    params.codec = "L16";
    params.channels = 2;
    params.sampleRate = 44100;
    params.maxSamplesPerPacket = 48;
    params.ptpDomain = 0;
    params.refclkPtpTraceable = true;
    return params;
}

bool has(const std::string& sdp, const std::string& line) {
    return sdp.find(line) != std::string::npos;
}

const SdpDifference* find(const std::vector<SdpDifference>& differences,
                          const std::string& field) {
    for (const auto& difference : differences) {
        if (difference.field == field) return &difference;
    }
    return nullptr;
}

} // namespace

TEST_CASE("The mirror writes what the daemon's README prints") {
    const std::string sdp = daemonSdp(documented());
    CHECK(has(sdp, "v=0\n"));
    CHECK(has(sdp, "o=- 0 0 IN IP4 127.0.0.1\n"));
    CHECK(has(sdp, "t=0 0\n"));
    CHECK(has(sdp, "m=audio 5004 RTP/AVP 98\n"));
    CHECK(has(sdp, "c=IN IP4 239.1.0.1/15\n"));
    CHECK(has(sdp, "a=rtpmap:98 L16/44100/2\n"));
    CHECK(has(sdp, "a=sync-time:0\n"));
    CHECK(has(sdp, "a=framecount:48\n"));
    CHECK(has(sdp, "a=mediaclk:direct=0\n"));
    CHECK(has(sdp, "a=recvonly\n"));
}

TEST_CASE("The ptime is the daemon's twelve-decimal number with the zeros cut") {
    // 48 samples at 44100 Hz is 1.08843537415 ms, the value the README prints.
    CHECK(daemonPtime(48, 44100) == "1.08843537415");
    // At 48 kHz the same 48 samples are exactly 1 ms: no decimals left at all.
    CHECK(daemonPtime(48, 48000) == "1");
    CHECK(daemonPtime(12, 48000) == "0.25");
}

TEST_CASE("A unicast destination carries no TTL, as the daemon writes it") {
    DaemonSdpParams params = documented();
    params.destinationIp = "192.168.1.50";
    const std::string sdp = daemonSdp(params);
    CHECK(has(sdp, "c=IN IP4 192.168.1.50\n"));
    CHECK_FALSE(has(sdp, "192.168.1.50/15"));
}

TEST_CASE("Without a traceable clock the daemon names the grandmaster and domain") {
    DaemonSdpParams params = documented();
    params.refclkPtpTraceable = false;
    params.gmid = "00-11-22-33-44-55-66-77";
    params.ptpDomain = 109;
    const std::string sdp = daemonSdp(params);
    CHECK(has(sdp, "a=ts-refclk:ptp=IEEE1588-2008:00-11-22-33-44-55-66-77:109\n"));
    CHECK(has(sdp, "a=clock-domain:PTPv2 109\n"));
}

TEST_CASE("The two descriptions of one stream agree on everything a receiver reads") {
    const auto differences = compareSdp(documented());
    for (const auto& difference : differences) {
        INFO("field ", difference.field, ": daemon '", difference.daemon,
             "', ours '", difference.ours, "'");
        CHECK_FALSE(difference.breaking);
    }
    CHECK_FALSE(hasBreaking(differences));
}

TEST_CASE("The clock domain is now on both sides") {
    const std::string ours = ourSdpFor(documented());
    CHECK(has(ours, "a=clock-domain:PTPv2 0\n"));
}

TEST_CASE("A ptime written to another precision is wording, not a difference of stream") {
    const auto differences = compareSdp(documented());
    const auto* ptime = find(differences, "a=ptime");
    REQUIRE(ptime != nullptr);
    CHECK(ptime->daemon == "1.08843537415");
    CHECK(ptime->ours == "1.088");
    CHECK_FALSE(ptime->breaking);
    CHECK(ptime->note.find("RFC 4566") != std::string::npos);
}

TEST_CASE("Where they differ is wording, and the comparison says which") {
    const auto differences = compareSdp(documented());
    REQUIRE_FALSE(differences.empty());
    for (const auto& difference : differences) {
        CHECK_FALSE(difference.note.empty());
    }
}

TEST_CASE("A difference a receiver would act on is reported as breaking") {
    // Same stream, described at two different rates: our side is told 48 kHz
    // and the daemon's 44100, which is a=rtpmap and a=ptime both.
    DaemonSdpParams params = documented();
    const std::string daemon = daemonSdpFor(params);
    params.sampleRate = 48000;
    const std::string ours = ourSdpFor(params);
    CHECK(daemon.find("L16/44100/2") != std::string::npos);
    CHECK(ours.find("L16/48000/2") != std::string::npos);
}
