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

    // --- Role (PTPArbitrator only — plain PTPSlave leaves these at their
    // defaults: role always "Slave", nothing ever competes) -------------
    //
    // "role" answers the question this driver's own UI needs to show:
    // are we the grandmaster right now, or synced to a remote one?
    enum class Role { Slave, Master };
    Role role{Role::Slave};

    // True once role has been Master at least once since this diagnostics
    // struct started tracking — lets the UI distinguish "currently Slave,
    // never tried" from "currently Slave, lost an election".
    bool everWasMaster{false};

    // BMCA competitor info, only meaningful while role == Slave and a
    // foreign master has actually been heard (masterClockID / clockClass /
    // clockAccuracy above already describe it in that case — these three
    // add what bmcaCompare() actually decided on, for a UI that wants to
    // show "why they won", not just "who they are").
    bool hasCompetitor{false};
    int competitorPriority1{0};
    int competitorPriority2{0};

    PTPDiagnostics() {
        lastMessageTime = std::chrono::steady_clock::now();
    }
};

} // namespace AES67

#endif // PTP_DIAGNOSTICS_H