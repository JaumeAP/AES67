#include "CompatibilityProfile.h"
#include "../Driver/DebugLog.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <pwd.h>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace AES67 {

// ============================================================================
// Profile definitions
// ============================================================================

CompatibilityProfile CompatibilityProfile::forKind(CompatibilityProfileKind kind) {
    CompatibilityProfile p;
    p.kind = kind;

    switch (kind) {
    case CompatibilityProfileKind::AES67:
        p.displayName = "AES67";
        // AES67's mandatory configuration: 1-8 channels, 16/24-bit,
        // 44.1/48/96 kHz, 1 ms packets.
        p.allowedSampleRates = {44100.0, 48000.0, 96000.0};
        p.allowedPtimesUs = {1000}; // 1 ms
        p.allowedEncodings = {"L16", "L24"};
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = false; // AES67 permits a random offset
        // AES67's mandatory configuration is PTP domain 0 — not a default
        // to override, part of what "AES67" means here. See
        // PTPSlaveConfig's own "PTP domain 0 (default), per AES67" comment.
        p.domainIsFixed = true;
        p.fixedDomain = 0;
        p.caveats =
            "Baseline. Accepts the three sample rates AES67 names; the device "
            "itself declares more (up to 384 kHz), which other AES67 gear may "
            "refuse. PTP domain fixed at 0.";
        break;

    case CompatibilityProfileKind::RAVENNA:
        p.displayName = "RAVENNA";
        // RAVENNA is a SUPERSET of AES67, so this profile must accept, not
        // narrow, what RAVENNA gear can send — otherwise a legitimate
        // RAVENNA stream (a high sample rate, a sub-millisecond packet time)
        // would be rejected on receive and the profile would be "compatible"
        // in name only. So:
        //  - the full RAVENNA sample-rate set, not AES67's three;
        //  - NO packet-time restriction at all (empty = validate() accepts
        //    any ptime), because RAVENNA frame sizes run 1-192 samples, a
        //    continuum of durations no fixed list could enumerate. Our own
        //    transmitter still emits 1 ms (a valid RAVENNA ptime); the empty
        //    set only widens what we ACCEPT, it doesn't make us send anything
        //    new.
        // Encodings stay L16/L24: those are what PCMCodec can actually
        // decode. RAVENNA also defines L32, but accepting an SDP we can't
        // decode would be a false claim, so it is deliberately excluded.
        p.allowedSampleRates = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
        p.allowedPtimesUs = {}; // empty = accept any packet time (RAVENNA is unrestricted here)
        p.allowedEncodings = {"L16", "L24"};
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = false;
        p.caveats =
            "A true AES67 superset on receive: accepts RAVENNA's full sample-"
            "rate set (44.1-192 kHz) and any packet time, so a RAVENNA source "
            "is not rejected for using a rate or ptime AES67 doesn't name. "
            "Two honest edges remain, both receiver-architecture limits, not "
            "RAVENNA ones: a single stream is still capped at 8 channels per "
            "flow (wider RAVENNA streams must be split), and only L16/L24 are "
            "decoded (RAVENNA's L32 is not). Transmit still emits 1 ms L24. "
            "RAVENNA's Bonjour discovery and stream redundancy are not "
            "implemented.";
        break;

    case CompatibilityProfileKind::ST2110_30:
        p.displayName = "SMPTE ST 2110-30 (Level A)";
        // Level A: 48 kHz only, 1 ms packets, 1-8 channels, 16/24-bit.
        // See Docs/st2110_30_vs_aes67.md.
        p.allowedSampleRates = {48000.0};
        p.allowedPtimesUs = {1000}; // 1 ms
        p.allowedEncodings = {"L16", "L24"}; // AM824 is ST 2110-31, not -30
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = true;
        p.caveats =
            "The mandatory level, and the safe common ground: gear claiming "
            "any higher level must support this one too. 48 kHz, 1 ms "
            "packets, up to 8 channels per stream. Pick Level B instead for "
            "125 us packets. ST 2110-30 also requires stricter PTP than "
            "AES67, and this driver's PTP has never been verified against a "
            "real grandmaster. Selecting this profile enforces the "
            "parameters it can check; it is not a conformance claim.";
        break;

    case CompatibilityProfileKind::ST2110_30_LevelB:
        p.displayName = "SMPTE ST 2110-30 (Level B)";
        // Level B is Level A at 125 us instead of 1 ms — same 48 kHz, same
        // 16/24-bit, same 1-8 channels per stream. Emitting 125 us packets
        // is possible as of the commit that moved packet time to
        // microseconds; before that this profile could not have been
        // honoured on transmit at all.
        //
        // Levels C, AX, BX and CX are deliberately absent:
        //  - C is Level B with up to 64 channels in ONE stream, which this
        //    driver can't do — StreamChannelMapper::kMaxChannelsPerFlow
        //    caps a flow at 8 and the flow splitter divides anything wider.
        //    Offering it would be a claim this driver can't honour.
        //  - AX/BX/CX are the 96 kHz variants with the channel counts
        //    halved (4, 4, 32). Supportable in principle; not added
        //    speculatively, since nothing has asked for them.
        p.allowedSampleRates = {48000.0};
        p.allowedPtimesUs = {125};
        p.allowedEncodings = {"L16", "L24"}; // AM824 is ST 2110-31, not -30
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = true;
        p.caveats =
            "Level A's constraints at a 125 us packet time: 48 kHz, up to 8 "
            "channels per stream. Only choose this if the receiving gear "
            "actually claims Level B — a Level A device must not be sent "
            "125 us packets, and Level A is what everything supports. This "
            "driver's transmitter emits whatever packet time the stream "
            "asks for, so 125 us is reachable, but it has never been tested "
            "against real Level B gear. Levels C (64 channels in one "
            "stream) and AX/BX/CX (96 kHz) are not offered — see the code "
            "comment for why. Same PTP caveat as Level A: ST 2110-30 "
            "requires stricter PTP than AES67 and this driver's has never "
            "been verified against a real grandmaster. Not a conformance "
            "claim.";
        break;

    case CompatibilityProfileKind::Dante:
        p.displayName = "Dante (AES67 mode)";
        // Source: Audinate's own Dante Controller "AES67 Config"
        // documentation. AES67 mode is NARROWER than AES67's own baseline
        // in three ways this profile previously got wrong by assuming
        // Dante simply inherited that baseline:
        //  - 48 kHz only. Dante devices run 44.1/96/... natively, but an
        //    AES67 flow out of one is 48 kHz, full stop.
        //  - L24 only. "AES67 flows generated by Dante devices must use 24
        //    bit linear encoding" — L16 is part of AES67's own baseline but
        //    not something a Dante device will produce or accept here.
        //  - PTPv2 domain fixed at 0. Dante's *native* PTPv1 clocking has
        //    its own domain concept, which is what the old "domains 0-127"
        //    comment here confused it with; AES67 mode itself is documented
        //    as a fixed domain 0, and consequently can only be enabled for
        //    one domain at a time.
        p.allowedSampleRates = {48000.0};
        p.allowedPtimesUs = {1000}; // 1 ms = 48 samples per channel per frame, per Audinate
        p.allowedEncodings = {"L24"};
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = false;
        p.domainIsFixed = true;
        p.fixedDomain = 0;
        // Dante marks audio EF/46 (and PTP CS7/56 — a documented conflict
        // with 'standard' AES67 implementations, which mark PTP 46
        // instead). Informational only, like every recommendedDscp here.
        p.recommendedDscp = 46;
        // 239.69 is Dante's factory-default multicast prefix, and Dante
        // Controller can change it (the documented range is 239.nnn/16).
        // Kept as a hard requirement anyway, per explicit instruction: a
        // profile is a filter, and this catches the common
        // misconfiguration. A site that has deliberately moved its prefix
        // should select the AES67 baseline profile instead of this one.
        p.requiredMulticastPrefix = "239.69";
        p.caveats =
            "Requires the Dante device to have AES67 mode explicitly enabled "
            "— this driver can't do that remotely, it's a setting on the "
            "Dante hardware itself (Dante Controller). Dante natively "
            "syncs with PTPv1; AES67 mode is what switches it to PTPv2, "
            "which is what this driver speaks, on a fixed domain 0. "
            "AES67 mode is narrower than AES67 itself: 48 kHz only "
            "(whatever the device runs natively), L24 only, 1 ms packets, "
            "port 5004. Enforces the 239.69.0.0/16 multicast range — that "
            "prefix is Dante's factory default and is configurable in Dante "
            "Controller, so a site that has moved it should use the AES67 "
            "baseline profile instead. Dante marks audio DSCP EF/46 and PTP "
            "CS7/56, where standard AES67 gear marks PTP 46 — a documented "
            "QoS conflict to watch for on shared networks, though this "
            "driver marks its own transmit traffic with Dante's audio "
            "value, 46.";
        break;

    case CompatibilityProfileKind::Dolby: {
        p.displayName = "Dolby";
        // One profile for the whole Dolby Atmos Connect family — the cinema
        // processors that send (CP850, CP950/CP950A, IMS3000) and the
        // downstream endpoints that receive (DAC3202, DMA amplifiers). They
        // share one protocol, so this profile carries the shared parameters
        // and stays permissive on the two things that differ per element —
        // direction and PTP role — because the driver is a processor to the
        // amplifiers (transmits, master) and an amplifier to the processors
        // (receives, slave). Which one applies is discovered per element by
        // the passive PTP peer observer; the specific model, and thus channel
        // count, is confirmed per element via DolbyModelCatalog. This replaces
        // the former one-profile-per-model scheme (CP850/CP950/DAC3202/DMA).
        //
        // Shared parameters, all confirmed across the family's manuals:
        //  - 48/96 kHz, 1 ms, L16/L24 — DCI cinema audio, not AES67's three
        //    rates (CP850/CP950A/DAC3202/DMA all identical here).
        //  - PTP domain factory default 109 (not fixed — installers set one
        //    per auditorium in multi-screen sites).
        //  - Destination multicast factory default 239.81.83.67 (CP950A/DMA/
        //    DAC3202 §3.8, shared).
        //  - The Atmos Connect wire scheme: one multicast address, fixed RTP
        //    destination port (pass 6517), source port stepped per 8-channel
        //    flow — see StreamManager::createTxStreamFlows(). Applies when
        //    transmitting to an amplifier.
        //  - Up to three units chained (DMA manual §2.3).
        //  - DSCP EF/46 is the family's documented audio marking (CP850,
        //    DAC3202); the DMA manual instead specifies switch queue-based
        //    QoS, so 46 here is the informational family default, applied to
        //    this driver's own transmit sockets when it plays the processor.
        p.allowedSampleRates = {48000.0, 96000.0};
        p.allowedPtimesUs = {1000}; // 1 ms — Dolby Atmos Connect
        p.allowedEncodings = {"L16", "L24"};
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = false;
        p.domainIsFixed = false;
        p.recommendedPtpDomain = 109;
        p.recommendedMulticastAddress = "239.81.83.67";
        p.recommendedDscp = 46;
        // Open both ways: the driver receives from processors and transmits
        // to amplifiers; the passive PTP observer identifies which each
        // detected element is (master peer = input source, slave peer =
        // output sink).
        p.direction = ProfileDirection::Any;
        p.ptpRole = PTPRoleConstraint::Any;
        // No profile-level channel cap: the real count comes from the
        // detected elements (DolbyModelCatalog) applied to the device's
        // usable channel selection, itself bounded by the 128-channel device.
        p.maxTotalChannels = 0;
        p.useFixedMulticastWithPerFlowSourcePort = true;
        p.maxUnits = 3; // §2.3: at most three chained without a switch
        p.caveats =
            "One profile for the whole Dolby Atmos Connect family — the "
            "processors that send (CP850, CP950/CP950A, IMS3000) and the "
            "endpoints that receive (DAC3202, DMA amplifiers). The driver is "
            "a processor to an amplifier (it transmits, it is PTP master) and "
            "an amplifier to a processor (it receives, it is PTP slave), so "
            "direction and PTP role are left open and worked out per element "
            "by passive PTP detection — each detected unit is listed on the "
            "Inputs or Outputs tab, and confirming its model there sets its "
            "channel count. Shared parameters are enforced: 48/96 kHz, 1 ms, "
            "L16/L24; PTP domain factory-default 109; destination multicast "
            "factory-default 239.81.83.67; the real Atmos Connect wire scheme "
            "(one multicast address, fixed RTP destination port — pass 6517 — "
            "with the source port stepped per 8-channel flow); up to three "
            "chained units. DSCP EF/46 is the family's documented audio "
            "marking (the DMA instead uses switch queue-based QoS). Selecting "
            "this profile is not a conformance claim; PTP has never been "
            "verified against real Dolby hardware.";
        break;
    }
    }

    return p;
}

std::vector<CompatibilityProfile> CompatibilityProfile::all() {
    return {
        forKind(CompatibilityProfileKind::AES67),
        forKind(CompatibilityProfileKind::RAVENNA),
        forKind(CompatibilityProfileKind::ST2110_30),
        forKind(CompatibilityProfileKind::ST2110_30_LevelB),
        forKind(CompatibilityProfileKind::Dante),
        forKind(CompatibilityProfileKind::Dolby),
    };
}

std::string CompatibilityProfile::kindToString(CompatibilityProfileKind kind) {
    switch (kind) {
    case CompatibilityProfileKind::AES67:     return "aes67";
    case CompatibilityProfileKind::RAVENNA:   return "ravenna";
    case CompatibilityProfileKind::ST2110_30: return "st2110-30";
    case CompatibilityProfileKind::ST2110_30_LevelB: return "st2110-30-b";
    case CompatibilityProfileKind::Dante:     return "dante";
    case CompatibilityProfileKind::Dolby:     return "dolby";
    }
    return "aes67";
}

CompatibilityProfileKind CompatibilityProfile::kindFromString(const std::string& s) {
    if (s == "ravenna")   return CompatibilityProfileKind::RAVENNA;
    if (s == "st2110-30") return CompatibilityProfileKind::ST2110_30;
    if (s == "st2110-30-b") return CompatibilityProfileKind::ST2110_30_LevelB;
    if (s == "dante")     return CompatibilityProfileKind::Dante;
    if (s == "dolby")     return CompatibilityProfileKind::Dolby;
    // Migrate the former one-profile-per-model ids to the unified Dolby one.
    if (s == "cp850" || s == "cp950" || s == "dac3202" || s == "dma")
        return CompatibilityProfileKind::Dolby;
    return CompatibilityProfileKind::AES67;
}

// ============================================================================
// Validation
// ============================================================================

namespace {

std::string joinRates(const std::vector<double>& rates) {
    std::ostringstream oss;
    for (size_t i = 0; i < rates.size(); ++i) {
        if (i) oss << ", ";
        oss << static_cast<long>(rates[i]);
    }
    return oss.str();
}

std::string joinStrings(const std::vector<std::string>& items) {
    std::ostringstream oss;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) oss << ", ";
        oss << items[i];
    }
    return oss.str();
}

bool addressHasPrefix(const std::string& address, const std::string& prefix) {
    // Prefix is a dotted fragment like "239.69"; require a following dot so
    // "239.6" doesn't match "239.69.x.x".
    if (address.size() <= prefix.size()) return false;
    return address.compare(0, prefix.size(), prefix) == 0 &&
           address[prefix.size()] == '.';
}

} // namespace

bool CompatibilityProfile::validate(const SDPSession& sdp, bool isTransmit, std::string* errorOut) const {
    auto fail = [&](const std::string& reason) {
        if (errorOut) *errorOut = displayName + ": " + reason;
        return false;
    };

    if (direction == ProfileDirection::ReceiveOnly && isTransmit) {
        return fail("this device has no network audio input — this driver may only receive from it, not send to it");
    }
    if (direction == ProfileDirection::TransmitOnly && !isTransmit) {
        return fail("this device has no network audio output — this driver may only send to it, not receive from it");
    }

    if (!allowedSampleRates.empty()) {
        const bool ok = std::any_of(allowedSampleRates.begin(), allowedSampleRates.end(),
            [&](double rate) {
                // SDP rates are integers in practice; compare with a
                // tolerance rather than exact double equality.
                return std::abs(rate - sdp.sampleRate) < 1.0;
            });
        if (!ok) {
            return fail("sample rate " + std::to_string(static_cast<long>(sdp.sampleRate)) +
                        " Hz not permitted (allowed: " + joinRates(allowedSampleRates) + ")");
        }
    }

    if (!allowedPtimesUs.empty() && sdp.ptimeUs > 0) {
        const bool ok = std::find(allowedPtimesUs.begin(), allowedPtimesUs.end(),
                                   sdp.ptimeUs) != allowedPtimesUs.end();
        if (!ok) {
            return fail("packet time " + std::to_string(sdp.ptimeUs) +
                        " us not permitted");
        }
    }

    if (!allowedEncodings.empty() && !sdp.encoding.empty()) {
        const bool ok = std::find(allowedEncodings.begin(), allowedEncodings.end(),
                                   sdp.encoding) != allowedEncodings.end();
        if (!ok) {
            return fail("encoding " + sdp.encoding + " not permitted (allowed: " +
                        joinStrings(allowedEncodings) + ")");
        }
    }

    if (sdp.numChannels > maxChannelsPerFlow) {
        return fail(std::to_string(sdp.numChannels) + " channels exceeds the " +
                    std::to_string(maxChannelsPerFlow) +
                    "-channel flow limit — split it across multiple flows");
    }

    if (!requiredMulticastPrefix.empty() && !sdp.connectionAddress.empty()) {
        if (!addressHasPrefix(sdp.connectionAddress, requiredMulticastPrefix)) {
            return fail("multicast address " + sdp.connectionAddress +
                        " outside the required " + requiredMulticastPrefix + ".0.0/16 range");
        }
    }

    // -1 means "no PTP for this stream" — not a domain choice at all, so it
    // isn't subject to a fixed-domain requirement.
    if (domainIsFixed && sdp.ptpDomain != -1 && sdp.ptpDomain != static_cast<int>(fixedDomain)) {
        return fail("PTP domain " + std::to_string(sdp.ptpDomain) + " not permitted — "
                    "fixed at " + std::to_string(fixedDomain));
    }

    return true;
}

// ============================================================================
// Persistence
// ============================================================================

CompatibilityProfileManager::CompatibilityProfileManager() {
    std::string existing = findExistingConfig();
    configPath_ = existing.empty()
        ? "/Library/Application Support/AES67Driver/" + std::string(kDefaultConfigFile)
        : existing;
}

CompatibilityProfileManager::~CompatibilityProfileManager() = default;

std::string CompatibilityProfileManager::getConfigPath() const { return configPath_; }

std::vector<std::string> CompatibilityProfileManager::getConfigSearchPaths() {
    std::vector<std::string> paths;

    const char* envPath = std::getenv("AES67_COMPAT_PROFILE_CONFIG_PATH");
    if (envPath && envPath[0] != '\0') paths.push_back(envPath);

    const char* home = std::getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (home && home[0] != '\0') {
        paths.push_back(std::string(home) + "/Library/Application Support/AES67Driver/" + kDefaultConfigFile);
    }

    paths.push_back("/Library/Application Support/AES67Driver/" + std::string(kDefaultConfigFile));
    return paths;
}

std::string CompatibilityProfileManager::findExistingConfig() {
    for (const auto& path : getConfigSearchPaths()) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return path;
    }
    return "";
}

bool CompatibilityProfileManager::ensureConfigDirectoryExists() {
    size_t lastSlash = configPath_.find_last_of('/');
    if (lastSlash == std::string::npos) return false;
    std::string dir = configPath_.substr(0, lastSlash);

    struct stat st;
    if (stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        AES67_LOGF("CompatibilityProfileManager: Failed to create directory '%s': %s",
                   dir.c_str(), ec.message().c_str());
        return false;
    }
    chmod(dir.c_str(), 0755);
    return true;
}

CompatibilityProfileKind CompatibilityProfileManager::load() {
    std::ifstream file(configPath_);
    if (!file.is_open()) return CompatibilityProfileKind::AES67;

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();

    std::regex pattern("\"profile\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    if (std::regex_search(json, match, pattern) && match.size() > 1) {
        const auto kind = CompatibilityProfile::kindFromString(match[1].str());
        AES67_LOGF("CompatibilityProfileManager: Loaded profile '%s' from %s",
                   CompatibilityProfile::kindToString(kind).c_str(), configPath_.c_str());
        return kind;
    }

    return CompatibilityProfileKind::AES67;
}

bool CompatibilityProfileManager::save(CompatibilityProfileKind kind) {
    if (!ensureConfigDirectoryExists()) {
        AES67_LOG("CompatibilityProfileManager: Failed to create config directory");
        return false;
    }

    std::ostringstream json;
    json << "{\n";
    json << "  \"version\": \"1.0\",\n";
    json << "  \"profile\": \"" << CompatibilityProfile::kindToString(kind) << "\"\n";
    json << "}\n";

    std::ofstream file(configPath_);
    if (!file.is_open()) {
        AES67_LOGF("CompatibilityProfileManager: Failed to open %s for writing", configPath_.c_str());
        return false;
    }
    file << json.str();
    return true;
}

} // namespace AES67
