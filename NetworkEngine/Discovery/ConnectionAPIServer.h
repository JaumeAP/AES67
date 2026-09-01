//
// ConnectionAPIServer.h
// AES67 macOS Driver
//
// The IS-05 Connection API: how a controller patches this driver's
// receivers onto senders.
//
// IS-04 made the streams visible; a controller could see them and do
// nothing with them. This is the half that connects, and it is why the
// device resource can finally advertise a control instead of an empty
// list.
//
// SENDERS ARE READ ONLY HERE. Their transport file — the SDP a receiver
// needs — is served, and their staged and active endpoints answer, but a
// PATCH to a sender is refused with 501. This driver's senders are
// configured through its own settings and the app; pretending a
// controller can re-address them would be a control that answers and does
// not act.
//
// Everything served here is reachable by anyone on the segment and runs
// inside coreaudiod: the request size is bounded, no parse throws, and a
// request this does not understand is answered rather than dropped.
//
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace AES67 {

/// A sender as the Connection API sees it: an id, what a controller shows,
/// where it goes, and the description a receiver would need.
struct ConnectionSender {
    std::string id;
    std::string label;
    std::string multicastAddress;
    uint16_t port{0};
    std::string sourceAddress;
    /// The SDP, served verbatim at .../transportfile. Empty means this
    /// sender cannot describe itself, and the endpoint answers 404 rather
    /// than an empty file.
    std::string sdp;
    bool enabled{true};
};

/// A receiver: what it is, and what it is currently taking.
struct ConnectionReceiver {
    std::string id;
    std::string label;
    std::string multicastAddress;
    uint16_t port{0};
    std::string senderId;
    bool enabled{true};
};

/// What a PATCH asked for. Every field is optional because IS-05 patches
/// are partial: a controller changing only master_enable sends only that.
struct ConnectionPatch {
    std::optional<bool> masterEnable;
    std::optional<std::string> senderId;
    std::optional<std::string> multicastAddress;
    std::optional<uint16_t> port;
    std::optional<std::string> interfaceAddress;
    /// The sender's SDP, when the controller sent one. This is the whole
    /// point of `transport_file`: it carries the format, not just the
    /// address, so a receiver can be patched onto a stream it has never
    /// heard announced.
    std::optional<std::string> transportFile;
    /// True when the patch asked for immediate activation. A staged
    /// change with no activation is stored and not applied, which is what
    /// "staged" means.
    bool activateImmediate{false};
};

using ConnectionSenderLister = std::function<std::vector<ConnectionSender>()>;
using ConnectionReceiverLister = std::function<std::vector<ConnectionReceiver>()>;
/// Applies a patch to one receiver. False means the driver refused it,
/// which the controller sees as a 500 rather than a silent success.
using ConnectionReceiverPatcher =
    std::function<bool(const std::string& receiverId, const ConnectionPatch&)>;

class ConnectionAPIServer {
public:
    /// The version this serves. IS-05 v1.1 is what controllers have
    /// spoken since 2019.
    static constexpr const char* kApiVersion = "v1.1";

    /// Zero asks the kernel for a free port, which is what the driver
    /// wants: it publishes whatever it got in the device's control href.
    explicit ConnectionAPIServer(uint16_t port = 0);
    ~ConnectionAPIServer();

    ConnectionAPIServer(const ConnectionAPIServer&) = delete;
    ConnectionAPIServer& operator=(const ConnectionAPIServer&) = delete;

    bool start(ConnectionSenderLister senders,
               ConnectionReceiverLister receivers,
               ConnectionReceiverPatcher patcher);
    void stop();
    bool isRunning() const;

    /// The port actually bound, once started.
    uint16_t boundPort() const;

    /// The href that goes in the IS-04 device's `controls`.
    std::string controlHref(const std::string& host) const;

    /// The pure half: what a request means and what comes back. Exposed so
    /// the routing can be read without a socket.
    struct Reply {
        int status{200};
        std::string contentType{"application/json"};
        std::string body;
    };
    Reply route(const std::string& method, const std::string& path, const std::string& body) const;

    /// Reads a PATCH body. Public because what a controller may send is
    /// worth pinning down on its own.
    static ConnectionPatch parsePatch(const std::string& json);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace AES67
