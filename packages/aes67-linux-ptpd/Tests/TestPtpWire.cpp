//
// TestPtpWire.cpp
// AES67 Linux PTP daemon
// The bytes, checked against IEEE 1588-2008 sec 13 offset by offset.
//
// This is the half of the daemon that can be tested without a network, and it
// is the half where a mistake is invisible on the wire: a slave will not
// complain about a Follow_Up whose timestamp sits two octets late, it will
// just be wrong about the time.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "PtpWire.h"

#include <cstring>

using namespace AES67;
using namespace AES67::LinuxPtpd;

namespace {

PortContext testPort() {
    PortContext port;
    port.clockIdentity.id = {0xB8, 0x27, 0xEB, 0xFF, 0xFE, 0x01, 0x02, 0x03};
    port.portNumber = 1;
    port.domainNumber = 0;
    port.majorSdoId = 0;
    return port;
}

uint16_t read16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint64_t readTimestamp(const uint8_t* p) {
    uint64_t seconds = static_cast<uint64_t>(read16(p)) << 32;
    seconds |= (static_cast<uint64_t>(p[2]) << 24) | (static_cast<uint64_t>(p[3]) << 16) |
               (static_cast<uint64_t>(p[4]) << 8) | static_cast<uint64_t>(p[5]);
    const uint64_t nanos = (static_cast<uint64_t>(p[6]) << 24) |
                           (static_cast<uint64_t>(p[7]) << 16) |
                           (static_cast<uint64_t>(p[8]) << 8) |
                           static_cast<uint64_t>(p[9]);
    return seconds * 1000000000ULL + nanos;
}

}  // namespace

TEST_CASE("The header is the 34 octets of sec 13.3") {
    uint8_t message[kSyncSize];
    PortContext port = testPort();
    port.domainNumber = 7;
    port.majorSdoId = 1;

    REQUIRE(buildSync(message, sizeof(message), port, 0x1234, -3) == kSyncSize);

    CHECK(message[0] == 0x10);  // majorSdoId 1, messageType Sync 0
    CHECK(message[1] == 0x02);  // versionPTP 2
    CHECK(read16(message + 2) == kSyncSize);
    CHECK(message[4] == 7);
    CHECK(read16(message + 30) == 0x1234);
    CHECK(message[32] == kControlSync);
    CHECK(static_cast<int8_t>(message[33]) == -3);
    CHECK(std::memcmp(message + 20, port.clockIdentity.id.data(), 8) == 0);
    CHECK(read16(message + 28) == 1);

    // correctionField and both reserved fields: a master at the top of the
    // hierarchy corrects nothing.
    for (size_t i = 8; i < 20; ++i) CHECK(message[i] == 0);
}

TEST_CASE("A Sync says two-step and carries no time") {
    uint8_t message[kSyncSize];
    REQUIRE(buildSync(message, sizeof(message), testPort(), 1, 0) == kSyncSize);

    CHECK((read16(message + 6) & kFlagTwoStep) == kFlagTwoStep);
    CHECK((read16(message + 6) & kFlagPtpTimescale) == kFlagPtpTimescale);
    // The origin timestamp of a two-step Sync is meaningless and is sent as
    // zero: the Follow_Up is what carries the time.
    for (size_t i = 34; i < kSyncSize; ++i) CHECK(message[i] == 0);
}

TEST_CASE("The Follow_Up carries the transmit time and the Sync's sequence") {
    uint8_t message[kFollowUpSize];
    const uint64_t transmitNs = 1757203200123456789ULL;

    REQUIRE(buildFollowUp(message, sizeof(message), testPort(), 42, -3, transmitNs) ==
            kFollowUpSize);

    CHECK(read16(message + 30) == 42);
    CHECK(message[32] == kControlFollowUp);
    CHECK((read16(message + 6) & kFlagTwoStep) == 0);
    CHECK(readTimestamp(message + 34) == transmitNs);
}

TEST_CASE("The Delay_Resp answers one requester, by sequence and identity") {
    PTPPortIdentity requester;
    requester.clockIdentity.id = {1, 2, 3, 4, 5, 6, 7, 8};
    requester.portNumber = 9;

    uint8_t message[kDelayRespSize];
    const uint64_t receiveNs = 42000000123ULL;
    REQUIRE(buildDelayResp(message, sizeof(message), testPort(), requester, 77, -3,
                           receiveNs) == kDelayRespSize);

    CHECK(read16(message + 2) == kDelayRespSize);
    CHECK(read16(message + 30) == 77);
    CHECK(message[32] == kControlDelayResp);
    CHECK(readTimestamp(message + 34) == receiveNs);
    CHECK(std::memcmp(message + 44, requester.clockIdentity.id.data(), 8) == 0);
    CHECK(read16(message + 52) == 9);
}

TEST_CASE("The Announce carries the dataset a BMCA compares") {
    AnnounceDataset dataset;
    dataset.clockIdentity = testPort().clockIdentity;
    dataset.priority1 = 200;
    dataset.priority2 = 128;
    dataset.clockClass = 248;
    dataset.clockAccuracy = 0xFE;
    dataset.offsetScaledLogVariance = 0xFFFF;
    dataset.stepsRemoved = 0;
    dataset.timeSource = 0xA0;
    dataset.currentUtcOffset = 37;

    uint8_t message[kAnnounceSize];
    REQUIRE(buildAnnounce(message, sizeof(message), testPort(), dataset, 5, 0,
                          1000000000ULL) == kAnnounceSize);

    CHECK(read16(message + 2) == kAnnounceSize);
    CHECK(message[32] == kControlOther);
    CHECK(static_cast<int16_t>(read16(message + 44)) == 37);
    CHECK(message[47] == 200);
    CHECK(message[48] == 248);
    CHECK(message[49] == 0xFE);
    CHECK(read16(message + 50) == 0xFFFF);
    CHECK(message[52] == 128);
    CHECK(std::memcmp(message + 53, dataset.clockIdentity.id.data(), 8) == 0);
    CHECK(read16(message + 61) == 0);
    CHECK(message[63] == 0xA0);
}

TEST_CASE("An unset UTC offset is announced as not valid") {
    AnnounceDataset dataset;
    dataset.currentUtcOffsetValid = false;

    uint8_t message[kAnnounceSize];
    REQUIRE(buildAnnounce(message, sizeof(message), testPort(), dataset, 0, 0, 0) ==
            kAnnounceSize);
    CHECK((read16(message + 6) & kFlagCurrentUtcOffsetValid) == 0);

    dataset.currentUtcOffsetValid = true;
    REQUIRE(buildAnnounce(message, sizeof(message), testPort(), dataset, 0, 0, 0) ==
            kAnnounceSize);
    CHECK((read16(message + 6) & kFlagCurrentUtcOffsetValid) == kFlagCurrentUtcOffsetValid);
}

TEST_CASE("Nothing is written into a buffer that cannot hold the message") {
    uint8_t small[kSyncSize - 1];
    std::memset(small, 0xAA, sizeof(small));

    CHECK(buildSync(small, sizeof(small), testPort(), 1, 0) == 0);
    for (size_t i = 0; i < sizeof(small); ++i) CHECK(small[i] == 0xAA);
}

TEST_CASE("parseHeader reads back what the builders wrote") {
    uint8_t message[kAnnounceSize];
    PortContext port = testPort();
    port.domainNumber = 3;
    AnnounceDataset dataset;
    dataset.clockIdentity = port.clockIdentity;

    REQUIRE(buildAnnounce(message, sizeof(message), port, dataset, 9, 1, 0) ==
            kAnnounceSize);

    PTPHeader header{};
    REQUIRE(parseHeader(message, kAnnounceSize, header));
    CHECK(header.getMessageType() == PTPMessageType::Announce);
    CHECK(header.domainNumber == 3);
    CHECK(header.sequenceId == 9);
    CHECK(header.logMessageInterval == 1);
    CHECK(header.sourcePortIdentity.portNumber == 1);
    CHECK(header.sourcePortIdentity.clockIdentity == port.clockIdentity);
}

TEST_CASE("A datagram shorter than a header is refused") {
    uint8_t message[kHeaderSize];
    std::memset(message, 0, sizeof(message));
    PTPHeader header{};
    CHECK(parseHeader(message, kHeaderSize - 1, header) == false);
    CHECK(parseHeader(nullptr, kHeaderSize, header) == false);
    CHECK(parseHeader(message, kHeaderSize, header));
}
