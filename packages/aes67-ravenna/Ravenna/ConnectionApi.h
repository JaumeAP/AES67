//
// ConnectionApi.h
// AES67 RAVENNA session layer
// NMOS IS-05, which is how one device is told to take another's stream.
//
// This is the part that was missing. A sender publishes an SDP and a receiver
// can join a group, but nothing in between said "you, take that": in
// commercial gear that is the receiver's own interface, and between vendors
// it is this API. A controller PATCHes a transport file onto a receiver's
// staged endpoint, activates it, and the receiver joins.
//
// v1.1 of the Connection API, and the parts of it a device this size has:
// single senders and receivers, immediate activation, and the transport file.
// Scheduled activation is answered with a 400 saying so rather than accepted
// and forgotten, and bulk is not implemented -- a controller that finds no
// bulk endpoint uses the single ones.
//
// Platform-free: this decides what a request means and what the answer is,
// and HttpServer.h is what puts it on a socket.
//
#pragma once

#include "Ravenna/Json.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace AES67::Ravenna {

inline constexpr char kConnectionApiRoot[] = "/x-nmos/connection/v1.1";
/// IS-05's name for what this carries: RTP over multicast.
inline constexpr char kTransportRtpMulticast[] = "urn:x-nmos:transport:rtp.mcast";

/// What a sender or a receiver holds in each of its two states.
struct ConnectionState {
    /// IS-05 sec 4: nothing flows while this is false, whatever else is set.
    bool masterEnable = false;
    /// The SDP. A sender's is its own; a receiver's is the one a controller
    /// gave it, and is empty until then.
    std::string transportFile;
    /// Which sender a receiver was pointed at, empty for none. IS-05 sec 6
    /// carries it alongside the transport file, and it is the only place a
    /// controller reads back what a crosspoint was set to: a receiver that
    /// takes a stream and does not say whose looks unrouted.
    std::string senderId;
    /// activation.mode of the last change, "null" when it was never activated.
    std::string activationMode = "null";
    /// activation.activation_time, as IS-05 reports it: the time the change
    /// took effect, or empty when nothing has.
    std::string activationTime;
};

struct ConnectionSender {
    std::string id;              ///< the UUID a registry knows it by
    std::string label;
    std::string sdp;             ///< the transport file, always available
    ConnectionState staged;
    ConnectionState active;
};

struct ConnectionReceiver {
    std::string id;
    std::string label;
    ConnectionState staged;
    ConnectionState active;
};

/// One answer: a status, a content type and a body. Nothing here knows about
/// sockets, so nothing here writes to one.
struct ApiResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
};

class ConnectionApi {
public:
    /// Called when a receiver's staged state is activated, with the SDP it was
    /// given and whether it is enabled. This is where the host maps the
    /// stream's channels onto device channels -- aes67-core's
    /// StreamChannelMapper -- and where it joins or leaves the group.
    using ReceiverActivation =
        std::function<bool(const std::string& receiverId, const std::string& sdp,
                           bool masterEnable, std::string& error)>;

    void addSender(const ConnectionSender& sender);
    void addReceiver(const ConnectionReceiver& receiver);
    void onReceiverActivation(ReceiverActivation callback) {
        onActivation_ = std::move(callback);
    }

    std::optional<ConnectionSender> sender(const std::string& id) const;
    std::optional<ConnectionReceiver> receiver(const std::string& id) const;

    /// Everything this device offers and everything it can take, in id order.
    /// IS-04 lists the same resources this API connects, and listing them
    /// from two places is how the two come to disagree.
    std::vector<std::string> senderIds() const;
    std::vector<std::string> receiverIds() const;

    /// Answers one request. `method` is "GET" or "PATCH"; `path` is the whole
    /// path, root included; `body` is the payload of a PATCH.
    ApiResponse handle(const std::string& method, const std::string& path,
                       const std::string& body);

private:
    ApiResponse patchStagedReceiver(const std::string& id, const std::string& body);
    ApiResponse patchStagedSender(const std::string& id, const std::string& body);

    std::map<std::string, ConnectionSender> senders_;
    std::map<std::string, ConnectionReceiver> receivers_;
    ReceiverActivation onActivation_;
};

/// The state as IS-05 reports it, which is not how it is held: the
/// specification's staged object carries an activation sub-object and a
/// transport file wrapper, and a controller reads exactly those names.
/// `includeSenderId` is what makes this a receiver's state: a sender is the
/// far end of somebody else's subscription and carries a receiver_id instead.
JsonValue stateAsJson(const ConnectionState& state, bool includeTransportFile,
                      bool includeSenderId = false);

}  // namespace AES67::Ravenna
