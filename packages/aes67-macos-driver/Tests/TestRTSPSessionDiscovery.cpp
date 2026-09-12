//
// TestRTSPSessionDiscovery.cpp
// AES67 macOS Driver
//
// The step from "a service is registered on the link" to "a session is in the
// directory": build the URL, ask for the description, read it, file it.
//
// Driven with a fetcher the test supplies, so what is exercised is the
// discovery logic and not a device at the other end. The socket work is
// SDPFetcher's and has its own suite.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/RTSPSessionDiscovery.h"

#include <string>

using namespace AES67;

namespace {

std::string g_lastUrl;

const char* kSdp =
    "v=0\r\n"
    "o=- 1311738121 1 IN IP4 192.168.1.50\r\n"
    "s=Studio Mic 1\r\n"
    "c=IN IP4 239.69.0.1/32\r\n"
    "t=0 0\r\n"
    "m=audio 5004 RTP/AVP 97\r\n"
    "a=rtpmap:97 L24/48000/8\r\n"
    "a=ptime:1\r\n";

std::string answersWithSdp(const std::string& url, std::string& error) {
    g_lastUrl = url;
    (void)error;
    return kSdp;
}

std::string answersWithNothing(const std::string& url, std::string& error) {
    g_lastUrl = url;
    error = "connection refused";
    return {};
}

std::string answersWithRubbish(const std::string& url, std::string& error) {
    g_lastUrl = url;
    (void)error;
    return "this is not an SDP";
}

MDNSService resolvedService() {
    MDNSService service;
    service.name = "Studio Mic 1";
    service.type = MDNSBrowser::kServiceTypeRTSP;
    service.domain = "local.";
    service.hostTarget = "mic.local.";
    service.address = "192.168.1.50";
    service.port = 8554;
    return service;
}

} // namespace

TEST_CASE("A resolved service is described and becomes a session") {
    const RTSPSessionDiscovery::DescribeResult result =
        RTSPSessionDiscovery::describe(resolvedService(), &answersWithSdp);

    REQUIRE(result.ok);
    CHECK(g_lastUrl == "rtsp://192.168.1.50:8554/by-name/Studio Mic 1");

    // What the app needs comes out of the description, not out of the
    // service: the address the audio is actually on is in the SDP, and the
    // service only says where to ask.
    CHECK(result.entry.sessionName == "Studio Mic 1");
    CHECK(result.entry.multicastAddress == "239.69.0.1");
    CHECK(result.entry.port == 5004);
    CHECK(result.entry.sourceAddress == "192.168.1.50");
    CHECK(result.entry.sessionDescription == kSdp);

    REQUIRE(result.entry.sources.size() == 1);
    CHECK(result.entry.sources.front() == DiscoverySource::RTSP);

    // Identified the same way a SAP sighting of the same session would be,
    // which is what makes the two merge instead of showing twice.
    CHECK(result.entry.identity == SessionDirectory::identityOf(kSdp, "239.69.0.1", 5004));
}

TEST_CASE("A service nobody can describe is not listed") {
    const RTSPSessionDiscovery::DescribeResult refused =
        RTSPSessionDiscovery::describe(resolvedService(), &answersWithNothing);
    CHECK_FALSE(refused.ok);
    CHECK(refused.error == "connection refused");

    const RTSPSessionDiscovery::DescribeResult rubbish =
        RTSPSessionDiscovery::describe(resolvedService(), &answersWithRubbish);
    CHECK_FALSE(rubbish.ok);
    CHECK(rubbish.error == "description did not parse");
}

TEST_CASE("An unresolved service is not asked anything") {
    MDNSService unresolved = resolvedService();
    unresolved.port = 0;
    unresolved.address.clear();
    g_lastUrl.clear();

    const RTSPSessionDiscovery::DescribeResult result =
        RTSPSessionDiscovery::describe(unresolved, &answersWithSdp);
    CHECK_FALSE(result.ok);
    CHECK(result.error == "service not resolved");
    CHECK(g_lastUrl.empty());
}

TEST_CASE("The same session found both ways is one entry in the directory") {
    SessionDirectory directory;

    // Heard over SAP first.
    DiscoveredSessionEntry announced;
    announced.sessionName = "Studio Mic 1";
    announced.sourceAddress = "192.168.1.50";
    announced.multicastAddress = "239.69.0.1";
    announced.port = 5004;
    announced.sessionDescription = kSdp;
    announced.sources = {DiscoverySource::SAP};
    directory.offer(announced);

    // Then found registered and described.
    const RTSPSessionDiscovery::DescribeResult described =
        RTSPSessionDiscovery::describe(resolvedService(), &answersWithSdp);
    REQUIRE(described.ok);
    directory.offer(described.entry);

    REQUIRE(directory.size() == 1);
    const DiscoveredSessionEntry session = directory.sessions().front();
    CHECK(session.heardBy(DiscoverySource::SAP));
    CHECK(session.heardBy(DiscoverySource::RTSP));
}
