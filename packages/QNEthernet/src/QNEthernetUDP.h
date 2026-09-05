// SPDX-FileCopyrightText: (c) 2021-2022 Shawn Silverman <shawn@pobox.com>
// SPDX-License-Identifier: MIT

// QNEthernetUDP.h defines the UDP interface.
// This file is part of the QNEthernet library.

#ifndef QNE_ETHERNETUDP_H_
#define QNE_ETHERNETUDP_H_

// C++ includes
#include <cstdint>
#include <ctime>
#include <vector>

#include <IPAddress.h>
#include <Udp.h>

#include "lwip/ip_addr.h"
#include "lwip/opt.h"
#include "lwip/udp.h"

namespace qindesign {
namespace network {

class EthernetUDP : public UDP {
 public:
  EthernetUDP();

  // Creates a new UDP socket with the given receive queue size. It will be set
  // to a minimum of 1.
  explicit EthernetUDP(size_t queueSize);

  // Virtual, so that deleting a socket through this type is defined even
  // where the compiler cannot see that the static and dynamic types are
  // the same. Arduino's UDP has no virtual destructor, which is what
  // -Wdelete-non-virtual-dtor was pointing at.
  virtual ~EthernetUDP();

  // Returns the maximum number of UDP sockets.
  static constexpr int maxSockets() {
    return MEMP_NUM_UDP_PCB;
  }

  // Starts listening on a port. This returns true if successful and false if
  // the port is in use. This calls begin(localPort, false).
  uint8_t begin(uint16_t localPort) final;

  // Starts listening on a port and sets the SO_REUSEADDR socket option
  // according to the `reuse` parameter. This returns whether the attempt
  // was successful.
  uint8_t begin(uint16_t localPort, bool reuse);

  // Multicast functions make use of Ethernet.joinGroup()
  uint8_t beginMulticast(IPAddress ip, uint16_t port) final;
  uint8_t beginMulticast(IPAddress ip, uint16_t port, bool reuse);

  // Returns the port to which this socket is bound, or zero if it is not bound.
  uint16_t localPort();

  // Sets the TTL of outgoing multicast datagrams. lwIP starts every socket
  // at UDP_TTL, so multicast leaves the segment it was meant for unless
  // this is called; protocols that define their own scope, PTP among
  // them, need to say so.
  //
  // The value is remembered and applied to the underlying PCB whenever one
  // exists, including a PCB created later by begin() or by a send.
  void setMulticastTTL(uint8_t ttl);

  // The TTL outgoing multicast datagrams are sent with.
  uint8_t multicastTTL() const {
    return mcastTTL_;
  }

  // Sets the DSCP of outgoing datagrams: the top six bits of the IPv4 TOS
  // octet, given here as the DSCP value itself (0-63), not pre-shifted.
  // lwIP starts every socket unmarked, so traffic a switch is meant to
  // treat differently -- PTP among it, which the AES67 and RAVENNA guides
  // ask to be marked -- leaves looking like everything else.
  //
  // The value is remembered and applied to the underlying PCB whenever one
  // exists, including a PCB created later by begin() or by a send.
  void setOutgoingDiffServ(uint8_t dscp);

  // The DSCP outgoing datagrams are sent with.
  uint8_t outgoingDiffServ() const {
    return dscp_;
  }

  void stop() final;

  // Sending UDP packets
  int beginPacket(IPAddress ip, uint16_t port) final;
  int beginPacket(const char *host, uint16_t port) final;
  int endPacket() final;

  // Sends a UDP packet and returns whether the attempt was successful. This
  // combines the functions of beginPacket(), write(), and endPacket(), and
  // causes less overhead.
  bool send(const IPAddress &ip, uint16_t port,
            const uint8_t *data, size_t len);

  // Calls the other send() function after performing a DNS lookup.
  bool send(const char *host, uint16_t port, const uint8_t *data, size_t len);

  // Bring Print::write functions into scope
  using Print::write;

  size_t write(uint8_t b) final;
  size_t write(const uint8_t *buffer, size_t size) final;
  int availableForWrite() final;

  // Receiving UDP packets
  int parsePacket() final;
  int available() final;
  int read() final;

  // A NULL buffer allows the caller to skip bytes without having to read into
  // a buffer.
  int read(unsigned char *buffer, size_t len) final;

  // A NULL buffer allows the caller to skip bytes without having to read into
  // a buffer.
  int read(char *buffer, size_t len) final;

  int peek() final;
  void flush() final;

  // Returns the total size of the received packet data. This is only valid if a
  // packet has been received with parsePacket().
  size_t size() const;

  // Returns a pointer to the received packet data. This is only valid if a
  // packet has been received with parsePacket().
  const unsigned char *data() const;

  IPAddress remoteIP() final;
  uint16_t remotePort() final;

  // Returns whether the socket is listening.
  explicit operator bool() const;

  // Gets the IEEE 1588 timestamp for the received packet and assigns it to the
  // `timestamp` parameter, if available. This returns whether the received
  // packet has a timestamp.
  bool timestamp(timespec &timestamp) const;

 private:
  struct Packet {
    std::vector<unsigned char> data;
    ip_addr_t addr = *IP_ANY_TYPE;
    volatile uint16_t port = 0;
    volatile bool hasTimestamp = false;
    timespec timestamp{0, 0};
  };

  static void recvFunc(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                       const ip_addr_t *addr, u16_t port);

  // ip_addr_t versions of transmission functions
  bool beginPacket(const ip_addr_t *ipaddr, uint16_t port);
  bool send(const ip_addr_t *ipaddr, uint16_t port,
            const uint8_t *data, size_t len);

  // Checks if there's data still available in the packet.
  bool isAvailable() const;

  // Applies mcastTTL_ to pcb_, if there is one.
  void applyMulticastTTL();

  // Applies dscp_ to pcb_, if there is one.
  void applyDiffServ();

  udp_pcb *pcb_;
  uint8_t mcastTTL_ = UDP_TTL;
  uint8_t dscp_ = 0;

  // Received packet; updated every time one is received
  std::vector<Packet> inBuf_;  // Holds received packets
  size_t inBufTail_ = 0;
  size_t inBufHead_ = 0;
  size_t inBufSize_ = 0;

  // Packet being processed by the caller
  Packet packet_;  // Holds the packet being read
  int packetPos_;  // -1 if not currently reading a packet

  // Outgoing packets
  Packet out_;
  bool hasOutPacket_;
};

}  // namespace network
}  // namespace qindesign

#endif  // QNE_ETHERNETUDP_H_
