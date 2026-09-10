//
// ProfileConf.cpp
// aes67-linux-daemon
//

#include "Tools/ProfileConf.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace AES67::LinuxDriver {
namespace {

/// The rate every frame size in the daemon's configuration is expressed
/// against: tic_frame_size_at_1fs is frames per packet at 1FS, and 1FS is
/// 48 kHz whatever rate the device runs.
constexpr double kBaseRate = 48000.0;

/// Replaces the value of `"key": <value>` in a flat JSON object, one key per
/// line, which is the shape of upstream's daemon.conf. Returns false when the
/// key is not there -- a base file that has lost a key is a base file this
/// tool must not silently pass through.
bool replaceValue(std::string& conf, const std::string& key, const std::string& value) {
    const std::string needle = "\"" + key + "\"";
    const auto keyPos = conf.find(needle);
    if (keyPos == std::string::npos) return false;

    const auto colon = conf.find(':', keyPos + needle.size());
    if (colon == std::string::npos) return false;

    auto end = conf.find_first_of(",\n}", colon + 1);
    if (end == std::string::npos) end = conf.size();

    conf.replace(colon + 1, end - (colon + 1), " " + value);
    return true;
}

std::string quoted(const std::string& s) { return "\"" + s + "\""; }

std::string rateLiteral(double rate) {
    std::ostringstream out;
    out << static_cast<long long>(rate);
    return out.str();
}

std::string listOfRates(const std::vector<double>& rates) {
    std::ostringstream out;
    for (size_t i = 0; i < rates.size(); ++i) {
        if (i) out << ", ";
        out << rateLiteral(rates[i]);
    }
    return out.str();
}

std::string listOfPtimes(const std::vector<uint32_t>& ptimes) {
    std::ostringstream out;
    for (size_t i = 0; i < ptimes.size(); ++i) {
        if (i) out << ", ";
        out << ptimes[i];
    }
    return out.str();
}

} // namespace

bool isKnownProfileName(const std::string& name) {
    for (const auto& profile : CompatibilityProfile::all()) {
        if (CompatibilityProfile::kindToString(profile.kind) == name) return true;
    }
    return false;
}

ConfResult applyProfile(const std::string& base, const ConfRequest& request) {
    ConfResult result;
    const CompatibilityProfile profile = CompatibilityProfile::forKind(request.kind);

    // The sample rate: what was asked for if the profile allows it, otherwise
    // 48 kHz when that is allowed, otherwise the lowest rate that is.
    double rate = kBaseRate;
    const auto& rates = profile.allowedSampleRates;
    if (request.sampleRate) {
        rate = *request.sampleRate;
        if (!rates.empty() && std::find(rates.begin(), rates.end(), rate) == rates.end()) {
            result.error = "profile " + CompatibilityProfile::kindToString(profile.kind)
                         + " does not allow " + rateLiteral(rate)
                         + " Hz; it allows " + listOfRates(rates);
            return result;
        }
    } else if (!rates.empty()
               && std::find(rates.begin(), rates.end(), kBaseRate) == rates.end()) {
        rate = *std::min_element(rates.begin(), rates.end());
    }

    // The packet time, carried in the configuration as frames per packet at
    // 1FS: 1 ms is 48 frames, 125 us is 6.
    uint32_t ptimeUs = 1000;
    const auto& ptimes = profile.allowedPtimesUs;
    if (request.ptimeUs) {
        ptimeUs = *request.ptimeUs;
        if (!ptimes.empty() && std::find(ptimes.begin(), ptimes.end(), ptimeUs) == ptimes.end()) {
            result.error = "profile " + CompatibilityProfile::kindToString(profile.kind)
                         + " does not allow a packet time of " + std::to_string(ptimeUs)
                         + " us; it allows " + listOfPtimes(ptimes);
            return result;
        }
    } else if (!ptimes.empty()
               && std::find(ptimes.begin(), ptimes.end(), ptimeUs) == ptimes.end()) {
        ptimeUs = *std::min_element(ptimes.begin(), ptimes.end());
    }

    const double frames = static_cast<double>(ptimeUs) * kBaseRate / 1'000'000.0;
    if (frames != std::floor(frames) || frames < 1.0) {
        result.error = "a packet time of " + std::to_string(ptimeUs)
                     + " us is not a whole number of frames at 48 kHz";
        return result;
    }

    // A profile that requires a multicast prefix but documents no address is
    // one this tool cannot satisfy: upstream's base address is outside the
    // prefix, and inventing one inside it would be picking a site's address
    // for it.
    if (!profile.requiredMulticastPrefix.empty()
        && profile.recommendedMulticastAddress.empty()) {
        result.error = "profile " + CompatibilityProfile::kindToString(profile.kind)
                     + " requires multicast inside " + profile.requiredMulticastPrefix
                     + ".0.0/16 and documents no address: set rtp_mcast_base by hand";
        return result;
    }

    std::string conf = base;
    const auto set = [&](const std::string& key, const std::string& value) {
        if (replaceValue(conf, key, value)) return true;
        result.error = "the base configuration has no \"" + key + "\" key";
        return false;
    };

    if (!set("sample_rate", rateLiteral(rate))) return result;
    if (!set("tic_frame_size_at_1fs", std::to_string(static_cast<long long>(frames))))
        return result;

    const int domain = profile.domainIsFixed ? static_cast<int>(profile.fixedDomain)
                                             : profile.recommendedPtpDomain;
    if (domain >= 0 && !set("ptp_domain", std::to_string(domain))) return result;

    if (!profile.recommendedMulticastAddress.empty()
        && !set("rtp_mcast_base", quoted(profile.recommendedMulticastAddress)))
        return result;

    if (request.interfaceName && !set("interface_name", quoted(*request.interfaceName)))
        return result;

    result.ok = true;
    result.text = std::move(conf);
    return result;
}

} // namespace AES67::LinuxDriver
