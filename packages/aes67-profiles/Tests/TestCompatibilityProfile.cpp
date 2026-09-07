//
// TestCompatibilityProfile.cpp
// AES67 macOS Driver
// Tests for the per-standard constraint sets (Profiles/CompatibilityProfile.h).
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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Profiles/CompatibilityProfile.h"

#include <cmath>
#include <iostream>

using namespace AES67;



namespace {

/// A stream every profile here should accept in its own permitted
/// direction: 48 kHz, L24, 1 ms, 8 channels, Dante's multicast range (which
/// also happens to be a perfectly ordinary AES67 address), domain 0.
StreamDescription baselineSession() {
    StreamDescription stream;
    stream.connectionAddress = "239.69.1.10";
    stream.numChannels = 8;
    stream.sampleRate = 48000.0;
    stream.encoding = "L24";
    stream.ptimeUs = 1000;
    return stream;
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

TEST_CASE("All Profiles Accept The Common Baseline") {
    std::cout << "Test: A1 · every profile accepts 48kHz/L24/1ms/8ch, each in its own direction... ";
    const auto sdp = baselineSession(); // 239.69.1.10 — Dante's range, and no
                                        // other profile requires a prefix.
    for (const auto& profile : CompatibilityProfile::all()) {
        // ST 2110-30 Level B is the one profile that rejects the baseline
        // on purpose: it exists precisely to require 125 us packets where
        // the baseline (and Level A) use 1 ms.
        if (profile.kind == CompatibilityProfileKind::ST2110_30_LevelB) continue;
        std::string error;
        CHECK(profile.validate(sdp, transmitDirectionFor(profile), &error));
    }
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Every Profile Has Display Name And Caveats") {
    std::cout << "Test: A2 · every profile is presentable and honest about its gaps... ";
    for (const auto& profile : CompatibilityProfile::all()) {
        CHECK(!profile.displayName.empty());
        // Selecting a profile must never read as a conformance claim — each
        // one states what it can't enforce.
        CHECK(!profile.caveats.empty());
    }
    std::cout << "PASS" << std::endl;
}

// ============================================================================
// B. ST 2110-30 Level A restrictions
// ============================================================================

TEST_CASE("ST2110 Rejects Non48k Hz") {
    std::cout << "Test: B1 · ST 2110-30 Level A permits 48 kHz only... ";
    const auto profile = CompatibilityProfile::forKind(CompatibilityProfileKind::ST2110_30);

    auto sdp = baselineSession();
    std::string error;
    CHECK(profile.validate(sdp, false, &error));

    // 44.1 and 96 kHz are valid AES67 but outside ST 2110-30 Level A.
    for (double rate : {44100.0, 96000.0, 192000.0}) {
        sdp.sampleRate = rate;
        error.clear();
        CHECK(!profile.validate(sdp, false, &error));
        CHECK(!error.empty());
    }
    std::cout << "PASS" << std::endl;
}

TEST_CASE("ST2110 Level B Requires125us") {
    std::cout << "Test: B1b · ST 2110-30 Level B requires 125 us where Level A requires 1 ms... ";
    const auto levelA = CompatibilityProfile::forKind(CompatibilityProfileKind::ST2110_30);
    const auto levelB = CompatibilityProfile::forKind(CompatibilityProfileKind::ST2110_30_LevelB);

    CHECK(levelA.allowedPtimesUs == std::vector<uint32_t>{1000});
    CHECK(levelB.allowedPtimesUs == std::vector<uint32_t>{125});

    // The levels are claims about what the *receiving* gear supports, so
    // each must reject the other's packet time rather than quietly
    // accepting both — a Level A device sent 125 us packets is exactly the
    // failure this separation prevents.
    auto sdp = baselineSession(); // 1 ms
    std::string error;
    CHECK(levelA.validate(sdp, false, &error));
    CHECK(!levelB.validate(sdp, false, &error));

    sdp.ptimeUs = 125;
    CHECK(levelB.validate(sdp, false, &error));
    CHECK(!levelA.validate(sdp, false, &error));

    // Everything else is Level A's constraint set unchanged.
    CHECK(levelB.allowedSampleRates == levelA.allowedSampleRates);
    CHECK(levelB.maxChannelsPerFlow == levelA.maxChannelsPerFlow);
    CHECK(levelB.requiresZeroRtpTimestampOffset);

    std::cout << "PASS" << std::endl;
}

TEST_CASE("AES67 Accepts Rates ST2110 Rejects") {
    std::cout << "Test: B2 · the AES67 profile accepts rates ST 2110-30 refuses... ";
    const auto aes67 = CompatibilityProfile::forKind(CompatibilityProfileKind::AES67);
    const auto st2110 = CompatibilityProfile::forKind(CompatibilityProfileKind::ST2110_30);

    auto sdp = baselineSession();
    sdp.sampleRate = 96000.0;

    // This is the whole point of having separate profiles: the same stream
    // is fine for one target and not the other.
    CHECK(aes67.validate(sdp, false, nullptr));
    CHECK(!st2110.validate(sdp, false, nullptr));
    std::cout << "PASS" << std::endl;
}

TEST_CASE("ST2110 Requires Zero Rtp Offset Flag") {
    std::cout << "Test: B3 · ST 2110-30 records the zero-RTP-offset requirement... ";
    const auto st2110 = CompatibilityProfile::forKind(CompatibilityProfileKind::ST2110_30);
    const auto aes67 = CompatibilityProfile::forKind(CompatibilityProfileKind::AES67);

    // RTPTransmitter already starts at zero unconditionally, so nothing
    // enforces this at runtime — the flag exists so the zero isn't
    // randomised later for AES67 reasons, silently breaking ST 2110-30.
    CHECK(st2110.requiresZeroRtpTimestampOffset);
    CHECK(!aes67.requiresZeroRtpTimestampOffset);
    std::cout << "PASS" << std::endl;
}

// ============================================================================
// B2. Dante
// ============================================================================

TEST_CASE("Dante Requires Its Multicast Range") {
    std::cout << "Test: B4 · Dante requires 239.69.0.0/16... ";
    const auto dante = CompatibilityProfile::forKind(CompatibilityProfileKind::Dante);

    auto sdp = baselineSession();
    sdp.connectionAddress = "239.69.1.10";
    std::string error;
    CHECK(dante.validate(sdp, false, &error));

    sdp.connectionAddress = "239.1.1.10"; // valid AES67 multicast, wrong range for Dante
    error.clear();
    CHECK(!dante.validate(sdp, false, &error));

    // A near-miss prefix must not false-positive (239.6 is not 239.69).
    sdp.connectionAddress = "239.6.1.10";
    CHECK(!dante.validate(sdp, false, nullptr));

    std::cout << "PASS" << std::endl;
}

TEST_CASE("Dante AES67 Mode Is Narrower Than AES67 Itself") {
    std::cout << "Test: B5 · Dante's AES67 mode is narrower than AES67: 48k/L24/domain 0... ";
    const auto dante = CompatibilityProfile::forKind(CompatibilityProfileKind::Dante);
    const auto aes67 = CompatibilityProfile::forKind(CompatibilityProfileKind::AES67);

    // Per Audinate's own AES67 Config documentation. This profile used to
    // assume Dante simply inherited AES67's baseline (three rates, L16 or
    // L24, free domain) — it doesn't, on any of the three.
    CHECK((dante.allowedSampleRates.size() == 1 &&
                std::abs(dante.allowedSampleRates[0] - 48000.0) < 1.0));
    CHECK((dante.allowedEncodings.size() == 1 && dante.allowedEncodings[0] == "L24"));
    CHECK((dante.domainIsFixed && dante.fixedDomain == 0));
    CHECK(dante.recommendedDscp == 46);
    CHECK((aes67.domainIsFixed && aes67.fixedDomain == 0));

    // A rate AES67 itself permits but Dante's AES67 mode doesn't.
    auto sdp = baselineSession();
    sdp.connectionAddress = "239.69.1.10";
    sdp.sampleRate = 96000.0;
    std::string error;
    CHECK(!dante.validate(sdp, false, &error));
    CHECK(aes67.validate(sdp, false, nullptr));

    // Likewise an encoding AES67 permits but Dante's AES67 mode doesn't.
    sdp.sampleRate = 48000.0;
    sdp.encoding = "L16";
    CHECK(!dante.validate(sdp, false, &error));
    CHECK(aes67.validate(sdp, false, nullptr));

    // And a non-zero domain, now that Dante pins it like AES67 does.
    sdp.encoding = "L24";
    sdp.ptpDomain = 42;
    CHECK(!dante.validate(sdp, false, &error));
    CHECK(!error.empty());

    std::cout << "PASS" << std::endl;
}

TEST_CASE("AES67 Accepts No Ptp Domain Sentinel") {
    std::cout << "Test: B6 · domain -1 (\"no PTP\") is exempt from a fixed-domain requirement... ";
    const auto aes67 = CompatibilityProfile::forKind(CompatibilityProfileKind::AES67);
    auto sdp = baselineSession();
    sdp.ptpDomain = -1; // SDPSession's own sentinel for "no PTP on this stream"
    CHECK(aes67.validate(sdp, false, nullptr));
    std::cout << "PASS" << std::endl;
}

// ============================================================================
// B3. CP850 / DAC3202 — direction, from OUR driver's point of view
// ============================================================================

TEST_CASE("Dolby Is One Family Profile Open Both Ways") {
    std::cout << "Test: B7 · Dolby is one family profile, open in direction and PTP role... ";
    const auto dolby = CompatibilityProfile::forKind(CompatibilityProfileKind::Dolby);

    // Covers processors (input) and amplifiers (output), so it must not
    // restrict direction or PTP role — detection sorts each element out.
    CHECK(dolby.direction == ProfileDirection::Any);
    CHECK(dolby.ptpRole == PTPRoleConstraint::Any);

    // No profile-level channel cap — the count comes from detection.
    CHECK(dolby.maxTotalChannels == 0);

    // Shared family parameters.
    CHECK(dolby.recommendedPtpDomain == 109);
    CHECK(!dolby.domainIsFixed);
    CHECK(dolby.recommendedMulticastAddress == "239.81.83.67");
    CHECK(dolby.recommendedDscp == 46);
    CHECK(dolby.maxUnits == 1);
    CHECK(!dolby.usesLanAutoDetection);
    CHECK(dolby.useFixedMulticastWithPerFlowSourcePort);

    // Both directions accepted at the family's rates.
    auto sdp = baselineSession();
    sdp.sampleRate = 96000.0; // DCI's higher rate
    std::string error;
    CHECK(dolby.validate(sdp, /*isTransmit=*/true, &error));
    CHECK(dolby.validate(sdp, /*isTransmit=*/false, &error));
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Dolby Rejects Non Family Parameters") {
    std::cout << "Test: B8 · Dolby still enforces the shared family parameters... ";
    const auto dolby = CompatibilityProfile::forKind(CompatibilityProfileKind::Dolby);
    std::string error;

    // 44.1 kHz isn't a DCI cinema rate.
    auto badRate = baselineSession();
    badRate.sampleRate = 44100.0;
    CHECK(!dolby.validate(badRate, /*isTransmit=*/true, &error));

    // More than 8 channels in one flow is still rejected.
    auto wide = baselineSession();
    wide.numChannels = 16;
    CHECK(!dolby.validate(wide, /*isTransmit=*/true, &error));
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Dolby LAN Adds Detection And Chaining") {
    std::cout << "Test: B9 · Dolby LAN = Dolby plus auto-detection and up-to-3 output units... ";
    const auto dolby = CompatibilityProfile::forKind(CompatibilityProfileKind::Dolby);
    const auto lan = CompatibilityProfile::forKind(CompatibilityProfileKind::DolbyLAN);

    // Same shared family parameters as plain Dolby.
    CHECK(lan.allowedSampleRates == dolby.allowedSampleRates);
    CHECK(lan.allowedEncodings == dolby.allowedEncodings);
    CHECK(lan.recommendedPtpDomain == 109);
    CHECK(lan.recommendedMulticastAddress == "239.81.83.67");
    CHECK(lan.recommendedDscp == 46);
    CHECK(lan.direction == ProfileDirection::Any);
    CHECK(lan.ptpRole == PTPRoleConstraint::Any);
    CHECK(lan.useFixedMulticastWithPerFlowSourcePort);

    // The two differences.
    CHECK(lan.usesLanAutoDetection);
    CHECK(!dolby.usesLanAutoDetection);
    CHECK(lan.maxUnits == 3);
    CHECK(dolby.maxUnits == 1);
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Profiles Record The Dscp Their Gear Expects") {
    std::cout << "Test: B10 · profiles carry the DSCP their gear expects... ";
    const auto aes67 = CompatibilityProfile::forKind(CompatibilityProfileKind::AES67);
    const auto dante = CompatibilityProfile::forKind(CompatibilityProfileKind::Dante);
    const auto dolby = CompatibilityProfile::forKind(CompatibilityProfileKind::Dolby);

    // AES67/RAVENNA have no profile-specific DSCP; -1 means "none recorded".
    CHECK(aes67.recommendedDscp == -1);
    // Dante marks audio EF/46 (Audinate).
    CHECK(dante.recommendedDscp == 46);
    // Dolby's documented family audio marking is EF/46.
    CHECK(dolby.recommendedDscp == 46);

    // -1 must stay distinguishable from 0 (a real codepoint), and every
    // value is a valid 6-bit DSCP.
    for (const auto& profile : CompatibilityProfile::all()) {
        CHECK((profile.recommendedDscp == -1 || profile.recommendedDscp > 0));
        CHECK(profile.recommendedDscp <= 63);
    }
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Only Dolby Endpoints Chain Multiple Units") {
    std::cout << "Test: B14 · only the Dolby profile chains more than one unit (max 3)... ";
    for (const auto& profile : CompatibilityProfile::all()) {
        const bool isDolby = CompatibilityProfile::kindToString(profile.kind).rfind("dolby", 0) == 0;
        if (!isDolby) {
            // No non-Dolby profile chains — a single unit only.
            CHECK(profile.maxUnits == 1);
            continue;
        }
        // Within the Dolby family, only the OUTPUT profiles (amplifiers this
        // driver feeds) chain, up to three; input/generic ones stay at one.
        const bool chains = profile.maxUnits == 3;
        if (chains) {
            CHECK((profile.direction == ProfileDirection::TransmitOnly || profile.direction == ProfileDirection::Any));
            CHECK(profile.useFixedMulticastWithPerFlowSourcePort);
        } else {
            CHECK(profile.maxUnits == 1);
        }
    }
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Only DMA And DAC3202 Use The Fixed Multicast Addressing Scheme") {
    std::cout << "Test: B13 · only the Dolby profile uses the fixed-multicast/per-flow-source-port scheme... ";
    for (const auto& profile : CompatibilityProfile::all()) {
        const bool isDolby = CompatibilityProfile::kindToString(profile.kind).rfind("dolby", 0) == 0;
        if (isDolby) continue; // the whole Dolby family may use the Atmos Connect scheme
        CHECK(!profile.useFixedMulticastWithPerFlowSourcePort);
    }
    std::cout << "PASS" << std::endl;
}

// ============================================================================
// C. Limits shared by every profile
// ============================================================================

TEST_CASE("No Profile Raises The Flow Channel Limit Past The Transport") {
    std::cout << "Test: C1 · no profile permits more than 64 channels in one flow... ";
    auto sdp = baselineSession();
    sdp.numChannels = 65;

    // A profile can only narrow what this driver accepts, never widen it
    // past what the code implements: 64 is the widest flow the RTP path
    // carries (StreamChannelMapper::kMaxChannelsPerFlow), RAVENNA's and
    // ST 2110-30 Level C's ceiling. Whether 64 fit in a frame depends on
    // the packet time and is the driver's check, not a profile's.
    for (const auto& profile : CompatibilityProfile::all()) {
        CHECK(profile.maxChannelsPerFlow <= 64);
        CHECK(!profile.validate(sdp, transmitDirectionFor(profile), nullptr));
    }
    std::cout << "PASS" << std::endl;
}

TEST_CASE("Only RAVENNA Takes A Flow Wider Than Eight") {
    std::cout << "Test: C1b · RAVENNA accepts 64 channels in one flow, AES67 and Dante keep 8... ";
    const auto ravenna = CompatibilityProfile::forKind(CompatibilityProfileKind::RAVENNA);
    const auto aes67 = CompatibilityProfile::forKind(CompatibilityProfileKind::AES67);
    const auto dante = CompatibilityProfile::forKind(CompatibilityProfileKind::Dante);

    // What Merging's gear sends: 64 channels of L24 at 125 us.
    auto wide = baselineSession();
    wide.numChannels = 64;
    wide.ptimeUs = 125;
    CHECK(ravenna.maxChannelsPerFlow == 64);
    CHECK(ravenna.validate(wide, /*isTransmit=*/false, nullptr));

    // AES67's own limit and Dante Controller's split stay at 8.
    wide.connectionAddress = "239.69.1.10";
    CHECK(aes67.maxChannelsPerFlow == 8);
    CHECK(dante.maxChannelsPerFlow == 8);
    wide.ptimeUs = 1000;
    CHECK(!aes67.validate(wide, /*isTransmit=*/false, nullptr));
    CHECK(!dante.validate(wide, /*isTransmit=*/false, nullptr));
    std::cout << "PASS" << std::endl;
}

TEST_CASE("No Profile Accepts AM824") {
    std::cout << "Test: C2 · no profile accepts AM824 (that's ST 2110-31)... ";
    auto sdp = baselineSession();
    sdp.encoding = "AM824";
    for (const auto& profile : CompatibilityProfile::all()) {
        CHECK(!profile.validate(sdp, transmitDirectionFor(profile), nullptr));
    }
    std::cout << "PASS" << std::endl;
}

// ============================================================================
// D. Persistence round-trip of the selection's identity
// ============================================================================

TEST_CASE("Kind String Round Trip") {
    std::cout << "Test: D1 · profile identity survives the round trip to disk... ";
    for (const auto& profile : CompatibilityProfile::all()) {
        const std::string s = CompatibilityProfile::kindToString(profile.kind);
        CHECK(CompatibilityProfile::kindFromString(s) == profile.kind);
    }
    // An unknown or corrupt value must fall back to the unrestricted
    // baseline, never to a profile that would reject working streams.
    CHECK(CompatibilityProfile::kindFromString("") == CompatibilityProfileKind::AES67);
    CHECK(CompatibilityProfile::kindFromString("nonsense") == CompatibilityProfileKind::AES67);
    // The former per-model ids migrate to the unified Dolby profile.
    for (const char* legacy : {"cp850", "cp950", "dac3202", "dma"}) {
        CHECK(CompatibilityProfile::kindFromString(legacy) == CompatibilityProfileKind::DolbyLAN);
    }
    std::cout << "PASS" << std::endl;
}

// ============================================================================

