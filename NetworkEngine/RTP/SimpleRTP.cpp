//
// SimpleRTP.cpp
// AES67 macOS Driver - Build #8
// Minimal RTP implementation for AES67
//

#include "SimpleRTP.h"
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
}

RTPSocket::~RTPSocket() {
    close();
}

bool RTPSocket::openReceiver(const char* multicastIP, uint16_t port, const char* interfaceIP) {
    // Create UDP socket
    sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
        return false;
    }

    // Allow multiple sockets to bind to same port (for multiple streams)
    int reuse = 1;
    if (setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
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
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // Join multicast group
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(multicastIP);
    mreq.imr_interface.s_addr = interfaceIP ? inet_addr(interfaceIP) : htonl(INADDR_ANY);

    if (setsockopt(sockfd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
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

bool RTPSocket::openTransmitter(const char* multicastIP, uint16_t port, const char* interfaceIP) {
    // Create UDP socket
    sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
        return false;
    }

    // Set multicast TTL
    uint8_t ttl = 32;
    if (setsockopt(sockfd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // Set multicast interface
    if (interfaceIP) {
        struct in_addr ifaddr;
        ifaddr.s_addr = inet_addr(interfaceIP);
        if (setsockopt(sockfd_, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr)) < 0) {
            ::close(sockfd_);
            sockfd_ = -1;
            return false;
        }
    }

    // Increase send buffer size
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(sockfd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

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
        if (isReceiver_) {
            struct ip_mreq mreq;
            mreq.imr_multiaddr = multicastAddr_.sin_addr;
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
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

void L16Codec::encode(const float* samples, size_t numSamples, uint8_t* output) {
    for (size_t i = 0; i < numSamples; i++) {
        // Clamp float to [-1.0, 1.0]
        float sample = std::max(-1.0f, std::min(1.0f, samples[i]));

        // Convert to 16-bit signed integer
        int16_t pcm = static_cast<int16_t>(sample * 32767.0f);

        // Store as big-endian (network byte order)
        output[i * 2 + 0] = (pcm >> 8) & 0xFF;  // MSB
        output[i * 2 + 1] = pcm & 0xFF;         // LSB
    }
}

void L16Codec::decode(const uint8_t* input, size_t numBytes, float* samples) {
    size_t numSamples = numBytes / 2;

    for (size_t i = 0; i < numSamples; i++) {
        // Read big-endian 16-bit value
        int16_t pcm = (input[i * 2 + 0] << 8) | input[i * 2 + 1];

        // Convert to float [-1.0, 1.0]
        samples[i] = pcm / 32768.0f;
    }
}

//
// L24Codec Implementation
//

void L24Codec::encode(const float* samples, size_t numSamples, uint8_t* output) {
    for (size_t i = 0; i < numSamples; i++) {
        // Clamp float to [-1.0, 1.0]
        float sample = std::max(-1.0f, std::min(1.0f, samples[i]));

        // Convert to 24-bit signed integer
        int32_t pcm = static_cast<int32_t>(sample * 8388607.0f); // 2^23 - 1

        // Store as big-endian 24-bit (network byte order)
        output[i * 3 + 0] = (pcm >> 16) & 0xFF;  // MSB
        output[i * 3 + 1] = (pcm >> 8) & 0xFF;   // Middle byte
        output[i * 3 + 2] = pcm & 0xFF;          // LSB
    }
}

void L24Codec::decode(const uint8_t* input, size_t numBytes, float* samples) {
    size_t numSamples = numBytes / 3;

    for (size_t i = 0; i < numSamples; i++) {
        // Read big-endian 24-bit value
        int32_t pcm = (input[i * 3 + 0] << 16) |
                      (input[i * 3 + 1] << 8) |
                      input[i * 3 + 2];

        // Sign extend from 24-bit to 32-bit
        if (pcm & 0x800000) {
            pcm |= 0xFF000000;
        }

        // Convert to float [-1.0, 1.0]
        samples[i] = pcm / 8388608.0f;  // 2^23
    }
}

} // namespace RTP
} // namespace AES67
