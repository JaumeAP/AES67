//
// tone-sender.h
// t41-ptp
//
// An AES67 transmit stream carrying a 1 kHz tone: one channel, 48 kHz,
// L24, one millisecond per packet.
//
// What it is for: a board that holds a PTP clock and sends nothing is a
// board you cannot check from the other end of the room. This puts a
// known signal on the network -- known level, known frequency, and a
// media clock derived from the same PTP time the rest of this library
// disciplines -- so a receiver can be pointed at it and the chain
// measured: lock, packet timing, and whether the audio arrives at the
// level it left at.
//
// THE MEDIA CLOCK IS THE PTP CLOCK. The RTP timestamp is the sample count
// of the PTP timeline, not a counter this board keeps: two boards started
// hours apart put the same sample at the same instant, which is what
// makes a tone from here worth comparing against anything else.
//
// The tone is generated, not stored: 1 kHz at 48 kHz is exactly 48
// samples a cycle, which is exactly one packet, so every packet carries
// one whole cycle and the phase is continuous across packets by
// construction.
//
// The phase comes off the 64-bit media time, not the 32-bit RTP
// timestamp the packet carries. RTP timestamps wrap, and 2^32 is not a
// multiple of 48: taking the phase from the wrapped value put a step of
// sixteen samples in the tone every time it came round, roughly once a
// day, which is a click in a signal whose whole purpose is to be
// measured. The RTP timestamp on the wire still wraps, because RTP says
// it must.
//
#pragma once

#include <QNEthernet.h>

#include "ptp/ptp-base.h"

class ToneSender
{
public:
    static constexpr uint32_t SAMPLE_RATE = 48000;
    /// One millisecond, AES67's default packet time and what every
    /// receiver accepts.
    static constexpr uint16_t FRAMES_PER_PACKET = 48;
    static constexpr uint8_t CHANNELS = 1;
    /// L24: three bytes a sample, big endian on the wire.
    static constexpr uint8_t BYTES_PER_SAMPLE = 3;
    /// A dynamic payload type, which is what AES67 uses for L24.
    static constexpr uint8_t PAYLOAD_TYPE = 98;
    static constexpr uint16_t TONE_HZ = 1000;
    /// The level, as RMS. A sine's peak is its RMS times the square root
    /// of two, so -20 dBFS RMS is 0.1 * 1.41421 = 0.14142 of full scale.
    static constexpr float LEVEL_DBFS_RMS = -20.0f;

    /// How many packets one update() will send when it is behind. A board
    /// that stalled for a second must not then transmit a second of audio
    /// as fast as the wire takes it: the receiver's buffer would overrun
    /// and the catch-up would be worse than the gap.
    static constexpr uint16_t MAX_PACKETS_PER_UPDATE = 16;

    /// The TTL the stream is sent with, and the one its description
    /// advertises. One value, because the two used to disagree.
    static constexpr uint8_t MULTICAST_TTL = 1;

    explicit ToneSender(PTPBase &ptp);

    /// Starts sending to a multicast group. AES67 audio lives in 239.x.
    void begin(const IPAddress &group, uint16_t port = 5004);
    void end();

    /// The DSCP outgoing audio carries. EF (46) is what the AES67 guides
    /// mark media with; 0 leaves it unmarked.
    void setDscp(uint8_t dscp);

    /// Call from loop(). Sends whatever the media clock says is due.
    void update();

    uint32_t getPacketCount() const { return packetCount; }
    uint32_t getSendFailureCount() const { return sendFailureCount; }
    /// The SSRC this stream carries, derived from the clock identity so
    /// two boards on one segment do not collide.
    uint32_t getSSRC() const { return ssrc; }

    /// Writes the SDP a receiver needs. Returns the length written, or 0
    /// when the buffer is too small. `origin` is this board's own address,
    /// which the description has to name and this class has no other way
    /// of knowing.
    size_t describe(char *buffer, size_t size, const IPAddress &origin) const;

    /// The sample at a point on the media timeline, as the 24-bit value
    /// that goes on the wire. Static and pure, so what the tone IS can be
    /// checked without a socket.
    ///
    /// The media time, not the RTP timestamp: see the note at the top of
    /// this file for why the difference matters once a day.
    static int32_t sampleAt(uint64_t mediaTime);

    /// The media clock: the PTP timeline counted in samples. Sixty-four
    /// bits, so it does not come round; the low thirty-two of it are the
    /// RTP timestamp.
    static uint64_t mediaTimeFrom(const timespec &ptpTime);

private:
    void sendPacket(uint64_t mediaTime);

    PTPBase &ptp;
    qindesign::network::EthernetUDP socket;

    IPAddress group{};
    uint16_t port = 5004;
    bool running = false;
    uint8_t dscp = 46;

    uint32_t ssrc = 0;
    uint16_t sequence = 0;
    /// The media time of the next packet, picked up from the clock on the
    /// first update().
    uint64_t nextTimestamp = 0;
    bool started = false;

    uint32_t packetCount = 0;
    uint32_t sendFailureCount = 0;
};
