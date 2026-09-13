//
// RegistryBrowser.h
// AES67 RAVENNA session layer
// Finding the NMOS registries on the link, and keeping up with them.
//
// IS-04 sec 3.1: a node does not get told where the registry is, it browses
// for _nmos-register._tcp and picks the one with the lowest `pri`. The TXT
// records carry which API versions that registry speaks, whether it wants
// HTTP or HTTPS, and that priority, and a plant with two registries
// advertises both so a node can fail over between them.
//
// It listens rather than asking once. A responder announces a service when it
// appears, unprompted, and answers a question once inside a short window; a
// browse that opens a socket, asks, waits a second and closes it hears only
// what happened to fall in that second, which on a plant bringing its backup
// registry up is usually nothing. So the socket stays open for as long as the
// daemon runs, every announcement it hears goes into the list, and the
// question is asked again from time to time for whatever was already there.
//
// It shares port 5353 with whatever else answers mDNS on this machine,
// because a responder replies to the multicast group and not to whoever
// asked: everyone on the link wants the answer, which is the whole idea.
//
#pragma once

#include "Ravenna/DnsSd.h"

#include <cstdint>
#include <map>
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
    /// How this one is named in a log line, and what tells two of them apart.
    std::string endpoint() const { return host + ":" + std::to_string(port); }
};

class RegistryBrowser {
public:
    RegistryBrowser(std::string interfaceName, uint32_t addressV4,
                    std::string wantedVersion = "v1.3")
        : interfaceName_(std::move(interfaceName)),
          addressV4_(addressV4),
          wantedVersion_(std::move(wantedVersion)) {}
    ~RegistryBrowser();

    RegistryBrowser(const RegistryBrowser&) = delete;
    RegistryBrowser& operator=(const RegistryBrowser&) = delete;

    /// Opens the socket, joins the group and asks the first question.
    bool start(std::string& error);
    void stop();

    /// Reads whatever has arrived, waiting up to `waitMs` for it, and folds it
    /// into what is known. Called from the caller's own loop, so this class
    /// carries no thread of its own.
    void service(int waitMs);

    /// Asks again. Cheap, and what makes a registry that was already up before
    /// this started show up at all.
    void ask();

    /// The registries known now: lowest priority first, and one that speaks
    /// neither this API version nor plain HTTP left out rather than talked to
    /// in terms it never offered.
    std::vector<NmosRegistry> registries() const;

private:
    std::string interfaceName_;
    uint32_t addressV4_ = 0;
    std::string wantedVersion_;
    int socket_ = -1;
    /// By instance name, because the records that make one up arrive in
    /// separate packets and a later one fills in what the first left out.
    std::map<std::string, DiscoveredService> known_;
};

/// The registries a set of advertisements describes: the half of the browse
/// that can be held against a packet in a test.
std::vector<NmosRegistry> registriesFrom(const std::vector<DiscoveredService>& services,
                                         const std::string& wantedVersion);

}  // namespace AES67::Ravenna
