#ifndef PTP_DIAGNOSTICS_H
#define PTP_DIAGNOSTICS_H

#include <string>
#include <chrono>

namespace AES67 {

struct PTPDiagnostics {
    // Connection status
    bool isConnected{false};
    bool isLocked{false};
    std::string masterClockID{""};
    int clockClass{248};  // Default: slave-only
    int clockAccuracy{254};  // Default: unknown accuracy
    int64_t offsetNs{0};
    
    // Network diagnostics
    bool firewallBlockingPTP{false};  // True if UDP 319/320 blocked
    bool firewallBlockingRTP{false};  // True if RTP ports blocked
    int lastMessageReceived{-1};  // Last PTP message type received
    std::chrono::steady_clock::time_point lastMessageTime;
    
    // Quality metrics
    double currentOffset{0.0};  // Current offset in nanoseconds
    double meanOffset{0.0};     // Mean offset over time
    double offsetStdDev{0.0};   // Standard deviation of offset
    double frequencyOffset{0.0}; // Frequency offset in PPM
    
    // Timing quality
    int syncMessagesReceived{0};
    int followUpMessagesReceived{0};
    int delayReqMessagesSent{0};
    int delayRespMessagesReceived{0};
    int announceMessagesReceived{0};
    
    // Error counters
    int stateTransitions{0};
    int ignoredAnnounce{0};
    int domainMismatchErrors{0};
    
    // PTP domain info
    int currentDomain{0};
    int preferredDomain{0};
    
    PTPDiagnostics() {
        lastMessageTime = std::chrono::steady_clock::now();
    }
};

} // namespace AES67

#endif // PTP_DIAGNOSTICS_H