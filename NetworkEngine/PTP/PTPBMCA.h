//
// PTPBMCA.h
// AES67 macOS Driver
// IEEE 1588-2008 §9.3 — the dataset comparison algorithm at the heart of the
// Best Master Clock Algorithm. Pure function, no sockets/threads/state: given
// two clocks' announced quality, says which one a compliant port must prefer.
//
// Scoped to the same-domain, non-topology case (no path-trace / steps-removed
// tie-breaking beyond the identity fallback) — the realistic shape of an AES67
// LAN with a handful of clocks on one segment, not a routed PTP boundary-clock
// hierarchy. What §9.3 calls the "grandmaster comparison": compare
// priority1, then clockClass, then clockAccuracy, then offsetScaledLogVariance,
// then priority2, then clockIdentity as a deterministic tiebreak. Lower wins
// at every step except identity, which is just "smallest byte string wins" so
// two clocks never both think they're better.
//
#pragma once

#include "PTPSlave.h"

namespace AES67 {

enum class PTPBMCAWinner {
    A,   // Dataset A is the better clock
    B,   // Dataset B is the better clock
};

/// IEEE 1588 clockClass 255: "slave-only", per §7.6.2.4 — a clock advertising
/// this can never legally become a master. Anything at or above this in the
/// comparison already loses on step 2, but callers that build their own
/// Announce data should refuse to transmit at all if their source reports
/// this class, rather than rely on comparison-based rejection.
constexpr uint8_t kPTPClockClassSlaveOnly = 255;

/// Compares two Announce datasets and returns which one wins the BMCA
/// grandmaster comparison (IEEE 1588-2008 Figure 27/28, §9.3.2.5). A tie on
/// every quality field falls through to comparing clockIdentity byte-by-byte
/// — arbitrary, but the same rule at both ends, so it can never deadlock.
inline PTPBMCAWinner bmcaCompare(const PTPAnnounceData& a, const PTPAnnounceData& b) {
    if (a.grandmasterPriority1 != b.grandmasterPriority1)
        return a.grandmasterPriority1 < b.grandmasterPriority1 ? PTPBMCAWinner::A : PTPBMCAWinner::B;

    if (a.grandmasterClockClass != b.grandmasterClockClass)
        return a.grandmasterClockClass < b.grandmasterClockClass ? PTPBMCAWinner::A : PTPBMCAWinner::B;

    if (a.grandmasterClockAccuracy != b.grandmasterClockAccuracy)
        return a.grandmasterClockAccuracy < b.grandmasterClockAccuracy ? PTPBMCAWinner::A : PTPBMCAWinner::B;

    if (a.grandmasterOffsetScaledLogVariance != b.grandmasterOffsetScaledLogVariance)
        return a.grandmasterOffsetScaledLogVariance < b.grandmasterOffsetScaledLogVariance
             ? PTPBMCAWinner::A : PTPBMCAWinner::B;

    if (a.grandmasterPriority2 != b.grandmasterPriority2)
        return a.grandmasterPriority2 < b.grandmasterPriority2 ? PTPBMCAWinner::A : PTPBMCAWinner::B;

    // Deterministic tiebreak: lower clockIdentity (compared as an 8-byte
    // big-endian number) wins. Neither side can ever see itself as strictly
    // greater than the other's identity and also strictly less, so this
    // can't produce a two-master standoff.
    for (size_t i = 0; i < a.grandmasterIdentity.id.size(); ++i) {
        if (a.grandmasterIdentity.id[i] != b.grandmasterIdentity.id[i])
            return a.grandmasterIdentity.id[i] < b.grandmasterIdentity.id[i]
                 ? PTPBMCAWinner::A : PTPBMCAWinner::B;
    }
    // Fully identical datasets (e.g. hearing our own Announce looped back):
    // no real winner: caller should treat this as "not a foreign competitor".
    return PTPBMCAWinner::A;
}

} // namespace AES67
