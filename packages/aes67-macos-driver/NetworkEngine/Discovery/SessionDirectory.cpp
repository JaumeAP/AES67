//
// SessionDirectory.cpp
// AES67 macOS Driver
//

#include "NetworkEngine/Discovery/SessionDirectory.h"

#include <algorithm>
#include <sstream>

namespace AES67 {

const char* discoverySourceName(DiscoverySource source) {
    switch (source) {
        case DiscoverySource::SAP:  return "sap";
        case DiscoverySource::RTSP: return "rtsp";
        case DiscoverySource::NMOS: return "nmos";
    }
    return "unknown";
}

bool DiscoveredSessionEntry::heardBy(DiscoverySource source) const {
    return std::find(sources.begin(), sources.end(), source) != sources.end();
}

namespace {

/// The `o=` line's fields, or an empty vector when there is no such line.
/// Split on whitespace: RFC 4566 writes the six fields separated by single
/// spaces, but a generator that used more than one should still be read.
std::vector<std::string> originFields(const std::string& sdp) {
    std::istringstream lines(sdp);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("o=", 0) != 0) continue;

        std::istringstream fields(line.substr(2));
        std::vector<std::string> parts;
        std::string field;
        while (fields >> field) parts.push_back(field);
        return parts;
    }
    return {};
}

} // namespace

std::string SessionDirectory::identityOf(const std::string& sdp,
                                         const std::string& multicastAddress, int port) {
    const std::vector<std::string> origin = originFields(sdp);
    // <user> <sess-id> <sess-version> <nettype> <addrtype> <unicast-address>:
    // everything but the version, which changes with the description.
    if (origin.size() >= 6) {
        return "o:" + origin[0] + " " + origin[1] + " " + origin[5];
    }
    return "c:" + multicastAddress + ":" + std::to_string(port);
}

void SessionDirectory::offer(const DiscoveredSessionEntry& entry,
                             std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);

    const std::string identity = entry.identity.empty()
        ? identityOf(entry.sessionDescription, entry.multicastAddress, entry.port)
        : entry.identity;

    for (DiscoveredSessionEntry& existing : entries_) {
        if (existing.identity != identity) continue;

        existing.lastSeen = now;
        for (DiscoverySource source : entry.sources) {
            if (!existing.heardBy(source)) existing.sources.push_back(source);
        }
        // The newer description wins: a session that changes what it says --
        // a rate, a channel count, an added a=framecount -- is the same
        // session saying something new, and the stale copy is the one that
        // would be subscribed to.
        if (!entry.sessionDescription.empty()) {
            existing.sessionDescription = entry.sessionDescription;
        }
        if (!entry.sessionName.empty())      existing.sessionName = entry.sessionName;
        if (!entry.multicastAddress.empty()) existing.multicastAddress = entry.multicastAddress;
        if (entry.port != 0)                 existing.port = entry.port;
        if (!entry.sourceAddress.empty())    existing.sourceAddress = entry.sourceAddress;
        if (entry.ptpDomain != 0)            existing.ptpDomain = entry.ptpDomain;
        return;
    }

    DiscoveredSessionEntry fresh = entry;
    fresh.identity = identity;
    fresh.lastSeen = now;
    entries_.push_back(std::move(fresh));
}

std::vector<DiscoveredSessionEntry> SessionDirectory::sessions(
    std::chrono::steady_clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);

    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [now](const DiscoveredSessionEntry& entry) {
                                      return now - entry.lastSeen > kSessionTimeout;
                                  }),
                   entries_.end());
    return entries_;
}

bool SessionDirectory::forget(const std::string& identity) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&identity](const DiscoveredSessionEntry& entry) {
                                      return entry.identity == identity;
                                  }),
                   entries_.end());
    return entries_.size() != before;
}

size_t SessionDirectory::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

} // namespace AES67
