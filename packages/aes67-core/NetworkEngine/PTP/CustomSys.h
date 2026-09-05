#ifndef PTPD_CUSTOM_SYS_H
#define PTPD_CUSTOM_SYS_H

// This header defines custom implementations of system functions
// that update our shared state instead of adjusting the system clock

#include <cstdint>
#include <time.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration of our state structure
struct PTPDInterface;

// Custom implementations that update shared state instead of system clock
void custom_setTime(struct PTPDInterface* ptpd_interface, int64_t seconds, int32_t nanoseconds);
int custom_adjFreq(struct PTPDInterface* ptpd_interface, double adj);

// Functions to get time using standard system calls
void standard_getTime(int64_t* seconds, int32_t* nanoseconds);

#ifdef __cplusplus
}
#endif

#endif /* PTPD_CUSTOM_SYS_H */