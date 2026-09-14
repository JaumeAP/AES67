//
// TestAudioThreadPriority.cpp
// AES67 macOS Driver
//
// millisToAbsolute() -- ns * tb.denom / tb.numer cast straight to a
// uint32_t -- had no test at all, and no caller between it and
// PTPMasterConfig::syncIntervalMs/announceIntervalMs, which are read from
// a JSON file with no min/max clamp anywhere on the way. Once the double
// on the way in exceeds what a uint32_t can hold, the cast is undefined
// behavior, not a wrapped or saturated one -- the kind of bug a plain build
// can look correct under and UBSan (this package's own AES67_ANALYSE=1 run,
// -fsanitize=undefined) is what actually catches.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Driver/AudioThreadPriority.h"

#include <limits>

using namespace AES67;

TEST_CASE("An ordinary packet period converts to a small, sane tick count") {
    // 1 ms is what AES67 defaults to; on any Mach timebase this is a
    // small positive number of ticks, nowhere near the range this pins.
    const std::uint32_t oneMs = AudioThreadPriority::millisToAbsoluteForTest(1.0);
    CHECK(oneMs > 0);
    CHECK(oneMs < 1'000'000'000u);
}

TEST_CASE("A period past what a uint32_t can hold is clamped, not cast into UB") {
    // Roughly what a hand-edited ptp_master.json's syncIntervalMs/
    // announceIntervalMs would need to reach to overflow -- the finding
    // that found this used ~4300 ms as the threshold; this is comfortably
    // past it and past anything a real audio period is.
    const std::uint32_t huge = AudioThreadPriority::millisToAbsoluteForTest(1.0e9);
    CHECK(huge == std::numeric_limits<std::uint32_t>::max());
}

TEST_CASE("Zero, negative and NaN periods come back as zero rather than garbage") {
    CHECK(AudioThreadPriority::millisToAbsoluteForTest(0.0) == 0);
    CHECK(AudioThreadPriority::millisToAbsoluteForTest(-1.0) == 0);
    CHECK(AudioThreadPriority::millisToAbsoluteForTest(
              std::numeric_limits<double>::quiet_NaN()) == 0);
}

TEST_CASE("configureForRealTime survives an absurd period rather than crashing") {
    // The public entry point a caller actually uses, with the same
    // out-of-range value: the return value says whether the OS accepted
    // the policy, not whether this refused to convert it, so only "did not
    // crash / did not trip UBSan" is asserted here.
    (void)AudioThreadPriority::configureForRealTime(1.0e9);
    AudioThreadPriority::restoreNormalPriority();
}
