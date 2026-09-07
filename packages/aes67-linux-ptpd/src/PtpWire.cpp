#include "PtpWire.h"

#include <cstring>

namespace AES67::LinuxPtpd {
namespace {

void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

void put32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

uint16_t get16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

/// sec 5.3.3: six octets of seconds and four of nanoseconds, both unsigned
/// and both big-endian.
void putTimestamp(uint8_t* p, uint64_t timeNs) {
    const uint64_t seconds = timeNs / 1000000000ULL;
    const uint32_t nanos = static_cast<uint32_t>(timeNs % 1000000000ULL);
    put16(p, static_cast<uint16_t>((seconds >> 32) & 0xFFFF));
    put32(p + 2, static_cast<uint32_t>(seconds & 0xFFFFFFFFULL));
    put32(p + 6, nanos);
}

/// The 34 octets every message starts with. correctionField is left at zero:
/// this is an ordinary clock at the top of the hierarchy, it corrects nothing
/// it did not itself delay.
void putHeader(uint8_t* out, const PortContext& port, PTPMessageType type,
               uint16_t messageLength, uint16_t flags, uint16_t sequenceId,
               uint8_t controlField, int8_t logMessageInterval) {
    std::memset(out, 0, kHeaderSize);
    out[0] = static_cast<uint8_t>((port.majorSdoId << 4) |
                                  (static_cast<uint8_t>(type) & 0x0F));
    out[1] = 0x02;  // versionPTP 2, reserved nibble zero
    put16(out + 2, messageLength);
    out[4] = port.domainNumber;
    put16(out + 6, flags);
    // 8..15 correctionField, 16..19 reserved: already zero.
    std::memcpy(out + 20, port.clockIdentity.id.data(), 8);
    put16(out + 28, port.portNumber);
    put16(out + 30, sequenceId);
    out[32] = controlField;
    out[33] = static_cast<uint8_t>(logMessageInterval);
}

}  // namespace

bool parseHeader(const uint8_t* data, size_t length, PTPHeader& out) {
    if (data == nullptr || length < kHeaderSize) return false;

    out = PTPHeader{};
    out.transportAndType = data[0];
    out.versionPTP = data[1];
    out.messageLength = get16(data + 2);
    out.domainNumber = data[4];
    out.flagField = get16(data + 6);
    std::memcpy(out.sourcePortIdentity.clockIdentity.id.data(), data + 20, 8);
    out.sourcePortIdentity.portNumber = get16(data + 28);
    out.sequenceId = get16(data + 30);
    out.controlField = data[32];
    out.logMessageInterval = static_cast<int8_t>(data[33]);
    return true;
}

size_t buildAnnounce(uint8_t* out, size_t capacity, const PortContext& port,
                     const AnnounceDataset& dataset, uint16_t sequenceId,
                     int8_t logAnnounceInterval, uint64_t originTimeNs) {
    if (out == nullptr || capacity < kAnnounceSize) return 0;

    uint16_t flags = kFlagPtpTimescale;
    if (dataset.currentUtcOffsetValid) flags |= kFlagCurrentUtcOffsetValid;

    putHeader(out, port, PTPMessageType::Announce,
              static_cast<uint16_t>(kAnnounceSize), flags, sequenceId,
              kControlOther, logAnnounceInterval);

    // sec 13.5. The originTimestamp of an Announce is not used for
    // synchronisation and is sent as the current time rather than as zero,
    // which is what every implementation on the wire does.
    putTimestamp(out + 34, originTimeNs);
    put16(out + 44, static_cast<uint16_t>(dataset.currentUtcOffset));
    out[46] = 0;  // reserved
    out[47] = dataset.priority1;
    out[48] = dataset.clockClass;
    out[49] = dataset.clockAccuracy;
    put16(out + 50, dataset.offsetScaledLogVariance);
    out[52] = dataset.priority2;
    std::memcpy(out + 53, dataset.clockIdentity.id.data(), 8);
    put16(out + 61, dataset.stepsRemoved);
    out[63] = dataset.timeSource;
    return kAnnounceSize;
}

size_t buildSync(uint8_t* out, size_t capacity, const PortContext& port,
                 uint16_t sequenceId, int8_t logSyncInterval) {
    if (out == nullptr || capacity < kSyncSize) return 0;

    // Two-step: the Sync carries no usable time and the Follow_Up carries the
    // hardware transmit timestamp. A one-step master would need the NIC to
    // write the timestamp into this packet as it leaves, which is a different
    // capability and not one this daemon claims.
    putHeader(out, port, PTPMessageType::Sync,
              static_cast<uint16_t>(kSyncSize), kFlagTwoStep | kFlagPtpTimescale,
              sequenceId, kControlSync, logSyncInterval);
    std::memset(out + 34, 0, 10);
    return kSyncSize;
}

size_t buildFollowUp(uint8_t* out, size_t capacity, const PortContext& port,
                     uint16_t sequenceId, int8_t logSyncInterval,
                     uint64_t preciseOriginTimeNs) {
    if (out == nullptr || capacity < kFollowUpSize) return 0;

    putHeader(out, port, PTPMessageType::Follow_Up,
              static_cast<uint16_t>(kFollowUpSize), kFlagPtpTimescale,
              sequenceId, kControlFollowUp, logSyncInterval);
    putTimestamp(out + 34, preciseOriginTimeNs);
    return kFollowUpSize;
}

size_t buildDelayResp(uint8_t* out, size_t capacity, const PortContext& port,
                      const PTPPortIdentity& requester, uint16_t sequenceId,
                      int8_t logMinDelayReqInterval, uint64_t receiveTimeNs) {
    if (out == nullptr || capacity < kDelayRespSize) return 0;

    // sec 13.6: the sequenceId is the request's, not a counter of ours, and
    // the requesting port identity is what tells one slave's response from
    // another's on a group every one of them is listening to.
    putHeader(out, port, PTPMessageType::Delay_Resp,
              static_cast<uint16_t>(kDelayRespSize), kFlagPtpTimescale,
              sequenceId, kControlDelayResp, logMinDelayReqInterval);
    putTimestamp(out + 34, receiveTimeNs);
    std::memcpy(out + 44, requester.clockIdentity.id.data(), 8);
    put16(out + 52, requester.portNumber);
    return kDelayRespSize;
}

}  // namespace AES67::LinuxPtpd
