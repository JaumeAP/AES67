#include "Ravenna/RegistryBrowser.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
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
            registry.priority = static_cast<int>(std::strtol(priority.c_str(), nullptr, 10));
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

RegistryBrowser::~RegistryBrowser() { stop(); }

bool RegistryBrowser::start(std::string& error) {
    if (socket_ >= 0) return true;
    (void)interfaceName_;

    socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0) {
        error = std::string("socket(): ") + std::strerror(errno);
        return false;
    }

    // 5353 is a port every responder on the machine binds at once, this
    // daemon's own included, so both of these are needed before the bind.
    const int on = 1;
    ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    ::setsockopt(socket_, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif

    struct sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(kMdnsPort);
    if (::bind(socket_, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) < 0) {
        error = std::string("bind 5353: ") + std::strerror(errno);
        stop();
        return false;
    }

    struct ip_mreq join {};
    join.imr_multiaddr.s_addr = ::inet_addr(kMdnsGroup);
    join.imr_interface.s_addr = htonl(addressV4_);
    if (::setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &join, sizeof(join)) < 0) {
        error = std::string("IP_ADD_MEMBERSHIP on ") + kMdnsGroup + ": " + std::strerror(errno);
        stop();
        return false;
    }

    // Out of the interface this node's streams are on, so a machine with two
    // networks does not look for its registry across whichever one the
    // routing table happened to prefer.
    struct in_addr outgoing {};
    outgoing.s_addr = htonl(addressV4_);
    ::setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_IF, &outgoing, sizeof(outgoing));

    // And loop the question back, because the registry a test runs against is
    // often on this same machine.
    const unsigned char loopback = 1;
    ::setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_LOOP, &loopback, sizeof(loopback));

    ask();
    return true;
}

void RegistryBrowser::stop() {
    if (socket_ >= 0) ::close(socket_);
    socket_ = -1;
}

void RegistryBrowser::ask() {
    if (socket_ < 0) return;

    struct sockaddr_in group {};
    group.sin_family = AF_INET;
    group.sin_port = htons(kMdnsPort);
    group.sin_addr.s_addr = ::inet_addr(kMdnsGroup);

    const std::vector<uint8_t> query = buildQuery(kNmosRegisterService);
    (void)::sendto(socket_, query.data(), query.size(), 0,
                   reinterpret_cast<struct sockaddr*>(&group), sizeof(group));
}

void RegistryBrowser::service(int waitMs) {
    if (socket_ < 0) return;

    uint8_t buffer[4096];
    while (true) {
        struct pollfd waiting {};
        waiting.fd = socket_;
        waiting.events = POLLIN;
        if (::poll(&waiting, 1, waitMs) <= 0) return;
        // Only the first read waits: once something has arrived, whatever else
        // is queued behind it is taken without going back to the clock.
        waitMs = 0;

        const ssize_t received = ::recv(socket_, buffer, sizeof(buffer), 0);
        if (received <= 0) return;

        // Sharing 5353 means hearing every responder on the link, this
        // daemon's own included, so most of what arrives is about some other
        // service and parses to nothing.
        for (const DiscoveredService& service :
             parseServiceResponse(buffer, static_cast<size_t>(received), kNmosRegisterService)) {
            DiscoveredService& held = known_[service.instanceName];
            held.instanceName = service.instanceName;
            // Filled in rather than replaced: the SRV, the TXT and the A of
            // one service are not required to arrive together, and a packet
            // carrying only some of them must not blank out the rest.
            if (!service.hostName.empty()) held.hostName = service.hostName;
            if (service.port != 0) held.port = service.port;
            if (service.addressV4 != 0) held.addressV4 = service.addressV4;
            if (!service.txtEntries.empty()) held.txtEntries = service.txtEntries;
        }
    }
}

std::vector<NmosRegistry> RegistryBrowser::registries() const {
    std::vector<DiscoveredService> services;
    services.reserve(known_.size());
    for (const auto& [name, service] : known_) services.push_back(service);
    return registriesFrom(services, wantedVersion_);
}

}  // namespace AES67::Ravenna
