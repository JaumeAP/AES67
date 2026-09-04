//
// TestPTPPeerObserver.cpp
// AES67 macOS Driver
//
// Passive PTP discovery: which participants the observer reports from the
// messages it hears, and what it reads out of each one.
//
// This is how the driver notices a Dolby element that never announces itself
// at the SAP layer (see Docs/dac3202_autodetection_study.md), so the fields
// it pulls out of a message decide whether a real device is listed, listed
// twice, or missed. Watching a live segment needs a grandmaster and a
// network; deliverMessage() takes the same bytes, which is what makes any of
// this testable — the class was at zero coverage until 2026-09-04.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/PTP/PTPPeerObserver.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace AES67;

namespace {

// PTP message types, the low nibble of byte 0 (IEEE 1588-2008 §13.3.2.2).
constexpr uint8_t kSync = 0x00;
constexpr uint8_t kDelayReq = 0x01;
constexpr uint8_t kPdelayReq = 0x02;
constexpr uint8_t kFollowUp = 0x08;
constexpr uint8_t kAnnounce = 0x0B;

/// A PTP message: a 34-byte header is the minimum this reads, with the
/// message type in byte 0, the domain in byte 4 and the source clock
/// identity in bytes 20-27.
std::vector<uint8_t> ptpMessage(uint8_t messageType, int domain,
                                const std::array<uint8_t, 8>& clockId,
                                size_t length = 44) {
    std::vector<uint8_t> message(length, 0);
    message[0] = static_cast<uint8_t>(messageType & 0x0F);  // transportSpecific 0
    message[1] = 0x02;                                      // PTP version 2
    message[4] = static_cast<uint8_t>(domain);
    for (size_t i = 0; i < 8 && 20 + i < length; ++i) message[20 + i] = clockId[i];
    return message;
}

const std::array<uint8_t, 8> kGrandmaster = {0x00, 0x0B, 0x0E, 0xFF, 0xFE, 0x01, 0x02, 0x03};
const std::array<uint8_t, 8> kReceiver = {0x00, 0x90, 0x8F, 0xFF, 0xFE, 0x0A, 0x0B, 0x0C};

void deliver(PTPPeerObserver& observer, const std::vector<uint8_t>& message,
             const std::string& from) {
    observer.deliverMessage(message.data(), message.size(), from);
}

const PTPPeerObservation* find(const std::vector<PTPPeerObservation>& peers,
                               const std::array<uint8_t, 8>& clockId) {
    for (const auto& peer : peers) {
        if (peer.clockId == clockId) return &peer;
    }
    return nullptr;
}

} // namespace

TEST_CASE("A message makes a peer, identified by its clock identity") {
    PTPPeerObserver observer;
    deliver(observer, ptpMessage(kAnnounce, 0, kGrandmaster), "192.168.1.10");

    const auto peers = observer.peers();
    REQUIRE(peers.size() == 1);
    CHECK(peers[0].clockId == kGrandmaster);
    CHECK(peers[0].sourceIp == "192.168.1.10");
    CHECK(peers[0].domain == 0);
    CHECK(peers[0].messageCount == 1);
}

TEST_CASE("One clock is one peer however many messages it sends") {
    // A grandmaster sends Announce, Sync and Follow_Up continuously. Counting
    // those as separate participants would report one device as three.
    PTPPeerObserver observer;
    deliver(observer, ptpMessage(kAnnounce, 0, kGrandmaster), "192.168.1.10");
    deliver(observer, ptpMessage(kSync, 0, kGrandmaster), "192.168.1.10");
    deliver(observer, ptpMessage(kFollowUp, 0, kGrandmaster), "192.168.1.10");

    const auto peers = observer.peers();
    REQUIRE(peers.size() == 1);
    CHECK(peers[0].messageCount == 3);
}

TEST_CASE("Two clocks are two peers") {
    PTPPeerObserver observer;
    deliver(observer, ptpMessage(kAnnounce, 0, kGrandmaster), "192.168.1.10");
    deliver(observer, ptpMessage(kDelayReq, 0, kReceiver), "192.168.1.11");

    const auto peers = observer.peers();
    REQUIRE(peers.size() == 2);
    CHECK(find(peers, kGrandmaster) != nullptr);
    CHECK(find(peers, kReceiver) != nullptr);
}

TEST_CASE("What a peer sends is what says which role it plays") {
    // The point of watching: a master announces and syncs, a receiver asks
    // for delays. That is how a DAC3202 gets found without announcing itself.
    PTPPeerObserver observer;
    deliver(observer, ptpMessage(kAnnounce, 0, kGrandmaster), "192.168.1.10");
    deliver(observer, ptpMessage(kSync, 0, kGrandmaster), "192.168.1.10");
    deliver(observer, ptpMessage(kDelayReq, 0, kReceiver), "192.168.1.11");

    const auto peers = observer.peers();
    const PTPPeerObservation* master = find(peers, kGrandmaster);
    const PTPPeerObservation* slave = find(peers, kReceiver);
    REQUIRE(master != nullptr);
    REQUIRE(slave != nullptr);
    CHECK(master->role() == PTPPeerRole::Master);
    CHECK(slave->role() == PTPPeerRole::Slave);

    // A boundary clock does both, and is neither.
    deliver(observer, ptpMessage(kPdelayReq, 0, kGrandmaster), "192.168.1.10");
    const PTPPeerObservation* mixed = find(observer.peers(), kGrandmaster);
    REQUIRE(mixed != nullptr);
    CHECK(mixed->role() == PTPPeerRole::Mixed);
}

TEST_CASE("The vendor is read out of the clock identity") {
    // A PTP clock identity is the MAC as EUI-64, so its first three bytes are
    // the OUI — which is what names the vendor of a device that says nothing
    // else about itself.
    PTPPeerObserver observer;
    deliver(observer, ptpMessage(kAnnounce, 0, kGrandmaster), "192.168.1.10");

    const auto peers = observer.peers();
    REQUIRE(peers.size() == 1);
    const std::array<uint8_t, 3> expected = {0x00, 0x0B, 0x0E};
    CHECK(peers[0].oui() == expected);
}

TEST_CASE("The domain is recorded as sent") {
    // AES67 says domain 0, and gear that ignores that is exactly what an
    // operator needs to see rather than have filtered away.
    PTPPeerObserver observer;
    deliver(observer, ptpMessage(kAnnounce, 127, kGrandmaster), "192.168.1.10");

    const auto peers = observer.peers();
    REQUIRE(peers.size() == 1);
    CHECK(peers[0].domain == 127);
}

TEST_CASE("A datagram too short to be a PTP header is ignored") {
    // The clock identity lives at bytes 20-27, so anything shorter than the
    // header would be read out of memory that is not the message.
    PTPPeerObserver observer;

    const std::vector<uint8_t> runt(33, 0x0B);
    deliver(observer, runt, "192.168.1.12");
    CHECK(observer.peers().empty());

    const std::vector<uint8_t> empty;
    observer.deliverMessage(empty.data(), 0, "192.168.1.12");
    CHECK(observer.peers().empty());

    // Exactly the minimum is enough, and is read.
    deliver(observer, ptpMessage(kAnnounce, 0, kGrandmaster, /*length=*/34), "192.168.1.12");
    CHECK(observer.peers().size() == 1);
}

TEST_CASE("A clock that moves address is still one clock") {
    // The identity is the key, not the address: a device that changes
    // address, or is behind one that changes, must not appear twice.
    PTPPeerObserver observer;
    deliver(observer, ptpMessage(kAnnounce, 0, kGrandmaster), "192.168.1.10");
    deliver(observer, ptpMessage(kAnnounce, 0, kGrandmaster), "192.168.1.99");

    const auto peers = observer.peers();
    REQUIRE(peers.size() == 1);
    CHECK(peers[0].sourceIp == "192.168.1.99");  // the last one seen
    CHECK(peers[0].messageCount == 2);
}

TEST_CASE("The transport-specific nibble is not part of the message type") {
    // gPTP sets the high nibble of byte 0. Reading the whole byte as the
    // type would file every gPTP message under a type that does not exist,
    // and the peer's role with it.
    PTPPeerObserver observer;
    std::vector<uint8_t> gptpAnnounce = ptpMessage(kAnnounce, 0, kGrandmaster);
    gptpAnnounce[0] = static_cast<uint8_t>(0x10 | kAnnounce);
    deliver(observer, gptpAnnounce, "192.168.1.13");

    const auto peers = observer.peers();
    REQUIRE(peers.size() == 1);
    CHECK(peers[0].role() == PTPPeerRole::Master);
}
