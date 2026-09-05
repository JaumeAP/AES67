//
// tone-sender.cpp
// t41-ptp
//

#include "audio/tone-sender.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

/// Full scale for a 24-bit sample: what -20 dBFS is measured against.
constexpr float kFullScale24 = 8388607.0f;   // 2^23 - 1

/// The peak of the tone, in 24-bit counts.
///
/// -20 dBFS RMS is a tenth of full scale RMS, and a sine's peak is its
/// RMS times the square root of two. Written as the arithmetic rather
/// than as a number so it is checkable by eye: 0.1 * 1.41421 * 8388607.
const float kPeak24 = 0.1f * 1.41421356f * kFullScale24;

} // namespace

ToneSender::ToneSender(PTPBase &ptp_) : ptp(ptp_) {}

uint64_t ToneSender::mediaTimeFrom(const timespec &ptpTime)
{
    // Seconds first, then the nanoseconds, so nothing leaves 64 bits:
    // 999999999 times 48000 is 4.8e13, which fits, and the seconds part
    // is small.
    //
    // Handed back whole. It used to be truncated to the RTP timestamp's
    // 32 bits here, which threw away the only continuous form of the
    // media clock this class has.
    const uint64_t fromSeconds = static_cast<uint64_t>(ptpTime.tv_sec) * SAMPLE_RATE;
    const uint64_t fromNanos =
        (static_cast<uint64_t>(ptpTime.tv_nsec) * SAMPLE_RATE) / 1000000000ULL;
    return fromSeconds + fromNanos;
}

int32_t ToneSender::sampleAt(uint64_t mediaTime)
{
    // 1 kHz at 48 kHz is 48 samples a cycle exactly, so the phase comes
    // out of the media time with a modulo and there is no accumulator to
    // drift.
    //
    // Off the 64-bit media time, not the 32-bit RTP timestamp. 2^32 is
    // 89478485 cycles and sixteen samples, so a phase taken from the
    // wrapped value stepped by those sixteen samples every time the
    // timestamp came round -- once every 24 hours and 51 minutes, in a
    // signal that exists to be measured.
    const uint64_t samplesPerCycle = SAMPLE_RATE / TONE_HZ;
    const uint64_t phase = mediaTime % samplesPerCycle;
    const float angle = 2.0f * 3.14159265358979f * static_cast<float>(phase) /
                        static_cast<float>(samplesPerCycle);
    return static_cast<int32_t>(lrintf(kPeak24 * sinf(angle)));
}

void ToneSender::begin(const IPAddress &group_, uint16_t port_)
{
    group = group_;
    port = port_;

    // The SSRC has to be unique on the segment; the clock identity
    // already is, so two boards never collide and the same board keeps
    // its SSRC across reboots.
    const uint8_t *id = ptp.getClockIdentity();
    ssrc = (static_cast<uint32_t>(id[4]) << 24) | (static_cast<uint32_t>(id[5]) << 16) |
           (static_cast<uint32_t>(id[6]) << 8) | static_cast<uint32_t>(id[7]);

    socket.setMulticastTTL(MULTICAST_TTL);
    socket.setOutgoingDiffServ(dscp);

    sequence = 0;
    packetCount = 0;
    sendFailureCount = 0;
    started = false;
    running = true;
}

void ToneSender::end()
{
    if (!running) return;
    socket.stop();
    running = false;
    started = false;
}

void ToneSender::setDscp(uint8_t value)
{
    dscp = value;
    if (running) socket.setOutgoingDiffServ(dscp);
}

void ToneSender::sendPacket(uint64_t mediaTime)
{
    uint8_t packet[12 + FRAMES_PER_PACKET * CHANNELS * BYTES_PER_SAMPLE];

    // What goes in the header is the low 32 bits, which is what RTP asks
    // for and what wraps. The audio below is generated from the whole
    // thing.
    const uint32_t timestamp = static_cast<uint32_t>(mediaTime);

    packet[0] = 0x80;          // version 2, no padding, no extension, no CSRC
    packet[1] = PAYLOAD_TYPE;  // marker clear: this stream never stops to restart
    packet[2] = static_cast<uint8_t>(sequence >> 8);
    packet[3] = static_cast<uint8_t>(sequence & 0xff);
    packet[4] = static_cast<uint8_t>(timestamp >> 24);
    packet[5] = static_cast<uint8_t>((timestamp >> 16) & 0xff);
    packet[6] = static_cast<uint8_t>((timestamp >> 8) & 0xff);
    packet[7] = static_cast<uint8_t>(timestamp & 0xff);
    packet[8] = static_cast<uint8_t>(ssrc >> 24);
    packet[9] = static_cast<uint8_t>((ssrc >> 16) & 0xff);
    packet[10] = static_cast<uint8_t>((ssrc >> 8) & 0xff);
    packet[11] = static_cast<uint8_t>(ssrc & 0xff);

    size_t at = 12;
    for (uint16_t frame = 0; frame < FRAMES_PER_PACKET; frame++)
    {
        // The sample comes from the media timeline, not from a per-packet
        // counter: a packet that is late still carries the audio that
        // belongs at its own timestamp.
        const int32_t sample = sampleAt(mediaTime + frame);
        packet[at++] = static_cast<uint8_t>((sample >> 16) & 0xff);
        packet[at++] = static_cast<uint8_t>((sample >> 8) & 0xff);
        packet[at++] = static_cast<uint8_t>(sample & 0xff);
    }

    if (socket.send(group, port, packet, sizeof(packet)))
    {
        packetCount++;
    }
    else
    {
        // A send that failed is a hole the receiver will hear. Counted
        // rather than swallowed, like every other failure here.
        sendFailureCount++;
    }
    sequence++;
}

void ToneSender::update()
{
    if (!running) return;

    timespec now;
    if (!qindesign::network::EthernetIEEE1588.readTimer(now))
    {
        return;
    }
    const uint64_t mediaNow = mediaTimeFrom(now);

    if (!started)
    {
        // The first packet starts on a packet boundary of the PTP
        // timeline rather than wherever this board happened to boot, so
        // every sender on the network packetises on the same grid.
        nextTimestamp = mediaNow - (mediaNow % FRAMES_PER_PACKET);
        started = true;
    }

    // Where the media clock is against where this sender is, and the
    // widest gap one update() can honestly serve: the cap below, plus the
    // packet the loop needs to have elapsed before it sends anything.
    const int64_t behind = static_cast<int64_t>(mediaNow - nextTimestamp);
    const int64_t servable =
        static_cast<int64_t>(FRAMES_PER_PACKET) * (MAX_PACKETS_PER_UPDATE + 1);
    if (behind < 0 || behind > servable)
    {
        // Two ways out of the window, and the same answer to both: pick
        // the grid up from the clock and carry on from there.
        //
        // BEHIND ZERO is the clock stepping BACKWARDS, which is what the
        // servo does in coarse mode -- offsetTimer() with a negative
        // correction is how a slave gets on to its master's time. Only
        // ever moving nextTimestamp forward left it in the future, and
        // the loop below cannot send anything until the clock catches up
        // with it: a step of a second is a second of silence, and a slave
        // that had been on TAI meeting a master counting from its own
        // boot went quiet for as long as the difference, which is
        // decades.
        //
        // PAST WHAT IS SERVABLE is the stall the cap already handled, but
        // handled after sending sixteen packets of audio belonging to the
        // gap. The receiver has no use for those: what it wants is the
        // audio that belongs now.
        nextTimestamp = mediaNow - (mediaNow % FRAMES_PER_PACKET);
    }

    uint16_t sent = 0;
    while (static_cast<int64_t>(mediaNow - (nextTimestamp + FRAMES_PER_PACKET)) >= 0)
    {
        sendPacket(nextTimestamp);
        nextTimestamp += FRAMES_PER_PACKET;
        if (++sent >= MAX_PACKETS_PER_UPDATE)
        {
            // Whatever is still owed is skipped rather than queued: the
            // media clock says where we are, and catching up by sending
            // old audio faster than real time only moves the problem into
            // the receiver's buffer.
            nextTimestamp = mediaNow - (mediaNow % FRAMES_PER_PACKET);
            break;
        }
    }
}

size_t ToneSender::describe(char *buffer, size_t size, const IPAddress &origin) const
{
    if (buffer == nullptr || size == 0) return 0;

    // The clock the stream is timed by, which is not this board's own
    // unless this board is the one holding it. A receiver reads
    // ts-refclk to decide whether it shares a reference with the sender;
    // naming our own identity while following somebody else's clock told
    // it we were the grandmaster, and two boards slaved to one master
    // each claimed a different reference for the same timeline.
    const uint8_t *id = ptp.hasSelectedMaster() ? ptp.getSelectedMaster().grandmasterIdentity
                                                : ptp.getClockIdentity();
    char refclk[48];
    snprintf(refclk, sizeof(refclk), "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X", id[0], id[1],
             id[2], id[3], id[4], id[5], id[6], id[7]);

    // Both places the PTP domain appears were the literal 0, while the
    // port it describes can be on any domain setDomainNumber() was given.
    const unsigned domain = ptp.getDomainNumber();

    const int written = snprintf(
        buffer, size,
        "v=0\r\n"
        "o=- %lu 0 IN IP4 %u.%u.%u.%u\r\n"
        "s=t41-ptp 1 kHz tone\r\n"
        // The number after the group is the TTL the stream is sent with,
        // not a prefix length. It was the literal 32 while the socket has
        // always sent with MULTICAST_TTL, so the description promised a
        // reach the sender does not have.
        "c=IN IP4 %u.%u.%u.%u/%u\r\n"
        "t=0 0\r\n"
        "a=clock-domain:PTPv2 %u\r\n"
        "m=audio %u RTP/AVP %u\r\n"
        "a=rtpmap:%u L24/%lu/%u\r\n"
        "a=ptime:1\r\n"
        "a=ts-refclk:ptp=IEEE1588-2008:%s:%u\r\n"
        "a=mediaclk:direct=0\r\n"
        "a=sendonly\r\n",
        static_cast<unsigned long>(ssrc), origin[0], origin[1], origin[2], origin[3], group[0],
        group[1], group[2], group[3], unsigned{MULTICAST_TTL}, domain, port, PAYLOAD_TYPE,
        PAYLOAD_TYPE, static_cast<unsigned long>(SAMPLE_RATE), CHANNELS, refclk, domain);

    if (written <= 0 || static_cast<size_t>(written) >= size) return 0;
    return static_cast<size_t>(written);
}
