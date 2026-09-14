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
// A sender's transport file — the SDP a receiver needs — is served, and its
// staged and active endpoints answer. A PATCH to a sender is applied when the
// driver supplied a sender patcher, and refused with 501 when it did not.
//
// What a request means is not decided here. This owns the socket, the driver's
// senders and receivers, and the patches that reach the driver; the
// specification itself is `aes67-ravenna`'s ConnectionApi, which this hands
// every request to. Both halves of the tree used to answer IS-05 on their own
// and had drifted apart -- against the AMWA IS-05-01 suite, 57 of 61 there and
// 15 here, over nine causes, every one of them already fixed there.
//
// Re-addressing a sender is how a stream reaches gear that cannot be
// configured over the network at all: Dolby Atmos Connect has no control
// protocol, and its receiver's address, destination port and per-flow source
// ports are fixed by its manual. Nothing can be patched onto that device, so
// the only end a controller can configure is this one -- the source.
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
    /// The transport file of the stream this receiver is taking, empty when
    /// it is taking none. IS-05 reports it on the receiver's own endpoints,
    /// and it is the only thing that says what the stream is rather than
    /// merely where it is: a receiver that answers with an address and no
    /// file is one a controller cannot copy onto another device.
    std::string sdp;
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

/// What IS-05's own connection state says about one resource, read back
/// from the real Ravenna::ConnectionApi this server drives -- not guessed
/// at from whether a stream object happens to exist. A stream that exists
/// is not necessarily the one a controller last enabled: master_enable is
/// PATCHed independently of the resource itself, and a receiver/sender
/// pair name each other by id, neither of which either the direct Node API
/// or the NMOS registration client used to have any way to ask this about.
struct ConnectionActiveState {
    bool exists{false};       ///< false when this server knows no such id
    bool masterEnable{false}; ///< IS-05's own word for "is this switched on"
    /// A sender's connected receiver_id, or a receiver's connected
    /// sender_id -- empty when nothing is on the other end.
    std::string peerId;
};

using ConnectionSenderLister = std::function<std::vector<ConnectionSender>()>;
using ConnectionReceiverLister = std::function<std::vector<ConnectionReceiver>()>;
/// Applies a patch to one receiver. False means the driver refused it,
/// which the controller sees as a 500 rather than a silent success.
using ConnectionReceiverPatcher =
    std::function<bool(const std::string& receiverId, const ConnectionPatch&)>;
/// Applies a patch to one sender: where it transmits. Absent (a default-
/// constructed function) means senders stay read-only and a PATCH to one is
/// answered 501, which is what this served before there was anywhere to
/// send that could not answer for itself.
using ConnectionSenderPatcher =
    std::function<bool(const std::string& senderId, const ConnectionPatch&)>;

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
               ConnectionReceiverPatcher patcher,
               ConnectionSenderPatcher senderPatcher = ConnectionSenderPatcher{});
    void stop();
    bool isRunning() const;

    /// The port actually bound, once started.
    uint16_t boundPort() const;

    /// The href that goes in the IS-04 device's `controls`.
    std::string controlHref(const std::string& host) const;

    /// IS-05's own answer for what a sender/receiver is actually doing --
    /// master_enable and the id on the other end of the connection -- for
    /// whoever else on this driver publishes the same resource under the
    /// same id (the direct Node API, the NMOS registration client) and
    /// would otherwise have no way to ask this server rather than infer it.
    /// `exists` is false, the rest default, for an id this server has never
    /// heard of.
    ConnectionActiveState senderActiveState(const std::string& senderId) const;
    ConnectionActiveState receiverActiveState(const std::string& receiverId) const;

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

    using FallbackRouter = std::function<Reply(const std::string& method,
                                               const std::string& path,
                                               const std::string& body)>;
    /// Answers requests under /x-nmos/ that are not the Connection API:
    /// the Node API shares this port. Set before start(); the serving
    /// thread reads it without a lock.
    void setFallbackRouter(FallbackRouter router);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace AES67
