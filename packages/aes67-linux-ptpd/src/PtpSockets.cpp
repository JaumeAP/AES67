#include "PtpSockets.h"

#include "PtpWire.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/errqueue.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace AES67::LinuxPtpd {
namespace {

/// How long to wait for the NIC to hand back a transmit timestamp. The stamp
/// is taken as the frame leaves and posted to the error queue right after, so
/// this is generous by two orders of magnitude; it exists so a driver that
/// never posts one cannot stall the send loop.
constexpr int kTxTimestampWaitMs = 20;

uint64_t toNanoseconds(const struct timespec& ts) {
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

/// The hardware stamp is ts[2]; ts[0] is the software one. A driver that took
/// no hardware stamp leaves ts[2] zero, and this returns the software stamp
/// rather than a zero that would read as 1970.
uint64_t timestampFrom(const struct scm_timestamping& stamping) {
    if (stamping.ts[2].tv_sec != 0 || stamping.ts[2].tv_nsec != 0) {
        return toNanoseconds(stamping.ts[2]);
    }
    return toNanoseconds(stamping.ts[0]);
}

}  // namespace

PtpSockets::~PtpSockets() { close(); }

void PtpSockets::close() {
    if (eventFd_ >= 0) ::close(eventFd_);
    if (generalFd_ >= 0) ::close(generalFd_);
    eventFd_ = -1;
    generalFd_ = -1;
}

bool PtpSockets::enableHardwareTimestamping(const std::string& interfaceName,
                                            std::string& error) {
    struct hwtstamp_config config {};
    config.tx_type = HWTSTAMP_TX_ON;
    config.rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;

    struct ifreq request {};
    std::strncpy(request.ifr_name, interfaceName.c_str(), IFNAMSIZ - 1);
    request.ifr_data = reinterpret_cast<char*>(&config);

    const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        error = std::string("socket() for SIOCSHWTSTAMP: ") + std::strerror(errno);
        return false;
    }
    const int result = ::ioctl(sock, SIOCSHWTSTAMP, &request);
    const int savedErrno = errno;
    ::close(sock);

    if (result < 0) {
        error = "SIOCSHWTSTAMP on " + interfaceName + ": " +
                std::strerror(savedErrno) +
                (savedErrno == EPERM ? " (needs CAP_NET_ADMIN)" : "");
        return false;
    }
    return true;
}

bool PtpSockets::openOne(uint16_t port, int& fd, std::string& error) {
    fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        error = std::string("socket(): ") + std::strerror(errno);
        return false;
    }

    int on = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        error = std::string("SO_REUSEADDR: ") + std::strerror(errno);
        return false;
    }

    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0) {
        error = "bind " + std::to_string(port) + ": " + std::strerror(errno) +
                (errno == EACCES ? " (ports below 1024 need privilege)" : "");
        return false;
    }

    struct ip_mreqn join {};
    ::inet_pton(AF_INET, kPtpPrimaryGroup, &join.imr_multiaddr);
    join.imr_address.s_addr = interfaceAddress_;
    join.imr_ifindex = static_cast<int>(interfaceIndex_);
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &join, sizeof(join)) < 0) {
        error = std::string("IP_ADD_MEMBERSHIP: ") + std::strerror(errno);
        return false;
    }

    // Send out of the interface we were given, not out of whichever one the
    // routing table prefers for a multicast address.
    if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &join, sizeof(join)) < 0) {
        error = std::string("IP_MULTICAST_IF: ") + std::strerror(errno);
        return false;
    }

    // IEEE 1588 Annex D: one hop. Nothing is gained by letting PTP leave the
    // subnet, and a stray master two networks away is a fault nobody sees.
    const int ttl = 1;
    if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        error = std::string("IP_MULTICAST_TTL: ") + std::strerror(errno);
        return false;
    }

    // Do not hear our own Announce and Sync back: the loop would treat them
    // as a foreign master.
    const int loop = 0;
    if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        error = std::string("IP_MULTICAST_LOOP: ") + std::strerror(errno);
        return false;
    }

    // DSCP 46 (Expedited Forwarding) on the event socket and 34 on the
    // general one, as AES67 sec 6.3 asks for. The field is the top six bits.
    const int dscp = (port == kEventPort ? 46 : 34) << 2;
    if (::setsockopt(fd, IPPROTO_IP, IP_TOS, &dscp, sizeof(dscp)) < 0) {
        error = std::string("IP_TOS: ") + std::strerror(errno);
        return false;
    }

    return true;
}

bool PtpSockets::open(const std::string& interfaceName,
                      bool allowSoftwareTimestamps, std::string& error) {
    interfaceIndex_ = ::if_nametoindex(interfaceName.c_str());
    if (interfaceIndex_ == 0) {
        error = "no interface called " + interfaceName;
        return false;
    }

    // The MAC is what the clock identity is made of, and the IPv4 address is
    // what the multicast joins are anchored to. Both come from the same walk.
    struct ifaddrs* addresses = nullptr;
    if (::getifaddrs(&addresses) != 0) {
        error = std::string("getifaddrs(): ") + std::strerror(errno);
        return false;
    }
    bool haveMac = false;
    bool haveAddress = false;
    for (struct ifaddrs* entry = addresses; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || interfaceName != entry->ifa_name) continue;
        if (entry->ifa_addr->sa_family == AF_PACKET) {
            const auto* link = reinterpret_cast<const struct sockaddr_ll*>(entry->ifa_addr);
            if (link->sll_halen == 6) {
                std::memcpy(mac_.data(), link->sll_addr, 6);
                haveMac = true;
            }
        } else if (entry->ifa_addr->sa_family == AF_INET) {
            const auto* inet = reinterpret_cast<const struct sockaddr_in*>(entry->ifa_addr);
            interfaceAddress_ = inet->sin_addr.s_addr;
            haveAddress = true;
        }
    }
    ::freeifaddrs(addresses);

    if (!haveMac) {
        error = interfaceName + " has no MAC address to build a clock identity from";
        return false;
    }
    if (!haveAddress) {
        error = interfaceName + " has no IPv4 address; PTP over UDP needs one";
        return false;
    }

    std::string timestampError;
    hardwareTimestamps_ = enableHardwareTimestamping(interfaceName, timestampError);
    if (!hardwareTimestamps_ && !allowSoftwareTimestamps) {
        error = timestampError;
        return false;
    }

    if (!openOne(kEventPort, eventFd_, error)) return false;
    if (!openOne(kGeneralPort, generalFd_, error)) return false;

    // Ask for both directions on the event socket. The general socket needs
    // none: its messages carry the time rather than being timed.
    int stampingFlags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_TX_SOFTWARE |
                        SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_OPT_TSONLY;
    if (hardwareTimestamps_) {
        stampingFlags |= SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_TX_HARDWARE |
                         SOF_TIMESTAMPING_RAW_HARDWARE;
    }
    if (::setsockopt(eventFd_, SOL_SOCKET, SO_TIMESTAMPING, &stampingFlags,
                     sizeof(stampingFlags)) < 0) {
        error = std::string("SO_TIMESTAMPING: ") + std::strerror(errno);
        return false;
    }

    return true;
}

bool PtpSockets::sendEvent(const uint8_t* data, size_t length, uint64_t& txTimeNs,
                           std::string& error) {
    txTimeNs = 0;
    if (eventFd_ < 0) {
        error = "event socket is not open";
        return false;
    }

    struct sockaddr_in destination {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(kEventPort);
    ::inet_pton(AF_INET, kPtpPrimaryGroup, &destination.sin_addr);

    if (::sendto(eventFd_, data, length, 0,
                 reinterpret_cast<struct sockaddr*>(&destination),
                 sizeof(destination)) < 0) {
        error = std::string("sendto(319): ") + std::strerror(errno);
        return false;
    }

    // The stamp comes back on the error queue, not on the socket.
    struct pollfd waiting {};
    waiting.fd = eventFd_;
    waiting.events = POLLERR;
    if (::poll(&waiting, 1, kTxTimestampWaitMs) <= 0) return true;

    uint8_t discard[kMaxMessageSize];
    uint8_t control[512];
    struct iovec vector {};
    vector.iov_base = discard;
    vector.iov_len = sizeof(discard);

    struct msghdr message {};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);

    if (::recvmsg(eventFd_, &message, MSG_ERRQUEUE) < 0) return true;

    for (struct cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SO_TIMESTAMPING) {
            struct scm_timestamping stamping {};
            std::memcpy(&stamping, CMSG_DATA(header), sizeof(stamping));
            txTimeNs = timestampFrom(stamping);
        }
    }
    return true;
}

bool PtpSockets::sendGeneral(const uint8_t* data, size_t length, std::string& error) {
    if (generalFd_ < 0) {
        error = "general socket is not open";
        return false;
    }

    struct sockaddr_in destination {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(kGeneralPort);
    ::inet_pton(AF_INET, kPtpPrimaryGroup, &destination.sin_addr);

    if (::sendto(generalFd_, data, length, 0,
                 reinterpret_cast<struct sockaddr*>(&destination),
                 sizeof(destination)) < 0) {
        error = std::string("sendto(320): ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool PtpSockets::receive(int fd, uint8_t* buffer, size_t capacity, size_t& length,
                         uint64_t& receiveTimeNs) {
    length = 0;
    receiveTimeNs = 0;
    if (fd < 0) return false;

    uint8_t control[512];
    struct iovec vector {};
    vector.iov_base = buffer;
    vector.iov_len = capacity;

    struct msghdr message {};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);

    const ssize_t received = ::recvmsg(fd, &message, MSG_DONTWAIT);
    if (received <= 0) return false;
    length = static_cast<size_t>(received);

    for (struct cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SO_TIMESTAMPING) {
            struct scm_timestamping stamping {};
            std::memcpy(&stamping, CMSG_DATA(header), sizeof(stamping));
            receiveTimeNs = timestampFrom(stamping);
        }
    }
    return true;
}

}  // namespace AES67::LinuxPtpd
