#include "AudioThreadPriority.h"
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/resource.h>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace AES67 {

namespace {

// ns -> Mach absolute time. mach_timebase_info is a per-process constant;
// cache it once instead of querying the kernel on every call.
std::uint32_t millisToAbsolute(double ms) {
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t{};
        mach_timebase_info(&t);
        return t;
    }();
    const double ns = ms * 1.0e6;
    const double absolute = ns * tb.denom / tb.numer;
    // Nothing between a hand-edited ptp_master.json's syncIntervalMs/
    // announceIntervalMs and this clamps them, and a value at or above
    // roughly 4.3 s (UINT32_MAX absolute-time ticks, denom/numer close to 1
    // on both Apple Silicon and Intel) makes a double-to-uint32_t cast of an
    // out-of-range value -- undefined behavior, not a wrapped or saturated
    // one, unlike an integer-to-integer narrowing. Clamped here rather than
    // validated further up the chain: this is the one place every caller's
    // milliseconds value is turned into the type Mach's policy actually
    // takes, so it is the one place that has to hold regardless of how a
    // caller arrived at an unreasonable period.
    if (!(absolute > 0.0)) return 0;  // also catches NaN, which > always refuses
    if (absolute >= static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(absolute);
}

} // namespace

bool AudioThreadPriority::configureForRealTime() {
    // 1ms: what AES67 packets default to when the caller has no more
    // specific figure (e.g. hasn't parsed an SDP yet).
    return configureForRealTime(1.0);
}

bool AudioThreadPriority::configureForRealTime(double periodMs) {
    return configureThreadForRealTime(pthread_self(), periodMs);
}

bool AudioThreadPriority::configureThreadForRealTime(pthread_t thread, double periodMs) {
    // THREAD_TIME_CONSTRAINT_POLICY: the Mach policy Apple documents for
    // real-time audio work. It gives the scheduler an actual deadline
    // (period/computation/constraint), unlike THREAD_EXTENDED_POLICY +
    // THREAD_PRECEDENCE_POLICY, which only set a coarse priority hint.
    // computation is a 50% budget of the period — the thread does network
    // I/O and jitter-buffer bookkeeping, not just arithmetic, so leaving
    // headroom matters more here than in a pure DSP callback.
    thread_time_constraint_policy_data_t policy{};
    policy.period      = millisToAbsolute(periodMs);
    policy.computation = millisToAbsolute(periodMs * 0.5);
    policy.constraint  = millisToAbsolute(periodMs);
    policy.preemptible = 0; // kernel ignores this field, but it's the conventional value

    const kern_return_t result = thread_policy_set(
        pthread_mach_thread_np(thread),
        THREAD_TIME_CONSTRAINT_POLICY,
        (thread_policy_t)&policy,
        THREAD_TIME_CONSTRAINT_POLICY_COUNT
    );

    if (result != KERN_SUCCESS) {
        (void)fprintf(stderr, "AES67 AudioThreadPriority: THREAD_TIME_CONSTRAINT_POLICY failed "
                "(kern_return=%d: %s), falling back to nice -20\n",
                result, mach_error_string(result));
        setpriority(PRIO_PROCESS, 0, -20);
        return false;
    }

    // Deliberately not calling THREAD_AFFINITY_POLICY: Apple Silicon doesn't
    // implement it (thread_policy_set returns KERN_NOT_SUPPORTED on every
    // arm64 Mac) and it isn't part of Apple's real-time audio guidance
    // anyway — THREAD_TIME_CONSTRAINT_POLICY above is what tells the
    // scheduler this thread is real-time.
    //
    // Not joining a CoreAudio Audio Workgroup here either — that needs an
    // os_workgroup_t obtained from the device's
    // kAudioDevicePropertyIOThreadOSWorkgroup, which libASPL (this driver's
    // AudioServerPlugIn wrapper) doesn't currently expose, and this class is
    // also used by network-only threads with no device context at all (see
    // Tools/AES67TestSender, AES67TestReceiver). Wiring it in would mean
    // patching the libASPL submodule and threading a workgroup handle down
    // from AES67IOHandler — real work, and not verifiable without a live
    // AES67 device on real hardware. Tracked, not done here.

    return true;
}

void AudioThreadPriority::restoreNormalPriority() {
    thread_extended_policy_data_t extendedPolicy;
    extendedPolicy.timeshare = TRUE; // Timeshare - normal scheduling

    thread_policy_set(
        mach_thread_self(),
        THREAD_EXTENDED_POLICY,
        (thread_policy_t)&extendedPolicy,
        THREAD_EXTENDED_POLICY_COUNT
    );

    // Restore normal nice value
    setpriority(PRIO_PROCESS, 0, 0);
}

std::uint32_t AudioThreadPriority::millisToAbsoluteForTest(double ms) {
    return millisToAbsolute(ms);
}

} // namespace AES67
