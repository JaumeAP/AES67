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

#include "NetworkEngine/PTP/PTPBMCA.h"
#include "NetworkEngine/PTP/PTPClockSource.h"

#include <iostream>

using namespace AES67;



namespace {

PTPAnnounceData makeAnnounce(uint8_t priority1, uint8_t clockClass, uint8_t accuracy,
                             uint16_t variance, uint8_t priority2, uint8_t identityLastByte) {
    PTPAnnounceData d{};
    d.grandmasterPriority1 = priority1;
    d.grandmasterClockClass = clockClass;
    d.grandmasterClockAccuracy = accuracy;
    d.grandmasterOffsetScaledLogVariance = variance;
    d.grandmasterPriority2 = priority2;
    d.grandmasterIdentity.id.fill(0);
    d.grandmasterIdentity.id[7] = identityLastByte;
    return d;
}

/// Same fields bmcaCompare() reads. Two datasets equal by this measure are
/// indistinguishable to bmcaCompare — see the "no deadlock" test below for
/// why that's the one case its guarantee doesn't (and can't) cover.
bool sameQuality(const PTPAnnounceData& x, const PTPAnnounceData& y) {
    return x.grandmasterPriority1 == y.grandmasterPriority1 &&
           x.grandmasterClockClass == y.grandmasterClockClass &&
           x.grandmasterClockAccuracy == y.grandmasterClockAccuracy &&
           x.grandmasterOffsetScaledLogVariance == y.grandmasterOffsetScaledLogVariance &&
           x.grandmasterPriority2 == y.grandmasterPriority2 &&
           x.grandmasterIdentity == y.grandmasterIdentity;
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
    CHECK(bmcaCompare(a, b) == PTPBMCAWinner::A);
    CHECK(bmcaCompare(b, a) == PTPBMCAWinner::B);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("BMCA Clock Class Tiebreak") {
    std::cout << "Test: A2 · equal priority1 falls through to clockClass... ";
    auto a = makeAnnounce(128, 6, 0xFE, 0xFFFF, 128, 0x01);   // GPS-locked
    auto b = makeAnnounce(128, 248, 0xFE, 0xFFFF, 128, 0x01); // free-running
    CHECK(bmcaCompare(a, b) == PTPBMCAWinner::A);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("BMCA Accuracy Tiebreak") {
    std::cout << "Test: A3 · equal priority1+clockClass falls through to clockAccuracy... ";
    auto a = makeAnnounce(128, 13, 0x21, 0xFFFF, 128, 0x01); // within 1us
    auto b = makeAnnounce(128, 13, 0xFE, 0xFFFF, 128, 0x01); // unknown
    CHECK(bmcaCompare(a, b) == PTPBMCAWinner::A);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("BMCA Variance Tiebreak") {
    std::cout << "Test: A4 · equal so far falls through to offsetScaledLogVariance... ";
    auto a = makeAnnounce(128, 13, 0x21, 0x1000, 128, 0x01);
    auto b = makeAnnounce(128, 13, 0x21, 0x8000, 128, 0x01);
    CHECK(bmcaCompare(a, b) == PTPBMCAWinner::A);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("BMCA Priority2 Tiebreak") {
    std::cout << "Test: A5 · equal so far falls through to priority2... ";
    auto a = makeAnnounce(128, 13, 0x21, 0x1000, 50, 0x01);
    auto b = makeAnnounce(128, 13, 0x21, 0x1000, 200, 0x01);
    CHECK(bmcaCompare(a, b) == PTPBMCAWinner::A);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("BMCA Identity Tiebreak") {
    std::cout << "Test: A6 · fully tied quality falls through to clockIdentity, deterministically... ";
    auto a = makeAnnounce(128, 13, 0x21, 0x1000, 128, 0x01);
    auto b = makeAnnounce(128, 13, 0x21, 0x1000, 128, 0x02);
    CHECK(bmcaCompare(a, b) == PTPBMCAWinner::A);
    CHECK(bmcaCompare(b, a) == PTPBMCAWinner::B);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("BMCA Never Deadlocks") {
    std::cout << "Test: A7 · two clocks with DIFFERING quality can never both consider themselves the winner... ";
    // A spread of realistic-ish datasets, all pairs — if the comparison were
    // asymmetric for any pair, both sides could conclude "I'm better" and
    // both start transmitting as master forever.
    //
    // Deliberately includes a value-duplicate (samples[4] == samples[0]) to
    // exercise the one case bmcaCompare's own doc comment already calls
    // out: on a full tie it returns A unconditionally, which — for two
    // callers each comparing themselves as the first argument against an
    // identical competitor — means BOTH would claim victory. That's not
    // tested as "must not happen" here, because it's real: it's just not
    // reachable in practice. clockIdentity comes from a MAC address, unique
    // per real NIC, so two genuinely different clocks never carry identical
    // datasets; and PTPMaster::handleForeignAnnounce filters out hearing its
    // own Announce echoed back (identity match) before this function is
    // ever called with a truly-tied pair. The guarantee this test checks —
    // no deadlock — only needs to hold when the two datasets differ, which
    // is the only case that can occur between two distinct real clocks.
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
            const auto xy = bmcaCompare(x, y);
            const auto yx = bmcaCompare(y, x);
            // Both can agree "x wins" (xy=A, yx=B) or "y wins" (xy=B, yx=A);
            // what must never happen, for genuinely differing data, is both
            // claiming victory — that's the two-master standoff.
            const bool bothClaimWin = (xy == PTPBMCAWinner::A && yx == PTPBMCAWinner::A);
            CHECK(!bothClaimWin);
        }
    }
    std::cout << "PASS" << std::endl;
}

TEST_CASE("BMCA Identical Datasets Favor First Argument") {
    std::cout << "Test: A8 · fully identical datasets: bmcaCompare() always favors its first argument (documented, not a bug)... ";
    auto a = makeAnnounce(128, 6, 0x20, 0x1000, 128, 0x01);
    auto b = a; // byte-for-byte identical
    CHECK(bmcaCompare(a, b) == PTPBMCAWinner::A);
    CHECK(bmcaCompare(b, a) == PTPBMCAWinner::A);
    // The pattern above IS the caller-dependent-outcome PTPBMCA.h's comment
    // warns about. It's harmless only because it's unreachable between two
    // distinct real clocks — see testBMCANeverDeadlocks() above.
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
    // source ever reports kPTPClockClassSlaveOnly — this is the guarantee
    // that check depends on for the Internal source.
    CHECK(src.clockClass() != kPTPClockClassSlaveOnly);
    std::cout << "PASS" << std::endl;
}

// ============================================================================
// main
// ============================================================================

