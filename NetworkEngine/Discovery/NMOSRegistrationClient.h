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
// THIS IS THE NODE AND ITS HEARTBEAT, AND NOTHING ELSE YET. IS-04 also
// carries devices, sources, flows, senders and receivers, which is where
// the streams this driver actually serves would go. A node on its own is
// what a controller needs to see the driver at all, and it is the half
// that can be written without touching stream lifecycle.
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

    /// DELETEs the Node resource. A registry that is told beats waiting
    /// for a timeout to expire.
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

private:
    bool postNode();

    NMOSNodeInfo node_;
    NMOSRegistry registry_;
    mutable std::mutex mutex_;
    std::atomic<bool> registered_{false};
    std::atomic<bool> running_{false};
    std::thread heartbeatThread_;
};

} // namespace AES67
