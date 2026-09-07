//
// PacketBudget.h
// AES67 Core
//
// How much audio one RTP packet can carry, and therefore how many channels
// one flow can hold at a given packet time.
//
// The limit on a flow is bytes, not channels. AES67's "up to 8 channels" is
// what fits in a 1500-byte Ethernet frame at 48 kHz and 1 ms: eight channels
// of L24 make 1152 bytes of audio, ten would still fit, nine at 96 kHz would
// not. RAVENNA and ST 2110-30 Level C carry 64 channels in one flow by
// sending shorter packets -- 64 channels of L24 at 125 us are 1164 bytes with
// the RTP header, well inside the frame -- and no packet time makes 64
// channels fit at 1 ms: that is 9228 bytes, over even a 9000-byte jumbo
// frame's 8972.
//
// So the transmit path splits a wide stream into flows sized by this budget,
// and stream validation refuses a channel count and packet time that do not
// fit together, in both directions, before a socket is opened: the receiver
// drops any packet over the frame size (RTPReceiver::validatePacket), so a
// stream that cannot be sent whole cannot be received either.
//
// Freestanding arithmetic, usable in a constant expression; no allocation,
// no platform.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace AES67 {
namespace PacketBudget {

/// The fixed RTP header. AES67 senders use neither a CSRC list nor an
/// extension, and this driver sends neither.
inline constexpr size_t kRtpHeaderBytes = 12;

/// IPv4 header plus UDP header, what the frame spends before the RTP packet.
inline constexpr size_t kIpv4UdpOverheadBytes = 20 + 8;

/// The Ethernet MTU this driver assumes. No jumbo frames: a setting nobody
/// can verify end to end (every switch in the path has to agree) is not a
/// thing to size buffers by.
inline constexpr size_t kEthernetMtuBytes = 1500;

/// The largest RTP packet, header included, that goes out in one frame.
inline constexpr size_t kMaxRtpPacketBytes = kEthernetMtuBytes - kIpv4UdpOverheadBytes;

/// Audio bytes one packet can carry once the header is paid for.
inline constexpr size_t kMaxAudioBytesPerPacket = kMaxRtpPacketBytes - kRtpHeaderBytes;

/// Bytes one sample of one channel takes on the wire, or 0 for an encoding
/// this driver does not carry.
constexpr size_t bytesPerSample(std::string_view encoding) {
    if (encoding == "L16") return 2;
    if (encoding == "L24") return 3;
    if (encoding == "AM824") return 4;
    return 0;
}

/// Samples per channel in one packet. An explicit framecount (a RAVENNA
/// extension) wins; otherwise the packet time at the sample rate, truncated
/// the same way RTPTransmitter and RTPReceiver truncate it, so the three
/// never disagree about what a packet holds.
constexpr uint32_t framesPerPacket(uint32_t sampleRate, uint32_t ptimeUs, uint32_t framecount) {
    if (framecount > 0) return framecount;
    return static_cast<uint32_t>((static_cast<uint64_t>(sampleRate) * ptimeUs) / 1000000ULL);
}

/// The RTP packet, header included, for this many channels at this many
/// samples each.
constexpr size_t rtpPacketBytes(uint16_t channels, size_t bytesPerSample, uint32_t framesPerPacket) {
    return kRtpHeaderBytes +
           static_cast<size_t>(channels) * bytesPerSample * static_cast<size_t>(framesPerPacket);
}

/// True when the packet goes out in one frame.
constexpr bool fits(uint16_t channels, size_t bytesPerSample, uint32_t framesPerPacket) {
    return rtpPacketBytes(channels, bytesPerSample, framesPerPacket) <= kMaxRtpPacketBytes;
}

/// The most channels one packet holds at this sample width and packet
/// length: 10 for L24 at 48 samples (1 ms at 48 kHz), 5 at 96, 81 at 6
/// (125 us at 48 kHz). Zero when a single channel does not fit, or when the
/// packet would hold no samples at all.
constexpr uint16_t maxChannelsPerPacket(size_t bytesPerSample, uint32_t framesPerPacket) {
    if (bytesPerSample == 0 || framesPerPacket == 0) return 0;
    const size_t perChannel = bytesPerSample * static_cast<size_t>(framesPerPacket);
    const size_t channels = kMaxAudioBytesPerPacket / perChannel;
    return channels > 0xFFFF ? uint16_t{0xFFFF} : static_cast<uint16_t>(channels);
}

/// The most samples per channel one packet holds for this many channels:
/// what to tell someone whose stream does not fit -- 7 for 64 channels of
/// L24, which at 48 kHz is a packet time of 125 us and not 1 ms. Zero when
/// not even one sample per channel fits.
constexpr uint32_t maxFramesPerPacket(uint16_t channels, size_t bytesPerSample) {
    if (channels == 0 || bytesPerSample == 0) return 0;
    const size_t perFrame = static_cast<size_t>(channels) * bytesPerSample;
    return static_cast<uint32_t>(kMaxAudioBytesPerPacket / perFrame);
}

} // namespace PacketBudget
} // namespace AES67
