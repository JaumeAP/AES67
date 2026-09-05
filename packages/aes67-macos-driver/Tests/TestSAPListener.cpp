//
// TestSAPListener.cpp
// AES67 macOS Driver
//
// SAP announcement parsing (RFC 2974), which until now had no test of its
// own — the audit of 2026-09-04 counted it among the components at zero
// coverage, and it is the one that reads bytes from a multicast group any
// host on the network can send to.
//
// The refusals are the point. A parser that accepts a truncated header, a
// version it does not implement, or an auth length that runs past the end of
// the datagram hands the rest of the driver a session it invented.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/Discovery/SAPListener.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace AES67;

namespace {

const std::string kSDP =
    "v=0\r\n"
    "o=- 1 1 IN IP4 192.168.1.50\r\n"
    "s=Studio Mic 1\r\n"
    "c=IN IP4 239.1.1.1/32\r\n"
    "t=0 0\r\n"
    "m=audio 5004 RTP/AVP 97\r\n"
    "a=rtpmap:97 L24/48000/8\r\n";

/// A SAP datagram: version 1, `authWords` 32-bit words of authentication
/// data, and `body` as the payload. `deletion` sets the type bit.
std::vector<char> buildSAP(const std::string& body, uint8_t authWords = 0,
                           bool deletion = false, uint8_t version = 1) {
    std::vector<char> packet;
    uint8_t flags = static_cast<uint8_t>(version << 5);
    if (deletion) flags |= 0x04;
    packet.push_back(static_cast<char>(flags));
    packet.push_back(static_cast<char>(authWords));
    packet.push_back(static_cast<char>(0x12));  // message id hash, high
    packet.push_back(static_cast<char>(0x34));  // message id hash, low
    packet.push_back(static_cast<char>(192));   // originating source
    packet.push_back(static_cast<char>(168));
    packet.push_back(static_cast<char>(1));
    packet.push_back(static_cast<char>(50));
    for (int i = 0; i < authWords * 4; ++i) packet.push_back(static_cast<char>(0xAA));
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

SAPAnnouncement parse(const std::vector<char>& packet,
                      const std::string& from = "192.168.1.50") {
    return SAPListener::parseAnnouncement(packet.data(), packet.size(), from);
}

} // namespace

TEST_CASE("An announcement yields the session it describes") {
    const SAPAnnouncement announced = parse(buildSAP(kSDP));

    CHECK(announced.sessionDescription == kSDP);
    CHECK(announced.sessionName == "Studio Mic 1");
    CHECK(announced.multicastAddress == "239.1.1.1/32");
    CHECK(announced.port == 5004);
    CHECK(announced.sourceAddress == "192.168.1.50");
    CHECK_FALSE(announced.isDeletion);
}

TEST_CASE("Identity comes from the header, not from the body") {
    // The message id hash and originating source are what the listener
    // matches an announcement and its later deletion on, so they have to
    // survive a body that carries no name at all.
    const SAPAnnouncement announced = parse(buildSAP(kSDP));
    CHECK(announced.msgIdHash == 0x1234);
    CHECK(announced.originatingSource == 0xC0A80132u);

    const std::string nameless =
        "v=0\r\nc=IN IP4 239.1.1.2\r\nm=audio 5006 RTP/AVP 97\r\n";
    const SAPAnnouncement anonymous = parse(buildSAP(nameless));
    CHECK(anonymous.sessionName.empty());
    CHECK(anonymous.msgIdHash == 0x1234);
    CHECK(anonymous.port == 5006);
}

TEST_CASE("A deletion is identified without an SDP body") {
    // A deletion carries identity and often nothing else. Reading it as a
    // malformed announcement is how the listener used to drop them, leaving
    // the session to time out instead of going when told.
    const SAPAnnouncement deletion = parse(buildSAP("", 0, /*deletion=*/true));

    CHECK(deletion.isDeletion);
    CHECK(deletion.msgIdHash == 0x1234);
    CHECK(deletion.originatingSource == 0xC0A80132u);
    CHECK(deletion.sessionDescription.empty());
}

TEST_CASE("Authentication data is skipped, not decoded") {
    // The auth block sits between the header and the payload. Its length is
    // the sender's to choose, so the payload offset moves with it.
    const SAPAnnouncement announced = parse(buildSAP(kSDP, /*authWords=*/3));

    CHECK(announced.sessionDescription == kSDP);
    CHECK(announced.port == 5004);
}

TEST_CASE("A datagram that does not add up yields nothing") {
    SAPAnnouncement result;

    // Shorter than the four-byte minimum header.
    const std::vector<char> runt = {0x20, 0x00};
    result = parse(runt);
    CHECK(result.sessionDescription.empty());
    CHECK(result.sessionName.empty());

    // A SAP version this parser does not implement.
    result = parse(buildSAP(kSDP, 0, false, /*version=*/2));
    CHECK(result.sessionDescription.empty());

    // An auth length that runs past the end of the datagram: the payload
    // would start beyond what was received. 255 words is a kilobyte of
    // authentication data the sender never sent.
    std::vector<char> overrun = buildSAP(kSDP, /*authWords=*/255);
    overrun.resize(64);
    result = parse(overrun);
    CHECK(result.sessionDescription.empty());

    // A payload that is not an SDP at all.
    result = parse(buildSAP("this is not a session description"));
    CHECK(result.sessionDescription.empty());

    // A payload too short to be one.
    result = parse(buildSAP("v=0"));
    CHECK(result.sessionDescription.empty());
}

TEST_CASE("Encrypted and compressed announcements are refused") {
    // Neither is supported, and guessing at the bytes behind either flag
    // would mean parsing something other than what was sent.
    std::vector<char> encrypted = buildSAP(kSDP);
    encrypted[0] = static_cast<char>(static_cast<uint8_t>(encrypted[0]) | 0x02);
    CHECK(parse(encrypted).sessionDescription.empty());

    std::vector<char> compressed = buildSAP(kSDP);
    compressed[0] = static_cast<char>(static_cast<uint8_t>(compressed[0]) | 0x01);
    CHECK(parse(compressed).sessionDescription.empty());
}

TEST_CASE("The announcer's address is recorded, never inferred") {
    // The driver compares this against the SDP's own origin before letting
    // an announcement re-point a receiver, so it has to be the address the
    // datagram actually came from rather than anything inside it.
    const SAPAnnouncement announced = parse(buildSAP(kSDP), "10.9.9.9");
    CHECK(announced.sourceAddress == "10.9.9.9");
    CHECK(announced.sessionDescription.find("192.168.1.50") != std::string::npos);
}

TEST_CASE("A body without a trailing newline still parses") {
    // SDP bodies arrive both ways, and the last line is where the media
    // port lives often enough to matter.
    const std::string unterminated =
        "v=0\r\ns=Last Line\r\nc=IN IP4 239.1.1.9\r\nm=audio 5008 RTP/AVP 97";
    const SAPAnnouncement announced = parse(buildSAP(unterminated));

    CHECK(announced.sessionName == "Last Line");
    CHECK(announced.port == 5008);
}

TEST_CASE("A media port that is not a number does not become one") {
    const std::string bad = "v=0\r\ns=Bad Port\r\nm=audio nowhere RTP/AVP 97\r\n";
    const SAPAnnouncement announced = parse(buildSAP(bad));

    CHECK(announced.sessionName == "Bad Port");
    CHECK(announced.port == 0);
}
