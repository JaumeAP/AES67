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
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
    /// The node's own API root. Empty is legal and honest here: this
    /// driver serves no IS-04 Node API yet, and a registry that cannot
    /// reach one simply does not.
    std::string href;
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

    explicit NMOSRegistrationClient(NMOSNodeInfo node);
    ~NMOSRegistrationClient();

    NMOSRegistrationClient(const NMOSRegistrationClient&) = delete;
    NMOSRegistrationClient& operator=(const NMOSRegistrationClient&) = delete;

    /// Looks for a registry on the local link. Blocks for at most
    /// `waitFor`. Returns nothing when there is no registry, which is the
    /// normal case on a small installation and never an error.
    static std::optional<NMOSRegistry> discoverRegistry(
        std::chrono::milliseconds waitFor = std::chrono::milliseconds(2000));

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

    /// Registers the device and everything under it, and removes whatever
    /// the registry still holds from a previous call and this one does not
    /// mention. Call it after the streams are known and again whenever
    /// they change: a registry showing a stream that is gone sends
    /// controllers after nothing.
    ///
    /// The node has to be registered first — IS-04 refuses a device whose
    /// node it does not know.
    bool syncResources(const std::vector<NMOSSenderResource>& senders,
                       const std::vector<NMOSReceiverResource>& receivers);

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
    static std::string buildDeviceBody(const std::string& deviceId,
                                       const std::string& nodeId,
                                       const std::string& label,
                                       const std::vector<std::string>& senderIds,
                                       const std::vector<std::string>& receiverIds,
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
    static std::string buildSenderBody(const std::string& senderId,
                                       const std::string& flowId,
                                       const std::string& deviceId,
                                       const NMOSSenderResource& sender,
                                       int64_t versionSeconds, int32_t versionNanos);
    static std::string buildReceiverBody(const std::string& receiverId,
                                         const std::string& deviceId,
                                         const NMOSReceiverResource& receiver,
                                         int64_t versionSeconds, int32_t versionNanos);

private:
    bool postNode();

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
    std::atomic<bool> running_{false};
    std::thread heartbeatThread_;
};

} // namespace AES67
