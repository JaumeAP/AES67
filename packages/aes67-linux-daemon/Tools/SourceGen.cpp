//
// SourceGen.cpp
// aes67-linux-daemon
//

#include "Tools/SourceGen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>

namespace AES67::LinuxDriver {
namespace {

/// The samples per packet the daemon documents (daemon/README.md), with the
/// packet durations they mean at 48 kHz: 250 us, 333 us, 1 ms, 2 ms, 4 ms.
constexpr std::array<int, 5> kSamplesPerPacket{12, 16, 48, 96, 192};

/// The DSCP values the daemon documents for a source: EF, AF41, AF31, BE.
constexpr std::array<int, 4> kSourceDscp{46, 34, 26, 0};

/// Sources and sinks are numbered 0-63 (daemon/README.md).
constexpr int kMaxSourceId = 63;

/// Upstream's own example values for the fields no profile speaks to.
constexpr int kTtl = 15;
constexpr int kPayloadType = 98;

std::string ratePart(double rate) {
    std::ostringstream out;
    out << static_cast<long long>(rate);
    return out.str();
}

std::string listOfRates(const std::vector<double>& rates) {
    std::ostringstream out;
    for (size_t i = 0; i < rates.size(); ++i) {
        if (i) out << ", ";
        out << ratePart(rates[i]);
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

std::string listOfEncodings(const std::vector<std::string>& encodings) {
    std::ostringstream out;
    for (size_t i = 0; i < encodings.size(); ++i) {
        if (i) out << ", ";
        out << encodings[i];
    }
    return out.str();
}

} // namespace

SourceResult generateSources(const SourceRequest& request) {
    SourceResult result;
    const CompatibilityProfile profile = CompatibilityProfile::forKind(request.kind);
    const std::string name = CompatibilityProfile::kindToString(profile.kind);

    if (request.count < 1) {
        result.error = "no sources asked for";
        return result;
    }
    if (request.channelsPerSource < 1) {
        result.error = "a source carries at least one channel";
        return result;
    }
    if (request.channelsPerSource > static_cast<int>(profile.maxChannelsPerFlow)) {
        result.error = "profile " + name + " carries at most "
                     + std::to_string(profile.maxChannelsPerFlow) + " channels in one flow";
        return result;
    }
    if (request.startId < 0 || request.startId + request.count - 1 > kMaxSourceId) {
        result.error = "sources are numbered 0-" + std::to_string(kMaxSourceId)
                     + "; ids " + std::to_string(request.startId) + " to "
                     + std::to_string(request.startId + request.count - 1) + " do not fit";
        return result;
    }

    // The rate, then the packet time, both against what the profile allows.
    double rate = 48000.0;
    const auto& rates = profile.allowedSampleRates;
    if (request.sampleRate) {
        rate = *request.sampleRate;
        if (!rates.empty() && std::find(rates.begin(), rates.end(), rate) == rates.end()) {
            result.error = "profile " + name + " does not allow " + ratePart(rate)
                         + " Hz; it allows " + listOfRates(rates);
            return result;
        }
    } else if (!rates.empty() && std::find(rates.begin(), rates.end(), rate) == rates.end()) {
        rate = *std::min_element(rates.begin(), rates.end());
    }

    uint32_t ptimeUs = 1000;
    const auto& ptimes = profile.allowedPtimesUs;
    if (request.ptimeUs) {
        ptimeUs = *request.ptimeUs;
        if (!ptimes.empty() && std::find(ptimes.begin(), ptimes.end(), ptimeUs) == ptimes.end()) {
            result.error = "profile " + name + " does not allow a packet time of "
                         + std::to_string(ptimeUs) + " us; it allows " + listOfPtimes(ptimes);
            return result;
        }
    } else if (!ptimes.empty()
               && std::find(ptimes.begin(), ptimes.end(), ptimeUs) == ptimes.end()) {
        ptimeUs = *std::min_element(ptimes.begin(), ptimes.end());
    }

    // Samples per packet is rate times packet time, and the daemon documents
    // five values for it. A packet time that falls between two of them is
    // rounded up to the next documented one -- a longer packet the daemon
    // knows how to size, rather than a number nothing upstream describes.
    const double exact = static_cast<double>(ptimeUs) * rate / 1'000'000.0;
    int samples = 0;
    for (const int candidate : kSamplesPerPacket) {
        if (static_cast<double>(candidate) >= exact) { samples = candidate; break; }
    }
    if (samples == 0) {
        result.error = "a packet time of " + std::to_string(ptimeUs) + " us at "
                     + ratePart(rate) + " Hz is " + std::to_string(static_cast<long long>(exact))
                     + " samples, past the largest the daemon documents ("
                     + std::to_string(kSamplesPerPacket.back()) + ")";
        return result;
    }
    if (static_cast<double>(samples) != exact) {
        const auto actualUs = static_cast<long long>(
            static_cast<double>(samples) * 1'000'000.0 / rate);
        result.warnings.push_back(
            "a packet time of " + std::to_string(ptimeUs) + " us at " + ratePart(rate)
            + " Hz is " + std::to_string(static_cast<long long>(exact))
            + " samples, which the daemon does not document: rounded up to "
            + std::to_string(samples) + ", a packet of " + std::to_string(actualUs) + " us");
    }

    // The codec: what was asked for if the profile allows it, otherwise the
    // first the profile names.
    std::string codec = "L24";
    const auto& encodings = profile.allowedEncodings;
    if (request.codec) {
        codec = *request.codec;
        if (!encodings.empty()
            && std::find(encodings.begin(), encodings.end(), codec) == encodings.end()) {
            result.error = "profile " + name + " does not allow " + codec
                         + "; it allows " + listOfEncodings(encodings);
            return result;
        }
    } else if (!encodings.empty()) {
        codec = encodings.front();
    }

    // The media marking. Unlike daemon.conf's ptp_dscp, this field is the one
    // the profile's recommendedDscp is about.
    int dscp = 34;  // AF41, upstream's own example value.
    if (profile.recommendedDscp >= 0) {
        if (std::find(kSourceDscp.begin(), kSourceDscp.end(), profile.recommendedDscp)
            == kSourceDscp.end()) {
            result.error = "profile " + name + " marks its media DSCP "
                         + std::to_string(profile.recommendedDscp)
                         + ", which is not one of the daemon's four (46, 34, 26, 0)";
            return result;
        }
        dscp = profile.recommendedDscp;
    }

    std::ostringstream out;
    out << "{\n  \"sources\": [\n";
    int channel = 0;
    for (int i = 0; i < request.count; ++i) {
        const int id = request.startId + i;
        out << "    {\n"
            << "      \"id\": " << id << ",\n"
            << "      \"enabled\": true,\n"
            << "      \"name\": \"AES67 Source " << id << "\",\n"
            << "      \"io\": \"Audio Device\",\n"
            << "      \"codec\": \"" << codec << "\",\n"
            << "      \"address\": \"" << profile.recommendedMulticastAddress << "\",\n"
            << "      \"max_samples_per_packet\": " << samples << ",\n"
            << "      \"ttl\": " << kTtl << ",\n"
            << "      \"payload_type\": " << kPayloadType << ",\n"
            << "      \"dscp\": " << dscp << ",\n"
            << "      \"refclk_ptp_traceable\": true,\n"
            << "      \"map\": [ ";
        for (int c = 0; c < request.channelsPerSource; ++c) {
            if (c) out << ", ";
            out << channel++;
        }
        out << " ]\n"
            << "    }" << (i + 1 < request.count ? "," : "") << "\n";
    }
    out << "  ]\n}\n";

    result.ok = true;
    result.json = out.str();
    return result;
}

} // namespace AES67::LinuxDriver
