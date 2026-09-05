#ifndef NETWORK_ERROR_HANDLER_H
#define NETWORK_ERROR_HANDLER_H

#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>

namespace AES67 {

enum class NetworkErrorType {
    SOCKET_ERROR,
    CONNECTION_TIMEOUT,
    PACKET_LOSS,
    MULTICAST_JOIN_FAILURE,
    INTERFACE_DOWN,
    INSUFFICIENT_BANDWIDTH,
    UNKNOWN_ERROR
};

struct NetworkError {
    NetworkErrorType type;
    std::string message;
    std::string source;
    std::chrono::steady_clock::time_point timestamp;
    int errorCode;
    
    NetworkError(NetworkErrorType t, const std::string& msg, const std::string& src, int err = 0)
        : type(t), message(msg), source(src), errorCode(err) {
        timestamp = std::chrono::steady_clock::now();
    }
};

class NetworkErrorHandler {
public:
    using ErrorHandlerCallback = std::function<void(const NetworkError& error)>;
    
    NetworkErrorHandler();
    ~NetworkErrorHandler();
    
    // Register an error handler callback
    void registerErrorHandler(ErrorHandlerCallback callback);
    
    // Report an error
    void reportError(const NetworkError& error);
    
    // Report a simple error with type and message
    void reportError(NetworkErrorType type, const std::string& message, 
                     const std::string& source = "", int errorCode = 0);
    
    // Check if we're in a recovery state
    bool isInRecovery() const { return recoveryActive_.load(); }
    
    // Get error count
    size_t getErrorCount() const;
    
    // Reset error statistics
    void reset();
    
    // Perform recovery action if needed
    bool attemptRecovery();
    
private:
    // Error handling callback
    ErrorHandlerCallback errorHandlerCallback_;
    mutable std::mutex callbackMutex_;
    
    // Error statistics
    std::atomic<size_t> totalErrorCount_{0};
    std::atomic<size_t> recentErrorCount_{0};
    std::chrono::steady_clock::time_point lastRecoveryAttempt_;
    
    // Recovery state
    std::atomic<bool> recoveryActive_{false};
    std::chrono::steady_clock::time_point recoveryStartTime_;
    
    // Constants
    static constexpr size_t MAX_RECENT_ERRORS = 10;
    static constexpr std::chrono::seconds RECOVERY_COOLDOWN{5};
    static constexpr std::chrono::seconds ERROR_WINDOW{10};
};

// Global error handler instance
extern std::unique_ptr<NetworkErrorHandler> g_networkErrorHandler;

// Macros for easy error reporting
#define REPORT_NETWORK_ERROR(type, msg, source) \
    if (AES67::g_networkErrorHandler) { \
        AES67::g_networkErrorHandler->reportError(type, msg, source); \
    }

#define REPORT_SOCKET_ERROR(msg, source, err_code) \
    REPORT_NETWORK_ERROR(AES67::NetworkErrorType::SOCKET_ERROR, msg, source, err_code)

#define REPORT_TIMEOUT_ERROR(msg, source) \
    REPORT_NETWORK_ERROR(AES67::NetworkErrorType::CONNECTION_TIMEOUT, msg, source)

} // namespace AES67

#endif // NETWORK_ERROR_HANDLER_H