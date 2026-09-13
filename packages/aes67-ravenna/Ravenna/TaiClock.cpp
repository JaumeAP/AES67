#include "Ravenna/TaiClock.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <exception>

namespace AES67::Ravenna {

TaiTime taiNow() {
    struct timespec now {};
    ::clock_gettime(CLOCK_REALTIME, &now);
    return {static_cast<uint64_t>(now.tv_sec) + kTaiMinusUtcSeconds,
            static_cast<uint32_t>(now.tv_nsec)};
}

std::string taiText(const TaiTime& time) {
    return std::to_string(time.seconds) + ":" + std::to_string(time.nanos);
}

bool parseTai(const std::string& text, TaiTime& time) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == text.size()) return false;
    const std::string seconds = text.substr(0, colon);
    const std::string nanos = text.substr(colon + 1);
    const auto digits = [](const std::string& part) {
        return std::all_of(part.begin(), part.end(),
                           [](unsigned char digit) { return std::isdigit(digit) != 0; });
    };
    if (!digits(seconds) || !digits(nanos)) return false;

    try {
        time.seconds = std::stoull(seconds);
        const unsigned long long fraction = std::stoull(nanos);
        if (fraction >= kNanosPerSecond) return false;
        time.nanos = static_cast<uint32_t>(fraction);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

TaiTime taiSum(const TaiTime& left, const TaiTime& right) {
    TaiTime sum{left.seconds + right.seconds, left.nanos + right.nanos};
    if (sum.nanos >= kNanosPerSecond) {
        sum.nanos -= kNanosPerSecond;
        ++sum.seconds;
    }
    return sum;
}

bool taiReached(const TaiTime& due, const TaiTime& now) {
    if (now.seconds != due.seconds) return now.seconds > due.seconds;
    return now.nanos >= due.nanos;
}

}  // namespace AES67::Ravenna
