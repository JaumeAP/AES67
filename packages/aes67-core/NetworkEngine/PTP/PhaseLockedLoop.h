#ifndef PHASE_LOCKED_LOOP_H
#define PHASE_LOCKED_LOOP_H

#include <chrono>
#include <mutex>
#include <cstdint>

namespace AES67 {

/**
 * Phase-Locked Loop for Audio Clock Recovery
 *
 * This PLL correlates PTP time with audio sample count to maintain precise
 * synchronization between the AES67 source and the local audio device.
 *
 * Per AES67-2018 Section 8.2, the media clock recovery must track the
 * relationship between RTP timestamps and PTP time, and compensate for
 * clock drift between the remote source and local audio hardware.
 */
class PhaseLockedLoop {
public:
    /**
     * Constructor
     * @param bandwidth Loop bandwidth in Hz (typically 0.1-10 Hz)
     * @param damping Damping factor (typically 0.707 for critical damping)
     */
    explicit PhaseLockedLoop(double bandwidth = 1.0, double damping = 0.707);

    /**
     * Update the PLL with new timing measurements
     * @param localTime Local time in nanoseconds
     * @param remoteTime Remote time (from PTP) in nanoseconds
     * @param sampleCount Number of audio samples that have been processed
     * @param sampleRate Audio sample rate in Hz
     */
    void update(uint64_t localTime, uint64_t remoteTime, uint64_t sampleCount, uint32_t sampleRate);

    /**
     * Get the adjusted sample rate based on PLL corrections
     * @return Adjusted sample rate in Hz
     */
    double getAdjustedSampleRate() const;

    /**
     * Get adjusted sample rate for a specific nominal rate
     * @param nominalRate The nominal sample rate (e.g., 48000, 96000)
     * @return Adjusted sample rate with drift compensation
     */
    double getAdjustedSampleRate(uint32_t nominalRate) const;

    /**
     * Get the current phase error
     * @return Phase error in nanoseconds
     */
    double getPhaseError() const;

    /**
     * Get the current frequency correction
     * @return Frequency correction factor (e.g., 0.0001 = 100 ppm faster)
     */
    double getFrequencyCorrection() const;

    /**
     * Get the clock drift ratio for resampling
     * @return Ratio of remote clock to local clock (1.0 = no drift)
     */
    double getDriftRatio() const;

    /**
     * Reset the PLL to initial state
     */
    void reset();

    /**
     * Get PLL lock status
     * @return true if PLL has converged to stable tracking
     */
    bool isLocked() const { return lockCount_ > 10; }

    /**
     * Get the lock quality (0-100)
     * @return Lock quality percentage
     */
    int getLockQuality() const;

    /**
     * Set the lock threshold for determining locked state
     * @param threshold Number of consecutive good samples needed
     */
    void setLockThreshold(int threshold) { lockThreshold_ = threshold; }

private:
    // Loop filter parameters
    double bandwidth_;
    double damping_;

    // Loop filter coefficients
    double proportionalGain_;
    double integralGain_;

    // PLL state
    double phaseErrorAccumulator_;
    double frequencyCorrection_;
    double lastPhaseError_;

    // Timing references
    uint64_t lastLocalTime_;
    uint64_t lastRemoteTime_;
    uint64_t lastSampleCount_;
    uint32_t lastSampleRate_;

    // Status
    mutable std::mutex pllMutex_;
    int lockCount_;
    int lockThreshold_;

    // Constants
    // AES67 allows up to +/- 4.6 ppm for sample rate accuracy
    // We allow a wider range for tracking external clocks
    static constexpr double MAX_FREQUENCY_CORRECTION = 0.001;  // +1000 ppm
    static constexpr double MIN_FREQUENCY_CORRECTION = -0.001; // -1000 ppm

    // Phase error threshold for considering PLL locked
    // 1ms is fairly loose; tighter values may be needed for pro audio
    static constexpr double CONVERGENCE_THRESHOLD = 1000000.0; // 1ms in nanoseconds

    // Maximum lock count for quality calculation
    static constexpr int MAX_LOCK_COUNT = 100;
};

} // namespace AES67

#endif // PHASE_LOCKED_LOOP_H