//
// TestSessionDirectory.cpp
// AES67 macOS Driver
//
// The one list every discoverer arrives at.
//
// What matters here is that the same session found two ways is one entry.
// SAP and RTSP see overlapping halves of a network -- a RAVENNA sender
// registers a service and may never announce, a Dante box in AES67 mode
// announces and registers nothing, and plenty of gear does both -- so a
// directory that keyed on how a session was heard would show the same flow
// twice and let someone subscribe to it twice.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/SessionDirectory.h"

#include <chrono>
#include <string>

using namespace AES67;

namespace {

const std::string kSdp =
    "v=0\r\n"
    "o=- 1311738121 1 IN IP4 192.168.1.50\r\n"
    "s=Studio Mic 1\r\n"
    "c=IN IP4 239.69.0.1/32\r\n"
    "t=0 0\r\n"
    "m=audio 5004 RTP/AVP 97\r\n"
    "a=rtpmap:97 L24/48000/8\r\n";

/// The same session as kSdp, one version later and with a line added --
/// what an announcer sends after someone changes its configuration.
const std::string kSdpUpdated =
    "v=0\r\n"
    "o=- 1311738121 2 IN IP4 192.168.1.50\r\n"
    "s=Studio Mic 1\r\n"
    "c=IN IP4 239.69.0.1/32\r\n"
    "t=0 0\r\n"
    "m=audio 5004 RTP/AVP 97\r\n"
    "a=rtpmap:97 L24/48000/8\r\n"
    "a=framecount:48\r\n";

DiscoveredSessionEntry entry(const std::string& sdp, DiscoverySource source,
                             const std::string& name = "Studio Mic 1") {
    DiscoveredSessionEntry session;
    session.sessionName = name;
    session.sourceAddress = "192.168.1.50";
    session.multicastAddress = "239.69.0.1";
    session.port = 5004;
    session.sessionDescription = sdp;
    session.sources = {source};
    return session;
}

} // namespace

TEST_CASE("A session's identity is its origin, not its description") {
    // Same session, later version, extra attribute: one identity.
    CHECK(SessionDirectory::identityOf(kSdp, "239.69.0.1", 5004) ==
          SessionDirectory::identityOf(kSdpUpdated, "239.69.0.1", 5004));

    // Different origin, same destination: two sessions. Two senders
    // configured onto one address is a fault worth seeing, not one to hide
    // by merging them.
    const std::string other =
        "v=0\r\no=- 999 1 IN IP4 192.168.1.77\r\ns=Other\r\n"
        "c=IN IP4 239.69.0.1/32\r\nm=audio 5004 RTP/AVP 97\r\n";
    CHECK(SessionDirectory::identityOf(kSdp, "239.69.0.1", 5004) !=
          SessionDirectory::identityOf(other, "239.69.0.1", 5004));

    // No usable o= line: the destination is the identity, so the same flow
    // announced twice is still one entry.
    const std::string noOrigin = "v=0\r\ns=No Origin\r\nm=audio 5004 RTP/AVP 97\r\n";
    CHECK(SessionDirectory::identityOf(noOrigin, "239.69.0.1", 5004) ==
          SessionDirectory::identityOf(noOrigin, "239.69.0.1", 5004));
    CHECK(SessionDirectory::identityOf(noOrigin, "239.69.0.1", 5004) !=
          SessionDirectory::identityOf(noOrigin, "239.69.0.2", 5004));
}

TEST_CASE("One session heard two ways is one entry that knows both") {
    SessionDirectory directory;
    directory.offer(entry(kSdp, DiscoverySource::SAP));
    directory.offer(entry(kSdp, DiscoverySource::RTSP));

    REQUIRE(directory.size() == 1);
    const DiscoveredSessionEntry session = directory.sessions().front();
    CHECK(session.sources.size() == 2);
    CHECK(session.heardBy(DiscoverySource::SAP));
    CHECK(session.heardBy(DiscoverySource::RTSP));
    CHECK_FALSE(session.heardBy(DiscoverySource::NMOS));

    // Heard again by a route already known: still one source each.
    directory.offer(entry(kSdp, DiscoverySource::SAP));
    CHECK(directory.sessions().front().sources.size() == 2);
}

TEST_CASE("Two different sessions stay two") {
    SessionDirectory directory;
    DiscoveredSessionEntry second = entry(kSdp, DiscoverySource::SAP, "Studio Mic 2");
    second.sessionDescription =
        "v=0\r\no=- 22 1 IN IP4 192.168.1.51\r\ns=Studio Mic 2\r\n"
        "c=IN IP4 239.69.0.2/32\r\nm=audio 5006 RTP/AVP 97\r\n";
    second.multicastAddress = "239.69.0.2";
    second.port = 5006;

    directory.offer(entry(kSdp, DiscoverySource::SAP));
    directory.offer(second);
    CHECK(directory.size() == 2);
}

TEST_CASE("The newer description replaces the older one") {
    SessionDirectory directory;
    directory.offer(entry(kSdp, DiscoverySource::SAP));
    directory.offer(entry(kSdpUpdated, DiscoverySource::RTSP));

    REQUIRE(directory.size() == 1);
    const DiscoveredSessionEntry session = directory.sessions().front();
    // Subscribing has to use what the sender says now: the added
    // a=framecount is the difference between locking and not, for the gear
    // that reads it.
    CHECK(session.sessionDescription.find("a=framecount:48") != std::string::npos);
}

TEST_CASE("A session that stops being refreshed is swept") {
    SessionDirectory directory;
    const auto start = std::chrono::steady_clock::now();
    directory.offer(entry(kSdp, DiscoverySource::SAP), start);

    CHECK(directory.sessions(start + std::chrono::seconds(299)).size() == 1);
    CHECK(directory.sessions(start + std::chrono::seconds(301)).empty());
}

TEST_CASE("A refresh from either route keeps a session alive") {
    SessionDirectory directory;
    const auto start = std::chrono::steady_clock::now();
    directory.offer(entry(kSdp, DiscoverySource::SAP), start);
    directory.offer(entry(kSdp, DiscoverySource::RTSP), start + std::chrono::seconds(200));

    // Refreshed at 200 s, so at 450 s it is still inside the timeout the
    // second sighting started.
    CHECK(directory.sessions(start + std::chrono::seconds(450)).size() == 1);
    CHECK(directory.sessions(start + std::chrono::seconds(550)).empty());
}

TEST_CASE("A discoverer that knows a session is gone can say so") {
    SessionDirectory directory;
    directory.offer(entry(kSdp, DiscoverySource::SAP));
    const std::string identity = directory.sessions().front().identity;

    CHECK(directory.forget(identity));
    CHECK(directory.size() == 0);
    CHECK_FALSE(directory.forget(identity));
}

TEST_CASE("Every source has a name the Manager app can show") {
    CHECK(std::string(discoverySourceName(DiscoverySource::SAP)) == "sap");
    CHECK(std::string(discoverySourceName(DiscoverySource::RTSP)) == "rtsp");
    CHECK(std::string(discoverySourceName(DiscoverySource::NMOS)) == "nmos");
}
