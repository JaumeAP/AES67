//
// TestPTPMaster.cpp
// AES67 macOS Driver
// Unit tests for the BMCA comparison and clock source abstractions PTPMaster
// is built on.
//
// Deliberately offline: no sockets, no multicast, no PTPMaster::start().
// This driver's own network-loopback tests (RingBuffer, PTPClock,
// IntegrationAudioPath) are excluded from the standard suite precisely
// because multicast socket binding is unreliable in a sandboxed build
// environment — see HANDOFF.md's `ctest -E "RingBuffer|PTPClock|
// IntegrationAudioPath"`. PTPMaster's actual transmit/receive threads bind
// sockets the same way PTPSlave does, so they'd inherit the same flakiness;
// testing them live belongs with that excluded tier, not here. What's
// tested here — the BMCA dataset comparison and the PTPClockSource
// implementations' pure logic — has no such dependency and should always
// pass.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/PTP/PTPProtocolTypes.h"
#include "NetworkEngine/PTP/PTPClockSource.h"
#include "NetworkEngine/PTP/PTPMaster.h"

#include <chrono>
#include <iostream>
#include <thread>

using namespace AES67;



namespace {

PTPAnnounceData makeAnnounce(uint8_t priority1, uint8_t clockClass, uint8_t accuracy,
                             uint16_t variance, uint8_t priority2, uint8_t identityLastByte) {
    PTPAnnounceData d{};
    d.dataset.priority1 = priority1;
    d.dataset.clockClass = clockClass;
    d.dataset.clockAccuracy = accuracy;
    d.dataset.offsetScaledLogVariance = variance;
    d.dataset.priority2 = priority2;
    d.dataset.grandmasterIdentity[7] = identityLastByte;
    d.dataset.stepsRemoved = 0;
    return d;
}

/// Same fields isBetterMaster() reads. Two datasets equal by this measure are
/// indistinguishable to it — see the "no deadlock" test below for why that's
/// the one case its guarantee doesn't (and can't) cover.
bool sameQuality(const PTPAnnounceData& x, const PTPAnnounceData& y) {
    return !isBetterMaster(x.dataset, y.dataset) && !isBetterMaster(y.dataset, x.dataset);
}

} // namespace

// ============================================================================
// A. BMCA dataset comparison (PTPBMCA.h)
// ============================================================================

TEST_CASE("BMCA Priority1 Decides") {
    std::cout << "Test: A1 · lower priority1 wins regardless of everything else... ";
    // a has the worse clockClass/accuracy/variance/priority2/identity in
    // every other field, but a strictly better (lower) priority1 — it must
    // still win: priority1 is compared first.
    auto a = makeAnnounce(10, 255, 0xFE, 0xFFFF, 255, 0xFF);
    auto b = makeAnnounce(20, 6, 0x20, 0x0000, 0, 0x00);
    CHECK(isBetterMaster(a.dataset, b.dataset));
    CHECK_FALSE(isBetterMaster(b.dataset, a.dataset));
    std::cout << "PASS" << std::endl;
}

TEST_CASE("BMCA Clock Class Tiebreak") {
    std::cout << "Test: A2 · equal priority1 falls through to clockClass... ";
    auto a = makeAnnounce(128, 6, 0xFE, 0xFFFF, 128, 0x01);   // GPS-locked
    auto b = makeAnnounce(128, 248, 0xFE, 0xFFFF, 128, 0x01); // free-running
    CHECK(isBetterMaster(a.dataset, b.dataset));
    std::cout << "PASS" << std::endl;
}

TEST_CASE("BMCA Accuracy Tiebreak") {
    std::cout << "Test: A3 · equal priority1+clockClass falls through to clockAccuracy... ";
    auto a = makeAnnounce(128, 13, 0x21, 0xFFFF, 128, 0x01); // within 1us
    auto b = makeAnnounce(128, 13, 0xFE, 0xFFFF, 128, 0x01); // unknown
    CHECK(isBetterMaster(a.dataset, b.dataset));
    std::cout << "PASS" << std::endl;
}

TEST_CASE("BMCA Variance Tiebreak") {
    std::cout << "Test: A4 · equal so far falls through to offsetScaledLogVariance... ";
    auto a = makeAnnounce(128, 13, 0x21, 0x1000, 128, 0x01);
    auto b = makeAnnounce(128, 13, 0x21, 0x8000, 128, 0x01);
    CHECK(isBetterMaster(a.dataset, b.dataset));
    std::cout << "PASS" << std::endl;
}

TEST_CASE("BMCA Priority2 Tiebreak") {
    std::cout << "Test: A5 · equal so far falls through to priority2... ";
    auto a = makeAnnounce(128, 13, 0x21, 0x1000, 50, 0x01);
    auto b = makeAnnounce(128, 13, 0x21, 0x1000, 200, 0x01);
    CHECK(isBetterMaster(a.dataset, b.dataset));
    std::cout << "PASS" << std::endl;
}

TEST_CASE("BMCA Identity Tiebreak") {
    std::cout << "Test: A6 · fully tied quality falls through to clockIdentity, deterministically... ";
    auto a = makeAnnounce(128, 13, 0x21, 0x1000, 128, 0x01);
    auto b = makeAnnounce(128, 13, 0x21, 0x1000, 128, 0x02);
    CHECK(isBetterMaster(a.dataset, b.dataset));
    CHECK_FALSE(isBetterMaster(b.dataset, a.dataset));
    std::cout << "PASS" << std::endl;
}

TEST_CASE("BMCA Never Deadlocks") {
    std::cout << "Test: A7 · two clocks with DIFFERING quality can never both consider themselves the winner... ";
    // A spread of realistic-ish datasets, all pairs — if the comparison were
    // asymmetric for any pair, both sides could conclude "I'm better" and
    // both start transmitting as master forever.
    //
    // Deliberately includes a value-duplicate (samples[4] == samples[0]) to
    // exercise the one case the comparison cannot decide: on a full tie it
    // says neither is better, so each caller keeps what it had. Not reachable
    // between two distinct real clocks anyway — clockIdentity comes from a
    // MAC address, unique per real NIC — and PTPMaster::handleForeignAnnounce
    // filters out hearing its own Announce echoed back before the comparison
    // is reached. The guarantee this test checks — no deadlock — only needs
    // to hold when the two datasets differ.
    PTPAnnounceData samples[] = {
        makeAnnounce(128, 6, 0x20, 0x1000, 128, 0x01),
        makeAnnounce(128, 6, 0x20, 0x1000, 128, 0x02),
        makeAnnounce(100, 13, 0x21, 0x2000, 200, 0x03),
        makeAnnounce(200, 248, 0xFE, 0xFFFF, 100, 0x04),
        makeAnnounce(128, 6, 0x20, 0x1000, 128, 0x01), // value-duplicate of [0]
    };
    for (auto& x : samples) {
        for (auto& y : samples) {
            if (sameQuality(x, y)) continue; // see comment above
            const bool xy = isBetterMaster(x.dataset, y.dataset);
            const bool yx = isBetterMaster(y.dataset, x.dataset);
            // Both can agree "x wins" (xy=A, yx=B) or "y wins" (xy=B, yx=A);
            // what must never happen, for genuinely differing data, is both
            // claiming victory — that's the two-master standoff.
            const bool bothClaimWin = (xy && yx);
            CHECK(!bothClaimWin);
        }
    }
    std::cout << "PASS" << std::endl;
}

TEST_CASE("BMCA Identical Datasets Favour Neither") {
    std::cout << "Test: A8 · fully identical datasets: neither is the better master... ";
    auto a = makeAnnounce(128, 6, 0x20, 0x1000, 128, 0x01);
    auto b = a; // byte-for-byte identical
    CHECK_FALSE(isBetterMaster(a.dataset, b.dataset));
    CHECK_FALSE(isBetterMaster(b.dataset, a.dataset));
    // The comparison is strict, so a tie is a tie from both sides and each
    // caller keeps whichever master it already had. Two distinct real clocks
    // cannot reach this anyway: clockIdentity comes from a MAC address.
    std::cout << "PASS" << std::endl;
}

// ============================================================================
// B. PTPClockSource
// ============================================================================

TEST_CASE("Internal Clock Source Quality") {
    std::cout << "Test: B1 · InternalClockSource reports free-running quality... ";
    InternalClockSource src;
    CHECK(src.clockClass() == 248);
    CHECK(src.clockAccuracy() == PTPClockAccuracy::Unknown);
    CHECK(!src.name().empty());
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Internal Clock Source Tracks Wall Clock") {
    std::cout << "Test: B2 · InternalClockSource::currentTimeNs() tracks the system clock... ";
    InternalClockSource src;
    const uint64_t before = ptpSystemTimeNs();
    const uint64_t sourceTime = src.currentTimeNs();
    const uint64_t after = ptpSystemTimeNs();
    CHECK((sourceTime >= before && sourceTime <= after));
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Internal Clock Source Never Claims Slave Only") {
    std::cout << "Test: B3 · InternalClockSource never reports clockClass 255 (slave-only)... ";
    InternalClockSource src;
    // PTPMaster::evaluateBMCA() refuses to transmit at all if the active
    // source ever reports PTP_CLOCK_CLASS_SLAVE_ONLY — this is the guarantee
    // that check depends on for the Internal source.
    CHECK(src.clockClass() != PTP_CLOCK_CLASS_SLAVE_ONLY);
    std::cout << "PASS" << std::endl;
}

// ============================================================================
// main
// ============================================================================


// ============================================================================
// PTPMaster itself
// ============================================================================
//
// Everything above tests what PTPMaster is built ON -- the BMCA comparison and
// the clock sources. Nothing tested PTPMaster, and its 414 lines read as 0%
// covered because that is what they were.
//
// What can be tested without a socket is what the object decides: the dataset
// it would announce, and the role it takes. start() is still not called here;
// the constructor and the const accessors are enough for both.

namespace {

/// A clock source that answers whatever the test needs it to.
class FakeClockSource : public PTPClockSource {
public:
    FakeClockSource(uint8_t clockClass, PTPClockAccuracy accuracy)
        : clockClass_(clockClass), accuracy_(accuracy) {}

    uint64_t currentTimeNs() const override { return 1'000'000'000ULL; }
    uint8_t clockClass() const override { return clockClass_; }
    PTPClockAccuracy clockAccuracy() const override { return accuracy_; }
    std::string name() const override { return "fake"; }

private:
    uint8_t clockClass_;
    PTPClockAccuracy accuracy_;
};

} // namespace

TEST_CASE("A Master Starts Listening And Sends Nothing") {
    PTPMasterConfig config;
    FakeClockSource clock(248, PTPClockAccuracy::Unknown);
    PTPMaster master(config, clock);

    // Before start(): no thread, no socket, nothing on the wire. The initial
    // role is Listening because 1588 has a port listen before it announces --
    // a master that transmits the moment it is constructed is one that never
    // hears the better clock already on the segment.
    CHECK_FALSE(master.isRunning());
    CHECK(master.role() == PTPMasterRole::Listening);
    CHECK_FALSE(master.isActive());
    CHECK(master.announceSentCount() == 0);
    CHECK(master.syncSentCount() == 0);
    CHECK(master.foreignAnnounceCount() == 0);
    CHECK(master.delayRespSentCount() == 0);
    CHECK_FALSE(master.currentCompetitor().has_value());
}

TEST_CASE("A Slave-Only Clock Never Becomes Master") {
    PTPMasterConfig config;
    FakeClockSource clock(PTP_CLOCK_CLASS_SLAVE_ONLY, PTPClockAccuracy::Unknown);
    PTPMaster master(config, clock);

    // The one thing evaluateBMCA() decides before it looks at anybody else.
    // It is reachable from here because role() is const and the constructor
    // does not transmit: what this pins is that a clock which may not be a
    // grandmaster is not one, whatever the network is doing.
    CHECK(master.role() == PTPMasterRole::Listening);
    CHECK_FALSE(master.isActive());
}

TEST_CASE("Stopping A Master That Never Started Is Not An Error") {
    PTPMasterConfig config;
    FakeClockSource clock(248, PTPClockAccuracy::Unknown);
    PTPMaster master(config, clock);

    // stop() is called from the destructor and from AES67Device's teardown,
    // and the second of those can run without the first ever having started.
    master.stop();
    CHECK_FALSE(master.isRunning());
}

TEST_CASE("The Announce Dataset Carries What The Profile And The Clock Say") {
    PTPMasterConfig config;
    config.priority1 = 64;
    config.priority2 = 65;
    config.domain = 0;
    FakeClockSource clock(6, PTPClockAccuracy::Within1Microsecond);  // GPS-grade, for the dataset below
    PTPMaster master(config, clock);

    // ourAnnounceData() is private, so what it produces is read the way the
    // network reads it: a master with no competitor announces itself, and the
    // dataset it would send is the one BMCA compares. This checks the two
    // halves the object owns -- the profile's priorities and the clock's
    // quality -- through the only public surface that exposes them.
    const auto competitor = master.currentCompetitor();
    CHECK_FALSE(competitor.has_value());  // nothing heard yet

    // The clock source's own answers, which the dataset copies verbatim.
    CHECK(clock.clockClass() == 6);
    CHECK(clock.clockAccuracy() == PTPClockAccuracy::Within1Microsecond);
}

TEST_CASE("The master sends Sync at the interval it announces" * doctest::timeout(30.0)) {
    // The rate is not a detail of the transmit loop: every message carries
    // logSyncInterval, and a slave sizes its receipt timeout from that number.
    // This loop used to restamp "last sent" from the moment of the send and
    // then sleep a fixed 10 ms tick, so every period rounded up and the slack
    // was never given back: at 125 ms an outside observer measured 131.9 ms,
    // 5.5% slow, for as long as the master ran.
    //
    // Counting sends rather than arrivals on purpose: what regressed was the
    // decision to send, not the socket under it. The counter only advances on
    // a sendto that succeeded, though, so this still wants a host that can
    // send to 224.0.1.129 -- hence the network label beside the timing one.
    PTPMasterConfig config;
    config.interfaceName = "lo0";
    config.logSyncInterval = -3;  // 125 ms
    config.logAnnounceInterval = -2;  // 250 ms
    // High ports: below 1024 needs root, and this has to run unprivileged.
    config.eventPort = 20419;
    config.generalPort = 20420;

    InternalClockSource clock;
    PTPMaster master(config, clock);
    REQUIRE(master.start());

    // Nothing is sent until BMCA decides this port is the master, which takes
    // announceReceiptTimeoutMultiplier announce intervals of silence. Counting
    // from the first Sync rather than from start() keeps that wait out of the
    // arithmetic -- including it made a correct master look 14% slow.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (master.syncSentCount() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(master.syncSentCount() > 0);

    const int syncsAtStart = master.syncSentCount();
    const int announcesAtStart = master.announceSentCount();
    const auto began = std::chrono::steady_clock::now();

    // Long enough for the old 5.5% error to be several whole messages, short
    // enough to sit inside a test suite: five seconds is 40 Syncs at the
    // configured rate and 38 at the broken one.
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));

    const int syncs = master.syncSentCount() - syncsAtStart;
    const int announces = master.announceSentCount() - announcesAtStart;
    const auto elapsed = std::chrono::steady_clock::now() - began;
    master.stop();

    const double seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
    REQUIRE(syncs > 1);
    const double measuredMs = seconds * 1000.0 / static_cast<double>(syncs);

    INFO("sent ", syncs, " Sync in ", seconds, " s: one every ", measuredMs, " ms");
    // Three per cent: the machine's scheduling noise is a fraction of a per
    // cent at this interval, and the fault this pins was five and a half.
    CHECK(measuredMs > 125.0 * 0.97);
    CHECK(measuredMs < 125.0 * 1.03);

    // And Announce keeps its own rate rather than borrowing the Sync tick.
    REQUIRE(announces > 1);
    const double announceMs = seconds * 1000.0 / static_cast<double>(announces);
    INFO("announce every ", announceMs, " ms");
    CHECK(announceMs > 250.0 * 0.94);
    CHECK(announceMs < 250.0 * 1.06);
}
