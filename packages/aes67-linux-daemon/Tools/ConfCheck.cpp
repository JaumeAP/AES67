//
// ConfCheck.cpp
// aes67-linux-daemon
//

#include "Tools/ConfCheck.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <sstream>

namespace AES67::LinuxDriver {
namespace {

constexpr double kBaseRate = 48000.0;

/// The keys daemon/json.cpp reads. Anything else in the file is assigned to
/// nothing and reported by nobody, which is what makes a misspelling silent.
constexpr std::array<const char*, 37> kKnownKeys{
    "auto_sinks_update", "custom_node_id", "http_base_dir", "http_port",
    "interface_name", "ip_addr", "log_severity", "mac_addr",
    "max_tic_frame_size", "mdns_enabled", "nmos_enabled", "nmos_label",
    "nmos_node_port", "nmos_registry_address", "nmos_registry_port", "node_id",
    "playout_delay", "ptp_domain", "ptp_dscp", "ptp_status_script",
    "rtp_mcast_base", "rtp_mcast_base_sec", "rtp_port", "rtp_port_sec",
    "rtsp_port", "sample_rate", "sap_interval", "sap_mcast_addr",
    "status_file", "streamer_channels", "streamer_enabled",
    "streamer_file_duration", "streamer_files_num",
    "streamer_player_buffer_files_num", "syslog_proto", "syslog_server",
    "tic_frame_size_at_1fs"};

/// The rates the project's own test scripts drive the driver at
/// (README.md:368). Not a limit the daemon enforces -- a warning's worth.
constexpr std::array<long long, 8> kTestedRates{44100, 48000, 88200, 96000,
                                                176400, 192000, 352800, 384000};

/// What the daemon reads each numeric key as (daemon/config.hpp). A value that
/// does not fit is truncated by Boost's conversion, silently.
const std::map<std::string, long long> kNumericCeiling{
    {"http_port", 65535},          {"rtsp_port", 65535},
    {"rtp_port", 65535},           {"rtp_port_sec", 65535},
    {"sap_interval", 65535},       {"nmos_node_port", 65535},
    {"nmos_registry_port", 65535}, {"streamer_file_duration", 65535},
    {"ptp_domain", 255},           {"ptp_dscp", 255},
    {"streamer_channels", 255},    {"streamer_files_num", 255},
    {"streamer_player_buffer_files_num", 255},
    {"playout_delay", 4294967295}, {"tic_frame_size_at_1fs", 4294967295},
    {"max_tic_frame_size", 4294967295}, {"sample_rate", 4294967295}};

struct Entry {
    std::string key;
    std::string value;  ///< As written, quotes stripped for strings.
    bool quoted{false};
};

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r\n,");
    return s.substr(first, last - first + 1);
}

/// Reads the flat object upstream writes: one "key": value per line. Not a
/// JSON parser -- a nested object would defeat it, and daemon.conf has none.
std::vector<Entry> read(const std::string& conf) {
    std::vector<Entry> entries;
    std::istringstream in(conf);
    std::string line;
    while (std::getline(in, line)) {
        const auto keyStart = line.find('"');
        if (keyStart == std::string::npos) continue;
        const auto keyEnd = line.find('"', keyStart + 1);
        if (keyEnd == std::string::npos) continue;
        const auto colon = line.find(':', keyEnd + 1);
        if (colon == std::string::npos) continue;

        Entry entry;
        entry.key = line.substr(keyStart + 1, keyEnd - keyStart - 1);
        std::string value = trim(line.substr(colon + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            entry.quoted = true;
            value = value.substr(1, value.size() - 2);
        }
        entry.value = value;
        entries.push_back(entry);
    }
    return entries;
}

bool isInteger(const std::string& s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

std::optional<long long> integerOf(const std::vector<Entry>& entries, const std::string& key) {
    for (const auto& entry : entries) {
        if (entry.key == key && isInteger(entry.value)) return std::stoll(entry.value);
    }
    return std::nullopt;
}

std::optional<std::string> textOf(const std::vector<Entry>& entries, const std::string& key) {
    for (const auto& entry : entries) {
        if (entry.key == key) return entry.value;
    }
    return std::nullopt;
}

/// The first octet of a dotted address, or nothing when it is not one.
std::optional<int> firstOctet(const std::string& address) {
    const auto dot = address.find('.');
    if (dot == std::string::npos || dot == 0) return std::nullopt;
    const std::string head = address.substr(0, dot);
    if (!isInteger(head)) return std::nullopt;
    return static_cast<int>(std::stoll(head));
}

bool isMulticast(const std::string& address) {
    const auto octet = firstOctet(address);
    return octet && *octet >= 224 && *octet <= 239;  // 224.0.0.0/4, RFC 5771.
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

void add(std::vector<Finding>& out, Severity severity, std::string key, std::string message) {
    out.push_back({severity, std::move(key), std::move(message)});
}

void checkProfile(std::vector<Finding>& out,
                  const std::vector<Entry>& entries,
                  const CompatibilityProfile& profile) {
    const std::string name = CompatibilityProfile::kindToString(profile.kind);

    if (const auto rate = integerOf(entries, "sample_rate")) {
        const auto& rates = profile.allowedSampleRates;
        if (!rates.empty()
            && std::find(rates.begin(), rates.end(), static_cast<double>(*rate)) == rates.end()) {
            add(out, Severity::Error, "sample_rate",
                std::to_string(*rate) + " Hz is not allowed by profile " + name);
        }
    }

    if (const auto tic = integerOf(entries, "tic_frame_size_at_1fs")) {
        const auto ptimeUs = static_cast<uint32_t>(
            static_cast<double>(*tic) * 1'000'000.0 / kBaseRate);
        const auto& ptimes = profile.allowedPtimesUs;
        if (!ptimes.empty()
            && std::find(ptimes.begin(), ptimes.end(), ptimeUs) == ptimes.end()) {
            add(out, Severity::Error, "tic_frame_size_at_1fs",
                std::to_string(*tic) + " frames is a packet time of "
                    + std::to_string(ptimeUs) + " us, which profile " + name
                    + " does not allow");
        }
    }

    if (const auto domain = integerOf(entries, "ptp_domain")) {
        if (profile.domainIsFixed && *domain != static_cast<long long>(profile.fixedDomain)) {
            add(out, Severity::Error, "ptp_domain",
                "profile " + name + " fixes the PTP domain at "
                    + std::to_string(profile.fixedDomain));
        } else if (!profile.domainIsFixed && profile.recommendedPtpDomain >= 0
                   && *domain != profile.recommendedPtpDomain) {
            add(out, Severity::Warning, "ptp_domain",
                "profile " + name + " documents domain "
                    + std::to_string(profile.recommendedPtpDomain) + " as the factory default");
        }
    }

    for (const char* key : {"rtp_mcast_base", "rtp_mcast_base_sec"}) {
        const auto address = textOf(entries, key);
        if (!address) continue;
        if (!profile.requiredMulticastPrefix.empty()
            && !startsWith(*address, profile.requiredMulticastPrefix + ".")) {
            add(out, Severity::Error, key,
                *address + " is outside " + profile.requiredMulticastPrefix
                    + ".0.0/16, which profile " + name + " requires");
        } else if (!profile.recommendedMulticastAddress.empty()
                   && *address != profile.recommendedMulticastAddress) {
            add(out, Severity::Warning, key,
                "profile " + name + " documents " + profile.recommendedMulticastAddress
                    + " as the factory default");
        }
    }
}

void checkFile(std::vector<Finding>& out,
               const std::vector<Entry>& entries,
               const PathProbe& exists) {
    for (const auto& entry : entries) {
        // A key the daemon does not read is assigned to nothing and reported
        // by nobody: json.cpp walks the file key by key and ignores the rest.
        if (std::find_if(kKnownKeys.begin(), kKnownKeys.end(), [&](const char* known) {
                return entry.key == known;
            }) == kKnownKeys.end()) {
            add(out, Severity::Warning, entry.key, "the daemon does not read this key");
            continue;
        }

        // Boost's conversion truncates a value that does not fit the setter's
        // type (daemon/config.hpp), and nothing says so at run time.
        const auto ceiling = kNumericCeiling.find(entry.key);
        if (ceiling != kNumericCeiling.end() && isInteger(entry.value)) {
            const long long value = std::stoll(entry.value);
            if (value > ceiling->second) {
                add(out, Severity::Error, entry.key,
                    entry.value + " does not fit the type the daemon reads it as (max "
                        + std::to_string(ceiling->second) + ")");
            }
        }
    }

    for (const char* key : {"rtp_mcast_base", "rtp_mcast_base_sec", "sap_mcast_addr"}) {
        const auto address = textOf(entries, key);
        if (address && !isMulticast(*address)) {
            add(out, Severity::Error, key, *address + " is not a multicast address (224.0.0.0/4)");
        }
    }

    // RFC 2974 gives SAP one address per scope: 224.2.127.254 globally, and
    // the highest address of an administratively scoped range.
    if (const auto sap = textOf(entries, "sap_mcast_addr")) {
        if (isMulticast(*sap) && *sap != "224.2.127.254" && *sap != "239.255.255.255") {
            add(out, Severity::Warning, "sap_mcast_addr",
                *sap + " is neither of SAP's own addresses (RFC 2974)");
        }
    }

    // RFC 3550 section 11: RTP on an even port, RTCP on the odd one above it.
    if (const auto port = integerOf(entries, "rtp_port")) {
        if (*port % 2 != 0) {
            add(out, Severity::Error, "rtp_port",
                std::to_string(*port) + " is odd; RTP takes the even port of the pair");
        }
    }

    if (const auto domain = integerOf(entries, "ptp_domain")) {
        if (*domain > 127) {
            add(out, Severity::Error, "ptp_domain",
                std::to_string(*domain) + " is outside 0-127 (IEEE 1588-2008)");
        }
    }

    if (const auto dscp = integerOf(entries, "ptp_dscp")) {
        if (*dscp > 63) {
            add(out, Severity::Error, "ptp_dscp",
                std::to_string(*dscp) + " does not fit the six bits of the DS field");
        }
    }

    const auto tic = integerOf(entries, "tic_frame_size_at_1fs");
    const auto maxTic = integerOf(entries, "max_tic_frame_size");
    if (tic && *tic == 0) {
        add(out, Severity::Error, "tic_frame_size_at_1fs", "zero frames per packet");
    }
    if (tic && maxTic && *tic > *maxTic) {
        add(out, Severity::Error, "tic_frame_size_at_1fs",
            std::to_string(*tic) + " frames is more than max_tic_frame_size ("
                + std::to_string(*maxTic) + ")");
    }

    // Four servers, four ports, one host.
    const std::array<const char*, 4> portKeys{"http_port", "rtsp_port", "nmos_node_port",
                                              "nmos_registry_port"};
    for (size_t i = 0; i < portKeys.size(); ++i) {
        const auto first = integerOf(entries, portKeys[i]);
        if (!first) continue;
        for (size_t j = i + 1; j < portKeys.size(); ++j) {
            const auto second = integerOf(entries, portKeys[j]);
            if (second && *first == *second) {
                add(out, Severity::Error, portKeys[j],
                    std::string(portKeys[j]) + " and " + portKeys[i] + " are both "
                        + std::to_string(*first));
            }
        }
    }

    if (const auto rate = integerOf(entries, "sample_rate")) {
        if (std::find(kTestedRates.begin(), kTestedRates.end(), *rate) == kTestedRates.end()) {
            add(out, Severity::Warning, "sample_rate",
                std::to_string(*rate) + " is not one of the rates the project tests");
        }
    }

    // log.cpp:50 and 54 test for "none" and "udp"; everything else falls into
    // the TCP branch without a word.
    if (const auto proto = textOf(entries, "syslog_proto")) {
        if (*proto != "none" && *proto != "udp" && *proto != "tcp") {
            add(out, Severity::Warning, "syslog_proto",
                *proto + " is neither none nor udp, so the daemon logs over TCP");
        }
    }

    if (const auto severity = integerOf(entries, "log_severity")) {
        if (*severity > 5) {
            add(out, Severity::Warning, "log_severity",
                std::to_string(*severity) + " is above fatal, the highest Boost.Log level");
        }
    }

    for (const char* key : {"http_base_dir", "status_file", "ptp_status_script"}) {
        const auto path = textOf(entries, key);
        if (path && !path->empty() && !exists(*path)) {
            add(out, Severity::Warning, key, *path + " does not exist");
        }
    }
}

} // namespace

std::vector<Finding> checkConf(const std::string& conf,
                               std::optional<CompatibilityProfileKind> kind,
                               const PathProbe& exists) {
    const auto entries = read(conf);
    std::vector<Finding> findings;
    if (entries.empty()) {
        add(findings, Severity::Error, "", "no configuration keys were read");
        return findings;
    }
    if (kind) checkProfile(findings, entries, CompatibilityProfile::forKind(*kind));
    checkFile(findings, entries, exists);
    return findings;
}

bool hasError(const std::vector<Finding>& findings) {
    return std::any_of(findings.begin(), findings.end(),
                       [](const Finding& f) { return f.severity == Severity::Error; });
}

} // namespace AES67::LinuxDriver
