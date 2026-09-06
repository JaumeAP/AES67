// Host unit tests for PTPBase.
//
// The class is exercised through a subclass that implements the three
// pure virtuals and captures everything the library tries to send, so
// nothing private has to be reopened. What the servo does to the clock
// is observed through the QNEthernet stub in stubs/.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <vector>

#include "ptp/ptp-base.h"
#include "ptp_messages.h"
#include "stubs/stub_state.h"
#include "test_harness.h"

struct SentMessage
{
    std::vector<uint8_t> data;
    bool general;
    bool peer;
};

class TestPTP : public PTPBase
{
public:
    TestPTP(bool master_, bool slave_, bool p2p_) : PTPBase(master_, slave_, p2p_) {}

    using PTPBase::parsePTPMessage;

    std::vector<SentMessage> sent;

    void feed(const std::vector<uint8_t> &msg, NanoTime recv)
    {
        timespec ts;
        nanoTimeToTimespec(recv, ts);
        parsePTPMessage(msg.data(), (int)msg.size(), ts);
    }

    int closeSocketsCalls = 0;

private:
    void initSockets() override {}
    void closeSockets() override { closeSocketsCalls++; }
    void updateSockets() override {}
    void sendPTPMessage(const uint8_t *buf, int size, bool generalMessage,
                        bool peerAddress) override
    {
        sent.push_back(SentMessage{std::vector<uint8_t>(buf, buf + size), generalMessage,
                                   peerAddress});
    }
};

// The sequence ID of the last message the library sent, which for a
// Delay_Req is the one its Delay_Resp has to carry back.
static uint16_t lastSentSequenceID(const TestPTP &ptp)
{
    if (ptp.sent.empty())
    {
        return 0;
    }
    const std::vector<uint8_t> &last = ptp.sent.back().data;
    return (uint16_t)((last[30] << 8) | last[31]);
}

// How many of the messages the library sent carry the message type given.
//
// A peer-delay port sends its own Pdelay_Req from update(), on its own
// interval and not off a Sync, so counting everything it sent no longer
// says what a test about the answers means.
static size_t countSentOfType(const TestPTP &ptp, uint8_t messageType)
{
    size_t count = 0;
    for (size_t i = 0; i < ptp.sent.size(); i++)
    {
        if (!ptp.sent[i].data.empty() && (ptp.sent[i].data[0] & 0x0f) == messageType)
        {
            count++;
        }
    }
    return count;
}

// One end-to-end slave exchange: Sync, Follow_Up, the Delay_Req the
// library emits in update(), and the Delay_Resp answering it.
static void runSlaveCycle(TestPTP &ptp, uint16_t sequenceID, NanoTime t1, NanoTime t2, NanoTime t3,
                          NanoTime t4)
{
    ptp.feed(makeSync(sequenceID), t2);
    ptp.feed(makeFollowUp(sequenceID, t1), 0);
    setTxTimestamp(t3);
    ptp.update();  // sends Delay_Req and takes t3
    ptp.feed(makeResponse(9, lastSentSequenceID(ptp), t4), 0);
    ptp.update();  // completes the cycle
}

// ------------------------------------------------------------------- tests

static void testBufferConversions()
{
    ptptest::state().reset();

    std::vector<uint8_t> buf(54, 0);
    putTimestamp(buf, 1234567890123456789LL % (NS_PER_S * 1000000LL));
    const NanoTime value = 123456789012345LL;
    putTimestamp(buf, value);
    CHECK_EQ(bufferToNanoTime(buf.data()), value);

    // Seconds beyond MAX_SAFE_SECONDS are clamped instead of overflowing
    // the multiplication by NS_PER_S.
    std::vector<uint8_t> huge(54, 0);
    for (size_t i = 34; i < 40; i++)
    {
        huge[i] = 0xff;  // 2^48-1 seconds
    }
    huge[40] = 0;
    huge[41] = 0;
    huge[42] = 0;
    huge[43] = 0;
    CHECK_EQ(bufferToNanoTime(huge.data()), MAX_SAFE_SECONDS * NS_PER_S);

    // correctionField is scaled by 2^16.
    std::vector<uint8_t> corr(54, 0);
    corr[14] = 0x00;
    corr[15] = 0x00;
    corr[13] = 0x01;  // 1 << 16 == 1 ns
    CHECK_EQ(bufferToCorrection(corr.data()), 1);

    std::vector<uint8_t> negative(54, 0);
    for (size_t i = 8; i < 16; i++)
    {
        negative[i] = 0xff;  // -1 in 2^-16 ns
    }
    CHECK_EQ(bufferToCorrection(negative.data()), -1);

    // timespecToBuffer and bufferToNanoTime are inverses.
    timespec ts;
    nanoTimeToTimespec(value, ts);
    CHECK_EQ(ts.tv_sec, value / NS_PER_S);
    CHECK_EQ(ts.tv_nsec, value % NS_PER_S);
    std::vector<uint8_t> roundtrip(54, 0);
    timespecToBuffer(ts, roundtrip.data());
    CHECK_EQ(bufferToNanoTime(roundtrip.data()), value);
    CHECK_EQ(timespecToNanoTime(ts), value);
}

// The seconds of a timespec are 32 bits on the board and 64 here, so this
// is the one case the development machine cannot reach on its own: it is
// checked against int32_t directly.
static void testSecondsAreClampedNotWrapped()
{
    CHECK_EQ(clampSeconds<int32_t>(0), 0);
    CHECK_EQ(clampSeconds<int32_t>(1700000000), 1700000000);
    CHECK_EQ(clampSeconds<int32_t>(2147483647), 2147483647);

    // A second past the edge stays at the edge. Wrapping would put the
    // clock in 1901, and MAX_SAFE_SECONDS lets a packet name 292 years.
    CHECK_EQ(clampSeconds<int32_t>(2147483648LL), 2147483647);
    CHECK_EQ(clampSeconds<int32_t>(MAX_SAFE_SECONDS), 2147483647);
    CHECK_EQ(clampSeconds<int32_t>(-2147483649LL), -2147483648LL);

    // On this machine the type is wider, so nothing is clamped.
    CHECK_EQ(clampToTvSec(MAX_SAFE_SECONDS),
             static_cast<TvSec>(clampSeconds<TvSec>(MAX_SAFE_SECONDS)));
}

static void testShortMessagesAreIgnored()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    // Shorter than the common header.
    std::vector<uint8_t> tiny = makeSync(1);
    tiny.resize(PTP_HEADER_LEN - 1);
    ptp.feed(tiny, 1000);

    // Long enough for the header, one byte short of a Sync.
    std::vector<uint8_t> shortSync = makeSync(1);
    shortSync.resize(PTP_SYNC_LEN - 1);
    ptp.feed(shortSync, 1000);

    std::vector<uint8_t> shortFollowUp = makeFollowUp(1, 1000000000LL);
    shortFollowUp.resize(PTP_SYNC_LEN - 1);
    ptp.feed(shortFollowUp, 0);

    setTxTimestamp(2000);
    ptp.update();

    // Neither message was accepted, so no Delay_Req was triggered.
    CHECK_EQ(ptp.sent.size(), 0);
}

static void testWrongVersionIsIgnored()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    std::vector<uint8_t> sync = makeSync(1);
    sync[1] = 1;  // PTPv1
    ptp.feed(sync, 1000);
    std::vector<uint8_t> followUp = makeFollowUp(1, 1000000000LL);
    followUp[1] = 1;
    ptp.feed(followUp, 0);

    setTxTimestamp(2000);
    ptp.update();
    CHECK_EQ(ptp.sent.size(), 0);
}

static void testEndToEndSlaveCycle()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    // A symmetric path of 10 us and a perfectly aligned clock: the offset
    // that comes out is the hardware offset alone.
    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;

    runSlaveCycle(ptp, 1, base, base + pathDelay, base + turnaround,
                  base + turnaround + pathDelay);

    // The first cycle has no previous Sync to compare against, so the
    // servo has not run yet.
    CHECK_EQ(ptp.getOffset(), 0);
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 0);

    const NanoTime second = base + NS_PER_S;
    runSlaveCycle(ptp, 2, second, second + pathDelay, second + turnaround,
                  second + turnaround + pathDelay);

    CHECK_EQ(ptp.getDelay(), pathDelay);
    CHECK_EQ(ptp.getOffset(), HW_OFFSET);

    // One Delay_Req per cycle, type 1, sent as an event message.
    CHECK_EQ(ptp.sent.size(), 2);
    if (ptp.sent.size() == 2)
    {
        CHECK_EQ(ptp.sent[0].data[0], 1);
        CHECK_EQ(ptp.sent[0].data.size(), 44);
        CHECK_EQ(ptp.sent[0].general, false);
        // The sequence ID of the Delay_Req is the library's own counter.
        CHECK_EQ((ptp.sent[0].data[30] << 8) | ptp.sent[0].data[31], 0);
        CHECK_EQ((ptp.sent[1].data[30] << 8) | ptp.sent[1].data[31], 1);
    }

    // Two clocks one second apart with the same rate: no frequency error,
    // and an offset inside the fine band, so the servo trims the
    // frequency rather than stepping the timer.
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 1);
    CHECK_EQ(ptptest::state().offsetTimerCalls.size(), 0);
    // 200 ns of offset is more than the +-100 ns lock band.
    CHECK_EQ(ptp.getLockCount(), 0);
}

static void testDelayResponseForAnotherPortIsIgnored()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();
    // No filter, so the delay is whatever the last accepted exchange
    // measured, and a refused answer shows as the delay standing still.
    ptp.setDelayFilterLength(1);

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;

    runSlaveCycle(ptp, 1, base, base + pathDelay, base + turnaround,
                  base + turnaround + pathDelay);
    CHECK_EQ(ptp.getDelay(), pathDelay);

    // Second exchange on a longer path, answered by somebody else's port.
    const NanoTime second = base + NS_PER_S;
    const NanoTime longer = 30000;
    ptp.feed(makeSync(2), second + longer);
    ptp.feed(makeFollowUp(2, second), 0);
    setTxTimestamp(second + turnaround);
    ptp.update();
    ptp.feed(makeResponse(9, lastSentSequenceID(ptp), second + turnaround + longer,
                          /*matchingIdentity=*/false), 0);
    ptp.update();

    CHECK_EQ(ptp.getDelay(), pathDelay);
}

static void testEqualSyncTimestampsDoNotDriveTheServo()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;

    // Both cycles carry the same T1 and T2: the drift denominator is
    // zero and the controller must not run on it.
    runSlaveCycle(ptp, 1, base, base + pathDelay, base + turnaround,
                  base + turnaround + pathDelay);
    runSlaveCycle(ptp, 2, base, base + pathDelay, base + turnaround,
                  base + turnaround + pathDelay);

    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 0);
    CHECK_EQ(ptptest::state().offsetTimerCalls.size(), 0);
}

static void testCoarseOffsetStepsTheTimer()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;
    const NanoTime clockError = 50000;  // 50 us late, well past the 1 us band

    // The local clock is behind by clockError, so every local timestamp
    // (T2 and T3) is that much lower.
    for (int cycle = 0; cycle < 2; cycle++)
    {
        const NanoTime t1 = base + cycle * NS_PER_S;
        runSlaveCycle(ptp, (uint16_t)(cycle + 1), t1, t1 + pathDelay - clockError,
                      t1 + turnaround - clockError, t1 + turnaround + pathDelay);
    }

    CHECK_EQ(ptp.getOffset(), -clockError + HW_OFFSET);
    CHECK_EQ(ptptest::state().offsetTimerCalls.size(), 1);
    if (ptptest::state().offsetTimerCalls.size() == 1)
    {
        CHECK_EQ(ptptest::state().offsetTimerCalls[0], clockError - HW_OFFSET);
    }
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 0);
}

static void testPeerDelayCycle()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, true);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;
    const NanoTime residence = 1000;

    // The request goes out on the port's own interval, so the clock has to
    // move for the next one to be due: half of the default second here,
    // and a second between the cycles.
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
        ptp.feed(makeSync(seq), t2);
        ptp.feed(makeFollowUp(seq, t1), 0);
        setTxTimestamp(t3);
        ptp.update();  // sends Pdelay_Req and takes t3
        const uint16_t requestSeq = lastSentSequenceID(ptp);
        ptp.feed(makeResponse(3, requestSeq, t4), t6);
        ptp.feed(makePdelayRespFollowUp(requestSeq, t5), 0);
        ptp.update();
    }

    CHECK_EQ(ptp.getDelay(), pathDelay);
    // The peer-delay branch adds a fixed 500 ns instead of hwOffset.
    CHECK_EQ(ptp.getOffset(), 500);

    // Pdelay_Req is message type 2 and 54 bytes long.
    CHECK_EQ(ptp.sent.size(), 2);
    if (!ptp.sent.empty())
    {
        CHECK_EQ(ptp.sent[0].data[0], 2);
        CHECK_EQ(ptp.sent[0].data.size(), 54);
        CHECK_EQ(ptp.sent[0].general, false);
    }
}

// A path with transparent clocks in it, from the answering side.
//
// Every transparent clock a request crosses adds its residence time to the
// request's correctionField, and the requester takes that back out of the
// answer. An answer that returns zero there throws the whole accounting
// away, and the measured path delay is wrong by however long the request
// sat inside the switches. 1588 clause 11.3 for the Delay_Resp, 11.4.3 for
// the Pdelay_Resp.
static void testTheAnswerCarriesTheRequestsCorrection()
{
    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    const NanoTime residence = 1234;

    // End to end: a master answering a Delay_Req.
    {
        ptptest::state().reset();
        TestPTP ptp(true, false, false);
        ptp.begin();

        std::vector<uint8_t> request = makeDelayRequest(7, requester);
        putCorrection(request, residence);
        ptp.feed(request, 2000000000LL);

        CHECK_EQ(ptp.sent.size(), 1);
        if (ptp.sent.size() == 1)
        {
            const std::vector<uint8_t> &resp = ptp.sent[0].data;
            CHECK_EQ(resp[0], 9);
            CHECK_EQ(bufferToCorrection(resp.data()), residence);
            for (size_t i = 8; i < 16; i++)
            {
                CHECK_EQ(resp[i], request[i]);
            }
        }
    }

    // Peer to peer: the responder answering a Pdelay_Req.
    {
        ptptest::state().reset();
        TestPTP ptp(true, false, true);
        ptp.begin();

        std::vector<uint8_t> request = makePeerDelayRequest(9, requester);
        putCorrection(request, residence);
        ptp.feed(request, 3000000000LL);

        CHECK_EQ(ptp.sent.size(), 1);
        if (ptp.sent.size() == 1)
        {
            const std::vector<uint8_t> &resp = ptp.sent[0].data;
            CHECK_EQ(resp[0], 3);
            CHECK_EQ(bufferToCorrection(resp.data()), residence);
        }
    }
}

// The same path, from the asking side.
//
// The peer-delay answer arrives in two messages and both carry a
// correction: the Pdelay_Resp's was already taken out of T4, the
// Pdelay_Resp_Follow_Up's was ignored entirely. With switches in the path
// that made the link look longer than it is by their residence time.
static void testTheFollowUpsCorrectionLeavesTheDelay()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, true);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;
    const NanoTime residence = 1000;
    // What the switches keep of each half: the request sits 700 ns inside
    // them, the response 300 ns, and each declares its own.
    const NanoTime requestResidence = 700;
    const NanoTime responseResidence = 300;

    for (int cycle = 0; cycle < 2; cycle++)
    {
        const uint16_t seq = (uint16_t)(cycle + 1);
        const NanoTime t1 = base + cycle * NS_PER_S;
        const NanoTime t2 = t1 + pathDelay;
        const NanoTime t3 = t1 + turnaround;
        // The request spends the link plus its residence getting there,
        // and the response the same on the way back.
        const NanoTime t4 = t3 + pathDelay + requestResidence;
        const NanoTime t5 = t4 + residence;
        const NanoTime t6 = t5 + pathDelay + responseResidence;

        ptp.feed(makeSync(seq), t2);
        ptp.feed(makeFollowUp(seq, t1), 0);
        setTxTimestamp(t3);
        ptp.update();
        const uint16_t requestSeq = lastSentSequenceID(ptp);

        // The timestamps are the responder's own; what the switches kept
        // travels in the correctionField of each half. The Pdelay_Resp
        // carries the request's, copied over as 1588 asks.
        std::vector<uint8_t> response = makeResponse(3, requestSeq, t4);
        putCorrection(response, requestResidence);
        ptp.feed(response, t6);

        std::vector<uint8_t> followUp = makePdelayRespFollowUp(requestSeq, t5);
        putCorrection(followUp, responseResidence);
        ptp.feed(followUp, 0);
        ptp.update();
    }

    // Both corrections accounted for, so the link measures what it is.
    CHECK_EQ(ptp.getDelay(), pathDelay);
}

// The link, not the hierarchy.
//
// A Pdelay_Req is answered by whatever the port happens to be: the
// exchange measures the wire between two ports, and the peer asking for
// it may be the master, the slave or neither. Only a master used to
// answer, so a peer talking to a slave-only port could never measure the
// link.
static void testAnyPortAnswersAPeerDelayRequest()
{
    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};

    struct Role { bool master; bool slave; };
    const Role roles[] = {{true, false}, {false, true}, {true, true}};

    for (const Role &role : roles)
    {
        ptptest::state().reset();
        TestPTP ptp(role.master, role.slave, true);
        ptp.begin();

        ptp.feed(makePeerDelayRequest(4, requester), 3000000000LL);

        CHECK_EQ(ptp.sent.size(), 1);
        if (ptp.sent.size() == 1)
        {
            const std::vector<uint8_t> &resp = ptp.sent[0].data;
            CHECK_EQ(resp[0], 3);            // Pdelay_Resp
            CHECK_EQ(resp.size(), 54);
            CHECK_EQ(resp[6], 2);            // twoStepFlag
            CHECK_EQ((resp[30] << 8) | resp[31], 4);
            for (size_t i = 0; i < 10; i++)
            {
                CHECK_EQ(resp[44 + i], requester[i]);
            }
        }
    }
}

// The numbers a profile fixes, checked where they land: on the wire.
//
// Setting them one at a time was always possible; what was missing was the
// combination each ecosystem expects. A profile that changed a member and
// never reached the header would be worth nothing, so this reads them back
// out of the messages the port generates.
static void testAProfileReachesTheWire()
{
    struct Case
    {
        PTPBase::ProfileSettings profile;
        int8_t sync;
        int8_t announce;
        int8_t delayReq;
        uint8_t sdoId;
    };
    // The three combinations this library used to hold its own table of. They
    // live in packages/aes67-profiles now; the numbers are written out here so
    // that this test still fails if applyProfile() stops applying what it is
    // handed, which is the only thing left for it to get wrong.
    const Case cases[] = {
        {{0, 0, 0, 1, 0}, 0, 1, 0, 0},    // IEEE 1588-2008 default
        {{0, 0, -3, 0, -3}, -3, 0, -3, 0}, // the AES67 media profile
        {{0, 1, -3, 0, 0}, -3, 0, 0, 1},   // 802.1AS
    };

    for (const Case &c : cases)
    {
        ptptest::state().reset();
        TestPTP ptp(true, false, false);
        ptp.begin();
        ptp.applyProfile(c.profile);

        CHECK_EQ(ptp.getLogSyncInterval(), c.sync);
        CHECK_EQ(ptp.getLogAnnounceInterval(), c.announce);
        CHECK_EQ(ptp.getLogMinDelayReqInterval(), c.delayReq);
        CHECK_EQ(ptp.getMajorSdoId(), c.sdoId);

        // The Announce carries its own interval, and the sdoId in the top
        // nibble of the first octet.
        ptp.sent.clear();
        ptp.announceMessage();
        CHECK_EQ(ptp.sent.size(), 1);
        if (ptp.sent.size() == 1)
        {
            const std::vector<uint8_t> &announce = ptp.sent[0].data;
            CHECK_EQ(announce[0] >> 4, c.sdoId);
            CHECK_EQ((int8_t)announce[33], c.announce);
        }

        // The Delay_Resp carries the rate this master asks its slaves for.
        // The request has to arrive under the same majorSdoId or the port
        // drops it as another profile's traffic, which is what that field
        // is for.
        const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
        std::vector<uint8_t> request = makeDelayRequest(3, requester);
        request[0] = (uint8_t)((c.sdoId << 4) | 1);
        ptp.sent.clear();
        ptp.feed(request, 2000000000LL);
        CHECK_EQ(ptp.sent.size(), 1);
        if (ptp.sent.size() == 1)
        {
            CHECK_EQ((int8_t)ptp.sent[0].data[33], c.delayReq);
        }
    }
}

static void testSlaveDoesNotAnswerDelayRequests()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    ptp.feed(makeDelayRequest(7, requester), 2000000000LL);

    CHECK_EQ(ptp.sent.size(), 0);
}

static void testMasterAnswersDelayRequest()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    const NanoTime recv = 2000000000LL;
    ptp.feed(makeDelayRequest(7, requester), recv);

    CHECK_EQ(ptp.sent.size(), 1);
    if (ptp.sent.size() != 1)
    {
        return;
    }
    const std::vector<uint8_t> &resp = ptp.sent[0].data;
    CHECK_EQ(resp.size(), 54);
    CHECK_EQ(resp[0], 9);   // Delay_Resp
    CHECK_EQ(resp[1], 2);   // versionPTP
    CHECK_EQ((resp[2] << 8) | resp[3], 54);
    CHECK_EQ(resp[32], 3);  // controlField
    CHECK_EQ((resp[30] << 8) | resp[31], 7);  // the request's sequence ID
    CHECK_EQ(ptp.sent[0].general, true);

    // receiveTimestamp is the arrival time plus the hardware offset.
    CHECK_EQ(bufferToNanoTime(resp.data()), recv + HW_OFFSET);

    // requestingPortIdentity echoes the requester's source port identity.
    for (size_t i = 0; i < 10; i++)
    {
        CHECK_EQ(resp[44 + i], requester[i]);
    }

    // sourcePortIdentity is our own clock ID.
    uint8_t id[8];
    expectedClockID(id);
    for (size_t i = 0; i < 8; i++)
    {
        CHECK_EQ(resp[20 + i], id[i]);
    }
}

static void testMasterSyncAndFollowUp()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();

    const NanoTime txTime = 3000000000LL;
    setTxTimestamp(txTime);
    ptp.syncMessage();
    // The Sync goes out at once and its Follow_Up when the timestamp has
    // been collected, which is from update().
    CHECK_EQ(ptp.sent.size(), 1);
    ptp.update();

    CHECK_EQ(ptptest::state().timestampNextFrameCalls, 1);
    CHECK_EQ(ptp.sent.size(), 2);
    if (ptp.sent.size() != 2)
    {
        return;
    }

    const std::vector<uint8_t> &sync = ptp.sent[0].data;
    CHECK_EQ(sync.size(), 44);
    CHECK_EQ(sync[0], 0);      // Sync
    CHECK_EQ(sync[6], 0x02);   // twoStepFlag
    CHECK_EQ(sync[32], 0);     // controlField
    CHECK_EQ(ptp.sent[0].general, false);

    const std::vector<uint8_t> &followUp = ptp.sent[1].data;
    CHECK_EQ(followUp.size(), 44);
    CHECK_EQ(followUp[0], 8);  // Follow_Up
    CHECK_EQ(followUp[32], 2); // controlField
    CHECK_EQ(ptp.sent[1].general, true);

    // Both carry the same sequence ID, and the Follow_Up publishes the
    // transmit timestamp corrected by the hardware offset.
    CHECK_EQ((sync[30] << 8) | sync[31], 0);
    CHECK_EQ((followUp[30] << 8) | followUp[31], 0);
    CHECK_EQ(bufferToNanoTime(followUp.data()), txTime + HW_OFFSET);

    // The next exchange uses the next sequence ID.
    setTxTimestamp(txTime + NS_PER_S);
    ptp.syncMessage();
    ptp.update();
    CHECK_EQ(ptp.sent.size(), 4);
    if (ptp.sent.size() == 4)
    {
        CHECK_EQ((ptp.sent[2].data[30] << 8) | ptp.sent[2].data[31], 1);
        CHECK_EQ((ptp.sent[3].data[30] << 8) | ptp.sent[3].data[31], 1);
    }
}

static void testMissingTxTimestampSkipsTheFollowUp()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();

    // The hardware never posts the timestamp.
    ptptest::state().txAvailable = false;
    ptp.syncMessage();
    ptp.update();

    // The Sync went out and the Follow_Up did not.
    CHECK_EQ(ptp.sent.size(), 1);
    if (!ptp.sent.empty())
    {
        CHECK_EQ(ptp.sent[0].data[0], 0);
    }

    // And nothing spun waiting for it: micros() is only read inside the
    // wait loop, and the wait loop is not entered at all any more. It
    // used to run for a millisecond every time the hardware went quiet,
    // which at eight Sync a second is eight milliseconds of loop() a
    // second with no packets being drained.
    CHECK_EQ(ptptest::state().microsNow, 0);

    // Past the deadline the exchange is given up, so the next Sync is not
    // held back by it.
    ptptest::state().millisNow += SYNC_FOLLOW_UP_TIMEOUT_MS + 1;
    ptp.update();
    setTxTimestamp(3000000000LL);
    ptp.syncMessage();
    ptp.update();
    CHECK_EQ(ptp.sent.size(), 3);
    if (ptp.sent.size() == 3)
    {
        CHECK_EQ(ptp.sent[1].data[0], 0);  // the second Sync
        CHECK_EQ(ptp.sent[2].data[0], 8);  // and its Follow_Up
    }
}

static void testSlaveMissingTxTimestampSkipsTheExchange()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    ptp.feed(makeSync(1), base + 10000);
    ptp.feed(makeFollowUp(1, base), 0);
    ptptest::state().txAvailable = false;
    ptp.update();

    // The Delay_Req itself is out on the wire, but with no T3 the cycle
    // is abandoned rather than completed with an invented timestamp.
    CHECK_EQ(ptp.sent.size(), 1);
    ptp.feed(makeResponse(9, lastSentSequenceID(ptp), base + 500000000LL), 0);
    ptp.update();
    CHECK_EQ(ptp.getOffset(), 0);
    CHECK_EQ(ptp.getDelay(), 0);
}

static void testAnnounceDataset()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();

    ptp.setClockClass(6);
    ptp.setClockAccuracy(0x21);
    ptp.setOffsetScaledLogVariance(0x4E5D);
    ptp.setPriority1(64);
    ptp.setPriority2(65);
    ptp.setTimeSource(0x20);  // GPS
    ptp.setCurrentUtcOffset(37);
    ptp.setUtcOffsetValid(true);
    ptp.setLogAnnounceInterval(1);

    ptp.announceMessage();

    CHECK_EQ(ptp.sent.size(), 1);
    if (ptp.sent.size() != 1)
    {
        return;
    }
    const std::vector<uint8_t> &a = ptp.sent[0].data;
    CHECK_EQ(a.size(), 64);
    CHECK_EQ(a[0], 11);  // Announce
    CHECK_EQ(a[1], 2);
    CHECK_EQ((a[2] << 8) | a[3], 64);
    CHECK_EQ(a[7], 0x08 | 0x04);  // PTPTimescale and currentUtcOffsetValid
    CHECK_EQ(a[32], 5);           // controlField
    CHECK_EQ((int8_t)a[33], 1);   // logAnnounceInterval
    CHECK_EQ((int16_t)((a[44] << 8) | a[45]), 37);
    CHECK_EQ(a[47], 64);          // priority1
    CHECK_EQ(a[48], 6);           // clockClass
    CHECK_EQ(a[49], 0x21);        // clockAccuracy
    CHECK_EQ((a[50] << 8) | a[51], 0x4E5D);
    CHECK_EQ(a[52], 65);          // priority2
    CHECK_EQ(a[63], 0x20);        // timeSource
    CHECK_EQ(ptp.sent[0].general, true);

    uint8_t id[8];
    expectedClockID(id);
    for (size_t i = 0; i < 8; i++)
    {
        CHECK_EQ(a[53 + i], id[i]);  // grandmasterIdentity
    }

    // The flag drops back when the offset is declared unusable.
    ptp.setUtcOffsetValid(false);
    ptp.announceMessage();
    CHECK_EQ(ptp.sent.size(), 2);
    if (ptp.sent.size() == 2)
    {
        CHECK_EQ(ptp.sent[1].data[7], 0x08);
        // Announce carries its own sequence counter.
        CHECK_EQ((ptp.sent[1].data[30] << 8) | ptp.sent[1].data[31], 1);
    }
}

static void testAnnounceAndSyncNeedTheMasterRole()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    ptp.announceMessage();
    ptp.syncMessage();
    CHECK_EQ(ptp.sent.size(), 0);

    // And nothing is sent before begin().
    TestPTP notStarted(true, false, false);
    notStarted.announceMessage();
    notStarted.syncMessage();
    CHECK_EQ(notStarted.sent.size(), 0);
}

static void testPpsRequiresTheMasterRole()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    ptp.ppsInterruptTriggered(base, base + 300);
    ptp.ppsInterruptTriggered(base + NS_PER_S, base + NS_PER_S + 300);
    ptp.update();

    CHECK_EQ(ptp.getOffset(), 0);
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 0);
}

static void testPpsDrivesTheServoOnAMaster()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime localError = 300;

    ptp.ppsInterruptTriggered(base, base + localError);
    ptp.update();
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 0);

    ptp.ppsInterruptTriggered(base + NS_PER_S, base + NS_PER_S + localError);
    ptp.update();

    // The PPS path measures the offset with no path delay at all.
    CHECK_EQ(ptp.getDelay(), 0);
    CHECK_EQ(ptp.getOffset(), localError);
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 1);
}

static void testTheHardwareClockIsZeroedOnceOnly()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();

    CHECK_EQ(ptptest::state().writeTimerCalls.size(), 1);
    if (!ptptest::state().writeTimerCalls.empty())
    {
        CHECK_EQ(ptptest::state().writeTimerCalls[0].tv_sec, 0);
        CHECK_EQ(ptptest::state().writeTimerCalls[0].tv_nsec, 0);
    }

    // Tuning a gain resets the servo state. The clock is not servo state:
    // it used to be thrown back to the epoch by either setter.
    ptp.setKp(0.5);
    ptp.setKi(0.25);
    ptp.reset();
    CHECK_EQ(ptptest::state().writeTimerCalls.size(), 1);

    // Neither is a port brought back up after the link dropped.
    ptp.end();
    ptp.begin();
    CHECK_EQ(ptptest::state().writeTimerCalls.size(), 1);
}

// ------------------------------------------------- regressions, batch one

static void testDelayResponseForAnotherRequestIsIgnored()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();
    ptp.setDelayFilterLength(1);

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;

    runSlaveCycle(ptp, 1, base, base + pathDelay, base + turnaround,
                  base + turnaround + pathDelay);
    CHECK_EQ(ptp.getDelay(), pathDelay);

    // Second exchange, answered with a sequence ID that is not the one the
    // Delay_Req went out with.
    const NanoTime second = base + NS_PER_S;
    const NanoTime longer = 30000;
    ptp.feed(makeSync(2), second + longer);
    ptp.feed(makeFollowUp(2, second), 0);
    setTxTimestamp(second + turnaround);
    ptp.update();
    const uint16_t wrongSequence = (uint16_t)(lastSentSequenceID(ptp) + 7);
    ptp.feed(makeResponse(9, wrongSequence, second + turnaround + longer), 0);
    ptp.update();

    CHECK_EQ(ptp.getDelay(), pathDelay);
}

static void testDelayResponseForAnotherPortNumberIsIgnored()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();
    ptp.setDelayFilterLength(1);

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;

    runSlaveCycle(ptp, 1, base, base + pathDelay, base + turnaround,
                  base + turnaround + pathDelay);

    // Right clock identity, right sequence, wrong port of that clock.
    const NanoTime second = base + NS_PER_S;
    const NanoTime longer = 30000;
    ptp.feed(makeSync(2), second + longer);
    ptp.feed(makeFollowUp(2, second), 0);
    setTxTimestamp(second + turnaround);
    ptp.update();
    std::vector<uint8_t> response = makeResponse(9, lastSentSequenceID(ptp),
                                                 second + turnaround + longer);
    response[53] = 2;
    ptp.feed(response, 0);
    ptp.update();

    CHECK_EQ(ptp.getDelay(), pathDelay);
}

static void testDuplicateDelayResponseIsIgnored()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;

    runSlaveCycle(ptp, 1, base, base + pathDelay, base + turnaround,
                  base + turnaround + pathDelay);

    const NanoTime second = base + NS_PER_S;
    ptp.feed(makeSync(2), second + pathDelay);
    ptp.feed(makeFollowUp(2, second), 0);
    setTxTimestamp(second + turnaround);
    ptp.update();

    const uint16_t requestSequence = lastSentSequenceID(ptp);
    ptp.feed(makeResponse(9, requestSequence, second + turnaround + pathDelay), 0);
    // A second copy of the same answer, carrying a timestamp that would
    // double the measured path delay.
    ptp.feed(makeResponse(9, requestSequence, second + turnaround + 3 * pathDelay), 0);
    ptp.update();

    CHECK_EQ(ptp.getDelay(), pathDelay);
    CHECK_EQ(ptp.getOffset(), HW_OFFSET);
}

static void testSyncSequenceAdvancesWithoutATxTimestamp()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();

    // First exchange: the hardware never posts the timestamp.
    ptptest::state().txAvailable = false;
    ptp.syncMessage();
    ptptest::state().millisNow += SYNC_FOLLOW_UP_TIMEOUT_MS + 1;
    ptp.update();
    CHECK_EQ(ptp.sent.size(), 1);

    // Second exchange, this time with a timestamp. It must not reuse the
    // sequence ID the abandoned Sync already put on the wire.
    setTxTimestamp(3000000000LL);
    ptp.syncMessage();
    ptp.update();
    CHECK_EQ(ptp.sent.size(), 3);
    if (ptp.sent.size() != 3)
    {
        return;
    }
    CHECK_EQ((ptp.sent[0].data[30] << 8) | ptp.sent[0].data[31], 0);
    CHECK_EQ((ptp.sent[1].data[30] << 8) | ptp.sent[1].data[31], 1);  // Sync
    CHECK_EQ((ptp.sent[2].data[30] << 8) | ptp.sent[2].data[31], 1);  // its Follow_Up
    CHECK_EQ(ptp.sent[2].data[0], 8);
}

static void testResetClearsTheMeasurement()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;
    // A local clock 200 ns ahead cancels the hardware offset exactly, so
    // the servo reports a locked clock.
    const NanoTime local = 200;

    for (int cycle = 0; cycle < 3; cycle++)
    {
        const NanoTime t1 = base + cycle * NS_PER_S;
        runSlaveCycle(ptp, (uint16_t)(cycle + 1), t1, t1 + pathDelay + local,
                      t1 + turnaround + local, t1 + turnaround + pathDelay);
    }

    CHECK_EQ(ptp.getOffset(), 0);
    CHECK_EQ(ptp.getDelay(), pathDelay);
    CHECK(ptp.getLockCount() > 0);

    // A gain is the loop's own business: it clears the loop, and leaves
    // the measurement and the chosen master where they are.
    ptp.setKp(1.0);
    CHECK_EQ(ptp.getLockCount(), 0);
    CHECK_EQ(ptp.getDelay(), pathDelay);

    // A reset is the whole port, and takes the measurement with it.
    ptp.reset();
    CHECK_EQ(ptp.getLockCount(), 0);
    CHECK_EQ(ptp.getOffset(), 0);
    CHECK_EQ(ptp.getDelay(), 0);
}

static void testResponseToAnAbandonedRequestIsRefused()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();
    ptp.setDelayFilterLength(1);

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;
    const NanoTime longer = 30000;

    runSlaveCycle(ptp, 1, base, base + pathDelay, base + turnaround,
                  base + turnaround + pathDelay);
    CHECK_EQ(ptp.getDelay(), pathDelay);

    // Second exchange on a longer path: the Delay_Req goes out but its
    // transmit timestamp never arrives, so there is no T3 to pair with.
    const NanoTime second = base + NS_PER_S;
    ptp.feed(makeSync(2), second + longer);
    ptp.feed(makeFollowUp(2, second), 0);
    ptptest::state().txAvailable = false;
    ptp.update();
    ptp.feed(makeResponse(9, lastSentSequenceID(ptp), second + turnaround + longer), 0);
    ptp.update();
    CHECK_EQ(ptp.getDelay(), pathDelay);

    // The request that never got its T3 holds the timestamp register until
    // its deadline, and is given up there.
    ptptest::state().millisNow += DELAY_REQUEST_TIMEOUT_MS + 1;
    ptp.update();

    // Third exchange, complete, on that longer path: the measurement is
    // its own, not a T4 left over from the exchange that was dropped.
    const NanoTime third = base + 2 * NS_PER_S;
    runSlaveCycle(ptp, 3, third, third + longer, third + turnaround,
                  third + turnaround + longer);
    CHECK_EQ(ptp.getDelay(), longer);
}

static void testNewSyncBlocksTheOvertakenCycle()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;

    runSlaveCycle(ptp, 1, base, base + pathDelay, base + turnaround,
                  base + turnaround + pathDelay);
    const size_t corrections = ptptest::state().adjustFreqCalls.size();
    const NanoTime offsetBefore = ptp.getOffset();

    const NanoTime second = base + NS_PER_S;
    ptp.feed(makeSync(2), second + pathDelay);
    ptp.feed(makeFollowUp(2, second), 0);
    setTxTimestamp(second + turnaround);

    // A newer Sync starts before this pair has been acted on: T1 and T2 no
    // longer belong with each other, so no correction comes of them.
    ptp.feed(makeSync(3), second + NS_PER_S + pathDelay);
    ptp.update();
    ptp.feed(makeResponse(9, lastSentSequenceID(ptp), second + turnaround + pathDelay), 0);
    ptp.update();

    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), corrections);
    CHECK_EQ(ptp.getOffset(), offsetBefore);
}

static void testPeerFollowUpBeforeItsResponseIsIgnored()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, true);
    ptp.begin();
    ptp.setDelayFilterLength(1);

    const NanoTime base = 1000000000LL;
    const NanoTime turnaround = 500000000LL;
    const NanoTime residence = 1000;
    const NanoTime paths[2] = {10000, 30000};

    for (int cycle = 0; cycle < 2; cycle++)
    {
        const NanoTime pathDelay = paths[cycle];
        const NanoTime t1 = base + cycle * NS_PER_S;
        const NanoTime t3 = t1 + turnaround;
        const NanoTime t4 = t3 + pathDelay;
        const NanoTime t5 = t4 + residence;
        const NanoTime t6 = t5 + pathDelay;

        ptp.feed(makeSync((uint16_t)(cycle + 1)), t1 + pathDelay);
        ptp.feed(makeFollowUp((uint16_t)(cycle + 1), t1), 0);
        setTxTimestamp(t3);
        ptp.update();

        const uint16_t requestSeq = lastSentSequenceID(ptp);
        if (cycle == 0)
        {
            ptp.feed(makeResponse(3, requestSeq, t4), t6);
            ptp.feed(makePdelayRespFollowUp(requestSeq, t5), 0);
        }
        else
        {
            // The Follow_Up arrives first, with nothing to follow: T5 is
            // not taken from it, so this exchange measures nothing.
            ptp.feed(makePdelayRespFollowUp(requestSeq, t5), 0);
            ptp.feed(makeResponse(3, requestSeq, t4), t6);
        }
        ptp.update();
    }

    CHECK_EQ(ptp.getDelay(), paths[0]);
}

static void testClockIdentityIsTheEui64Mapping()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();
    ptp.announceMessage();

    CHECK_EQ(ptp.sent.size(), 1);
    if (ptp.sent.empty())
    {
        return;
    }
    const std::vector<uint8_t> &a = ptp.sent[0].data;
    const uint8_t *mac = ptptest::state().mac;
    // sourcePortIdentity: MAC[0..2], FF FE, MAC[3..5].
    CHECK_EQ(a[20], mac[0]);
    CHECK_EQ(a[21], mac[1]);
    CHECK_EQ(a[22], mac[2]);
    CHECK_EQ(a[23], 0xFF);
    CHECK_EQ(a[24], 0xFE);
    CHECK_EQ(a[25], mac[3]);
    CHECK_EQ(a[26], mac[4]);
    CHECK_EQ(a[27], mac[5]);
}

static void testOtherDomainsAreIgnored()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    std::vector<uint8_t> sync = makeSync(1);
    sync[4] = 1;  // domainNumber 1, this port is on 0
    std::vector<uint8_t> followUp = makeFollowUp(1, base);
    followUp[4] = 1;
    ptp.feed(sync, base + 10000);
    ptp.feed(followUp, 0);
    setTxTimestamp(base + 500000000LL);
    ptp.update();
    CHECK_EQ(ptp.sent.size(), 0);

    // Moved to domain 1, the same pair is ours.
    ptp.setDomainNumber(1);
    ptp.feed(sync, base + 10000);
    ptp.feed(followUp, 0);
    ptp.update();
    CHECK_EQ(ptp.sent.size(), 1);
    if (!ptp.sent.empty())
    {
        CHECK_EQ(ptp.sent[0].data[4], 1);  // and what goes out says so
    }
}

static void testOneStepSyncIsAccepted()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;

    // No Follow_Up anywhere in this exchange.
    for (int cycle = 0; cycle < 2; cycle++)
    {
        const NanoTime t1 = base + cycle * NS_PER_S;
        ptp.feed(makeOneStepSync((uint16_t)(cycle + 1), t1), t1 + pathDelay);
        setTxTimestamp(t1 + turnaround);
        ptp.update();
        ptp.feed(makeResponse(9, lastSentSequenceID(ptp), t1 + turnaround + pathDelay), 0);
        ptp.update();
    }

    CHECK_EQ(ptp.getDelay(), pathDelay);
    CHECK_EQ(ptp.getOffset(), HW_OFFSET);
}

static void testSequenceIdZeroIsAccepted()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;

    // A master that has just started counts from zero.
    runSlaveCycle(ptp, 0, base, base + pathDelay, base + turnaround,
                  base + turnaround + pathDelay);
    const NanoTime second = base + NS_PER_S;
    runSlaveCycle(ptp, 1, second, second + pathDelay, second + turnaround,
                  second + turnaround + pathDelay);

    CHECK_EQ(ptp.getDelay(), pathDelay);
    CHECK_EQ(ptp.getOffset(), HW_OFFSET);
}

static void testPeerDelayRequestIsAnswered()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, true);
    ptp.begin();

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    const NanoTime recv = 2000000000LL;
    const NanoTime tx = recv + 30000;
    setTxTimestamp(tx);
    ptp.feed(makePeerDelayRequest(9, requester), recv);
    // The response goes out during the parse; its Follow_Up waits for the
    // transmit timestamp, which update() collects without spinning.
    CHECK_EQ(ptp.sent.size(), 1);
    ptp.update();

    // The answer, its Follow_Up, and the Pdelay_Req this port sends on its
    // own account: peer delay is measured from both ends of a link.
    CHECK_EQ(ptp.sent.size(), 3);
    if (ptp.sent.size() != 3)
    {
        return;
    }
    CHECK_EQ(ptp.sent[2].data[0] & 0x0f, 2);

    const std::vector<uint8_t> &resp = ptp.sent[0].data;
    CHECK_EQ(resp.size(), 54);
    CHECK_EQ(resp[0], 3);      // Pdelay_Resp
    CHECK_EQ(resp[6], 0x02);   // twoStepFlag
    CHECK_EQ(resp[32], 5);     // controlField
    CHECK_EQ((resp[30] << 8) | resp[31], 9);
    CHECK_EQ(ptp.sent[0].general, false);
    CHECK_EQ(bufferToNanoTime(resp.data()), recv + HW_OFFSET);

    const std::vector<uint8_t> &followUp = ptp.sent[1].data;
    CHECK_EQ(followUp.size(), 54);
    CHECK_EQ(followUp[0], 10);  // Pdelay_Resp_Follow_Up
    CHECK_EQ((followUp[30] << 8) | followUp[31], 9);
    CHECK_EQ(ptp.sent[1].general, true);
    CHECK_EQ(bufferToNanoTime(followUp.data()), tx + HW_OFFSET);

    // Both name the port that asked.
    for (size_t i = 0; i < 10; i++)
    {
        CHECK_EQ(resp[44 + i], requester[i]);
        CHECK_EQ(followUp[44 + i], requester[i]);
    }
}

static void testPeerDelayRequestNeedsTheP2pRole()
{
    ptptest::state().reset();
    TestPTP endToEnd(true, false, false);
    endToEnd.begin();

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    setTxTimestamp(2000030000LL);
    endToEnd.feed(makePeerDelayRequest(9, requester), 2000000000LL);
    CHECK_EQ(endToEnd.sent.size(), 0);

    // And with no transmit timestamp only the response goes out, never a
    // Follow_Up carrying a made-up departure time.
    ptptest::state().reset();
    TestPTP peer(true, false, true);
    peer.begin();
    ptptest::state().txAvailable = false;
    peer.feed(makePeerDelayRequest(9, requester), 2000000000LL);
    peer.update();
    ptptest::state().millisNow += PEER_FOLLOW_UP_TIMEOUT_MS + 1;
    peer.update();
    CHECK_EQ(countSentOfType(peer, 3), 1);
    CHECK_EQ(countSentOfType(peer, 10), 0);
}

static void testDelayRequestsFollowTheAnnouncedInterval()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;

    // Half of the default one-second interval.
    ptptest::state().randomValue = 500;

    ptp.feed(makeSync(1), base + pathDelay);
    ptp.feed(makeFollowUp(1, base), 0);
    setTxTimestamp(base + turnaround);
    ptp.update();
    CHECK_EQ(ptp.sent.size(), 1);

    // The answer says this master wants requests every two seconds.
    std::vector<uint8_t> response = makeResponse(9, lastSentSequenceID(ptp),
                                                 base + turnaround + pathDelay);
    response[33] = 1;  // logMinDelayReqInterval, 2 s
    ptp.feed(response, 0);
    ptp.update();

    // The next Sync comes before the scheduled instant: no request.
    const NanoTime second = base + NS_PER_S;
    ptp.feed(makeSync(2), second + pathDelay);
    ptp.feed(makeFollowUp(2, second), 0);
    setTxTimestamp(second + turnaround);
    ptp.update();
    CHECK_EQ(ptp.sent.size(), 1);

    // Once it passes, the next Sync does carry one.
    ptptest::state().millisNow = 500;
    const NanoTime third = base + 2 * NS_PER_S;
    ptp.feed(makeSync(3), third + pathDelay);
    ptp.feed(makeFollowUp(3, third), 0);
    setTxTimestamp(third + turnaround);
    ptp.update();
    CHECK_EQ(ptp.sent.size(), 2);

    // Drawn uniformly over twice the interval: one second at first, two
    // once the master had said what it wants.
    CHECK_EQ(ptptest::state().randomBounds.size(), 2);
    if (ptptest::state().randomBounds.size() == 2)
    {
        CHECK_EQ(ptptest::state().randomBounds[0], 2001);
        CHECK_EQ(ptptest::state().randomBounds[1], 4001);
    }
}

static void testNonsensicalIntervalIsNotAdopted()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    ptp.feed(makeSync(1), base + 10000);
    ptp.feed(makeFollowUp(1, base), 0);
    setTxTimestamp(base + 500000000LL);
    ptp.update();

    std::vector<uint8_t> response = makeResponse(9, lastSentSequenceID(ptp), base + 500010000LL);
    response[33] = 100;  // outside the range 1588 allows
    ptp.feed(response, 0);
    ptp.update();

    // Still the default one second, so the bound stays at twice that.
    const NanoTime second = base + NS_PER_S;
    ptp.feed(makeSync(2), second + 10000);
    ptp.feed(makeFollowUp(2, second), 0);
    setTxTimestamp(second + 500000000LL);
    ptp.update();
    CHECK_EQ(ptptest::state().randomBounds.size(), 2);
    if (ptptest::state().randomBounds.size() == 2)
    {
        CHECK_EQ(ptptest::state().randomBounds[1], 2001);
    }
}

static void testEndUndoesBegin()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();

    setTxTimestamp(3000000000LL);
    ptp.syncMessage();
    ptp.update();
    CHECK_EQ(ptp.sent.size(), 2);

    ptp.end();
    CHECK_EQ(ptp.closeSocketsCalls, 1);

    // A stopped port sends nothing.
    setTxTimestamp(4000000000LL);
    ptp.syncMessage();
    ptp.announceMessage();
    CHECK_EQ(ptp.sent.size(), 2);

    // end() on a stopped port is not a second teardown.
    ptp.end();
    CHECK_EQ(ptp.closeSocketsCalls, 1);

    // And it can be brought back up.
    ptp.begin();
    setTxTimestamp(5000000000LL);
    ptp.syncMessage();
    ptp.update();
    CHECK_EQ(ptp.sent.size(), 4);
}


// ----------------------------------------------- regressions, batch three

// Runs count exchanges one second apart, with the local clock a fixed
// amount away from the master's.
static void runSteadyOffsetCycles(TestPTP &ptp, int count, NanoTime localError)
{
    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;

    for (int cycle = 0; cycle < count; cycle++)
    {
        const NanoTime t1 = base + cycle * NS_PER_S;
        runSlaveCycle(ptp, (uint16_t)(cycle + 1), t1, t1 + pathDelay + localError,
                      t1 + turnaround + localError, t1 + turnaround + pathDelay);
    }
}

static void testIntegralTermIsBounded()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    // A gain that reaches the limit in a couple of exchanges, so the test
    // does not have to run for the days the default would take.
    ptp.setKi(100.0);

    // The local clock sits 500 ns behind and never catches up, which is
    // the shape of a fault: the offset stays put and the integral term
    // keeps adding it up.
    runSteadyOffsetCycles(ptp, 8, -500);

    const std::vector<double> &calls = ptptest::state().adjustFreqCalls;
    CHECK(calls.size() >= 6);
    for (size_t i = 0; i < calls.size(); i++)
    {
        CHECK(calls[i] <= 100000.0);
        CHECK(calls[i] >= -100000.0);
    }
    // And it settles at the limit rather than climbing past it.
    if (calls.size() >= 2)
    {
        CHECK_EQ((long long)calls[calls.size() - 1], 100000);
        CHECK_EQ((long long)calls[calls.size() - 2], 100000);
    }
}

static void testIntegralTermIsBoundedBelowZeroToo()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();
    ptp.setKi(100.0);

    // The same fault the other way round: the local clock runs ahead.
    runSteadyOffsetCycles(ptp, 8, 900);

    const std::vector<double> &calls = ptptest::state().adjustFreqCalls;
    CHECK(calls.size() >= 6);
    for (size_t i = 0; i < calls.size(); i++)
    {
        CHECK(calls[i] >= -100000.0);
    }
    if (!calls.empty())
    {
        CHECK_EQ((long long)calls[calls.size() - 1], -100000);
    }
}

static void testFrequencyModeAccumulatorIsBounded()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;
    // 50 us of rate error per second: past the 1 us that puts the servo in
    // frequency mode, short of the 100 us it calls a broken master.
    const NanoTime ratePerCycle = 50000;

    for (int cycle = 0; cycle < 6; cycle++)
    {
        const NanoTime t1 = base + cycle * NS_PER_S;
        const NanoTime local = cycle * ratePerCycle;
        runSlaveCycle(ptp, (uint16_t)(cycle + 1), t1, t1 + pathDelay + local,
                      t1 + turnaround + local, t1 + turnaround + pathDelay);
    }

    const std::vector<double> &calls = ptptest::state().adjustFreqCalls;
    CHECK(calls.size() >= 4);
    for (size_t i = 0; i < calls.size(); i++)
    {
        CHECK(calls[i] >= -100000.0);
        CHECK(calls[i] <= 100000.0);
    }
    // Three exchanges of 50 us each would have carried the accumulator to
    // 150 us without the bound.
    if (!calls.empty())
    {
        CHECK_EQ((long long)calls[calls.size() - 1], -100000);
    }
}

static void testNormalConvergenceIsNotClamped()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    // A hundred nanoseconds out with the stock gains: nowhere near the
    // limit, and the correction is the plain sum of the three terms.
    runSteadyOffsetCycles(ptp, 3, 100);

    const std::vector<double> &calls = ptptest::state().adjustFreqCalls;
    CHECK_EQ(calls.size(), 2);
    for (size_t i = 0; i < calls.size(); i++)
    {
        CHECK(calls[i] > -1000.0);
        CHECK(calls[i] < 1000.0);
    }
}


// ------------------------------------------------ regressions, batch four

static void testLockIsLostWhenSyncStops()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;
    const NanoTime local = 200;  // cancels the hardware offset, so it locks

    for (int cycle = 0; cycle < 3; cycle++)
    {
        ptptest::state().millisNow = (unsigned long)cycle * 1000;
        const NanoTime t1 = base + cycle * NS_PER_S;
        runSlaveCycle(ptp, (uint16_t)(cycle + 1), t1, t1 + pathDelay + local,
                      t1 + turnaround + local, t1 + turnaround + pathDelay);
    }

    CHECK(ptp.getLockCount() > 0);
    CHECK(ptp.isSyncReceiptValid());
    const NanoTime lockedOffset = ptp.getOffset();
    const NanoTime lockedDelay = ptp.getDelay();

    // Two intervals of silence: still inside the timeout.
    ptptest::state().millisNow = 2000 + 2000;
    ptp.update();
    CHECK(ptp.getLockCount() > 0);
    CHECK(ptp.isSyncReceiptValid());

    // Past three, the lock means nothing.
    ptptest::state().millisNow = 2000 + 3001;
    ptp.update();
    CHECK_EQ(ptp.getLockCount(), 0);
    CHECK(!ptp.isSyncReceiptValid());

    // The clock keeps the rate it learned and the last measurement it
    // took: no correction is applied on the way out.
    const size_t adjustments = ptptest::state().adjustFreqCalls.size();
    CHECK_EQ(ptp.getOffset(), lockedOffset);
    CHECK_EQ(ptp.getDelay(), lockedDelay);
    ptp.update();
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), adjustments);
}

static void testTimeoutFollowsTheAnnouncedSyncInterval()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;

    // A master announcing one Sync every four seconds.
    std::vector<uint8_t> sync = makeSync(1);
    sync[33] = 2;  // logSyncInterval 2, so 4 s
    ptp.feed(sync, base + 10000);
    CHECK(ptp.isSyncReceiptValid());

    // Ten seconds of silence would be over the default timeout of three
    // seconds, but not over three of this master's four-second intervals.
    ptptest::state().millisNow = 10000;
    ptp.update();
    CHECK(ptp.isSyncReceiptValid());

    ptptest::state().millisNow = 12001;
    ptp.update();
    CHECK(!ptp.isSyncReceiptValid());
}

static void testSyncAfterATimeoutStartsCleanly()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;

    runSlaveCycle(ptp, 1, base, base + pathDelay, base + turnaround,
                  base + turnaround + pathDelay);

    // The master disappears for a minute.
    ptptest::state().millisNow = 60000;
    ptp.update();
    CHECK(!ptp.isSyncReceiptValid());

    // When it comes back, the first exchange has nothing to be measured
    // against: the timestamps from before the gap were dropped.
    const NanoTime later = base + 60 * NS_PER_S;
    runSlaveCycle(ptp, 2, later, later + pathDelay, later + turnaround,
                  later + turnaround + pathDelay);
    CHECK(ptp.isSyncReceiptValid());
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 0);

    // The one after it is measured normally.
    const NanoTime after = later + NS_PER_S;
    runSlaveCycle(ptp, 3, after, after + pathDelay, after + turnaround,
                  after + turnaround + pathDelay);
    CHECK_EQ(ptp.getDelay(), pathDelay);
    CHECK_EQ(ptp.getOffset(), HW_OFFSET);
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 1);
}

static void testMasterDoesNotTimeOutOnItsOwnSilence()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    ptp.ppsInterruptTriggered(base, base + 200);
    ptp.update();
    ptp.ppsInterruptTriggered(base + NS_PER_S, base + NS_PER_S + 200);
    ptp.update();
    const int locked = ptp.getLockCount();

    // A grandmaster receives no Sync by definition. An hour of it must not
    // take its lock away.
    ptptest::state().millisNow = 3600000;
    ptp.update();
    CHECK_EQ(ptp.getLockCount(), locked);
}

static void testAnnounceCarriesTheWholeDataset()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();

    ptp.setUtcOffsetValid(true);
    ptp.setLeap61(true);
    ptp.setTimeTraceable(true);
    ptp.setFrequencyTraceable(true);
    ptp.setStepsRemoved(2);
    ptp.announceMessage();

    CHECK_EQ(ptp.sent.size(), 1);
    if (ptp.sent.empty())
    {
        return;
    }
    const std::vector<uint8_t> &a = ptp.sent[0].data;
    // PTPTimescale, currentUtcOffsetValid, leap61, timeTraceable,
    // frequencyTraceable.
    CHECK_EQ(a[7], 0x08 | 0x04 | 0x01 | 0x10 | 0x20);
    CHECK_EQ((a[61] << 8) | a[62], 2);

    // leap59 is the other end of the same day, and never both at once in
    // any sane dataset -- but the library announces what it is told.
    ptp.setLeap61(false);
    ptp.setLeap59(true);
    ptp.setTimeTraceable(false);
    ptp.setFrequencyTraceable(false);
    ptp.setStepsRemoved(0);
    ptp.announceMessage();
    CHECK_EQ(ptp.sent.size(), 2);
    if (ptp.sent.size() == 2)
    {
        CHECK_EQ(ptp.sent[1].data[7], 0x08 | 0x04 | 0x02);
        CHECK_EQ((ptp.sent[1].data[61] << 8) | ptp.sent[1].data[62], 0);
    }
}


// ------------------------------------------------ regressions, batch five

// One exchange with the path delay given, symmetric in both directions.
static void runCycleWithDelay(TestPTP &ptp, uint16_t sequenceID, NanoTime t1, NanoTime pathDelay)
{
    const NanoTime turnaround = 500000000LL;
    runSlaveCycle(ptp, sequenceID, t1, t1 + pathDelay, t1 + turnaround,
                  t1 + turnaround + pathDelay);
}

static void testDelayIsTheMinimumOfTheWindow()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    runCycleWithDelay(ptp, 1, base, 10000);
    runCycleWithDelay(ptp, 2, base + NS_PER_S, 10000);
    CHECK_EQ(ptp.getDelay(), 10000);

    // A switch queues this one for another 20 us. The path did not
    // change, so neither does the delay.
    runCycleWithDelay(ptp, 3, base + 2 * NS_PER_S, 30000);
    CHECK_EQ(ptp.getDelay(), 10000);
    runCycleWithDelay(ptp, 4, base + 3 * NS_PER_S, 50000);
    CHECK_EQ(ptp.getDelay(), 10000);
}

static void testDelayFollowsARealChangeOnceTheWindowTurnsOver()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    runCycleWithDelay(ptp, 1, base, 10000);
    runCycleWithDelay(ptp, 2, base + NS_PER_S, 10000);
    CHECK_EQ(ptp.getDelay(), 10000);

    // The route really is longer now. Eight exchanges later the short
    // sample has aged out of the window and the new path is the minimum.
    for (size_t i = 0; i < 8; i++)
    {
        runCycleWithDelay(ptp, (uint16_t)(3 + i),
                          base + static_cast<NanoTime>(3 + i) * NS_PER_S, 30000);
    }
    CHECK_EQ(ptp.getDelay(), 30000);
}

static void testDelayFilterCanBeTurnedOff()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();
    ptp.setDelayFilterLength(1);

    const NanoTime base = 1000000000LL;
    runCycleWithDelay(ptp, 1, base, 10000);
    runCycleWithDelay(ptp, 2, base + NS_PER_S, 10000);
    CHECK_EQ(ptp.getDelay(), 10000);

    // With a window of one, every exchange is the measurement.
    runCycleWithDelay(ptp, 3, base + 2 * NS_PER_S, 30000);
    CHECK_EQ(ptp.getDelay(), 30000);
}

static void testNegativeDelayIsNotStored()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime turnaround = 500000000LL;
    runCycleWithDelay(ptp, 1, base, 10000);
    runCycleWithDelay(ptp, 2, base + NS_PER_S, 10000);
    CHECK_EQ(ptp.getDelay(), 10000);

    // A local clock a millisecond ahead of where the timestamps say it
    // should be: the arithmetic gives a path of minus half a millisecond.
    const NanoTime third = base + 2 * NS_PER_S;
    runSlaveCycle(ptp, 3, third, third + 10000, third + turnaround + 1000000,
                  third + turnaround);
    CHECK_EQ(ptp.getDelay(), 10000);
}

static void testTimestampOffsetIsAParameter()
{
    ptptest::state().reset();
    TestPTP master(true, false, false);
    master.begin();
    master.setTimestampOffset(-500);

    const NanoTime tx = 3000000000LL;
    setTxTimestamp(tx);
    master.syncMessage();
    master.update();
    CHECK_EQ(master.sent.size(), 2);
    if (master.sent.size() == 2)
    {
        CHECK_EQ(bufferToNanoTime(master.sent[1].data.data()), tx - 500);
    }

    // And on the measuring side it is the term the offset carries.
    ptptest::state().reset();
    TestPTP slave(false, true, false);
    slave.begin();
    slave.setTimestampOffset(0);

    const NanoTime base = 1000000000LL;
    runCycleWithDelay(slave, 1, base, 10000);
    runCycleWithDelay(slave, 2, base + NS_PER_S, 10000);
    CHECK_EQ(slave.getOffset(), 0);
}

static void testPeerOffsetCorrectionIsAParameter()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, true);
    ptp.begin();
    ptp.setPeerOffsetCorrection(0);

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;
    const NanoTime residence = 1000;

    for (int cycle = 0; cycle < 2; cycle++)
    {
        const uint16_t seq = (uint16_t)(cycle + 1);
        const NanoTime t1 = base + cycle * NS_PER_S;
        const NanoTime t3 = t1 + turnaround;
        const NanoTime t4 = t3 + pathDelay;
        const NanoTime t5 = t4 + residence;
        const NanoTime t6 = t5 + pathDelay;

        ptp.feed(makeSync(seq), t1 + pathDelay);
        ptp.feed(makeFollowUp(seq, t1), 0);
        setTxTimestamp(t3);
        ptp.update();
        const uint16_t requestSeq = lastSentSequenceID(ptp);
        ptp.feed(makeResponse(3, requestSeq, t4), t6);
        ptp.feed(makePdelayRespFollowUp(requestSeq, t5), 0);
        ptp.update();
    }

    CHECK_EQ(ptp.getDelay(), pathDelay);
    CHECK_EQ(ptp.getOffset(), 0);  // 500 by default
}

static void testFrequencyGainDampsTheStep()
{
    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;
    const NanoTime ratePerCycle = 50000;

    double firstAdjustment[2] = {0, 0};
    const double gains[2] = {1.0, 0.5};

    for (int run = 0; run < 2; run++)
    {
        ptptest::state().reset();
        TestPTP ptp(false, true, false);
        ptp.begin();
        ptp.setKf(gains[run]);

        for (int cycle = 0; cycle < 2; cycle++)
        {
            const NanoTime t1 = base + cycle * NS_PER_S;
            const NanoTime local = cycle * ratePerCycle;
            runSlaveCycle(ptp, (uint16_t)(cycle + 1), t1, t1 + pathDelay + local,
                          t1 + turnaround + local, t1 + turnaround + pathDelay);
        }

        CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 1);
        if (!ptptest::state().adjustFreqCalls.empty())
        {
            firstAdjustment[run] = ptptest::state().adjustFreqCalls[0];
        }
    }

    // The default takes the whole measured rate error in one step.
    CHECK_EQ((long long)firstAdjustment[0], -50000);
    // Half the gain, half the step.
    CHECK_EQ((long long)firstAdjustment[1], -25000);
}


// ------------------------------------------------- regressions, batch six

static const uint8_t kMasterA[8] = {0xAA, 0xAA, 0xAA, 0xFF, 0xFE, 0x00, 0x00, 0x01};
static const uint8_t kMasterB[8] = {0xBB, 0xBB, 0xBB, 0xFF, 0xFE, 0x00, 0x00, 0x02};

// One exchange spoken by the master given, so the source identity of the
// Sync and its Follow_Up is that master's.
static void runCycleFrom(TestPTP &ptp, const uint8_t *identity, uint16_t sequenceID, NanoTime t1,
                         NanoTime pathDelay)
{
    const NanoTime turnaround = 500000000LL;
    std::vector<uint8_t> sync = makeSync(sequenceID);
    putSource(sync, identity);
    std::vector<uint8_t> followUp = makeFollowUp(sequenceID, t1);
    putSource(followUp, identity);

    ptp.feed(sync, t1 + pathDelay);
    ptp.feed(followUp, 0);
    setTxTimestamp(t1 + turnaround);
    ptp.update();
    std::vector<uint8_t> response = makeResponse(9, lastSentSequenceID(ptp),
                                                 t1 + turnaround + pathDelay);
    putSource(response, identity);
    ptp.feed(response, 0);
    ptp.update();
}

static void testTheBestMasterIsChosen()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();
    CHECK(!ptp.hasSelectedMaster());

    // A middling master first, then a better one.
    ptp.feed(makeAnnounce(1, kMasterA, /*priority1=*/200), 0);
    CHECK(ptp.hasSelectedMaster());
    CHECK_EQ(ptp.getSelectedMaster().priority1, 200);

    ptp.feed(makeAnnounce(1, kMasterB, /*priority1=*/100), 0);
    CHECK_EQ(ptp.getSelectedMaster().priority1, 100);
    CHECK_EQ(ptp.getSelectedMaster().grandmasterIdentity[0], 0xBB);

    // And it stays chosen when the worse one speaks again.
    ptp.feed(makeAnnounce(2, kMasterA, 200), 0);
    CHECK_EQ(ptp.getSelectedMaster().priority1, 100);
}

static void testTheDatasetDecidesWhenPriorityTies()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    // Same priority1, better clockClass wins.
    ptp.feed(makeAnnounce(1, kMasterA, 128, /*clockClass=*/248), 0);
    ptp.feed(makeAnnounce(1, kMasterB, 128, /*clockClass=*/6), 0);
    CHECK_EQ(ptp.getSelectedMaster().clockClass, 6);
    CHECK_EQ(ptp.getSelectedMaster().grandmasterIdentity[0], 0xBB);

    // Same class, fewer steps from the grandmaster wins.
    ptptest::state().reset();
    TestPTP hops(false, true, false);
    hops.begin();
    hops.feed(makeAnnounce(1, kMasterA, 128, 6, 128, /*stepsRemoved=*/3), 0);
    CHECK_EQ(hops.getSelectedMaster().stepsRemoved, 3);
    std::vector<uint8_t> closer = makeAnnounce(1, kMasterA, 128, 6, 128, /*stepsRemoved=*/1);
    // Same grandmaster, a different port announcing it.
    closer[20] = 0xCC;
    hops.feed(closer, 0);
    CHECK_EQ(hops.getSelectedMaster().stepsRemoved, 1);
}

static void testOnlyTheChosenMasterIsFollowed()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    ptp.feed(makeAnnounce(1, kMasterB, 100), 0);

    const NanoTime base = 1000000000LL;
    runCycleFrom(ptp, kMasterB, 1, base, 10000);
    runCycleFrom(ptp, kMasterB, 2, base + NS_PER_S, 10000);
    CHECK_EQ(ptp.getDelay(), 10000);
    CHECK_EQ(ptp.getOffset(), HW_OFFSET);

    // The other master on the segment sends its own Sync, half a second
    // out. Nothing of it is taken.
    const size_t adjustments = ptptest::state().adjustFreqCalls.size();
    const NanoTime elsewhere = base + 2 * NS_PER_S + 500000000LL;
    runCycleFrom(ptp, kMasterA, 3, elsewhere, 10000);
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), adjustments);
    CHECK_EQ(ptp.getOffset(), HW_OFFSET);
}

static void testSyncIsFollowedBeforeAnyAnnounceArrives()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    // A master that sends no Announce is still a master.
    const NanoTime base = 1000000000LL;
    runCycleFrom(ptp, kMasterA, 1, base, 10000);
    runCycleFrom(ptp, kMasterA, 2, base + NS_PER_S, 10000);
    CHECK(!ptp.hasSelectedMaster());
    CHECK_EQ(ptp.getDelay(), 10000);
    CHECK_EQ(ptp.getOffset(), HW_OFFSET);
}

static void testAnotherMasterTakesOverWhenTheChosenOneGoesQuiet()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    ptp.feed(makeAnnounce(1, kMasterB, 100), 0);
    CHECK_EQ(ptp.getSelectedMaster().priority1, 100);

    // The worse master keeps announcing, but while the better one is
    // still being heard it does not get a look in.
    ptptest::state().millisNow = 1000;
    ptp.feed(makeAnnounce(2, kMasterA, 200), 0);
    ptp.update();
    CHECK_EQ(ptp.getSelectedMaster().priority1, 100);

    // Three announce intervals with nothing from the chosen one and the
    // choice is released.
    ptptest::state().millisNow = 3001;
    ptp.update();
    CHECK(!ptp.hasSelectedMaster());

    // The one still talking takes over.
    ptp.feed(makeAnnounce(3, kMasterA, 200), 0);
    CHECK(ptp.hasSelectedMaster());
    CHECK_EQ(ptp.getSelectedMaster().priority1, 200);
}

static void testTheChosenMasterMayDowngradeItself()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    // A grandmaster locked to GPS, which then loses its reference and
    // says so. It is still the one being followed until something better
    // speaks up.
    ptp.feed(makeAnnounce(1, kMasterA, 128, /*clockClass=*/6), 0);
    CHECK_EQ(ptp.getSelectedMaster().clockClass, 6);
    ptp.feed(makeAnnounce(2, kMasterA, 128, /*clockClass=*/248), 0);
    CHECK_EQ(ptp.getSelectedMaster().clockClass, 248);
    CHECK_EQ(ptp.getSelectedMaster().grandmasterIdentity[0], 0xAA);
}

static void testDelayResponseFromAnotherMasterIsIgnored()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();
    ptp.setDelayFilterLength(1);

    ptp.feed(makeAnnounce(1, kMasterB, 100), 0);

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;
    const NanoTime longer = 30000;
    runCycleFrom(ptp, kMasterB, 1, base, pathDelay);
    CHECK_EQ(ptp.getDelay(), pathDelay);

    // Second exchange, answered by the wrong master with the right
    // sequence ID and the right requesting port.
    const NanoTime second = base + NS_PER_S;
    std::vector<uint8_t> sync = makeSync(2);
    putSource(sync, kMasterB);
    std::vector<uint8_t> followUp = makeFollowUp(2, second);
    putSource(followUp, kMasterB);
    ptp.feed(sync, second + longer);
    ptp.feed(followUp, 0);
    setTxTimestamp(second + turnaround);
    ptp.update();
    std::vector<uint8_t> response = makeResponse(9, lastSentSequenceID(ptp),
                                                 second + turnaround + longer);
    putSource(response, kMasterA);
    ptp.feed(response, 0);
    ptp.update();

    CHECK_EQ(ptp.getDelay(), pathDelay);
}


// ----------------------------------------------- regressions, batch seven

static void testEverySyncCorrectsTheClock()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;

    // One full exchange, so a path delay is known.
    runSlaveCycle(ptp, 1, base, base + pathDelay, base + turnaround,
                  base + turnaround + pathDelay);
    const size_t corrections = ptptest::state().adjustFreqCalls.size();

    // Now hold every Delay_Req back: half a second of pacing, and Sync
    // arriving every second.
    ptptest::state().randomValue = 500;
    ptptest::state().millisNow = 0;

    for (int cycle = 1; cycle < 4; cycle++)
    {
        const NanoTime t1 = base + cycle * NS_PER_S;
        ptp.feed(makeSync((uint16_t)(cycle + 1)), t1 + pathDelay);
        ptp.feed(makeFollowUp((uint16_t)(cycle + 1), t1), 0);
        ptp.update();
    }

    // Three Sync pairs, three corrections, on the delay measured before.
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), corrections + 3);
    CHECK_EQ(ptp.getDelay(), pathDelay);
    CHECK_EQ(ptp.getOffset(), HW_OFFSET);
}

static void testNoOffsetBeforeADelayIsKnown()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;

    // Sync pairs with no Delay_Req ever answered: there is no path delay,
    // so there is nothing to say about the offset.
    ptptest::state().txAvailable = false;
    for (int cycle = 0; cycle < 3; cycle++)
    {
        const NanoTime t1 = base + cycle * NS_PER_S;
        ptp.feed(makeSync((uint16_t)(cycle + 1)), t1 + pathDelay);
        ptp.feed(makeFollowUp((uint16_t)(cycle + 1), t1), 0);
        ptp.update();
    }

    CHECK_EQ(ptp.getOffset(), 0);
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 0);
}

static void testReleasingAMasterDoesNotOpenThePort()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    ptp.feed(makeAnnounce(1, kMasterB, 100), 0);
    const NanoTime base = 1000000000LL;
    runCycleFrom(ptp, kMasterB, 1, base, 10000);

    // The chosen master goes quiet and the choice is released.
    ptptest::state().millisNow = 3001;
    ptp.update();
    CHECK(!ptp.hasSelectedMaster());

    // Another master's Sync is still not followed: an Announce has been
    // heard on this port, so a master is chosen or nothing is.
    const size_t corrections = ptptest::state().adjustFreqCalls.size();
    runCycleFrom(ptp, kMasterA, 2, base + NS_PER_S, 10000);
    runCycleFrom(ptp, kMasterA, 3, base + 2 * NS_PER_S, 10000);
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), corrections);

    // Once it announces itself, it is followed.
    ptp.feed(makeAnnounce(2, kMasterA, 200), 0);
    CHECK(ptp.hasSelectedMaster());
    runCycleFrom(ptp, kMasterA, 4, base + 3 * NS_PER_S, 10000);
    runCycleFrom(ptp, kMasterA, 5, base + 4 * NS_PER_S, 10000);
    CHECK(ptptest::state().adjustFreqCalls.size() > corrections);
}

static void testOurOwnAnnounceIsIgnored()
{
    ptptest::state().reset();
    TestPTP ptp(true, true, false);
    ptp.begin();

    uint8_t own[8];
    expectedClockID(own);
    ptp.feed(makeAnnounce(1, own, /*priority1=*/0), 0);

    // A port that is master and slave at once must not become its own
    // reference, whatever its own dataset says.
    CHECK(!ptp.hasSelectedMaster());
}

static void testAPinnedMasterIsTheOnlyOneHeard()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();
    ptp.setMasterIdentity(kMasterA);

    // The better master on the segment is not the pinned one.
    ptp.feed(makeAnnounce(1, kMasterB, /*priority1=*/0), 0);
    CHECK(!ptp.hasSelectedMaster());

    ptp.feed(makeAnnounce(1, kMasterA, 200), 0);
    CHECK(ptp.hasSelectedMaster());
    CHECK_EQ(ptp.getSelectedMaster().grandmasterIdentity[0], 0xAA);

    // And the better one still cannot take it over.
    ptp.feed(makeAnnounce(2, kMasterB, 0), 0);
    CHECK_EQ(ptp.getSelectedMaster().grandmasterIdentity[0], 0xAA);

    // Unpinned, the choice is made on the dataset again.
    ptp.clearMasterIdentity();
    ptp.feed(makeAnnounce(3, kMasterB, 0), 0);
    CHECK_EQ(ptp.getSelectedMaster().grandmasterIdentity[0], 0xBB);
}


static void testDelayResponseCarriesTheRequestedInterval()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();
    ptp.setLogMinDelayReqInterval(3);  // every eight seconds

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    ptp.feed(makeDelayRequest(7, requester), 2000000000LL);

    CHECK_EQ(ptp.sent.size(), 1);
    if (!ptp.sent.empty())
    {
        CHECK_EQ((int8_t)ptp.sent[0].data[33], 3);
    }
}

static void testMajorSdoIdIsWrittenAndChecked()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();
    ptp.setMajorSdoId(1);
    ptp.announceMessage();

    CHECK_EQ(ptp.sent.size(), 1);
    if (!ptp.sent.empty())
    {
        // Announce is message type 11, under sdoId 1.
        CHECK_EQ(ptp.sent[0].data[0], 0x1b);
    }

    // A Delay_Req under the default profile is not ours to answer.
    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    ptp.feed(makeDelayRequest(7, requester), 2000000000LL);
    CHECK_EQ(ptp.sent.size(), 1);

    // The same request under ours is.
    std::vector<uint8_t> request = makeDelayRequest(8, requester);
    request[0] = 0x11;
    ptp.feed(request, 2000000000LL);
    CHECK_EQ(ptp.sent.size(), 2);
}

static void testPpsOffsetIsAParameter()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();
    ptp.setPpsOffset(-300);

    const NanoTime base = 1000000000LL;
    ptp.ppsInterruptTriggered(base, base + 300);
    ptp.update();
    ptp.ppsInterruptTriggered(base + NS_PER_S, base + NS_PER_S + 300);
    ptp.update();

    // Three hundred nanoseconds of local error, cancelled exactly.
    CHECK_EQ(ptp.getOffset(), 0);
}

static void testPeerDelayMessagesAskForThePeerGroup()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, true);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    ptp.feed(makeSync(1), base + 10000);
    ptp.feed(makeFollowUp(1, base), 0);
    setTxTimestamp(base + 500000000LL);
    ptp.update();

    CHECK_EQ(ptp.sent.size(), 1);
    if (!ptp.sent.empty())
    {
        CHECK_EQ(ptp.sent[0].data[0], 2);   // Pdelay_Req
        CHECK(ptp.sent[0].peer);
        CHECK(!ptp.sent[0].general);
    }

    // The end-to-end request does not.
    ptptest::state().reset();
    TestPTP endToEnd(false, true, false);
    endToEnd.begin();
    endToEnd.feed(makeSync(1), base + 10000);
    endToEnd.feed(makeFollowUp(1, base), 0);
    setTxTimestamp(base + 500000000LL);
    endToEnd.update();
    CHECK_EQ(endToEnd.sent.size(), 1);
    if (!endToEnd.sent.empty())
    {
        CHECK(!endToEnd.sent[0].peer);
    }
}


static void testOnePeerAnswerAtATime()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, true);
    ptp.begin();

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};

    // This port sends Pdelay_Req of its own, and they take the same
    // timestamp register. One goes out here and the next is a long way
    // off, so what follows is about the answers alone.
    ptptest::state().randomValue = 100000;
    setTxTimestamp(2000010000LL);
    ptp.update();

    setTxTimestamp(2000030000LL);

    // A peer asking as fast as it can gets one answer per exchange this
    // port can finish, not two frames for every request.
    for (int i = 0; i < 5; i++)
    {
        ptp.feed(makePeerDelayRequest((uint16_t)(i + 1), requester), 2000000000LL + i);
    }
    CHECK_EQ(countSentOfType(ptp, 3), 1);

    // And the timestamp is collected without spinning: micros() is read
    // only inside a wait loop, and there is no longer one to enter.
    CHECK_EQ(ptptest::state().microsNow, 0);
    ptp.update();
    CHECK_EQ(countSentOfType(ptp, 10), 1);

    // With that exchange finished, the next request is answered.
    setTxTimestamp(2001000000LL);
    ptp.feed(makePeerDelayRequest(6, requester), 2000900000LL);
    CHECK_EQ(countSentOfType(ptp, 3), 2);
}

// A Sync does not wait for a pending peer answer, and does not steal its
// timestamp either: the answer is finished first, out of the register it
// is owed, and the Sync goes out in the same call.
static void testSyncFinishesAPendingPeerAnswerFirst()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, true);
    ptp.begin();

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    ptptest::state().txAvailable = false;
    ptp.feed(makePeerDelayRequest(9, requester), 2000000000LL);
    CHECK_EQ(countSentOfType(ptp, 3), 1);

    // The hardware posts the timestamp late, after the wait had given up.
    // It belongs to that Pdelay_Resp.
    postTxTimestamp(2000030000LL);
    ptp.syncMessage();

    CHECK_EQ(countSentOfType(ptp, 10), 1);  // the Follow_Up it was owed
    CHECK_EQ(countSentOfType(ptp, 0), 1);   // and the Sync, in the same call
    // The Sync's own timestamp is a separate one, and the hardware is
    // posting none: its Follow_Up is the exchange that goes without.
    CHECK_EQ(countSentOfType(ptp, 8), 0);
}

// A Pdelay_Req that arrives while a Sync is waiting for its departure
// time is not answered yet: answering arms the register again, and the
// Sync's Follow_Up would carry the wrong timestamp or none at all.
static void testAPeerRequestWaitsForAPendingSyncTimestamp()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, true);
    ptp.begin();

    // This port's own Pdelay_Req takes the same register: one goes out
    // here and the next is a long way off.
    ptptest::state().randomValue = 100000;
    setTxTimestamp(2999000000LL);
    ptp.update();

    setTxTimestamp(3000000000LL);
    ptp.syncMessage();
    CHECK_EQ(countSentOfType(ptp, 0), 1);

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    ptp.feed(makePeerDelayRequest(3, requester), 3000100000LL);
    CHECK_EQ(countSentOfType(ptp, 3), 0);

    // Once the Follow_Up has had its timestamp, the next request is
    // answered as usual.
    ptp.update();
    CHECK_EQ(countSentOfType(ptp, 8), 1);
    setTxTimestamp(3001000000LL);
    ptp.feed(makePeerDelayRequest(4, requester), 3000200000LL);
    CHECK_EQ(countSentOfType(ptp, 3), 1);
}

// A neighbour sending Pdelay_Req cannot stop the master sending Sync.
//
// syncMessage() used to return while a peer answer was pending, and every
// update() answers the next request in the queue and arms the register
// again: with one request arriving per pass the pending answer was never
// not pending, and the master fell silent. Measured before the fix: one
// Sync where a hundred were due.
static void testAPeerDelayFloodDoesNotSilenceTheSync()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, true);
    ptp.begin();
    ptptest::state().txAvailable = true;

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    const int rounds = 20;
    for (int i = 0; i < rounds; i++)
    {
        ptptest::state().millisNow += 1;
        const NanoTime now = 2000000000LL + i * 1000000LL;
        ptp.update();
        // The request arrives inside update(), which is where the socket
        // hands it over: the answer is pending when the sketch's Sync
        // timer fires next, and the Sync used to be dropped for it.
        ptp.feed(makePeerDelayRequest((uint16_t)i, requester), now);
        setTxTimestamp(now + 500000LL);
        ptp.syncMessage();
    }
    ptp.update();   // the last Follow_Up

    CHECK_EQ(countSentOfType(ptp, 0), (size_t)rounds);
    CHECK_EQ(countSentOfType(ptp, 8), (size_t)rounds);
}

static void testStaleDelaySamplesAreNotUsed()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime turnaround = 500000000LL;

    // A short path measured once, long ago.
    runSlaveCycle(ptp, 1, base, base + 10000, base + turnaround, base + turnaround + 10000);
    CHECK_EQ(ptp.getDelay(), 10000);

    // Half a minute later the route is longer. The old sample is past the
    // age at which the window would have turned over completely, so it no
    // longer holds the measurement down.
    ptptest::state().millisNow = 30000;
    const NanoTime later = base + 30 * NS_PER_S;
    runSlaveCycle(ptp, 2, later, later + 30000, later + turnaround, later + turnaround + 30000);
    CHECK_EQ(ptp.getDelay(), 30000);
}


// A board whose oscillator, network or idea of "locked" is not the one
// these numbers were chosen against has to be able to say so: they were
// literals inside the controller, where nothing could reach them.
static void testTheServoThresholdsAreParameters()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();
    ptp.setDelayFilterLength(1);

    // A lock window of a nanosecond: the offset an exchange with no path
    // asymmetry leaves is the hardware offset, -200 ns, which is inside
    // the default hundred-nanosecond window and outside this one.
    ptp.setLockThresholdNs(1);

    const NanoTime base = 1000000000LL;
    runCycleWithDelay(ptp, 1, base, 10000);
    runCycleWithDelay(ptp, 2, base + NS_PER_S, 10000);
    runCycleWithDelay(ptp, 3, base + 2 * NS_PER_S, 10000);
    CHECK_EQ(ptp.getOffset(), HW_OFFSET);
    CHECK_EQ(ptp.getLockCount(), 0);

    // Widened past that offset, the same exchanges count as locked.
    ptp.setLockThresholdNs(1000);
    runCycleWithDelay(ptp, 4, base + 3 * NS_PER_S, 10000);
    runCycleWithDelay(ptp, 5, base + 4 * NS_PER_S, 10000);
    CHECK(ptp.getLockCount() > 0);

    // And a coarse threshold below that offset steps the clock instead of
    // steering it.
    const size_t steps = ptptest::state().offsetTimerCalls.size();
    ptp.setCoarseModeThresholdNs(100);
    runCycleWithDelay(ptp, 6, base + 5 * NS_PER_S, 10000);
    CHECK(ptptest::state().offsetTimerCalls.size() > steps);
}

static void testEveryParameterReadsBack()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();

    ptp.setClockClass(6);
    ptp.setClockAccuracy(0x21);
    ptp.setOffsetScaledLogVariance(0x4E5D);
    ptp.setPriority1(64);
    ptp.setPriority2(65);
    ptp.setTimeSource(0x20);
    ptp.setDomainNumber(3);
    ptp.setMajorSdoId(1);
    ptp.setCurrentUtcOffset(37);
    ptp.setUtcOffsetValid(true);
    ptp.setLeap59(true);
    ptp.setLeap61(true);
    ptp.setTimeTraceable(true);
    ptp.setFrequencyTraceable(true);
    ptp.setStepsRemoved(2);
    ptp.setLogSyncInterval(-1);
    ptp.setLogAnnounceInterval(1);
    ptp.setLogMinDelayReqInterval(3);
    ptp.setKp(2.5);
    ptp.setKi(0.25);
    ptp.setKf(0.5);
    ptp.setDelayFilterLength(4);
    ptp.setTimestampOffset(-500);
    ptp.setPeerOffsetCorrection(250);
    ptp.setMaxDriftNsps(50000.0);
    ptp.setFreqModeThresholdNsps(2000.0);
    ptp.setCoarseModeThresholdNs(5000);
    ptp.setLockThresholdNs(250);
    ptp.setPpsOffset(-125);

    CHECK_EQ(ptp.getClockClass(), 6);
    CHECK_EQ(ptp.getClockAccuracy(), 0x21);
    CHECK_EQ(ptp.getOffsetScaledLogVariance(), 0x4E5D);
    CHECK_EQ(ptp.getPriority1(), 64);
    CHECK_EQ(ptp.getPriority2(), 65);
    CHECK_EQ(ptp.getTimeSource(), 0x20);
    CHECK_EQ(ptp.getDomainNumber(), 3);
    CHECK_EQ(ptp.getMajorSdoId(), 1);
    CHECK_EQ(ptp.getCurrentUtcOffset(), 37);
    CHECK(ptp.getUtcOffsetValid());
    CHECK(ptp.getLeap59());
    CHECK(ptp.getLeap61());
    CHECK(ptp.getTimeTraceable());
    CHECK(ptp.getFrequencyTraceable());
    CHECK_EQ(ptp.getStepsRemoved(), 2);
    CHECK_EQ(ptp.getLogSyncInterval(), -1);
    CHECK_EQ(ptp.getLogAnnounceInterval(), 1);
    CHECK_EQ(ptp.getLogMinDelayReqInterval(), 3);
    CHECK_EQ((long long)(ptp.getKp() * 10), 25);
    CHECK_EQ((long long)(ptp.getKi() * 100), 25);
    CHECK_EQ((long long)(ptp.getKf() * 10), 5);
    CHECK_EQ(ptp.getDelayFilterLength(), 4);
    CHECK_EQ(ptp.getTimestampOffset(), -500);
    CHECK_EQ(ptp.getPeerOffsetCorrection(), 250);
    CHECK_EQ(ptp.getPpsOffset(), -125);
    CHECK_EQ((long long)ptp.getMaxDriftNsps(), 50000);
    CHECK_EQ((long long)ptp.getFreqModeThresholdNsps(), 2000);
    CHECK_EQ(ptp.getCoarseModeThresholdNs(), 5000);
    CHECK_EQ(ptp.getLockThresholdNs(), 250);

    // The filter length is held inside its own range.
    ptp.setDelayFilterLength(0);
    CHECK_EQ(ptp.getDelayFilterLength(), 1);
    ptp.setDelayFilterLength(200);
    CHECK_EQ(ptp.getDelayFilterLength(), 8);
}


// ----------------------------------------------- regressions, batch eight

static void testAMasterStepsAsideForABetterOne()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();
    ptp.setPriority1(128);

    CHECK(ptp.getPortState() == PortState::Master);
    setTxTimestamp(3000000000LL);
    ptp.syncMessage();
    ptp.update();
    ptp.announceMessage();
    CHECK_EQ(ptp.sent.size(), 3);

    // A better master announces itself on the same segment.
    ptp.feed(makeAnnounce(1, kMasterA, /*priority1=*/10), 0);
    CHECK(ptp.getPortState() == PortState::Passive);

    // Nothing more goes out from here.
    setTxTimestamp(4000000000LL);
    ptp.syncMessage();
    ptp.announceMessage();
    CHECK_EQ(ptp.sent.size(), 3);

    // Three Announce intervals of silence and this port takes over again.
    ptptest::state().millisNow = 3001;
    ptp.update();
    CHECK(ptp.getPortState() == PortState::Master);
    setTxTimestamp(5000000000LL);
    ptp.announceMessage();
    CHECK_EQ(ptp.sent.size(), 4);
}

static void testAMasterIgnoresAWorseOne()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();
    ptp.setPriority1(10);

    ptp.feed(makeAnnounce(1, kMasterA, /*priority1=*/200), 0);
    CHECK(ptp.getPortState() == PortState::Master);

    ptp.announceMessage();
    CHECK_EQ(ptp.sent.size(), 1);
}

static void testTheComparisonCanBeTurnedOff()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();
    ptp.setBmcaEnabled(false);

    ptp.feed(makeAnnounce(1, kMasterA, /*priority1=*/0), 0);
    CHECK(ptp.getPortState() == PortState::Master);
    ptp.announceMessage();
    CHECK_EQ(ptp.sent.size(), 1);
}

static void testAPortThatIsBothFollowsTheBetterClock()
{
    ptptest::state().reset();
    TestPTP ptp(true, true, false);
    ptp.begin();
    ptp.setPriority1(128);

    CHECK(ptp.getPortState() == PortState::Master);

    // A better clock appears: this port becomes its slave rather than
    // going quiet, because it is configured to do both.
    ptp.feed(makeAnnounce(1, kMasterA, 10), 0);
    CHECK(ptp.getPortState() == PortState::Slave);

    // It no longer sends, and it does follow.
    setTxTimestamp(3000000000LL);
    ptp.announceMessage();
    CHECK_EQ(ptp.sent.size(), 0);

    const NanoTime base = 1000000000LL;
    runCycleFrom(ptp, kMasterA, 1, base, 10000);
    runCycleFrom(ptp, kMasterA, 2, base + NS_PER_S, 10000);
    CHECK_EQ(ptp.getDelay(), 10000);
    CHECK_EQ(ptp.getOffset(), HW_OFFSET);
}

static void testASlaveListensBeforeItChooses()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    // Nothing heard yet: anything that arrives is followed, as it was
    // before Announce was parsed at all.
    CHECK(ptp.getPortState() == PortState::Slave);

    ptp.feed(makeAnnounce(1, kMasterB, 100), 0);
    CHECK(ptp.getPortState() == PortState::Slave);

    // The master goes quiet: an Announce has been heard on this port, so
    // it waits for one rather than following whatever turns up.
    ptptest::state().millisNow = 3001;
    ptp.update();
    CHECK(ptp.getPortState() == PortState::Listening);

    ptp.feed(makeAnnounce(2, kMasterA, 200), 0);
    CHECK(ptp.getPortState() == PortState::Slave);
}

static void testTheStateFollowsTheLifecycle()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    CHECK(ptp.getPortState() == PortState::Initializing);

    ptp.begin();
    CHECK(ptp.getPortState() == PortState::Master);

    ptp.end();
    CHECK(ptp.getPortState() == PortState::Initializing);

    ptp.begin();
    CHECK(ptp.getPortState() == PortState::Master);
}


// ------------------------------------------------ regressions, batch nine

static void testAMasterDoesNotFollowWhileItAnnounces()
{
    ptptest::state().reset();
    TestPTP ptp(true, true, false);
    ptp.begin();
    ptp.setPriority1(10);

    // This port is the better clock, so it is the one announcing. A Sync
    // from the other one must not discipline it.
    CHECK(ptp.getPortState() == PortState::Master);
    const NanoTime base = 1000000000LL;
    runCycleFrom(ptp, kMasterA, 1, base, 10000);
    runCycleFrom(ptp, kMasterA, 2, base + NS_PER_S, 10000);
    CHECK_EQ(ptp.getOffset(), 0);
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 0);

    // Told it is the worse clock, it follows that same master.
    ptp.setPriority1(200);
    ptp.feed(makeAnnounce(1, kMasterA, 100), 0);
    CHECK(ptp.getPortState() == PortState::Slave);
    runCycleFrom(ptp, kMasterA, 3, base + 2 * NS_PER_S, 10000);
    runCycleFrom(ptp, kMasterA, 4, base + 3 * NS_PER_S, 10000);
    CHECK_EQ(ptp.getOffset(), HW_OFFSET);
}

static void testOurOwnDatasetChangingDecidesAgain()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();
    ptp.setPriority1(10);

    ptp.feed(makeAnnounce(1, kMasterA, 100), 0);
    CHECK(ptp.getPortState() == PortState::Master);

    // Nothing new arrives; this port is simply told it is the worse of
    // the two, and steps aside without waiting for another Announce.
    ptp.setPriority1(200);
    CHECK(ptp.getPortState() == PortState::Passive);

    // A grandmaster that recovers its reference is the better clock
    // again, by class this time.
    ptp.setPriority1(100);
    ptp.setClockClass(6);
    CHECK(ptp.getPortState() == PortState::Master);
}

static void testAHeldBackRequestDoesNotSpendItsTurn()
{
    ptptest::state().reset();
    TestPTP ptp(true, true, true);
    ptp.begin();
    ptp.setPriority1(200);
    ptp.feed(makeAnnounce(1, kMasterA, 10), 0);
    CHECK(ptp.getPortState() == PortState::Slave);

    // A peer's request leaves a Pdelay_Resp owed the transmit timestamp,
    // and nothing else may take it.
    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    ptptest::state().txAvailable = false;
    ptp.feed(makePeerDelayRequest(1, requester), 900000000LL);

    const NanoTime base = 1000000000LL;
    std::vector<uint8_t> sync = makeSync(1);
    putSource(sync, kMasterA);
    std::vector<uint8_t> followUp = makeFollowUp(1, base);
    putSource(followUp, kMasterA);
    ptp.feed(sync, base + 10000);
    ptp.feed(followUp, 0);

    const size_t sentBefore = ptp.sent.size();
    ptp.update();
    // No Pdelay_Req went out: the hardware owes a timestamp elsewhere.
    CHECK_EQ(ptp.sent.size(), sentBefore);
    // And no interval was drawn for a request that never happened.
    CHECK_EQ(ptptest::state().randomBounds.size(), 0);
}


static void testAStaleTransmitTimestampIsNotPublished()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();

    // A timestamp left in the register by an earlier exchange whose wait
    // gave up before the hardware posted it.
    postTxTimestamp(1000000000LL);

    setTxTimestamp(3000000000LL);
    ptp.syncMessage();
    ptp.update();

    CHECK_EQ(ptp.sent.size(), 2);
    if (ptp.sent.size() == 2)
    {
        // The Follow_Up carries this Sync's departure, not the one left
        // over from before it.
        CHECK_EQ(bufferToNanoTime(ptp.sent[1].data.data()), 3000000000LL + HW_OFFSET);
    }
}

static void testAPassivePortDoesNotAnswerDelayRequests()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();
    ptp.setPriority1(200);

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    ptp.feed(makeDelayRequest(1, requester), 2000000000LL);
    CHECK_EQ(ptp.sent.size(), 1);

    // A better master takes over the segment. This port is no longer the
    // reference, and answering would pull whoever asked towards a clock
    // that is not it.
    ptp.feed(makeAnnounce(1, kMasterA, 10), 0);
    CHECK(ptp.getPortState() == PortState::Passive);
    ptp.feed(makeDelayRequest(2, requester), 2001000000LL);
    CHECK_EQ(ptp.sent.size(), 1);

    // Back on air, it answers again.
    ptptest::state().millisNow = 3001;
    ptp.update();
    CHECK(ptp.getPortState() == PortState::Master);
    ptp.feed(makeDelayRequest(3, requester), 2002000000LL);
    CHECK_EQ(ptp.sent.size(), 2);
}

static void testAnAnnounceFromTooFarAwayIsIgnored()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    // 255 steps is what a message that has gone round a loop carries.
    ptp.feed(makeAnnounce(1, kMasterA, 10, 6, 128, /*stepsRemoved=*/255), 0);
    CHECK(!ptp.hasSelectedMaster());

    ptp.feed(makeAnnounce(2, kMasterA, 10, 6, 128, /*stepsRemoved=*/254), 0);
    CHECK(ptp.hasSelectedMaster());
}

// --------------------------------------------------------------- entry point

// ----------------------------------------------- regressions, batch nine

static void testTheReferenceOutranksTheSync()
{
    ptptest::state().reset();
    TestPTP ptp(true, true, false);
    ptp.begin();
    ptp.setPriority1(128);

    // A better clock takes the segment, so this port follows it.
    ptp.feed(makeAnnounce(1, kMasterA, 10), 0);
    CHECK(ptp.getPortState() == PortState::Slave);

    // The pin is still being fed, and standing aside does not change
    // that. ppsInterruptTriggered() used to return unless the port was
    // currently Master, so a port configured to do both threw its own
    // reference away the moment a better master appeared and disciplined
    // itself from the network instead of from the clock it is wired to.
    const NanoTime base = 1000000000LL;
    const NanoTime referenceError = 300;
    ptp.ppsInterruptTriggered(base, base + referenceError);
    ptp.update();
    ptp.ppsInterruptTriggered(base + NS_PER_S, base + NS_PER_S + referenceError);
    ptp.update();
    CHECK_EQ(ptp.getOffset(), referenceError);

    const size_t adjustments = ptptest::state().adjustFreqCalls.size();

    // And a complete exchange with that master now corrects nothing while
    // the reference is live: the offset is still the one the pin gave.
    runCycleFrom(ptp, kMasterA, 1, base, 10000);
    runCycleFrom(ptp, kMasterA, 2, base + NS_PER_S, 10000);
    CHECK_EQ(ptp.getOffset(), referenceError);
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), adjustments);
}

static void testASyncCannotBeMistakenForTheReference()
{
    ptptest::state().reset();
    TestPTP ptp(true, true, false);
    ptp.begin();
    ptp.setPriority1(128);
    ptp.feed(makeAnnounce(1, kMasterA, 10), 0);

    const NanoTime base = 1000000000LL;
    const NanoTime referenceError = 300;
    ptp.ppsInterruptTriggered(base, base + referenceError);
    ptp.update();
    ptp.ppsInterruptTriggered(base + NS_PER_S, base + NS_PER_S + referenceError);

    // A Sync landing between the edge and the update() that consumes it.
    // The reference used to hand its pair to the Sync path's t1/t2, which
    // this overwrote, so what reached the servo as the reference offset
    // was the master's pair measured with no path delay at all.
    std::vector<uint8_t> sync = makeSync(3);
    putSource(sync, kMasterA);
    std::vector<uint8_t> followUp = makeFollowUp(3, base + NS_PER_S);
    putSource(followUp, kMasterA);
    ptp.feed(sync, base + NS_PER_S + 10000);
    ptp.feed(followUp, 0);
    ptp.update();

    CHECK_EQ(ptp.getOffset(), referenceError);
}

static void testTheSyncTakesTheClockBackWhenTheReferenceStops()
{
    ptptest::state().reset();
    TestPTP ptp(true, true, false);
    ptp.begin();
    ptp.setPriority1(128);
    ptp.feed(makeAnnounce(1, kMasterA, 10), 0);

    const NanoTime base = 1000000000LL;
    ptp.ppsInterruptTriggered(base, base + 300);
    ptp.update();
    ptp.ppsInterruptTriggered(base + NS_PER_S, base + NS_PER_S + 300);
    ptp.update();
    CHECK_EQ(ptp.getOffset(), 300);

    // The reference stops. Past the timeout it is gone, and the network
    // is what is left to discipline this clock.
    ptptest::state().millisNow += EXTERNAL_REFERENCE_TIMEOUT_MS + 1;
    ptp.feed(makeAnnounce(2, kMasterA, 10), 0);

    runCycleFrom(ptp, kMasterA, 1, base, 10000);
    runCycleFrom(ptp, kMasterA, 2, base + NS_PER_S, 10000);
    CHECK_EQ(ptp.getDelay(), 10000);
    CHECK_EQ(ptp.getOffset(), HW_OFFSET);
}


// A nanoseconds field of its own full 32 bits, which 1588 does not allow
// and nothing on the wire stops. The seconds clamp alone was not enough:
// MAX_SAFE_SECONDS times a thousand million left 854775807 ns of room
// under the top of an int64_t and the field can say four thousand
// million, so the addition the clamp was there to protect overflowed.
static void testAnOversizedNanosecondsFieldIsClamped()
{
    ptptest::state().reset();

    std::vector<uint8_t> buf(54, 0);
    for (size_t i = 34; i < 40; i++)
    {
        buf[i] = 0xff;  // 2^48-1 seconds, clamped to MAX_SAFE_SECONDS
    }
    for (size_t i = 40; i < 44; i++)
    {
        buf[i] = 0xff;  // 0xffffffff nanoseconds
    }

    const NanoTime value = bufferToNanoTime(buf.data());
    CHECK_EQ(value, MAX_SAFE_SECONDS * NS_PER_S + NS_PER_S - 1);
    CHECK(value > 0);
    CHECK(value <= std::numeric_limits<NanoTime>::max());
}

// The top octet of the correctionField carries its sign. Shifted into a
// signed 64-bit value it overflowed for everything from 0x80 up, which is
// every negative correction a real path produces, not only a hostile one.
static void testTheCorrectionFieldsTopOctetIsRead()
{
    ptptest::state().reset();

    // The most negative correctionField there is: 0x8000000000000000 in
    // units of 2^-16 ns, which is -2^47 nanoseconds.
    std::vector<uint8_t> mostNegative(54, 0);
    mostNegative[8] = 0x80;
    CHECK_EQ(bufferToCorrection(mostNegative.data()), -(NanoTime{1} << 47));

    // And the largest positive one, a shade under +2^47.
    std::vector<uint8_t> mostPositive(54, 0);
    mostPositive[8] = 0x7f;
    for (size_t i = 9; i < 16; i++)
    {
        mostPositive[i] = 0xff;
    }
    CHECK_EQ(bufferToCorrection(mostPositive.data()), (NanoTime{1} << 47) - 1);
}

// The two clamped values still have to be added to each other. A
// timestamp held at the top of the range plus a correctionField of a day
// and a half wrapped into a negative time before this.
static void testATimestampPlusItsCorrectionStopsAtTheEdge()
{
    ptptest::state().reset();

    std::vector<uint8_t> buf(54, 0);
    for (size_t i = 34; i < 40; i++)
    {
        buf[i] = 0xff;
    }
    for (size_t i = 40; i < 44; i++)
    {
        buf[i] = 0xff;
    }
    buf[8] = 0x7f;
    for (size_t i = 9; i < 16; i++)
    {
        buf[i] = 0xff;
    }

    const NanoTime sum = addSaturating(bufferToNanoTime(buf.data()), bufferToCorrection(buf.data()));
    CHECK_EQ(sum, std::numeric_limits<NanoTime>::max());
    CHECK(sum > 0);

    // And the same going the other way, which is what the delay path does
    // with the answer's correction.
    std::vector<uint8_t> low(54, 0);
    low[8] = 0x80;  // -2^47 ns
    CHECK_EQ(addSaturating(std::numeric_limits<NanoTime>::min(),
                           -bufferToCorrection(low.data())),
             std::numeric_limits<NanoTime>::min() + (NanoTime{1} << 47));
    CHECK_EQ(addSaturating(std::numeric_limits<NanoTime>::min(), -1),
             std::numeric_limits<NanoTime>::min());
}

// The Delay_Req path armed the hardware without clearing it first, which
// is the door the Sync path closed long ago. A timestamp posted after an
// earlier wait had given up was read as this request's departure, and the
// path delay came out of a T3 that belonged to another message.
static void testAStaleTransmitTimestampIsNotUsedAsT3()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    // Left in the register by an earlier exchange, and nothing will be
    // posted for the request about to go out.
    postTxTimestamp(1000000000LL);
    ptptest::state().txAvailable = false;

    ptp.feed(makeSync(1), 2000000000LL);
    ptp.feed(makeFollowUp(1, 1999000000LL), 0);
    ptp.update();  // sends the Delay_Req
    CHECK_EQ(ptp.sent.size(), 1);

    // With no departure time of its own the request is abandoned, so its
    // answer is refused and no path delay is ever measured. Taking the
    // stale value instead produced one of a second.
    ptp.feed(makeResponse(9, lastSentSequenceID(ptp), 3000000000LL), 0);
    ptp.update();
    CHECK_EQ(ptp.getDelay(), 0);
}

// The external reference has no path to subtract, and used to say so by
// zeroing the path delay the network had measured. The zero outlived the
// reference: when the pin fell silent the Sync took the clock back with
// no delay in the offset until the next exchange finished.
static void testThePpsDoesNotThrowAwayTheMeasuredPathDelay()
{
    ptptest::state().reset();
    TestPTP ptp(true, true, false);
    ptp.begin();

    // A better clock on the segment, so this port follows it while still
    // being the one wired to the reference.
    ptp.feed(makeAnnounce(1, kMasterA, /*priority1=*/10), 0);
    CHECK(ptp.getPortState() == PortState::Slave);

    const NanoTime base = 1000000000LL;
    runCycleFrom(ptp, kMasterA, 1, base, 10000);
    runCycleFrom(ptp, kMasterA, 2, base + NS_PER_S, 10000);
    CHECK_EQ(ptp.getDelay(), 10000);

    // Two edges of the reference: the first arms the path, the second
    // drives the servo.
    const NanoTime ppsBase = 4000000000LL;
    ptp.ppsInterruptTriggered(ppsBase, ppsBase + 300);
    ptp.update();
    ptp.ppsInterruptTriggered(ppsBase + NS_PER_S, ppsBase + NS_PER_S + 300);
    ptp.update();

    // The offset is measured against the pin, with no path in it.
    CHECK_EQ(ptp.getOffset(), 300);
    // And the path the network measured is still there for the Sync to be
    // corrected against when the reference goes away.
    CHECK_EQ(ptp.getDelay(), 10000);
}


// ------------------------------------------------ regressions, batch nine

static void testAPeerDelayResponseIsRefusedEndToEnd()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();
    ptp.setDelayFilterLength(1);

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;

    ptp.feed(makeSync(1), base + pathDelay);
    ptp.feed(makeFollowUp(1, base), 0);
    setTxTimestamp(base + turnaround);
    ptp.update();  // Delay_Req out, T3 taken

    // The answer this port is waiting for, relabelled as a Pdelay_Resp.
    // That branch is the one with no check on where the message came
    // from -- a peer is not the master and is not compared against it --
    // so on a port measuring the delay end to end it must be shut: with
    // it open, changing the message type from 9 to 3 walked past the
    // check and set T4 from anybody at all.
    const uint16_t sequenceID = lastSentSequenceID(ptp);
    ptp.feed(makeResponse(3, sequenceID, base + turnaround + 8 * pathDelay), 0);
    ptp.update();
    CHECK_EQ(ptp.getDelay(), 0);

    // And the request was not spent on the one that was refused.
    ptp.feed(makeResponse(9, sequenceID, base + turnaround + pathDelay), 0);
    ptp.update();
    CHECK_EQ(ptp.getDelay(), pathDelay);
}

static void testPinningReleasesTheStateWithTheChoice()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();

    // A better master on the segment takes this one off the air.
    ptp.feed(makeAnnounce(1, kMasterB, /*priority1=*/0), 0);
    CHECK(ptp.getPortState() == PortState::Passive);

    // Pinning another identity drops the choice, and the state that came
    // out of it goes with it. Dropping the choice alone left the port
    // Passive with nothing left to release it: it never announced again,
    // and the pinned master was the only thing that could have freed it.
    ptp.setMasterIdentity(kMasterA);
    CHECK(!ptp.hasSelectedMaster());
    CHECK(ptp.getPortState() == PortState::Master);

    // The same on a slave: an Announce has been heard, so a port with no
    // master chosen is Listening, not Slave.
    TestPTP slave(false, true, false);
    slave.begin();
    slave.feed(makeAnnounce(1, kMasterB, 100), 0);
    CHECK(slave.getPortState() == PortState::Slave);
    slave.setMasterIdentity(kMasterA);
    CHECK(slave.getPortState() == PortState::Listening);
}

static void testABadFirstExchangeIsNotAPath()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    const NanoTime turnaround = 500000000LL;

    // T3 a millisecond past where the other three timestamps put it: the
    // arithmetic gives a negative path, which is a bad exchange and not
    // a measurement of anything.
    runSlaveCycle(ptp, 1, base, base + 10000, base + turnaround + 1000000, base + turnaround);
    runSlaveCycle(ptp, 2, base + NS_PER_S, base + NS_PER_S + 10000,
                  base + NS_PER_S + turnaround + 1000000, base + NS_PER_S + turnaround);

    // So nothing has been measured and nothing corrects the clock. The
    // path used to count as measured at whatever currentDelay held --
    // zero on a port that has just come up -- and the second Sync
    // steered the clock with the whole path delay left in the offset.
    CHECK_EQ(ptp.getDelay(), 0);
    CHECK_EQ(ptp.getOffset(), 0);
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 0);
    CHECK_EQ(ptptest::state().offsetTimerCalls.size(), 0);
}

static void testAPortThatIsBothStillSlavesWithTheComparisonOff()
{
    ptptest::state().reset();
    TestPTP ptp(true, true, false);
    ptp.begin();
    ptp.setBmcaEnabled(false);

    // Nothing chosen yet, so it is the master and it announces.
    CHECK(ptp.getPortState() == PortState::Master);
    ptp.announceMessage();
    CHECK_EQ(ptp.sent.size(), 1);

    // A master on the segment, worse than this port by the dataset: with
    // the comparison off there is nothing to weigh it against, so the
    // configured slave role stands and the port follows it. It used to
    // stay Master for ever -- the comparison being off made the test
    // that put it into Slave false whatever arrived -- so the slave half
    // of a port configured as both never ran at all.
    ptp.feed(makeAnnounce(1, kMasterA, /*priority1=*/200), 0);
    CHECK(ptp.getPortState() == PortState::Slave);

    ptp.sent.clear();
    ptp.announceMessage();
    CHECK_EQ(ptp.sent.size(), 0);

    const NanoTime base = 1000000000LL;
    runCycleFrom(ptp, kMasterA, 1, base, 10000);
    runCycleFrom(ptp, kMasterA, 2, base + NS_PER_S, 10000);
    CHECK_EQ(ptp.getDelay(), 10000);
    CHECK_EQ(ptp.getOffset(), HW_OFFSET);

    // And it takes the segment back when that master goes quiet.
    ptptest::state().millisNow = 3001;
    ptp.update();
    CHECK(ptp.getPortState() == PortState::Master);
}

static void testANegativeTimestampKeepsItsValue()
{
    ptptest::state().reset();

    // Integer division truncates towards zero, so a negative NanoTime
    // came back out of a timespec with a negative tv_nsec.
    const NanoTime values[] = {-1, -100, -NS_PER_S, -NS_PER_S - 1,
                               -2 * NS_PER_S - 999999999};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
    {
        timespec ts;
        nanoTimeToTimespec(values[i], ts);
        CHECK(ts.tv_nsec >= 0);
        CHECK(ts.tv_nsec < NS_PER_S);
        CHECK_EQ(timespecToNanoTime(ts), values[i]);
    }

    // And what a negative tv_nsec put on the wire: the field is written
    // as it stands, so minus a hundred nanoseconds became a nanoseconds
    // field of four thousand million. Reachable without a hostile
    // packet -- every timestamp taken has timestampOffset added to it,
    // the default is negative, and the clock starts at zero.
    TestPTP master(true, false, false);
    master.begin();
    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    master.feed(makeDelayRequest(7, requester), 100);
    CHECK_EQ(master.sent.size(), 1);
    if (!master.sent.empty())
    {
        const std::vector<uint8_t> &answer = master.sent[0].data;
        const uint32_t ns = ((uint32_t)answer[40] << 24) | ((uint32_t)answer[41] << 16) |
                            ((uint32_t)answer[42] << 8) | (uint32_t)answer[43];
        CHECK(ns < (uint32_t)NS_PER_S);
    }
}

// ------------------------------------------------ regressions, batch ten

// The two delay mechanisms are exclusive: a port configured for peer
// delay does not run the end-to-end exchange, and 1588 gives it no
// Delay_Req to answer. The branch asked only whether this port was the
// master, so a peer-delay master handed out a Delay_Resp carrying its
// receipt timestamp, and whoever asked measured a path against a clock
// that had never agreed to be measured that way.
static void testAPeerDelayMasterDoesNotAnswerDelayRequests()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, true);
    ptp.begin();

    const uint8_t requester[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x01};
    ptp.feed(makeDelayRequest(7, requester), 1000);
    CHECK_EQ(ptp.sent.size(), 0);

    // The request its own mechanism defines is still answered.
    ptp.feed(makePeerDelayRequest(8, requester), 2000);
    CHECK_EQ(ptp.sent.size(), 1);
    if (!ptp.sent.empty())
    {
        CHECK_EQ(ptp.sent[0].data[0] & 0x0f, 3);
    }
}

// The other half of testAPeerDelayResponseIsRefusedEndToEnd(). A
// Delay_Resp accepted on a port measuring the delay peer to peer went
// through the same function as a Pdelay_Resp: it set T4 from its own
// receiveTimestamp, T6 from its arrival and the flag that lets a
// Pdelay_Resp_Follow_Up in, so a peer-delay measurement was completed
// out of an end-to-end answer the peer never sent.
static void testAnEndToEndDelayResponseIsRefusedPeerToPeer()
{
    ptptest::state().reset();
    // Far enough out that the port's own pacing sends one request and
    // waits: the exchange under test is the one it just sent.
    ptptest::state().randomValue = 5000;
    TestPTP ptp(false, true, true);
    ptp.begin();
    ptp.setDelayFilterLength(1);

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;
    const NanoTime residence = 1000;
    const NanoTime t3 = base + turnaround;
    const NanoTime t4 = t3 + pathDelay;
    const NanoTime t5 = t4 + residence;
    const NanoTime t6 = t5 + pathDelay;

    ptp.feed(makeSync(1), base + pathDelay);
    ptp.feed(makeFollowUp(1, base), 0);
    setTxTimestamp(t3);
    ptp.update();  // Pdelay_Req out, T3 taken
    const uint16_t sequenceID = lastSentSequenceID(ptp);

    ptp.feed(makeResponse(9, sequenceID, t4), t6);
    ptp.feed(makePdelayRespFollowUp(sequenceID, t5), 0);
    ptp.update();
    CHECK_EQ(ptp.getDelay(), 0);

    // And the request was not spent on the one that was refused.
    ptp.feed(makeResponse(3, sequenceID, t4), t6);
    ptp.feed(makePdelayRespFollowUp(sequenceID, t5), 0);
    ptp.update();
    CHECK_EQ(ptp.getDelay(), pathDelay);
}

// The two halves of a peer-delay answer were tied together by the
// sequence ID and by our own requestingPortIdentity, both of which are in
// the Pdelay_Resp for anyone on the segment to read. Any other device
// could therefore send the Follow_Up, and the link delay became the
// difference between two clocks that had never met.
static void testAPeerFollowUpFromAnotherPortIsIgnored()
{
    ptptest::state().reset();
    // Far enough out that the port's own pacing sends one request and
    // waits: the exchange under test is the one it just sent.
    ptptest::state().randomValue = 5000;
    TestPTP ptp(false, true, true);
    ptp.begin();
    ptp.setDelayFilterLength(1);

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;
    const NanoTime residence = 1000;
    const NanoTime t3 = base + turnaround;
    const NanoTime t4 = t3 + pathDelay;
    const NanoTime t5 = t4 + residence;
    const NanoTime t6 = t5 + pathDelay;

    const uint8_t responder[8] = {0xaa, 0xbb, 0xcc, 0xff, 0xfe, 0x01, 0x02, 0x03};
    const uint8_t impostor[8] = {0xde, 0xad, 0xbe, 0xff, 0xfe, 0x0a, 0x0b, 0x0c};

    ptp.feed(makeSync(1), base + pathDelay);
    ptp.feed(makeFollowUp(1, base), 0);
    setTxTimestamp(t3);
    ptp.update();  // Pdelay_Req out, T3 taken
    const uint16_t sequenceID = lastSentSequenceID(ptp);

    std::vector<uint8_t> response = makeResponse(3, sequenceID, t4);
    putSource(response, responder);
    ptp.feed(response, t6);

    // A T5 the real peer never sent, from a port that never answered.
    std::vector<uint8_t> forged = makePdelayRespFollowUp(sequenceID, t5 + 6000);
    putSource(forged, impostor);
    ptp.feed(forged, 0);
    ptp.update();
    CHECK_EQ(ptp.getDelay(), 0);

    // The peer that did answer still completes its own exchange.
    std::vector<uint8_t> followUp = makePdelayRespFollowUp(sequenceID, t5);
    putSource(followUp, responder);
    ptp.feed(followUp, 0);
    ptp.update();
    CHECK_EQ(ptp.getDelay(), pathDelay);
}

// --------------------------------------------- regressions, batch eleven

// Peer delay measures the link and not the hierarchy: a port that hears
// no Sync at all still has a neighbour, and still has a link worth
// measuring. The request used to be armed by a matched Sync pair, like
// the end-to-end one, so a peer-delay master -- which parses no Sync --
// never asked for its link delay and never had one; and the branch that
// takes the answer asked to be following a master, so an answer would
// have been refused even if the request had gone out.
static void testAPeerDelayPortMeasuresWithoutASync()
{
    ptptest::state().reset();
    // Far enough out that one request goes out and the port then waits.
    ptptest::state().randomValue = 5000;
    TestPTP ptp(true, false, true);
    ptp.begin();
    ptp.setDelayFilterLength(1);

    const NanoTime pathDelay = 10000;
    const NanoTime residence = 1000;
    const NanoTime t3 = 2000000000LL;
    const NanoTime t4 = t3 + pathDelay;
    const NanoTime t5 = t4 + residence;
    const NanoTime t6 = t5 + pathDelay;

    setTxTimestamp(t3);
    ptp.update();
    CHECK_EQ(countSentOfType(ptp, 2), 1);
    const uint16_t sequenceID = lastSentSequenceID(ptp);

    ptp.feed(makeResponse(3, sequenceID, t4), t6);
    ptp.feed(makePdelayRespFollowUp(sequenceID, t5), 0);
    ptp.update();
    CHECK_EQ(ptp.getDelay(), pathDelay);
}

// The interval a peer-delay port asks at is its own. No master names one
// -- the exchange runs between neighbours, and the Pdelay_Resp carries
// 0x7f where the end-to-end answer carries a rate -- so the number
// setLogMinDelayReqInterval() holds, which such a port could otherwise
// only announce and never act on, is what paces it.
static void testThePeerDelayRateIsTheConfiguredOne()
{
    ptptest::state().reset();
    // Three quarters of the two seconds configured below, which the stub
    // hands back whole because it is inside the bound it is drawn from.
    ptptest::state().randomValue = 3000;
    TestPTP ptp(true, false, true);
    ptp.begin();
    ptp.setLogMinDelayReqInterval(1);  // one request every two seconds

    setTxTimestamp(2000000000LL);
    ptp.update();
    CHECK_EQ(countSentOfType(ptp, 2), 1);

    // Drawn uniformly over twice the interval, as end to end.
    CHECK_EQ(ptptest::state().randomBounds.size(), 1);
    if (ptptest::state().randomBounds.size() == 1)
    {
        CHECK_EQ(ptptest::state().randomBounds[0], 4001);
    }

    // And nothing goes out before the instant that drew.
    ptptest::state().millisNow = 2999;
    ptp.update();
    CHECK_EQ(countSentOfType(ptp, 2), 1);
    ptptest::state().millisNow = 3000;
    ptp.update();
    CHECK_EQ(countSentOfType(ptp, 2), 2);
}

// The finished exchange is worked out before the next request goes out.
//
// A Delay_Resp and a Sync pair drained in the same update() -- an
// ordinary pass at eight Sync a second, not a rare one -- had the request
// block run first, so T3 and the T1 and T2 the request was taken against
// already belonged to the new exchange by the time the delay was
// computed. What came out was one exchange's departure against another's
// arrival: here a negative sample, which the filter then refuses, leaving
// the port with no measured path at all.
static void testAnAnswerIsNotMixedWithTheNextRequest()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();
    ptp.setDelayFilterLength(1);

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;

    ptp.feed(makeSync(1), base + pathDelay);
    ptp.feed(makeFollowUp(1, base), 0);
    setTxTimestamp(base + turnaround);
    ptp.update();  // Delay_Req out, T3 taken
    const uint16_t sequenceID = lastSentSequenceID(ptp);

    // The answer to it and the next Sync pair, all in one pass.
    ptp.feed(makeResponse(9, sequenceID, base + turnaround + pathDelay), 0);
    const NanoTime second = base + NS_PER_S;
    ptp.feed(makeSync(2), second + pathDelay);
    ptp.feed(makeFollowUp(2, second), 0);
    setTxTimestamp(second + turnaround);
    ptp.update();

    CHECK_EQ(ptp.getDelay(), pathDelay);
}

// An Announce the parser refuses does not end the port's synchronisation.
//
// announceHeard was set before the stepsRemoved limit was checked, and
// fromSelectedMaster() reads it: once set with no master chosen, every
// Sync was refused. A port following a master that sends Sync and no
// Announce -- which this library supports on purpose -- was therefore put
// out of service by one Announce carrying stepsRemoved 255, which is what
// a message that has gone round a loop carries and what anyone on an
// unauthenticated multicast group can send.
static void testARefusedAnnounceDoesNotEndTheSynchronisation()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();
    setTxTimestamp(2000);

    // A master that sends no Announce is followed.
    ptp.feed(makeSync(1), 1000);
    ptp.feed(makeFollowUp(1, 500), 0);
    ptp.update();
    CHECK_EQ(countSentOfType(ptp, 1), 1);

    const uint8_t other[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ptp.feed(makeAnnounce(1, other, 128, 248, 128, MAX_STEPS_REMOVED), 0);
    CHECK(!ptp.hasSelectedMaster());
    CHECK(ptp.getPortState() == PortState::Slave);

    // And the next Sync of the master it was following still is.
    setTxTimestamp(4000);
    ptp.feed(makeSync(2), 3000);
    ptp.feed(makeFollowUp(2, 2500), 0);
    ptp.update();
    CHECK_EQ(countSentOfType(ptp, 1), 2);
}

// The interval a sketch announces is clamped, like the one off the wire.
//
// Peer to peer it is what paces the exchange: delayRequestIntervalMillis()
// hands it to logIntervalToMillis(), which shifts a thousand by it. An
// int8_t reaches 127 and unsigned long is 32 bits on the board, so a wide
// value is undefined behaviour there; what it produces here is the
// nonsense the cast in scheduleNextDelayRequest() leaves behind -- a bound
// of 1, which is a request on every pass through loop().
static void testTheAnnouncedDelayRequestIntervalIsClamped()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, true);
    ptp.begin();
    setTxTimestamp(1000);

    ptp.setLogMinDelayReqInterval(40);
    CHECK_EQ(ptp.getLogMinDelayReqInterval(), MAX_LOG_INTERVAL);
    ptp.update();
    CHECK_EQ(ptptest::state().randomBounds.size(), 1);
    if (!ptptest::state().randomBounds.empty())
    {
        // Twice 2^7 seconds, plus one.
        CHECK_EQ(ptptest::state().randomBounds[0], 256001);
    }

    ptp.setLogMinDelayReqInterval(-40);
    CHECK_EQ(ptp.getLogMinDelayReqInterval(), MIN_LOG_INTERVAL);
}

// A step of the clock moves the reference's own timestamp with it.
//
// The Sync path had its T2 corrected and the pin's pair did not, so the
// edge after a coarse step measured the step as drift: five microseconds
// stepped out came back a second later as five thousand nanoseconds per
// second of rate error, which is inside the frequency mode's window and
// so went into the frequency term and stayed there.
static void testAPpsStepDoesNotBecomeDrift()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();

    const NanoTime base = 4000000000LL;
    const NanoTime error = 5000;  // past coarseModeThresholdNs

    ptp.ppsInterruptTriggered(base, base + error);
    ptp.update();
    ptp.ppsInterruptTriggered(base + NS_PER_S, base + NS_PER_S + error);
    ptp.update();

    // The clock was stepped back by the whole offset.
    CHECK_EQ(ptptest::state().offsetTimerCalls.size(), 1);
    if (!ptptest::state().offsetTimerCalls.empty())
    {
        CHECK_EQ(ptptest::state().offsetTimerCalls[0], -error);
    }
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 0);

    // The third edge lands on a clock that is now on the reference: the
    // local pair advanced by a second less the step, which is no drift at
    // all once the step is accounted for.
    ptp.ppsInterruptTriggered(base + 2 * NS_PER_S, base + 2 * NS_PER_S);
    ptp.update();

    // The fine mode always writes a rate; what it writes is nothing.
    CHECK_EQ(ptp.getOffset(), 0);
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 1);
    if (!ptptest::state().adjustFreqCalls.empty())
    {
        CHECK_EQ(ptptest::state().adjustFreqCalls[0], 0.0);
    }
}

// A reference that has been away starts again from one edge.
//
// externalReferenceSeen never expired, so the edge that brought the pin
// back was measured against the pair from before the silence -- with the
// network holding the clock in the meantime, the step it made read as
// drift spread over the whole gap.
static void testAReferenceThatCameBackStartsAgain()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();

    const NanoTime base = 4000000000LL;
    ptp.ppsInterruptTriggered(base, base + 300);
    ptp.update();
    ptp.ppsInterruptTriggered(base + NS_PER_S, base + NS_PER_S + 300);
    ptp.update();
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 1);

    // The pin falls silent for longer than the reference is believed for.
    ptptest::state().millisNow += EXTERNAL_REFERENCE_TIMEOUT_MS + 1;
    const NanoTime later = base + 100 * NS_PER_S;
    ptp.ppsInterruptTriggered(later, later + 300);
    ptp.update();
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 1);

    // The pair that follows it is a pair again.
    ptp.ppsInterruptTriggered(later + NS_PER_S, later + NS_PER_S + 300);
    ptp.update();
    CHECK_EQ(ptptest::state().adjustFreqCalls.size(), 2);
}

// The Delay_Req does not spin for its T3 either.
//
// The last of the three waits: it ran inside update(), so a hardware that
// stopped posting timestamps cost a millisecond of every pass that sent a
// request. T3 comes from the same place the other two departure times do.
static void testTheDelayRequestDoesNotSpinForItsT3()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();
    ptp.setDelayFilterLength(1);

    const NanoTime base = 1000000000LL;
    const NanoTime pathDelay = 10000;
    const NanoTime turnaround = 500000000LL;
    runSlaveCycle(ptp, 1, base, base + pathDelay, base + turnaround,
                  base + turnaround + pathDelay);

    // The measurement is there, and micros() -- which only the wait loop
    // ever read -- was never touched.
    CHECK_EQ(ptp.getDelay(), pathDelay);
    CHECK_EQ(ptptest::state().microsNow, 0);
}

// A request whose T3 never arrives is given up at its deadline, and the
// register goes back to whoever needs it next.
static void testAnUnstampedRequestIsGivenUpOnItsDeadline()
{
    ptptest::state().reset();
    TestPTP ptp(false, true, false);
    ptp.begin();

    const NanoTime base = 1000000000LL;
    ptptest::state().txAvailable = false;
    ptp.feed(makeSync(1), base + 10000);
    ptp.feed(makeFollowUp(1, base), 0);
    ptp.update();
    CHECK_EQ(countSentOfType(ptp, 1), 1);

    // Still waiting, so nothing else may take the register: the next Sync
    // pair sends no request of its own.
    ptp.feed(makeSync(2), base + NS_PER_S + 10000);
    ptp.feed(makeFollowUp(2, base + NS_PER_S), 0);
    ptp.update();
    CHECK_EQ(countSentOfType(ptp, 1), 1);

    // Past the deadline it is given up, and the next pair asks again.
    ptptest::state().millisNow += DELAY_REQUEST_TIMEOUT_MS + 1;
    setTxTimestamp(base + 2 * NS_PER_S + 500000000LL);
    ptp.feed(makeSync(3), base + 2 * NS_PER_S + 10000);
    ptp.feed(makeFollowUp(3, base + 2 * NS_PER_S), 0);
    ptp.update();
    CHECK_EQ(countSentOfType(ptp, 1), 2);
}

// Every announced interval is clamped, not only the one that paces the
// peer-delay exchange.
static void testTheAnnouncedIntervalsAreClamped()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();

    ptp.setLogSyncInterval(40);
    CHECK_EQ(ptp.getLogSyncInterval(), MAX_LOG_INTERVAL);
    ptp.setLogSyncInterval(-40);
    CHECK_EQ(ptp.getLogSyncInterval(), MIN_LOG_INTERVAL);

    ptp.setLogAnnounceInterval(40);
    CHECK_EQ(ptp.getLogAnnounceInterval(), MAX_LOG_INTERVAL);
    ptp.setLogAnnounceInterval(-40);
    CHECK_EQ(ptp.getLogAnnounceInterval(), MIN_LOG_INTERVAL);

    // And what goes on the wire is the clamped value.
    ptp.setLogSyncInterval(3);
    setTxTimestamp(3000000000LL);
    ptp.syncMessage();
    CHECK_EQ(ptp.sent.size(), 1);
    if (!ptp.sent.empty())
    {
        CHECK_EQ((int8_t)ptp.sent[0].data[33], 3);
    }
}

// A second begin() re-initialises the port, sockets included.
//
// It used to reset the state and return, leaving the transport as it was:
// the chosen master, the delay window and the servo went, and the sockets
// stayed.
static void testASecondBeginReinitialisesTheSockets()
{
    ptptest::state().reset();
    TestPTP ptp(true, false, false);
    ptp.begin();
    CHECK_EQ(ptp.closeSocketsCalls, 0);
    CHECK(ptp.getPortState() == PortState::Master);

    ptp.begin();
    CHECK_EQ(ptp.closeSocketsCalls, 1);
    CHECK(ptp.getPortState() == PortState::Master);

    // And the port works: it is up, not half torn down.
    setTxTimestamp(3000000000LL);
    ptp.syncMessage();
    ptp.update();
    CHECK_EQ(countSentOfType(ptp, 0), 1);
    CHECK_EQ(countSentOfType(ptp, 8), 1);
}

void runPtpBaseTests()
{
    testBufferConversions();
    testSecondsAreClampedNotWrapped();
    testShortMessagesAreIgnored();
    testWrongVersionIsIgnored();
    testEndToEndSlaveCycle();
    testDelayResponseForAnotherPortIsIgnored();
    testEqualSyncTimestampsDoNotDriveTheServo();
    testCoarseOffsetStepsTheTimer();
    testPeerDelayCycle();
    testSlaveDoesNotAnswerDelayRequests();
    testMasterAnswersDelayRequest();
    testTheAnswerCarriesTheRequestsCorrection();
    testTheFollowUpsCorrectionLeavesTheDelay();
    testAnyPortAnswersAPeerDelayRequest();
    testAProfileReachesTheWire();
    testMasterSyncAndFollowUp();
    testMissingTxTimestampSkipsTheFollowUp();
    testSlaveMissingTxTimestampSkipsTheExchange();
    testAnnounceDataset();
    testAnnounceAndSyncNeedTheMasterRole();
    testPpsRequiresTheMasterRole();
    testPpsDrivesTheServoOnAMaster();
    testThePpsDoesNotThrowAwayTheMeasuredPathDelay();
    testAPeerDelayResponseIsRefusedEndToEnd();
    testPinningReleasesTheStateWithTheChoice();
    testABadFirstExchangeIsNotAPath();
    testAPortThatIsBothStillSlavesWithTheComparisonOff();
    testANegativeTimestampKeepsItsValue();
    testAnOversizedNanosecondsFieldIsClamped();
    testTheCorrectionFieldsTopOctetIsRead();
    testATimestampPlusItsCorrectionStopsAtTheEdge();
    testAStaleTransmitTimestampIsNotUsedAsT3();
    testTheHardwareClockIsZeroedOnceOnly();
    testARefusedAnnounceDoesNotEndTheSynchronisation();
    testTheAnnouncedDelayRequestIntervalIsClamped();
    testAPpsStepDoesNotBecomeDrift();
    testAReferenceThatCameBackStartsAgain();
    testDelayResponseForAnotherRequestIsIgnored();
    testDelayResponseForAnotherPortNumberIsIgnored();
    testDuplicateDelayResponseIsIgnored();
    testSyncSequenceAdvancesWithoutATxTimestamp();
    testResetClearsTheMeasurement();
    testResponseToAnAbandonedRequestIsRefused();
    testNewSyncBlocksTheOvertakenCycle();
    testPeerFollowUpBeforeItsResponseIsIgnored();
    testClockIdentityIsTheEui64Mapping();
    testOtherDomainsAreIgnored();
    testOneStepSyncIsAccepted();
    testSequenceIdZeroIsAccepted();
    testPeerDelayRequestIsAnswered();
    testPeerDelayRequestNeedsTheP2pRole();
    testDelayRequestsFollowTheAnnouncedInterval();
    testNonsensicalIntervalIsNotAdopted();
    testEndUndoesBegin();
    testIntegralTermIsBounded();
    testIntegralTermIsBoundedBelowZeroToo();
    testFrequencyModeAccumulatorIsBounded();
    testNormalConvergenceIsNotClamped();
    testLockIsLostWhenSyncStops();
    testTimeoutFollowsTheAnnouncedSyncInterval();
    testSyncAfterATimeoutStartsCleanly();
    testMasterDoesNotTimeOutOnItsOwnSilence();
    testAnnounceCarriesTheWholeDataset();
    testDelayIsTheMinimumOfTheWindow();
    testDelayFollowsARealChangeOnceTheWindowTurnsOver();
    testDelayFilterCanBeTurnedOff();
    testNegativeDelayIsNotStored();
    testTimestampOffsetIsAParameter();
    testPeerOffsetCorrectionIsAParameter();
    testFrequencyGainDampsTheStep();
    testTheBestMasterIsChosen();
    testTheDatasetDecidesWhenPriorityTies();
    testOnlyTheChosenMasterIsFollowed();
    testSyncIsFollowedBeforeAnyAnnounceArrives();
    testAnotherMasterTakesOverWhenTheChosenOneGoesQuiet();
    testTheChosenMasterMayDowngradeItself();
    testDelayResponseFromAnotherMasterIsIgnored();
    testEverySyncCorrectsTheClock();
    testNoOffsetBeforeADelayIsKnown();
    testReleasingAMasterDoesNotOpenThePort();
    testOurOwnAnnounceIsIgnored();
    testAPinnedMasterIsTheOnlyOneHeard();
    testDelayResponseCarriesTheRequestedInterval();
    testMajorSdoIdIsWrittenAndChecked();
    testPpsOffsetIsAParameter();
    testPeerDelayMessagesAskForThePeerGroup();
    testOnePeerAnswerAtATime();
    testSyncFinishesAPendingPeerAnswerFirst();
    testAPeerDelayFloodDoesNotSilenceTheSync();
    testAPeerRequestWaitsForAPendingSyncTimestamp();
    testTheDelayRequestDoesNotSpinForItsT3();
    testAnUnstampedRequestIsGivenUpOnItsDeadline();
    testTheAnnouncedIntervalsAreClamped();
    testASecondBeginReinitialisesTheSockets();
    testStaleDelaySamplesAreNotUsed();
    testTheServoThresholdsAreParameters();
    testEveryParameterReadsBack();
    testAMasterStepsAsideForABetterOne();
    testAMasterIgnoresAWorseOne();
    testTheComparisonCanBeTurnedOff();
    testAPortThatIsBothFollowsTheBetterClock();
    testASlaveListensBeforeItChooses();
    testTheStateFollowsTheLifecycle();
    testAMasterDoesNotFollowWhileItAnnounces();
    testOurOwnDatasetChangingDecidesAgain();
    testAHeldBackRequestDoesNotSpendItsTurn();
    testAStaleTransmitTimestampIsNotPublished();
    testAPassivePortDoesNotAnswerDelayRequests();
    testAnAnnounceFromTooFarAwayIsIgnored();
    testTheReferenceOutranksTheSync();
    testASyncCannotBeMistakenForTheReference();
    testTheSyncTakesTheClockBackWhenTheReferenceStops();
    testAPeerDelayMasterDoesNotAnswerDelayRequests();
    testAnEndToEndDelayResponseIsRefusedPeerToPeer();
    testAPeerFollowUpFromAnotherPortIsIgnored();
    testAPeerDelayPortMeasuresWithoutASync();
    testThePeerDelayRateIsTheConfiguredOne();
    testAnAnswerIsNotMixedWithTheNextRequest();

}
