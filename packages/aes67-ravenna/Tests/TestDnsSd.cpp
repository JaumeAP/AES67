//
// TestDnsSd.cpp
// AES67 RAVENNA session layer
// The advertisement, byte for byte.
//
// A DNS record that is one byte wrong is not a record that half works: the
// resolver drops the packet and the session simply never appears, with
// nothing anywhere saying why.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Ravenna/DnsSd.h"

#include <cstring>

using namespace AES67::Ravenna;

namespace {

SessionAdvertisement testSession() {
    SessionAdvertisement session;
    session.instanceName = "Mix A";
    session.hostName = "box.local";
    session.port = 8554;
    session.addressV4 = 0xC0A80132;  // 192.168.1.50
    session.txtEntries = {"txtvers=1", "path=/by-name/Mix A"};
    return session;
}

uint16_t read16(const std::vector<uint8_t>& packet, size_t offset) {
    return static_cast<uint16_t>((packet[offset] << 8) | packet[offset + 1]);
}

/// Where a name's bytes start inside the packet, or npos.
size_t find(const std::vector<uint8_t>& packet, const std::vector<uint8_t>& needle) {
    if (needle.empty() || packet.size() < needle.size()) return std::string::npos;
    for (size_t i = 0; i + needle.size() <= packet.size(); ++i) {
        if (std::memcmp(packet.data() + i, needle.data(), needle.size()) == 0) return i;
    }
    return std::string::npos;
}

}  // namespace

TEST_CASE("A name is length-prefixed labels ending in a zero") {
    const std::vector<uint8_t> encoded = encodeName("box.local");
    const std::vector<uint8_t> expected = {3, 'b', 'o', 'x', 5, 'l', 'o', 'c', 'a', 'l', 0};
    CHECK(encoded == expected);

    // The root, and a trailing dot, are the same zero byte.
    CHECK(encodeName("") == std::vector<uint8_t>{0});
    CHECK(encodeName("local.") == encodeName("local"));
}

TEST_CASE("The header says response, authoritative, and how many answers") {
    const std::vector<uint8_t> packet = buildAnnouncement(testSession());

    REQUIRE(packet.size() > 12);
    CHECK(read16(packet, 0) == 0);       // no transaction id in an mDNS response
    CHECK(read16(packet, 2) == 0x8400);  // response + authoritative
    CHECK(read16(packet, 4) == 0);       // no questions
    CHECK(read16(packet, 6) == 5);       // PTR, subtype PTR, SRV, TXT, A
    CHECK(read16(packet, 8) == 0);
    CHECK(read16(packet, 10) == 0);
}

TEST_CASE("Without the subtype there is one PTR fewer") {
    const std::vector<uint8_t> packet = buildAnnouncement(testSession(), false);
    CHECK(read16(packet, 6) == 4);
    CHECK(find(packet, encodeName(kRavennaSessionSubtype)) == std::string::npos);
}

TEST_CASE("The names a RAVENNA device browses for are in there") {
    const std::vector<uint8_t> packet = buildAnnouncement(testSession());

    CHECK(find(packet, encodeName(kRtspService)) != std::string::npos);
    CHECK(find(packet, encodeName(kRavennaSessionSubtype)) != std::string::npos);
    CHECK(find(packet, encodeName("Mix A._rtsp._tcp.local")) != std::string::npos);
    CHECK(find(packet, encodeName("box.local")) != std::string::npos);
}

TEST_CASE("The SRV carries the port the RTSP server is actually on") {
    const std::vector<uint8_t> packet = buildAnnouncement(testSession());

    // The SRV's data is priority, weight, port, target: the port sits two
    // 16-bit fields into the record's data.
    const size_t instance = find(packet, encodeName("Mix A._rtsp._tcp.local"));
    REQUIRE(instance != std::string::npos);

    bool found = false;
    for (size_t i = instance; i + 1 < packet.size(); ++i) {
        if (read16(packet, i) == 8554) found = true;
    }
    CHECK(found);
}

TEST_CASE("An SRV's target name is read only from inside its own declared length") {
    // readName() used to be bounded by the whole packet here, not this
    // record's own data length -- the same reasoning the TXT branch right
    // below already gets right. A response whose SRV dataLength understates
    // its real target name (malformed, or crafted) let the reader walk
    // straight past the record's own boundary into whatever bytes came
    // next and hand those back as the resolved host.
    //
    // This patches one real packet's SRV dataLength down to 6 -- priority,
    // weight and port, and no room left for a target name at all -- without
    // touching the target name bytes physically still sitting right after
    // it. The fixed reader has nothing to read inside that shrunk boundary
    // and must refuse; the bug this pins is a reader that keeps going
    // regardless and finds the real name a few bytes past where the record
    // said it ended.
    const SessionAdvertisement session = testSession();
    std::vector<uint8_t> packet = buildAnnouncement(session);

    // The port sits at data+4 (priority, weight, then port), same as "The
    // SRV carries the port the RTSP server is actually on" above -- found
    // the same way, by its literal value, rather than by the owner name's
    // bytes: those also appear as both PTRs' RDATA, earlier in the packet,
    // so anchoring on the first match of the name finds a PTR, not the SRV.
    size_t portAt = std::string::npos;
    for (size_t i = 0; i + 1 < packet.size(); ++i) {
        if (read16(packet, i) == session.port) {
            portAt = i;
            break;
        }
    }
    REQUIRE(portAt != std::string::npos);
    const size_t data = portAt - 4;         // priority(2) + weight(2) before port
    const size_t dataLengthAt = data - 2;   // dataLength(2) right before the data
    const uint16_t realDataLength = read16(packet, dataLengthAt);
    REQUIRE(realDataLength > 6);  // there really is a target name past it

    // Shrink only the SRV record's own declared length. The real target
    // name bytes are left exactly where they are -- physically still in the
    // packet, still readable by the loop's own `length` bound -- which is
    // the point: they no longer belong to this record's own span, and the
    // fixed reader has to notice.
    packet[dataLengthAt] = 0;
    packet[dataLengthAt + 1] = 6;

    // The answer count at offset 6 (fixed header, RFC 1035) is trimmed to
    // match: PTR, subtype PTR, SRV, and no further record. Otherwise the
    // loop would try to read a fourth record starting where the (still
    // present) real target name bytes are, and get lost in that on its own
    // account -- a second, broader question about how this parser recovers
    // from a corrupted dataLength in general, not the one this pins.
    REQUIRE(read16(packet, 6) == 5);
    packet[6] = 0;
    packet[7] = 3;

    const auto services = parseServiceResponse(packet.data(), packet.size(), kRtspService);
    REQUIRE(services.size() == 1);
    // hostName defaults to empty; the fixed reader leaves it that way rather
    // than reading past dataLength to find "box.local" still sitting there.
    CHECK(services[0].hostName.empty());
}

TEST_CASE("An empty TXT is one zero-length string, not an empty record") {
    // RFC 6763 sec 6.1. A resolver that sees a zero-length TXT record treats
    // the service as absent, so this is the difference between a session
    // appearing and not.
    SessionAdvertisement session = testSession();
    session.txtEntries.clear();
    const std::vector<uint8_t> packet = buildAnnouncement(session);

    const size_t txtType = [&]() -> size_t {
        for (size_t i = 12; i + 1 < packet.size(); ++i) {
            if (read16(packet, i) == kTypeTXT && read16(packet, i + 2) == (kClassIN | kCacheFlush)) {
                return i;
            }
        }
        return std::string::npos;
    }();
    REQUIRE(txtType != std::string::npos);
    CHECK(read16(packet, txtType + 8) == 1);   // one byte of data
    CHECK(packet[txtType + 10] == 0);          // and it is a zero length
}

TEST_CASE("A goodbye is the same records with a zero TTL") {
    const std::vector<uint8_t> announcement = buildAnnouncement(testSession());
    const std::vector<uint8_t> goodbye = buildGoodbye(testSession());

    CHECK(goodbye.size() == announcement.size());
    CHECK(read16(goodbye, 6) == read16(announcement, 6));
    CHECK(announcement != goodbye);

    // No TTL anywhere in it is anything but zero: that is what withdraws a
    // service now instead of in 75 minutes.
    for (size_t i = 0; i + 3 < goodbye.size(); ++i) {
        if (goodbye[i] == 0x11 && goodbye[i + 1] == 0x94) {
            FAIL("a goodbye still carries the 4500 second TTL");
        }
    }
}

TEST_CASE("A query is read for what it asks about") {
    // OPTIONS-style browse for _rtsp._tcp.local: header, one question.
    std::vector<uint8_t> query = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    const std::vector<uint8_t> name = encodeName("_RTSP._TCP.local");
    query.insert(query.end(), name.begin(), name.end());
    query.insert(query.end(), {0, 12, 0, 1});  // PTR, IN

    const std::vector<std::string> names = parseQueryNames(query.data(), query.size());
    REQUIRE(names.size() == 1);
    // Lowercased, because DNS names are case insensitive and a device that
    // shouts is still asking the same question.
    CHECK(names[0] == "_rtsp._tcp.local");
}

TEST_CASE("A response is not read as a query, and rubbish is not read at all") {
    std::vector<uint8_t> response = buildAnnouncement(testSession());
    CHECK(parseQueryNames(response.data(), response.size()).empty());

    CHECK(parseQueryNames(nullptr, 0).empty());
    const std::vector<uint8_t> tooShort = {0, 0, 0, 0};
    CHECK(parseQueryNames(tooShort.data(), tooShort.size()).empty());

    // A question whose label runs past the end of the packet.
    std::vector<uint8_t> truncated = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 40, 'x'};
    CHECK(parseQueryNames(truncated.data(), truncated.size()).empty());
}

TEST_CASE("An instance name is one label, and a dot in it makes two") {
    // Why an advertised instance name must not carry a dot: the encoder reads
    // one as a label separator, so the name arrives as two labels in front of
    // the service type and a resolver lists nothing.
    const std::vector<uint8_t> spaced = encodeName("aes67 local");
    REQUIRE(spaced.size() == 13);
    CHECK(spaced[0] == 11);

    const std::vector<uint8_t> dotted = encodeName("aes67.local");
    REQUIRE(dotted.size() == 13);
    CHECK(dotted[0] == 5);
    CHECK(dotted[6] == 5);
}

TEST_CASE("What this package announces, it reads back as a service") {
    // The announcement side is the only responder this package can be sure
    // of, so the round trip is what says the two halves agree on the bytes.
    SessionAdvertisement registry;
    registry.instanceName = "Registry 1";
    registry.hostName = "reg.local";
    registry.port = 8010;
    registry.addressV4 = 0xC0A8000A;  // 192.168.0.10
    registry.txtEntries = {"api_ver=v1.3", "api_proto=http", "pri=10"};
    registry.serviceType = kNmosRegisterService;
    registry.subtype.clear();

    const std::vector<uint8_t> packet = buildAnnouncement(registry);
    const std::vector<DiscoveredService> found =
        parseServiceResponse(packet.data(), packet.size(), kNmosRegisterService);

    REQUIRE(found.size() == 1);
    CHECK(found[0].hostName == "reg.local");
    CHECK(found[0].port == 8010);
    CHECK(found[0].addressV4 == 0xC0A8000A);
    CHECK(found[0].txt("pri") == "10");
    CHECK(found[0].txt("api_ver") == "v1.3");
    // A key nobody advertised is the fallback, not an empty string that looks
    // like an answer.
    CHECK(found[0].txt("api_auth", "false") == "false");
}

TEST_CASE("A PTR with nothing beside it still names the instance") {
    // The SRV, the TXT and the A of a service are not required to arrive in
    // the same packet as the PTR that named it. A browser that dropped the
    // name would have nothing to attach them to when they came.
    std::vector<uint8_t> ptrOnly = {0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    const std::vector<uint8_t> owner = encodeName(kNmosRegisterService);
    ptrOnly.insert(ptrOnly.end(), owner.begin(), owner.end());
    for (const uint8_t byte : {0, 12, 0, 1, 0, 0, 0x11, 0x94}) ptrOnly.push_back(byte);
    const std::vector<uint8_t> instance =
        encodeName(std::string("Registry 1.") + kNmosRegisterService);
    ptrOnly.push_back(0);
    ptrOnly.push_back(static_cast<uint8_t>(instance.size()));
    ptrOnly.insert(ptrOnly.end(), instance.begin(), instance.end());

    const std::vector<DiscoveredService> found =
        parseServiceResponse(ptrOnly.data(), ptrOnly.size(), kNmosRegisterService);
    REQUIRE(found.size() == 1);
    CHECK(found[0].instanceName == std::string("registry 1.") + kNmosRegisterService);
    CHECK(found[0].port == 0);
}

TEST_CASE("Another service's records are not this service's") {
    SessionAdvertisement node;
    node.instanceName = "Mix A";
    node.hostName = "box.local";
    node.port = 8080;
    node.serviceType = kNmosNodeService;
    node.subtype.clear();

    const std::vector<uint8_t> packet = buildAnnouncement(node);
    CHECK(parseServiceResponse(packet.data(), packet.size(), kNmosRegisterService).empty());
    CHECK(parseServiceResponse(packet.data(), packet.size(), kNmosNodeService).size() == 1);
}

TEST_CASE("A query is one PTR question for the service asked about") {
    const std::vector<uint8_t> packet = buildQuery(kNmosRegisterService);
    REQUIRE(packet.size() > 12);
    CHECK(packet[5] == 1);   // one question
    CHECK(packet[7] == 0);   // and no answers
    CHECK(parseQueryNames(packet.data(), packet.size()) ==
          std::vector<std::string>{kNmosRegisterService});
}

TEST_CASE("A packet off the network cannot make this loop or read past itself") {
    // Everything here is what an unfriendly responder sends, and none of it
    // may do more than return nothing.
    const std::vector<uint8_t> empty;
    CHECK(parseServiceResponse(nullptr, 0, kNmosRegisterService).empty());
    CHECK(parseServiceResponse(empty.data(), 0, kNmosRegisterService).empty());

    // A pointer at itself: the classic loop.
    std::vector<uint8_t> loop = {0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    loop.push_back(0xC0);
    loop.push_back(12);
    CHECK(parseServiceResponse(loop.data(), loop.size(), kNmosRegisterService).empty());

    // A pointer forwards, which a legal packet never has.
    std::vector<uint8_t> forwards = {0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    forwards.push_back(0xC0);
    forwards.push_back(20);
    forwards.resize(32, 0);
    CHECK(parseServiceResponse(forwards.data(), forwards.size(), kNmosRegisterService).empty());

    // A record whose data length runs off the end of the packet: reading
    // stops there, so whatever the PTR named is left with nothing to reach it
    // by rather than with half a record read past the end of the buffer.
    SessionAdvertisement service;
    service.instanceName = "Registry 1";
    service.hostName = "reg.local";
    service.port = 8010;
    service.serviceType = kNmosRegisterService;
    service.subtype.clear();
    std::vector<uint8_t> truncated = buildAnnouncement(service);
    truncated.resize(truncated.size() / 2);
    for (const DiscoveredService& partial :
         parseServiceResponse(truncated.data(), truncated.size(), kNmosRegisterService)) {
        CHECK(partial.hostName.empty());
        CHECK(partial.port == 0);
    }
}
