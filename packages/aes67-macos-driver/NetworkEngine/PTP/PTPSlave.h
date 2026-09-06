//
// PTPSlave.h
// AES67 macOS Driver
// IEEE 1588-2008 PTP Slave-Only Implementation
//
// Implements the PTP slave state machine for AES67 audio synchronization.
// Handles Sync, Follow_Up, Delay_Req, Delay_Resp message exchange on
// UDP multicast ports 319 (event) and 320 (general).
//
// This is a slave-only implementation per AES67-2018 requirements:
//   - PTP domain 0 (default)
//   - Two-step clock (Sync + Follow_Up)
//   - 8 Sync messages per second expected (125ms interval)
//   - Calculates offset and path delay using standard PTP formulas
//

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <array>
#include <chrono>

// The IEEE 1588 message types, timestamps, identities, announce data and this
// engine's configuration are plain data and live in the core:
// NetworkEngine/PTP/PTPProtocolTypes.h. What is left here is the state machine
// that owns the sockets and threads.
#include "NetworkEngine/PTP/PTPProtocolTypes.h"

namespace AES67 {

// Forward declaration
struct PTPDiagnostics;


// ============================================================================
// PTPSlave — IEEE 1588 PTP Slave-Only State Machine
// ============================================================================

class PTPSlave {
public:
    explicit PTPSlave(const PTPSlaveConfig& config);
    ~PTPSlave();

    // Prevent copy/move
    PTPSlave(const PTPSlave&) = delete;
    PTPSlave& operator=(const PTPSlave&) = delete;

    // Lifecycle
    bool start();
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // Status
    bool isLocked() const { return locked_.load(std::memory_order_acquire); }
    int64_t getOffsetNs() const { return offsetNs_.load(std::memory_order_acquire); }
    int64_t getMeanPathDelayNs() const { return pathDelayNs_.load(std::memory_order_acquire); }
    double getFrequencyDriftPpb() const { return frequencyDriftPpb_.load(std::memory_order_acquire); }
    std::string getGrandmasterID() const;
    uint8_t getClockClass() const { return clockClass_.load(std::memory_order_acquire); }
    uint8_t getClockAccuracy() const { return clockAccuracy_.load(std::memory_order_acquire); }

    // What the master says it is doing, in milliseconds, or 0 when it has
    // not said anything usable yet. The sync interval is reporting only --
    // it is what the grandmaster claims to send at, not a timeout here.
    int getAdvertisedSyncIntervalMs() const {
        return advertisedSyncIntervalMs_.load(std::memory_order_relaxed);
    }
    int getAdvertisedAnnounceIntervalMs() const {
        return advertisedAnnounceIntervalMs_.load(std::memory_order_relaxed);
    }
    int getAdvertisedDelayReqIntervalMs() const {
        return advertisedDelayReqIntervalMs_.load(std::memory_order_relaxed);
    }
    // Sync messages dropped for being one-step while `twoStepOnly` is on.
    // Non-zero means a master is on this domain that this slave will not
    // follow, which looks exactly like silence unless it is counted.
    int getOneStepRejectedCount() const {
        return oneStepRejectedCount_.load(std::memory_order_relaxed);
    }
    int getSdoIdMismatchCount() const {
        return sdoIdMismatchCount_.load(std::memory_order_relaxed);
    }

    // Peer-delay counters. The link delay itself is getMeanPathDelayNs():
    // in peer-to-peer mode that is what the Pdelay exchange measured.
    int getPdelayReqSentCount() const {
        return pdelayReqSentCount_.load(std::memory_order_relaxed);
    }
    int getPdelayRespCount() const {
        return pdelayRespCount_.load(std::memory_order_relaxed);
    }
    int getPdelayRespFollowUpCount() const {
        return pdelayRespFollowUpCount_.load(std::memory_order_relaxed);
    }
    int getPdelayReqAnsweredCount() const {
        return pdelayReqAnsweredCount_.load(std::memory_order_relaxed);
    }

    // Milliseconds for a logMessageInterval, or 0 when it is unusable
    // (0x7F, "sending stopped", or outside the configured bounds).
    int logIntervalToMs(int8_t logInterval) const;

    // Set callback for measurement updates
    void setMeasurementCallback(PTPMeasurementCallback cb);

    /// This port's identity: the clock identity built from the interface MAC
    /// by start(), and the configured port number, which is set from
    /// construction. Zero clock identity means start() has not run.
    PTPPortIdentity getPortIdentity() const { return selfPortId_; }

    /// Feeds one PTP message in as if it had arrived on the event socket
    /// (319) or the general one (320) at `receiveTimeNs`. Everything a real
    /// datagram goes through it goes through: the profile and domain checks,
    /// then the same handler.
    ///
    /// This exists so a grandmaster's traffic can be replayed against this
    /// slave without a network — see Tests/TestPTPMasterBoxInterop.cpp, which
    /// drives it with the bytes the AES67-MasterBox emits. Nothing inside the
    /// driver calls it.
    void deliverMessage(const uint8_t* data, size_t len, uint64_t receiveTimeNs,
                        bool onEventSocket);

    // Update diagnostics structure (thread-safe)
    void updateDiagnostics(PTPDiagnostics& diag) const;

private:
    // Socket management
    bool createSockets();
    void closeSockets();

    // Main receive thread
    void receiveThread();

    // Delay request thread
    void delayReqThread();

    // Message parsing
    bool parseHeader(const uint8_t* data, size_t len, PTPHeader& header);
    /// One received message, checked and routed. Both sockets and
    /// deliverMessage() come through here, so a check added for one is a
    /// check added for all of them.
    void dispatchMessage(const uint8_t* data, size_t len, uint64_t receiveTimeNs,
                         bool onEventSocket);
    bool parseTimestamp(const uint8_t* data, size_t offset, PTPTimestamp& ts);
    void parseClockIdentity(const uint8_t* data, size_t offset, PTPClockIdentity& id);
    void parsePortIdentity(const uint8_t* data, size_t offset, PTPPortIdentity& pid);

    // Message handling
    void handleSync(const PTPHeader& header, const uint8_t* data, size_t len,
                    uint64_t receiveTimeNs);
    void handleFollowUp(const PTPHeader& header, const uint8_t* data, size_t len);
    void handleDelayResp(const PTPHeader& header, const uint8_t* data, size_t len);
    void handleAnnounce(const PTPHeader& header, const uint8_t* data, size_t len);

    // Delay_Req transmission
    bool sendDelayReq();

    // Peer delay (IEEE 1588-2008 sec 11.4).
    bool sendPdelayReq();
    bool sendPdelayResp(const PTPHeader& request, uint64_t receiptTimeNs);
    void handlePdelayReq(const PTPHeader& header, const uint8_t* data, size_t len,
                         uint64_t receiveTimeNs);
    void handlePdelayResp(const PTPHeader& header, const uint8_t* data, size_t len,
                          uint64_t receiveTimeNs);
    void handlePdelayRespFollowUp(const PTPHeader& header, const uint8_t* data,
                                  size_t len);
    // linkDelay = ((t4 - t1) - (t3 - t2)) / 2, filtered and published as the
    // path delay the offset arithmetic uses.
    void completePdelay(int64_t t3MinusT2Ns);
    void storeFilteredPathDelay(int64_t delayNs);

    // Offset/delay calculation
    void calculateOffsetAndDelay();

    // Get current system time in nanoseconds
    static uint64_t getSystemTimeNs();

    // Get MAC address of the configured interface
    bool getInterfaceMAC(uint8_t mac[6]) const;

    // Configuration
    PTPSlaveConfig config_;

    // Sockets
    int eventSocket_;    // UDP port 319 (Sync, Delay_Req, Delay_Resp)
    int generalSocket_;  // UDP port 320 (Follow_Up, Announce)

    // Our clock identity
    PTPPortIdentity selfPortId_;

    // Thread management
    std::thread receiveThread_;
    std::thread delayReqThread_;
    std::atomic<bool> running_{false};

    // Measurement callback
    PTPMeasurementCallback measurementCallback_;
    mutable std::mutex callbackMutex_;

    // ---- PTP State ----

    // Current master (from Announce/BMCA)
    mutable std::mutex masterMutex_;
    PTPAnnounceData currentMaster_;
    bool hasMaster_{false};

    // Sync state: waiting for Follow_Up with matching sequenceId
    mutable std::mutex syncMutex_;
    uint16_t lastSyncSequenceId_{0};
    uint64_t t2_receiveTimeNs_{0};        // Our receive time of Sync message
    bool waitingForFollowUp_{false};
    PTPTimestamp syncOriginTimestamp_;     // One-step: origin from Sync itself
    int64_t syncCorrectionField_{0};      // Correction from Sync message

    // Follow_Up provides t1 (precise origin timestamp)
    PTPTimestamp t1_syncOriginTimestamp_;

    // Delay_Req state
    mutable std::mutex delayMutex_;
    uint16_t delayReqSequenceId_{0};
    uint64_t t3_delayReqSendTimeNs_{0};   // Our send time of Delay_Req
    bool waitingForDelayResp_{false};

    // Delay_Resp provides t4
    PTPTimestamp t4_delayRespReceiveTimestamp_;

    // Computed values (atomics for lock-free reading from other threads)
    std::atomic<int64_t> offsetNs_{0};
    std::atomic<int64_t> pathDelayNs_{0};
    std::atomic<double> frequencyDriftPpb_{0.0};
    std::atomic<bool> locked_{false};
    std::atomic<uint8_t> clockClass_{255};
    std::atomic<uint8_t> clockAccuracy_{0xFE};

    // Grandmaster identity (protected by masterMutex_)
    PTPClockIdentity grandmasterIdentity_;

    // Offset filtering — simple moving average
    static constexpr size_t kOffsetFilterSize = 8;
    std::array<int64_t, kOffsetFilterSize> offsetHistory_;
    size_t offsetHistoryIndex_{0};
    size_t offsetHistoryCount_{0};

    // Path delay filtering
    static constexpr size_t kDelayFilterSize = 8;
    std::array<int64_t, kDelayFilterSize> delayHistory_;
    size_t delayHistoryIndex_{0};
    size_t delayHistoryCount_{0};

    // Lock detection
    int consecutiveGoodMeasurements_{0};
    static constexpr int kLockThreshold = 8;
    static constexpr int64_t kLockToleranceNs = 10000000; // 10ms — coarse initial lock

    // Drift estimation (simple linear regression over recent offsets)
    uint64_t lastDriftCalcTimeNs_{0};
    int64_t lastDriftCalcOffsetNs_{0};

    // Statistics
    std::atomic<int> syncCount_{0};
    std::atomic<int> followUpCount_{0};
    std::atomic<int> delayReqSentCount_{0};
    std::atomic<int> delayRespCount_{0};
    std::atomic<int> announceCount_{0};
    std::atomic<int> domainMismatchCount_{0};
    std::atomic<int> oneStepRejectedCount_{0};
    std::atomic<int> sdoIdMismatchCount_{0};
    std::atomic<int> pdelayReqSentCount_{0};
    std::atomic<int> pdelayRespCount_{0};
    std::atomic<int> pdelayRespFollowUpCount_{0};
    std::atomic<int> pdelayReqAnsweredCount_{0};

    // Peer-delay exchange in flight, all touched only by the receive and
    // request threads under pdelayMutex_.
    std::mutex pdelayMutex_;
    uint16_t pdelayReqSequenceId_{0};
    uint64_t t1_pdelayReqSendTimeNs_{0};
    uint64_t t4_pdelayRespReceiveTimeNs_{0};
    PTPTimestamp t2_pdelayRequestReceipt_;
    int64_t pdelayCorrectionNs_{0};
    bool waitingForPdelayResp_{false};
    bool waitingForPdelayFollowUp_{false};

    // Intervals as advertised by the master, in milliseconds; 0 until one
    // has been heard, in which case the configured value stands.
    std::atomic<int> advertisedDelayReqIntervalMs_{0};
    std::atomic<int> advertisedAnnounceIntervalMs_{0};
    std::atomic<int> advertisedSyncIntervalMs_{0};
};

} // namespace AES67
