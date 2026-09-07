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
