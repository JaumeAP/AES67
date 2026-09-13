//
// RegistrationClient.h
// AES67 RAVENNA session layer
// Telling an NMOS registry this node is here, and keeping it told.
//
// IS-04 has two ways for a controller to find a device. Peer-to-peer is the
// one this daemon already did: advertise _nmos-node._tcp and let whoever is
// browsing read the Node API directly. The other is a registry, and it is
// what a plant of any size actually runs -- a controller asks one server what
// exists instead of shouting at the link -- and a node that never registers
// is invisible to every one of them.
//
// So: listen for registries, POST the resources in the order their references
// need, and heartbeat every five seconds. A registry that stops answering is
// left for the next one on the list, which is what the priority in its
// advertisement is for.
//
// One thread, which drives the browser's socket as well as its own timers. It
// does not touch the Node API's state: it reads the resources and posts them,
// and everything it gets back is another machine's, parsed as such.
//
#pragma once

#include "Ravenna/NodeApi.h"
#include "Ravenna/RegistryBrowser.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace AES67::Ravenna {

/// IS-04 sec 4.1: a node is dropped when the registry has not heard from it
/// for the garbage collection interval, and the default heartbeat is every
/// five seconds.
inline constexpr int kHeartbeatSeconds = 5;

class RegistrationClient {
public:
    RegistrationClient(const NodeApi& node, std::string nodeId, std::string interfaceName,
                       uint32_t addressV4)
        : node_(node),
          nodeId_(std::move(nodeId)),
          interfaceName_(std::move(interfaceName)),
          addressV4_(addressV4) {}
    ~RegistrationClient();

    RegistrationClient(const RegistrationClient&) = delete;
    RegistrationClient& operator=(const RegistrationClient&) = delete;

    /// Starts the thread that browses, registers and heartbeats.
    void start();
    /// Stops it and waits for it. Safe to call twice, and the destructor
    /// calls it: the usual way a daemon ends is a signal.
    void stop();

    /// Where this node is registered right now, empty when nowhere. For the
    /// log line a person reads when a plant is not seeing a device.
    std::string registeredWith() const;

private:
    void run();
    /// Posts everything, in order. False when the registry refused or went
    /// away, which is the caller's signal to try the next one.
    bool registerEverything(const NmosRegistry& registry);
    /// POSTs the health resource. `unknown` comes back true when the registry
    /// answered 404, which is how it says it has never heard of this node.
    bool heartbeat(const NmosRegistry& registry, bool* unknown = nullptr);
    /// Takes up with a registry: a heartbeat first, because IS-04 sec 4.1
    /// says a node failing over asks whether the new registry already has it
    /// and only registers again when the answer is 404. Posting everything
    /// unasked is how a plant's failover turns into a burst of writes.
    bool takeUp(const NmosRegistry& registry);

    const NodeApi& node_;
    std::string nodeId_;
    std::string interfaceName_;
    uint32_t addressV4_ = 0;

    RegistryBrowser browser_{interfaceName_, addressV4_};
    std::thread thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex registeredLock_;
    std::string registeredWith_;
};

}  // namespace AES67::Ravenna
