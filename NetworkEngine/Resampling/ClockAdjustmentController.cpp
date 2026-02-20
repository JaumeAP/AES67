#include "ClockAdjustmentController.h"
#include <chrono>

namespace AES67 {

ClockAdjustmentController::ClockAdjustmentController(JitterBuffer& jitterBuffer,
                                                   Resampler& resampler,
                                                   double targetFillRatio,
                                                   double kp,
                                                   double ki)
    : jitterBuffer_(jitterBuffer), resampler_(resampler),
      piController_(kp, ki, -0.01, 0.01, 5),  // Limit to ±1% adjustment with smoothing window of 5
      targetFillRatio_(targetFillRatio),
      currentRatio_(1.0), targetBufferLevel_(0), maxBufferLevel_(jitterBuffer.getMaxBufferSize()),
      lastUpdateTime_(std::chrono::steady_clock::now()), lastError_(0.0) {
    // resampler_ reserved for future use (adaptive resampling ratio adjustment)
    (void)resampler_;
    // Calculate the target buffer level based on the max size and target ratio
    targetBufferLevel_ = static_cast<size_t>(maxBufferLevel_ * targetFillRatio_);
}

void ClockAdjustmentController::updateClockAdjustment() {
    std::lock_guard<std::mutex> lock(controllerMutex_);

    // Get current buffer level
    size_t currentBufferLevel = jitterBuffer_.getBufferedPacketCount();

    // Calculate the error (deviation from target)
    double error = static_cast<double>(static_cast<int64_t>(targetBufferLevel_) -
                                      static_cast<int64_t>(currentBufferLevel));

    // Calculate time delta since last update
    auto now = std::chrono::steady_clock::now();
    auto deltaTime = std::chrono::duration<double>(now - lastUpdateTime_).count();
    lastUpdateTime_ = now;

    // Use PI controller to calculate adjustment
    double adjustment = piController_.update(error, deltaTime);

    // Apply adjustment to current ratio
    currentRatio_ = 1.0 + adjustment;

    // Limit the ratio to reasonable bounds to prevent extreme adjustments
    const double MIN_RATIO = 0.99; // Maximum 1% slowdown
    const double MAX_RATIO = 1.01; // Maximum 1% speedup
    if (currentRatio_ < MIN_RATIO) {
        currentRatio_ = MIN_RATIO;
        // If we hit limits, reset the integrator to avoid windup
        if (adjustment > 0) {
            piController_.reset();
        }
    } else if (currentRatio_ > MAX_RATIO) {
        currentRatio_ = MAX_RATIO;
        // If we hit limits, reset the integrator to avoid windup
        if (adjustment < 0) {
            piController_.reset();
        }
    }

    lastError_ = error;
}

double ClockAdjustmentController::getCurrentRatio() const {
    std::lock_guard<std::mutex> lock(controllerMutex_);
    return currentRatio_;
}

void ClockAdjustmentController::reset() {
    std::lock_guard<std::mutex> lock(controllerMutex_);
    currentRatio_ = 1.0;
    lastError_ = 0.0;
    lastUpdateTime_ = std::chrono::steady_clock::now();
    piController_.reset();
}

} // namespace AES67