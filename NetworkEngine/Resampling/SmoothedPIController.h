#ifndef SMOOTHED_PI_CONTROLLER_H
#define SMOOTHED_PI_CONTROLLER_H

#include <vector>
#include <algorithm>

namespace AES67 {

/**
 * PI (Proportional-Integral) Controller with smoothing for audio clock adjustment
 * 
 * This controller is designed to maintain zero steady-state error while
 * minimizing noise amplification in audio applications.
 */
class SmoothedPIController {
public:
    /**
     * Constructor
     * @param kp Proportional gain
     * @param ki Integral gain
     * @param minOutput Minimum output value
     * @param maxOutput Maximum output value
     * @param smoothingWindow Size of the smoothing window (higher = more smoothing)
     */
    SmoothedPIController(double kp = 0.1, double ki = 0.01, 
                        double minOutput = -0.1, double maxOutput = 0.1,
                        size_t smoothingWindow = 5);
    
    /**
     * Update the controller with a new error value
     * @param error The error signal (difference between desired and actual)
     * @param deltaTime Time elapsed since last update (in seconds)
     * @return Controller output
     */
    double update(double error, double deltaTime);
    
    /**
     * Reset the controller to initial state
     */
    void reset();
    
    /**
     * Get current integral term
     */
    double getIntegral() const { return integral_; }
    
    /**
     * Get current proportional term
     */
    double getProportional() const { return proportionalTerm_; }

private:
    double kp_;           // Proportional gain
    double ki_;           // Integral gain
    double minOutput_;    // Minimum output limit
    double maxOutput_;    // Maximum output limit
    
    double integral_;     // Accumulated integral term
    double lastError_;    // Previous error for derivative calculation (if needed)
    
    // Smoothing buffer
    std::vector<double> smoothingBuffer_;
    size_t smoothingIndex_;
    size_t smoothingWindow_;
    
    // For debugging/monitoring
    double proportionalTerm_;
    double integralTerm_;
    
    // Apply smoothing to the error signal
    double applySmoothing(double error);
};

} // namespace AES67

#endif // SMOOTHED_PI_CONTROLLER_H