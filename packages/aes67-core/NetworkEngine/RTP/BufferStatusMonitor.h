#ifndef BUFFER_STATUS_MONITOR_H
#define BUFFER_STATUS_MONITOR_H

#include <atomic>
#include <chrono>

namespace AES67 {

/**
 * Monitors buffer status to detect underruns/overruns
 * 
 * Critical for identifying audio quality issues before they become audible
 */
class BufferStatusMonitor {
public:
    BufferStatusMonitor(size_t bufferSize);
    
    /**
     * Update the monitor with current fill level
     * @param fillLevel Current fill level of the buffer
     */
    void updateFillLevel(size_t fillLevel);
    
    /**
     * Check if the buffer is in danger zone (approaching underrun/overrun)
     * @return true if buffer status is concerning
     */
    bool isDangerous() const;
    
    /**
     * Check if buffer underrun occurred
     * @return true if underrun detected
     */
    bool isUnderrun() const;
    
    /**
     * Check if buffer overrun occurred
     * @return true if overrun detected
     */
    bool isOverrun() const;
    
    /**
     * Get current fill percentage (0.0 to 1.0)
     */
    double getFillPercentage() const;
    
    /**
     * Get the fill level that represents the danger zone
     */
    size_t getDangerZoneLow() const;
    size_t getDangerZoneHigh() const;
    
    /**
     * Get statistics
     */
    uint64_t getUnderrunCount() const;
    uint64_t getOverrunCount() const;
    uint64_t getDangerEventsCount() const;
    
    /**
     * Reset statistics
     */
    void resetStatistics();
    
private:
    size_t bufferSize_;
    
    // Thresholds for detection
    static constexpr double DANGER_LOW_PERCENT = 0.10;  // 10% - approaching underrun
    static constexpr double DANGER_HIGH_PERCENT = 0.90; // 90% - approaching overrun
    static constexpr double CRITICAL_LOW_PERCENT = 0.02; // 2% - critical underrun risk
    static constexpr double CRITICAL_HIGH_PERCENT = 0.98; // 98% - critical overrun risk
    
    // Current state
    std::atomic<size_t> currentFillLevel_{0};
    std::chrono::steady_clock::time_point lastUpdate_;
    
    // Statistics
    std::atomic<uint64_t> underrunCount_{0};
    std::atomic<uint64_t> overrunCount_{0};
    std::atomic<uint64_t> dangerEventsCount_{0};
    
    // State tracking
    std::atomic<bool> lastWasUnderrun_{false};
    std::atomic<bool> lastWasOverrun_{false};
    
    // Timing for hysteresis
    std::chrono::steady_clock::time_point lastUnderrunEvent_;
    std::chrono::steady_clock::time_point lastOverrunEvent_;
    
    static constexpr auto HYSTERESIS_PERIOD = std::chrono::milliseconds(100);
};

} // namespace AES67

#endif // BUFFER_STATUS_MONITOR_H