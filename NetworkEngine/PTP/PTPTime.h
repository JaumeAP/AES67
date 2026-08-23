//
// PTPTime.h
// AES67 macOS Driver
// One-line system time helper shared by PTPSlave and PTPMaster, so the two
// don't each keep their own private copy of the same clock_gettime() call.
//
#pragma once

#include <cstdint>
#include <time.h>

namespace AES67 {

inline uint64_t ptpSystemTimeNs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

} // namespace AES67
