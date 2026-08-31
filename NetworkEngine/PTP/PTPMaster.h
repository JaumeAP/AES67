//
// PTPMaster.h
// AES67 macOS Driver
// IEEE 1588-2008 PTP master transmission + a scoped Best Master Clock
// Algorithm (see PTPBMCA.h for the comparison itself).
//
// Not a competing implementation of PTPSlave's careful four-timestamp offset
// math — a master doesn't need any of that, it just stamps its own clock
// into outgoing Sync/Follow_Up/Announce and lets everyone else run the slave
// side against it. PTPArbitrator (PTPArbitrator.h) is what actually decides
// which of PTPMaster/PTPSlave gets to be active on the wire at once; this
// class only decides "should *I* be transmitting", via BMCA against whatever
// foreign Announce traffic it hears on the wire.
//
// Same honesty as the rest of this driver's PTP subsystem: written against
// the standard, exercised with synthetic tests (Tests/TestPTPMaster.cpp),
// never run against a real second PTP master on real hardware.
//
#pragma once

#include "PTPBMCA.h"
#include "PTPClockSource.h"
#include "PTPSlave.h"   // PTPMessageType, PTPTimestamp, PTPClockIdentity,
                        // PTPPortIdentity, PTPHeader, PTPAnnounceData

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace AES67 {

enum class PTPMasterRole {
    Listening,  // Not transmitting yet: still within the initial listen window
    Master,     // We won BMCA (or heard no competitor): transmitting Announce/Sync
    Passive,    // A better clock is on the wire: not transmitting, deferring to it
};

struct PTPMasterConfig {
    int domain = 0;
    std::string interfaceName = "en0";

    // IEEE 1588 §7.6.3 defaults. Lower priority1 makes this clock more
    // likely to win BMCA — leave at the spec default unless there's a
    // reason to bias selection.
    uint8_t priority1 = 128;
    uint8_t priority2 = 128;

    int syncIntervalMs = 125;       // 8/s — matches what PTPSlave expects
    int announceIntervalMs = 1000;  // 1/s, AES67 Media Profile default
    int announceReceiptTimeoutMultiplier = 3; // silence this many announce
                                               // intervals before assuming
                                               // we're alone on the segment

    // IEEE 1588-2008 §13.1 ports. Defaults are the spec's; overridable so
    // an unprivileged loopback test can run a master and a slave against
    // each other on high ports (2026-08-31 — ports below 1024 need root,
    // which is why the PTP exchange had never been exercised end to end).
    uint16_t eventPort = 319;
    uint16_t generalPort = 320;
};

class PTPMaster {
public:
    /// clockSource must outlive this PTPMaster.
    PTPMaster(const PTPMasterConfig& config, PTPClockSource& clockSource);
    ~PTPMaster();

    PTPMaster(const PTPMaster&) = delete;
    PTPMaster& operator=(const PTPMaster&) = delete;

    bool start();
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    PTPMasterRole role() const { return role_.load(std::memory_order_acquire); }
    bool isActive() const { return role() == PTPMasterRole::Master; }

    /// The foreign competitor currently causing us to stay Passive, if any.
    std::optional<PTPAnnounceData> currentCompetitor() const;

    int announceSentCount() const { return announceSentCount_.load(std::memory_order_relaxed); }
    int syncSentCount() const { return syncSentCount_.load(std::memory_order_relaxed); }
    int foreignAnnounceCount() const { return foreignAnnounceCount_.load(std::memory_order_relaxed); }
    int delayRespSentCount() const { return delayRespSentCount_.load(std::memory_order_relaxed); }

private:
    bool createSockets();
    void closeSockets();

    // Listens for foreign Announce (BMCA input) on the general socket.
    void receiveThread();
    // Ticks the BMCA decision and, while Master, sends Announce/Sync/Follow_Up.
    void transmitThread();

    void handleForeignAnnounce(const PTPHeader& header, const uint8_t* data, size_t len);
    // Answers a slave's Delay_Req with a Delay_Resp carrying t4 — the
    // master half of the delay exchange (2026-08-31; until then the
    // master never listened on the event port at all, so no slave could
    // ever measure path delay against us).
    void handleDelayReq(const PTPHeader& header, uint16_t sequenceId, uint64_t t4Ns);
    void evaluateBMCA();

    /// Our own Announce dataset, built from config_ + clockSource_ — what we
    /// advertise as grandmaster if we're Master, and what evaluateBMCA()
    /// compares against currentCompetitor_.
    PTPAnnounceData ourAnnounceData() const;

    bool sendAnnounce();
    bool sendSyncAndFollowUp();

    bool getInterfaceMAC(uint8_t mac[6]) const;

    PTPMasterConfig config_;
    PTPClockSource& clockSource_;

    int eventSocket_ = -1;
    int generalSocket_ = -1;

    PTPPortIdentity selfPortId_;
    PTPClockIdentity grandmasterIdentity_; // same as selfPortId_.clockIdentity — we're our own grandmaster while Master

    std::thread receiveThread_;
    std::thread transmitThread_;
    std::atomic<bool> running_{false};

    std::atomic<PTPMasterRole> role_{PTPMasterRole::Listening};
    std::chrono::steady_clock::time_point startTime_;

    mutable std::mutex competitorMutex_;
    std::optional<PTPAnnounceData> competitor_;
    std::chrono::steady_clock::time_point competitorLastSeen_;

    uint16_t announceSequenceId_ = 0;
    uint16_t syncSequenceId_ = 0;

    std::atomic<int> announceSentCount_{0};
    std::atomic<int> syncSentCount_{0};
    std::atomic<int> foreignAnnounceCount_{0};
    std::atomic<int> delayRespSentCount_{0};
};

} // namespace AES67
