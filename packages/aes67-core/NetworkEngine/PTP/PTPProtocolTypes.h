//
// PTPProtocolTypes.h
// AES67 macOS Driver
// The IEEE 1588-2008 types the PTP engines exchange, and the configuration
// they are built from.
//
// Everything here is plain data: wire message layouts, clock identities,
// announce datasets, and the two engine configs. No sockets, no threads, no
// state -- the engines that own those live in the macOS driver package
// (NetworkEngine/PTP/PTPSlave.h and PTPMaster.h), which includes this.
//
// Split out so that what is genuinely the protocol can be reasoned about,
// compared and tested without a socket: the BMCA comparison (PTPBMCA.h) and
// the settings mapping (PTPSettingsMapping.h) are both pure functions over
// these types, and both sat in the driver package purely because the types
// they read did.
//
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

// The dataset comparison of IEEE 1588 §9.3 and the dataset it compares live in
// the Teensy PTP library, packages/t41-ptp: that implementation came first and
// this one was written from it, so rather than keep a second copy that can
// drift, this includes it. The header is plain data and one pure function --
// nothing of the board comes with it.
#include "ptp/ptp-bmca.h"

namespace AES67 {

// ============================================================================
// IEEE 1588 PTP Message Types
// ============================================================================

// IEEE 1588-2008 sec 8.2.5.4.4: how this port measures the path.
// End to end is Delay_Req/Delay_Resp with the master and is what AES67
// deployments use; peer to peer measures the link to the immediate
// neighbour with Pdelay, which is what 802.1AS and transparent-clock
// topologies use.
enum class DelayMechanism : uint8_t {
    EndToEnd,
    PeerToPeer,
};

enum class PTPMessageType : uint8_t {
    Sync           = 0x00,
    Delay_Req      = 0x01,
    Pdelay_Req     = 0x02,
    Pdelay_Resp    = 0x03,
    Follow_Up      = 0x08,
    Delay_Resp     = 0x09,
    Pdelay_Resp_FU = 0x0A,
    Announce       = 0x0B,
    Signaling      = 0x0C,
    Management     = 0x0D,
};

// ============================================================================
// PTP Timestamp (IEEE 1588 Section 5.3.3)
// ============================================================================

struct PTPTimestamp {
    uint16_t secondsHi;    // Upper 16 bits of seconds
    uint32_t secondsLo;    // Lower 32 bits of seconds
    uint32_t nanoseconds;  // Nanoseconds (0 - 999,999,999)

    PTPTimestamp() : secondsHi(0), secondsLo(0), nanoseconds(0) {}

    PTPTimestamp(uint64_t totalNs) {
        uint64_t totalSec = totalNs / 1000000000ULL;
        secondsHi = static_cast<uint16_t>((totalSec >> 32) & 0xFFFF);
        secondsLo = static_cast<uint32_t>(totalSec & 0xFFFFFFFF);
        nanoseconds = static_cast<uint32_t>(totalNs % 1000000000ULL);
    }

    uint64_t toNanoseconds() const {
        uint64_t totalSec = (static_cast<uint64_t>(secondsHi) << 32) |
                            static_cast<uint64_t>(secondsLo);
        return totalSec * 1000000000ULL + nanoseconds;
    }

    bool isZero() const {
        return secondsHi == 0 && secondsLo == 0 && nanoseconds == 0;
    }
};

// ============================================================================
// PTP Clock Identity (IEEE 1588 Section 5.3.4)
// ============================================================================

struct PTPClockIdentity {
    std::array<uint8_t, 8> id;

    PTPClockIdentity() { id.fill(0); }

    bool operator==(const PTPClockIdentity& other) const { return id == other.id; }
    bool operator!=(const PTPClockIdentity& other) const { return id != other.id; }

    std::string toString() const;

    // Build from MAC address (EUI-48 to EUI-64 conversion)
    static PTPClockIdentity fromMAC(const uint8_t mac[6]);
};

// ============================================================================
// PTP Port Identity (IEEE 1588 Section 5.3.5)
// ============================================================================

struct PTPPortIdentity {
    PTPClockIdentity clockIdentity;
    uint16_t portNumber;

    PTPPortIdentity() : portNumber(0) {}

    bool operator==(const PTPPortIdentity& other) const {
        return clockIdentity == other.clockIdentity && portNumber == other.portNumber;
    }
};

// ============================================================================
// PTP Common Header (IEEE 1588 Section 13.3)
// ============================================================================

struct PTPHeader {
    uint8_t transportAndType;     // transportSpecific (4 bits) | messageType (4 bits)
    uint8_t versionPTP;           // Reserved (4 bits) | versionPTP (4 bits)
    uint16_t messageLength;
    uint8_t domainNumber;
    uint8_t reserved1;
    uint16_t flagField;
    int64_t correctionField;      // 64-bit fixed point (ns * 2^16)
    uint32_t reserved2;
    PTPPortIdentity sourcePortIdentity;
    uint16_t sequenceId;
    uint8_t controlField;
    int8_t logMessageInterval;

    PTPMessageType getMessageType() const {
        return static_cast<PTPMessageType>(transportAndType & 0x0F);
    }

    // The top nibble of octet 0: majorSdoId in IEEE 1588-2019, and
    // transportSpecific in 1588-2008. 0 is the default profile, 1 is what
    // 802.1AS/gPTP puts there. It was parsed but never looked at, so a gPTP
    // master on the same segment and domain was followed as if it belonged
    // to this profile.
    uint8_t getMajorSdoId() const {
        return static_cast<uint8_t>((transportAndType >> 4) & 0x0F);
    }
};

// ============================================================================
// PTP Announce Message Data (for BMCA)
// ============================================================================

struct PTPAnnounceData {
    /// Everything §9.3 compares -- priorities, class, accuracy, variance,
    /// grandmaster identity, steps removed and the sending port -- is the
    /// Teensy library's MasterDataset, not a second declaration of the same
    /// fields. isBetterMaster() reads exactly this.
    MasterDataset dataset;

    /// What this driver needs and the comparison does not look at.
    uint8_t timeSource;
    int8_t logAnnounceInterval;
    std::chrono::steady_clock::time_point lastReceived;

    /// The grandmaster identity as this codebase's own type, for printing and
    /// comparing against the identity a slave is locked to.
    PTPClockIdentity grandmasterId() const {
        PTPClockIdentity id{};
        for (size_t i = 0; i < id.id.size(); ++i) id.id[i] = dataset.grandmasterIdentity[i];
        return id;
    }

    void setGrandmasterId(const PTPClockIdentity& id) {
        for (size_t i = 0; i < id.id.size(); ++i) dataset.grandmasterIdentity[i] = id.id[i];
    }

    /// The sending port, packed the way an Announce carries it: the eight
    /// identity bytes then the port number, big-endian.
    void setSourcePort(const PTPPortIdentity& port) {
        for (size_t i = 0; i < port.clockIdentity.id.size(); ++i)
            dataset.portIdentity[i] = port.clockIdentity.id[i];
        dataset.portIdentity[8] = static_cast<uint8_t>((port.portNumber >> 8) & 0xFF);
        dataset.portIdentity[9] = static_cast<uint8_t>(port.portNumber & 0xFF);
    }
};

// ============================================================================
// PTP Slave Configuration
// ============================================================================

struct PTPSlaveConfig {
    int domain = 0;                              // PTP domain number
    std::string interfaceName = "en0";           // Network interface
    int delayReqIntervalMs = 1000;               // Delay_Req interval (ms)
    int announceTimeoutMultiplier = 3;           // Announce receipt timeout multiplier
    int announceIntervalMs = 1000;               // Expected announce interval
    bool twoStepOnly = true;                     // Only accept two-step clocks (AES67)

    // DSCP to mark this port's outgoing PTP with, or -1 to leave it
    // unmarked, which is what the stack sends and what this driver has
    // always sent.
    //
    // A switch that treats DSCP puts unmarked traffic in the same queue as
    // everything else, so on a loaded segment the PTP queues behind the
    // audio it is meant to be timing. The RAVENNA driver exposes the same
    // knob ($.network.PTP.DSCP). Which value is the network's business --
    // EF is 46, and Dante marks PTP CS7 (56) -- so this takes it rather
    // than choosing one.
    int dscp = -1;

    // IEEE 1588-2008 sec 7.7.2.4 and 9.5.11.2: the rates above are the
    // master's to announce, not the slave's to assume. With this on, the
    // intervals actually advertised -- logMinDelayReqInterval in Delay_Resp,
    // logAnnounceInterval in Announce -- take over once heard, and the
    // configured values stay as the starting point and as the fallback for a
    // master that advertises nothing usable (0x7F, "stopped", or a value
    // outside the range below). Off restores the fixed behaviour.
    bool followAdvertisedIntervals = true;

    // Bounds on an advertised interval, in log2 seconds: 1/32 s to 32 s.
    // Anything outside is treated as unusable rather than obeyed, so a
    // misconfigured master cannot make this slave send 128 Delay_Req a
    // second or go quiet for an hour.
    int8_t minLogInterval = -5;
    int8_t maxLogInterval = 5;

    // Which profile's traffic to accept, by majorSdoId (the top nibble of
    // octet 0): 0 is the default profile AES67 uses, 1 is 802.1AS.
    uint8_t majorSdoId = 0;
    bool enforceMajorSdoId = true;

    // Peer delay. End to end stays the default: it is what this driver has
    // always done and what an AES67 grandmaster expects. In peer-to-peer
    // mode the slave joins 224.0.0.107, measures the link delay with
    // Pdelay_Req/Pdelay_Resp(/_Follow_Up) instead of Delay_Req, and feeds
    // that link delay into the same offset arithmetic.
    DelayMechanism delayMechanism = DelayMechanism::EndToEnd;

    // logMinPdelayReqInterval advertised in our Pdelay_Req, log2 seconds.
    int8_t logMinPdelayReqInterval = 0;

    // Answer a neighbour's Pdelay_Req. A peer-to-peer port that stays silent
    // leaves its neighbour unable to measure the link, so this is on; it only
    // has an effect in peer-to-peer mode.
    bool respondToPdelayReq = true;

    // portNumber of this PTP port (IEEE 1588-2008 sec 7.5.2.3). One per
    // physical port; it only needs setting when two ports share a clock
    // identity, which is what happens with two instances on one host.
    uint16_t portNumber = 1;

    // IEEE 1588-2008 §13.1 ports; overridable for the unprivileged
    // loopback test (2026-08-31), same knob as PTPMasterConfig's.
    uint16_t eventPort = 319;
    uint16_t generalPort = 320;

    // IP_MULTICAST_LOOP on the sending socket. Off in production, and
    // that is the right default: a slave has no use for its own
    // Delay_Req coming back, and on a busy segment it is pure noise.
    // On only for a same-host master/slave pair (TestPTPLoopback), where
    // the kernel would otherwise never deliver the slave's Delay_Req to
    // a master in another process on this machine -- measured
    // 2026-08-31: 39 Delay_Req sent, 0 seen by the master.
    bool multicastLoopback = false;
};

// ============================================================================
// Callback for offset/delay updates
// ============================================================================

struct PTPMeasurement {
    int64_t offsetFromMasterNs;       // offset = ((t2 - t1) + (t3 - t4)) / 2
    int64_t meanPathDelayNs;          // delay  = ((t2 - t1) - (t3 - t4)) / 2  (simplified)
    double frequencyDriftPpb;         // parts per billion drift estimate
    PTPClockIdentity grandmasterID;
    uint8_t clockClass;
    uint8_t clockAccuracy;
    bool valid;
};

using PTPMeasurementCallback = std::function<void(const PTPMeasurement&)>;

// ============================================================================
// PTP Master configuration
// ============================================================================

struct PTPMasterConfig {
    int domain = 0;
    std::string interfaceName = "en0";

    // IEEE 1588 §7.6.3 defaults. Lower priority1 makes this clock more
    // likely to win BMCA — leave at the spec default unless there's a
    // reason to bias selection.
    uint8_t priority1 = 128;
    uint8_t priority2 = 128;

    // DSCP to mark this port's outgoing PTP with, or -1 to leave it
    // unmarked. Same reasoning as PTPSlaveConfig::dscp.
    int dscp = -1;

    int syncIntervalMs = 125;       // 8/s — matches what PTPSlave expects
    int announceIntervalMs = 1000;  // 1/s, AES67 Media Profile default

    // logMinDelayReqInterval advertised in Delay_Resp: the rate this master
    // asks its slaves to send Delay_Req at, in log2 seconds. 0 is one per
    // second, which is what was hard-coded before and what AES67 uses.
    int8_t logMinDelayReqInterval = 0;
    int announceReceiptTimeoutMultiplier = 3; // silence this many announce
                                               // intervals before assuming
                                               // we're alone on the segment

    // IEEE 1588-2008 §13.1 ports. Defaults are the spec's; overridable so
    // an unprivileged loopback test can run a master and a slave against
    // each other on high ports (2026-08-31 — ports below 1024 need root,
    // which is why the PTP exchange had never been exercised end to end).
    uint16_t eventPort = 319;
    uint16_t generalPort = 320;
};

} // namespace AES67
