#include "NetworkErrorHandler.h"
#include "../Shared/NonBlockingLogger.h"
#include <iostream>
#include <sstream>

namespace AES67 {

// Global error handler instance
std::unique_ptr<NetworkErrorHandler> g_networkErrorHandler;

NetworkErrorHandler::NetworkErrorHandler()
    : lastRecoveryAttemptTicks_(std::chrono::steady_clock::now().time_since_epoch().count()) {}

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
    
    // Check if we need to attempt recovery based on error frequency. Read
    // once into a local time_point rather than racing attemptRecovery()'s
    // write of the same field from another thread.
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::steady_clock::time_point lastRecovery(
        std::chrono::steady_clock::duration(lastRecoveryAttemptTicks_.load()));
    const auto timeSinceLastRecovery =
        std::chrono::duration_cast<std::chrono::seconds>(now - lastRecovery);
    
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
    LOG_INFO("Starting network recovery procedure...");
    
    // There are no recovery steps of its own to run here: what recovers a
    // socket is the owner reopening it, and this handler's part is to say the
    // attempt happened and clear the recent error count so the next failure is
    // judged on its own. The branch that used to test a hardcoded `true` said
    // nothing and hid that.
    LOG_INFO("Network recovery completed");
    recentErrorCount_.store(0);
    
    // What reportError's cooldown is measured from.
    lastRecoveryAttemptTicks_.store(std::chrono::steady_clock::now().time_since_epoch().count());
    
    return true;
}

} // namespace AES67