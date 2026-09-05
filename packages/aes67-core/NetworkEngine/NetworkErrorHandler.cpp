#include "NetworkErrorHandler.h"
#include "../Shared/NonBlockingLogger.h"
#include <iostream>
#include <sstream>

namespace AES67 {

// Global error handler instance
std::unique_ptr<NetworkErrorHandler> g_networkErrorHandler;

NetworkErrorHandler::NetworkErrorHandler() : lastRecoveryAttempt_(std::chrono::steady_clock::now()) {
    // Initialize with current time
    recoveryStartTime_ = std::chrono::steady_clock::now();
}

NetworkErrorHandler::~NetworkErrorHandler() = default;

void NetworkErrorHandler::registerErrorHandler(ErrorHandlerCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    errorHandlerCallback_ = std::move(callback);
}

void NetworkErrorHandler::reportError(const NetworkError& error) {
    // Increment error counters
    totalErrorCount_.fetch_add(1);
    recentErrorCount_.fetch_add(1);
    
    // Log the error
    std::ostringstream logMsg;
    logMsg << "Network Error [" << error.source << "]: ";
    logMsg << error.message;
    if (error.errorCode != 0) {
        logMsg << " (Code: " << error.errorCode << ")";
    }
    
    LOG_ERROR(logMsg.str());
    
    // Check if we need to attempt recovery based on error frequency
    auto now = std::chrono::steady_clock::now();
    auto timeSinceLastRecovery = std::chrono::duration_cast<std::chrono::seconds>(
        now - lastRecoveryAttempt_);
    
    // If we've had many recent errors, consider attempting recovery
    if (recentErrorCount_.load() > MAX_RECENT_ERRORS && 
        timeSinceLastRecovery > RECOVERY_COOLDOWN) {
        attemptRecovery();
    }
    
    // Call registered error handler if available
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (errorHandlerCallback_) {
            errorHandlerCallback_(error);
        }
    }
}

void NetworkErrorHandler::reportError(NetworkErrorType type, const std::string& message, 
                                     const std::string& source, int errorCode) {
    NetworkError error(type, message, source, errorCode);
    reportError(error);
}

size_t NetworkErrorHandler::getErrorCount() const {
    return totalErrorCount_.load();
}

void NetworkErrorHandler::reset() {
    totalErrorCount_.store(0);
    recentErrorCount_.store(0);
}

bool NetworkErrorHandler::attemptRecovery() {
    if (recoveryActive_.load()) {
        // Already in recovery, don't start another
        return false;
    }
    
    // Set recovery flag
    recoveryActive_.store(true);
    recoveryStartTime_ = std::chrono::steady_clock::now();
    
    LOG_INFO("Starting network recovery procedure...");
    
    // Perform recovery steps based on the type of errors
    // This is where specific recovery logic would go
    bool success = true;  // Placeholder - implement actual recovery logic
    
    if (success) {
        LOG_INFO("Network recovery completed successfully");
        recentErrorCount_.store(0);  // Reset recent error count
    } else {
        LOG_ERROR("Network recovery failed");
    }
    
    // Update recovery timestamp
    lastRecoveryAttempt_ = std::chrono::steady_clock::now();
    
    // Clear recovery flag after a delay
    recoveryActive_.store(false);
    
    return success;
}

} // namespace AES67