//
// CustomSys.cpp
// AES67 macOS Driver
// Custom system time functions for PTP state tracking.
// These functions provide a way for external PTP code to update our
// driver's PTP state without modifying the actual system clock.
//

#include "CustomSys.h"
#include <time.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

void custom_setTime(struct PTPDInterface* ptpd_interface, int64_t seconds, int32_t nanoseconds) {
    // This function is called when PTP wants to "set" the clock.
    // In our driver, we don't modify the system clock — instead we record
    // the offset between PTP time and local system time.
    // The actual offset tracking is now done in PTPSlave via the
    // four-timestamp calculation (t1, t2, t3, t4), so this stub
    // remains for compatibility with any code that calls it.
    (void)ptpd_interface;
    (void)seconds;
    (void)nanoseconds;
}

int custom_adjFreq(struct PTPDInterface* ptpd_interface, double adj) {
    // Frequency adjustment request from PTP.
    // In our implementation, frequency tracking is handled by PTPSlave's
    // drift estimation and the PhaseLockedLoop. This stub remains for
    // compatibility.
    (void)ptpd_interface;
    (void)adj;
    return 1; // Success
}

void standard_getTime(int64_t* seconds, int32_t* nanoseconds) {
    struct timespec ts;
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
}

#ifdef __cplusplus
}
#endif
