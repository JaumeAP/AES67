#pragma once
#include <stdlib.h>
#define GFP_ATOMIC 0
#define GFP_KERNEL 0
static inline void* kmalloc(unsigned long size, int flags) { (void)flags; return malloc(size); }
static inline void kfree(void* p) { free(p); }
