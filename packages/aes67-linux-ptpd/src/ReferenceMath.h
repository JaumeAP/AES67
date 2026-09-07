//
// ReferenceMath.h
// AES67 Linux PTP daemon
// Where a reference edge falls with respect to the clock's own second.
//
// Platform-free, and tested, because getting it wrong is silent: a sign
// mistake here steers the clock away from the reference at exactly the rate
// it should be steering towards it, and everything downstream still looks
// like a working servo.
//
#pragma once

#include <cstdint>

namespace AES67::LinuxPtpd {

inline constexpr int64_t kNanosecondsPerSecond = 1000000000LL;

/// How far a reference edge is from the nearest second of the clock that
/// stamped it, in nanoseconds, positive when the edge is late.
///
/// A one-per-second reference says nothing about which second it is: what it
/// carries is the boundary. So an edge stamped at x.999999998 is two
/// nanoseconds early, not 999999998 late, and the result is folded into
/// [-500 ms, +500 ms) for that reason.
constexpr int64_t offsetFromSecondBoundary(uint64_t edgeTimeNs) {
    int64_t withinSecond = static_cast<int64_t>(edgeTimeNs % static_cast<uint64_t>(kNanosecondsPerSecond));
    if (withinSecond >= kNanosecondsPerSecond / 2) {
        withinSecond -= kNanosecondsPerSecond;
    }
    return withinSecond;
}

}  // namespace AES67::LinuxPtpd
