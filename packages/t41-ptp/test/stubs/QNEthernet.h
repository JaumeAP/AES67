#pragma once

// Host stub for the parts of QNEthernet the library calls. Every call is
// recorded in ptptest::state() so the tests can assert on what the
// library asked the hardware to do.

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "Arduino.h"

namespace qindesign
{
namespace network
{

class EthernetClass
{
public:
    void macAddress(uint8_t *mac);
    void setMACAddressAllowed(const uint8_t *mac, bool allowed);
};

class EthernetIEEE1588Class
{
public:
    bool writeTimer(const timespec &tm) const;
    bool readTimer(timespec &tm) const;
    void timestampNextFrame() const;
    bool readAndClearTxTimestamp(timespec &tm) const;
    bool adjustFreq(double nsps) const;
    bool offsetTimer(int64_t ns) const;
};

class EthernetFrameClass
{
public:
    static constexpr int minFrameLen() { return 64; }
    static constexpr int maxFrameLen() { return 1522; }

    int parseFrame();
    int read(unsigned char *buf, size_t len);
    bool timestamp(timespec &tm) const;

    void beginFrame(const uint8_t dstAddr[6], const uint8_t srcAddr[6], uint16_t typeOrLen);
    size_t write(const uint8_t *buf, size_t len);
    bool endFrame();
};

/// The TCP client the NMOS node talks HTTP over. It records what was
/// written and hands back whatever the test staged, which is the only way
/// to see what this board actually puts on the wire.
class EthernetClient
{
public:
    void setConnectionTimeout(uint16_t timeout);
    int connect(IPAddress ip, uint16_t port);
    size_t writeFully(const char *s, size_t size);
    void flush();
    int read();
    bool connected();
    void stop();

private:
    bool open = false;
    size_t readPos = 0;
};

class EthernetUDP
{
public:
    uint8_t beginMulticast(IPAddress ip, uint16_t port, bool reuse);
    void setMulticastTTL(uint8_t ttl);
    uint8_t multicastTTL() const { return mcastTTL; }
    void setOutgoingDiffServ(uint8_t dscp);
    uint8_t outgoingDiffServ() const { return dscp; }
    void stop();
    int parsePacket();
    int read(unsigned char *buf, size_t len);
    bool timestamp(timespec &tm) const;
    bool send(const IPAddress &ip, uint16_t port, const uint8_t *data, size_t len);

private:
    // Set at beginMulticast, used to pick this socket's inbound queue.
    std::string key;
    uint8_t mcastTTL = 255;
    uint8_t dscp = 0;
    std::vector<uint8_t> packet;
    timespec packetTimestamp = {0, 0};
};

extern EthernetClass Ethernet;
extern EthernetIEEE1588Class EthernetIEEE1588;
extern EthernetFrameClass EthernetFrame;

}  // namespace network
}  // namespace qindesign
