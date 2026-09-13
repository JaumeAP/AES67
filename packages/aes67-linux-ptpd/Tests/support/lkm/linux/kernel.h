// Kernel names PTP.c uses, for a user-space build of it.
//
// The module's PTP decisions are the thing under test, and they are plain C
// over a packet: the kernel it is written against supplies an allocator, a
// spinlock, a print and two macros, and none of that decides anything. So the
// file is compiled here with these in place of <linux/*.h>, and what it
// decides is exercised directly rather than mirrored.
#pragma once
// stdbool because the module is C and uses bool, true and false throughout,
// and in the kernel those come from <linux/types.h>. A C compiler gives them
// only from here -- clang happened to have them already through another
// header on one platform, which is why this was missing and still built.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KERN_INFO ""
#define KERN_ERR ""
#define printk(...) do { } while (0)
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifdef __cplusplus
extern "C" {
#endif
/// The module's counter clock. The test supplies one that does not move
/// unless it says so.
unsigned long long MTAL_LK_GetCounterTime(void);
#ifdef __cplusplus
}
#endif
