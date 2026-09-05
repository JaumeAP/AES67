//
// SimpleRTP.h
// AES67 macOS Driver - Build #8
// Minimal RTP implementation for AES67 (RFC 3550)
//

#pragma once

#include <cstdint>

// The wire format lives in its own header, free of sockets, so that consumers
// that only need to build or read an RTP header do not have to take the
// transport with it. See RTPHeader.h.
#include "NetworkEngine/RTP/RTPHeader.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace AES67 {
namespace RTP {

//
// RTP Packet
//
struct RTPPacket {
    RTPHeader header;
    uint8_t* payload;
    size_t payloadSize;

    RTPPacket() : payload(nullptr), payloadSize(0) {
        header.version = 2;
        header.padding = 0;
        header.extension = 0;
        header.cc = 0;
        header.marker = 0;
        header.payloadType = PT_AES67_L16;
        header.sequenceNumber = 0;
        header.timestamp = 0;
        header.ssrc = 0;
    }
};

//
// RTP Socket - Simple UDP multicast wrapper
//
class RTPSocket {
public:
    RTPSocket();
    ~RTPSocket();

    // Receiver setup
    bool openReceiver(const char* multicastIP, uint16_t port, const char* interfaceIP = nullptr);

    // Transmitter setup. sourcePort binds the socket to that specific local
    // (source) UDP port before sending — 0 (default) leaves it kernel-
    // assigned/ephemeral, this driver's behavior before CompatibilityProfile
    // ::useFixedMulticastWithPerFlowSourcePort existed. Nonzero is for gear
    // (the Dolby DMA profile) that identifies flows by source port rather
    // than by destination address.
    //
    // dscp marks outgoing packets with that DSCP codepoint (46 = EF, the
    // value most AES67 gear expects for audio), via
    // NetworkUtils::setQoSTrafficClass(). -1 (default) leaves the socket
    // unmarked — this driver's behavior before the profiles' documented
    // DSCP values were actually applied. Only transmitters mark: a
    // receiver sends no audio to prioritise.
    bool openTransmitter(const char* multicastIP, uint16_t port, const char* interfaceIP = nullptr,
                          uint16_t sourcePort = 0, int dscp = -1);

    // Send RTP packet
    ssize_t send(const RTPPacket& packet);

    // Receive RTP packet
    ssize_t receive(RTPPacket& packet, uint8_t* buffer, size_t bufferSize);

    // Split a received datagram into header and payload, RFC 3550 §5.1:
    // the fixed 12 bytes, then the CSRC list, then the extension header if
    // the X bit is set, and trailing padding removed if the P bit is. Static
    // and separate from receive() so it can be exercised without a socket —
    // this is the part that decides which bytes reach the decoder, and it
    // used to assume the fixed header was the whole of it (2026-09-04 audit).
    //
    // `packet.header` is filled in host byte order and `packet.payload`
    // points into `buffer`. Returns false for anything malformed: too short
    // for the header it declares, or padding that does not fit.
    static bool parseFrame(const uint8_t* buffer, size_t length, RTPPacket& packet);

    // Close socket
    void close();

    // Re-issue IP_ADD_MEMBERSHIP for a receiver socket, so a multicast
    // membership dropped by a network-interface flap (cable unplug/replug) is
    // re-established. Harmless (EADDRINUSE) while still joined; no-op for a
    // transmitter or a closed socket.
    void rejoinMulticast();

    // Socket status checks
    bool isOpen() const { return sockfd_ >= 0; }
    bool isValid() const;

    // Get socket file descriptor for select/poll
    int getFd() const { return sockfd_; }

private:
    int sockfd_;
    struct sockaddr_in multicastAddr_;
    bool isReceiver_;
    struct in_addr boundInterfaceAddr_;  // Interface used for multicast join (for proper leave)
    struct in_addr multicastGroupAddr_{};  // Group joined, for rejoinMulticast()
};

//
// L16 Audio Encoder/Decoder (16-bit PCM, network byte order)
//
class L16Codec {
public:
    // Encode float samples to L16 (big-endian 16-bit PCM)
    static void encode(const float* samples, size_t numSamples, uint8_t* output);

    // Decode L16 to float samples
    static void decode(const uint8_t* input, size_t numBytes, float* samples);
};

//
// L24 Audio Encoder/Decoder (24-bit PCM, network byte order)
//
class L24Codec {
public:
    // Encode float samples to L24 (big-endian 24-bit PCM)
    static void encode(const float* samples, size_t numSamples, uint8_t* output);

    // Decode L24 to float samples
    static void decode(const uint8_t* input, size_t numBytes, float* samples);
};

} // namespace RTP
} // namespace AES67
