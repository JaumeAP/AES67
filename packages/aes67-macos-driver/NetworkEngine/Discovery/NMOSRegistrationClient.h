//
// NMOSRegistrationClient.h
// AES67 macOS Driver
//
// Registers this driver as an IS-04 Node with an NMOS registry, and keeps
// the registration alive.
//
// A registry is how a broadcast plant knows what exists on it: gear
// registers, controllers read the registry rather than probing the
// network. The RAVENNA driver installed on this machine registers
// (`_nmos-register._tcp` in its bundle, `$.NMOS.configuration`); this one
// browsed for registries and never told one it was here.
//
// The node and its heartbeat came first, because a node on its own is what
// a controller needs to see the driver at all. The rest of the tree —
// device, source, flow, sender, receiver — is what makes the streams
// visible, and it is here too: a registry that knows the driver exists but
// not what it sends is an inventory with a hole in it.
//
// What the registry answers is parsed defensively: it is another machine
// on the network, this runs inside coreaudiod, and a registry that has
// been replaced by something else entirely must not take the audio daemon
// down with it.
//
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace AES67::Ravenna {
struct NmosRegistry;
}

namespace AES67 {

/// Where a registry is. The API version is not discovered — the TXT
/// records that would carry `api_ver`, `api_proto` and `pri` are not
/// exposed by MDNSBrowser today — so v1.3 over plain HTTP is assumed,
/// which is what registries have served since 2018.
struct NMOSRegistry {
    std::string host;
    uint16_t port{0};
    std::string apiVersion{"v1.3"};

    bool valid() const { return !host.empty() && port != 0; }
};

/// What goes in the Node resource. The id has to be the SAME UUID across
/// restarts: a registry keyed by a new id every launch fills up with
/// ghosts of this driver.
struct NMOSNodeInfo {
    std::string id;
    std::string label{"AES67 macOS Driver"};
    std::string description{"AES67 virtual audio device"};
    std::string hostname;
    /// The node's own API root. The driver serves an IS-04 Node API on
    /// the Connection API's port and fills this in with it. Empty stays
    /// legal, for the runs where no port was bound: a registry that
    /// cannot reach a Node API simply does not.
    std::string href;
    /// Where the node's own IS-04 Node API answers, for `api.endpoints`.
    /// Port 0 means none is served and the list stays empty.
    std::string apiHost;
    uint16_t apiPort{0};
    /// The interface this node's streams leave by and arrive on. IS-04 makes
    /// a node publish its interfaces and makes every sender and receiver name
    /// the one it uses, and a controller works out which devices can reach
    /// each other from exactly that. Empty publishes no interfaces, which is
    /// what a node with no network to speak of should say.
    std::string interfaceName;
    /// That interface's hardware address, as IS-04 writes one: six lower-case
    /// hex pairs joined by hyphens. Empty when the interface has none of its
    /// own, which is the loopback's case.
    std::string interfaceMac;
};

/// One transmit stream, as IS-04 sees it: a source (what the audio IS), a
/// flow (how it is encoded) and a sender (where it goes). The three ids
/// are derived, not stored, so they are the same after a restart.
struct NMOSSenderResource {
    std::string name;          ///< The session name, which is what a controller shows.
    std::string description;
    std::string multicastAddress;
    uint16_t port{0};
    std::string sourceAddress; ///< The interface this leaves by, when known.
    uint32_t sampleRate{48000};
    uint16_t channels{2};
    /// "L16" or "L24" — what goes in the flow's media_type as audio/L16
    /// or audio/L24.
    std::string encoding{"L24"};
};

/// One receive stream. IS-04 receivers advertise what they CAN take, not
/// what they are taking, so this carries the capability and the state of
/// the subscription separately.
struct NMOSReceiverResource {
    std::string name;
    std::string description;
    /// The sender this receiver is currently pulling, when it is pulling
    /// one. Empty is a receiver that is configured and idle, which is a
    /// state a controller needs to see.
    std::string subscribedMulticastAddress;
    bool active{false};
};

class NMOSRegistrationClient {
public:
    /// How often IS-04 wants to hear from a node. The registry drops a
    /// node it has not heard from in 12 seconds; 5 leaves room for two
    /// missed beats.
    static constexpr std::chrono::seconds kHeartbeatPeriod{5};

    /// Where the version stamped on every resource comes from. IS-04 makes
    /// a resource's version part of what it is, so the copy a registry holds
    /// and the copy the Node API serves have to carry the same one until the
    /// resource actually changes. Each was taking its own reading of the
    /// clock, so the two never matched and a controller reading both saw one
    /// resource superseding the other for ever.
    ///
    /// Unset means this client reads the clock itself, which is right for a
    /// node that serves no Node API of its own.
    using VersionSource = std::function<void(int64_t& seconds, int32_t& nanos)>;

    explicit NMOSRegistrationClient(NMOSNodeInfo node);
    ~NMOSRegistrationClient();

    NMOSRegistrationClient(const NMOSRegistrationClient&) = delete;
    NMOSRegistrationClient& operator=(const NMOSRegistrationClient&) = delete;

    /// The registries on the link, lowest IS-04 priority first, and only
    /// those whose advertisement says they speak this node's API version over
    /// plain HTTP. Blocks for at most `waitFor`. An empty list is the normal
    /// case on a small installation and never an error.
    ///
    /// The browsing is aes67-ravenna's RegistryBrowser: it reads the TXT
    /// records rather than taking the first thing that answers, which is what
    /// this used to do -- it registered with whatever advertised
    /// _nmos-register._tcp, whatever version or protocol that registry said
    /// it spoke, and never looked at `pri` at all.
    std::vector<NMOSRegistry> discoverRegistries(
        std::chrono::milliseconds waitFor = std::chrono::milliseconds(2000)) const;

    /// The first of them, or nothing.
    std::optional<NMOSRegistry> discoverRegistry(
        std::chrono::milliseconds waitFor = std::chrono::milliseconds(2000)) const;

    /// POSTs the Node resource. True when the registry took it: 201 for a
    /// new node, 200 when it already knew this id.
    bool registerWith(const NMOSRegistry& registry);

    /// One heartbeat. False when the registry is gone or refuses; the
    /// caller decides whether that is worth re-registering for.
    bool heartbeat();

    /// Heartbeats on a thread of its own until stop(). Re-registers when
    /// the registry answers 404: that is what it says after it has
    /// garbage-collected us, and it is the documented way back in.
    void startHeartbeats();
    void stop();

    /// Takes the version to stamp from somewhere else. Set before
    /// registering; the registration and heartbeat threads only read it.
    void useVersionFrom(VersionSource source) { versionSource_ = std::move(source); }

    /// Registers the device and everything under it, and removes whatever
    /// the registry still holds from a previous call and this one does not
    /// mention. Call it after the streams are known and again whenever
    /// they change: a registry showing a stream that is gone sends
    /// controllers after nothing.
    ///
    /// The node has to be registered first — IS-04 refuses a device whose
    /// node it does not know.
    bool syncResources(const std::vector<NMOSSenderResource>& senders,
                       const std::vector<NMOSReceiverResource>& receivers,
                       const std::string& controlHref = {});

    /// DELETEs the Node resource, and everything under it first: a
    /// registry that is told beats waiting for a timeout to expire.
    bool unregister();

    bool isRegistered() const { return registered_.load(std::memory_order_relaxed); }

    // --- The pure parts, so they can be checked without a registry ---

    /// The registration endpoint for an API version.
    static std::string registrationPath(const std::string& apiVersion);
    /// The health endpoint for one node.
    static std::string healthPath(const std::string& apiVersion, const std::string& nodeId);
    /// The POST body: {"type": "node", "data": {...}}.
    static std::string buildRegistrationBody(const NMOSNodeInfo& node,
                                             int64_t versionSeconds,
                                             int32_t versionNanos);

    /// A UUID derived from a name inside a namespace, RFC 4122 version 5.
    ///
    /// Every resource under a node needs an id that survives a restart:
    /// a registry keyed by fresh ids each launch accumulates copies of the
    /// same stream. Deriving them from the node id and the stream's own
    /// name gives the same answer on every run without storing anything.
    static std::string deriveId(const std::string& namespaceUuid, const std::string& name);

    /// The bodies, so they can be read without a registry. Each returns
    /// the full {"type": ..., "data": {...}} a registration POST takes.
    /// `controlHref` is the IS-05 Connection API's root. Empty leaves the
    /// controls list empty, which is what to say when there is no
    /// connection control to point at.
    static std::string buildDeviceBody(const std::string& deviceId,
                                       const std::string& nodeId,
                                       const std::string& label,
                                       const std::vector<std::string>& senderIds,
                                       const std::vector<std::string>& receiverIds,
                                       const std::string& controlHref,
                                       int64_t versionSeconds, int32_t versionNanos);
    static std::string buildSourceBody(const std::string& sourceId,
                                       const std::string& deviceId,
                                       const NMOSSenderResource& sender,
                                       int64_t versionSeconds, int32_t versionNanos);
    static std::string buildFlowBody(const std::string& flowId,
                                     const std::string& sourceId,
                                     const std::string& deviceId,
                                     const NMOSSenderResource& sender,
                                     int64_t versionSeconds, int32_t versionNanos);
    /// `interfaceName` goes into the resource's `interface_bindings`; see
    /// buildSenderData below.
    static std::string buildSenderBody(const std::string& senderId,
                                       const std::string& flowId,
                                       const std::string& deviceId,
                                       const NMOSSenderResource& sender,
                                       int64_t versionSeconds, int32_t versionNanos,
                                       const std::string& interfaceName = {});
    static std::string buildReceiverBody(const std::string& receiverId,
                                         const std::string& deviceId,
                                         const NMOSReceiverResource& receiver,
                                         int64_t versionSeconds, int32_t versionNanos,
                                         const std::string& interfaceName = {});

    /// The bare resource objects. A registry takes them wrapped by
    /// wrapResource(); the Node API serves them as they are.
    static std::string buildNodeData(const NMOSNodeInfo& node,
                                     int64_t versionSeconds, int32_t versionNanos);
    static std::string buildDeviceData(const std::string& deviceId,
                                       const std::string& nodeId,
                                       const std::string& label,
                                       const std::vector<std::string>& senderIds,
                                       const std::vector<std::string>& receiverIds,
                                       const std::string& controlHref,
                                       int64_t versionSeconds, int32_t versionNanos);
    static std::string buildSourceData(const std::string& sourceId,
                                       const std::string& deviceId,
                                       const NMOSSenderResource& sender,
                                       int64_t versionSeconds, int32_t versionNanos);
    static std::string buildFlowData(const std::string& flowId,
                                     const std::string& sourceId,
                                     const std::string& deviceId,
                                     const NMOSSenderResource& sender,
                                     int64_t versionSeconds, int32_t versionNanos);
    /// `interfaceName` is the node interface this resource is bound to, and
    /// has to be one the node's own `interfaces` names: IS-04 is how a
    /// controller works out which devices can reach each other, and it does
    /// it by matching these. Empty publishes an empty binding list.
    static std::string buildSenderData(const std::string& senderId,
                                       const std::string& flowId,
                                       const std::string& deviceId,
                                       const NMOSSenderResource& sender,
                                       int64_t versionSeconds, int32_t versionNanos,
                                       const std::string& interfaceName = {});
    static std::string buildReceiverData(const std::string& receiverId,
                                         const std::string& deviceId,
                                         const NMOSReceiverResource& receiver,
                                         int64_t versionSeconds, int32_t versionNanos,
                                         const std::string& interfaceName = {});
    /// `{"type": <type>, "data": <data>}`, the shape a registration POST takes.
    static std::string wrapResource(const std::string& type, const std::string& data);

private:
    bool postNode();
    /// Registers with the first of these that takes it, skipping the one in
    /// use. Called by the heartbeat thread when that one has stopped
    /// answering. False when none of them would have it.
    bool failOverTo(const std::vector<Ravenna::NmosRegistry>& candidates);
    /// The version to stamp: the source above when one was given, the clock
    /// otherwise.
    void versionNow(int64_t& seconds, int32_t& nanos) const;

    /// POSTs one already-built body. Shared by everything above.
    bool postResource(const std::string& body);
    /// DELETEs one resource by type and id, e.g. ("senders", id).
    bool deleteResource(const std::string& type, const std::string& id);

    NMOSNodeInfo node_;
    NMOSRegistry registry_;
    /// What the registry currently holds because of us, so the next sync
    /// knows what to take away. Type to ids.
    std::vector<std::pair<std::string, std::string>> published_;
    mutable std::mutex mutex_;
    std::atomic<bool> registered_{false};
    VersionSource versionSource_;
    std::atomic<bool> running_{false};
    std::thread heartbeatThread_;
};

} // namespace AES67
