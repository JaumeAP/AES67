//
// test_tone.cpp
// The 1 kHz tone this board sends, checked where it can be checked: in
// the bytes that go on the wire.
//
// A test tone is only worth anything if its level and frequency are what
// they claim, and if its timestamps follow the PTP clock rather than a
// counter of this board's own. Those are what this pins down -- the level
// in dBFS RMS, one whole cycle per packet, and a media clock that comes
// out of the same timeline the servo disciplines.
//
#include "audio/tone-sender.h"
#include "ptp_messages.h"
#include "stubs/stub_state.h"
#include "test_harness.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

class ToneTestPTP : public PTPBase
{
public:
    ToneTestPTP() : PTPBase(false, true, false) {}

    /// Hands a message to the parser, so a test can put this port under a
    /// master and see what the description then says about the clock.
    void feed(const std::vector<uint8_t> &msg)
    {
        timespec ts = {0, 0};
        parsePTPMessage(msg.data(), static_cast<int>(msg.size()), ts);
    }

private:
    void initSockets() override {}
    void closeSockets() override {}
    void updateSockets() override {}
    void sendPTPMessage(const uint8_t *, int, bool, bool) override {}
};

/// The board's clock, in nanoseconds.
void setClock(uint64_t nanos)
{
    ptptest::state().hardwareTime.tv_sec = static_cast<time_t>(nanos / 1000000000ULL);
    ptptest::state().hardwareTime.tv_nsec = static_cast<long>(nanos % 1000000000ULL);
}

int32_t sampleFrom(const std::vector<uint8_t> &packet, size_t frame)
{
    const size_t at = 12 + frame * 3;
    int32_t value = (static_cast<int32_t>(packet[at]) << 16) |
                    (static_cast<int32_t>(packet[at + 1]) << 8) |
                    static_cast<int32_t>(packet[at + 2]);
    if (value & 0x800000) value -= 0x1000000;   // sign extend the 24 bits
    return value;
}

uint32_t timestampOf(const std::vector<uint8_t> &packet)
{
    return (static_cast<uint32_t>(packet[4]) << 24) | (static_cast<uint32_t>(packet[5]) << 16) |
           (static_cast<uint32_t>(packet[6]) << 8) | static_cast<uint32_t>(packet[7]);
}

uint16_t sequenceOf(const std::vector<uint8_t> &packet)
{
    return static_cast<uint16_t>((packet[2] << 8) | packet[3]);
}

} // namespace

static void testTheLevelIsMinusTwentyDbfsRms()
{
    // The whole point of a test tone: a receiver at the other end has to
    // measure what this says it sends. One cycle is 48 samples, so the
    // RMS over a packet is the RMS of the tone.
    double sumOfSquares = 0.0;
    int32_t peak = 0;
    for (uint32_t n = 0; n < 48; n++)
    {
        const int32_t sample = ToneSender::sampleAt(n);
        sumOfSquares += static_cast<double>(sample) * static_cast<double>(sample);
        if (std::abs(sample) > peak) peak = std::abs(sample);
    }

    const double fullScale = 8388607.0;
    const double rms = std::sqrt(sumOfSquares / 48.0) / fullScale;
    const double dbfs = 20.0 * std::log10(rms);
    CHECK(std::fabs(dbfs - (-20.0)) < 0.05);

    // And the peak that goes with it: RMS times the square root of two.
    CHECK(std::fabs(static_cast<double>(peak) / fullScale - 0.1414) < 0.001);
}

static void testOneCyclePerPacketAtAKilohertz()
{
    // 1 kHz at 48 kHz is 48 samples a cycle, which is exactly one packet:
    // the tone repeats packet for packet and the phase never drifts.
    for (uint32_t n = 0; n < 480; n++)
    {
        CHECK_EQ(ToneSender::sampleAt(n), ToneSender::sampleAt(n + 48));
    }

    // A cycle really is a cycle: zero at the start, positive a quarter of
    // the way through, negative three quarters through.
    CHECK_EQ(ToneSender::sampleAt(0), 0);
    CHECK(ToneSender::sampleAt(12) > 0);
    CHECK(ToneSender::sampleAt(36) < 0);
}

static void testTheMediaClockIsThePtpClock()
{
    // A second of PTP time is 48000 samples, and the sample at a given
    // instant does not depend on when this board booted.
    timespec t{0, 0};
    CHECK_EQ(ToneSender::mediaTimeFrom(t), 0u);

    t.tv_sec = 1;
    CHECK_EQ(ToneSender::mediaTimeFrom(t), 48000u);

    t.tv_sec = 0;
    t.tv_nsec = 500000000;   // half a second
    CHECK_EQ(ToneSender::mediaTimeFrom(t), 24000u);

    // A sample is 20833.33 ns, and this truncates rather than rounds:
    // 20833 is still inside sample zero, 20834 is the first one after it.
    t.tv_sec = 3;
    t.tv_nsec = 20833;
    CHECK_EQ(ToneSender::mediaTimeFrom(t), 144000u);
    t.tv_nsec = 20834;
    CHECK_EQ(ToneSender::mediaTimeFrom(t), 144001u);
}

static void testItSendsOnePacketPerMillisecond()
{
    ptptest::state().reset();
    ToneTestPTP ptp;
    ptp.begin();

    ToneSender tone(ptp);
    tone.begin(IPAddress(239, 69, 0, 1), 5004);

    setClock(0);
    tone.update();                       // nothing due yet: this sets the grid
    CHECK_EQ(tone.getPacketCount(), 0u);

    // Ten milliseconds of clock is ten packets, and no more.
    setClock(10000000ULL);
    tone.update();
    CHECK_EQ(tone.getPacketCount(), 10u);

    const auto &sent = ptptest::state().udpTx;
    CHECK_EQ(sent.size(), 10);
    if (sent.size() == 10)
    {
        // Where it went, and how big: twelve bytes of header and 48
        // samples of three bytes.
        CHECK(sent[0].destination == "239.69.0.1:5004");
        CHECK_EQ(sent[0].data.size(), 12 + 48 * 3);

        // Sequence numbers walk, timestamps walk by a packet's worth of
        // samples, and the SSRC never moves.
        for (size_t i = 0; i < sent.size(); i++)
        {
            CHECK_EQ(sequenceOf(sent[i].data), static_cast<uint16_t>(i));
            if (i > 0)
            {
                CHECK_EQ(timestampOf(sent[i].data) - timestampOf(sent[i - 1].data), 48u);
            }
        }
    }
}

static void testThePacketsCarryTheToneAtTheirOwnTimestamp()
{
    ptptest::state().reset();
    ToneTestPTP ptp;
    ptp.begin();

    ToneSender tone(ptp);
    tone.begin(IPAddress(239, 69, 0, 1), 5004);
    setClock(0);
    tone.update();
    setClock(2000000ULL);                // two milliseconds
    tone.update();

    const auto &sent = ptptest::state().udpTx;
    CHECK_EQ(sent.size(), 2);
    if (sent.size() == 2)
    {
        // Every sample is the one the media timeline says belongs there,
        // which is what lets a receiver line two senders up against each
        // other.
        const uint32_t base = timestampOf(sent[1].data);
        for (size_t frame = 0; frame < 48; frame++)
        {
            CHECK_EQ(sampleFrom(sent[1].data, frame),
                     ToneSender::sampleAt(base + static_cast<uint64_t>(frame)));
        }

        // Version 2 and the payload type.
        CHECK_EQ(sent[0].data[0], 0x80);
        CHECK_EQ(sent[0].data[1], ToneSender::PAYLOAD_TYPE);
    }
}

static void testAStallDoesNotBecomeABurst()
{
    ptptest::state().reset();
    ToneTestPTP ptp;
    ptp.begin();

    ToneSender tone(ptp);
    tone.begin(IPAddress(239, 69, 0, 1), 5004);
    setClock(0);
    tone.update();

    // A whole second of loop() gone: a thousand packets are owed. None of
    // them is sent. They carry the audio of the gap, which is not what a
    // receiver wants back and not something the wire should be given as
    // fast as it takes it. The grid is picked up from the clock instead.
    //
    // The cap used to be reached first, so sixteen packets of that stale
    // audio went out before the resynchronisation.
    setClock(1000000000ULL);
    tone.update();
    CHECK_EQ(tone.getPacketCount(), 0u);

    // And it is back on the grid straight away, not a second behind: the
    // next millisecond of clock is the next packet.
    setClock(1001000000ULL);
    tone.update();
    CHECK_EQ(tone.getPacketCount(), 1u);

    // A gap the cap can still serve is served, not thrown away: sixteen
    // packets is what one update() may send.
    setClock(1017000000ULL);
    tone.update();
    CHECK_EQ(tone.getPacketCount(), 1u + ToneSender::MAX_PACKETS_PER_UPDATE);
}

// The servo steps the clock backwards in coarse mode -- offsetTimer()
// with a negative correction is how a slave gets on to its master's time
// -- and the media time of the next packet only ever moved forward. It
// was then in the future, the loop could send nothing until the clock
// caught up with it, and the stream went silent for the whole size of the
// step: a second for a second, and decades for a board that had been on
// TAI meeting a master counting from its own boot.
static void testAClockSteppedBackwardsDoesNotSilenceTheStream()
{
    ptptest::state().reset();
    ToneTestPTP ptp;
    ptp.begin();

    ToneSender tone(ptp);
    tone.begin(IPAddress(239, 69, 0, 1), 5004);

    setClock(2000000000ULL);
    tone.update();
    setClock(2010000000ULL);
    tone.update();
    CHECK_EQ(tone.getPacketCount(), 10u);

    // The clock goes back a second. The next packet is due one
    // millisecond later, on the grid of where the clock now is.
    setClock(1000000000ULL);
    tone.update();
    CHECK_EQ(tone.getPacketCount(), 10u);

    setClock(1001000000ULL);
    tone.update();
    CHECK_EQ(tone.getPacketCount(), 11u);

    const auto &sent = ptptest::state().udpTx;
    CHECK_EQ(sent.size(), 11);
    if (sent.size() == 11)
    {
        // Timestamped where the clock now says, not where it was.
        CHECK_EQ(timestampOf(sent[10].data), static_cast<uint32_t>(1000000000ULL / 1000000ULL * 48));
        // The sequence number carries on: RTP counts packets, not time.
        CHECK_EQ(sequenceOf(sent[10].data), 10);
    }
}

static void testAFailedSendIsCountedNotSwallowed()
{
    ptptest::state().reset();
    ToneTestPTP ptp;
    ptp.begin();

    ToneSender tone(ptp);
    tone.begin(IPAddress(239, 69, 0, 1), 5004);
    setClock(0);
    tone.update();

    ptptest::state().udpSendResult = false;
    setClock(3000000ULL);
    tone.update();

    CHECK_EQ(tone.getPacketCount(), 0u);
    CHECK_EQ(tone.getSendFailureCount(), 3u);

    // The sequence still walked: a hole in the stream is a hole, and
    // pretending those packets never existed would hide it from the
    // receiver.
    ptptest::state().udpSendResult = true;
    setClock(4000000ULL);
    tone.update();
    const auto &sent = ptptest::state().udpTx;
    CHECK_EQ(sent.size(), 1);
    if (!sent.empty()) CHECK_EQ(sequenceOf(sent[0].data), 3);
}

static void testTheStreamMarksItselfAndCanBeDescribed()
{
    ptptest::state().reset();
    ToneTestPTP ptp;
    ptp.begin();

    ToneSender tone(ptp);
    tone.begin(IPAddress(239, 69, 0, 1), 5004);

    // EF by default, which is what the AES67 guides mark media with.
    bool markedEF = false;
    for (const auto &entry : ptptest::state().udpDscps)
    {
        if (entry.second == 46) markedEF = true;
    }
    CHECK(markedEF);

    char sdp[512];
    const size_t length = tone.describe(sdp, sizeof(sdp), IPAddress(192, 168, 0, 40));
    CHECK(length > 0);
    const std::string text(sdp, length);
    CHECK(text.find("m=audio 5004 RTP/AVP 98") != std::string::npos);
    CHECK(text.find("a=rtpmap:98 L24/48000/1") != std::string::npos);
    CHECK(text.find("c=IN IP4 239.69.0.1/1") != std::string::npos);
    CHECK(text.find("a=ptime:1") != std::string::npos);
    CHECK(text.find("a=ts-refclk:ptp=IEEE1588-2008:") != std::string::npos);
    CHECK(text.find("a=sendonly") != std::string::npos);

    // A buffer too small says so rather than writing half a description.
    char small[16];
    CHECK_EQ(tone.describe(small, sizeof(small), IPAddress(192, 168, 0, 40)), 0u);
}


// ts-refclk names the clock the stream is timed by, which is how a
// receiver decides whether it shares a reference with the sender. It
// named this board's own identity whatever the board was following, so
// two boards slaved to one master each claimed a different reference for
// the same timeline. The domain, in both places it appears, was the
// literal 0 while the port can be on any domain at all.
static void testTheDescriptionNamesTheClockItIsActuallyOn()
{
    ptptest::state().reset();
    ToneTestPTP ptp;
    ptp.begin();
    ptp.setDomainNumber(3);

    const uint8_t master[8] = {0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0x01, 0x02, 0x03};
    std::vector<uint8_t> announce = makeAnnounce(1, master, /*priority1=*/10);
    announce[4] = 3;  // domainNumber, or this port would not listen
    ptp.feed(announce);
    CHECK(ptp.hasSelectedMaster());

    ToneSender tone(ptp);
    tone.begin(IPAddress(239, 69, 0, 1), 5004);

    char sdp[512];
    const size_t length = tone.describe(sdp, sizeof(sdp), IPAddress(192, 168, 0, 40));
    CHECK(length > 0);
    const std::string text(sdp, length);

    CHECK(text.find("a=ts-refclk:ptp=IEEE1588-2008:AA-BB-CC-FF-FE-01-02-03:3") !=
          std::string::npos);
    CHECK(text.find("a=clock-domain:PTPv2 3") != std::string::npos);

    // With nobody chosen, this board's own clock is the one to name.
    ptptest::state().reset();
    ToneTestPTP alone;
    alone.begin();
    ToneSender own(alone);
    own.begin(IPAddress(239, 69, 0, 1), 5004);
    const size_t ownLength = own.describe(sdp, sizeof(sdp), IPAddress(192, 168, 0, 40));
    CHECK(ownLength > 0);
    uint8_t id[8];
    expectedClockID(id);
    char expected[64];
    snprintf(expected, sizeof(expected),
             "a=ts-refclk:ptp=IEEE1588-2008:%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X:0", id[0],
             id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
    CHECK(std::string(sdp, ownLength).find(expected) != std::string::npos);
}


// The RTP timestamp wraps every 2^32 samples, and 2^32 is 89478485 cycles
// of this tone plus sixteen samples. Taking the phase from the wrapped
// value therefore stepped the tone by those sixteen samples once every 24
// hours and 51 minutes -- a click in the middle of a packet, in a signal
// whose only job is to be measured. The phase comes off the 64-bit media
// time now; only the header still wraps, because RTP says it must.
static void testTheToneDoesNotStepWhenTheTimestampWraps()
{
    ptptest::state().reset();
    ToneTestPTP ptp;
    ptp.begin();

    ToneSender tone(ptp);
    tone.begin(IPAddress(239, 69, 0, 1), 5004);

    // 89478.485 s of PTP time is media time 4294967280, the last packet
    // boundary before the timestamp comes round: the packet starting
    // there runs over the wrap at its seventeenth sample.
    const uint64_t lastBeforeWrap = 4294967280ULL;
    setClock(89478485000000ULL);
    tone.update();
    CHECK_EQ(ptptest::state().udpTx.size(), 0);

    setClock(89478486000000ULL);   // one packet later
    tone.update();
    setClock(89478487000000ULL);   // and the one after it
    tone.update();

    const auto &sent = ptptest::state().udpTx;
    CHECK_EQ(sent.size(), 2);
    if (sent.size() != 2)
    {
        return;
    }

    // The header carries the low 32 bits, so the second packet's
    // timestamp has come round while the first one's has not.
    CHECK_EQ(timestampOf(sent[0].data), static_cast<uint32_t>(lastBeforeWrap));
    CHECK_EQ(timestampOf(sent[1].data), 32u);

    // Both boundaries are whole cycles from the start of the timeline, so
    // both packets carry the tone from phase zero -- the same 48 samples
    // every other packet carries. Under a wrapped phase the first of them
    // stepped at its seventeenth sample and the second started sixteen
    // samples into the cycle.
    for (size_t frame = 0; frame < 48; frame++)
    {
        CHECK_EQ(sampleFrom(sent[0].data, frame), ToneSender::sampleAt(lastBeforeWrap + frame));
        CHECK_EQ(sampleFrom(sent[0].data, frame), ToneSender::sampleAt(frame));
        CHECK_EQ(sampleFrom(sent[1].data, frame), ToneSender::sampleAt(frame));
    }
}

void runToneTests()
{
    testTheLevelIsMinusTwentyDbfsRms();
    testOneCyclePerPacketAtAKilohertz();
    testTheMediaClockIsThePtpClock();
    testItSendsOnePacketPerMillisecond();
    testThePacketsCarryTheToneAtTheirOwnTimestamp();
    testAStallDoesNotBecomeABurst();
    testAClockSteppedBackwardsDoesNotSilenceTheStream();
    testAFailedSendIsCountedNotSwallowed();
    testTheStreamMarksItselfAndCanBeDescribed();
    testTheDescriptionNamesTheClockItIsActuallyOn();
    testTheToneDoesNotStepWhenTheTimestampWraps();
}
