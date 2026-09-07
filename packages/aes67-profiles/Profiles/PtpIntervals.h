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
constexpr uint32_t ptpLogIntervalToMilliseconds(int8_t logInterval) {
    if (logInterval >= 0) {
        return logInterval > 21 ? 0u : (1000u << logInterval);
    }
    // 1000 / 2^n, rounded: add half the divisor before dividing.
    const uint32_t divisor = 1u << static_cast<unsigned>(-logInterval);
    return (1000u + divisor / 2u) / divisor;
}

} // namespace AES67
