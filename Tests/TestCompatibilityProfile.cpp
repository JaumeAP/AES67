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
// everything else in this driver that hasn't been run end to end. Same
// applies to useFixedMulticastWithPerFlowSourcePort: this file confirms
// only DMA sets it; the actual per-flow source-port arithmetic it drives
// lives in StreamManager::createTxStreamFlows() and is untested here for
// the same reason.
//

#include "../NetworkEngine/CompatibilityProfile.h"

#include <cmath>
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
    sdp.ptimeUs = 1000;
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
    for (const auto& profile : CompatibilityProfile::all()) {
        // ST 2110-30 Level B is the one profile that rejects the baseline
        // on purpose: it exists precisely to require 125 us packets where
        // the baseline (and Level A) use 1 ms.
        if (profile.kind == CompatibilityProfileKind::ST2110_30_LevelB) continue;
        auto sdp = baselineSession();
        // Use a multicast the profile actually permits — Dante needs 239.69,
        // the Dolby family needs 239.81; a profile with no required prefix
        // accepts the default.
        if (!profile.requiredMulticastPrefix.empty()) {
            sdp.connectionAddress = profile.requiredMulticastPrefix + ".1.10";
        }
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

bool testST2110LevelBRequires125us() {
    std::cout << "Test: B1b · ST 2110-30 Level B requires 125 us where Level A requires 1 ms... ";
    const auto levelA = CompatibilityProfile::forKind(CompatibilityProfileKind::ST2110_30);
    const auto levelB = CompatibilityProfile::forKind(CompatibilityProfileKind::ST2110_30_LevelB);

    TEST_ASSERT(levelA.allowedPtimesUs == std::vector<uint32_t>{1000}, "Level A is 1 ms");
    TEST_ASSERT(levelB.allowedPtimesUs == std::vector<uint32_t>{125}, "Level B is 125 us");

    // The levels are claims about what the *receiving* gear supports, so
    // each must reject the other's packet time rather than quietly
    // accepting both — a Level A device sent 125 us packets is exactly the
    // failure this separation prevents.
    auto sdp = baselineSession(); // 1 ms
    std::string error;
    TEST_ASSERT(levelA.validate(sdp, false, &error), "Level A must accept 1 ms: " + error);
    TEST_ASSERT(!levelB.validate(sdp, false, &error), "Level B must reject 1 ms");

    sdp.ptimeUs = 125;
    TEST_ASSERT(levelB.validate(sdp, false, &error), "Level B must accept 125 us: " + error);
    TEST_ASSERT(!levelA.validate(sdp, false, &error), "Level A must reject 125 us");

    // Everything else is Level A's constraint set unchanged.
    TEST_ASSERT(levelB.allowedSampleRates == levelA.allowedSampleRates, "same 48 kHz-only rate");
    TEST_ASSERT(levelB.maxChannelsPerFlow == levelA.maxChannelsPerFlow, "same 8-channel flow limit");
    TEST_ASSERT(levelB.requiresZeroRtpTimestampOffset, "Level B keeps the zero-RTP-offset requirement");

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

bool testDanteAES67ModeIsNarrowerThanAES67Itself() {
    std::cout << "Test: B5 · Dante's AES67 mode is narrower than AES67: 48k/L24/domain 0... ";
    const auto dante = CompatibilityProfile::forKind(CompatibilityProfileKind::Dante);
    const auto aes67 = CompatibilityProfile::forKind(CompatibilityProfileKind::AES67);

    // Per Audinate's own AES67 Config documentation. This profile used to
    // assume Dante simply inherited AES67's baseline (three rates, L16 or
    // L24, free domain) — it doesn't, on any of the three.
    TEST_ASSERT(dante.allowedSampleRates.size() == 1 &&
                std::abs(dante.allowedSampleRates[0] - 48000.0) < 1.0,
                "Dante AES67 flows are 48 kHz only, whatever the device runs natively");
    TEST_ASSERT(dante.allowedEncodings.size() == 1 && dante.allowedEncodings[0] == "L24",
                "Dante AES67 flows must use 24-bit linear encoding");
    TEST_ASSERT(dante.domainIsFixed && dante.fixedDomain == 0,
                "Dante's AES67 mode uses a fixed PTPv2 domain 0 — the 0-127 range belongs to "
                "its native PTPv1 clocking, not to AES67 mode");
    TEST_ASSERT(dante.recommendedDscp == 46, "Dante marks audio EF/46");
    TEST_ASSERT(aes67.domainIsFixed && aes67.fixedDomain == 0,
                "AES67's mandatory configuration is domain 0");

    // A rate AES67 itself permits but Dante's AES67 mode doesn't.
    auto sdp = baselineSession();
    sdp.connectionAddress = "239.69.1.10";
    sdp.sampleRate = 96000.0;
    std::string error;
    TEST_ASSERT(!dante.validate(sdp, false, &error), "Dante must reject 96 kHz in AES67 mode");
    TEST_ASSERT(aes67.validate(sdp, false, nullptr), "AES67 itself permits 96 kHz");

    // Likewise an encoding AES67 permits but Dante's AES67 mode doesn't.
    sdp.sampleRate = 48000.0;
    sdp.encoding = "L16";
    TEST_ASSERT(!dante.validate(sdp, false, &error), "Dante must reject L16 in AES67 mode");
    TEST_ASSERT(aes67.validate(sdp, false, nullptr), "AES67 itself permits L16");

    // And a non-zero domain, now that Dante pins it like AES67 does.
    sdp.encoding = "L24";
    sdp.ptpDomain = 42;
    TEST_ASSERT(!dante.validate(sdp, false, &error), "Dante must reject a non-zero PTP domain");
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

bool testDolbyIsOneFamilyProfileOpenBothWays() {
    std::cout << "Test: B7 · Dolby is one family profile, open in direction and PTP role... ";
    const auto dolby = CompatibilityProfile::forKind(CompatibilityProfileKind::Dolby);

    // Covers processors (input) and amplifiers (output), so it must not
    // restrict direction or PTP role — detection sorts each element out.
    TEST_ASSERT(dolby.direction == ProfileDirection::Any,
                "Dolby covers both processors and amplifiers, so direction is open");
    TEST_ASSERT(dolby.ptpRole == PTPRoleConstraint::Any,
                "Dolby is master to amplifiers and slave to processors, so PTP role is open");

    // No profile-level channel cap — the count comes from detection.
    TEST_ASSERT(dolby.maxTotalChannels == 0, "Dolby places no profile channel cap");

    // Shared family parameters.
    TEST_ASSERT(dolby.recommendedPtpDomain == 109, "Dolby PTP domain factory default 109");
    TEST_ASSERT(!dolby.domainIsFixed, "Dolby PTP domain isn't fixed — set per auditorium");
    TEST_ASSERT(dolby.recommendedMulticastAddress == "239.81.83.67",
                "Dolby shares the family destination multicast default");
    TEST_ASSERT(dolby.recommendedDscp == 46, "Dolby records the family EF/46 audio marking");
    TEST_ASSERT(dolby.maxUnits == 1, "plain Dolby is a single generic unit (max 1)");
    TEST_ASSERT(!dolby.usesLanAutoDetection, "plain Dolby has no auto-detection");
    TEST_ASSERT(dolby.useFixedMulticastWithPerFlowSourcePort,
                "Dolby uses the Atmos Connect fixed-multicast/per-flow-source-port scheme");

    // Both directions accepted at the family's rates.
    auto sdp = baselineSession();
    sdp.connectionAddress = "239.81.83.67"; // Dolby's required multicast range
    sdp.sampleRate = 96000.0; // DCI's higher rate
    std::string error;
    TEST_ASSERT(dolby.validate(sdp, /*isTransmit=*/true, &error),
                "transmitting to a Dolby amplifier at 96 kHz must be accepted: " + error);
    TEST_ASSERT(dolby.validate(sdp, /*isTransmit=*/false, &error),
                "receiving from a Dolby processor at 96 kHz must be accepted: " + error);
    std::cout << "PASS" << std::endl;
    return true;
}

bool testDolbyRejectsNonFamilyParameters() {
    std::cout << "Test: B8 · Dolby still enforces the shared family parameters... ";
    const auto dolby = CompatibilityProfile::forKind(CompatibilityProfileKind::Dolby);
    std::string error;

    // 44.1 kHz isn't a DCI cinema rate.
    auto badRate = baselineSession();
    badRate.sampleRate = 44100.0;
    TEST_ASSERT(!dolby.validate(badRate, /*isTransmit=*/true, &error),
                "Dolby must reject 44.1 kHz (not a DCI rate)");

    // More than 8 channels in one flow is still rejected.
    auto wide = baselineSession();
    wide.numChannels = 16;
    TEST_ASSERT(!dolby.validate(wide, /*isTransmit=*/true, &error),
                "Dolby must reject more than 8 channels in one flow");
    std::cout << "PASS" << std::endl;
    return true;
}

bool testDolbyLANAddsDetectionAndChaining() {
    std::cout << "Test: B9 · Dolby LAN = Dolby plus auto-detection and up-to-3 output units... ";
    const auto dolby = CompatibilityProfile::forKind(CompatibilityProfileKind::Dolby);
    const auto lan = CompatibilityProfile::forKind(CompatibilityProfileKind::DolbyLAN);

    // Same shared family parameters as plain Dolby.
    TEST_ASSERT(lan.allowedSampleRates == dolby.allowedSampleRates, "same rates as Dolby");
    TEST_ASSERT(lan.allowedEncodings == dolby.allowedEncodings, "same encodings as Dolby");
    TEST_ASSERT(lan.recommendedPtpDomain == 109, "same PTP domain default");
    TEST_ASSERT(lan.recommendedMulticastAddress == "239.81.83.67", "same multicast default");
    TEST_ASSERT(lan.recommendedDscp == 46, "same DSCP");
    TEST_ASSERT(lan.direction == ProfileDirection::Any, "open direction");
    TEST_ASSERT(lan.ptpRole == PTPRoleConstraint::Any, "open PTP role");
    TEST_ASSERT(lan.useFixedMulticastWithPerFlowSourcePort, "Atmos Connect wire scheme");

    // The two differences.
    TEST_ASSERT(lan.usesLanAutoDetection, "Dolby LAN auto-detects; plain Dolby does not");
    TEST_ASSERT(!dolby.usesLanAutoDetection, "plain Dolby does not auto-detect");
    TEST_ASSERT(lan.maxUnits == 3, "Dolby LAN chains up to three OUTPUT units");
    TEST_ASSERT(dolby.maxUnits == 1, "plain Dolby is a single unit");
    std::cout << "PASS" << std::endl;
    return true;
}

bool testProfilesRecordTheDscpTheirGearExpects() {
    std::cout << "Test: B10 · profiles carry the DSCP their gear expects... ";
    const auto aes67 = CompatibilityProfile::forKind(CompatibilityProfileKind::AES67);
    const auto dante = CompatibilityProfile::forKind(CompatibilityProfileKind::Dante);
    const auto dolby = CompatibilityProfile::forKind(CompatibilityProfileKind::Dolby);

    // AES67/RAVENNA have no profile-specific DSCP; -1 means "none recorded".
    TEST_ASSERT(aes67.recommendedDscp == -1, "AES67 baseline has no profile-specific DSCP");
    // Dante marks audio EF/46 (Audinate).
    TEST_ASSERT(dante.recommendedDscp == 46, "Dante marks audio EF (46)");
    // Dolby's documented family audio marking is EF/46.
    TEST_ASSERT(dolby.recommendedDscp == 46, "Dolby records the family EF (46) audio marking");

    // -1 must stay distinguishable from 0 (a real codepoint), and every
    // value is a valid 6-bit DSCP.
    for (const auto& profile : CompatibilityProfile::all()) {
        TEST_ASSERT(profile.recommendedDscp == -1 || profile.recommendedDscp > 0,
                    profile.displayName + ": DSCP must be -1 (unmarked) or a real codepoint");
        TEST_ASSERT(profile.recommendedDscp <= 63,
                    profile.displayName + ": DSCP is a 6-bit field");
    }
    std::cout << "PASS" << std::endl;
    return true;
}

bool testOnlyDolbyEndpointsChainMultipleUnits() {
    std::cout << "Test: B14 · only the Dolby profile chains more than one unit (max 3)... ";
    for (const auto& profile : CompatibilityProfile::all()) {
        const bool isDolby = CompatibilityProfile::kindToString(profile.kind).rfind("dolby", 0) == 0;
        if (!isDolby) {
            // No non-Dolby profile chains — a single unit only.
            TEST_ASSERT(profile.maxUnits == 1,
                        profile.displayName + " is a single-unit profile");
            continue;
        }
        // Within the Dolby family, only the OUTPUT profiles (amplifiers this
        // driver feeds) chain, up to three; input/generic ones stay at one.
        const bool chains = profile.maxUnits == 3;
        if (chains) {
            TEST_ASSERT(profile.direction == ProfileDirection::TransmitOnly ||
                        profile.direction == ProfileDirection::Any,
                        profile.displayName + " chaining is output-side");
            TEST_ASSERT(profile.useFixedMulticastWithPerFlowSourcePort,
                        profile.displayName + " must use per-flow source ports for unit "
                        "selection to mean anything");
        } else {
            TEST_ASSERT(profile.maxUnits == 1,
                        profile.displayName + " is a single-unit Dolby profile");
        }
    }
    std::cout << "PASS" << std::endl;
    return true;
}

bool testOnlyDMAAndDAC3202UseTheFixedMulticastAddressingScheme() {
    std::cout << "Test: B13 · only the Dolby profile uses the fixed-multicast/per-flow-source-port scheme... ";
    for (const auto& profile : CompatibilityProfile::all()) {
        const bool isDolby = CompatibilityProfile::kindToString(profile.kind).rfind("dolby", 0) == 0;
        if (isDolby) continue; // the whole Dolby family may use the Atmos Connect scheme
        TEST_ASSERT(!profile.useFixedMulticastWithPerFlowSourcePort,
                    profile.displayName + " must keep the AES67/Dante default addressing scheme");
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
    // The former per-model ids migrate to the unified Dolby profile.
    for (const char* legacy : {"cp850", "cp950", "dac3202", "dma"}) {
        TEST_ASSERT(CompatibilityProfile::kindFromString(legacy) == CompatibilityProfileKind::DolbyLAN,
                    std::string("legacy id ") + legacy + " must migrate to Dolby LAN");
    }
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
    testST2110LevelBRequires125us();
    testAES67AcceptsRatesST2110Rejects();
    testST2110RequiresZeroRtpOffsetFlag();
    std::cout << std::endl;

    std::cout << "B2 · Dante" << std::endl;
    std::cout << "----------" << std::endl;
    testDanteRequiresItsMulticastRange();
    testDanteAES67ModeIsNarrowerThanAES67Itself();
    testAES67AcceptsNoPtpDomainSentinel();
    std::cout << std::endl;

    std::cout << "B3 · Dolby (one family profile, direction/role open)" << std::endl;
    std::cout << "-----------------------------------------------------" << std::endl;
    testDolbyIsOneFamilyProfileOpenBothWays();
    testDolbyRejectsNonFamilyParameters();
    testDolbyLANAddsDetectionAndChaining();
    testProfilesRecordTheDscpTheirGearExpects();
    testOnlyDMAAndDAC3202UseTheFixedMulticastAddressingScheme();
    testOnlyDolbyEndpointsChainMultipleUnits();
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
