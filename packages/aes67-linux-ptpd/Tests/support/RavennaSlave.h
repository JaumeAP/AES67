//
// RavennaSlave.h
// aes67-linux-ptpd - Tests
// What the RAVENNA ALSA kernel module accepts from a grandmaster.
//
// The interface to process_PTP_packet in the Merging module vendored in this
// repository, external/ravenna-alsa-lkm/driver/PTP.c:229-508. That file is
// kernel C: it reads whole UDP frames out of a netfilter hook and keeps its
// state in a device structure. It is compiled as it stands and called through
// the shim in RavennaSlave.cpp, so this daemon's Announce, Sync and Follow_Up
// are held against the real slave's own code rather than against IEEE 1588
// alone, and an upstream change shows up as a failing test.
//
// The constants below are the module's own values, repeated here for the
// cases that reason about its timeouts without feeding it a packet.
//
#pragma once

#include <cstddef>
#include <cstdint>

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
///
/// It owns the module's clock once one has been made, and frees it. Leaving
/// that to whoever wrote the case meant most of them did not, which nothing
/// on a Mac could say: LeakSanitizer is a Linux thing, and the suite leaked a
/// clock and two spinlocks per case until a runner counted them.
struct SlaveState {
    SlaveState() = default;
    ~SlaveState();
    /// Not copied: two states sharing one clock would free it twice.
    SlaveState(const SlaveState&) = delete;
    SlaveState& operator=(const SlaveState&) = delete;

    /// The module's own TClock_PTP, made on first use. Opaque here so the
    /// test needs none of the module's headers.
    void* impl = nullptr;
    uint8_t configuredDomain = 0;
    uint64_t masterClockIdentity = 0;  ///< 0 until an Announce elects one.
    uint64_t grandmasterIdentity = 0;
    uint16_t lastSyncSequenceId = 0;
    /// The module's own m_ui64T2, the arrival time it kept for the last Sync
    /// it took, in the unit PTP.c stores it: the counter clock divided by
    /// NS_2_REF_UNIT. A Sync the module ignores leaves it where it was.
    uint64_t syncArrivalTime = 0;
    bool haveSync = false;
    /// The PTP half of the module's lock, m_usPTPLockCounter == 0. The other
    /// half, m_usTICLockCounter, only moves in the audio frame timer, which
    /// does not run here, so GetLockStatus would never say PTPLS_LOCKED.
    bool locked = false;
};

/// What the module did with one packet.
struct SlaveVerdict {
    bool used = false;        ///< false is the module's DR_PACKET_NOT_USED.
    bool elected = false;     ///< this Announce elected or kept a master
    bool lockReset = false;   ///< a Sync gap reset the internal lock
};

/// One PTP payload, as the module would see it arriving on the event or the
/// general port. `destinationPort` is 319 or 320; anything else is not a PTP
/// packet to it at all.
SlaveVerdict feed(SlaveState& state, const uint8_t* data, size_t length,
                  uint16_t destinationPort);

/// True when the module would wait for a Follow_Up rather than take the
/// Sync's own origin timestamp (PTP.c:437, IS_PTP_TWO_STEP).
bool syncIsTwoStep(const uint8_t* data);

/// Moves the counter clock the module reads, in nanoseconds, which is what
/// the kernel's get_clock_time hands it: a test that wants time to pass says
/// so rather than waiting for it.
void setCounterTime(uint64_t timeNs);

/// Releases the module clock a state carries. The destructor calls this; it
/// stays for a case that wants to let go of the clock and carry on.
void destroy(SlaveState& state);

} // namespace AES67::LinuxPtpd::Tests
