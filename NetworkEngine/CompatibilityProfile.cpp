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
        p.caveats =
            "Baseline. Accepts the three sample rates AES67 names; the device "
            "itself declares more (up to 384 kHz), which other AES67 gear may "
            "refuse.";
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
    }

    return p;
}

std::vector<CompatibilityProfile> CompatibilityProfile::all() {
    return {
        forKind(CompatibilityProfileKind::AES67),
        forKind(CompatibilityProfileKind::RAVENNA),
        forKind(CompatibilityProfileKind::ST2110_30),
    };
}

std::string CompatibilityProfile::kindToString(CompatibilityProfileKind kind) {
    switch (kind) {
    case CompatibilityProfileKind::AES67:     return "aes67";
    case CompatibilityProfileKind::RAVENNA:   return "ravenna";
    case CompatibilityProfileKind::ST2110_30: return "st2110-30";
    }
    return "aes67";
}

CompatibilityProfileKind CompatibilityProfile::kindFromString(const std::string& s) {
    if (s == "ravenna")   return CompatibilityProfileKind::RAVENNA;
    if (s == "st2110-30") return CompatibilityProfileKind::ST2110_30;
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
