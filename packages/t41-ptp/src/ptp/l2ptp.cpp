#include <QNEthernet.h>

#include "l2ptp.h"

// How many frames one call may take. Same reasoning as the layer 3
// transport: one per call leaves a burst queued, and no cap at all lets a
// flood hold update().
static constexpr int MAX_FRAMES_PER_UPDATE = 4;

using namespace qindesign::network;

// The two multicast addresses Annex F of 1588 gives PTP over 802.3, and
// the only ones this transport asks the interface to accept.
static const uint8_t kPtpPeerMac[6] = {0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e};
static const uint8_t kPtpDefaultMac[6] = {0x01, 0x1b, 0x19, 0x00, 0x00, 0x00};

static bool sameMac(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 6; i++)
    {
        if (a[i] != b[i])
        {
            return false;
        }
    }
    return true;
}

l2PTP::l2PTP(bool master_, bool slave_, bool p2p_):
PTPBase(master_,slave_,p2p_)
{

}

void l2PTP::initSockets()
{
    uint8_t mac[6];
    mac[0] = 0x01;
    mac[1] = 0x80;
    mac[2] = 0xc2;
    mac[3] = 0x00;
    mac[4] = 0x00;
    mac[5] = 0x0e;
    qindesign::network::Ethernet.setMACAddressAllowed(mac, true);
    mac[0] = 0x01;
    mac[1] = 0x1b;
    mac[2] = 0x19;
    mac[3] = 0x00;
    mac[4] = 0x00;
    mac[5] = 0x00;
    qindesign::network::Ethernet.setMACAddressAllowed(mac, true);
    filtersOpen = true;

    qindesign::network::Ethernet.macAddress(sourceMac);
}

l2PTP::~l2PTP()
{
    closeSockets();
}

// The two multicast MAC addresses opened in initSockets() are handed
// back: nothing else in the sketch asked for them.
void l2PTP::closeSockets()
{
    if (!filtersOpen)
    {
        return;
    }
    filtersOpen = false;
    uint8_t mac[6];
    mac[0] = 0x01;
    mac[1] = 0x80;
    mac[2] = 0xc2;
    mac[3] = 0x00;
    mac[4] = 0x00;
    mac[5] = 0x0e;
    qindesign::network::Ethernet.setMACAddressAllowed(mac, false);
    mac[0] = 0x01;
    mac[1] = 0x1b;
    mac[2] = 0x19;
    mac[3] = 0x00;
    mac[4] = 0x00;
    mac[5] = 0x00;
    qindesign::network::Ethernet.setMACAddressAllowed(mac, false);
}

// Takes one frame. Returns false when there was nothing to take.
bool l2PTP::readFrame()
{
    const int bufferSize = qindesign::network::EthernetFrame.parseFrame();
    if (bufferSize <= 0)
    {
        return false;
    }

    // A fixed buffer, not a variable-length one: the size of what went
    // on the stack used to be decided by whoever sent the frame.
    if (bufferSize > MAX_ETHERNET_FRAME_LEN)
    {
        return true;
    }

    // Only as much as the parser can read. The rest of the frame goes
    // nowhere -- parseFrame() has already taken it off the queue -- and
    // copying it was fifteen hundred bytes of work per frame for the
    // eighty-two that are looked at.
    //
    // The length the frame arrived with, not the length that was read, is
    // what says whether it is long enough to be a frame at all.
    if (bufferSize < qindesign::network::EthernetFrame.minFrameLen() - 4)
    {
        return true;
    }

    const int wanted = bufferSize < L2_RECV_BUF_LEN ? bufferSize : L2_RECV_BUF_LEN;
    const int frameSize =
        qindesign::network::EthernetFrame.read(frameBuffer, static_cast<size_t>(wanted));
    if (frameSize < 14)
    {
        return true;
    }

    // Addressed to PTP, and not merely carrying its EtherType. The two
    // multicast addresses this transport opens are not the only ones the
    // interface accepts -- its own unicast address and broadcast come as
    // well -- and nothing here looked at which of them a frame arrived
    // on, so a PTP message sent to this board alone was taken as one sent
    // to the group.
    if (!sameMac(frameBuffer, kPtpPeerMac) && !sameMac(frameBuffer, kPtpDefaultMac))
    {
        return true;
    }

    uint16_t type = static_cast<uint16_t>((uint16_t{frameBuffer[12]} << 8) | frameBuffer[13]);
    int payloadStart = 14;

    // A VLAN tag pushes the EtherType and the payload four bytes along.
    // Without this a tagged PTP frame -- which is how PTP normally rides
    // on a network that separates traffic at all -- read its EtherType
    // out of the tag and was dropped.
    if (type == 0x8100)
    {
        if (frameSize < 18)
        {
            return true;
        }

        type = static_cast<uint16_t>((uint16_t{frameBuffer[16]} << 8) | frameBuffer[17]);
        payloadStart = 18;
    }

    if(type != 0x88f7){
        return true;
    }

    timespec recv_ts;
    EthernetFrame.timestamp(recv_ts);

    parsePTPMessage(&frameBuffer[payloadStart], frameSize - payloadStart, recv_ts);
    return true;
}

void l2PTP::updateSockets()
{
    for (int i = 0; i < MAX_FRAMES_PER_UPDATE; i++)
    {
        if (!readFrame())
        {
            return;
        }
    }
}

// `generalMessage` is deliberately unused here: at L3 it picks the port,
// but at L2 both ports are one EtherType and there is nothing to pick.
// It stays in the signature because the two implementations share it.
//
// `peerAddress` is another matter. Annex F of 1588 gives peer-delay
// messages 01-80-C2-00-00-0E and everything else 01-1B-19-00-00-00, and
// this sent the lot to the peer-delay address -- which is in the range
// 802.1D reserves for link-local control traffic, so a bridge is
// entitled to drop it rather than forward it. A Sync addressed there
// reaches whatever shares the segment and nothing beyond, which is not
// what a two-way exchange across a switch needs. Both filters were
// already open on the way in; only the way out was wrong.
//
// 802.1AS is the exception, not the mistake: it puts every message on
// 01-80-C2-00-00-0E precisely because gPTP is hop by hop. It is the
// profile this port announces with majorSdoId 1, which is what picks it
// out here.
void l2PTP::sendPTPMessage(const uint8_t *buf, int size, bool /*generalMessage*/,
                           bool peerAddress)
{
    const uint8_t *dstmac =
        (peerAddress || getMajorSdoId() == GPTP_SDO_ID) ? kPtpPeerMac : kPtpDefaultMac;

    qindesign::network::EthernetFrame.beginFrame(dstmac,sourceMac,(uint16_t)0x88f7);
    int w=(int)qindesign::network::EthernetFrame.write(buf,static_cast<size_t>(size));
    // A short write means the frame on the wire is not the message.
    if (w != size)
    {
        qindesign::network::EthernetFrame.endFrame();
        txFailureCount++;
        return;
    }
    // The padding up to Ethernet's minimum frame. A fixed buffer, not a
    // variable-length one: 46 is the most that can ever be needed, and a
    // variable-length array on the stack for a bounded value buys
    // nothing.
    const int fill = 46-w;
    if(fill>0){
        uint8_t buf0[46] = {0};
        w+=static_cast<int>(qindesign::network::EthernetFrame.write(buf0, static_cast<size_t>(fill)));
    }
    // The result is checked. It used to be collected into a variable
    // nobody read, with the check commented out just below, so a Sync or
    // an Announce that did not go out left the slaves without a
    // reference and nothing said so.
    const bool sent = qindesign::network::EthernetFrame.endFrame();
    if (!sent)
    {
        txFailureCount++;
    }
}
