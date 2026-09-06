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

#include "NetworkEngine/PTP/PTPClockSource.h"
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

// PTPMasterConfig is plain data and lives in the core, alongside the wire
// types: NetworkEngine/PTP/PTPProtocolTypes.h, included through PTPSlave.h.

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

    // The two message intervals, settled once in the constructor. The log
    // values are what goes on the wire; the periods are what the transmit
    // loop waits, derived from those same log values. Read these rather than
    // config_.syncIntervalMs and config_.announceIntervalMs: two numbers for
    // one rate is how a port ends up announcing 125 ms while sending every
    // 100.
    int8_t logSyncInterval_;
    int8_t logAnnounceInterval_;
    std::chrono::nanoseconds syncPeriod_;
    std::chrono::nanoseconds announcePeriod_;

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
