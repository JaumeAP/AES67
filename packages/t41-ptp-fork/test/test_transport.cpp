// Host unit tests for the two transports: l2ptp.cpp (raw 802.3 frames,
// EtherType 0x88F7) and l3ptp.cpp (UDP multicast on 319 and 320).
//
// Both are driven end to end: packets are pushed into the QNEthernet
// stub's receive queues, update() is called, and what comes back out is
// read from the stub's transmit lists.

#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "ptp/l2ptp.h"
#include "ptp/l3ptp.h"
#include "ptp_messages.h"
#include "stubs/stub_state.h"
#include "test_harness.h"

// The addresses and ports l3ptp.cpp is hardwired to.
static const std::string kEventEndpoint = "224.0.1.129:319";
static const std::string kGeneralEndpoint = "224.0.1.129:320";
static const std::string kPeerEventEndpoint = "224.0.0.107:319";
static const std::string kPeerGeneralEndpoint = "224.0.0.107:320";

// The two addresses Annex F of 1588 gives layer 2 PTP: peer-delay
// messages on 01-80-C2-00-00-0E, everything else on 01-1B-19-00-00-00.
static const uint8_t kPtpPeerMac[6] = {0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e};
static const uint8_t kPtpMulticastMac[6] = {0x01, 0x1b, 0x19, 0x00, 0x00, 0x00};

// ------------------------------------------------------------------- layer 2

// An 802.3 frame carrying a PTP payload, padded to the 60 bytes a real
// sender would pad to (the library drops anything below minFrameLen-4).
static std::vector<uint8_t> makeFrame(const std::vector<uint8_t> &payload, uint16_t etherType)
{
    std::vector<uint8_t> frame;
    frame.insert(frame.end(), kPtpPeerMac, kPtpPeerMac + 6);
    const uint8_t src[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    frame.insert(frame.end(), src, src + 6);
    frame.push_back((uint8_t)(etherType >> 8));
    frame.push_back((uint8_t)(etherType & 0xff));
    frame.insert(frame.end(), payload.begin(), payload.end());
    while (frame.size() < 60)
    {
        frame.push_back(0);
    }
    return frame;
}

static void pushFrame(const std::vector<uint8_t> &frame, NanoTime recv)
{
    ptptest::Datagram d;
    d.data = frame;
    nanoTimeToTimespec(recv, d.timestamp);
    ptptest::state().frameRx.push_back(d);
}

static void testL2MasterAnswersDelayRequest()
{
    ptptest::state().reset();
    l2PTP ptp(true, false, false);
    ptp.begin();

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    const NanoTime recv = 2000000000LL;
    pushFrame(makeFrame(makeDelayRequest(7, requester), 0x88f7), recv);
    ptp.update();

    CHECK_EQ(ptptest::state().frameTx.size(), 1);
    if (ptptest::state().frameTx.empty())
    {
        return;
    }
    const ptptest::SentFrame &f = ptptest::state().frameTx[0];
    CHECK_EQ(f.type, 0x88f7);
    for (int i = 0; i < 6; i++)
    {
        CHECK_EQ(f.dst[i], kPtpMulticastMac[i]);
        CHECK_EQ(f.src[i], ptptest::state().mac[i]);
    }
    CHECK_EQ(f.payload.size(), 54);       // Delay_Resp, no padding needed
    CHECK_EQ(f.payload[0], 9);            // Delay_Resp
    CHECK_EQ((f.payload[30] << 8) | f.payload[31], 7);
    CHECK_EQ(bufferToNanoTime(f.payload.data()), recv + HW_OFFSET);
    CHECK_EQ(ptp.getTxFailureCount(), 0);
}

// The same frame with an 802.1Q tag between the addresses and the
// EtherType.
static std::vector<uint8_t> makeVlanFrame(const std::vector<uint8_t> &payload, uint16_t vlanId)
{
    std::vector<uint8_t> frame;
    frame.insert(frame.end(), kPtpPeerMac, kPtpPeerMac + 6);
    const uint8_t src[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    frame.insert(frame.end(), src, src + 6);
    frame.push_back(0x81);
    frame.push_back(0x00);
    frame.push_back((uint8_t)(vlanId >> 8));
    frame.push_back((uint8_t)(vlanId & 0xff));
    frame.push_back(0x88);
    frame.push_back(0xf7);
    frame.insert(frame.end(), payload.begin(), payload.end());
    while (frame.size() < 60)
    {
        frame.push_back(0);
    }
    return frame;
}

static void testL2AcceptsVlanTaggedFrames()
{
    ptptest::state().reset();
    l2PTP ptp(true, false, false);
    ptp.begin();

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    const NanoTime recv = 2000000000LL;
    pushFrame(makeVlanFrame(makeDelayRequest(7, requester), 0x0064), recv);
    ptp.update();

    CHECK_EQ(ptptest::state().frameTx.size(), 1);
    if (ptptest::state().frameTx.empty())
    {
        return;
    }
    const ptptest::SentFrame &f = ptptest::state().frameTx[0];
    CHECK_EQ(f.payload[0], 9);  // Delay_Resp
    CHECK_EQ((f.payload[30] << 8) | f.payload[31], 7);
    CHECK_EQ(bufferToNanoTime(f.payload.data()), recv + HW_OFFSET);
}

static void testL2EndReleasesTheMulticastAddresses()
{
    ptptest::state().reset();
    l2PTP ptp(true, false, false);
    ptp.begin();

    CHECK_EQ(ptptest::state().macFilters.size(), 2);
    if (ptptest::state().macFilters.size() == 2)
    {
        CHECK(ptptest::state().macFilters[0].second);
        CHECK(ptptest::state().macFilters[1].second);
    }

    ptp.end();
    CHECK_EQ(ptptest::state().macFilters.size(), 4);
    if (ptptest::state().macFilters.size() == 4)
    {
        CHECK(ptptest::state().macFilters[2].first == "01:80:c2:00:00:0e");
        CHECK(!ptptest::state().macFilters[2].second);
        CHECK(!ptptest::state().macFilters[3].second);
    }

    // Nothing arrives on a stopped port.
    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    pushFrame(makeFrame(makeDelayRequest(7, requester), 0x88f7), 2000000000LL);
    ptp.update();
    CHECK_EQ(ptptest::state().frameTx.size(), 0);
}

static void testL2IgnoresOtherEtherTypes()
{
    ptptest::state().reset();
    l2PTP ptp(true, false, false);
    ptp.begin();

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    pushFrame(makeFrame(makeDelayRequest(7, requester), 0x0800), 2000000000LL);
    ptp.update();

    CHECK_EQ(ptptest::state().frameTx.size(), 0);
}

static void testL2DropsShortFrames()
{
    ptptest::state().reset();
    l2PTP ptp(true, false, false);
    ptp.begin();

    // 59 bytes: one below the minimum the library accepts.
    std::vector<uint8_t> frame = makeFrame(std::vector<uint8_t>(44, 0), 0x88f7);
    frame.resize(59);
    frame[14] = 1;  // Delay_Req, had it been read
    frame[15] = 2;
    pushFrame(frame, 2000000000LL);
    ptp.update();

    CHECK_EQ(ptptest::state().frameTx.size(), 0);
}

static void testL2DropsOversizedFrames()
{
    ptptest::state().reset();
    l2PTP ptp(true, false, false);
    ptp.begin();

    // Larger than the buffer the library reads into: it must bail out
    // before reading, not read into a stack buffer sized by the sender.
    std::vector<uint8_t> frame = makeFrame(std::vector<uint8_t>(44, 0), 0x88f7);
    frame.resize(1523, 0);
    pushFrame(frame, 2000000000LL);
    ptp.update();

    CHECK_EQ(ptptest::state().frameReadLens.size(), 0);
    CHECK_EQ(ptptest::state().frameTx.size(), 0);
}

static void testL2PadsToMinimumPayload()
{
    ptptest::state().reset();
    l2PTP ptp(true, false, false);
    ptp.begin();

    setTxTimestamp(3000000000LL);
    ptp.syncMessage();
    ptp.update();   // the Follow_Up goes out with the collected timestamp

    // Sync and Follow_Up are 44 bytes of PTP, padded out to 46.
    CHECK_EQ(ptptest::state().frameTx.size(), 2);
    if (ptptest::state().frameTx.size() != 2)
    {
        return;
    }
    CHECK_EQ(ptptest::state().frameTx[0].payload.size(), 46);
    CHECK_EQ(ptptest::state().frameTx[0].payload[0], 0);  // Sync
    CHECK_EQ(ptptest::state().frameTx[1].payload.size(), 46);
    CHECK_EQ(ptptest::state().frameTx[1].payload[0], 8);  // Follow_Up
    // The padding is zero, not whatever was on the stack.
    CHECK_EQ(ptptest::state().frameTx[0].payload[44], 0);
    CHECK_EQ(ptptest::state().frameTx[0].payload[45], 0);
}

static void testL2CountsTransmitFailures()
{
    ptptest::state().reset();
    l2PTP ptp(true, false, false);
    ptp.begin();

    ptptest::state().endFrameResult = false;
    ptp.announceMessage();
    ptp.announceMessage();
    CHECK_EQ(ptp.getTxFailureCount(), 2);
    CHECK_EQ(ptptest::state().frameTx.size(), 0);

    ptptest::state().endFrameResult = true;
    ptp.announceMessage();
    CHECK_EQ(ptp.getTxFailureCount(), 2);
    CHECK_EQ(ptptest::state().frameTx.size(), 1);
}

// ------------------------------------------------------------------- layer 3

// The sequence ID of the last datagram the library sent, which for a
// Delay_Req is the one its Delay_Resp has to carry back.
static uint16_t lastSentUdpSequenceID()
{
    const std::vector<ptptest::SentDatagram> &tx = ptptest::state().udpTx;
    if (tx.empty())
    {
        return 0;
    }
    return (uint16_t)((tx.back().data[30] << 8) | tx.back().data[31]);
}

static void pushDatagram(const std::string &endpoint, const std::vector<uint8_t> &data,
                         NanoTime recv)
{
    ptptest::Datagram d;
    d.data = data;
    nanoTimeToTimespec(recv, d.timestamp);
    ptptest::state().udpRx[endpoint].push_back(d);
}

static void testL3BindsTheStandardEndpoints()
{
    ptptest::state().reset();
    {
        l3PTP ptp(false, true, false);
        ptp.begin();
        const std::vector<std::string> &bound = ptptest::state().udpBound;
        CHECK_EQ(bound.size(), 2);
        if (bound.size() == 2)
        {
            CHECK(bound[0] == kEventEndpoint);
            CHECK(bound[1] == kGeneralEndpoint);
        }
    }

    ptptest::state().reset();
    {
        l3PTP ptp(false, true, true);
        ptp.begin();
        const std::vector<std::string> &bound = ptptest::state().udpBound;
        CHECK_EQ(bound.size(), 4);
        if (bound.size() == 4)
        {
            CHECK(bound[2] == kPeerEventEndpoint);
            CHECK(bound[3] == kPeerGeneralEndpoint);
        }
    }
}

// One end-to-end slave exchange over UDP: Sync on the event port,
// Follow_Up on the general port, then the Delay_Resp answering the
// Delay_Req the library sends from update().
static void runL3SlaveCycle(l3PTP &ptp, uint16_t sequenceID, NanoTime t1, NanoTime t2, NanoTime t3,
                            NanoTime t4)
{
    pushDatagram(kEventEndpoint, makeSync(sequenceID), t2);
    pushDatagram(kGeneralEndpoint, makeFollowUp(sequenceID, t1), 0);
    setTxTimestamp(t3);
    ptp.update();
    pushDatagram(kGeneralEndpoint, makeResponse(9, lastSentUdpSequenceID(), t4), 0);
    ptp.update();
}

static void testL3EndClosesTheSockets()
{
    ptptest::state().reset();
    l3PTP ptp(false, true, true);
    ptp.begin();
    CHECK_EQ(ptptest::state().udpBound.size(), 4);

    ptp.end();
    CHECK_EQ(ptptest::state().udpStopped.size(), 4);
    if (ptptest::state().udpStopped.size() == 4)
    {
        CHECK(ptptest::state().udpStopped[0] == kEventEndpoint);
        CHECK(ptptest::state().udpStopped[3] == kPeerGeneralEndpoint);
    }

    // A stopped port reads nothing, and update() does not go near the
    // sockets it no longer has.
    pushDatagram(kEventEndpoint, makeSync(1), 1000010000LL);
    ptp.update();
    CHECK_EQ(ptptest::state().udpReadLens.size(), 0);

    // Brought back up, it binds the same four endpoints again.
    ptp.begin();
    CHECK_EQ(ptptest::state().udpBound.size(), 8);
}

static void testL3SetsTheMulticastTTL()
{
    ptptest::state().reset();
    l3PTP ptp(false, true, true);
    ptp.begin();

    // All four sockets, opened with a TTL of one: PTP multicast has no
    // business leaving the segment, and lwIP would have sent it with 255.
    CHECK_EQ(ptptest::state().udpTTLs.size(), 4);
    for (size_t i = 0; i < ptptest::state().udpTTLs.size(); i++)
    {
        CHECK_EQ(ptptest::state().udpTTLs[i].second, 1);
    }

    // A profile that wants a different scope can say so, and it reaches
    // the sockets that are already open.
    ptp.setMulticastTTL(4);
    CHECK_EQ(ptptest::state().udpTTLs.size(), 8);
    if (ptptest::state().udpTTLs.size() == 8)
    {
        CHECK_EQ(ptptest::state().udpTTLs[4].second, 4);
        CHECK(ptptest::state().udpTTLs[4].first == kEventEndpoint);
        CHECK_EQ(ptptest::state().udpTTLs[7].second, 4);
        CHECK(ptptest::state().udpTTLs[7].first == kPeerGeneralEndpoint);
    }
}

// The queue PTP travels in.
//
// lwIP sends every datagram unmarked, so in a switch that treats DSCP the
// PTP ends up behind the audio it is meant to time. The AES67 and RAVENNA
// guides mark it; which value is the network's business, so the library
// takes it and does not choose.
static void testL3SetsTheDscp()
{
    ptptest::state().reset();
    l3PTP ptp(false, true, true);

    // Unmarked by default: what this library has always sent, so turning
    // it on is a decision and never a surprise.
    CHECK_EQ(ptp.getDscp(), 0);

    ptp.setDscp(46);   // EF
    ptp.begin();
    CHECK_EQ(ptptest::state().udpDscps.size(), 4);
    for (size_t i = 0; i < ptptest::state().udpDscps.size(); i++)
    {
        CHECK_EQ(ptptest::state().udpDscps[i].second, 46);
    }

    // And it reaches the sockets that are already open.
    ptp.setDscp(56);   // CS7, what Dante marks PTP with
    CHECK_EQ(ptptest::state().udpDscps.size(), 8);
    if (ptptest::state().udpDscps.size() == 8)
    {
        CHECK_EQ(ptptest::state().udpDscps[4].second, 56);
        CHECK(ptptest::state().udpDscps[4].first == kEventEndpoint);
        CHECK_EQ(ptptest::state().udpDscps[7].second, 56);
        CHECK(ptptest::state().udpDscps[7].first == kPeerGeneralEndpoint);
    }

    // Six bits: the two below them are ECN and are not ours to set.
    ptp.setDscp(0xff);
    CHECK_EQ(ptp.getDscp(), 0x3f);
}


static void testL3SlaveCycle()
{
    ptptest::state().reset();
    l3PTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;

    for (int cycle = 0; cycle < 2; cycle++)
    {
        const NanoTime t1 = base + cycle * NS_PER_S;
        runL3SlaveCycle(ptp, (uint16_t)(cycle + 1), t1, t1 + pathDelay, t1 + turnaround,
                        t1 + turnaround + pathDelay);
    }

    CHECK_EQ(ptp.getDelay(), pathDelay);
    CHECK_EQ(ptp.getOffset(), HW_OFFSET);

    // Delay_Req is an event message: it goes out on port 319.
    CHECK_EQ(ptptest::state().udpTx.size(), 2);
    if (!ptptest::state().udpTx.empty())
    {
        CHECK(ptptest::state().udpTx[0].destination == kEventEndpoint);
        CHECK_EQ(ptptest::state().udpTx[0].data[0], 1);
        CHECK_EQ(ptptest::state().udpTx[0].data.size(), 44);
    }
    CHECK_EQ(ptp.getTxFailureCount(), 0);
}

static void testL3ReadsAtMostTheParsedLength()
{
    ptptest::state().reset();
    l3PTP ptp(false, true, false);
    ptp.begin();

    // A Sync padded out with trailing bytes the library has no interest
    // in. It must read a bounded amount and still parse the message.
    std::vector<uint8_t> sync = makeSync(1);
    sync.resize(400, 0xAA);
    pushDatagram(kEventEndpoint, sync, 1000010000LL);
    pushDatagram(kGeneralEndpoint, makeFollowUp(1, 1000000000LL), 0);
    setTxTimestamp(1500000000LL);
    ptp.update();

    CHECK_EQ(ptptest::state().udpReadLens.size(), 2);
    if (ptptest::state().udpReadLens.size() == 2)
    {
        CHECK_EQ(ptptest::state().udpReadLens[0], 64);  // clamped
        CHECK_EQ(ptptest::state().udpReadLens[1], 44);  // the Follow_Up as it came
    }
    // The pair still matched, so the Delay_Req went out.
    CHECK_EQ(ptptest::state().udpTx.size(), 1);
}

static void testL3MasterUsesEventAndGeneralPorts()
{
    ptptest::state().reset();
    l3PTP ptp(true, false, false);
    ptp.begin();

    setTxTimestamp(3000000000LL);
    ptp.syncMessage();
    ptp.update();
    ptp.announceMessage();

    CHECK_EQ(ptptest::state().udpTx.size(), 3);
    if (ptptest::state().udpTx.size() != 3)
    {
        return;
    }
    CHECK(ptptest::state().udpTx[0].destination == kEventEndpoint);
    CHECK_EQ(ptptest::state().udpTx[0].data[0], 0);   // Sync
    CHECK(ptptest::state().udpTx[1].destination == kGeneralEndpoint);
    CHECK_EQ(ptptest::state().udpTx[1].data[0], 8);   // Follow_Up
    CHECK(ptptest::state().udpTx[2].destination == kGeneralEndpoint);
    CHECK_EQ(ptptest::state().udpTx[2].data[0], 11);  // Announce
}

static void testL3CountsTransmitFailures()
{
    ptptest::state().reset();
    l3PTP ptp(true, false, false);
    ptp.begin();

    ptptest::state().udpSendResult = false;
    ptp.announceMessage();
    ptp.announceMessage();
    CHECK_EQ(ptp.getTxFailureCount(), 2);
    CHECK_EQ(ptptest::state().udpTx.size(), 0);

    ptptest::state().udpSendResult = true;
    ptp.announceMessage();
    CHECK_EQ(ptp.getTxFailureCount(), 2);
    CHECK_EQ(ptptest::state().udpTx.size(), 1);
}

static void testL3PeerDelayCycle()
{
    ptptest::state().reset();
    l3PTP ptp(false, true, true);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;
    const NanoTime residence = 1000;

    // The peer-delay request goes out on the port's own interval, so the
    // clock has to move for a second one to be due: half of the default
    // second, and a second between the cycles.
    ptptest::state().randomValue = 500;

    for (int cycle = 0; cycle < 2; cycle++)
    {
        const uint16_t seq = (uint16_t)(cycle + 1);
        const NanoTime t1 = base + cycle * NS_PER_S;
        const NanoTime t2 = t1 + pathDelay;
        const NanoTime t3 = t1 + turnaround;
        const NanoTime t4 = t3 + pathDelay;
        const NanoTime t5 = t4 + residence;
        const NanoTime t6 = t5 + pathDelay;

        ptptest::state().millisNow = (unsigned long)cycle * 1000;
        pushDatagram(kEventEndpoint, makeSync(seq), t2);
        pushDatagram(kGeneralEndpoint, makeFollowUp(seq, t1), 0);
        setTxTimestamp(t3);
        ptp.update();  // sends Pdelay_Req and takes t3

        // Pdelay_Resp is an event message, its Follow_Up a general one,
        // both on the peer-delay multicast address.
        const uint16_t requestSeq = lastSentUdpSequenceID();
        pushDatagram(kPeerEventEndpoint, makeResponse(3, requestSeq, t4), t6);
        pushDatagram(kPeerGeneralEndpoint, makePdelayRespFollowUp(requestSeq, t5), 0);
        ptp.update();
    }

    CHECK_EQ(ptp.getDelay(), pathDelay);
    CHECK_EQ(ptp.getOffset(), 500);  // the peer-delay branch adds a fixed 500 ns

    // The request went out as a Pdelay_Req, on the peer-delay group,
    // which is also the only group this library listens for them on.
    CHECK_EQ(ptptest::state().udpTx.size(), 2);
    if (!ptptest::state().udpTx.empty())
    {
        CHECK_EQ(ptptest::state().udpTx[0].data[0], 2);
        CHECK_EQ(ptptest::state().udpTx[0].data.size(), 54);
        CHECK(ptptest::state().udpTx[0].destination == kPeerEventEndpoint);
    }
}


static void testL2TakesSeveralFramesPerUpdate()
{
    ptptest::state().reset();
    l2PTP ptp(true, false, false);
    ptp.begin();

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    for (int i = 0; i < 3; i++)
    {
        pushFrame(makeFrame(makeDelayRequest((uint16_t)(i + 1), requester), 0x88f7),
                  2000000000LL + i);
    }

    // Three requests, one update: one frame per call used to leave two of
    // them queued.
    ptp.update();
    CHECK_EQ(ptptest::state().frameTx.size(), 3);
    if (ptptest::state().frameTx.size() == 3)
    {
        CHECK_EQ((ptptest::state().frameTx[2].payload[30] << 8) |
                     ptptest::state().frameTx[2].payload[31],
                 3);
    }
}

static void testL3TakesSeveralDatagramsPerUpdate()
{
    ptptest::state().reset();
    l3PTP ptp(true, false, false);
    ptp.begin();

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    for (int i = 0; i < 3; i++)
    {
        pushDatagram(kEventEndpoint, makeDelayRequest((uint16_t)(i + 1), requester),
                     2000000000LL + i);
    }

    ptp.update();
    CHECK_EQ(ptptest::state().udpTx.size(), 3);

    // The cap holds: a flood cannot keep update() inside the socket.
    for (int i = 0; i < 10; i++)
    {
        pushDatagram(kEventEndpoint, makeDelayRequest((uint16_t)(10 + i), requester),
                     2000000000LL + i);
    }
    ptp.update();
    CHECK_EQ(ptptest::state().udpTx.size(), 7);
}


static void testL3PeerDelayAnswerGoesToThePeerGroup()
{
    ptptest::state().reset();
    l3PTP ptp(true, false, true);
    ptp.begin();

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    const NanoTime recv = 2000000000LL;
    setTxTimestamp(recv + 30000);
    pushDatagram(kPeerEventEndpoint, makePeerDelayRequest(9, requester), recv);
    ptp.update();

    // The answer and its Follow_Up, and the Pdelay_Req this port sends on
    // its own account: peer delay runs in both directions on a link.
    CHECK_EQ(ptptest::state().udpTx.size(), 3);
    if (ptptest::state().udpTx.size() != 3)
    {
        return;
    }
    // Pdelay_Resp on the peer event port, its Follow_Up on the peer
    // general port. Both used to go to 224.0.1.129.
    CHECK_EQ(ptptest::state().udpTx[0].data[0], 3);
    CHECK(ptptest::state().udpTx[0].destination == kPeerEventEndpoint);
    CHECK_EQ(ptptest::state().udpTx[1].data[0], 10);
    CHECK(ptptest::state().udpTx[1].destination == kPeerGeneralEndpoint);

    // This port's own Pdelay_Req is on the peer group too.
    CHECK_EQ(ptptest::state().udpTx[2].data[0] & 0x0f, 2);
    CHECK(ptptest::state().udpTx[2].destination == kPeerEventEndpoint);

    // And the ordinary messages stay where they were.
    ptp.announceMessage();
    CHECK_EQ(ptptest::state().udpTx.size(), 4);
    if (ptptest::state().udpTx.size() == 4)
    {
        CHECK(ptptest::state().udpTx[3].destination == kGeneralEndpoint);
    }
}

static void testL3CountsBindFailures()
{
    ptptest::state().reset();
    ptptest::state().udpBindResult = false;

    l3PTP ptp(false, true, true);
    ptp.begin();

    // Four groups, none of them joined: a port that hears nothing now
    // says so.
    CHECK_EQ(ptp.getBindFailureCount(), 4);
}

static void testL2CountsShortWrites()
{
    ptptest::state().reset();
    l2PTP ptp(true, false, false);
    ptp.begin();

    // The frame buffer refuses everything past the first ten bytes.
    ptptest::state().frameWriteLimit = 10;
    ptp.announceMessage();

    // The failure is counted whatever the frame layer then does with the
    // truncated frame it was left holding.
    CHECK_EQ(ptp.getTxFailureCount(), 1);
    if (!ptptest::state().frameTx.empty())
    {
        CHECK(ptptest::state().frameTx[0].payload.size() < 64);
    }
}

// --------------------------------------------------------------- entry point


// Annex F of 1588 gives peer-delay messages 01-80-C2-00-00-0E and
// everything else 01-1B-19-00-00-00. This sent the lot to the peer-delay
// address, which is inside the range 802.1D reserves for link-local
// control traffic and a bridge is entitled to drop rather than forward:
// a Sync addressed there reaches the segment and nothing past it.
static void testL2NonPeerMessagesUseTheNonPeerAddress()
{
    ptptest::state().reset();
    l2PTP ptp(true, false, false);
    ptp.begin();

    setTxTimestamp(3000000000LL);
    ptp.syncMessage();
    ptp.update();
    ptp.announceMessage();

    CHECK_EQ(ptptest::state().frameTx.size(), 3);  // Sync, Follow_Up, Announce
    for (const ptptest::SentFrame &f : ptptest::state().frameTx)
    {
        for (int i = 0; i < 6; i++)
        {
            CHECK_EQ(f.dst[i], kPtpMulticastMac[i]);
        }
    }
}

// Peer-delay is the exception: it stays on 01-80-C2-00-00-0E, which is
// where a peer listens for it.
static void testL2PeerDelayMessagesKeepThePeerAddress()
{
    ptptest::state().reset();
    l2PTP ptp(false, true, true);
    ptp.begin();

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    pushFrame(makeFrame(makePeerDelayRequest(4, requester), 0x88f7), 2000000000LL);
    ptp.update();

    CHECK_EQ(ptptest::state().frameTx.size(), 1);
    if (!ptptest::state().frameTx.empty())
    {
        const ptptest::SentFrame &f = ptptest::state().frameTx[0];
        CHECK_EQ(f.payload[0], 3);  // Pdelay_Resp
        for (int i = 0; i < 6; i++)
        {
            CHECK_EQ(f.dst[i], kPtpPeerMac[i]);
        }
    }
}

// 802.1AS is the one profile that really does put every message on the
// peer-delay address, because gPTP is hop by hop. It is the profile this
// port announces with majorSdoId 1.
static void testL2GptpKeepsEverythingOnThePeerAddress()
{
    ptptest::state().reset();
    l2PTP ptp(true, false, true);
    ptp.applyProfile(PTPBase::ProfileSettings{0, 1, -3, 0, 0});  // 802.1AS
    ptp.begin();

    setTxTimestamp(3000000000LL);
    ptp.syncMessage();
    ptp.update();   // the Follow_Up, and this port's own Pdelay_Req

    CHECK_EQ(ptptest::state().frameTx.size(), 3);
    for (const ptptest::SentFrame &f : ptptest::state().frameTx)
    {
        for (int i = 0; i < 6; i++)
        {
            CHECK_EQ(f.dst[i], kPtpPeerMac[i]);
        }
    }
}


static void testL2FiltersAreHandedBackWhenTheObjectGoes()
{
    ptptest::state().reset();
    {
        l2PTP ptp(true, false, false);
        ptp.begin();
        CHECK_EQ(ptptest::state().macFilters.size(), 2);
    }

    // Destroyed without end(). The two multicast addresses are global to
    // the interface and only this object asked for them, so they are
    // handed back here too -- l3PTP has always closed its sockets from
    // its destructor.
    CHECK_EQ(ptptest::state().macFilters.size(), 4);
    if (ptptest::state().macFilters.size() == 4)
    {
        CHECK(!ptptest::state().macFilters[2].second);
        CHECK(!ptptest::state().macFilters[3].second);
    }

    // A port never begun asks for nothing, and hands nothing back.
    ptptest::state().reset();
    {
        l2PTP ptp(true, false, false);
        (void)ptp;
    }
    CHECK_EQ(ptptest::state().macFilters.size(), 0);
}

static void testBindFailuresAreCountedSinceBoot()
{
    ptptest::state().reset();
    ptptest::state().udpBindResult = false;

    l3PTP ptp(false, true, true);
    ptp.begin();
    CHECK_EQ(ptp.getBindFailureCount(), 4);

    // reset() drops the measurement and the state around it, not the
    // failure counts: it used to clear this one and leave the transmit
    // one alone, so the two numbers a sketch logs side by side covered
    // different spans of time and neither said which.
    ptp.reset();
    CHECK_EQ(ptp.getBindFailureCount(), 4);
}

// A full-size frame is read only as far as the parser looks.
//
// The whole frame used to be copied into a 1522-byte member -- fifteen
// hundred bytes of memcpy on the busiest path in the loop -- for the
// eighty-two bytes that are ever read out of it.
static void testL2ReadsOnlyWhatTheParserNeeds()
{
    ptptest::state().reset();
    l2PTP ptp(true, false, false);
    ptp.begin();

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    std::vector<uint8_t> frame = makeFrame(makeDelayRequest(4, requester), 0x88f7);
    frame.resize(1500, 0);   // a PTP message inside a full-size frame
    pushFrame(frame, 2000000000LL);
    ptp.update();

    CHECK_EQ(ptptest::state().frameReadLens.size(), 1);
    if (!ptptest::state().frameReadLens.empty())
    {
        CHECK_EQ(ptptest::state().frameReadLens[0], (size_t)L2_RECV_BUF_LEN);
    }
    // And the message inside it is still answered.
    CHECK_EQ(ptptest::state().frameTx.size(), 1);
}

// The interface MAC is read when the port comes up, not once per frame.
static void testL2ReadsTheInterfaceMacOnce()
{
    ptptest::state().reset();
    l2PTP ptp(true, false, false);
    ptp.begin();

    const int afterBegin = ptptest::state().macAddressReads;
    setTxTimestamp(3000000000LL);
    ptp.syncMessage();
    ptp.update();            // Sync and, once the timestamp is in, Follow_Up
    ptp.announceMessage();

    CHECK_EQ(ptptest::state().frameTx.size(), 3);
    CHECK_EQ(ptptest::state().macAddressReads, afterBegin);
    // The frames still carry it.
    if (!ptptest::state().frameTx.empty())
    {
        CHECK_EQ(ptptest::state().frameTx[0].src[0], ptptest::state().mac[0]);
        CHECK_EQ(ptptest::state().frameTx[0].src[5], ptptest::state().mac[5]);
    }
}

// A PTP frame that was not addressed to either PTP group is ignored.
//
// The interface accepts its own unicast address and broadcast as well as
// the two groups this transport opens, and nothing looked at which of
// them a frame arrived on: a PTP message sent to this board alone was
// taken as one sent to the group.
static void testL2IgnoresFramesNotAddressedToPtp()
{
    ptptest::state().reset();
    l2PTP ptp(true, false, false);
    ptp.begin();

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    std::vector<uint8_t> frame = makeFrame(makeDelayRequest(7, requester), 0x88f7);
    // Readdressed to this board's own MAC.
    for (size_t i = 0; i < 6; i++)
    {
        frame[i] = ptptest::state().mac[i];
    }
    pushFrame(frame, 2000000000LL);
    ptp.update();
    CHECK_EQ(ptptest::state().frameTx.size(), 0);

    // The same message on the group it belongs to is answered.
    std::vector<uint8_t> onTheGroup = makeFrame(makeDelayRequest(8, requester), 0x88f7);
    pushFrame(onTheGroup, 2000100000LL);
    ptp.update();
    CHECK_EQ(ptptest::state().frameTx.size(), 1);
}

void runTransportTests()
{
    testL2MasterAnswersDelayRequest();
    testL2AcceptsVlanTaggedFrames();
    testL2EndReleasesTheMulticastAddresses();
    testL2IgnoresOtherEtherTypes();
    testL2DropsShortFrames();
    testL2DropsOversizedFrames();
    testL2PadsToMinimumPayload();
    testL2CountsTransmitFailures();
    testL3BindsTheStandardEndpoints();
    testL3EndClosesTheSockets();
    testL3SetsTheMulticastTTL();
    testL3SetsTheDscp();
    testL3SlaveCycle();
    testL3ReadsAtMostTheParsedLength();
    testL3MasterUsesEventAndGeneralPorts();
    testL3CountsTransmitFailures();
    testL3PeerDelayCycle();
    testL3PeerDelayAnswerGoesToThePeerGroup();
    testL3CountsBindFailures();
    testBindFailuresAreCountedSinceBoot();
    testL2ReadsOnlyWhatTheParserNeeds();
    testL2ReadsTheInterfaceMacOnce();
    testL2IgnoresFramesNotAddressedToPtp();
    testL2FiltersAreHandedBackWhenTheObjectGoes();
    testL2CountsShortWrites();
    testL2NonPeerMessagesUseTheNonPeerAddress();
    testL2PeerDelayMessagesKeepThePeerAddress();
    testL2GptpKeepsEverythingOnThePeerAddress();
    testL2TakesSeveralFramesPerUpdate();
    testL3TakesSeveralDatagramsPerUpdate();
}
