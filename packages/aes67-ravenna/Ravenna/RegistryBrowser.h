//
// RegistryBrowser.h
// AES67 RAVENNA session layer
// Finding the NMOS registries on the link.
//
// IS-04 sec 3.1: a node does not get told where the registry is, it browses
// for _nmos-register._tcp and picks the one with the lowest `pri`. The TXT
// records carry which API versions that registry speaks, whether it wants
// HTTP or HTTPS, and that priority, and a plant with two registries
// advertises both so a node can fail over between them.
//
// One shot rather than a running browser: a node browses when it starts and
// again when the registry it was using stops answering, which is twice in a
// working day. A thread sitting on a socket for that is a thread to shut
// down cleanly for nothing.
//
#pragma once

#include "Ravenna/DnsSd.h"

#include <cstdint>
#include <string>
#include <vector>

namespace AES67::Ravenna {

/// A registry this node could use, as its advertisement described it.
struct NmosRegistry {
    std::string host;        ///< the address, or the name when there was no A
    uint16_t port = 0;
    std::string apiVersion = "v1.3";
    /// RFC 6763's TXT `pri`, lowest first. IS-04 sec 3.1 says a node uses the
    /// lowest it can reach and only moves on when that one fails.
    int priority = 100;

    bool valid() const { return !host.empty() && port != 0; }
    /// Where the registration API of this registry lives.
    std::string registrationPath() const { return "/x-nmos/registration/" + apiVersion; }
};

/// Asks the link for registries and waits `timeoutMs` for the answers, on
/// `interfaceName` -- the one this node's streams are on, because a machine
/// with two networks would otherwise register itself on whichever the
/// routing table preferred.
///
/// The list comes back sorted: lowest priority first, and a registry that
/// does not speak `wantedVersion` is left out rather than talked to in a
/// version it never offered. Empty means nothing answered, which is the
/// ordinary state of a link with no registry on it.
std::vector<NmosRegistry> browseForRegistries(const std::string& interfaceName,
                                              uint32_t addressV4, int timeoutMs,
                                              const std::string& wantedVersion = "v1.3");

/// The registries an mDNS response describes, without any sockets: the half
/// of the browse that can be held against a packet in a test.
std::vector<NmosRegistry> registriesFrom(const std::vector<DiscoveredService>& services,
                                         const std::string& wantedVersion);

}  // namespace AES67::Ravenna
