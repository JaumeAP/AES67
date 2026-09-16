//
// TestPtpProfiles.cpp
// AES67 profiles
//
// The PTP profiles are five numbers each and nothing else, so what is worth
// checking is not arithmetic: it is that the numbers are the ones each
// ecosystem actually expects, that looking one up by name works, and that the
// whole header stays usable in a constant expression -- which is what makes it
// safe to include from firmware.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Profiles/PtpProfiles.h"
#include "Profiles/PtpIntervals.h"

#include <cmath>
#include <cstdint>

using namespace AES67;

TEST_CASE("The default profile is IEEE 1588-2008's own") {
    // Annex J: Sync every second, Announce every two, Delay_Req every second,
    // domain 0, and the default profile's own sdoId.
    CHECK(kPtpDefaultProfile.settings.domainNumber == 0);
    CHECK(kPtpDefaultProfile.settings.majorSdoId == 0);
    CHECK(kPtpDefaultProfile.settings.logSyncInterval == 0);
    CHECK(kPtpDefaultProfile.settings.logAnnounceInterval == 1);
    CHECK(kPtpDefaultProfile.settings.logMinDelayReqInterval == 0);
}

TEST_CASE("The AES67 media profile is what the gear runs") {
    // Eight Sync a second and eight Delay_Req, one Announce, domain 0. This is
    // the combination the IEEE default profile does not permit and every
    // AES67 device on the market sends.
    CHECK(kPtpAes67MediaProfile.settings.logSyncInterval == -3);
    CHECK(kPtpAes67MediaProfile.settings.logAnnounceInterval == 0);
    CHECK(kPtpAes67MediaProfile.settings.logMinDelayReqInterval == -3);
    CHECK(kPtpAes67MediaProfile.settings.majorSdoId == 0);
}

TEST_CASE("The tight media profile is the same ecosystem, sent faster") {
    // Sixteen Sync a second and sixteen Delay_Req, one Announce. Domain and
    // majorSdoId are the media profile's, because this is the same ecosystem:
    // what changes is the rate, not who it talks to.
    CHECK(kPtpAes67TightProfile.settings.domainNumber ==
          kPtpAes67MediaProfile.settings.domainNumber);
    CHECK(kPtpAes67TightProfile.settings.majorSdoId ==
          kPtpAes67MediaProfile.settings.majorSdoId);
    CHECK(kPtpAes67TightProfile.settings.logSyncInterval == -4);
    CHECK(kPtpAes67TightProfile.settings.logAnnounceInterval == 0);
    CHECK(kPtpAes67TightProfile.settings.logMinDelayReqInterval == -4);
}

TEST_CASE("gPTP is the one that takes an sdoId of its own") {
    // majorSdoId 1 is what makes an 802.1AS receiver accept the traffic at
    // all, and what makes everything else ignore it.
    CHECK(kPtpGptpProfile.settings.majorSdoId == 1);
    CHECK(kPtpGptpProfile.settings.logSyncInterval == -3);
    CHECK(kPtpGptpProfile.settings.logAnnounceInterval == 0);
    // Pdelay_Req once a second: peer delay measures a link, not a hierarchy,
    // and does not need the rate the Sync goes at.
    CHECK(kPtpGptpProfile.settings.logMinDelayReqInterval == 0);
}

TEST_CASE("Profiles are found by the name a person types") {
    CHECK(ptpProfileByName("default1588") == &kPtpDefaultProfile);
    CHECK(ptpProfileByName("aes67") == &kPtpAes67MediaProfile);
    CHECK(ptpProfileByName("aes67-tight") == &kPtpAes67TightProfile);
    CHECK(ptpProfileByName("gptp") == &kPtpGptpProfile);

    CHECK(ptpProfileByName("ravenna") == nullptr);
    CHECK(ptpProfileByName("") == nullptr);
    CHECK(ptpProfileByName(nullptr) == nullptr);
    // A prefix is not a name: "aes" must not find "aes67".
    CHECK(ptpProfileByName("aes") == nullptr);
    CHECK(ptpProfileByName("aes670") == nullptr);
}

TEST_CASE("Every profile is in the list, once") {
    CHECK(kPtpProfileCount == 4);
    for (size_t i = 0; i < kPtpProfileCount; ++i) {
        CHECK(kPtpProfiles[i] != nullptr);
        CHECK(ptpProfileByName(kPtpProfiles[i]->name) == kPtpProfiles[i]);
    }
}

TEST_CASE("The lookup works in a constant expression") {
    // This is the property that keeps the header honest for firmware: if any
    // of it needed the heap, a string or the C library, none of these would
    // compile.
    static_assert(ptpProfileByName("aes67") == &kPtpAes67MediaProfile,
                  "the lookup has to work at compile time");
    static_assert(ptpProfileByName("nope") == nullptr, "and so does failing");
    static_assert(kPtpAes67MediaProfile.settings.logSyncInterval == -3,
                  "and so does reading a setting");
    static_assert(ptpProfileNameEquals("aes67", "aes67"), "");
    static_assert(!ptpProfileNameEquals("aes67", "aes6"), "");
    CHECK(true);
}

#include "Profiles/PtpIntervals.h"

TEST_CASE("One interval conversion, exact where the two used to disagree") {
    CHECK(ptpLogIntervalToMilliseconds(0) == 1000);
    CHECK(ptpLogIntervalToMilliseconds(1) == 2000);
    CHECK(ptpLogIntervalToMilliseconds(-3) == 125);
    // 2^-7 s is 7.8125 ms: the driver rounded it to 8, the Teensy truncated
    // to 7. Rounded is the answer.
    CHECK(ptpLogIntervalToMilliseconds(-7) == 8);
    CHECK(ptpLogIntervalToMilliseconds(-4) == 63);   // 62.5, rounded up
    CHECK(ptpLogIntervalToMilliseconds(4) == 16000);
    // Beyond what a shift can hold is not an interval anyone sends.
    CHECK(ptpLogIntervalToMilliseconds(22) == 0);
    static_assert(ptpLogIntervalToMilliseconds(-3) == 125, "usable at compile time");

    // Nanoseconds, where the rounding the millisecond form has to do does not
    // arise: -4 is 62.5 ms exactly, and a sender pacing itself by this one
    // sends at the rate it announces.
    CHECK(ptpLogIntervalToNanoseconds(0) == 1000000000ull);
    CHECK(ptpLogIntervalToNanoseconds(1) == 2000000000ull);
    CHECK(ptpLogIntervalToNanoseconds(-3) == 125000000ull);
    CHECK(ptpLogIntervalToNanoseconds(-4) == 62500000ull);
    CHECK(ptpLogIntervalToNanoseconds(-9) == 1953125ull);
    CHECK(ptpLogIntervalToNanoseconds(-10) == 0ull);
    CHECK(ptpLogIntervalToNanoseconds(22) == 0ull);
    static_assert(ptpLogIntervalToNanoseconds(-4) == 62500000ull,
                  "usable at compile time too");
}


//
// Exhaustive over the whole int8_t domain -- 256 values, cheap enough to
// enumerate completely rather than sample. This is what the severe defect
// found in this driver's PTPMasterSettings::load() should have been caught
// by well before it reached three independent code reviews: a raw exponent
// wrapped into a value like -56 was still a legal int8_t, and nothing
// checked what it actually meant.
//

TEST_CASE("Every int8_t is either an interval to follow or answers zero, never garbage") {
    for (int raw = -128; raw <= 127; ++raw) {
        const auto logInterval = static_cast<int8_t>(raw);
        const uint64_t ns = ptpLogIntervalToNanoseconds(logInterval);
        const uint32_t ms = ptpLogIntervalToMilliseconds(logInterval);

        INFO("logInterval: " << static_cast<int>(logInterval));

        if (logInterval < -9 || logInterval > 21) {
            // Outside the domain this driver's PTP ports actually run in:
            // both conversions say so with zero, which a caller reads as
            // "not an interval to follow" -- never as a legitimate period,
            // and in particular never as the near-infinite or near-zero
            // period a naive shift would produce for the ends of the range.
            CHECK(ns == 0);
            CHECK(ms == 0);
        } else {
            // Inside it, both answer a real, bounded period: at least a
            // microsecond (finer than any PTP rate this driver configures)
            // and at most the ~24.3 days 2^21 seconds is.
            CHECK(ns >= 1000);
            CHECK(ns <= 2'097'152'000'000'000ull);
            CHECK(ms >= 1);
        }

        // Whichever branch, no result is ever the pathological near-zero
        // period that pins a transmit loop at "now" forever -- the busy
        // loop this whole domain check exists to keep unreachable.
        CHECK_FALSE((ns > 0 && ns < 1000));
    }
}

TEST_CASE("Nanoseconds and milliseconds agree at every legal interval") {
    // Same domain, cross-checked: the two conversions are two different
    // formulas (one exact, one rounded to the nearest millisecond), and
    // they have disagreed with each other's predecessors before. They must
    // not disagree by more than the millisecond rounding itself allows.
    for (int raw = -9; raw <= 21; ++raw) {
        const auto logInterval = static_cast<int8_t>(raw);
        const uint64_t ns = ptpLogIntervalToNanoseconds(logInterval);
        const uint32_t ms = ptpLogIntervalToMilliseconds(logInterval);

        INFO("logInterval: " << static_cast<int>(logInterval));
        const double msFromNs = static_cast<double>(ns) / 1'000'000.0;
        CHECK(std::abs(msFromNs - static_cast<double>(ms)) < 1.0);
    }
}
