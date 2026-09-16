//
// PTPIntervals.h
// AES67 core
// The log2-seconds intervals IEEE 1588 carries, and milliseconds, and the
// conversions between them.
//
// 1588-2008 SS 7.7.2.1 puts every message rate on the wire as a signed power
// of two seconds: 0 is one per second, -3 is eight, 1 is one every two. A
// port announces the rate it sends at, and a conforming slave times its
// master-lost window and its own Delay_Req rate off those bytes -- so a rate
// announced and a rate sent that differ is a lie the other end acts on.
//
// Milliseconds cannot express every legal rate: sixteen Sync per second is
// 62.5 ms, and an int of milliseconds rounds it before anybody can object.
// The exponent is therefore what gets stored and configured, and milliseconds
// are a rendering of it for a person.
//
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>

namespace AES67 {

/// The period an interval names, in nanoseconds.
///
/// Nanoseconds because milliseconds cannot hold the fast rates, and because
/// 2^n seconds is a whole number of nanoseconds for every n down to -9 --
/// 512 per second, well past anything a PTP port sends at. Below that it
/// truncates, by under a nanosecond.
constexpr std::chrono::nanoseconds logIntervalToNs(int8_t logInterval) {
    constexpr int64_t kNsPerSecond = 1000000000;

    // The reachable range is much narrower than int8_t, but the parameter is
    // an int8_t and a shift of 127 on an int64 is undefined behaviour -- not
    // a large number, undefined. 1e9 is just under 2^30, so a left shift of
    // 33 is the last one that fits; on the right, 62 is the last that stays
    // defined for a signed 64-bit value, and everything past ~30 is zero
    // nanoseconds anyway. Clamping changes nothing that can happen; it puts
    // the limit where the compiler can see it.
    constexpr int kMaxLeftShift = 33;
    constexpr int kMaxRightShift = 62;

    if (logInterval >= 0) {
        const int shift = logInterval < kMaxLeftShift ? logInterval : kMaxLeftShift;
        return std::chrono::nanoseconds(kNsPerSecond << shift);
    }

    // This is the branch where logInterval is below zero, so negating it
    // lands above zero.
    int shift = -static_cast<int>(logInterval);
    if (shift > kMaxRightShift) shift = kMaxRightShift;
    return std::chrono::nanoseconds(kNsPerSecond >> shift);
}

/// The same period in milliseconds, fractional: this is what a person is
/// shown, and 62.5 is a legal answer.
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
