//
// PTPIntervals.h
// AES67 core
// The one direction Profiles/PtpIntervals.h does not carry: milliseconds
// back to the log2-seconds exponent IEEE 1588 puts on the wire.
//
// Profiles/PtpIntervals.h is the canonical conversion the other way --
// exponent to nanoseconds and to milliseconds -- written once after two
// implementations here disagreed at the edges. This wraps it rather than
// converting again: an earlier version of this file had its own
// exponent-to-nanoseconds function with its own shift clamps, which
// disagreed with the canonical one exactly the way the canonical one exists
// to prevent.
//
#pragma once

#include "Profiles/PtpIntervals.h"

#include <chrono>
#include <cmath>
#include <cstdint>

namespace AES67 {

/// The period an interval names, as the rest of this codebase's PTP timing
/// wants it: a chrono duration rather than a bare integer.
constexpr std::chrono::nanoseconds logIntervalToNs(int8_t logInterval) {
    return std::chrono::nanoseconds(ptpLogIntervalToNanoseconds(logInterval));
}

/// The same period in milliseconds, for a person to read. Fractional,
/// because 62.5 is what sixteen Sync a second is and rounding it is what
/// storing the exponent instead of the milliseconds exists to stop doing.
inline double logIntervalToMs(int8_t logInterval) {
    return std::chrono::duration<double, std::milli>(logIntervalToNs(logInterval)).count();
}

/// The nearest legal interval to a period given in milliseconds.
///
/// Rounding, not truncation, and it rounds in log space: 100 ms is nearer to
/// 125 ms than to 62.5 ms, so it becomes -3. What this cannot do is keep 100
/// ms, and that is the point of storing the exponent instead -- a value that
/// has been through here once is exact from then on.
inline int8_t msToLogInterval(int milliseconds) {
    if (milliseconds <= 0) return 0;
    const double seconds = static_cast<double>(milliseconds) / 1000.0;
    const long rounded = std::lround(std::log2(seconds));
    if (rounded < -128) return -128;
    if (rounded > 127) return 127;
    return static_cast<int8_t>(rounded);
}

} // namespace AES67
