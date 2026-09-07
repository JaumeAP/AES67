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
