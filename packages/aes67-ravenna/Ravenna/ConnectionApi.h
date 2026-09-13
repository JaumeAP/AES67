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
// v1.1 of the Connection API: single senders and receivers, the transport
// file, the bulk endpoints, and all three activation modes. A scheduled one
// is answered with a 202 and happens at the request that first arrives after
// its time -- this API has no thread of its own, and nothing learns what is
// active without asking.
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
/// IS-04's name for what this carries: RTP over multicast. It is the value a
/// node's sender and receiver resources publish as their `transport`.
inline constexpr char kTransportRtpMulticast[] = "urn:x-nmos:transport:rtp.mcast";
/// The same transport with the subclassification taken off, which is what
/// IS-05's own transporttype endpoint answers: its schema is an enum of the
/// four base URNs, and rtp.mcast is not one of them. A controller reads the
/// multicast half from IS-04.
inline constexpr char kTransportRtp[] = "urn:x-nmos:transport:rtp";

/// The three ways NMOS activates a change, and the only three there are.
/// IS-05 stages a connection and IS-08 a grid, and both use these names.
inline constexpr char kActivateImmediate[] = "activate_immediate";
inline constexpr char kActivateRelative[] = "activate_scheduled_relative";
inline constexpr char kActivateAbsolute[] = "activate_scheduled_absolute";

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
    /// Which receiver a controller pointed at this sender, empty for none.
    /// IS-05 sec 6 reports it as `receiver_id`, and it is not the field above
    /// under another name: the two ends name each other, and one sender read
    /// back as its own subscriber is how a controller loses track of a route.
    std::string receiverId;
    /// The transport parameters a controller fixed by PATCH, by name. IS-05
    /// fills these from the transport file and then lets a controller correct
    /// them, so a name here wins over what the SDP says and a name absent is
    /// whatever the SDP gives.
    JsonObject transportParams;
    /// activation.mode of the last change, "null" when it was never activated.
    std::string activationMode = "null";
    /// activation.requested_time, which only a scheduled activation carries:
    /// the offset or the instant a controller asked for, echoed back.
    std::string activationRequestedTime;
    /// activation.activation_time, as IS-05 reports it: the time the change
    /// took effect, or the time a scheduled one is due, or empty when nothing
    /// has happened.
    std::string activationTime;
};

/// A scheduled activation waiting for its moment. IS-05 sec 4 answers a
/// scheduled request with a 202 and makes the change later, so this is what
/// has been promised and when.
struct PendingActivation {
    bool waiting = false;
    uint64_t dueSeconds = 0;   ///< TAI, as the activation time reports it
    uint32_t dueNanos = 0;
    ConnectionState state;     ///< what becomes active when the time comes
};

struct ConnectionSender {
    std::string id;              ///< the UUID a registry knows it by
    std::string label;
    std::string sdp;             ///< the transport file, always available
    ConnectionState staged;
    ConnectionState active;
    PendingActivation pending;
};

struct ConnectionReceiver {
    std::string id;
    std::string label;
    ConnectionState staged;
    ConnectionState active;
    PendingActivation pending;
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

    /// Called when a sender's staged state is activated. The SDP is the one
    /// the sender now describes itself with, destination and port already
    /// followed to whatever the controller staged, so a host that re-points a
    /// stream reads where it goes from the file rather than from the leg.
    ///
    /// Senders had no equivalent of the callback above, and a device whose
    /// only configurable end is the source -- gear with no control protocol
    /// of its own is patched by moving the sender, not the receiver -- had
    /// nowhere to be told that a controller had moved it.
    using SenderActivation =
        std::function<bool(const std::string& senderId, const std::string& sdp,
                           bool masterEnable, std::string& error)>;

    void addSender(const ConnectionSender& sender);
    void addReceiver(const ConnectionReceiver& receiver);
    /// Forgets a resource. A host whose set of senders and receivers changes
    /// while this is serving -- the macOS driver's does, as streams are
    /// discovered and go away -- would otherwise keep answering for one that
    /// is no longer there.
    void removeSender(const std::string& id);
    void removeReceiver(const std::string& id);
    void onReceiverActivation(ReceiverActivation callback) {
        onActivation_ = std::move(callback);
    }
    void onSenderActivation(SenderActivation callback) {
        onSenderActivation_ = std::move(callback);
    }

    /// The address of the interface this device receives on. IS-05 lets a
    /// receiver answer "auto" while nothing is activated, but what is active
    /// has to name the interface it is actually using, and only the host
    /// knows which that is.
    void setInterfaceAddress(std::string address) { interfaceAddress_ = std::move(address); }

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
    /// Answers a POST to /bulk/senders or /bulk/receivers by running each
    /// entry through the single endpoint it stands for.
    ApiResponse patchInBulk(bool forSenders, const std::string& body);
    /// Fires every scheduled activation whose time has come. Called at the
    /// top of every request, which is the only clock this API is driven by:
    /// nothing reads a state without going through here first.
    void applyDueActivations();
    /// A state as the active endpoint has to report it, with every "auto"
    /// resolved. A resource nobody has activated yet still answers /active,
    /// and answering "auto" there tells a controller nothing about where the
    /// stream is.
    JsonValue activeAsJson(const ConnectionState& state, bool forSender) const;
    bool activateSender(ConnectionSender& sender, ConnectionState state, std::string& error);
    bool activateReceiver(ConnectionReceiver& receiver, ConnectionState state,
                          std::string& error);

    std::map<std::string, ConnectionSender> senders_;
    std::map<std::string, ConnectionReceiver> receivers_;
    ReceiverActivation onActivation_;
    SenderActivation onSenderActivation_;
    std::string interfaceAddress_;
};

/// The state as IS-05 reports it, which is not how it is held: the
/// specification's staged object carries an activation sub-object and a
/// transport file wrapper, and a controller reads exactly those names.
/// `includeSenderId` is what makes this a receiver's state: a sender is the
/// far end of somebody else's subscription and carries a receiver_id instead.
JsonValue stateAsJson(const ConnectionState& state, bool forSender);

}  // namespace AES67::Ravenna
