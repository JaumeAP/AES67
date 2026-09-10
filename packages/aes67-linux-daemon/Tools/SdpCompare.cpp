//
// SdpCompare.cpp
// aes67-linux-daemon
//

#include "Tools/SdpCompare.h"

#include <algorithm>
#include <map>
#include <sstream>

namespace AES67::LinuxDriver {
namespace {

/// Every line of an SDP, keyed by what identifies it: the letter for a
/// session line, the attribute name for an "a=" line. Later lines of the same
/// key are kept, joined, so a repeated attribute is not lost.
std::map<std::string, std::string> linesOf(const std::string& sdp) {
    std::map<std::string, std::string> lines;
    std::istringstream in(sdp);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 2 || line[1] != '=') continue;
        std::string key(1, line[0]);
        std::string value = line.substr(2);
        if (key == "a") {
            const auto colon = value.find(':');
            key = "a=" + (colon == std::string::npos ? value : value.substr(0, colon));
            value = colon == std::string::npos ? "" : value.substr(colon + 1);
        }
        auto it = lines.find(key);
        if (it == lines.end()) {
            lines.emplace(key, value);
        } else {
            it->second += " | " + value;
        }
    }
    return lines;
}

/// What a receiver acts on: the group, the transport, the format and the
/// clock. A difference anywhere else is a difference in wording.
bool isBreaking(const std::string& field) {
    static const std::vector<std::string> breaking{
        "c", "m", "a=rtpmap", "a=ptime", "a=framecount", "a=ts-refclk",
        "a=mediaclk", "a=clock-domain"};
    return std::find(breaking.begin(), breaking.end(), field) != breaking.end();
}

/// a=ptime is a recommendation, not a parameter a receiver must honour
/// (RFC 4566 section 6), and a=framecount states the same packet exactly, in
/// samples. So two ptimes that disagree while the framecounts agree describe
/// one packet written to different precisions: the daemon prints twelve
/// decimals of a millisecond, this project prints three from an integer count
/// of microseconds. Both say 48 samples.
bool ptimeIsWording(const std::map<std::string, std::string>& daemon,
                    const std::map<std::string, std::string>& ours) {
    const auto theirs = daemon.find("a=framecount");
    const auto mine = ours.find("a=framecount");
    return theirs != daemon.end() && mine != ours.end() && theirs->second == mine->second;
}

std::string noteFor(const std::string& field) {
    if (field == "o") return "the origin line: Dante keys a flow on it, receivers do not";
    if (field == "s") return "the session name, shown to a person";
    if (field == "a=source-filter") return "a receiver may use it to filter, or ignore it";
    if (field == "a=sync-time") return "RAVENNA's, and nobody reads it back";
    if (field == "a=recvonly" || field == "a=sendonly") return "the direction of the session";
    if (isBreaking(field)) return "a receiver acts on this";
    return "";
}

} // namespace

std::string daemonSdpFor(const DaemonSdpParams& params) { return daemonSdp(params); }

std::string ourSdpFor(const DaemonSdpParams& params) {
    SDPSession session;
    session.sessionName = params.nodeId + " " + params.sourceName;
    session.sessionID = params.sessionId;
    session.sessionVersion = params.sessionVersion;
    session.originAddress = params.sourceIp;
    session.connectionAddress = params.destinationIp;
    session.ttl = params.ttl;
    session.port = params.destinationPort;
    session.payloadType = params.payloadType;
    session.encoding = params.codec;
    session.sampleRate = params.sampleRate;
    session.numChannels = params.channels;
    session.framecount = params.maxSamplesPerPacket;
    session.ptimeUs = static_cast<uint32_t>(
        static_cast<double>(params.maxSamplesPerPacket) * 1'000'000.0
        / static_cast<double>(params.sampleRate));
    session.sourceAddress = params.sourceIp;
    session.ptpDomain = params.ptpDomain;
    session.ptpTraceable = params.refclkPtpTraceable;
    if (!params.refclkPtpTraceable) session.ptpMasterMAC = params.gmid;
    session.direction = "recvonly";
    return SDPParser::generate(session);
}

std::vector<SdpDifference> compareSdp(const DaemonSdpParams& params) {
    const auto daemon = linesOf(daemonSdpFor(params));
    const auto ours = linesOf(ourSdpFor(params));

    std::vector<std::string> fields;
    for (const auto& [key, value] : daemon) fields.push_back(key);
    for (const auto& [key, value] : ours) {
        if (daemon.find(key) == daemon.end()) fields.push_back(key);
    }
    std::sort(fields.begin(), fields.end());

    std::vector<SdpDifference> differences;
    for (const auto& field : fields) {
        const auto inDaemon = daemon.find(field);
        const auto inOurs = ours.find(field);
        const std::string left = inDaemon == daemon.end() ? "" : inDaemon->second;
        const std::string right = inOurs == ours.end() ? "" : inOurs->second;
        if (left == right) continue;
        bool breaking = isBreaking(field);
        std::string note = noteFor(field);
        if (field == "a=ptime" && ptimeIsWording(daemon, ours)) {
            breaking = false;
            note = "a recommendation (RFC 4566 section 6), and both carry the same framecount";
        }
        differences.push_back({field, left, right, breaking, note});
    }
    return differences;
}

bool hasBreaking(const std::vector<SdpDifference>& differences) {
    return std::any_of(differences.begin(), differences.end(),
                       [](const SdpDifference& d) { return d.breaking; });
}

} // namespace AES67::LinuxDriver
