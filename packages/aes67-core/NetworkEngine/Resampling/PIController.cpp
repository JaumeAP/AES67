#include "PIController.h"
#include <algorithm>
#include <cmath>

namespace AES67 {

PIController::PIController(double kp, double ki, double minOutput, double maxOutput)
    : kp_(kp), ki_(ki), minOutput_(minOutput), maxOutput_(maxOutput),
      integral_(0.0), lastError_(0.0), proportionalTerm_(0.0), integralTerm_(0.0) {
}

double PIController::update(double error, double deltaTime) {
    // Calculate proportional term
    proportionalTerm_ = kp_ * error;
    
    // Update integral with anti-windup protection
    integral_ += error * deltaTime;
    
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

void PIController::reset() {
    integral_ = 0.0;
    lastError_ = 0.0;
    proportionalTerm_ = 0.0;
    integralTerm_ = 0.0;
}

} // namespace AES67