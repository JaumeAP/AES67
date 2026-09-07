//
// PtpWire.h
// AES67 Linux PTP daemon
// The bytes of the four messages a grandmaster puts on the wire, and the
// header it reads back.
//
// Platform-free on purpose, and the reason this file is separate from
// everything else in the package: a socket cannot be tested without a
// network, and a byte layout can. What builds here builds on the Mac this was
// written on and on the Pi it runs on, and Tests/TestPtpWire.cpp exercises it
// on either.
//
// The types come from packages/aes67-core (NetworkEngine/PTP/
// PTPProtocolTypes.h) rather than from a copy: the identities and timestamps
// a message carries are the same ones the rest of the repository already
// agrees on.
//
#pragma once

#include "NetworkEngine/PTP/PTPProtocolTypes.h"

#include <cstddef>
#include <cstdint>

namespace AES67::LinuxPtpd {

/// IEEE 1588-2008 Annex D: the event and general ports, and the group every
/// non-peer message goes to.
inline constexpr uint16_t kEventPort = 319;
inline constexpr uint16_t kGeneralPort = 320;
inline constexpr char kPtpPrimaryGroup[] = "224.0.1.129";

/// Message sizes, header included. IEEE 1588-2008 Table 19 and sec 13.
inline constexpr size_t kHeaderSize = 34;
inline constexpr size_t kSyncSize = 44;
inline constexpr size_t kFollowUpSize = 44;
inline constexpr size_t kDelayRespSize = 54;
inline constexpr size_t kAnnounceSize = 64;
/// Nothing this daemon reads is longer than an Announce with a suffix it
/// ignores; the receive buffer is sized for a normal frame regardless.
inline constexpr size_t kMaxMessageSize = 1500;

/// sec 13.3.2.6, the flags of octets 6 and 7, as one 16-bit field whose high
/// byte is octet 6.
inline constexpr uint16_t kFlagTwoStep = 0x0200;
inline constexpr uint16_t kFlagUnicast = 0x0400;
inline constexpr uint16_t kFlagPtpTimescale = 0x0008;
inline constexpr uint16_t kFlagCurrentUtcOffsetValid = 0x0004;
inline constexpr uint16_t kFlagTimeTraceable = 0x0010;
inline constexpr uint16_t kFlagFrequencyTraceable = 0x0020;

/// sec 13.3.2.10: the controlField of the messages this daemon sends. Kept
/// for version 1 compatibility and ignored by version 2 receivers, which read
/// the messageType instead -- but a wrong value here is still a wrong message.
inline constexpr uint8_t kControlSync = 0x00;
inline constexpr uint8_t kControlFollowUp = 0x02;
inline constexpr uint8_t kControlDelayResp = 0x03;
inline constexpr uint8_t kControlOther = 0x05;

/// What a grandmaster announces about itself. Every field is a decision of
/// this clock rather than of the profile, which is why none of them come from
/// the shared profile table.
struct AnnounceDataset {
    PTPClockIdentity clockIdentity{};
    uint8_t priority1 = 128;
    uint8_t priority2 = 128;
    uint8_t clockClass = 248;
    uint8_t clockAccuracy = 0xFE;
    uint16_t offsetScaledLogVariance = 0xFFFF;
    uint16_t stepsRemoved = 0;
    uint8_t timeSource = 0xA0;  ///< sec 7.6.2.6: INTERNAL_OSCILLATOR
    int16_t currentUtcOffset = 37;
    bool currentUtcOffsetValid = false;
};

/// What every message this daemon sends has in common.
struct PortContext {
    PTPClockIdentity clockIdentity{};
    uint16_t portNumber = 1;
    uint8_t domainNumber = 0;
    uint8_t majorSdoId = 0;
};

/// The header fields a receiver needs from us. Returns false when the buffer
/// is too short to hold one, which is the only way this can fail: every field
/// is a fixed offset.
bool parseHeader(const uint8_t* data, size_t length, PTPHeader& out);

/// The requesting port identity of a Delay_Req is its source port identity,
/// so parseHeader is all a Delay_Resp needs. These four return the number of
/// bytes written, which is the message's own size, and write nothing if the
/// buffer is smaller than that.
size_t buildAnnounce(uint8_t* out, size_t capacity, const PortContext& port,
                     const AnnounceDataset& dataset, uint16_t sequenceId,
                     int8_t logAnnounceInterval, uint64_t originTimeNs);

size_t buildSync(uint8_t* out, size_t capacity, const PortContext& port,
                 uint16_t sequenceId, int8_t logSyncInterval);

size_t buildFollowUp(uint8_t* out, size_t capacity, const PortContext& port,
                     uint16_t sequenceId, int8_t logSyncInterval,
                     uint64_t preciseOriginTimeNs);

size_t buildDelayResp(uint8_t* out, size_t capacity, const PortContext& port,
                      const PTPPortIdentity& requester, uint16_t sequenceId,
                      int8_t logMinDelayReqInterval, uint64_t receiveTimeNs);

}  // namespace AES67::LinuxPtpd
