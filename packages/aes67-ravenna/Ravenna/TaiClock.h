//
// TaiClock.h
// AES67 RAVENNA session layer
// The clock NMOS counts in.
//
// IS-05 and IS-08 both carry times as TAI seconds and nanoseconds written
// "<seconds>:<nanoseconds>", and both schedule activations against them. The
// clock underneath is the system's, which is UTC, and the difference is the
// leap seconds inserted since 1972. A device with no traceable time cannot
// discover that number, so it is written down here once: a second copy in the
// other API is a second thing to get wrong the next time it moves.
//
#pragma once

#include <cstdint>
#include <string>

namespace AES67::Ravenna {

/// 37 since 2017-01-01. A controller scheduling an activation one second out
/// would otherwise be told to wait thirty-eight.
inline constexpr uint64_t kTaiMinusUtcSeconds = 37;
inline constexpr uint32_t kNanosPerSecond = 1'000'000'000;

/// A TAI instant, as the two specifications write one.
struct TaiTime {
    uint64_t seconds = 0;
    uint32_t nanos = 0;
};

TaiTime taiNow();
std::string taiText(const TaiTime& time);

/// Reads "<seconds>:<nanoseconds>". False on anything else, including a
/// number too large to hold: a controller that sends one is refused rather
/// than quietly given some other instant.
bool parseTai(const std::string& text, TaiTime& time);

TaiTime taiSum(const TaiTime& left, const TaiTime& right);
bool taiReached(const TaiTime& due, const TaiTime& now);

}  // namespace AES67::Ravenna
