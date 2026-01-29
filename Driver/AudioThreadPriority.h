#ifndef AUDIO_THREAD_PRIORITY_H
#define AUDIO_THREAD_PRIORITY_H

#include <pthread.h>
#include <sched.h>

namespace AES67 {

/**
 * Utility class to manage real-time audio thread priorities
 * 
 * Critical for preventing audio dropouts due to thread preemption
 */
class AudioThreadPriority {
public:
    /**
     * Configure the current thread for real-time audio processing
     * @return true if successful, false otherwise
     */
    static bool configureForRealTime();
    
    /**
     * Configure a specific thread for real-time audio processing
     * @param thread Thread to configure
     * @return true if successful, false otherwise
     */
    static bool configureThreadForRealTime(pthread_t thread);
    
    /**
     * Restore normal priority to the current thread
     */
    static void restoreNormalPriority();
    
    /**
     * Get the recommended scheduling policy for audio threads
     */
    static int getRecommendedSchedulingPolicy();
    
    /**
     * Get the recommended priority for audio threads
     */
    static int getRecommendedPriority();
};

} // namespace AES67

#endif // AUDIO_THREAD_PRIORITY_H