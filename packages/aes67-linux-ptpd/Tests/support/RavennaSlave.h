//
// RavennaSlave.h
// aes67-linux-ptpd - Tests
// What the RAVENNA ALSA kernel module accepts from a grandmaster.
//
// Mirrors process_PTP_packet in the Merging module vendored in this
// repository, packages/aes67-linux-daemon/external/ravenna-alsa-lkm/
// driver/PTP.c:229-470. That code cannot be called
// from here: it is kernel C, it reads whole UDP frames out of a netfilter
// hook and it keeps its state in a device structure. What it decides, though,
// is a handful of tests on fixed offsets, and those are what this holds -- so
// this daemon's Announce, Sync and Follow_Up can be held against a real
// slave's rules rather than against IEEE 1588 alone.
//
// A mirror drifts when upstream moves. The suite that uses it names the lines
// it came from, and the constants below carry the module's own values.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace AES67::LinuxPtpd::Tests {

/// PTP.c:47. Sync sequence numbers further apart than this reset the lock.
inline constexpr uint16_t kLockHysteresis = 4;

/// PTP.c:56, in units of 100 ns: five seconds without an Announce from the
/// elected master and the election starts again.
inline constexpr uint64_t kAnnounceTimeout100ns = 50'000'000;

/// PTP.c:48, the module's own comment: "we assume to receive at least one
/// sync each 2s".
inline constexpr uint64_t kSyncWatchdogNs = 2'000'000'000;

/// The message sizes the module demands, as PTP payload rather than as its
/// own structures, which count the Ethernet, IP and UDP headers too:
/// TPTPPacketBase is the 34-byte header, an Announce adds 30 and a Sync or a
/// Follow_Up 10 (PTP_defs.h).
inline constexpr size_t kMinHeaderBytes = 34;
inline constexpr size_t kMinAnnounceBytes = 64;
inline constexpr size_t kMinSyncBytes = 44;

/// The slave's state: what it was configured with, and what it has elected.
struct SlaveState {
    uint8_t configuredDomain = 0;
    uint64_t masterClockIdentity = 0;  ///< 0 until an Announce elects one.
    uint64_t grandmasterIdentity = 0;
    uint16_t lastSyncSequenceId = 0;
    bool haveSync = false;
    bool locked = false;
};

/// What the module did with one packet.
struct SlaveVerdict {
    bool used = false;        ///< false is the module's DR_PACKET_NOT_USED.
    bool elected = false;     ///< this Announce elected or kept a master
    bool lockReset = false;   ///< a Sync gap reset the internal lock
    std::string reason;       ///< why it was not used, when it was not
};

/// One PTP payload, as the module would see it arriving on the event or the
/// general port. `destinationPort` is 319 or 320; anything else is not a PTP
/// packet to it at all.
SlaveVerdict feed(SlaveState& state, const uint8_t* data, size_t length,
                  uint16_t destinationPort);

/// True when the module would wait for a Follow_Up rather than take the
/// Sync's own origin timestamp (PTP.c:437, IS_PTP_TWO_STEP).
bool syncIsTwoStep(const uint8_t* data);

} // namespace AES67::LinuxPtpd::Tests
