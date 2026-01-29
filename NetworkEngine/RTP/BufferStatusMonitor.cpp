#include "BufferStatusMonitor.h"
#include <algorithm>

namespace AES67 {

BufferStatusMonitor::BufferStatusMonitor(size_t bufferSize) : bufferSize_(bufferSize), lastUpdate_(std::chrono::steady_clock::now()) {
    lastUnderrunEvent_ = std::chrono::steady_clock::now();
    lastOverrunEvent_ = std::chrono::steady_clock::now();
}

void BufferStatusMonitor::updateFillLevel(size_t fillLevel) {
    currentFillLevel_.store(fillLevel, std::memory_order_relaxed);
    
    auto now = std::chrono::steady_clock::now();
    lastUpdate_ = now;
    
    // Calculate fill percentage
    double fillPercent = bufferSize_ > 0 ? static_cast<double>(fillLevel) / static_cast<double>(bufferSize_) : 0.0;
    
    // Check for underrun (very low fill level)
    if (fillPercent < CRITICAL_LOW_PERCENT) {
        // Check hysteresis to avoid counting the same event multiple times
        if (now - lastUnderrunEvent_ > HYSTERESIS_PERIOD) {
            underrunCount_.fetch_add(1, std::memory_order_relaxed);
            lastUnderrunEvent_ = now;
            lastWasUnderrun_.store(true, std::memory_order_relaxed);
        }
    } else {
        lastWasUnderrun_.store(false, std::memory_order_relaxed);
    }
    
    // Check for overrun (very high fill level)
    if (fillPercent > CRITICAL_HIGH_PERCENT) {
        // Check hysteresis to avoid counting the same event multiple times
        if (now - lastOverrunEvent_ > HYSTERESIS_PERIOD) {
            overrunCount_.fetch_add(1, std::memory_order_relaxed);
            lastOverrunEvent_ = now;
            lastWasOverrun_.store(true, std::memory_order_relaxed);
        }
    } else {
        lastWasOverrun_.store(false, std::memory_order_relaxed);
    }
    
    // Check for danger zone events
    if (fillPercent < DANGER_LOW_PERCENT || fillPercent > DANGER_HIGH_PERCENT) {
        dangerEventsCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool BufferStatusMonitor::isDangerous() const {
    double fillPercent = bufferSize_ > 0 ? 
        static_cast<double>(currentFillLevel_.load(std::memory_order_relaxed)) / static_cast<double>(bufferSize_) : 0.0;
    
    return fillPercent < DANGER_LOW_PERCENT || fillPercent > DANGER_HIGH_PERCENT;
}

bool BufferStatusMonitor::isUnderrun() const {
    double fillPercent = bufferSize_ > 0 ? 
        static_cast<double>(currentFillLevel_.load(std::memory_order_relaxed)) / static_cast<double>(bufferSize_) : 0.0;
    
    return fillPercent < CRITICAL_LOW_PERCENT;
}

bool BufferStatusMonitor::isOverrun() const {
    double fillPercent = bufferSize_ > 0 ? 
        static_cast<double>(currentFillLevel_.load(std::memory_order_relaxed)) / static_cast<double>(bufferSize_) : 0.0;
    
    return fillPercent > CRITICAL_HIGH_PERCENT;
}

double BufferStatusMonitor::getFillPercentage() const {
    return bufferSize_ > 0 ? 
        static_cast<double>(currentFillLevel_.load(std::memory_order_relaxed)) / static_cast<double>(bufferSize_) : 0.0;
}

size_t BufferStatusMonitor::getDangerZoneLow() const {
    return static_cast<size_t>(bufferSize_ * DANGER_LOW_PERCENT);
}

size_t BufferStatusMonitor::getDangerZoneHigh() const {
    return static_cast<size_t>(bufferSize_ * DANGER_HIGH_PERCENT);
}

uint64_t BufferStatusMonitor::getUnderrunCount() const {
    return underrunCount_.load(std::memory_order_relaxed);
}

uint64_t BufferStatusMonitor::getOverrunCount() const {
    return overrunCount_.load(std::memory_order_relaxed);
}

uint64_t BufferStatusMonitor::getDangerEventsCount() const {
    return dangerEventsCount_.load(std::memory_order_relaxed);
}

void BufferStatusMonitor::resetStatistics() {
    underrunCount_.store(0, std::memory_order_relaxed);
    overrunCount_.store(0, std::memory_order_relaxed);
    dangerEventsCount_.store(0, std::memory_order_relaxed);
    
    lastUnderrunEvent_ = std::chrono::steady_clock::now();
    lastOverrunEvent_ = std::chrono::steady_clock::now();
}

} // namespace AES67