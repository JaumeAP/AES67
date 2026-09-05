//
// TestSAPAnnouncer.cpp
// AES67 macOS Driver
//
// What this driver puts on the SAP groups, read back by the parser that has
// to make sense of it.
//
// Announcing was at zero coverage until 2026-09-04, and it is the half of
// discovery nobody notices is broken from this side: a receiver that cannot
// read our header simply never lists us. So every packet built here is
// parsed by SAPListener, which is the same code path a remote AES67 device
// runs — the closest thing to a round trip that does not need a network.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/SAPAnnouncer.h"
#include "NetworkEngine/Discovery/SAPListener.h"

#include <arpa/inet.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace AES67;

namespace {

const std::string kSDP =
    "v=0\r\n"
    "o=- 1 1 IN IP4 192.168.1.20\r\n"
    "s=Driver Out 1\r\n"
    "c=IN IP4 239.69.0.1/32\r\n"
    "t=0 0\r\n"
    "m=audio 5004 RTP/AVP 97\r\n"
    "a=rtpmap:97 L24/48000/8\r\n";

uint32_t sourceOf(const char* dotted) { return ::inet_addr(dotted); }

SAPAnnouncement roundTrip(const std::vector<uint8_t>& packet,
                          const std::string& from = "192.168.1.20") {
    return SAPListener::parseAnnouncement(reinterpret_cast<const char*>(packet.data()),
                                          packet.size(), from);
}

} // namespace

TEST_CASE("What we announce is what a listener reads") {
    const uint16_t hash = SAPAnnouncer::messageIdHash(kSDP);
    const std::vector<uint8_t> packet =
        SAPAnnouncer::buildPacket(kSDP, hash, sourceOf("192.168.1.20"), /*deletion=*/false);

    const SAPAnnouncement heard = roundTrip(packet);

    CHECK_FALSE(heard.isDeletion);
    CHECK(heard.sessionDescription == kSDP);
    CHECK(heard.sessionName == "Driver Out 1");
    CHECK(heard.multicastAddress == "239.69.0.1/32");
    CHECK(heard.port == 5004);
    CHECK(heard.msgIdHash == hash);
    CHECK(heard.originatingSource == 0xC0A80114u);  // 192.168.1.20, host order
}

TEST_CASE("A deletion is heard as one, and identifies what it withdraws") {
    // The deletion has to carry the hash the announcement went out with, or
    // the receiver cannot tell which session is being taken away and waits
    // out the timeout instead.
    const uint16_t hash = SAPAnnouncer::messageIdHash(kSDP);
    const std::vector<uint8_t> withdrawal =
        SAPAnnouncer::buildPacket(kSDP, hash, sourceOf("192.168.1.20"), /*deletion=*/true);

    const SAPAnnouncement heard = roundTrip(withdrawal);

    CHECK(heard.isDeletion);
    CHECK(heard.msgIdHash == hash);
    CHECK(heard.originatingSource == 0xC0A80114u);
}

TEST_CASE("The header is the one RFC 2974 describes") {
    const std::vector<uint8_t> packet =
        SAPAnnouncer::buildPacket(kSDP, 0xBEEF, sourceOf("10.1.2.3"), false);

    REQUIRE(packet.size() > 8);
    CHECK(((packet[0] >> 5) & 0x07) == 1);  // version 1
    CHECK(((packet[0] >> 4) & 0x01) == 0);  // address type: IPv4
    CHECK(((packet[0] >> 2) & 0x01) == 0);  // announcement, not deletion
    CHECK((packet[0] & 0x03) == 0);         // neither encrypted nor compressed
    CHECK(packet[1] == 0);                  // no authentication data
    CHECK(packet[2] == 0xBE);
    CHECK(packet[3] == 0xEF);
    CHECK(packet[4] == 10);
    CHECK(packet[5] == 1);
    CHECK(packet[6] == 2);
    CHECK(packet[7] == 3);

    // The body follows the eight-byte header with no payload-type prefix.
    const std::string body(packet.begin() + 8, packet.end());
    CHECK(body == kSDP);

    std::vector<uint8_t> deletion =
        SAPAnnouncer::buildPacket(kSDP, 0xBEEF, sourceOf("10.1.2.3"), true);
    CHECK(((deletion[0] >> 2) & 0x01) == 1);
}

TEST_CASE("The message id hash follows the content, and only the content") {
    // RFC 2974 §6: stable while the session is unchanged, different when it
    // changes. A hash that moved on its own would announce every repeat as a
    // new session; one that never moved would leave an edited session
    // looking untouched.
    CHECK(SAPAnnouncer::messageIdHash(kSDP) == SAPAnnouncer::messageIdHash(kSDP));

    std::string edited = kSDP;
    edited.replace(edited.find("5004"), 4, "5006");
    CHECK(SAPAnnouncer::messageIdHash(edited) != SAPAnnouncer::messageIdHash(kSDP));

    CHECK(SAPAnnouncer::messageIdHash("") == SAPAnnouncer::messageIdHash(""));
}

TEST_CASE("Two sessions announce under two identities") {
    // Distinct bodies have to reach a listener as distinct sessions, which
    // is what the hash is for: the listener keys on it before it looks at
    // any name.
    std::string second = kSDP;
    second.replace(second.find("Driver Out 1"), 12, "Driver Out 2");
    second.replace(second.find("239.69.0.1"), 10, "239.69.0.2");

    const SAPAnnouncement first = roundTrip(SAPAnnouncer::buildPacket(
        kSDP, SAPAnnouncer::messageIdHash(kSDP), sourceOf("192.168.1.20"), false));
    const SAPAnnouncement other = roundTrip(SAPAnnouncer::buildPacket(
        second, SAPAnnouncer::messageIdHash(second), sourceOf("192.168.1.20"), false));

    CHECK(first.msgIdHash != other.msgIdHash);
    CHECK(first.sessionName == "Driver Out 1");
    CHECK(other.sessionName == "Driver Out 2");
    CHECK(other.multicastAddress == "239.69.0.2/32");
}

TEST_CASE("An unknown interface announces as 0.0.0.0, and still parses") {
    // initialize() with no interface leaves the originating source at zero.
    // That is a worse identity than an address, but it must not produce a
    // packet a receiver throws away.
    const std::vector<uint8_t> packet =
        SAPAnnouncer::buildPacket(kSDP, SAPAnnouncer::messageIdHash(kSDP), 0, false);

    const SAPAnnouncement heard = roundTrip(packet);
    CHECK(heard.sessionDescription == kSDP);
    CHECK(heard.originatingSource == 0u);
}
