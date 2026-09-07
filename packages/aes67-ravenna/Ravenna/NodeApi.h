//
// NodeApi.h
// AES67 RAVENNA session layer
// NMOS IS-04, the half that makes this device exist to a controller.
//
// IS-05 connects and IS-08 routes, but a controller has to find the thing
// first and know what it is made of: a node, its device, and for each stream a
// source, a flow and a sender, with receivers for what it can take. That is
// this file, and it is what turns a list of IP addresses into a list of names
// somebody can route.
//
// Served, not registered. IS-04 has two modes and this is the peer-to-peer
// one: the node advertises itself over mDNS as _nmos-node._tcp and a
// controller browsing the link reads it here. Registering with a registry is
// an HTTP client and a heartbeat, which is a different piece of work and is
// not pretended at.
//
// The resources are built from what the device actually holds -- the session
// catalogue, the connection API's receivers -- rather than kept alongside it,
// for the same reason the channel grid is: two descriptions of one device
// disagree the moment one changes.
//
#pragma once

#include "Ravenna/ConnectionApi.h"
#include "Ravenna/SessionCatalogue.h"

#include <string>

namespace AES67::Ravenna {

inline constexpr char kNodeApiRoot[] = "/x-nmos/node/v1.3";

/// What this node says about itself, none of which it can work out alone.
struct NodeIdentity {
    /// UUIDs. A controller keys everything on them and they have to be the
    /// same across restarts, or every restart looks like a new device.
    std::string nodeId;
    std::string deviceId;
    std::string label = "AES67";
    std::string description = "AES67 sender and receiver";
    std::string hostName = "aes67.local";
    uint32_t addressV4 = 0;   ///< host byte order
    uint16_t apiPort = 8080;  ///< where this API and the others answer
    /// The PTP grandmaster this device's clock follows, as IS-04 names it.
    /// Empty means it says its clock is internal, which is the honest answer
    /// for a device with no reference.
    std::string ptpGrandmaster;
};

class NodeApi {
public:
    NodeApi(const NodeIdentity& identity, const SessionCatalogue& catalogue,
            const ConnectionApi& connections)
        : identity_(identity), catalogue_(catalogue), connections_(connections) {}

    ApiResponse handle(const std::string& method, const std::string& path,
                       const std::string& body);

    /// The DNS-SD advertisement that makes a controller find this node
    /// without a registry.
    SessionAdvertisement advertisement() const;

private:
    JsonValue self() const;
    JsonValue devices() const;
    JsonValue sources() const;
    JsonValue flows() const;
    JsonValue senders() const;
    JsonValue receivers() const;

    /// A sender's id is the connection API's, found by the session's name.
    /// IS-05 addresses a sender by the id IS-04 published, so a second id
    /// derived here would be a route a controller cannot follow. Receivers
    /// are the connection API's ids outright, with nothing to match on.
    std::string senderIdFor(const std::string& sessionName) const;

    /// Sources and flows are derived from the session's name rather than
    /// random, so a restart does not renumber a plant's routing. IS-05 never
    /// addresses either, so nothing else has to agree on them.
    std::string sourceIdFor(const std::string& sessionName) const;
    std::string flowIdFor(const std::string& sessionName) const;

    NodeIdentity identity_;
    const SessionCatalogue& catalogue_;
    const ConnectionApi& connections_;
};

/// A UUID built from a name, so the same name always gives the same id and two
/// different names never give one. Version 5 in shape rather than in
/// derivation: what matters here is that it is stable and unique, and a
/// controller only ever compares them.
std::string stableUuidFrom(const std::string& name);

}  // namespace AES67::Ravenna
