//
// TestPhaseLockedLoop.cpp
// The media-clock PLL, which had no tests at all.
//
// It is in AES67_CORE_SOURCES -- a consumer off macOS gets it as part of the
// deal -- and the coverage run put it at 0% of lines. This suite covers what
// the class promises rather than how it computes it: that a steady offset
// converges, that the correction stays inside the stated +/-1000 ppm, that lock
// is reached and lost on the documented thresholds, and that the guards against
// nonsense timestamps actually guard.
//
// The numbers below come from the header's own constants, so a change to them
// breaks these tests, which is the point: they are a contract, not magic.
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "NetworkEngine/PTP/PhaseLockedLoop.h"

#include <cstdint>

using AES67::PhaseLockedLoop;

namespace {

constexpr uint64_t kSecond = 1000000000ULL;
constexpr uint32_t kRate = 48000;

/// Feeds `count` updates one second apart, with the remote clock a fixed
/// `offsetNs` ahead of the local one.
void feedSteadyOffset(PhaseLockedLoop& pll, int count, int64_t offsetNs) {
    for (int i = 1; i <= count; ++i) {
        const uint64_t local = static_cast<uint64_t>(i) * kSecond;
        const uint64_t remote = static_cast<uint64_t>(static_cast<int64_t>(local) + offsetNs);
        pll.update(local, remote, static_cast<uint64_t>(i) * kRate, kRate);
    }
}

}  // namespace

TEST_CASE("A fresh PLL is unlocked and applies no correction") {
    PhaseLockedLoop pll;
    CHECK(pll.isLocked() == false);
    CHECK(pll.getLockQuality() == 0);
    CHECK(pll.getFrequencyCorrection() == doctest::Approx(0.0));
    CHECK(pll.getAdjustedSampleRate(kRate) == doctest::Approx(double(kRate)));
}

TEST_CASE("The first update is a reference, not a measurement") {
    // update() returns early while lastLocalTime_ is zero: with nothing to
    // measure an interval against, the first call can only establish one. Worth
    // pinning, because it means N updates produce N-1 lock counts and every
    // count below depends on that.
    PhaseLockedLoop pll;
    feedSteadyOffset(pll, 1, 100000);

    CHECK(pll.getLockQuality() == 0);
    CHECK(pll.getFrequencyCorrection() == doctest::Approx(0.0));
}

TEST_CASE("An offset inside the convergence threshold locks the PLL") {
    // CONVERGENCE_THRESHOLD is 1 ms; isLocked() wants more than 10 good
    // samples, and lock quality is a percentage of MAX_LOCK_COUNT (100). The
    // first update only sets the reference, hence 12 updates for 11 counts.
    PhaseLockedLoop pll;
    feedSteadyOffset(pll, 12, 100000);  // 100 us, well inside 1 ms

    CHECK(pll.isLocked());
    CHECK(pll.getLockQuality() == 11);
}

TEST_CASE("An offset beyond the threshold never locks") {
    PhaseLockedLoop pll;
    feedSteadyOffset(pll, 50, 5000000);  // 5 ms, five times the threshold

    CHECK(pll.isLocked() == false);
    CHECK(pll.getLockQuality() == 0);
}

TEST_CASE("Lock decays faster than it builds") {
    // Deliberate asymmetry: one good sample adds 1, one bad sample subtracts 2,
    // so a link that alternates good and bad does not report itself locked.
    PhaseLockedLoop pll;
    feedSteadyOffset(pll, 31, 100000);  // 30 counted updates
    const int lockedQuality = pll.getLockQuality();
    REQUIRE(lockedQuality > 0);

    // From 32: second 31 was the last good update, and repeating its timestamp
    // would be a zero interval, which update() skips.
    for (int i = 32; i <= 41; ++i) {
        const uint64_t local = static_cast<uint64_t>(i) * kSecond;
        pll.update(local, local + 5000000, static_cast<uint64_t>(i) * kRate, kRate);
    }

    CHECK(pll.getLockQuality() == lockedQuality - 2 * 10);
}

TEST_CASE("Frequency correction stays inside the stated limits") {
    // A degenerate one-second offset drives the loop filter as hard as it goes;
    // the clamp is what keeps a broken master from dragging the sample rate
    // somewhere absurd.
    PhaseLockedLoop pll;
    feedSteadyOffset(pll, 200, 900000000);  // 0.9 s ahead, every second

    CHECK(pll.getFrequencyCorrection() <= 0.001);
    CHECK(pll.getFrequencyCorrection() >= -0.001);
    CHECK(pll.getAdjustedSampleRate(kRate) <= 48000.0 * 1.001);
    CHECK(pll.getAdjustedSampleRate(kRate) >= 48000.0 * 0.999);
}

TEST_CASE("Nonsense intervals are ignored rather than trusted") {
    PhaseLockedLoop pll;
    feedSteadyOffset(pll, 5, 100000);
    const double before = pll.getFrequencyCorrection();
    const int qualityBefore = pll.getLockQuality();

    // Same local timestamp as the last update: delta time is zero.
    pll.update(5 * kSecond, 5 * kSecond + 100000, 5 * kRate, kRate);
    // And one an hour later: delta time beyond the 10 s sanity limit.
    pll.update(3600 * kSecond, 3600 * kSecond + 100000, 3600ULL * kRate, kRate);

    CHECK(pll.getFrequencyCorrection() == doctest::Approx(before));
    CHECK(pll.getLockQuality() == qualityBefore);
}

TEST_CASE("Reset returns the PLL to its initial state") {
    PhaseLockedLoop pll;
    feedSteadyOffset(pll, 20, 100000);
    REQUIRE(pll.isLocked());

    pll.reset();

    CHECK(pll.isLocked() == false);
    CHECK(pll.getLockQuality() == 0);
    CHECK(pll.getFrequencyCorrection() == doctest::Approx(0.0));
}
