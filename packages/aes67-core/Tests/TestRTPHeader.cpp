//
// TestRTPHeader.cpp
// The RTP wire header on its own, with no transport underneath it.
//
// This suite includes RTPHeader.h and nothing else from the project. That is
// half the point: the header is part of AES67_CORE_SOURCES' promise that a
// consumer off macOS -- ESP32 firmware over lwIP, say -- can build and read RTP
// without taking the socket layer with it. If someone reintroduces a socket
// include there, this file stops compiling, which is a louder failure than a
// consumer discovering it in their own build.
//
// The other half is the byte-order helpers. They replaced htons/ntohs, which
// come from <arpa/inet.h>, so they need checking against the wire layout
// rather than against the functions they stand in for.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/RTP/RTPHeader.h"

#include <cstring>

using AES67::RTP::RTPHeader;

namespace {

/// The header as it must look on the wire, byte by byte.
RTPHeader makeHeader() {
    RTPHeader h{};
    h.version = 2;
    h.padding = 0;
    h.extension = 0;
    h.cc = 0;
    h.marker = 1;
    h.payloadType = AES67::RTP::PT_AES67_L24;
    h.sequenceNumber = 0x1234;
    h.timestamp = 0xDEADBEEF;
    h.ssrc = 0xCAFEBABE;
    return h;
}

}  // namespace

TEST_CASE("Header is exactly twelve bytes") {
    CHECK(sizeof(RTPHeader) == 12);
}

TEST_CASE("Network order matches the RFC 3550 wire layout") {
    RTPHeader h = makeHeader();
    h.toNetworkOrder();

    unsigned char wire[12];
    std::memcpy(wire, &h, sizeof(wire));

    // Byte 0: V=2, P=0, X=0, CC=0 -> 0x80
    CHECK(wire[0] == 0x80);
    // Byte 1: M=1, PT=97 -> 0x80 | 97 = 0xE1
    CHECK(wire[1] == 0xE1);
    // Bytes 2-3: sequence number, big-endian
    CHECK(wire[2] == 0x12);
    CHECK(wire[3] == 0x34);
    // Bytes 4-7: timestamp, big-endian
    CHECK(wire[4] == 0xDE);
    CHECK(wire[5] == 0xAD);
    CHECK(wire[6] == 0xBE);
    CHECK(wire[7] == 0xEF);
    // Bytes 8-11: SSRC, big-endian
    CHECK(wire[8] == 0xCA);
    CHECK(wire[9] == 0xFE);
    CHECK(wire[10] == 0xBA);
    CHECK(wire[11] == 0xBE);
}

TEST_CASE("Host order is the exact inverse of network order") {
    const RTPHeader original = makeHeader();
    RTPHeader h = original;

    h.toNetworkOrder();
    h.toHostOrder();

    CHECK(h.sequenceNumber == original.sequenceNumber);
    CHECK(h.timestamp == original.timestamp);
    CHECK(h.ssrc == original.ssrc);
    CHECK(h.payloadType == original.payloadType);
    CHECK(h.marker == original.marker);
    CHECK(h.version == original.version);
}

TEST_CASE("A header read off the wire decodes to its fields") {
    // Same bytes as the layout test, arriving from the network this time.
    const unsigned char wire[12] = {0x80, 0xE1, 0x12, 0x34, 0xDE, 0xAD,
                                    0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
    RTPHeader h{};
    std::memcpy(&h, wire, sizeof(wire));
    h.toHostOrder();

    CHECK(h.version == 2);
    CHECK(h.marker == 1);
    CHECK(h.payloadType == AES67::RTP::PT_AES67_L24);
    CHECK(h.sequenceNumber == 0x1234);
    CHECK(h.timestamp == 0xDEADBEEF);
    CHECK(h.ssrc == 0xCAFEBABE);
}
