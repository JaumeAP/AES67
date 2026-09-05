#ifndef CLOCK_ADJUSTMENT_CONTROLLER_H
#define CLOCK_ADJUSTMENT_CONTROLLER_H

#include "../RTP/LockFreeCircularJitterBuffer.h"
#include "Resampler.h"
#include "SmoothedPIController.h"
#include <mutex>
#include <chrono>

namespace AES67 {

class ClockAdjustmentController {
public:
    ClockAdjustmentController(LockFreeCircularJitterBuffer& jitterBuffer, Resampler& resampler,
                            double targetFillRatio = 0.5, // Target 50% fill
                            double kp = 0.001,            // Proportional gain
                            double ki = 0.0001);          // Integral gain

    // Call this regularly to monitor buffer levels and adjust resampling ratio
    void updateClockAdjustment();

    // Get the current resampling ratio
    double getCurrentRatio() const;

    // Reset the controller
    void reset();

    // Get PI controller for tuning/debugging
    SmoothedPIController& getPIController() { return piController_; }

private:
    LockFreeCircularJitterBuffer& jitterBuffer_;
    Resampler& resampler_;
    SmoothedPIController piController_;

    // Configuration
    double targetFillRatio_;

    // State
    mutable std::mutex controllerMutex_;
    double currentRatio_;
    size_t targetBufferLevel_;
    size_t maxBufferLevel_;

    // Timing for delta time calculation
    std::chrono::steady_clock::time_point lastUpdateTime_;

    // Stats
    double lastError_;
};

} // namespace AES67

#endif // CLOCK_ADJUSTMENT_CONTROLLER_H