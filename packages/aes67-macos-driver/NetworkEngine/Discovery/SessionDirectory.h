#ifndef SESSION_DIRECTORY_H
#define SESSION_DIRECTORY_H

//
// SessionDirectory
// AES67 macOS Driver
//
// One list of the sessions on the network, whichever way they were found.
//
// AES67 gear announces itself in ways that do not see each other: SAP shouts
// an SDP on 224.2.127.254 (SAPListener), RAVENNA registers `_rtsp._tcp` and
// serves its SDP to whoever asks with DESCRIBE (MDNSBrowser plus SDPFetcher),
// and NMOS publishes senders through IS-04. A device that does one of those
// and not the others is ordinary, not exotic -- and until this existed, only
// the SAP list reached the Manager app, so a Merging unit that registers a
// service and never announces was invisible in the interface of a driver that
// could already find it.
//
// What this class is: the place all of them arrive at, keyed by the session's
// own identity rather than by how it was heard, so the same session found
// twice is one entry that knows it was found twice. What it is not: a
// discoverer. It opens no socket and starts no thread; the discoverers call
// offer() and it decides what is new, what is a refresh and what has gone
// quiet.
//

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace AES67 {

/// How a session reached us. A session can arrive by more than one route,
/// which is why an entry carries a set of these rather than one value.
enum class DiscoverySource {
    SAP,    ///< announced on the SAP group
    RTSP,   ///< registered as _rtsp._tcp and described over RTSP
    NMOS,   ///< published as an IS-04 sender
};

const char* discoverySourceName(DiscoverySource source);

/// One session, as the Manager app needs it: enough to show, and the SDP so
/// that subscribing to it needs nothing re-derived.
struct DiscoveredSessionEntry {
    std::string identity;            ///< dedupe key, see SessionDirectory::identityOf
    std::string sessionName;
    std::string sourceAddress;       ///< who told us (announcer or RTSP host)
    std::string multicastAddress;    ///< where the audio is
    int port{0};
    int ptpDomain{0};
    std::string sessionDescription;  ///< the full SDP

    /// Every route this session has been heard by, in the order first heard.
    std::vector<DiscoverySource> sources;

    std::chrono::steady_clock::time_point lastSeen{};

    bool heardBy(DiscoverySource source) const;
};

class SessionDirectory {
public:
    /// How long a session may go unrefreshed before it is dropped.
    ///
    /// The same 300 s SAPListener uses, and for the same reason: announcers
    /// repeat on their own schedule, ten missed announcements is the rule the
    /// AES67 Linux daemon applies, and a listener cannot know the interval.
    /// An RTSP session is re-checked by its browser rather than announced, so
    /// it ages out the same way when the browser stops seeing the service.
    static constexpr std::chrono::seconds kSessionTimeout{300};

    /// The identity of a session, which is what makes two sightings one
    /// entry.
    ///
    /// RFC 4566 SS 5.2: `o=<user> <sess-id> <sess-version> <nettype>
    /// <addrtype> <unicast-address>` names the session globally, and the
    /// version changes when the description does. So the key is the user,
    /// the session id and the origin address -- not the version, or an
    /// updated description would arrive as a second session. When there is no
    /// usable `o=` line, the destination the audio is on is the next best
    /// thing: two announcements for one address and port are one flow.
    static std::string identityOf(const std::string& sdp,
                                  const std::string& multicastAddress, int port);

    /// Records a sighting: a new session, or a refresh of one already here.
    ///
    /// A refresh keeps the identity and adds the source if it is new, takes
    /// the newer description (an announcer that changes its SDP is the case
    /// a=framecount arrived by), and moves lastSeen forward. `now` is a
    /// parameter so the ageing can be tested without waiting five minutes.
    void offer(const DiscoveredSessionEntry& entry,
               std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    /// What is on the network, sweeping what has gone quiet as it reads --
    /// the same shape as SAPListener::getDiscoveredStreams().
    std::vector<DiscoveredSessionEntry> sessions(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;

    /// Forgets a session before its timeout, for a discoverer that knows it
    /// is gone: a SAP deletion, or a service withdrawn from mDNS. Returns
    /// whether anything was removed.
    bool forget(const std::string& identity);

    size_t size() const;

private:
    mutable std::mutex mutex_;
    mutable std::vector<DiscoveredSessionEntry> entries_;
};

} // namespace AES67

#endif // SESSION_DIRECTORY_H
