//
// CustomSys.cpp
// AES67 macOS Driver
// Custom system time functions for PTP state tracking
//

#include "CustomSys.h"
#include <time.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

void custom_setTime(struct PTPDInterface* ptpd_interface, int64_t seconds, int32_t nanoseconds) {
    // Stub implementation - in a full PTP implementation, this would update
    // the PTPDInterface state with the time offset
    (void)ptpd_interface;
    (void)seconds;
    (void)nanoseconds;
}

int custom_adjFreq(struct PTPDInterface* ptpd_interface, double adj) {
    // Stub implementation - in a full PTP implementation, this would update
    // the PTPDInterface state with the frequency adjustment
    (void)ptpd_interface;
    (void)adj;
    return 1; // Success
}

void standard_getTime(int64_t* seconds, int32_t* nanoseconds) {
    struct timespec ts;

#if defined(_POSIX_TIMERS) && (_POSIX_TIMERS > 0)
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
        // Fallback to gettimeofday if clock_gettime fails
        struct timeval tv;
        gettimeofday(&tv, NULL);
        *seconds = tv.tv_sec;
        *nanoseconds = tv.tv_usec * 1000;
    } else {
        *seconds = ts.tv_sec;
        *nanoseconds = (int32_t)ts.tv_nsec;
    }
#else
    // Fallback for systems without POSIX timers
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *nanoseconds = tv.tv_usec * 1000;
#endif
}

#ifdef __cplusplus
}
#endif
