#pragma once

#include <cstdint>
#include <ctime>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ptptest
{

// One frame or datagram, in either direction.
struct Datagram
{
    std::vector<uint8_t> data;
    timespec timestamp = {0, 0};
};

struct SentFrame
{
    uint8_t dst[6] = {0};
    uint8_t src[6] = {0};
    uint16_t type = 0;
    std::vector<uint8_t> payload;
};

struct SentDatagram
{
    std::string destination;  // "a.b.c.d:port"
    std::vector<uint8_t> data;
};

struct StubState
{
    // MAC address handed back by Ethernet.macAddress(). reset() puts the
    // library's clockID at FF FF DE AD BE EF 00 01.
    uint8_t mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};

    // How many times it was asked for: the layer 2 transport used to ask
    // once per frame sent.
    int macAddressReads = 0;

    // The transmit timestamp the hardware will post for the next frame it
    // is asked to stamp. txAvailable false is the timeout path: the frame
    // goes out and nothing is ever posted for it.
    bool txAvailable = false;
    // A posted timestamp nobody has read yet -- what a wait that gave up
    // too early leaves behind.
    bool txPending = false;
    timespec txTimestamp = {0, 0};
    int txTimestampReads = 0;
    int timestampNextFrameCalls = 0;

    // What the servo asked the hardware to do.
    std::vector<double> adjustFreqCalls;
    std::vector<int64_t> offsetTimerCalls;
    std::vector<timespec> writeTimerCalls;

    unsigned long microsNow = 0;
    unsigned long microsStep = 100;

    // millis() and random() are whatever the test says they are.
    unsigned long millisNow = 0;
    uint32_t randomValue = 0;
    std::vector<uint32_t> randomBounds;

    // Layer 2 transport.
    // Multicast MAC filters the library asked for: address and whether it
    // was being allowed or handed back.
    std::vector<std::pair<std::string, bool>> macFilters;

    std::deque<Datagram> frameRx;
    std::vector<SentFrame> frameTx;
    std::vector<size_t> frameReadLens;
    bool endFrameResult = true;
    // How many bytes a frame write accepts, -1 for all of them.
    int frameWriteLimit = -1;
    Datagram currentFrame;       // what parseFrame() last handed out
    SentFrame frameUnderConstruction;

    // Layer 3 transport, keyed by "a.b.c.d:port".
    std::map<std::string, std::deque<Datagram>> udpRx;
    std::vector<SentDatagram> udpTx;
    std::vector<size_t> udpReadLens;
    std::vector<std::string> udpBound;
    std::vector<std::string> udpStopped;
    // Endpoint and TTL of every socket that was given one.
    std::vector<std::pair<std::string, uint8_t>> udpTTLs;
    std::vector<std::pair<std::string, uint8_t>> udpDscps;

    // The TCP client the NMOS node uses: what it sent, where it tried to
    // go, and what the test says the far end answers.
    // What EthernetIEEE1588.readTimer() answers: the board's clock, as
    // the test sets it.
    timespec hardwareTime = {0, 0};

    std::vector<std::string> tcpRequests;
    std::vector<std::pair<std::string, uint16_t>> tcpConnects;
    bool tcpConnectResult = true;
    std::string tcpResponse = "HTTP/1.1 201 Created\r\n\r\n";
    bool udpSendResult = true;
    bool udpBindResult = true;

    void reset();
};

StubState &state();

// "a.b.c.d:port", the key both the stub and the tests use.
std::string endpointKey(const uint8_t ip[4], uint16_t port);

}  // namespace ptptest
