//
// SessionCatalogue.h
// AES67 RAVENNA session layer
// What this device offers, and where each session's channels land.
//
// A RAVENNA session is two things at once: an SDP describing a multicast
// stream, and a claim about which device channels that stream carries. The
// first is packages/aes67-core's SDPSession, which already generates the SDP
// RAVENNA wants, a=framecount included. The second is the core's
// StreamChannelMapper: the 128-channel routing matrix, with its overlap
// prevention and its per-channel routing, which this package uses rather than
// growing a second one that disagrees with it.
//
// So there is nothing here about audio and nothing about sockets. This is the
// index a DESCRIBE is answered from and the list mDNS advertises.
//
#pragma once

#include "Driver/SDPParser.h"
#include "NetworkEngine/StreamChannelMapper.h"
#include "Ravenna/DnsSd.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace AES67::Ravenna {

struct RavennaSession {
    /// What a person sees in a session list, and the DNS-SD instance name.
    std::string name;
    /// The path a DESCRIBE asks for: "/by-name/<name>" is what RAVENNA
    /// devices use, and it is the only form this answers.
    std::string path;
    /// The stream itself.
    SDPSession sdp;
    /// Which device channels this session carries. Its channel count and the
    /// SDP's have to agree, and validate() is what says so.
    ChannelMapping mapping;
};

class SessionCatalogue {
public:
    /// Adds a session, or replaces the one with the same name. Returns false
    /// with `error` set when the session does not hold together -- an SDP the
    /// core calls invalid, or a mapping whose channel count is not the SDP's.
    bool add(const RavennaSession& session, std::string& error);

    /// Removes it, so mDNS can say goodbye for it. False when there was none.
    bool remove(const std::string& name);

    /// The SDP for a DESCRIBE path, or nothing when no session answers to it.
    std::optional<std::string> describe(const std::string& path) const;

    /// The path a session is described at, built the one way this answers.
    static std::string pathFor(const std::string& name);

    /// What to advertise, one entry per session.
    std::vector<SessionAdvertisement> advertisements(const std::string& hostName,
                                                     uint16_t port,
                                                     uint32_t addressV4) const;

    std::vector<std::string> names() const;
    size_t size() const { return sessions_.size(); }

private:
    /// By name, because that is what has to be unique: two sessions with one
    /// name are one instance in DNS-SD and a coin toss in a DESCRIBE.
    std::map<std::string, RavennaSession> sessions_;
};

}  // namespace AES67::Ravenna
