#include "SmoothedPIController.h"
#include <algorithm>
#include <numeric>

namespace AES67 {

SmoothedPIController::SmoothedPIController(double kp, double ki, double minOutput, double maxOutput, size_t smoothingWindow)
    : kp_(kp), ki_(ki), minOutput_(minOutput), maxOutput_(maxOutput),
      integral_(0.0), lastError_(0.0),
      smoothingIndex_(0), smoothingWindow_(smoothingWindow),
      proportionalTerm_(0.0), integralTerm_(0.0) {
    smoothingBuffer_.resize(smoothingWindow_, 0.0);
}

double SmoothedPIController::update(double error, double deltaTime) {
    // Apply smoothing to the error signal to reduce noise
    double smoothedError = applySmoothing(error);
    
    // Calculate proportional term
    proportionalTerm_ = kp_ * smoothedError;
    
    // Update integral with anti-windup protection
    integral_ += smoothedError * deltaTime;
    
    // Clamp the integral term to prevent windup
    double maxIntegral = maxOutput_ / ki_;
    double minIntegral = minOutput_ / ki_;
    integral_ = std::clamp(integral_, minIntegral, maxIntegral);
    
    integralTerm_ = ki_ * integral_;
    
    // Calculate output
    double output = proportionalTerm_ + integralTerm_;
    
    // Clamp output to limits
    output = std::clamp(output, minOutput_, maxOutput_);
    
    return output;
}

void SmoothedPIController::reset() {
    integral_ = 0.0;
    lastError_ = 0.0;
    proportionalTerm_ = 0.0;
    integralTerm_ = 0.0;
    
    // Reset smoothing buffer
    std::fill(smoothingBuffer_.begin(), smoothingBuffer_.end(), 0.0);
    smoothingIndex_ = 0;
}

double SmoothedPIController::applySmoothing(double error) {
    if (smoothingWindow_ == 0) {
        return error;
    }
    
    // Add the new error value to the smoothing buffer
    smoothingBuffer_[smoothingIndex_] = error;
    smoothingIndex_ = (smoothingIndex_ + 1) % smoothingWindow_;
    
    // Calculate the average of the values in the smoothing buffer
    double sum = 0.0;
    for (double val : smoothingBuffer_) {
        sum += val;
    }
    
    return sum / static_cast<double>(smoothingBuffer_.size());
}

} // namespace AES67