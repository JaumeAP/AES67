#include "CustomSys.h"
#include "PTPDInterface.h"

#include <time.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Include the original ptpd structures we need
#include "../vendor/ptpd/src/datatypes.h"

void custom_setTime(PTPD::PTPDInterface* ptpd_interface, int64_t seconds, int32_t nanoseconds) {
    if (!ptpd_interface) {
        return;
    }
    
    // Instead of setting the system time, we update our shared state
    // Calculate the offset between the PTP time and the local system time
    int64_t current_system_seconds;
    int32_t current_system_nanoseconds;
    
    standard_getTime(&current_system_seconds, &current_system_nanoseconds);
    
    // Calculate the offset in nanoseconds
    int64_t ptp_time_ns = seconds * 1000000000LL + nanoseconds;
    int64_t local_time_ns = current_system_seconds * 1000000000LL + current_system_nanoseconds;
    
    int64_t offset_ns = ptp_time_ns - local_time_ns;
    
    // Update our shared state
    ptpd_interface->getState().masterOffsetNs.store(offset_ns);
    ptpd_interface->getState().isLocked.store(true);
}

int custom_adjFreq(PTPD::PTPDInterface* ptpd_interface, double adj) {
    if (!ptpd_interface) {
        return 0; // failure
    }
    
    // Instead of adjusting the system frequency, update our shared state
    ptpd_interface->getState().frequencyDrift.store(adj);
    
    // Return success
    return 1;
}

void standard_getTime(int64_t* seconds, int32_t* nanoseconds) {
    struct timespec ts;
    
#if defined(_POSIX_TIMERS) && (_POSIX_TIMERS > 0)
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
        // Fallback to gettimeofday if clock_gettime fails
        struct timeval tv;
        gettimeofday(&tv, 0);
        *seconds = tv.tv_sec;
        *nanoseconds = tv.tv_usec * 1000;
    } else {
        *seconds = ts.tv_sec;
        *nanoseconds = ts.tv_nsec;
    }
#else
    struct timeval tv;
    gettimeofday(&tv, 0);
    *seconds = tv.tv_sec;
    *nanoseconds = tv.tv_usec * 1000;
#endif
}

#ifdef __cplusplus
}
#endif