//
// TestCompatibilityProfile.cpp
// AES67 macOS Driver
// Tests for the per-standard constraint sets (NetworkEngine/CompatibilityProfile.h).
//
// Pure validation logic — no sockets, no driver, no persistence of the
// user's actual selection. Safe in the standard suite.
//

#include "../NetworkEngine/CompatibilityProfile.h"

#include <iostream>

using namespace AES67;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST_ASSERT(condition, message) \
    if (!(condition)) { \
        std::cerr << "FAIL: " << message << std::endl; \
        testsFailed++; \
        return false; \
    } else { \
        testsPassed++; \
    }

namespace {

/// A stream every profile here should accept: 48 kHz, L24, 1 ms, 8 channels.
SDPSession baselineSession() {
    SDPSession sdp;
    sdp.sessionName = "test";
    sdp.connectionAddress = "239.69.1.10";
    sdp.port = 5004;
    sdp.numChannels = 8;
    sdp.sampleRate = 48000.0;
    sdp.encoding = "L24";
    sdp.ptime = 1;
    return sdp;
}

} // namespace

// ============================================================================
// A. The common baseline
// ============================================================================

bool testAllProfilesAcceptTheCommonBaseline() {
    std::cout << "Test: A1 · every profile accepts 48kHz/L24/1ms/8ch... ";
    const auto sdp = baselineSession();
    for (const auto& profile : CompatibilityProfile::all()) {
        std::string error;
        TEST_ASSERT(profile.validate(sdp, &error),
                    profile.displayName + " rejected the common baseline: " + error);
    }
    std::cout << "PASS" << std::endl;
    return true;
}

bool testEveryProfileHasDisplayNameAndCaveats() {
    std::cout << "Test: A2 · every profile is presentable and honest about its gaps... ";
    for (const auto& profile : CompatibilityProfile::all()) {
        TEST_ASSERT(!profile.displayName.empty(), "profile needs a display name for the UI");
        // Selecting a profile must never read as a conformance claim — each
        // one states what it can't enforce.
        TEST_ASSERT(!profile.caveats.empty(), profile.displayName + " must state its caveats");
    }
    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// B. ST 2110-30 Level A restrictions
// ============================================================================

bool testST2110RejectsNon48kHz() {
    std::cout << "Test: B1 · ST 2110-30 Level A permits 48 kHz only... ";
    const auto profile = CompatibilityProfile::forKind(CompatibilityProfileKind::ST2110_30);

    auto sdp = baselineSession();
    std::string error;
    TEST_ASSERT(profile.validate(sdp, &error), "48 kHz must be accepted");

    // 44.1 and 96 kHz are valid AES67 but outside ST 2110-30 Level A.
    for (double rate : {44100.0, 96000.0, 192000.0}) {
        sdp.sampleRate = rate;
        error.clear();
        TEST_ASSERT(!profile.validate(sdp, &error),
                    "ST 2110-30 Level A must reject " + std::to_string(static_cast<long>(rate)) + " Hz");
        TEST_ASSERT(!error.empty(), "rejection must explain itself");
    }
    std::cout << "PASS" << std::endl;
    return true;
}

bool testAES67AcceptsRatesST2110Rejects() {
    std::cout << "Test: B2 · the AES67 profile accepts rates ST 2110-30 refuses... ";
    const auto aes67 = CompatibilityProfile::forKind(CompatibilityProfileKind::AES67);
    const auto st2110 = CompatibilityProfile::forKind(CompatibilityProfileKind::ST2110_30);

    auto sdp = baselineSession();
    sdp.sampleRate = 96000.0;

    // This is the whole point of having separate profiles: the same stream
    // is fine for one target and not the other.
    TEST_ASSERT(aes67.validate(sdp, nullptr), "96 kHz is valid AES67");
    TEST_ASSERT(!st2110.validate(sdp, nullptr), "96 kHz is outside ST 2110-30 Level A");
    std::cout << "PASS" << std::endl;
    return true;
}

bool testST2110RequiresZeroRtpOffsetFlag() {
    std::cout << "Test: B3 · ST 2110-30 records the zero-RTP-offset requirement... ";
    const auto st2110 = CompatibilityProfile::forKind(CompatibilityProfileKind::ST2110_30);
    const auto aes67 = CompatibilityProfile::forKind(CompatibilityProfileKind::AES67);

    // RTPTransmitter already starts at zero unconditionally, so nothing
    // enforces this at runtime — the flag exists so the zero isn't
    // randomised later for AES67 reasons, silently breaking ST 2110-30.
    TEST_ASSERT(st2110.requiresZeroRtpTimestampOffset, "ST 2110-30 requires a zero RTP offset");
    TEST_ASSERT(!aes67.requiresZeroRtpTimestampOffset, "AES67 permits a random RTP offset");
    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// C. Limits shared by every profile
// ============================================================================

bool testNoProfileRaisesTheFlowChannelLimit() {
    std::cout << "Test: C1 · no profile permits more than 8 channels in one flow... ";
    auto sdp = baselineSession();
    sdp.numChannels = 16;

    // A profile can only narrow what this driver accepts, never widen it
    // past what the code implements — 8 is AES67's own limit.
    for (const auto& profile : CompatibilityProfile::all()) {
        TEST_ASSERT(profile.maxChannelsPerFlow <= 8,
                    profile.displayName + " must not raise the 8-channel flow limit");
        TEST_ASSERT(!profile.validate(sdp, nullptr),
                    profile.displayName + " must reject a 16-channel flow");
    }
    std::cout << "PASS" << std::endl;
    return true;
}

bool testNoProfileAcceptsAM824() {
    std::cout << "Test: C2 · no profile accepts AM824 (that's ST 2110-31)... ";
    auto sdp = baselineSession();
    sdp.encoding = "AM824";
    for (const auto& profile : CompatibilityProfile::all()) {
        TEST_ASSERT(!profile.validate(sdp, nullptr),
                    profile.displayName + " must reject AM824");
    }
    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// D. Persistence round-trip of the selection's identity
// ============================================================================

bool testKindStringRoundTrip() {
    std::cout << "Test: D1 · profile identity survives the round trip to disk... ";
    for (const auto& profile : CompatibilityProfile::all()) {
        const std::string s = CompatibilityProfile::kindToString(profile.kind);
        TEST_ASSERT(CompatibilityProfile::kindFromString(s) == profile.kind,
                    "round trip failed for " + profile.displayName);
    }
    // An unknown or corrupt value must fall back to the unrestricted
    // baseline, never to a profile that would reject working streams.
    TEST_ASSERT(CompatibilityProfile::kindFromString("") == CompatibilityProfileKind::AES67,
                "empty string must fall back to AES67");
    TEST_ASSERT(CompatibilityProfile::kindFromString("nonsense") == CompatibilityProfileKind::AES67,
                "unknown value must fall back to AES67");
    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================

int main() {
    std::cout << std::endl << "Compatibility Profile Tests" << std::endl << std::endl;

    std::cout << "A · Common baseline" << std::endl;
    std::cout << "-------------------" << std::endl;
    testAllProfilesAcceptTheCommonBaseline();
    testEveryProfileHasDisplayNameAndCaveats();
    std::cout << std::endl;

    std::cout << "B · ST 2110-30 Level A restrictions" << std::endl;
    std::cout << "-----------------------------------" << std::endl;
    testST2110RejectsNon48kHz();
    testAES67AcceptsRatesST2110Rejects();
    testST2110RequiresZeroRtpOffsetFlag();
    std::cout << std::endl;

    std::cout << "C · Limits shared by every profile" << std::endl;
    std::cout << "----------------------------------" << std::endl;
    testNoProfileRaisesTheFlowChannelLimit();
    testNoProfileAcceptsAM824();
    std::cout << std::endl;

    std::cout << "D · Persistence" << std::endl;
    std::cout << "---------------" << std::endl;
    testKindStringRoundTrip();
    std::cout << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "  Passed: " << testsPassed << std::endl;
    std::cout << "  Failed: " << testsFailed << std::endl;
    std::cout << "========================================" << std::endl;

    return testsFailed == 0 ? 0 : 1;
}
