#include <QNEthernet.h>

#include "l3ptp.h"

// The most we read from a datagram. The longest PTP message this library
// parses is 54 bytes; anything beyond that is TLVs nothing here looks at.
//
// A fixed buffer, not a variable-length one. It used to be
// `uint8_t buf[esize]`, with esize straight from parsePacket(): the size
// of what went on the stack was decided by whoever sent the packet.
// Bounded by the frame in the normal case, but IP_REASSEMBLY is on by
// default in lwIP and is not turned off in QNEthernet's lwipopts.h, so a
// fragmented datagram can arrive larger.
static constexpr int PTP_RECV_BUF_LEN = 64;

// How many datagrams one socket may hand over in a single update().
//
// It used to be one per socket per call, so a burst -- or a loop() that
// spent a millisecond waiting for a transmit timestamp -- left the rest
// queued until the following calls, each of which could only take one
// more. The cap is what keeps a flood from holding update() indefinitely.
static constexpr int MAX_PACKETS_PER_UPDATE = 4;

const IPAddress adr{224, 0, 1, 129};
const IPAddress pAdr{224, 0, 0, 107};
const int eventPort = 319;
const int generalPort = 320;

l3PTP::l3PTP(bool master_, bool slave_, bool p2p_):
PTPBase(master_,slave_,p2p_)
{

}

l3PTP::~l3PTP()
{
    // Qualified, for the reason given in l2PTP's destructor.
    l3PTP::closeSockets();
}

void l3PTP::setMulticastTTL(uint8_t val)
{
    multicastTTL = val;
    qindesign::network::EthernetUDP *sockets[4] = {eventSocket, generalSocket, pEventSocket,
                                                   pGeneralSocket};
    for (int i = 0; i < 4; i++)
    {
        if (sockets[i] != nullptr)
        {
            sockets[i]->setMulticastTTL(multicastTTL);
        }
    }
}

void l3PTP::setDscp(uint8_t val)
{
    dscp = val & 0x3f;
    qindesign::network::EthernetUDP *sockets[4] = {eventSocket, generalSocket, pEventSocket,
                                                   pGeneralSocket};
    for (int i = 0; i < 4; i++)
    {
        if (sockets[i] != nullptr)
        {
            sockets[i]->setOutgoingDiffServ(dscp);
        }
    }
}

// One socket, opened on the group and port given.
//
// The allocation is checked. The Teensy core is built without exceptions,
// so operator new has none to throw and answers a null pointer when the
// heap is out -- and every line that followed the four allocations
// dereferenced the result straight away. A board that ran out of RAM
// bringing the port up therefore faulted inside begin(), instead of
// coming up with the failure counted, which is the number
// getBindFailureCount() exists to report.
qindesign::network::EthernetUDP *l3PTP::openSocket(const IPAddress &group, uint16_t port)
{
    qindesign::network::EthernetUDP *socket = new qindesign::network::EthernetUDP;
    if (socket == nullptr)
    {
        bindFailureCount++;
        return nullptr;
    }
    socket->setMulticastTTL(multicastTTL);
    socket->setOutgoingDiffServ(dscp);
    // A group this port could not join is a port that hears nothing, and
    // the result used to be dropped on the floor.
    //
    // The socket is kept rather than destroyed, deliberately: a failed join
    // stops this port receiving on that group and does not stop it sending,
    // and a master whose Announce and Sync still go out while it hears nothing
    // is more useful than a port that has silently stopped being a port. The
    // failure is in getBindFailureCount() for anyone who wants to act on it.
    if (!socket->beginMulticast(group, port, true))
    {
        bindFailureCount++;
    }
    return socket;
}

void l3PTP::initSockets()
{
    eventSocket = openSocket(adr, eventPort);
    generalSocket = openSocket(adr, generalPort);

    if(p2p){
        pEventSocket = openSocket(pAdr, eventPort);
        pGeneralSocket = openSocket(pAdr, generalPort);
    }
}

void l3PTP::closeSockets()
{
    qindesign::network::EthernetUDP *sockets[4] = {eventSocket, generalSocket, pEventSocket,
                                                   pGeneralSocket};
    for (int i = 0; i < 4; i++)
    {
        if (sockets[i] != nullptr)
        {
            sockets[i]->stop();
            delete sockets[i];
        }
    }
    eventSocket = nullptr;
    generalSocket = nullptr;
    pEventSocket = nullptr;
    pGeneralSocket = nullptr;
}

void l3PTP::drainSocket(qindesign::network::EthernetUDP *socket)
{
    if (socket == nullptr)
    {
        return;
    }
    for (int i = 0; i < MAX_PACKETS_PER_UPDATE; i++)
    {
        const int size = socket->parsePacket();
        if (size <= 0)
        {
            return;
        }
        timespec recv_ts;
        socket->timestamp(recv_ts);
        uint8_t buf[PTP_RECV_BUF_LEN];
        const int n = size > PTP_RECV_BUF_LEN ? PTP_RECV_BUF_LEN : size;
        socket->read(buf, static_cast<size_t>(n));
        parsePTPMessage(buf,n,recv_ts);
    }
}

void l3PTP::updateSockets()
{
    drainSocket(eventSocket);
    drainSocket(generalSocket);
    if(p2p){
        drainSocket(pEventSocket);
        drainSocket(pGeneralSocket);
    }
}

void l3PTP::sendPTPMessage(const uint8_t *buf, int size, bool generalMessage,
                           bool peerAddress){
    // Peer-delay messages belong on 224.0.0.107, which is also the only
    // group this library listens for them on. Everything went to the
    // default group before, so a Pdelay_Req or a Pdelay_Resp of ours
    // reached nobody expecting it.
    qindesign::network::EthernetUDP *socket;
    if (peerAddress)
    {
        socket = generalMessage ? pGeneralSocket : pEventSocket;
    }
    else
    {
        socket = generalMessage ? generalSocket : eventSocket;
    }
    if (socket == nullptr)
    {
        txFailureCount++;
        return;
    }

    const IPAddress &destination = peerAddress ? pAdr : adr;
    const int port = generalMessage ? generalPort : eventPort;

    // The result is checked: it used to be discarded entirely, so a Sync
    // or an Announce that did not go out left the slaves without a
    // reference with nothing to say so.
    if (!socket->send(destination, static_cast<uint16_t>(port), buf, static_cast<size_t>(size)))
    {
        txFailureCount++;
    }
}
