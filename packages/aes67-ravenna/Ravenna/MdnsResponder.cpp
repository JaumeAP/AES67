#include "Ravenna/MdnsResponder.h"

#include "Ravenna/DnsSd.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace AES67::Ravenna {
namespace {

constexpr size_t kMaxPacketBytes = 4096;

/// Lowercased, so a query for _RTSP._TCP.local matches: DNS names are case
/// insensitive and mDNS queries arrive in whatever case the asker used.
std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool asksAbout(const std::vector<std::string>& asked,
               const std::vector<SessionAdvertisement>& advertised) {
    for (const std::string& name : asked) {
        for (const SessionAdvertisement& service : advertised) {
            if (name == lowered(service.serviceType)) return true;
            if (!service.subtype.empty() && name == lowered(service.subtype)) return true;
        }
    }
    return false;
}

}  // namespace

MdnsResponder::~MdnsResponder() {
    if (socket_ >= 0) goodbye();
    stop();
}

void MdnsResponder::stop() {
    if (socket_ >= 0) ::close(socket_);
    socket_ = -1;
}

bool MdnsResponder::start(const std::string& interfaceName, const std::string& hostName,
                          uint32_t addressV4, uint16_t port, std::string& error) {
    const unsigned int index = ::if_nametoindex(interfaceName.c_str());
    if (index == 0) {
        error = "no interface called " + interfaceName;
        return false;
    }

    socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0) {
        error = std::string("socket(): ") + std::strerror(errno);
        return false;
    }

    int on = 1;
    ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    // mDNS is a port every responder on the machine binds at once: without
    // this, starting this alongside the system's own responder fails.
    ::setsockopt(socket_, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif

    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(kMdnsPort);
    if (::bind(socket_, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0) {
        error = std::string("bind 5353: ") + std::strerror(errno);
        stop();
        return false;
    }

    struct ip_mreq join {};
    ::inet_pton(AF_INET, kMdnsGroup, &join.imr_multiaddr);
    join.imr_interface.s_addr = htonl(addressV4);
    if (::setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &join, sizeof(join)) < 0) {
        error = std::string("IP_ADD_MEMBERSHIP on ") + kMdnsGroup + ": " +
                std::strerror(errno);
        stop();
        return false;
    }

    struct in_addr outgoing {};
    outgoing.s_addr = htonl(addressV4);
    ::setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_IF, &outgoing, sizeof(outgoing));

    // RFC 6762 sec 11: mDNS is link-local and its TTL is 255, which is also
    // how a receiver tells a local packet from a routed one.
    const int ttl = 255;
    ::setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    hostName_ = hostName;
    addressV4_ = addressV4;
    port_ = port;
    return true;
}

bool MdnsResponder::sendPacket(const std::vector<uint8_t>& packet) {
    if (socket_ < 0 || packet.empty()) return false;

    struct sockaddr_in destination {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(kMdnsPort);
    ::inet_pton(AF_INET, kMdnsGroup, &destination.sin_addr);

    return ::sendto(socket_, packet.data(), packet.size(), 0,
                    reinterpret_cast<struct sockaddr*>(&destination),
                    sizeof(destination)) >= 0;
}

void MdnsResponder::alsoAdvertise(const SessionAdvertisement& service) {
    extras_.push_back(service);
}

std::vector<SessionAdvertisement> MdnsResponder::everything() const {
    std::vector<SessionAdvertisement> all =
        catalogue_.advertisements(hostName_, port_, addressV4_);
    all.insert(all.end(), extras_.begin(), extras_.end());
    return all;
}

void MdnsResponder::announce() {
    for (const SessionAdvertisement& service : everything()) {
        sendPacket(buildAnnouncement(service));
    }
}

void MdnsResponder::goodbye() {
    for (const SessionAdvertisement& service : everything()) {
        sendPacket(buildGoodbye(service));
    }
}

size_t MdnsResponder::service() {
    if (socket_ < 0) return 0;

    size_t answered = 0;
    while (true) {
        struct pollfd waiting {};
        waiting.fd = socket_;
        waiting.events = POLLIN;
        if (::poll(&waiting, 1, 0) <= 0) break;

        uint8_t buffer[kMaxPacketBytes];
        const ssize_t received = ::recv(socket_, buffer, sizeof(buffer), 0);
        if (received <= 0) break;

        const std::vector<std::string> names =
            parseQueryNames(buffer, static_cast<size_t>(received));
        if (!asksAbout(names, everything())) continue;

        // Answered to the group rather than to the asker: everything else on
        // the link gets to fill its cache from one packet, which is the whole
        // point of mDNS being multicast.
        announce();
        ++answered;
    }
    return answered;
}

}  // namespace AES67::Ravenna
