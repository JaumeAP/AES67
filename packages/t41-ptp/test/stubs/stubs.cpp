#include "Arduino.h"
#include "QNEthernet.h"
#include "TimeLib.h"
#include "stub_state.h"

#include <cstdio>

SerialStub Serial;

int SerialStub::printf(const char *, ...)
{
    return 0;
}

uint32_t ENET_ATINC = 0;
uint32_t ENET_ATPER = 0;
uint32_t ENET_ATCOR = 0;

namespace ptptest
{

void StubState::reset()
{
    const uint8_t defaultMac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
    for (int i = 0; i < 6; i++)
    {
        mac[i] = defaultMac[i];
    }
    macAddressReads = 0;
    txAvailable = false;
    txPending = false;
    txTimestamp = {0, 0};
    txTimestampReads = 0;
    timestampNextFrameCalls = 0;
    adjustFreqCalls.clear();
    offsetTimerCalls.clear();
    writeTimerCalls.clear();
    microsNow = 0;
    microsStep = 100;
    millisNow = 0;
    randomValue = 0;
    randomBounds.clear();

    macFilters.clear();
    frameRx.clear();
    frameTx.clear();
    frameReadLens.clear();
    endFrameResult = true;
    frameWriteLimit = -1;
    currentFrame = Datagram{};
    frameUnderConstruction = SentFrame{};

    udpRx.clear();
    udpTx.clear();
    udpReadLens.clear();
    udpBound.clear();
    udpStopped.clear();
    udpTTLs.clear();
    udpDscps.clear();
    hardwareTime = timespec{0, 0};
    tcpRequests.clear();
    tcpConnects.clear();
    tcpConnectResult = true;
    tcpResponse = "HTTP/1.1 201 Created\r\n\r\n";
    udpSendResult = true;
    udpBindResult = true;
}

StubState &state()
{
    static StubState s;
    return s;
}

std::string endpointKey(const uint8_t ip[4], uint16_t port)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3],
                  (unsigned)port);
    return std::string(buf);
}

}  // namespace ptptest

unsigned long micros()
{
    ptptest::StubState &s = ptptest::state();
    const unsigned long now = s.microsNow;
    s.microsNow += s.microsStep;
    return now;
}

unsigned long millis()
{
    return ptptest::state().millisNow;
}

uint32_t random(uint32_t howbig)
{
    ptptest::state().randomBounds.push_back(howbig);
    if (howbig == 0)
    {
        return 0;
    }
    return ptptest::state().randomValue % howbig;
}

void breakTime(time_t, tmElements_t &tm)
{
    tm = tmElements_t{};
}

namespace qindesign
{
namespace network
{

EthernetClass Ethernet;
EthernetIEEE1588Class EthernetIEEE1588;
EthernetFrameClass EthernetFrame;

static std::string keyOf(const IPAddress &ip, uint16_t port)
{
    const uint8_t octets[4] = {ip[0], ip[1], ip[2], ip[3]};
    return ptptest::endpointKey(octets, port);
}

void EthernetClass::macAddress(uint8_t *mac)
{
    ptptest::state().macAddressReads++;
    for (int i = 0; i < 6; i++)
    {
        mac[i] = ptptest::state().mac[i];
    }
}

void EthernetClass::setMACAddressAllowed(const uint8_t *mac, bool allowed)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5]);
    ptptest::state().macFilters.push_back({std::string(buf), allowed});
}

bool EthernetIEEE1588Class::writeTimer(const timespec &tm) const
{
    ptptest::state().writeTimerCalls.push_back(tm);
    return true;
}

bool EthernetIEEE1588Class::readTimer(timespec &tm) const
{
    // What the hardware timer says. A test that drives a media clock
    // needs to move it, so it lives in the state rather than being a
    // constant zero.
    tm = ptptest::state().hardwareTime;
    return true;
}

void EthernetIEEE1588Class::timestampNextFrame() const
{
    ptptest::StubState &s = ptptest::state();
    s.timestampNextFrameCalls++;
    // The frame goes out and the hardware posts its timestamp, unless
    // the test says it never will.
    if (s.txAvailable)
    {
        s.txPending = true;
    }
}

bool EthernetIEEE1588Class::readAndClearTxTimestamp(timespec &tm) const
{
    ptptest::StubState &s = ptptest::state();
    s.txTimestampReads++;
    if (!s.txPending)
    {
        return false;
    }
    tm = s.txTimestamp;
    s.txPending = false;    // the hardware clears it on read
    s.txAvailable = false;  // and one value is posted per setTxTimestamp
    return true;
}

bool EthernetIEEE1588Class::adjustFreq(double nsps) const
{
    ptptest::state().adjustFreqCalls.push_back(nsps);
    return true;
}

bool EthernetIEEE1588Class::offsetTimer(int64_t ns) const
{
    ptptest::state().offsetTimerCalls.push_back(ns);
    return true;
}

int EthernetFrameClass::parseFrame()
{
    ptptest::StubState &s = ptptest::state();
    if (s.frameRx.empty())
    {
        return 0;
    }
    s.currentFrame = s.frameRx.front();
    s.frameRx.pop_front();
    return (int)s.currentFrame.data.size();
}

int EthernetFrameClass::read(unsigned char *buf, size_t len)
{
    ptptest::StubState &s = ptptest::state();
    s.frameReadLens.push_back(len);
    const size_t n = len < s.currentFrame.data.size() ? len : s.currentFrame.data.size();
    for (size_t i = 0; i < n; i++)
    {
        buf[i] = s.currentFrame.data[i];
    }
    return (int)n;
}

bool EthernetFrameClass::timestamp(timespec &tm) const
{
    tm = ptptest::state().currentFrame.timestamp;
    return true;
}

void EthernetFrameClass::beginFrame(const uint8_t dstAddr[6], const uint8_t srcAddr[6],
                                    uint16_t typeOrLen)
{
    ptptest::SentFrame &f = ptptest::state().frameUnderConstruction;
    f = ptptest::SentFrame{};
    for (int i = 0; i < 6; i++)
    {
        f.dst[i] = dstAddr[i];
        f.src[i] = srcAddr[i];
    }
    f.type = typeOrLen;
}

size_t EthernetFrameClass::write(const uint8_t *buf, size_t len)
{
    ptptest::StubState &s = ptptest::state();
    size_t accepted = len;
    if (s.frameWriteLimit >= 0)
    {
        const size_t room = (size_t)s.frameWriteLimit > s.frameUnderConstruction.payload.size()
                                ? (size_t)s.frameWriteLimit - s.frameUnderConstruction.payload.size()
                                : 0;
        accepted = len < room ? len : room;
    }
    s.frameUnderConstruction.payload.insert(s.frameUnderConstruction.payload.end(), buf,
                                            buf + accepted);
    return accepted;
}

bool EthernetFrameClass::endFrame()
{
    ptptest::StubState &s = ptptest::state();
    // QNEthernet refuses a frame outside 60 to maxFrameLen-4 bytes, the
    // payload counted with the fourteen bytes of header. A stub that took
    // anything would let a message the hardware would never send through
    // a test.
    const size_t length = s.frameUnderConstruction.payload.size() + 14;
    const bool acceptable = length >= 60 && length <= (size_t)maxFrameLen() - 4;
    const bool sent = s.endFrameResult && acceptable;
    if (sent)
    {
        s.frameTx.push_back(s.frameUnderConstruction);
    }
    s.frameUnderConstruction = ptptest::SentFrame{};
    return sent;
}

uint8_t EthernetUDP::beginMulticast(IPAddress ip, uint16_t port, bool)
{
    key = keyOf(ip, port);
    ptptest::state().udpBound.push_back(key);
    return ptptest::state().udpBindResult ? 1 : 0;
}

void EthernetUDP::setMulticastTTL(uint8_t ttl)
{
    mcastTTL = ttl;
    ptptest::state().udpTTLs.push_back({key, ttl});
}

void EthernetUDP::setOutgoingDiffServ(uint8_t value)
{
    dscp = value;
    ptptest::state().udpDscps.push_back({key, value});
}

void EthernetClient::setConnectionTimeout(uint16_t) {}

int EthernetClient::connect(IPAddress ip, uint16_t port)
{
    char text[32];
    snprintf(text, sizeof(text), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    ptptest::state().tcpConnects.push_back({std::string(text), port});
    if (!ptptest::state().tcpConnectResult) return 0;
    open = true;
    readPos = 0;
    ptptest::state().tcpRequests.push_back(std::string());
    return 1;
}

size_t EthernetClient::writeFully(const char *s, size_t size)
{
    if (!open || ptptest::state().tcpRequests.empty()) return 0;
    ptptest::state().tcpRequests.back().append(s, size);
    return size;
}

void EthernetClient::flush() {}

int EthernetClient::read()
{
    const std::string &answer = ptptest::state().tcpResponse;
    if (!open || readPos >= answer.size()) return -1;
    return static_cast<unsigned char>(answer[readPos++]);
}

bool EthernetClient::connected()
{
    return open && readPos < ptptest::state().tcpResponse.size();
}

void EthernetClient::stop() { open = false; }

void EthernetUDP::stop()
{
    ptptest::state().udpStopped.push_back(key);
    key.clear();
    packet.clear();
}

int EthernetUDP::parsePacket()
{
    ptptest::StubState &s = ptptest::state();
    std::deque<ptptest::Datagram> &queue = s.udpRx[key];
    if (queue.empty())
    {
        packet.clear();
        return 0;
    }
    packet = queue.front().data;
    packetTimestamp = queue.front().timestamp;
    queue.pop_front();
    return (int)packet.size();
}

int EthernetUDP::read(unsigned char *buf, size_t len)
{
    ptptest::state().udpReadLens.push_back(len);
    const size_t n = len < packet.size() ? len : packet.size();
    for (size_t i = 0; i < n; i++)
    {
        buf[i] = packet[i];
    }
    return (int)n;
}

bool EthernetUDP::timestamp(timespec &tm) const
{
    tm = packetTimestamp;
    return true;
}

bool EthernetUDP::send(const IPAddress &ip, uint16_t port, const uint8_t *data, size_t len)
{
    ptptest::StubState &s = ptptest::state();
    if (!s.udpSendResult)
    {
        return false;
    }
    s.udpTx.push_back(ptptest::SentDatagram{keyOf(ip, port),
                                            std::vector<uint8_t>(data, data + len)});
    return true;
}

}  // namespace network
}  // namespace qindesign
