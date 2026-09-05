#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AES67 {

/// One stream this server offers, as an RTSP path and the SDP that
/// describes it.
struct RTSPPublishedStream {
    std::string path;  ///< Request path, e.g. "/by-name/Studio Mic 1".
    std::string sdp;   ///< The full session description served for it.
};

/// Supplies what to serve, called per request rather than cached: a
/// stream added or removed since the last DESCRIBE has to be reflected
/// without anyone remembering to refresh the server.
using RTSPStreamProvider = std::function<std::vector<RTSPPublishedStream>()>;

/// Serves this driver's own transmit streams over RTSP, so other gear can
/// fetch their SDP with DESCRIBE (2026-08-31).
///
/// The complement of RTSPClient, and the half that was missing: we could
/// read someone else's session description, but nobody could read ours
/// except by catching a SAP announcement in flight. Merging's RAVENNA
/// driver both serves and resolves RTSP (its bundle carries
/// httprtsp/server.cpp and a `_rtsp._tcp` resolver), and that is the
/// convention a professional receiver expects — SAP is a broadcast you
/// have to be listening for, DESCRIBE is a question anyone can ask at
/// any time.
///
/// Deliberately DESCRIBE-only (plus OPTIONS, which every client sends
/// first). SETUP/PLAY/TEARDOWN are the unicast-negotiation half of RTSP;
/// AES67 streams are multicast and already running, so a receiver needs
/// the description and nothing else. Answering 501 for the rest is
/// honest: a client learns immediately that this endpoint describes,
/// it does not negotiate.
class RTSPServer {
public:
    /// IANA's RTSP port. Overridable, and the tests use a high port —
    /// binding 554 needs root, which a user-space driver does not have.
    static constexpr uint16_t kDefaultPort = 554;

    /// Longest request this server will read before giving up on a
    /// client. RTSP requests are headers only here; anything larger is a
    /// malformed or hostile peer, not a DESCRIBE.
    static constexpr size_t kMaxRequestBytes = 8192;

    /// How many clients may be queued by the kernel. Small on purpose:
    /// this serves occasional description requests, not a media session.
    static constexpr int kListenBacklog = 8;

    explicit RTSPServer(uint16_t port = kDefaultPort);
    ~RTSPServer();

    RTSPServer(const RTSPServer&) = delete;
    RTSPServer& operator=(const RTSPServer&) = delete;

    /// Starts listening. Returns false if the port cannot be bound —
    /// which, like every other discovery surface here, costs the feature
    /// and never the audio.
    bool start(RTSPStreamProvider provider);
    void stop();
    bool isRunning() const;

    /// The port actually bound. Differs from the requested one only when
    /// 0 was requested (ephemeral), which is how the tests avoid
    /// colliding with anything already listening.
    uint16_t boundPort() const;

    /// Requests served since start(), for diagnostics.
    int requestCount() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace AES67

#endif // RTSP_SERVER_H
