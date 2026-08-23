//
// TestCompatibilityProfile.cpp
// AES67 macOS Driver
// Tests for the per-standard constraint sets (NetworkEngine/CompatibilityProfile.h).
//
// Pure validation logic — no sockets, no driver, no persistence of the
// user's actual selection. Safe in the standard suite.
//
// What this file does NOT cover: CompatibilityProfile::maxTotalChannels
// (CP850's 64ch / DAC3202's 32ch aggregate caps) is enforced by
// StreamManager::canAddStream(), which sums channels across every stream
// currently open — that needs a real StreamManager instance (ring buffers,
// sockets), which this file's test suites deliberately avoid constructing
// (see TestStreamManager.cpp's own "without RTP instance creation" scope).
// This file tests that the field itself carries the right value; the
// aggregate enforcement is unverified by an automated test, same honesty as
// everything else in this driver that hasn't been run end to end.
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

/// A stream every profile here should accept in its own permitted
/// direction: 48 kHz, L24, 1 ms, 8 channels, Dante's multicast range (which
/// also happens to be a perfectly ordinary AES67 address), domain 0.
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

/// Whether THIS driver would be transmitting when talking to the gear a
/// profile describes — false for everything except a TransmitOnly profile
/// (DAC3202). Lets the shared/loop tests exercise each profile in the one
/// direction it actually allows, so a rejection they check for is caused by
/// the thing under test, not a direction mismatch on top of it.
bool transmitDirectionFor(const CompatibilityProfile& profile) {
    return profile.direction == ProfileDirection::TransmitOnly;
}

} // namespace

// ============================================================================
// A. The common baseline
// ============================================================================

bool testAllProfilesAcceptTheCommonBaseline() {
    std::cout << "Test: A1 · every profile accepts 48kHz/L24/1ms/8ch, each in its own direction... ";
    const auto sdp = baselineSession();
    for (const auto& profile : CompatibilityProfile::all()) {
        std::string error;
        TEST_ASSERT(profile.validate(sdp, transmitDirectionFor(profile), &error),
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
    TEST_ASSERT(profile.validate(sdp, false, &error), "48 kHz must be accepted");

    // 44.1 and 96 kHz are valid AES67 but outside ST 2110-30 Level A.
    for (double rate : {44100.0, 96000.0, 192000.0}) {
        sdp.sampleRate = rate;
        error.clear();
        TEST_ASSERT(!profile.validate(sdp, false, &error),
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
    TEST_ASSERT(aes67.validate(sdp, false, nullptr), "96 kHz is valid AES67");
    TEST_ASSERT(!st2110.validate(sdp, false, nullptr), "96 kHz is outside ST 2110-30 Level A");
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
// B2. Dante
// ============================================================================

bool testDanteRequiresItsMulticastRange() {
    std::cout << "Test: B4 · Dante requires 239.69.0.0/16... ";
    const auto dante = CompatibilityProfile::forKind(CompatibilityProfileKind::Dante);

    auto sdp = baselineSession();
    sdp.connectionAddress = "239.69.1.10";
    std::string error;
    TEST_ASSERT(dante.validate(sdp, false, &error), "239.69.x.x must be accepted: " + error);

    sdp.connectionAddress = "239.1.1.10"; // valid AES67 multicast, wrong range for Dante
    error.clear();
    TEST_ASSERT(!dante.validate(sdp, false, &error), "Dante must reject an address outside 239.69.0.0/16");

    // A near-miss prefix must not false-positive (239.6 is not 239.69).
    sdp.connectionAddress = "239.6.1.10";
    TEST_ASSERT(!dante.validate(sdp, false, nullptr), "239.6.x.x must not match the 239.69 prefix");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testDanteDomainIsConfigurable() {
    std::cout << "Test: B5 · Dante's PTP domain is user-configurable, unlike AES67's... ";
    const auto dante = CompatibilityProfile::forKind(CompatibilityProfileKind::Dante);
    const auto aes67 = CompatibilityProfile::forKind(CompatibilityProfileKind::AES67);

    TEST_ASSERT(!dante.domainIsFixed, "Dante Controller allows domain segmentation (0-127)");
    TEST_ASSERT(aes67.domainIsFixed && aes67.fixedDomain == 0,
                "AES67's mandatory configuration is domain 0");

    // With domain free, any domain in a stream must be accepted.
    auto sdp = baselineSession();
    sdp.connectionAddress = "239.69.1.10";
    sdp.ptpDomain = 42;
    TEST_ASSERT(dante.validate(sdp, false, nullptr), "Dante must accept a non-zero domain");

    // With domain fixed at 0, the same stream must be rejected.
    std::string error;
    TEST_ASSERT(!aes67.validate(sdp, false, &error), "AES67 must reject a non-zero domain");
    TEST_ASSERT(!error.empty(), "rejection must explain itself");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testAES67AcceptsNoPtpDomainSentinel() {
    std::cout << "Test: B6 · domain -1 (\"no PTP\") is exempt from a fixed-domain requirement... ";
    const auto aes67 = CompatibilityProfile::forKind(CompatibilityProfileKind::AES67);
    auto sdp = baselineSession();
    sdp.ptpDomain = -1; // SDPSession's own sentinel for "no PTP on this stream"
    TEST_ASSERT(aes67.validate(sdp, false, nullptr),
                "domain -1 means \"no PTP\", not \"domain -1\" — must not be rejected as a domain mismatch");
    std::cout << "PASS" << std::endl;
    return true;
}

// ============================================================================
// B3. CP850 / DAC3202 — direction, from OUR driver's point of view
// ============================================================================

bool testCP850IsReceiveOnlyFromOurSide() {
    std::cout << "Test: B7 · CP850: this driver may only receive from it, up to 64 channels... ";
    const auto cp850 = CompatibilityProfile::forKind(CompatibilityProfileKind::CP850);
    TEST_ASSERT(cp850.direction == ProfileDirection::ReceiveOnly,
                "CP850 renders and sends over AES67, it doesn't accept AES67 input");
    TEST_ASSERT(cp850.maxTotalChannels == 64, "64 is the most the CP850 renders");

    auto sdp = baselineSession();
    sdp.sampleRate = 96000.0; // DCI's higher rate, not part of AES67's own three
    std::string error;
    TEST_ASSERT(cp850.validate(sdp, /*isTransmit=*/false, &error),
                "receiving from a CP850 at 96 kHz (DCI spec) must be accepted: " + error);
    TEST_ASSERT(!cp850.validate(sdp, /*isTransmit=*/true, &error),
                "this driver must not be allowed to transmit to a CP850");
    TEST_ASSERT(!error.empty(), "rejection must explain itself");
    std::cout << "PASS" << std::endl;
    return true;
}

bool testDAC3202IsTransmitOnlyFromOurSide() {
    std::cout << "Test: B8 · DAC3202: this driver may only transmit to it, up to 32 channels... ";
    const auto dac3202 = CompatibilityProfile::forKind(CompatibilityProfileKind::DAC3202);
    TEST_ASSERT(dac3202.direction == ProfileDirection::TransmitOnly,
                "DAC3202 only converts digital-in to analog-out, nothing returns to the network");
    TEST_ASSERT(dac3202.maxTotalChannels == 32, "32 is its full analog output count");

    auto sdp = baselineSession();
    sdp.sampleRate = 96000.0;
    std::string error;
    TEST_ASSERT(dac3202.validate(sdp, /*isTransmit=*/true, &error),
                "transmitting to a DAC3202 at 96 kHz (DCI spec) must be accepted: " + error);
    TEST_ASSERT(!dac3202.validate(sdp, /*isTransmit=*/false, &error),
                "this driver must not be allowed to receive from a DAC3202");
    TEST_ASSERT(!error.empty(), "rejection must explain itself");
    std::cout << "PASS" << std::endl;
    return true;
}

bool testOnlyCP850AndDAC3202RestrictDirection() {
    std::cout << "Test: B9 · every other profile leaves direction unrestricted... ";
    for (const auto& profile : CompatibilityProfile::all()) {
        if (profile.kind == CompatibilityProfileKind::CP850 ||
            profile.kind == CompatibilityProfileKind::DAC3202) {
            continue;
        }
        TEST_ASSERT(profile.direction == ProfileDirection::Any,
                    profile.displayName + " should not restrict direction");
        TEST_ASSERT(profile.maxTotalChannels == 0,
                    profile.displayName + " should not have an aggregate channel cap");
    }
    std::cout << "PASS" << std::endl;
    return true;
}

bool testCinemaProfilesRecordDscpAsInformationalOnly() {
    std::cout << "Test: B10 · CP850/DAC3202 record a DSCP value but this driver doesn't apply it... ";
    const auto cp850 = CompatibilityProfile::forKind(CompatibilityProfileKind::CP850);
    const auto dac3202 = CompatibilityProfile::forKind(CompatibilityProfileKind::DAC3202);
    TEST_ASSERT(cp850.recommendedDscp == 46, "CP850 documents EF (46)");
    TEST_ASSERT(dac3202.recommendedDscp == 46, "DAC3202 documents EF (46)");
    // AES67 and RAVENNA have no documented DSCP requirement from either
    // standard's own baseline — -1 means "none recorded", not "zero".
    const auto aes67 = CompatibilityProfile::forKind(CompatibilityProfileKind::AES67);
    TEST_ASSERT(aes67.recommendedDscp == -1, "AES67 baseline has no profile-specific DSCP");
    std::cout << "PASS" << std::endl;
    return true;
}

bool testCP850AndDAC3202ForcePTPRole() {
    std::cout << "Test: B11 · CP850 forces this driver to PTP slave, DAC3202 to PTP master... ";
    const auto cp850 = CompatibilityProfile::forKind(CompatibilityProfileKind::CP850);
    const auto dac3202 = CompatibilityProfile::forKind(CompatibilityProfileKind::DAC3202);
    TEST_ASSERT(cp850.ptpRole == PTPRoleConstraint::ForcedSlave,
                "this driver must always be PTP slave under the CP850 profile");
    TEST_ASSERT(dac3202.ptpRole == PTPRoleConstraint::ForcedMaster,
                "this driver must always be PTP master under the DAC3202 profile");

    for (const auto& profile : CompatibilityProfile::all()) {
        if (profile.kind == CompatibilityProfileKind::CP850 ||
            profile.kind == CompatibilityProfileKind::DAC3202) {
            continue;
        }
        TEST_ASSERT(profile.ptpRole == PTPRoleConstraint::Any,
                    profile.displayName + " should not force a PTP role — BMCA decides");
    }
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
        TEST_ASSERT(!profile.validate(sdp, transmitDirectionFor(profile), nullptr),
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
        TEST_ASSERT(!profile.validate(sdp, transmitDirectionFor(profile), nullptr),
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

    std::cout << "B2 · Dante" << std::endl;
    std::cout << "----------" << std::endl;
    testDanteRequiresItsMulticastRange();
    testDanteDomainIsConfigurable();
    testAES67AcceptsNoPtpDomainSentinel();
    std::cout << std::endl;

    std::cout << "B3 · CP850 / DAC3202 direction (always from our own point of view)" << std::endl;
    std::cout << "--------------------------------------------------------------------" << std::endl;
    testCP850IsReceiveOnlyFromOurSide();
    testDAC3202IsTransmitOnlyFromOurSide();
    testOnlyCP850AndDAC3202RestrictDirection();
    testCinemaProfilesRecordDscpAsInformationalOnly();
    testCP850AndDAC3202ForcePTPRole();
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
