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

#include <map>
#include <mutex>
#include <string>
#include <utility>

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
    /// The interface this node's streams are bound to. Senders and receivers
    /// name it in their interface_bindings, and a controller matches those
    /// against this node's own list, so the two have to be the same string.
    std::string interfaceName = "eth0";
    /// Its MAC, as IS-04 writes one: six lowercase hex pairs joined by
    /// hyphens. The schema requires a Port ID in that shape -- it is what
    /// IS-06 does topology discovery with -- and takes neither null nor
    /// anything else, so a node that cannot read its own says so with zeros
    /// rather than making the resource invalid.
    std::string interfaceMac = "00-00-00-00-00-00";
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

    /// Everything this node has, paired with the IS-04 type name of each, in
    /// the order a registry has to be given them: a resource is refused
    /// while the one it names is not there yet, so the node comes first, then
    /// its device, then the sources, flows, senders and receivers that point
    /// back at it.
    std::vector<std::pair<std::string, JsonValue>> resourcesInRegistrationOrder() const;

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

    /// Stamps a resource with its version. IS-04 sec 4: the version says when
    /// a resource last CHANGED, so a fresh one on every read makes the copy a
    /// registry holds differ from the one this API serves a moment later, and
    /// a controller comparing them concludes the node is out of date on every
    /// comparison. The shape is remembered instead, and the version moves
    /// only when the shape does.
    JsonValue versioned(JsonObject resource) const;


    NodeIdentity identity_;
    const SessionCatalogue& catalogue_;
    const ConnectionApi& connections_;

    /// Per resource id: the last shape seen, and the version stamped on it.
    /// Guarded because the registration client reads the resources from its
    /// own thread while the HTTP server answers from the main one.
    mutable std::mutex versionsLock_;
    mutable std::map<std::string, std::pair<std::string, std::string>> versions_;
};

/// A UUID built from a name, so the same name always gives the same id and two
/// different names never give one. Version 5 in shape rather than in
/// derivation: what matters here is that it is stable and unique, and a
/// controller only ever compares them.
std::string stableUuidFrom(const std::string& name);

}  // namespace AES67::Ravenna
