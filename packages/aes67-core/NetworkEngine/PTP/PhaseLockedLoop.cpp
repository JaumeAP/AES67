#include "PhaseLockedLoop.h"
#include <cmath>
#include <algorithm>

namespace AES67 {

PhaseLockedLoop::PhaseLockedLoop(double bandwidth, double damping)
    : bandwidth_(bandwidth)
    , damping_(damping)
    , phaseErrorAccumulator_(0.0)
    , frequencyCorrection_(0.0)
    , lastPhaseError_(0.0)
    , lastLocalTime_(0)
    , lastRemoteTime_(0)
    , lastSampleCount_(0)
    , lastSampleRate_(48000)
    , lockCount_(0)
    , lockThreshold_(10)
{
    // Calculate loop filter coefficients based on bandwidth and damping
    // These are derived from standard second-order PLL theory
    //
    // The transfer function of a second-order PLL is:
    //   H(s) = (2*zeta*omega_n*s + omega_n^2) / (s^2 + 2*zeta*omega_n*s + omega_n^2)
    //
    // For a PI controller implementation:
    //   Kp (proportional) controls the transient response
    //   Ki (integral) controls the steady-state tracking

    double omega_n = 2.0 * M_PI * bandwidth_;
    double omega_n_squared = omega_n * omega_n;

    // Standard coefficients for a type-2 second-order loop:
    // Kp = 2 * zeta * omega_n (for normalized phase detector gain)
    // Ki = omega_n^2
    //
    // We scale these for our discrete-time implementation:
    proportionalGain_ = 2.0 * damping_ * omega_n;
    integralGain_ = omega_n_squared;
}

void PhaseLockedLoop::update(uint64_t localTime, uint64_t remoteTime, uint64_t sampleCount, uint32_t sampleRate) {
    std::lock_guard<std::mutex> lock(pllMutex_);

    // Skip if this is the first update (no previous reference)
    if (lastLocalTime_ == 0) {
        lastLocalTime_ = localTime;
        lastRemoteTime_ = remoteTime;
        lastSampleCount_ = sampleCount;
        lastSampleRate_ = sampleRate;
        return;
    }

    // Calculate phase error (difference between remote PTP time and local time)
    // Positive error means remote clock is ahead of local clock
    double phaseError = static_cast<double>(static_cast<int64_t>(remoteTime) - static_cast<int64_t>(localTime));

    // Calculate time elapsed since last update (in seconds)
    double deltaTime = static_cast<double>(localTime - lastLocalTime_) * 1e-9;

    // Sanity check: skip updates with very small or negative delta time
    if (deltaTime <= 0.0 || deltaTime > 10.0) {
        // Invalid delta time, skip this update
        return;
    }

    // Normalize phase error for the loop filter
    // We want the frequency correction to be in fractional units
    double normalizedError = phaseError * 1e-9; // Convert ns to seconds

    // Update the loop filter (proportional + integral)
    // The proportional term provides fast response to phase changes
    double proportionalTerm = proportionalGain_ * normalizedError;

    // The integral term accumulates error over time for zero steady-state error
    phaseErrorAccumulator_ += normalizedError * deltaTime;

    // Apply anti-windup to prevent integrator saturation
    // Limit to +/- 1 second worth of accumulated error
    const double INTEGRAL_LIMIT = 1.0;
    phaseErrorAccumulator_ = std::clamp(phaseErrorAccumulator_, -INTEGRAL_LIMIT, INTEGRAL_LIMIT);

    double integralTerm = integralGain_ * phaseErrorAccumulator_;

    // Calculate total frequency correction
    // This represents the fractional frequency offset (e.g., 0.0001 = 100 ppm)
    double newCorrection = proportionalTerm + integralTerm;

    // Apply smoothing to avoid sudden jumps
    const double SMOOTHING_FACTOR = 0.1;
    frequencyCorrection_ = frequencyCorrection_ * (1.0 - SMOOTHING_FACTOR) +
                           newCorrection * SMOOTHING_FACTOR;

    // Constrain frequency correction to reasonable limits
    frequencyCorrection_ = std::clamp(frequencyCorrection_, MIN_FREQUENCY_CORRECTION, MAX_FREQUENCY_CORRECTION);

    // Update timing references
    lastLocalTime_ = localTime;
    lastRemoteTime_ = remoteTime;
    lastSampleCount_ = sampleCount;
    lastSampleRate_ = sampleRate;

    // Update lock status based on phase error magnitude
    // Use a tighter threshold for professional audio applications
    double lockThresholdNs = CONVERGENCE_THRESHOLD;
    if (std::abs(phaseError) < lockThresholdNs) {
        if (lockCount_ < MAX_LOCK_COUNT) {
            lockCount_++;
        }
    } else {
        // Decay lock count more slowly to avoid frequent lock/unlock transitions
        if (lockCount_ > 0) {
            lockCount_ = std::max(0, lockCount_ - 2);
        }
    }

    lastPhaseError_ = phaseError;
}

double PhaseLockedLoop::getAdjustedSampleRate() const {
    std::lock_guard<std::mutex> lock(pllMutex_);
    // Use the last known sample rate as the base
    return static_cast<double>(lastSampleRate_) * (1.0 + frequencyCorrection_);
}

double PhaseLockedLoop::getAdjustedSampleRate(uint32_t nominalRate) const {
    std::lock_guard<std::mutex> lock(pllMutex_);
    return static_cast<double>(nominalRate) * (1.0 + frequencyCorrection_);
}

double PhaseLockedLoop::getDriftRatio() const {
    std::lock_guard<std::mutex> lock(pllMutex_);
    // The drift ratio is simply 1.0 + the frequency correction
    // A ratio > 1.0 means the remote clock is faster
    // A ratio < 1.0 means the remote clock is slower
    return 1.0 + frequencyCorrection_;
}

int PhaseLockedLoop::getLockQuality() const {
    std::lock_guard<std::mutex> lock(pllMutex_);
    // Convert lock count to a percentage (0-100)
    return std::min(100, (lockCount_ * 100) / MAX_LOCK_COUNT);
}

double PhaseLockedLoop::getPhaseError() const {
    std::lock_guard<std::mutex> lock(pllMutex_);
    return lastPhaseError_;
}

double PhaseLockedLoop::getFrequencyCorrection() const {
    std::lock_guard<std::mutex> lock(pllMutex_);
    return frequencyCorrection_;
}

void PhaseLockedLoop::reset() {
    std::lock_guard<std::mutex> lock(pllMutex_);

    phaseErrorAccumulator_ = 0.0;
    frequencyCorrection_ = 0.0;
    lastPhaseError_ = 0.0;
    lastLocalTime_ = 0;
    lastRemoteTime_ = 0;
    lastSampleCount_ = 0;
    lastSampleRate_ = 48000;
    lockCount_ = 0;
}

} // namespace AES67