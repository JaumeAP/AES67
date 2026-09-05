#pragma once

#include "ptp-base.h"

class IPAddress;

namespace qindesign
{
    namespace network
    {
        class EthernetUDP;
    }
}

class l3PTP : public PTPBase
{
public:
    l3PTP(bool master_, bool slave_, bool p2p_);
    ~l3PTP() override;

    // The TTL outgoing PTP datagrams carry.
    //
    // lwIP starts a socket at 255, so PTP multicast used to be routable
    // off the segment it was meant for. One is what the default profile
    // asks for, and what the peer-delay address -- which is link local by
    // definition -- can ever need.
    //
    // Call before begin(): it is applied to the sockets as they open.
    void setMulticastTTL(uint8_t val);

    // The DSCP outgoing PTP datagrams carry, 0 to 63.
    //
    // Zero, the default, is what lwIP sends: unmarked, and therefore in
    // the same queue as everything else in a switch that has any. The
    // AES67 and RAVENNA guides mark PTP so it does not queue behind the
    // audio it is meant to time; which value depends on the network, so
    // this takes it rather than choosing one. EF is 46 and CS7, what
    // Dante uses for PTP, is 56.
    //
    // Applied to the sockets as they open and to any that are already
    // open, like the TTL above.
    void setDscp(uint8_t val);
    uint8_t getDscp() const { return dscp; }
private:
    void initSockets() override;
    void closeSockets() override;
    void updateSockets() override;
    void drainSocket(qindesign::network::EthernetUDP *socket);

    // Opens one socket on the group and port given, or counts the
    // failure and answers null.
    qindesign::network::EthernetUDP *openSocket(const IPAddress &group, uint16_t port);

    uint8_t multicastTTL = 1;
    uint8_t dscp = 0;
    void sendPTPMessage(const uint8_t *buf, int size, bool generalMessage,
                        bool peerAddress) override;
    
    // Null until initSockets() runs, and null again after closeSockets():
    // an uninitialised pointer here is a socket the port does not have.
    qindesign::network::EthernetUDP *eventSocket = nullptr;
    qindesign::network::EthernetUDP *generalSocket = nullptr;
    qindesign::network::EthernetUDP *pEventSocket = nullptr;
    qindesign::network::EthernetUDP *pGeneralSocket = nullptr;
};
