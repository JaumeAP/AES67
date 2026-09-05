#ifndef AUDIO_THREAD_PRIORITY_H
#define AUDIO_THREAD_PRIORITY_H

#include <pthread.h>
#include <sched.h>

namespace AES67 {

/**
 * Utility class to manage real-time audio thread priorities.
 *
 * Uses THREAD_TIME_CONSTRAINT_POLICY, the Mach policy Apple documents for
 * real-time audio work — not THREAD_EXTENDED_POLICY/THREAD_PRECEDENCE_POLICY,
 * which don't give the scheduler a deadline, and not THREAD_AFFINITY_POLICY,
 * which Apple Silicon doesn't implement (thread_policy_set for it returns
 * KERN_NOT_SUPPORTED on every arm64 Mac). period/computation/constraint are
 * derived from the caller's actual packet period, not fixed guesses.
 *
 * Critical for preventing audio dropouts due to thread preemption.
 */
class AudioThreadPriority {
public:
    /**
     * Configure the current thread for real-time audio processing, sized to
     * a specific packet/callback period.
     * @param periodMs Real cycle length this thread runs at — e.g. the RTP
     *                 stream's ptime, or the device's I/O buffer duration.
     *                 Computation budget is 50% of the period, constraint is
     *                 the full period.
     * @return true if successful, false otherwise
     */
    static bool configureForRealTime(double periodMs);

    /**
     * Configure the current thread for real-time audio processing using the
     * 1ms period AES67 packets default to when the caller doesn't have a
     * more specific figure at hand (e.g. sdp_.ptime).
     * @return true if successful, false otherwise
     */
    static bool configureForRealTime();

    /**
     * Configure a specific thread for real-time audio processing.
     * @param thread Thread to configure
     * @param periodMs Real cycle length this thread runs at
     * @return true if successful, false otherwise
     */
    static bool configureThreadForRealTime(pthread_t thread, double periodMs);

    /**
     * Restore normal priority to the current thread
     */
    static void restoreNormalPriority();
};

} // namespace AES67

#endif // AUDIO_THREAD_PRIORITY_H