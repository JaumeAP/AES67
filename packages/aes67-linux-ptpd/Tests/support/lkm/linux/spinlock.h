#pragma once
typedef int spinlock_t;
static inline void spin_lock_init(spinlock_t* l) { (void)l; }
static inline void spin_lock(spinlock_t* l) { (void)l; }
static inline void spin_unlock(spinlock_t* l) { (void)l; }
static inline void spin_lock_irqsave(spinlock_t* l, unsigned long f) { (void)l; (void)f; }
static inline void spin_unlock_irqrestore(spinlock_t* l, unsigned long f) { (void)l; (void)f; }
