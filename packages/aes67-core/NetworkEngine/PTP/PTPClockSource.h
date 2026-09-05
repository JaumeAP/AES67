//
// PTPClockSource.h
// AES67 macOS Driver
// What PTPMaster serves to the network when it's the one acting as PTP
// master: where "now" and the announced quality bits come from. Two kinds —
// InternalClockSource (this Mac's own free-running clock, ClockClass 248 per
// IEEE 1588 §7.6.2.4 "default, not synchronized to a primary reference") and
// CoreAudioClockSource (NetworkEngine/PTP/CoreAudioClockSource.h — locks to
// another local audio interface's hardware clock, e.g. a word-clock input).
//
#pragma once

#include "PTPTime.h"

#include <cstdint>
#include <string>

namespace AES67 {

/// IEEE 1588-2008 §7.6.2.5, Table 6 — clockAccuracy enumeration values used
/// here. Not the full table: just the ones this driver's clock sources use.
enum class PTPClockAccuracy : uint8_t {
    /// 0xFE — accuracy Unknown. Honest default for a free-running crystal
    /// with no calibration data behind it.
    Unknown = 0xFE,
    /// 0x21 — accurate within 1 microsecond. What a locally word-clock-locked
    /// audio interface can reasonably claim; not GPS-grade, but far tighter
    /// than an uncalibrated crystal.
    Within1Microsecond = 0x21,
};

/// A source of time (and the quality bits describing how good that time is)
/// that PTPMaster can serve to the network. Implementations only need to be
/// safe to call from PTPMaster's own transmit thread — no other threading
/// guarantee is assumed or required.
class PTPClockSource {
public:
    virtual ~PTPClockSource() = default;

    /// Current time, nanoseconds since the PTP epoch (1970-01-01, same as
    /// CLOCK_REALTIME) — what gets stamped into the Sync/Follow_Up origin
    /// timestamp when this source is driving the master.
    virtual uint64_t currentTimeNs() const = 0;

    /// IEEE 1588 clockClass to announce. kPTPClockClassSlaveOnly (255, see
    /// PTPBMCA.h) is a deliberate opt-out: PTPMaster refuses to transmit at
    /// all if the active source ever reports it, rather than rely on losing
    /// the BMCA comparison.
    virtual uint8_t clockClass() const = 0;

    /// IEEE 1588 clockAccuracy to announce.
    virtual PTPClockAccuracy clockAccuracy() const = 0;

    /// Human-readable label — what the "select a source" UI shows, and
    /// what ends up in logs/diagnostics.
    virtual std::string name() const = 0;
};

/// This Mac's own free-running clock (CLOCK_REALTIME). No external
/// reference, no calibration — the honest baseline every Mac has without
/// needing any other hardware attached.
class InternalClockSource : public PTPClockSource {
public:
    uint64_t currentTimeNs() const override { return ptpSystemTimeNs(); }
    uint8_t clockClass() const override { return 248; } // §7.6.2.4: default, free-running
    PTPClockAccuracy clockAccuracy() const override { return PTPClockAccuracy::Unknown; }
    std::string name() const override { return "Internal (this Mac's clock)"; }
};

} // namespace AES67
