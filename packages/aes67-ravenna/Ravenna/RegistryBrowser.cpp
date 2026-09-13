#include "Ravenna/RegistryBrowser.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace AES67::Ravenna {
namespace {

std::string addressText(uint32_t addressV4) {
    struct in_addr address {};
    address.s_addr = htonl(addressV4);
    char text[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &address, text, sizeof(text)) == nullptr) return {};
    return text;
}

/// Whether a registry advertising `versions` speaks the one this node uses.
/// The key is a comma-separated list (IS-04 sec 3.1), and a registry that
/// advertises none is taken at its word for the default rather than dropped:
/// not every implementation fills the TXT in.
bool speaks(const std::string& versions, const std::string& wanted) {
    if (versions.empty()) return true;

    size_t start = 0;
    while (start <= versions.size()) {
        const size_t comma = versions.find(',', start);
        const size_t end = comma == std::string::npos ? versions.size() : comma;
        std::string one = versions.substr(start, end - start);
        one.erase(0, one.find_first_not_of(" \t"));
        const size_t last = one.find_last_not_of(" \t");
        if (last != std::string::npos) one.erase(last + 1);
        if (one == wanted) return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

}  // namespace

std::vector<NmosRegistry> registriesFrom(const std::vector<DiscoveredService>& services,
                                         const std::string& wantedVersion) {
    std::vector<NmosRegistry> registries;

    for (const DiscoveredService& service : services) {
        if (!speaks(service.txt("api_ver"), wantedVersion)) continue;
        // No TLS here, so a registry that only offers HTTPS is one this node
        // cannot reach: saying so by leaving it out beats failing at connect.
        if (service.txt("api_proto", "http") != "http") continue;

        NmosRegistry registry;
        // The address when the response carried one, and the name when it did
        // not: a registry that answered the browse but sent no A record is
        // still reachable through whatever resolves its host name.
        registry.host = service.addressV4 != 0 ? addressText(service.addressV4)
                                               : service.hostName;
        registry.port = service.port;
        registry.apiVersion = wantedVersion;

        // A priority that is not a number is one this node cannot order by,
        // and the default leaves it behind everything that named one.
        const std::string priority = service.txt("pri");
        if (!priority.empty() && priority.size() <= 6 &&
            std::all_of(priority.begin(), priority.end(), [](unsigned char digit) {
                return std::isdigit(digit) != 0;
            })) {
            registry.priority = std::atoi(priority.c_str());
        }
        if (registry.valid()) registries.push_back(registry);
    }

    // Lowest first, and a stable sort so two registries at the same priority
    // stay in the order they answered rather than swapping between browses.
    std::stable_sort(registries.begin(), registries.end(),
                     [](const NmosRegistry& left, const NmosRegistry& right) {
                         return left.priority < right.priority;
                     });
    return registries;
}

std::vector<NmosRegistry> browseForRegistries(const std::string& interfaceName,
                                              uint32_t addressV4, int timeoutMs,
                                              const std::string& wantedVersion) {
    (void)interfaceName;

    const int socketFd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socketFd < 0) return {};

    // An ephemeral port rather than 5353: this asks and listens for the
    // answers to its own question, and binding the mDNS port would fight the
    // responder this daemon already runs there.
    struct sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (::bind(socketFd, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) < 0) {
        ::close(socketFd);
        return {};
    }

    // Out of the interface this node's streams are on, so a machine with two
    // networks does not register itself across whichever one the routing
    // table happened to prefer.
    struct in_addr outgoing {};
    outgoing.s_addr = htonl(addressV4);
    ::setsockopt(socketFd, IPPROTO_IP, IP_MULTICAST_IF, &outgoing, sizeof(outgoing));

    struct ip_mreq join {};
    join.imr_multiaddr.s_addr = ::inet_addr(kMdnsGroup);
    join.imr_interface.s_addr = htonl(addressV4);
    ::setsockopt(socketFd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &join, sizeof(join));

    struct sockaddr_in group {};
    group.sin_family = AF_INET;
    group.sin_port = htons(kMdnsPort);
    group.sin_addr.s_addr = ::inet_addr(kMdnsGroup);

    const std::vector<uint8_t> query = buildQuery(kNmosRegisterService);
    if (::sendto(socketFd, query.data(), query.size(), 0,
                 reinterpret_cast<struct sockaddr*>(&group), sizeof(group)) < 0) {
        ::close(socketFd);
        return {};
    }

    // Every answer inside the window, not the first: a link with two
    // registries answers twice, and taking the first would pick whichever was
    // quicker rather than whichever has the priority.
    std::vector<DiscoveredService> found;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    uint8_t buffer[4096];

    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) break;

        struct pollfd waiting {};
        waiting.fd = socketFd;
        waiting.events = POLLIN;
        const int ready = ::poll(&waiting, 1, static_cast<int>(left));
        if (ready <= 0) break;

        const ssize_t received = ::recv(socketFd, buffer, sizeof(buffer), 0);
        if (received <= 0) continue;

        for (DiscoveredService& service :
             parseServiceResponse(buffer, static_cast<size_t>(received), kNmosRegisterService)) {
            const auto already = std::find_if(
                found.begin(), found.end(), [&service](const DiscoveredService& seen) {
                    return seen.instanceName == service.instanceName;
                });
            if (already == found.end()) {
                found.push_back(std::move(service));
            } else {
                // A later packet carrying the A record for one already seen,
                // which is how a responder is entitled to split them.
                if (already->addressV4 == 0) already->addressV4 = service.addressV4;
                if (already->txtEntries.empty()) already->txtEntries = service.txtEntries;
            }
        }
    }

    ::close(socketFd);
    return registriesFrom(found, wantedVersion);
}

}  // namespace AES67::Ravenna
