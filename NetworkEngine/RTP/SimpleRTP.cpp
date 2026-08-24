//
// SimpleRTP.cpp
// AES67 macOS Driver - Build #8
// Minimal RTP implementation for AES67
//

#include "SimpleRTP.h"
#include "../NetworkUtils.h"
#include "PCMCodec.h"
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace AES67 {
namespace RTP {

//
// RTPSocket Implementation
//

RTPSocket::RTPSocket()
    : sockfd_(-1)
    , isReceiver_(false)
{
    memset(&multicastAddr_, 0, sizeof(multicastAddr_));
    memset(&boundInterfaceAddr_, 0, sizeof(boundInterfaceAddr_));
}

RTPSocket::~RTPSocket() {
    close();
}

bool RTPSocket::openReceiver(const char* multicastIP, uint16_t port, const char* interfaceIP) {
    // Create UDP socket
    sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
        fprintf(stderr, "AES67 RTP openReceiver: socket() failed for %s:%u (errno=%d: %s)\n",
                multicastIP, port, errno, strerror(errno));
        return false;
    }

    // Allow multiple sockets to bind to same port (for multiple streams)
    // macOS/BSD requires both SO_REUSEADDR and SO_REUSEPORT for UDP port sharing
    int reuse = 1;
    if (setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        fprintf(stderr, "AES67 RTP openReceiver: SO_REUSEADDR failed for %s:%u (errno=%d: %s)\n",
                multicastIP, port, errno, strerror(errno));
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }
    if (setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        fprintf(stderr, "AES67 RTP openReceiver: SO_REUSEPORT failed for %s:%u (errno=%d: %s)\n",
                multicastIP, port, errno, strerror(errno));
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // Bind to port
    struct sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons(port);

    if (bind(sockfd_, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
        fprintf(stderr, "AES67 RTP openReceiver: bind() failed on port %u (errno=%d: %s)\n",
                port, errno, strerror(errno));
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // Resolve interface address for multicast binding
    struct in_addr ifaceAddr;
    if (interfaceIP) {
        ifaceAddr.s_addr = inet_addr(interfaceIP);
    } else {
        ifaceAddr.s_addr = htonl(INADDR_ANY);
    }

    // Store bound interface for proper IP_DROP_MEMBERSHIP on close()
    boundInterfaceAddr_ = ifaceAddr;

    // Bind multicast reception to a specific interface (prevents duplicate
    // packets on machines with multiple NICs, common in pro audio setups)
    if (interfaceIP) {
        if (setsockopt(sockfd_, IPPROTO_IP, IP_MULTICAST_IF, &ifaceAddr, sizeof(ifaceAddr)) < 0) {
            fprintf(stderr, "AES67 RTP openReceiver: IP_MULTICAST_IF failed for %s:%u iface=%s (errno=%d: %s)\n",
                    multicastIP, port, interfaceIP, errno, strerror(errno));
            ::close(sockfd_);
            sockfd_ = -1;
            return false;
        }
        fprintf(stderr, "AES67 RTP openReceiver: bound multicast to interface %s for %s:%u\n",
                interfaceIP, multicastIP, port);
    }

    // Join multicast group on the specified interface
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(multicastIP);
    mreq.imr_interface = ifaceAddr;

    if (setsockopt(sockfd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        fprintf(stderr, "AES67 RTP openReceiver: IP_ADD_MEMBERSHIP failed for %s:%u (errno=%d: %s)\n",
                multicastIP, port, errno, strerror(errno));
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // Set non-blocking mode
    int flags = fcntl(sockfd_, F_GETFL, 0);
    fcntl(sockfd_, F_SETFL, flags | O_NONBLOCK);

    // Add receive timeout (100ms) to prevent indefinite blocking
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms
    if (setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        // Log warning but continue - non-critical
        // Timeout is a safeguard; non-blocking mode is primary mechanism
    }

    // Increase receive buffer size (4 MB for high channel counts)
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(sockfd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    isReceiver_ = true;

    // Store multicast address for reference
    multicastAddr_.sin_family = AF_INET;
    multicastAddr_.sin_addr.s_addr = inet_addr(multicastIP);
    multicastAddr_.sin_port = htons(port);

    return true;
}

bool RTPSocket::openTransmitter(const char* multicastIP, uint16_t port, const char* interfaceIP,
                                 uint16_t sourcePort, int dscp) {
    // Create UDP socket
    sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
        fprintf(stderr, "AES67 RTP openTransmitter: socket() failed for %s:%u (errno=%d: %s)\n",
                multicastIP, port, errno, strerror(errno));
        return false;
    }

    // Bind to a specific local port when the caller needs one (Dolby Atmos
    // Connect identifies flows by source port, not destination address —
    // see CompatibilityProfile::useFixedMulticastWithPerFlowSourcePort).
    // Left unbound (kernel-assigned ephemeral source port) otherwise, this
    // driver's behavior before that existed. SO_REUSEADDR so a lingering
    // socket from a just-stopped stream doesn't block the rebind.
    if (sourcePort != 0) {
        int reuse = 1;
        setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in localAddr;
        memset(&localAddr, 0, sizeof(localAddr));
        localAddr.sin_family = AF_INET;
        localAddr.sin_addr.s_addr = INADDR_ANY;
        localAddr.sin_port = htons(sourcePort);
        if (bind(sockfd_, reinterpret_cast<struct sockaddr*>(&localAddr), sizeof(localAddr)) < 0) {
            fprintf(stderr, "AES67 RTP openTransmitter: bind() to source port %u failed for %s:%u (errno=%d: %s)\n",
                    sourcePort, multicastIP, port, errno, strerror(errno));
            ::close(sockfd_);
            sockfd_ = -1;
            return false;
        }
    }

    // Set multicast TTL
    uint8_t ttl = 32;
    if (setsockopt(sockfd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        fprintf(stderr, "AES67 RTP openTransmitter: IP_MULTICAST_TTL failed for %s:%u (errno=%d: %s)\n",
                multicastIP, port, errno, strerror(errno));
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // Set multicast interface
    if (interfaceIP) {
        struct in_addr ifaddr;
        ifaddr.s_addr = inet_addr(interfaceIP);
        if (setsockopt(sockfd_, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr)) < 0) {
            fprintf(stderr, "AES67 RTP openTransmitter: IP_MULTICAST_IF failed for %s:%u iface=%s (errno=%d: %s)\n",
                    multicastIP, port, interfaceIP, errno, strerror(errno));
            ::close(sockfd_);
            sockfd_ = -1;
            return false;
        }
    }

    // Increase send buffer size
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(sockfd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    // Mark the traffic if the active profile documents a codepoint. Not
    // fatal on failure: a stream that plays unmarked is better than no
    // stream, and the switch may well be classifying by port anyway.
    if (dscp >= 0) {
        if (!NetworkUtils::setQoSTrafficClass(sockfd_, dscp)) {
            fprintf(stderr, "AES67 RTP openTransmitter: DSCP %d not applied for %s:%u "
                            "— continuing unmarked\n", dscp, multicastIP, port);
        }
    }

    isReceiver_ = false;

    // Store destination address
    multicastAddr_.sin_family = AF_INET;
    multicastAddr_.sin_addr.s_addr = inet_addr(multicastIP);
    multicastAddr_.sin_port = htons(port);

    return true;
}

ssize_t RTPSocket::send(const RTPPacket& packet) {
    if (sockfd_ < 0 || isReceiver_) {
        return -1;
    }

    // Prepare header (convert to network byte order)
    RTPHeader header = packet.header;
    header.toNetworkOrder();

    // Send header + payload
    struct iovec iov[2];
    iov[0].iov_base = (void*)&header;
    iov[0].iov_len = sizeof(header);
    iov[1].iov_base = packet.payload;
    iov[1].iov_len = packet.payloadSize;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &multicastAddr_;
    msg.msg_namelen = sizeof(multicastAddr_);
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    return sendmsg(sockfd_, &msg, 0);
}

ssize_t RTPSocket::receive(RTPPacket& packet, uint8_t* buffer, size_t bufferSize) {
    if (sockfd_ < 0 || !isReceiver_) {
        return -1;
    }

    // Receive into buffer
    ssize_t bytesReceived = recvfrom(sockfd_, buffer, bufferSize, 0, nullptr, nullptr);

    // Handle receive errors and conditions
    if (bytesReceived < 0) {
        // EAGAIN/EWOULDBLOCK means no data available (not an error in non-blocking mode)
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; // No data available, caller should retry
        }

        // Actual error occurred (connection lost, socket closed, etc.)
        // Possible errors: EBADF, ECONNREFUSED, EFAULT, EINTR, EINVAL, ENOMEM, ENOTCONN, ENOTSOCK
        return -1;
    }

    // Check for valid RTP packet size
    if (bytesReceived < (ssize_t)sizeof(RTPHeader)) {
        // Packet too small to contain RTP header
        return -1;
    }

    // Parse header
    memcpy(&packet.header, buffer, sizeof(RTPHeader));
    packet.header.toHostOrder();

    // Set payload pointer and size
    packet.payload = buffer + sizeof(RTPHeader);
    packet.payloadSize = bytesReceived - sizeof(RTPHeader);

    return bytesReceived;
}

void RTPSocket::close() {
    if (sockfd_ >= 0) {
        // Leave multicast group if receiver
        // Use the same interface address that was used for IP_ADD_MEMBERSHIP
        if (isReceiver_) {
            struct ip_mreq mreq;
            mreq.imr_multiaddr = multicastAddr_.sin_addr;
            mreq.imr_interface = boundInterfaceAddr_;
            if (setsockopt(sockfd_, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
                // Log but don't fail — socket is closing anyway.
                // Repeated failures here could indicate multicast membership leak on macOS.
                fprintf(stderr, "AES67 RTP: IP_DROP_MEMBERSHIP failed (errno=%d: %s)\n",
                        errno, strerror(errno));
            }
        }

        ::close(sockfd_);
        sockfd_ = -1;
    }
}

bool RTPSocket::isValid() const {
    if (sockfd_ < 0) {
        return false;
    }

    // Use getsockopt with SO_ERROR to check if socket is in error state
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        return false; // getsockopt itself failed
    }

    // If error is non-zero, socket is in error state
    return error == 0;
}

//
// L16Codec Implementation
//

// These thin wrappers delegate to the free functions in PCMCodec, which are
// the single implementation of the float<->PCM arithmetic (and the one with
// the vDSP/Accelerate path and its own unit test, TestPCMCodec). The class
// API is kept because tests use it; the duplicated per-sample loops that used
// to live here are gone. numBytes here is a byte count, so convert to a
// sample count for the free functions' totalSamples argument.

void L16Codec::encode(const float* samples, size_t numSamples, uint8_t* output) {
    encodeL16BE(samples, numSamples, output);
}

void L16Codec::decode(const uint8_t* input, size_t numBytes, float* samples) {
    decodeL16BE(input, numBytes / 2, samples);
}

//
// L24Codec Implementation
//

void L24Codec::encode(const float* samples, size_t numSamples, uint8_t* output) {
    encodeL24BE(samples, numSamples, output);
}

void L24Codec::decode(const uint8_t* input, size_t numBytes, float* samples) {
    decodeL24BE(input, numBytes / 3, samples);
}

} // namespace RTP
} // namespace AES67
