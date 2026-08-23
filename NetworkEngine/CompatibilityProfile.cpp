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
        p.allowedPtimesMs = {1};
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
        // RAVENNA is natively AES67 and imposes no *narrower* limits on
        // these parameters — it is more permissive, not less (1-192 samples
        // per packet vs AES67's fixed 1 ms). Since this driver's
        // transmitter only emits 1 ms packets, that extra freedom isn't
        // reachable, so the enforced constraint set is identical to AES67.
        p.allowedSampleRates = {44100.0, 48000.0, 96000.0};
        p.allowedPtimesMs = {1};
        p.allowedEncodings = {"L16", "L24"};
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = false;
        p.caveats =
            "Constraints are currently identical to AES67: RAVENNA is more "
            "permissive, not less, and the extra freedom (1-192 samples per "
            "packet) needs a configurable transmit packet time this driver "
            "doesn't have yet. RAVENNA's own additions — Bonjour discovery "
            "and stream redundancy — are not implemented.";
        break;

    case CompatibilityProfileKind::ST2110_30:
        p.displayName = "SMPTE ST 2110-30 (Level A)";
        // Level A: 48 kHz only, 1 ms packets, 1-8 channels, 16/24-bit.
        // See Docs/st2110_30_vs_aes67.md.
        p.allowedSampleRates = {48000.0};
        p.allowedPtimesMs = {1};
        p.allowedEncodings = {"L16", "L24"}; // AM824 is ST 2110-31, not -30
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = true;
        p.caveats =
            "Level A only — Levels B and C need 125 us packets, which this "
            "driver's transmitter can't emit (it is fixed at 1 ms). "
            "ST 2110-30 also requires stricter PTP than AES67, and this "
            "driver's PTP has never been verified against a real "
            "grandmaster. Selecting this profile enforces the parameters it "
            "can check; it is not a conformance claim.";
        break;

    case CompatibilityProfileKind::Dante:
        p.displayName = "Dante (AES67 mode)";
        // Dante in AES67 mode conforms to AES67's own mandatory
        // configuration — the difference is entirely in addressing, not
        // sample rate/ptime/encoding.
        p.allowedSampleRates = {44100.0, 48000.0, 96000.0};
        p.allowedPtimesMs = {1};
        p.allowedEncodings = {"L16", "L24"};
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = false;
        // Dante Controller lets a network be split into domains (0-127) for
        // isolating multiple Dante networks — not pinned to one value the
        // way AES67's mandatory config is.
        p.domainIsFixed = false;
        p.requiredMulticastPrefix = "239.69";
        p.caveats =
            "Requires the Dante device to have AES67 mode explicitly enabled "
            "— this driver can't do that remotely, it's a setting on the "
            "Dante hardware itself (Dante Controller). Dante natively "
            "syncs with PTPv1; AES67 mode is what switches it to PTPv2, "
            "which is what this driver speaks. Enforces the "
            "239.69.0.0/16 multicast range Dante requires in AES67 mode.";
        break;

    case CompatibilityProfileKind::CP850:
        p.displayName = "Dolby CP850 (Atmos Cinema Processor)";
        // Digital cinema audio (DCI spec): 48 or 96 kHz, up to 24-bit PCM.
        // Not AES67's 44.1 kHz — cinema doesn't use it.
        p.allowedSampleRates = {48000.0, 96000.0};
        p.allowedPtimesMs = {1};
        p.allowedEncodings = {"L16", "L24"};
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = false;
        p.domainIsFixed = false; // no documented fixed domain; cinema installs set their own house PTP domain
        p.recommendedDscp = 46;  // EF — Dolby's documented value for AES67 traffic on this line
        p.caveats =
            "The CP850 uses AES67 as its transport to Dolby Atmos Connect "
            "Interfaces (DAC3202), not the full Dante protocol. Dolby's own "
            "documentation notes it applies a more traditional DSCP marking "
            "than typical Dante configurations (EF/46) — this driver has a "
            "DSCP-setting function (NetworkUtils::setQoSTrafficClass) but "
            "nothing calls it yet, so no marking is actually applied. No "
            "documented fixed PTP domain; cinema installations set their own.";
        break;

    case CompatibilityProfileKind::DAC3202:
        p.displayName = "Dolby DAC3202 (Atmos Connect Interface)";
        // Receiving end of the same CP850 link — same audio parameters.
        p.allowedSampleRates = {48000.0, 96000.0};
        p.allowedPtimesMs = {1};
        p.allowedEncodings = {"L16", "L24"};
        // 32 analog outputs per interface — exactly 4 flows at this
        // driver's 8-channel-per-flow limit, not a coincidence: AES67
        // itself is why the DAC3202 is organized that way.
        p.maxChannelsPerFlow = 8;
        p.requiresZeroRtpTimestampOffset = false;
        p.domainIsFixed = false;
        p.recommendedDscp = 46;
        p.caveats =
            "Same link as CP850 (above), receiving end — 32 analog outputs, "
            "so a full-width feed to one DAC3202 is 4 flows of 8 channels "
            "under this driver's flow splitter. Same DSCP note as CP850: "
            "documented as EF/46 but not actually applied by this driver.";
        break;
    }

    return p;
}

std::vector<CompatibilityProfile> CompatibilityProfile::all() {
    return {
        forKind(CompatibilityProfileKind::AES67),
        forKind(CompatibilityProfileKind::RAVENNA),
        forKind(CompatibilityProfileKind::ST2110_30),
        forKind(CompatibilityProfileKind::Dante),
        forKind(CompatibilityProfileKind::CP850),
        forKind(CompatibilityProfileKind::DAC3202),
    };
}

std::string CompatibilityProfile::kindToString(CompatibilityProfileKind kind) {
    switch (kind) {
    case CompatibilityProfileKind::AES67:     return "aes67";
    case CompatibilityProfileKind::RAVENNA:   return "ravenna";
    case CompatibilityProfileKind::ST2110_30: return "st2110-30";
    case CompatibilityProfileKind::Dante:     return "dante";
    case CompatibilityProfileKind::CP850:     return "cp850";
    case CompatibilityProfileKind::DAC3202:   return "dac3202";
    }
    return "aes67";
}

CompatibilityProfileKind CompatibilityProfile::kindFromString(const std::string& s) {
    if (s == "ravenna")   return CompatibilityProfileKind::RAVENNA;
    if (s == "st2110-30") return CompatibilityProfileKind::ST2110_30;
    if (s == "dante")     return CompatibilityProfileKind::Dante;
    if (s == "cp850")     return CompatibilityProfileKind::CP850;
    if (s == "dac3202")   return CompatibilityProfileKind::DAC3202;
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

bool CompatibilityProfile::validate(const SDPSession& sdp, std::string* errorOut) const {
    auto fail = [&](const std::string& reason) {
        if (errorOut) *errorOut = displayName + ": " + reason;
        return false;
    };

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

    if (!allowedPtimesMs.empty() && sdp.ptime > 0) {
        const bool ok = std::find(allowedPtimesMs.begin(), allowedPtimesMs.end(),
                                   sdp.ptime) != allowedPtimesMs.end();
        if (!ok) {
            return fail("packet time " + std::to_string(sdp.ptime) +
                        " ms not permitted");
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
