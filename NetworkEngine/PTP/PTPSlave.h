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

namespace AES67 {

// Forward declaration
struct PTPDiagnostics;

// ============================================================================
// IEEE 1588 PTP Message Types
// ============================================================================

// IEEE 1588-2008 sec 8.2.5.4.4: how this port measures the path.
// End to end is Delay_Req/Delay_Resp with the master and is what AES67
// deployments use; peer to peer measures the link to the immediate
// neighbour with Pdelay, which is what 802.1AS and transparent-clock
// topologies use.
enum class DelayMechanism : uint8_t {
    EndToEnd,
    PeerToPeer,
};

enum class PTPMessageType : uint8_t {
    Sync           = 0x00,
    Delay_Req      = 0x01,
    Pdelay_Req     = 0x02,
    Pdelay_Resp    = 0x03,
    Follow_Up      = 0x08,
    Delay_Resp     = 0x09,
    Pdelay_Resp_FU = 0x0A,
    Announce       = 0x0B,
    Signaling      = 0x0C,
    Management     = 0x0D,
};

// ============================================================================
// PTP Timestamp (IEEE 1588 Section 5.3.3)
// ============================================================================

struct PTPTimestamp {
    uint16_t secondsHi;    // Upper 16 bits of seconds
    uint32_t secondsLo;    // Lower 32 bits of seconds
    uint32_t nanoseconds;  // Nanoseconds (0 - 999,999,999)

    PTPTimestamp() : secondsHi(0), secondsLo(0), nanoseconds(0) {}

    PTPTimestamp(uint64_t totalNs) {
        uint64_t totalSec = totalNs / 1000000000ULL;
        secondsHi = static_cast<uint16_t>((totalSec >> 32) & 0xFFFF);
        secondsLo = static_cast<uint32_t>(totalSec & 0xFFFFFFFF);
        nanoseconds = static_cast<uint32_t>(totalNs % 1000000000ULL);
    }

    uint64_t toNanoseconds() const {
        uint64_t totalSec = (static_cast<uint64_t>(secondsHi) << 32) |
                            static_cast<uint64_t>(secondsLo);
        return totalSec * 1000000000ULL + nanoseconds;
    }

    bool isZero() const {
        return secondsHi == 0 && secondsLo == 0 && nanoseconds == 0;
    }
};

// ============================================================================
// PTP Clock Identity (IEEE 1588 Section 5.3.4)
// ============================================================================

struct PTPClockIdentity {
    std::array<uint8_t, 8> id;

    PTPClockIdentity() { id.fill(0); }

    bool operator==(const PTPClockIdentity& other) const { return id == other.id; }
    bool operator!=(const PTPClockIdentity& other) const { return id != other.id; }

    std::string toString() const;

    // Build from MAC address (EUI-48 to EUI-64 conversion)
    static PTPClockIdentity fromMAC(const uint8_t mac[6]);
};

// ============================================================================
// PTP Port Identity (IEEE 1588 Section 5.3.5)
// ============================================================================

struct PTPPortIdentity {
    PTPClockIdentity clockIdentity;
    uint16_t portNumber;

    PTPPortIdentity() : portNumber(0) {}

    bool operator==(const PTPPortIdentity& other) const {
        return clockIdentity == other.clockIdentity && portNumber == other.portNumber;
    }
};

// ============================================================================
// PTP Common Header (IEEE 1588 Section 13.3)
// ============================================================================

struct PTPHeader {
    uint8_t transportAndType;     // transportSpecific (4 bits) | messageType (4 bits)
    uint8_t versionPTP;           // Reserved (4 bits) | versionPTP (4 bits)
    uint16_t messageLength;
    uint8_t domainNumber;
    uint8_t reserved1;
    uint16_t flagField;
    int64_t correctionField;      // 64-bit fixed point (ns * 2^16)
    uint32_t reserved2;
    PTPPortIdentity sourcePortIdentity;
    uint16_t sequenceId;
    uint8_t controlField;
    int8_t logMessageInterval;

    PTPMessageType getMessageType() const {
        return static_cast<PTPMessageType>(transportAndType & 0x0F);
    }

    // The top nibble of octet 0: majorSdoId in IEEE 1588-2019, and
    // transportSpecific in 1588-2008. 0 is the default profile, 1 is what
    // 802.1AS/gPTP puts there. It was parsed but never looked at, so a gPTP
    // master on the same segment and domain was followed as if it belonged
    // to this profile.
    uint8_t getMajorSdoId() const {
        return static_cast<uint8_t>((transportAndType >> 4) & 0x0F);
    }
};

// ============================================================================
// PTP Announce Message Data (for BMCA)
// ============================================================================

struct PTPAnnounceData {
    PTPPortIdentity masterPortId;
    PTPClockIdentity grandmasterIdentity;
    uint8_t grandmasterClockClass;
    uint8_t grandmasterClockAccuracy;
    uint16_t grandmasterOffsetScaledLogVariance;
    uint8_t grandmasterPriority1;
    uint8_t grandmasterPriority2;
    uint16_t stepsRemoved;
    uint8_t timeSource;
    int8_t logAnnounceInterval;
    std::chrono::steady_clock::time_point lastReceived;
};

// ============================================================================
// PTP Slave Configuration
// ============================================================================

struct PTPSlaveConfig {
    int domain = 0;                              // PTP domain number
    std::string interfaceName = "en0";           // Network interface
    int delayReqIntervalMs = 1000;               // Delay_Req interval (ms)
    int announceTimeoutMultiplier = 3;           // Announce receipt timeout multiplier
    int announceIntervalMs = 1000;               // Expected announce interval
    bool twoStepOnly = true;                     // Only accept two-step clocks (AES67)

    // IEEE 1588-2008 sec 7.7.2.4 and 9.5.11.2: the rates above are the
    // master's to announce, not the slave's to assume. With this on, the
    // intervals actually advertised -- logMinDelayReqInterval in Delay_Resp,
    // logAnnounceInterval in Announce -- take over once heard, and the
    // configured values stay as the starting point and as the fallback for a
    // master that advertises nothing usable (0x7F, "stopped", or a value
    // outside the range below). Off restores the fixed behaviour.
    bool followAdvertisedIntervals = true;

    // Bounds on an advertised interval, in log2 seconds: 1/32 s to 32 s.
    // Anything outside is treated as unusable rather than obeyed, so a
    // misconfigured master cannot make this slave send 128 Delay_Req a
    // second or go quiet for an hour.
    int8_t minLogInterval = -5;
    int8_t maxLogInterval = 5;

    // Which profile's traffic to accept, by majorSdoId (the top nibble of
    // octet 0): 0 is the default profile AES67 uses, 1 is 802.1AS.
    uint8_t majorSdoId = 0;
    bool enforceMajorSdoId = true;

    // Peer delay. End to end stays the default: it is what this driver has
    // always done and what an AES67 grandmaster expects. In peer-to-peer
    // mode the slave joins 224.0.0.107, measures the link delay with
    // Pdelay_Req/Pdelay_Resp(/_Follow_Up) instead of Delay_Req, and feeds
    // that link delay into the same offset arithmetic.
    DelayMechanism delayMechanism = DelayMechanism::EndToEnd;

    // logMinPdelayReqInterval advertised in our Pdelay_Req, log2 seconds.
    int8_t logMinPdelayReqInterval = 0;

    // Answer a neighbour's Pdelay_Req. A peer-to-peer port that stays silent
    // leaves its neighbour unable to measure the link, so this is on; it only
    // has an effect in peer-to-peer mode.
    bool respondToPdelayReq = true;

    // portNumber of this PTP port (IEEE 1588-2008 sec 7.5.2.3). One per
    // physical port; it only needs setting when two ports share a clock
    // identity, which is what happens with two instances on one host.
    uint16_t portNumber = 1;

    // IEEE 1588-2008 §13.1 ports; overridable for the unprivileged
    // loopback test (2026-08-31), same knob as PTPMasterConfig's.
    uint16_t eventPort = 319;
    uint16_t generalPort = 320;

    // IP_MULTICAST_LOOP on the sending socket. Off in production, and
    // that is the right default: a slave has no use for its own
    // Delay_Req coming back, and on a busy segment it is pure noise.
    // On only for a same-host master/slave pair (TestPTPLoopback), where
    // the kernel would otherwise never deliver the slave's Delay_Req to
    // a master in another process on this machine -- measured
    // 2026-08-31: 39 Delay_Req sent, 0 seen by the master.
    bool multicastLoopback = false;
};

// ============================================================================
// Callback for offset/delay updates
// ============================================================================

struct PTPMeasurement {
    int64_t offsetFromMasterNs;       // offset = ((t2 - t1) + (t3 - t4)) / 2
    int64_t meanPathDelayNs;          // delay  = ((t2 - t1) - (t3 - t4)) / 2  (simplified)
    double frequencyDriftPpb;         // parts per billion drift estimate
    PTPClockIdentity grandmasterID;
    uint8_t clockClass;
    uint8_t clockAccuracy;
    bool valid;
};

using PTPMeasurementCallback = std::function<void(const PTPMeasurement&)>;

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
