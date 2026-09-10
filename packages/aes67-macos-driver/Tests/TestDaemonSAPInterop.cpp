//
// TestDaemonSAPInterop.cpp
// AES67 macOS Driver - Tests
// This driver's SAP against the AES67 Linux daemon's rules for taking it.
//
// The daemon is vendored in this repository
// (packages/aes67-linux-daemon/external/aes67-linux-daemon), so its receive
// path is readable rather than guessed at, and support/DaemonSap mirrors it.
// This suite is what says whether the two speak: it found that they did not.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/SAPAnnouncer.h"
#include "NetworkEngine/Discovery/SAPListener.h"
#include "support/DaemonSap.h"

#include <arpa/inet.h>

#include <string>

namespace AES67 {
namespace Tests {

namespace {

const std::string kSDP =
    "v=0\n"
    "o=- 1 1 IN IP4 192.168.1.20\n"
    "s=Driver Out 1\n"
    "t=0 0\n"
    "m=audio 5004 RTP/AVP 98\n"
    "c=IN IP4 239.69.0.1/32\n"
    "a=rtpmap:98 L24/48000/2\n"
    "a=ptime:1\n"
    "a=framecount:48\n"
    "a=ts-refclk:ptp=IEEE1588-2008:traceable\n"
    "a=mediaclk:direct=0\n"
    "a=clock-domain:PTPv2 0\n"
    "a=recvonly\n";

uint32_t sourceOf(const char* dotted) { return ::inet_addr(dotted); }

} // namespace

TEST_CASE("The daemon takes what this driver announces") {
    const uint16_t hash = SAPAnnouncer::messageIdHash(kSDP);
    const auto packet =
        SAPAnnouncer::buildPacket(kSDP, hash, sourceOf("192.168.1.20"), false);

    const DaemonSapRead read = daemonSapRead(packet.data(), packet.size());

    INFO("refused because: ", read.refusal);
    REQUIRE(read.accepted);
    CHECK(read.isAnnouncement);
    CHECK(read.sdp == kSDP);
}

TEST_CASE("A deletion of ours is a deletion to the daemon") {
    const uint16_t hash = SAPAnnouncer::messageIdHash(kSDP);
    const auto packet =
        SAPAnnouncer::buildPacket(kSDP, hash, sourceOf("192.168.1.20"), true);

    const DaemonSapRead read = daemonSapRead(packet.data(), packet.size());

    REQUIRE(read.accepted);
    CHECK_FALSE(read.isAnnouncement);
    CHECK(read.sdp == kSDP);
}

TEST_CASE("Without the payload type the daemon drops the packet") {
    // What this driver used to send: the eight-byte header and the body. The
    // daemon reads offset 8 for its type and finds "v=0\n", so it returns
    // false and the session is never seen -- no log line, no rejection, the
    // announcement simply does not exist for it.
    const uint16_t hash = SAPAnnouncer::messageIdHash(kSDP);
    auto packet = SAPAnnouncer::buildPacket(kSDP, hash, sourceOf("192.168.1.20"), false);
    packet.erase(packet.begin() + 8, packet.begin() + 24);

    const DaemonSapRead read = daemonSapRead(packet.data(), packet.size());

    CHECK_FALSE(read.accepted);
    CHECK(read.refusal.find("application/sdp") != std::string::npos);
}

TEST_CASE("What the daemon sends, this driver's listener reads") {
    const auto packet = daemonSapPacket(kSDP, 0xBEEF, sourceOf("192.168.1.30"), false);

    const SAPAnnouncement heard = SAPListener::parseAnnouncement(
        reinterpret_cast<const char*>(packet.data()), packet.size(), "192.168.1.30");

    CHECK_FALSE(heard.isDeletion);
    CHECK(heard.sessionDescription == kSDP);
    CHECK(heard.sessionName == "Driver Out 1");
    CHECK(heard.multicastAddress == "239.69.0.1/32");
    CHECK(heard.port == 5004);
}

TEST_CASE("The two write the message id hash in opposite byte orders") {
    // Ours goes out big-endian, network order, as RFC 2974's field diagram
    // implies. The daemon memcpy's a uint16_t, so on a little-endian host it
    // writes the two bytes the other way round, and reads them back the same
    // way. Nothing breaks -- the value is opaque, each side only ever
    // compares it with its own -- but the bytes on the wire differ, and a
    // receiver matching a deletion against an announcement from the OTHER
    // implementation would be comparing 0xBEEF with 0xEFBE.
    const auto ours = SAPAnnouncer::buildPacket(kSDP, 0xBEEF, sourceOf("10.1.2.3"), false);
    const auto theirs = daemonSapPacket(kSDP, 0xBEEF, sourceOf("10.1.2.3"), false);

    CHECK(ours[2] == 0xBE);
    CHECK(ours[3] == 0xEF);

    const uint16_t asWritten = static_cast<uint16_t>(theirs[2] | (theirs[3] << 8));
    CHECK(asWritten == 0xBEEF);  // little-endian on every host this builds for

    // Each side reads its own back correctly, which is why this is a
    // difference and not a defect.
    CHECK(daemonSapRead(theirs.data(), theirs.size()).msgIdHash == 0xBEEF);
    CHECK(SAPListener::parseAnnouncement(reinterpret_cast<const char*>(ours.data()),
                                         ours.size(), "10.1.2.3")
              .msgIdHash == 0xBEEF);
}

TEST_CASE("The headers are the same length, and say the same things") {
    const auto ours = SAPAnnouncer::buildPacket(kSDP, 1, sourceOf("10.1.2.3"), false);
    const auto theirs = daemonSapPacket(kSDP, 1, sourceOf("10.1.2.3"), false);

    REQUIRE(ours.size() == theirs.size());
    CHECK(ours[0] == theirs[0]);   // 0x20: version 1, IPv4, announcement
    CHECK(ours[1] == theirs[1]);   // no authentication data
    for (size_t i = 4; i < kDaemonSapHeaderLen; ++i) {
        INFO("byte ", i);
        CHECK(ours[i] == theirs[i]);  // address, then the payload type
    }
}

} // namespace Tests
} // namespace AES67
