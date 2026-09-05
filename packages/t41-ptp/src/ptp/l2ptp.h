#pragma once

#include "ptp-base.h"

// The largest frame 802.3 allows with a VLAN tag, which is what
// QNEthernet declares as MAX_FRAME_LEN. Nothing larger is a frame this
// transport should be looking at.
constexpr int MAX_ETHERNET_FRAME_LEN = 1522;

// The most of a frame this transport ever reads: fourteen bytes of
// Ethernet header, four more when the frame carries a VLAN tag, and the
// sixty-four of the longest PTP message the parser looks at, which is an
// Announce.
//
// The buffer used to be the size of the largest frame on the wire, so a
// full-size frame was copied whole -- fifteen hundred bytes of memcpy per
// frame, on the busiest path in the loop -- for the eighty-two the parser
// would go on to read. It was also 1522 bytes of RAM standing in every
// l2PTP object, three quarters of the object.
constexpr int L2_RECV_BUF_LEN = 18 + 64;

// The majorSdoId 802.1AS uses. A port announcing it is speaking gPTP,
// which addresses every message to the peer-delay MAC rather than only
// the peer-delay ones.
constexpr uint8_t GPTP_SDO_ID = 1;

class l2PTP : public PTPBase
{
public:
    l2PTP(bool master_, bool slave_, bool p2p_);

    // The MAC filters this transport opens are global to the interface,
    // and only this object knows it asked for them. l3PTP has always
    // closed its sockets from its destructor; an l2PTP destroyed without
    // end() left both multicast addresses accepted for the life of the
    // sketch.
    ~l2PTP() override;
private:
    void initSockets() override;
    void closeSockets() override;
    void updateSockets() override;
    void sendPTPMessage(const uint8_t *buf, int size, bool generalMessage,
                        bool peerAddress) override;
    bool readFrame();

    // The interface's own MAC, read once when the port comes up.
    //
    // Every frame sent used to ask the driver for it again: the address
    // does not change between begin() and end(), and the clock identity
    // is already built from it in reset().
    uint8_t sourceMac[6] = {0};

    // Whether the filters are open right now, so that closing them twice
    // -- end() and then the destructor -- does not hand back an address
    // this object no longer holds, and a port never begun does not hand
    // back one it never asked for.
    bool filtersOpen = false;

    // Where a received frame is read to. It lived on the stack of
    // updateSockets(), which put fifteen hundred bytes there on every
    // call of the busiest function in the loop.
    uint8_t frameBuffer[L2_RECV_BUF_LEN];
};