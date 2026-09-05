//
// nmos-node.h
// t41-ptp
//
// Registers this board with an NMOS IS-04 registry as a Node, and keeps
// the registration alive.
//
// A registry is how a plant knows what is on it: gear registers,
// controllers read the registry instead of probing. A PTP node has no
// senders and no receivers — this board carries a clock, not audio — so
// what it registers is a node and the clock underneath it, and that is
// all this writes. Devices, sources, flows, senders and receivers belong
// to the thing that carries the audio, not to this.
//
// WHAT THIS COSTS THE PTP PATH, SAID PLAINLY. There is no non-blocking
// connect in QNEthernet, so registering and every heartbeat block loop()
// for as long as the connection takes, up to kConnectTimeoutMs. That is
// why the timeout is short, why nothing is attempted more often than the
// heartbeat period, and why this is opt-in: a board whose only job is
// holding a clock steady should not be talking HTTP at all, and one that
// has spare loop() time can afford 100 ms every five seconds.
//
// The node id is derived from the clock identity, which is the MAC
// mapped to EUI-64: the same board is the same node after a reboot with
// nothing stored anywhere.
//
#pragma once

#include <QNEthernet.h>

#include "ptp/ptp-base.h"

/// Registers a PTP port as an IS-04 node.
class NMOSNode
{
public:
    /// How often IS-04 wants to hear from a node. A registry drops one it
    /// has not heard from in twelve seconds; five leaves room for two
    /// missed beats.
    static constexpr unsigned long HEARTBEAT_INTERVAL_MS = 5000;

    /// How long a connection to the registry may take before it is
    /// abandoned. Short on purpose: this blocks loop().
    static constexpr uint16_t CONNECT_TIMEOUT_MS = 100;

    /// The API version this speaks. What registries have served since
    /// 2018.
    static constexpr const char *API_VERSION = "v1.3";

    explicit NMOSNode(PTPBase &ptp);

    /// The registry to talk to. There is no discovery here: QNEthernet
    /// answers mDNS queries, it does not browse for services, so the
    /// address is configured rather than found.
    void begin(const IPAddress &registry, uint16_t port = 80);

    /// What a controller shows for this node. Defaults to "t41-ptp" plus
    /// the node id's first block.
    void setLabel(const char *label);

    /// Call from loop(). Registers when it has not, heartbeats when it
    /// has, and registers again when the registry says it has forgotten
    /// us -- which is what a 404 on the heartbeat means.
    void update();

    /// Removes the registration and stops the node: update() does
    /// nothing afterwards until begin() is called again. Worth calling
    /// before a planned reboot -- a controller showing a node that is
    /// gone is worse than one showing nothing.
    void unregisterNode();

    bool isRegistered() const { return registered; }

    /// The node's UUID, derived from the clock identity.
    const char *getNodeId() const { return nodeId; }

    /// How many times the registry has refused or gone quiet. Zero is
    /// what a working plant looks like.
    uint32_t getFailureCount() const { return failureCount; }

private:
    /// Builds the node id from the clock identity. Called once, when the
    /// PTP port has one.
    void buildNodeId();

    /// One request. Returns the HTTP status, or 0 when the registry could
    /// not be reached at all.
    int request(const char *method, const char *path, const char *body);

    bool postNode();
    bool heartbeat();

    PTPBase &ptp;
    IPAddress registryAddress{};
    uint16_t registryPort = 80;
    bool started = false;
    bool registered = false;
    // Zero is a time, not a sentinel: millis() really is 0 for the first
    // millisecond after a board boots, and using it to mean "never" made
    // the interval check pass every time through loop().
    bool beaten = false;
    unsigned long lastBeatMillis = 0;
    uint32_t failureCount = 0;

    char nodeId[37] = {0};
    char label[64] = {0};
};
