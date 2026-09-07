#include "ErrorRecoveryManager.h"
#include <thread>
#include <utility>

namespace AES67 {

ErrorRecoveryManager::ErrorRecoveryManager() {
    lastErrorTime_ = std::chrono::steady_clock::now();
}

ErrorRecoveryManager::~ErrorRecoveryManager() = default;

void ErrorRecoveryManager::registerRecoveryAction(ErrorType errorType, RecoveryAction action) {
    RecoveryState state;
    state.action = std::move(action);
    state.lastAttempt = std::chrono::steady_clock::time_point{};  // epoch
    state.attemptCount = 0;
    state.inProgress = false;
    
    recoveryActions_[errorType] = state;
}

void ErrorRecoveryManager::reportError(ErrorType errorType, const std::string& context) {
    lastErrorTime_ = std::chrono::steady_clock::now();
    
    auto it = recoveryActions_.find(errorType);
    if (it == recoveryActions_.end() || !autoRecoveryEnabled_) {
        return;
    }
    
    RecoveryState& state = it->second;
    
    // Check if we're in cooldown period
    auto now = std::chrono::steady_clock::now();
    if (now - state.lastAttempt < RECOVERY_COOLDOWN) {
        return;
    }
    
    // Check if we've exceeded maximum attempts
    if (state.attemptCount >= MAX_RECOVERY_ATTEMPTS) {
        return;
    }
    
    // Perform recovery action
    state.inProgress = true;
    state.lastAttempt = now;
    state.attemptCount++;
    recoveryAttempts_.fetch_add(1);
    
    bool success = false;
    try {
        success = state.action();
    } catch (...) {
        success = false;
    }
    
    if (success) {
        // Reset attempt count on success
        state.attemptCount = 0;
    }
    
    state.inProgress = false;
}

std::chrono::steady_clock::duration ErrorRecoveryManager::getTimeSinceLastError() const {
    auto now = std::chrono::steady_clock::now();
    return now - lastErrorTime_;
}

bool ErrorRecoveryManager::isInRecovery() const {
    for (const auto& pair : recoveryActions_) {
        if (pair.second.inProgress) {
            return true;
        }
    }
    return false;
}

size_t ErrorRecoveryManager::getRecoveryAttempts() const {
    return recoveryAttempts_.load();
}

void ErrorRecoveryManager::resetStatistics() {
    recoveryAttempts_.store(0);
    for (auto& pair : recoveryActions_) {
        pair.second.attemptCount = 0;
    }
}

void ErrorRecoveryManager::setAutoRecoveryEnabled(bool enabled) {
    autoRecoveryEnabled_.store(enabled);
}

bool ErrorRecoveryManager::isAutoRecoveryEnabled() const {
    return autoRecoveryEnabled_.load();
}

} // namespace AES67