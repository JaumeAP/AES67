#include "Ravenna/SessionCatalogue.h"

#include <algorithm>
#include <numeric>

namespace AES67::Ravenna {

std::string SessionCatalogue::pathFor(const std::string& name) {
    return "/by-name/" + name;
}

bool SessionCatalogue::add(const RavennaSession& session, std::string& error) {
    if (session.name.empty()) {
        error = "a session needs a name: it is what DNS-SD advertises and what "
                "a DESCRIBE asks for";
        return false;
    }

    if (!session.sdp.isValid()) {
        const std::vector<std::string> problems = session.sdp.getValidationErrors();
        error = "the SDP for " + session.name + " is not valid";
        error = std::accumulate(problems.begin(), problems.end(), error,
                                [](const std::string& text, const std::string& problem) {
                                    return text + "\n  - " + problem;
                                });
        return false;
    }

    // The SDP says how many channels are on the wire and the mapping says how
    // many the device puts there. A session where those disagree advertises
    // one thing and carries another, and neither side finds out on the wire.
    if (session.mapping.deviceChannelCount != session.sdp.numChannels) {
        error = "session " + session.name + " maps " +
                std::to_string(session.mapping.deviceChannelCount) +
                " device channels but its SDP announces " +
                std::to_string(session.sdp.numChannels);
        return false;
    }

    RavennaSession stored = session;
    if (stored.path.empty()) stored.path = pathFor(stored.name);
    sessions_[stored.name] = stored;
    return true;
}

bool SessionCatalogue::remove(const std::string& name) {
    return sessions_.erase(name) > 0;
}

std::optional<std::string> SessionCatalogue::describe(const std::string& path) const {
    for (const auto& [name, session] : sessions_) {
        if (session.path == path) {
            return SDPParser::generate(session.sdp);
        }
    }
    return std::nullopt;
}

std::vector<SessionAdvertisement> SessionCatalogue::advertisements(
    const std::string& hostName, uint16_t port, uint32_t addressV4) const {
    std::vector<SessionAdvertisement> advertised;
    advertised.reserve(sessions_.size());

    for (const auto& [name, session] : sessions_) {
        SessionAdvertisement entry;
        entry.instanceName = name;
        entry.hostName = hostName;
        entry.port = port;
        entry.addressV4 = addressV4;
        // What a browser can use before it opens a connection. The path is
        // the one thing it cannot guess, and the channel count is what turns
        // a list of names into something a person can route.
        entry.txtEntries.emplace_back("txtvers=1");
        entry.txtEntries.push_back("path=" + session.path);
        entry.txtEntries.push_back("channels=" + std::to_string(session.sdp.numChannels));
        advertised.push_back(entry);
    }
    return advertised;
}

std::optional<RavennaSession> SessionCatalogue::session(const std::string& name) const {
    const auto found = sessions_.find(name);
    if (found == sessions_.end()) return std::nullopt;
    return found->second;
}

std::vector<std::string> SessionCatalogue::names() const {
    std::vector<std::string> found;
    found.reserve(sessions_.size());
    for (const auto& [name, session] : sessions_) found.push_back(name);
    return found;
}

}  // namespace AES67::Ravenna
