//
// RavennaSlave.cpp
// aes67-linux-ptpd - Tests
//

#include "support/RavennaSlave.h"

#include <cstring>

namespace AES67::LinuxPtpd::Tests {
namespace {

/// PTP_defs.h:131-139. The module switches on the low nibble of octet 0.
constexpr uint8_t kSync = 0;
constexpr uint8_t kFollowUp = 8;
constexpr uint8_t kAnnounce = 11;

/// PTP_defs.h:71. The module tests the flag field's second octet, which is
/// the twoStepFlag of IEEE 1588-2008 section 13.3.2.6.
constexpr uint8_t kTwoStepBit = 0x02;

uint16_t beU16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

/// The module reads the clock identity as a uint64_t straight out of the
/// packet and compares it with another read the same way, so the byte order
/// never matters to it: what matters is that the same eight bytes compare
/// equal. This does the same thing, portably.
uint64_t identityOf(const uint8_t* p) {
    uint64_t value = 0;
    std::memcpy(&value, p, 8);
    return value;
}

} // namespace

SlaveVerdict feed(SlaveState& state, const uint8_t* data, size_t length,
                  uint16_t destinationPort) {
    SlaveVerdict verdict;

    // PTP.c:238. 319 is the event port, 320 the general port.
    if (destinationPort != 319 && destinationPort != 320) {
        verdict.reason = "not the PTP event or general port";
        return verdict;
    }

    // PTP.c:243, against sizeof(TPTPPacketBase).
    if (length < kMinHeaderBytes) {
        verdict.reason = "shorter than a PTP header";
        return verdict;
    }

    // PTP.c:266. The low nibble of octet 1 is versionPTP.
    if ((data[1] & 0x0F) != 2) {
        verdict.reason = "not PTP version 2";
        return verdict;
    }

    const uint8_t messageType = data[0] & 0x0F;
    const uint8_t domain = data[4];
    const uint16_t sequenceId = beU16(data + 30);
    const uint64_t sourceIdentity = identityOf(data + 20);

    switch (messageType) {
    case kAnnounce: {
        // PTP.c:280, against sizeof(TPTPV2MsgAnnouncePacket).
        if (length < kMinAnnounceBytes) {
            verdict.reason = "shorter than an Announce";
            return verdict;
        }
        // PTP.c:319 and its else at 365: an Announce on another domain is
        // read and dropped, with "Announced domain %d look for domain %d".
        if (domain != state.configuredDomain) {
            verdict.used = true;
            verdict.reason = "announced domain is not the configured one";
            return verdict;
        }
        // PTP.c:310-314: the elected master changing domain restarts the
        // election. PTP.c:331: with no master elected, the first Announce on
        // the right domain takes it.
        if (state.masterClockIdentity == 0) {
            state.masterClockIdentity = sourceIdentity;
        }
        if (sourceIdentity == state.masterClockIdentity) {
            // PTP.c:352-360: the grandmaster identity of the Announce body,
            // at offset 53 of the message.
            state.grandmasterIdentity = identityOf(data + 53);
            verdict.elected = true;
        }
        verdict.used = true;
        return verdict;
    }

    case kSync: {
        // PTP.c:383, against sizeof(TPTPV2MsgSyncPacket).
        if (length < kMinSyncBytes) {
            verdict.reason = "shorter than a Sync";
            return verdict;
        }
        // PTP.c:392: a Sync from anything but the elected master is dropped.
        if (sourceIdentity != state.masterClockIdentity) {
            verdict.reason = "Sync from a clock that is not the elected master";
            return verdict;
        }
        // PTP.c:407-412: sequence numbers must be contiguous within the
        // hysteresis, or the lock is dropped and rebuilt.
        if (state.haveSync) {
            const uint16_t delta =
                static_cast<uint16_t>(sequenceId - state.lastSyncSequenceId);
            if (delta > kLockHysteresis) {
                verdict.lockReset = true;
                state.locked = false;
            } else {
                state.locked = true;
            }
        }
        state.haveSync = true;
        state.lastSyncSequenceId = sequenceId;
        verdict.used = true;
        return verdict;
    }

    case kFollowUp:
        if (length < kMinSyncBytes) {
            verdict.reason = "shorter than a Follow_Up";
            return verdict;
        }
        if (sourceIdentity != state.masterClockIdentity) {
            verdict.reason = "Follow_Up from a clock that is not the elected master";
            return verdict;
        }
        verdict.used = true;
        return verdict;

    default:
        verdict.reason = "a message type this slave does not act on";
        return verdict;
    }
}

/// PTP.c:437, through PTP_defs.h:71. The module reads the flag field as a
/// uint16_t without swapping it and tests bit 1 of the low byte of the value,
/// which on a little-endian host is octet 6 of the message -- its own comment
/// says exactly that. Octet 6 is where IEEE 1588-2008 section 13.3.2.6 puts
/// twoStepFlag, so the module is right, and reading octet 7 instead would
/// pass this test on a packet whose second flag octet happened to carry
/// something in that bit.
bool syncIsTwoStep(const uint8_t* data) { return (data[6] & kTwoStepBit) == kTwoStepBit; }

} // namespace AES67::LinuxPtpd::Tests
