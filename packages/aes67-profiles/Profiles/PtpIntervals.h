//
// PtpIntervals.h
// AES67 profiles
// One conversion from a log2-second interval to milliseconds, for every
// implementation here.
//
// IEEE 1588 carries every interval as a signed log2 of seconds: 0 is one
// second, -3 is 125 ms, 1 is two seconds. Two implementations in this
// repository each converted that to milliseconds their own way, and they
// disagreed at the edges: for -7 the driver said 8 ms (pow and lround) and
// the Teensy said 7 (a shift, which truncates). One rule, here, exact for the
// range 1588 allows, and freestanding so the firmware can use it.
//
#pragma once

#include <cstdint>

namespace AES67 {

/// The value 1588 reserves for "no interval" in logMessageInterval.
inline constexpr int8_t kPtpLogIntervalReserved = 0x7F;

/// Milliseconds for a log2-second interval, rounded to nearest. Exact for
/// every value from -7 (7.8125 ms, returned as 8) up to 21 (2^21 s); outside
/// that a caller has already decided the interval is not one to follow.
///
/// The lower bound is not cosmetic: without it, any logInterval at or below
/// -32 shifts `1u` by 32 or more bits, which the standard leaves undefined
/// for a 32-bit operand -- not merely wrong, unspecified. On this compiler
/// and architecture that came back as though the shift count had wrapped
/// modulo 32, producing a plausible-looking millisecond figure for a value
/// that should have been refused; nothing about that behaviour is
/// guaranteed, on this target or the next one. And this is reachable from
/// the network, not only from a hand-edited file: PTPSlave.cpp passes
/// header.logMessageInterval -- one byte off the wire, from a Sync,
/// Delay_Resp or Announce a peer sent -- straight into logIntervalToMs(),
/// which calls this. Any device on the segment, real or hostile, choosing
/// that byte reaches this undefined behaviour on every message it sends.
/// ptpLogIntervalToNanoseconds already refused below -9; this was the one
/// place in the file that did not, found by an exhaustive sweep of the
/// whole int8_t domain rather than by the handful of documented examples
/// anyone had thought to try.
constexpr uint32_t ptpLogIntervalToMilliseconds(int8_t logInterval) {
    if (logInterval >= 0) {
        return logInterval > 21 ? 0u : (1000u << logInterval);
    }
    if (logInterval < -9) return 0u;
    // 1000 / 2^n, rounded: add half the divisor before dividing.
    const uint32_t divisor = 1u << static_cast<unsigned>(-logInterval);
    return (1000u + divisor / 2u) / divisor;
}

/// Nanoseconds for the same interval, exact rather than rounded. A sender
/// pacing itself in milliseconds sends a -4 interval every 63 ms and
/// announces 62.5, which is a rate that does not match what it says; at
/// nanoseconds the two agree. Zero outside -9..21, the same "not an interval
/// to follow" the millisecond form returns.
constexpr uint64_t ptpLogIntervalToNanoseconds(int8_t logInterval) {
    if (logInterval >= 0) {
        return logInterval > 21 ? 0ull
                                : (1000000000ull << static_cast<unsigned>(logInterval));
    }
    if (logInterval < -9) return 0ull;
    return 1000000000ull >> static_cast<unsigned>(-logInterval);
}

} // namespace AES67
