//
// ExternalReference.h
// AES67 Linux PTP daemon
// One pulse per second, stamped by the clock that stamps the packets, and
// what it does to that clock.
//
// This is the whole difference between a grandmaster that is merely precise
// and one that is right. Without it the NIC's clock free-runs on its crystal:
// every device on the network agrees with it to the nanosecond and the whole
// network drifts together, away from the studio.
//
// The edge has to be stamped by the PHC and not by the kernel. Linux offers
// that through PTP_EXTTS_REQUEST on the PHC descriptor, which is a separate
// capability from stamping packets: a NIC can do one and not the other, and
// the count is in PTP_CLOCK_GETCAPS. When it is zero there is no path, and
// this says so rather than quietly stamping against CLOCK_REALTIME through
// /dev/pps, which would mean disciplining one clock and announcing another.
//
// The servo is not a new one. It is packages/t41-ptp's ptp-servo, the same
// one the Teensy box runs against its word clock, which is platform-free for
// exactly this reason.
//
#pragma once

#include "ptp/ptp-servo.h"

#include <cstdint>
#include <string>

namespace AES67::LinuxPtpd {

class PhcClock;

struct ReferenceStatus {
    bool available = false;   ///< the PHC has an external timestamp channel
    bool locked = false;      ///< inside the lock window for long enough
    int64_t offsetNs = 0;     ///< the last edge's distance from the second
    double driftNsps = 0;     ///< what the servo is holding the clock at
    uint64_t edges = 0;       ///< edges seen since start
    uint64_t rejected = 0;    ///< edges dropped as not credible
};

class ExternalReference {
public:
    ExternalReference() = default;
    ~ExternalReference();

    ExternalReference(const ExternalReference&) = delete;
    ExternalReference& operator=(const ExternalReference&) = delete;

    /// Asks the PHC for external timestamps on `channel`. False with `error`
    /// set when the clock offers none, which is a fact about the board rather
    /// than a failure of this daemon.
    bool enable(PhcClock& clock, unsigned int channel, std::string& error);

    /// Reads whatever edges are waiting and steers the clock. Cheap when
    /// nothing has arrived, which is almost every call.
    void service();

    const ReferenceStatus& status() const { return status_; }

    /// How many seconds without an edge before a locked clock stops calling
    /// itself locked. Three missed edges: one is a glitch, three is a cable.
    static constexpr uint64_t kHoldoverSeconds = 3;

private:
    void apply(const t41ptp::ServoOutcome& outcome);

    PhcClock* clock_ = nullptr;
    unsigned int channel_ = 0;
    bool enabled_ = false;

    t41ptp::ServoState servo_{};
    t41ptp::ServoTuning tuning_{};

    uint64_t lastEdgeNs_ = 0;
    bool haveLastEdge_ = false;
    uint64_t consecutiveInWindow_ = 0;
    ReferenceStatus status_{};
};

}  // namespace AES67::LinuxPtpd
